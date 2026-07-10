/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "performance_overlay.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "gui_util.h"
#include "hw/pvr/spg.h"
#include "imgui.h"
#include "mainui.h"
#include "oslib/process_memory.h"
#include "rend/TexCache.h"
#include "stdclass.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace performance_overlay {

namespace {

constexpr const char *Section = "rend";

std::atomic<std::uint64_t> sh4WorkMicroseconds { 0 };
std::atomic<std::uint64_t> pvrWorkMicroseconds { 0 };
std::atomic<std::uint32_t> vblankCount { 0 };
thread_local std::uint64_t lastSh4ThreadCpuTime = 0;

struct Metrics {
	float fps = 0.f;
	float vps = 0.f;
	float sh4Usage = 0.f;
	float pvrUsage = 0.f;
	float frameTimeAverage = 0.f;
	float frameTimeMinimum = 0.f;
	float frameTimeMaximum = 0.f;
	float textureUploadsPerSecond = 0.f;
	std::uint64_t processResidentBytes = 0;
	std::array<float, 120> frameTimes {};
	std::size_t frameTimePosition = 0;
	std::size_t frameTimeCount = 0;
	std::uint64_t lastFrameTime = 0;
	std::uint64_t lastSampleTime = 0;
	std::uint64_t lastMemorySampleTime = 0;
	u32 lastFrameCount = 0;
	u64 lastTextureUploadCount = 0;
	bool processMemoryValid = false;
};

Metrics metrics;

bool show(const char *key, bool defaultValue = true)
{
	return config::loadBool(Section, key, defaultValue);
}

void updateMetrics()
{
	const std::uint64_t now = getTimeMs();
	if (metrics.lastMemorySampleTime == 0 || now - metrics.lastMemorySampleTime >= 1000)
	{
		const process_memory::Stats memory = process_memory::queryCurrentProcess();
		metrics.processResidentBytes = memory.residentBytes;
		metrics.processMemoryValid = memory.valid;
		metrics.lastMemorySampleTime = now;
	}
	if (metrics.lastSampleTime == 0)
	{
		metrics.lastSampleTime = now;
		metrics.lastFrameCount = MainFrameCount;
		metrics.lastTextureUploadCount = getTextureUploadCount();
		sh4WorkMicroseconds.exchange(0);
		pvrWorkMicroseconds.exchange(0);
		vblankCount.exchange(0);
		return;
	}

	const std::uint64_t elapsed = now - metrics.lastSampleTime;
	if (elapsed < 500)
		return;

	metrics.fps = static_cast<float>(MainFrameCount - metrics.lastFrameCount) * 1000.f / static_cast<float>(elapsed);
	metrics.vps = static_cast<float>(vblankCount.exchange(0)) * 1000.f / static_cast<float>(elapsed);
	metrics.sh4Usage = std::min(100.f, static_cast<float>(sh4WorkMicroseconds.exchange(0)) / (static_cast<float>(elapsed) * 1000.f) * 100.f);
	metrics.pvrUsage = std::min(100.f, static_cast<float>(pvrWorkMicroseconds.exchange(0)) / (static_cast<float>(elapsed) * 1000.f) * 100.f);
	const u64 textureUploads = getTextureUploadCount();
	metrics.textureUploadsPerSecond = static_cast<float>(textureUploads - metrics.lastTextureUploadCount) * 1000.f / static_cast<float>(elapsed);
	metrics.lastTextureUploadCount = textureUploads;
	metrics.lastSampleTime = now;
	metrics.lastFrameCount = MainFrameCount;
}

std::string formatSpeed()
{
	const float targetFps = SPG_CONTROL.isPAL() ? 50.f : 60.f;
	const float currentSpeed = metrics.vps / targetFps * 100.f;
	if (!settings.input.fastForwardMode)
		return "Speed: " + std::to_string(static_cast<int>(std::round(currentSpeed))) + "% (set 100%)";

	const int limit = config::FastForwardSpeedLimit.get();
	if (limit >= 300)
		return "Speed: " + std::to_string(static_cast<int>(std::round(currentSpeed))) + "% (set Infinite)";
	return "Speed: " + std::to_string(static_cast<int>(std::round(currentSpeed))) + "% (set " + std::to_string(100 + limit) + "%)";
}

std::string formatResolution()
{
	const int internalHeight = std::max(1, config::RenderResolution.get());
	int internalWidth = static_cast<int>(std::round(640.f * internalHeight / 480.f));
	if (config::Widescreen)
	{
		const float aspect = config::SuperWidescreen
			? static_cast<float>(settings.display.width) / std::max(1, settings.display.height)
			: 16.f / 9.f;
		internalWidth = static_cast<int>(std::round(internalWidth * aspect / (4.f / 3.f)));
	}
	return "Resolution: " + std::to_string(internalWidth) + "x" + std::to_string(internalHeight)
		+ " -> " + std::to_string(settings.display.width) + "x" + std::to_string(settings.display.height);
}

std::string formatGpuStatistics()
{
	const RenderType renderer = config::RendererType.get();
	const char *api = isOpenGL(renderer) ? "OpenGL" : isVulkan(renderer) ? "Vulkan" : isDirectX(renderer) ? "DirectX" : "Renderer";
	char statistics[128];
	std::snprintf(statistics, sizeof(statistics), "GPU: %s | Tex %u | Upload %.1f/s | Time N/A", api,
		getTrackedTextureCount(), metrics.textureUploadsPerSecond);
	return statistics;
}

std::string formatBytes(std::uint64_t bytes)
{
	char value[32];
	constexpr double Mebibyte = 1024.0 * 1024.0;
	constexpr double Gibibyte = 1024.0 * Mebibyte;
	if (bytes >= static_cast<std::uint64_t>(Gibibyte))
		std::snprintf(value, sizeof(value), "%.2f GiB", bytes / Gibibyte);
	else
		std::snprintf(value, sizeof(value), "%.1f MiB", bytes / Mebibyte);
	return value;
}

std::string formatRam()
{
	const std::string host = metrics.processMemoryValid ? formatBytes(metrics.processResidentBytes) : "unavailable";
	return "Host RAM: " + host + " | DC: " + std::to_string(RAM_SIZE / 1_MB) + " MiB";
}

std::string formatVram()
{
	return "Texture VRAM Est: " + formatBytes(getEstimatedTextureVramUsageBytes())
		+ " | DC: " + std::to_string(VRAM_SIZE / 1_MB) + " MiB";
}

std::string formatThreadingState()
{
	return static_cast<bool>(config::ThreadedRendering)
		? "Threads: ON (SH4 + PVR separate)"
		: "Threads: OFF (SH4 + PVR shared)";
}

std::string formatSh4Work()
{
#if defined(_WIN32) || defined(CLOCK_THREAD_CPUTIME_ID)
	if (config::ThreadedRendering)
		return "SH4 Thread: " + std::to_string(static_cast<int>(std::round(metrics.sh4Usage))) + "% CPU core";
	return "SH4 + PVR Thread: " + std::to_string(static_cast<int>(std::round(metrics.sh4Usage))) + "% CPU core";
#else
	return "SH4 CPU Work: unavailable (no thread CPU timer)";
#endif
}

std::string formatPvrWork()
{
#if defined(_WIN32) || defined(CLOCK_THREAD_CPUTIME_ID)
	return std::string(static_cast<bool>(config::ThreadedRendering) ? "PVR Thread: " : "PVR Portion: ")
		+ std::to_string(static_cast<int>(std::round(metrics.pvrUsage))) + "% CPU core";
#else
	return "PVR Renderer CPU Work: unavailable (no thread CPU timer)";
#endif
}

void updateFrameTime(float frameTime)
{
	metrics.frameTimes[metrics.frameTimePosition] = frameTime;
	metrics.frameTimePosition = (metrics.frameTimePosition + 1) % metrics.frameTimes.size();
	metrics.frameTimeCount = std::min(metrics.frameTimeCount + 1, metrics.frameTimes.size());
	metrics.frameTimeAverage = 0.f;
	metrics.frameTimeMinimum = FLT_MAX;
	metrics.frameTimeMaximum = 0.f;
	for (std::size_t i = 0; i < metrics.frameTimeCount; ++i)
	{
		metrics.frameTimeAverage += metrics.frameTimes[i];
		metrics.frameTimeMinimum = std::min(metrics.frameTimeMinimum, metrics.frameTimes[i]);
		metrics.frameTimeMaximum = std::max(metrics.frameTimeMaximum, metrics.frameTimes[i]);
	}
	if (metrics.frameTimeCount != 0)
		metrics.frameTimeAverage /= static_cast<float>(metrics.frameTimeCount);
}

} // namespace

