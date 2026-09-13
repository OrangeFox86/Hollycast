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
static void drawCustomizePanel();
static void drawSelectedElement();
static void drawTouchIndicator(bool pointerOverUi);
static void handleCustomizeDrag(bool pointerOverUi);
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
static Element SelectedElement = Elem_DPad;
static Element DraggedElement = Elem_None;
static ImVec2 LastDragMousePos;
static std::string PendingImagePath;
constexpr float REFERENCE_WIDTH = 640.f;
constexpr float REFERENCE_HEIGHT = 480.f;
constexpr float MIN_ELEMENT_SCALE = 0.5f;
constexpr float MAX_ELEMENT_SCALE = 1.8f;
constexpr float MAX_TOUCH_PADDING = 24.f;
constexpr int MAX_PERCENT = 100;
constexpr s64 MAX_BUTTON_IMAGE_FILE_SIZE = 4 * 1024 * 1024;
constexpr int MAX_BUTTON_IMAGE_WIDTH = 4096;
constexpr int MAX_BUTTON_IMAGE_HEIGHT = 2048;
constexpr char const *BTN_PATH = "picture/buttons.png";
constexpr char const *BTN_PATH_ARCADE = "picture/buttons-arcade.png";
constexpr char const *CFG_SECTION = "vgamepad";

void displayCommands()
{
	draw();
	drawCustomizePanel();
}

static const char *getButtonsResPath() {
	return settings.platform.isConsole() ? BTN_PATH : BTN_PATH_ARCADE;
}

static const char *getButtonsCfgName() {
	return settings.platform.isConsole() ? "image" : "image_arcade";
}

static bool isValidButtonImage(int width, int height)
{
	return width > 0 && height > 0
			&& width <= MAX_BUTTON_IMAGE_WIDTH && height <= MAX_BUTTON_IMAGE_HEIGHT
			&& width == height * 2;
}

static bool updateOSDButtons(const u8 *imageData, int width, int height, ImTextureID *textureId = nullptr)
{
	if (!isValidButtonImage(width, height))
		return false;
	try {
		const ImTextureID id = imguiDriver->updateTexture(getButtonsResPath(), imageData, width, height, false);
		if (textureId != nullptr)
			*textureId = id;
		return true;
	} catch (...) {
		// Vulkan can throw while the surface is resizing.
		return false;
	}
}

static bool loadOSDButtonsResource(const char *path, ImTextureID *textureId = nullptr)
{
	size_t size;
	std::unique_ptr<u8[]> data = resource::load(path, size);
	if (data == nullptr || size > static_cast<size_t>(std::numeric_limits<int>::max()))
		return false;

	stbi_set_flip_vertically_on_load_thread(1);
	int width, height, channels;
	u8 *imageData = stbi_load_from_memory(data.get(), static_cast<int>(size), &width, &height, &channels,
			STBI_rgb_alpha);
	if (imageData == nullptr)
		return false;
	const bool loaded = updateOSDButtons(imageData, width, height, textureId);
	stbi_image_free(imageData);
	return loaded;
}

static bool loadOSDButtons(const std::string& path)
{
	if (path.empty())
		return false;
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rb"));
	if (file == nullptr)
		return false;
	const s64 fileSize = file->size();
	if (fileSize <= 0 || fileSize > MAX_BUTTON_IMAGE_FILE_SIZE)
		return false;

	stbi_set_flip_vertically_on_load_thread(1);
	int width, height, channels;
	if (!stbi_info_from_callbacks(&STBI::callbacks, file.get(), &width, &height, &channels)
			|| !isValidButtonImage(width, height) || file->seek(0, SEEK_SET) != 0)
		return false;

	u8 *imageData = stbi_load_from_file(file.get(), &width, &height, &channels, STBI_rgb_alpha);
	if (imageData == nullptr)
		return false;
	const bool loaded = updateOSDButtons(imageData, width, height);
	stbi_image_free(imageData);
	return loaded;
}

static ImTextureID loadDefaultOSDButtons()
{
	ImTextureID id{};
	if (settings.platform.isConsole())
	{
		// legacy buttons.png in data folder
		if (loadOSDButtons(get_readonly_data_path("buttons.png")))
			return id;
		// also try the home folder (android)
		if (loadOSDButtons(get_readonly_config_path("buttons.png")))
			return id;
	}
	loadOSDButtonsResource(getButtonsResPath(), &id);
	return id;
}

