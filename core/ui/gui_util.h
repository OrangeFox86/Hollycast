/*
	Copyright 2024 flyinghead
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

#include "types.h"
#include "cfg/option.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "gui.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "stdclass.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <functional>
#include <string>
#include <mutex>

using StringCallback = std::function<bool(bool cancelled, const std::string& selection)>;

void select_file_popup(const char *prompt, const StringCallback& callback,
		bool selectFile = false, const std::string& extension = "");

enum class StoragePopupResult
{
	Supported,
	Unsupported,
	CallbackAlreadySet,
};

StoragePopupResult select_storage_popup(bool isDirectory, bool writeAccess, const std::string& description,
		const StringCallback& callback, const std::string& mimeType = {});

void scrollWhenDraggingOnVoid(ImGuiMouseButton mouse_button = ImGuiMouseButton_Left);

// Helper to display a little (?) mark which shows a tooltip when hovered.
void ShowHelpMarker(const char* desc);
template<bool PerGameOption>
bool OptionCheckbox(const char *name, config::Option<bool, PerGameOption>& option, const char *help = nullptr);
template<bool PerGameOption>
bool OptionSlider(const char *name, config::Option<int, PerGameOption>& option, int min, int max, const char *help = nullptr, const char *format = nullptr);
template<typename T>
bool OptionRadioButton(const char *name, config::Option<T>& option, T value, const char *help = nullptr);
template<bool PerGameOption>
void OptionComboBox(const char *name, config::Option<int, PerGameOption>& option, const char *values[], int count,
			const char *help = nullptr);
bool OptionArrowButtons(const char *name, config::Option<int>& option, int min, int max, const char *help = nullptr, const char *format = "%d");

// Helper to render a clickable settings row with label and current value
// Returns true if the row was clicked
bool SettingsRow(const char* label, const char* currentValue, const char* helpText = nullptr);

// Helper to render a centered selection popup for options
// Returns the index of selected option, or -1 if cancelled/none selected
int SelectionPopup(const char* popupId, const char* title, const char* options[], int optionCount, int currentSelection);

static inline void centerNextWindow()
{
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2.f, ImGui::GetIO().DisplaySize.y / 2.f),
			ImGuiCond_Always, ImVec2(0.5f, 0.5f));
}

void fullScreenWindow(bool modal);
void windowDragScroll();

class BackgroundGameLoader
{
public:
	void load(const std::string& path)
	{
		progress.reset();
		future = std::async(std::launch::async, [this, path] {
			ThreadName _("GameLoader");
			emu.loadGame(path.c_str(), &progress);
		});
	}

	void cancel()
	{
		if (progress.cancelled)
			return;
		progress.cancelled = true;
		if (future.valid())
			try {
				future.get();
			} catch (const FlycastException&) {
			}
		emu.unloadGame();
		gui_setState(GuiState::Main);
	}

	bool ready()
	{
		if (!future.valid())
			return true;
		if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			future.get();
			return true;
		}
		return false;
	}

	const LoadProgress& getProgress() const {
		return progress;
	}

private:
	LoadProgress progress;
	std::future<void> future;
};

static inline float uiScaled(float f) {
	return f * settings.display.uiScale;
}

static inline float uiLargeFontSize()
{
	return uiScaled(22.f);
}

struct ScaledVec2 : public ImVec2
{
	ScaledVec2()
		: ImVec2() {}
	ScaledVec2(float x, float y)
		: ImVec2(uiScaled(x), uiScaled(y)) {}
};

inline static ImVec2 min(const ImVec2& l, const ImVec2& r) {
	return ImVec2(std::min(l.x, r.x), std::min(l.y, r.y));
}

class DisabledScope
{
public:
	DisabledScope(bool disabled) : disabled(disabled)
	{
		if (disabled)
		{
	        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
	        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
	}
	~DisabledScope()
	{
		if (disabled)
		{
	        ImGui::PopItemFlag();
	        ImGui::PopStyleVar();
		}
	}
	bool isDisabled() const {
		return disabled;
	}

private:
	bool disabled;
};

class ImguiID
{
public:
	ImguiID(const std::string& id)
		: ImguiID(id.c_str()) {}
	ImguiID(const char *id) {
		ImGui::PushID(id);
	}
	~ImguiID() {
		ImGui::PopID();
	}
};

class ImguiStyleVar
{
public:
	ImguiStyleVar(ImGuiStyleVar idx, const ImVec2& val) {
		ImGui::PushStyleVar(idx, val);
	}
	ImguiStyleVar(ImGuiStyleVar idx, float val) {
		ImGui::PushStyleVar(idx, val);
	}
	~ImguiStyleVar() {
		ImGui::PopStyleVar();
	}
};

class ImguiStyleColor
{
public:
	ImguiStyleColor(ImGuiCol idx, const ImVec4& col) {
		ImGui::PushStyleColor(idx, col);
	}
	ImguiStyleColor(ImGuiCol idx, ImU32 col) {
		ImGui::PushStyleColor(idx, col);
	}
	~ImguiStyleColor() {
		ImGui::PopStyleColor();
	}
};

class ImguiTexture
{
public:
	void draw(const ImVec2& size, const ImVec4& tint_col = ImVec4(1, 1, 1, 1),
			const ImVec4& border_col = ImVec4(0, 0, 0, 0));
	void draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size, float alpha);
	void draw(ImDrawList *drawList, const ImVec2& pos, const ImVec2& size,
			const ImVec2& uv0 = ImVec2(0, 0), const ImVec2& uv1 = ImVec2(1, 1), const ImVec4& color = ImVec4(1, 1, 1, 1));
	bool button(const char* str_id, const ImVec2& image_size, const std::string& title = {}, const ImVec4& bg_col = ImVec4(0, 0, 0, 0),
			const ImVec4& tint_col = ImVec4(1, 1, 1, 1));

	operator ImTextureID() {
		return getId();
	}
	void setNearestSampling(bool nearestSampling) {
		this->nearestSampling = nearestSampling;
	}

	virtual ImTextureID getId() = 0;
	virtual ~ImguiTexture() = default;

protected:
	bool nearestSampling = false;
};

class ImguiFileTexture : public ImguiTexture
{
public:
	ImguiFileTexture() = default;
	ImguiFileTexture(const std::string& path) : ImguiTexture(), path(path) {}

	bool operator==(const ImguiFileTexture& other) const {
		return other.path == path;
	}
	ImTextureID getId() override;

	static void resetLoadCount() {
		textureLoadCount = 0;
	}

private:
	std::string path;
	static int textureLoadCount;
};

class ImguiStateTexture : public ImguiTexture
{
public:
	ImguiStateTexture(int slot = config::SavestateSlot) : slot(slot) {}
	ImTextureID getId() override;

	bool exists();
	void invalidate();

private:
	struct LoadedPic
	{
		u8 *data;
		int width;
		int height;
	};
	int slot;
	static std::future<LoadedPic> asyncLoad;
};

class ImguiVmuTexture : public ImguiTexture
{
public:
	ImguiVmuTexture(int index = 0) : index(index) {}

	// draw all active vmus in a single column at the given position
	static void displayVmus(const ImVec2& pos);
	ImTextureID getId() override;

private:
	int index = 0;
	std::string idPath;
	u64 vmuLastChanged = 0;

	static std::array<ImguiVmuTexture, 8> Vmus;
};

class IconButton
{
public:
	IconButton(const char *icon, const std::string& label, const ImVec2& size = {})
		: size(size)
	{
		if (icon == nullptr)
			str = strprintf("%s", label.c_str());
		else
			str = strprintf("%s  %s", icon, label.c_str());
	}

	IconButton(const std::string& label, const ImVec2& size = {})
		: IconButton(nullptr, label, size)
	{}

	bool realize() {
		return ImGui::Button(str.c_str(), size);
	}

	float width() {
		return ImGui::CalcTextSize(str.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
	}

private:
	std::string str;
	ImVec2 size;
};

static inline ImU32 alphaOverride(ImU32 color, float alpha) {
	return (color & ~IM_COL32_A_MASK) | (IM_F32_TO_INT8_SAT(alpha) << IM_COL32_A_SHIFT);
}

class Toast
{
public:
	void show(const std::string& title, const std::string& message, u32 durationMs);
	bool draw();

private:
	static constexpr u64 START_ANIM_TIME = 500;
	static constexpr u64 END_ANIM_TIME = 1000;

	std::string title;
	std::string message;
	u64 startTime = 0;
	u64 endTime = 0;
	std::mutex mutex;
};

std::string middleEllipsis(const std::string& s, float width);

bool beginFrame(const char *label, const ImVec2& size_arg = ImVec2(0, 0), ImVec2 *out_size = nullptr);
void endFrame();

bool InputText(const char *label, std::string *str, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);
bool InputText(const char *label, char *str, size_t size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);
bool InputTextMultiline(const char* label, char* buf, size_t buf_size, const ImVec2& size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);

// Phase 0 Widget Infrastructure Components

// SectionDivider - Visual separator with optional text label
void SectionDivider(const char* text = nullptr);

// SectionHeaderWithIcon - Display icon and text header with highlighted color and separator
void SectionHeaderWithIcon(const char* icon, const char* text);

// SettingIcon - Display settings icon with proper scaling
void SettingIcon(const char* icon, const ImVec2& size = ImVec2(0, 0));

// SettingsRowParams - Parameters for BeginSettingsRow
struct SettingsRowParams {
	const char* label;
	const char* description;
	const char* icon;
	const char* helpText;
	float minHeight;
};

// BeginSettingsRow/EndSettingsRow - Settings row container with icon, label, control layout
void BeginSettingsRow(const SettingsRowParams& params);
void EndSettingsRow();

// ToggleSwitch - iOS-style toggle switch with animation support
bool ToggleSwitch(const char* label, bool* value, const char* helpText = nullptr);

// SettingsOption - Complete settings row with toggle for boolean options
template<bool PerGameOption>
bool SettingsOption(const char* label, config::Option<bool, PerGameOption>& option,
                   const char* description = nullptr, const char* icon = nullptr,
                   const char* helpText = nullptr);

// Unified Popup Widget API for Settings UI
namespace SettingsUI {

// Popup widget types
enum class PopupType : int {
    Options,    // Multi-option selection (Language, Region, etc.)
    Slider      // Percentage/value slider (UI Scaling)
};

// Configuration for option-based popups
struct PopupOptionsConfig {
    // Text labels
    const char* label = nullptr;
    const char* icon = nullptr;
    const char* tooltip = nullptr;
    const char* popupID = nullptr;
    const char* title = nullptr;
    const char* description = nullptr;

    // Option data
    const char* const* options = nullptr;
    int optionCount = 0;
    int* currentValue = nullptr;

    // Optional: Custom value-to-string conversion
    std::function<const char*(int)> valueToString = nullptr;

    // Optional: Custom index-to-storage mapping
    std::function<int(int)> storageIndexMap = nullptr;

    // Optional: Callback when value changes
    std::function<bool(int)> onChange = nullptr;
    // Optional: Called when an option is hovered/focused/selected in the popup.
    // Params: storage index, option label text.
    std::function<void(int, const char*)> onOptionHighlight = nullptr;

    // Optional: Disabled state
    bool disabled = false;
    const char* disabledPrefix = nullptr;
    bool valueClickable = true;
    bool showHeaderDivider = true;

    // Spacing/sizing (defaults)
    float iconSize = 20.0f;
    float iconSpacing = 8.0f;
    float valueWidth = 150.0f;
    float valueRightPadding = 28.0f;
    // Vertical offset for the right-side value text (in current ImGui units).
    // Used to align value text with the description line when needed.
    float valueVerticalOffset = 0.0f;
};

// Configuration for slider-based popups
struct PopupSliderConfig {
    // Text labels
    const char* label = nullptr;
    const char* icon = nullptr;
    const char* tooltip = nullptr;
    const char* popupID = nullptr;
    const char* description = nullptr;

    // Slider range
    int* currentValue = nullptr;
    int minValue = 0;
    int maxValue = 100;
    int defaultValue = 100;  // Default value for display
    const char* format = "%d%%";
    std::function<std::string(int)> valueFormatter = nullptr;

    // Optional: Custom apply button
    const char* applyButtonText = "Apply";
    std::function<void()> onApply = nullptr;

    // Optional: Callback for value change
    std::function<void()> onValueChange = nullptr;

    // Spacing/sizing values are base UI units. RenderSliderPopup() applies uiScaled()
    // when these fields are consumed, so call sites should not pre-scale them.
    float iconSize = 20.0f;
    float iconSpacing = 8.0f;
    float valueWidth = 100.0f;
    float sliderWidth = 300.0f;
    float buttonWidth = 100.0f;
    // Vertical offset for the right-side current value text (in current ImGui units).
    float valueVerticalOffset = 0.0f;

    // Internal state management for apply button
    bool* showApplyFlag = nullptr;  // External flag to control apply button visibility
    bool hasPendingChanges = false; // Internal flag tracking if value changed
    bool preferTextEntry = false;   // Open popup in text-entry mode (mouse flow)
};

// Unified configuration struct
struct PopupConfig {
    PopupType type;
    PopupOptionsConfig options;
    PopupSliderConfig slider;
};

// Main entry point function
bool SettingPopup(PopupConfig& config);

// Convenience overloads
bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* const* options,
    int optionCount,
    int* currentValue,
    bool disabled = false,
    const char* disabledPrefix = nullptr
);

bool SettingPopup(
    const char* label,
    const char* icon,
    const char* popupID,
    const char* description,
    int* currentValue,
    int minValue,
    int maxValue,
    const char* format = "%d%%",
    std::function<void()> onApply = nullptr
);

} // namespace SettingsUI
