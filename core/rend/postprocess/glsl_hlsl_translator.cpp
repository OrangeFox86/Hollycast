/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "shader_chain.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace postprocess {
namespace {

static_assert(MaxShaderParameterComponents == 24,
		"Update the HLSL parameter arrays when changing the shared limit.");

constexpr char HlslPreamble[] = R"(
Texture2D Source : register(t0);
SamplerState SourceSampler : register(s0);

cbuffer ShaderFrame : register(b0)
{
	float2 SourceSize;
	float2 OutputSize;
	int FrameCount;
	int Rotation;
	float2 OriginalSize;
	// HLSL scalar arrays consume one 16-byte register per element. A float4
	// array keeps the CPU's contiguous 16-float upload layout backend-neutral.
	float4 Parameters[6];
};

float GlslMod(float x, float y) { return x - y * floor(x / y); }
float2 GlslMod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 GlslMod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 GlslMod(float4 x, float4 y) { return x - y * floor(x / y); }
float4x4 GlslMat4(float4 c0, float4 c1, float4 c2, float4 c3)
{
	return float4x4(c0.x, c0.y, c0.z, c0.w, c1.x, c1.y, c1.z, c1.w,
			c2.x, c2.y, c2.z, c2.w, c3.x, c3.y, c3.z, c3.w);
}
float4x4 GlslMat4(float v) { return float4x4(v, 0, 0, 0, 0, v, 0, 0, 0, 0, v, 0, 0, 0, 0, v); }
float4x4 GlslMat4(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7,
		float a8, float a9, float a10, float a11, float a12, float a13, float a14, float a15)
{
	return float4x4(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
}
float3x3 GlslMat3(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8)
{
	return float3x3(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}
float3x3 GlslMat3(float v) { return float3x3(v, 0, 0, 0, v, 0, 0, 0, v); }
// GLSL indexes matrices by column. The bridge stores those columns as HLSL
// rows, so the mul argument order is intentionally reversed here.
float2 GlslMul(float2x2 m, float2 v) { return mul(v, m); }
float2 GlslMul(float2 v, float2x2 m) { return mul(m, v); }
float3 GlslMul(float3x3 m, float3 v) { return mul(v, m); }
float3 GlslMul(float3 v, float3x3 m) { return mul(m, v); }
float4 GlslMul(float4x4 m, float4 v) { return mul(v, m); }
float4 GlslMul(float4 v, float4x4 m) { return mul(m, v); }
float3 GlslMul(float4x3 m, float4 v) { return mul(v, m); }
float2 GlslGreaterThan(float2 a, float2 b) { return (float2)(a > b); }
float3 GlslGreaterThan(float3 a, float3 b) { return (float3)(a > b); }
float4 GlslGreaterThan(float4 a, float4 b) { return (float4)(a > b); }

struct PixelInput
{
	float4 position : SV_POSITION;
	float2 vTexCoord : TEXCOORD0;
};

)";

constexpr char HlslVertexPreamble[] = R"(
Texture2D Source : register(t0);
SamplerState SourceSampler : register(s0);

cbuffer ShaderFrame : register(b0)
{
	float2 SourceSize;
	float2 OutputSize;
	int FrameCount;
	int Rotation;
	float2 OriginalSize;
	// Keep this layout identical to ShaderFrameUniforms and the pixel stage.
	float4 Parameters[6];
};

float GlslMod(float x, float y) { return x - y * floor(x / y); }
float2 GlslMod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 GlslMod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 GlslMod(float4 x, float4 y) { return x - y * floor(x / y); }
float4x4 GlslMat4(float4 c0, float4 c1, float4 c2, float4 c3)
{
	return float4x4(c0.x, c0.y, c0.z, c0.w, c1.x, c1.y, c1.z, c1.w,
			c2.x, c2.y, c2.z, c2.w, c3.x, c3.y, c3.z, c3.w);
}
float4x4 GlslMat4(float v) { return float4x4(v, 0, 0, 0, 0, v, 0, 0, 0, 0, v, 0, 0, 0, 0, v); }
float4x4 GlslMat4(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7,
		float a8, float a9, float a10, float a11, float a12, float a13, float a14, float a15)
{
	return float4x4(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
}
float3x3 GlslMat3(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8)
{
	return float3x3(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}
float3x3 GlslMat3(float v) { return float3x3(v, 0, 0, 0, v, 0, 0, 0, v); }
float2 GlslMul(float2x2 m, float2 v) { return mul(v, m); }
float2 GlslMul(float2 v, float2x2 m) { return mul(m, v); }
float3 GlslMul(float3x3 m, float3 v) { return mul(v, m); }
float3 GlslMul(float3 v, float3x3 m) { return mul(m, v); }
float4 GlslMul(float4x4 m, float4 v) { return mul(v, m); }
float4 GlslMul(float4 v, float4x4 m) { return mul(m, v); }
float3 GlslMul(float4x3 m, float4 v) { return mul(v, m); }
float2 GlslGreaterThan(float2 a, float2 b) { return (float2)(a > b); }
float3 GlslGreaterThan(float3 a, float3 b) { return (float3)(a > b); }
float4 GlslGreaterThan(float4 a, float4 b) { return (float4)(a > b); }

struct VertexInput
{
	float2 position : POSITION;
	float2 texCoord : TEXCOORD0;
};

struct VertexOutput
{
	float4 position : SV_POSITION;
	float2 vTexCoord : TEXCOORD0;
};

)";

bool contains(const std::string& source, const char *pattern)
{
	return std::regex_search(source, std::regex(pattern));
}

std::string stripComments(const std::string& source)
{
	std::string result = source;
	bool lineComment = false;
	bool blockComment = false;
	for (size_t index = 0; index < result.size(); ++index) {
		const char current = result[index];
		const char next = index + 1 < result.size() ? result[index + 1] : '\0';
		if (lineComment) {
			if (current == '\n')
				lineComment = false;
			else
				result[index] = ' ';
			continue;
		}
		if (blockComment) {
			result[index] = current == '\n' ? '\n' : ' ';
			if (current == '*' && next == '/') {
				result[index + 1] = ' ';
				++index;
				blockComment = false;
			}
			continue;
		}
		if (current == '/' && next == '/') {
			result[index] = result[index + 1] = ' ';
			++index;
			lineComment = true;
		}
		else if (current == '/' && next == '*') {
			result[index] = result[index + 1] = ' ';
			++index;
			blockComment = true;
		}
	}
	return result;
}

void replaceAll(std::string& source, const std::regex& pattern, const std::string& replacement)
{
	source = std::regex_replace(source, pattern, replacement);
}

std::vector<std::pair<size_t, size_t>> argumentRanges(const std::string& source, size_t begin, size_t end);
bool findClosingParenthesis(const std::string& source, size_t open, size_t& close);

std::string normalizeHlslVectorConstructors(const std::string& source)
{
	std::string result;
	result.reserve(source.size());
	for (size_t index = 0; index < source.size();) {
		uint32_t components = 0;
		const bool startsToken = index == 0
				|| (!std::isalnum(static_cast<unsigned char>(source[index - 1])) && source[index - 1] != '_');
		if (startsToken && source.compare(index, 6, "float2") == 0)
			components = 2;
		else if (startsToken && source.compare(index, 6, "float3") == 0)
			components = 3;
		else if (startsToken && source.compare(index, 6, "float4") == 0)
			components = 4;
		if (components == 0) {
			result += source[index++];
			continue;
		}
		size_t open = index + 6;
		while (open < source.size() && std::isspace(static_cast<unsigned char>(source[open])))
			++open;
		if (open == source.size() || source[open] != '(') {
			result.append(source, index, open - index);
			index = open;
			continue;
		}
		int depth = 1;
		bool hasTopLevelComma = false;
		size_t close = open + 1;
		for (; close < source.size() && depth != 0; ++close) {
			if (source[close] == '(')
				++depth;
			else if (source[close] == ')')
				--depth;
			else if (source[close] == ',' && depth == 1)
				hasTopLevelComma = true;
		}
		if (depth != 0) {
			result.append(source, index, std::string::npos);
			break;
		}
		const std::string arguments = normalizeHlslVectorConstructors(
				source.substr(open + 1, close - open - 2));
		const std::string type = "float" + std::to_string(components);
		// GLSL splats a scalar in vecN(x). HLSL constructor syntax does not,
		// but its explicit numeric cast preserves scalar splats and vector copies.
		result += hasTopLevelComma ? type + "(" + arguments + ")"
				: "(" + type + ")(" + arguments + ")";
		index = close;
	}
	return result;
}

std::string stripPreprocessorLines(const std::string& source)
{
	std::string result = source;
	size_t lineStart = 0;
	while (lineStart < result.size()) {
		const size_t lineEnd = result.find('\n', lineStart);
		const size_t end = lineEnd == std::string::npos ? result.size() : lineEnd;
		const size_t directive = result.find_first_not_of(" \t\r", lineStart);
		if (directive < end && result[directive] == '#')
			for (size_t index = lineStart; index < end; ++index)
				if (result[index] != '\r')
					result[index] = ' ';
		lineStart = lineEnd == std::string::npos ? result.size() : lineEnd + 1;
	}
	return result;
}

void lowerGlslMatrixMultiplication(std::string& source)
{
	// The portable graph uses non-square matrices as indexed column packs, not
	// multiplication operands. Excluding them also avoids rewriting unrelated
	// vector locals which legitimately shadow those short-lived pack names.
	static const std::regex declaration(R"(\bmat[234]\s+([A-Za-z_][A-Za-z0-9_]*))");
	static const std::regex functionDeclaration(R"(\bmat[234]\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()");
	std::vector<std::string> functions;
	for (std::sregex_iterator it(source.begin(), source.end(), functionDeclaration), end; it != end; ++it)
		functions.push_back((*it)[1].str());

	std::vector<std::string> matrices;
	for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it) {
		const std::string name = (*it)[1].str();
		if (std::find(functions.begin(), functions.end(), name) == functions.end()
				&& std::find(matrices.begin(), matrices.end(), name) == matrices.end())
			matrices.push_back(name);
	}
	for (const std::string& matrix : matrices) {
		replaceAll(source, std::regex("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\*=\\s*" + matrix + "\\b"),
				"$1 = GlslMul($1, " + matrix + ")");
		replaceAll(source, std::regex("\\b" + matrix + R"(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*))"),
				"GlslMul(" + matrix + ", $1)");
		replaceAll(source, std::regex("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*\\*\\s*" + matrix + "\\b"),
				"GlslMul($1, " + matrix + ")");
	}
	for (const std::string& function : functions) {
		// Function and constructor arguments in the portable subset are already
		// expression-balanced before this pass; nested calls remain rejected by
		// the normal HLSL compiler diagnostics rather than being misrewritten.
		replaceAll(source, std::regex("\\b(" + function
				+ R"(\s*\([^()]*\))\s*\*\s*(vec[234]\s*\([^()]*\)|[A-Za-z_][A-Za-z0-9_]*))"),
				"GlslMul($1, $2)");
	}
}

void lowerGlslVectorEquality(std::string& source)
{
	const std::string value = R"(([A-Za-z_][A-Za-z0-9_]*(?:\.[rgbaxyzw]{2,4})?))";
	const std::string constructor = R"(((?:[iu]?vec[234])\s*\([^()]*\)))";
	replaceAll(source, std::regex(value + R"(\s*==\s*)" + constructor), "all($1 == $2)");
	replaceAll(source, std::regex(value + R"(\s*!=\s*)" + constructor), "any($1 != $2)");
	replaceAll(source, std::regex(constructor + R"(\s*==\s*)" + value), "all($1 == $2)");
	replaceAll(source, std::regex(constructor + R"(\s*!=\s*)" + value), "any($1 != $2)");
}

bool lowerGlslRelationalConversions(std::string& source, std::string& error)
{
	static const std::regex conversion(R"(\bfloat\s*\(\s*greaterThan\s*\()");
	std::vector<std::pair<std::pair<size_t, size_t>, std::string>> replacements;
	const std::string searchable = stripPreprocessorLines(stripComments(source));
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), conversion), end; it != end; ++it) {
		const size_t position = static_cast<size_t>(it->position());
		const size_t outerOpen = searchable.find('(', position);
		const size_t innerOpen = searchable.find('(', outerOpen + 1);
		size_t innerClose;
		if (outerOpen == std::string::npos || innerOpen == std::string::npos
				|| !findClosingParenthesis(searchable, innerOpen, innerClose)) {
			error = "DX11 GLSL relational conversion is malformed.";
			return false;
		}
		const size_t outerClose = searchable.find_first_not_of(" \t\r\n", innerClose + 1);
		if (outerClose == std::string::npos || searchable[outerClose] != ')') {
			error = "DX11 GLSL relational conversion is malformed.";
			return false;
		}
		replacements.push_back({ { position, outerClose + 1 }, "GlslGreaterThan"
				+ source.substr(innerOpen, innerClose - innerOpen + 1) });
	}
	for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
		source.replace(it->first.first, it->first.second - it->first.first, it->second);
	return true;
}

