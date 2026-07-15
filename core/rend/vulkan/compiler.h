/*
    Created on: Nov 6, 2019

	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

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
#include "vulkan.h"

class ShaderCompiler
{
public:
	static void Init();
	static void Term();
	static vk::UniqueShaderModule Compile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText);
	// User shader selection is recoverable: unlike built-in shaders, a bad file
	// must leave the previously active post-processing program untouched.
	static bool TryCompile(vk::ShaderStageFlagBits shaderStage, std::string const& shaderText,
			vk::UniqueShaderModule& shaderModule, std::string& error);
	// Syntax and SPIR-V validation without requiring a live Vulkan device.
	static bool Validate(vk::ShaderStageFlagBits shaderStage, const std::string& shaderText, std::string& error);
private:
	static int initCount;
};
