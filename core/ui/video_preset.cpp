#include "video_preset.h"
#include "cfg/option.h"

namespace SettingsNew {

// ============================================================
// PRESET DEFINITIONS
// ============================================================
// Easy to extend - just add settings to each array

static const PresetSetting potatoSettings[] = {
	{"RenderResolution", 480},                    // 0.75x - 480x360
	{"AnisotropicFiltering", 0},                  // Disabled
	{"AutoSkipFrame", 2},                         // Maximum skip
	{"DupeFrames", true},                         // Enable duplicate frames
	{"VSync", false},                             // Disable VSync
	{"PerStripSorting", true},                    // Fastest sorting
	{"RenderToTextureBuffer", false},             // Disable
	{"ThreadedRendering", true},                  // Enable threading
};

static const PresetSetting lowSettings[] = {
	{"RenderResolution", 640},                    // 1x - 640x480 (Native)
	{"AnisotropicFiltering", 0},                  // Disabled
	{"AutoSkipFrame", 1},                         // Normal skip
	{"DupeFrames", true},                         // Enable duplicate frames
	{"VSync", false},                             // Disable VSync
	{"PerStripSorting", true},                    // Fastest sorting
	{"RenderToTextureBuffer", false},             // Disable
	{"ThreadedRendering", true},                  // Enable threading
};

static const PresetSetting mediumSettings[] = {
	{"RenderResolution", 960},                    // 1.5x - 960x720
	{"AnisotropicFiltering", 1},                  // 2x
	{"AutoSkipFrame", 0},                         // No skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", true},                              // Enable VSync
	{"PerStripSorting", true},                    // Fastest sorting
	{"RenderToTextureBuffer", false},             // Disable
	{"ThreadedRendering", true},                  // Enable threading
};

static const PresetSetting highSettings[] = {
	{"RenderResolution", 1280},                   // 2x - 1280x960
	{"AnisotropicFiltering", 3},                  // 8x
	{"AutoSkipFrame", 0},                         // No skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", true},                              // Enable VSync
	{"PerStripSorting", false},                   // Per Triangle
	{"RenderToTextureBuffer", true},              // Enable
	{"ThreadedRendering", true},                  // Enable threading
};

static const PresetSetting godModeSettings[] = {
	{"RenderResolution", 1920},                   // 3x - 1920x1440
	{"AnisotropicFiltering", 4},                  // 16x (max)
	{"AutoSkipFrame", 0},                         // No skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", true},                              // Enable VSync
	{"PerStripSorting", false},                   // Per Triangle
	{"RenderToTextureBuffer", true},              // Enable
	{"ThreadedRendering", true},                  // Enable threading
};

// ============================================================
// PRESET REGISTRY
// ============================================================

static const VideoPreset videoPresets[] = {
	{
		"Potato",
		"Maximum performance, minimum quality. Best for low-end devices.",
		potatoSettings,
		sizeof(potatoSettings) / sizeof(PresetSetting)
	},
	{
		"Low",
		"Better performance with reduced quality.",
		lowSettings,
		sizeof(lowSettings) / sizeof(PresetSetting)
	},
	{
		"Medium",
		"Balanced performance and quality.",
		mediumSettings,
		sizeof(mediumSettings) / sizeof(PresetSetting)
	},
	{
		"High",
		"High quality with good performance requirements.",
		highSettings,
		sizeof(highSettings) / sizeof(PresetSetting)
	},
	{
		"God Mode",
		"Maximum quality. Requires powerful hardware.",
		godModeSettings,
		sizeof(godModeSettings) / sizeof(PresetSetting)
	},
};

// ============================================================
// SETTING APPLICATION
// ============================================================

static void applySetting(const PresetSetting& setting) {
	// String-based matching for easy extensibility
	std::string name = setting.name;

	if (name == "RenderResolution") {
		config::RenderResolution.set(std::get<int>(setting.value));
	}
	else if (name == "AnisotropicFiltering") {
		config::AnisotropicFiltering.set(std::get<int>(setting.value));
	}
	else if (name == "AutoSkipFrame") {
		config::AutoSkipFrame.set(std::get<int>(setting.value));
	}
	else if (name == "DupeFrames") {
		config::DupeFrames.set(std::get<bool>(setting.value));
	}
	else if (name == "VSync") {
		config::VSync.set(std::get<bool>(setting.value));
	}
	else if (name == "PerStripSorting") {
		config::PerStripSorting.set(std::get<bool>(setting.value));
	}
	else if (name == "RenderToTextureBuffer") {
		config::RenderToTextureBuffer.set(std::get<bool>(setting.value));
	}
	else if (name == "ThreadedRendering") {
		config::ThreadedRendering.set(std::get<bool>(setting.value));
	}
	// Add new settings here when extending the system
}

// ============================================================
// PUBLIC API IMPLEMENTATION
// ============================================================

const VideoPreset* getVideoPresets() {
	return videoPresets;
}

size_t getVideoPresetCount() {
	return sizeof(videoPresets) / sizeof(VideoPreset);
}

const VideoPreset* getPresetByLevel(VideoPresetLevel level) {
	if (level == VideoPresetLevel::Custom)
		return nullptr;
	return &videoPresets[static_cast<size_t>(level)];
}

void applyVideoPreset(const VideoPreset& preset) {
	for (size_t i = 0; i < preset.settingCount; ++i) {
		applySetting(preset.settings[i]);
	}
}

VideoPresetLevel detectCurrentPreset() {
	// Check each preset to see if current settings match
	for (size_t i = 0; i < getVideoPresetCount(); ++i) {
		const VideoPreset& preset = videoPresets[i];
		bool match = true;

		for (size_t j = 0; j < preset.settingCount; ++j) {
			const PresetSetting& setting = preset.settings[j];
			std::string name = setting.name;

			// Check if setting matches current value
			if (name == "RenderResolution") {
				if (config::RenderResolution.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "AnisotropicFiltering") {
				if (config::AnisotropicFiltering.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "AutoSkipFrame") {
				if (config::AutoSkipFrame.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "DupeFrames") {
				if (config::DupeFrames.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "VSync") {
				if (config::VSync.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "PerStripSorting") {
				if (config::PerStripSorting.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "RenderToTextureBuffer") {
				if (config::RenderToTextureBuffer.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "ThreadedRendering") {
				if (config::ThreadedRendering.get() != std::get<bool>(setting.value))
					match = false;
			}

			if (!match) break;
		}

		if (match) return static_cast<VideoPresetLevel>(i);
	}

	return VideoPresetLevel::Custom;
}

} // namespace SettingsNew
