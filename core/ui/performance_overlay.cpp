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
#include "oslib/gpu_memory.h"
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
#include <time.h>
#endif

namespace performance_overlay {

namespace {

constexpr const char *Section = "rend";

std::atomic<std::uint64_t> sh4WorkMicroseconds { 0 };
std::atomic<std::uint64_t> pvrWorkMicroseconds { 0 };
std::atomic<std::uint32_t> vblankCount { 0 };
std::atomic<int> rawRenderWidth { 0 };
std::atomic<int> rawRenderHeight { 0 };
std::atomic<int> renderedWidth { 0 };
std::atomic<int> renderedHeight { 0 };
std::atomic<int> outputViewportWidth { 0 };
std::atomic<int> outputViewportHeight { 0 };
thread_local std::uint64_t lastSh4ThreadCpuTime = 0;

struct PresentSamples {
	std::mutex mutex;
	std::uint64_t lastPresentMicroseconds = 0;
	std::uint64_t intervalSumMicroseconds = 0;
	std::uint64_t intervalMinimumMicroseconds = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t intervalMaximumMicroseconds = 0;
	std::uint32_t presentCount = 0;
	std::uint32_t intervalCount = 0;
};

PresentSamples presentSamples;

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
	std::vector<gpu_memory::AdapterStats> gpuAdapters;
	std::uint64_t lastSampleTime = 0;
	std::uint64_t lastMemorySampleTime = 0;
	u64 lastTextureUploadCount = 0;
	bool processMemoryValid = false;
	bool frameTimeValid = false;
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
		const gpu_memory::Stats gpu = gpu_memory::queryCurrentProcess();
		metrics.gpuAdapters = gpu.adapters;
		metrics.lastMemorySampleTime = now;
	}
	if (metrics.lastSampleTime == 0)
	{
		metrics.lastSampleTime = now;
		metrics.lastTextureUploadCount = getTextureUploadCount();
		sh4WorkMicroseconds.exchange(0);
		pvrWorkMicroseconds.exchange(0);
		vblankCount.exchange(0);
		return;
	}

	const std::uint64_t elapsed = now - metrics.lastSampleTime;
	if (elapsed < 500)
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
	}
	metrics.fps = static_cast<float>(presentCount) * 1000.f / static_cast<float>(elapsed);
	metrics.frameTimeValid = intervalCount != 0;
	if (metrics.frameTimeValid)
	{
		metrics.frameTimeAverage = static_cast<float>(intervalSum) / intervalCount / 1000.f;
		metrics.frameTimeMinimum = static_cast<float>(intervalMinimum) / 1000.f;
		metrics.frameTimeMaximum = static_cast<float>(intervalMaximum) / 1000.f;
	}
	metrics.vps = static_cast<float>(vblankCount.exchange(0)) * 1000.f / static_cast<float>(elapsed);
	metrics.sh4Usage = std::min(100.f, static_cast<float>(sh4WorkMicroseconds.exchange(0)) / (static_cast<float>(elapsed) * 1000.f) * 100.f);
	metrics.pvrUsage = std::min(100.f, static_cast<float>(pvrWorkMicroseconds.exchange(0)) / (static_cast<float>(elapsed) * 1000.f) * 100.f);
	const u64 textureUploads = getTextureUploadCount();
	metrics.textureUploadsPerSecond = static_cast<float>(textureUploads - metrics.lastTextureUploadCount) * 1000.f / static_cast<float>(elapsed);
	metrics.lastTextureUploadCount = textureUploads;
	metrics.lastSampleTime = now;
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
	const int width = renderedWidth.load(std::memory_order_relaxed);
	const int height = renderedHeight.load(std::memory_order_relaxed);
	const std::string rendered = width > 0 && height > 0
		? std::to_string(width) + "x" + std::to_string(height)
		: "unavailable";
	const int rawWidth = rawRenderWidth.load(std::memory_order_relaxed);
	const int rawHeight = rawRenderHeight.load(std::memory_order_relaxed);
	const std::string raw = rawWidth > 0 && rawHeight > 0
		? std::to_string(rawWidth) + "x" + std::to_string(rawHeight)
		: "unavailable";
	const std::string output = settings.display.width > 0 && settings.display.height > 0
		? std::to_string(settings.display.width) + "x" + std::to_string(settings.display.height)
		: "unavailable";
	std::string value = "Rendered: " + rendered + " | Raw: " + raw;
	if (config::IntegerScale)
	{
		const int viewportWidth = outputViewportWidth.load(std::memory_order_relaxed);
		const int viewportHeight = outputViewportHeight.load(std::memory_order_relaxed);
		if (viewportWidth > 0 && viewportHeight > 0
				&& (viewportWidth != settings.display.width || viewportHeight != settings.display.height))
			value += " | Viewport: " + std::to_string(viewportWidth) + "x" + std::to_string(viewportHeight);
	}
	return value + " | Output: " + output;
}

std::string formatGpuStatistics()
{
	const RenderType renderer = config::RendererType.get();
	const char *api = isOpenGL(renderer) ? "OpenGL" : isVulkan(renderer) ? "Vulkan" : isDirectX(renderer) ? "DirectX" : "Renderer";
	char statistics[128];
	std::snprintf(statistics, sizeof(statistics), "GPU API: %s | Tracked Tex %u | Upload %.1f/s | Time unavailable", api,
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
	return "Host RAM (Resident): " + host + " | DC RAM: " + std::to_string(RAM_SIZE / 1_MB) + " MiB";
}

std::string formatVram()
{
	std::string value;
	for (const gpu_memory::AdapterStats& adapter : metrics.gpuAdapters)
	{
		if (!value.empty())
			value += " | ";
		value += "GPU " + std::to_string(adapter.adapterIndex) + " VRAM (DXGI App): "
			+ formatBytes(adapter.usedBytes) + " / " + formatBytes(adapter.budgetBytes);
	}
	if (value.empty())
		value = "GPU VRAM: unavailable (no authoritative query)";
	return value + " | DC VRAM: " + std::to_string(VRAM_SIZE / 1_MB) + " MiB";
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

void recordRenderedResolution(int rawWidth, int rawHeight, int renderedWidth, int renderedHeight)
{
	rawRenderWidth.store(rawWidth, std::memory_order_relaxed);
	rawRenderHeight.store(rawHeight, std::memory_order_relaxed);
	performance_overlay::renderedWidth.store(renderedWidth, std::memory_order_relaxed);
	performance_overlay::renderedHeight.store(renderedHeight, std::memory_order_relaxed);
}

void recordOutputViewport(int width, int height)
{
	outputViewportWidth.store(width, std::memory_order_relaxed);
	outputViewportHeight.store(height, std::memory_order_relaxed);
}

void recordPresentedFrame()
{
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
	}
	presentSamples.lastPresentMicroseconds = now;
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
		return;

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
		if (metrics.frameTimeValid)
			std::snprintf(text, sizeof(text), "Present Intervals: %.2f ms avg (%.2f-%.2f)", metrics.frameTimeAverage,
				metrics.frameTimeMinimum, metrics.frameTimeMaximum);
		else
			std::snprintf(text, sizeof(text), "Present Intervals: unavailable");
		add(show("OverlayShowFrameTimes"), text);
		add(show("OverlayShowGpuStatistics"), formatGpuStatistics());
		add(show("OverlayShowLatencyStatistics"), "Latency: N/A (no end to end probe)");
		add(show("OverlayShowVram"), formatVram());
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
	const float totalHeight = lineHeight * lines.size() - lineSpacing;
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
}

} // namespace performance_overlay