bool lowerTextureCalls(std::string& source, std::string& error)
{
	static const std::regex textureCall(R"(\btexture\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
	const std::string searchable = stripPreprocessorLines(stripComments(source));
	struct Replacement {
		size_t position;
		size_t length;
		std::string text;
	};
	std::vector<Replacement> replacements;
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), textureCall), end; it != end; ++it) {
		const size_t position = static_cast<size_t>(it->position());
		const size_t open = searchable.find('(', position);
		size_t close;
		if (open == std::string::npos || !findClosingParenthesis(searchable, open, close)) {
			error = "DX11 GLSL texture call is malformed.";
			return false;
		}
		const size_t argumentCount = argumentRanges(searchable, open + 1, close).size();
		if (argumentCount != 2 && argumentCount != 3) {
			error = "DX11 GLSL texture calls require coordinates and an optional bias.";
			return false;
		}
		replacements.push_back({ position, static_cast<size_t>(it->length()), (*it)[1].str()
				+ (argumentCount == 3 ? ".SampleBias(SourceSampler," : ".Sample(SourceSampler,") });
	}
	for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
		source.replace(it->position, it->length, it->text);
	return true;
}

bool lowerSquareMatrixConstructors(std::string& source, std::string& error)
{
	static const std::regex constructor(R"(\bmat([34])\s*\()");
	const std::string searchable = stripPreprocessorLines(stripComments(source));
	struct Replacement {
		size_t position;
		size_t length;
		std::string text;
	};
	std::vector<Replacement> replacements;
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), constructor), end; it != end; ++it) {
		const size_t position = static_cast<size_t>(it->position());
		const size_t open = searchable.find('(', position);
		size_t close;
		if (open == std::string::npos || !findClosingParenthesis(searchable, open, close)) {
			error = "DX11 GLSL matrix constructor is malformed.";
			return false;
		}
		const size_t arguments = argumentRanges(searchable, open + 1, close).size();
		const std::string dimension = (*it)[1].str();
		replacements.push_back({ position, open - position + 1,
				arguments == 1 ? "GlslMat" + dimension + "(" : "float" + dimension + "x" + dimension + "(" });
	}
	for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
		source.replace(it->position, it->length, it->text);
	return true;
}

