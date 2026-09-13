/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#if defined(__ANDROID__) || defined(TARGET_IPHONE)

#include "vgamepad.h"
#include "gui.h"
#include "stdclass.h"
#include "imgui.h"
#include "rend/osd.h"
#include "imgui_driver.h"
#include "input/gamepad.h"
#include "input/gamepad_device.h"
#include "oslib/storage.h"
#include "oslib/resources.h"
#include "oslib/i18n.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "input/mouse.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/card_reader.h"
#include "hw/maple/maple_devs.h"
#include "stbi.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace vgamepad
{
static void stopEditing(bool canceled);
static void loadLayout();
static Element controlToElement(ControlId control);
static int getControlTransparency(ControlId control);
static float getControlHitPadding(ControlId control);
static float getFingerHitPadding();

struct Control
{
	Control() = default;
	Control(float x, float y, float w = 64.f, float h = 64.f)
		: pos(x, y), size(w, h), uv0(0, 0), uv1(1, 1) {}

	ImVec2 pos;
	ImVec2 size;
	ImVec2 uv0;
	ImVec2 uv1;
	bool disabled = false;
};
static Control Controls[_Count];
static bool Visible = true;
static bool Editing;
static bool serviceMode;
static float AlphaTrans = 1.f;
static ImVec2 StickPos;	// analog stick position [-1, 1]
constexpr float REFERENCE_WIDTH = 640.f;
constexpr float REFERENCE_HEIGHT = 480.f;
constexpr float MIN_ELEMENT_SCALE = 0.5f;
constexpr float MAX_ELEMENT_SCALE = 1.8f;
constexpr float MAX_TOUCH_PADDING = 24.f;
constexpr int MAX_PERCENT = 100;
constexpr char const *BTN_PATH = "picture/buttons.png";
constexpr char const *BTN_PATH_ARCADE = "picture/buttons-arcade.png";
constexpr char const *CFG_SECTION = "vgamepad";

void displayCommands()
{
	draw();
    centerNextWindow();

    ImGui::Begin("##vgamepad", nullptr, ImGuiWindowFlags_NoDecoration);

	if (ImGui::Button(i18n::T("Save"), ScaledVec2(150, 50)))
	{
		stopEditing(false);
		gui_setState(GuiState::Settings);
	}
	ImGui::SameLine();
	if (ImGui::Button(i18n::T("Reset"), ScaledVec2(150, 50)))
	{
		resetEditing();
		startEditing();
		gui_setState(GuiState::VJoyEdit);
	}

	ImGui::SameLine();
	if (ImGui::Button(i18n::T("Cancel"), ScaledVec2(150, 50)))
	{
		stopEditing(true);
		gui_setState(GuiState::Settings);
	}
    ImGui::End();
}

static const char *getButtonsResPath() {
	return settings.platform.isConsole() ? BTN_PATH : BTN_PATH_ARCADE;
}

static const char *getButtonsCfgName() {
	return settings.platform.isConsole() ? "image" : "image_arcade";
}

static bool loadOSDButtons(const std::string& path)
{
	if (path.empty())
		return false;
	hostfs::File *file = hostfs::storage().openFile(path, "rb");
	if (file == nullptr)
		return false;

	stbi_set_flip_vertically_on_load_thread(1);
    int width, height, n;
	u8 *image_data = stbi_load_from_file(file, &width, &height, &n, STBI_rgb_alpha);
	delete file;
	if (image_data == nullptr)
		return false;
    try {
        imguiDriver->updateTexture(getButtonsResPath(), image_data, width, height, false);
    } catch (...) {
        // vulkan can throw during resizing
    }
    free(image_data);

    return true;
}

static ImTextureID loadOSDButtons()
{
	ImTextureID id{};
	// custom image
	std::string path = config::loadStr(CFG_SECTION, getButtonsCfgName());
	if (loadOSDButtons(path))
		return id;
	if (settings.platform.isConsole())
	{
		// legacy buttons.png in data folder
		if (loadOSDButtons(get_readonly_data_path("buttons.png")))
			return id;
		// also try the home folder (android)
		if (loadOSDButtons(get_readonly_config_path("buttons.png")))
			return id;
	}
	// default in resource
	size_t size;
	std::unique_ptr<u8[]> data = resource::load(getButtonsResPath(), size);
	stbi_set_flip_vertically_on_load_thread(1);
	int width, height, n;
	u8 *image_data = stbi_load_from_memory(data.get(), (int)size, &width, &height, &n, STBI_rgb_alpha);
    if (image_data != nullptr)
    {
        try {
            id = imguiDriver->updateTexture(getButtonsResPath(), image_data, width, height, false);
        } catch (...) {
            // vulkan can throw during resizing
        }
        free(image_data);
    }
    return id;
}

ImTextureID ImguiVGamepadTexture::getId()
{
	ImTextureID id = imguiDriver->getTexture(getButtonsResPath());
	if (id == ImTextureID())
		id = loadOSDButtons();

	return id;
}

constexpr float vjoy_tex[_Count][4] = {
	// L
	{   0,   0,  64,  64 },
	// U
	{  64,   0,  64,  64 },
	// R
	{ 128,   0,  64,  64 },
	// D
	{ 192,   0,  64,  64 },
	// Y, btn3
	{ 256,   0,  64,  64 },
	// X, btn2
	{ 320,   0,  64,  64 },
	// B, btn1
	{ 384,   0,  64,  64 },
	// A, btn0
	{ 448,   0,  64,  64 },

	// Start
	{   0,  64,  64,  64 },
	// LT
	{  64,  64,  90,  64 },
	// RT
	{ 154,  64,  90,  64 },
	// Analog
	{ 244,  64, 128, 128 },
	// Stick
	{ 372,  64,  64,  64 },
	// Fast forward
	{ 436,  64,  64,  64 },

	// C, btn4
	{   0, 128,  64,  64 },
	// Z, btn5
	{  64, 128,  64,  64 },

	// service mode
	{   0, 192,  64,  64 },
	// insert card
	{  64, 192,  64,  64 },

	// Special controls
	// service
	{ 128, 128,  64,  64 },
	// coin
	{ 384, 128,  64,  64 },
	// test
	{ 448, 128,  64,  64 },
};

static ImVec2 coinUV0, coinUV1;
static ImVec2 serviceUV0, serviceUV1;
static ImVec2 testUV0, testUV1;

constexpr float OSD_TEX_W = 512.f;
constexpr float OSD_TEX_H = 256.f;

static void setUV()
{
	int i = 0;

	for (auto& control : Controls)
	{
		control.uv0.x = (vjoy_tex[i][0] + 1) / OSD_TEX_W;
		control.uv0.y = 1.f - (vjoy_tex[i][1] + 1) / OSD_TEX_H;
		control.uv1.x = (vjoy_tex[i][0] + vjoy_tex[i][2] - 1) / OSD_TEX_W;
		control.uv1.y = 1.f - (vjoy_tex[i][1] + vjoy_tex[i][3] - 1) / OSD_TEX_H;
		i++;
		if (i >= _VisibleCount)
			break;
	}
	serviceUV0.x = (vjoy_tex[i][0] + 1) / OSD_TEX_W;
	serviceUV0.y = 1.f - (vjoy_tex[i][1] + 1) / OSD_TEX_H;
	serviceUV1.x = (vjoy_tex[i][0] + vjoy_tex[i][2] - 1) / OSD_TEX_W;
	serviceUV1.y = 1.f - (vjoy_tex[i][1] + vjoy_tex[i][3] - 1) / OSD_TEX_H;
	i++;
	coinUV0.x = (vjoy_tex[i][0] + 1) / OSD_TEX_W;
	coinUV0.y = 1.f - (vjoy_tex[i][1] + 1) / OSD_TEX_H;
	coinUV1.x = (vjoy_tex[i][0] + vjoy_tex[i][2] - 1) / OSD_TEX_W;
	coinUV1.y = 1.f - (vjoy_tex[i][1] + vjoy_tex[i][3] - 1) / OSD_TEX_H;
	i++;
	testUV0.x = (vjoy_tex[i][0] + 1) / OSD_TEX_W;
	testUV0.y = 1.f - (vjoy_tex[i][1] + 1) / OSD_TEX_H;
	testUV1.x = (vjoy_tex[i][0] + vjoy_tex[i][2] - 1) / OSD_TEX_W;
	testUV1.y = 1.f - (vjoy_tex[i][1] + vjoy_tex[i][3] - 1) / OSD_TEX_H;
	i++;

}
static OnLoad _(&setUV);

void show() {
	Visible = true;
}

void hide() {
	Visible = false;
}

ControlId hitTest(float x, float y)
{
	if (!config::VirtualGamepadEnabled && !Editing)
		return None;

	ControlId closestControl = None;
	float closestDistanceSquared = std::numeric_limits<float>::max();
	const float fingerPadding = getFingerHitPadding();
	for (const auto& control : Controls)
	{
		const ControlId controlId = static_cast<ControlId>(&control - &Controls[0]);
		const float padding = getControlHitPadding(controlId) + fingerPadding;
		if (control.disabled
				|| x < control.pos.x - padding || x >= control.pos.x + control.size.x + padding
				|| y < control.pos.y - padding || y >= control.pos.y + control.size.y + padding)
			continue;

		// Expanded targets can overlap. Prefer the control whose original bounds
		// are nearest to the touch instead of relying on enum order.
		const float nearestX = std::clamp(x, control.pos.x, control.pos.x + control.size.x);
		const float nearestY = std::clamp(y, control.pos.y, control.pos.y + control.size.y);
		const float dx = x - nearestX;
		const float dy = y - nearestY;
		const float distanceSquared = dx * dx + dy * dy;
		if (distanceSquared < closestDistanceSquared)
		{
			closestControl = controlId;
			closestDistanceSquared = distanceSquared;
		}
	}
	return closestControl;
}

static u32 buttonMap[_Count] {
	DC_DPAD_LEFT,
	DC_DPAD_UP,
	DC_DPAD_RIGHT,
	DC_DPAD_DOWN,
	DC_BTN_X,
	DC_BTN_Y,
	DC_BTN_B,
	DC_BTN_A,
	DC_BTN_START,
	DC_AXIS_LT,
	DC_AXIS_RT,
	0,			// not used: analog area
	0,			// not used: analog stick
	EMU_BTN_FFORWARD,
	DC_BTN_Y,	// button 4
	DC_BTN_Z,	// button 5
	EMU_BTN_SRVMODE,
	DC_BTN_INSERT_CARD,
	DC_DPAD_LEFT | DC_DPAD_UP,
	DC_DPAD_RIGHT | DC_DPAD_UP,
	DC_DPAD_LEFT | DC_DPAD_DOWN,
	DC_DPAD_RIGHT | DC_DPAD_DOWN,
};

void setButtonMap()
{
	const bool arcade = settings.platform.isArcade();
	if (serviceMode)
	{
		buttonMap[A] = DC_BTN_D;
		buttonMap[B] = DC_DPAD2_UP;
		buttonMap[X] = DC_DPAD2_DOWN;
	}
	else
	{
		buttonMap[A] = DC_BTN_A;
		buttonMap[B] = DC_BTN_B;
		buttonMap[X] = arcade ? DC_BTN_C : DC_BTN_X;
	}
	buttonMap[Y] = arcade ? DC_BTN_X : DC_BTN_Y;
}

u32 controlToDcKey(ControlId control)
{
	if (control >= Left && control < _Count)
		return buttonMap[control];
	else
		return 0;
}

void setAnalogStick(float x, float y) {
	StickPos.x = x;
	StickPos.y = y;
}

float getControlWidth(ControlId control) {
	return Controls[control].size.x;	
}

void toggleServiceMode()
{
	serviceMode = !serviceMode;
	if (serviceMode)
	{
		Controls[A].disabled = false;
		Controls[B].disabled = false;
		Controls[X].disabled = false;
		setButtonMap();
	}
	else {
		startGame();
	}
}

static void drawButtonDim(ImDrawList *drawList, const Control& control, int state)
{
	if (control.disabled)
		return;
	if (!config::VirtualGamepadVisualFeedback)
		state = 0;
	float scale_h = settings.display.height / REFERENCE_HEIGHT;
	float offs_x = (settings.display.width - scale_h * REFERENCE_WIDTH) / 2.f;
	ImVec2 pos = control.pos * scale_h;
	ImVec2 size = control.size * scale_h;
	pos.x += offs_x;
	ControlId controlId = static_cast<ControlId>(&control - &Controls[0]);
	if (controlId == AnalogStick)
		pos += StickPos * size;

	float col = (0.5f - 0.25f * state / 255) * AlphaTrans;
	const float globalAlpha = (MAX_PERCENT - std::clamp(static_cast<int>(config::VirtualGamepadTransparency), 0, MAX_PERCENT))
			/ static_cast<float>(MAX_PERCENT);
	const float elementAlpha = (MAX_PERCENT - getControlTransparency(controlId)) / static_cast<float>(MAX_PERCENT);
	float alpha = globalAlpha * elementAlpha * AlphaTrans;
	ImVec4 color(col, col, col, alpha);

	const ImVec2* uv0 = &control.uv0;
	const ImVec2* uv1 = &control.uv1;
	if (serviceMode)
		switch (controlId)
		{
		case A:
			uv0 = &coinUV0;
			uv1 = &coinUV1;
			break;
		case B:
			uv0 = &serviceUV0;
			uv1 = &serviceUV1;
			break;
		case X:
			uv0 = &testUV0;
			uv1 = &testUV1;
			break;
		default:
			break;
		}

    ImguiVGamepadTexture tex;
	tex.draw(drawList, pos, size, *uv0, *uv1, color);
}

static void drawButton(ImDrawList *drawList, const Control& control, bool state) {
	drawButtonDim(drawList, control, state ? 0 : 255);
}

void draw()
{
	if (Controls[Left].pos.x == 0.f)
	{
		loadLayout();
		if (Controls[Left].pos.x == 0.f)
			// mark done
			Controls[Left].pos.x = 1e-12f;
	}
	if (!config::VirtualGamepadEnabled && !Editing)
		return;
		
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	drawButton(drawList, Controls[Left], kcode[0] & buttonMap[Left]);
	drawButton(drawList, Controls[Up], kcode[0] & buttonMap[Up]);
	drawButton(drawList, Controls[Right], kcode[0] & buttonMap[Right]);
	drawButton(drawList, Controls[Down], kcode[0] & buttonMap[Down]);

	drawButton(drawList, Controls[X], kcode[0] & buttonMap[X]);
	drawButton(drawList, Controls[Y], kcode[0] & buttonMap[Y]);
	drawButton(drawList, Controls[B], kcode[0] & buttonMap[B]);
	drawButton(drawList, Controls[A], kcode[0] & buttonMap[A]);

	drawButton(drawList, Controls[Start], kcode[0] & buttonMap[Start]);

	drawButtonDim(drawList, Controls[LeftTrigger], lt[0] >> 8);

	drawButtonDim(drawList, Controls[RightTrigger], rt[0] >> 8);

	drawButton(drawList, Controls[AnalogArea], true);
	drawButton(drawList, Controls[AnalogStick], false);

	drawButton(drawList, Controls[FastForward], false);

	drawButton(drawList, Controls[Btn4], kcode[0] & buttonMap[Btn4]);
	drawButton(drawList, Controls[Btn5], kcode[0] & buttonMap[Btn5]);
	drawButton(drawList, Controls[ServiceMode], !serviceMode);
	drawButton(drawList, Controls[InsertCard], kcode[0] & buttonMap[InsertCard]);

	AlphaTrans += ((float)Visible - AlphaTrans) / 2;
}

static float getUIScale() {
	// scale is 1.1 for a 320 dpi screen of height 750
	return 1.1f * 750.f / settings.display.height * settings.display.dpi / 320.f;
}

struct LayoutElement
{
	const std::string name;
	const float dx, dy;	// default pos in dc coords, relative to sides (or middle if 0)
	const float dw, dh;	// default size in dc coords

	float x, y;			// normalized coordinates [0, 1]
	float w, h;			// normalized coordinates [0, 1], scaled with uiScale
	float scale;		// user scale
	int transparency = 0;	// per-unit transparency [0, 100]
	int touchSize = 0;		// extra hitbox padding [0, 100]

	void load()
	{
		x = config::loadFloat(CFG_SECTION, name + "_x", x);
		y = config::loadFloat(CFG_SECTION, name + "_y", y);
		const float loadedScale = config::loadFloat(CFG_SECTION, name + "_scale", scale);
		scale = std::isfinite(loadedScale)
				? std::clamp(loadedScale, MIN_ELEMENT_SCALE, MAX_ELEMENT_SCALE)
				: 1.f;
		transparency = std::clamp(config::loadInt(CFG_SECTION, name + "_transparency", transparency), 0, MAX_PERCENT);
		touchSize = std::clamp(config::loadInt(CFG_SECTION, name + "_touch_size", touchSize), 0, MAX_PERCENT);
	}
	void save() const
	{
		config::saveFloat(CFG_SECTION, name + "_x", x);
		config::saveFloat(CFG_SECTION, name + "_y", y);
		config::saveFloat(CFG_SECTION, name + "_scale", scale);
		config::saveInt(CFG_SECTION, name + "_transparency", transparency);
		config::saveInt(CFG_SECTION, name + "_touch_size", touchSize);
	}

	bool hitTest(float nx, float ny) const {
		return nx >= x && nx < x + w * scale
			&& ny >= y && ny < y + h * scale;
	}

	void applyUiScale()
	{
		const float dcw = REFERENCE_HEIGHT * (float)settings.display.width / settings.display.height;
		const float uiscale = getUIScale();
		w = dw / dcw * uiscale;
		h = dh / REFERENCE_HEIGHT * uiscale;
	}

	void reset()
	{
		applyUiScale();
		scale = 1.f;
		transparency = 0;
		touchSize = 0;
		const float dcw = REFERENCE_HEIGHT * (float)settings.display.width / settings.display.height;
		const float uiscale = getUIScale();
		if (dx == 0)
			x = 0.5f - w / 2;
		else if (dx > 0)
			x = dx / dcw * uiscale;
		else
			x = 1.f - w + dx / dcw * uiscale;
		if (dy == 0)
			y = 0.5f - h / 2;
		else if (dy > 0)
			y = dy / REFERENCE_HEIGHT * uiscale;
		else
			y = 1.f - h + dy / REFERENCE_HEIGHT * uiscale;
	}

	float touchPadding() const
	{
		return MAX_TOUCH_PADDING * touchSize / MAX_PERCENT;
	}
};
static LayoutElement Layout[] {
	{ "dpad",      32.f, -24.f, 192.f, 192.f },
	{ "buttons",  -24.f, -24.f, 192.f, 192.f },
	{ "start",      0.f, -24.f,  64.f,  64.f },
	{ "LT",      -134.f,-240.f,  90.f,  64.f },
	{ "RT",       -32.f,-240.f,  90.f,  64.f },
	{ "analog",    40.f,-320.f, 128.f, 128.f },
	{ "fforward", -24.f,  24.f,  64.f,  64.f },

	{ "btn4",     -24.f,-216.f,  64.f,  64.f },
	{ "btn5",    -152.f,-216.f,  64.f,  64.f },
	{ "service",  -24.f,  96.f,  64.f,  64.f },
	{ "inscard",   40.f,-250.f,  64.f,  64.f },
};

struct LayoutState
{
	float x;
	float y;
	float scale;
	int transparency;
	int touchSize;
};

struct EditSessionState
{
	std::array<LayoutState, Elem_Count> layout;
	bool enabled;
	bool hapticFeedback;
	bool visualFeedback;
	int vibration;
	int transparency;
	int idleTimeout;
	int fingerSize;
	std::string imagePath;
};
static EditSessionState EditSession;

static bool isValidElement(Element element)
{
	return element >= Elem_DPad && element < Elem_Count;
}

static Element controlToElement(ControlId control)
{
	switch (control)
	{
	case Left:
	case Up:
	case Right:
	case Down:
	case LeftUp:
	case RightUp:
	case LeftDown:
	case RightDown:
		return Elem_DPad;
	case X:
	case Y:
	case B:
	case A:
		return Elem_Buttons;
	case Start:
		return Elem_Start;
	case LeftTrigger:
		return Elem_LT;
	case RightTrigger:
		return Elem_RT;
	case AnalogArea:
	case AnalogStick:
		return Elem_Analog;
	case FastForward:
		return Elem_FForward;
	case Btn4:
		return Elem_Btn4;
	case Btn5:
		return Elem_Btn5;
	case ServiceMode:
		return Elem_ServiceMode;
	case InsertCard:
		return Elem_InsertCard;
	default:
		return Elem_None;
	}
}

static int getControlTransparency(ControlId control)
{
	const Element element = controlToElement(control);
	return isValidElement(element) ? Layout[element].transparency : 0;
}

static float getControlHitPadding(ControlId control)
{
	const Element element = controlToElement(control);
	return isValidElement(element) ? Layout[element].touchPadding() : 0.f;
}

static float getFingerHitPadding()
{
	return MAX_TOUCH_PADDING * std::clamp(static_cast<int>(config::VirtualGamepadFingerSize), 0, MAX_PERCENT)
			/ MAX_PERCENT;
}

static void applyLayout()
{
	const float dcw = REFERENCE_HEIGHT * (float)settings.display.width / settings.display.height;
	const float dx = (dcw - REFERENCE_WIDTH) / 2;
	const float uiscale = getUIScale();
	float x, y, scale;

	// DPad
	x = Layout[Elem_DPad].x * dcw - dx;
	y = Layout[Elem_DPad].y * REFERENCE_HEIGHT;
	scale = Layout[Elem_DPad].scale * uiscale;
	Controls[Left].pos =   { x + 0.f * scale, y + 64.f * scale };
	Controls[Up].pos =     { x + 64.f * scale, y + 0.f * scale };
	Controls[Right].pos =  { x + 128.f * scale, y + 64.f * scale };
	Controls[Down].pos =   { x + 64.f * scale, y + 128.f * scale };
	for (int control = Left; control <= Down; control++)
		Controls[control].size = { 64.f * scale, 64.f * scale };

	Controls[LeftUp].pos =     { x + 0.f * scale, y + 0.f * scale };
	Controls[LeftDown].pos =   { x + 0.f * scale, y + 128.f * scale };
	Controls[RightUp].pos =    { x + 128.f * scale, y + 0.f * scale };
	Controls[RightDown].pos =  { x + 128.f * scale, y + 128.f * scale };
	for (int control = LeftUp; control <= RightDown; control++)
		Controls[control].size = { 64.f * scale, 64.f * scale };

	// Buttons
	x = Layout[Elem_Buttons].x * dcw - dx;
	y = Layout[Elem_Buttons].y * REFERENCE_HEIGHT;
	scale = Layout[Elem_Buttons].scale * uiscale;
	Controls[X].pos =  { x + 0.f * scale, y + 64.f * scale };
	Controls[Y].pos =  { x + 64.f * scale, y + 0.f * scale };
	Controls[B].pos =  { x + 128.f * scale, y + 64.f * scale };
	Controls[A].pos =  { x + 64.f * scale, y + 128.f * scale };
	for (int control = X; control <= A; control++)
		Controls[control].size = { 64.f * scale, 64.f * scale };

	// Start
	scale = Layout[Elem_Start].scale * uiscale;
	Controls[Start].pos =   { Layout[Elem_Start].x * dcw - dx, Layout[Elem_Start].y * REFERENCE_HEIGHT };
	Controls[Start].size =  { Layout[Elem_Start].dw * scale, Layout[Elem_Start].dh * scale };

	// Left trigger
	scale = Layout[Elem_LT].scale * uiscale;
	Controls[LeftTrigger].pos = { Layout[Elem_LT].x * dcw - dx, Layout[Elem_LT].y * REFERENCE_HEIGHT };
	Controls[LeftTrigger].size = { Layout[Elem_LT].dw * scale, Layout[Elem_LT].dh * scale };

	// Right trigger
	scale = Layout[Elem_RT].scale * uiscale;
	Controls[RightTrigger].pos =  { Layout[Elem_RT].x * dcw - dx, Layout[Elem_RT].y * REFERENCE_HEIGHT };
	Controls[RightTrigger].size = { Layout[Elem_RT].dw * scale, Layout[Elem_RT].dh * scale };

	// Analog
	x = Layout[Elem_Analog].x * dcw - dx;
	y = Layout[Elem_Analog].y * REFERENCE_HEIGHT;
	scale = Layout[Elem_Analog].scale * uiscale;
	Controls[AnalogArea].pos =   { x, y };
	Controls[AnalogArea].size =  { Layout[Elem_Analog].dw * scale, Layout[Elem_Analog].dh * scale };
	Controls[AnalogStick].pos =  { x + 32.f * scale, y + 32.f * scale };
	Controls[AnalogStick].size = { 64.f * scale, 64.f * scale };

	// Fast forward
	scale = Layout[Elem_FForward].scale * uiscale;
	Controls[FastForward].pos =  { Layout[Elem_FForward].x * dcw - dx, Layout[Elem_FForward].y * REFERENCE_HEIGHT };
	Controls[FastForward].size = { Layout[Elem_FForward].dw * scale, Layout[Elem_FForward].dh * scale };

	// ARCADE
	// Button 4
	scale = Layout[Elem_Btn4].scale * uiscale;
	Controls[Btn4].pos =  { Layout[Elem_Btn4].x * dcw - dx, Layout[Elem_Btn4].y * REFERENCE_HEIGHT };
	Controls[Btn4].size = { Layout[Elem_Btn4].dw * scale, Layout[Elem_Btn4].dh * scale };
	// Button 5
	scale = Layout[Elem_Btn5].scale * uiscale;
	Controls[Btn5].pos =  { Layout[Elem_Btn5].x * dcw - dx, Layout[Elem_Btn5].y * REFERENCE_HEIGHT };
	Controls[Btn5].size = { Layout[Elem_Btn5].dw * scale, Layout[Elem_Btn5].dh * scale };

	// Service Mode
	scale = Layout[Elem_ServiceMode].scale * uiscale;
	Controls[ServiceMode].pos =  { Layout[Elem_ServiceMode].x * dcw - dx, Layout[Elem_ServiceMode].y * REFERENCE_HEIGHT };
	Controls[ServiceMode].size = { Layout[Elem_ServiceMode].dw * scale, Layout[Elem_ServiceMode].dh * scale };

	// Insert Card
	scale = Layout[Elem_InsertCard].scale * uiscale;
	Controls[InsertCard].pos =  { Layout[Elem_InsertCard].x * dcw - dx, Layout[Elem_InsertCard].y * REFERENCE_HEIGHT };
	Controls[InsertCard].size = { Layout[Elem_InsertCard].dw * scale, Layout[Elem_InsertCard].dh * scale };
}

void applyUiScale() {
	for (auto& element : Layout)
		element.applyUiScale();
}

static void loadLayout()
{
	for (auto& element : Layout) {
		element.reset();
		element.load();
	}
	applyLayout();
}

static void saveCustomization(const std::string& imagePath)
{
	config::setAutoSave(false);
	for (auto& element : Layout)
		element.save();
	config::VirtualGamepadEnabled.save();
	config::VirtualGamepadHapticFeedback.save();
	config::VirtualGamepadVisualFeedback.save();
	config::VirtualGamepadVibration.save();
	config::VirtualGamepadTransparency.save();
	config::VirtualGamepadIdleTimeout.save();
	config::VirtualGamepadFingerSize.save();
	config::saveStr(CFG_SECTION, getButtonsCfgName(), imagePath);
	config::setAutoSave(true);
}

static void resetLayout()
{
	for (auto& element : Layout)
		element.reset();
	applyLayout();
}

static void captureEditSession()
{
	for (int i = 0; i < Elem_Count; i++)
	{
		const LayoutElement& element = Layout[i];
		EditSession.layout[i] = {
			element.x,
			element.y,
			element.scale,
			element.transparency,
			element.touchSize,
		};
	}
	EditSession.enabled = static_cast<bool>(config::VirtualGamepadEnabled);
	EditSession.hapticFeedback = static_cast<bool>(config::VirtualGamepadHapticFeedback);
	EditSession.visualFeedback = static_cast<bool>(config::VirtualGamepadVisualFeedback);
	EditSession.vibration = static_cast<int>(config::VirtualGamepadVibration);
	EditSession.transparency = static_cast<int>(config::VirtualGamepadTransparency);
	EditSession.idleTimeout = static_cast<int>(config::VirtualGamepadIdleTimeout);
	EditSession.fingerSize = static_cast<int>(config::VirtualGamepadFingerSize);
	EditSession.imagePath = config::loadStr(CFG_SECTION, getButtonsCfgName(), "");
}

static void restoreEditSession()
{
	const bool imageChanged = config::loadStr(CFG_SECTION, getButtonsCfgName(), "") != EditSession.imagePath;
	for (int i = 0; i < Elem_Count; i++)
	{
		LayoutElement& element = Layout[i];
		const LayoutState& state = EditSession.layout[i];
		element.x = state.x;
		element.y = state.y;
		element.scale = state.scale;
		element.transparency = state.transparency;
		element.touchSize = state.touchSize;
	}
	config::VirtualGamepadEnabled = EditSession.enabled;
	config::VirtualGamepadHapticFeedback = EditSession.hapticFeedback;
	config::VirtualGamepadVisualFeedback = EditSession.visualFeedback;
	config::VirtualGamepadVibration = EditSession.vibration;
	config::VirtualGamepadTransparency = EditSession.transparency;
	config::VirtualGamepadIdleTimeout = EditSession.idleTimeout;
	config::VirtualGamepadFingerSize = EditSession.fingerSize;
	saveCustomization(EditSession.imagePath);
	if (imageChanged)
	{
		// The saved path is restored first so an unavailable custom image falls
		// back to the default artwork instead of leaving the replacement active.
		loadOSDButtons();
	}
	applyLayout();
}

Element layoutHitTest(float x, float y)
{
	for (const auto& element : Layout)
		if (element.hitTest(x, y))
			return static_cast<Element>(&element - &Layout[0]);
	return Elem_None;
}

void translateElement(Element element, float dx, float dy)
{
	if (!isValidElement(element) || !std::isfinite(dx) || !std::isfinite(dy))
		return;

	LayoutElement& e = Layout[element];
	e.x += dx;
	e.y += dy;
	applyLayout();
}

void scaleElement(Element element, float factor)
{
	if (!isValidElement(element) || !std::isfinite(factor) || factor <= 0.f)
		return;

	LayoutElement& e = Layout[element];
	const float newScale = std::clamp(e.scale * factor, MIN_ELEMENT_SCALE, MAX_ELEMENT_SCALE);
	if (newScale == e.scale)
		return;

	// Keep the unit centered using the actual clamped scale change.
	e.x += e.w * (e.scale - newScale) / 2.f;
	e.y += e.h * (e.scale - newScale) / 2.f;
	e.scale = newScale;
	applyLayout();
}

void loadImage(const std::string& path)
{
	if (path.empty()) {
		config::saveStr(CFG_SECTION, getButtonsCfgName(), "");
		loadOSDButtons();
	}
	else if (loadOSDButtons(path)) {
		config::saveStr(CFG_SECTION, getButtonsCfgName(), path);
	}
}

static void enableAllControls()
{
	for (auto& control : Controls)
		control.disabled = false;
}

static void disableControl(ControlId ctrlId)
{
#ifdef TARGET_IPHONE
	if (ctrlId == Up || ctrlId == Down)
		// Needed to pause the emulator
		return;
#endif

	Controls[ctrlId].disabled = true;
	switch (ctrlId)
	{
	case Left:
		Controls[LeftUp].disabled = true;
		Controls[LeftDown].disabled = true;
		break;
	case Right:
		Controls[RightUp].disabled = true;
		Controls[RightDown].disabled = true;
		break;
	case Up:
		Controls[LeftUp].disabled = true;
		Controls[RightUp].disabled = true;
		break;
	case Down:
		Controls[LeftDown].disabled = true;
		Controls[RightDown].disabled = true;
		break;
	case AnalogArea:
	case AnalogStick:
		Controls[AnalogArea].disabled = true;
		Controls[AnalogStick].disabled = true;
		break;
	default:
		break;
	}
}

void startGame()
{
	enableAllControls();
	serviceMode = false;
	setButtonMap();
	bool enableTouchMouse = false;
	if (settings.platform.isConsole())
	{
		disableControl(Btn4);
		disableControl(Btn5);
		disableControl(ServiceMode);
		disableControl(InsertCard);
		switch (config::MapleMainDevices[0])
		{
		case MDT_LightGun:
			enableTouchMouse = true;
			disableControl(AnalogArea);
			disableControl(LeftTrigger);
			disableControl(RightTrigger);
			disableControl(A);
			disableControl(X);
			disableControl(Y);
			break;
		case MDT_AsciiStick:
			// TODO add CZ
			disableControl(AnalogArea);
			disableControl(LeftTrigger);
			disableControl(RightTrigger);
			break;
		case MDT_PopnMusicController:
			// TODO add C btn
			disableControl(AnalogArea);
			disableControl(LeftTrigger);
			disableControl(RightTrigger);
			break;
		case MDT_RacingController:
			disableControl(X);
			disableControl(Y);
			break;
		default:
			break;
		}
	}
	else
	{
		// arcade game
		if (!card_reader::readerAvailable())
			disableControl(InsertCard);
		if (settings.platform.isAtomiswave()) {
			disableControl(Btn5);
		}
		else if (settings.platform.isSystemSP())
		{
			disableControl(Btn4);
			disableControl(Btn5);
		}
		if (NaomiGameInputs != nullptr)
		{
			bool fullAnalog = false;
			bool rt = false;
			bool lt = false;
			for (const auto& axis : NaomiGameInputs->axes)
			{
				if (axis.name == nullptr)
					break;
				switch (axis.axis)
				{
				case 0:
				case 1:
					fullAnalog = true;
					break;
				case 4:
					rt = true;
					break;
				case 5:
					lt = true;
					break;
				}
			}
			if (!fullAnalog)
				disableControl(AnalogArea);
			if (!lt)
				disableControl(LeftTrigger);
			else
				disableControl(Btn5);
			if (!rt)
				disableControl(RightTrigger);
			else
				disableControl(Btn4);
			u32 usedButtons = 0;
			for (const auto& button : NaomiGameInputs->buttons)
			{
				if (button.name == nullptr)
					break;
				usedButtons |= button.source;
			}
			if (settings.platform.isAtomiswave())
			{
				// button order: A B X Y B4
				if ((usedButtons & AWAVE_BTN0_KEY) == 0 || settings.input.lightgunGame)
					disableControl(A);
				if ((usedButtons & AWAVE_BTN1_KEY) == 0)
					disableControl(B);
				if ((usedButtons & AWAVE_BTN2_KEY) == 0)
					disableControl(X);
				if ((usedButtons & AWAVE_BTN3_KEY) == 0)
					disableControl(Y);
				if ((usedButtons & AWAVE_BTN4_KEY) == 0)
					disableControl(Btn4);
				if ((usedButtons & AWAVE_UP_KEY) == 0)
					disableControl(Up);
				if ((usedButtons & AWAVE_DOWN_KEY) == 0)
					disableControl(Down);
				if ((usedButtons & AWAVE_LEFT_KEY) == 0)
					disableControl(Left);
				if ((usedButtons & AWAVE_RIGHT_KEY) == 0)
					disableControl(Right);
				if ((usedButtons & AWAVE_START_KEY) == 0)
					disableControl(Start);
			}
			else if (settings.platform.isSystemSP())
			{
				if ((usedButtons & DC_BTN_A) == 0)
					disableControl(A);
				if ((usedButtons & DC_BTN_B) == 0)
					disableControl(B);
				if ((usedButtons & DC_BTN_C) == 0)
					disableControl(X);
				if ((usedButtons & DC_BTN_X) == 0)
					disableControl(Y);
				if ((usedButtons & DC_DPAD_UP) == 0)
					disableControl(Up);
				if ((usedButtons & DC_DPAD_DOWN) == 0)
					disableControl(Down);
				if ((usedButtons & DC_DPAD_LEFT) == 0)
					disableControl(Left);
				if ((usedButtons & DC_DPAD_RIGHT) == 0)
					disableControl(Right);
				if ((usedButtons & DC_BTN_START) == 0)
					disableControl(Start);
			}
			else
			{
				if ((usedButtons & NAOMI_BTN0_KEY) == 0 || settings.input.lightgunGame)
					disableControl(A);
				if ((usedButtons & (NAOMI_BTN1_KEY | NAOMI_RELOAD_KEY)) == 0)
					disableControl(B);
				else if (settings.input.lightgunGame
						&& (usedButtons & NAOMI_RELOAD_KEY) != 0
						&& (usedButtons & NAOMI_BTN1_KEY) == 0)
					// Remap button 1 to reload for lightgun games that need it
					buttonMap[B] = DC_BTN_RELOAD;
				if ((usedButtons & NAOMI_BTN2_KEY) == 0)
					// C
					disableControl(X);
				if ((usedButtons & NAOMI_BTN3_KEY) == 0)
					// X
					disableControl(Y);
				if ((usedButtons & NAOMI_BTN4_KEY) == 0)
					// Y
					disableControl(Btn4);
				if ((usedButtons & NAOMI_BTN5_KEY) == 0)
					// Z
					disableControl(Btn5);
				if ((usedButtons & NAOMI_UP_KEY) == 0)
					disableControl(Up);
				if ((usedButtons & NAOMI_DOWN_KEY) == 0)
					disableControl(Down);
				if ((usedButtons & NAOMI_LEFT_KEY) == 0)
					disableControl(Left);
				if ((usedButtons & NAOMI_RIGHT_KEY) == 0)
					disableControl(Right);
				if ((usedButtons & NAOMI_START_KEY) == 0)
					disableControl(Start);
			}
		}
		else
		{
			if (settings.input.lightgunGame)
			{
				disableControl(A);
				disableControl(X);
				disableControl(Y);
				disableControl(Btn4);
				disableControl(Btn5);
				disableControl(AnalogArea);
				disableControl(LeftTrigger);
				disableControl(RightTrigger);
				disableControl(Up);
				disableControl(Down);
				disableControl(Left);
				disableControl(Right);
			}
			else
			{
				// all analog games *should* have an input description
				disableControl(AnalogArea);
				disableControl(LeftTrigger);
				disableControl(RightTrigger);
			}
		}
		if (settings.input.lightgunGame || settings.input.mouseGame)
			enableTouchMouse = true;
	}
	std::shared_ptr<TouchMouse> touchMouse = GamepadDevice::GetGamepad<TouchMouse>();
	if (touchMouse != nullptr)
	{
		if (enableTouchMouse) {
			if (touchMouse->maple_port() == -1)
				touchMouse->set_maple_port(0);
		}
		else {
			if (touchMouse->maple_port() == 0)
				touchMouse->set_maple_port(-1);
		}
	}
}

void resetEditing() {
	resetLayout();
}

void startEditing()
{
	if (!Editing)
	{
		loadLayout();
		captureEditSession();
	}
	enableAllControls();
	show();
	Editing = true;
	setEditMode(true);
}

void pauseEditing() {
	setEditMode(false);
}

static void stopEditing(bool canceled)
{
	setEditMode(false);
	if (canceled)
		restoreEditSession();
	else
		saveCustomization(config::loadStr(CFG_SECTION, getButtonsCfgName(), ""));
	Editing = false;
}

}	// namespace vgamepad

#endif // __ANDROID__ || TARGET_IPHONE
