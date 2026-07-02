/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

	Hollycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	Hollycast is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "gui_menu.h"
#include "gui.h"
#include "gui_util.h"
#include "cfg/cfg.h"
#include "emulator.h"
#include "log/LogManager.h"
#include "imgui.h"
#include "cfg/option.h"
#include "types.h"
#include "settings.h"
#include "oslib/i18n.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif

// External game state flag from gui.cpp
extern bool game_started;
extern ImFont *settingsTitleFont;

using namespace i18n;

namespace GuiMenu {

// Menu visibility state
bool menuVisible = true;
static bool menuBarVisibleThisFrame = true;
static float menuBarHeightThisFrame = 0.0f;
static double touchMenuVisibleUntil = 0.0;

static bool isFullscreenMenuMode()
{
#if defined(__ANDROID__)
	return true;
#elif defined(USE_SDL)
	return sdl_is_fullscreen();
#else
	return false;
#endif
}

static bool shouldShowMenuBar()
{
	if (!menuVisible)
		return false;
	if (!isFullscreenMenuMode())
		return true;

	ImGuiIO& io = ImGui::GetIO();
	ImFont* menuFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImGui::PushFont(menuFont);
	const float revealHeight = ImGui::GetFrameHeight() * 1.75f;
	ImGui::PopFont();
	const bool hasPointer = io.MousePos.x != -FLT_MAX && io.MousePos.y != -FLT_MAX;
	const bool pointerAtTop = hasPointer && io.MousePos.y <= revealHeight;
	const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);

