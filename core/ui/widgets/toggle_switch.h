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

#include "types.h"
#include "imgui.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include <string>

namespace Widgets {

// Toggle switch visual style configuration
// Note: Default colors are placeholders - actual rendering uses theme colors
// when no custom style is provided
struct ToggleStyle {
	// Dimensions (will be scaled by UI scale)
	float width = 50.0f;       // Total width of the toggle
	float height = 26.0f;      // Total height of the toggle
	float knobSize = 22.0f;    // Diameter of the knob (circle)
	float knobRadius = 11.0f;  // Radius of the knob

	// Colors (32-bit RGBA) - these defaults are overridden by theme colors
	// when no custom style is provided to ToggleSwitch()
	ImU32 colorOn = 0;         // Set dynamically from ImGuiCol_ButtonActive
	ImU32 colorOff = 0;        // Set dynamically from ImGuiCol_FrameBg
	ImU32 colorOnHover = 0;    // Set dynamically (brighter ButtonActive)
	ImU32 colorOffHover = 0;   // Set dynamically from ImGuiCol_FrameBgHovered
	ImU32 colorDisabled = 0;   // Set dynamically (dimmed FrameBg)
	ImU32 knobColor = 0;       // Set dynamically based on theme brightness

	// Animation
	float animationSpeed = 0.15f; // Animation duration in seconds

	// Layout
	bool alignRight = true;      // Align toggle to the right side
	float rightMargin = 20.0f;   // Right margin when aligning
};

// iOS/Android-style toggle switch with smooth animation
// Returns true if the value was changed (user clicked)
//
// Parameters:
//   label      - Unique identifier for the toggle (use "##" prefix for invisible labels)
//   value      - Pointer to boolean value to toggle
//   tooltip    - Optional tooltip text to display on hover
//   disabled   - If true, the toggle is disabled (grayed out, no interaction)
//   style      - Optional custom style (uses default if nullptr)
//
// Usage:
//   bool enableFeature = true;
//   if (ToggleSwitch("##enable_feature", &enableFeature, "Enable this feature")) {
//       // Handle value change
//   }
bool ToggleSwitch(const char* label, bool* value, const char* tooltip = nullptr,
                  bool disabled = false, const ToggleStyle* style = nullptr);

// Toggle switch with integrated label and alignment
// Automatically positions the toggle on the right side of the available space
//
// Parameters:
//   label      - Text label to display on the left
//   value      - Pointer to boolean value to toggle
//   tooltip    - Optional tooltip text
//   disabled   - If true, the toggle is disabled
//
// Returns true if the value was changed
//
// Usage:
//   bool vsync = true;
//   if (ToggleSwitchOption("V-Sync", &vsync, "Enable vertical synchronization")) {
//       // Value changed
//   }
bool ToggleSwitchOption(const char* label, bool* value, const char* tooltip = nullptr,
                        bool disabled = false);

// Template version for config::Option integration
// Automatically handles option.readOnly state and saves changes
template<bool PerGameOption>
bool ToggleSwitchOption(const char* label, config::Option<bool, PerGameOption>& option,
                        const char* tooltip = nullptr)
{
	bool value = option;
	bool disabled = option.isReadOnly();
	bool changed = ToggleSwitchOption(label, &value, tooltip, disabled);
	if (changed && !disabled)
		option = value;
	return changed;
}

// Helper to set a fixed column width for perfect alignment
// Call this at the start of a settings section to ensure all toggles align
//
// Usage:
//   BeginToggleColumn(300.0f);  // Reserve 300px for labels
//   ToggleSwitchOption("Option 1", &val1);
//   ToggleSwitchOption("Option 2", &val2);
//   ToggleSwitchOption("Very Long Option Name", &val3);  // All toggles align
//   EndToggleColumn();
void BeginToggleColumn(float labelWidth = 300.0f);

void EndToggleColumn();

// Get default toggle style (can be customized by the application)
ToggleStyle& GetDefaultToggleStyle();

} // namespace Widgets
