/*
	Copyright 2019 flyinghead

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
#include "settings_new.h"
#include "gui_util.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "audio/audiostream.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "video_preset.h"
#include "network/ice.h"
#include "network/ggpo.h"
#include "widgets/widgets.h"
#include "version.h"
#include "wsi/context.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#ifdef USE_DREAMLINK_DEVICES
#include "sdl/dreamlink.h"
#endif
#include "log/LogManager.h"
#include "hw/mem/addrspace.h"
#include "settings.h"
#include "gui.h"
#include "boxart/boxart.h"
#include "IconsFontAwesome6.h"
#include "mainui.h"
#include "oslib/storage.h"
#include "stdclass.h"
#include "achievements/achievements.h"
#include "vgamepad.h"
#ifdef __ANDROID__
#if HOST_CPU == CPU_ARM64 && USE_VULKAN
#include "rend/vulkan/adreno.h"
#endif
#endif

extern ImFont *largeFont;
extern ImFont *settingsTitleFont;
extern ImFont *settingsRightValueFont;

namespace SettingsNew {

// Static state for gamepad settings popup
static std::shared_ptr<GamepadDevice> g_currentGamepadForSettings;
static float g_twoLineRowExtraHeightPx = 0.0f;
static bool g_twoLineRowSeparatorsEnabled = false;
static float g_twoLineRowSeparatorAlpha = 0.5f;
static float g_twoLineRowExtraGapPx = 0.0f;
static bool g_mapleDevicesChangedInSettings = false;
static bool g_scrollToBoxArtSection = false;

// Visual-only toggle switch rendering (no interaction)
// Renders the toggle switch appearance based on the boolean value
// The parent Selectable handles all user input
static void RenderToggleSwitchVisual(bool value)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return;

	const float height = settings.display.uiScale * 24;
	const float width = settings.display.uiScale * 48;
	const float radius = height * 0.5f;
	const ImVec2 pos = window->DC.CursorPos;
	const ImVec2 size(width, height);

	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);

	// Animation
	float anim = value ? 1.0f : 0.0f;

	// Render background
	ImU32 col_bg;
	if (value)
		col_bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
	else
		col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);

	// Render knob - use theme-aware color
	ImVec2 knob_pos;
	knob_pos.x = ImLerp(bb.Min.x + radius, bb.Max.x - radius, anim);
	knob_pos.y = bb.Min.y + radius;

	// Detect light vs dark theme based on window background luminance
	ImGuiStyle& guiStyle = ImGui::GetStyle();
	float bgLuminance = (guiStyle.Colors[ImGuiCol_WindowBg].x
	                   + guiStyle.Colors[ImGuiCol_WindowBg].y
	                   + guiStyle.Colors[ImGuiCol_WindowBg].z) / 3.0f;
	bool isLightTheme = bgLuminance > 0.5f;

	// Use CheckMark color for knob (theme's accent color) or contrasting color
	ImU32 knobColor = isLightTheme
		? ImGui::GetColorU32(ImGuiCol_Text)  // Dark text color for light themes
		: IM_COL32(255, 255, 255, 255);      // White for dark themes
	draw_list->AddCircleFilled(knob_pos, radius - 1.0f, knobColor);
}

static float TwoLineSettingContentHeight()
{
	return ImGui::GetTextLineHeightWithSpacing() * 2.0f;
}

static float TwoLineSettingRowHeight()
{
	return TwoLineSettingContentHeight() + uiScaled(g_twoLineRowExtraHeightPx);
}

static float TwoLineSettingContentOffsetY()
{
	return std::max(0.0f, (TwoLineSettingRowHeight() - TwoLineSettingContentHeight()) * 0.5f);
}

static float RightColumnX(float itemWidth, float rightPadding = 28.0f)
{
	const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	return rowRightX - itemWidth - uiScaled(rightPadding);
}

class ScopedTwoLineRowStyle
{
public:
	ScopedTwoLineRowStyle(float rowHeightExtraPx, bool drawSeparators, float separatorAlpha = 0.5f, float extraGapPx = 0.0f)
		: m_prevRowHeightExtraPx(g_twoLineRowExtraHeightPx)
		, m_prevDrawSeparators(g_twoLineRowSeparatorsEnabled)
		, m_prevSeparatorAlpha(g_twoLineRowSeparatorAlpha)
		, m_prevExtraGapPx(g_twoLineRowExtraGapPx)
	{
		g_twoLineRowExtraHeightPx = rowHeightExtraPx;
		g_twoLineRowSeparatorsEnabled = drawSeparators;
		g_twoLineRowSeparatorAlpha = separatorAlpha;
		g_twoLineRowExtraGapPx = extraGapPx;
	}

	~ScopedTwoLineRowStyle()
	{
		g_twoLineRowExtraHeightPx = m_prevRowHeightExtraPx;
		g_twoLineRowSeparatorsEnabled = m_prevDrawSeparators;
		g_twoLineRowSeparatorAlpha = m_prevSeparatorAlpha;
		g_twoLineRowExtraGapPx = m_prevExtraGapPx;
	}

private:
	float m_prevRowHeightExtraPx;
	bool m_prevDrawSeparators;
	float m_prevSeparatorAlpha;
	float m_prevExtraGapPx;
};

static ImVec2 BeginTwoLineSettingRowContent()
{
	ImGui::SameLine(0, 0);
	ImVec2 line1Start = ImGui::GetCursorPos();
	line1Start.y += TwoLineSettingContentOffsetY();
	ImGui::SetCursorPos(line1Start);
	return line1Start;
}

static bool BeginTwoLineSettingRow(const char* rowId, const char* tooltip = nullptr, bool disabled = false)
{
	ImGuiSelectableFlags rowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap;
	if (disabled)
		rowFlags |= ImGuiSelectableFlags_Disabled;

	ImguiStyleVar rowRounding(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
	const bool clicked = ImGui::Selectable(rowId, false, rowFlags, ImVec2(0.0f, TwoLineSettingRowHeight()));
	if (!disabled && tooltip != nullptr && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", tooltip);

	if (g_twoLineRowSeparatorsEnabled)
	{
		const ImVec2 rowMin = ImGui::GetItemRectMin();
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		const float lineInset = uiScaled(6.0f);
		ImVec4 lineColor = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		lineColor.w *= g_twoLineRowSeparatorAlpha;
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(rowMin.x + lineInset, rowMax.y - 1.0f),
			ImVec2(rowMax.x - lineInset, rowMax.y - 1.0f),
			ImGui::ColorConvertFloat4ToU32(lineColor),
			1.0f
		);
	}

	return clicked && !disabled;
}

static void RenderTwoLineSettingDescription(const ImVec2& line1Start, const char* description)
{
	if (description == nullptr || description[0] == '\0')
		return;

	ImGui::SetCursorPos(ImVec2(
		line1Start.x + uiScaled(28.0f),
		line1Start.y + ImGui::GetTextLineHeightWithSpacing()
	));
	ImGui::PushFont(settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont());

	// Determine if we're on a light or dark theme
	ImGuiStyle& guiStyle = ImGui::GetStyle();
	float bgLuminance = (guiStyle.Colors[ImGuiCol_WindowBg].x
	                   + guiStyle.Colors[ImGuiCol_WindowBg].y
	                   + guiStyle.Colors[ImGuiCol_WindowBg].z) / 3.0f;
	bool isLightTheme = bgLuminance > 0.5f;

	// For dark themes: use solid white text
	// For light themes: blend with disabled color for secondary text
	ImVec4 textColor;
	if (isLightTheme) {
		ImVec4 normalColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
		ImVec4 disabledColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
		textColor.x = (normalColor.x + disabledColor.x) * 0.5f;
		textColor.y = (normalColor.y + disabledColor.y) * 0.5f;
		textColor.z = (normalColor.z + disabledColor.z) * 0.5f;
		textColor.w = 1.0f;
	} else {
		// Solid white for dark themes
		textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	}
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	ImGui::TextUnformatted(description);
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

static const char* GetPopupId(const SettingsUI::PopupConfig& popupCfg)
{
	switch (popupCfg.type)
	{
	case SettingsUI::PopupType::Options:
		return popupCfg.options.popupID;
	case SettingsUI::PopupType::Slider:
		return popupCfg.slider.popupID;
	default:
		return nullptr;
	}
}

static bool RenderGeneralPopupSettingRow(const char* id, const char* description,
	SettingsUI::PopupConfig& popupCfg, const char* tooltip = nullptr, bool disabled = false)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	if (popupCfg.type == SettingsUI::PopupType::Options)
	{
		popupCfg.options.valueClickable = false;
		if ((popupCfg.options.title == nullptr || popupCfg.options.title[0] == '\0')
			&& popupCfg.options.label != nullptr && popupCfg.options.label[0] != '\0')
			popupCfg.options.title = popupCfg.options.label;
		if ((popupCfg.options.description == nullptr || popupCfg.options.description[0] == '\0')
			&& rowTooltip != nullptr && rowTooltip[0] != '\0')
			popupCfg.options.description = rowTooltip;
	}
	else if (popupCfg.type == SettingsUI::PopupType::Slider)
	{
		if ((popupCfg.slider.description == nullptr || popupCfg.slider.description[0] == '\0')
			&& rowTooltip != nullptr && rowTooltip[0] != '\0')
			popupCfg.slider.description = rowTooltip;
		popupCfg.slider.preferTextEntry = false;
	}

	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	if (rowActivated)
	{
		const char* popupId = GetPopupId(popupCfg);
		if (popupId != nullptr && popupId[0] != '\0')
		{
			if (popupCfg.type == SettingsUI::PopupType::Slider)
				popupCfg.slider.preferTextEntry = ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left];
			ImGui::OpenPopup(popupId);
		}
	}

	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	// Vertically center right-side value text within the full highlighted row.
	// Cursor is already at row content start, so offset from that baseline.
	const float centeredValueOffset = (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f;
	if (popupCfg.type == SettingsUI::PopupType::Options)
		popupCfg.options.valueVerticalOffset = centeredValueOffset;
	else if (popupCfg.type == SettingsUI::PopupType::Slider)
		popupCfg.slider.valueVerticalOffset = centeredValueOffset;

	{
		DisabledScope _(disabled);
		SettingsUI::SettingPopup(popupCfg);
	}
	RenderTwoLineSettingDescription(line1Start, description);
	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	return rowActivated;
}

template <typename OnToggle>
static bool RenderGeneralToggleSettingRow(const char* id, const char* icon, const char* label,
	const char* description, bool value, OnToggle&& onToggle, const char* tooltip = nullptr,
	bool disabled = false)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	if (rowActivated)
	{
		value = !value;
		onToggle(value);
	}

	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	{
		DisabledScope _(disabled);
		SettingIcon(icon, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(label);
		ImGui::PopFont();

		const float toggleWidth = uiScaled(50.0f);
		ImGui::SameLine(RightColumnX(toggleWidth));

		const float toggleHeight = settings.display.uiScale * 24.0f;
		const float verticalOffset = (TwoLineSettingContentHeight() - toggleHeight) * 0.5f;
		const ImVec2 togglePos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(togglePos.x, togglePos.y + verticalOffset));
		RenderToggleSwitchVisual(value);
	}

	RenderTwoLineSettingDescription(line1Start, description);
	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	return rowActivated;
}

template <typename DrawInput>
static void RenderGeneralTextInputSettingRow(const char* id, const char* icon, const char* label,
	const char* description, DrawInput&& drawInput, const char* tooltip = nullptr, bool disabled = false,
	float inputWidthPx = 220.0f)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	{
		DisabledScope _(disabled);
		SettingIcon(icon, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(label);
		ImGui::PopFont();

		const float inputWidth = uiScaled(inputWidthPx);
		ImGui::SameLine(RightColumnX(inputWidth));
		if (rowActivated)
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(inputWidth);
		drawInput();
	}

	RenderTwoLineSettingDescription(line1Start, description);
	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
}

// Gamepad Settings Popup (ported from settings_controls.cpp)
// Handles rumble, deadzone, saturation, and virtual gamepad settings
static void gamepadSettingsPopup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	centerNextWindow();
	ImGui::SetNextWindowSize(min(ImGui::GetIO().DisplaySize, ScaledVec2(450.f, 300.f)));

	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::BeginPopupModal("Gamepad Settings", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_DragScrolling))
	{
		if (ImGui::Button("Done", ScaledVec2(100, 30)))
		{
			gamepad->save_mapping();
			// Update both console and arcade profile/mapping
			int rumblePower = gamepad->get_rumble_power();
			float deadzone = gamepad->get_dead_zone();
			float saturation = gamepad->get_saturation();
			int otherPlatform = settings.platform.isConsole() ? DC_PLATFORM_NAOMI : DC_PLATFORM_DREAMCAST;
			if (!gamepad->find_mapping(otherPlatform))
				if (otherPlatform == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
					gamepad->resetMappingToDefault(otherPlatform != DC_PLATFORM_DREAMCAST, true);
			std::shared_ptr<InputMapping> mapping = gamepad->get_input_mapping();
			if (mapping != nullptr)
			{
				if (gamepad->is_rumble_enabled() && rumblePower != mapping->rumblePower) {
					mapping->rumblePower = rumblePower;
					mapping->set_dirty();
				}
				if (gamepad->has_analog_stick())
				{
					if (deadzone != mapping->dead_zone) {
						mapping->dead_zone = deadzone;
						mapping->set_dirty();
					}
					if (saturation != mapping->saturation) {
						mapping->saturation = saturation;
						mapping->set_dirty();
					}
				}
				if (mapping->is_dirty())
					gamepad->save_mapping(otherPlatform);
			}
			gamepad->find_mapping();

			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::NewLine();
		if (gamepad->is_virtual_gamepad())
		{
			if (gamepad->is_rumble_enabled()) {
				header("Haptic");
				OptionSlider("Power", config::VirtualGamepadVibration, 0, 100, "Haptic feedback power", "%d%%");
			}
			header("View");
			OptionSlider("Transparency", config::VirtualGamepadTransparency, 0, 100, "Virtual gamepad buttons transparency", "%d%%");

#if defined(__ANDROID__) || defined(TARGET_IPHONE)
			vgamepad::ImguiVGamepadTexture tex;
			ImGui::Image(tex.getId(), ScaledVec2(300.f, 150.f), ImVec2(0, 1), ImVec2(1, 0));
#endif
			const char *gamepadPngTitle = "Select a PNG file";
			if (ImGui::Button("Choose Image...", ScaledVec2(150, 30)))
#ifdef __ANDROID__
			{
				if (!hostfs::addStorage(false, false, gamepadPngTitle, [](bool cancelled, std::string path) {
					if (!cancelled)
						gui_runOnUiThread([path]() {
							vgamepad::loadImage(path);
						});
					return true;
				}, "image/png"))
					ImGui::OpenPopup(gamepadPngTitle);
			}
#else
			{
				ImGui::OpenPopup(gamepadPngTitle);
			}
#endif
			ImGui::SameLine();
			if (ImGui::Button("Use Default", ScaledVec2(150, 30)))
				vgamepad::loadImage("");

			select_file_popup(gamepadPngTitle, [](bool cancelled, std::string selection)
				{
					if (!cancelled)
						gui_runOnUiThread([selection]() {
							vgamepad::loadImage(selection);
						});
					return true;
				}, true, "png");
		}
		else if (gamepad->is_rumble_enabled())
		{
			header("Rumble");
			int power = gamepad->get_rumble_power();
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Power", &power, 0, 100, "%d%%"))
				gamepad->set_rumble_power(power);
			ImGui::SameLine();
			ShowHelpMarker("Rumble power");
		}
		if (gamepad->has_analog_stick())
		{
			header("Thumbsticks");
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Dead zone", &deadzone, 0, 100, "%d%%"))
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("Minimum deflection to register as input");
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Saturation", &saturation, 50, 200, "%d%%"))
				gamepad->set_saturation(saturation / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("Value sent to the game at 100% thumbstick deflection. "
					"Values greater than 100% will saturate before full deflection of the thumbstick.");
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
		ImGui::EndPopup();
	}
}

// ============================================================
// CONTROLLER MAPPING - Static state
// ============================================================
static std::shared_ptr<GamepadDevice> g_currentGamepadForMapping;
static InputMapping::InputSet g_mapped_codes;  // Stores multiple buttons in the order they were entered
static u64 g_map_start_time;
static bool g_arcade_button_mode;
static u32 g_gamepad_port_for_mapping;
static std::unordered_set<DreamcastKey> g_buttonState;

static const char* const kMaplePorts[] = { "None", "A", "B", "C", "D", "All" };

struct MappingEntry
{
	DreamcastKey key;
	const char* name;
};

static const MappingEntry kDreamcastControls[] = {
	{ EMU_BTN_NONE, "Directions" },
	{ DC_DPAD_UP, "Up" },
	{ DC_DPAD_DOWN, "Down" },
	{ DC_DPAD_LEFT, "Left" },
	{ DC_DPAD_RIGHT, "Right" },

	{ DC_AXIS_UP, "Thumbstick Up" },
	{ DC_AXIS_DOWN, "Thumbstick Down" },
	{ DC_AXIS_LEFT, "Thumbstick Left" },
	{ DC_AXIS_RIGHT, "Thumbstick Right" },

	{ DC_AXIS2_UP, "R.Thumbstick Up" },
	{ DC_AXIS2_DOWN, "R.Thumbstick Down" },
	{ DC_AXIS2_LEFT, "R.Thumbstick Left" },
	{ DC_AXIS2_RIGHT, "R.Thumbstick Right" },

	{ DC_AXIS3_UP, "Axis 3 Up" },
	{ DC_AXIS3_DOWN, "Axis 3 Down" },
	{ DC_AXIS3_LEFT, "Axis 3 Left" },
	{ DC_AXIS3_RIGHT, "Axis 3 Right" },

	{ DC_DPAD2_UP, "DPad2 Up" },
	{ DC_DPAD2_DOWN, "DPad2 Down" },
	{ DC_DPAD2_LEFT, "DPad2 Left" },
	{ DC_DPAD2_RIGHT, "DPad2 Right" },

	{ EMU_BTN_NONE, "Buttons" },
	{ DC_BTN_A, "A" },
	{ DC_BTN_B, "B" },
	{ DC_BTN_X, "X" },
	{ DC_BTN_Y, "Y" },
	{ DC_BTN_C, "C" },
	{ DC_BTN_D, "D" },
	{ DC_BTN_Z, "Z" },

	{ EMU_BTN_NONE, "Triggers" },
	{ DC_AXIS_LT, "Left Trigger" },
	{ DC_AXIS_RT, "Right Trigger" },
	{ DC_AXIS_LT2, "Left Trigger 2" },
	{ DC_AXIS_RT2, "Right Trigger 2" },

	{ EMU_BTN_NONE, "System Buttons" },
	{ DC_BTN_START, "Start" },
	{ DC_BTN_RELOAD, "Reload" },

	{ EMU_BTN_NONE, "Emulator" },
	{ EMU_BTN_MENU, "Menu" },
	{ EMU_BTN_ESCAPE, "Exit" },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_BYPASS_KB, "Bypass Emulated Keyboard" },
	{ EMU_BTN_SCREENSHOT, "Save Screenshot" },

	{ EMU_BTN_NONE, nullptr }
};

static const MappingEntry kArcadeControls[] = {
	{ EMU_BTN_NONE, "Directions" },
	{ DC_DPAD_UP, "Up" },
	{ DC_DPAD_DOWN, "Down" },
	{ DC_DPAD_LEFT, "Left" },
	{ DC_DPAD_RIGHT, "Right" },

	{ DC_AXIS_UP, "Thumbstick Up" },
	{ DC_AXIS_DOWN, "Thumbstick Down" },
	{ DC_AXIS_LEFT, "Thumbstick Left" },
	{ DC_AXIS_RIGHT, "Thumbstick Right" },

	{ DC_AXIS2_UP, "R.Thumbstick Up" },
	{ DC_AXIS2_DOWN, "R.Thumbstick Down" },
	{ DC_AXIS2_LEFT, "R.Thumbstick Left" },
	{ DC_AXIS2_RIGHT, "R.Thumbstick Right" },

	{ EMU_BTN_NONE, "Buttons" },
	{ DC_BTN_A, "Button 1" },
	{ DC_BTN_B, "Button 2" },
	{ DC_BTN_C, "Button 3" },
	{ DC_BTN_X, "Button 4" },
	{ DC_BTN_Y, "Button 5" },
	{ DC_BTN_Z, "Button 6" },
	{ DC_DPAD2_LEFT, "Button 7" },
	{ DC_DPAD2_RIGHT, "Button 8" },

	{ EMU_BTN_NONE, "Triggers" },
	{ DC_AXIS_LT, "Left Trigger" },
	{ DC_AXIS_RT, "Right Trigger" },
	{ DC_AXIS_LT2, "Left Trigger 2" },
	{ DC_AXIS_RT2, "Right Trigger 2" },

	{ EMU_BTN_NONE, "System Buttons" },
	{ DC_BTN_START, "Start" },
	{ DC_BTN_RELOAD, "Reload" },
	{ DC_BTN_D, "Coin" },
	{ DC_DPAD2_UP, "Service" },
	{ DC_DPAD2_DOWN, "Test" },
	{ DC_BTN_INSERT_CARD, "Insert Card" },

	{ EMU_BTN_NONE, "Emulator" },
	{ EMU_BTN_MENU, "Menu" },
	{ EMU_BTN_ESCAPE, "Exit" },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_BYPASS_KB, "Bypass Emulated Keyboard" },
	{ EMU_BTN_SCREENSHOT, "Save Screenshot" },

	{ EMU_BTN_NONE, nullptr }
};

static void buttonListener(int port, DreamcastKey key, bool pressed);

static void ResetControllerMappingRuntime()
{
	if (g_currentGamepadForMapping != nullptr)
	{
		g_currentGamepadForMapping->cancel_detect_input();
		g_currentGamepadForMapping->unlistenButtons(buttonListener);
		g_currentGamepadForMapping.reset();
	}
	g_mapped_codes.clear();
	g_buttonState.clear();
}

static void unmapControl(const std::shared_ptr<InputMapping>& mapping, u32 gamepad_port, DreamcastKey key)
{
	mapping->clear_button(gamepad_port, key);
	mapping->clear_axis(gamepad_port, key);
	g_buttonState.erase(key);
}

static DreamcastKey getOppositeDirectionKey(DreamcastKey key)
{
	switch (key)
	{
	case DC_DPAD_UP: return DC_DPAD_DOWN;
	case DC_DPAD_DOWN: return DC_DPAD_UP;
	case DC_DPAD_LEFT: return DC_DPAD_RIGHT;
	case DC_DPAD_RIGHT: return DC_DPAD_LEFT;
	case DC_DPAD2_UP: return DC_DPAD2_DOWN;
	case DC_DPAD2_DOWN: return DC_DPAD2_UP;
	case DC_DPAD2_LEFT: return DC_DPAD2_RIGHT;
	case DC_DPAD2_RIGHT: return DC_DPAD2_LEFT;
	case DC_AXIS_UP: return DC_AXIS_DOWN;
	case DC_AXIS_DOWN: return DC_AXIS_UP;
	case DC_AXIS_LEFT: return DC_AXIS_RIGHT;
	case DC_AXIS_RIGHT: return DC_AXIS_LEFT;
	case DC_AXIS2_UP: return DC_AXIS2_DOWN;
	case DC_AXIS2_DOWN: return DC_AXIS2_UP;
	case DC_AXIS2_LEFT: return DC_AXIS2_RIGHT;
	case DC_AXIS2_RIGHT: return DC_AXIS2_LEFT;
	case DC_AXIS3_UP: return DC_AXIS3_DOWN;
	case DC_AXIS3_DOWN: return DC_AXIS3_UP;
	case DC_AXIS3_LEFT: return DC_AXIS3_RIGHT;
	case DC_AXIS3_RIGHT: return DC_AXIS3_LEFT;
	default: return EMU_BTN_NONE;
	}
}

static void displayLabelOrCode(const char* label, u32 code, const char* suffix = "")
{
	if (label != nullptr)
		ImGui::Text("%s%s", label, suffix);
	else
		ImGui::Text("[%d]%s", code, suffix);
}

static void detect_input_popup(const MappingEntry* mapping)
{
	ImVec2 padding = ScaledVec2(20, 20);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
	ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
	if (ImGui::BeginPopupModal("Map Control", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text("Waiting for control '%s'...", mapping->name);
		u64 now = getTimeMs();

		if (now >= g_map_start_time)
		{
			bool still_detecting = g_currentGamepadForMapping != nullptr && g_currentGamepadForMapping->is_input_detecting();
			int remaining = still_detecting ? static_cast<int>(5 - (now - g_map_start_time) / 1000) : 0;
			if (remaining < 0)
				remaining = 5;

			if (still_detecting)
				ImGui::Text("Time out in %d s", remaining);

			if (!g_mapped_codes.empty())
			{
				ImGui::Text("Current inputs: ");
				ImGui::SameLine();
				bool first = true;
				for (const InputMapping::InputDef& inputDef : g_mapped_codes)
				{
					if (!first)
					{
						ImGui::SameLine();
						ImGui::Text("&");
						ImGui::SameLine();
					}

					const char* name = nullptr;
					if (inputDef.is_button())
						name = g_currentGamepadForMapping->get_button_name(inputDef.code);
					else
						name = g_currentGamepadForMapping->get_axis_name(inputDef.code);

					displayLabelOrCode(name, inputDef.code);
					first = false;
				}

				if (ImGui::Button("Confirm"))
					remaining = 0;
			}

			if (remaining <= 0)
			{
				std::shared_ptr<InputMapping> input_mapping = g_currentGamepadForMapping->get_input_mapping();
				if (input_mapping != nullptr && !g_mapped_codes.empty())
				{
					unmapControl(input_mapping, g_gamepad_port_for_mapping, mapping->key);
					if (g_mapped_codes.size() == 1 && g_mapped_codes.front().is_axis())
					{
						const InputMapping::InputDef& axisInputDef = g_mapped_codes.front();
						const bool positive = (axisInputDef.type == InputMapping::InputDef::InputType::AXIS_POS);
						input_mapping->set_axis(g_gamepad_port_for_mapping, mapping->key, axisInputDef.code, positive);
						const DreamcastKey opposite = getOppositeDirectionKey(mapping->key);
						if (opposite != EMU_BTN_NONE
							&& input_mapping->get_axis_id(g_gamepad_port_for_mapping, axisInputDef.code, !positive) == EMU_BTN_NONE
							&& input_mapping->get_axis_code(g_gamepad_port_for_mapping, opposite).first == static_cast<u32>(-1)
							&& input_mapping->get_button_code(g_gamepad_port_for_mapping, opposite) == static_cast<u32>(-1))
						{
							input_mapping->set_axis(g_gamepad_port_for_mapping, opposite, axisInputDef.code, !positive);
						}
					}
					else
					{
						input_mapping->set_button(g_gamepad_port_for_mapping, mapping->key, InputMapping::ButtonCombo{g_mapped_codes, true});
					}
				}

				if (g_currentGamepadForMapping != nullptr)
					g_currentGamepadForMapping->cancel_detect_input();
				g_mapped_codes.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
}

static void displayMappedControl(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey key)
{
	std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
	if (input_mapping == nullptr)
		return;

	InputMapping::ButtonCombo combo = input_mapping->get_button_combo(g_gamepad_port_for_mapping, key);
	if (combo.inputs.empty())
	{
		const std::pair<u32, bool> pair = input_mapping->get_axis_code(g_gamepad_port_for_mapping, key);
		const InputMapping::InputDef inputDef = InputMapping::InputDef::from_axis(pair.first, pair.second);
		if (inputDef.is_valid())
			displayLabelOrCode(gamepad->get_axis_name(inputDef.code), inputDef.code, inputDef.get_suffix());
		return;
	}

	bool first = true;
	for (const InputMapping::InputDef& inputDef : combo.inputs)
	{
		if (!first)
		{
			ImGui::SameLine();
			ImGui::Text("&");
			ImGui::SameLine();
		}

		const char* name = nullptr;
		if (inputDef.is_button())
			name = gamepad->get_button_name(inputDef.code);
		else if (inputDef.is_axis())
			name = gamepad->get_axis_name(inputDef.code);

		displayLabelOrCode(name, inputDef.code, inputDef.get_suffix());
		first = false;
	}

	if (combo.inputs.size() > 1)
	{
		if (ImGui::Checkbox("Sequential", &(combo.sequential)))
			input_mapping->set_button(g_gamepad_port_for_mapping, key, combo);
		ImGui::SameLine();
		ShowHelpMarker(
			"When checked, this combo will only activate when all keys are pressed in the given sequence.\n"
			"When not checked, the combo will activate when all keys are pressed in any order.");
	}
}

static float getAxisValue(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey axis)
{
	int port = gamepad->maple_port();
	if (port == -1)
		return 0.f;
	if (port == MAPLE_PORTS)
		port = static_cast<int>(g_gamepad_port_for_mapping);

	float v;
	switch (axis)
	{
	case DC_AXIS_UP: v = -joyy[port] / 32768.f; break;
	case DC_AXIS_DOWN: v = joyy[port] / 32767.f; break;
	case DC_AXIS_LEFT: v = -joyx[port] / 32768.f; break;
	case DC_AXIS_RIGHT: v = joyx[port] / 32767.f; break;
	case DC_AXIS2_UP: v = -joyry[port] / 32768.f; break;
	case DC_AXIS2_DOWN: v = joyry[port] / 32767.f; break;
	case DC_AXIS2_LEFT: v = -joyrx[port] / 32768.f; break;
	case DC_AXIS2_RIGHT: v = joyrx[port] / 32767.f; break;
	case DC_AXIS3_UP: v = -joy3y[port] / 32768.f; break;
	case DC_AXIS3_DOWN: v = joy3y[port] / 32767.f; break;
	case DC_AXIS3_LEFT: v = -joy3x[port] / 32768.f; break;
	case DC_AXIS3_RIGHT: v = joy3x[port] / 32767.f; break;
	case DC_AXIS_LT: v = lt[port] / 65535.f; break;
	case DC_AXIS_RT: v = rt[port] / 65535.f; break;
	case DC_AXIS_LT2: v = lt2[port] / 65535.f; break;
	case DC_AXIS_RT2: v = rt2[port] / 65535.f; break;
	default: v = 0.f; break;
	}
	return std::clamp(v, 0.f, 1.f);
}

static void buttonListener(int port, DreamcastKey key, bool pressed)
{
	if (g_currentGamepadForMapping == nullptr || port == -1)
		return;
	if (g_currentGamepadForMapping->maple_port() == MAPLE_PORTS && port != static_cast<int>(g_gamepad_port_for_mapping))
		return;

	if (pressed)
		g_buttonState.insert(key);
	else
		g_buttonState.erase(key);
}

static bool getButtonState(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey btn)
{
	(void)gamepad;
	return g_buttonState.count(btn) != 0;
}

static void controller_mapping_popup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	fullScreenWindow(true);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	if (!ImGui::BeginPopupModal("Controller Mapping", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		return;

	const ImGuiStyle& style = ImGui::GetStyle();
	const float winWidth = ImGui::GetIO().DisplaySize.x - insetLeft - insetRight
		- (style.WindowBorderSize + style.WindowPadding.x) * 2;
	const float col_width = (winWidth - style.GrabMinSize - style.ItemSpacing.x
		- (ImGui::CalcTextSize("Map").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)
		- (ImGui::CalcTextSize("Unmap").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)) / 3;

	static int map_system = DC_PLATFORM_DREAMCAST;
	static int item_current_map_idx = 0;
	static int last_item_current_map_idx = 2;
	if (g_currentGamepadForMapping == nullptr)
	{
		g_currentGamepadForMapping = gamepad;
		g_currentGamepadForMapping->listenButtons(buttonListener);
	}

	std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
	if (input_mapping == nullptr || ImGui::Button("Done", ScaledVec2(100, 30)))
	{
		ImGui::CloseCurrentPopup();
		gamepad->save_mapping(map_system);
		last_item_current_map_idx = 2;
		ImGui::EndPopup();
		ResetControllerMappingRuntime();
		return;
	}
	ImGui::SetItemDefaultFocus();

	float portWidth = 0.0f;
	if (gamepad->maple_port() == MAPLE_PORTS)
	{
		ImGui::SameLine();
		ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding,
			ImVec2(ImGui::GetStyle().FramePadding.x, (uiScaled(30) - ImGui::GetFontSize()) / 2));
		portWidth = ImGui::CalcTextSize("AA").x + ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::GetFontSize();
		ImGui::SetNextItemWidth(portWidth);
		if (ImGui::BeginCombo("Port", kMaplePorts[g_gamepad_port_for_mapping + 1]))
		{
			for (u32 j = 0; j < MAPLE_PORTS; j++)
			{
				bool is_selected = g_gamepad_port_for_mapping == j;
				if (ImGui::Selectable(kMaplePorts[j + 1], &is_selected))
					g_gamepad_port_for_mapping = j;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		portWidth += ImGui::CalcTextSize("Port").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x;
	}

	const float comboWidth = ImGui::CalcTextSize("Dreamcast Controls").x + ImGui::GetStyle().ItemSpacing.x
		+ ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 4;
	float gameConfigWidth = 0.0f;
	if (!settings.content.gameId.empty())
	{
		gameConfigWidth = ImGui::CalcTextSize(gamepad->isPerGameMapping() ? "Delete Game Config" : "Make Game Config").x
			+ ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2;
	}

	ImGui::SameLine(0, ImGui::GetContentRegionAvail().x - comboWidth - gameConfigWidth
		- ImGui::GetStyle().ItemSpacing.x - uiScaled(100) * 2 - portWidth);
	ImGui::AlignTextToFramePadding();

	if (!settings.content.gameId.empty())
	{
		if (gamepad->isPerGameMapping())
		{
			if (ImGui::Button("Delete Game Config", ScaledVec2(0, 30)))
			{
				gamepad->setPerGameMapping(false);
				if (!gamepad->find_mapping(map_system))
				{
					if (map_system == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
						gamepad->resetMappingToDefault(g_arcade_button_mode, true);
				}
			}
		}
		else
		{
			if (ImGui::Button("Make Game Config", ScaledVec2(0, 30)))
				gamepad->setPerGameMapping(true);
		}
		ImGui::SameLine();
	}

	if (ImGui::Button("Reset...", ScaledVec2(100, 30)))
		ImGui::OpenPopup("Confirm Reset");

	{
		ImguiStyleVar windowPadding(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
		if (ImGui::BeginPopupModal("Confirm Reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::Text("Are you sure you want to reset the mappings to default?");
			static bool hitbox = false;
			if (g_arcade_button_mode)
			{
				ImGui::Text("Controller Type:");
				if (ImGui::RadioButton("Gamepad", !hitbox))
					hitbox = false;
				ImGui::SameLine();
				if (ImGui::RadioButton("Arcade / Hit Box", hitbox))
					hitbox = true;
			}
			ImGui::NewLine();
			{
				ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
				ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
				if (ImGui::Button("Yes"))
				{
					gamepad->resetMappingToDefault(g_arcade_button_mode, !hitbox);
					gamepad->save_mapping(map_system);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("No"))
					ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	ImGui::SameLine();
	const char* items[] = { "Dreamcast Controls", "Arcade Controls" };
	if (last_item_current_map_idx == 2 && game_started)
		item_current_map_idx = settings.platform.isArcade() ? 1 : 0;

	ImGui::SetNextItemWidth(comboWidth);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(ImGui::GetStyle().FramePadding.x, (uiScaled(30) - ImGui::GetFontSize()) / 2));
	ImGui::Combo("##arcadeMode", &item_current_map_idx, items, IM_ARRAYSIZE(items));
	ImGui::PopStyleVar();
	if (last_item_current_map_idx != 2 && item_current_map_idx != last_item_current_map_idx)
		gamepad->save_mapping(map_system);

	const MappingEntry* systemMapping = kDreamcastControls;
	if (item_current_map_idx == 0)
	{
		g_arcade_button_mode = false;
		map_system = DC_PLATFORM_DREAMCAST;
		systemMapping = kDreamcastControls;
	}
	else
	{
		g_arcade_button_mode = true;
		map_system = DC_PLATFORM_NAOMI;
		systemMapping = kArcadeControls;
	}

	if (item_current_map_idx != last_item_current_map_idx)
	{
		if (!gamepad->find_mapping(map_system))
		{
			if (map_system == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
				gamepad->resetMappingToDefault(g_arcade_button_mode, true);
		}
		input_mapping = gamepad->get_input_mapping();
		last_item_current_map_idx = item_current_map_idx;
	}

	char key_id[32];
	ImGui::BeginChild(ImGui::GetID("buttons"), ImVec2(0, 0), ImGuiChildFlags_FrameStyle,
		ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NavFlattened);
	for (; systemMapping->name != nullptr; systemMapping++)
	{
		if (systemMapping->key == EMU_BTN_NONE)
		{
			ImGui::Columns(1, nullptr, false);
			header(systemMapping->name);
			ImGui::Columns(4, "bindings", false);
			ImGui::SetColumnWidth(0, col_width);
			ImGui::SetColumnWidth(1, col_width);
			ImGui::SetColumnWidth(2, col_width);
			continue;
		}

		snprintf(key_id, sizeof(key_id), "key_id%d", systemMapping->key);
		ImguiID mappingId(key_id);
		(void)mappingId;

		const char* game_btn_name = nullptr;
		if (g_arcade_button_mode)
		{
			game_btn_name = GetCurrentGameButtonName(systemMapping->key);
			if (game_btn_name == nullptr)
				game_btn_name = GetCurrentGameAxisName(systemMapping->key);
		}
		if (game_btn_name != nullptr && game_btn_name[0] != '\0')
			ImGui::Text("%s - %s", systemMapping->name, game_btn_name);
		else
			ImGui::Text("%s", systemMapping->name);

		ImGui::NextColumn();
		displayMappedControl(gamepad, systemMapping->key);

		ImGui::NextColumn();
		if (dynamic_cast<KeyboardDevice*>(gamepad.get()) == nullptr && dynamic_cast<Mouse*>(gamepad.get()) == nullptr)
		{
			if ((systemMapping->key & DC_BTN_GROUP_MASK) == DC_AXIS_STICKS
				|| (systemMapping->key & DC_BTN_GROUP_MASK) == DC_AXIS_TRIGGERS)
			{
				ImguiStyleColor plotColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
				const float v = getAxisValue(gamepad, systemMapping->key);
				char s[32];
				snprintf(s, sizeof(s), "%.0f%%", v * 100.f);
				ImGui::ProgressBar(v, ImVec2(-1, 0), s);
			}
			else if (getButtonState(gamepad, systemMapping->key))
			{
				ImGui::Text(ICON_FA_CIRCLE_DOT);
			}
		}

		ImGui::NextColumn();
		if (ImGui::Button("Map"))
		{
			g_map_start_time = getTimeMs() + 300;
			ImGui::OpenPopup("Map Control");
			g_mapped_codes.clear();
			g_buttonState.erase(systemMapping->key);

			const bool detectCombo = (systemMapping->key & DC_BTN_GROUP_MASK) == EMU_BUTTONS;
			gamepad->detectInput(detectCombo, [](u32 code, bool analog, bool positive) {
				if (analog)
					g_mapped_codes.insert_back(InputMapping::InputDef::from_axis(code, positive));
				else
					g_mapped_codes.insert_back(InputMapping::InputDef::from_button(code));
			});
		}
		detect_input_popup(systemMapping);
		ImGui::SameLine();
		if (ImGui::Button("Unmap"))
		{
			input_mapping = gamepad->get_input_mapping();
			unmapControl(input_mapping, g_gamepad_port_for_mapping, systemMapping->key);
		}
		ImGui::NextColumn();
	}
	ImGui::Columns(1, nullptr, false);
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
	error_popup();
	ImGui::EndPopup();
}

static void RenderGeneralRightValue(const std::string& value, float valueWidthPx, float extraRightOffsetPx = 0.0f, bool disabled = false)
{
	const float valueWidth = uiScaled(valueWidthPx);
	const float extraRight = uiScaled(extraRightOffsetPx);
	const float verticalOffset = (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f;
	std::string clipped = middleEllipsis(value, valueWidth);
	ImFont* valueFont = settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;

	ImGui::SameLine(RightColumnX(valueWidth + extraRight));
	const ImVec2 valuePos = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(valuePos.x, valuePos.y + verticalOffset));
	ImGui::PushFont(valueFont);
	if (disabled)
		ImGui::TextDisabled("%s", clipped.c_str());
	else
		ImGui::TextUnformatted(clipped.c_str());
	ImGui::PopFont();
}

static void RenderGeneralInfoRow(const char* id, const char* icon, const char* label, const char* description, const std::string& value, bool disabledValue = false)
{
	ImGui::PushID(id);
	BeginTwoLineSettingRow("##row", description, true);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	SettingIcon(icon, ImVec2(uiScaled(20), uiScaled(20)));
	ImGui::SameLine(0, uiScaled(8));
	ImGui::PushFont(largeFont);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();
	RenderGeneralRightValue(value, 280.0f, 0.0f, disabledValue);
	RenderTwoLineSettingDescription(line1Start, description);

	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
}

static void manageSinglePath(const char* label, config::Option<std::string, false>& pathOption, const char* helpText)
{
	ImGui::PushID(label);
	const bool rowActivated = BeginTwoLineSettingRow("##row", helpText);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	SettingIcon(ICON_FA_FOLDER_OPEN, ImVec2(uiScaled(20), uiScaled(20)));
	ImGui::SameLine(0, uiScaled(8));
	ImGui::PushFont(largeFont);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();

	bool openPopup = rowActivated;
	const bool hasPath = !pathOption.get().empty();
	if (hasPath)
	{
		const float deleteBtnWidthPx = 26.0f;
		const float deleteBtnHeightPx = 22.0f;
		const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
		RenderGeneralRightValue(pathOption.get(), 280.0f, extraOffsetPx);

		const float buttonWidth = uiScaled(deleteBtnWidthPx);
		const float buttonHeight = uiScaled(deleteBtnHeightPx);
		const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(buttonWidth));
		ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
		if (ImGui::Button(ICON_FA_TRASH_CAN "##ClearPath", ImVec2(buttonWidth, buttonHeight)))
		{
			pathOption.get().clear();
			openPopup = false;
		}
	}
	else
	{
		RenderGeneralRightValue("Set Path", 280.0f, 0.0f, true);
	}

	RenderTwoLineSettingDescription(line1Start, helpText);
	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));

	const std::string popupName = std::string("Select ") + label;
	select_file_popup(popupName.c_str(), [&pathOption](bool cancelled, const std::string& selection) {
		if (!cancelled)
			pathOption.get() = selection;
		return true;
	});
	if (openPopup)
		ImGui::OpenPopup(popupName.c_str());
}

static void managePathList(const char* label, std::vector<std::string>& paths, const char* helpText)
{
	ImguiID _(label);
	int to_delete = -1;
	bool openPopup = false;
	std::string singularLabel = label != nullptr ? label : "Folder";
	if (!singularLabel.empty() && singularLabel.back() == 's')
		singularLabel.pop_back();
	const std::string addRowLabel = "Add " + singularLabel;
	const std::string addRowDescription = "Add another " + singularLabel + " path";

	for (u32 i = 0; i < paths.size(); i++)
	{
		ImGui::PushID(static_cast<int>(i));
		const bool rowActivated = BeginTwoLineSettingRow("##row", helpText);
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();
		(void)rowActivated;

		SettingIcon(ICON_FA_FOLDER_OPEN, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		if (i == 0)
			ImGui::TextUnformatted(label);
		else
		{
			std::string rowLabel = singularLabel + " " + std::to_string(i + 1);
			ImGui::TextUnformatted(rowLabel.c_str());
		}
		ImGui::PopFont();

		const float deleteBtnWidthPx = 26.0f;
		const float deleteBtnHeightPx = 22.0f;
		const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
		RenderGeneralRightValue(paths[i], 280.0f, extraOffsetPx);

		const float buttonWidth = uiScaled(deleteBtnWidthPx);
		const float buttonHeight = uiScaled(deleteBtnHeightPx);
		const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(buttonWidth));
		ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
		if (ImGui::Button(ICON_FA_TRASH_CAN "##DeletePath", ImVec2(buttonWidth, buttonHeight)))
			to_delete = static_cast<int>(i);

		const std::string additionalDesc = "Additional " + singularLabel + " path";
		const char* desc = (i == 0) ? helpText : additionalDesc.c_str();
		RenderTwoLineSettingDescription(line1Start, desc);
		ImGui::PopID();
		ImGui::Spacing();
		if (g_twoLineRowExtraGapPx > 0.0f)
			ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	}

	ImGui::PushID("AddPathRow");
	{
		const bool addRowActivated = BeginTwoLineSettingRow("##row", helpText);
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();

		SettingIcon(ICON_FA_PLUS, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(addRowLabel.c_str());
		ImGui::PopFont();
		RenderGeneralRightValue("Select Path", 280.0f, 0.0f, true);
		RenderTwoLineSettingDescription(line1Start, addRowDescription.c_str());
		openPopup = addRowActivated;
	}
	ImGui::PopID();
	ImGui::Spacing();
	if (g_twoLineRowExtraGapPx > 0.0f)
		ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));

	if (to_delete >= 0)
	{
		paths.erase(paths.begin() + to_delete);
		SaveSettings();
	}

	// Handle file selection popup
	std::string popupTitle = std::string("Select ") + label;
	auto* pathsPtr = &paths;
	select_file_popup(popupTitle.c_str(), [pathsPtr](bool cancelled, const std::string& selection) {
		if (!cancelled)
		{
			pathsPtr->push_back(selection);
			SaveSettings();
		}
		return true;
	});
#ifdef __ANDROID__
	if (openPopup)
	{
		bool supported = select_storage_popup(true, false, popupTitle, [pathsPtr](bool cancelled, const std::string& selection) {
			if (!cancelled)
			{
				pathsPtr->push_back(selection);
				SaveSettings();
			}
			return true;
		});
		if (!supported)
			ImGui::OpenPopup(popupTitle.c_str());
	}
#else
	if (openPopup)
		ImGui::OpenPopup(popupTitle.c_str());
#endif
}


// Global state instance
SettingsUIState g_state;

// Reset state when entering settings screen
void resetState()
{
	g_state.currentTab = SettingsTab::General;
}

void openTab(SettingsTab tab)
{
	g_state.currentTab = tab;
	g_scrollToBoxArtSection = false;
}

void focusBoxArtSection()
{
	openTab(SettingsTab::General);
	g_scrollToBoxArtSection = true;
}

// Get display name for each tab
const char* getTabName(SettingsTab tab)
{
	switch (tab)
	{
	case SettingsTab::General:
		return "General";
	case SettingsTab::Video:
		return "Video";
	case SettingsTab::Audio:
		return "Audio";
	case SettingsTab::Controls:
		return "Controls";
	case SettingsTab::Network:
		return "Network";
	case SettingsTab::Advanced:
		return "Advanced";
	case SettingsTab::About:
		return "About";
	default:
		return "Unknown";
	}
}

// Render left navigation rail with tab buttons (225px wide)
// This creates the vertical strip on the left side with navigation items
static void renderNavigationRail()
{
	// Left navigation rail: 225px wide child window
	// Using ImGui::BeginChild() to create a scrollable region for navigation
	ImGui::BeginChild("NavigationRail", ImVec2(225, 0), true);

	// Iterate through all tabs and create navigation buttons
	// Using ImGui::Selectable() which provides proper hover and selection states
	// Colors come from ImGuiCol_Header in the current theme
	for (int i = 0; i < (int)SettingsTab::Count; i++)
	{
		SettingsTab tab = (SettingsTab)i;
		const char* tabName = getTabName(tab);

		// Highlight the currently active tab
		// When selected, ImGui uses ImGuiCol_Header (from current theme)
		bool isSelected = (g_state.currentTab == tab);

		// Create selectable item that fills the navigation rail width
		// Clicking changes the current tab
		if (ImGui::Selectable(tabName, isSelected, ImGuiSelectableFlags_None, ImVec2(225, 0)))
		{
			g_state.currentTab = tab;
		}
	}

	ImGui::EndChild();
}

// Render right content area (remaining width for settings content)
// This displays the actual settings for the currently selected tab
static void renderContentArea()
{
	// Right content area: fills remaining space
	// ImGui::SameLine() positions this after the navigation rail
	// No width specified means it takes all remaining space
	ImGui::BeginChild("ContentArea", ImVec2(0, 0), true,
	                  ImGuiWindowFlags_DragScrolling);

	// Call the appropriate tab renderer based on current selection
	switch (g_state.currentTab)
	{
	case SettingsTab::General:
		renderGeneralTab();
		break;
	case SettingsTab::Video:
		renderVideoTab();
		break;
	case SettingsTab::Audio:
		renderAudioTab();
		break;
	case SettingsTab::Controls:
		renderControlsTab();
		break;
	case SettingsTab::Network:
		renderNetworkTab();
		break;
	case SettingsTab::Advanced:
		renderAdvancedTab();
		break;
	case SettingsTab::About:
		renderAboutTab();
		break;
	default:
		break;
	}

	ImGui::EndChild();
}

// Render General tab with all settings from settings_general.cpp
void renderGeneralTab()
{
	using namespace SettingsUI;
	ScopedTwoLineRowStyle generalRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("General Configuration");
	ImGui::Separator();

	// ========================================
	// Language & Region Section
	// ========================================
	SectionHeaderWithIcon(ICON_FA_GLOBE, "Language & Region");

	{
		static const char* languages[] = { "Japanese", "English", "German", "French", "Spanish", "Italian", "Default" };
		SettingsUI::PopupConfig languageCfg {};
		languageCfg.type = SettingsUI::PopupType::Options;
		languageCfg.options.label = "Language";
		languageCfg.options.icon = ICON_FA_LANGUAGE;
		languageCfg.options.popupID = "LanguagePopup";
		languageCfg.options.options = languages;
		languageCfg.options.optionCount = IM_ARRAYSIZE(languages);
		languageCfg.options.currentValue = &config::Language.get();
		languageCfg.options.disabled = settings.platform.isArcade();
		languageCfg.options.disabledPrefix = "(auto) ";
		languageCfg.options.valueWidth = 220.0f;
		RenderGeneralPopupSettingRow(
			"LanguageSetting",
			"The language as configured in the Dreamcast BIOS",
			languageCfg);
	}

	{
		static const char* broadcast[] = { "NTSC", "PAL", "PAL/M", "PAL/N", "Default" };
		SettingsUI::PopupConfig broadcastCfg {};
		broadcastCfg.type = SettingsUI::PopupType::Options;
		broadcastCfg.options.label = "Broadcast";
		broadcastCfg.options.icon = ICON_FA_TV;
		broadcastCfg.options.popupID = "BroadcastPopup";
		broadcastCfg.options.options = broadcast;
		broadcastCfg.options.optionCount = IM_ARRAYSIZE(broadcast);
		broadcastCfg.options.currentValue = &config::Broadcast.get();
		broadcastCfg.options.disabled = settings.platform.isArcade();
		broadcastCfg.options.disabledPrefix = "(auto) ";
		broadcastCfg.options.valueWidth = 220.0f;
		RenderGeneralPopupSettingRow(
			"BroadcastSetting",
			"TV broadcasting standard for non-VGA modes",
			broadcastCfg);
	}

	{
		static const char* consoleRegion[] = { "Japan", "USA", "Europe", "Default" };
		static const char* arcadeRegion[] = { "Japan", "USA", "Export", "Korea" };
		const char* const* region = settings.platform.isArcade() ? arcadeRegion : consoleRegion;

		SettingsUI::PopupConfig regionCfg {};
		regionCfg.type = SettingsUI::PopupType::Options;
		regionCfg.options.label = "Region";
		regionCfg.options.icon = ICON_FA_EARTH_AMERICAS;
		regionCfg.options.popupID = "RegionPopup";
		regionCfg.options.options = region;
		regionCfg.options.optionCount = 4;
		regionCfg.options.currentValue = &config::Region.get();
		regionCfg.options.valueWidth = 220.0f;
		RenderGeneralPopupSettingRow("RegionSetting", "BIOS region", regionCfg);
	}

	{
		static const char* cable[] = { "VGA", "RGB Component", "TV Composite" };
		const bool disabled = config::Cable.isReadOnly() || settings.platform.isArcade();

		SettingsUI::PopupConfig cableCfg {};
		cableCfg.type = SettingsUI::PopupType::Options;
		cableCfg.options.label = "Cable";
		cableCfg.options.icon = ICON_FA_PLUG;
		cableCfg.options.popupID = "CablePopup";
		cableCfg.options.options = cable;
		cableCfg.options.optionCount = IM_ARRAYSIZE(cable);
		cableCfg.options.currentValue = &config::Cable.get();
		cableCfg.options.disabled = disabled;
		cableCfg.options.disabledPrefix = "(auto) ";
		cableCfg.options.valueWidth = 220.0f;
		cableCfg.options.valueToString = [](int storageValue) -> const char* {
			switch (storageValue)
			{
			case 0:
			case 1:
				return "VGA";
			case 2:
				return "RGB Component";
			case 3:
				return "TV Composite";
			default:
				return "VGA";
			}
		};
		cableCfg.options.storageIndexMap = [](int displayIndex) -> int {
			return displayIndex == 0 ? 0 : displayIndex + 1;
		};
		RenderGeneralPopupSettingRow("CableSetting", "Video connection type", cableCfg, nullptr, disabled);
	}

	// ========================================
	// Content Paths Section
	// ========================================
#if !defined(TARGET_IPHONE)
	SectionHeaderWithIcon(ICON_FA_FOLDER, "Content Paths");

	int to_delete = -1;
	bool addContentRequested = false;
	for (u32 i = 0; i < config::ContentPath.get().size(); i++)
	{
		ImGui::PushID(static_cast<int>(i));
		BeginTwoLineSettingRow("##row", "The folders where your games are stored");
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();

		SettingIcon(ICON_FA_FOLDER_OPEN, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		if (i == 0)
			ImGui::TextUnformatted("Content Folders");
		else
		{
			std::string rowLabel = std::string("Content Folder ") + std::to_string(i + 1);
			ImGui::TextUnformatted(rowLabel.c_str());
		}
		ImGui::PopFont();

		const float deleteBtnWidthPx = 26.0f;
		const float deleteBtnHeightPx = 22.0f;
		const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
		RenderGeneralRightValue(config::ContentPath.get()[i], 280.0f, extraOffsetPx);

		const float buttonWidth = uiScaled(deleteBtnWidthPx);
		const float buttonHeight = uiScaled(deleteBtnHeightPx);
		const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;
		ImGui::SameLine(RightColumnX(buttonWidth));
		ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
		if (ImGui::Button(ICON_FA_TRASH_CAN "##DeleteContentPath", ImVec2(buttonWidth, buttonHeight)))
			to_delete = static_cast<int>(i);

		const char* desc = (i == 0) ? "The folders where your games are stored" : "Additional content folder";
		RenderTwoLineSettingDescription(line1Start, desc);

		ImGui::PopID();
		ImGui::Spacing();
		if (g_twoLineRowExtraGapPx > 0.0f)
			ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	}

	{
		ImGui::PushID("ContentAddRow");
		const bool addRowActivated = BeginTwoLineSettingRow("##row", "Add another content folder");
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();
		SettingIcon(ICON_FA_PLUS, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted("Add Content Folder");
		ImGui::PopFont();

		const float refreshBtnWidthPx = 26.0f;
		const float refreshBtnHeightPx = 22.0f;
		const float extraOffsetPx = refreshBtnWidthPx + 8.0f;
		RenderGeneralRightValue("Select Path", 280.0f, extraOffsetPx, true);

		const float refreshButtonWidth = uiScaled(refreshBtnWidthPx);
		const float refreshButtonHeight = uiScaled(refreshBtnHeightPx);
		const float refreshVerticalOffset = (TwoLineSettingContentHeight() - refreshButtonHeight) * 0.5f;
		ImGui::SameLine(RightColumnX(refreshButtonWidth));
		ImVec2 refreshButtonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(refreshButtonPos.x, refreshButtonPos.y + refreshVerticalOffset));
		const bool refreshPressed = ImGui::Button(ICON_FA_ARROWS_ROTATE "##RescanContentBtn", ImVec2(refreshButtonWidth, refreshButtonHeight));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Refresh");

		RenderTwoLineSettingDescription(line1Start, "Add another content folder");
		addContentRequested = addRowActivated;
		if (refreshPressed)
			scanner.refresh();
		ImGui::PopID();
		ImGui::Spacing();
		if (g_twoLineRowExtraGapPx > 0.0f)
			ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	}

	// Keep this call every frame so the popup can render while open.
	addContentPath(addContentRequested);

	if (to_delete >= 0)
	{
		scanner.stop();
		config::ContentPath.get().erase(config::ContentPath.get().begin() + to_delete);
		scanner.refresh();
	}

#if defined(__linux__) && !defined(__ANDROID__)
	SectionHeaderWithIcon(ICON_FA_FLOPPY_DISK, "Data Folder");
	RenderGeneralInfoRow(
		"DataFolderPathRow",
		ICON_FA_FLOPPY_DISK,
		"Data Folder",
		"The folder containing BIOS files, as well as saved VMUs and states",
		get_writable_data_path(""));
#else
	SectionHeaderWithIcon(ICON_FA_HOUSE, "Home Directory");
	RenderGeneralInfoRow(
		"HomeFolderPathRow",
		ICON_FA_HOUSE,
		"Home Directory",
		"The folder where Flycast saves configuration files and VMUs. BIOS files should be in a subfolder named \"data\"",
		get_writable_config_path(""));

	ImguiStyleVar _homePadding(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
#ifdef __ANDROID__
	{
		DisabledScope _(!config::UseSafFilePicker);
		if (ImGui::Button("Import"))
			hostfs::importHomeDirectory();
		ImGui::SameLine();
		if (ImGui::Button("Export"))
			hostfs::exportHomeDirectory();
	}
#endif
#ifdef TARGET_MAC
	if (ImGui::Button("Reveal in Finder"))
	{
		char temp[512];
		snprintf(temp, sizeof(temp), "open \"%s\"", get_writable_config_path("").c_str());
		system(temp);
	}
#endif
#endif // !linux
	ImGui::Spacing();
#else // TARGET_IPHONE
	{
		ImGui::PushID("IphoneRescanContentRow");
		const bool rescanRowActivated = BeginTwoLineSettingRow("##row", "Rescan all content folders");
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();
		SettingIcon(ICON_FA_ARROWS_ROTATE, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted("Rescan Content");
		ImGui::PopFont();

		const float refreshBtnWidthPx = 26.0f;
		const float refreshBtnHeightPx = 22.0f;
		const float refreshButtonWidth = uiScaled(refreshBtnWidthPx);
		const float refreshButtonHeight = uiScaled(refreshBtnHeightPx);
		const float refreshVerticalOffset = (TwoLineSettingContentHeight() - refreshButtonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(refreshButtonWidth));
		ImVec2 refreshButtonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(refreshButtonPos.x, refreshButtonPos.y + refreshVerticalOffset));
		const bool refreshPressed = ImGui::Button(ICON_FA_ARROWS_ROTATE "##IphoneRescanContentBtn", ImVec2(refreshButtonWidth, refreshButtonHeight));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Refresh");

		RenderTwoLineSettingDescription(line1Start, "Rescan all content folders");
		if (rescanRowActivated || refreshPressed)
			scanner.refresh();
		ImGui::PopID();
		ImGui::Spacing();
		if (g_twoLineRowExtraGapPx > 0.0f)
			ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
	}
#endif
	ImGui::Spacing();

	// ========================================
	// UI Settings Section
	// ========================================
	SectionHeaderWithIcon(ICON_FA_GEAR, "UI Settings");

	{
		static const char* themes[] = { "Dark", "Light", "Dreamcast", "High Contrast", "Nintendo", "Aqua Chill" };
		SettingsUI::PopupConfig themeCfg {};
		themeCfg.type = SettingsUI::PopupType::Options;
		themeCfg.options.label = "UI Theme";
		themeCfg.options.icon = ICON_FA_PALETTE;
		themeCfg.options.popupID = "UIThemePopup";
			themeCfg.options.options = themes;
			themeCfg.options.optionCount = IM_ARRAYSIZE(themes);
			themeCfg.options.currentValue = &config::UITheme.get();
			themeCfg.options.valueWidth = 220.0f;
			themeCfg.options.onChange = [](int) {
				applyCurrentTheme();
				return true;
		};
		RenderGeneralPopupSettingRow("UIThemeSetting", "Select the UI color theme.", themeCfg);
	}

	{
		static bool showApplyButtonForUIScaling = false;
		if (uiUserScaleUpdated && !showApplyButtonForUIScaling)
			showApplyButtonForUIScaling = true;

		SettingsUI::PopupConfig uiScalingCfg {};
		uiScalingCfg.type = SettingsUI::PopupType::Slider;
		uiScalingCfg.slider.label = "UI Scaling";
		uiScalingCfg.slider.icon = ICON_FA_RULER_HORIZONTAL;
		uiScalingCfg.slider.popupID = "UIScalingPopup";
		uiScalingCfg.slider.description = "Adjust UI element sizes";
			uiScalingCfg.slider.currentValue = &config::UIScaling.get();
			uiScalingCfg.slider.minValue = 50;
			uiScalingCfg.slider.maxValue = 200;
			uiScalingCfg.slider.format = "%d%%";
			uiScalingCfg.slider.valueWidth = 220.0f;
			uiScalingCfg.slider.showApplyFlag = &showApplyButtonForUIScaling;
		uiScalingCfg.slider.onApply = [&]() {
			mainui_reinit();
			uiUserScaleUpdated = false;
			showApplyButtonForUIScaling = false;
		};
		RenderGeneralPopupSettingRow(
			"UIScalingSetting",
			"Adjust the size of UI elements. Requires restart.",
			uiScalingCfg);
	}

	RenderGeneralToggleSettingRow(
		"HideLegacyNaomiRoms",
		ICON_FA_LOCK,
		"Hide Legacy Naomi Roms",
		"Hide .bin, .dat and .lst files from the content browser",
		static_cast<bool>(config::HideLegacyNaomiRoms),
		[](bool enabled) {
			config::HideLegacyNaomiRoms = enabled;
			scanner.refresh();
		},
		"Hide .bin, .dat and .lst files from the content browser");

#ifdef __ANDROID__
	RenderGeneralToggleSettingRow(
		"UseSafFilePicker",
		ICON_FA_MOBILE_SCREEN_BUTTON,
		"Use SAF File Picker",
		"Use Android Storage Access Framework for file browsing",
		static_cast<bool>(config::UseSafFilePicker),
		[](bool enabled) { config::UseSafFilePicker = enabled; });
#endif

	// ========================================
	// Box Art Section
	// ========================================
	SectionHeaderWithIcon(ICON_FA_IMAGE, "Box Art");
	if (g_scrollToBoxArtSection)
	{
		ImGui::SetScrollHereY(0.0f);
		g_scrollToBoxArtSection = false;
	}

	{
		static const char* boxartSources[] = { "Original Box Art", "Physical Media", "Custom Boxart" };
		SettingsUI::PopupConfig boxartSourceCfg {};
		boxartSourceCfg.type = SettingsUI::PopupType::Options;
		boxartSourceCfg.options.label = "Box Art Source";
		boxartSourceCfg.options.icon = ICON_FA_IMAGE;
		boxartSourceCfg.options.popupID = "BoxArtSourcePopup";
		boxartSourceCfg.options.options = boxartSources;
		boxartSourceCfg.options.optionCount = IM_ARRAYSIZE(boxartSources);
		boxartSourceCfg.options.currentValue = &config::BoxartSourceMode.get();
		boxartSourceCfg.options.valueWidth = 220.0f;
		boxartSourceCfg.options.onChange = [](int) {
			gui_refresh_custom_boxart(false);
			return true;
		};
		RenderGeneralPopupSettingRow(
			"BoxartSourceSetting",
			"Choose which box art source to display in the game list",
			boxartSourceCfg);
	}

	{
		ImGui::PushID("CustomBoxartFolderRow");
		const bool rowActivated = BeginTwoLineSettingRow("##row", "Folder containing custom box art images (png/jpg). File names should match game names");
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();

		SettingIcon(ICON_FA_FOLDER_OPEN, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted("Custom Boxart Folder");
		ImGui::PopFont();

		bool openPopup = rowActivated;
		bool refreshPressed = false;
		const bool hasPath = !config::BoxartPath.get().empty();
		if (hasPath)
		{
			const float buttonWidthPx = 26.0f;
			const float buttonHeightPx = 22.0f;
			const float buttonSpacingPx = 8.0f;
			const float extraOffsetPx = buttonWidthPx * 2.0f + buttonSpacingPx;
			RenderGeneralRightValue(config::BoxartPath.get(), 280.0f, extraOffsetPx);

			const float buttonWidth = uiScaled(buttonWidthPx);
			const float buttonHeight = uiScaled(buttonHeightPx);
			const float buttonSpacing = uiScaled(buttonSpacingPx);
			const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

			ImGui::SameLine(RightColumnX(buttonWidth * 2.0f + buttonSpacing));
			ImVec2 buttonPos = ImGui::GetCursorPos();
			ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
			refreshPressed = ImGui::Button(ICON_FA_ARROWS_ROTATE "##BoxartRefreshBtn", ImVec2(buttonWidth, buttonHeight));
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Refresh");

			ImGui::SameLine(0, buttonSpacing);
			ImVec2 deletePos = ImGui::GetCursorPos();
			ImGui::SetCursorPos(ImVec2(deletePos.x, buttonPos.y + verticalOffset));
			if (ImGui::Button(ICON_FA_TRASH_CAN "##ClearBoxartPath", ImVec2(buttonWidth, buttonHeight)))
			{
				config::BoxartPath.get().clear();
				openPopup = false;
			}
		}
		else
		{
			RenderGeneralRightValue("Set Path", 280.0f, 0.0f, true);
		}

		RenderTwoLineSettingDescription(line1Start, "Folder containing custom box art images (png/jpg). File names should match game names");
		ImGui::PopID();
		ImGui::Spacing();
		if (g_twoLineRowExtraGapPx > 0.0f)
			ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));

		const std::string popupName = "Select Custom Boxart Folder";
		select_file_popup(popupName.c_str(), [](bool cancelled, const std::string& selection) {
			if (!cancelled)
				config::BoxartPath.get() = selection;
			return true;
		}, false, "");
		if (openPopup)
			ImGui::OpenPopup(popupName.c_str());

		static std::string lastBoxartPath;
		if (lastBoxartPath != config::BoxartPath.get())
		{
			gui_refresh_custom_boxart(true);
			lastBoxartPath = config::BoxartPath.get();
		}
		if (refreshPressed)
			gui_refresh_custom_boxart(true);
	}

	RenderGeneralToggleSettingRow(
		"BoxartDisplayMode",
		ICON_FA_IMAGE,
		"Box Art Game List",
		"Display game cover art in the game list.",
		static_cast<bool>(config::BoxartDisplayMode),
		[](bool enabled) { config::BoxartDisplayMode = enabled; },
		"Display game cover art in the game list");

	const bool physicalOnly = config::BoxartSourceMode.get() == static_cast<int>(BoxartSourceMode::PhysicalOnly);
	RenderGeneralToggleSettingRow(
		"FetchBoxart",
		ICON_FA_DOWNLOAD,
		"Fetch Box Art",
		"Fetch cover images from TheGamesDB.net.",
		static_cast<bool>(config::FetchBoxart),
		[](bool enabled) { config::FetchBoxart = enabled; },
		"Fetch cover images from TheGamesDB.net",
		physicalOnly);

	// ========================================
	// Automatic Save States Section
	// ========================================
	SectionHeaderWithIcon(ICON_FA_FLOPPY_DISK, "Automatic Save States");

	RenderGeneralToggleSettingRow(
		"AutoLoadState",
		ICON_FA_FOLDER_OPEN,
		"Auto-Load on Start",
		"Load the last saved state of the game when starting",
		static_cast<bool>(config::AutoLoadState),
		[](bool enabled) { config::AutoLoadState = enabled; });

	RenderGeneralToggleSettingRow(
		"AutoSaveState",
		ICON_FA_FLOPPY_DISK,
		"Auto-Save on Exit",
		"Save the state of the game when stopping",
		static_cast<bool>(config::AutoSaveState),
		[](bool enabled) { config::AutoSaveState = enabled; });

	RenderGeneralToggleSettingRow(
		"NaomiFreePlay",
		ICON_FA_TROPHY,
		"Naomi Free Play",
		"Configure Naomi games in Free Play mode.",
		static_cast<bool>(config::ForceFreePlay),
		[](bool enabled) { config::ForceFreePlay = enabled; });

#if USE_DISCORD
	RenderGeneralToggleSettingRow(
		"discord_presence",
		ICON_FA_DISCORD,
		"Discord Presence",
		"Show which game you are playing on Discord",
		static_cast<bool>(config::DiscordPresence),
		[](bool enabled) { config::DiscordPresence = enabled; });
#endif

	ImGui::Spacing();

	// ========================================
	// RetroAchievements Section
	// ========================================
#ifdef USE_RACHIEVEMENTS
	if (ImGui::CollapsingHeader(ICON_FA_TROPHY " RetroAchievements##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"EnableAchievements",
			ICON_FA_TROPHY,
			"Enable RetroAchievements",
			"Track your game achievements using RetroAchievements.org",
			static_cast<bool>(config::EnableAchievements),
			[](bool enabled) { config::EnableAchievements = enabled; });

		if (config::EnableAchievements)
		{
			ImGui::Indent();

			RenderGeneralToggleSettingRow(
				"AchievementsHardcoreMode",
				ICON_FA_FIRE,
				"Hardcore Mode",
				"Enable RetroAchievements hardcore mode. Using cheats and loading a state are not allowed.",
				static_cast<bool>(config::AchievementsHardcoreMode),
				[](bool enabled) { config::AchievementsHardcoreMode = enabled; });

			ImGui::Spacing();

			// Username display and login/logout
			if (achievements::isLoggedOn())
			{
				ImGui::Text("User: %s", config::AchievementsUserName.get().c_str());
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Logout##RA").x - ImGui::GetStyle().FramePadding.x * 2);
				if (ImGui::Button("Logout##RA"))
					achievements::logout();
			}
			else
			{
				static char password[256];
				static std::future<void> futureLogin;

				RenderGeneralTextInputSettingRow(
					"RAUsernameRow",
					ICON_FA_USER,
					"Username",
					"RetroAchievements account username",
					[]() {
						InputText("##UsernameInput", &config::AchievementsUserName.get(), ImGuiInputTextFlags_None);
					});

				RenderGeneralTextInputSettingRow(
					"RAPasswordRow",
					ICON_FA_LOCK,
					"Password",
					"RetroAchievements account password",
					[&]() {
						InputText("##PasswordInput", password, sizeof(password), ImGuiInputTextFlags_Password);
					});

				if (futureLogin.valid())
				{
					if (futureLogin.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
						ImGui::Text("Authenticating...");
					else
					{
						try
						{
							futureLogin.get();
						}
						catch (const FlycastException& e)
						{
							gui_error(e.what());
						}
					}
				}

				{
					DisabledScope _(config::AchievementsUserName.get().empty() || password[0] == '\0');
					if (ImGui::Button("Login##RA") && !futureLogin.valid())
					{
						achievements::init();
						futureLogin = achievements::login(config::AchievementsUserName.get().c_str(), password);
						memset(password, 0, sizeof(password));
					}
				}
			}

			ImGui::Unindent();
		}
	}
	ImGui::Spacing();
#endif

	// ========================================
	// Custom Paths Section
	// ========================================
#if !defined(TARGET_IPHONE)
	SectionHeaderWithIcon(ICON_FA_SLIDERS, "Custom Paths");

	managePathList("BIOS Folders", config::BiosPath.get(),
		"Folders containing BIOS/Flash files (e.g. dc_boot.bin, dc_flash.bin) and arcade BIOS");
	ImGui::Spacing();

#if !defined(__ANDROID__)
	manageSinglePath("VMU Folder", config::VMUPath,
		"Folder where VMU (.bin) saves are stored");
	ImGui::Spacing();

	managePathList("Savestate Folders", config::SavestatePath.get(),
		"Folders for save states. First path is used for new states; all are searched when loading");
	ImGui::Spacing();

	manageSinglePath("Game Save Folder", config::SavePath,
		"Folder for game save data (e.g. arcade NVRAM)");
	ImGui::Spacing();
#endif

	managePathList("Texture Pack Folders", config::TexturePath.get(),
		"Folders containing textures/<gameId> or <gameId> under a textures subfolder");
	ImGui::Spacing();

#if !defined(__ANDROID__)
	manageSinglePath("Texture Dump Folder", config::TextureDumpPath,
		"Folder where texture dumps are saved. Game-specific subfolders will be created automatically");
	ImGui::Spacing();

	managePathList("Controller Mapping Folders", config::MappingsPath.get(),
		"Folders containing controller mapping files (.cfg). The emulator also looks in Home Folder/mappings. Per-game mappings are suffixed with _<gameId>.cfg");
	ImGui::Spacing();

	managePathList("Cheat Folders", config::CheatPath.get(),
		"Folders containing cheat files (.cht/.txt) named with the game ID. Flycast will auto-load matching files if present");
	ImGui::Spacing();
#endif  // !ANDROID
#endif  // !IPHONE
}


void renderVideoTab()
{
	ScopedTwoLineRowStyle videoRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Video Configuration");
	ImGui::Separator();

	// ============================================================
	// PRESET SELECTOR
	// ============================================================
	SectionHeaderWithIcon(ICON_FA_BOOKMARK, "Quality Presets");

	static int currentPresetIndex = 0;
	static bool presetJustApplied = false;

	// Auto-detect current preset
	if (!presetJustApplied)
	{
		VideoPresetLevel detected = detectCurrentPreset();
		if (detected != VideoPresetLevel::Custom)
			currentPresetIndex = static_cast<int>(detected);
	}

	// Build preset labels
	const char* presetLabels[] = {
		"Potato (Max Performance)",
		"Low",
		"Medium (Balanced)",
		"High",
		"God Mode (Max Quality)",
		"Custom"
	};

	// Preset dropdown
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	if (ImGui::Combo("##VideoPreset", &currentPresetIndex, presetLabels, 6))
	{
		// User changed preset
		if (currentPresetIndex < 5)  // Not "Custom"
		{
			const VideoPreset* preset = getPresetByLevel(
				static_cast<VideoPresetLevel>(currentPresetIndex)
			);
			if (preset)
			{
				applyVideoPreset(*preset);
				presetJustApplied = true;
			}
		}
	}

	// Show preset description on hover
	if (ImGui::IsItemHovered() && currentPresetIndex < 5)
	{
		const VideoPreset* preset = getPresetByLevel(
			static_cast<VideoPresetLevel>(currentPresetIndex)
		);
		if (preset && preset->description)
			ImGui::SetTooltip("%s", preset->description);
	}

	// Show "Custom" indicator if settings were manually changed
	VideoPresetLevel currentLevel = detectCurrentPreset();
	if (currentLevel == VideoPresetLevel::Custom && !presetJustApplied)
	{
		ImGui::SameLine();
		ImGui::TextDisabled("(Modified)");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Settings have been manually modified from the last preset");
	}

	ImGui::Separator();
	presetJustApplied = false;

	// Graphics API Selection section
	SectionHeaderWithIcon(ICON_FA_GEAR, "Graphics API");

	int renderApi;
	bool perPixel;
	switch (config::RendererType)
	{
	default:
	case RenderType::OpenGL:
		renderApi = 0;
		perPixel = false;
		break;
	case RenderType::OpenGL_OIT:
		renderApi = 0;
		perPixel = true;
		break;
	case RenderType::Vulkan:
		renderApi = 1;
		perPixel = false;
		break;
	case RenderType::Vulkan_OIT:
		renderApi = 1;
		perPixel = true;
		break;
	case RenderType::DirectX9:
		renderApi = 2;
		perPixel = false;
		break;
	case RenderType::DirectX11:
		renderApi = 3;
		perPixel = false;
		break;
	case RenderType::DirectX11_OIT:
		renderApi = 3;
		perPixel = true;
		break;
	}

	constexpr int apiCount = 0
#ifdef USE_VULKAN
		+ 1
#endif
#ifdef USE_DX9
		+ 1
#endif
#ifdef USE_OPENGL
		+ 1
#endif
#ifdef USE_DX11
		+ 1
#endif
		;

	if (apiCount > 1)
	{
		std::array<const char*, 4> apiLabels {};
		std::array<int, 4> apiValues {};
		int apiOptionCount = 0;
#ifdef USE_OPENGL
		apiLabels[apiOptionCount] = "OpenGL";
		apiValues[apiOptionCount++] = 0;
#endif
#ifdef USE_VULKAN
#ifdef __APPLE
		apiLabels[apiOptionCount] = "Vulkan (Metal)";
#else
		apiLabels[apiOptionCount] = "Vulkan";
#endif
		apiValues[apiOptionCount++] = 1;
#endif
#ifdef USE_DX9
		apiLabels[apiOptionCount] = "DirectX 9";
		apiValues[apiOptionCount++] = 2;
#endif
#ifdef USE_DX11
		apiLabels[apiOptionCount] = "DirectX 11";
		apiValues[apiOptionCount++] = 3;
#endif

		int currentApiIndex = 0;
		for (int i = 0; i < apiOptionCount; i++)
		{
			if (apiValues[i] == renderApi)
			{
				currentApiIndex = i;
				break;
			}
		}

		SettingsUI::PopupConfig apiCfg {};
		apiCfg.type = SettingsUI::PopupType::Options;
		apiCfg.options.label = "Graphics API";
		apiCfg.options.icon = ICON_FA_GEAR;
		apiCfg.options.popupID = "GraphicsApiPopup";
		apiCfg.options.options = apiLabels.data();
		apiCfg.options.optionCount = apiOptionCount;
		apiCfg.options.currentValue = &currentApiIndex;
		apiCfg.options.valueWidth = 220.0f;
		apiCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= apiOptionCount)
				return false;
			renderApi = apiValues[selectedIndex];
			if (renderApi == 2)
				perPixel = false;
			return true;
		};

		RenderGeneralPopupSettingRow(
			"GraphicsApiSetting",
			"The best option depends on the game and your device hardware. If you're at your FPS limit, try changing this.",
			apiCfg);
	}

	// Transparent Sorting section
	SectionHeaderWithIcon(ICON_FA_WAND_SPARKLES, "Transparent Sorting");

	{
		const bool has_per_pixel = GraphicsContext::Instance()->hasPerPixel();
		int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;

		std::array<const char*, 3> sortingLabels {};
		std::array<int, 3> sortingValues {};
		int sortingOptionCount = 0;
		sortingLabels[sortingOptionCount] = "Per Triangle";
		sortingValues[sortingOptionCount++] = 0;
		sortingLabels[sortingOptionCount] = "Per Strip";
		sortingValues[sortingOptionCount++] = 1;
		if (has_per_pixel)
		{
			sortingLabels[sortingOptionCount] = "Per Pixel";
			sortingValues[sortingOptionCount++] = 2;
		}

		int sortingSelection = 0;
		for (int i = 0; i < sortingOptionCount; i++)
		{
			if (sortingValues[i] == renderer)
			{
				sortingSelection = i;
				break;
			}
		}

		SettingsUI::PopupConfig sortingCfg {};
		sortingCfg.type = SettingsUI::PopupType::Options;
		sortingCfg.options.label = "Sorting Method";
		sortingCfg.options.icon = ICON_FA_WAND_SPARKLES;
		sortingCfg.options.popupID = "TransparentSortingPopup";
		sortingCfg.options.options = sortingLabels.data();
		sortingCfg.options.optionCount = sortingOptionCount;
		sortingCfg.options.currentValue = &sortingSelection;
		sortingCfg.options.valueWidth = 220.0f;
		sortingCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= sortingOptionCount)
				return false;
			const int selectedRenderer = sortingValues[selectedIndex];
			switch (selectedRenderer)
			{
			case 0:
				perPixel = false;
				config::PerStripSorting.set(false);
				break;
			case 1:
				perPixel = false;
				config::PerStripSorting.set(true);
				break;
			case 2:
				perPixel = true;
				config::PerStripSorting.set(false);
				break;
			default:
				return false;
			}
			return true;
		};

		RenderGeneralPopupSettingRow(
			"TransparentSortingSetting",
			"Choose the tradeoff between transparency accuracy and rendering performance.",
			sortingCfg);
	}


	// Rendering Options section
	SectionHeaderWithIcon(ICON_FA_SLIDERS, "Rendering Options");

	// Internal Resolution
	{
		constexpr std::array<float, 20> scalings {
			0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.25f, 2.5f, 2.75f,
			3.f, 3.5f, 4.f, 4.5f, 5.f, 5.5f, 6.f, 7.f, 8.f, 9.f
		};
		constexpr std::array<const char*, 20> scalingNames {
			"Half", "x0.75", "Native", "x1.25", "x1.5", "x1.75", "x2", "x2.25", "x2.5", "x2.75",
			"x3", "x3.5", "x4", "x4.5", "x5", "x5.5", "x6", "x7", "x8", "x9"
		};

		std::array<int, scalings.size()> horizontalRes {};
		std::array<std::string, scalings.size()> resolutionLabels {};
		std::array<const char*, scalings.size()> resolutionLabelPtrs {};
		int internalResSelection = 0;

		for (size_t i = 0; i < scalings.size(); i++)
		{
			const int verticalRes = static_cast<int>(scalings[i] * 480.0f);
			horizontalRes[i] = !config::Widescreen
				? static_cast<int>(scalings[i] * 640.0f)
				: static_cast<int>(scalings[i] * 480.0f * 16.0f / 9.0f);

			if (horizontalRes[i] == config::RenderResolution.get())
				internalResSelection = static_cast<int>(i);

			resolutionLabels[i] = std::to_string(horizontalRes[i]) + "x" + std::to_string(verticalRes) + " (" + scalingNames[i] + ")";
			resolutionLabelPtrs[i] = resolutionLabels[i].c_str();
		}

		SettingsUI::PopupConfig internalResCfg {};
		internalResCfg.type = SettingsUI::PopupType::Options;
		internalResCfg.options.label = "Internal Resolution";
		internalResCfg.options.icon = ICON_FA_DISPLAY;
		internalResCfg.options.popupID = "InternalResPopup";
		internalResCfg.options.options = resolutionLabelPtrs.data();
		internalResCfg.options.optionCount = static_cast<int>(resolutionLabelPtrs.size());
		internalResCfg.options.currentValue = &internalResSelection;
		internalResCfg.options.valueWidth = 220.0f;
		internalResCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(horizontalRes.size()))
				return false;
			config::RenderResolution.set(horizontalRes[selectedIndex]);
			return true;
		};

		RenderGeneralPopupSettingRow(
			"InternalResolutionSetting",
			"Internal render resolution. Higher values improve clarity but increase GPU cost.",
			internalResCfg,
			"Internal render resolution. Higher values can be used for supersampling and better antialiasing.");
	}

	RenderGeneralToggleSettingRow(
		"IntegerScaling",
		ICON_FA_COMPRESS,
		"Integer Scaling",
		"Scales output by maximum integer multiple allowed by display resolution",
		static_cast<bool>(config::IntegerScale),
		[](bool enabled) { config::IntegerScale.set(enabled); },
		"Scales output by maximum integer multiple allowed by display resolution");

	RenderGeneralToggleSettingRow(
		"LinearInterpolation",
		ICON_FA_WAND_MAGIC,
		"Linear Interpolation",
		"Scales output with linear interpolation. Disable for nearest neighbor. Use with integer scaling.",
		static_cast<bool>(config::LinearInterpolation),
		[](bool enabled) { config::LinearInterpolation.set(enabled); },
		"Scales output with linear interpolation. Disable for nearest neighbor. Use with integer scaling.");

#ifndef TARGET_IPHONE
	RenderGeneralToggleSettingRow(
		"VSync",
		ICON_FA_ARROWS_ROTATE,
		"VSync",
		"Synchronizes frame rate with screen refresh rate. Recommended",
		static_cast<bool>(config::VSync),
		[](bool enabled) { config::VSync.set(enabled); },
		"Synchronizes frame rate with screen refresh rate. Recommended");

	if (isVulkan(config::RendererType))
	{
		ImGui::Indent();
		RenderGeneralToggleSettingRow(
			"DuplicateFrames",
			ICON_FA_CLONE,
			"Duplicate Frames",
			"Duplicate frames on high refresh rate monitors (120 Hz and above)",
			static_cast<bool>(config::DupeFrames),
			[](bool enabled) { config::DupeFrames.set(enabled); },
			"Duplicate frames on high refresh rate monitors (120 Hz and above)",
			!config::VSync);
		ImGui::Unindent();
	}
#endif

	RenderGeneralToggleSettingRow(
		"ShowVMU",
		ICON_FA_GAMEPAD,
		"Show VMU In-Game",
		"Display VMU LCD screens while in-game",
		static_cast<bool>(config::FloatVMUs),
		[](bool enabled) { config::FloatVMUs.set(enabled); },
		"Display VMU LCD screens while in-game");

	RenderGeneralToggleSettingRow(
		"Framebuffer",
		ICON_FA_FILE,
		"Full Framebuffer Emulation",
		"Accurate VRAM framebuffer emulation. Required for some games' effects. Very slow and incompatible with upscaling/widescreen.",
		static_cast<bool>(config::EmulateFramebuffer),
		[](bool enabled) { config::EmulateFramebuffer.set(enabled); },
		"Accurate VRAM framebuffer emulation. Required for some games' effects. Very slow and incompatible with upscaling/widescreen.");

	RenderGeneralToggleSettingRow(
		"CustomTextures",
		ICON_FA_IMAGE,
		"Load Custom Textures",
		"Load custom/high-res textures from data/textures/<game id>",
		static_cast<bool>(config::CustomTextures),
		[](bool enabled) { config::CustomTextures.set(enabled); },
		"Load custom/high-res textures from data/textures/<game id>",
		game_started);

	ImGui::Indent();
	RenderGeneralToggleSettingRow(
		"PreloadTextures",
		ICON_FA_DOWNLOAD,
		"Preload Custom Textures",
		"Preload custom textures at game start. May improve performance but increases memory usage",
		static_cast<bool>(config::PreloadCustomTextures),
		[](bool enabled) { config::PreloadCustomTextures.set(enabled); },
		"Preload custom textures at game start. May improve performance but increases memory usage",
		!config::CustomTextures);
	ImGui::Unindent();

	// Aspect Ratio section
	SectionHeaderWithIcon(ICON_FA_TV, "Aspect Ratio");

	RenderGeneralToggleSettingRow(
		"Widescreen",
		ICON_FA_TV,
		"Widescreen",
		"Draw geometry outside normal 4:3 aspect ratio. May produce glitches in revealed areas.",
		static_cast<bool>(config::Widescreen),
		[](bool enabled) { config::Widescreen.set(enabled); },
		"Draw geometry outside normal 4:3 aspect ratio. May produce glitches in revealed areas.");

	ImGui::Indent();
	RenderGeneralToggleSettingRow(
		"SuperWidescreen",
		ICON_FA_EXPAND,
		"Super Widescreen",
		"Use full width of screen/window when aspect ratio > 16:9. Aspect Fill removes black bars.",
		static_cast<bool>(config::SuperWidescreen),
		[](bool enabled) { config::SuperWidescreen.set(enabled); },
		"Use full width of screen/window when aspect ratio > 16:9. Not compatible with integer scaling.",
		!config::Widescreen || config::IntegerScale);
	ImGui::Unindent();

	RenderGeneralToggleSettingRow(
		"WidescreenGameCheats",
		ICON_FA_CODE,
		"Widescreen Game Cheats",
		"Modify game to display in 16:9 anamorphic format with horizontal stretching. Only some games are supported.",
		static_cast<bool>(config::WidescreenGameHacks),
		[](bool enabled) { config::WidescreenGameHacks.set(enabled); },
		"Modify game to display in 16:9 anamorphic format with horizontal stretching. Only some games are supported.");

	{
		SettingsUI::PopupSliderConfig stretchCfg {};
		stretchCfg.label = "Horizontal Stretching";
		stretchCfg.icon = ICON_FA_EXPAND;
		stretchCfg.popupID = "HorizontalStretchPopup";
		stretchCfg.description = "Stretches the image sideways to fill your display. Use 132% for 16:9 correction.";
		stretchCfg.currentValue = &config::ScreenStretching.get();
		stretchCfg.minValue = 100;
		stretchCfg.maxValue = 250;
		stretchCfg.format = "%d%%";
		stretchCfg.valueWidth = 220.0f;
		stretchCfg.sliderWidth = 320.0f;

		SettingsUI::PopupConfig stretchPopupCfg {};
		stretchPopupCfg.type = SettingsUI::PopupType::Slider;
		stretchPopupCfg.slider = stretchCfg;

		RenderGeneralPopupSettingRow(
			"HorizontalStretchingSetting",
			"Stretches the image sideways to fill your screen. 132% is common for 16:9 aspect correction.",
			stretchPopupCfg,
			"Stretches the image sideways to fill in your screen. Use 132% for 16:9 screens to correct aspect ratio.");
	}

	RenderGeneralToggleSettingRow(
		"RotateScreen",
		ICON_FA_ROTATE_RIGHT,
		"Rotate Screen 90°",
		"Rotate the screen 90° counterclockwise",
		static_cast<bool>(config::Rotate90),
		[](bool enabled) { config::Rotate90.set(enabled); },
		"Rotate the screen 90° counterclockwise");

	// Per Pixel Settings (only shown when Per Pixel sorting is enabled)
	if (perPixel)
	{
		SectionHeaderWithIcon(ICON_FA_IMAGE, "Per Pixel Settings");

		const std::array<int64_t, 4> bufSizes{ 512_MB, 1_GB, 2_GB, 4_GB };
		const std::array<std::string, 4> bufSizesText{ "512 MB", "1 GB", "2 GB", "4 GB" };

		float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
		ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
		u32 selected = 0;
		for (; selected < bufSizes.size(); selected++)
			if (bufSizes[selected] == config::PixelBufferSize)
				break;
		if (selected == bufSizes.size())
			selected = 0;

		if (ImGui::BeginCombo("##PixelBuffer", bufSizesText[selected].c_str(), ImGuiComboFlags_NoArrowButton))
		{
			for (u32 i = 0; i < bufSizes.size(); i++)
			{
				bool is_selected = i == selected;
				if (ImGui::Selectable(bufSizesText[i].c_str(), is_selected))
					config::PixelBufferSize = bufSizes[i];
				if (is_selected) {
					ImGui::SetItemDefaultFocus();
					selected = i;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		ImGui::SameLine(0, innerSpacing);

		if (ImGui::ArrowButton("##Decrease BufSize", ImGuiDir_Left))
		{
			if (selected > 0)
				config::PixelBufferSize = bufSizes[selected - 1];
		}
		ImGui::SameLine(0, innerSpacing);
		if (ImGui::ArrowButton("##Increase BufSize", ImGuiDir_Right))
		{
			if (selected < bufSizes.size() - 1)
				config::PixelBufferSize = bufSizes[selected + 1];
		}
		ImGui::SameLine(0, innerSpacing);

		ImGui::Text("Pixel Buffer Size");
		ImGui::SameLine();
		ShowHelpMarker("Size of pixel buffer. May need increase when upscaling by large factor.");

		OptionSlider("Maximum Layers", config::PerPixelLayers, 8, 128,
			"Maximum number of transparent layers. May need increase for complex scenes. Decrease to improve performance.");
	}

	// Performance section
	SectionHeaderWithIcon(ICON_FA_GAUGE_HIGH, "Performance");
	ImGui::Spacing();

	{
		const std::array<const char*, 3> autoSkipLabels { "Disabled", "Normal", "Maximum" };
		int autoSkipSelection = config::AutoSkipFrame.get();
		if (autoSkipSelection < 0 || autoSkipSelection >= static_cast<int>(autoSkipLabels.size()))
			autoSkipSelection = 0;

		SettingsUI::PopupConfig autoSkipCfg {};
		autoSkipCfg.type = SettingsUI::PopupType::Options;
		autoSkipCfg.options.label = "Automatic Frame Skipping";
		autoSkipCfg.options.icon = ICON_FA_FORWARD;
		autoSkipCfg.options.popupID = "AutoFrameSkipPopup";
		autoSkipCfg.options.options = autoSkipLabels.data();
		autoSkipCfg.options.optionCount = static_cast<int>(autoSkipLabels.size());
		autoSkipCfg.options.currentValue = &autoSkipSelection;
		autoSkipCfg.options.valueWidth = 220.0f;
		autoSkipCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(autoSkipLabels.size()))
				return false;
			config::AutoSkipFrame.set(selectedIndex);
			return true;
		};

		RenderGeneralPopupSettingRow(
			"AutoFrameSkippingSetting",
			"Automatically skip frames when emulation falls behind target speed.",
			autoSkipCfg,
			"Disabled: no auto-skip. Normal: skip when CPU and GPU are both slow. Maximum: skip when GPU is slow.");
	}

	{
		SettingsUI::PopupSliderConfig frameSkipCfg {};
		frameSkipCfg.label = "Frame Skipping";
		frameSkipCfg.icon = ICON_FA_FORWARD;
		frameSkipCfg.popupID = "FrameSkipPopup";
		frameSkipCfg.description = "Set how many frames to skip between rendered frames.";
		frameSkipCfg.currentValue = &config::SkipFrame.get();
		frameSkipCfg.minValue = 0;
		frameSkipCfg.maxValue = 6;
		frameSkipCfg.format = "%d";
		frameSkipCfg.valueWidth = 220.0f;
		frameSkipCfg.sliderWidth = 320.0f;

		SettingsUI::PopupConfig frameSkipPopupCfg {};
		frameSkipPopupCfg.type = SettingsUI::PopupType::Slider;
		frameSkipPopupCfg.slider = frameSkipCfg;

		RenderGeneralPopupSettingRow(
			"FrameSkippingSetting",
			"Set how many frames you want skipped between rendered frames.",
			frameSkipPopupCfg,
			"Number of frames to skip between rendered frames");
	}

	// 2x height toggle rows for Performance settings
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 12));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));

		// Shadows toggle
		RenderGeneralToggleSettingRow(
			"Shadows",
			ICON_FA_MOON,
			"Shadows",
			"Enable modifier volumes for realistic shadow effects",
			static_cast<bool>(config::ModifierVolumes),
			[](bool enabled) { config::ModifierVolumes.set(enabled); },
			"Enable modifier volumes, usually used for shadows");

		// Fog toggle
		RenderGeneralToggleSettingRow(
			"Fog",
			ICON_FA_CLOUD,
			"Fog",
			"Enable atmospheric fog effects for enhanced depth",
			static_cast<bool>(config::Fog),
			[](bool enabled) { config::Fog.set(enabled); },
			"Enable fog effects");

	ImGui::PopStyleVar(2);

	// Advanced section
	SectionHeaderWithIcon(ICON_FA_WAND_MAGIC, "Advanced");
	ImGui::Spacing();

	// 2x height toggle rows for Advanced settings
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 12));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));

		// Delay Frame Swapping
		RenderGeneralToggleSettingRow(
			"DelayFrameSwapping",
			ICON_FA_CLOCK,
			"Delay Frame Swapping",
			"Avoid flashing screen or glitchy videos",
			static_cast<bool>(config::DelayFrameSwapping),
			[](bool enabled) { config::DelayFrameSwapping = enabled; },
			"Useful to avoid flashing screen or glitchy videos. Not recommended on slow platforms");

		// Fix Upscale Bleeding Edge
		RenderGeneralToggleSettingRow(
			"FixUpscaleBleedingEdge",
			ICON_FA_PLUG,
			"Fix Upscale Bleeding Edge",
			"Fix texture bleeding when upscaling",
			static_cast<bool>(config::FixUpscaleBleedingEdge),
			[](bool enabled) { config::FixUpscaleBleedingEdge = enabled; },
			"Helps with texture bleeding when upscaling. Disable if pixels warp in 2D games (MVC2, CVS, KOF, etc.)");

		// Native Depth Interpolation
		RenderGeneralToggleSettingRow(
			"NativeDepthInterpolation",
			ICON_FA_MICROCHIP,
			"Native Depth Interpolation",
			"Fix texture corruption on AMD/Intel GPUs",
			static_cast<bool>(config::NativeDepthInterpolation),
			[](bool enabled) { config::NativeDepthInterpolation = enabled; },
			"Helps with texture corruption and depth issues on AMD GPUs. Can also help Intel GPUs in some cases.");

		// Copy Rendered Textures to VRAM
		RenderGeneralToggleSettingRow(
			"RenderToTextureBuffer",
			ICON_FA_COPY,
			"Copy Rendered Textures",
			"Copy rendered textures back to VRAM. Slower but accurate",
			static_cast<bool>(config::RenderToTextureBuffer),
			[](bool enabled) { config::RenderToTextureBuffer = enabled; },
			"Copy rendered-to textures back to VRAM. Slower but accurate");

	ImGui::PopStyleVar(2);

	// Anisotropic Filtering
	{
		const std::array<int, 5> anisoValues { 1, 2, 4, 8, 16 };
		const std::array<const char*, 5> anisoLabels { "Disabled", "2x", "4x", "8x", "16x" };
		int anisoSelection = 0;
		for (size_t i = 0; i < anisoValues.size(); i++)
		{
			if (anisoValues[i] == config::AnisotropicFiltering.get())
			{
				anisoSelection = static_cast<int>(i);
				break;
			}
		}

		SettingsUI::PopupConfig anisoCfg {};
		anisoCfg.type = SettingsUI::PopupType::Options;
		anisoCfg.options.label = "Anisotropic Filtering";
		anisoCfg.options.icon = ICON_FA_GEM;
		anisoCfg.options.popupID = "AnisotropicPopup";
		anisoCfg.options.options = anisoLabels.data();
		anisoCfg.options.optionCount = static_cast<int>(anisoLabels.size());
		anisoCfg.options.currentValue = &anisoSelection;
		anisoCfg.options.valueWidth = 220.0f;
		anisoCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(anisoValues.size()))
				return false;
			config::AnisotropicFiltering.set(anisoValues[selectedIndex]);
			return true;
		};

		RenderGeneralPopupSettingRow(
			"AnisotropicFilteringSetting",
			"Higher values reduce texture shimmer at sharp angles, with higher GPU cost.",
			anisoCfg,
			"Higher values make textures viewed at oblique angles sharper. More demanding on GPU. Only affects mipmapped textures.");
	}

	// Texture Filtering
	SectionHeaderWithIcon(ICON_FA_FILTER, "Texture Filtering");
	{
		const std::array<const char*, 3> textureFilterLabels {
			"Default",
			"Force Nearest-Neighbor",
			"Force Linear"
		};
		int textureFilteringSelection = config::TextureFiltering.get();
		if (textureFilteringSelection < 0 || textureFilteringSelection >= static_cast<int>(textureFilterLabels.size()))
			textureFilteringSelection = 0;

		SettingsUI::PopupConfig textureFilterCfg {};
		textureFilterCfg.type = SettingsUI::PopupType::Options;
		textureFilterCfg.options.label = "Filtering Mode";
		textureFilterCfg.options.icon = ICON_FA_FILTER;
		textureFilterCfg.options.popupID = "TextureFilteringPopup";
		textureFilterCfg.options.options = textureFilterLabels.data();
		textureFilterCfg.options.optionCount = static_cast<int>(textureFilterLabels.size());
		textureFilterCfg.options.currentValue = &textureFilteringSelection;
		textureFilterCfg.options.valueWidth = 220.0f;
		textureFilterCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(textureFilterLabels.size()))
				return false;
			config::TextureFiltering.set(selectedIndex);
			return true;
		};

		RenderGeneralPopupSettingRow(
			"TextureFilteringSetting",
			"Choose how textures are filtered when sampled.",
			textureFilterCfg,
			"Default uses game behavior. Force Nearest-Neighbor is crisper. Force Linear is smoother.");
	}

		// Show FPS Counter
		RenderGeneralToggleSettingRow(
			"ShowFPS",
			ICON_FA_GAUGE_HIGH,
			"Show FPS Counter",
			"Display on-screen frame rate",
			static_cast<bool>(config::ShowFPS),
			[](bool enabled) { config::ShowFPS.set(enabled); },
			"Display on-screen frame rate counter");

#ifdef VIDEO_ROUTING
	// Video Routing section (platform-specific)
#ifdef __APPLE__
	SectionHeaderWithIcon(ICON_FA_SHARE_NODES, "Video Routing (Syphon)");
#elif defined(_WIN32)
	((renderApi == 0) || (renderApi == 3)) ? SectionHeaderWithIcon(ICON_FA_SHARE_NODES, "Video Routing (Spout)") : SectionHeaderWithIcon(ICON_FA_SHARE_NODES, "Video Routing (Only available with OpenGL or DirectX 11)");
#else
	SectionHeaderWithIcon(ICON_FA_SHARE_NODES, "Video Routing");
#endif

#ifdef _WIN32
	DisabledScope routingScope(!((renderApi == 0) || (renderApi == 3)));
#endif

		// Send Video to Another Program
		RenderGeneralToggleSettingRow(
			"VideoRouting",
			ICON_FA_SHARE_NODES,
			"Send Video to Another Program",
			"Stream GPU texture directly to OBS Studio",
			static_cast<bool>(config::VideoRouting),
			[](bool enabled) { config::VideoRouting.set(enabled); },
			"Route GPU texture to OBS Studio directly instead of CPU-intensive Display/Window Capture");

	{
		DisabledScope scope(!config::VideoRouting);

			// Scale Down Before Sending (Indented)
			ImGui::Indent();
			{
				RenderGeneralToggleSettingRow(
					"VideoRoutingScale",
					ICON_FA_COMPRESS,
					"Scale Down Before Sending",
					"Share smaller texture for better performance",
					static_cast<bool>(config::VideoRoutingScale),
					[](bool enabled) { config::VideoRoutingScale.set(enabled); },
					"Could increase performance when sharing smaller texture, YMMV");
			}
			ImGui::Unindent();
				{
					static int vres = config::VideoRoutingVRes;
					if (vres != config::VideoRoutingVRes)
						vres = config::VideoRoutingVRes;

				SettingsUI::PopupSliderConfig vresCfg {};
				vresCfg.label = "Output Vertical Resolution";
				vresCfg.icon = ICON_FA_SLIDERS;
				vresCfg.popupID = "VideoRoutingVResPopup";
				vresCfg.description = "Set the vertical resolution used for the shared OBS texture output.";
				vresCfg.currentValue = &vres;
				vresCfg.minValue = 120;
				vresCfg.maxValue = 2160;
				vresCfg.defaultValue = 720;
				vresCfg.format = "%d px";
				vresCfg.valueWidth = 220.0f;
				vresCfg.sliderWidth = 320.0f;
				vresCfg.onValueChange = [&]() {
					config::VideoRoutingVRes = vres;
				};

				SettingsUI::PopupConfig vresPopupCfg {};
				vresPopupCfg.type = SettingsUI::PopupType::Slider;
				vresPopupCfg.slider = vresCfg;

				RenderGeneralPopupSettingRow(
					"VideoRoutingVResSetting",
					"Set vertical resolution for the shared output texture.",
					vresPopupCfg,
					"Set the vertical resolution used for shared texture routing.",
					!config::VideoRoutingScale);

					config::VideoRoutingVRes = vres;
				}

				const int outputWidth = config::VideoRoutingScale
					? config::VideoRoutingVRes * settings.display.width / settings.display.height
					: settings.display.width;
				const int outputHeight = config::VideoRoutingScale
					? config::VideoRoutingVRes
					: settings.display.height;
				const std::string outputSizeText = std::to_string(outputWidth) + " x " + std::to_string(outputHeight);

					ImGui::PushID("VideoRoutingOutputTextureSize");
					BeginTwoLineSettingRow("##row", "Calculated size of the shared output texture.", true);
					const ImVec2 line1Start = BeginTwoLineSettingRowContent();

				SettingIcon(ICON_FA_EXPAND, ImVec2(uiScaled(20), uiScaled(20)));
				ImGui::SameLine(0, uiScaled(8));
				ImGui::PushFont(largeFont);
				ImGui::TextUnformatted("Output Texture Size");
				ImGui::PopFont();

				const float valueWidth = uiScaled(220.0f);
				ImGui::SameLine(RightColumnX(valueWidth));
				const float valueStartX = ImGui::GetCursorPosX();
				const float valueTextWidth = ImGui::CalcTextSize(outputSizeText.c_str()).x;
				ImGui::SetCursorPosX(valueStartX + std::max(0.0f, (valueWidth - valueTextWidth) * 0.5f));
				ImGui::PushFont(largeFont);
				ImGui::TextUnformatted(outputSizeText.c_str());
				ImGui::PopFont();

				RenderTwoLineSettingDescription(line1Start, "Calculated size of the shared output texture.");
				ImGui::PopID();
				ImGui::Spacing();
			}
	#endif

	// Update renderer type based on selections
	switch (renderApi)
	{
	case 0:
		config::RendererType = perPixel ? RenderType::OpenGL_OIT : RenderType::OpenGL;
		break;
	case 1:
		config::RendererType = perPixel ? RenderType::Vulkan_OIT : RenderType::Vulkan;
		break;
	case 2:
		config::RendererType = RenderType::DirectX9;
		break;
	case 3:
		config::RendererType = perPixel ? RenderType::DirectX11_OIT : RenderType::DirectX11;
		break;
	}
}

void renderAudioTab()
{
	ScopedTwoLineRowStyle audioRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Audio Configuration");
	ImGui::Separator();

	// Audio Playback Section
	SectionHeaderWithIcon(ICON_FA_VOLUME_HIGH, "Playback");

	RenderGeneralToggleSettingRow(
		"EnableDSP",
		ICON_FA_SLIDERS,
		"Enable DSP",
		"Dreamcast Digital Sound Processor",
		static_cast<bool>(config::DSPEnabled),
		[](bool enabled) { config::DSPEnabled.set(enabled); },
		"Enable the Dreamcast Digital Sound Processor. Only recommended on fast platforms");

	RenderGeneralToggleSettingRow(
		"EnableVmuSounds",
		ICON_FA_BELL,
		"VMU Sounds",
		"Play VMU beeps when enabled",
		static_cast<bool>(config::VmuSound),
		[](bool enabled) { config::VmuSound.set(enabled); },
		"Play VMU beeps when enabled");

	// Volume Level
	{
		static int volumeTemp = config::AudioVolume.get();

		SettingsUI::PopupSliderConfig volumeConfig {};
		volumeConfig.label = "Volume Level";
		volumeConfig.icon = ICON_FA_VOLUME_HIGH;
		volumeConfig.tooltip = "Adjust the emulator's audio level";
		volumeConfig.popupID = "VolumeLevelPopup";
		volumeConfig.description = "Adjust the emulator's audio level";
		volumeConfig.currentValue = &volumeTemp;
		volumeConfig.minValue = 0;
		volumeConfig.maxValue = 100;
		volumeConfig.defaultValue = 100;
		volumeConfig.format = "%d%%";
		volumeConfig.applyButtonText = "Apply";
		volumeConfig.onApply = []() {
			config::AudioVolume.set(volumeTemp);
			config::AudioVolume.calcDbPower();
		};
		volumeConfig.onValueChange = nullptr;
		volumeConfig.iconSize = uiScaled(20.0f);
		volumeConfig.iconSpacing = uiScaled(8.0f);
		volumeConfig.valueWidth = 220.0f;
		volumeConfig.sliderWidth = 320.0f;
		volumeConfig.showApplyFlag = nullptr;
		volumeConfig.hasPendingChanges = false;

		SettingsUI::PopupConfig volumePopupCfg {};
		volumePopupCfg.type = SettingsUI::PopupType::Slider;
		volumePopupCfg.slider = volumeConfig;

		RenderGeneralPopupSettingRow(
			"VolumeLevelSetting",
			"Adjust the emulator's audio level",
			volumePopupCfg);
	}

	// Audio Latency Section
#ifdef __ANDROID__
	// Automatic Latency (Android only)
	if (config::AudioBackend.get() == "auto" || config::AudioBackend.get() == "android")
	{
		RenderGeneralToggleSettingRow(
			"AutoLatency",
			ICON_FA_WAND_MAGIC,
			"Automatic Latency",
			"Automatically set audio latency",
			static_cast<bool>(config::AutoLatency),
			[](bool enabled) { config::AutoLatency.set(enabled); },
			"Automatically set audio latency. Recommended");
	}
#endif

	// Manual Latency Control
	if (!config::AutoLatency
		|| (config::AudioBackend.get() != "auto" && config::AudioBackend.get() != "android"))
	{
		static int latencyTemp = (int)roundf(config::AudioBufferSize.get() * 1000.f / 44100.f);

		SettingsUI::PopupSliderConfig latencyConfig {};
		latencyConfig.label = "Audio Latency";
		latencyConfig.icon = ICON_FA_STOPWATCH;
		latencyConfig.tooltip = "Buffer size in milliseconds";
		latencyConfig.popupID = "AudioLatencyPopup";
		latencyConfig.description = "Lower values reduce audio lag but may cause audio issues. "
		                            "Higher values are more stable but increase latency.";
		latencyConfig.currentValue = &latencyTemp;
		latencyConfig.minValue = 12;
		latencyConfig.maxValue = 512;
		latencyConfig.defaultValue = 64;
		latencyConfig.format = "%d ms";
		latencyConfig.applyButtonText = "Apply";
		latencyConfig.onApply = nullptr;
		latencyConfig.onValueChange = []() {
			config::AudioBufferSize.set((int)roundf(latencyTemp * 44100.f / 1000.f));
		};
		latencyConfig.iconSize = uiScaled(20.0f);
		latencyConfig.iconSpacing = uiScaled(8.0f);
		latencyConfig.valueWidth = 220.0f;
		latencyConfig.sliderWidth = 320.0f;
		latencyConfig.showApplyFlag = nullptr;
		latencyConfig.hasPendingChanges = false;

		SettingsUI::PopupConfig latencyPopupCfg {};
		latencyPopupCfg.type = SettingsUI::PopupType::Slider;
		latencyPopupCfg.slider = latencyConfig;

		RenderGeneralPopupSettingRow(
			"AudioLatencySetting",
			"Buffer size in milliseconds",
			latencyPopupCfg);
	}

	// Audio Latency Section
	SectionHeaderWithIcon(ICON_FA_CLOCK, "Audio Latency");

	// Audio Driver Selection
	SectionHeaderWithIcon(ICON_FA_HEADPHONES, "Audio Driver");

	// Get current backend name for display
	AudioBackend *currentBackend = AudioBackend::getBackend(config::AudioBackend.get());
	std::string currentBackendName = currentBackend ? currentBackend->name : "Unknown";

	// Build dynamic options list
	std::vector<const char*> backendOptions;
	std::vector<std::string> backendLabels;
	for (u32 i = 0; i < AudioBackend::getCount(); i++)
	{
		const AudioBackend* backend = AudioBackend::getBackend(i);
		backendLabels.push_back(backend->slug + " - " + backend->name);
		backendOptions.push_back(backendLabels.back().c_str());
	}

	// Find current index
	int currentBackendIndex = 0;
	for (u32 i = 0; i < AudioBackend::getCount(); i++)
	{
		if (config::AudioBackend.get() == AudioBackend::getBackend(i)->slug)
		{
			currentBackendIndex = (int)i;
			break;
		}
	}

	// Static storage for selected index
	static int selectedBackendIndex = currentBackendIndex;

	SettingsUI::PopupOptionsConfig driverConfig {};
	driverConfig.label = "Audio Driver";
	driverConfig.icon = ICON_FA_HEADPHONES;
	driverConfig.popupID = "AudioDriverPopup";
	driverConfig.options = backendOptions.data();
	driverConfig.optionCount = (int)AudioBackend::getCount();
	driverConfig.currentValue = &selectedBackendIndex;
	driverConfig.valueToString = nullptr;  // Use options array directly
	driverConfig.storageIndexMap = nullptr;
	driverConfig.onChange = [](int selectedIndex) {
		const AudioBackend* backend = AudioBackend::getBackend(selectedIndex);
		if (backend)
		{
			config::AudioBackend.set(backend->slug);
			selectedBackendIndex = selectedIndex;
		}
		return true;
	};
	driverConfig.disabled = false;
	driverConfig.iconSize = uiScaled(20.0f);
	driverConfig.iconSpacing = uiScaled(8.0f);
	driverConfig.valueWidth = 220.0f;

	SettingsUI::PopupConfig driverPopupCfg {};
	driverPopupCfg.type = SettingsUI::PopupType::Options;
	driverPopupCfg.options = driverConfig;

	RenderGeneralPopupSettingRow(
		"AudioDriverSetting",
		"The audio backend determines how audio is submitted to the host API.",
		driverPopupCfg);

	AudioBackend *current_backend = currentBackend;

	// Backend-specific options
	if (current_backend != nullptr)
	{
		// Get backend specific options
		int option_count;
		const AudioBackend::Option *options = current_backend->getOptions(&option_count);

		if (option_count > 0)
		{
			SectionHeaderWithIcon(ICON_FA_SLIDERS, "Driver Options");

			for (int o = 0; o < option_count; o++)
			{
				std::string value = cfgLoadStr(current_backend->slug, options->name, "");

				if (options->type == AudioBackend::Option::integer)
				{
					static std::unordered_map<std::string, int> g_audioBackendIntValues;
					const std::string optionKey = current_backend->slug + "." + options->name;
					const std::string popupId = "AudioBackendInt_" + optionKey;
					const int cfgVal = stoi(value);

					if (!ImGui::IsPopupOpen(popupId.c_str()))
						g_audioBackendIntValues[optionKey] = cfgVal;

					int& tempVal = g_audioBackendIntValues[optionKey];

					SettingsUI::PopupSliderConfig backendIntCfg {};
					backendIntCfg.label = options->caption.c_str();
					backendIntCfg.icon = ICON_FA_SLIDERS;
					backendIntCfg.popupID = popupId.c_str();
					backendIntCfg.description = options->caption.c_str();
					backendIntCfg.currentValue = &tempVal;
					backendIntCfg.minValue = options->minValue;
					backendIntCfg.maxValue = options->maxValue;
					backendIntCfg.defaultValue = options->minValue;
					backendIntCfg.format = "%d";
					backendIntCfg.valueWidth = 220.0f;
					backendIntCfg.sliderWidth = 320.0f;
					backendIntCfg.onApply = [&]() {
						cfgSaveStr(current_backend->slug, options->name, std::to_string(tempVal));
					};

					SettingsUI::PopupConfig backendIntPopupCfg {};
					backendIntPopupCfg.type = SettingsUI::PopupType::Slider;
					backendIntPopupCfg.slider = backendIntCfg;

					RenderGeneralPopupSettingRow(
						popupId.c_str(),
						options->caption.c_str(),
						backendIntPopupCfg);
				}
					else if (options->type == AudioBackend::Option::checkbox)
					{
						bool check = value == "1";
						const std::string optionName = options->name;
						const std::string rowId = "AudioBackendOption_" + optionName;
						RenderGeneralToggleSettingRow(
							rowId.c_str(),
							ICON_FA_CHECK,
							options->caption.c_str(),
							"",
							check,
							[&, optionName](bool enabled) {
								cfgSaveStr(current_backend->slug, optionName, enabled ? "1" : "0");
							},
							options->caption.c_str());
					}
				else if (options->type == AudioBackend::Option::list)
				{
					std::vector<const char*> listOptions;
					std::vector<std::string> listLabels;
					listLabels.reserve(options->values.size());
					listOptions.reserve(options->values.size());
					for (const auto& cur : options->values)
					{
						listLabels.push_back(cur);
						listOptions.push_back(listLabels.back().c_str());
					}

					int currentIndex = 0;
					for (size_t idx = 0; idx < options->values.size(); idx++)
					{
						if (options->values[idx] == value)
						{
							currentIndex = static_cast<int>(idx);
							break;
						}
					}

					static std::unordered_map<std::string, int> g_audioBackendListIndex;
					const std::string optionKey = current_backend->slug + "." + options->name;
					const std::string popupId = "AudioBackendList_" + optionKey;
					if (!ImGui::IsPopupOpen(popupId.c_str()))
						g_audioBackendListIndex[optionKey] = currentIndex;
					int& tempIndex = g_audioBackendListIndex[optionKey];

					SettingsUI::PopupOptionsConfig listCfg {};
					listCfg.label = options->caption.c_str();
					listCfg.icon = ICON_FA_LIST;
					listCfg.popupID = popupId.c_str();
					listCfg.options = listOptions.data();
					listCfg.optionCount = static_cast<int>(listOptions.size());
					listCfg.currentValue = &tempIndex;
					listCfg.valueWidth = 220.0f;
					listCfg.onChange = [&](int selectedIndex) {
						if (selectedIndex < 0 || selectedIndex >= static_cast<int>(options->values.size()))
							return false;
						cfgSaveStr(current_backend->slug, options->name, options->values[selectedIndex]);
						return true;
					};

					SettingsUI::PopupConfig listPopupCfg {};
					listPopupCfg.type = SettingsUI::PopupType::Options;
					listPopupCfg.options = listCfg;

					RenderGeneralPopupSettingRow(
						popupId.c_str(),
						options->caption.c_str(),
						listPopupCfg);
				}
				else
				{
					WARN_LOG(RENDERER, "Unknown audio backend option type");
				}

				options++;
			}
		}
	}
}

void renderControlsTab()
{
	ScopedTwoLineRowStyle controlsRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Controls Configuration");
	ImGui::Separator();

	SectionHeaderWithIcon(ICON_FA_GAMEPAD, "Physical Devices");

	if (ImGui::BeginTable("physicalDevices", 6, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
	{
		ImGui::TableSetupColumn("System", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);

		const float portComboWidth = ImGui::CalcTextSize("None").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
		const ImVec4 gray(0.5f, 0.5f, 0.5f, 1.f);

		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextColored(gray, "System");

		ImGui::TableSetColumnIndex(1);
		ImGui::TextColored(gray, "Name");

		ImGui::TableSetColumnIndex(2);
		ImGui::TextColored(gray, "Status");

		ImGui::TableSetColumnIndex(4);
		ImGui::TextColored(gray, "Port");

		// Iterate through all detected gamepads
		for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
		{
			std::shared_ptr<GamepadDevice> gamepad = GamepadDevice::GetGamepad(i);
			if (!gamepad)
				continue;
			char gamepad_row_id[32];
			snprintf(gamepad_row_id, sizeof(gamepad_row_id), "gamepad_row_%d", i);
			ImguiID gamepadRowId(gamepad_row_id);
			(void)gamepadRowId;

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("%s", gamepad->api_name().c_str());

			ImGui::TableSetColumnIndex(1);
			ImGui::Text("%s", gamepad->name().c_str());

			ImGui::TableSetColumnIndex(2);
#if defined(USE_DREAMLINK_DEVICES)
			DreamLinkGamepad* dreamLinkGamepad = dynamic_cast<DreamLinkGamepad*>(gamepad.get());
			if (dreamLinkGamepad != nullptr) {
				ImGui::Text("DreamLink status: %s", dreamLinkGamepad->dreamLinkStatus());
			}
#endif

			ImGui::TableSetColumnIndex(5);
			char port_name[32];
			snprintf(port_name, sizeof(port_name), "##mapleport%d", i);
				ImguiID _(port_name);
				ImGui::SetNextItemWidth(portComboWidth);

				// Port selection combo (None, A, B, C, D, All)
				const char* current_port = kMaplePorts[gamepad->maple_port() + 1];
				if (ImGui::BeginCombo(port_name, current_port))
				{
					for (int j = -1; j < IM_ARRAYSIZE(kMaplePorts) - 1; j++)
					{
						bool is_selected = gamepad->maple_port() == j;
						if (ImGui::Selectable(kMaplePorts[j + 1], &is_selected))
						{
							gamepad->set_maple_port(j);
							g_mapleDevicesChangedInSettings = true;
						}
						if (is_selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

			ImGui::TableSetColumnIndex(4);
			ImGui::SameLine(0, 8.0f);

			// Map button for controller mapping
			if (gamepad->remappable() && ImGui::Button("Map"))
			{
				ResetControllerMappingRuntime();
				g_currentGamepadForMapping = gamepad;
				g_currentGamepadForMapping->listenButtons(buttonListener);
				g_gamepad_port_for_mapping = 0;
				ImGui::OpenPopup("Controller Mapping");
			}

			// Settings button for rumble/deadzone/saturation
			if (gamepad->is_rumble_enabled() || gamepad->has_analog_stick() || gamepad->is_virtual_gamepad())
			{
				ImGui::SameLine(0, 16.0f);
				if (ImGui::Button("Settings"))
				{
					g_currentGamepadForSettings = gamepad;
					ImGui::OpenPopup("Gamepad Settings");
				}
			}

			// Render the gamepad settings popup for this gamepad
			if (g_currentGamepadForSettings == gamepad)
			{
				gamepadSettingsPopup(gamepad);
				if (!ImGui::IsPopupOpen("Gamepad Settings"))
					g_currentGamepadForSettings.reset();
			}

			// Render controller mapping popup for the selected gamepad
			if (g_currentGamepadForMapping == gamepad)
			{
				controller_mapping_popup(gamepad);
				if (!ImGui::IsPopupOpen("Controller Mapping"))
					ResetControllerMappingRuntime();
			}
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();

		SectionHeaderWithIcon(ICON_FA_CROSSHAIRS, "Mouse & Raw Input");

		{
			// Temp storage for popup slider
			static int sensitivityTemp = config::MouseSensitivity.get();

			SettingsUI::PopupSliderConfig sensitivityConfig {};
			sensitivityConfig.label = "Sensitivity";
			sensitivityConfig.icon = ICON_FA_BULLSEYE;
			sensitivityConfig.tooltip = "Mouse sensitivity for light gun and mouse emulation";
			sensitivityConfig.popupID = "SensitivityPopup";
			sensitivityConfig.description = "Adjust the mouse sensitivity for light gun and mouse emulation. "
			                               "Higher values make the cursor move faster.";
			sensitivityConfig.currentValue = &sensitivityTemp;
			sensitivityConfig.minValue = 1;
			sensitivityConfig.maxValue = 500;
			sensitivityConfig.defaultValue = 100;
			sensitivityConfig.format = "%d";
			sensitivityConfig.applyButtonText = "Apply";
			sensitivityConfig.onApply = []() {
				config::MouseSensitivity.set(sensitivityTemp);
			};
			sensitivityConfig.onValueChange = nullptr;
			sensitivityConfig.iconSize = uiScaled(20.0f);
			sensitivityConfig.iconSpacing = uiScaled(8.0f);
			sensitivityConfig.valueWidth = uiScaled(220.0f);
			sensitivityConfig.sliderWidth = uiScaled(320.0f);
			sensitivityConfig.showApplyFlag = nullptr;
			sensitivityConfig.hasPendingChanges = false;

			SettingsUI::PopupConfig sensitivityPopupCfg {};
			sensitivityPopupCfg.type = SettingsUI::PopupType::Slider;
			sensitivityPopupCfg.slider = sensitivityConfig;

			RenderGeneralPopupSettingRow(
				"SensitivitySetting",
				"Adjust the mouse sensitivity for light gun and mouse emulation. Higher values make the cursor move faster.",
				sensitivityPopupCfg);
		}

#if defined(_WIN32) && !defined(TARGET_UWP)
		RenderGeneralToggleSettingRow(
			"UseRawInput",
			ICON_FA_CROSSHAIRS,
			"Use Raw Input",
			"Supports multiple pointing devices (mice, light guns) and keyboards",
			static_cast<bool>(config::UseRawInput),
			[](bool enabled) { config::UseRawInput.set(enabled); },
			"Supports multiple pointing devices (mice, light guns) and keyboards");
#endif

	ImGui::Spacing();

	SectionHeaderWithIcon(ICON_FA_PLUG, "Dreamcast Devices");

	// Device type arrays for dropdowns
	static const char *maple_device_types[] =
	{
		"None",
		"Sega Controller",
		"Light Gun",
		"Keyboard",
		"Mouse",
		"Twin Stick",
		"Arcade/Ascii Stick",
		"Maracas Controller",
		"Fishing Controller",
		"Pop'n Music controller",
		"Racing Controller",
		"Densha de Go! Controller",
		"Panther DC/Full Controller",
	};

	static const char *maple_expansion_device_types[] =
	{
		"None",
		"Sega VMU",
		"Vibration Pack",
		"Microphone",
	};

	// Helper lambda to get device name
	auto maple_device_name = [](MapleDeviceType type) -> const char* {
		switch (type)
		{
		case MDT_SegaController: return maple_device_types[1];
		case MDT_LightGun: return maple_device_types[2];
		case MDT_Keyboard: return maple_device_types[3];
		case MDT_Mouse: return maple_device_types[4];
		case MDT_TwinStick: return maple_device_types[5];
		case MDT_AsciiStick: return maple_device_types[6];
		case MDT_MaracasController: return maple_device_types[7];
		case MDT_FishingController: return maple_device_types[8];
		case MDT_PopnMusicController: return maple_device_types[9];
		case MDT_RacingController: return maple_device_types[10];
		case MDT_DenshaDeGoController: return maple_device_types[11];
		case MDT_SegaControllerXL: return maple_device_types[12];
		case MDT_None: default: return maple_device_types[0];
		}
	};

	auto maple_expansion_device_name = [](MapleDeviceType type) -> const char* {
		switch (type)
		{
		case MDT_SegaVMU: return maple_expansion_device_types[1];
		case MDT_PurupuruPack: return maple_expansion_device_types[2];
		case MDT_Microphone: return maple_expansion_device_types[3];
		case MDT_None: default: return maple_expansion_device_types[0];
		}
	};

	// Helper lambda for device type conversion
	auto maple_device_type_from_index = [](int idx) -> MapleDeviceType {
		switch (idx)
		{
		case 1: return MDT_SegaController;
		case 2: return MDT_LightGun;
		case 3: return MDT_Keyboard;
		case 4: return MDT_Mouse;
		case 5: return MDT_TwinStick;
		case 6: return MDT_AsciiStick;
		case 7: return MDT_MaracasController;
		case 8: return MDT_FishingController;
		case 9: return MDT_PopnMusicController;
		case 10: return MDT_RacingController;
		case 11: return MDT_DenshaDeGoController;
		case 12: return MDT_SegaControllerXL;
		case 0: default: return MDT_None;
		}
	};

	auto maple_expansion_device_type_from_index = [](int idx) -> MapleDeviceType {
		switch (idx)
		{
		case 1: return MDT_SegaVMU;
		case 2: return MDT_PurupuruPack;
		case 3: return MDT_Microphone;
		case 0: default: return MDT_None;
		}
	};

	bool is_there_any_xhair = false;

	if (ImGui::BeginTable("dreamcastDevices", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings,
			ImVec2(0, 0), 8.0f))
	{
		const float mainComboWidth = ImGui::CalcTextSize("Densha de Go! Controller").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
		const float expComboWidth = ImGui::CalcTextSize("Vibration Pack").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();

		// Settings 3-22: Dreamcast Device Ports (4 ports x device + expansion)
		for (int bus = 0; bus < MAPLE_PORTS; bus++)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text("Port %c", bus + 'A');

			ImGui::TableSetColumnIndex(1);
			char device_name[32];
			snprintf(device_name, sizeof(device_name), "##device%d", bus);
			float w = ImGui::CalcItemWidth() / 3;
			ImGui::PushItemWidth(w);
			ImGui::SetNextItemWidth(mainComboWidth);

			// Main device selection
			if (ImGui::BeginCombo(device_name, maple_device_name(config::MapleMainDevices[bus])))
			{
				for (int i = 0; i < 13; i++)
				{
					bool is_selected = config::MapleMainDevices[bus] == maple_device_type_from_index(i);
						if (ImGui::Selectable(maple_device_types[i], &is_selected))
						{
							config::MapleMainDevices[bus] = maple_device_type_from_index(i);
							g_mapleDevicesChangedInSettings = true;
						}
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}

			// Expansion port count based on device type
			int port_count = 0;
			switch (config::MapleMainDevices[bus]) {
				case MDT_SegaController:
				case MDT_SegaControllerXL:
					port_count = 2;
					break;
				case MDT_LightGun:
				case MDT_TwinStick:
				case MDT_AsciiStick:
				case MDT_RacingController:
					port_count = 1;
					break;
				default: break;
			}

			// Expansion device selections
			for (int port = 0; port < port_count; port++)
			{
				ImGui::TableSetColumnIndex(2 + port);
				snprintf(device_name, sizeof(device_name), "##device%d.%d", bus, port + 1);
				ImguiID _(device_name);
				ImGui::SetNextItemWidth(expComboWidth);

				if (ImGui::BeginCombo(device_name, maple_expansion_device_name(config::MapleExpansionDevices[bus][port])))
				{
					for (int i = 0; i < 4; i++)
					{
						bool is_selected = config::MapleExpansionDevices[bus][port] == maple_expansion_device_type_from_index(i);
							if (ImGui::Selectable(maple_expansion_device_types[i], &is_selected))
							{
								config::MapleExpansionDevices[bus][port] = maple_expansion_device_type_from_index(i);
								g_mapleDevicesChangedInSettings = true;
							}
						if (is_selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}

			// Light gun crosshair color
			if (config::MapleMainDevices[bus] == MDT_LightGun)
			{
				ImGui::TableSetColumnIndex(3);
				snprintf(device_name, sizeof(device_name), "##device%d.xhair", bus);
				ImguiID _(device_name);

				u32 color = config::CrosshairColor[bus];
				float xhairColor[4] {
					(color & 0xff) / 255.f,
					((color >> 8) & 0xff) / 255.f,
					((color >> 16) & 0xff) / 255.f,
					((color >> 24) & 0xff) / 255.f
				};
				bool enabled = color != 0;

				// Toggle switch (clickable row)
				float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
				bool rowClicked = ImGui::Selectable("##crosshair_row", false,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
					ImVec2(0, rowHeight));
				ImGui::PopStyleVar();

				if (rowClicked)
				{
					enabled = !enabled;
					if (enabled)
					{
						config::CrosshairColor[bus] = (u8)(std::round(xhairColor[0] * 255.f))
								| ((u8)(std::round(xhairColor[1] * 255.f)) << 8)
								| ((u8)(std::round(xhairColor[2] * 255.f)) << 16)
								| ((u8)(std::round(xhairColor[3] * 255.f)) << 24);
						if (config::CrosshairColor[bus] == 0)
							config::CrosshairColor[bus] = 0xC0FFFFFF;
					}
					else
					{
						config::CrosshairColor[bus] = 0;
					}
				}

				ImGui::SameLine(0, 0);

				// Icon
				SettingIcon(ICON_FA_CROSSHAIRS, ImVec2(uiScaled(16), uiScaled(16)));
				ImGui::SameLine(0, uiScaled(6));

				// Label
				ImGui::Text("Crosshair");

				// Toggle switch
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - uiScaled(40));
				float toggleHeight = settings.display.uiScale * 20;
				float verticalOffset = (rowHeight - toggleHeight) * 0.5f;
				ImVec2 cursorPos = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y + verticalOffset));
				RenderToggleSwitchVisual(enabled);

				// Color picker on same line
				ImGui::SameLine(0, uiScaled(16));
				bool colorChanged = ImGui::ColorEdit4("##crosshair_color", xhairColor,
					ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf
					| ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoLabel);

				if (colorChanged)
				{
					if (enabled)
					{
						config::CrosshairColor[bus] = (u8)(std::round(xhairColor[0] * 255.f))
								| ((u8)(std::round(xhairColor[1] * 255.f)) << 8)
								| ((u8)(std::round(xhairColor[2] * 255.f)) << 16)
								| ((u8)(std::round(xhairColor[3] * 255.f)) << 24);
						if (config::CrosshairColor[bus] == 0)
							config::CrosshairColor[bus] = 0xC0FFFFFF;
					}
				}

				is_there_any_xhair |= enabled;
			}

			ImGui::PopItemWidth();
		}
		ImGui::EndTable();
	}

	// Setting 23: Crosshair Size (10-100)
	{
		DisabledScope scope(!is_there_any_xhair);

		// Temp storage for popup slider
		static int crosshairSizeTemp = config::CrosshairSize.get();

			SettingsUI::PopupSliderConfig crosshairConfig {};
			crosshairConfig.label = "Crosshair Size";
			crosshairConfig.icon = ICON_FA_CROSSHAIRS;
			crosshairConfig.tooltip = "Adjust the size of the on-screen crosshair";
			crosshairConfig.popupID = "CrosshairSizePopup";
			crosshairConfig.description = "Adjust the size of the on-screen crosshair for light guns. "
			                             "Only available when a light gun device is connected.";
			crosshairConfig.currentValue = &crosshairSizeTemp;
			crosshairConfig.minValue = 10;
			crosshairConfig.maxValue = 100;
			crosshairConfig.defaultValue = 50;
			crosshairConfig.format = "%d";
			crosshairConfig.applyButtonText = "Apply";
			crosshairConfig.onApply = []() {
				config::CrosshairSize.set(crosshairSizeTemp);
			};
			crosshairConfig.onValueChange = nullptr;
			crosshairConfig.iconSize = uiScaled(20.0f);
			crosshairConfig.iconSpacing = uiScaled(8.0f);
			crosshairConfig.valueWidth = uiScaled(220.0f);
			crosshairConfig.sliderWidth = uiScaled(320.0f);
			crosshairConfig.showApplyFlag = nullptr;
			crosshairConfig.hasPendingChanges = false;

			SettingsUI::PopupConfig crosshairPopupCfg {};
			crosshairPopupCfg.type = SettingsUI::PopupType::Slider;
			crosshairPopupCfg.slider = crosshairConfig;

			RenderGeneralPopupSettingRow(
				"CrosshairSizeSetting",
				"Adjust the size of the on-screen crosshair for light guns. Only available when a light gun device is connected.",
				crosshairPopupCfg,
				nullptr,
				!is_there_any_xhair);
		}

	ImGui::Spacing();
	SectionHeaderWithIcon(ICON_FA_MICROCHIP, "VMU Settings");
		RenderGeneralToggleSettingRow(
			"PerGameVmu",
			ICON_FA_MICROCHIP,
			"Per Game VMU A1",
			"When enabled, each game has its own VMU on port 1 of controller A",
			static_cast<bool>(config::PerGameVmu),
			[](bool enabled) { config::PerGameVmu.set(enabled); },
			"When enabled, each game has its own VMU on port 1 of controller A");

#ifdef USE_DREAMLINK_DEVICES
	{
		RenderGeneralToggleSettingRow(
			"UsePhysicalVmuMemory",
			ICON_FA_MEMORY,
			"Use Physical VMU Memory",
			"Enables direct read/write access to physical VMU memory via DreamPicoPort/DreamConn. This is not compatible with load state events.",
			static_cast<bool>(config::UsePhysicalVmuMemory),
			[](bool enabled) { config::UsePhysicalVmuMemory.set(enabled); },
			"Enables direct read/write access to physical VMU memory via DreamPicoPort/DreamConn. This is not compatible with load state events.",
			game_started);
	}

	for (int bus = 0; bus < MAPLE_PORTS; bus++)
	{
		int port_count = 0;
		switch (config::MapleMainDevices[bus]) {
			case MDT_SegaController:
			case MDT_SegaControllerXL:
				port_count = 2;
				break;
			case MDT_LightGun:
			case MDT_TwinStick:
			case MDT_AsciiStick:
			case MDT_RacingController:
				port_count = 1;
				break;
			default: break;
		}

		if (port_count > 0)
		{
			char rowId[64];
			char rowLabel[64];
			snprintf(rowId, sizeof(rowId), "UseNetworkExpansionDevices%d", bus);
			snprintf(rowLabel, sizeof(rowLabel), "Use Network Expansion Devices (Port %c)", 'A' + bus);
			RenderGeneralToggleSettingRow(
				rowId,
				ICON_FA_NETWORK_WIRED,
				rowLabel,
				"Connect to expansion devices such as VMUs over local TCP",
				static_cast<bool>(config::UseNetworkExpansionDevices[bus]),
				[bus](bool enabled) { config::UseNetworkExpansionDevices[bus].set(enabled); },
				"Connect to expansion devices such as VMUs over local TCP");
		}
	}
#endif
}

void renderNetworkTab()
{
	ScopedTwoLineRowStyle networkRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Network Configuration");
	ImGui::Separator();

	SectionHeaderWithIcon(ICON_FA_GLOBE, "Network Type");

	// Network Type Selection - Options Popup
	{
		static int netType = 0;
		if (config::GGPOEnable)
			netType = 1;
		else if (config::NetworkEnable)
			netType = 2;
		else if (config::BattleCableEnable)
			netType = 3;
		else
			netType = 0;

		static const char* const networkTypeOptions[] = {
			"Disabled",
			"GGPO",
			"Naomi",
			"Battle Cable"
		};

		SettingsUI::PopupConfig networkTypeCfg {};
		networkTypeCfg.type = SettingsUI::PopupType::Options;
		networkTypeCfg.options.label = "Network Type";
		networkTypeCfg.options.icon = ICON_FA_GLOBE;
		networkTypeCfg.options.popupID = "NetworkTypePopup";
		networkTypeCfg.options.options = networkTypeOptions;
		networkTypeCfg.options.optionCount = IM_ARRAYSIZE(networkTypeOptions);
		networkTypeCfg.options.currentValue = &netType;
		networkTypeCfg.options.valueWidth = 220.0f;
		networkTypeCfg.options.onChange = [](int selectedType) {
			config::GGPOEnable = (selectedType == 1);
			config::NetworkEnable = (selectedType == 2);
			config::BattleCableEnable = (selectedType == 3);
			return true;
		};
		RenderGeneralPopupSettingRow(
			"NetworkTypeSetting",
			"Select the active netplay backend.",
			networkTypeCfg);
	}

	// Show configuration section if any network type is enabled
	if (config::GGPOEnable || config::NetworkEnable || config::BattleCableEnable)
	{
		ImGui::Spacing();

		SectionHeaderWithIcon(ICON_FA_SLIDERS, "Configuration");

		// GGPO Settings
		if (config::GGPOEnable)
		{
			config::NetworkEnable = false;

			RenderGeneralToggleSettingRow(
				"PlayAsPlayer1",
				ICON_FA_USER,
				"Play as Player 1",
				"Host the game session",
				static_cast<bool>(config::ActAsServer),
				[](bool enabled) { config::ActAsServer = enabled; },
				"Deselect to play as player 2");

			// Peer Address
			RenderGeneralTextInputSettingRow(
				"GGPONetPeerRow",
				ICON_FA_NETWORK_WIRED,
				"Peer",
				"Your peer IP address and optional port",
				[]() {
					InputText("##PeerInput", &config::NetworkServer.get(),
						ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
						dnsCharFilter);
				});

			// Frame Delay
			{
				static int ggpoDelayTemp = config::GGPODelay.get();

				SettingsUI::PopupSliderConfig ggpoDelayCfg {};
				ggpoDelayCfg.label = "Frame Delay";
				ggpoDelayCfg.icon = ICON_FA_CLOCK;
				ggpoDelayCfg.popupID = "GGPOFrameDelayPopup";
				ggpoDelayCfg.description = "Sets Frame Delay, advisable for sessions with ping >100 ms.";
				ggpoDelayCfg.currentValue = &ggpoDelayTemp;
				ggpoDelayCfg.minValue = 0;
				ggpoDelayCfg.maxValue = 20;
				ggpoDelayCfg.defaultValue = 0;
				ggpoDelayCfg.format = "%d";
				ggpoDelayCfg.valueWidth = 220.0f;
				ggpoDelayCfg.sliderWidth = 320.0f;
				ggpoDelayCfg.onValueChange = []() {
					config::GGPODelay.set(ggpoDelayTemp);
				};

				SettingsUI::PopupConfig ggpoDelayPopupCfg {};
				ggpoDelayPopupCfg.type = SettingsUI::PopupType::Slider;
				ggpoDelayPopupCfg.slider = ggpoDelayCfg;

				RenderGeneralPopupSettingRow(
					"GGPOFrameDelaySetting",
					"Sets frame delay for higher-ping sessions.",
					ggpoDelayPopupCfg);
			}

			// Analog Axes Configuration
			SectionHeaderWithIcon(ICON_FA_GAMEPAD, "Analog Axes");

			{
				static const char* axesOptions[] = { "Disabled", "Horizontal", "Full" };
				int axesSelection = config::GGPOAnalogAxes.get();
				if (axesSelection < 0 || axesSelection > 2)
					axesSelection = 0;

				SettingsUI::PopupConfig axesCfg {};
				axesCfg.type = SettingsUI::PopupType::Options;
				axesCfg.options.label = "Left Thumbstick";
				axesCfg.options.icon = ICON_FA_GAMEPAD;
				axesCfg.options.popupID = "GGPOAnalogAxesPopup";
				axesCfg.options.options = axesOptions;
				axesCfg.options.optionCount = IM_ARRAYSIZE(axesOptions);
				axesCfg.options.currentValue = &axesSelection;
				axesCfg.options.valueWidth = 220.0f;
				axesCfg.options.onChange = [&](int selectedIndex) {
					config::GGPOAnalogAxes.set(selectedIndex);
					return true;
				};

				RenderGeneralPopupSettingRow(
					"GGPOAnalogAxesSetting",
					"Configure left thumbstick usage for GGPO.",
					axesCfg);
			}

			RenderGeneralToggleSettingRow(
				"EnableChat",
				ICON_FA_COMMENTS,
				"Enable Chat",
				"Open chat on message received",
				static_cast<bool>(config::GGPOChat),
				[](bool enabled) { config::GGPOChat.set(enabled); },
				"Open the chat window when a chat message is received");

			if (config::GGPOChat)
			{
				RenderGeneralToggleSettingRow(
					"GGPOChatTimeoutToggle",
					ICON_FA_HOURGLASS,
					"Chat Auto-Close",
					"Automatically close chat window",
					static_cast<bool>(config::GGPOChatTimeoutToggle),
					[](bool enabled) { config::GGPOChatTimeoutToggle = enabled; },
					"Automatically close chat window after timeout");

				if (config::GGPOChatTimeoutToggle)
				{
					ImGui::PushID("GGPOChatTimeoutRow");
					const bool rowActivated = BeginTwoLineSettingRow("##row", "Sets duration that chat window stays open after new message is received");
					const ImVec2 line1Start = BeginTwoLineSettingRowContent();

					SettingIcon(ICON_FA_HOURGLASS, ImVec2(uiScaled(20), uiScaled(20)));
					ImGui::SameLine(0, uiScaled(8));
					ImGui::PushFont(largeFont);
					ImGui::TextUnformatted("Chat Window Timeout (s)");
					ImGui::PopFont();

					const float inputWidth = uiScaled(220.0f);
					ImGui::SameLine(RightColumnX(inputWidth));
					if (rowActivated)
						ImGui::SetKeyboardFocusHere();
					ImGui::SetNextItemWidth(inputWidth);
					char chatTimeout[256];
					snprintf(chatTimeout, sizeof(chatTimeout), "%d", (int)config::GGPOChatTimeout);
					if (InputText("##ChatTimeoutInput", chatTimeout, sizeof(chatTimeout), ImGuiInputTextFlags_CharsDecimal))
						config::GGPOChatTimeout.set(atoi(chatTimeout));

					RenderTwoLineSettingDescription(line1Start, "Sets duration that chat window stays open after new message is received");
					ImGui::PopID();
					ImGui::Spacing();
					if (g_twoLineRowExtraGapPx > 0.0f)
						ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
				}
			}

			RenderGeneralToggleSettingRow(
				"NetworkStats",
				ICON_FA_CHART_BAR,
				"Network Statistics",
				"Display netplay stats on screen",
				static_cast<bool>(config::NetworkStats),
				[](bool enabled) { config::NetworkStats = enabled; },
				"Display network statistics on screen");
		}
		// Naomi Network Settings
		else if (config::NetworkEnable)
		{
			RenderGeneralToggleSettingRow(
				"ActAsServer",
				ICON_FA_SERVER,
				"Act as Server",
				"Host Naomi network game",
				static_cast<bool>(config::ActAsServer),
				[](bool enabled) { config::ActAsServer = enabled; },
				"Create a local server for Naomi network games");

			// Server Address (only when not acting as server)
			if (!config::ActAsServer)
			{
				RenderGeneralTextInputSettingRow(
					"NaomiServerRow",
					ICON_FA_NETWORK_WIRED,
					"Server",
					"The server to connect to. Leave blank to find a server automatically on the default port",
					[]() {
						InputText("##ServerInput", &config::NetworkServer.get(),
							ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
							dnsCharFilter);
					});
			}

			// Local Port
			{
				ImGui::PushID("NaomiLocalPortRow");
				const bool rowActivated = BeginTwoLineSettingRow("##row", "The local UDP port to use");
				const ImVec2 line1Start = BeginTwoLineSettingRowContent();

				SettingIcon(ICON_FA_KEY, ImVec2(uiScaled(20), uiScaled(20)));
				ImGui::SameLine(0, uiScaled(8));
				ImGui::PushFont(largeFont);
				ImGui::TextUnformatted("Local Port");
				ImGui::PopFont();

				const float inputWidth = uiScaled(220.0f);
				ImGui::SameLine(RightColumnX(inputWidth));
				if (rowActivated)
					ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(inputWidth);
				char localPort[256];
				snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
				if (InputText("##LocalPortInput", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal))
					config::LocalPort.set(atoi(localPort));

				RenderTwoLineSettingDescription(line1Start, "The local UDP port to use");
				ImGui::PopID();
				ImGui::Spacing();
				if (g_twoLineRowExtraGapPx > 0.0f)
					ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
			}
		}
		// Battle Cable Settings
		else if (config::BattleCableEnable)
		{
#ifdef USE_ICE
			if (ImGui::BeginTabBar("battleMode", ImGuiTabBarFlags_NoTooltip))
			{
				// Match Code Tab
				char matchCodeTabLabel[128];
				snprintf(matchCodeTabLabel, sizeof(matchCodeTabLabel), "%s Match Code", ICON_FA_TAG);
				if (ImGui::BeginTabItem(matchCodeTabLabel))
				{
					ice::State state = ice::getState();
					ImGuiInputTextFlags textFlags = state == ice::Offline ?
						ImGuiInputTextFlags_CharsNoBlank : ImGuiInputTextFlags_ReadOnly;
					static std::string matchCode;
					InputText("Code", &matchCode, textFlags);
					ImGui::SameLine();
					ShowHelpMarker("Choose a unique word or number and share it with your opponent");

					if (state == ice::Offline)
					{
						if (ImGui::Button("Connect") && !matchCode.empty())
							ice::init(matchCode, true);
					}
					else
					{
						if (ImGui::Button("Disconnect"))
							try { ice::term(); } catch (...) {}
					}

					// Display connection status
					std::string status;
					switch (state)
					{
					case ice::Offline:
						status = ice::getStatusText();
						break;
					case ice::Online:
						status = "Waiting at meeting point...";
						break;
					case ice::ChalAccepted:
						status = "Preparing game...";
						break;
					case ice::Playing:
						status = "Playing " + matchCode + " (" + ice::getStatusText() + ")";
						break;
					default:
						break;
					}
					ImGui::TextDisabled("%s", status.c_str());

					RenderGeneralToggleSettingRow(
						"NetworkStatsBattleCable",
						ICON_FA_CHART_BAR,
						"Network Stats",
						"Display network statistics",
						static_cast<bool>(config::NetworkStats),
						[](bool enabled) { config::NetworkStats = enabled; },
						"Display network statistics on screen");

					ImGui::EndTabItem();
				}

				// Manual Tab
				if (ImGui::BeginTabItem("Manual"))
				{
#endif
					RenderGeneralTextInputSettingRow(
						"BattlePeerRow",
						ICON_FA_NETWORK_WIRED,
						"Peer",
						"The peer to connect to. Leave blank to find a player automatically on the default port",
						[]() {
							InputText("##BattlePeerInput", &config::NetworkServer.get(),
								ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
								dnsCharFilter);
						});

					{
						ImGui::PushID("BattleLocalPortRow");
						const bool rowActivated = BeginTwoLineSettingRow("##row", "The local UDP port to use");
						const ImVec2 line1Start = BeginTwoLineSettingRowContent();

						SettingIcon(ICON_FA_KEY, ImVec2(uiScaled(20), uiScaled(20)));
						ImGui::SameLine(0, uiScaled(8));
						ImGui::PushFont(largeFont);
						ImGui::TextUnformatted("Local Port");
						ImGui::PopFont();

						const float inputWidth = uiScaled(220.0f);
						ImGui::SameLine(RightColumnX(inputWidth));
						if (rowActivated)
							ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(inputWidth);
						char localPort[256];
						snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
						if (InputText("##BattleLocalPortInput", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal))
							config::LocalPort.set(atoi(localPort));

						RenderTwoLineSettingDescription(line1Start, "The local UDP port to use");
						ImGui::PopID();
						ImGui::Spacing();
						if (g_twoLineRowExtraGapPx > 0.0f)
							ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
					}
#ifdef USE_ICE
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
#endif

			RenderGeneralToggleSettingRow(
				"ActAsMaster",
				ICON_FA_CROWN,
				"Act as Master",
				"Host for Maximum Speed mode",
				static_cast<bool>(config::ActAsServer),
				[](bool enabled) { config::ActAsServer = enabled; },
				"Only used for Maximum Speed. One of the peers must be master");
		}
	}

	ImGui::Spacing();

	SectionHeaderWithIcon(ICON_FA_GEAR, "Network Options");

	RenderGeneralToggleSettingRow(
		"EnableUPnP",
		ICON_FA_NETWORK_WIRED,
		"Enable UPnP",
		"Automatically configure your network router for netplay",
		static_cast<bool>(config::EnableUPnP),
		[](bool enabled) { config::EnableUPnP.set(enabled); });

	RenderGeneralToggleSettingRow(
		"BroadcastOutputs",
		ICON_FA_TOWER_BROADCAST,
		"Broadcast Digital Outputs",
		"Broadcast audio/video",
		static_cast<bool>(config::NetworkOutput),
		[](bool enabled) { config::NetworkOutput.set(enabled); });

	RenderGeneralToggleSettingRow(
		"BroadbandAdapter",
		ICON_FA_SERVER,
		"Broadband Adapter",
		"Emulate the Ethernet Broadband Adapter (BBA) instead of the Modem",
		static_cast<bool>(config::EmulateBBA),
		[](bool enabled) { config::EmulateBBA.set(enabled); },
		nullptr,
		game_started);

	RenderGeneralToggleSettingRow(
		"DCNet",
		ICON_FA_CLOUD,
		"DCNet Cloud",
		"Dreamcast Internet via cloud",
		static_cast<bool>(config::UseDCNet),
		[](bool enabled) { config::UseDCNet.set(enabled); });

	// ISP Username
	std::string& ispUsername = config::ISPUsername.get();
	RenderGeneralTextInputSettingRow(
		"ISPUsernameRow",
		ICON_FA_USER,
		"ISP Username",
		"The ISP user name stored in the console Flash RAM. Used by some online games as the player name. Leave blank to keep the current Flash RAM value",
		[&]() {
			InputText("##ISPUsernameInput", &ispUsername,
				ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
				[](ImGuiInputTextCallbackData* data) {
					return static_cast<int>(data->EventChar <= ' ' || data->EventChar > '~');
				});
			auto it = std::remove_if(ispUsername.begin(), ispUsername.end(),
				[](char c) { return c <= ' ' || c > '~'; });
			ispUsername.erase(it, ispUsername.end());
		});

#if !defined(NDEBUG) || defined(DEBUGFAST)
	// DNS Server (debug builds only)
	RenderGeneralTextInputSettingRow(
		"DNSServerRow",
		ICON_FA_ADDRESS_BOOK,
		"DNS Server",
		"DNS server name or IP address",
		[]() {
			InputText("##DNSInput", &config::DNS.get(),
				ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
				dnsCharFilter);
		},
		nullptr,
		config::UseDCNet);
#endif

#ifdef NAOMI_MULTIBOARD
	// Multiboard Screens
	ImGui::Spacing();

	SectionHeaderWithIcon(ICON_FA_TABLE, "Multiboard");

	OptionRadioButton<int>("1 (Twin)", config::MultiboardSlaves, 1,
		"One screen configuration (F355 Twin)");
	ImGui::SameLine();
	OptionRadioButton<int>("3 (Deluxe)", config::MultiboardSlaves, 2,
		"Three screens configuration");
#endif
}

void renderAdvancedTab()
{
	ScopedTwoLineRowStyle advancedRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Advanced Configuration");
	ImGui::Separator();

	// CPU & Emulation Section
	SectionHeaderWithIcon(ICON_FA_MICROCHIP, "CPU & Emulation");
	{
#if FEAT_SHREC != DYNAREC_NONE
		// CPU Mode
		{
			static const char* cpuModeLabels[] = { "Dynarec", "Interpreter" };
			int cpuModeSelection = config::DynarecEnabled.get() ? 0 : 1;

			SettingsUI::PopupConfig cpuModeCfg {};
			cpuModeCfg.type = SettingsUI::PopupType::Options;
			cpuModeCfg.options.label = "CPU Mode";
			cpuModeCfg.options.icon = ICON_FA_MICROCHIP;
			cpuModeCfg.options.popupID = "CpuModePopup";
			cpuModeCfg.options.options = cpuModeLabels;
			cpuModeCfg.options.optionCount = IM_ARRAYSIZE(cpuModeLabels);
			cpuModeCfg.options.currentValue = &cpuModeSelection;
			cpuModeCfg.options.valueWidth = 220.0f;
			cpuModeCfg.options.onChange = [&](int selectedIndex) {
				config::DynarecEnabled.set(selectedIndex == 0);
				return true;
			};

			RenderGeneralPopupSettingRow(
				"CpuModeSetting",
				"Use the dynamic recompiler for speed, or interpreter for compatibility.",
				cpuModeCfg,
				"Use the dynamic recompiler for speed, or interpreter for compatibility.");
		}

		// SH4 Clock
		{
			static int sh4ClockTemp = config::Sh4Clock.get();

			SettingsUI::PopupSliderConfig sh4ClockCfg {};
			sh4ClockCfg.label = "SH4 Clock";
			sh4ClockCfg.icon = ICON_FA_GAUGE_HIGH;
			sh4ClockCfg.popupID = "Sh4ClockPopup";
			sh4ClockCfg.description = "Over/Underclock the main SH4 CPU. Default is 200 MHz. "
			                          "Other values may crash, freeze or trigger unexpected nuclear reactions.";
			sh4ClockCfg.currentValue = &sh4ClockTemp;
			sh4ClockCfg.minValue = 100;
			sh4ClockCfg.maxValue = 300;
			sh4ClockCfg.defaultValue = 200;
			sh4ClockCfg.format = "%d MHz";
			sh4ClockCfg.valueWidth = 220.0f;
			sh4ClockCfg.sliderWidth = 320.0f;
			sh4ClockCfg.onValueChange = []() {
				config::Sh4Clock.set(sh4ClockTemp);
			};

			SettingsUI::PopupConfig sh4ClockPopupCfg {};
			sh4ClockPopupCfg.type = SettingsUI::PopupType::Slider;
			sh4ClockPopupCfg.slider = sh4ClockCfg;

			RenderGeneralPopupSettingRow(
				"Sh4ClockSetting",
				"Over/Underclock the main SH4 CPU.",
				sh4ClockPopupCfg);
		}
#endif

		// HLE BIOS
		RenderGeneralToggleSettingRow(
			"HLEBIOS",
			ICON_FA_MICROCHIP,
			"HLE BIOS",
			"High Level Emulation BIOS",
			static_cast<bool>(config::UseReios),
			[](bool enabled) { config::UseReios.set(enabled); },
			"Use High Level Emulation BIOS. Saves CPU power, but less accurate");

		// Multi-threaded emulation
		RenderGeneralToggleSettingRow(
			"MultiThreaded",
			ICON_FA_GEAR,
			"Multi-threading",
			"Multi-threaded CPU emulation",
			static_cast<bool>(config::ThreadedRendering),
			[](bool enabled) { config::ThreadedRendering.set(enabled); },
			"Use multi-threading for CPU emulation. May improve performance on modern CPUs");

		ImGui::Spacing();

		// Dreamcast 32MB RAM Mod (with warning)
		RenderGeneralToggleSettingRow(
			"ExtraRAM",
			ICON_FA_SD_CARD,
			"32MB RAM",
			"Extended memory (32MB)",
			static_cast<bool>(config::RamMod32MB),
			[](bool enabled) { config::RamMod32MB.set(enabled); },
			"Enable 32MB RAM extension. May break games or cause glitches",
			game_started);
	}

	// Debugging Section
#if defined(GDB_SERVER) || !defined(__ANDROID__)
	SectionHeaderWithIcon(ICON_FA_BUG, "Debugging");
	{
#if !defined(__ANDROID) && !defined(GDB_SERVER)
		// Serial Console - 2x Row Pattern
		RenderGeneralToggleSettingRow(
			"SerialConsole",
			ICON_FA_TERMINAL,
			"Serial Console",
			"Serial console output",
			static_cast<bool>(config::SerialConsole),
			[](bool enabled) { config::SerialConsole.set(enabled); },
			"Enable serial console output");
#endif

#ifdef GDB_SERVER
		// GDB Server - 2x Row
		RenderGeneralToggleSettingRow(
			"GDBWait",
			ICON_FA_BUG,
			"GDB Server",
			"GDB debug server",
			static_cast<bool>(config::GDBWait),
			[](bool enabled) { config::GDBWait.set(enabled); },
			"Enable GDB server and wait for connection");

		// GDB Server options (shown when enabled)
		if (config::GDBWait.get())
		{
			ImGui::Indent();

			// Wait for connection - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"GDBWaitForConnection",
				ICON_FA_LINK,
				"Wait for Connection",
				"Wait for debugger to connect",
				static_cast<bool>(config::GDBWaitForConnection),
				[](bool enabled) { config::GDBWaitForConnection.set(enabled); },
				"Start emulation once the debugger is connected");

			{
				ImGui::PushID("GDBPortRow");
				const bool rowActivated = BeginTwoLineSettingRow("##row", "Default port is 3263");
				const ImVec2 line1Start = BeginTwoLineSettingRowContent();

				SettingIcon(ICON_FA_LINK, ImVec2(uiScaled(20), uiScaled(20)));
				ImGui::SameLine(0, uiScaled(8));
				ImGui::PushFont(largeFont);
				ImGui::TextUnformatted("GDB Port");
				ImGui::PopFont();

				const float inputWidth = uiScaled(220.0f);
				ImGui::SameLine(RightColumnX(inputWidth));
				if (rowActivated)
					ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(inputWidth);
				static int gdbport = config::GDBPort;
				if (ImGui::InputInt("##GDBPortInput", &gdbport))
					config::GDBPort = gdbport;

				RenderTwoLineSettingDescription(line1Start, "Default port is 3263");
				ImGui::PopID();
				ImGui::Spacing();
				if (g_twoLineRowExtraGapPx > 0.0f)
					ImGui::Dummy(ImVec2(0.0f, uiScaled(g_twoLineRowExtraGapPx)));
			}
			ImGui::Unindent();
		}
#endif

		// Log to File - 2x Row Pattern
		const bool logToFileValue = cfgLoadBool("log", "LogToFile", false);
		RenderGeneralToggleSettingRow(
			"LogToFile",
			ICON_FA_FILE,
			"Log to File",
			"Save log output to file",
			logToFileValue,
			[](bool enabled) { cfgSaveBool("log", "LogToFile", enabled); },
			"Save log output to file");
	}
#endif

	// Logging Section (Debug builds only)
#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER
	SectionHeaderWithIcon(ICON_FA_FILE_LINES, "Logging");
	{
		LogManager *logManager = LogManager::GetInstance();

		// Log Verbosity
		static const char *levels[] = { "Notice", "Error", "Warning", "Info", "Debug" };
		{
			int logVerbositySelection = logManager->GetLogLevel() - 1;
			if (logVerbositySelection < 0 || logVerbositySelection >= (int)std::size(levels))
				logVerbositySelection = 0;

			SettingsUI::PopupConfig logVerbosityCfg {};
			logVerbosityCfg.type = SettingsUI::PopupType::Options;
			logVerbosityCfg.options.label = "Log Verbosity";
			logVerbosityCfg.options.icon = ICON_FA_SORT;
			logVerbosityCfg.options.popupID = "LogVerbosityPopup";
			logVerbosityCfg.options.options = levels;
			logVerbosityCfg.options.optionCount = (int)std::size(levels);
			logVerbosityCfg.options.currentValue = &logVerbositySelection;
			logVerbosityCfg.options.valueWidth = 220.0f;
			logVerbosityCfg.options.onChange = [&](int selectedIndex) {
				logManager->SetLogLevel((LogTypes::LOG_LEVELS)(selectedIndex + 1));
				cfgSaveInt("log", "Verbosity", selectedIndex + 1);
				return true;
			};

			RenderGeneralPopupSettingRow(
				"LogVerbositySetting",
				"Control the verbosity of log output",
				logVerbosityCfg);
		}

		// Log Categories (collapsible with 2-column toggle grid)
		if (ImGui::CollapsingHeader("Log Categories"))
		{
			ImGui::Indent();
			int columnCount = 0;
			for (LogTypes::LOG_TYPE type = LogTypes::AICA; type < LogTypes::NUMBER_OF_LOGS; type = (LogTypes::LOG_TYPE)(type + 1))
			{
				if (columnCount % 2 == 0)
					ImGui::Columns(2, "LogCategories", false);

				bool enabled = logManager->IsEnabled(type, logManager->GetLogLevel());
				std::string shortName = logManager->GetShortName(type);
				std::string fullName = logManager->GetFullName(type);

				// Use 2-row toggle pattern for each category
				ImGui::PushID(shortName.c_str());
				float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;

				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
				bool rowClicked = ImGui::Selectable(("##" + shortName + "_row").c_str(), false,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap,
					ImVec2(0, rowHeight));
				ImGui::PopStyleVar();

				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(fullName.c_str());

				if (rowClicked && logManager->GetLogLevel() > LogTypes::LWARNING)
				{
					enabled = !enabled;
					logManager->SetEnable(type, enabled);
					cfgSaveBool("log", shortName.c_str(), enabled);
				}

				ImGui::SameLine(0, 0);
				ImVec2 line1Start = ImGui::GetCursorPos();

				ImGui::PushFont(largeFont);
				ImGui::Text(shortName.c_str());
				ImGui::PopFont();

				float toggleWidth = uiScaled(50);
				ImGui::SameLine(RightColumnX(toggleWidth));

				float toggleHeight = settings.display.uiScale * 24;
				float verticalOffset = (rowHeight - toggleHeight) * 0.5f;
				ImVec2 cursorPos = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y + verticalOffset));

				RenderToggleSwitchVisual(enabled);

				ImGui::SetCursorPos(ImVec2(line1Start.x + uiScaled(28), line1Start.y + ImGui::GetTextLineHeightWithSpacing()));
				ImGui::TextDisabled(fullName.c_str());

				ImGui::PopID();

				columnCount++;
				if (columnCount % 2 == 0)
				{
					ImGui::Columns(1);
					ImGui::Spacing();
				}
			}
			if (columnCount % 2 != 0)
				ImGui::Columns(1);
			ImGui::Unindent();
		}

		// Log Server (network logging)
		SettingIcon(ICON_FA_SERVER, ImVec2(uiScaled(20), uiScaled(20)));
		ImGui::SameLine(0, uiScaled(8));
		ImGui::PushFont(largeFont);
		ImGui::Text("Log Server");
		ImGui::PopFont();
		ImGui::SameLine();
			InputText("", &config::LogServer.get(),
				ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
				dnsCharFilter);
		ImGui::SameLine();
		ShowHelpMarker("Log to this hostname[:port] with UDP. Default port is 31667.");
	}
#endif

	// Profiling Section
#if FC_PROFILER
	ImGui::Spacing();
	SectionHeaderWithIcon(ICON_FA_CHART_LINE, "Profiling");
	{
		RenderGeneralToggleSettingRow(
			"ProfilerEnabled",
			ICON_FA_CHART_LINE,
			"Enable Profiler",
			"Performance profiling",
			static_cast<bool>(config::ProfilerEnabled),
			[](bool enabled) { config::ProfilerEnabled.set(enabled); },
			"Enable performance profiling");
		if (config::ProfilerEnabled)
		{
			ImGui::Indent();

			// Display in GUI - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"ProfilerDrawToGUI",
				ICON_FA_DESKTOP,
				"Display in GUI",
				"Show profiler overlay",
				static_cast<bool>(config::ProfilerDrawToGUI),
				[](bool enabled) { config::ProfilerDrawToGUI.set(enabled); },
				"Draw the profiler output in an overlay");

			// Output to terminal - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"ProfilerOutputTTY",
				ICON_FA_TERMINAL,
				"Output to Terminal",
				"Write profiler to terminal",
				static_cast<bool>(config::ProfilerOutputTTY),
				[](bool enabled) { config::ProfilerOutputTTY.set(enabled); },
				"Write the profiler output to the terminal");

			ImGui::Unindent();
		}
	}
#endif

	// Experimental Features Section (with warnings)
	ImGui::Spacing();
	header("Experimental Features");
	{
		// Texture Dumping (with warning)
		ImVec4 warningColor = ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered];
		ImGui::TextColored(warningColor, "(!) Texture Dumping");
		ImGui::Indent();
		RenderGeneralToggleSettingRow(
			"DumpTextures",
			ICON_FA_DOWNLOAD,
			"Texture Dumping",
			"Dump textures to files",
			static_cast<bool>(config::DumpTextures),
			[](bool enabled) { config::DumpTextures.set(enabled); },
			"Dump textures to files for inspection");
		if (config::DumpTextures)
		{
			ImGui::Indent();

			// Dump Replaced Textures - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"DumpReplacedTextures",
				ICON_FA_COPY,
				"Dump Replaced",
				"Dump replaced textures too",
				static_cast<bool>(config::DumpReplacedTextures),
				[](bool enabled) { config::DumpReplacedTextures.set(enabled); },
				"Always dump textures that are already replaced by custom textures");

			ImGui::Unindent();
		}
		ImGui::Unindent();
	}

	// Developer Tools Section
	ImGui::Spacing();
	header("Developer Tools");
	{
#ifdef USE_LUA
		// Lua Scripting
		ImGui::Text("Lua Scripting");
		InputText("Lua Filename", &config::LuaFileName.get(), ImGuiInputTextFlags_CharsNoBlank);
		ImGui::SameLine();
		ShowHelpMarker("Specify lua filename to use. Should be located in Flycast config folder. Defaults to flycast.lua when empty.");
#endif

#ifdef SENTRY_UPLOAD
		// Crash Reporting - 2x Row Pattern
		RenderGeneralToggleSettingRow(
			"CrashReporting",
			ICON_FA_STETHOSCOPE,
			"Crash Reports",
			"Auto-upload crash reports",
			static_cast<bool>(config::UploadCrashLogs),
			[](bool enabled) { config::UploadCrashLogs.set(enabled); },
			"Automatically upload crash reports to sentry.io to help in troubleshooting. No personal information is included.");
#endif

#if defined(GDB_SERVER)
		// Virtual Memory Addresses (debug builds)
		if (config::GDB)
		{
			ImGui::Spacing();
			ImGui::Text("Virtual Memory Addresses");
			ImGui::Separator();
			void *ram_base, *ram, *vram, *aram;
			addrspace::getAddress(&ram_base, &ram, &vram, &aram);

			ImGui::Text("Base Address: %p", ram_base);

			if (ram == nullptr) {
				ImVec4 gray = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
				ImGui::TextColored(gray, "RAM addresses are not available until the emulation is started");
			} else {
				ImGui::Columns(3, "virtualMemoryAddress", false);
				ImGui::Text("RAM: %p", ram);
				ImGui::NextColumn();
				ImGui::Text("VRAM64: %p", vram);
				ImGui::NextColumn();
				ImGui::Text("ARAM: %p", aram);
				ImGui::Columns(1, nullptr, false);
			}
		}
#endif
	}
}

void renderAboutTab()
{
	// Use TextDisabled for the title (theme-aware)
	ImGui::TextDisabled("About Hollycast");
	ImGui::Separator();

	// Center content for better appearance
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));

	// Logo/Title Section
	ImGui::Spacing();
	ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
	const char* logoText = "  ____  __  __          _   _ ";
	const char* logoText2 = " / ___||  \\/  | ___  __| | | |";
	const char* logoText3 = " \\___ \\| |\\/| |/ _ \\/ _` | | |";
	const char* logoText4 = "  ___) | |  | |  __/ (_| | |_|";
	const char* logoText5 = " |____/|_|  |_|\\___|\\__,_|\\___/";

	ImGui::TextUnformatted(logoText);
	ImGui::TextUnformatted(logoText2);
	ImGui::TextUnformatted(logoText3);
	ImGui::TextUnformatted(logoText4);
	ImGui::TextUnformatted(logoText5);
	ImGui::PopStyleVar();

	ImGui::Spacing();
	ImGui::Spacing();

	// Version Information
	ImGui::TextDisabled("%s Version Information", ICON_FA_TAG);
	ImGui::Separator();
	ImGui::Text("Version: %s", GIT_VERSION);
	ImGui::Text("Git Hash: %s", GIT_HASH);
	ImGui::Text("Build Date: %s", BUILD_DATE);
	ImGui::Spacing();

	// Platform Information
	ImGui::TextDisabled("%s Platform", ICON_FA_COMPUTER);
	ImGui::Separator();
	ImGui::Text("CPU: %s",
#if HOST_CPU == CPU_X86
		"x86"
#elif HOST_CPU == CPU_ARM
		"ARM"
#elif HOST_CPU == CPU_X64
		"x86_64"
#elif HOST_CPU == CPU_ARM64
		"ARM64"
#else
		"Unknown"
#endif
	);
	ImGui::Text("Operating System: %s",
#ifdef __ANDROID__
		"Android"
#elif defined(__unix__)
		"Linux"
#elif defined(__APPLE__)
#ifdef TARGET_IPHONE
		"iOS"
#else
		"macOS"
#endif
#elif defined(TARGET_UWP)
		"Windows Universal Platform"
#elif defined(_WIN32)
		"Windows"
#elif defined(__SWITCH__)
		"Nintendo Switch"
#else
		"Unknown"
#endif
	);
#ifdef TARGET_IPHONE
	extern const char *getIosJitStatus();
	ImGui::Text("JIT Status: %s", getIosJitStatus());
#endif
	ImGui::Spacing();

	// Graphics Information
	if (isOpenGL(config::RendererType))
		ImGui::TextDisabled("%s Graphics - OpenGL", ICON_FA_DISPLAY);
	else if (isVulkan(config::RendererType))
		ImGui::TextDisabled("%s Graphics - Vulkan", ICON_FA_DISPLAY);
	else if (isDirectX(config::RendererType))
		ImGui::TextDisabled("%s Graphics - DirectX", ICON_FA_DISPLAY);
	ImGui::Separator();
	ImGui::Text("Driver: %s", GraphicsContext::Instance()->getDriverName().c_str());
	ImGui::Text("Version: %s", GraphicsContext::Instance()->getDriverVersion().c_str());

#if defined(__ANDROID__) && HOST_CPU == CPU_ARM64 && USE_VULKAN
	if (isVulkan(config::RendererType))
	{
		ImGui::Spacing();
		if (config::CustomGpuDriver)
		{
			std::string name, description, vendor, version;
			if (getCustomGpuDriverInfo(name, description, vendor, version))
			{
				ImGui::Text("Custom Driver:");
				ImGui::Indent();
				ImGui::Text("%s - %s", name.c_str(), description.c_str());
				ImGui::Text("%s - %s", vendor.c_str(), version.c_str());
				ImGui::Unindent();
			}
		}
	}
#endif
	ImGui::Spacing();

	// Project Description
	ImGui::TextDisabled("About");
	ImGui::Separator();
	ImGui::TextWrapped(
		"Hollycast is a multi-platform emulator for Sega Dreamcast, "
		"Naomi, Naomi 2, and Atomiswave arcade systems."
	);
	ImGui::TextWrapped(
		"Based on Flycast, providing accurate emulation with enhancements "
		"for modern systems."
	);
	ImGui::Spacing();

	// Links Section
	ImGui::TextDisabled("%s Links", ICON_FA_LINK);
	ImGui::Separator();

	// Website (colored text to look like a link)
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
	ImGui::Text("Website");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::TextDisabled("https://flycast-emu.com/");

	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
	ImGui::Text("Source Code");
	ImGui::PopStyleColor();
	ImGui::SameLine();
	ImGui::TextDisabled("https://github.com/flycast-emu/flycast");
	ImGui::Spacing();

	// License Information
	ImGui::TextDisabled("%s License", ICON_FA_FILE_CONTRACT);
	ImGui::Separator();
	ImGui::TextWrapped(
		"Copyright (C) 2019-2025 flyinghead and contributors"
	);
	ImGui::TextWrapped(
		"Hollycast/Flycast is free software; you can redistribute it and/or modify "
		"it under the terms of the GNU General Public License as published by "
		"the Free Software Foundation; either version 2 of the License, or "
		"(at your option) any later version."
	);
	ImGui::Spacing();

	// Credits
	ImGui::TextDisabled("%s Credits", ICON_FA_USERS);
	ImGui::Separator();
	ImGui::TextWrapped(
		"This emulator is based on the work of many talented developers "
		"who have contributed to the Flycast project and its dependencies."
	);
	ImGui::TextWrapped(
		"Special thanks to the original Flycast team and all contributors "
		"who make this project possible."
	);

	ImGui::PopStyleVar();
}

// Main entry point - renders the entire settings UI
void renderSettingsNew()
{
	// Set up full-screen window
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);

	// Main settings window
	if (ImGui::Begin("Settings", NULL, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse))
	{
		// Add exit button in top-right corner
		ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);

			// Exit button
			if (ImGui::Button("Exit", ImVec2(100, 30)))
			{
				if (g_mapleDevicesChangedInSettings)
				{
					g_mapleDevicesChangedInSettings = false;
					if (game_started && settings.platform.isConsole())
					{
#if defined(USE_DREAMLINK_DEVICES)
						reconnectDreamLinks();
#endif
						maple_ReconnectDevices();
						reset_vmus();
					}
				}

				// Save settings before exiting
				SaveSettings();

			// Return to appropriate screen based on game state
			if (game_started)
				gui_setState(GuiState::Commands);  // Return to in-game menu
			else
				gui_setState(GuiState::Main);      // Return to main menu
		}

		// Split into left navigation rail and right content area
		renderNavigationRail();
		ImGui::SameLine();
		renderContentArea();
	}
	ImGui::End();
}

} // namespace SettingsNew
