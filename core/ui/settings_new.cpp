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
#include "input/dreampotato.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/maplelinkregistry.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#ifdef USE_DREAMLINK_DEVICES
#include "sdl/dreamlink/dreamlinkgamepad.h"
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
#include <cstring>
#include <cfloat>
#ifdef __ANDROID__
#if HOST_CPU == CPU_ARM64 && USE_VULKAN
#include "rend/vulkan/adreno.h"
#endif
#endif

extern ImFont *largeFont;
extern ImFont *settingsTitleFont;
extern ImFont *settingsRightValueFont;

namespace SettingsNew {

static std::string cfgLoadStr(const std::string& section, const std::string& key, const std::string& def)
{
	return config::loadStr(section, key, def);
}

static void cfgSaveStr(const std::string& section, const std::string& key, const std::string& value)
{
	config::saveStr(section, key, value);
}

static bool cfgLoadBool(const std::string& section, const std::string& key, bool def)
{
	return config::loadBool(section, key, def);
}

static void cfgSaveBool(const std::string& section, const std::string& key, bool value)
{
	config::saveBool(section, key, value);
}

static void cfgSaveInt(const std::string& section, const std::string& key, int value)
{
	config::saveInt(section, key, value);
}

static int getResourceMonitorMode()
{
	return config::loadInt("rend", "ResourceMonitorMode", 0);
}

static void setResourceMonitorMode(int mode)
{
	config::saveInt("rend", "ResourceMonitorMode", mode);
}

#if defined(USE_DREAMLINK_DEVICES)
static void reconnectDreamLinks()
{
	// DreamLink devices are tied into maple reconnect flow; keep this hook so
	// settings exit can request DreamLink-specific reconnection behavior later.
}
#endif

// Static state for gamepad settings popup
static std::shared_ptr<GamepadDevice> g_currentGamepadForSettings;
static float g_twoLineRowExtraHeightPx = 0.0f;
static bool g_twoLineRowSeparatorsEnabled = false;
static float g_twoLineRowSeparatorAlpha = 0.5f;
static float g_twoLineRowExtraGapPx = 0.0f;
static bool g_mapleDevicesChangedInSettings = false;
static bool g_scrollToBoxArtSection = false;
static std::string g_settingsFooterText;
static constexpr float kSettingsFooterHeightPx = 152.0f;

static bool UseCompactSettingsLayout()
{
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	return displaySize.x < uiScaled(960.0f) || displaySize.y < uiScaled(720.0f);
}

static float SettingsFooterHeight()
{
	if (!UseCompactSettingsLayout())
		return uiScaled(kSettingsFooterHeightPx);
	return std::clamp(ImGui::GetIO().DisplaySize.y * 0.20f, uiScaled(104.0f), uiScaled(136.0f));
}

static float SettingsFooterGap()
{
	return UseCompactSettingsLayout() ? uiScaled(1.0f) : uiScaled(2.0f);
}

static float SettingsNavigationWidth()
{
	if (!UseCompactSettingsLayout())
		return uiScaled(225.0f);
	return std::clamp(ImGui::GetIO().DisplaySize.x * 0.24f, uiScaled(136.0f), uiScaled(180.0f));
}

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

static const char* GetTabHelpText(SettingsTab tab)
{
	switch (tab)
	{
	case SettingsTab::General:
		return "System, content, interface, and storage behavior. Highlight or select a setting to see a fuller explanation here.";
	case SettingsTab::Video:
		return "Graphics quality, renderer behavior, scaling, and performance tuning. Highlight or select a setting to see what it changes and why it matters.";
	case SettingsTab::Audio:
		return "Audio backend, latency, mixing, and output behavior. Highlight or select a setting for practical guidance before changing it.";
	case SettingsTab::Controls:
		return "Controller devices, mappings, sensitivity, and input behavior. Highlight or select a setting to see setup tips and tradeoffs.";
	case SettingsTab::Network:
		return "Online play, server, match code, and link features. Highlight or select a setting for connection details and recommendations.";
	case SettingsTab::Advanced:
		return "Power-user options, debugging, underclocking, overlays, and developer tools. Highlight or select a setting for deeper explanations.";
	case SettingsTab::About:
		return "Project information, links, license details, and credits.";
	case SettingsTab::Count:
		break;
	}
	return "Highlight or select a setting to see details here.";
}

static void ResetSettingsFooter()
{
	g_settingsFooterText = GetTabHelpText(g_state.currentTab);
}

static void SetSettingsFooterText(const char* text)
{
	if (text != nullptr && text[0] != '\0')
		g_settingsFooterText = text;
}

static void ShowFooterHelpMarker(const char* desc)
{
	ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
		SetSettingsFooterText(desc);
}

static void RenderSettingsFooterBar()
{
	const float footerHeight = SettingsFooterHeight();
	ImGui::BeginChild("SettingsFooterBar", ImVec2(0.0f, footerHeight), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 footerMin = ImGui::GetWindowPos();
	const ImVec2 footerMax(footerMin.x + ImGui::GetWindowSize().x, footerMin.y + ImGui::GetWindowSize().y);
	ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
	ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
	border.w = std::min(border.w + 0.10f, 1.0f);
	accent.w = 1.0f;

	const bool compactLayout = UseCompactSettingsLayout();
	const float padX = compactLayout ? uiScaled(1.0f) : uiScaled(2.0f);
	const float padY = compactLayout ? uiScaled(1.0f) : uiScaled(2.0f);
	ImFont* titleFont = settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;
	ImFont* bodyFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	const float footerTextScale = 1.25f;
	const float titleFontSize = titleFont->LegacySize * (compactLayout ? 0.93f : 0.97f) * footerTextScale;
	const float bodyFontSize = bodyFont->LegacySize * (compactLayout ? 0.89f : 0.93f) * footerTextScale;
	const float textPadX = compactLayout ? uiScaled(10.0f) : uiScaled(12.0f);
	const float textPadY = compactLayout ? uiScaled(8.0f) : uiScaled(10.0f);
	const ImVec2 textBoxMin(footerMin.x + padX, footerMin.y + padY);
	const ImVec2 textBoxMax(footerMax.x - padX, footerMax.y - padY);
	ImVec4 textBoxBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	textBoxBg.w = std::min(textBoxBg.w + 0.35f, 1.0f);
	drawList->AddRectFilled(textBoxMin, textBoxMax,
		ImGui::ColorConvertFloat4ToU32(textBoxBg), uiScaled(10.0f));
	drawList->AddRect(textBoxMin, textBoxMax,
		ImGui::ColorConvertFloat4ToU32(border), uiScaled(10.0f), 0, 1.0f);
	drawList->AddRectFilled(
		textBoxMin,
		ImVec2(textBoxMax.x, textBoxMin.y + uiScaled(3.0f)),
		ImGui::ColorConvertFloat4ToU32(accent),
		uiScaled(10.0f),
		ImDrawFlags_RoundCornersTop);

	const char* footerText = g_settingsFooterText.empty()
		? "Highlight or select a setting to see details here."
		: g_settingsFooterText.c_str();
	const ImVec2 clipMin(textBoxMin.x + textPadX, textBoxMin.y + textPadY);
	const ImVec2 clipMax(textBoxMax.x - textPadX, textBoxMax.y - textPadY);
	const ImVec2 bodyPos(clipMin.x, clipMin.y);
	const float wrapWidth = std::max(0.0f, clipMax.x - clipMin.x);
	const float bgLuminance = (textBoxBg.x + textBoxBg.y + textBoxBg.z) / 3.0f;
	const ImVec4 bodyColor = bgLuminance > 0.5f
		? ImVec4(0.08f, 0.08f, 0.08f, 1.0f)
		: ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	const char* bodyStart = footerText;
	std::string titleLine;
	if (const char* nl = strchr(footerText, '\n'); nl != nullptr)
	{
		titleLine.assign(footerText, nl);
		bodyStart = nl + 1;
	}
	const auto normalizeFooterBody = [](const char* text) -> std::string
	{
		if (text == nullptr || text[0] == '\0')
			return {};
		std::string out;
		out.reserve(strlen(text));
		int newlineCount = 0;
		for (const char* p = text; *p != '\0'; ++p)
		{
			char c = *p;
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				++newlineCount;
				continue;
			}

			if (newlineCount > 0)
			{
				if (newlineCount >= 2)
				{
					while (!out.empty() && out.back() == ' ')
						out.pop_back();
					if (!out.empty() && out.back() != '\n')
						out.push_back('\n');
					out.push_back('\n');
				}
				else
				{
					if (!out.empty() && out.back() != ' ' && out.back() != '\n')
						out.push_back(' ');
				}
				newlineCount = 0;
			}

			if (c == '\t')
				c = ' ';
			if (c == ' ')
			{
				if (out.empty() || out.back() == ' ' || out.back() == '\n')
					continue;
			}
			out.push_back(c);
		}

		if (newlineCount >= 2)
		{
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			if (!out.empty() && out.back() != '\n')
				out.push_back('\n');
			out.push_back('\n');
		}
		else if (newlineCount == 1)
		{
			if (!out.empty() && out.back() != ' ' && out.back() != '\n')
				out.push_back(' ');
		}

		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		return out;
	};
	const std::string bodyText = normalizeFooterBody(bodyStart);
	const std::string displayTitle = titleLine.empty()
		? std::string()
		: std::string("Setting Details: ") + titleLine;
	const float titleSpacing = compactLayout ? uiScaled(3.0f) : uiScaled(5.0f);
	const float availableHeight = std::max(0.0f, clipMax.y - clipMin.y);
	float finalTitleSize = titleFontSize;
	float finalBodySize = bodyFontSize;
	if (availableHeight > 0.0f && (!displayTitle.empty() || !bodyText.empty()))
	{
		float totalHeight = 0.0f;
		if (!displayTitle.empty())
			totalHeight += titleFont->CalcTextSizeA(titleFontSize, FLT_MAX, wrapWidth, displayTitle.c_str()).y + titleSpacing;
		if (!bodyText.empty())
			totalHeight += bodyFont->CalcTextSizeA(bodyFontSize, FLT_MAX, wrapWidth, bodyText.c_str()).y;
		if (totalHeight > availableHeight)
		{
			const float scale = std::clamp(availableHeight / totalHeight, 0.78f, 1.0f);
			finalTitleSize = titleFontSize * scale;
			finalBodySize = bodyFontSize * scale;
		}
	}

	ImVec2 textPos = bodyPos;
	drawList->PushClipRect(clipMin, clipMax, true);
	if (!displayTitle.empty())
	{
		const ImVec4 titleColor2 = ImVec4(
			std::min(bodyColor.x + 0.15f, 1.0f),
			std::min(bodyColor.y + 0.15f, 1.0f),
			std::min(bodyColor.z + 0.15f, 1.0f),
			1.0f);
		drawList->AddText(
			titleFont,
			finalTitleSize,
			textPos,
			ImGui::ColorConvertFloat4ToU32(titleColor2),
			displayTitle.c_str(),
			nullptr,
			wrapWidth);
		textPos.y += finalTitleSize + titleSpacing;
	}

	if (!bodyText.empty())
	{
		drawList->AddText(
			bodyFont,
			finalBodySize,
			textPos,
			ImGui::ColorConvertFloat4ToU32(bodyColor),
			bodyText.c_str(),
			nullptr,
			wrapWidth);
	}
	drawList->PopClipRect();

	ImGui::EndChild();
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
	ImGuiSelectableFlags rowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
	if (disabled)
		rowFlags |= ImGuiSelectableFlags_Disabled;

	ImguiStyleVar rowRounding(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
	const bool clicked = ImGui::Selectable(rowId, false, rowFlags, ImVec2(0.0f, TwoLineSettingRowHeight()));
	if (tooltip != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
		SetSettingsFooterText(tooltip);

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

	// Keep inline row descriptions compact: if text uses "Title\nBody",
	// show the first body line (or title when no body exists).
	auto compactDescriptionLine = [](const char* text) -> std::string
	{
		if (text == nullptr || text[0] == '\0')
			return {};
		const char* start = text;
		const char* firstNewline = strchr(text, '\n');
		if (firstNewline != nullptr)
		{
			start = firstNewline + 1;
			while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
				++start;
			if (*start == '\0')
				start = text;
		}
		const char* end = strchr(start, '\n');
		if (end == nullptr)
			end = start + strlen(start);
		while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
			--end;
		return std::string(start, end);
	};
	const std::string compactLine = compactDescriptionLine(description);
	if (compactLine.empty())
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
	ImGui::TextUnformatted(compactLine.c_str());
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
		if (popupCfg.options.onOptionHighlight == nullptr)
		{
			popupCfg.options.onOptionHighlight =
				[rowTooltip, label = popupCfg.options.label](int, const char* optionText)
				{
					std::string footerText;
					if (label != nullptr && label[0] != '\0')
					{
						footerText = label;
						if (optionText != nullptr && optionText[0] != '\0')
						{
							footerText += ": ";
							footerText += optionText;
						}
					}
					else if (optionText != nullptr && optionText[0] != '\0')
					{
						footerText = optionText;
					}

					if (rowTooltip != nullptr && rowTooltip[0] != '\0')
					{
						if (!footerText.empty())
							footerText += ". ";
						footerText += rowTooltip;
					}

					if (!footerText.empty())
						SetSettingsFooterText(footerText.c_str());
				};
		}
	}
	else if (popupCfg.type == SettingsUI::PopupType::Slider)
	{
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
	if (ImGui::BeginPopupModal("Gamepad Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_DragScrolling))
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
			ShowFooterHelpMarker("Rumble power");
		}
		if (gamepad->has_analog_stick())
		{
			header("Thumbsticks");
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Dead zone", &deadzone, 0, 100, "%d%%"))
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowFooterHelpMarker("Minimum deflection to register as input");
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Saturation", &saturation, 50, 200, "%d%%"))
				gamepad->set_saturation(saturation / 100.f);
			ImGui::SameLine();
			ShowFooterHelpMarker("Value sent to the game at 100% thumbstick deflection. "
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
	{ EMU_BTN_PAUSE, Tnop("Pause") },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_LOADSTATE_RAM, Tnop("Load State in RAM") },
	{ EMU_BTN_SAVESTATE_RAM, Tnop("Save State in RAM") },
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
	{ EMU_BTN_PAUSE, Tnop("Pause") },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_LOADSTATE_RAM, Tnop("Load State in RAM") },
	{ EMU_BTN_SAVESTATE_RAM, Tnop("Save State in RAM") },
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
		ShowFooterHelpMarker(
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
	ImGui::BeginChild(ImGui::GetID("buttons"), ImVec2(0, 0),
		ImGuiChildFlags_FrameStyle | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
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

static void RenderGeneralInfoRow(const char* id, const char* icon, const char* label, const char* description,
	const std::string& value, bool disabledValue = false, const char* tooltip = nullptr)
{
	ImGui::PushID(id);
	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	BeginTwoLineSettingRow("##row", rowTooltip, true);
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
		const StoragePopupResult storageResult = select_storage_popup(true, false, popupTitle, [pathsPtr](bool cancelled, const std::string& selection) {
			if (!cancelled)
			{
				pathsPtr->push_back(selection);
				SaveSettings();
			}
			return true;
		});
		if (storageResult == StoragePopupResult::Unsupported)
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
	ResetSettingsFooter();
}

void openTab(SettingsTab tab)
{
	g_state.currentTab = tab;
	g_scrollToBoxArtSection = false;
	ResetSettingsFooter();
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
static void renderNavigationRail(const std::function<void()>& exitSettings, const char* exitLabel)
{
	const float navWidth = SettingsNavigationWidth();
	const float footerGap = SettingsFooterGap();
	const float footerHeight = SettingsFooterHeight();

	// Left navigation rail: 225px wide child window
	// Match the main content column height so the shared footer sits flush below both panels.
	ImGui::BeginChild("NavigationRail", ImVec2(navWidth, -(footerHeight + footerGap)), true);

	IconButton backButton(ICON_FA_ARROW_LEFT, exitLabel, ImVec2(-1, 0));
	if (backButton.realize())
		exitSettings();
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

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
		if (ImGui::Selectable(tabName, isSelected, ImGuiSelectableFlags_None, ImVec2(navWidth, 0)))
		{
			g_state.currentTab = tab;
			ResetSettingsFooter();
		}
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText(GetTabHelpText(tab));
	}

	ImGui::EndChild();
}

// Render right content area (remaining width for settings content)
// This displays the actual settings for the currently selected tab
static void renderContentArea()
{
	const float footerGap = SettingsFooterGap();
	const float footerHeight = SettingsFooterHeight();

	// Right content area: fills remaining space
	// ImGui::SameLine() positions this after the navigation rail
	// No width specified means it takes all remaining space
	ImGui::BeginChild("ContentArea", ImVec2(0, -(footerHeight + footerGap)), true,
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
	ImGui::Dummy(ImVec2(0.0f, footerGap));
	RenderSettingsFooterBar();
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
	if (ImGui::CollapsingHeader(ICON_FA_GLOBE " Language & Region##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		static const char* languages[] = { "Japanese", "English", "German", "French", "Spanish", "Italian", "Default" };
		SettingsUI::PopupConfig languageCfg {};
		languageCfg.type = SettingsUI::PopupType::Options;
		languageCfg.options.label = "Dreamcast Language";
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
				"Dreamcast BIOS language (affects game text/menus).",
				languageCfg,
				"Language\n"
				"Sets the Dreamcast BIOS language that games read for menus and text.\n"
				"Most games will follow this setting; some may ignore it.\n\n"
				"Arcade platforms typically use automatic region/language behavior.");
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
				"TV broadcast standard for non-VGA output modes.",
				broadcastCfg,
				"Broadcast\n"
				"Controls the TV broadcast standard used for non-VGA video modes.\n"
				"NTSC is typically 60 Hz; PAL is typically 50 Hz.\n\n"
				"If you see timing issues or a game expects a specific region video mode, try changing this.");
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
			RenderGeneralPopupSettingRow(
				"RegionSetting",
				"BIOS region (can affect compatibility and defaults).",
				regionCfg,
				"Region\n"
				"Sets the emulated BIOS region.\n"
				"This can affect game compatibility, default language choices, and region-locked behavior in some titles.\n\n"
				"If a game refuses to boot or behaves like the wrong region, try changing this.");
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
			RenderGeneralPopupSettingRow(
				"CableSetting",
				"Emulated video cable type (affects game video modes).",
				cableCfg,
				"Cable\n"
				"Controls the emulated Dreamcast video cable type.\n"
				"Games can change behavior depending on whether they detect VGA vs. TV output.\n\n"
				"If a game has missing effects, wrong colors, or odd mode selection, this is worth trying.",
				disabled);
		}

	// ========================================
	// Content Paths Section
	// ========================================
#if !defined(TARGET_IPHONE)
		if (ImGui::CollapsingHeader(ICON_FA_FOLDER " Content Paths##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{

		int to_delete = -1;
	bool addContentRequested = false;
	for (u32 i = 0; i < config::ContentPath.get().size(); i++)
	{
		ImGui::PushID(static_cast<int>(i));
			BeginTwoLineSettingRow(
				"##row",
				"Content Paths\n"
				"These are the folders where your games are stored.\n"
				"Add one or more folders and use Rescan/Refresh to update the library view.");
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
			const bool addRowActivated = BeginTwoLineSettingRow(
				"##row",
				"Content Paths\n"
				"Adds another folder to scan for games.\n"
				"If you have performance issues when scanning, prefer a smaller folder or fewer subfolders.");
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
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText("Refresh the current content library view.");

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
		}

	#if defined(__linux__) && !defined(__ANDROID__)
		if (ImGui::CollapsingHeader(ICON_FA_FLOPPY_DISK " Data Folder##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{
		RenderGeneralInfoRow(
		"DataFolderPathRow",
		ICON_FA_FLOPPY_DISK,
		"Data Folder",
		"The folder containing BIOS files, as well as saved VMUs and states",
		get_writable_data_path(""),
		false,
		"Data Folder\n"
		"This is Flycast's writable data directory on this platform.\n\n"
		"It typically contains:\n"
		"- BIOS/Flash files (for example: `dc_boot.bin`, `dc_flash.bin`)\n"
		"- Save data (VMU files) and save states\n\n"
			"If Flycast cannot find a BIOS, double-check that your BIOS files are placed in the expected location and that their filenames match what Flycast looks for.\n"
			"Custom Paths below can also override where some of these files are stored.");
		}
	#else
		if (ImGui::CollapsingHeader(ICON_FA_HOUSE " Home Directory##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{
		RenderGeneralInfoRow(
		"HomeFolderPathRow",
		ICON_FA_HOUSE,
		"Home Directory",
		"The folder where Flycast saves configuration files and VMUs. BIOS files should be in a subfolder named \"data\"",
		get_writable_config_path(""),
		false,
		"Home Directory\n"
		"This is Flycast's main configuration directory.\n\n"
		"It typically contains:\n"
		"- Configuration files\n"
		"- VMU saves and other user data\n\n"
		"On platforms that use a `data` subfolder, BIOS/Flash files should be placed under `data` inside this directory.\n"
		"If you change custom paths below, Flycast may store some files elsewhere, but this directory remains the base for core configuration.");

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
		}
	#endif // !linux
		ImGui::Spacing();
#else // TARGET_IPHONE
	{
			ImGui::PushID("IphoneRescanContentRow");
			const bool rescanRowActivated = BeginTwoLineSettingRow(
				"##row",
				"Rescan Content\n"
				"Rescans all configured content folders and rebuilds the game list.\n\n"
				"This can take a moment if you have many games or slow storage.\n"
				"If you just added/removed files and they are not showing up, this is the first thing to try.");
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
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText("Refresh the current content library view.");

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
	if (ImGui::CollapsingHeader(ICON_FA_GEAR " UI Settings##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
			RenderGeneralPopupSettingRow(
				"UIThemeSetting",
				"Choose the UI theme colors.",
				themeCfg,
				"UI Theme\n"
				"Changes the overall look and contrast of the UI.\n"
				"If readability is an issue, try High Contrast.");
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
				"Scale the UI to fit your screen and viewing distance.",
				uiScalingCfg,
				"UI Scaling\n"
				"Adjusts UI element sizes.\n"
				"Useful on high-DPI displays, TVs, or small screens.\n\n"
				"After applying, the UI will reinitialize to apply the new scale.");
	}

		RenderGeneralToggleSettingRow(
			"HideLegacyNaomiRoms",
			ICON_FA_LOCK,
			"Hide Legacy Naomi Roms",
			"Hide legacy Naomi file types in the content browser.",
			static_cast<bool>(config::HideLegacyNaomiRoms),
		[](bool enabled) {
			config::HideLegacyNaomiRoms = enabled;
			scanner.refresh();
		},
			"Hide Legacy Naomi ROMs\n"
			"Hides `.bin`, `.dat`, and `.lst` files from the content browser.\n"
			"Useful if you have mixed Naomi sets and only want to see launchable content.");

#ifdef __ANDROID__
		RenderGeneralToggleSettingRow(
			"UseSafFilePicker",
			ICON_FA_MOBILE_SCREEN_BUTTON,
			"Use SAF File Picker",
			"Android storage picker integration.",
			static_cast<bool>(config::UseSafFilePicker),
			[](bool enabled) { config::UseSafFilePicker = enabled; },
			"Use SAF File Picker\n"
			"Uses Android's Storage Access Framework (SAF) for file browsing.\n"
			"This can improve compatibility with scoped storage and content providers on newer Android versions.");
#endif

	// ========================================
	// Box Art Section
	// ========================================
	if (ImGui::CollapsingHeader(ICON_FA_IMAGE " Box Art##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (g_scrollToBoxArtSection)
		{
			ImGui::SetScrollHereY(0.0f);
			g_scrollToBoxArtSection = false;
		}

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
				"Choose which box art source to display.",
				boxartSourceCfg,
				"Box Art Source\n"
				"Selects which artwork source is shown in the game list.\n"
				"Use Custom Boxart if you maintain your own images, or Original/Physical depending on your preference.");
	}

	{
		ImGui::PushID("CustomBoxartFolderRow");
			const bool rowActivated = BeginTwoLineSettingRow(
				"##row",
				"Custom Boxart Folder\n"
				"Folder containing custom box art images (png/jpg).\n"
				"File names should match game names.\n"
				"Use Refresh to rescan artwork sources.");
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
			if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
				SetSettingsFooterText("Refresh downloaded box art and rescan artwork sources.");

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
			"Box Art Game List\n"
			"Displays cover art tiles in the content list.\n"
			"Disable if you prefer a faster, more compact list or want to reduce UI clutter.");

	const bool physicalOnly = config::BoxartSourceMode.get() == static_cast<int>(BoxartSourceMode::PhysicalMediaOnly);
		RenderGeneralToggleSettingRow(
			"FetchBoxart",
			ICON_FA_DOWNLOAD,
			"Fetch Box Art",
			"Fetch cover images from TheGamesDB.net.",
			static_cast<bool>(config::FetchBoxart),
			[](bool enabled) { config::FetchBoxart = enabled; },
			"Fetch Box Art\n"
			"Downloads cover images from TheGamesDB.net.\n"
			"Disable if you only want physical media images, custom artwork, or to avoid network fetching.",
			physicalOnly);

	// ========================================
	// Automatic Save States Section
	// ========================================
	if (ImGui::CollapsingHeader(ICON_FA_FLOPPY_DISK " Automatic Save States##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"AutoLoadState",
			ICON_FA_FOLDER_OPEN,
			"Auto-Load on Start",
			"Load the last saved state of the game when starting",
			static_cast<bool>(config::AutoLoadState),
			[](bool enabled) { config::AutoLoadState = enabled; },
			"Auto-Load on Start\n"
			"Automatically loads the last saved state when starting a game.\n"
			"Convenient, but can hide boot-time issues or break games that don't like savestates.");

		RenderGeneralToggleSettingRow(
			"AutoSaveState",
			ICON_FA_FLOPPY_DISK,
			"Auto-Save on Exit",
			"Save the state of the game when stopping",
			static_cast<bool>(config::AutoSaveState),
			[](bool enabled) { config::AutoSaveState = enabled; },
			"Auto-Save on Exit\n"
			"Automatically saves a state when you stop a game.\n"
			"Good for quick resume workflows. Disable if you prefer manual state management.");

		RenderGeneralToggleSettingRow(
			"NaomiFreePlay",
			ICON_FA_TROPHY,
			"Naomi Free Play",
			"Configure Naomi games in Free Play mode.",
			static_cast<bool>(config::ForceFreePlay),
			[](bool enabled) { config::ForceFreePlay = enabled; },
			"Naomi Free Play\n"
			"Forces supported Naomi titles into Free Play mode.\n"
			"Useful if you don't want to manage credits/coin input.");

#if USE_DISCORD
		RenderGeneralToggleSettingRow(
			"discord_presence",
			ICON_FA_COMMENT,
			"Discord Presence",
			"Show which game you are playing on Discord",
			static_cast<bool>(config::DiscordPresence),
			[](bool enabled) { config::DiscordPresence = enabled; },
			"Discord Presence\n"
			"Shows which game you are playing on Discord.\n"
			"Disable if you prefer not to share activity status.");
#endif

		ImGui::Spacing();
	}

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
				[](bool enabled) { config::EnableAchievements = enabled; },
				"Enable RetroAchievements\n"
				"Enables RetroAchievements support and achievement tracking.\n"
				"Requires a RetroAchievements account and a supported title.");

		if (config::EnableAchievements)
		{
			ImGui::Indent();

				RenderGeneralToggleSettingRow(
					"AchievementsHardcoreMode",
					ICON_FA_FIRE,
					"Hardcore Mode",
					"Enable RetroAchievements hardcore mode. Using cheats and loading a state are not allowed.",
					static_cast<bool>(config::AchievementsHardcoreMode),
					[](bool enabled) { config::AchievementsHardcoreMode = enabled; },
					"Hardcore Mode\n"
					"RetroAchievements Hardcore Mode disables features that can invalidate achievements.\n"
					"Using cheats and loading a state are not allowed.");

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
						},
						"RetroAchievements Username\n"
						"Enter your RetroAchievements account username used for login.");

					RenderGeneralTextInputSettingRow(
						"RAPasswordRow",
						ICON_FA_LOCK,
						"Password",
						"RetroAchievements account password",
						[&]() {
							InputText("##PasswordInput", password, sizeof(password), ImGuiInputTextFlags_Password);
						},
						"RetroAchievements Password\n"
						"Enter your RetroAchievements account password used for login.");

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
	if (ImGui::CollapsingHeader(ICON_FA_SLIDERS " Custom Paths##Section"))
	{

	managePathList("BIOS Folders", config::BiosPath.get(),
		"BIOS Folders\n"
		"Folders that Flycast searches for BIOS/Flash files (Dreamcast BIOS/Flash and arcade BIOS).\n\n"
		"Typical Dreamcast files include `dc_boot.bin` and `dc_flash.bin`.\n"
		"If you keep multiple BIOS sets, you can add multiple folders and Flycast will search all of them.\n"
		"For troubleshooting, keep your BIOS files in a single known-good folder to avoid confusion.");
	ImGui::Spacing();

#if !defined(__ANDROID__)
	manageSinglePath("VMU Folder", config::VMUPath,
		"VMU Folder\n"
		"Where VMU memory card files (`.bin`) are stored.\n\n"
		"If you change this, Flycast will look for VMUs in the new location.\n"
		"Move/copy your existing VMU files if you want to keep your saves.\n"
		"Use this to keep saves on a specific drive or to share saves between installations.");
	ImGui::Spacing();

	managePathList("Savestate Folders", config::SavestatePath.get(),
		"Savestate Folders\n"
		"Folders used for save states.\n\n"
		"The first folder is used when creating new save states.\n"
		"All listed folders are searched when loading, which is useful if you keep states organized across different locations.\n"
		"Save states are not always portable between very different Flycast versions; if a state fails to load, try recreating it on your current build.");
	ImGui::Spacing();

	manageSinglePath("Game Save Folder", config::SavePath,
		"Game Save Folder\n"
		"Folder for non-VMU save data (for example: arcade NVRAM and other persistent game data).\n\n"
		"If you change this path, move/copy your existing save files if you want to keep progress.\n"
		"This is separate from save states, and separate from VMU files.");
	ImGui::Spacing();
#endif

	managePathList("Texture Pack Folders", config::TexturePath.get(),
		"Texture Pack Folders\n"
		"Folders that Flycast searches for custom texture packs.\n\n"
		"Common layouts include `textures/<gameId>/...` or `<gameId>` inside a `textures` subfolder.\n"
		"After installing a pack, enable Custom Textures in the Video tab and restart the game.\n"
		"If you notice stutter while textures stream in, consider using Preload Textures (more RAM/VRAM required).");
	ImGui::Spacing();

#if !defined(__ANDROID__)
	manageSinglePath("Texture Dump Folder", config::TextureDumpPath,
		"Texture Dump Folder\n"
		"Where Flycast saves dumped textures when Texture Dumping is enabled.\n\n"
		"Game-specific subfolders are created automatically.\n"
		"Dumping can generate a large number of files quickly and consume significant disk space.\n"
		"Enable dumping only when needed, then disable it after you are done.");
	ImGui::Spacing();

	managePathList("Controller Mapping Folders", config::MappingsPath.get(),
		"Folders containing controller mapping files (.cfg). The emulator also looks in Home Folder/mappings. Per-game mappings are suffixed with _<gameId>.cfg");
	ImGui::Spacing();

	managePathList("Cheat Folders", config::CheatPath.get(),
		"Cheat Folders\n"
		"Folders containing cheat files (`.cht` / `.txt`) named using the game's ID.\n\n"
		"Flycast can auto-load matching cheat files when present.\n"
		"If cheats are not loading, verify the filename matches the game ID and that the cheat file format is correct.\n"
		"Use cheats carefully: they can crash games or cause unexpected behavior.");
	ImGui::Spacing();
#endif  // !ANDROID
	}
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
	if (ImGui::CollapsingHeader(ICON_FA_BOOKMARK " Quality Presets##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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

		// Preset dropdown (custom so we can drive footer text per option)
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		const char* currentLabel = (currentPresetIndex >= 0 && currentPresetIndex < 6)
			? presetLabels[currentPresetIndex]
			: "Custom";
		if (ImGui::BeginCombo("##VideoPreset", currentLabel))
		{
			for (int i = 0; i < 6; ++i)
			{
				const bool isSelected = (currentPresetIndex == i);
				if (ImGui::Selectable(presetLabels[i], isSelected))
				{
					currentPresetIndex = i;
					if (currentPresetIndex < 5)
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
				if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
				{
					if (i < 5)
					{
						const VideoPreset* preset = getPresetByLevel(
							static_cast<VideoPresetLevel>(i)
						);
						if (preset && preset->description)
							SetSettingsFooterText(preset->description);
					}
					else
					{
						SetSettingsFooterText("Custom\nSettings have been manually modified from the last preset.");
					}
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		// Show preset guidance and the selected preset description in the footer bar
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
		{
			std::string help =
				"Quality Presets\n"
				"If you are having issues, try our presets. Unless you are on a very low-end device, starting at the Medium preset should be safe. "
				"Adjust the resolution for the fastest performance adjustment from within a preset, then tweak the preset as you like for your perfect experience. "
				"Turn on the FPS Counter setting to ensure you get a stable 30/60 FPS during gameplay while finding the best settings for your device. "
				"Note: Fog and Shadows are enabled on all preset tiers.";
			SetSettingsFooterText(help.c_str());
		}

		// Show "Custom" indicator if settings were manually changed
		VideoPresetLevel currentLevel = detectCurrentPreset();
		if (currentLevel == VideoPresetLevel::Custom && !presetJustApplied)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(Modified)");
			if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
				SetSettingsFooterText("Settings have been manually modified from the last preset.");
		}

		ImGui::Separator();
		presetJustApplied = false;
	}

	// Graphics API Selection section
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

	if (ImGui::CollapsingHeader(ICON_FA_GEAR " Graphics API##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
			apiCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
				if (selectedIndex < 0 || selectedIndex >= apiOptionCount)
					return;
				switch (apiValues[selectedIndex])
				{
				case 0:
					SetSettingsFooterText(
						"OpenGL\n"
						"This is the API that will randomly work best depending on the game and hardware, sometimes for no obvious reason.\n"
						"Always worth a try on any hardware if a game likes it.");
					break;
				case 1:
					SetSettingsFooterText(
						"Vulkan\n"
						"Tends to work best with NVIDIA GPUs, but remember this is just a recommendation, not a fact.\n"
						"Per-game and per-hardware mileage may vary.");
					break;
				case 3:
					SetSettingsFooterText(
						"DirectX 11\n"
						"Tends to give higher frame rates on AMD devices.\n"
						"Older and integrated graphics can benefit from this option as well.");
					break;
				case 2:
					SetSettingsFooterText(
						"DirectX 9\n"
						"Like DirectX 11, this tends to be best on AMD hardware.\n"
						"Lower-end and older devices often work even better with DX9, but again, this is just a thought, not a hard rule.");
					break;
				default:
					break;
				}
			};
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
				"Graphics API\n"
				"One of the most important settings when you're not getting a good experience.\n"
				"You will find that, per game and hardware, you can get different performance depending on the API selected.\n"
				"The best way to figure it out: if you are having FPS issues or glitchy gameplay, change this per your hardware recommendations and see what gives you the smoothest frame rate.\n"
				"(Enable the FPS Counter and test in actual gameplay.)",
				apiCfg);
		}

		// Transparent Sorting section
		if (ImGui::CollapsingHeader(ICON_FA_WAND_SPARKLES " Transparent Sorting##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{

			{
			const bool has_per_pixel = GraphicsContext::Instance()->hasPerPixel();
			int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;

			std::array<const char*, 3> sortingLabels {};
			std::array<int, 3> sortingValues {};
			int sortingOptionCount = 0;
			if (has_per_pixel)
			{
				sortingLabels[sortingOptionCount] = "Per Pixel";
				sortingValues[sortingOptionCount++] = 2;
			}
			sortingLabels[sortingOptionCount] = "Per Triangle";
			sortingValues[sortingOptionCount++] = 0;
			sortingLabels[sortingOptionCount] = "Per Strip";
			sortingValues[sortingOptionCount++] = 1;

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
			sortingCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
				if (selectedIndex < 0 || selectedIndex >= sortingOptionCount)
					return;
				switch (sortingValues[selectedIndex])
				{
				case 2:
					SetSettingsFooterText(
						"Per Pixel\n"
						"Highest performance requirements. This matches the Dreamcast's sorting quality, but only about 15% of games need it and show a meaningful difference versus the middle sorting option.\n"
						"Use only when needed (or if you have the resource overhead). It can almost double resource requirements.\n"
						"If you need full speed at a target resolution, you may have to lower resolution when using Per Pixel sorting.");
					break;
				case 0:
					SetSettingsFooterText(
						"Per Triangle\n"
						"Good enough for about 85% of games to play with no or only very brief flashes of incorrect sorting.\n"
						"For most mid-range devices trying to reach 4K resolutions, you will likely spend your time here unless you must use Per Pixel.\n"
						"This is the recommended option for most people, most of the time.");
					break;
				case 1:
					SetSettingsFooterText(
						"Per Strip\n"
						"Introduces more graphics glitching than Per Triangle, but can sometimes reduce resource requirements further.\n"
						"Not recommended unless you have to use it. Performance gains range from none to minimal, and may not be worth the quality tradeoff.");
					break;
				default:
					break;
				}
			};
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
				"Sorting Option\n"
				"This controls how we sort transparent layers in games.\n"
				"A handful of games will not render certain images correctly at all (example: Shenmue journal), or might have layering issues (example: the white of Sonic's eyes in Sonic Adventure), and a handful of others can show clipping or incorrect texture layering in some spots.\n\n"
				"There are three options with very different resource usage. While the highest setting matches Dreamcast sorting quality, it is only needed in about 15% of games and carries a large speed penalty.\n"
				"The middle tier allows just about all games to be played with a small amount of slight issues, but uses about half the resources of the top tier.\n"
				"It is \"good enough\" almost all the time.",
				sortingCfg);
			}
		}

		// Rendering Options section
		if (ImGui::CollapsingHeader(ICON_FA_SLIDERS " Rendering Options##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{

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
			"Internal render resolution (the biggest performance lever).",
			internalResCfg,
			"Internal Resolution\n"
			"This is the single biggest graphics performance setting.\n"
			"Lower values reduce GPU load (faster). Higher values improve clarity, reduce shimmer/aliasing, and act as a form of supersampling.\n\n"
			"If you are not holding a stable 30/60 FPS, reduce Internal Resolution first before turning off visual features.");
	}

	RenderGeneralToggleSettingRow(
		"IntegerScaling",
		ICON_FA_COMPRESS,
		"Integer Scaling",
		"Pixel-perfect scaling using whole-number multiples.",
		static_cast<bool>(config::IntegerScale),
		[](bool enabled) { config::IntegerScale.set(enabled); },
		"Integer Scaling\n"
		"Scales the output by the maximum whole-number multiple allowed by your display (1x, 2x, 3x, ...).\n"
		"Best for pixel-art/2D content because it avoids uneven scaling artifacts.\n\n"
		"Tip: for the crispest result, pair Integer Scaling with nearest-neighbor (disable Linear Interpolation).");

	RenderGeneralToggleSettingRow(
		"LinearInterpolation",
		ICON_FA_WAND_MAGIC,
		"Linear Interpolation",
		"Smoother scaling (linear) vs. sharper pixels (nearest-neighbor).",
		static_cast<bool>(config::LinearInterpolation),
		[](bool enabled) { config::LinearInterpolation.set(enabled); },
		"Linear Interpolation\n"
		"Enabled: smoother scaling (can look blurrier).\n"
		"Disabled: nearest-neighbor scaling (sharper, more pixelated).\n\n"
		"If you enable Integer Scaling, many users prefer disabling Linear Interpolation for a pixel-perfect look.");

#ifndef TARGET_IPHONE
		RenderGeneralToggleSettingRow(
			"VSync",
			ICON_FA_ARROWS_ROTATE,
			"VSync",
			"Sync presentation to your display to reduce tearing.",
			static_cast<bool>(config::VSync),
			[](bool enabled) { config::VSync.set(enabled); },
			"VSync\n"
			"Synchronizes frame presentation to your display refresh to reduce tearing and improve pacing.\n"
			"On some setups it can add input latency, and if you cannot maintain full speed it can contribute to stutter.\n\n"
			"Recommended for most users. If you notice unusual stutter or latency, try toggling it.");

	if (isVulkan(config::RendererType))
	{
		ImGui::Indent();
			RenderGeneralToggleSettingRow(
				"DuplicateFrames",
				ICON_FA_CLONE,
				"Duplicate Frames",
				"Improve pacing on high refresh-rate displays.",
				static_cast<bool>(config::DupeFrames),
				[](bool enabled) { config::DupeFrames.set(enabled); },
				"Duplicate Frames\n"
				"Duplicates frames on high refresh-rate monitors (120 Hz and above) to improve perceived pacing.\n"
				"Only available for Vulkan, and only when VSync is disabled.",
				!config::VSync);
		ImGui::Unindent();
	}
#endif

	RenderGeneralToggleSettingRow(
		"ShowVMU",
		ICON_FA_GAMEPAD,
		"Show VMU In-Game",
		"Show VMU LCD screens during gameplay.",
		static_cast<bool>(config::FloatVMUs),
		[](bool enabled) { config::FloatVMUs.set(enabled); },
		"Show VMU In-Game\n"
		"Displays the VMU LCD screens while in-game.\n"
		"Useful for games that rely on VMU info (status, menus, minigames).");

	RenderGeneralToggleSettingRow(
		"Framebuffer",
		ICON_FA_FILE,
		"Full Framebuffer Emulation",
		"Accurate effects, but extremely expensive.",
		static_cast<bool>(config::EmulateFramebuffer),
		[](bool enabled) { config::EmulateFramebuffer.set(enabled); },
		"Full Framebuffer Emulation\n"
		"Enables accurate VRAM framebuffer emulation. Required for some games' effects.\n"
		"Very slow, and generally incompatible with upscaling and widescreen.\n\n"
		"Only enable if a game needs it, and be prepared to lower Internal Resolution.");

	RenderGeneralToggleSettingRow(
		"CustomTextures",
		ICON_FA_IMAGE,
		"Load Custom Textures",
		"Enable texture replacement packs.",
		static_cast<bool>(config::CustomTextures),
		[](bool enabled) { config::CustomTextures.set(enabled); },
		"Load Custom Textures\n"
		"Loads custom/high-res textures from `data/textures/<game id>`.\n"
		"Great for community texture packs, but can increase load time and memory usage.\n\n"
		"If you see stutter from texture streaming, consider enabling Preload Custom Textures.",
		game_started);

	ImGui::Indent();
	RenderGeneralToggleSettingRow(
		"PreloadTextures",
		ICON_FA_DOWNLOAD,
		"Preload Custom Textures",
		"Trade memory for fewer texture hitches.",
		static_cast<bool>(config::PreloadCustomTextures),
		[](bool enabled) { config::PreloadCustomTextures.set(enabled); },
		"Preload Custom Textures\n"
		"Preloads custom textures at game start.\n"
		"Can reduce runtime stutter at the cost of increased memory usage and longer initial load times.",
		!config::CustomTextures);
	ImGui::Unindent();

		// Aspect Ratio section
		if (ImGui::CollapsingHeader(ICON_FA_TV " Aspect Ratio##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{

	RenderGeneralToggleSettingRow(
		"Widescreen",
		ICON_FA_TV,
		"Widescreen",
		"Renders a wider view (may reveal glitches).",
		static_cast<bool>(config::Widescreen),
		[](bool enabled) { config::Widescreen.set(enabled); },
		"Widescreen\n"
		"Draws geometry outside the normal 4:3 view to render a wider scene.\n"
		"Some games were not authored for widescreen, so newly revealed areas can show visual glitches, missing geometry, or incorrect culling.\n\n"
		"If a game looks broken in widescreen, disable this (or try Widescreen Game Cheats if supported).");

	ImGui::Indent();
	RenderGeneralToggleSettingRow(
		"SuperWidescreen",
		ICON_FA_EXPAND,
		"Super Widescreen",
		"Fills ultra-wide displays (more likely to reveal issues).",
		static_cast<bool>(config::SuperWidescreen),
		[](bool enabled) { config::SuperWidescreen.set(enabled); },
		"Super Widescreen\n"
		"Uses the full width of the screen/window when the aspect ratio is wider than 16:9.\n"
		"More likely to reveal out-of-bounds glitches than standard widescreen.\n\n"
		"Not compatible with Integer Scaling.",
		!config::Widescreen || config::IntegerScale);
	ImGui::Unindent();

	RenderGeneralToggleSettingRow(
		"WidescreenGameCheats",
		ICON_FA_CODE,
		"Widescreen Game Cheats",
		"Game-specific widescreen patches (when available).",
		static_cast<bool>(config::WidescreenGameHacks),
		[](bool enabled) { config::WidescreenGameHacks.set(enabled); },
		"Widescreen Game Cheats\n"
		"Modifies supported games to display in a 16:9 anamorphic format (game-specific patches).\n"
		"Only some games are supported, but when it works it can be cleaner than geometry widescreen.");

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
			"Fine-tune horizontal scaling (often 132% for 16:9 correction).",
			stretchPopupCfg,
			"Horizontal Stretching\n"
			"Stretches the image sideways.\n"
			"132% is a common value for 16:9 correction, but the best value depends on your display and personal preference.");
	}

		RenderGeneralToggleSettingRow(
			"RotateScreen",
			ICON_FA_ROTATE_RIGHT,
			"Rotate Screen 90°",
			"Rotate the entire output for rotated games.",
			static_cast<bool>(config::Rotate90),
			[](bool enabled) { config::Rotate90.set(enabled); },
			"Rotate Screen 90°\n"
			"Rotates the screen 90 degrees counterclockwise.\n"
			"Useful for games designed for rotated/vertical displays.");

			// Per Pixel Settings (only shown when Per Pixel sorting is enabled)
			if (perPixel && ImGui::CollapsingHeader(ICON_FA_IMAGE " Per Pixel Settings##Section", ImGuiTreeNodeFlags_DefaultOpen))
			{

			const std::array<int64_t, 4> bufSizes{ 512_MB, 1_GB, 2_GB, 4_GB };
			const std::array<const char*, 4> bufSizesText{ "512 MB", "1 GB", "2 GB", "4 GB" };
			int pixelBufferSelection = 0;
			for (int i = 0; i < static_cast<int>(bufSizes.size()); ++i)
			{
				if (bufSizes[i] == config::PixelBufferSize)
				{
					pixelBufferSelection = i;
					break;
				}
			}

			SettingsUI::PopupConfig pixelBufferCfg {};
			pixelBufferCfg.type = SettingsUI::PopupType::Options;
			pixelBufferCfg.options.label = "Pixel Buffer Size";
			pixelBufferCfg.options.icon = ICON_FA_MEMORY;
			pixelBufferCfg.options.popupID = "PixelBufferSizePopup";
			pixelBufferCfg.options.options = bufSizesText.data();
			pixelBufferCfg.options.optionCount = static_cast<int>(bufSizesText.size());
			pixelBufferCfg.options.currentValue = &pixelBufferSelection;
			pixelBufferCfg.options.valueWidth = 220.0f;
			pixelBufferCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
				if (selectedIndex < 0 || selectedIndex >= static_cast<int>(bufSizes.size()))
					return;
				SetSettingsFooterText(
					(selectedIndex == 0)
						? "Pixel Buffer Size\n512 MB reserves the least memory and is the fastest place to start. Use this if you want to minimize VRAM/RAM usage, but increase it if you see missing or incorrect transparency in heavier scenes."
						: (selectedIndex == 1)
							? "Pixel Buffer Size\n1 GB is a good middle ground if 512 MB is not enough. It gives Per-Pixel sorting more room for complex scenes without jumping straight to the larger memory costs."
							: (selectedIndex == 2)
								? "Pixel Buffer Size\n2 GB is useful for heavier scenes, higher internal resolutions, or games with lots of overlapping transparent effects. Use this if you still see transparency issues at 1 GB."
								: "Pixel Buffer Size\n4 GB is the largest buffer and is mainly for very demanding setups. It gives the most headroom for Per-Pixel sorting, but uses the most memory and is usually unnecessary unless you are pushing quality hard.");
			};
			pixelBufferCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= static_cast<int>(bufSizes.size()))
					return false;
				config::PixelBufferSize.set(bufSizes[selectedIndex]);
				return true;
			};

			RenderGeneralPopupSettingRow(
				"PixelBufferSizeSetting",
				"Reserved memory for Per-Pixel transparency rendering.",
				pixelBufferCfg,
				"Pixel Buffer Size\n"
				"Controls how much memory is reserved for Per-Pixel (OIT) transparency rendering.\n"
				"If you increase Internal Resolution a lot or see missing or incorrect transparency in complex scenes, you may need a larger buffer.\n"
				"Lower values reduce memory usage, but can limit transparency accuracy in heavier scenes.");

			static int perPixelLayersTemp = config::PerPixelLayers.get();
			if (!ImGui::IsPopupOpen("PerPixelMaximumLayersPopup"))
				perPixelLayersTemp = config::PerPixelLayers.get();

			SettingsUI::PopupSliderConfig perPixelLayersCfg {};
			perPixelLayersCfg.label = "Maximum Layers";
			perPixelLayersCfg.icon = ICON_FA_LAYER_GROUP;
			perPixelLayersCfg.popupID = "PerPixelMaximumLayersPopup";
			perPixelLayersCfg.description =
				"Controls how many transparent layers can be resolved per pixel in complex scenes.\n"
				"Increase this if you see missing transparency or incorrect layering.\n"
				"Lower values reduce memory pressure and can improve performance.";
			perPixelLayersCfg.currentValue = &perPixelLayersTemp;
			perPixelLayersCfg.minValue = 8;
			perPixelLayersCfg.maxValue = 128;
			perPixelLayersCfg.format = "%d";
			perPixelLayersCfg.valueWidth = 220.0f;
			perPixelLayersCfg.sliderWidth = 320.0f;
			perPixelLayersCfg.onValueChange = [&]() {
				config::PerPixelLayers.set(perPixelLayersTemp);
			};

			SettingsUI::PopupConfig perPixelLayersPopupCfg {};
			perPixelLayersPopupCfg.type = SettingsUI::PopupType::Slider;
			perPixelLayersPopupCfg.slider = perPixelLayersCfg;

			RenderGeneralPopupSettingRow(
				"PerPixelMaximumLayersSetting",
				"Maximum transparent layers resolved per pixel.",
				perPixelLayersPopupCfg,
				"Per-Pixel Maximum Layers\n"
				"Limits how many transparent layers can be resolved per pixel in complex scenes.\n"
				"Increase this if you see missing transparency or incorrect layering.\n"
				"Decrease it to improve performance and reduce memory pressure.");
			}
		}

		// Performance section
		if (ImGui::CollapsingHeader(ICON_FA_GAUGE_HIGH " Performance##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{
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
				"Auto-skip frames to maintain full-speed gameplay.",
				autoSkipCfg,
				"Automatic Frame Skipping\n"
				"Skips rendering frames when the emulator cannot keep up, to help maintain full-speed gameplay.\n\n"
				"Disabled: never auto-skip.\n"
				"Normal: skip when CPU and GPU are both slow.\n"
				"Maximum: skip when the GPU is slow.\n\n"
				"For best image quality, prefer lowering Internal Resolution before relying on heavy frame skipping.");
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
				"Force a fixed number of skipped frames.",
				frameSkipPopupCfg,
				"Frame Skipping\n"
				"Forces a fixed number of frames to be skipped between rendered frames.\n"
				"This is more aggressive and less adaptive than Automatic Frame Skipping.\n\n"
				"Use only if you know a title benefits from it, or as a last resort when tuning for speed.");
	}

	// 2x height toggle rows for Performance settings
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 12));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));

		// Shadows toggle
			RenderGeneralToggleSettingRow(
				"Shadows",
				ICON_FA_MOON,
				"Shadows",
				"Modifier volumes (often used for shadows).",
				static_cast<bool>(config::ModifierVolumes),
				[](bool enabled) { config::ModifierVolumes.set(enabled); },
				"Shadows (Modifier Volumes)\n"
				"Enables modifier volumes, which are usually used for shadowing and related effects.\n"
				"Disable if a game shows shadow-related artifacts, or if you need a little extra performance.");

		// Fog toggle
			RenderGeneralToggleSettingRow(
				"Fog",
				ICON_FA_CLOUD,
				"Fog",
				"Atmospheric fog effects.",
				static_cast<bool>(config::Fog),
				[](bool enabled) { config::Fog.set(enabled); },
				"Fog\n"
				"Enables atmospheric fog effects.\n"
				"Disable if you see fog artifacts, or if you need extra performance in heavy scenes.");

			ImGui::PopStyleVar(2);
		}

		// Advanced section
		if (ImGui::CollapsingHeader(ICON_FA_WAND_MAGIC " Advanced##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Spacing();

	// 2x height toggle rows for Advanced settings
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 12));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 8));

