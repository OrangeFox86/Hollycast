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
#include "imgui_internal.h"
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
#include <cmath>
#include <cstring>
#include <limits>

namespace vgamepad
{
static void stopEditing(bool canceled);
enum class LayoutProfile { Console, Arcade };
static void loadLayout(LayoutProfile profile);
static void drawCustomizePanel();
static void handleCustomizeDrag(const ImVec2& panelMin, const ImVec2& panelMax);
static void drawSelectedElement();
static void drawTouchIndicator(const ImVec2& panelMin, const ImVec2& panelMax);
static Element controlToElement(ControlId control);
static float getFingerHitPadding();

constexpr float REFERENCE_WIDTH = 640.f;
constexpr float REFERENCE_HEIGHT = 480.f;
constexpr float MIN_ELEMENT_SCALE = 0.5f;
constexpr float MAX_ELEMENT_SCALE = 1.8f;
constexpr float MAX_FINGER_HIT_PADDING = 24.f;
constexpr int MAX_PERCENT = 100;

struct Control
{
	Control() = default;
	Control(float x, float y, float w = 64.f, float h = 64.f)
		: pos(x, y), size(w, h), uv0(0, 0), uv1(1, 1) {}

	ImVec2 pos;
	ImVec2 size;
	ImVec2 uv0;
	ImVec2 uv1;
	int transparency = 0;
	bool disabled = false;
};
static Control Controls[_Count];
static bool Visible = true;
static bool Editing = false;
static bool serviceMode;
static float AlphaTrans = 1.f;
static ImVec2 StickPos;	// analog stick position [-1, 1]
static Element SelectedElement = Elem_DPad;
static Element DraggedElement = Elem_None;
static ImVec2 LastDragMousePos;
static LayoutProfile EditingLayoutProfile = LayoutProfile::Console;

struct EditSessionState
{
	bool enabled;
	bool hapticFeedback;
	int vibration;
	int idleTimeout;
	int fingerSize;
	std::string consoleImagePath;
	std::string arcadeImagePath;
};
static EditSessionState EditSession;

constexpr char const *BTN_PATH = "picture/buttons.png";
constexpr char const *BTN_PATH_ARCADE = "picture/buttons-arcade.png";
constexpr char const *CFG_SECTION = "vgamepad";
constexpr char const *PRESET_PREFIX = "preset:";
constexpr s64 MAX_BUTTON_IMAGE_FILE_SIZE = 4 * 1024 * 1024;
constexpr int MAX_BUTTON_IMAGE_WIDTH = 4096;
constexpr int MAX_BUTTON_IMAGE_HEIGHT = 2048;

struct ButtonImagePreset
{
	const char *fileName;
	const char *label;
};

constexpr ButtonImagePreset consoleImagePresets[] = {
	{ "blkclrDC.png", "Black / Color" },
	{ "drkgreywhtsolidDC.png", "Dark Gray / White Solid" },
	{ "briteneonDC.png", "Bright Neon" },
	{ "greywhitesolidDC.png", "Gray / White Solid" },
	{ "NeondimDC.png", "Neon Dim" },
};

constexpr ButtonImagePreset arcadeImagePresets[] = {
	{ "blkgryARC.png", "Black / Gray" },
	{ "blkcolorARC.png", "Black / Color" },
	{ "whitelineARC.png", "White Line" },
	{ "neonARC.png", "Neon" },
	{ "grywhitesldARC.png", "Gray / White Solid" },
};

void displayCommands()
{
	draw();
	drawCustomizePanel();
}

static LayoutProfile getRuntimeLayoutProfile()
{
	return settings.platform.isConsole() ? LayoutProfile::Console : LayoutProfile::Arcade;
}

static LayoutProfile getActiveLayoutProfile()
{
	return Editing ? EditingLayoutProfile : getRuntimeLayoutProfile();
}

static bool isConsoleLayoutProfile()
{
	return getActiveLayoutProfile() == LayoutProfile::Console;
}

static const char *getButtonsResPath() {
	return isConsoleLayoutProfile() ? BTN_PATH : BTN_PATH_ARCADE;
}

static const char *getButtonsCfgName() {
	return isConsoleLayoutProfile() ? "image" : "image_arcade";
}

static const ButtonImagePreset *getButtonImagePresets(size_t& count)
{
	if (isConsoleLayoutProfile())
	{
		count = std::size(consoleImagePresets);
		return consoleImagePresets;
	}
	count = std::size(arcadeImagePresets);
	return arcadeImagePresets;
}

static const ButtonImagePreset *findButtonImagePreset(const std::string& path)
{
	const size_t prefixLength = std::strlen(PRESET_PREFIX);
	if (path.compare(0, prefixLength, PRESET_PREFIX) != 0)
		return nullptr;

	const std::string fileName = path.substr(prefixLength);
	size_t count;
	const ButtonImagePreset *presets = getButtonImagePresets(count);
	for (size_t i = 0; i < count; i++)
		if (fileName == presets[i].fileName)
			return &presets[i];
	return nullptr;
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
	stbi_set_flip_vertically_on_load_thread(1);
	int width, height, n;
	u8 *imageData = stbi_load_from_memory(data.get(), static_cast<int>(size), &width, &height, &n, STBI_rgb_alpha);
	if (imageData == nullptr)
		return false;
	const bool loaded = updateOSDButtons(imageData, width, height, textureId);
	free(imageData);
	return loaded;
}

static bool loadOSDButtons(const std::string& path)
{
	if (path.empty())
		return false;
	if (path.compare(0, std::strlen(PRESET_PREFIX), PRESET_PREFIX) == 0)
	{
		// Presets are profile-specific so a console layout cannot load an arcade atlas, or vice versa.
		if (const ButtonImagePreset *preset = findButtonImagePreset(path))
			return loadOSDButtonsResource((std::string("picture/") + preset->fileName).c_str());
		return false;
	}

	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rb"));
	if (file == nullptr)
		return false;
	if (file->size() <= 0 || file->size() > MAX_BUTTON_IMAGE_FILE_SIZE)
		return false;

	stbi_set_flip_vertically_on_load_thread(1);
	int width, height, n;
	if (!stbi_info_from_callbacks(&STBI::callbacks, file.get(), &width, &height, &n)
			|| !isValidButtonImage(width, height))
		return false;
	file->seek(0, SEEK_SET);
	u8 *imageData = stbi_load_from_file(file.get(), &width, &height, &n, STBI_rgb_alpha);
	if (imageData == nullptr)
		return false;
	const bool loaded = updateOSDButtons(imageData, width, height);
	free(imageData);
	return loaded;
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
	// Default built-in sheet.
	loadOSDButtonsResource(getButtonsResPath(), &id);
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
	for (const auto& control : Controls)
	{
		const ControlId controlId = static_cast<ControlId>(&control - &Controls[0]);
		const float padding = getFingerHitPadding();
		if (control.disabled
				|| x < control.pos.x - padding || x >= control.pos.x + control.size.x + padding
				|| y < control.pos.y - padding || y >= control.pos.y + control.size.y + padding)
			continue;

		// Expanded hitboxes can overlap. Prefer the control whose original bounds are
		// closest to the touch instead of whichever happens to appear first.
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

static void drawButton(ImDrawList *drawList, const Control& control)
{
	if (control.disabled)
		return;
	float scale_h = settings.display.height / REFERENCE_HEIGHT;
	float offs_x = (settings.display.width - scale_h * REFERENCE_WIDTH) / 2.f;
	ImVec2 pos = control.pos * scale_h;
	ImVec2 size = control.size * scale_h;
	pos.x += offs_x;
	ControlId controlId = static_cast<ControlId>(&control - &Controls[0]);
	if (controlId == AnalogStick)
		pos += StickPos * size;

	float col = 0.25f * AlphaTrans;
	float elementAlpha = (MAX_PERCENT - std::clamp(control.transparency, 0, MAX_PERCENT)) / static_cast<float>(MAX_PERCENT);
	float alpha = AlphaTrans * elementAlpha;
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

void draw()
{
	if (Controls[Left].pos.x == 0.f)
	{
		loadLayout(getActiveLayoutProfile());
		if (Controls[Left].pos.x == 0.f)
			// mark done
			Controls[Left].pos.x = 1e-12f;
	}

	if (!config::VirtualGamepadEnabled && !Editing)
		return;
		
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	drawButton(drawList, Controls[Left]);
	drawButton(drawList, Controls[Up]);
	drawButton(drawList, Controls[Right]);
	drawButton(drawList, Controls[Down]);

	drawButton(drawList, Controls[X]);
	drawButton(drawList, Controls[Y]);
	drawButton(drawList, Controls[B]);
	drawButton(drawList, Controls[A]);

	drawButton(drawList, Controls[Start]);

	drawButton(drawList, Controls[LeftTrigger]);
	drawButton(drawList, Controls[RightTrigger]);

	drawButton(drawList, Controls[AnalogArea]);
	drawButton(drawList, Controls[AnalogStick]);

	drawButton(drawList, Controls[FastForward]);

	drawButton(drawList, Controls[Btn4]);
	drawButton(drawList, Controls[Btn5]);
	drawButton(drawList, Controls[ServiceMode]);
	drawButton(drawList, Controls[InsertCard]);

	drawSelectedElement();

	AlphaTrans += ((float)Visible - AlphaTrans) / 2;
}

static float getUIScale() {
	// scale is 1.1 for a 320 dpi screen of height 750
	return 1.1f * 750.f / settings.display.height * settings.display.dpi / 320.f;
}

static const char *getLayoutProfilePrefix(LayoutProfile profile)
{
	return profile == LayoutProfile::Console ? "console_" : "arcade_";
}

static bool hasSavedLayoutProfile(LayoutProfile profile)
{
	const std::string firstKey = std::string(getLayoutProfilePrefix(profile)) + "dpad_x";
	const std::vector<std::string> entries = config::getEntries(CFG_SECTION);
	return std::find(entries.begin(), entries.end(), firstKey) != entries.end();
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
	int transparency;	// per-unit transparency [0, 100]

	void load(LayoutProfile profile, bool useLegacyConsoleLayout)
	{
		const std::string keyPrefix = std::string(getLayoutProfilePrefix(profile)) + name;
		const auto legacyFloat = [this, useLegacyConsoleLayout](const char *suffix, float value) {
			return useLegacyConsoleLayout ? config::loadFloat(CFG_SECTION, name + suffix, value) : value;
		};
		const auto legacyInt = [this, useLegacyConsoleLayout](const char *suffix, int value) {
			return useLegacyConsoleLayout ? config::loadInt(CFG_SECTION, name + suffix, value) : value;
		};
		x = config::loadFloat(CFG_SECTION, keyPrefix + "_x", legacyFloat("_x", x));
		y = config::loadFloat(CFG_SECTION, keyPrefix + "_y", legacyFloat("_y", y));
		scale = std::clamp(config::loadFloat(CFG_SECTION, keyPrefix + "_scale", legacyFloat("_scale", scale)), MIN_ELEMENT_SCALE, MAX_ELEMENT_SCALE);
		transparency = std::clamp(config::loadInt(CFG_SECTION, keyPrefix + "_transparency", legacyInt("_transparency", transparency)), 0, MAX_PERCENT);
	}
	void save(LayoutProfile profile) const
	{
		const std::string keyPrefix = std::string(getLayoutProfilePrefix(profile)) + name;
		config::saveFloat(CFG_SECTION, keyPrefix + "_x", x);
		config::saveFloat(CFG_SECTION, keyPrefix + "_y", y);
		config::saveFloat(CFG_SECTION, keyPrefix + "_scale", scale);
		config::saveInt(CFG_SECTION, keyPrefix + "_transparency", transparency);
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

};
static LayoutElement Layout[Elem_Count] {
	{ "dpad",     Tnop("D-pad"),        32.f, -24.f, 192.f, 192.f },
	{ "buttons",  Tnop("Face Buttons"),-24.f, -24.f, 192.f, 192.f },
	{ "start",    Tnop("Start"),         0.f, -24.f,  64.f,  64.f },
	{ "LT",       Tnop("LT"),         -134.f,-240.f,  90.f,  64.f },
	{ "RT",       Tnop("RT"),          -32.f,-240.f,  90.f,  64.f },
	{ "analog",   Tnop("Stick"),        40.f,-320.f, 128.f, 128.f },
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
	float w;
	float h;
	float scale;
	int transparency;
};
static LayoutState EditedLayouts[2][Elem_Count];
static bool EditedLayoutLoaded[2] {};

static int getLayoutProfileIndex(LayoutProfile profile)
{
	return profile == LayoutProfile::Console ? 0 : 1;
}

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

static float getFingerHitPadding()
{
	return MAX_FINGER_HIT_PADDING * std::clamp(static_cast<int>(config::VirtualGamepadFingerSize), 0, MAX_PERCENT) / MAX_PERCENT;
}

static void applyControlSettings(ControlId control)
{
	Element element = controlToElement(control);
	if (!isValidElement(element))
		return;
	Controls[control].transparency = Layout[element].transparency;
}

static void applyControlSettings(ControlId first, ControlId last)
{
	for (int control = first; control <= last; control++)
		applyControlSettings(static_cast<ControlId>(control));
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
	applyControlSettings(Left, Down);
	applyControlSettings(LeftUp, RightDown);

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
	applyControlSettings(X, A);

	// Start
	scale = Layout[Elem_Start].scale * uiscale;
	Controls[Start].pos =   { Layout[Elem_Start].x * dcw - dx, Layout[Elem_Start].y * REFERENCE_HEIGHT };
	Controls[Start].size =  { Layout[Elem_Start].dw * scale, Layout[Elem_Start].dh * scale };
	applyControlSettings(Start);

	// Left trigger
	scale = Layout[Elem_LT].scale * uiscale;
	Controls[LeftTrigger].pos = { Layout[Elem_LT].x * dcw - dx, Layout[Elem_LT].y * REFERENCE_HEIGHT };
	Controls[LeftTrigger].size = { Layout[Elem_LT].dw * scale, Layout[Elem_LT].dh * scale };
	applyControlSettings(LeftTrigger);

	// Right trigger
	scale = Layout[Elem_RT].scale * uiscale;
	Controls[RightTrigger].pos =  { Layout[Elem_RT].x * dcw - dx, Layout[Elem_RT].y * REFERENCE_HEIGHT };
	Controls[RightTrigger].size = { Layout[Elem_RT].dw * scale, Layout[Elem_RT].dh * scale };
	applyControlSettings(RightTrigger);

	// Analog
	x = Layout[Elem_Analog].x * dcw - dx;
	y = Layout[Elem_Analog].y * REFERENCE_HEIGHT;
	scale = Layout[Elem_Analog].scale * uiscale;
	Controls[AnalogArea].pos =   { x, y };
	Controls[AnalogArea].size =  { Layout[Elem_Analog].dw * scale, Layout[Elem_Analog].dh * scale };
	Controls[AnalogStick].pos =  { x + 32.f * scale, y + 32.f * scale };
	Controls[AnalogStick].size = { 64.f * scale, 64.f * scale };
	applyControlSettings(AnalogArea);
	applyControlSettings(AnalogStick);

	// Fast forward
	scale = Layout[Elem_FForward].scale * uiscale;
	Controls[FastForward].pos =  { Layout[Elem_FForward].x * dcw - dx, Layout[Elem_FForward].y * REFERENCE_HEIGHT };
	Controls[FastForward].size = { Layout[Elem_FForward].dw * scale, Layout[Elem_FForward].dh * scale };
	applyControlSettings(FastForward);

	// ARCADE
	// Button 4
	scale = Layout[Elem_Btn4].scale * uiscale;
	Controls[Btn4].pos =  { Layout[Elem_Btn4].x * dcw - dx, Layout[Elem_Btn4].y * REFERENCE_HEIGHT };
	Controls[Btn4].size = { Layout[Elem_Btn4].dw * scale, Layout[Elem_Btn4].dh * scale };
	applyControlSettings(Btn4);
	// Button 5
	scale = Layout[Elem_Btn5].scale * uiscale;
	Controls[Btn5].pos =  { Layout[Elem_Btn5].x * dcw - dx, Layout[Elem_Btn5].y * REFERENCE_HEIGHT };
	Controls[Btn5].size = { Layout[Elem_Btn5].dw * scale, Layout[Elem_Btn5].dh * scale };
	applyControlSettings(Btn5);

	// Service Mode
	scale = Layout[Elem_ServiceMode].scale * uiscale;
	Controls[ServiceMode].pos =  { Layout[Elem_ServiceMode].x * dcw - dx, Layout[Elem_ServiceMode].y * REFERENCE_HEIGHT };
	Controls[ServiceMode].size = { Layout[Elem_ServiceMode].dw * scale, Layout[Elem_ServiceMode].dh * scale };
	applyControlSettings(ServiceMode);

	// Insert Card
	scale = Layout[Elem_InsertCard].scale * uiscale;
	Controls[InsertCard].pos =  { Layout[Elem_InsertCard].x * dcw - dx, Layout[Elem_InsertCard].y * REFERENCE_HEIGHT };
	Controls[InsertCard].size = { Layout[Elem_InsertCard].dw * scale, Layout[Elem_InsertCard].dh * scale };
	applyControlSettings(InsertCard);
}

void applyUiScale() {
	for (auto& element : Layout)
		element.applyUiScale();
}

static void applyProfileDefaultLayout(LayoutProfile profile)
{
	if (profile != LayoutProfile::Arcade)
		return;

	// Keep the arcade trigger pair above buttons 5 and 6 instead of sharing their touch area.
	const float triggerLift = 48.f / REFERENCE_HEIGHT * getUIScale();
	Layout[Elem_LT].y -= triggerLift;
	Layout[Elem_RT].y -= triggerLift;
}

static void loadLayout(LayoutProfile profile)
{
	const bool hasSavedLayout = hasSavedLayoutProfile(profile);
	const bool useLegacyConsoleLayout = profile == LayoutProfile::Console && !hasSavedLayout;
	for (auto& element : Layout) {
		element.reset();
		element.load(profile, useLegacyConsoleLayout);
	}
	if (!hasSavedLayout)
		applyProfileDefaultLayout(profile);
	applyLayout();
}

static void saveLayout(LayoutProfile profile)
{
	// A completed edit can update every unit. Batch the writes so the config file
	// is flushed once rather than once per property.
	config::setAutoSave(false);
	for (auto& element : Layout)
		element.save(profile);
	config::setAutoSave(true);
}

static void storeEditingLayout()
{
	const int index = getLayoutProfileIndex(EditingLayoutProfile);
	for (int i = 0; i < Elem_Count; i++)
	{
		const LayoutElement& source = Layout[i];
		EditedLayouts[index][i] = { source.x, source.y, source.w, source.h, source.scale,
			source.transparency };
	}
	EditedLayoutLoaded[index] = true;
}

static void restoreEditingLayout(LayoutProfile profile)
{
	const LayoutState *source = EditedLayouts[getLayoutProfileIndex(profile)];
	for (int i = 0; i < Elem_Count; i++)
	{
		LayoutElement& destination = Layout[i];
		destination.x = source[i].x;
		destination.y = source[i].y;
		destination.w = source[i].w;
		destination.h = source[i].h;
		destination.scale = source[i].scale;
		destination.transparency = source[i].transparency;
	}
	applyLayout();
}

static void selectEditingLayoutProfile(LayoutProfile profile)
{
	if (profile == EditingLayoutProfile)
		return;
	storeEditingLayout();
	EditingLayoutProfile = profile;
	const int index = getLayoutProfileIndex(profile);
	if (EditedLayoutLoaded[index])
		restoreEditingLayout(profile);
	else
	{
		loadLayout(profile);
		storeEditingLayout();
	}
}

static void resetLayout()
{
	for (auto& element : Layout)
		element.reset();
	applyProfileDefaultLayout(getActiveLayoutProfile());
	applyLayout();
}

static void resetElement(Element element)
{
	if (!isValidElement(element))
		return;
	Layout[element].reset();
	applyLayout();
}

static bool getElementScreenBounds(Element element, ImVec2& min, ImVec2& max)
{
	if (!isValidElement(element))
		return false;

	const LayoutElement& e = Layout[element];
	const float dcw = REFERENCE_HEIGHT * (float)settings.display.width / settings.display.height;
	const float dx = (dcw - REFERENCE_WIDTH) / 2;
	const float scaleH = settings.display.height / REFERENCE_HEIGHT;
	const float offsetX = (settings.display.width - scaleH * REFERENCE_WIDTH) / 2.f;
	const ImVec2 dcMin(e.x * dcw - dx, e.y * REFERENCE_HEIGHT);
	const ImVec2 dcSize(e.w * e.scale * dcw, e.h * e.scale * REFERENCE_HEIGHT);
	min = dcMin * scaleH;
	min.x += offsetX;
	max = (dcMin + dcSize) * scaleH;
	max.x += offsetX;
	return true;
}

static bool pointInRect(const ImVec2& point, const ImVec2& min, const ImVec2& max)
{
	return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
}

static void drawSelectedElement()
{
	if (!Editing || !isValidElement(SelectedElement))
		return;

	ImVec2 min, max;
	if (!getElementScreenBounds(SelectedElement, min, max))
		return;

	const float pad = uiScaled(4.f);
	min -= ImVec2(pad, pad);
	max += ImVec2(pad, pad);
	// Use the background list so the customization panel always covers controls
	// and selection outlines that happen to sit underneath it.
	ImDrawList *drawList = ImGui::GetBackgroundDrawList();
	drawList->AddRect(min, max, IM_COL32(34, 161, 110, 230), uiScaled(8.f), 0, uiScaled(2.f));

}

static void drawTouchIndicator(const ImVec2& panelMin, const ImVec2& panelMax)
{
	if (!Editing)
		return;

	ImGuiIO& io = ImGui::GetIO();
	if (!io.MouseDown[ImGuiMouseButton_Left] || pointInRect(io.MousePos, panelMin, panelMax))
		return;

	// Android routes the active setup-screen touch through ImGui's primary pointer.
	ImDrawList *drawList = ImGui::GetForegroundDrawList();
	const ImVec2 pos = io.MousePos;
	const float radius = uiScaled(6.f) + getFingerHitPadding() * settings.display.height / REFERENCE_HEIGHT;
	drawList->AddCircleFilled(pos, uiScaled(4.f), IM_COL32(255, 255, 255, 245), 24);
	drawList->AddCircle(pos, radius, IM_COL32(255, 202, 67, 245), 32, uiScaled(2.f));
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

	// Keep the visual center fixed when scaling from either the Java gesture or
	// the ImGui slider, including when the requested value hits a clamp limit.
	selected.x += selected.w * (selected.scale - clampedScale) / 2.f;
	selected.y += selected.h * (selected.scale - clampedScale) / 2.f;
	selected.scale = clampedScale;
	applyLayout();
}

static bool drawCustomizeSlider(const char *id, const char *label, int& value, int min, int max,
		const char *format = "%d%%")
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(uiScaled(150.f));
	ImGui::SetNextItemWidth(uiScaled(220.f));
	ImguiID _(id);
	return ImGui::SliderInt("##Value", &value, min, max, format);
}

static bool drawCustomizeCheckbox(const char *id, const char *label, bool& value)
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(uiScaled(220.f));
	ImguiID _(id);
	return ImGui::Checkbox("##Value", &value);
}

static void drawCustomizePanel()
{
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const float width = std::min(displaySize.x - uiScaled(24.f), uiScaled(430.f));
	const float height = std::min(displaySize.y * 0.45f, uiScaled(360.f));
	ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, uiScaled(8.f)), ImGuiCond_Appearing, ImVec2(0.5f, 0.f));
	ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
	ImGui::Begin("##VirtualGamepadCustomize", nullptr,
			ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_DragScrolling);
	// Keep controller editing gestures inside the panel from moving it. The title bar remains its only drag handle.
	ImGui::GetCurrentWindow()->BgClickFlags &= ~ImGuiWindowBgClickFlags_Move;

	ImGui::TextUnformatted(i18n::T("Virtual Gamepad"));
	ImGui::Separator();

	const char *layoutProfileNames[] = { i18n::T("Dreamcast"), i18n::T("Arcade") };
	int layoutProfile = getLayoutProfileIndex(EditingLayoutProfile);
	ImGui::TextUnformatted(i18n::T("Layout Profile"));
	ImGui::SetNextItemWidth(uiScaled(220.f));
	if (ImGui::Combo("##LayoutProfile", &layoutProfile, layoutProfileNames, IM_ARRAYSIZE(layoutProfileNames)))
		selectEditingLayoutProfile(layoutProfile == 0 ? LayoutProfile::Console : LayoutProfile::Arcade);

	if (!isValidElement(SelectedElement))
		SelectedElement = Elem_DPad;

	ImGui::Text("%s: %s", i18n::T("Selected"), getElementLabel(SelectedElement));

	ImGui::Separator();
	bool enabled = config::VirtualGamepadEnabled;
	bool hapticFeedback = config::VirtualGamepadHapticFeedback;
	if (drawCustomizeCheckbox("Enabled", i18n::T("Show virtual gamepad"), enabled))
		config::VirtualGamepadEnabled = enabled;
	if (drawCustomizeCheckbox("HapticFeedback", i18n::T("Vibration"), hapticFeedback))
		config::VirtualGamepadHapticFeedback = hapticFeedback;

	int vibration = std::clamp(static_cast<int>(config::VirtualGamepadVibration), 0, MAX_PERCENT);
	if (drawCustomizeSlider("VibrationPower", i18n::T("Vibration Power"), vibration, 0, MAX_PERCENT)
			|| vibration != static_cast<int>(config::VirtualGamepadVibration))
		config::VirtualGamepadVibration = vibration;

	int timeout = std::clamp(static_cast<int>(config::VirtualGamepadIdleTimeout), 0, MAX_IDLE_TIMEOUT_SECONDS);
	if (drawCustomizeSlider("IdleTimeout", i18n::T("Idle Timeout (s)"), timeout, 0,
			MAX_IDLE_TIMEOUT_SECONDS, "%d") || timeout != static_cast<int>(config::VirtualGamepadIdleTimeout))
		config::VirtualGamepadIdleTimeout = timeout;

	int fingerSize = std::clamp(static_cast<int>(config::VirtualGamepadFingerSize), 0, MAX_PERCENT);
	if (drawCustomizeSlider("FingerSize", i18n::T("Finger Touch Size"), fingerSize, 0, MAX_PERCENT)
			|| fingerSize != static_cast<int>(config::VirtualGamepadFingerSize))
		config::VirtualGamepadFingerSize = fingerSize;

	ImGui::Separator();
	LayoutElement& selected = Layout[SelectedElement];
	int unitTransparency = selected.transparency;
	int scale = getElementScalePercent(SelectedElement);
	bool elementSettingsChanged = drawCustomizeSlider("UnitTransparency", i18n::T("Unit Transparency"),
			unitTransparency, 0, MAX_PERCENT);
	elementSettingsChanged |= drawCustomizeSlider("Scale", i18n::T("Scaling"), scale,
			static_cast<int>(MIN_ELEMENT_SCALE * MAX_PERCENT), static_cast<int>(MAX_ELEMENT_SCALE * MAX_PERCENT));
	if (elementSettingsChanged)
	{
		selected.transparency = std::clamp(unitTransparency, 0, MAX_PERCENT);
		const float requestedScale = scale / static_cast<float>(MAX_PERCENT);
		if (requestedScale != selected.scale)
			setElementScale(SelectedElement, requestedScale);
		else
			applyLayout();
	}

	if (ImGui::Button(i18n::T("Reset Unit"), ScaledVec2(130, 32)))
		resetElement(SelectedElement);
	ImGui::SameLine();
	if (ImGui::Button(i18n::T("Reset Layout"), ScaledVec2(130, 32)))
		resetLayout();

	const std::string currentImagePath = config::loadStr(CFG_SECTION, getButtonsCfgName());
	const ButtonImagePreset *currentPreset = findButtonImagePreset(currentImagePath);
	const char *imagePreview = currentPreset != nullptr ? i18n::T(currentPreset->label)
			: currentImagePath.empty() ? i18n::T("Default") : i18n::T("Custom Image");
	ImGui::TextUnformatted(i18n::T("Controller Image"));
	ImGui::SetNextItemWidth(uiScaled(220.f));
	if (ImGui::BeginCombo("##ControllerImage", imagePreview))
	{
		if (ImGui::Selectable(i18n::T("Default"), currentImagePath.empty()))
			loadImage(Tnop(""));
		size_t presetCount;
		const ButtonImagePreset *presets = getButtonImagePresets(presetCount);
		for (size_t i = 0; i < presetCount; i++)
		{
			const std::string presetPath = std::string(PRESET_PREFIX) + presets[i].fileName;
			const bool selected = currentImagePath == presetPath;
			if (ImGui::Selectable(i18n::T(presets[i].label), selected))
				loadImage(presetPath);
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}

	const char *gamepadPngTitle = i18n::T("Select a Controller PNG");
	if (ImGui::Button(i18n::T("Change Image..."), ScaledVec2(130, 32)))
#ifdef __ANDROID__
	{
		if (!hostfs::addStorage(false, false, gamepadPngTitle, [](bool cancelled, std::string path) {
			if (!cancelled)
				gui_runOnUiThread([path]() {
					loadImage(path);
				});
		}, Tnop("image/png")))
			ImGui::OpenPopup(gamepadPngTitle);
	}
#else
	{
		ImGui::OpenPopup(gamepadPngTitle);
	}
#endif
	ImGui::SameLine();
	if (ImGui::Button(i18n::T("Use Default"), ScaledVec2(130, 32)))
		loadImage(Tnop(""));

	select_file_popup(gamepadPngTitle, [](bool cancelled, std::string selection)
			{
				if (!cancelled)
					gui_runOnUiThread([selection]() {
						loadImage(selection);
					});
				return true;
			}, true, Tnop("png"));

	ImGui::Separator();
	if (ImGui::Button(i18n::T("Done"), ScaledVec2(110, 36)))
	{
		stopEditing(false);
		gui_setState(GuiState::Settings);
	}
	ImGui::SameLine();
	if (ImGui::Button(i18n::T("Cancel"), ScaledVec2(110, 36)))
	{
		stopEditing(true);
		gui_setState(GuiState::Settings);
	}

	const ImVec2 panelMin = ImGui::GetWindowPos();
	const ImVec2 panelMax = panelMin + ImGui::GetWindowSize();
	if (!ImGui::GetCurrentWindow()->TitleBarRect().Contains(ImGui::GetIO().MousePos))
	{
		scrollWhenDraggingOnVoid();
		windowDragScroll();
	}
	ImGui::End();

	handleCustomizeDrag(panelMin, panelMax);
	drawTouchIndicator(panelMin, panelMax);
}

static void handleCustomizeDrag(const ImVec2& panelMin, const ImVec2& panelMax)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f)
		return;

