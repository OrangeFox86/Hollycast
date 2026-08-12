/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.
*/
#include "gpu_memory.h"

#if defined(_WIN32) && defined(HAVE_D3D11)
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <mutex>
#include <utility>
#include <vector>
#endif

namespace gpu_memory {

#if defined(_WIN32) && defined(HAVE_D3D11)
namespace {

using Microsoft::WRL::ComPtr;

struct Adapter {
	std::uint32_t index;
	ComPtr<IDXGIAdapter3> dxgi;
};

struct AdapterCache {
	std::mutex mutex;
	ComPtr<IDXGIFactory1> factory;
	std::vector<Adapter> adapters;

	void refreshIfNeeded()
	{
		if (factory != nullptr && factory->IsCurrent())
			return;

		ComPtr<IDXGIFactory1> newFactory;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&newFactory))))
			return;

		std::vector<Adapter> newAdapters;
		for (UINT index = 0;; ++index)
		{
			ComPtr<IDXGIAdapter1> adapter;
			if (newFactory->EnumAdapters1(index, &adapter) != S_OK)
				break;

			DXGI_ADAPTER_DESC1 description {};
			if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
				continue;

			ComPtr<IDXGIAdapter3> adapter3;
			if (SUCCEEDED(adapter.As(&adapter3)))
				newAdapters.push_back({ index, std::move(adapter3) });
		}

		factory = std::move(newFactory);
		adapters = std::move(newAdapters);
	}
};

AdapterCache adapterCache;

} // namespace
#endif

Stats queryCurrentProcess()
{
#if defined(_WIN32) && defined(HAVE_D3D11)
	Stats stats;
	std::lock_guard<std::mutex> lock(adapterCache.mutex);
	adapterCache.refreshIfNeeded();
	for (const Adapter& adapter : adapterCache.adapters)
	{
		// Report every hardware adapter instead of guessing which adapter the
		// renderer uses. Each value comes directly from DXGI for this process.
		DXGI_QUERY_VIDEO_MEMORY_INFO info {};
		if (SUCCEEDED(adapter.dxgi->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
			stats.adapters.push_back({ adapter.index, static_cast<std::uint64_t>(info.CurrentUsage),
				static_cast<std::uint64_t>(info.Budget) });
	}
	return stats;
#endif
	return {};
}

} // namespace gpu_memory