		// Delay Frame Swapping
			RenderGeneralToggleSettingRow(
				"DelayFrameSwapping",
				ICON_FA_CLOCK,
				"Delay Frame Swapping",
				"Reduce flashing and glitchy video playback.",
				static_cast<bool>(config::DelayFrameSwapping),
				[](bool enabled) { config::DelayFrameSwapping = enabled; },
				"Delay Frame Swapping\n"
				"Useful to avoid a flashing screen or glitchy videos in some titles.\n"
				"Not recommended on slow platforms.");

		// Fix Upscale Bleeding Edge
			RenderGeneralToggleSettingRow(
				"FixUpscaleBleedingEdge",
				ICON_FA_PLUG,
				"Fix Upscale Bleeding Edge",
				"Reduce texture bleeding when upscaling.",
				static_cast<bool>(config::FixUpscaleBleedingEdge),
				[](bool enabled) { config::FixUpscaleBleedingEdge = enabled; },
				"Fix Upscale Bleeding Edge\n"
				"Helps with texture bleeding when upscaling.\n"
				"Disable if pixels warp in 2D games (MVC2, CVS, KOF, etc.).");

		// Native Depth Interpolation
			RenderGeneralToggleSettingRow(
				"NativeDepthInterpolation",
				ICON_FA_MICROCHIP,
				"Native Depth Interpolation",
				"Fix depth/texture issues on some GPUs.",
				static_cast<bool>(config::NativeDepthInterpolation),
				[](bool enabled) { config::NativeDepthInterpolation = enabled; },
				"Native Depth Interpolation\n"
				"Helps with texture corruption and depth issues on some GPUs (often AMD, sometimes Intel).\n"
				"If you see depth-related flicker or corruption, try enabling this.");

