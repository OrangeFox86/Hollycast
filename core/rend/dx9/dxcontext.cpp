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
#include "dxcontext.h"
#include "d3d_renderer.h"
#include "rend/osd.h"
#ifdef USE_SDL
#include "sdl/sdl.h"
#endif
#include "hw/pvr/Renderer_if.h"
#include "emulator.h"
#include "dx9_driver.h"
#include "imgui_impl_dx9.h"
#include "ui/performance_overlay.h"

void DXContext::Create(void *window, void *display) {
	new DXContext(window, display);
}

DXContext::DXContext(void *window, void *display)
	: GraphicsContext(window, display)
{
	if (!init())
		throw FlycastException("DX9 initialization failed");
}

DXContext::~DXContext() {
	term();
}

bool DXContext::init(bool keepCurrentWindow)
{
	NOTICE_LOG(RENDERER, "DX9 Context initializing");
#ifdef USE_SDL
	if (!keepCurrentWindow && !sdl_recreate_window(0)) {
		term();
		return false;
	}
#endif

	decltype(Direct3DCreate9) *pDirect3DCreate9 = nullptr;
	pDirect3DCreate9 = d3d9Library.getFunc("Direct3DCreate9", pDirect3DCreate9);
	if (pDirect3DCreate9 == nullptr)
	{
		ERROR_LOG(RENDERER, "Cannot load D3D9.DLL");
		term();
		return false;
	}

	pD3D.reset(pDirect3DCreate9(D3D_SDK_VERSION));
	if (!pD3D)
	{
		ERROR_LOG(RENDERER, "Direct3DCreate9 failed");
		term();
		return false;
	}
	memset(&d3dpp, 0, sizeof(d3dpp));
	d3dpp.hDeviceWindow = (HWND)window;
	d3dpp.Windowed = true;
	d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
	d3dpp.EnableAutoDepthStencil = FALSE;						// No need for depth/stencil buffer for the backbuffer
	swapOnVSync = !settings.input.fastForwardMode && config::VSync;
	if (swapOnVSync)
		d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE; // Only one supported in Windowed mode
	else
		d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
	// TODO should be 0 in windowed mode
	//d3dpp.FullScreen_RefreshRateInHz = swapOnVSync ? 60 : 0;
	HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, (HWND)window,
			D3DCREATE_HARDWARE_VERTEXPROCESSING, &d3dpp, &pDevice.get());
	if (FAILED(hr))
	{
		ERROR_LOG(RENDERER, "DirectX9 device creation failed: %x", hr);
		term();
	    return false;
	}
	imguiDriver = std::unique_ptr<ImGuiDriver>(new DX9Driver(pDevice));
	overlay.init(pDevice);
	initGpuTiming();

	D3DADAPTER_IDENTIFIER9 id;
	pD3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
	driverName = std::string(id.Description);
	driverVersion = std::to_string(id.DriverVersion.HighPart >> 16) + "." + std::to_string((u16)id.DriverVersion.HighPart)
		+ "." + std::to_string(id.DriverVersion.LowPart >> 16) + "." + std::to_string((u16)id.DriverVersion.LowPart);
	amd = id.VendorId == 0x1002 || id.VendorId == 0x1022;
	deviceReady = true;

	return true;
}

void DXContext::term()
{
	NOTICE_LOG(RENDERER, "DX9 Context terminating");
	termGpuTiming();
	overlay.term();
	imguiDriver.reset();
	pDevice.reset();
	pD3D.reset();
	deviceReady = false;
}

bool DXContext::initGpuTiming()
{
	gpuTimingQueries = {};
	gpuTimingReadIndex = 0;
	gpuTimingWriteIndex = 0;
	gpuTimingWaitingCount = 0;
	gpuTimingActive = false;
	gpuTimingAvailable = false;
	if (!pDevice)
		return false;

	for (GpuTimingQuery& query : gpuTimingQueries)
	{
		if (FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &query.disjoint.get()))
				|| FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &query.frequency.get()))
				|| FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query.start.get()))
				|| FAILED(pDevice->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &query.end.get())))
		{
			gpuTimingQueries = {};
			return false;
		}
	}
	gpuTimingAvailable = true;
	return true;
}

void DXContext::termGpuTiming()
{
	gpuTimingActive = false;
	gpuTimingAvailable = false;
	gpuTimingReadIndex = 0;
	gpuTimingWriteIndex = 0;
	gpuTimingWaitingCount = 0;
	gpuTimingQueries = {};
	performance_overlay::setGpuTimingSupported(false);
}

void DXContext::pollGpuTiming()
{
	if (!gpuTimingAvailable)
		return;

	while (gpuTimingWaitingCount != 0)
	{
		GpuTimingQuery& query = gpuTimingQueries[gpuTimingReadIndex];
		BOOL disjoint = TRUE;
		const HRESULT disjointResult = query.disjoint->GetData(&disjoint, sizeof(disjoint), 0);
		if (disjointResult == S_FALSE)
			break;
		if (FAILED(disjointResult))
		{
			gpuTimingAvailable = false;
			performance_overlay::setGpuTimingSupported(false);
			return;
		}

		UINT64 start = 0;
		UINT64 end = 0;
		UINT64 frequency = 0;
		const HRESULT startResult = query.start->GetData(&start, sizeof(start), 0);
		const HRESULT endResult = query.end->GetData(&end, sizeof(end), 0);
		const HRESULT frequencyResult = query.frequency->GetData(&frequency, sizeof(frequency), 0);
		if (startResult == S_FALSE || endResult == S_FALSE || frequencyResult == S_FALSE)
			break;
		if (FAILED(startResult) || FAILED(endResult) || FAILED(frequencyResult))
		{
			gpuTimingAvailable = false;
			performance_overlay::setGpuTimingSupported(false);
			return;
		}

		if (!disjoint && end >= start && frequency != 0)
		{
			const double nanoseconds = static_cast<double>(end - start) * 1000000000.0 / static_cast<double>(frequency);
			performance_overlay::recordGpuTimingSample(static_cast<std::uint64_t>(nanoseconds), query.epoch);
		}
		gpuTimingReadIndex = (gpuTimingReadIndex + 1) % GpuTimingQueryCount;
		--gpuTimingWaitingCount;
	}
}

