/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "slang_frontend.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <regex>
#include <sstream>
#include <string_view>

namespace postprocess {
namespace {

constexpr size_t MaxShaderTextSize = 2 * 1024 * 1024;

bool fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

std::string trim(std::string value)
{
	const size_t first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t\r\n");
	return value.substr(first, last - first + 1);
}

bool startsWith(const std::string& value, std::string_view prefix)
{
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& value, std::string_view suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isIdentifier(const std::string& value)
{
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
		return false;
	return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

std::string stripComments(const std::string& line, bool& inBlockComment)
{
	std::string result;
	bool quoted = false;
	bool escaped = false;
	for (size_t index = 0; index < line.size(); ++index) {
		if (inBlockComment) {
			if (line[index] == '*' && index + 1 < line.size() && line[index + 1] == '/') {
				inBlockComment = false;
				++index;
			}
			continue;
		}
		if (!quoted && line[index] == '/' && index + 1 < line.size()) {
			if (line[index + 1] == '/')
				break;
			if (line[index + 1] == '*') {
				inBlockComment = true;
				++index;
				continue;
			}
		}
		result += line[index];
		if (line[index] == '"' && !escaped)
			quoted = !quoted;
		escaped = line[index] == '\\' && !escaped;
		if (line[index] != '\\')
			escaped = false;
	}
	return result;
}

bool tokenize(const std::string& text, std::vector<std::string>& tokens, std::string& error)
{
	for (size_t index = 0; index < text.size();) {
		while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])))
			++index;
		if (index == text.size())
			break;
		if (text[index] == '"') {
			std::string token;
			bool escaped = false;
			++index;
			for (; index < text.size(); ++index) {
				if (text[index] == '"' && !escaped)
					break;
				token += text[index];
				escaped = text[index] == '\\' && !escaped;
				if (text[index] != '\\')
					escaped = false;
			}
			if (index == text.size())
				return fail(error, "Unterminated quoted Slang pragma value.");
			tokens.push_back(std::move(token));
			++index;
			continue;
		}
		const size_t begin = index;
		int parentheses = 0;
		for (; index < text.size(); ++index) {
			if (text[index] == '(')
				++parentheses;
			else if (text[index] == ')') {
				if (parentheses == 0)
					return fail(error, "Unbalanced parentheses in Slang pragma value.");
				--parentheses;
			}
			else if (parentheses == 0 && std::isspace(static_cast<unsigned char>(text[index])))
				break;
		}
		if (parentheses != 0)
			return fail(error, "Unbalanced parentheses in Slang pragma value.");
		tokens.push_back(text.substr(begin, index - begin));
	}
	return true;
}

bool parseParameterType(const std::string& token, SlangParameterType& type)
{
	if (token == "float") type = SlangParameterType::Float;
	else if (token == "int") type = SlangParameterType::Int;
	else if (token == "bool") type = SlangParameterType::Bool;
	else if (token == "float2" || token == "vec2") type = SlangParameterType::Float2;
	else if (token == "float3" || token == "vec3") type = SlangParameterType::Float3;
	else if (token == "float4" || token == "vec4") type = SlangParameterType::Float4;
	else if (token == "float2x2" || token == "mat2") type = SlangParameterType::Float2x2;
	else if (token == "float3x3" || token == "mat3") type = SlangParameterType::Float3x3;
	else if (token == "float4x4" || token == "mat4") type = SlangParameterType::Float4x4;
	else return false;
	return true;
}

uint32_t componentCount(SlangParameterType type)
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

bool parseScalar(const std::string& text, SlangParameterType type, float& value)
{
	if (type == SlangParameterType::Bool) {
		if (text == "true" || text == "1") { value = 1.f; return true; }
		if (text == "false" || text == "0") { value = 0.f; return true; }
		return false;
	}
	char *end = nullptr;
	const float parsed = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed))
		return false;
	if (type == SlangParameterType::Int && std::floor(parsed) != parsed)
		return false;
	value = parsed;
	return true;
}

bool parseParameterValue(std::string text, SlangParameterType type, SlangParameterValue& value)
{
	value.componentCount = componentCount(type);
	const size_t opening = text.find('(');
	if (opening != std::string::npos) {
		if (text.back() != ')')
			return false;
		text = text.substr(opening + 1, text.size() - opening - 2);
	}
	std::stringstream stream(text);
	std::string component;
	uint32_t index = 0;
	while (std::getline(stream, component, ',')) {
		if (index >= value.componentCount || !parseScalar(trim(component), type, value.values[index]))
			return false;
		++index;
	}
	return index == value.componentCount;
}