bool lowerArrayConstructors(std::string& source, std::string& error)
{
	static const std::regex constructor(
			R"(\b(float|int|uint|bool|[iu]?vec[234])\s+[A-Za-z_][A-Za-z0-9_]*\s*\[\s*[1-9][0-9]*\s*\]\s*=\s*\1\s*\[\s*\]\s*\()");
	const std::string searchable = stripPreprocessorLines(stripComments(source));
	struct Replacement {
		size_t begin;
		size_t open;
		size_t close;
	};
	std::vector<Replacement> replacements;
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), constructor), end; it != end; ++it) {
		const size_t matchBegin = static_cast<size_t>(it->position());
		const size_t equal = searchable.find('=', matchBegin);
		const size_t valueBegin = searchable.find_first_not_of(" \t\r\n", equal + 1);
		const size_t open = searchable.find('(', valueBegin);
		size_t close;
		if (equal == std::string::npos || valueBegin == std::string::npos || open == std::string::npos
				|| !findClosingParenthesis(searchable, open, close)) {
			error = "DX11 GLSL array constructor is malformed.";
			return false;
		}
		replacements.push_back({ valueBegin, open, close });
	}
	for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
		source.replace(it->close, 1, "}");
		source.replace(it->begin, it->open - it->begin + 1, "{");
	}
	return true;
}

void removePortableVertexInterfaces(std::string& source)
{
	static const std::regex interfaceDeclaration(
			R"(^\s*(?:layout\s*\([^)]*\)\s*)?(?:flat\s+)?(?:in|out|attribute|varying)\s+(?:(?:lowp|mediump|highp)\s+)?(?:vec[234]|float[234])\s+(?:VertexCoord|Position|TexCoord|vTexCoord|in_pos|in_uv|aPosition|aTexCoord)\s*;\s*$)");
	std::istringstream lines(source);
	std::string cleaned;
	for (std::string line; std::getline(lines, line);)
		if (!std::regex_match(line, interfaceDeclaration))
			cleaned += line + '\n';
	source = std::move(cleaned);
}

struct Varying {
	std::string type;
	std::string name;
};

