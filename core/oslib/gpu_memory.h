/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>
#include <vector>

namespace gpu_memory {

struct AdapterStats {
	std::uint32_t adapterIndex = 0;
	std::uint64_t usedBytes = 0;
	std::uint64_t budgetBytes = 0;
};

struct Stats {
	std::vector<AdapterStats> adapters;
};

Stats queryCurrentProcess();

} // namespace gpu_memory