		// Copy Rendered Textures to VRAM
			RenderGeneralToggleSettingRow(
				"RenderToTextureBuffer",
				ICON_FA_COPY,
				"Copy Rendered Textures",
				"Accuracy option for render-to-texture effects.",
				static_cast<bool>(config::RenderToTextureBuffer),
				[](bool enabled) { config::RenderToTextureBuffer = enabled; },
				"Copy Rendered Textures\n"
				"Copies render-to-texture results back into VRAM.\n"
				"Slower, but more accurate. Enable if a game has missing or incorrect render-to-texture effects.");

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
			"Sharper textures at steep angles (mipmapped textures only).",
			anisoCfg,
			"Anisotropic Filtering\n"
			"Makes mipmapped textures viewed at sharp angles look cleaner (less shimmer).\n"
			"Increases GPU cost.\n\n"
			"Only affects mipmapped textures. If you want this to do anything, keep mipmaps enabled.");
			}
		}

		// Texture Filtering
		if (ImGui::CollapsingHeader(ICON_FA_FILTER " Texture Filtering##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
			"Override how textures are filtered (sampled).",
			textureFilterCfg,
			"Texture Filtering\n"
			"Default: uses game behavior.\n"
			"Force Nearest-Neighbor: crisp/pixelated look.\n"
			"Force Linear: smoother/blurrier look.\n\n"
			"For 2D/pixel-art, many users prefer Nearest-Neighbor. For 3D, Linear can look nicer.");
		}

		// Show FPS Counter
		RenderGeneralToggleSettingRow(
			"ShowFPS",
			ICON_FA_GAUGE_HIGH,
			"Show FPS Counter",
			"Show FPS so you can tune for stable 30/60.",
			static_cast<bool>(config::ShowFPS),
			[](bool enabled) { config::ShowFPS.set(enabled); },
			"Show FPS Counter\n"
			"Displays an on-screen frame rate counter.\n"
			"Strongly recommended while tuning settings: aim for stable 30/60 FPS in real gameplay.");