std::vector<Varying> extractVaryings(std::string& source, const char *direction)
{
	const std::regex declaration("^\\s*(?:layout\\s*\\([^)]*\\)\\s*)?(?:flat\\s+)?" + std::string(direction)
			+ R"(\s+(?:(?:lowp|mediump|highp)\s+)?(float|vec[234])\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
	std::vector<Varying> varyings;
	std::string cleaned;
	std::istringstream lines(source);
	for (std::string line; std::getline(lines, line);) {
		std::smatch match;
		if (std::regex_match(line, match, declaration))
			varyings.push_back({ match[1].str(), match[2].str() });
		else
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
	return varyings;
}

std::string hlslVaryingType(const std::string& type)
{
	return type == "float" ? "float" : "float" + type.substr(3);
}

bool findMainBody(const std::string& source, size_t& bodyStart, size_t& bodyEnd)
{
	// The first call sees GLSL's `void main()`, while the second runs after the
	// entry point has been lowered to HLSL with typed input arguments.
	const std::regex mainPattern(R"(\b[A-Za-z_][A-Za-z0-9_]*\s+main\s*\([^)]*\))");
	std::smatch match;
	if (!std::regex_search(source, match, mainPattern))
		return false;
	bodyStart = source.find('{', static_cast<size_t>(match.position() + match.length()));
	if (bodyStart == std::string::npos)
		return false;

	int depth = 0;
	bool lineComment = false;
	bool blockComment = false;
	bool stringLiteral = false;
	for (size_t index = bodyStart; index < source.size(); ++index) {
		const char current = source[index];
		const char next = index + 1 < source.size() ? source[index + 1] : '\0';
		if (lineComment) {
			lineComment = current != '\n';
			continue;
		}
		if (blockComment) {
			if (current == '*' && next == '/') {
				blockComment = false;
				++index;
			}
			continue;
		}
		if (!stringLiteral && current == '/' && next == '/') {
			lineComment = true;
			++index;
			continue;
		}
		if (!stringLiteral && current == '/' && next == '*') {
			blockComment = true;
			++index;
			continue;
		}
		if (current == '"' && (index == 0 || source[index - 1] != '\\')) {
			stringLiteral = !stringLiteral;
			continue;
		}
		if (stringLiteral)
			continue;
		if (current == '{')
			++depth;
		else if (current == '}' && --depth == 0) {
			bodyEnd = index;
			return true;
		}
	}
	return false;
}

uint32_t parameterComponents(SlangParameterType type)
{
	switch (type) {
	case SlangParameterType::Float2: return 2;
	case SlangParameterType::Float3: return 3;
	case SlangParameterType::Float4: return 4;
	case SlangParameterType::Float2x2: return 4;
	case SlangParameterType::Float3x3: return 9;
	case SlangParameterType::Float4x4: return 16;
	default: return 1;
	}
}

std::string hlslParameterValue(const SlangParameter& parameter, uint32_t offset)
{
	static constexpr char Components[] = { 'x', 'y', 'z', 'w' };
	const auto component = [&](uint32_t index) {
		return "Parameters[" + std::to_string(index / 4) + "]." + Components[index % 4];
	};
	const std::string value = component(offset);
	const auto matrix = [&](const char *type, uint32_t components) {
		std::string result = std::string(type) + "(";
		for (uint32_t index = 0; index < components; ++index) {
			if (index != 0)
				result += ", ";
			result += component(offset + index);
		}
		return result + ")";
	};
	switch (parameter.type) {
	case SlangParameterType::Float: return value;
	case SlangParameterType::Int: return "int(" + value + ")";
	case SlangParameterType::Bool: return "(" + value + " != 0.0)";
	case SlangParameterType::Float2:
		return "float2(" + value + ", " + component(offset + 1) + ")";
	case SlangParameterType::Float3:
		return "float3(" + value + ", " + component(offset + 1) + ", " + component(offset + 2) + ")";
	case SlangParameterType::Float4:
		return "float4(" + value + ", " + component(offset + 1) + ", " + component(offset + 2)
				+ ", " + component(offset + 3) + ")";
	case SlangParameterType::Float2x2: return matrix("float2x2", 4);
	case SlangParameterType::Float3x3: return matrix("float3x3", 9);
	case SlangParameterType::Float4x4: return matrix("float4x4", 16);
	}
	return value;
}

const char *hlslParameterType(SlangParameterType type)
{
	switch (type) {
	case SlangParameterType::Int: return "int";
	case SlangParameterType::Bool: return "bool";
	case SlangParameterType::Float2: return "float2";
	case SlangParameterType::Float3: return "float3";
	case SlangParameterType::Float4: return "float4";
	case SlangParameterType::Float2x2: return "float2x2";
	case SlangParameterType::Float3x3: return "float3x3";
	case SlangParameterType::Float4x4: return "float4x4";
	default: return "float";
	}
}

void removeResourceDeclarations(std::string& source, const std::vector<SlangParameter>& parameters)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:sampler|texture|image)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)", std::regex::icase);
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);) {
		const bool resourceDeclaration = std::regex_match(line, resource);
		const bool parameterDeclaration = std::any_of(parameters.begin(), parameters.end(), [&](const SlangParameter& parameter) {
			const std::regex pattern("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:float|int|bool|vec[2-4]|mat[2-4])\\s+"
					+ parameter.name + R"(\s*;\s*$)", std::regex::icase);
			return std::regex_match(line, pattern);
		});
		if (!resourceDeclaration && !parameterDeclaration)
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
}

struct SamplerFunction {
	std::string name;
	size_t argumentCount = 0;
	std::vector<size_t> samplerArguments;
	std::vector<std::string> samplerNames;
};

std::vector<std::pair<size_t, size_t>> argumentRanges(const std::string& source, size_t begin, size_t end)
{
	std::vector<std::pair<size_t, size_t>> ranges;
	size_t argumentStart = begin;
	int parentheses = 0;
	int brackets = 0;
	for (size_t index = begin; index < end; ++index) {
		if (source[index] == '(')
			++parentheses;
		else if (source[index] == ')')
			--parentheses;
		else if (source[index] == '[')
			++brackets;
		else if (source[index] == ']')
			--brackets;
		else if (source[index] == ',' && parentheses == 0 && brackets == 0) {
			ranges.emplace_back(argumentStart, index);
			argumentStart = index + 1;
		}
	}
	if (argumentStart != end || !ranges.empty())
		ranges.emplace_back(argumentStart, end);
	return ranges;
}

std::string trim(const std::string& value)
{
	const size_t begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

bool findClosingParenthesis(const std::string& source, size_t open, size_t& close)
{
	int depth = 1;
	for (size_t index = open + 1; index < source.size(); ++index) {
		if (source[index] == '(')
			++depth;
		else if (source[index] == ')' && --depth == 0) {
			close = index;
			return true;
		}
	}
	return false;
}

bool findClosingBrace(const std::string& source, size_t open, size_t& close)
{
	int depth = 1;
	for (size_t index = open + 1; index < source.size(); ++index) {
		if (source[index] == '{')
			++depth;
		else if (source[index] == '}' && --depth == 0) {
			close = index;
			return true;
		}
	}
	return false;
}

void addDeterministicFunctionFallbacks(std::string& source)
{
	// Reaching the end of a non-void GLSL function is undefined, while the
	// Shader Model 4 compiler rejects it outright. A zero fallback preserves
	// valid paths and makes out-of-range preset values deterministic.
	static const std::regex definition(
			R"(\b(float|int|bool|[iu]?vec[234]|mat(?:[234]|[234]x[234]))\s+[A-Za-z_][A-Za-z0-9_]*\s*\([^;{}]*\)\s*\{)");
	const std::string searchable = stripComments(source);
	std::vector<std::pair<size_t, std::string>> insertions;
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), definition), end; it != end; ++it) {
		const size_t open = searchable.find('{', static_cast<size_t>(it->position()));
		size_t close;
		if (open == std::string::npos || !findClosingBrace(searchable, open, close))
			continue;
		const std::string type = (*it)[1].str();
		const std::string value = type == "bool" ? "false" : type == "int" ? "0"
				: type == "float" ? "0.0" : type + "(0.0)";
		insertions.emplace_back(close, "\n\treturn " + value + ";\n");
	}
	for (auto it = insertions.rbegin(); it != insertions.rend(); ++it)
		source.insert(it->first, it->second);
}

