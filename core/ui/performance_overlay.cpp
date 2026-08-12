/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "performance_overlay.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "gui_font.h"
#include "gui_util.h"
#include "hw/pvr/spg.h"
#include "imgui.h"
#include "oslib/cpu_usage.h"
#include "oslib/gpu_memory.h"
#include "oslib/i18n.h"
#include "oslib/process_memory.h"
#include "rend/TexCache.h"
#include "stdclass.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && defined(CLOCK_THREAD_CPUTIME_ID) \
	&& defined(_POSIX_THREAD_CPUTIME) && _POSIX_THREAD_CPUTIME >= 0
#define HOLLYCAST_HAS_PTHREAD_CPU_CLOCK 1
#endif

namespace performance_overlay {

namespace {

constexpr const char *Section = "rend";
constexpr std::size_t FrameTimeHistorySize = 152;

std::atomic<bool> collectionEnabled { false };
std::atomic<std::uint64_t> pvrWorkMicroseconds { 0 };
std::atomic<std::uint32_t> vblankCount { 0 };
std::atomic<std::uint32_t> gameFrameCount { 0 };
std::atomic<std::uint64_t> gpuTimingNanoseconds { 0 };
std::atomic<std::uint32_t> gpuTimingSampleCount { 0 };
std::atomic<bool> gpuTimingSupported { false };
std::atomic<bool> gpuTimingRequested { false };
std::atomic<std::uint32_t> gpuTimingEpoch { 0 };
std::atomic<std::uint64_t> pvrPrimitiveCount { 0 };
std::atomic<std::uint64_t> pvrVertexCount { 0 };
std::atomic<std::uint32_t> rendererSceneCount { 0 };
std::atomic<std::uint32_t> rendererPassCount { 0 };
std::atomic<std::uint64_t> pvrScheduledRenderCycles { 0 };
std::atomic<std::uint64_t> pvrVideoFrameCycles { 0 };
std::atomic<int> rawRenderWidth { 0 };
std::atomic<int> rawRenderHeight { 0 };
std::atomic<int> renderedWidth { 0 };
std::atomic<int> renderedHeight { 0 };
std::atomic<int> outputViewportWidth { 0 };
std::atomic<int> outputViewportHeight { 0 };

#ifdef _WIN32
bool readThreadCpuTimeMicroseconds(HANDLE thread, std::uint64_t& microseconds)
{
	FILETIME creationTime {};
	FILETIME exitTime {};
	FILETIME kernelTime {};
	FILETIME userTime {};
	if (!GetThreadTimes(thread, &creationTime, &exitTime, &kernelTime, &userTime))
		return false;

	ULARGE_INTEGER kernel {};
	kernel.LowPart = kernelTime.dwLowDateTime;
	kernel.HighPart = kernelTime.dwHighDateTime;
	ULARGE_INTEGER user {};
	user.LowPart = userTime.dwLowDateTime;
	user.HighPart = userTime.dwHighDateTime;
	microseconds = (kernel.QuadPart + user.QuadPart) / 10;
	return true;
}
#endif

#if defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
bool readThreadCpuTimeMicroseconds(clockid_t clock, std::uint64_t& microseconds)
{
	timespec time {};
	if (clock_gettime(clock, &time) != 0)
		return false;
	microseconds = static_cast<std::uint64_t>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
	return true;
}
#endif

// Vblank callbacks cannot publish useful CPU time while the SH4 is stalled
// before its next vblank. Keep a real reference to the execution thread so the
// OSD thread can sample cumulative CPU time independently once per interval.
class Sh4ThreadCpuSampler
{
public:
	~Sh4ThreadCpuSampler()
	{
		clear();
	}

	void registerCurrentThread()
	{
#ifdef _WIN32
		HANDLE newThread = nullptr;
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
			&newThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
		const std::lock_guard<std::mutex> lock(mutex);
		releaseThreadLocked();
		thread = newThread;
#elif defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
		clockid_t newClock {};
		const bool newClockValid = pthread_getcpuclockid(pthread_self(), &newClock) == 0;
		const std::lock_guard<std::mutex> lock(mutex);
		clock = newClock;
		clockValid = newClockValid;
#else
		const std::lock_guard<std::mutex> lock(mutex);
#endif
		resetBaselineLocked();
	}

	void clear()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		releaseThreadLocked();
		baselineValid = false;
		baselineMicroseconds = 0;
	}

	void resetBaseline()
	{
		const std::lock_guard<std::mutex> lock(mutex);
		baselineValid = readRegisteredThreadLocked(baselineMicroseconds);
	}

	bool sample(std::uint64_t& elapsedMicroseconds)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		std::uint64_t currentMicroseconds = 0;
		if (!readRegisteredThreadLocked(currentMicroseconds))
		{
			baselineValid = false;
			elapsedMicroseconds = 0;
			return false;
		}
		if (!baselineValid || currentMicroseconds < baselineMicroseconds)
		{
			baselineMicroseconds = currentMicroseconds;
			baselineValid = true;
			elapsedMicroseconds = 0;
			return false;
		}

		elapsedMicroseconds = currentMicroseconds - baselineMicroseconds;
		baselineMicroseconds = currentMicroseconds;
		return true;
	}

private:
	bool readRegisteredThreadLocked(std::uint64_t& microseconds) const
	{
#ifdef _WIN32
		return thread != nullptr && readThreadCpuTimeMicroseconds(thread, microseconds);
#elif defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
		return clockValid && readThreadCpuTimeMicroseconds(clock, microseconds);
#else
		return false;
#endif
	}

	void resetBaselineLocked()
	{
		baselineValid = readRegisteredThreadLocked(baselineMicroseconds);
	}

	void releaseThreadLocked()
	{
#ifdef _WIN32
		if (thread != nullptr)
		{
			CloseHandle(thread);
			thread = nullptr;
		}
#elif defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
		clockValid = false;
#endif
	}

	std::mutex mutex;
#ifdef _WIN32
	HANDLE thread = nullptr;
#elif defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
	clockid_t clock {};
	bool clockValid = false;
#endif
	std::uint64_t baselineMicroseconds = 0;
	bool baselineValid = false;
};

Sh4ThreadCpuSampler sh4ThreadCpuSampler;