#ifdef VIDEO_ROUTING
		// Video Routing section (platform-specific)
#ifdef __APPLE__
		if (ImGui::CollapsingHeader(ICON_FA_SHARE_NODES " Video Routing (Syphon)##Section", ImGuiTreeNodeFlags_DefaultOpen))
#elif defined(_WIN32)
		if (((renderApi == 0) || (renderApi == 3))
			? ImGui::CollapsingHeader(ICON_FA_SHARE_NODES " Video Routing (Spout)##Section", ImGuiTreeNodeFlags_DefaultOpen)
			: ImGui::CollapsingHeader(ICON_FA_SHARE_NODES " Video Routing (Only available with OpenGL or DirectX 11)##Section", ImGuiTreeNodeFlags_DefaultOpen))
#else
		if (ImGui::CollapsingHeader(ICON_FA_SHARE_NODES " Video Routing##Section", ImGuiTreeNodeFlags_DefaultOpen))
#endif
		{
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
				"Video Routing\n"
				"Routes a GPU texture directly to OBS Studio (or a compatible capture tool), avoiding CPU-intensive Display/Window Capture.\n"
				"This can improve capture quality and reduce overhead compared to capturing the emulator window.");

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
						"Scale Down Before Sending\n"
						"Shares a smaller texture to reduce capture bandwidth and GPU/CPU overhead.\n"
						"Can improve performance on some systems, but results vary.");
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
						"Output Vertical Resolution\n"
						"Sets the vertical resolution used for the shared texture routing output.\n"
						"Lower values reduce bandwidth and overhead. Higher values improve capture clarity.",
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
						BeginTwoLineSettingRow(
							"##row",
							"Output Texture Size\n"
							"Shows the calculated size of the shared output texture based on your routing settings.",
							true);
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
}
}
void renderAudioTab()
{
	ScopedTwoLineRowStyle audioRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Audio Configuration");
	ImGui::Separator();

	// Audio Playback Section
	if (ImGui::CollapsingHeader(ICON_FA_VOLUME_HIGH " Playback##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{

		RenderGeneralToggleSettingRow(
			"EnableDSP",
			ICON_FA_SLIDERS,
			"Enable DSP",
			"Dreamcast Digital Sound Processor",
			static_cast<bool>(config::DSPEnabled),
			[](bool enabled) { config::DSPEnabled.set(enabled); },
			"Enable DSP\n"
			"Enables the Dreamcast Digital Sound Processor.\n"
			"Only recommended on fast platforms. If you experience audio stutter or performance drops, try disabling this.");

		RenderGeneralToggleSettingRow(
			"EnableVmuSounds",
			ICON_FA_BELL,
			"VMU Sounds",
			"Play VMU beeps when enabled",
			static_cast<bool>(config::VmuSound),
			[](bool enabled) { config::VmuSound.set(enabled); },
			"VMU Sounds\n"
			"Plays VMU beep sounds when enabled.\n"
			"Purely cosmetic; disable if you prefer silence or want to avoid extra audio events.");

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
				"Master audio volume.",
				volumePopupCfg,
				"Volume Level\n"
				"Adjusts the emulator's master audio volume.\n"
				"If audio clips or is too quiet, tune this before changing latency settings.");
	}

	}

	const bool showAudioLatency = ImGui::CollapsingHeader(ICON_FA_CLOCK " Audio Latency##Section", ImGuiTreeNodeFlags_DefaultOpen);
	if (showAudioLatency)
	{
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
				"Automatic Latency\n"
				"Automatically chooses an audio buffer size.\n"
				"Recommended for most users. Disable only if you need to manually tune latency vs stability.");
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
				"Audio buffer size in milliseconds.",
				latencyPopupCfg,
				"Audio Latency\n"
				"Lower values reduce audio lag but can cause crackling/stutter if your system cannot keep up.\n"
					"Higher values are more stable but add latency.\n\n"
					"If you hear pops, increase latency. If audio feels delayed, decrease it carefully.");
		}
	}

		// Audio Driver Selection
	if (ImGui::CollapsingHeader(ICON_FA_HEADPHONES " Audio Driver##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{

	// Get current backend name for display
	AudioBackend *currentBackend = AudioBackend::getBackend(config::AudioBackend.get());
	std::string currentBackendName = currentBackend ? currentBackend->getName() : "Unknown";

	// Build dynamic options list
	std::vector<const char*> backendOptions;
	std::vector<std::string> backendLabels;
	for (u32 i = 0; i < AudioBackend::getCount(); i++)
	{
		const AudioBackend* backend = AudioBackend::getBackend(i);
		backendLabels.push_back(backend->slug + " - " + backend->getName());
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
			"Select the host audio backend/driver.",
			driverPopupCfg,
			"Audio Driver\n"
			"Selects the audio backend used to submit sound to your OS.\n"
			"If you have crackling, missing audio, or unusually high latency, try changing the driver.\n\n"
			"After changing drivers, re-check Audio Latency settings.");

	AudioBackend *current_backend = currentBackend;

	// Backend-specific options
	if (current_backend != nullptr)
	{
		// Get backend specific options
		int option_count;
		const AudioBackend::Option *options = current_backend->getOptions(&option_count);

			if (option_count > 0 && ImGui::CollapsingHeader(ICON_FA_SLIDERS " Driver Options##Section", ImGuiTreeNodeFlags_DefaultOpen))
			{
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
}

void renderControlsTab()
{
	ScopedTwoLineRowStyle controlsRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Controls Configuration");
	ImGui::Separator();

	if (ImGui::CollapsingHeader(ICON_FA_GAMEPAD " Physical Devices##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
	}

	ImGui::Spacing();

	if (ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Mouse & Raw Input##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{

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
					"Mouse/light gun sensitivity.",
					sensitivityPopupCfg,
					"Sensitivity\n"
					"Adjusts mouse and light gun sensitivity.\n"
					"Higher values make the cursor move faster.\n\n"
					"Tip: tune this in actual gameplay (not just menus) and keep it consistent across devices when possible.");
			}

#if defined(_WIN32) && !defined(TARGET_UWP)
			RenderGeneralToggleSettingRow(
				"UseRawInput",
				ICON_FA_CROSSHAIRS,
				"Use Raw Input",
				"Supports multiple pointing devices (mice, light guns) and keyboards",
				static_cast<bool>(config::UseRawInput),
				[](bool enabled) { config::UseRawInput.set(enabled); },
				"Use Raw Input\n"
				"Enables raw input so multiple pointing devices (mice, light guns) and keyboards can be handled more accurately.\n"
				"Recommended if you use light guns or multiple input devices.");
#endif
	}

	ImGui::Spacing();

	if (ImGui::CollapsingHeader(ICON_FA_PLUG " Dreamcast Devices##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{

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

	static constexpr int MDT_DreamPotato = 100;
	static constexpr int MDT_DreamLink = 101;

	static const char *maple_expansion_device_types[] =
	{
		"None",
		"Sega VMU",
		"Vibration Pack",
		"Microphone",
		"DreamPotato",
		"DreamLink", // not shown unless a DreamLink controller is present
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
		case MDT_DreamPotato: return maple_expansion_device_types[4];
		case MDT_DreamLink: return maple_expansion_device_types[5];
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
		case 4: return (MapleDeviceType)MDT_DreamPotato;
		case 5: return (MapleDeviceType)MDT_DreamLink;
		case 0: default: return MDT_None;
		}
	};

	bool is_there_any_xhair = false;

	if (ImGui::BeginTable("dreamcastDevices", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings,
			ImVec2(0, 0), uiScaled(8)))
	{
		// DreamLink device names for main device
		const char* dream_link_names[MAPLE_PORTS]{};
		for (int bus = 0; bus < MAPLE_PORTS; bus++)
		{
			auto link = MapleLinkRegistry::GetMapleLink(bus, MAPLE_MAIN_DEV_IDX); // Registered controller, if any
			if (link && (link->dreamlink->getIssueDescription() == nullptr))
				dream_link_names[bus] = link->dreamlink->getName();
			else
				dream_link_names[bus] = "";
		}

		const float mainComboWidth = ImGui::CalcTextSize("Densha de Go! Controller").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
		const float expComboWidth = ImGui::CalcTextSize("Vibration Pack").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();

		// Settings 3-22: Dreamcast Device Ports (4 ports x device + expansion)
		for (int bus = 0; bus < MAPLE_PORTS; bus++)
		{
			const bool has_dream_link = (*dream_link_names[bus] != '\0');
			const char* selected_name = nullptr;

			if (has_dream_link)
				selected_name = dream_link_names[bus];
			else
				selected_name = maple_device_name(config::MapleMainDevices[bus]);

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::Text(T("Port %c"), bus + 'A');

			ImGui::TableSetColumnIndex(1);
			char device_name[32];
			snprintf(device_name, sizeof(device_name), "##device%d", bus);
			float w = ImGui::CalcItemWidth() / 3;
			ImGui::PushItemWidth(w);
			ImGui::SetNextItemWidth(mainComboWidth);

			if (has_dream_link)
			{
				// Using real hardware for this - disable selection
				ImGui::BeginDisabled();
			}

			if (ImGui::BeginCombo(device_name, selected_name, ImGuiComboFlags_None))
			{
				for (int i = 0; i < IM_ARRAYSIZE(maple_device_types); i++)
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

			int port_count = 0;
			int port_type_count = 0;
			if (has_dream_link)
			{
				ImGui::EndDisabled();
				port_count = 2;
				port_type_count = IM_ARRAYSIZE(maple_expansion_device_types);
			}
			else
			{
				port_count = maple_getPortCount(config::MapleMainDevices[bus]);
				// Remove DreamLink as an option
				port_type_count = IM_ARRAYSIZE(maple_expansion_device_types) - 1;
			}

			for (int port = 0; port < port_count; port++)
			{
				const bool port_has_dream_link = has_dream_link && (MapleLinkRegistry::GetMapleLink(bus, port) != std::nullopt);

				ImGui::TableSetColumnIndex(2 + port);
				snprintf(device_name, sizeof(device_name), "##device%d.%d", bus, port + 1);
				ImguiID _(device_name);
				ImGui::SetNextItemWidth(expComboWidth);
				int subtype = config::MapleExpansionDevices[bus][port];
				if (subtype == MDT_SegaVMU && config::NetworkExpansionDevices[bus][port] == 1) {
					subtype = MDT_DreamPotato;
				}
				else if (port_has_dream_link && (config::DreamLinkSelect[bus][port])) {
					subtype = MDT_DreamLink;
				}

				if (ImGui::BeginCombo(device_name, maple_expansion_device_name((MapleDeviceType)subtype), ImGuiComboFlags_None))
				{
					for (int i = 0; i < port_type_count; i++)
					{
						bool is_selected = subtype == maple_expansion_device_type_from_index(i);
						if (ImGui::Selectable(maple_expansion_device_types[i], &is_selected))
						{
							subtype = maple_expansion_device_type_from_index(i);
							if (subtype == MDT_DreamLink) {
								config::DreamLinkSelect[bus][port] = true;
							}
							else if (port_has_dream_link) {
								config::DreamLinkSelect[bus][port] = false;
							}

							if (subtype == MDT_DreamPotato) {
								config::MapleExpansionDevices[bus][port] = MDT_SegaVMU;
								config::NetworkExpansionDevices[bus][port] = 1;
							}
							else {
								if (subtype != MDT_DreamLink) {
									config::MapleExpansionDevices[bus][port] = (MapleDeviceType)subtype;
								}
								config::NetworkExpansionDevices[bus][port] = 0;
							}

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
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
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
					"Crosshair size for light guns.",
					crosshairPopupCfg,
					"Crosshair Size\n"
					"Adjusts the size of the on-screen crosshair for light guns.\n"
					"Only available when a light gun device is connected.\n\n"
					"Choose a size that is visible without covering targets.",
					!is_there_any_xhair);
			}

	ImGui::Spacing();
	if (ImGui::CollapsingHeader(ICON_FA_MICROCHIP " VMU Settings##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
			RenderGeneralToggleSettingRow(
				"PerGameVmu",
				ICON_FA_MICROCHIP,
				"Per Game VMU A1",
				"When enabled, each game has its own VMU on port 1 of controller A",
				static_cast<bool>(config::PerGameVmu),
				[](bool enabled) { config::PerGameVmu.set(enabled); },
				"Per Game VMU\n"
				"When enabled, each game has its own VMU on port 1 of controller A.\n"
				"Useful to prevent save-file conflicts between games.");

#ifdef USE_DREAMLINK_DEVICES
	{
			RenderGeneralToggleSettingRow(
				"UsePhysicalVmuMemory",
				ICON_FA_MEMORY,
				T("Use External VMU Storage"),
				T("Enables read and write access to physical/external VMU storage via DreamPicoPort or DreamPotato. "
					"VMUs may appear to reconnect after loading state."),
				static_cast<bool>(config::UsePhysicalVmuMemory),
				[](bool enabled) { config::UsePhysicalVmuMemory.set(enabled); },
				T("Use External VMU Storage\n"
				"Enables read and write access to physical/external VMU storage via DreamPicoPort or DreamPotato. "
					"VMUs may appear to reconnect after loading state."),
				game_started);
	}
#endif
	}
	}
}

void renderNetworkTab()
{
	ScopedTwoLineRowStyle networkRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Network Configuration");
	ImGui::Separator();

	if (ImGui::CollapsingHeader(ICON_FA_GLOBE " Network Type##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
				"Choose which network feature is active.",
				networkTypeCfg,
				"Network Type\n"
				"Selects the active network mode:\n"
				"GGPO for rollback netplay, Naomi for Naomi network games, or Battle Cable.\n\n"
				"Only one network type should be enabled at a time.");
	}

	// Show configuration section if any network type is enabled
	if (config::GGPOEnable || config::NetworkEnable || config::BattleCableEnable)
	{
		ImGui::Spacing();

		if (ImGui::CollapsingHeader(ICON_FA_SLIDERS " Configuration##Section", ImGuiTreeNodeFlags_DefaultOpen))
		{

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
					"Play as Player 1\n"
					"When enabled, you host the session (Player 1).\n"
					"Deselect to connect as Player 2.");

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
					},
					"GGPO Peer\n"
					"Enter your peer's IP address (and optional port).\n"
					"Use this when connecting directly without matchmaking.");

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
						"Extra buffering for higher ping sessions.",
						ggpoDelayPopupCfg,
						"Frame Delay\n"
						"Adds a fixed frame delay to help stabilize gameplay for higher-latency connections.\n"
						"Often advisable when ping is > 100 ms.\n\n"
						"Too much delay increases input lag; use the lowest value that feels stable.");
			}

				// Analog Axes Configuration
				if (ImGui::CollapsingHeader(ICON_FA_GAMEPAD " Analog Axes##Section", ImGuiTreeNodeFlags_DefaultOpen))
				{
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
						"Control how the left thumbstick is used in GGPO.",
						axesCfg,
						"Left Thumbstick (GGPO)\n"
						"Configures how the left thumbstick is mapped during GGPO sessions:\n"
						"Disabled, Horizontal only, or Full analog axes.\n\n"
						"Choose the option that matches the game and your preferred control style.");
			}

				RenderGeneralToggleSettingRow(
					"EnableChat",
					ICON_FA_COMMENTS,
					"Enable Chat",
					"Open chat on message received",
					static_cast<bool>(config::GGPOChat),
					[](bool enabled) { config::GGPOChat.set(enabled); },
					"Enable Chat\n"
					"Opens the chat window when a message is received during GGPO sessions.");

			if (config::GGPOChat)
			{
					RenderGeneralToggleSettingRow(
						"GGPOChatTimeoutToggle",
						ICON_FA_HOURGLASS,
						"Chat Auto-Close",
						"Automatically close chat window",
						static_cast<bool>(config::GGPOChatTimeoutToggle),
						[](bool enabled) { config::GGPOChatTimeoutToggle = enabled; },
						"Chat Auto-Close\n"
						"Automatically closes the chat window after a timeout.\n"
						"Useful if you want chat notifications without leaving the overlay open.");

					if (config::GGPOChatTimeoutToggle)
					{
						ImGui::PushID("GGPOChatTimeoutRow");
						const bool rowActivated = BeginTwoLineSettingRow(
							"##row",
							"Chat Window Timeout\n"
							"Controls how long the chat window stays open after receiving a new message.\n\n"
							"Increase this if you want more time to read messages.\n"
							"Decrease it if you only want brief notifications without the overlay staying on screen.");
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
			}

				RenderGeneralToggleSettingRow(
					"NetworkStats",
					ICON_FA_CHART_BAR,
					"Network Statistics",
					"Display netplay stats on screen",
					static_cast<bool>(config::NetworkStats),
					[](bool enabled) { config::NetworkStats = enabled; },
					"Network Statistics\n"
					"Shows a small on-screen overlay with live netplay information.\n"
					"This is useful for diagnosing stutter, desyncs, or “why does this feel laggy?” situations.\n\n"
					"If you are tuning settings for a stable experience, enable this along with the FPS counter and test during real gameplay.\n"
					"Look for stable latency/jitter and minimal dropped or delayed packets.");
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
					"Naomi Netplay: Act as Server\n"
					"When enabled, this device hosts the Naomi network session and waits for another player to connect.\n\n"
					"If you cannot connect to each other, check firewalls/NAT rules and confirm both players are using the same game/version.\n"
					"Use the Local Port setting if you need to match a specific port or avoid conflicts.");

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
						},
						"Naomi Netplay: Server Address\n"
						"Enter the host to connect to (for example: a hostname or an IP address).\n"
						"If a port is supported by your setup, it is typically written as `host:port`.\n\n"
						"Leave this field blank to use automatic discovery (when available) on the default port.\n"
						"If you have connection issues, try entering the host explicitly and verify your Local Port and firewall/NAT rules.");
				}

			// Local Port
				{
					ImGui::PushID("NaomiLocalPortRow");
					const bool rowActivated = BeginTwoLineSettingRow(
						"##row",
						"Local Port\n"
						"Sets the local UDP port used for Naomi netplay / Battle Cable connections.\n\n"
						"If you cannot connect, make sure both players are using compatible settings and that your firewall/router is not blocking this port.\n"
						"Only change this if you have a port conflict or you need to match a specific setup.");
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
					ShowFooterHelpMarker("Choose a unique word or number and share it with your opponent");

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
							"Network Statistics\n"
							"Shows a small on-screen overlay with live connection information.\n\n"
							"Use this to diagnose lag spikes, unstable connections, or dropped packets during Battle Cable sessions.\n"
							"For best results, enable the FPS counter too and test during actual gameplay.");

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
							},
							"Battle Cable: Peer Address\n"
							"Enter the opponent to connect to (hostname or IP address).\n"
							"If your setup supports specifying a port, it is typically written as `host:port`.\n\n"
							"Leave this field blank to use automatic discovery (when available) on the default port.\n"
							"If matchmaking fails, try entering the peer explicitly and verify your Local Port and firewall/NAT rules.");

						{
							ImGui::PushID("BattleLocalPortRow");
							const bool rowActivated = BeginTwoLineSettingRow(
								"##row",
								"Local Port\n"
								"Sets the local UDP port used for Battle Cable sessions.\n\n"
								"If you cannot connect, verify firewall/router rules and ensure both peers are using the same port.\n"
								"Leave this at the default unless you have a reason to change it.");
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
					"Maximum Speed: Master Peer\n"
					"Only used by Maximum Speed mode.\n"
					"Exactly one peer must be the master for the session to work correctly.\n\n"
					"If you run into connection problems, try switching which player is master.\n"
					"In general, the player with the more stable connection and lower latency is a good choice.");
			}
		}

	ImGui::Spacing();

	if (ImGui::CollapsingHeader(ICON_FA_GEAR " Network Options##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"EnableUPnP",
			ICON_FA_NETWORK_WIRED,
			"Enable UPnP",
			"Automatically configure your network router for netplay",
			static_cast<bool>(config::EnableUPnP),
			[](bool enabled) { config::EnableUPnP.set(enabled); },
			"Enable UPnP\n"
			"Attempts to automatically create the required port mappings on your router.\n\n"
			"This can make hosting/connecting easier on home networks, but it depends on your router supporting UPnP and allowing it.\n"
			"If it does not work (or if you prefer not to use UPnP), disable this and use manual port forwarding instead.");

		RenderGeneralToggleSettingRow(
			"BroadcastOutputs",
			ICON_FA_TOWER_BROADCAST,
			"Broadcast Digital Outputs",
			"Broadcast audio/video",
			static_cast<bool>(config::NetworkOutput),
			[](bool enabled) { config::NetworkOutput.set(enabled); },
			"Broadcast Digital Outputs\n"
			"Exports certain arcade I/O “outputs” over the network (for example, values used by some Naomi driving/force-feedback setups).\n\n"
			"This is not a general gameplay video/audio streaming feature.\n"
			"Enable it only if you are using external tooling or hardware that expects network output data.");

		RenderGeneralToggleSettingRow(
			"BroadbandAdapter",
			ICON_FA_SERVER,
			"Broadband Adapter",
			"Emulate the Ethernet Broadband Adapter (BBA) instead of the Modem",
			static_cast<bool>(config::EmulateBBA),
			[](bool enabled) { config::EmulateBBA.set(enabled); },
			"Broadband Adapter (BBA)\n"
			"Emulates the Dreamcast Ethernet Broadband Adapter (BBA) instead of the modem.\n\n"
			"Use this for games and features that expect Ethernet networking.\n"
			"If a title expects the modem path (or you are troubleshooting connectivity), try toggling this and re-testing.",
			game_started);

		RenderGeneralToggleSettingRow(
			"DCNet",
			ICON_FA_CLOUD,
			"DCNet Cloud",
			"Dreamcast Internet via cloud",
			static_cast<bool>(config::UseDCNet),
			[](bool enabled) { config::UseDCNet.set(enabled); },
			"DCNet Cloud\n"
			"Routes Dreamcast networking through a cloud-backed service instead of relying entirely on your local network configuration.\n\n"
			"This can simplify getting “online” in certain scenarios, but it also changes the networking path.\n"
			"If you have trouble connecting, try toggling this setting and re-testing with the same game and the same network options.");

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
			"DNS Server\n"
			"Overrides the DNS server used by the emulated network stack.\n\n"
			"Only change this if you know you need a specific DNS provider or you are debugging connectivity.\n"
			"Note: When DCNet Cloud is enabled, DNS behavior may be handled differently and this field may not apply.",
			config::UseDCNet);