std::vector<SamplerFunction> findSamplerFunctions(const std::string& source)
{
	// Combined RetroArch GLSL commonly passes a sampler through several helper
	// functions. Resource objects cannot be function parameters in ps_4_0, so
	// retain the argument positions for the graph-slot lowering below.
	static const std::regex definition(R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*\(([^{}()]*)\)\s*\{)");
	static const std::regex samplerParameter(R"(\bsampler2D\s+([A-Za-z_][A-Za-z0-9_]*)\b)");
	std::vector<SamplerFunction> functions;
	for (std::sregex_iterator it(source.begin(), source.end(), definition), end; it != end; ++it) {
		const std::string arguments = (*it)[2].str();
		const auto ranges = argumentRanges(arguments, 0, arguments.size());
		SamplerFunction function;
		function.name = (*it)[1].str();
		function.argumentCount = ranges.size();
		for (size_t index = 0; index < ranges.size(); ++index) {
			std::smatch match;
			const std::string argument = arguments.substr(ranges[index].first,
					ranges[index].second - ranges[index].first);
			if (std::regex_search(argument, match, samplerParameter)) {
				function.samplerArguments.push_back(index);
				function.samplerNames.push_back(match[1].str());
			}
		}
		if (!function.samplerArguments.empty())
			functions.push_back(std::move(function));
	}
	return functions;
}

bool lowerSamplerFunctionParameters(std::string& source, const std::vector<ShaderPassInput>& inputs,
		bool& needsSamplerHelper, std::string& error)
{
	const std::vector<SamplerFunction> functions = findSamplerFunctions(source);
	needsSamplerHelper = !functions.empty();
	if (functions.empty())
		return true;
	std::vector<std::string> samplerNames;
	for (const SamplerFunction& function : functions)
		for (const std::string& name : function.samplerNames)
			if (std::find(samplerNames.begin(), samplerNames.end(), name) == samplerNames.end())
				samplerNames.push_back(name);

	for (const SamplerFunction& function : functions) {
		for (const std::string& name : function.samplerNames)
			replaceAll(source, std::regex("\\btexture\\s*\\(\\s*" + name + "\\s*,"),
					"SampleGraphTexture(" + name + ",");

		const std::regex call("\\b" + function.name + R"(\s*\()");
		size_t searchFrom = 0;
		while (searchFrom < source.size()) {
			std::smatch match;
			const auto searchBegin = source.cbegin() + static_cast<std::ptrdiff_t>(searchFrom);
			if (!std::regex_search(searchBegin, source.cend(), match, call))
				break;
			const size_t open = searchFrom + static_cast<size_t>(match.position() + match.length()) - 1;
			size_t close;
			if (!findClosingParenthesis(source, open, close)) {
				error = "DX11 GLSL sampler helper call is malformed.";
				return false;
			}
			const auto ranges = argumentRanges(source, open + 1, close);
			if (ranges.size() == function.argumentCount) {
				std::vector<std::pair<std::pair<size_t, size_t>, std::string>> replacements;
				bool definition = false;
				for (size_t argumentIndex : function.samplerArguments) {
					const auto& range = ranges[argumentIndex];
					const std::string argument = trim(source.substr(range.first, range.second - range.first));
					if (argument.find("sampler2D") != std::string::npos) {
						definition = true;
						break;
					}
					const auto input = std::find_if(inputs.begin(), inputs.end(), [&](const ShaderPassInput& value) {
						return value.bindingName == argument;
					});
					if (input != inputs.end()) {
						replacements.push_back({ range, std::to_string(std::distance(inputs.begin(), input)) });
						continue;
					}
					if (std::find(samplerNames.begin(), samplerNames.end(), argument) == samplerNames.end()) {
						error = "DX11 GLSL sampler helper '" + function.name
								+ "' receives unsupported resource expression '" + argument + "'.";
						return false;
					}
				}
				if (!definition) {
					std::ptrdiff_t delta = 0;
					for (const auto& replacement : replacements)
						delta += static_cast<std::ptrdiff_t>(replacement.second.size())
								- static_cast<std::ptrdiff_t>(replacement.first.second - replacement.first.first);
					for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
						source.replace(it->first.first, it->first.second - it->first.first, it->second);
					close = static_cast<size_t>(static_cast<std::ptrdiff_t>(close) + delta);
				}
			}
			searchFrom = close + 1;
		}
	}
	replaceAll(source, std::regex(R"(\bsampler2D\b)"), "int");
	return true;
}

std::string graphResourceDeclarations(const std::vector<ShaderPassInput>& inputs, bool samplerHelper)
{
	std::string declarations;
	for (size_t slot = 1; slot < inputs.size(); ++slot)
		declarations += "Texture2D GraphTexture" + std::to_string(slot) + " : register(t" + std::to_string(slot) + ");\n"
				+ "SamplerState GraphSampler" + std::to_string(slot) + " : register(s" + std::to_string(slot) + ");\n";
	if (samplerHelper) {
		declarations += "float4 SampleGraphTexture(int slot, float2 coordinates)\n{\n";
		for (size_t slot = 1; slot < inputs.size(); ++slot)
			declarations += "\tif (slot == " + std::to_string(slot) + ") return GraphTexture"
					+ std::to_string(slot) + ".Sample(GraphSampler" + std::to_string(slot) + ", coordinates);\n";
		declarations += "\treturn Source.Sample(SourceSampler, coordinates);\n}\n"
				"float4 SampleGraphTexture(int slot, float2 coordinates, float bias)\n{\n";
		for (size_t slot = 1; slot < inputs.size(); ++slot)
			declarations += "\tif (slot == " + std::to_string(slot) + ") return GraphTexture"
					+ std::to_string(slot) + ".SampleBias(GraphSampler" + std::to_string(slot)
					+ ", coordinates, bias);\n";
		declarations += "\treturn Source.SampleBias(SourceSampler, coordinates, bias);\n}\n";
	}
	return declarations;
}

bool prepareGraphSource(std::string& glsl, const std::vector<SlangParameter>& parameters,
		const std::vector<ShaderPassInput>& inputs, std::vector<std::string>& sampledBindings,
		bool& needsSamplerHelper, std::string& error)
{
	if (inputs.empty() || inputs.front().bindingName != "Source") {
		error = "DX11 shader graph must bind Source to texture slot zero.";
		return false;
	}
	removeResourceDeclarations(glsl, parameters);
	if (!lowerSamplerFunctionParameters(glsl, inputs, needsSamplerHelper, error))
		return false;

	const std::regex textureCall(R"(\btexture\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,)");
	const std::string searchable = stripPreprocessorLines(stripComments(glsl));
	std::vector<std::pair<size_t, size_t>> replacements;
	for (std::sregex_iterator it(searchable.begin(), searchable.end(), textureCall), end; it != end; ++it) {
		const std::string name = (*it)[1].str();
		if (std::none_of(inputs.begin(), inputs.end(), [&](const ShaderPassInput& input) {
				return input.bindingName == name;
		})) {
			error = "DX11 shader samples undeclared resource '" + name + "'.";
			return false;
		}
		sampledBindings.push_back(name);
		replacements.emplace_back(static_cast<size_t>(it->position()), static_cast<size_t>(it->length()));
	}
	for (auto it = replacements.rbegin(); it != replacements.rend(); ++it)
		glsl.replace(it->first, it->second, "texture(Source,");
	return true;
}

bool finishGraphTranslation(std::string& hlsl, const std::vector<std::string>& sampledBindings,
		const std::vector<ShaderPassInput>& inputs, bool needsSamplerHelper, std::string& error)
{
	const std::string sourceTexture = "Source.Sample";
	size_t searchFrom = 0;
	for (const std::string& binding : sampledBindings) {
		const size_t position = hlsl.find(sourceTexture, searchFrom);
		if (position == std::string::npos) {
			error = "DX11 shader bridge could not map a graph texture sample.";
			return false;
		}
		const size_t samplerPosition = hlsl.find("SourceSampler", position + sourceTexture.size());
		if (samplerPosition == std::string::npos) {
			error = "DX11 shader bridge emitted a malformed texture sample.";
			return false;
		}
		const auto input = std::find_if(inputs.begin(), inputs.end(), [&](const ShaderPassInput& value) {
			return value.bindingName == binding;
		});
		const size_t slot = static_cast<size_t>(std::distance(inputs.begin(), input));
		if (slot != 0) {
			hlsl.replace(samplerPosition, std::string("SourceSampler").size(), "GraphSampler" + std::to_string(slot));
			hlsl.replace(position, std::string("Source").size(), "GraphTexture" + std::to_string(slot));
		}
		searchFrom = position + sourceTexture.size();
	}

	const std::string marker = "SamplerState SourceSampler : register(s0);\n";
	const size_t markerPosition = hlsl.find(marker);
	if (markerPosition == std::string::npos) {
		error = "DX11 shader bridge did not emit its resource preamble.";
		return false;
	}
	hlsl.insert(markerPosition + marker.size(), graphResourceDeclarations(inputs, needsSamplerHelper));
	return true;
}

} // namespace

