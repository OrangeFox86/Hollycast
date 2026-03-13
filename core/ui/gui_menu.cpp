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

// External game state flag from gui.cpp
extern bool game_started;

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
	if (ImGui::BeginMenu("File"))
	{
		// Set ROM Directory
		if (ImGui::MenuItem("Set ROM Directory", nullptr, false, true))
		{
			// Open directory selector for ROM path
			select_file_popup("Select ROM Directory", [](bool cancelled, std::string selection) {
				if (!cancelled && !selection.empty())
				{
					config::ContentPath.get().push_back(selection);
					gui_refresh_files();
				}
				return true;
			}, false, "");
		}

		// Rescan ROM Directory
		if (ImGui::MenuItem("Rescan ROM Directory", nullptr, false, true))
		{
			gui_refresh_files();
		}

		ImGui::Separator();

		// Load ROM
		if (ImGui::MenuItem("Load ROM", nullptr, false, true))
		{
			// Open file selector for ROM
			select_file_popup("Select ROM File", [](bool cancelled, std::string selection) {
				if (!cancelled && !selection.empty())
				{
					gui_start_game(selection);
				}
				return true;
			}, true, "");
		}

		ImGui::Separator();

		// Save State
		if (ImGui::MenuItem("Save State", nullptr, false, ::game_started))
		{
			if (::game_started)
			{
				gui_saveState();
			}
		}

		// Load State
		if (ImGui::MenuItem("Load State", nullptr, false, ::game_started))
		{
			if (::game_started)
			{
				gui_loadState();
			}
		}

		ImGui::Separator();

		// Exit Game
		if (ImGui::MenuItem("Exit Game", nullptr, false, ::game_started))
		{
			showExitSaveDialog = true; // Trigger dialog instead of immediate exit
		}

		ImGui::EndMenu();
	}
}

// Render System menu (only visible when game is running)
void renderSystemMenu()
{
	if (ImGui::BeginMenu("System", ::game_started))
	{
		// Pause/Resume
		if (::game_started)
		{
				if (gui_state == GuiState::Closed)
				{
					if (ImGui::MenuItem("Pause", nullptr, false, true))
					{
						emu.stop();
						gui_setState(GuiState::Commands);
					}
			}
				else
				{
					if (ImGui::MenuItem("Resume", nullptr, false, true))
					{
						gui_setState(GuiState::Closed);
						emu.start();
					}
			}
		}

		// Restart
			if (ImGui::MenuItem("Restart", nullptr, false, ::game_started))
			{
				if (::game_started)
				{
					emu.stop();
					emu.start();
				}
		}

		ImGui::Separator();

		// Fast Forward
			if (ImGui::MenuItem("Fast Forward", nullptr, false, ::game_started))
			{
				if (::game_started)
					settings.input.fastForwardMode = !settings.input.fastForwardMode;
			}

		// Screenshot
			if (ImGui::MenuItem("Screenshot", nullptr, false, ::game_started))
			{
				if (::game_started)
					gui_takeScreenshot();
			}

		ImGui::Separator();

		// Save State with submenu
		if (ImGui::BeginMenu("Save State", ::game_started))
		{
			::render_save_state_slots(true);
			ImGui::EndMenu();
		}

		// Load State with submenu
		if (ImGui::BeginMenu("Load State", ::game_started))
		{
			::render_save_state_slots(false);
			ImGui::EndMenu();
		}

		ImGui::Separator();

		// Cheats
			if (ImGui::MenuItem("Cheats", nullptr, false, ::game_started))
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
	if (ImGui::BeginMenu("Tools"))
	{
		ImGui::MenuItem("CHD Convert", nullptr, false, false);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("CHD conversion is not available yet.");

		// Custom Boxart
		if (ImGui::MenuItem("Custom Boxart", nullptr, false, true))
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
	if (ImGui::BeginMenu("Settings"))
	{
		// General
		if (ImGui::MenuItem("General", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::General);
			gui_setState(GuiState::Settings);
		}

		// Video
		if (ImGui::MenuItem("Video", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Video);
			gui_setState(GuiState::Settings);
		}

		// Audio
		if (ImGui::MenuItem("Audio", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Audio);
			gui_setState(GuiState::Settings);
		}

		// Input/Controls
		if (ImGui::MenuItem("Input", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Controls);
			gui_setState(GuiState::Settings);
		}

		// Network
		if (ImGui::MenuItem("Network", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Network);
			gui_setState(GuiState::Settings);
		}

		// Advanced
		if (ImGui::MenuItem("Advanced", nullptr, false, true))
		{
			gui_prepare_settings_tab(GuiSettingsTab::Advanced);
			gui_setState(GuiState::Settings);
		}

		ImGui::Separator();

		// Open full settings (defaults to current tab state; reset to general here)
		if (ImGui::MenuItem("All Settings", nullptr, false, true))
		{
			gui_reset_settings_view();
			gui_setState(GuiState::Settings);
		}

		ImGui::Separator();

		// About tab
		if (ImGui::MenuItem("About", nullptr, false, true))
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
	if (ImGui::BeginMenu("Help"))
	{
		const bool canOpenLinks = ImGui::GetPlatformIO().Platform_OpenInShellFn != nullptr;
		if (ImGui::MenuItem("Discord", nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://discord.gg/X8YWP8w"))
				WARN_LOG(COMMON, "Unable to open Discord URL from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Opening links is not supported on this platform.");

		if (ImGui::MenuItem("Report Bug", nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://github.com/flyinghead/flycast/issues/new/choose"))
				WARN_LOG(COMMON, "Unable to open bug report URL from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Opening links is not supported on this platform.");

		ImGui::Separator();

		if (ImGui::MenuItem("Check for Updates", nullptr, false, canOpenLinks))
		{
			if (!openUrlInShell("https://flyinghead.github.io/flycast-builds/"))
				WARN_LOG(COMMON, "Unable to open updates page from Help menu");
		}
		if (!canOpenLinks && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Opening links is not supported on this platform.");

		ImGui::Separator();

		if (ImGui::MenuItem("About Flycast", nullptr, false, true))
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
