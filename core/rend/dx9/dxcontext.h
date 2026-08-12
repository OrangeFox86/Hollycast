/*
	Copyright 2021 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "build.h"
#if defined(_WIN32) && !defined(TARGET_UWP)
#include "types.h"
#include <windows.h>
#include <d3d9.h>
#include "windows/comptr.h"
#include "windows/dynlink.h"
#include "d3d_overlay.h"
#include "wsi/context.h"

class DXContext : public GraphicsContext
{
public:
	static void Create(void *window, void *display);
	static DXContext *Instance() { return static_cast<DXContext *>(GraphicsContext::Instance()); }
	~DXContext();
	void EndImGuiFrame();
	void Present();
	void BeginGpuTiming();
	void EndGpuTiming();
	void resize() override;

	const ComPtr<IDirect3D9>& getD3D() const {
		return pD3D;
	}
	const ComPtr<IDirect3DDevice9>& getDevice() const {
		return pDevice;
	}
	void setOverlay(bool overlayOnly) {
		this->overlayOnly = overlayOnly;
	}
	std::string getDriverName() override {
		return driverName;
	}
	std::string getDriverVersion() override {
		return driverVersion;
	}
	bool isAMD() override {
		return amd;
	}
	void setFrameRendered() {
		frameRendered = true;
	}
	bool isReady() const {
		return deviceReady;
	}

private:
	DXContext(void *window, void *display);
	bool init(bool keepCurrentWindow = false);
	void term();
	void resetDevice();
	bool initGpuTiming();
	void termGpuTiming();
	void pollGpuTiming();

	WinLibLoader d3d9Library{ "D3D9.DLL" };
	ComPtr<IDirect3D9> pD3D;
	ComPtr<IDirect3DDevice9> pDevice;
	D3DPRESENT_PARAMETERS d3dpp{};
	bool overlayOnly = false;
	D3DOverlay overlay;
	bool swapOnVSync = false;
	bool frameRendered = false;
	std::string driverName;
	std::string driverVersion;
	bool amd = false;
	bool deviceReady = false;

	struct GpuTimingQuery
	{
		ComPtr<IDirect3DQuery9> disjoint;
		ComPtr<IDirect3DQuery9> frequency;
		ComPtr<IDirect3DQuery9> start;
		ComPtr<IDirect3DQuery9> end;
		u32 epoch = 0;
	};
	static constexpr std::size_t GpuTimingQueryCount = 4;
	std::array<GpuTimingQuery, GpuTimingQueryCount> gpuTimingQueries {};
	std::size_t gpuTimingReadIndex = 0;
	std::size_t gpuTimingWriteIndex = 0;
	std::size_t gpuTimingWaitingCount = 0;
	bool gpuTimingActive = false;
	bool gpuTimingAvailable = false;
};

class DX9GpuTimingScope
{
public:
	explicit DX9GpuTimingScope(DXContext *context, bool enabled)
		: context(enabled ? context : nullptr)
	{
		if (this->context != nullptr)
			this->context->BeginGpuTiming();
	}

	~DX9GpuTimingScope()
	{
		if (context != nullptr)
			context->EndGpuTiming();
	}
	DX9GpuTimingScope(const DX9GpuTimingScope&) = delete;
	DX9GpuTimingScope& operator=(const DX9GpuTimingScope&) = delete;

private:
	DXContext *const context;
};
#endif