bool translateGlslFragmentToHlsl(const std::string& glslSource, std::string& hlslSource, std::string& error)
{
	static const std::vector<ShaderParameter> noParameters;
	return translateGlslFragmentToHlsl(glslSource, noParameters, hlslSource, error);
}

bool translateGlslFragmentToHlsl(const std::string& glslSource, const std::vector<ShaderParameter>& parameters,
		std::string& hlslSource, std::string& error)
{
	std::vector<SlangParameter> typedParameters;
	typedParameters.reserve(parameters.size());
	for (const ShaderParameter& parameter : parameters) {
		SlangParameter typed;
		typed.name = parameter.name;
		typed.type = SlangParameterType::Float;
		typedParameters.push_back(std::move(typed));
	}
	return translateGlslFragmentToHlsl(glslSource, typedParameters, hlslSource, error);
}

bool translateGlslFragmentToHlsl(const std::string& glslSource, const std::vector<SlangParameter>& parameters,
		std::string& hlslSource, std::string& error, uint32_t outputCount)
{
	if (outputCount == 0 || outputCount > 8) {
		error = "DX11 shader chains support between one and eight color outputs.";
		return false;
	}
	uint32_t parameterOffset = 0;
	for (const SlangParameter& parameter : parameters) {
		parameterOffset += parameterComponents(parameter.type);
		if (parameterOffset > MaxShaderParameterComponents) {
			error = "DX11 shader chains may use at most 24 parameter components per pass.";
			return false;
		}
	}
	std::string source = glslSource;
	const std::vector<Varying> varyings = extractVaryings(source, "in");
	const size_t firstLineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") == 0)
		source.erase(0, firstLineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", firstLineEnd));

	const std::string validationSource = stripComments(source);
	if (contains(validationSource, R"(\b(layout|uniform|attribute|varying)\b)")) {
		error = "DX11 GLSL shaders must use HollyCast's supplied Source and frame variables without declaring interface resources.";
		return false;
	}
	if (contains(validationSource, R"(#\s*(extension|version))") || contains(validationSource, R"(\b(texelFetch|textureLod|textureGrad)\b)")) {
		error = "DX11 GLSL shaders use an unsupported language feature.";
		return false;
	}

	if (!lowerTextureCalls(source, error))
		return false;

	size_t bodyStart;
	size_t bodyEnd;
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 GLSL shaders require exactly one void main() entry point.";
		return false;
	}

	lowerGlslMatrixMultiplication(source);
	lowerGlslVectorEquality(source);
	if (!lowerGlslRelationalConversions(source, error))
		return false;
	addDeterministicFunctionFallbacks(source);
	replaceAll(source, std::regex(R"(\bvoid\s+main\s*\(\s*\))"),
			outputCount == 1 ? "float4 main(PixelInput input) : SV_TARGET" : "PixelOutput main(PixelInput input)");
	if (!lowerArrayConstructors(source, error) || !lowerSquareMatrixConstructors(source, error))
		return false;
	replaceAll(source, std::regex(R"(\bvec2\b)"), "float2");
	replaceAll(source, std::regex(R"(\bvec3\b)"), "float3");
	replaceAll(source, std::regex(R"(\bvec4\b)"), "float4");
	replaceAll(source, std::regex(R"(\bivec2\b)"), "int2");
	replaceAll(source, std::regex(R"(\bivec3\b)"), "int3");
	replaceAll(source, std::regex(R"(\bivec4\b)"), "int4");
	replaceAll(source, std::regex(R"(\buvec2\b)"), "uint2");
	replaceAll(source, std::regex(R"(\buvec3\b)"), "uint3");
	replaceAll(source, std::regex(R"(\buvec4\b)"), "uint4");
	// Preserve GLSL column indexing: mat4x3 has four indexable vec3 columns.
	replaceAll(source, std::regex(R"(\bmat4x3\b)"), "float4x3");
	replaceAll(source, std::regex(R"(\bmat2\b)"), "float2x2");
	replaceAll(source, std::regex(R"(\bmat3\b)"), "float3x3");
	replaceAll(source, std::regex(R"(\bmat4\b)"), "float4x4");
	replaceAll(source, std::regex(R"(\bsampler2D\b)"), "Texture2D");
	source = normalizeHlslVectorConstructors(source);
	replaceAll(source, std::regex(R"(\b(mediump|highp|lowp)\b\s*)"), "");
	replaceAll(source, std::regex(R"(\bgl_FragColor\b)"), outputCount == 1 ? "FragColor" : "FragColor0");
	for (uint32_t output = 0; output < outputCount; ++output)
		replaceAll(source, std::regex("\\bFxOutput" + std::to_string(output) + R"(\b)"),
				outputCount == 1 ? "FragColor" : "FragColor" + std::to_string(output));
	replaceAll(source, std::regex(R"(\bfract\b)"), "frac");
	replaceAll(source, std::regex(R"(\binversesqrt\b)"), "rsqrt");
	replaceAll(source, std::regex(R"(\bdFdx\b)"), "ddx");
	replaceAll(source, std::regex(R"(\bdFdy\b)"), "ddy");
	replaceAll(source, std::regex(R"(\bmix\b)"), "lerp");
	replaceAll(source, std::regex(R"(\bmod\b)"), "GlslMod");

	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 GLSL shader main() is malformed.";
		return false;
	}
	std::string mainPreamble = "\n\tfloat2 vTexCoord = input.vTexCoord;\n";
	for (const Varying& varying : varyings)
		mainPreamble += "\t" + varying.name + " = input." + varying.name + ";\n";
	if (outputCount == 1)
		mainPreamble += "\tfloat4 FragColor = float4(0.0, 0.0, 0.0, 1.0);\n";
	else {
		mainPreamble += "\tPixelOutput output;\n";
		for (uint32_t output = 0; output < outputCount; ++output)
			mainPreamble += "\tfloat4 FragColor" + std::to_string(output) + " = float4(0.0, 0.0, 0.0, 1.0);\n";
	}
	source.insert(bodyStart + 1, mainPreamble);
	bodyEnd += mainPreamble.size();
	std::string mainBody = source.substr(bodyStart + 1, bodyEnd - bodyStart - 1);
	replaceAll(mainBody, std::regex(R"(\breturn\s*;)"), outputCount == 1 ? "return FragColor;" : "return output;");
	source.replace(bodyStart + 1, bodyEnd - bodyStart - 1, mainBody);
	bodyEnd = bodyStart + 1 + mainBody.size();
	if (outputCount == 1)
		source.insert(bodyEnd, "\n\treturn FragColor;\n");
	else {
		std::string returns;
		for (uint32_t output = 0; output < outputCount; ++output)
			returns += "\n\toutput.color" + std::to_string(output) + " = FragColor" + std::to_string(output) + ";";
		returns += "\n\treturn output;\n";
		source.insert(bodyEnd, returns);
	}

	std::string parameterGlobals;
	parameterOffset = 0;
	for (const SlangParameter& parameter : parameters) {
		// A real global preserves GLSL shadowing rules. A preprocessor alias would
		// corrupt helper parameters such as `float range` in upstream shaders.
		parameterGlobals += "static " + std::string(hlslParameterType(parameter.type)) + " " + parameter.name
				+ " = " + hlslParameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += parameterComponents(parameter.type);
	}
	std::string outputDeclaration;
	if (outputCount > 1) {
		outputDeclaration = "struct PixelOutput\n{\n";
		for (uint32_t output = 0; output < outputCount; ++output)
			outputDeclaration += "\tfloat4 color" + std::to_string(output) + " : SV_TARGET" + std::to_string(output) + ";\n";
		outputDeclaration += "};\n\n";
	}
	std::string inputPreamble = HlslPreamble;
	const std::string pixelInputMarker = "\tfloat2 vTexCoord : TEXCOORD0;\n";
	const size_t pixelInputPosition = inputPreamble.find(pixelInputMarker);
	for (size_t index = 0; index < varyings.size(); ++index)
		inputPreamble.insert(pixelInputPosition + pixelInputMarker.size(), "\t" + hlslVaryingType(varyings[index].type)
				+ " " + varyings[index].name + " : TEXCOORD" + std::to_string(index + 1) + ";\n");
	std::string varyingGlobals;
	for (const Varying& varying : varyings)
		varyingGlobals += "static " + hlslVaryingType(varying.type) + " " + varying.name + ";\n";
	hlslSource = inputPreamble + outputDeclaration + parameterGlobals + varyingGlobals + source;
	error.clear();
	return true;
}

