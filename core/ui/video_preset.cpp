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

#include "video_preset.h"
#include "cfg/option.h"

namespace SettingsNew {

// ============================================================
// PRESET DEFINITIONS
// ============================================================
// Easy to extend - just add settings to each array

static const PresetSetting potatoSettings[] = {
	{"Sh4Clock", 200},                            // 100% SH4 clock
	{"RenderResolution", 320},                    // Ultra-low resolution
	{"AnisotropicFiltering", 1},                  // 2x
	{"AutoSkipFrame", 2},                         // Maximum skip
	{"SkipFrame", 0},                             // No fixed skip
	{"DupeFrames", false},                        // Keep frame pacing clean
	{"VSync", true},                              // Match current potato build
	{"PerStripSorting", true},                    // Per-strip sorting
	{"RenderToTextureBuffer", false},             // Disable RTT buffer
	{"ThreadedRendering", true},                  // Enable threading
	{"TextureFiltering", 0},                      // Default
	{"TextureUpscale", 1},                        // Off/1x
	{"MaxFilteredTextureSize", 256},              // Small filtered textures only
	{"PerPixelLayers", 32},                       // Default layer budget
	{"PixelBufferSize", 536870912LL},             // 512 MB
	{"NativeDepthInterpolation", true},           // Keep native depth behavior
	{"EmulateFramebuffer", false},                // Disable FB emulation
	{"UseMipmaps", true},                         // Keep mipmaps on
	{"FixUpscaleBleedingEdge", true},             // Reduce upscale seams
	{"Fog", true},                                // Preserve fog
	{"ModifierVolumes", true},                    // Preserve modifier volumes
	{"IntegerScale", false},                      // Allow free scaling
	{"LinearInterpolation", false},               // Match current display filter
	{"Widescreen", false},                        // 4:3
	{"SuperWidescreen", false},                   // Off
	{"WidescreenGameHacks", false},               // Off
	{"FloatVMUs", false},                         // Dock VMUs
	{"ExtraDepthScale", 1.0f},                    // Default depth scale
	{"MaxThreads", 3},                            // Current multithread setting
};

static const PresetSetting lowSettings[] = {
	{"Sh4Clock", 190},                            // Underclock SH4
	{"RenderResolution", 480},                    // 1x - 640x480 (Native)
	{"AnisotropicFiltering", 0},                  // Disabled
	{"AutoSkipFrame", 1},                         // Normal skip
	{"DupeFrames", true},                         // Enable duplicate frames
	{"VSync", false},                             // Disable VSync
	{"PerStripSorting", false},                   // Per-triangle sorting
	{"RenderToTextureBuffer", false},             // Disable
	{"ThreadedRendering", true},                  // Enable threading
	{"ModifierVolumes", true},                    // Shadows
	{"Fog", true},                                // Fog
	{"DelayFrameSwapping", true},                 // Enable delay frame swapping
};

static const PresetSetting mediumSettings[] = {
	{"Sh4Clock", 215},                            // Mild SH4 overclock
	{"RenderResolution", 720},                    // 1.5x - 960x720
	{"AnisotropicFiltering", 4},                  // 16x
	{"AutoSkipFrame", 0},                         // No skip
	{"SkipFrame", 0},                             // No fixed skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", false},                             // Disable VSync
	{"PerStripSorting", false},                   // Per-triangle sorting
	{"RenderToTextureBuffer", false},             // Disable
	{"ThreadedRendering", true},                  // Enable threading
	{"ModifierVolumes", true},                    // Shadows
	{"Fog", true},                                // Fog
	{"DelayFrameSwapping", true},                 // Enable delay frame swapping
};

static const PresetSetting highSettings[] = {
	{"Sh4Clock", 220},                            // Overclock SH4
	{"RenderResolution", 1200},                   // 2.5x - 1600x1200
	{"AnisotropicFiltering", 3},                  // 8x
	{"AutoSkipFrame", 0},                         // No skip
	{"SkipFrame", 0},                             // No fixed skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", true},                              // Enable VSync
	{"PerStripSorting", false},                   // Per-triangle sorting
	{"RenderToTextureBuffer", true},              // Enable
	{"ThreadedRendering", true},                  // Enable threading
	{"PerPixelLayers", 64},                       // Higher OIT layer budget
	{"ModifierVolumes", true},                    // Shadows
	{"Fog", true},                                // Fog
};

static const PresetSetting godModeSettings[] = {
	{"Sh4Clock", 250},                            // Overclock SH4
	{"RenderResolution", 1920},                   // 4x - 2560x1920
	{"AnisotropicFiltering", 4},                  // 16x (max)
	{"AutoSkipFrame", 0},                         // No skip
	{"SkipFrame", 0},                             // No fixed skip
	{"DupeFrames", false},                        // Disable duplicate frames
	{"VSync", true},                              // Enable VSync
	{"PerStripSorting", false},                   // Per Triangle
	{"RenderToTextureBuffer", true},              // Enable
	{"ThreadedRendering", true},                  // Enable threading
	{"ModifierVolumes", true},                    // Shadows
	{"Fog", true},                                // Fog
};

// ============================================================
// PRESET REGISTRY
// ============================================================

static const VideoPreset videoPresets[] = {
		{
			"Potato",
			"Potato\n"
			"For the oldest and weakest machines that will not play on higher presets, even with the lowest resolution. Use this only when nothing else can reach full speed. It is more of an \"any type of playing is better than no playing\" setup.\n\n"
			"Expect some graphics glitching due to per-strip sorting. Some games may still drop frames because of the SH4 underclock and maximum auto frame skipping. If you have a little headroom, raise resolution in small steps to bring image quality back up.",
			potatoSettings,
			sizeof(potatoSettings) / sizeof(PresetSetting)
		},
		{
			"Low",
			"Low\n"
			"SH4 underclock is reduced to the minimum needed to make resource requirements slightly lower, and auto frame skipping is reduced but still enabled when the CPU or GPU hits its limit. You may still see slight frame dips in heavier scenes to help keep full speed.\n\n"
			"Per-triangle sorting keeps graphics cleaner than Potato while still being much lighter than per-pixel. At native resolution this is a good jump in image quality. It is the recommended starting point for most low-end devices that can handle it. If you are close to full speed, lower resolution a step or two before dropping to Potato.",
			lowSettings,
			sizeof(lowSettings) / sizeof(PresetSetting)
		},
		{
			"Medium",
			"Medium\n"
			"Most games should play with no or very minimal graphics issues here. Only a small handful of titles really need per-pixel sorting to be perfect with layering.\n\n"
			"With no frame skipping and a modest SH4 overclock to 215 MHz, games can run as they did on Dreamcast as long as the device has the power. This also helps titles that had native frame drops. Mipmapped textures and 16x anisotropic filtering reduce shimmering for a cleaner image.",
			mediumSettings,
			sizeof(mediumSettings) / sizeof(PresetSetting)
		},
		{
			"High",
			"High\n"
			"This is for higher-end devices. Perfect sorting means games do not risk transparency sorting glitches, so layering should render correctly everywhere.\n\n"
			"The stronger SH4 overclock helps heavier titles hold better-than-native frame rates, and the higher resolutions give you a very sharp, smooth image with clean, stable graphics.",
			highSettings,
			sizeof(highSettings) / sizeof(PresetSetting)
		},
		{
			"God Mode",
			"God Tier\n"
			"This is the preset for when you have all the horsepower and want the sharpest possible image. It is built around a super-sharp 4K-style presentation, heavy image quality, and a strong overclock.\n\n"
			"It should prevent frame drops across essentially all Dreamcast titles, which was one of the original hardware's biggest weak spots, while giving you the cleanest and most oversampled image of the preset tiers.",
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
	else if (name == "Sh4Clock") {
		config::Sh4Clock.set(std::get<int>(setting.value));
	}
	else if (name == "AnisotropicFiltering") {
		config::AnisotropicFiltering.set(std::get<int>(setting.value));
	}
	else if (name == "AutoSkipFrame") {
		config::AutoSkipFrame.set(std::get<int>(setting.value));
	}
	else if (name == "SkipFrame") {
		config::SkipFrame.set(std::get<int>(setting.value));
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
	else if (name == "DelayFrameSwapping") {
		config::DelayFrameSwapping.set(std::get<bool>(setting.value));
	}
	else if (name == "TextureFiltering") {
		config::TextureFiltering.set(std::get<int>(setting.value));
	}
	else if (name == "TextureUpscale") {
		config::TextureUpscale.set(std::get<int>(setting.value));
	}
	else if (name == "MaxFilteredTextureSize") {
		config::MaxFilteredTextureSize.set(std::get<int>(setting.value));
	}
	else if (name == "PerPixelLayers") {
		config::PerPixelLayers.set(std::get<int>(setting.value));
	}
	else if (name == "PixelBufferSize") {
		config::PixelBufferSize.set(std::get<int64_t>(setting.value));
	}
	else if (name == "NativeDepthInterpolation") {
		config::NativeDepthInterpolation.set(std::get<bool>(setting.value));
	}
	else if (name == "EmulateFramebuffer") {
		config::EmulateFramebuffer.set(std::get<bool>(setting.value));
	}
	else if (name == "UseMipmaps") {
		config::UseMipmaps.set(std::get<bool>(setting.value));
	}
	else if (name == "FixUpscaleBleedingEdge") {
		config::FixUpscaleBleedingEdge.set(std::get<bool>(setting.value));
	}
	else if (name == "Fog") {
		config::Fog.set(std::get<bool>(setting.value));
	}
	else if (name == "ModifierVolumes") {
		config::ModifierVolumes.set(std::get<bool>(setting.value));
	}
	else if (name == "IntegerScale") {
		config::IntegerScale.set(std::get<bool>(setting.value));
	}
	else if (name == "LinearInterpolation") {
		config::LinearInterpolation.set(std::get<bool>(setting.value));
	}
	else if (name == "Widescreen") {
		config::Widescreen.set(std::get<bool>(setting.value));
	}
	else if (name == "SuperWidescreen") {
		config::SuperWidescreen.set(std::get<bool>(setting.value));
	}
	else if (name == "WidescreenGameHacks") {
		config::WidescreenGameHacks.set(std::get<bool>(setting.value));
	}
	else if (name == "FloatVMUs") {
		config::FloatVMUs.set(std::get<bool>(setting.value));
	}
	else if (name == "ExtraDepthScale") {
		config::ExtraDepthScale.set(std::get<float>(setting.value));
	}
	else if (name == "MaxThreads") {
		config::MaxThreads.set(std::get<int>(setting.value));
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
			else if (name == "Sh4Clock") {
				if (config::Sh4Clock.get() != std::get<int>(setting.value))
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
			else if (name == "SkipFrame") {
				if (config::SkipFrame.get() != std::get<int>(setting.value))
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
			else if (name == "DelayFrameSwapping") {
				if (config::DelayFrameSwapping.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "TextureFiltering") {
				if (config::TextureFiltering.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "TextureUpscale") {
				if (config::TextureUpscale.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "MaxFilteredTextureSize") {
				if (config::MaxFilteredTextureSize.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "PerPixelLayers") {
				if (config::PerPixelLayers.get() != std::get<int>(setting.value))
					match = false;
			}
			else if (name == "PixelBufferSize") {
				if (config::PixelBufferSize.get() != std::get<int64_t>(setting.value))
					match = false;
			}
			else if (name == "NativeDepthInterpolation") {
				if (config::NativeDepthInterpolation.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "EmulateFramebuffer") {
				if (config::EmulateFramebuffer.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "UseMipmaps") {
				if (config::UseMipmaps.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "FixUpscaleBleedingEdge") {
				if (config::FixUpscaleBleedingEdge.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "Fog") {
				if (config::Fog.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "ModifierVolumes") {
				if (config::ModifierVolumes.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "IntegerScale") {
				if (config::IntegerScale.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "LinearInterpolation") {
				if (config::LinearInterpolation.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "Widescreen") {
				if (config::Widescreen.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "SuperWidescreen") {
				if (config::SuperWidescreen.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "WidescreenGameHacks") {
				if (config::WidescreenGameHacks.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "FloatVMUs") {
				if (config::FloatVMUs.get() != std::get<bool>(setting.value))
					match = false;
			}
			else if (name == "ExtraDepthScale") {
				if (config::ExtraDepthScale.get() != std::get<float>(setting.value))
					match = false;
			}
			else if (name == "MaxThreads") {
				if (config::MaxThreads.get() != std::get<int>(setting.value))
					match = false;
			}

			if (!match) break;
		}

		if (match) return static_cast<VideoPresetLevel>(i);
	}

	return VideoPresetLevel::Custom;
}

} // namespace SettingsNew
