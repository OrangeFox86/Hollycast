/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#pragma once

#include <cstdint>

namespace process_memory {

struct Stats {
	std::uint64_t residentBytes = 0;
	bool valid = false;
};

Stats queryCurrentProcess();

} // namespace process_memory