	const ImVec2 mouse = io.MousePos;
	const bool mouseInPanel = pointInRect(mouse, panelMin, panelMax);

	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !mouseInPanel)
	{
		DraggedElement = layoutHitTest(mouse.x / io.DisplaySize.x, mouse.y / io.DisplaySize.y);
		LastDragMousePos = mouse;
	}
	else if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && isValidElement(DraggedElement))
	{
		ImVec2 delta = mouse - LastDragMousePos;
		translateElement(DraggedElement, delta.x / io.DisplaySize.x, delta.y / io.DisplaySize.y);
		LastDragMousePos = mouse;
	}
	else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		DraggedElement = Elem_None;
	}
}

Element layoutHitTest(float x, float y)
{
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
	setElementScale(element, Layout[element].scale * factor);
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
	loadLayout(getRuntimeLayoutProfile());
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

void startEditing()
{
	if (!Editing)
	{
		EditingLayoutProfile = getRuntimeLayoutProfile();
		EditedLayoutLoaded[0] = false;
		EditedLayoutLoaded[1] = false;
		loadLayout(EditingLayoutProfile);
		storeEditingLayout();
		EditSession = {
			static_cast<bool>(config::VirtualGamepadEnabled),
			static_cast<bool>(config::VirtualGamepadHapticFeedback),
			static_cast<int>(config::VirtualGamepadVibration),
			static_cast<int>(config::VirtualGamepadIdleTimeout),
			static_cast<int>(config::VirtualGamepadFingerSize),
			config::loadStr(CFG_SECTION, "image", ""),
			config::loadStr(CFG_SECTION, "image_arcade", ""),
		};
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
	const LayoutProfile runtimeProfile = getRuntimeLayoutProfile();
	setEditMode(false);
	DraggedElement = Elem_None;
	if (canceled)
	{
		config::VirtualGamepadEnabled = EditSession.enabled;
		config::VirtualGamepadHapticFeedback = EditSession.hapticFeedback;
		config::VirtualGamepadVibration = EditSession.vibration;
		config::VirtualGamepadIdleTimeout = EditSession.idleTimeout;
		config::VirtualGamepadFingerSize = EditSession.fingerSize;
		EditingLayoutProfile = LayoutProfile::Console;
		if (config::loadStr(CFG_SECTION, getButtonsCfgName(), "") != EditSession.consoleImagePath)
			loadImage(EditSession.consoleImagePath);
		EditingLayoutProfile = LayoutProfile::Arcade;
		if (config::loadStr(CFG_SECTION, getButtonsCfgName(), "") != EditSession.arcadeImagePath)
			loadImage(EditSession.arcadeImagePath);
	}
	else
	{
		storeEditingLayout();
		for (LayoutProfile profile : { LayoutProfile::Console, LayoutProfile::Arcade })
		{
			const int index = getLayoutProfileIndex(profile);
			if (!EditedLayoutLoaded[index])
				continue;
			EditingLayoutProfile = profile;
			restoreEditingLayout(profile);
			saveLayout(profile);
		}
	}
	Editing = false;
	loadLayout(runtimeProfile);
}

}	// namespace vgamepad

#endif // __ANDROID__ || TARGET_IPHONE