struct PresentSamples {
	std::mutex mutex;
	std::uint64_t lastPresentMicroseconds = 0;
	std::uint64_t intervalSumMicroseconds = 0;
	std::uint64_t intervalMinimumMicroseconds = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t intervalMaximumMicroseconds = 0;
	std::uint32_t presentCount = 0;
	std::uint32_t intervalCount = 0;
	std::array<std::uint64_t, FrameTimeHistorySize> intervalHistory {};
	std::uint32_t intervalHistoryCount = 0;
	std::uint32_t intervalHistoryNext = 0;
};

PresentSamples presentSamples;

struct MetricHistory {
	std::array<float, FrameTimeHistorySize> values {};
	std::uint32_t count = 0;
	std::uint32_t next = 0;
};

void appendHistory(MetricHistory& history, float value)
{
	history.values[history.next] = value;
	history.next = (history.next + 1) % FrameTimeHistorySize;
	history.count = std::min<std::uint32_t>(history.count + 1, FrameTimeHistorySize);
}

float historyValue(const MetricHistory& history, std::uint32_t index)
{
	const std::uint32_t first = (history.next + FrameTimeHistorySize - history.count) % FrameTimeHistorySize;
	return history.values[(first + index) % FrameTimeHistorySize];
}

struct Metrics {
	float gameFps = 0.f;
	float outputFps = 0.f;
	float vps = 0.f;
	float sh4Usage = 0.f;
	float pvrUsage = 0.f;
	float hostProcessCpuUsage = 0.f;
	float hostSystemCpuUsage = 0.f;
	float sh4ExecutionRate = 0.f;
	float sh4TimePerVblank = 0.f;
	float pvrTimePerVblank = 0.f;
	float gpuUsage = 0.f;
	float gpuTimePerRenderedFrame = 0.f;
	float pvrScheduledRenderPercent = 0.f;
	float pvrPrimitivesPerScene = 0.f;
	float pvrVerticesPerScene = 0.f;
	float rendererPassesPerScene = 0.f;
	float frameTimeAverage = 0.f;
	float frameTimeMinimum = 0.f;
	float frameTimeMaximum = 0.f;
	std::array<float, FrameTimeHistorySize> frameTimeHistory {};
	std::uint32_t frameTimeHistoryCount = 0;
	MetricHistory hostProcessCpuHistory;
	MetricHistory hostSystemCpuHistory;
	MetricHistory gpuUsageHistory;
	float textureUploadsPerSecond = 0.f;
	std::uint64_t processResidentBytes = 0;
	std::vector<gpu_memory::AdapterStats> gpuAdapters;
	std::uint64_t lastSampleTime = 0;
	std::uint64_t lastMemorySampleTime = 0;
	u64 lastTextureUploadCount = 0;
	bool processMemoryValid = false;
	bool hostProcessCpuValid = false;
	bool hostSystemCpuValid = false;
	bool sh4ThreadCpuValid = false;
	bool sh4TimePerVblankValid = false;
	bool frameTimeValid = false;
	bool gpuTimingSupported = false;
	bool gpuTimingValid = false;
};

Metrics metrics;

bool show(const char *key, bool defaultValue = true)
{
	return config::loadBool(Section, key, defaultValue);
}

