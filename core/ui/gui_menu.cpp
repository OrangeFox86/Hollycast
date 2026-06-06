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

// External game state flag from gui.cpp
extern bool game_started;

using namespace i18n;

namespace GuiMenu {

// Menu visibility state
bool menuVisible = true;

static bool openUrlInShell(const char* url)
{
	if (url == nullptr || *url == '\0')
		return false;
	ImGuiPlatformIO& platformIo = ImGui::GetPlatformIO();
	if (platformIo.Platform_OpenInShellFn == nullptr)
		return false;
	return platformIo.Platform_OpenInShellFn(ImGui::GetCurrentContext(), url);
}

// Render the main menu bar using standard ImGui.
void renderMainMenuBar()
{
	if (!menuVisible)
		return;

	// Use ImGui's native main menu bar - it handles everything automatically:
	// - Background styling from current theme (ImGuiCol_MenuBarBg)
	// - Mouse/keyboard interaction
	// - Hover effects (ImGuiCol_HeaderHovered) and dropdown menus
	// - Cross-platform native appearance
	// Theme colors are already set by applyCurrentTheme().
	if (ImGui::BeginMainMenuBar())
	{
		renderFileMenu();
		renderSystemMenu();
		renderToolsMenu();
		renderSettingsMenu();
		renderHelpMenu();
		ImGui::EndMainMenuBar();
	}
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
					config::ContentPath.get().push_back(selection);
					SaveSettings();
					gui_refresh_files();
				}
				return true;
			}, false, "");
		}

		// Rescan ROM Directory
		if (ImGui::MenuItem(T("Rescan ROM Directory"), nullptr, false, true))
		{
			gui_refresh_files();
		}

		ImGui::Separator();

		// Load ROM
		if (ImGui::MenuItem(T("Load ROM"), nullptr, false, true))
		{
			// Open file selector for ROM
			select_file_popup(T("Select ROM File"), [](bool cancelled, std::string selection) {
				if (!cancelled && !selection.empty())
				{
					gui_start_game(selection);
				}
				return true;
			}, true, "");
		}

		ImGui::Separator();

		// Save State
		if (ImGui::MenuItem(T("Save State"), nullptr, false, ::game_started))
		{
			if (::game_started)
			{
				gui_saveState();
			}
		}

		// Load State
		if (ImGui::MenuItem(T("Load State"), nullptr, false, ::game_started))
		{
			if (::game_started)
			{
				gui_loadState();
			}
		}

		ImGui::Separator();

		// Exit Game
		if (ImGui::MenuItem(T("Exit Game"), nullptr, false, ::game_started))
		{
			showExitSaveDialog = true; // Trigger dialog instead of immediate exit
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
					emu.stop();
					gui_setState(GuiState::Commands);
				}
			}
			else
			{
				if (ImGui::MenuItem(T("Resume"), nullptr, false, true))
				{
					gui_setState(GuiState::Closed);
					emu.start();
				}
			}
		}

		// Restart
		if (ImGui::MenuItem(T("Restart"), nullptr, false, ::game_started))
		{
			if (::game_started)
			{
				emu.stop();
				emu.start();
			}
		}

		ImGui::Separator();

		// Fast Forward
		if (ImGui::MenuItem(T("Fast Forward"), nullptr, false, ::game_started))
		{
			if (::game_started)
				settings.input.fastForwardMode = !settings.input.fastForwardMode;
		}

		// Screenshot
		if (ImGui::MenuItem(T("Screenshot"), nullptr, false, ::game_started))
		{
			if (::game_started)
				gui_takeScreenshot();
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
			if (::game_started)
			{
				gui_setState(GuiState::Cheats);
			}
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
			gui_setState(GuiState::Settings);
			gui_focus_boxart_settings_section();
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
			gui_prepare_settings_tab(GuiSettingsTab::General);
			gui_setState(GuiState::Settings);
		}

		// Video
		if (ImGui::MenuItem(T("Video"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Video);
			gui_setState(GuiState::Settings);
		}

		// Audio
		if (ImGui::MenuItem(T("Audio"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Audio);
			gui_setState(GuiState::Settings);
		}

		// Input/Controls
		if (ImGui::MenuItem(T("Input"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Controls);
			gui_setState(GuiState::Settings);
		}

		// Network
		if (ImGui::MenuItem(T("Network"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Network);
			gui_setState(GuiState::Settings);
		}

		// Advanced
		if (ImGui::MenuItem(T("Advanced"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Advanced);
			gui_setState(GuiState::Settings);
		}

		ImGui::Separator();

		// Open full settings (defaults to current tab state; reset to general here)
		if (ImGui::MenuItem(T("All Settings"), nullptr, false, true))
		{
			gui_reset_settings_view();
			gui_setState(GuiState::Settings);
		}

		ImGui::Separator();

		// About tab
		if (ImGui::MenuItem(T("About"), nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::About);
			gui_setState(GuiState::Settings);
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
			gui_prepare_settings_tab(GuiSettingsTab::About);
			gui_setState(GuiState::Settings);
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

// Check if menu is currently visible
bool isMenuVisible()
{
	return menuVisible;
}

// Initialize menu system
// Called during GUI initialization to set up menu state
void initialize()
{
	menuVisible = true;
}

// Cleanup menu system
// Called during GUI shutdown to release resources
void shutdown()
{
}

} // namespace GuiMenu