#endif
	}

#ifdef NAOMI_MULTIBOARD
	// Multiboard Screens
	ImGui::Spacing();

	if (ImGui::CollapsingHeader(ICON_FA_TABLE " Multiboard##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		OptionRadioButton<int>("1 (Twin)", config::MultiboardSlaves, 1,
			"One screen configuration (F355 Twin)");
		ImGui::SameLine();
		OptionRadioButton<int>("3 (Deluxe)", config::MultiboardSlaves, 2,
			"Three screens configuration");
	}
#endif
}

void renderAdvancedTab()
{
	ScopedTwoLineRowStyle advancedRowStyle(20.0f, true, 0.5f, 8.0f);

	ImGui::TextDisabled("Advanced Configuration");
	ImGui::Separator();

	// CPU & Emulation Section
	if (ImGui::CollapsingHeader(ICON_FA_MICROCHIP " CPU & Emulation##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
				cpuModeCfg.options.onOptionHighlight = [](int idx, const char*)
				{
					static const char* kHelp[] = {
						"CPU Mode: Dynarec\n"
						"Recommended for almost everyone.\n"
						"Uses the dynamic recompiler (JIT) for the best performance.\n\n"
						"If a specific game is crashing or behaving incorrectly, switching to the Interpreter can be a useful compatibility test.",
						"CPU Mode: Interpreter\n"
						"Highest compatibility, lowest performance.\n"
						"Runs the SH4 in interpreter mode which is much slower than Dynarec.\n\n"
						"Use this mainly for troubleshooting hard-to-reproduce bugs, or for rare titles that misbehave under Dynarec.",
					};
					if (idx >= 0 && idx < (int)std::size(kHelp))
						SetSettingsFooterText(kHelp[idx]);
				};
					cpuModeCfg.options.onChange = [&](int selectedIndex) {
						config::DynarecEnabled.set(selectedIndex == 0);
						return true;
					};

				RenderGeneralPopupSettingRow(
					"CpuModeSetting",
					"Use the dynamic recompiler for speed, or interpreter for compatibility.",
					cpuModeCfg,
					"CPU Mode\n"
					"Controls how Flycast emulates the SH4 CPU.\n\n"
					"Dynarec is dramatically faster and is the normal choice for gameplay.\n"
					"Interpreter is much slower, but can be helpful for compatibility testing.\n\n"
					"If you are troubleshooting a game-specific issue, switch modes and re-test the exact same scene to compare behavior.");
			}

			// SH4 Clock
			{
				static int sh4ClockTemp = config::Sh4Clock.get();

				SettingsUI::PopupSliderConfig sh4ClockCfg {};
				sh4ClockCfg.label = "SH4 Clock";
				sh4ClockCfg.icon = ICON_FA_GAUGE_HIGH;
				sh4ClockCfg.popupID = "Sh4ClockPopup";
				sh4ClockCfg.description =
					"Adjusts the emulated SH4 CPU clock. Default is 200 MHz.\n"
					"Higher values may reduce CPU-side frame drops in some games, but can also break timing.\n"
					"Lower values can reduce CPU load, but may cause slowdowns or audio/video instability.";
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
					sh4ClockPopupCfg,
					"SH4 Clock\n"
					"Overclocks/underclocks the emulated Dreamcast CPU.\n\n"
					"Default is 200 MHz.\n"
					"Increasing the clock can help some CPU-limited games maintain full speed, but it can also introduce timing problems, bugs, or instability.\n"
					"Decreasing the clock can lower CPU requirements, but may reduce performance or cause glitches.\n\n"
					"If you change this, re-test gameplay with the FPS counter enabled and keep adjustments small.");
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
				"HLE BIOS\n"
				"Uses a High Level Emulation BIOS implementation instead of a real BIOS.\n\n"
				"This can reduce CPU overhead and simplifies setup, but it can be less accurate than using a real BIOS.\n"
				"If you encounter game-specific boot issues or unusual behavior, try disabling HLE BIOS (and using a real BIOS if available) for maximum compatibility.");

			// Multi-threaded emulation
				RenderGeneralToggleSettingRow(
					"MultiThreaded",
					ICON_FA_GEAR,
					"Multi-threading",
					"Use additional CPU threads",
					static_cast<bool>(config::ThreadedRendering),
					[](bool enabled) { config::ThreadedRendering.set(enabled); },
					"Multi-threading\n"
					"Allows Flycast to use more than one CPU thread for certain emulation/rendering work.\n\n"
					"On modern multi-core devices this can improve performance or reduce stutter.\n"
					"On some drivers/devices it can introduce instability or odd timing issues.\n\n"
					"If you see new crashes, visual glitches, or inconsistent frame pacing after enabling it, try turning it off and re-testing.");

		ImGui::Spacing();

		// Dreamcast 32MB RAM Mod (with warning)
			RenderGeneralToggleSettingRow(
				"ExtraRAM",
				ICON_FA_SD_CARD,
				"32MB RAM",
				"Extended memory (32MB)",
				static_cast<bool>(config::RamMod32MB),
				[](bool enabled) { config::RamMod32MB.set(enabled); },
				"32MB RAM (Memory Mod)\n"
				"Enables an extended-memory mode (not standard Dreamcast hardware).\n\n"
				"Only use this if a specific title/homebrew explicitly requires it.\n"
				"Because it changes the memory map, it can break games, cause graphical corruption, or lead to crashes.\n\n"
				"If you are unsure, leave this disabled.",
				game_started);
		}

	// Debugging Section
