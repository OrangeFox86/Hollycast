/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "process_memory.h"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__) || defined(__ANDROID__)
#include <fstream>
#include <unistd.h>
#endif

namespace process_memory {

Stats queryCurrentProcess()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters {};
	if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
		return { static_cast<std::uint64_t>(counters.WorkingSetSize), true };
#elif defined(__APPLE__)
	mach_task_basic_info_data_t info {};
	mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
		return { static_cast<std::uint64_t>(info.resident_size), true };
#elif defined(__linux__) || defined(__ANDROID__)
	std::ifstream statm("/proc/self/statm");
	std::uint64_t totalPages = 0;
	std::uint64_t residentPages = 0;
	if (statm >> totalPages >> residentPages)
	{
		const long pageSize = sysconf(_SC_PAGESIZE);
		if (pageSize > 0)
			return { residentPages * static_cast<std::uint64_t>(pageSize), true };
	}
#endif
	return {};
}

} // namespace process_memory