	if (io.MouseSource == ImGuiMouseSource_TouchScreen)
	{
#if defined(__ANDROID__)
		// Settings rows also use ImGui popups. Do not treat those popups as a
		// reason to reveal the top menu on Android: showing the menu changes the
		// Settings window position between OpenPopup() and BeginPopup(), which can
		// make row value popups flicker and fail unless the menu was already shown.
		const bool settingsOwnsTouch = gui_state == GuiState::Settings;
		if (!settingsOwnsTouch && !popupOpen && pointerAtTop && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			touchMenuVisibleUntil = ImGui::GetTime() + 5.0;
		return ImGui::GetTime() < touchMenuVisibleUntil;
#else
		if (!popupOpen && pointerAtTop && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			touchMenuVisibleUntil = ImGui::GetTime() + 5.0;
		return popupOpen || ImGui::GetTime() < touchMenuVisibleUntil;
#endif
	}

	return popupOpen || pointerAtTop;
}

static bool openUrlInShell(const char* url)
{
	if (url == nullptr || *url == '\0')
		return false;
	ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
	if (platformIo.Platform_OpenInShellFn == nullptr)
		return false;
	return platformIo.Platform_OpenInShellFn(ImGui::GetCurrentContext(), url);
}

void addRomDirectory(const std::string& path)
{
	if (path.empty())
		return;
	config::ContentPath.get().push_back(path);
	SaveSettings();
	gui_refresh_files();
}

void loadRomFile(const std::string& path)
{
	if (!path.empty())
		gui_start_game(path);
}

void rescanRomDirectory()
{
	gui_refresh_files();
}

void saveState()
{
	if (::game_started)
		gui_saveState();
}

void loadState()
{
	if (::game_started)
		gui_loadState();
}

void exitEmulator()
{
	gui_request_exit_emulator();
}

void closeGame()
{
	if (::game_started)
		gui_request_exit_to_library();
}

void pauseOrResume()
{
	if (!::game_started)
		return;
	if (gui_state == GuiState::Closed)
	{
		emu.stop();
		gui_setState(GuiState::Commands);
	}
	else
	{
		gui_setState(GuiState::Closed);
		emu.start();
	}
}

void restartGame()
{
	if (::game_started)
	{
		emu.stop();
		emu.start();
	}
}

void toggleFastForward()
{
	if (::game_started)
		settings.input.fastForwardMode = !settings.input.fastForwardMode;
}

void takeScreenshot()
{
	if (::game_started)
		gui_takeScreenshot();
}

void openCheats()
{
	if (::game_started)
		gui_setState(GuiState::Cheats);
}

void openCustomBoxartSettings()
{
	gui_setState(GuiState::Settings);
	gui_focus_boxart_settings_section();
}

static void openSettingsTab(GuiSettingsTab tab)
{
	gui_prepare_settings_tab(tab);
	gui_setState(GuiState::Settings);
}

void openGeneralSettings()
{
	openSettingsTab(GuiSettingsTab::General);
}

void openVideoSettings()
{
	openSettingsTab(GuiSettingsTab::Video);
}

void openAudioSettings()
{
	openSettingsTab(GuiSettingsTab::Audio);
}

void openControlsSettings()
{
	openSettingsTab(GuiSettingsTab::Controls);
}

void openNetworkSettings()
{
	openSettingsTab(GuiSettingsTab::Network);
}

void openAdvancedSettings()
{
	openSettingsTab(GuiSettingsTab::Advanced);
}

void openAboutSettings()
{
	openSettingsTab(GuiSettingsTab::About);
}

bool isGameRunning()
{
	return ::game_started;
}

// Render the main menu bar using standard ImGui.
void renderMainMenuBar()
{
#if defined(TARGET_MAC)
	menuBarVisibleThisFrame = false;
	menuBarHeightThisFrame = 0.0f;
	return;
#endif

	menuBarVisibleThisFrame = shouldShowMenuBar();
	menuBarHeightThisFrame = 0.0f;
	if (!menuBarVisibleThisFrame)
		return;

	// Use ImGui's native main menu bar - it handles everything automatically:
	// - Background styling from current theme (ImGuiCol_MenuBarBg)
	// - Mouse/keyboard interaction
	// - Hover effects (ImGuiCol_HeaderHovered) and dropdown menus
	// - Cross-platform native appearance
	// Theme colors are already set by applyCurrentTheme().
	ImFont* menuFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImGui::PushFont(menuFont);
	if (ImGui::BeginMainMenuBar())
	{
		menuBarHeightThisFrame = ImGui::GetWindowHeight();
		renderFileMenu();
		renderSystemMenu();
		renderToolsMenu();
		renderSettingsMenu();
		renderHelpMenu();
		ImGui::EndMainMenuBar();
	}
	ImGui::PopFont();
}

// Render File menu
void renderFileMenu()
{
	if (ImGui::BeginMenu(T("File")))
	{
		// Set ROM Directory
		if (ImGui::MenuItem(T("Set ROM Directory"), nullptr, false, true))
		{
			// Open directory selector for ROM path
			select_file_popup(T("Select ROM Directory"), [](bool cancelled, std::string selection) {
				if (!cancelled && !selection.empty())
				{
					addRomDirectory(selection);
				}
				return true;
			}, false, "");
		}

		// Rescan ROM Directory
		if (ImGui::MenuItem(T("Rescan ROM Directory"), nullptr, false, true))
		{
			rescanRomDirectory();
		}

		ImGui::Separator();

		// Load ROM
		if (ImGui::MenuItem(T("Load ROM"), nullptr, false, true))
		{
			// Open file selector for ROM
			select_file_popup(T("Select ROM File"), [](bool cancelled, std::string selection) {
				if (!cancelled && !selection.empty())
				{
					loadRomFile(selection);
				}
				return true;
			}, true, "");
		}

		ImGui::Separator();

		// Save State
		if (ImGui::MenuItem(T("Save State"), nullptr, false, ::game_started))
		{
			saveState();
		}

		// Load State
		if (ImGui::MenuItem(T("Load State"), nullptr, false, ::game_started))
		{
			loadState();
		}

		ImGui::Separator();

		// Exit Emulator
		if (ImGui::MenuItem(T("Exit Emulator"), nullptr, false, true))
		{
			exitEmulator();
		}

		ImGui::EndMenu();
	}
}

// Render System menu (only visible when game is running)
void renderSystemMenu()
{
	if (ImGui::BeginMenu(T("System"), ::game_started))
	{
		// Pause/Resume
		if (::game_started)
		{
			if (gui_state == GuiState::Closed)
			{
				if (ImGui::MenuItem(T("Pause"), nullptr, false, true))
				{
					pauseOrResume();
				}
			}
			else
			{
				if (ImGui::MenuItem(T("Resume"), nullptr, false, true))
				{
					pauseOrResume();
				}
			}
		}

		// Restart
		if (ImGui::MenuItem(T("Restart"), nullptr, false, ::game_started))
		{
			restartGame();
		}

		// Close Game
		if (ImGui::MenuItem(T("Close Game"), nullptr, false, ::game_started))
		{
			closeGame();
		}

		ImGui::Separator();

		// Fast Forward
		if (ImGui::MenuItem(T("Fast Forward"), nullptr, false, ::game_started))
		{
			toggleFastForward();
		}

		// Screenshot
		if (ImGui::MenuItem(T("Screenshot"), nullptr, false, ::game_started))
		{
			takeScreenshot();
		}

		ImGui::Separator();

		// Save State with submenu
		if (ImGui::BeginMenu(T("Save State"), ::game_started))
		{
			::render_save_state_slots(true);
			ImGui::EndMenu();
		}

		// Load State with submenu
		if (ImGui::BeginMenu(T("Load State"), ::game_started))
		{
			::render_save_state_slots(false);
			ImGui::EndMenu();
		}

		ImGui::Separator();

		// Cheats
		if (ImGui::MenuItem(T("Cheats"), nullptr, false, ::game_started))
		{
			openCheats();
		}

		ImGui::EndMenu();
	}
}

// Render Tools menu
void renderToolsMenu()
{
	if (ImGui::BeginMenu(T("Tools")))
	{
		ImGui::MenuItem(T("CHD Convert"), nullptr, false, false);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", T("CHD conversion is not available yet."));

		// Custom Boxart
		if (ImGui::MenuItem(T("Custom Boxart"), nullptr, false, true))
		{
			openCustomBoxartSettings();
		}

		ImGui::EndMenu();
	}
}

// Render Settings menu
void renderSettingsMenu()
{
	if (ImGui::BeginMenu(T("Settings")))
	{
		// General
		if (ImGui::MenuItem(T("General"), nullptr, false, true))
		{
			openGeneralSettings();
		}

		// Video
		if (ImGui::MenuItem(T("Video"), nullptr, false, true))
		{
			openVideoSettings();
		}

		// Audio
		if (ImGui::MenuItem(T("Audio"), nullptr, false, true))
		{
			openAudioSettings();
		}

		// Controls
		if (ImGui::MenuItem(T("Controls"), nullptr, false, true))
		{
			openControlsSettings();
		}

		// Network
		if (ImGui::MenuItem(T("Network"), nullptr, false, true))
		{
			openNetworkSettings();
		}

		// Advanced
		if (ImGui::MenuItem(T("Advanced"), nullptr, false, true))
		{
			openAdvancedSettings();
		}

		ImGui::EndMenu();
	}
}

// Render Help menu
void renderHelpMenu()
{
	if (ImGui::BeginMenu(T("Help")))
	{
		const bool canOpenLinks = ImGui::GetPlatformIO().Platform_OpenInShellFn != nullptr;
		if (ImGui::MenuItem(T("Discord"), nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://discord.gg/X8YWP8w"))
				WARN_LOG(COMMON, "Unable to open Discord URL from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", T("Opening links is not supported on this platform."));

		if (ImGui::MenuItem(T("Report Bug"), nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://github.com/flyinghead/flycast/issues/new/choose"))
				WARN_LOG(COMMON, "Unable to open bug report URL from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", T("Opening links is not supported on this platform."));

		ImGui::Separator();

		if (ImGui::MenuItem(T("Check for Updates"), nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://flyinghead.github.io/flycast-builds/"))
				WARN_LOG(COMMON, "Unable to open updates page from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", T("Opening links is not supported on this platform."));

		ImGui::Separator();

		if (ImGui::MenuItem(T("About Hollycast"), nullptr, false, true))
		{
			openAboutSettings();
		}

		ImGui::EndMenu();
	}
}

// Menu visibility control
// Shows or hides the menu bar programmatically
void setMenuVisible(bool visible)
{
	menuVisible = visible;
}

// Toggle menu visibility
// Useful for keyboard shortcuts (e.g., Alt key)
void toggleMenuVisibility()
{
	menuVisible = !menuVisible;
}

// Check if menu bar is currently visible
bool isMenuBarVisible()
{
	return menuVisible;
}

float mainMenuBarHeight()
{
	return menuBarHeightThisFrame;
}

// Initialize menu system
// Called during GUI initialization to set up menu state
void initialize()
{
	menuVisible = true;
	menuBarVisibleThisFrame = true;
	touchMenuVisibleUntil = 0.0;
}

// Cleanup menu system
// Called during GUI shutdown to release resources
void shutdown()
{
}

} // namespace GuiMenu
