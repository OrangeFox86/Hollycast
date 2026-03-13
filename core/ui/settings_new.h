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

#pragma once

namespace SettingsNew {

// Settings tabs enumeration
enum class SettingsTab
{
	General,
	Video,
	Audio,
	Controls,
	Network,
	Advanced,
	About,
	Count  // Sentinel value for iteration
};

// Settings UI state structure
struct SettingsUIState
{
	SettingsTab currentTab = SettingsTab::General;
};

// Global state instance
extern SettingsUIState g_state;

// Main entry point for the new settings UI
// Renders the full-screen settings interface with navigation rail and content area
void renderSettingsNew();

// Reset state when entering settings screen
void resetState();

// Get display name for each tab
const char* getTabName(SettingsTab tab);

// Individual tab renderers
void renderGeneralTab();
void renderVideoTab();
void renderAudioTab();
void renderControlsTab();
void renderNetworkTab();
void renderAdvancedTab();
void renderAboutTab();
void focusBoxArtSection();
void openTab(SettingsTab tab);

} // namespace SettingsNew
