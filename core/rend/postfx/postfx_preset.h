/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#pragma once
#include <string>
#include <vector>

namespace postfx
{

enum class ScaleType
{
	Source,
	Viewport,
	Absolute,
};

enum class Filter
{
	Nearest,
	Linear,
};

struct ParameterDef
{
	std::string id;
	std::string label;
	bool integer = false;
	float defaultValue = 0.f;
	float minValue = 0.f;
	float maxValue = 1.f;
	float step = 0.01f;
};

struct PassDef
{
	struct SamplerBinding
	{
		std::string name;
		std::string source; // "$prev" or named FX render target
		Filter filter = Filter::Linear;
	};

	std::string shaderPath;
	std::string shaderBody; // stripped of #version / #pragma parameter and common builtins
	std::string renderTargetName; // FX-only: symbolic RenderTarget name

	ScaleType scaleType = ScaleType::Viewport;
	float scaleX = 1.f;
	float scaleY = 1.f;
	Filter filter = Filter::Linear;
	bool srgb = false;
	std::vector<SamplerBinding> samplers;
};

struct Preset
{
	std::string id; // preset path/identifier
	std::string name;
	std::vector<PassDef> passes;
	std::vector<ParameterDef> params;
};

bool LoadPreset(const std::string& presetPath, Preset& out, std::string& error);

std::vector<ParameterDef> ParsePragmaParameters(const std::string& shaderText);
std::string StripBuiltinsAndPragmas(const std::string& shaderText);
std::string StripBuiltinsAndPragmas(const std::string& shaderText, const std::vector<ParameterDef>& params);

} // namespace postfx