void updateMetrics(bool queryGpuMemory)
{
	const std::uint64_t now = getTimeMs();
	if (metrics.lastMemorySampleTime == 0 || now - metrics.lastMemorySampleTime >= 1000)
	{
		const process_memory::Stats memory = process_memory::queryCurrentProcess();
		const cpu_usage::Stats cpu = cpu_usage::queryCurrentProcessAndSystem();
		metrics.processResidentBytes = memory.residentBytes;
		metrics.processMemoryValid = memory.valid;
		metrics.hostProcessCpuUsage = cpu.processPercent;
		metrics.hostSystemCpuUsage = cpu.systemPercent;
		metrics.hostProcessCpuValid = cpu.processValid;
		metrics.hostSystemCpuValid = cpu.systemValid;
		if (queryGpuMemory)
		{
			const gpu_memory::Stats gpu = gpu_memory::queryCurrentProcess();
			metrics.gpuAdapters = gpu.adapters;
		}
		else
			metrics.gpuAdapters.clear();
		metrics.lastMemorySampleTime = now;
	}
	if (metrics.lastSampleTime == 0)
	{
		metrics.lastSampleTime = now;
		metrics.lastTextureUploadCount = getTextureUploadCount();
		metrics.gameFps = 0.f;
		metrics.outputFps = 0.f;
		metrics.vps = 0.f;
		metrics.sh4Usage = 0.f;
		metrics.pvrUsage = 0.f;
		metrics.sh4ExecutionRate = 0.f;
		metrics.sh4TimePerVblank = 0.f;
		metrics.pvrTimePerVblank = 0.f;
		metrics.gpuUsage = 0.f;
		metrics.gpuTimePerRenderedFrame = 0.f;
		metrics.pvrScheduledRenderPercent = 0.f;
		metrics.pvrPrimitivesPerScene = 0.f;
		metrics.pvrVerticesPerScene = 0.f;
		metrics.rendererPassesPerScene = 0.f;
		metrics.textureUploadsPerSecond = 0.f;
		metrics.sh4ThreadCpuValid = false;
		metrics.sh4TimePerVblankValid = false;
		metrics.frameTimeValid = false;
		// The CPU sampler is delta-based. The call above establishes a new
		// baseline after Off; do not display the hidden interval it replaced.
		metrics.hostProcessCpuValid = false;
		metrics.hostSystemCpuValid = false;
		metrics.gpuTimingSupported = gpuTimingSupported.load(std::memory_order_relaxed);
		metrics.gpuTimingValid = false;
		metrics.frameTimeHistoryCount = 0;
		metrics.hostProcessCpuHistory = {};
		metrics.hostSystemCpuHistory = {};
		metrics.gpuUsageHistory = {};
		sh4ThreadCpuSampler.resetBaseline();
		pvrWorkMicroseconds.exchange(0);
		vblankCount.exchange(0);
		gameFrameCount.exchange(0);
		gpuTimingNanoseconds.exchange(0);
		gpuTimingSampleCount.exchange(0);
		pvrPrimitiveCount.exchange(0);
		pvrVertexCount.exchange(0);
		rendererSceneCount.exchange(0);
		rendererPassCount.exchange(0);
		pvrScheduledRenderCycles.exchange(0);
		pvrVideoFrameCycles.exchange(0);
		rawRenderWidth.store(0, std::memory_order_relaxed);
		rawRenderHeight.store(0, std::memory_order_relaxed);
		renderedWidth.store(0, std::memory_order_relaxed);
		renderedHeight.store(0, std::memory_order_relaxed);
		outputViewportWidth.store(0, std::memory_order_relaxed);
		outputViewportHeight.store(0, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(presentSamples.mutex);
		presentSamples.lastPresentMicroseconds = 0;
		presentSamples.intervalSumMicroseconds = 0;
		presentSamples.intervalMinimumMicroseconds = std::numeric_limits<std::uint64_t>::max();
		presentSamples.intervalMaximumMicroseconds = 0;
		presentSamples.presentCount = 0;
		presentSamples.intervalCount = 0;
		presentSamples.intervalHistory.fill(0);
		presentSamples.intervalHistoryCount = 0;
		presentSamples.intervalHistoryNext = 0;
		return;
	}

	const std::uint64_t elapsed = now - metrics.lastSampleTime;
	if (elapsed < 1000)
		return;

	std::uint32_t presentCount;
	std::uint32_t intervalCount;
	std::uint64_t intervalSum;
	std::uint64_t intervalMinimum;
	std::uint64_t intervalMaximum;
	{
		std::lock_guard<std::mutex> lock(presentSamples.mutex);
		presentCount = presentSamples.presentCount;
		intervalCount = presentSamples.intervalCount;
		intervalSum = presentSamples.intervalSumMicroseconds;
		intervalMinimum = presentSamples.intervalMinimumMicroseconds;
		intervalMaximum = presentSamples.intervalMaximumMicroseconds;
		presentSamples.presentCount = 0;
		presentSamples.intervalCount = 0;
		presentSamples.intervalSumMicroseconds = 0;
		presentSamples.intervalMinimumMicroseconds = std::numeric_limits<std::uint64_t>::max();
		presentSamples.intervalMaximumMicroseconds = 0;

		metrics.frameTimeHistoryCount = presentSamples.intervalHistoryCount;
		const std::uint32_t firstHistorySample =
			(presentSamples.intervalHistoryNext + FrameTimeHistorySize - metrics.frameTimeHistoryCount) % FrameTimeHistorySize;
		for (std::uint32_t i = 0; i < metrics.frameTimeHistoryCount; ++i)
		{
			const std::uint32_t historyIndex = (firstHistorySample + i) % FrameTimeHistorySize;
			metrics.frameTimeHistory[i] = static_cast<float>(presentSamples.intervalHistory[historyIndex]) / 1000.f;
		}
	}
	metrics.outputFps = static_cast<float>(presentCount) * 1000.f / static_cast<float>(elapsed);
	metrics.frameTimeValid = intervalCount != 0;
	if (metrics.frameTimeValid)
	{
		metrics.frameTimeAverage = static_cast<float>(intervalSum) / intervalCount / 1000.f;
		metrics.frameTimeMinimum = static_cast<float>(intervalMinimum) / 1000.f;
		metrics.frameTimeMaximum = static_cast<float>(intervalMaximum) / 1000.f;
	}
	const std::uint32_t vblanks = vblankCount.exchange(0);
	const std::uint32_t gameFrames = gameFrameCount.exchange(0);
	std::uint64_t sh4Work = 0;
	metrics.sh4ThreadCpuValid = sh4ThreadCpuSampler.sample(sh4Work);
	metrics.sh4TimePerVblankValid = metrics.sh4ThreadCpuValid && vblanks != 0;
	const std::uint64_t pvrWork = pvrWorkMicroseconds.exchange(0);
	metrics.vps = static_cast<float>(vblanks) * 1000.f / static_cast<float>(elapsed);
	metrics.gameFps = static_cast<float>(gameFrames) * 1000.f / static_cast<float>(elapsed);
	metrics.sh4Usage = metrics.sh4ThreadCpuValid
		? std::min(100.f, static_cast<float>(sh4Work) / (static_cast<float>(elapsed) * 1000.f) * 100.f)
		: 0.f;
	metrics.pvrUsage = std::min(100.f, static_cast<float>(pvrWork) / (static_cast<float>(elapsed) * 1000.f) * 100.f);
	metrics.sh4ExecutionRate = spg_getSh4ExecutionRate();
	const float vblankSamples = static_cast<float>(std::max(vblanks, 1u));
	metrics.sh4TimePerVblank = metrics.sh4TimePerVblankValid
		? static_cast<float>(sh4Work) / vblankSamples / 1000.f
		: 0.f;
	metrics.pvrTimePerVblank = static_cast<float>(pvrWork) / vblankSamples / 1000.f;
	const std::uint64_t gpuNanoseconds = gpuTimingNanoseconds.exchange(0);
	const std::uint32_t gpuSamples = gpuTimingSampleCount.exchange(0);
	metrics.gpuTimingSupported = gpuTimingSupported.load(std::memory_order_relaxed);
	metrics.gpuTimingValid = gpuSamples != 0;
	if (metrics.gpuTimingValid)
	{
		metrics.gpuTimePerRenderedFrame = static_cast<float>(gpuNanoseconds) / static_cast<float>(gpuSamples) / 1000000.f;
		metrics.gpuUsage = std::min(100.f, static_cast<float>(gpuNanoseconds) / (static_cast<float>(elapsed) * 1000000.f) * 100.f);
		appendHistory(metrics.gpuUsageHistory, metrics.gpuUsage);
	}
	if (metrics.hostProcessCpuValid)
		appendHistory(metrics.hostProcessCpuHistory, metrics.hostProcessCpuUsage);
	if (metrics.hostSystemCpuValid)
		appendHistory(metrics.hostSystemCpuHistory, metrics.hostSystemCpuUsage);
	const std::uint32_t rendererScenes = rendererSceneCount.exchange(0);
	const std::uint64_t pvrPrimitives = pvrPrimitiveCount.exchange(0);
	const std::uint64_t pvrVertices = pvrVertexCount.exchange(0);
	const std::uint32_t rendererPasses = rendererPassCount.exchange(0);
	const float scenes = static_cast<float>(std::max(rendererScenes, 1u));
	metrics.pvrPrimitivesPerScene = static_cast<float>(pvrPrimitives) / scenes;
	metrics.pvrVerticesPerScene = static_cast<float>(pvrVertices) / scenes;
	metrics.rendererPassesPerScene = static_cast<float>(rendererPasses) / scenes;
	const std::uint64_t scheduledRenderCycles = pvrScheduledRenderCycles.exchange(0);
	const std::uint64_t videoFrameCycles = pvrVideoFrameCycles.exchange(0);
	metrics.pvrScheduledRenderPercent = videoFrameCycles == 0
		? 0.f
		: static_cast<float>(scheduledRenderCycles) / static_cast<float>(videoFrameCycles) * 100.f;
	const u64 textureUploads = getTextureUploadCount();
	metrics.textureUploadsPerSecond = static_cast<float>(textureUploads - metrics.lastTextureUploadCount) * 1000.f / static_cast<float>(elapsed);
	metrics.lastTextureUploadCount = textureUploads;
	metrics.lastSampleTime = now;
}

std::string formatSpeed()
{
	const float targetFps = SPG_CONTROL.isPAL() ? 50.f : 60.f;
	const int currentSpeed = static_cast<int>(std::round(metrics.vps / targetFps * 100.f));
	char value[64];
	if (!settings.input.fastForwardMode)
		std::snprintf(value, sizeof(value), i18n::T("%d%% (set 100%%)"), currentSpeed);
	else
	{
		const int limit = config::FastForwardSpeedLimit.get();
		if (limit >= 300)
			std::snprintf(value, sizeof(value), i18n::T("%d%% (set Infinite)"), currentSpeed);
		else
			std::snprintf(value, sizeof(value), i18n::T("%d%% (set %d%%)"), currentSpeed, 100 + limit);
	}

	return value;
}

std::string formatFrameRate(bool includeRates, bool includeSpeed)
{
	std::string value;
	if (includeRates)
	{
		char rates[64];
		std::snprintf(rates, sizeof(rates), i18n::T("%.2f FPS | %.2f VPS"), metrics.gameFps, metrics.vps);
		value = rates;
	}
	if (includeSpeed)
	{
		if (!value.empty())
			value += " | ";
		else
			value = i18n::T("Speed: ");
		value += formatSpeed();
	}
	return value;
}

std::string formatOutputFrameRate()
{
	char rate[32];
	std::snprintf(rate, sizeof(rate), i18n::T("Output FPS: %.2f"), metrics.outputFps);
	return rate;
}

std::string formatResolution()
{
	const int width = renderedWidth.load(std::memory_order_relaxed);
	const int height = renderedHeight.load(std::memory_order_relaxed);
	const std::string rendered = width > 0 && height > 0
		? std::to_string(width) + "x" + std::to_string(height)
		: i18n::T("unavailable");
	const int rawWidth = rawRenderWidth.load(std::memory_order_relaxed);
	const int rawHeight = rawRenderHeight.load(std::memory_order_relaxed);
	const std::string raw = rawWidth > 0 && rawHeight > 0
		? std::to_string(rawWidth) + "x" + std::to_string(rawHeight)
		: i18n::T("unavailable");
	const std::string output = settings.display.width > 0 && settings.display.height > 0
		? std::to_string(settings.display.width) + "x" + std::to_string(settings.display.height)
		: i18n::T("unavailable");
	std::string value = i18n::T("Rendered: ") + rendered + i18n::T(" | Raw: ") + raw;
	if (config::IntegerScale)
	{
		const int viewportWidth = outputViewportWidth.load(std::memory_order_relaxed);
		const int viewportHeight = outputViewportHeight.load(std::memory_order_relaxed);
		if (viewportWidth > 0 && viewportHeight > 0
				&& (viewportWidth != settings.display.width || viewportHeight != settings.display.height))
			value += i18n::T(" | Viewport: ") + std::to_string(viewportWidth) + "x" + std::to_string(viewportHeight);
	}
	return value + i18n::T(" | Output: ") + output;
}

std::string formatGpuStatistics()
{
	const RenderType renderer = config::RendererType.get();
	const char *api = isOpenGL(renderer) ? "OpenGL" : isVulkan(renderer) ? "Vulkan" : isDirectX(renderer) ? "DirectX" : "Renderer";
	char statistics[192];
	std::snprintf(statistics, sizeof(statistics), i18n::T("%s%s HW | %.0f PVR P | %.0f V | %.1f RP | %.1f TU/s"), api,
		static_cast<bool>(config::ThreadedRendering) ? "-MT" : "", metrics.pvrPrimitivesPerScene,
		metrics.pvrVerticesPerScene, metrics.rendererPassesPerScene, metrics.textureUploadsPerSecond);
	return statistics;
}

std::string formatGpuTiming()
{
	if (!metrics.gpuTimingSupported)
		return i18n::T("Host GPU: unavailable (backend timing unsupported)");
	if (!metrics.gpuTimingValid)
		return i18n::T("Host GPU: sampling timestamp queries");
	char value[96];
	std::snprintf(value, sizeof(value), i18n::T("Host GPU: %.1f%% (%.2f ms/rendered frame)"), metrics.gpuUsage,
		metrics.gpuTimePerRenderedFrame);
	return value;
}

std::string formatPvrRenderSchedule()
{
	char value[128];
	std::snprintf(value, sizeof(value), i18n::T("Dreamcast PVR: %.1f%% scheduled frame pressure"),
		metrics.pvrScheduledRenderPercent);
	return value;
}

std::string formatBytes(std::uint64_t bytes)
{
	char value[32];
	constexpr double Mebibyte = 1024.0 * 1024.0;
	constexpr double Gibibyte = 1024.0 * Mebibyte;
	if (bytes >= static_cast<std::uint64_t>(Gibibyte))
		std::snprintf(value, sizeof(value), i18n::T("%.2f GiB"), bytes / Gibibyte);
	else
		std::snprintf(value, sizeof(value), i18n::T("%.1f MiB"), bytes / Mebibyte);
	return value;
}

std::string formatRam()
{
	const std::string host = metrics.processMemoryValid ? formatBytes(metrics.processResidentBytes) : i18n::T("unavailable");
	char value[96];
	std::snprintf(value, sizeof(value), i18n::T("HollyCast RAM (Resident): %s"), host.c_str());
	return value;
}

std::string formatVram()
{
	std::string value;
	for (const gpu_memory::AdapterStats& adapter : metrics.gpuAdapters)
	{
		if (!value.empty())
			value += " | ";
		char adapterValue[128];
		const std::string used = formatBytes(adapter.usedBytes);
		const std::string budget = formatBytes(adapter.budgetBytes);
		std::snprintf(adapterValue, sizeof(adapterValue), i18n::T("GPU %u VRAM (DXGI App): %s / %s"), adapter.adapterIndex,
			used.c_str(), budget.c_str());
		value += adapterValue;
	}
	if (value.empty())
		value = i18n::T("GPU VRAM: unavailable (no authoritative query)");
	return value;
}

std::string formatThreadingState()
{
	return static_cast<bool>(config::ThreadedRendering)
		? i18n::T("Threads: ON (SH4 + PVR separate)")
		: i18n::T("Threads: OFF (SH4 + PVR shared)");
}

std::string formatThreadWork(const char *label, float usage, float millisecondsPerVblank)
{
	char value[128];
	std::snprintf(value, sizeof(value), i18n::T("%s: %.1f%% (%.2f ms/vblank)"), label, usage, millisecondsPerVblank);
	return value;
}

std::string formatSh4Work()
{
#if defined(_WIN32) || defined(HOLLYCAST_HAS_PTHREAD_CPU_CLOCK)
	if (!metrics.sh4ThreadCpuValid)
		return i18n::T("Host CPU (SH4): sampling thread usage");
	if (!metrics.sh4TimePerVblankValid)
	{
		const char *label = config::ThreadedRendering
			? i18n::T("Host CPU (SH4 Thread)")
			: i18n::T("Host CPU (SH4 + PVR)");
		char value[128];
		std::snprintf(value, sizeof(value), i18n::T("%s: %.1f%% (no vblank)"), label, metrics.sh4Usage);
		return value;
	}
	if (config::ThreadedRendering)
		return formatThreadWork(i18n::T("Host CPU (SH4 Thread)"), metrics.sh4Usage, metrics.sh4TimePerVblank);
	return formatThreadWork(i18n::T("Host CPU (SH4 + PVR)"), metrics.sh4Usage, metrics.sh4TimePerVblank);
#else
	return i18n::T("SH4 CPU Work: unavailable (no thread CPU timer)");
#endif
}

std::string formatPvrWork()
{
#if defined(_WIN32) || defined(CLOCK_THREAD_CPUTIME_ID)
	return formatThreadWork(static_cast<bool>(config::ThreadedRendering) ? i18n::T("Host CPU (PVR Renderer)") : i18n::T("Host CPU (PVR Portion)"), metrics.pvrUsage,
		metrics.pvrTimePerVblank);
#else
	return i18n::T("PVR Renderer CPU Work: unavailable (no thread CPU timer)");
#endif
}

std::string formatHostCpu()
{
	if (!metrics.hostProcessCpuValid && !metrics.hostSystemCpuValid)
		return i18n::T("Host CPU: sampling process and device usage");
	char value[128];
	if (metrics.hostProcessCpuValid && metrics.hostSystemCpuValid)
		std::snprintf(value, sizeof(value), i18n::T("Host CPU: HollyCast %.1f%% | Device %.1f%%"), metrics.hostProcessCpuUsage,
			metrics.hostSystemCpuUsage);
	else if (metrics.hostProcessCpuValid)
		std::snprintf(value, sizeof(value), i18n::T("Host CPU: HollyCast %.1f%% of device capacity"), metrics.hostProcessCpuUsage);
	else
		std::snprintf(value, sizeof(value), i18n::T("Host CPU: Device %.1f%%"), metrics.hostSystemCpuUsage);
	return value;
}

std::string formatSh4ExecutionRate()
{
	if (metrics.sh4ExecutionRate <= 0.f)
		return i18n::T("Dreamcast SH4: sampling execution rate");
	char value[96];
	std::snprintf(value, sizeof(value), i18n::T("Dreamcast SH4: %.0f%% target execution rate"), metrics.sh4ExecutionRate);
	return value;
}

void drawFrameTimeGraph(ImDrawList *drawList, ImFont *font, float fontSize, const ImVec2& position, float width,
	float height, ImU32 textColor, float opacity)
{
	if (metrics.frameTimeHistoryCount < 2)
		return;

	float minimum = metrics.frameTimeHistory[0];
	float maximum = minimum;
	for (std::uint32_t i = 1; i < metrics.frameTimeHistoryCount; ++i)
	{
		minimum = std::min(minimum, metrics.frameTimeHistory[i]);
		maximum = std::max(maximum, metrics.frameTimeHistory[i]);
	}
	if (maximum - minimum < 4.f)
	{
		minimum = std::max(0.f, std::floor(minimum) - 2.f);
		maximum = std::ceil(maximum) + 2.f;
	}
	else
	{
		const float padding = (maximum - minimum) * 0.1f;
		minimum = std::max(0.f, minimum - padding);
		maximum += padding;
	}

	const float labelSize = std::max(10.f, fontSize * 0.65f);
	const float labelHeight = labelSize + uiScaled(2.f);
	const ImVec2 plotTopLeft = position + ImVec2(uiScaled(4.f), labelHeight);
	const ImVec2 plotBottomRight = position + ImVec2(width - uiScaled(4.f), height - labelHeight);
	const ImU32 backgroundColor = IM_COL32(0, 0, 0, static_cast<int>(64.f * opacity));
	drawList->AddRectFilled(position, position + ImVec2(width, height), backgroundColor, uiScaled(6.f));

	const float plotWidth = plotBottomRight.x - plotTopLeft.x;
	const float plotHeight = plotBottomRight.y - plotTopLeft.y;
	ImVec2 previous;
	for (std::uint32_t i = 0; i < metrics.frameTimeHistoryCount; ++i)
	{
		const float x = plotTopLeft.x + plotWidth * static_cast<float>(i) / static_cast<float>(metrics.frameTimeHistoryCount - 1);
		const float normalized = (metrics.frameTimeHistory[i] - minimum) / (maximum - minimum);
		const ImVec2 current(x, plotBottomRight.y - normalized * plotHeight);
		if (i != 0)
			drawList->AddLine(previous, current, textColor, std::max(1.f, uiScaled(1.f)));
		previous = current;
	}

	char maximumLabel[16];
	char minimumLabel[16];
	std::snprintf(maximumLabel, sizeof(maximumLabel), i18n::T("%.0f ms"), maximum);
	std::snprintf(minimumLabel, sizeof(minimumLabel), i18n::T("%.0f ms"), minimum);
	drawList->AddText(font, labelSize, position + ImVec2(uiScaled(4.f), 0.f), textColor, maximumLabel);
	const ImVec2 minimumSize = font->CalcTextSizeA(labelSize, FLT_MAX, 0.f, minimumLabel);
	drawList->AddText(font, labelSize, position + ImVec2(width - minimumSize.x - uiScaled(4.f), height - labelSize), textColor,
		minimumLabel);
}

void drawUsageGraph(ImDrawList *drawList, ImFont *font, float fontSize, const ImVec2& position, float width,
	float height, ImU32 primaryColor, float opacity, const char *title, const MetricHistory& primary,
	const MetricHistory *secondary = nullptr)
{
	if (primary.count < 2)
		return;

	const float labelSize = std::max(10.f, fontSize * 0.65f);
	const float labelHeight = labelSize + uiScaled(2.f);
	const ImVec2 plotTopLeft = position + ImVec2(uiScaled(4.f), labelHeight);
	const ImVec2 plotBottomRight = position + ImVec2(width - uiScaled(4.f), height - labelSize);
	const ImU32 backgroundColor = IM_COL32(0, 0, 0, static_cast<int>(64.f * opacity));
	const ImU32 secondaryColor = IM_COL32(92, 190, 255, static_cast<int>(255.f * opacity));
	drawList->AddRectFilled(position, position + ImVec2(width, height), backgroundColor, uiScaled(6.f));
	const float plotWidth = plotBottomRight.x - plotTopLeft.x;
	const float plotHeight = plotBottomRight.y - plotTopLeft.y;
	const float midY = plotBottomRight.y - plotHeight * 0.5f;
	drawList->AddLine(ImVec2(plotTopLeft.x, midY), ImVec2(plotBottomRight.x, midY), IM_COL32(255, 255, 255, static_cast<int>(36.f * opacity)));

	const auto drawHistory = [&](const MetricHistory& history, ImU32 color) {
		if (history.count < 2)
			return;
		ImVec2 previous;
		for (std::uint32_t i = 0; i < history.count; ++i)
		{
			const float x = plotTopLeft.x + plotWidth * static_cast<float>(i) / static_cast<float>(history.count - 1);
			const float normalized = std::clamp(historyValue(history, i), 0.f, 100.f) / 100.f;
			const ImVec2 current(x, plotBottomRight.y - normalized * plotHeight);
			if (i != 0)
				drawList->AddLine(previous, current, color, std::max(1.f, uiScaled(1.f)));
			previous = current;
		}
	};

	drawHistory(primary, primaryColor);
	if (secondary != nullptr)
		drawHistory(*secondary, secondaryColor);

	char label[112];
	if (secondary != nullptr && secondary->count != 0)
		std::snprintf(label, sizeof(label), i18n::T("%s: %.0f%% app | %.0f%% device"), title,
			historyValue(primary, primary.count - 1), historyValue(*secondary, secondary->count - 1));
	else
		std::snprintf(label, sizeof(label), i18n::T("%s: %.0f%%"), title, historyValue(primary, primary.count - 1));
	drawList->AddText(font, labelSize, position + ImVec2(uiScaled(4.f), 0.f), primaryColor, label);
	const char *zeroLabel = "0%";
	const ImVec2 zeroLabelSize = font->CalcTextSizeA(labelSize, FLT_MAX, 0.f, zeroLabel);
	drawList->AddText(font, labelSize, position + ImVec2(width - zeroLabelSize.x - uiScaled(4.f), height - labelSize), primaryColor, zeroLabel);
}

} // namespace

