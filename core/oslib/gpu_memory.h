/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>

namespace gpu_memory {

struct Stats {
	std::uint64_t usedBytes = 0;
	std::uint64_t budgetBytes = 0;
	bool valid = false;
};

Stats queryCurrentProcess();

} // namespace gpu_memory
