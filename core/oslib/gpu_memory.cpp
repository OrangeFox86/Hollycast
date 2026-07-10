/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "gpu_memory.h"

#if defined(_WIN32) && defined(HAVE_D3D11)
#include <dxgi1_6.h>
#endif

namespace gpu_memory {

Stats queryCurrentProcess()
{
#if defined(_WIN32) && defined(HAVE_D3D11)
	IDXGIFactory6 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void **>(&factory))))
		return {};

	for (UINT index = 0;; ++index)
	{
		IDXGIAdapter1 *adapter = nullptr;
		if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
			__uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&adapter)) != S_OK)
			break;

		IDXGIAdapter3 *adapter3 = nullptr;
		if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter3))))
		{
			adapter->Release();
			continue;
		}

		// DXGI reports this process's local segment usage, which is the closest
		// Windows API value to actual application VRAM ownership.
		DXGI_QUERY_VIDEO_MEMORY_INFO info {};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
		{
			adapter3->Release();
			adapter->Release();
			factory->Release();
			return { static_cast<std::uint64_t>(info.CurrentUsage), static_cast<std::uint64_t>(info.Budget), true };
		}
		adapter3->Release();
		adapter->Release();
	}
	factory->Release();
#endif
	return {};
}

} // namespace gpu_memory