#if defined(GDB_SERVER) || !defined(__ANDROID__)
	if (ImGui::CollapsingHeader(ICON_FA_BUG " Debugging##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
				"Serial Console\n"
				"Enables serial console output for debugging.\n\n"
				"Most users should leave this off.\n"
				"Enable it only if you are debugging, capturing logs, or using tools that rely on serial output.");
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
				"GDB Server\n"
				"Starts a GDB debug server for developers.\n\n"
				"When enabled, Flycast can pause startup and wait for a debugger to attach.\n"
				"Leave this disabled unless you are actively debugging.");

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
					"Wait for Connection\n"
					"Delays emulation until the debugger connects to the GDB server.\n\n"
					"Useful when you need to catch very early boot issues.\n"
					"Disable this for normal play.");

				{
					ImGui::PushID("GDBPortRow");
					const bool rowActivated = BeginTwoLineSettingRow(
						"##row",
						"GDB Port\n"
						"Sets the TCP port used by the built-in GDB debug server.\n\n"
						"Default is 3263.\n"
						"Only change this if you have a port conflict or you need to match your debugging tool configuration.");
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
					"Log to File\n"
					"Saves log output to a file on disk.\n\n"
					"This is helpful when you need to share logs for troubleshooting, but it can increase I/O and may reduce performance on slower storage.\n"
					"If you enable this, try to reproduce the issue, then disable it again to avoid unnecessary disk usage.");
		}
	#endif

	// Logging Section (Debug builds only)
