/*
	Copyright 2026 The Hollycast Authors

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
#pragma once

#include <string>

namespace GuiMenu {

// Menu state management
// Controls visibility and behavior of the menu bar
extern bool menuVisible;

// Main entry point for rendering the menu bar
// Called each frame when the GUI is active
// Renders all menu items and handles user interaction
void renderMainMenuBar();

// Individual menu renderers
// Each function renders a specific menu dropdown and handles its items

// File Menu - Game content management
// Items: Load Game, Load State, Save State, Screenshot, Exit
void renderFileMenu();

// System Menu - emulation control
// Items: Start, Pause, Reset, Settings, Close
void renderSystemMenu();

// Tools Menu - utilities and debugging
// Items: Cheats, Achievements, Profiler, Network, Disk Swap
void renderToolsMenu();

// Settings Menu - configuration
// Items: Graphics, Audio, Input, About
void renderSettingsMenu();

// Help Menu - documentation and support
// Items: Documentation, Report Issue, About
void renderHelpMenu();

// Menu visibility control
// Shows or hides the menu bar programmatically
void setMenuVisible(bool visible);

// Toggle menu visibility
// Useful for keyboard shortcuts (e.g., Alt key)
void toggleMenuVisibility();

// Check if menu is currently visible
bool isMenuVisible();

// Initialize menu system
// Called during GUI initialization to set up menu state
void initialize();

// Cleanup menu system
// Called during GUI shutdown to release resources
void shutdown();

} // namespace GuiMenu

// Save state slot rendering with thumbnails (global function, not in namespace)
// isSaving: true for save menu, false for load menu
void render_save_state_slots(bool isSaving);