int mode()
{
	return std::clamp(config::loadInt(Section, "ResourceMonitorMode", 0), 0, 2);
}

bool isCollectionEnabled()
{
	return collectionEnabled.load(std::memory_order_acquire);
}

void resetSh4ThreadSample()
{
	sh4ThreadCpuSampler.registerCurrentThread();
}

void clearSh4ThreadSample()
{
	sh4ThreadCpuSampler.clear();
}

void recordSh4Vblank()
{
	if (isCollectionEnabled())
		vblankCount.fetch_add(1, std::memory_order_relaxed);
}

void recordPvrWork(std::uint64_t microseconds)
{
	if (isCollectionEnabled())
		pvrWorkMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
}

void recordRenderedResolution(int rawWidth, int rawHeight, int renderedWidth, int renderedHeight)
{
	if (!isCollectionEnabled())
		return;
	rawRenderWidth.store(rawWidth, std::memory_order_relaxed);
	rawRenderHeight.store(rawHeight, std::memory_order_relaxed);
	performance_overlay::renderedWidth.store(renderedWidth, std::memory_order_relaxed);
	performance_overlay::renderedHeight.store(renderedHeight, std::memory_order_relaxed);
}

void recordOutputViewport(int width, int height)
{
	if (!isCollectionEnabled())
		return;
	outputViewportWidth.store(width, std::memory_order_relaxed);
	outputViewportHeight.store(height, std::memory_order_relaxed);
}

