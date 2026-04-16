/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#pragma once
#include "postfx_preset.h"

#include <cstdint>
#include <string>
#include <vector>

namespace postfx
{

// Call from render backends once per frame (or before use) to pick up config changes.
void Tick();

bool IsEnabled();
uint64_t GetConfigVersion();

// Returns nullptr if disabled or preset failed to load.
const Preset* GetActivePreset();
const std::string& GetLastError();
void SetRuntimeError(const std::string& error);
void ClearRuntimeError();

// Lists all available presets (embedded + external files discovered at startup).
std::vector<std::string> ListAvailablePresets();
// Lists built-in presets from embedded resources (paths under shaders/postfx/presets/).
std::vector<std::string> ListBuiltInPresets();

// Parameter access (values persisted in the main config by preset).
bool GetParamValue(const std::string& id, float& outValue);
void SetParamValue(const std::string& id, float value);

} // namespace postfx
