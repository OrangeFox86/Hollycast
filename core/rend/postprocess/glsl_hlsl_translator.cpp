/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "shader_chain.h"

#include <cctype>
#include <regex>
#include <sstream>

namespace postprocess {
namespace {

constexpr char HlslPreamble[] = R"(
Texture2D Source : register(t0);
SamplerState SourceSampler : register(s0);

cbuffer ShaderFrame : register(b0)
{
	float2 SourceSize;
	float2 OutputSize;
	int FrameCount;
	float3 ShaderFramePadding;
	// HLSL scalar arrays consume one 16-byte register per element. A float4
	// array keeps the CPU's contiguous 16-float upload layout backend-neutral.
	float4 Parameters[4];
};

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
	float3 ShaderFramePadding;
	// Keep this layout identical to ShaderFrameUniforms and the pixel stage.
	float4 Parameters[4];
};

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
		if (parameterOffset > 16) {
			error = "DX11 shader chains may use at most 16 parameter components per pass.";
			return false;
		}
	}
	std::string source = glslSource;
	const size_t firstLineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") == 0)
		source.erase(0, firstLineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", firstLineEnd));

	const std::string validationSource = stripComments(source);
	if (contains(validationSource, R"(\b(layout|uniform|attribute|varying|sampler[[:alnum:]_]*)\b)")) {
		error = "DX11 GLSL shaders must use HollyCast's supplied Source and frame variables without declaring interface resources.";
		return false;
	}
	if (contains(validationSource, R"(#\s*(extension|version))") || contains(validationSource, R"(\b(texelFetch|textureLod|textureGrad)\b)")) {
		error = "DX11 GLSL shaders use an unsupported language feature.";
		return false;
	}

	const std::regex textureCall(R"(\btexture\s*\(\s*([[:alpha:]_][[:alnum:]_]*)\s*,)");
	for (std::sregex_iterator it(validationSource.begin(), validationSource.end(), textureCall), end; it != end; ++it) {
		if ((*it)[1].str() != "Source") {
			error = "DX11 GLSL shaders may only sample the supplied Source texture.";
			return false;
		}
	}
	if (contains(source, R"(\btexture\s*\()"))
		replaceAll(source, textureCall, "Source.Sample(SourceSampler,");

	size_t bodyStart;
	size_t bodyEnd;
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 GLSL shaders require exactly one void main() entry point.";
		return false;
	}

	replaceAll(source, std::regex(R"(\bvoid\s+main\s*\(\s*\))"),
			outputCount == 1 ? "float4 main(PixelInput input) : SV_TARGET" : "PixelOutput main(PixelInput input)");
	replaceAll(source, std::regex(R"(\bvec2\b)"), "float2");
	replaceAll(source, std::regex(R"(\bvec3\b)"), "float3");
	replaceAll(source, std::regex(R"(\bvec4\b)"), "float4");
	replaceAll(source, std::regex(R"(\bmat2\b)"), "float2x2");
	replaceAll(source, std::regex(R"(\bmat3\b)"), "float3x3");
	replaceAll(source, std::regex(R"(\bmat4\b)"), "float4x4");
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
	replaceAll(source, std::regex(R"(\bmod\b)"), "fmod");

	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 GLSL shader main() is malformed.";
		return false;
	}
	std::string mainPreamble = "\n\tfloat2 vTexCoord = input.vTexCoord;\n";
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

	std::string parameterDefines;
	parameterOffset = 0;
	for (const SlangParameter& parameter : parameters) {
		parameterDefines += "#define " + parameter.name + " " + hlslParameterValue(parameter, parameterOffset) + "\n";
		parameterOffset += parameterComponents(parameter.type);
	}
	std::string outputDeclaration;
	if (outputCount > 1) {
		outputDeclaration = "struct PixelOutput\n{\n";
		for (uint32_t output = 0; output < outputCount; ++output)
			outputDeclaration += "\tfloat4 color" + std::to_string(output) + " : SV_TARGET" + std::to_string(output) + ";\n";
		outputDeclaration += "};\n\n";
	}
	hlslSource = std::string(HlslPreamble) + outputDeclaration + parameterDefines + source;
	error.clear();
	return true;
}