void recordPresentedFrame()
{
	if (!isCollectionEnabled())
		return;
	const std::uint64_t now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
	std::lock_guard<std::mutex> lock(presentSamples.mutex);
	++presentSamples.presentCount;
	if (presentSamples.lastPresentMicroseconds != 0 && now > presentSamples.lastPresentMicroseconds)
	{
		const std::uint64_t interval = now - presentSamples.lastPresentMicroseconds;
		presentSamples.intervalSumMicroseconds += interval;
		presentSamples.intervalMinimumMicroseconds = std::min(presentSamples.intervalMinimumMicroseconds, interval);
		presentSamples.intervalMaximumMicroseconds = std::max(presentSamples.intervalMaximumMicroseconds, interval);
		++presentSamples.intervalCount;
		presentSamples.intervalHistory[presentSamples.intervalHistoryNext] = interval;
		presentSamples.intervalHistoryNext = (presentSamples.intervalHistoryNext + 1) % FrameTimeHistorySize;
		presentSamples.intervalHistoryCount = std::min<std::uint32_t>(presentSamples.intervalHistoryCount + 1, FrameTimeHistorySize);
	}
	presentSamples.lastPresentMicroseconds = now;
}

void recordPvrScene(bool displayScene, std::uint32_t primitiveCount, std::uint32_t vertexCount)
{
	if (!isCollectionEnabled())
		return;
	// This mirrors DuckStation's internal-frame counter: a completed PVR scene
	// for the Dreamcast display, excluding render-to-texture passes. It must not
	// be derived from Present(), which can repeat or omit a host output frame.
	if (displayScene)
		gameFrameCount.fetch_add(1, std::memory_order_relaxed);
	rendererSceneCount.fetch_add(1, std::memory_order_relaxed);
	pvrPrimitiveCount.fetch_add(primitiveCount, std::memory_order_relaxed);
	pvrVertexCount.fetch_add(vertexCount, std::memory_order_relaxed);
}