bool translateGlslVertexToHlsl(const std::string& glslSource, const std::vector<SlangParameter>& parameters,
		std::string& hlslSource, std::string& error)
{
	std::string source = glslSource;
	removePortableVertexInterfaces(source);
	const std::vector<Varying> varyings = extractVaryings(source, "out");
	replaceAll(source, std::regex(R"(\b(?:Position|in_pos|aPosition)\b)"), "VertexCoord");
	replaceAll(source, std::regex(R"(\b(?:in_uv|aTexCoord)\b)"), "TexCoord");
	const size_t firstLineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") == 0)
		source.erase(0, firstLineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", firstLineEnd));
	if (contains(stripComments(source), R"(\b(layout|attribute|varying)\b)")) {
		error = "DX11 custom Slang vertex stages must use HollyCast's VertexCoord and TexCoord interface.";
		return false;
	}
	if (!lowerTextureCalls(source, error))
		return false;
	size_t bodyStart;
	size_t bodyEnd;
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 custom Slang vertex stages require exactly one void main() entry point.";
		return false;
	}
	lowerGlslMatrixMultiplication(source);
	lowerGlslVectorEquality(source);
	if (!lowerGlslRelationalConversions(source, error))
		return false;
	addDeterministicFunctionFallbacks(source);
	replaceAll(source, std::regex(R"(\bvoid\s+main\s*\(\s*\))"), "VertexOutput main(VertexInput input)");
	if (!lowerArrayConstructors(source, error) || !lowerSquareMatrixConstructors(source, error))
		return false;
	replaceAll(source, std::regex(R"(\bvec2\b)"), "float2");
	replaceAll(source, std::regex(R"(\bvec3\b)"), "float3");
	replaceAll(source, std::regex(R"(\bvec4\b)"), "float4");
	replaceAll(source, std::regex(R"(\bivec2\b)"), "int2");
	replaceAll(source, std::regex(R"(\bivec3\b)"), "int3");
	replaceAll(source, std::regex(R"(\bivec4\b)"), "int4");
	replaceAll(source, std::regex(R"(\buvec2\b)"), "uint2");
	replaceAll(source, std::regex(R"(\buvec3\b)"), "uint3");
	replaceAll(source, std::regex(R"(\buvec4\b)"), "uint4");
	replaceAll(source, std::regex(R"(\bmat4x3\b)"), "float4x3");
	replaceAll(source, std::regex(R"(\bmat2\b)"), "float2x2");
	replaceAll(source, std::regex(R"(\bmat3\b)"), "float3x3");
	replaceAll(source, std::regex(R"(\bmat4\b)"), "float4x4");
	replaceAll(source, std::regex(R"(\bsampler2D\b)"), "Texture2D");
	source = normalizeHlslVectorConstructors(source);
	replaceAll(source, std::regex(R"(\b(mediump|highp|lowp)\b\s*)"), "");
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 custom Slang vertex stage is malformed.";
		return false;
	}
	std::string mainPreamble = "\n\tfloat4 VertexCoord = float4(input.position, 1.0, 1.0);\n"
			"\tfloat2 TexCoord = input.texCoord;\n\tfloat4 gl_Position = VertexCoord;\n\tfloat2 vTexCoord = TexCoord;\n"
			"\tVertexOutput output;\n\toutput.position = gl_Position;\n\toutput.vTexCoord = vTexCoord;\n";
	for (const Varying& varying : varyings)
		mainPreamble += "\t" + hlslVaryingType(varying.type) + " " + varying.name + " = ("
				+ hlslVaryingType(varying.type) + ")0;\n";
	source.insert(bodyStart + 1, mainPreamble);
	bodyEnd += mainPreamble.size();
	std::string mainBody = source.substr(bodyStart + 1, bodyEnd - bodyStart - 1);
	replaceAll(mainBody, std::regex(R"(\breturn\s*;)"), "return output;");
	source.replace(bodyStart + 1, bodyEnd - bodyStart - 1, mainBody);
	bodyEnd = bodyStart + 1 + mainBody.size();
	std::string outputAssignments = "\n\toutput.position = gl_Position;\n\toutput.vTexCoord = vTexCoord;\n";
	for (const Varying& varying : varyings)
		outputAssignments += "\toutput." + varying.name + " = " + varying.name + ";\n";
	outputAssignments += "\treturn output;\n";
	source.insert(bodyEnd, outputAssignments);

	uint32_t parameterOffset = 0;
	std::string parameterGlobals;
	for (const SlangParameter& parameter : parameters) {
		parameterOffset += parameterComponents(parameter.type);
		if (parameterOffset > MaxShaderParameterComponents) {
			error = "DX11 shader chains may use at most 24 parameter components per pass.";
			return false;
		}
		parameterGlobals += "static " + std::string(hlslParameterType(parameter.type)) + " " + parameter.name
				+ " = " + hlslParameterValue(parameter,
						parameterOffset - parameterComponents(parameter.type)) + ";\n";
	}
	std::string vertexPreamble = HlslVertexPreamble;
	const std::string vertexOutputMarker = "\tfloat2 vTexCoord : TEXCOORD0;\n";
	const size_t vertexOutputPosition = vertexPreamble.find(vertexOutputMarker);
	for (size_t index = 0; index < varyings.size(); ++index)
		vertexPreamble.insert(vertexOutputPosition + vertexOutputMarker.size(), "\t" + hlslVaryingType(varyings[index].type)
				+ " " + varyings[index].name + " : TEXCOORD" + std::to_string(index + 1) + ";\n");
	hlslSource = vertexPreamble + parameterGlobals + source;
	error.clear();
	return true;
}

bool translateGlslGraphFragmentToHlsl(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		uint32_t outputCount, std::string& hlslSource, std::string& error)
{
	std::string glsl = source.fragment;
	std::vector<std::string> sampledBindings;
	bool needsSamplerHelper = false;
	if (!prepareGraphSource(glsl, parameters, inputs, sampledBindings, needsSamplerHelper, error)
			|| !translateGlslFragmentToHlsl(glsl, parameters, hlslSource, error, outputCount))
		return false;
	return finishGraphTranslation(hlslSource, sampledBindings, inputs, needsSamplerHelper, error);
}

bool translateGlslGraphVertexToHlsl(const std::string& vertexSource,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& inputs,
		std::string& hlslSource, std::string& error)
{
	std::string glsl = vertexSource;
	std::vector<std::string> sampledBindings;
	bool needsSamplerHelper = false;
	if (!prepareGraphSource(glsl, parameters, inputs, sampledBindings, needsSamplerHelper, error)
			|| !translateGlslVertexToHlsl(glsl, parameters, hlslSource, error))
		return false;
	return finishGraphTranslation(hlslSource, sampledBindings, inputs, needsSamplerHelper, error);
}

} // namespace postprocess
