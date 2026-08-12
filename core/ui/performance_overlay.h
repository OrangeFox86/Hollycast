/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>

namespace performance_overlay {

int mode();
void draw();

// Producers use this atomic gate before collecting diagnostics. It is
// intentionally independent of config access so disabled collection stays
// cheap on emulation and renderer hot paths.
bool isCollectionEnabled();

void resetSh4ThreadSample();
void clearSh4ThreadSample();
void recordSh4Vblank();
void recordPvrWork(std::uint64_t microseconds);
void recordRenderedResolution(int rawWidth, int rawHeight, int renderedWidth, int renderedHeight);
void recordOutputViewport(int width, int height);
void recordPresentedFrame();
void recordPvrScene(bool displayScene, std::uint32_t primitiveCount, std::uint32_t vertexCount);
void recordRendererPass(std::uint32_t count = 1);
void recordGpuTimingSample(std::uint64_t nanoseconds, std::uint32_t epoch);
void setGpuTimingSupported(bool supported);
bool isGpuTimingRequested();
std::uint32_t getGpuTimingEpoch();
void recordPvrRenderSchedule(std::uint32_t renderCycles);
void recordPvrFrameBudget(std::uint32_t frameCycles);
std::uint64_t currentThreadCpuTimeMicroseconds();

} // namespace performance_overlay