void recordRendererPass(std::uint32_t count)
{
	if (isCollectionEnabled())
		rendererPassCount.fetch_add(count, std::memory_order_relaxed);
}

void recordGpuTimingSample(std::uint64_t nanoseconds, std::uint32_t epoch)
{
	// Timestamp queries complete asynchronously. Accept only samples started in
	// the current enabled timing interval so an old query cannot contaminate a
	// later Off-to-On or metric re-enable interval.
	if (isCollectionEnabled() && isGpuTimingRequested()
			&& epoch == gpuTimingEpoch.load(std::memory_order_acquire))
	{
		gpuTimingNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
		gpuTimingSampleCount.fetch_add(1, std::memory_order_relaxed);
	}
}

void setGpuTimingSupported(bool supported)
{
	if (isCollectionEnabled())
		gpuTimingSupported.store(supported, std::memory_order_relaxed);
}

bool isGpuTimingRequested()
{
	return gpuTimingRequested.load(std::memory_order_acquire);
}

std::uint32_t getGpuTimingEpoch()
{
	return gpuTimingEpoch.load(std::memory_order_acquire);
}

void recordPvrRenderSchedule(std::uint32_t renderCycles)
{
	if (!isCollectionEnabled())
		return;
	pvrScheduledRenderCycles.fetch_add(renderCycles, std::memory_order_relaxed);
}