bool parseParameter(const std::string& pragma, SlangParameter& parameter, std::string& error)
{
	std::vector<std::string> tokens;
	if (!tokenize(pragma, tokens, error))
		return false;
	if (tokens.size() < 7 || tokens[0] != "#pragma" || tokens[1] != "parameter")
		return fail(error, "Malformed Slang #pragma parameter declaration.");
	parameter = {};
	size_t index = 2;
	SlangParameterType type;
	if (parseParameterType(tokens[index], type)) {
		parameter.type = type;
		++index;
	}
	if (tokens.size() != index + 6 || !isIdentifier(tokens[index]))
		return fail(error, "Slang #pragma parameter needs a name, label, default, minimum, maximum, and step.");
	parameter.name = tokens[index++];
	parameter.label = tokens[index++];
	if (parameter.label.empty()
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.defaultValue)
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.minimum)
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.maximum)
			|| !parseParameterValue(tokens[index++], parameter.type, parameter.step))
		return fail(error, "Slang #pragma parameter has an invalid typed value.");
	return true;
}

bool parseResourceType(const std::string& text, SlangResourceType& type)
{
	if (text == "sampler1D") type = SlangResourceType::Sampler1D;
	else if (text == "sampler2D") type = SlangResourceType::Sampler2D;
	else if (text == "sampler2DArray") type = SlangResourceType::Sampler2DArray;
	else if (text == "sampler3D") type = SlangResourceType::Sampler3D;
	else if (text == "samplerCube") type = SlangResourceType::SamplerCube;
	else if (text == "texture1D") type = SlangResourceType::Texture1D;
	else if (text == "texture2D") type = SlangResourceType::Texture2D;
	else if (text == "texture2DArray") type = SlangResourceType::Texture2DArray;
	else if (text == "texture3D") type = SlangResourceType::Texture3D;
	else if (text == "textureCube") type = SlangResourceType::TextureCube;
	else if (text == "image1D") type = SlangResourceType::Image1D;
	else if (text == "image2D") type = SlangResourceType::Image2D;
	else if (text == "image2DArray") type = SlangResourceType::Image2DArray;
	else if (text == "image3D") type = SlangResourceType::Image3D;
	else return false;
	return true;
}

void classifyResource(SlangResource& resource)
{
	// Slang commonly uses a Texture suffix for the GLSL binding while the
	// preset alias names the underlying render target. Keep the exact binding
	// name above, but resolve graph conventions from the logical name.
	std::string logicalName = resource.name;
	if (endsWith(logicalName, "Texture"))
		logicalName.resize(logicalName.size() - std::string_view("Texture").size());
	else if (endsWith(logicalName, "Sampler"))
		logicalName.resize(logicalName.size() - std::string_view("Sampler").size());
	if (logicalName == "Source") {
		resource.convention = SlangResourceConvention::Source;
		return;
	}
	if (logicalName == "Original") {
		resource.convention = SlangResourceConvention::Original;
		return;
	}
	if (endsWith(logicalName, "Feedback")) {
		resource.convention = SlangResourceConvention::Feedback;
		resource.alias = logicalName.substr(0, logicalName.size() - std::string_view("Feedback").size());
		return;
	}
	std::smatch indexedFeedback;
	if (std::regex_match(logicalName, indexedFeedback, std::regex(R"(^PassFeedback([0-9]+)$)"))) {
		resource.convention = SlangResourceConvention::Feedback;
		resource.alias = logicalName;
		return;
	}
	std::smatch historyMatch;
	if (std::regex_match(logicalName, historyMatch, std::regex(R"(^(.+)History([0-9]+)?$)"))) {
		resource.convention = SlangResourceConvention::History;
		resource.alias = historyMatch[1].str();
		if (historyMatch[2].matched) {
			const unsigned long offset = std::strtoul(historyMatch[2].str().c_str(), nullptr, 10);
			resource.historyOffset = static_cast<uint32_t>(std::min<unsigned long>(offset, UINT32_MAX));
		}
		return;
	}
	resource.convention = SlangResourceConvention::PassAlias;
	resource.alias = std::move(logicalName);
}

bool parseResource(const std::string& line, SlangResource& resource)
{
	static const std::regex declaration(R"(^\s*(?:layout\s*\(([^)]*)\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?((?:sampler|texture|image)(?:2DArray|1D|2D|3D|Cube))\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\[\s*([1-9][0-9]*)\s*\])?\s*;\s*$)");
	std::smatch match;
	if (!std::regex_match(line, match, declaration) || !parseResourceType(match[2].str(), resource.type))
		return false;
	resource.name = match[3].str();
	if (match[4].matched) {
		const std::string countText = match[4].str();
		char *end = nullptr;
		const unsigned long count = std::strtoul(countText.c_str(), &end, 10);
		if (end == nullptr || *end != '\0' || count > std::numeric_limits<uint32_t>::max())
			return false;
		resource.arrayCount = static_cast<uint32_t>(count);
	}
	else {
		resource.arrayCount = 1;
	}
	if (match[1].matched) {
		std::smatch bindingMatch;
		const std::string layout = match[1].str();
		try {
			if (std::regex_search(layout, bindingMatch, std::regex(R"(\bset\s*=\s*([0-9]+))", std::regex::icase))) {
				resource.bindingSet = static_cast<uint32_t>(std::stoul(bindingMatch[1].str()));
				resource.hasExplicitBinding = true;
			}
			if (std::regex_search(layout, bindingMatch, std::regex(R"(\bbinding\s*=\s*([0-9]+))", std::regex::icase))) {
				resource.binding = static_cast<uint32_t>(std::stoul(bindingMatch[1].str()));
				resource.hasExplicitBinding = true;
			}
		} catch (...) {
			return false;
		}
	}
	classifyResource(resource);
	return true;
}

