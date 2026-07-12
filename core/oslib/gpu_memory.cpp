/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "gpu_memory.h"

#if defined(_WIN32) && defined(HAVE_D3D11)
#include <dxgi1_4.h>
#endif

namespace gpu_memory {

Stats queryCurrentProcess()
{
#if defined(_WIN32) && defined(HAVE_D3D11)
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
		return {};

	Stats stats;
	for (UINT index = 0;; ++index)
	{
		IDXGIAdapter1 *adapter = nullptr;
		if (factory->EnumAdapters1(index, &adapter) != S_OK)
			break;
		DXGI_ADAPTER_DESC1 description {};
		if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
		{
			adapter->Release();
			continue;
		}

		IDXGIAdapter3 *adapter3 = nullptr;
		if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter3))))
		{
			adapter->Release();
			continue;
		}

		// Report every hardware adapter instead of guessing which adapter the
		// renderer uses. Each value comes directly from DXGI for this process.
		DXGI_QUERY_VIDEO_MEMORY_INFO info {};
		if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
			stats.adapters.push_back({ index, static_cast<std::uint64_t>(info.CurrentUsage),
				static_cast<std::uint64_t>(info.Budget) });
		adapter3->Release();
		adapter->Release();
	}
	factory->Release();
	return stats;
#endif
	return {};
}

} // namespace gpu_memory