void recordPvrFrameBudget(std::uint32_t frameCycles)
{
	if (!isCollectionEnabled() || frameCycles == 0)
		return;
	pvrVideoFrameCycles.fetch_add(frameCycles, std::memory_order_relaxed);
}

std::uint64_t currentThreadCpuTimeMicroseconds()
{
#ifdef _WIN32
	FILETIME creationTime, exitTime, kernelTime, userTime;
	if (GetThreadTimes(GetCurrentThread(), &creationTime, &exitTime, &kernelTime, &userTime))
	{
		ULARGE_INTEGER kernel { kernelTime.dwLowDateTime, kernelTime.dwHighDateTime };
		ULARGE_INTEGER user { userTime.dwLowDateTime, userTime.dwHighDateTime };
		return (kernel.QuadPart + user.QuadPart) / 10;
	}
#elif defined(CLOCK_THREAD_CPUTIME_ID)
	timespec time {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0)
		return static_cast<std::uint64_t>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
#endif
	// Wall time includes sleeps and waits, so using it here would produce a
	// believable but incorrect workload percentage on unsupported platforms.
	return 0;
}

void draw()
{
	const int overlayMode = mode();
	if (overlayMode == 0)
	{
		if (collectionEnabled.exchange(false, std::memory_order_acq_rel))
		{
			metrics.lastSampleTime = 0;
			metrics.lastMemorySampleTime = 0;
			metrics.gpuAdapters.clear();
			gpuTimingRequested.store(false, std::memory_order_release);
			gpuTimingSupported.store(false, std::memory_order_relaxed);
			gpuTimingNanoseconds.store(0, std::memory_order_relaxed);
			gpuTimingSampleCount.store(0, std::memory_order_relaxed);
		}
		return;
	}
	// Publish collection before producers inspect the gate. updateMetrics()
	// resets every baseline on the first enabled draw, so no hidden interval is
	// included when diagnostics are re-enabled.
	if (!collectionEnabled.load(std::memory_order_relaxed))
	{
		collectionEnabled.store(true, std::memory_order_release);
	}
	const bool requestGpuTiming = overlayMode == 2
		&& (show("OverlayShowGpuUsage", true) || show("OverlayShowGpuGraph", false));
	const bool wasGpuTimingRequested = gpuTimingRequested.load(std::memory_order_relaxed);
	if (requestGpuTiming && !wasGpuTimingRequested)
	{
		// Publish the new generation before enabling producers. An acquire load of
		// gpuTimingRequested therefore observes the matching epoch.
		gpuTimingEpoch.fetch_add(1, std::memory_order_relaxed);
	}
	gpuTimingRequested.store(requestGpuTiming, std::memory_order_release);

	const bool showVram = overlayMode == 2 && show("OverlayShowVram", true);
	updateMetrics(showVram);

	std::vector<std::string> lines;
	char text[128];
	const auto add = [&lines](bool enabled, std::string value) {
		if (enabled)
			lines.push_back(std::move(value));
	};

	const bool showFrameRate = overlayMode == 1 || show("OverlayShowFPS");
	const bool showOutputFrameRate = overlayMode == 2 && show("OverlayShowOutputFPS", false);
	const bool showSpeed = overlayMode == 1 || show("OverlayShowEmulationSpeed");
	const bool showFrameTimes = overlayMode == 2 && show("OverlayShowFrameTimes");
	add(showFrameRate || showSpeed, formatFrameRate(showFrameRate, showSpeed));
	add(showOutputFrameRate, formatOutputFrameRate());
	if (overlayMode == 1)
	{
		add(true, formatHostCpu());
		add(true, formatSh4ExecutionRate());
		add(true, formatSh4Work());
		add(true, formatPvrWork());
	}
	else
	{
		add(show("OverlayShowHostCpuUsage", true), formatHostCpu());
		add(show("OverlayShowGpuUsage", true), formatGpuTiming());
		add(show("OverlayShowSh4ExecutionRate", true), formatSh4ExecutionRate());
		add(show("OverlayShowPvrTimingPressure", true), formatPvrRenderSchedule());
		add(show("OverlayShowThreadingState"), formatThreadingState());
		add(show("OverlayShowCpuUsage", false), formatSh4Work());
		add(show("OverlayShowPvrUsage", false), formatPvrWork());
		add(show("OverlayShowResolution"), formatResolution());
		if (showFrameTimes && metrics.frameTimeValid)
			std::snprintf(text, sizeof(text), i18n::T("Frame Times: %.2f ms Min | %.2f ms Avg | %.2f ms Max"), metrics.frameTimeMinimum,
				metrics.frameTimeAverage, metrics.frameTimeMaximum);
		else
			std::snprintf(text, sizeof(text), "%s", i18n::T("Frame Times: unavailable"));
		add(showFrameTimes, text);
		add(show("OverlayShowGpuStatistics"), formatGpuStatistics());
		add(show("OverlayShowLatencyStatistics", false), i18n::T("Latency: N/A (no end to end probe)"));
		add(showVram, formatVram());
		add(show("OverlayShowRam"), formatRam());
	}

	if (lines.empty())
		return;

	ImFont *font = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	const float fontSize = uiScaled(static_cast<float>(std::clamp(config::loadInt(Section, "OverlayTextSize", 16), 10, 48)));
	const float opacity = std::clamp(config::loadInt(Section, "OverlayTransparency", 85), 10, 100) / 100.f;
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const float margin = uiScaled(12.f);
	const float lineSpacing = uiScaled(1.f);
	const float lineHeight = fontSize + lineSpacing;
	const bool showFrameTimeGraph = showFrameTimes && show("OverlayShowFrameTimeGraph", true) && metrics.frameTimeHistoryCount >= 2;
	const bool showHostCpuGraph = overlayMode == 2 && show("OverlayShowHostCpuGraph", false)
		&& metrics.hostProcessCpuHistory.count >= 2;
	const bool showGpuGraph = overlayMode == 2 && show("OverlayShowGpuGraph", false) && metrics.gpuUsageHistory.count >= 2;
	const int graphCount = static_cast<int>(showFrameTimeGraph) + static_cast<int>(showHostCpuGraph) + static_cast<int>(showGpuGraph);
	const float graphSpacing = graphCount != 0 ? uiScaled(5.f) : 0.f;
	const float graphHeight = graphCount != 0 ? uiScaled(52.f) : 0.f;
	const float totalGraphHeight = graphCount == 0 ? 0.f : graphCount * (graphSpacing + graphHeight);
	const float totalHeight = lineHeight * lines.size() - lineSpacing + totalGraphHeight;
	std::vector<float> lineWidths;
	lineWidths.reserve(lines.size());
	float blockWidth = 0.f;
	for (const std::string& line : lines)
	{
		const float width = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, line.c_str()).x;
		lineWidths.push_back(width);
		blockWidth = std::max(blockWidth, width);
	}
	const int corner = std::clamp(config::loadInt(Section, "OverlayCorner", 1), 0, 3);
	const float offsetX = uiScaled(static_cast<float>(std::clamp(config::loadInt(Section, "OverlayOffsetX", 0), -1000, 1000)));
	const float offsetY = uiScaled(static_cast<float>(std::clamp(config::loadInt(Section, "OverlayOffsetY", 0), -1000, 1000)));
	float blockX = corner == 0 || corner == 2
		? margin
		: std::max(margin, displaySize.x - blockWidth - margin);
	blockX = std::clamp(blockX + offsetX, margin, std::max(margin, displaySize.x - blockWidth - margin));
	float y;
	if (corner == 0 || corner == 1)
		y = margin;
	else
		y = std::max(margin, displaySize.y - totalHeight - margin);
	y = std::clamp(y + offsetY, margin, std::max(margin, displaySize.y - totalHeight - margin));

	const u32 packedColor = static_cast<u32>(config::loadInt(Section, "OverlayColor", 0xFFFFFFFF));
	const ImU32 textColor = IM_COL32((packedColor >> 0) & 0xFF, (packedColor >> 8) & 0xFF, (packedColor >> 16) & 0xFF,
		static_cast<int>(((packedColor >> 24) & 0xFF) * opacity));
	const ImU32 shadowColor = IM_COL32(0, 0, 0, static_cast<int>(190.f * opacity));
	const float shadowOffset = std::max(1.f, uiScaled(0.75f));
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	for (std::size_t i = 0; i < lines.size(); ++i)
	{
		const float x = corner == 0 || corner == 2
			? blockX
			: blockX + blockWidth - lineWidths[i];
		const ImVec2 pos(x, y);
		drawList->AddText(font, fontSize, pos + ImVec2(shadowOffset, shadowOffset), shadowColor, lines[i].c_str());
		drawList->AddText(font, fontSize, pos, textColor, lines[i].c_str());
		y += lineHeight;
	}
	if (graphCount != 0)
	{
		const float graphWidth = std::clamp(uiScaled(200.f), uiScaled(120.f), std::max(uiScaled(120.f), displaySize.x - margin * 2.f));
		const float graphX = corner == 0 || corner == 2 ? blockX : displaySize.x - margin - graphWidth;
		auto drawNextGraph = [&](const auto& drawGraph) {
			y += graphSpacing;
			drawGraph(ImVec2(graphX, y));
			y += graphHeight;
		};
		if (showFrameTimeGraph)
			drawNextGraph([&](const ImVec2& position) { drawFrameTimeGraph(drawList, font, fontSize, position, graphWidth, graphHeight, textColor, opacity); });
		if (showHostCpuGraph)
			drawNextGraph([&](const ImVec2& position) { drawUsageGraph(drawList, font, fontSize, position, graphWidth, graphHeight, textColor, opacity,
				i18n::T("Host CPU"), metrics.hostProcessCpuHistory, metrics.hostSystemCpuHistory.count >= 2 ? &metrics.hostSystemCpuHistory : nullptr); });
		if (showGpuGraph)
			drawNextGraph([&](const ImVec2& position) { drawUsageGraph(drawList, font, fontSize, position, graphWidth, graphHeight, textColor, opacity,
				i18n::T("Host GPU"), metrics.gpuUsageHistory); });
	}
}

} // namespace performance_overlay