bool looksLikeResourceDeclaration(const std::string& line)
{
	static const std::regex resourceStart(R"(^\s*(?:layout\s*\([^)]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:sampler|texture|image))");
	return std::regex_search(line, resourceStart);
}

} // namespace

bool parseSlangFrontend(const std::string& shaderText, SlangShaderIR& shader, std::string& error)
{
	shader = {};
	error.clear();
	if (shaderText.empty() || shaderText.size() > MaxShaderTextSize || shaderText.find('\0') != std::string::npos)
		return fail(error, "Slang source is empty, too large, or contains NUL bytes.");

	SlangShaderIR parsed;
	std::string commonSource;
	std::string vertexSource;
	std::string fragmentSource;
	bool inBlockComment = false;
	bool versionSeen = false;
	bool stageSeen = false;
	bool vertexSeen = false;
	bool fragmentSeen = false;
	enum class Stage { None, Vertex, Fragment } stage = Stage::None;

	std::istringstream stream(shaderText);
	std::string rawLine;
	while (std::getline(stream, rawLine)) {
		const std::string line = trim(stripComments(rawLine, inBlockComment));
		if (line.empty())
			continue;
		if (startsWith(line, "#version")) {
			if (versionSeen || stageSeen || !commonSource.empty() || !vertexSource.empty() || !fragmentSource.empty())
				return fail(error, "Slang #version must appear once before declarations and stages.");
			versionSeen = true;
			commonSource += rawLine + '\n';
			continue;
		}
		if (!versionSeen)
			return fail(error, "Slang source must start with #version.");
		if (startsWith(line, "#pragma parameter")) {
			SlangParameter parameter;
			if (!parseParameter(line, parameter, error))
				return false;
			if (std::any_of(parsed.parameters.begin(), parsed.parameters.end(), [&](const SlangParameter& value) { return value.name == parameter.name; }))
				return fail(error, "Duplicate Slang parameter '" + parameter.name + "'.");
			parsed.parameters.push_back(std::move(parameter));
			continue;
		}
		if (startsWith(line, "#pragma stage")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error))
				return false;
			if (tokens.size() != 3 || tokens[0] != "#pragma" || tokens[1] != "stage")
				return fail(error, "Malformed Slang #pragma stage declaration.");
			stageSeen = true;
			if (tokens[2] == "vertex" && !vertexSeen) {
				stage = Stage::Vertex;
				vertexSeen = true;
			}
			else if (tokens[2] == "fragment" && !fragmentSeen) {
				stage = Stage::Fragment;
				fragmentSeen = true;
			}
			else
				return fail(error, "Slang stages must be one vertex and one fragment block.");
			continue;
		}
		if (startsWith(line, "#pragma name")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error) || tokens.size() != 3 || !parsed.displayName.empty())
				return fail(error, "Malformed or duplicate Slang #pragma name declaration.");
			parsed.displayName = tokens[2];
			continue;
		}
		if (startsWith(line, "#pragma format")) {
			std::vector<std::string> tokens;
			if (!tokenize(line, tokens, error) || tokens.size() != 3 || !parsed.format.empty())
				return fail(error, "Malformed or duplicate Slang #pragma format declaration.");
			parsed.format = tokens[2];
			continue;
		}

		SlangResource resource;
		if (parseResource(line, resource)) {
			if (std::any_of(parsed.resources.begin(), parsed.resources.end(), [&](const SlangResource& value) { return value.name == resource.name; }))
				return fail(error, "Duplicate Slang resource '" + resource.name + "'.");
			parsed.resources.push_back(std::move(resource));
		}
		else if (looksLikeResourceDeclaration(line)) {
			return fail(error, "Malformed or unsupported Slang resource declaration.");
		}

		std::string *destination = stage == Stage::Vertex ? &vertexSource : stage == Stage::Fragment ? &fragmentSource : &commonSource;
		*destination += rawLine + '\n';
	}
	if (inBlockComment)
		return fail(error, "Unterminated block comment in Slang source.");
	if (!versionSeen)
		return fail(error, "Slang source must start with #version.");
	if (!stageSeen) {
		parsed.fragment = { commonSource, true };
	}
	else if (!fragmentSeen || fragmentSource.empty())
		return fail(error, "Slang source must declare a non-empty fragment stage.");
	else {
		parsed.vertex = { vertexSeen ? commonSource + vertexSource : std::string(), vertexSeen };
		parsed.fragment = { commonSource + fragmentSource, true };
	}
	shader = std::move(parsed);
	return true;
}

} // namespace postprocess