static ImTextureID loadOSDButtons()
{
	const std::string path = config::loadStr(CFG_SECTION, getButtonsCfgName());
	if (loadOSDButtons(path))
		return {};
	return loadDefaultOSDButtons();
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
	const float elementAlpha = (MAX_PERCENT - getControlTransparency(controlId)) / static_cast<float>(MAX_PERCENT);
	float alpha = elementAlpha * AlphaTrans;
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
	const char *label;
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
		const int legacyTransparency = std::clamp(static_cast<int>(config::VirtualGamepadTransparency), 0, MAX_PERCENT);
		transparency = std::clamp(config::loadInt(CFG_SECTION, name + "_transparency", legacyTransparency), 0, MAX_PERCENT);
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
static LayoutElement Layout[Elem_Count] {
	{ "dpad",     Tnop("D-pad"),        32.f, -24.f, 192.f, 192.f },
	{ "buttons",  Tnop("Face Buttons"),-24.f, -24.f, 192.f, 192.f },
	{ "start",    Tnop("Start"),         0.f, -24.f,  64.f,  64.f },
	// The shared default keeps the triggers clear of buttons 4 and 5 in arcade games.
	{ "LT",       Tnop("LT"),         -134.f,-288.f,  90.f,  64.f },
	{ "RT",       Tnop("RT"),          -32.f,-288.f,  90.f,  64.f },
	{ "analog",   Tnop("Analog Stick"), 40.f,-320.f, 128.f, 128.f },
	{ "fforward", Tnop("Fast Forward"),-24.f,  24.f,  64.f,  64.f },

	{ "btn4",     Tnop("Button 4"),    -24.f,-216.f,  64.f,  64.f },
	{ "btn5",     Tnop("Button 5"),   -152.f,-216.f,  64.f,  64.f },
	{ "service",  Tnop("Service"),     -24.f,  96.f,  64.f,  64.f },
	{ "inscard",  Tnop("Insert Card"),  40.f,-250.f,  64.f,  64.f },
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

static const char *getElementLabel(Element element)
{
	return isValidElement(element) ? i18n::T(Layout[element].label) : i18n::T("None");
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

static void resetElement(Element element)
{
	if (!isValidElement(element))
		return;
	Layout[element].reset();
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
	const bool imageChanged = PendingImagePath != EditSession.imagePath;
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
	PendingImagePath = EditSession.imagePath;
	applyLayout();
}

static bool getElementScreenBounds(Element element, ImVec2& min, ImVec2& max)
{
	if (!isValidElement(element))
		return false;

	const LayoutElement& layout = Layout[element];
	const float dcw = REFERENCE_HEIGHT * static_cast<float>(settings.display.width) / settings.display.height;
	const float dcOffsetX = (dcw - REFERENCE_WIDTH) / 2.f;
	const float screenScale = settings.display.height / REFERENCE_HEIGHT;
	const float screenOffsetX = (settings.display.width - screenScale * REFERENCE_WIDTH) / 2.f;
	const ImVec2 dcMin(layout.x * dcw - dcOffsetX, layout.y * REFERENCE_HEIGHT);
	const ImVec2 dcSize(layout.w * layout.scale * dcw, layout.h * layout.scale * REFERENCE_HEIGHT);
	min = dcMin * screenScale;
	min.x += screenOffsetX;
	max = (dcMin + dcSize) * screenScale;
	max.x += screenOffsetX;
	return true;
}

static bool getControlScreenBounds(ControlId controlId, ImVec2& min, ImVec2& max, bool includeInputPadding)
{
	if (controlId < Left || controlId >= _Count || Controls[controlId].disabled)
		return false;

	const Control& control = Controls[controlId];
	const float screenScale = settings.display.height / REFERENCE_HEIGHT;
	const float screenOffsetX = (settings.display.width - screenScale * REFERENCE_WIDTH) / 2.f;
	min = control.pos * screenScale;
	min.x += screenOffsetX;
	max = (control.pos + control.size) * screenScale;
	max.x += screenOffsetX;
	if (includeInputPadding)
	{
		// Keep this calculation identical to hitTest(): blue outlines represent
		// the actual target after unit and finger expansion are combined.
		const float padding = (getControlHitPadding(controlId) + getFingerHitPadding()) * screenScale;
		min -= ImVec2(padding, padding);
		max += ImVec2(padding, padding);
	}
	return true;
}

static void drawSelectedElement()
{
	if (!Editing || !isValidElement(SelectedElement))
		return;

	ImVec2 min;
	ImVec2 max;
	if (!getElementScreenBounds(SelectedElement, min, max))
		return;

	const float outlinePadding = uiScaled(4.f);
	min -= ImVec2(outlinePadding, outlinePadding);
	max += ImVec2(outlinePadding, outlinePadding);
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	drawList->AddRect(min, max, IM_COL32(34, 161, 110, 230), uiScaled(8.f), 0, uiScaled(2.f));

	if (Layout[SelectedElement].touchSize <= 0 && getFingerHitPadding() <= 0.f)
		return;

	for (int control = Left; control < _Count; control++)
	{
		const ControlId controlId = static_cast<ControlId>(control);
		if (controlToElement(controlId) != SelectedElement)
			continue;

		ImVec2 touchMin;
		ImVec2 touchMax;
		if (getControlScreenBounds(controlId, touchMin, touchMax, true))
			drawList->AddRect(touchMin, touchMax, IM_COL32(64, 196, 255, 210), uiScaled(8.f), 0, uiScaled(2.f));
	}
}

static void drawTouchIndicator(bool pointerOverUi)
{
	if (!Editing || pointerOverUi || !ImGui::GetIO().MouseDown[ImGuiMouseButton_Left])
		return;

	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	const ImVec2 position = ImGui::GetIO().MousePos;
	const float radius = getFingerHitPadding() * settings.display.height / REFERENCE_HEIGHT;
	drawList->AddCircleFilled(position, uiScaled(4.f), IM_COL32(255, 255, 255, 245), 24);
	if (radius > 0.f)
		drawList->AddCircle(position, radius, IM_COL32(255, 202, 67, 245), 32, uiScaled(2.f));
}

static int getElementScalePercent(Element element)
{
	return isValidElement(element) ? std::lround(Layout[element].scale * MAX_PERCENT) : MAX_PERCENT;
}

static void setElementScale(Element element, float scale)
{
	if (!isValidElement(element) || !std::isfinite(scale))
		return;

	LayoutElement& selected = Layout[element];
	const float clampedScale = std::clamp(scale, MIN_ELEMENT_SCALE, MAX_ELEMENT_SCALE);
	if (clampedScale == selected.scale)
		return;

	// Use the clamped scale delta so the unit stays centered at either limit.
	selected.x += selected.w * (selected.scale - clampedScale) / 2.f;
	selected.y += selected.h * (selected.scale - clampedScale) / 2.f;
	selected.scale = clampedScale;
	applyLayout();
}

static bool drawCustomizeSlider(const char *id, const char *label, int& value, int min, int max,
		const char *format = "%d%%")
{
	ImGui::TextUnformatted(label);
	ImGui::SetNextItemWidth(-1.f);
	ImguiID _(id);
	return ImGui::SliderInt("##Value", &value, min, max, format);
}

static bool drawCustomizeCheckbox(const char *id, const char *label, bool& value)
{
	ImguiID _(id);
	const bool changed = ImGui::Checkbox("##Value", &value);
	ImGui::SameLine();
	ImGui::TextUnformatted(label);
	return changed;
}

static bool drawCustomizeButton(const char *id, const char *label, const ImVec2& size)
{
	const std::string widgetLabel = std::string(label) + "###" + id;
	return ImGui::Button(widgetLabel.c_str(), size);
}

static void drawCustomizePanel()
{
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	if (displaySize.x <= 0.f || displaySize.y <= 0.f)
		return;

	const float margin = uiScaled(8.f);
	const float width = std::max(1.f, std::min(displaySize.x - margin * 2.f, uiScaled(430.f)));
	const float height = std::max(1.f, std::min(displaySize.y - margin * 2.f, uiScaled(680.f)));
	ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, margin), ImGuiCond_Appearing, ImVec2(0.5f, 0.f));
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
	const std::string windowTitle = std::string(i18n::T("Virtual Controller")) + "###VirtualControllerCustomize";
	ImGui::Begin(windowTitle.c_str(), nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

	if (!isValidElement(SelectedElement))
		SelectedElement = Elem_DPad;

	ImGui::Text("%s: %s", i18n::T("Selected"), getElementLabel(SelectedElement));
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::BeginCombo("##SelectedVirtualControllerUnit", getElementLabel(SelectedElement)))
	{
		for (int i = Elem_DPad; i < Elem_Count; i++)
		{
			const Element element = static_cast<Element>(i);
			const bool selected = element == SelectedElement;
			const std::string itemLabel = std::string(getElementLabel(element))
					+ "###VirtualControllerUnit" + Layout[element].name;
			if (ImGui::Selectable(itemLabel.c_str(), selected))
				SelectedElement = element;
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	ImGui::Separator();
	bool enabled = static_cast<bool>(config::VirtualGamepadEnabled);
	bool visualFeedback = static_cast<bool>(config::VirtualGamepadVisualFeedback);
	bool hapticFeedback = static_cast<bool>(config::VirtualGamepadHapticFeedback);
	if (drawCustomizeCheckbox("VirtualControllerEnabled", i18n::T("Show Virtual Controller"), enabled))
		config::VirtualGamepadEnabled = enabled;
	if (drawCustomizeCheckbox("VirtualControllerVisualFeedback", i18n::T("Visual Feedback"), visualFeedback))
		config::VirtualGamepadVisualFeedback = visualFeedback;
	if (drawCustomizeCheckbox("VirtualControllerHapticFeedback", i18n::T("Vibration"), hapticFeedback))
		config::VirtualGamepadHapticFeedback = hapticFeedback;

	int vibration = std::clamp(static_cast<int>(config::VirtualGamepadVibration), 0, MAX_PERCENT);
	if (vibration != static_cast<int>(config::VirtualGamepadVibration))
		config::VirtualGamepadVibration = vibration;
	ImGui::BeginDisabled(!hapticFeedback);
	if (drawCustomizeSlider("VirtualControllerVibrationPower", i18n::T("Vibration Power"), vibration, 0, MAX_PERCENT))
		config::VirtualGamepadVibration = vibration;
	ImGui::EndDisabled();

	int timeout = std::clamp(static_cast<int>(config::VirtualGamepadIdleTimeout), 0, MAX_IDLE_TIMEOUT_SECONDS);
	if (drawCustomizeSlider("VirtualControllerIdleTimeout", i18n::T("Idle Timeout in Seconds"), timeout, 0,
			MAX_IDLE_TIMEOUT_SECONDS, "%d") || timeout != static_cast<int>(config::VirtualGamepadIdleTimeout))
		config::VirtualGamepadIdleTimeout = timeout;

	int fingerSize = std::clamp(static_cast<int>(config::VirtualGamepadFingerSize), 0, MAX_PERCENT);
	if (drawCustomizeSlider("VirtualControllerFingerSize", i18n::T("Finger Size"), fingerSize, 0, MAX_PERCENT)
			|| fingerSize != static_cast<int>(config::VirtualGamepadFingerSize))
		config::VirtualGamepadFingerSize = fingerSize;

	ImGui::Separator();
	LayoutElement& selected = Layout[SelectedElement];
	int unitTransparency = selected.transparency;
	if (drawCustomizeSlider("VirtualControllerUnitTransparency", i18n::T("Unit Transparency"),
			unitTransparency, 0, MAX_PERCENT))
		selected.transparency = std::clamp(unitTransparency, 0, MAX_PERCENT);

	int touchSize = selected.touchSize;
	if (drawCustomizeSlider("VirtualControllerTouchSize", i18n::T("Touch Size"), touchSize, 0, MAX_PERCENT))
		selected.touchSize = std::clamp(touchSize, 0, MAX_PERCENT);

	int scale = getElementScalePercent(SelectedElement);
	if (drawCustomizeSlider("VirtualControllerScale", i18n::T("Scaling"), scale,
			static_cast<int>(MIN_ELEMENT_SCALE * MAX_PERCENT), static_cast<int>(MAX_ELEMENT_SCALE * MAX_PERCENT)))
		setElementScale(SelectedElement, scale / static_cast<float>(MAX_PERCENT));

	const float pairedButtonWidth = std::max(1.f,
			(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.f);
	if (drawCustomizeButton("VirtualControllerResetUnit", i18n::T("Reset Unit"), ImVec2(pairedButtonWidth, uiScaled(32))))
		resetElement(SelectedElement);
	ImGui::SameLine();
	if (drawCustomizeButton("VirtualControllerResetLayout", i18n::T("Reset Layout"), ImVec2(pairedButtonWidth, uiScaled(32))))
		resetLayout();

	const char *imagePickerLabel = i18n::T("Select a PNG file");
	const std::string imagePickerTitle = std::string(imagePickerLabel) + "###VirtualControllerImagePicker";
	if (drawCustomizeButton("VirtualControllerChooseImage", i18n::T("Choose Image..."), ImVec2(pairedButtonWidth, uiScaled(32))))
#ifdef __ANDROID__
	{
		if (!hostfs::addStorage(false, false, imagePickerLabel, [](bool cancelled, std::string path) {
			if (!cancelled)
				gui_runOnUiThread([path]() { loadImage(path); });
		}, Tnop("image/png")))
			ImGui::OpenPopup(imagePickerTitle.c_str());
	}
#else
	{
		ImGui::OpenPopup(imagePickerTitle.c_str());
	}
#endif
	ImGui::SameLine();
	if (drawCustomizeButton("VirtualControllerDefaultImage", i18n::T("Use Default"), ImVec2(pairedButtonWidth, uiScaled(32))))
		loadImage("");

	select_file_popup(imagePickerTitle.c_str(), [](bool cancelled, std::string selection)
			{
				if (!cancelled)
					gui_runOnUiThread([selection]() { loadImage(selection); });
				return true;
			}, true, Tnop("png"));

	ImGui::Separator();
	if (drawCustomizeButton("VirtualControllerDone", i18n::T("Done"), ImVec2(pairedButtonWidth, uiScaled(36))))
	{
		stopEditing(false);
		gui_setState(GuiState::Settings);
	}
	ImGui::SameLine();
	if (drawCustomizeButton("VirtualControllerCancel", i18n::T("Cancel"), ImVec2(pairedButtonWidth, uiScaled(36))))
	{
		stopEditing(true);
		gui_setState(GuiState::Settings);
	}

	const bool panelControlsActive = ImGui::IsAnyItemActive();
	const bool pointerOverUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)
			|| ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
	ImGui::End();

	handleCustomizeDrag(pointerOverUi);
	if (!panelControlsActive)
		drawSelectedElement();
	drawTouchIndicator(pointerOverUi);
}

static void handleCustomizeDrag(bool pointerOverUi)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f)
		return;

	const ImVec2 mouse = io.MousePos;
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		DraggedElement = pointerOverUi
				? Elem_None
				: layoutHitTest(mouse.x / io.DisplaySize.x, mouse.y / io.DisplaySize.y);
		LastDragMousePos = mouse;
	}
	else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && isValidElement(DraggedElement))
	{
		if (!pointerOverUi)
		{
			const ImVec2 delta = mouse - LastDragMousePos;
			translateElement(DraggedElement, delta.x / io.DisplaySize.x, delta.y / io.DisplaySize.y);
		}
		LastDragMousePos = mouse;
	}
	else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		DraggedElement = Elem_None;
	}
}

Element layoutHitTest(float x, float y)
{
	if (!std::isfinite(x) || !std::isfinite(y))
		return Elem_None;

	for (const auto& element : Layout)
		if (element.hitTest(x, y))
		{
			SelectedElement = static_cast<Element>(&element - &Layout[0]);
			return SelectedElement;
		}
	return Elem_None;
}

void translateElement(Element element, float dx, float dy)
{
	if (!isValidElement(element) || !std::isfinite(dx) || !std::isfinite(dy) || (dx == 0.f && dy == 0.f))
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
	setElementScale(element, Layout[element].scale * factor);
}

void loadImage(const std::string& path)
{
	if (!Editing)
		return;

	if (path.empty())
	{
		loadDefaultOSDButtons();
		PendingImagePath.clear();
	}
	else if (loadOSDButtons(path))
	{
		PendingImagePath = path;
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
		PendingImagePath = EditSession.imagePath;
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
	DraggedElement = Elem_None;
	if (canceled)
		restoreEditSession();
	else
		saveCustomization(PendingImagePath);
	Editing = false;
}

}	// namespace vgamepad

#endif // __ANDROID__ || TARGET_IPHONE