#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER
	if (ImGui::CollapsingHeader(ICON_FA_FILE_LINES " Logging##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
				logVerbosityCfg.options.onOptionHighlight = [](int idx, const char*)
				{
					static const char* kHelp[] = {
						"Log Verbosity: Notice\n"
						"Minimal information.\n"
						"Best for normal usage when you only want important notices.",
						"Log Verbosity: Error\n"
						"Only errors.\n"
						"Useful when you want to reduce log noise and focus on failures.",
						"Log Verbosity: Warning\n"
						"Errors and warnings.\n"
						"A good balance for general troubleshooting with moderate verbosity.",
						"Log Verbosity: Info\n"
						"Adds informational messages.\n"
						"Helpful when investigating subsystems without going full debug spam.",
						"Log Verbosity: Debug\n"
						"Most verbose.\n"
						"Can be very noisy and may impact performance; use temporarily when debugging.",
					};
					if (idx >= 0 && idx < (int)std::size(kHelp))
						SetSettingsFooterText(kHelp[idx]);
				};
					logVerbosityCfg.options.onChange = [&](int selectedIndex) {
						LogManager::GetInstance()->SetLogLevel((LogTypes::LOG_LEVELS)(selectedIndex + 1));
						cfgSaveInt("log", "Verbosity", selectedIndex + 1);
						return true;
					};

				RenderGeneralPopupSettingRow(
					"LogVerbositySetting",
					"Control the verbosity of log output",
					logVerbosityCfg,
					"Log Verbosity\n"
					"Controls how much information Flycast writes to the log.\n\n"
					"Lower verbosity is cleaner and faster.\n"
					"Higher verbosity is useful when diagnosing problems, but can generate a lot of output (and can impact performance in extreme cases).\n\n"
					"For troubleshooting, start with Warning or Info, then use Debug only if you need deeper detail.");
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
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
					ImVec2(0, rowHeight));
				ImGui::PopStyleVar();

				if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
					SetSettingsFooterText(fullName.c_str());

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
		ShowFooterHelpMarker("Log to this hostname[:port] with UDP. Default port is 31667.");
	}
