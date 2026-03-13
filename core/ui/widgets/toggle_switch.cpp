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
#include "toggle_switch.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>

namespace Widgets {

// Global default style (can be customized)
static ToggleStyle g_defaultStyle;
static float g_labelColumnWidth = 0.0f;

// Get theme-aware toggle colors based on current ImGui style
static ToggleStyle GetThemeAwareToggleStyle()
{
	ToggleStyle style;

	ImGuiStyle& guiStyle = ImGui::GetStyle();

	// Get colors from current theme
	ImVec4 accentColor = guiStyle.Colors[ImGuiCol_ButtonActive];
	ImVec4 frameColor = guiStyle.Colors[ImGuiCol_FrameBg];
	ImVec4 frameHoverColor = guiStyle.Colors[ImGuiCol_FrameBgHovered];
	ImVec4 textColor = guiStyle.Colors[ImGuiCol_Text];

	// ON state: use accent color from theme (ButtonActive)
	style.colorOn = ImGui::ColorConvertFloat4ToU32(accentColor);

	// Hover variation of ON state - slightly brighter
	ImVec4 onHover = accentColor;
	onHover.x = std::min(1.0f, onHover.x + 0.08f);
	onHover.y = std::min(1.0f, onHover.y + 0.08f);
	onHover.z = std::min(1.0f, onHover.z + 0.08f);
	style.colorOnHover = ImGui::ColorConvertFloat4ToU32(onHover);

	// OFF state: use FrameBg color
	style.colorOff = ImGui::ColorConvertFloat4ToU32(frameColor);
	style.colorOffHover = ImGui::ColorConvertFloat4ToU32(frameHoverColor);

	// Disabled state - dimmed version of FrameBg
	ImVec4 disabledColor = frameColor;
	disabledColor.w = 0.5f;
	style.colorDisabled = ImGui::ColorConvertFloat4ToU32(disabledColor);

	// Knob color: use text color for contrast (dark on light themes, light on dark themes)
	style.knobColor = ImGui::ColorConvertFloat4ToU32(textColor);

	return style;
}

ToggleStyle& GetDefaultToggleStyle()
{
	return g_defaultStyle;
}

void BeginToggleColumn(float labelWidth)
{
	g_labelColumnWidth = labelWidth * settings.display.uiScale;
}

void EndToggleColumn()
{
	g_labelColumnWidth = 0.0f;
}

bool ToggleSwitch(const char* label, bool* value, const char* tooltip,
                  bool disabled, const ToggleStyle* stylePtr)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(label);

	// Use provided style or get theme-aware default
	// When no custom style is provided, we compute colors from current ImGui theme
	const ToggleStyle& style = stylePtr ? *stylePtr : GetThemeAwareToggleStyle();

	// Scale dimensions by UI scale
	const float height = style.height * settings.display.uiScale;
	const float width = style.width * settings.display.uiScale;
	const float radius = (style.knobRadius * settings.display.uiScale);
	const ImVec2 size(width, height);
	const ImVec2 pos = window->DC.CursorPos;

	// Handle alignment
	ImVec2 finalPos = pos;
	if (g_labelColumnWidth > 0.0f && style.alignRight)
	{
		// Align to right of the label column
		finalPos.x = pos.x + std::max(0.0f, g_labelColumnWidth - width - style.rightMargin * settings.display.uiScale);
	}

	const ImRect bb(finalPos, ImVec2(finalPos.x + size.x, finalPos.y + size.y));
	ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	// Button behavior (disabled if needed)
	bool hovered, held;
	bool pressed = false;
	if (!disabled)
	{
		pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
	}
	else
	{
		hovered = false;
		held = false;
	}

	// Toggle value on press
	if (pressed)
	{
		*value = !*value;
	}

	// Smooth animation using linear interpolation
	float anim = *value ? 1.0f : 0.0f;
	if (g.LastActiveId == id && !disabled)
	{
		// Animate transition
		float t = ImMin((float)(g.Time - g.LastActiveIdTimer) / style.animationSpeed, 1.0f);
		// Use smooth easing for more natural feel
		t = t * t * (3.0f - 2.0f * t); // Smoothstep
		anim = *value ? t : (1.0f - t);
	}

	// Determine background color based on state
	ImU32 bgColor;
	if (disabled)
	{
		bgColor = style.colorDisabled;
	}
	else if (*value)
	{
		bgColor = hovered ? style.colorOnHover : style.colorOn;
	}
	else
	{
		bgColor = hovered ? style.colorOffHover : style.colorOff;
	}

	// Render toggle background (rounded rectangle)
	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddRectFilled(bb.Min, bb.Max, bgColor, height * 0.5f);

	// Render knob (uses theme's text color for contrast)
	ImVec2 knobPos;
	knobPos.x = ImLerp(bb.Min.x + radius, bb.Max.x - radius, anim);
	knobPos.y = bb.Min.y + radius;
	const float knobRadius = (style.knobSize * 0.5f) * settings.display.uiScale;

	// Determine theme brightness for shadow color
	ImGuiStyle& guiStyle = ImGui::GetStyle();
	float bgLuminance = (guiStyle.Colors[ImGuiCol_WindowBg].x
	                   + guiStyle.Colors[ImGuiCol_WindowBg].y
	                   + guiStyle.Colors[ImGuiCol_WindowBg].z) / 3.0f;
	bool isLightTheme = bgLuminance > 0.5f;

	// Add subtle shadow to knob for depth
	// Use white shadow on light themes, black shadow on dark themes
	ImVec2 shadowOffset = ImVec2(0.0f, 1.0f * settings.display.uiScale);
	ImU32 shadowColor = isLightTheme
		? IM_COL32(255, 255, 255, 80)   // Light shadow for light themes
		: IM_COL32(0, 0, 0, 50);        // Dark shadow for dark themes
	draw_list->AddCircleFilled(knobPos + shadowOffset,
	                          knobRadius,
	                          shadowColor,
	                          knobRadius);

	// Draw knob
	draw_list->AddCircleFilled(knobPos, knobRadius - 1.0f, style.knobColor);

	// Tooltip on hover
	if (tooltip != nullptr && !disabled && ImGui::IsItemHovered())
	{
		ImGui::SetTooltip("%s", tooltip);
	}

	return pressed;
}

bool ToggleSwitchOption(const char* label, bool* value, const char* tooltip, bool disabled)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	// Calculate available space
	ImVec2 cursorPos = window->DC.CursorPos;
	float availWidth = ImGui::GetContentRegionAvail().x;

	// Draw label text
	ImGui::TextUnformatted(label);

	// Position toggle on the right
	ImGui::SameLine(0, 0);
	float toggleX = cursorPos.x + availWidth - (g_defaultStyle.width * settings.display.uiScale);

	// If there's a label column set, use it
	if (g_labelColumnWidth > 0.0f)
	{
		toggleX = cursorPos.x + g_labelColumnWidth;
	}

	ImGui::SetCursorPosX(toggleX);

	// Create invisible label for unique ID
	std::string toggleLabel = std::string("##") + label;
	bool changed = ToggleSwitch(toggleLabel.c_str(), value, tooltip, disabled);

	// Add spacing below
	ImGui::SetCursorPosY(window->DC.CursorPos.y + ImGui::GetStyle().ItemSpacing.y);

	return changed;
}

// Explicit template instantiations
template bool ToggleSwitchOption<true>(const char* label, config::Option<bool, true>& option,
                                        const char* tooltip);
template bool ToggleSwitchOption<false>(const char* label, config::Option<bool, false>& option,
                                         const char* tooltip);

} // namespace Widgets