int mode()
{
	return std::clamp(config::loadInt(Section, "ResourceMonitorMode", 2), 0, 2);
}

void resetSh4ThreadSample()
{
	lastSh4ThreadCpuTime = currentThreadCpuTimeMicroseconds();
}

void sampleSh4Thread()
{
	vblankCount.fetch_add(1, std::memory_order_relaxed);
	const std::uint64_t current = currentThreadCpuTimeMicroseconds();
	if (lastSh4ThreadCpuTime != 0 && current >= lastSh4ThreadCpuTime)
		sh4WorkMicroseconds.fetch_add(current - lastSh4ThreadCpuTime, std::memory_order_relaxed);
	lastSh4ThreadCpuTime = current;
}

void recordPvrWork(std::uint64_t microseconds)
{
	pvrWorkMicroseconds.fetch_add(microseconds, std::memory_order_relaxed);
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

void recordFrame()
{
	const std::uint64_t now = getTimeMs();
	if (metrics.lastFrameTime != 0 && now > metrics.lastFrameTime)
		updateFrameTime(static_cast<float>(now - metrics.lastFrameTime));
	metrics.lastFrameTime = now;
}

void draw()
{
	const int overlayMode = mode();
	if (overlayMode == 0)
		return;

	recordFrame();
	updateMetrics();

	std::vector<std::string> lines;
	char text[128];
	const auto add = [&lines](bool enabled, std::string value) {
		if (enabled)
			lines.push_back(std::move(value));
	};

	std::snprintf(text, sizeof(text), "FPS: %.1f", metrics.fps);
	add(overlayMode == 1 || show("OverlayShowFPS"), text);
	if (overlayMode == 1)
	{
		add(true, formatSh4Work());
		add(true, formatPvrWork());
	}
	else
	{
		add(show("OverlayShowEmulationSpeed"), formatSpeed());
		add(show("OverlayShowThreadingState"), formatThreadingState());
		add(show("OverlayShowCpuUsage"), formatSh4Work());
		add(show("OverlayShowPvrUsage"), formatPvrWork());
		add(show("OverlayShowGpuUsage"), "Hardware GPU: N/A (backend timing needed)");
		add(show("OverlayShowResolution"), formatResolution());
		std::snprintf(text, sizeof(text), "Frame Times: %.2f ms avg (%.2f-%.2f)", metrics.frameTimeAverage,
			metrics.frameTimeMinimum, metrics.frameTimeMaximum);
		add(show("OverlayShowFrameTimes"), text);
		add(show("OverlayShowGpuStatistics"), formatGpuStatistics());
		add(show("OverlayShowLatencyStatistics"), "Latency: N/A (no end to end probe)");
		add(show("OverlayShowVram"), formatVram());
		add(show("OverlayShowRam"), formatRam());
	}

	if (lines.empty())
		return;

	std::string message;
	for (const std::string &line : lines)
	{
		if (!message.empty())
			message += '\n';
		message += line;
	}

	const float fontSize = uiScaled(static_cast<float>(std::clamp(config::loadInt(Section, "OverlayTextSize", 18), 10, 48)));
	const float opacity = std::clamp(config::loadInt(Section, "OverlayTransparency", 85), 10, 100) / 100.f;
	const ImVec2 padding(uiScaled(10.f), uiScaled(8.f));
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const float margin = uiScaled(16.f);
	const float wrapWidth = std::max(uiScaled(120.f), displaySize.x - margin * 2.f - padding.x * 2.f);
	const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, message.c_str()) + padding * 2.f;
	const int corner = std::clamp(config::loadInt(Section, "OverlayCorner", 1), 0, 3);
	ImVec2 pos;
	if (corner == 0 || corner == 2)
		pos.x = margin;
	else
		pos.x = std::max(margin, displaySize.x - size.x - margin);
	if (corner == 0 || corner == 1)
		pos.y = margin;
	else
		pos.y = std::max(margin, displaySize.y - size.y - margin);

	const u32 packedColor = static_cast<u32>(config::loadInt(Section, "OverlayColor", 0xFFFFFFFF));
	const ImU32 textColor = IM_COL32((packedColor >> 0) & 0xFF, (packedColor >> 8) & 0xFF, (packedColor >> 16) & 0xFF,
		static_cast<int>(((packedColor >> 24) & 0xFF) * opacity));
	const ImU32 backgroundColor = IM_COL32(20, 20, 20, static_cast<int>(190.f * opacity));
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	drawList->AddRectFilled(pos, pos + size, backgroundColor, uiScaled(6.f));
	drawList->AddText(nullptr, fontSize, pos + padding, textColor, message.c_str(), nullptr, wrapWidth);
}

} // namespace performance_overlay