void DXContext::BeginGpuTiming()
{
	// Retire completed queries even while timing is disabled so samples from a
	// previous enabled interval cannot leak into a later one.
	pollGpuTiming();
	if (!performance_overlay::isGpuTimingRequested())
		return;
	performance_overlay::setGpuTimingSupported(gpuTimingAvailable);
	if (!gpuTimingAvailable || !pDevice)
		return;

	if (gpuTimingActive || gpuTimingWaitingCount == GpuTimingQueryCount)
		return;

	GpuTimingQuery& query = gpuTimingQueries[gpuTimingWriteIndex];
	const u32 epoch = performance_overlay::getGpuTimingEpoch();
	if (FAILED(query.disjoint->Issue(D3DISSUE_BEGIN)) || FAILED(query.start->Issue(D3DISSUE_END)))
	{
		gpuTimingAvailable = false;
		performance_overlay::setGpuTimingSupported(false);
		return;
	}
	query.epoch = epoch;
	gpuTimingActive = true;
}

void DXContext::EndGpuTiming()
{
	if (!gpuTimingActive || !pDevice)
		return;

	GpuTimingQuery& query = gpuTimingQueries[gpuTimingWriteIndex];
	if (FAILED(query.end->Issue(D3DISSUE_END)) || FAILED(query.frequency->Issue(D3DISSUE_END))
			|| FAILED(query.disjoint->Issue(D3DISSUE_END)))
	{
		gpuTimingAvailable = false;
		performance_overlay::setGpuTimingSupported(false);
	}
	else
	{
		gpuTimingWriteIndex = (gpuTimingWriteIndex + 1) % GpuTimingQueryCount;
		++gpuTimingWaitingCount;
	}
	gpuTimingActive = false;
}

void DXContext::Present()
{
	if (!frameRendered)
		return;
	if (!pDevice)
	{
		if (init(true))
		{
			renderer = new D3DRenderer();
			rend_init_renderer();
		}
		return;
	}
	HRESULT result = pDevice->Present(NULL, NULL, NULL, NULL);
	// Handle loss of D3D9 device
	if (result == D3DERR_DEVICELOST)
	{
		deviceReady = false;
		result = pDevice->TestCooperativeLevel();
		if (result == D3DERR_DEVICENOTRESET)
			resetDevice();
	}
	else if (FAILED(result))
		WARN_LOG(RENDERER, "Present failed %x", result);
	else
	{
		frameRendered = false;
		if (swapOnVSync != (!settings.input.fastForwardMode && config::VSync))
		{
			DEBUG_LOG(RENDERER, "Switch vsync %d", !swapOnVSync);
			rend_term_renderer();
			term();
			if (init(true))
			{
				renderer = new D3DRenderer();
				rend_init_renderer();
			}
			else
			{
				deviceReady = false;
			}
		}
	}
}

void DXContext::EndImGuiFrame()
{
	if (deviceReady)
	{
		verify((bool)pDevice);
		pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
		pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		pDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		if (!overlayOnly)
		{
			pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_RGBA(0, 0, 0, 255), 1.0f, 0);
			if (renderer != nullptr)
				renderer->RenderLastFrame();
		}
		if (SUCCEEDED(pDevice->BeginScene()))
		{
			if (overlayOnly)
				overlay.draw(settings.display.width, settings.display.height, config::FloatVMUs, true);
			ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			pDevice->EndScene();
		}
	}
	frameRendered = true;
}

void DXContext::resize()
{
	if (!pDevice)
		return;
	RECT rect;
	GetClientRect((HWND)window, &rect);
	d3dpp.BackBufferWidth = settings.display.width = rect.right;
	d3dpp.BackBufferHeight = settings.display.height = rect.bottom;
	if (settings.display.width == 0 || settings.display.height == 0)
		// window minimized
		return;
	resetDevice();
}

void DXContext::resetDevice()
{
	D3DRenderer *dxrenderer{};
	if (renderer != nullptr)
		dxrenderer = dynamic_cast<D3DRenderer*>(renderer);
	if (dxrenderer != nullptr)
		dxrenderer->preReset();
	// D3D9 query objects become invalid with a lost device. Release the full
	// timing ring before Reset so stale queries cannot block later samples.
	termGpuTiming();
	overlay.term();
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = pDevice->Reset(&d3dpp);
    if (FAILED(hr))
    {
        ERROR_LOG(RENDERER, "DX9 device reset failed: %x", hr);
        deviceReady = false;
        return;
    }
    deviceReady = true;
    ImGui_ImplDX9_CreateDeviceObjects();
    overlay.init(pDevice);
	initGpuTiming();
	if (dxrenderer != nullptr)
		dxrenderer->postReset();
}
