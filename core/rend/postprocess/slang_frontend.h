/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace postprocess {

enum class SlangParameterType {
	Float,
	Int,
	Bool,
	Float2,
	Float3,
	Float4,
	Float2x2,
	Float3x3,
	Float4x4,
};

struct SlangParameterValue {
	std::array<float, 16> values {};
	uint32_t componentCount = 1;
};

struct SlangParameter {
	SlangParameterType type = SlangParameterType::Float;
	std::string name;
	std::string label;
	SlangParameterValue defaultValue;
	SlangParameterValue minimum;
	SlangParameterValue maximum;
	SlangParameterValue step;
};

enum class SlangResourceType {
	Sampler1D,
	Sampler2D,
	Sampler2DArray,
	Sampler3D,
	SamplerCube,
	Texture1D,
	Texture2D,
	Texture2DArray,
	Texture3D,
	TextureCube,
	Image1D,
	Image2D,
	Image2DArray,
	Image3D,
};

enum class SlangResourceConvention {
	Source,
	Original,
	PassAlias,
	History,
	Feedback,
};

struct SlangResource {
	SlangResourceType type = SlangResourceType::Sampler2D;
	SlangResourceConvention convention = SlangResourceConvention::PassAlias;
	std::string name;
	std::string alias;
	uint32_t arrayCount = 1;
	uint32_t bindingSet = 0;
	uint32_t binding = 0;
	bool hasExplicitBinding = false;
	// History2 means two completed frames ago. Plain History is offset one.
	uint32_t historyOffset = 1;
};

struct SlangStageSource {
	std::string source;
	bool declared = false;
};

// Backend-neutral result of parsing already-expanded Slang source. The frontend
// intentionally records declarations and stage text only; no backend code is
// generated or selected here.
struct SlangShaderIR {
	std::string displayName;
	std::string format;
	SlangStageSource vertex;
	SlangStageSource fragment;
	std::vector<SlangParameter> parameters;
	std::vector<SlangResource> resources;
};

bool parseSlangFrontend(const std::string& shaderText, SlangShaderIR& shader, std::string& error);

} // namespace postprocess
