/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>

namespace cpu_usage {

struct Stats {
	float processPercent = 0.f;
	float systemPercent = 0.f;
	std::uint32_t logicalProcessorCount = 0;
	bool processValid = false;
	bool systemValid = false;
};

// Returns deltas since the prior call. Process usage is normalized against all
// logical processors, while system usage includes every process on the device.
Stats queryCurrentProcessAndSystem();

} // namespace cpu_usage