bool translateGlslVertexToHlsl(const std::string& glslSource, const std::vector<SlangParameter>& parameters,
		std::string& hlslSource, std::string& error)
{
	std::string source = glslSource;
	removePortableVertexInterfaces(source);
	replaceAll(source, std::regex(R"(\b(?:Position|in_pos|aPosition)\b)"), "VertexCoord");
	replaceAll(source, std::regex(R"(\b(?:in_uv|aTexCoord)\b)"), "TexCoord");
	const size_t firstLineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") == 0)
		source.erase(0, firstLineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", firstLineEnd));
	if (contains(stripComments(source), R"(\b(layout|attribute|varying|sampler[[:alnum:]_]*)\b)")) {
		error = "DX11 custom Slang vertex stages must use HollyCast's VertexCoord and TexCoord interface.";
		return false;
	}
	const std::regex textureCall(R"(\btexture\s*\(\s*Source\s*,)");
	if (contains(source, R"(\btexture\s*\()"))
		replaceAll(source, textureCall, "Source.Sample(SourceSampler,");
	size_t bodyStart;
	size_t bodyEnd;
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 custom Slang vertex stages require exactly one void main() entry point.";
		return false;
	}
	replaceAll(source, std::regex(R"(\bvoid\s+main\s*\(\s*\))"), "VertexOutput main(VertexInput input)");
	replaceAll(source, std::regex(R"(\bvec2\b)"), "float2");
	replaceAll(source, std::regex(R"(\bvec3\b)"), "float3");
	replaceAll(source, std::regex(R"(\bvec4\b)"), "float4");
	replaceAll(source, std::regex(R"(\bmat2\b)"), "float2x2");
	replaceAll(source, std::regex(R"(\bmat3\b)"), "float3x3");
	replaceAll(source, std::regex(R"(\bmat4\b)"), "float4x4");
	replaceAll(source, std::regex(R"(\b(mediump|highp|lowp)\b\s*)"), "");
	if (!findMainBody(source, bodyStart, bodyEnd)) {
		error = "DX11 custom Slang vertex stage is malformed.";
		return false;
	}
	const std::string mainPreamble = "\n\tfloat4 VertexCoord = float4(input.position, 1.0, 1.0);\n"
			"\tfloat2 TexCoord = input.texCoord;\n\tfloat4 gl_Position = VertexCoord;\n\tfloat2 vTexCoord = TexCoord;\n"
			"\tVertexOutput output;\n\toutput.position = gl_Position;\n\toutput.vTexCoord = vTexCoord;\n";
	source.insert(bodyStart + 1, mainPreamble);
	bodyEnd += mainPreamble.size();
	std::string mainBody = source.substr(bodyStart + 1, bodyEnd - bodyStart - 1);
	replaceAll(mainBody, std::regex(R"(\breturn\s*;)"), "return output;");
	source.replace(bodyStart + 1, bodyEnd - bodyStart - 1, mainBody);
	bodyEnd = bodyStart + 1 + mainBody.size();
	source.insert(bodyEnd, "\n\toutput.position = gl_Position;\n\toutput.vTexCoord = vTexCoord;\n\treturn output;\n");

	uint32_t parameterOffset = 0;
	std::string parameterDefines;
	for (const SlangParameter& parameter : parameters) {
		parameterOffset += parameterComponents(parameter.type);
		if (parameterOffset > 16) {
			error = "DX11 shader chains may use at most 16 parameter components per pass.";
			return false;
		}
		parameterDefines += "#define " + parameter.name + " " + hlslParameterValue(parameter,
				parameterOffset - parameterComponents(parameter.type)) + "\n";
	}
	hlslSource = std::string(HlslVertexPreamble) + parameterDefines + source;
	error.clear();
	return true;
}

} // namespace postprocess
