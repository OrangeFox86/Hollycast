/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace postprocess {

enum class ReShadeValueType {
	Unknown,
	Bool,
	Int,
	UInt,
	Float,
	Float2,
	Float3,
	Float4,
	Float2x2,
	Float3x3,
	Float4x4,
};

struct ReShadeAnnotation {
	std::string name;
	std::string value;
};

struct ReShadeUniform {
	ReShadeValueType valueType = ReShadeValueType::Unknown;
	std::string typeName;
	std::string name;
	std::string defaultValue;
	std::vector<ReShadeAnnotation> annotations;
};

struct ReShadeTexture {
	std::string typeName;
	std::string name;
	std::string definition;
	std::vector<ReShadeAnnotation> annotations;
};

struct ReShadeSampler {
	std::string typeName;
	std::string name;
	std::string definition;
	std::vector<ReShadeAnnotation> annotations;
};

struct ReShadeStruct {
	std::string name;
	std::string body;
};

struct ReShadeFunction {
	std::string returnType;
	std::string name;
	std::string signature;
	std::string body;
	size_t line = 0;
};

struct ReShadePass {
	std::string name;
	std::string vertexShader;
	std::string pixelShader;
	std::string stateBlock;
	size_t line = 0;
};

struct ReShadeTechnique {
	std::string name;
	std::vector<ReShadePass> passes;
	size_t line = 0;
};

// This is a syntax IR, not a compiler. It deliberately preserves declarations
// and bodies so later backend emitters can lower one validated representation.
struct ReShadeEffect {
	std::vector<std::string> preprocessorDirectives;
	std::vector<ReShadeUniform> uniforms;
	std::vector<ReShadeTexture> textures;
	std::vector<ReShadeSampler> samplers;
	std::vector<ReShadeStruct> structs;
	std::vector<std::string> globals;
	std::vector<ReShadeFunction> functions;
	std::vector<ReShadeTechnique> techniques;
	std::optional<size_t> selectedTechnique;
	std::optional<size_t> selectedPass;
};

// Shared deterministic preprocessor used by ReShade FX and RetroArch GLSL.
// Includes are resolved by the package loader before this function is called.
bool preprocessShaderSource(const std::string& source,
		const std::unordered_map<std::string, std::string>& initialDefines,
		std::string& output, std::string& error);

// Lexes and parses ReShade FX declarations into a backend-neutral syntax IR.
// Includes are rejected because effect packages are not trusted to escape their
// selected root. Compilation and backend emission intentionally happen later.
bool parseReShadeEffect(const std::string& source, ReShadeEffect& effect, std::string& error);

} // namespace postprocess
