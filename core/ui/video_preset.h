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
#include "cfg/option.h"
#include <cstdint>
#include <string>
#include <variant>

namespace SettingsNew {

// Type-erased value wrapper to handle different setting types
using PresetValue = std::variant<int, int64_t, bool, float>;

// Individual setting entry within a preset
struct PresetSetting {
	const char* name;           // Setting name (for matching)
	PresetValue value;          // Value to apply
};

// Complete preset definition
struct VideoPreset {
	const char* name;           // Display name
	const char* description;    // Tooltip description
	const PresetSetting* settings;  // Array of settings
	size_t settingCount;        // Number of settings
};

// Preset level enumeration
enum class VideoPresetLevel {
	Potato = 0,
	Low,
	Medium,
	High,
	GodMode,
	Custom  // When user manually modifies settings
};

// Public API
const VideoPreset* getVideoPresets();
size_t getVideoPresetCount();
const VideoPreset* getPresetByLevel(VideoPresetLevel level);
void applyVideoPreset(const VideoPreset& preset);
VideoPresetLevel detectCurrentPreset();

} // namespace SettingsNew
