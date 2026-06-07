/*
	Copyright 2024 flyinghead
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
#include "gui_util.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>

#include "types.h"
#include "stdclass.h"
#include "oslib/oslib.h"
#include "oslib/directory.h"
#include "oslib/storage.h"
#include "oslib/http_client.h"
#include "oslib/i18n.h"
#include "imgui_driver.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_stdlib.h"
#include "gui_font.h"
#include "stdclass.h"
#include "rend/osd.h"
#include <stb_image.h>

using namespace i18n;

static std::string select_current_directory = "**home**";
static std::vector<hostfs::FileInfo> subfolders;
static std::vector<hostfs::FileInfo> folderFiles;
bool subfolders_read;
static std::mutex g_storageCallbackMutex;
static StringCallback g_storageCallback;

extern int insetLeft, insetRight, insetTop, insetBottom;
void error_popup();

static void storage_popup_callback(bool cancelled, std::string selectedPath)
{
	StringCallback callback;

	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		callback = g_storageCallback;
		g_storageCallback = {};
	}

	if (callback)
		callback(cancelled, selectedPath);
}

static ImFont* SettingsDescriptionFont()
{
	return settingsTitleFont != nullptr ? settingsTitleFont : largeFont;
}

static ImFont* PopupEmphasisFont()
{
	return settingsValueFont != nullptr ? settingsValueFont : largeFont;
}

static ImFont* SettingsRightValueFont()
{
	return settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;
}

namespace hostfs
{
	bool operator<(const FileInfo& a, const FileInfo& b) {
		return locale()(a.name, b.name);
	}
}

void select_file_popup(const char *prompt, const StringCallback& callback,
		bool selectFile, const std::string& selectExtension)
{
	fullScreenWindow(true);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_FramePadding, ImVec2(4, 3)); // default

	if (ImGui::BeginPopup(prompt, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize ))
	{
		static std::string error_message;

		if (select_current_directory == "**home**")
			select_current_directory = hostfs::storage().getDefaultDirectory();

		if (!subfolders_read)
		{
			subfolders.clear();
            folderFiles.clear();
			error_message.clear();

			try {
				for (const hostfs::FileInfo& entry : hostfs::storage().listContent(select_current_directory))
				{
					if (entry.isDirectory)
					{
						subfolders.push_back(entry);
					}
					else
					{
						std::string extension = get_file_extension(entry.name);
						if (selectFile)
						{
							if (extension == selectExtension)
								folderFiles.push_back(entry);
						}
						else if (extension == "zip" || extension == "7z" || extension == "chd"
								|| extension == "gdi" || extension == "cdi" || extension == "cue"
								|| (!config::HideLegacyNaomiRoms
										&& (extension == "bin" || extension == "lst" || extension == "dat")))
							folderFiles.push_back(entry);
					}
				}
			} catch (const hostfs::StorageException& e) {
				error_message = e.what();
			}

			std::sort(subfolders.begin(), subfolders.end());
			std::sort(folderFiles.begin(), folderFiles.end());
			subfolders_read = true;
		}
		if (prompt != nullptr) {
			ImguiStyleVar _(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
			ImguiStyleVar _1(ImGuiStyleVar_DisabledAlpha, 1.0f);
			ImGui::BeginDisabled();
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::ButtonEx(prompt, ImVec2(-1, 0));
			ImGui::PopFont();
			ImGui::EndDisabled();
		}
		std::string title;
		if (!error_message.empty())
			title = error_message;
		else if (select_current_directory.empty())
			title = T("Storage");
		else
			title = select_current_directory;

		ImGui::Text("%s", title.c_str());
		ImGui::BeginChild(ImGui::GetID("dir_list"), ImVec2(0, - uiScaled(30) - ImGui::GetStyle().ItemSpacing.y),
				ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
		{
			ImguiStyleVar _(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 20));

			if (!select_current_directory.empty() && select_current_directory != "/")
			{
				if (ImGui::Selectable(T(".. Up to Parent Folder")))
				{
					subfolders_read = false;
					select_current_directory = hostfs::storage().getParentPath(select_current_directory);
				}
			}

			for (const auto& entry : subfolders)
			{
				if (ImGui::Selectable(entry.name.c_str()))
				{
					subfolders_read = false;
					select_current_directory = entry.path;
				}
			}
			ImguiStyleColor _1(ImGuiCol_Text, { 1, 1, 1, selectFile ? 1.f : 0.3f });
			for (const auto& entry : folderFiles)
			{
				if (selectFile)
				{
					if (ImGui::Selectable(entry.name.c_str()))
					{
						subfolders_read = false;
						if (callback && callback(false, entry.path))
							ImGui::CloseCurrentPopup();
					}
				}
				else
				{
					ImGui::Text("%s", entry.name.c_str());
				}
			}
			scrollWhenDraggingOnVoid();
			windowDragScroll();
		}
		ImGui::EndChild();
		if (!selectFile)
		{
			if (ImGui::Button(T("Select Current Folder"), ScaledVec2(0, 30)))
			{
				if (callback && callback(false, select_current_directory))
				{
					subfolders_read = false;
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SameLine();
		}
		if (ImGui::Button(T("Cancel"), ScaledVec2(0, 30)))
		{
			subfolders_read = false;
			if (callback)
				callback(true, "");
			ImGui::CloseCurrentPopup();
		}
		error_popup();
		ImGui::EndPopup();
	}
}

StoragePopupResult select_storage_popup(bool isDirectory, bool writeAccess, const std::string& description,
		const StringCallback& callback, const std::string& mimeType)
{
	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		if (g_storageCallback)
			return StoragePopupResult::CallbackAlreadySet;
		g_storageCallback = callback;
	}

	const bool supported = hostfs::addStorage(isDirectory, writeAccess, description, &storage_popup_callback, mimeType);
	if (!supported)
	{
		std::lock_guard<std::mutex> lock(g_storageCallbackMutex);
		g_storageCallback = {};
	}
	return supported ? StoragePopupResult::Supported : StoragePopupResult::Unsupported;
}

// See https://github.com/ocornut/imgui/issues/3379
void scrollWhenDraggingOnVoid(ImGuiMouseButton mouse_button)
{
	ImGuiContext& g = *ImGui::GetCurrentContext();
	ImGuiWindow* window = g.CurrentWindow;
	while (window != nullptr
			&& (window->Flags & ImGuiWindowFlags_ChildWindow)
			&& !(window->Flags & ImGuiWindowFlags_DragScrolling)
			&& window->ScrollMax.x == 0.0f
			&& window->ScrollMax.y == 0.0f)
		window = window->ParentWindow;
	if (window == nullptr || !(window->Flags & ImGuiWindowFlags_DragScrolling))
		return;
    bool hovered = false;
    bool held = false;
    ImGuiButtonFlags button_flags = (mouse_button == ImGuiMouseButton_Left) ? ImGuiButtonFlags_MouseButtonLeft
    		: (mouse_button == ImGuiMouseButton_Right) ? ImGuiButtonFlags_MouseButtonRight : ImGuiButtonFlags_MouseButtonMiddle;
    // If nothing hovered so far in the frame (not same as IsAnyItemHovered()!) or item is disabled
	if (g.HoveredId == 0 || g.HoveredIdIsDisabled)
    {
    	bool hoveredAllowOverlap = g.HoveredIdAllowOverlap;
    	g.HoveredIdAllowOverlap = true;
    	ImGuiID overlayId = window->GetID("##scrolldraggingoverlay");
    	ImGui::ButtonBehavior(window->Rect(), overlayId, &hovered, &held, button_flags);
    	ImGui::KeepAliveID(overlayId);
    	g.HoveredIdAllowOverlap = hoveredAllowOverlap;
    }
    const ImVec2& delta = ImGui::GetIO().MouseDelta;
    if (held && delta != ImVec2())
    {
    	window->DragScrolling = true;
    	window->ScrollSpeed = delta;
    }
}

// Helper to display a little (?) mark which shows a tooltip when hovered.
void ShowHelpMarker(const char* desc)
{
    ImGui::TextDisabled("%s", T("(?)"));
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

template<bool PerGameOption>
bool OptionCheckbox(const char *name, config::Option<bool, PerGameOption>& option, const char *help)
{
	bool pressed;
	{
		DisabledScope scope(option.isReadOnly());

		bool b = option;
		pressed = ImGui::Checkbox(name, &b);
		if (pressed)
			option.set(b);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return pressed;
}
template bool OptionCheckbox(const char *name, config::Option<bool, true>& option, const char *help);
template bool OptionCheckbox(const char *name, config::Option<bool, false>& option, const char *help);

template<bool PerGameOption>
bool OptionSlider(const char *name, config::Option<int, PerGameOption>& option, int min, int max, const char *help, const char *format)
{
	bool valueChanged;
	{
		DisabledScope scope(option.isReadOnly());

		int v = option;
		valueChanged = ImGui::SliderInt(name, &v, min, max, format);
		if (valueChanged)
			option.set(v);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return valueChanged;
}
template bool OptionSlider(const char *name, config::Option<int, true>& option, int min, int max, const char *help, const char *format);
template bool OptionSlider(const char *name, config::Option<int, false>& option, int min, int max, const char *help, const char *format);

bool OptionArrowButtons(const char *name, config::Option<int>& option, int min, int max, const char *help, const char *format)
{
	const float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
	const std::string id = "##" + std::string(name);
	{
		ImguiStyleVar _(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
		ImguiStyleColor _1(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
		const float width = ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f;
		ImguiStyleVar _2(ImGuiStyleVar_DisabledAlpha, 1.0f);
		ImGui::BeginDisabled();
		std::string value = strprintf(format, (int)option);
		ImGui::ButtonEx((value + id).c_str(), ImVec2(width, 0));
		ImGui::EndDisabled();
	}

	ImGui::SameLine(0.0f, innerSpacing);
	ImGui::PushButtonRepeat(true);
	bool valueChanged = false;
	{
		DisabledScope scope(option.isReadOnly());

		if (ImGui::ArrowButton((id + "left").c_str(), ImGuiDir_Left)) { option.set(std::max(min, option - 1)); valueChanged = true; }
		ImGui::SameLine(0.0f, innerSpacing);
		if (ImGui::ArrowButton((id + "right").c_str(), ImGuiDir_Right)) { option.set(std::min(max, option + 1)); valueChanged = true; }
	}
	ImGui::PopButtonRepeat();
	ImGui::SameLine(0.0f, innerSpacing);
	ImGui::Text("%s", name);
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return valueChanged;
}

template<typename T>
bool OptionRadioButton(const char *name, config::Option<T>& option, T value, const char *help)
{
	bool pressed;
	{
		DisabledScope scope(option.isReadOnly());

		int v = (int)option;
		pressed = ImGui::RadioButton(name, &v, (int)value);
		if (pressed)
			option.set((T)v);
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
	return pressed;
}
template bool OptionRadioButton<bool>(const char *name, config::Option<bool>& option, bool value, const char *help);
template bool OptionRadioButton<int>(const char *name, config::Option<int>& option, int value, const char *help);

template<bool PerGameOption>
void OptionComboBox(const char *name, config::Option<int, PerGameOption>& option, const char *values[], int count,
			const char *help)
{
	{
		DisabledScope scope(option.isReadOnly());

		const char *value = option >= 0 && option < count ? values[option] : "?";
		if (ImGui::BeginCombo(name, value, ImGuiComboFlags_None))
		{
			for (int i = 0; i < count; i++)
			{
				bool is_selected = option == i;
				if (ImGui::Selectable(values[i], &is_selected))
					option = i;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
	}
	if (help != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(help);
	}
}

// Explicit template instantiations
template void OptionComboBox<true>(const char *name, config::Option<int, true>& option, const char *values[], int count, const char *help);
template void OptionComboBox<false>(const char *name, config::Option<int, false>& option, const char *values[], int count, const char *help);

bool SettingsRow(const char* label, const char* currentValue, const char* helpText)
{
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);

	// Create selectable that spans both columns (full row is clickable)
	std::string selectableId = std::string("##row_") + label;
	bool clicked = ImGui::Selectable(selectableId.c_str(), false,
	                                 ImGuiSelectableFlags_SpanAllColumns |
	                                 ImGuiSelectableFlags_AllowOverlap);

	// Calculate position for label (left column)
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	float labelX = cursorPos.x + ImGui::GetStyle().CellPadding.x;
	float labelY = cursorPos.y + ImGui::GetStyle().FramePadding.y;

	// Draw label
	ImGui::SetCursorScreenPos(ImVec2(labelX, labelY));
	ImGui::TextUnformatted(label);

	// Calculate position for value (right column)
	float valueX = cursorPos.x + ImGui::GetColumnWidth(0) + ImGui::GetStyle().CellPadding.x * 2.0f;
	float valueY = cursorPos.y + ImGui::GetStyle().FramePadding.y;

	// Draw current value
	ImGui::SetCursorScreenPos(ImVec2(valueX, valueY));
	ImGui::TextUnformatted(currentValue);

	// Help marker
	if (helpText != nullptr)
	{
		float valueWidth = ImGui::CalcTextSize(currentValue).x;
		ImGui::SetCursorScreenPos(ImVec2(valueX + valueWidth + ImGui::GetStyle().ItemSpacing.x, valueY));
		ShowHelpMarker(helpText);
	}

	return clicked;
}

int SelectionPopup(const char* popupId, const char* title, const char* options[], int optionCount, int currentSelection)
{
	int selectedIndex = -1;

	if (!ImGui::IsPopupOpen(popupId))
		return -1;

	// Center the popup
	centerNextWindow();

	// Set popup size (auto width, constrained height)
	float maxWidth = ImGui::GetIO().DisplaySize.x * 0.8f;
	float maxHeight = ImGui::GetIO().DisplaySize.y * 0.6f;
	ImGui::SetNextWindowSize(ImVec2(maxWidth, 0), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(200, 0), ImVec2(maxWidth, maxHeight));

	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
								   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

	if (ImGui::BeginPopupModal(popupId, nullptr, windowFlags))
	{
		// Title
		if (title != nullptr)
		{
			ImGui::TextUnformatted(title);
			ImGui::Separator();
			ImGui::Spacing();
		}

		// Options list
		for (int i = 0; i < optionCount; i++)
		{
			bool isSelected = (i == currentSelection);
			if (ImGui::Selectable(options[i], isSelected, ImGuiSelectableFlags_DontClosePopups))
			{
				selectedIndex = i;
				ImGui::CloseCurrentPopup();
			}
			if (isSelected)
				ImGui::SetItemDefaultFocus();
		}

		// Close on Escape or click outside
		if (ImGui::IsKeyPressed(ImGuiKey_Escape)
			|| (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
				&& !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)))
		{
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	return selectedIndex;
}

void fullScreenWindow(bool modal)
{
	if (!modal)
	{
		ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
		ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);

		if (insetLeft > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(insetLeft, ImGui::GetIO().DisplaySize.y));
			ImGui::Begin("##insetLeft", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetRight > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - insetRight, 0));
			ImGui::SetNextWindowSize(ImVec2(insetRight, ImGui::GetIO().DisplaySize.y));
			ImGui::Begin("##insetRight", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetTop > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, 0));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, insetTop));
			ImGui::Begin("##insetTop", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
		if (insetBottom > 0)
		{
			ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y - insetBottom));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, insetBottom));
			ImGui::Begin("##insetBottom", nullptr, ImGuiWindowFlags_NoDecoration);
			ImGui::End();
		}
	}
	// Position the main window below the menu bar to avoid covering it
	float menuBarHeight = ImGui::GetFrameHeight();  // Standard menu bar height
	ImGui::SetNextWindowPos(ImVec2(insetLeft, insetTop + menuBarHeight));
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x - insetLeft - insetRight, ImGui::GetIO().DisplaySize.y - insetTop - insetBottom - menuBarHeight));
}

static void computeScrollSpeed(float &v)
{
	constexpr float friction = 3.f;
	if (std::abs(v) > friction)
	{
		float sign = (v > 0.f) - (v < 0.f);
		v -= friction * sign;
	}
	else
	{
		v = 0.f;
	}
}

void windowDragScroll()
{
	ImGuiWindow *window = ImGui::GetCurrentWindow();
	if (window->DragScrolling)
	{
		if (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left])
		{
			computeScrollSpeed(window->ScrollSpeed.x);
			computeScrollSpeed(window->ScrollSpeed.y);
			if (window->ScrollSpeed == ImVec2())
			{
				window->DragScrolling = false;
				// FIXME we should really move the mouse off-screen after a touch up and this wouldn't be necessary
				// the only problem is tool tips
				gui_set_mouse_position(-1, -1, true);
			}
		}
		else
		{
			ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
			if (delta != ImVec2())
				ImGui::ResetMouseDragDelta();
			window->ScrollSpeed = delta;
		}
		if (window->DragScrolling)
		{
			ImGui::SetScrollX(window, window->Scroll.x - window->ScrollSpeed.x);
			ImGui::SetScrollY(window, window->Scroll.y - window->ScrollSpeed.y);
		}
	}
}

static void setUV(float ar, ImVec2& uv0, ImVec2& uv1)
{
	uv0 = { 0.f, 0.f };
	uv1 = { 1.f, 1.f };
	if (ar > 1)
	{
		uv0.y = -(ar - 1) / 2;
		uv1.y = 1 + (ar - 1) / 2;
	}
	else if (ar != 0)
	{
		ar = 1 / ar;
		uv0.x = -(ar - 1) / 2;
		uv1.x = 1 + (ar - 1) / 2;
	}
}

void ImguiTexture::draw(const ImVec2& size, const ImVec4& tint_col, const ImVec4& border_col)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		ImGui::Dummy(size);
	else
	{
		const float ar = imguiDriver->getAspectRatio(id);
		ImVec2 drawSize(size);
		if (size.x == 0.f)
			drawSize.x = size.y * ar;
		else if (size.y == 0.f)
			drawSize.y = size.x / ar;
		ImVec2 uv0, uv1;
		setUV(ar / drawSize.x * drawSize.y, uv0, uv1);
		ImGui::Image(id, drawSize, uv0, uv1, tint_col, border_col);
	}
}

void ImguiTexture::draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size, float alpha)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		return;
	const float ar = imguiDriver->getAspectRatio(id);
	ImVec2 uv0, uv1;
	setUV(ar / size.x * size.y, uv0, uv1);
	u32 col = alphaOverride(0xffffff, alpha);
	drawList->AddImage(id, pos, pos + size, uv0, uv1, col);
}

void ImguiTexture::draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size,
		const ImVec2& uv0, const ImVec2& uv1, const ImVec4& color)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		return;
	u32 col = ImGui::ColorConvertFloat4ToU32(color);
	drawList->AddImage(id, pos, pos + size, uv0, uv1, col);
}

bool ImguiTexture::button(const char* str_id, const ImVec2& image_size, const std::string& title,
		const ImVec4& bg_col, const ImVec4& tint_col)
{
	ImTextureID id = getId();
	if (id == ImTextureID{})
		return ImGui::Button(title.c_str(), image_size);
	else
	{
		const float ar = imguiDriver->getAspectRatio(id);
		const ImVec2 size = image_size - ImGui::GetStyle().FramePadding * 2;
		ImVec2 uv0, uv1;
		setUV(ar / size.x * size.y, uv0, uv1);
		return ImGui::ImageButton(str_id, id, size, uv0, uv1, bg_col, tint_col);
	}
}

static u8 *loadImage(const std::string& path, int& width, int& height)
{
	FILE *file = nowide::fopen(path.c_str(), "rb");
	if (file == nullptr)
		return nullptr;

	int channels;
	stbi_set_flip_vertically_on_load_thread(0);
	u8 *imgData = stbi_load_from_file(file, &width, &height, &channels, STBI_rgb_alpha);
	std::fclose(file);
	return imgData;
}

int ImguiFileTexture::textureLoadCount;

ImTextureID ImguiFileTexture::getId()
{
	if (path.empty())
		return {};
	ImTextureID id = imguiDriver->getTexture(path);
	if (id == ImTextureID() && textureLoadCount < 10)
	{
		textureLoadCount++;
		int width, height;
		u8 *imgData = loadImage(path, width, height);
		if (imgData != nullptr)
		{
			try {
				id = imguiDriver->updateTextureAndAspectRatio(path, imgData, width, height, nearestSampling);
			} catch (...) {
				// vulkan can throw during resizing
			}
			free(imgData);
		}
	}
	return id;
}

std::future<ImguiStateTexture::LoadedPic> ImguiStateTexture::asyncLoad;

bool ImguiStateTexture::exists()
{
	std::string path = hostfs::getSavestatePath(config::SavestateSlot, false);
	return hostfs::storage().exists(path);
}

ImTextureID ImguiStateTexture::getId()
{
	std::string path = hostfs::getSavestatePath(config::SavestateSlot, false);
	ImTextureID texid = imguiDriver->getTexture(path);
	if (texid != ImTextureID())
		return texid;
	if (asyncLoad.valid())
	{
		if (asyncLoad.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
			return {};
		LoadedPic loadedPic = asyncLoad.get();
		if (loadedPic.data != nullptr)
		{
			try {
				texid = imguiDriver->updateTextureAndAspectRatio(path, loadedPic.data, loadedPic.width, loadedPic.height, nearestSampling);
			} catch (...) {
				// vulkan can throw during resizing
			}
			free(loadedPic.data);
		}
		return texid;
	}
	asyncLoad = std::async(std::launch::async, []() {
		LoadedPic loadedPic{};
		// load savestate info
		std::vector<u8> pngData;
		dc_getStateScreenshot(config::SavestateSlot, pngData);
		if (pngData.empty())
			return loadedPic;

		int channels;
		stbi_set_flip_vertically_on_load_thread(0);
		loadedPic.data = stbi_load_from_memory(&pngData[0], pngData.size(), &loadedPic.width, &loadedPic.height, &channels, STBI_rgb_alpha);

		return loadedPic;
	});
	return {};
}

void ImguiStateTexture::invalidate()
{
	if (imguiDriver)
	{
		std::string path = hostfs::getSavestatePath(config::SavestateSlot, false);
		imguiDriver->deleteTexture(path);
	}
}

std::array<ImguiVmuTexture, 8> ImguiVmuTexture::Vmus { 0, 1, 2, 3, 4, 5, 6, 7 };
constexpr float VMU_WIDTH = 96.f;
constexpr float VMU_HEIGHT = 64.f;
constexpr float VMU_PADDING = 8.f;

ImTextureID ImguiVmuTexture::getId()
{
	if (!vmu_lcd_status[index])
		return {};
	if (idPath.empty())
		idPath = ":vmu:" + std::to_string(index);
	ImTextureID texid = imguiDriver->getTexture(idPath);
	if (texid == ImTextureID() || vmuLastChanged != ::vmuLastChanged[index])
	{
		try {
			texid = imguiDriver->updateTexture(idPath, (const u8 *)vmu_lcd_data[index], 48, 32, true);
			vmuLastChanged = ::vmuLastChanged[index];
		} catch (...) {
		}
	}
	return texid;
}

void ImguiVmuTexture::displayVmus(const ImVec2& pos)
{
	const ScaledVec2 size(VMU_WIDTH, VMU_HEIGHT);
	const float padding = uiScaled(VMU_PADDING);
	assert((ImGui::GetCurrentWindow()->Flags & ImGuiWindowFlags_ChildWindow) == 0);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 cpos(pos + ScaledVec2(2.f, 0));	// 96 pixels wide + 2 * 2 -> 100
	for (int i = 0; i < 8; i++)
	{
		if (!vmu_lcd_status[i])
			continue;

		ImTextureID texid = Vmus[i].getId();
		if (texid == ImTextureID())
			continue;
		ImVec2 pos_b = cpos + size;
		dl->AddImage(texid, cpos, pos_b, ImVec2(0, 1), ImVec2(1, 0), 0x80ffffff);
		cpos.y += size.y + padding;
	}
}

void Toast::show(const std::string& title, const std::string& message, u32 durationMs)
{
	const u64 now = getTimeMs();
	std::lock_guard<std::mutex> _{mutex};
	// no start anim if still visible
	if (now > endTime + END_ANIM_TIME)
		startTime = getTimeMs();
	endTime = now + durationMs;
	this->title = title;
	this->message = message;
}

bool Toast::draw()
{
	const u64 now = getTimeMs();
	std::lock_guard<std::mutex> _{mutex};
	if (now > endTime + END_ANIM_TIME) {
		title.clear();
		message.clear();
	}
	if (title.empty() && message.empty())
		return false;
	float alpha = 1.f;
	if (now > endTime)
		// Fade out
		alpha = (std::cos((now - endTime) / (float)END_ANIM_TIME * (float)M_PI) + 1.f) / 2.f;

	const ImVec2 displaySize(ImGui::GetIO().DisplaySize);
	const float maxW = std::min(uiScaled(640.f), displaySize.x);
	ImFont *regularFont = ImGui::GetFont();
	const ImVec2 titleSize = title.empty() ? ImVec2()
			: ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, maxW, &title.front(), &title.back() + 1);
	const ImVec2 msgSize = message.empty() ? ImVec2()
			: regularFont->CalcTextSizeA(regularFont->LegacySize, FLT_MAX, maxW, &message.front(), &message.back() + 1);
	const ScaledVec2 padding(5.f, 4.f);
	const ScaledVec2 spacing(0.f, 2.f);
	ImVec2 totalSize(std::max(titleSize.x, msgSize.x), titleSize.y + msgSize.y);
	totalSize += padding * 2.f + spacing * (float)(!title.empty() && !message.empty());

	ImVec2 pos(insetLeft, displaySize.y - totalSize.y);
	if (now - startTime < START_ANIM_TIME)
		// Slide up
		pos.y += totalSize.y * (std::cos((now - startTime) / (float)START_ANIM_TIME * (float)M_PI) + 1.f) / 2.f;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImU32 bg_col = alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), alpha / 2.f);
	dl->AddRectFilled(pos, pos + totalSize, bg_col, 0.f);
	const ImU32 col = alphaOverride(ImGui::GetColorU32(ImGuiCol_Border), alpha);
	dl->AddRect(pos, pos + totalSize, col, 0.f);

	pos += padding;
	if (!title.empty())
	{
		const ImU32 col = alphaOverride(ImGui::GetColorU32(ImGuiCol_Text), alpha);
		dl->AddText(nullptr, uiLargeFontSize(), pos, col, &title.front(), &title.back() + 1, maxW);
		pos.y += spacing.y + titleSize.y;
	}
	if (!message.empty())
	{
		const ImU32 col = alphaOverride(0xFF00FFFF, alpha);	// yellow
		dl->AddText(regularFont, regularFont->LegacySize, pos, col, &message.front(), &message.back() + 1, maxW);
	}

	return true;
}

std::string middleEllipsis(const std::string& s, float width)
{
	float tw = ImGui::CalcTextSize(s.c_str()).x;
	if (tw <= width)
		return s;
	char buf[5];
	ImTextCharToUtf8(buf, ImGui::GetFont()->EllipsisChar);
	std::string ellipsis = buf;

	int l = s.length() / 2;
	int d = l;

	while (true)
	{
		std::string ss = s.substr(0, l / 2) + ellipsis + s.substr(s.length() - l / 2 - (l & 1));
		tw = ImGui::CalcTextSize(ss.c_str()).x;
		if (tw == width)
			return ss;
		d /= 2;
		if (d == 0)
			return ss;
		if (tw > width)
			l -= d;
		else
			l += d;
	}
}

bool beginFrame(const char *label, const ImVec2& size_arg, ImVec2 *out_size)
{
	using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return false;
    const ImGuiStyle& style = g.Style;
	const ImVec2 label_size = CalcTextSize(label, nullptr, true);
    ImVec2 size = ImTrunc(CalcItemSize(size_arg, CalcItemWidth(), GetTextLineHeightWithSpacing() * 7.25f + style.FramePadding.y * 2.0f));
    ImVec2 frame_size = ImVec2(size.x, ImMax(size.y, label_size.y));
    ImRect frame_bb(window->DC.CursorPos, window->DC.CursorPos + frame_size);
    ImRect bb(frame_bb.Min, frame_bb.Max + ImVec2(label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f, 0.0f));
    window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, bb.Max);

    BeginGroup();
    if (label_size.x > 0.0f)
    {
        ImVec2 label_pos = ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x, frame_bb.Min.y + style.FramePadding.y);
        RenderText(label_pos, label);
        window->DC.CursorMaxPos = ImMax(window->DC.CursorMaxPos, label_pos + label_size);
    }

    const ImU32 bg_col = GetColorU32(ImGuiCol_FrameBg);
    window->DrawList->AddRectFilled(frame_bb.Min, frame_bb.Max, bg_col, g.Style.FrameRounding, 0);
    window->DC.CursorPos += style.FramePadding;
    PushClipRect(frame_bb.Min + style.FramePadding, frame_bb.Max - style.FramePadding, false);
    if (out_size != nullptr)
    	*out_size = frame_size - style.FramePadding * 2.f;
    BeginGroup();

    return true;
}

void endFrame()
{
	using namespace ImGui;
	EndGroup();
	PopClipRect();
	EndGroup();
}

#ifdef __SWITCH__

static constexpr unsigned Flags_Multiline = 1 << 31;

bool switchEditText(char *value, size_t capacity, ImGuiInputTextFlags flags, bool multiline);

static int switchInputTextCallback(ImGuiInputTextCallbackData *data)
{
	if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways && (data->Flags & ImGuiInputTextFlags_ReadOnly) == 0)
	{
		data->Buf[data->BufTextLen] = '\0';
		if (switchEditText(data->Buf, data->BufSize, data->Flags, (data->Flags & Flags_Multiline) != 0))
		{
			data->BufDirty = true;
			data->BufTextLen = strlen(data->Buf);
			ImGui::ClearActiveID();
			return 1;
		}
		ImGui::ClearActiveID();
	}
	return 0;
}
#endif

bool InputText(const char *label, std::string *str, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
	{
		// TODO This doesn't handle growing the string capacity dynamically
		str->reserve(512);
		return ImGui::InputText(label, str, flags | ImGuiInputTextFlags_CallbackAlways, switchInputTextCallback);
	}
#endif
	return ImGui::InputText(label, str, flags, callback, user_data);
}

bool InputText(const char *label, char *str, size_t size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
		return ImGui::InputText(label, str, size, flags | ImGuiInputTextFlags_CallbackAlways, switchInputTextCallback);
#endif
	return ImGui::InputText(label, str, size, flags, callback, user_data);
}

bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data)
{
#ifdef __SWITCH__
	if ((flags & ImGuiInputTextFlags_ReadOnly) == 0)
		return ImGui::InputTextMultiline(label, buf, buf_size, size, flags | ImGuiInputTextFlags_CallbackAlways | Flags_Multiline, switchInputTextCallback);
#endif
	return ImGui::InputTextMultiline(label, buf, buf_size, size, flags, callback, user_data);
}

// ============================================================================
// Phase 0 Widget Infrastructure Components
// ============================================================================

void SectionDivider(const char* text)
{
	ImGuiStyle& style = ImGui::GetStyle();

	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Separator, style.Colors[ImGuiCol_Border]);
	ImGui::Separator();
	ImGui::PopStyleColor();

	if (text != nullptr && text[0] != '\0')
	{
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_Text]);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	}

	ImGui::Spacing();
}

void SectionHeaderWithIcon(const char* icon, const char* text)
{
	ImGui::Spacing();

	// Icon + text with highlighted color
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
	ImGui::Text("%s %s", icon, text);
	ImGui::PopStyleColor();

	// Separator line
	ImGui::Separator();

	ImGui::Spacing();
}

void SettingIcon(const char* icon, const ImVec2& size)
{
	ImGuiStyle& style = ImGui::GetStyle();

	ImVec2 iconSize = size;
	if (iconSize.x <= 0 || iconSize.y <= 0)
		iconSize = ImVec2(settings.display.uiScale * 16, settings.display.uiScale * 16);

	ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_Text]);
	ImGui::Text("%s", icon);
	ImGui::PopStyleColor();
}

void BeginSettingsRow(const SettingsRowParams& params)
{
	ImGui::BeginChild(params.label, ImVec2(0, params.minHeight > 0 ? params.minHeight : settings.display.uiScale * 48),
		ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);

	// Draw icon if provided
	if (params.icon != nullptr)
	{
		ImGui::Text("%s", params.icon);
		ImGui::SameLine(0, settings.display.uiScale * 12);
	}

	// Draw label (bold)
	ImGui::TextUnformatted(params.label);

	// Reserve space for control on right
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - settings.display.uiScale * 100);
}

void EndSettingsRow()
{
	ImGui::EndChild();
	ImGui::Spacing();
}

bool ToggleSwitch(const char* label, bool* value, const char* helpText)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return false;

	ImGuiContext& g = *GImGui;
	const ImGuiID id = window->GetID(label);

	const float height = settings.display.uiScale * 24;
	const float width = settings.display.uiScale * 48;
	const float radius = height * 0.5f;
	const ImVec2 pos = window->DC.CursorPos;
	const ImVec2 size(width, height);

	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);
	if (!ImGui::ItemAdd(bb, id))
		return false;

	bool hovered, held;
	bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);

	if (pressed)
	{
		*value = !*value;
		return true;
	}

	// Animation
	float anim = 0;
	if (g.LastActiveId == id)
	{
		float t = ImMin((float)(g.Time - g.LastActiveIdTimer) / 0.15f, 1.0f);
		anim = *value ? t : (1.0f - t);
	}
	else
	{
		anim = *value ? 1.0f : 0.0f;
	}

	// Render
	ImU32 col_bg;
	if (*value)
		col_bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
	else
		col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);

	// Knob - use theme-aware color
	ImVec2 knob_pos;
	knob_pos.x = ImLerp(bb.Min.x + radius, bb.Max.x - radius, anim);
	knob_pos.y = bb.Min.y + radius;

	// Detect light vs dark theme
	ImGuiStyle& guiStyle = ImGui::GetStyle();
	float bgLuminance = (guiStyle.Colors[ImGuiCol_WindowBg].x
	                   + guiStyle.Colors[ImGuiCol_WindowBg].y
	                   + guiStyle.Colors[ImGuiCol_WindowBg].z) / 3.0f;
	bool isLightTheme = bgLuminance > 0.5f;

	ImU32 knobColor = isLightTheme
		? ImGui::GetColorU32(ImGuiCol_Text)  // Dark text color for light themes
		: IM_COL32(255, 255, 255, 255);      // White for dark themes
	draw_list->AddCircleFilled(knob_pos, radius - 1.0f, knobColor);

	return pressed;
}

template<bool PerGameOption>
bool SettingsOption(const char* label, config::Option<bool, PerGameOption>& option,
                   const char* description, const char* icon, const char* helpText)
{
	BeginSettingsRow({label, description, icon, nullptr, settings.display.uiScale * 48});

	bool value = option;
	bool changed = ToggleSwitch(("##" + std::string(label)).c_str(), &value);
	if (changed)
		option = value;

	EndSettingsRow();

	if (helpText != nullptr)
	{
		ImGui::SameLine();
		ShowHelpMarker(helpText);
	}

	return changed;
}

// Explicit template instantiations for SettingsOption
template bool SettingsOption<true>(const char* label, config::Option<bool, true>& option,
                                   const char* description, const char* icon, const char* helpText);
template bool SettingsOption<false>(const char* label, config::Option<bool, false>& option,
                                    const char* description, const char* icon, const char* helpText);

namespace SettingsUI {

namespace Detail {

// DuckStation-style layout constants
namespace Layout {
    constexpr float SMALL_POPUP_PADDING = 20.0f;
    constexpr float MENU_BUTTON_PADDING = 8.0f;
    constexpr float MENU_BUTTON_HEIGHT = 28.0f;
    constexpr float MENU_BUTTON_SPACING = 8.0f;
    constexpr float WIDGET_FRAME_ROUNDING = 4.0f;
    constexpr float POPUP_WIDTH = 600.0f;
    constexpr float POPUP_MIN_WIDTH = 300.0f;
    constexpr float POPUP_ROUNDING = 18.0f;
    constexpr float POPUP_VALUE_RIGHT_PADDING = 28.0f;
}

// Color helper functions (DuckStation-style)
static inline ImVec4 ModAlpha(const ImVec4& v, float a)
{
    return ImVec4(v.x, v.y, v.z, a);
}

static inline u32 ModAlpha(u32 col32, float a)
{
    return (col32 & ~IM_COL32_A_MASK) | (static_cast<u32>(a * 255.0f) << IM_COL32_A_SHIFT);
}

static inline ImVec4 DarkerColor(const ImVec4& v, float f = 0.8f)
{
    return ImVec4(std::max(v.x, 1.0f / 255.0f) * f,
                std::max(v.y, 1.0f / 255.0f) * f,
                std::max(v.z, 1.0f / 255.0f) * f, v.w);
}

// Helper: Apply DuckStation-style popup styling
struct PopupStyleScope {
    PopupStyleScope(float padding = Layout::SMALL_POPUP_PADDING, float rounding = Layout::POPUP_ROUNDING) {
        // Get the popup background color from current style
        ImVec4 popupBg = ImGui::GetStyle().Colors[ImGuiCol_PopupBg];

        // DuckStation-style popup background with full opacity
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ModAlpha(popupBg, 1.0f));
        // Button active state (darker for pressed state)
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ModAlpha(DarkerColor(popupBg, 1.8f), 1.0f));
        // Button hovered state (medium dark)
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ModAlpha(DarkerColor(popupBg, 1.3f), 1.0f));
        // Frame background for input widgets
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ModAlpha(DarkerColor(popupBg, 0.8f), 0.5f));

        // DuckStation-style window padding and rounding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(uiScaled(padding), uiScaled(padding)));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, uiScaled(rounding));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, uiScaled(rounding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(Layout::WIDGET_FRAME_ROUNDING));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(Layout::MENU_BUTTON_SPACING), uiScaled(Layout::MENU_BUTTON_SPACING)));
    }

    ~PopupStyleScope() {
        ImGui::PopStyleVar(6);
        ImGui::PopStyleColor(4);
    }
};

// Helper: Begin menu button list (DuckStation-style)
struct BeginMenuButtons {
    BeginMenuButtons() {
        // Apply proper spacing for menu buttons
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(uiScaled(Layout::MENU_BUTTON_PADDING), uiScaled(Layout::MENU_BUTTON_PADDING)));
        // Focus reset for keyboard navigation
        ImGui::SetNextItemWidth(-1.0f);
    }
    ~BeginMenuButtons() {
        ImGui::PopStyleVar(2);
    }
};

// Helper: End menu button list (DuckStation-style)
struct EndMenuButtons {
    EndMenuButtons() = default;
};

// Internal helper to render options popup
bool RenderOptionsPopup(const PopupOptionsConfig& cfg)
{
    // Icon and row label
    SettingIcon(cfg.icon, ImVec2(uiScaled(cfg.iconSize), uiScaled(cfg.iconSize)));
    ImGui::SameLine(0, uiScaled(cfg.iconSpacing));
    const char* rowLabel = cfg.label ? cfg.label : "";
    ImGui::PushFont(largeFont);
    ImGui::TextUnformatted(rowLabel);
    ImGui::PopFont();

    // Get current display string
    const char* currentStr = "";
    if (cfg.valueToString) {
        currentStr = cfg.valueToString(*cfg.currentValue);
    } else {
        int idx = (*cfg.currentValue >= 0 && *cfg.currentValue < cfg.optionCount)
                  ? *cfg.currentValue : 0;
        currentStr = cfg.options[idx];
    }
    if (currentStr == nullptr)
        currentStr = "";

    std::string displayValue = currentStr;
    if (cfg.disabled && cfg.disabledPrefix && cfg.disabledPrefix[0] != '\0')
        displayValue.insert(0, cfg.disabledPrefix);

    const float valueWidth = uiScaled(cfg.valueWidth);
    const float valueRightPadding = uiScaled(cfg.valueRightPadding);
    const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float slotStartX = rowRightX - valueWidth - valueRightPadding;
    const float toggleCenterX = rowRightX - uiScaled(28.0f) - uiScaled(50.0f) * 0.5f;
    const float minRightMargin = uiScaled(16.0f);
    ImGui::SameLine(slotStartX);
    if (cfg.valueVerticalOffset != 0.0f) {
        const ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + cfg.valueVerticalOffset));
    }

    auto centeredValueX = [&](float textWidth) {
        const float maxX = rowRightX - minRightMargin - textWidth;
        if (maxX <= slotStartX)
            return slotStartX;
        float x = toggleCenterX - textWidth * 0.5f;
        if (x < slotStartX)
            x = slotStartX;
        if (x > maxX)
            x = maxX;
        return x;
    };

    ImFont* valueFont = SettingsRightValueFont();
    auto renderValueText = [&](const char* text, bool disabledText) {
        const char* safeText = text != nullptr ? text : "";
        const float textWidth = valueFont->CalcTextSizeA(valueFont->LegacySize, FLT_MAX, -1.f, safeText).x;
        ImGui::SetCursorPosX(centeredValueX(textWidth));
        ImGui::PushFont(valueFont);
        if (disabledText)
            ImGui::TextDisabled("%s", safeText);
        else
            ImGui::TextUnformatted(safeText);
        ImGui::PopFont();
    };

    if (!cfg.disabled && cfg.valueClickable) {
        ImGui::PushFont(valueFont);
        if (ImGui::Selectable(displayValue.c_str(), false, ImGuiSelectableFlags_None, ImVec2(valueWidth, 0.0f)))
            ImGui::OpenPopup(cfg.popupID);
        ImGui::PopFont();
    } else if (cfg.disabled) {
        renderValueText(displayValue.c_str(), true);
    } else {
        renderValueText(displayValue.c_str(), false);
    }

    // Render popup
    bool valueChanged = false;

    // Configure popup window BEFORE opening (DuckStation-style)
    centerNextWindow();
    float maxWidth = ImGui::GetIO().DisplaySize.x * 0.5f;
    float maxHeight = ImGui::GetIO().DisplaySize.y * 0.7f;

    // Set fixed width for DuckStation-style choice dialog
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(uiScaled(Layout::POPUP_MIN_WIDTH), 0),
        ImVec2(maxWidth, maxHeight)
    );
    ImGui::SetNextWindowSize(ImVec2(uiScaled(Layout::POPUP_WIDTH), 0), ImGuiCond_FirstUseEver);

    PopupStyleScope style;
    if (ImGui::BeginPopup(cfg.popupID, ImGuiWindowFlags_NoScrollbar)) {
        const char* popupTitle = (cfg.title != nullptr && cfg.title[0] != '\0') ? cfg.title : rowLabel;
        const char* popupDescription = (cfg.description != nullptr && cfg.description[0] != '\0') ? cfg.description : cfg.tooltip;
        const bool hasHeader = (popupTitle != nullptr && popupTitle[0] != '\0')
                            || (popupDescription != nullptr && popupDescription[0] != '\0');

        if (popupTitle != nullptr && popupTitle[0] != '\0') {
            ImGui::PushFont(largeFont);
            ImGui::TextUnformatted(popupTitle);
            ImGui::PopFont();
        }
        if (popupDescription != nullptr && popupDescription[0] != '\0') {
            ImGui::PushFont(SettingsDescriptionFont());
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(popupDescription);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
        if (hasHeader && cfg.showHeaderDivider) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
        }

        // DuckStation-style menu button layout
        {
            BeginMenuButtons menuButtons;

            for (int i = 0; i < cfg.optionCount; i++) {
                int storageIdx = cfg.storageIndexMap ? cfg.storageIndexMap(i) : i;
                bool isSelected = (*cfg.currentValue == storageIdx);

                // Render selectable with proper sizing
                ImVec2 buttonSize(ImGui::GetContentRegionAvail().x, uiScaled(Layout::MENU_BUTTON_HEIGHT));
                auto onSelected = [&]() {
                    if (ImGui::Selectable(cfg.options[i], isSelected, ImGuiSelectableFlags_DontClosePopups, buttonSize)) {
                        *cfg.currentValue = storageIdx;
                        valueChanged = true;

                        bool shouldClose = true;
                        if (cfg.onChange) {
                            shouldClose = cfg.onChange(storageIdx);
                        }
                        if (shouldClose) {
                            ImGui::CloseCurrentPopup();
                        }
                    }
                };

                // Highlight selected item with the active row style.
                if (isSelected) {
                    ImguiStyleColor selectColor(ImGuiCol_Header, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    ImguiStyleColor hoverColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    onSelected();
                } else {
                    onSelected();
                }

                // Set default focus on selected item
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        } // End menu buttons

        ImGui::EndPopup();
    }

    return valueChanged;
}

// Internal helper to render slider popup (DuckStation-style)
bool RenderSliderPopup(PopupSliderConfig& cfg)
{
    const auto formatValueText = [&](int value, char* out, size_t outSize) {
        if (cfg.valueFormatter) {
            const std::string text = cfg.valueFormatter(value);
            snprintf(out, outSize, "%s", text.c_str());
        } else {
            snprintf(out, outSize, cfg.format, value);
        }
    };

    // Icon and label
    SettingIcon(cfg.icon, ImVec2(uiScaled(cfg.iconSize), uiScaled(cfg.iconSize)));
    ImGui::SameLine(0, uiScaled(cfg.iconSpacing));
    const char* rowLabel = cfg.label ? cfg.label : "";
    ImGui::PushFont(largeFont);
    ImGui::TextUnformatted(rowLabel);
    ImGui::PopFont();

    // Current value display (centered toward toggle column)
    const float valueWidth = uiScaled(cfg.valueWidth);
    const float valueRightPadding = uiScaled(Layout::POPUP_VALUE_RIGHT_PADDING);
    const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    const float slotStartX = rowRightX - valueWidth - valueRightPadding;
    const float toggleCenterX = rowRightX - uiScaled(28.0f) - uiScaled(50.0f) * 0.5f;
    const float minRightMargin = uiScaled(16.0f);
    ImGui::SameLine(slotStartX);
    if (cfg.valueVerticalOffset != 0.0f) {
        const ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + cfg.valueVerticalOffset));
    }

    char currentValue[32];
    formatValueText(*cfg.currentValue, currentValue, sizeof(currentValue));

    // Display current value as centered text (full row handles popup opening)
    ImFont* valueFont = SettingsRightValueFont();
    const float currentValueWidth = valueFont->CalcTextSizeA(valueFont->LegacySize, FLT_MAX, -1.f, currentValue).x;
    const float maxX = rowRightX - minRightMargin - currentValueWidth;
    float centeredX = toggleCenterX - currentValueWidth * 0.5f;
    if (maxX <= slotStartX)
        centeredX = slotStartX;
    else
    {
        if (centeredX < slotStartX)
            centeredX = slotStartX;
        if (centeredX > maxX)
            centeredX = maxX;
    }
    ImGui::SetCursorPosX(centeredX);
    ImGui::PushFont(valueFont);
    ImGui::TextUnformatted(currentValue);
    ImGui::PopFont();

    // Render popup
    bool valueChanged = false;

    // Configure popup window BEFORE opening (DuckStation-style)
    centerNextWindow();
    float maxWidth = ImGui::GetIO().DisplaySize.x * 0.5f;
    float maxHeight = ImGui::GetIO().DisplaySize.y * 0.7f;
    ImGui::SetNextWindowSizeConstraints(ImVec2(uiScaled(250), 0), ImVec2(maxWidth, maxHeight));
    ImGui::SetNextWindowSize(ImVec2(uiScaled(500), 0), ImGuiCond_FirstUseEver);

    PopupStyleScope style;
    if (ImGui::BeginPopup(cfg.popupID, ImGuiWindowFlags_NoScrollbar)) {
        if (rowLabel[0] != '\0') {
            ImGui::PushFont(largeFont);
            ImGui::TextUnformatted(rowLabel);
            ImGui::PopFont();
        }

        if (cfg.description) {
            ImGui::PushFont(SettingsDescriptionFont());

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
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(cfg.description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Display value range and default value (DuckStation-style)
        char rangeText[128];
        snprintf(rangeText, sizeof(rangeText), "Value Range: %d - %d", cfg.minValue, cfg.maxValue);
        ImGui::PushFont(PopupEmphasisFont());
        ImGui::TextDisabled("%s", rangeText);

        if (cfg.defaultValue >= cfg.minValue && cfg.defaultValue <= cfg.maxValue) {
            char defaultText[128];
            snprintf(defaultText, sizeof(defaultText), "Default Value: %d", cfg.defaultValue);
            ImGui::SameLine(0, uiScaled(20));
            ImGui::TextDisabled("%s", defaultText);
        }
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // DuckStation-style rounded slider with no border
        const float frameRounding = 20.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(frameRounding));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, uiScaled(frameRounding));

        float sliderWidth = uiScaled(cfg.sliderWidth);
        ImVec2 sliderPos = ImGui::GetCursorScreenPos();
        const ImGuiID popupId = ImGui::GetID(cfg.popupID);
        static ImGuiID modePopupId = 0;
        static bool modeTextEntry = false;

        if (ImGui::IsWindowAppearing())
        {
            ImGuiContext& g = *GImGui;
            const bool controllerOpen = (g.NavInputSource == ImGuiInputSource_Gamepad);
            modePopupId = popupId;
            // Mouse/keyboard opens use direct text entry.
            // Controller opens use slider-only interaction.
            modeTextEntry = cfg.preferTextEntry || !controllerOpen;
        }

        const bool textEntryMode = (modePopupId == popupId) && modeTextEntry;
        bool requestFocusApply = false;

        if (textEntryMode)
        {
            char sliderValueText[32];
            formatValueText(*cfg.currentValue, sliderValueText, sizeof(sliderValueText));
            const ImVec2 valueTextSize = ImGui::CalcTextSize(sliderValueText);
            int typedValue = *cfg.currentValue;
            const float inputWidth = std::max(uiScaled(84.0f), valueTextSize.x + uiScaled(18.0f));
            ImGui::SetCursorScreenPos(ImVec2(
                sliderPos.x + (sliderWidth - inputWidth) * 0.5f,
                sliderPos.y
            ));
            ImGui::SetNextItemWidth(inputWidth);
            ImGui::PushID("SliderTypedValue");
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            const bool typedChanged = ImGui::InputInt("##TypedValue", &typedValue, 0, 0);
            const bool accept = typedChanged || ImGui::IsItemDeactivatedAfterEdit();
            ImGui::PopID();

            if (accept)
            {
                typedValue = std::clamp(typedValue, cfg.minValue, cfg.maxValue);
                if (typedValue != *cfg.currentValue)
                {
                    *cfg.currentValue = typedValue;
                    valueChanged = true;
                    if (cfg.showApplyFlag)
                        *cfg.showApplyFlag = true;
                    cfg.hasPendingChanges = true;
                    if (cfg.onValueChange)
                        cfg.onValueChange();
                }
            }
        }
        else
        {
            ImGui::SetNextItemWidth(sliderWidth);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            int tempValue = *cfg.currentValue;
            const bool sliderChanged = ImGui::SliderInt(
                "##SliderPopup",
                &tempValue,
                cfg.minValue,
                cfg.maxValue,
                "",
                ImGuiSliderFlags_NoInput
            );
            const bool sliderFocused = ImGui::IsItemFocused();
            const bool sliderActive = ImGui::IsItemActive();

            if (sliderFocused)
            {
                int navDelta = 0;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)
                    || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)
                    || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true))
                    navDelta = -1;
                else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)
                         || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)
                         || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true))
                    navDelta = 1;

                if (navDelta != 0)
                {
                    tempValue = std::clamp(tempValue + navDelta, cfg.minValue, cfg.maxValue);
                    if (tempValue != *cfg.currentValue)
                    {
                        *cfg.currentValue = tempValue;
                        valueChanged = true;
                        if (cfg.showApplyFlag)
                            *cfg.showApplyFlag = true;
                        cfg.hasPendingChanges = true;
                        if (cfg.onValueChange)
                            cfg.onValueChange();
                    }
                }

                const bool navDown = ImGui::IsKeyPressed(ImGuiKey_DownArrow, false)
                                  || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false)
                                  || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false);
                const bool navUp = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false)
                                || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false)
                                || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false);
                if (sliderActive && (navDown || navUp))
                {
                    ImGui::ClearActiveID();
                    requestFocusApply = navDown;
                }
            }

            if (sliderChanged) {
                *cfg.currentValue = tempValue;
                valueChanged = true;

                // Update state tracking
                if (cfg.showApplyFlag) {
                    *cfg.showApplyFlag = true;
                }
                cfg.hasPendingChanges = true;

                if (cfg.onValueChange) {
                    cfg.onValueChange();
                }
            }

            char sliderValueText[32];
            formatValueText(*cfg.currentValue, sliderValueText, sizeof(sliderValueText));
            const ImVec2 valueTextSize = ImGui::CalcTextSize(sliderValueText);
            const ImVec2 sliderMin = ImGui::GetItemRectMin();
            const ImVec2 sliderMax = ImGui::GetItemRectMax();
            const ImVec2 valueTextPos(
                sliderMin.x + (sliderMax.x - sliderMin.x - valueTextSize.x) * 0.5f,
                sliderMin.y + (sliderMax.y - sliderMin.y - valueTextSize.y) * 0.5f
            );
            ImGui::GetWindowDrawList()->AddText(valueTextPos, ImGui::GetColorU32(ImGuiCol_Text), sliderValueText);
        }

        ImGui::PopStyleVar(3); // Pop FrameRounding, FrameBorderSize, GrabRounding

        ImGui::Spacing();

        // DuckStation-style button layout (right-aligned)
        bool shouldShowApply = cfg.hasPendingChanges ||
                               (cfg.showApplyFlag && *cfg.showApplyFlag);

        if (cfg.onApply && shouldShowApply) {
            // Begin menu buttons container
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(8), uiScaled(8)));

            const float buttonWidth = uiScaled(120);
            const float buttonHeight = uiScaled(32);

            // Right-align buttons
            float availableWidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(availableWidth - buttonWidth);

            // Apply button with rounded corners
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(uiScaled(16), uiScaled(8)));

            if (requestFocusApply)
                ImGui::SetKeyboardFocusHere();

            if (ImGui::Button(cfg.applyButtonText, ImVec2(buttonWidth, buttonHeight))) {
                cfg.onApply();
                cfg.hasPendingChanges = false; // Reset state
                if (cfg.showApplyFlag) {
                    *cfg.showApplyFlag = false; // Reset external flag
                }
                ImGui::CloseCurrentPopup();
            }

            ImGui::PopStyleVar(3); // Pop ItemSpacing, FrameRounding, FramePadding
        }

        ImGui::EndPopup();
    }

    return valueChanged;
}

} // namespace Detail

// Main entry point
bool SettingPopup(PopupConfig& config)
{
    switch (config.type) {
        case PopupType::Options:
            return Detail::RenderOptionsPopup(config.options);
        case PopupType::Slider:
            return Detail::RenderSliderPopup(config.slider);
        default:
            return false;
    }
}

// Convenience overload for options
bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* const* options,
    int optionCount,
    int* currentValue,
    bool disabled,
    const char* disabledPrefix)
{
    PopupConfig cfg;
    cfg.type = PopupType::Options;
    cfg.options.label = label;
    cfg.options.icon = icon;
    cfg.options.popupID = popupID;
    cfg.options.options = options;
    cfg.options.optionCount = optionCount;
    cfg.options.currentValue = currentValue;
    cfg.options.disabled = disabled;
    cfg.options.disabledPrefix = disabledPrefix;
    return SettingPopup(cfg);
}

// Convenience overload for slider
bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* description,
    int* currentValue,
    int minValue,
    int maxValue,
    const char* format,
    std::function<void()> onApply)
{
    PopupConfig cfg;
    cfg.type = PopupType::Slider;
    cfg.slider.label = label;
    cfg.slider.icon = icon;
    cfg.slider.popupID = popupID;
    cfg.slider.description = description;
    cfg.slider.currentValue = currentValue;
    cfg.slider.minValue = minValue;
    cfg.slider.maxValue = maxValue;
    cfg.slider.format = format;
    cfg.slider.onApply = std::move(onApply);
    return SettingPopup(cfg);
}

} // namespace SettingsUI
