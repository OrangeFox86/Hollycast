/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>

namespace performance_overlay {

int mode();
void draw();
void recordFrame();

void resetSh4ThreadSample();
void sampleSh4Thread();
void recordPvrWork(std::uint64_t microseconds);
void recordRenderedResolution(int rawWidth, int rawHeight, int renderedWidth, int renderedHeight);
std::uint64_t currentThreadCpuTimeMicroseconds();

} // namespace performance_overlay