#endif

	// Profiling Section
#if FC_PROFILER
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(ICON_FA_CHART_LINE " Profiling##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
			RenderGeneralToggleSettingRow(
				"ProfilerEnabled",
				ICON_FA_CHART_LINE,
				"Enable Profiler",
				"Performance profiling",
				static_cast<bool>(config::ProfilerEnabled),
				[](bool enabled) { config::ProfilerEnabled.set(enabled); },
				"Profiler\n"
				"Enables performance profiling tools for developers and power users.\n\n"
				"This can add overhead. Leave it off unless you are actively profiling performance.");
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
					"Profiler: Display in GUI\n"
					"Draws profiler output as an in-game overlay.\n\n"
					"Useful for quickly spotting spikes or expensive subsystems while reproducing a performance issue.");

			// Output to terminal - 2x Row Pattern
				RenderGeneralToggleSettingRow(
					"ProfilerOutputTTY",
					ICON_FA_TERMINAL,
					"Output to Terminal",
					"Write profiler to terminal",
					static_cast<bool>(config::ProfilerOutputTTY),
					[](bool enabled) { config::ProfilerOutputTTY.set(enabled); },
					"Profiler: Output to Terminal\n"
					"Writes profiler output to the terminal/console.\n\n"
					"Useful when capturing data during automated runs or when you want to copy/paste profiling output.");

			ImGui::Unindent();
		}
	}
#endif

	// Experimental Features Section (with warnings)
	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Experimental Features##Section", ImGuiTreeNodeFlags_DefaultOpen))
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
				"Texture Dumping\n"
				"Saves textures to disk as they are used.\n\n"
				"This is mainly for creating or debugging custom texture packs.\n"
				"It can create a large number of files quickly and may reduce performance due to disk I/O.\n\n"
				"Enable it only when you specifically need a dump, then disable it again.");
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
				"Dump Replaced Textures\n"
				"Also dumps textures even when they are being replaced by a custom texture pack.\n\n"
				"This is useful when comparing originals vs replacements, but it increases disk usage even more.\n"
				"Leave this off unless you are actively working on textures.");

			RenderGeneralToggleSettingRow(
				"DiscardVideoAndAnimatedTextures",
				ICON_FA_COPY,
				T("Discard Video and Animated Textures"),
				T("Skip dumping video (YUV) and already updated textures"),
				static_cast<bool>(config::DumpUniqueTextures),
				[](bool enabled) { config::DumpUniqueTextures.set(enabled); },
				T("Skip dumping video (YUV) and already updated textures")
			);

			ImGui::Unindent();
		}
		ImGui::Unindent();
	}

	// Developer Tools Section
	ImGui::Spacing();
	if (ImGui::CollapsingHeader("Developer Tools##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const std::array<const char*, 3> monitorModeLabels {
			"Off",
			"Minimal",
			"Full"
		};
		int monitorModeSelection = getResourceMonitorMode();
		if (monitorModeSelection < 0 || monitorModeSelection >= static_cast<int>(monitorModeLabels.size()))
			monitorModeSelection = 0;

		SettingsUI::PopupConfig monitorModeCfg {};
		monitorModeCfg.type = SettingsUI::PopupType::Options;
		monitorModeCfg.options.label = "Overlay Mode";
		monitorModeCfg.options.icon = ICON_FA_CHART_LINE;
		monitorModeCfg.options.popupID = "ResourceMonitorModePopup";
		monitorModeCfg.options.options = monitorModeLabels.data();
		monitorModeCfg.options.optionCount = static_cast<int>(monitorModeLabels.size());
		monitorModeCfg.options.currentValue = &monitorModeSelection;
		monitorModeCfg.options.valueWidth = 220.0f;
		monitorModeCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= 3)
				return false;
			setResourceMonitorMode(selectedIndex);
			return true;
		};

		RenderGeneralPopupSettingRow(
			"ResourceMonitorMode",
			"Real-time performance overlay updated once per second. Minimal: CPU/GPU/FPS. Full: CPU/GPU/frame time/FPS/VRAM/RAM.",
			monitorModeCfg,
			"Resource Monitor Overlay\n"
			"Displays a real-time performance overlay updated once per second.\n\n"
			"Minimal shows CPU, GPU, and FPS.\n"
			"Full also shows average frame time plus memory usage (VRAM/RAM).\n\n"
			"Use this when tuning settings like Internal Resolution, Sorting, and Frame Skipping to keep performance stable.");

#ifdef USE_LUA
		// Lua Scripting
		ImGui::Text("Lua Scripting");
		InputText("Lua Filename", &config::LuaFileName.get(), ImGuiInputTextFlags_CharsNoBlank);
		ImGui::SameLine();
		ShowFooterHelpMarker("Specify lua filename to use. Should be located in Flycast config folder. Defaults to flycast.lua when empty.");
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
				"Crash Reports\n"
				"Automatically uploads crash reports to help developers diagnose and fix issues.\n\n"
				"Crash reports typically include technical details about the crash and environment.\n"
				"Leave this enabled if you want to help improve stability; disable it if you prefer not to send crash data.");
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
	if (ImGui::CollapsingHeader(ICON_FA_TAG " Version Information##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("Version: %s", GIT_VERSION);
		ImGui::Text("Git Hash: %s", GIT_HASH);
		ImGui::Text("Build Date: %s", BUILD_DATE);
		ImGui::Spacing();
	}

	// Platform Information
	if (ImGui::CollapsingHeader(ICON_FA_COMPUTER " Platform##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
	}

	// Graphics Information
	{
		const char *graphicsTitle = ICON_FA_DISPLAY " Graphics##Section";
		if (isOpenGL(config::RendererType))
			graphicsTitle = ICON_FA_DISPLAY " Graphics - OpenGL##Section";
		else if (isVulkan(config::RendererType))
			graphicsTitle = ICON_FA_DISPLAY " Graphics - Vulkan##Section";
		else if (isDirectX(config::RendererType))
			graphicsTitle = ICON_FA_DISPLAY " Graphics - DirectX##Section";

		if (ImGui::CollapsingHeader(graphicsTitle, ImGuiTreeNodeFlags_DefaultOpen))
		{
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
		}
	}

	// Project Description
	if (ImGui::CollapsingHeader("About##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped(
			"Hollycast is a multi-platform emulator for Sega Dreamcast, "
			"Naomi, Naomi 2, and Atomiswave arcade systems."
		);
		ImGui::TextWrapped(
			"Based on Flycast, providing accurate emulation with enhancements "
			"for modern systems."
		);
		ImGui::Spacing();
	}

	// Links Section
	if (ImGui::CollapsingHeader(ICON_FA_LINK " Links##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
	}

	// License Information
	if (ImGui::CollapsingHeader(ICON_FA_FILE_CONTRACT " License##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
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
	}

	// Credits
	if (ImGui::CollapsingHeader(ICON_FA_USERS " Credits##Section", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped(
			"This emulator is based on the work of many talented developers "
			"who have contributed to the Flycast project and its dependencies."
		);
		ImGui::TextWrapped(
			"Special thanks to the original Flycast team and all contributors "
			"who make this project possible."
		);
	}

	ImGui::PopStyleVar();
}

// Main entry point - renders the entire settings UI
void renderSettingsNew()
{
	if (g_settingsFooterText.empty())
		ResetSettingsFooter();

	// Set up full-screen window
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);

	// Main settings window
	if (ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		const std::function<void()> exitSettings = [&]() {
			dreampotato::update();
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

			SaveSettings();

			if (game_started)
				gui_setState(GuiState::Commands);
			else
				gui_setState(GuiState::Main);
		};

		const char* backLabel = game_started ? T("Back to Game") : T("Back to Library");

		// Split into left navigation rail and right content area
		renderNavigationRail(exitSettings, backLabel);
		ImGui::SameLine();
		renderContentArea();
	}
	ImGui::End();
}

} // namespace SettingsNew
