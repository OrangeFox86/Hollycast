/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "shader_chain.h"
#include "reshade_fx.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace postprocess {
namespace {

std::string lowercase(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

void replaceAll(std::string& value, const std::regex& pattern, const std::string& replacement)
{
	value = std::regex_replace(value, pattern, replacement);
}

void lowerModuloOperators(std::string& source)
{
	struct Replacement {
		size_t begin;
		size_t end;
		std::string value;
	};
	std::vector<Replacement> replacements;
	bool lineComment = false;
	bool blockComment = false;
	bool stringLiteral = false;
	for (size_t position = 0; position < source.size(); ++position) {
		const char current = source[position];
		const char next = position + 1 < source.size() ? source[position + 1] : '\0';
		if (lineComment) {
			lineComment = current != '\n';
			continue;
		}
		if (blockComment) {
			if (current == '*' && next == '/') {
				blockComment = false;
				++position;
			}
			continue;
		}
		if (!stringLiteral && current == '/' && next == '/') {
			lineComment = true;
			++position;
			continue;
		}
		if (!stringLiteral && current == '/' && next == '*') {
			blockComment = true;
			++position;
			continue;
		}
		if (current == '"' && (position == 0 || source[position - 1] != '\\')) {
			stringLiteral = !stringLiteral;
			continue;
		}
		if (stringLiteral || current != '%' || position == 0 || source[position - 1] == '=' || next == '=')
			continue;

		size_t leftEnd = source.find_last_not_of(" \t\r\n", position - 1);
		size_t rightBegin = source.find_first_not_of(" \t\r\n", position + 1);
		if (leftEnd == std::string::npos || rightBegin == std::string::npos)
			continue;
		size_t leftBegin = leftEnd;
		if (source[leftEnd] == ')') {
			int depth = 1;
			while (leftBegin > 0 && depth != 0) {
				--leftBegin;
				if (source[leftBegin] == ')') ++depth;
				else if (source[leftBegin] == '(') --depth;
			}
			if (depth != 0)
				continue;
		}
		else {
			while (leftBegin > 0 && (std::isalnum(static_cast<unsigned char>(source[leftBegin - 1]))
					|| source[leftBegin - 1] == '_' || source[leftBegin - 1] == '.'))
				--leftBegin;
		}

		size_t rightEnd = rightBegin;
		if (source[rightBegin] == '(') {
			int depth = 1;
			while (++rightEnd < source.size() && depth != 0) {
				if (source[rightEnd] == '(') ++depth;
				else if (source[rightEnd] == ')') --depth;
			}
			if (depth != 0)
				continue;
		}
		else {
			while (rightEnd + 1 < source.size() && (std::isalnum(static_cast<unsigned char>(source[rightEnd + 1]))
					|| source[rightEnd + 1] == '_' || source[rightEnd + 1] == '.'))
				++rightEnd;
		}
		replacements.push_back({ leftBegin, rightEnd + 1,
				"FxMod(" + source.substr(leftBegin, leftEnd - leftBegin + 1) + ", "
						+ source.substr(rightBegin, rightEnd - rightBegin + 1) + ")" });
	}
	for (auto replacement = replacements.rbegin(); replacement != replacements.rend(); ++replacement)
		source.replace(replacement->begin, replacement->end - replacement->begin, replacement->value);
}

std::string glslType(const std::string& type)
{
	const std::string lower = lowercase(type);
	if (lower == "float2") return "vec2";
	if (lower == "float3") return "vec3";
	if (lower == "float4") return "vec4";
	if (lower == "float2x2") return "mat2";
	if (lower == "float3x3") return "mat3";
	if (lower == "float4x4") return "mat4";
	if (lower == "uint2") return "uvec2";
	if (lower == "uint3") return "uvec3";
	if (lower == "uint4") return "uvec4";
	if (lower == "int2") return "ivec2";
	if (lower == "int3") return "ivec3";
	if (lower == "int4") return "ivec4";
	return type;
}

void lowerLanguage(std::string& source)
{
	lowerModuloOperators(source);
	// GLSL has no namespaces. The FX parser keeps namespace members in one
	// validated effect scope, so qualified references can be lowered directly.
	replaceAll(source, std::regex(R"(\b(?:[A-Za-z_][A-Za-z0-9_]*\s*::\s*)+)"), "");
	// ReShade effects commonly use these legal HLSL identifiers for locals,
	// but GLSL reserves them. Rename whole tokens before backend compilation.
	replaceAll(source, std::regex(R"(\binput\b)"), "fxInput");
	replaceAll(source, std::regex(R"(\boutput\b)"), "fxOutput");
	replaceAll(source, std::regex(R"(\bmain\b)"), "fxMain");
	// `point` is accepted as an ordinary identifier by FX compilers but is a
	// reserved geometry-shader token when the generated GLSL is lowered to HLSL.
	replaceAll(source, std::regex(R"(\bpoint\b)"), "fxPoint");
	replaceAll(source, std::regex(R"(\bstatic\s+const\b)"), "const");
	replaceAll(source, std::regex(R"(\bfloat2x2\b)"), "mat2");
	replaceAll(source, std::regex(R"(\bfloat3x3\b)"), "mat3");
	replaceAll(source, std::regex(R"(\bfloat4x4\b)"), "mat4");
	replaceAll(source, std::regex(R"(\bfloat4x3\b)"), "mat4x3");
	replaceAll(source, std::regex(R"(\bfloat2\b)"), "vec2");
	replaceAll(source, std::regex(R"(\bfloat3\b)"), "vec3");
	replaceAll(source, std::regex(R"(\bfloat4\b)"), "vec4");
	replaceAll(source, std::regex(R"(\buint2\b)"), "uvec2");
	replaceAll(source, std::regex(R"(\buint3\b)"), "uvec3");
	replaceAll(source, std::regex(R"(\buint4\b)"), "uvec4");
	replaceAll(source, std::regex(R"(\bint2\b)"), "ivec2");
	replaceAll(source, std::regex(R"(\bint3\b)"), "ivec3");
	replaceAll(source, std::regex(R"(\bint4\b)"), "ivec4");
	replaceAll(source, std::regex(R"(\bsampler\b)"), "sampler2D");
	// HLSL splats scalar initializers across vectors, while GLSL requires an
	// explicit constructor. This covers the common single-expression form.
	replaceAll(source, std::regex(R"(\b(vec[234]|ivec[234]|uvec[234])\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^,;{}]+);)"),
			"$1 $2 = $1($3);");
	replaceAll(source, std::regex(R"(\blerp\s*\()"), "FxMix(");
	replaceAll(source, std::regex(R"(\bpow\s*\()"), "FxPow(");
	replaceAll(source, std::regex(R"(\bfrac\s*\()"), "fract(");
	replaceAll(source, std::regex(R"(\brsqrt\s*\()"), "inversesqrt(");
	replaceAll(source, std::regex(R"(\btex2D\s*\()"), "texture(");
	replaceAll(source, std::regex(R"(\btex2Dgrad\s*\()"), "textureGrad(");
	replaceAll(source, std::regex(R"(\btex2Dlod\s*\()"), "textureLod(");
	replaceAll(source, std::regex(R"(\bdot\s*\()"), "FxDot(");
	// Semantics have already been reflected into wrapper inputs and outputs.
	replaceAll(source, std::regex(R"(\s*:\s*(?:SV_[A-Za-z0-9_]+|POSITION[0-9]*|TEXCOORD[0-9]*|COLOR[0-9]*))",
			std::regex::icase), "");
}

SlangParameterType parameterType(ReShadeValueType type)
{
	switch (type) {
	case ReShadeValueType::Bool: return SlangParameterType::Bool;
	case ReShadeValueType::Int:
	case ReShadeValueType::UInt: return SlangParameterType::Int;
	case ReShadeValueType::Float2: return SlangParameterType::Float2;
	case ReShadeValueType::Float3: return SlangParameterType::Float3;
	case ReShadeValueType::Float4: return SlangParameterType::Float4;
	case ReShadeValueType::Float2x2: return SlangParameterType::Float2x2;
	case ReShadeValueType::Float3x3: return SlangParameterType::Float3x3;
	case ReShadeValueType::Float4x4: return SlangParameterType::Float4x4;
	default: return SlangParameterType::Float;
	}
}

uint32_t componentCount(SlangParameterType type)
{
	switch (type) {
	case SlangParameterType::Float2: return 2;
	case SlangParameterType::Float3: return 3;
	case SlangParameterType::Float4:
	case SlangParameterType::Float2x2: return 4;
	case SlangParameterType::Float3x3: return 9;
	case SlangParameterType::Float4x4: return 16;
	default: return 1;
	}
}

bool matchesConstructor(const std::string& name, SlangParameterType type)
{
	const std::string value = lowercase(name);
	switch (type) {
	case SlangParameterType::Float: return value == "float";
	case SlangParameterType::Int: return value == "int" || value == "uint";
	case SlangParameterType::Bool: return value == "bool";
	case SlangParameterType::Float2: return value == "float2" || value == "vec2";
	case SlangParameterType::Float3: return value == "float3" || value == "vec3";
	case SlangParameterType::Float4: return value == "float4" || value == "vec4";
	case SlangParameterType::Float2x2: return value == "float2x2" || value == "mat2";
	case SlangParameterType::Float3x3: return value == "float3x3" || value == "mat3";
	case SlangParameterType::Float4x4: return value == "float4x4" || value == "mat4";
	}
	return false;
}

bool parseValues(const std::string& expression, SlangParameterType type, SlangParameterValue& result)
{
	result = {};
	result.componentCount = componentCount(type);
	if (expression.empty())
		return true;
	auto trim = [](std::string_view value) {
		const size_t first = value.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos)
			return std::string();
		return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
	};
	std::string values = trim(expression);
	const std::string lower = lowercase(values);
	if (type == SlangParameterType::Bool && (lower == "true" || lower == "false")) {
		result.values[0] = lower == "true" ? 1.f : 0.f;
		return true;
	}
	const size_t open = values.find('(');
	if (open != std::string::npos) {
		if (values.back() != ')' || values.find('(', open + 1) != std::string::npos
				|| !matchesConstructor(trim(std::string_view(values).substr(0, open)), type))
			return false;
		values = values.substr(open + 1, values.size() - open - 2);
	}
	else if (values.find(')') != std::string::npos) {
		return false;
	}
	uint32_t index = 0;
	for (size_t begin = 0;;) {
		const size_t separator = values.find(',', begin);
		const std::string token = trim(std::string_view(values).substr(begin,
				separator == std::string::npos ? std::string::npos : separator - begin));
		if (token.empty() || index >= result.componentCount)
			return false;
		float value;
		if (!parseShaderFloat(token, value)
				|| (type == SlangParameterType::Int && std::floor(value) != value)
				|| (type == SlangParameterType::Bool && value != 0.f && value != 1.f))
			return false;
		result.values[index++] = value;
		if (separator == std::string::npos)
			break;
		begin = separator + 1;
	}
	return index == result.componentCount;
}

std::vector<std::string> splitChoiceLabels(const std::string& values)
{
	std::vector<std::string> labels;
	for (size_t begin = 0; begin < values.size();) {
		const size_t end = values.find('\0', begin);
		const size_t length = (end == std::string::npos ? values.size() : end) - begin;
		if (length != 0)
			labels.emplace_back(values.substr(begin, length));
		if (end == std::string::npos)
			break;
		begin = end + 1;
	}
	return labels;
}

const ReShadeFunction *findFunction(const ReShadeEffect& effect, const std::string& name)
{
	const auto function = std::find_if(effect.functions.begin(), effect.functions.end(), [&](const ReShadeFunction& value) {
		return value.name == name;
	});
	return function == effect.functions.end() ? nullptr : &*function;
}

struct FunctionParameter {
	std::string type;
	std::string name;
	std::string semantic;
	bool output = false;
	bool inout = false;
	uint32_t arrayCount = 0;
};

std::vector<std::string> splitArguments(const std::string& value)
{
	std::vector<std::string> result;
	size_t begin = 0;
	int depth = 0;
	for (size_t index = 0; index <= value.size(); ++index) {
		const char c = index < value.size() ? value[index] : ',';
		if (c == '(' || c == '[' || c == '<') ++depth;
		else if (c == ')' || c == ']' || c == '>') --depth;
		else if (c == ',' && depth == 0) {
			result.push_back(value.substr(begin, index - begin));
			begin = index + 1;
		}
	}
	return result;
}

bool parseFunctionParameters(const ReShadeFunction& function, std::vector<FunctionParameter>& parameters, std::string& error)
{
	const size_t open = function.signature.find('(');
	const size_t close = function.signature.rfind(')');
	if (open == std::string::npos || close == std::string::npos || close < open) {
		error = "ReShade FX function '" + function.name + "' has a malformed signature.";
		return false;
	}
	std::string list = function.signature.substr(open + 1, close - open - 1);
	replaceAll(list, std::regex(R"(//[^\r\n]*)"), "");
	replaceAll(list, std::regex(R"(/\*[\s\S]*?\*/)"), "");
	if (std::regex_match(list, std::regex(R"(\s*(?:void)?\s*)")))
		return true;
	for (std::string argument : splitArguments(list)) {
		const bool inout = std::regex_search(argument, std::regex(R"(\binout\b)"));
		const bool output = inout || std::regex_search(argument, std::regex(R"(\bout\b)"));
		replaceAll(argument, std::regex(R"(\b(?:in|out|inout|const|static)\b)"), " ");
		std::smatch match;
		const std::regex declaration(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*\[\s*([1-9][0-9]*)\s*\])?(?:\s*:\s*([A-Za-z_][A-Za-z0-9_]*))?\s*$)");
		if (!std::regex_match(argument, match, declaration)) {
			error = "ReShade FX function '" + function.name + "' has an unsupported parameter declaration.";
			return false;
		}
		uint32_t arrayCount = 0;
		if (match[3].matched && !parseShaderUnsigned(match[3].str(), arrayCount)) {
			error = "ReShade FX function '" + function.name + "' has an invalid array parameter size.";
			return false;
		}
		parameters.push_back({ match[1].str(), match[2].str(), match[4].str(), output, inout, arrayCount });
	}
	return true;
}

struct StructField {
	std::string type;
	std::string name;
	std::string semantic;
};

struct VertexVarying {
	std::string type;
	std::string semantic;
	std::string name;
	std::string source;
};

std::vector<StructField> structFields(const ReShadeEffect& effect, const std::string& type)
{
	const auto structure = std::find_if(effect.structs.begin(), effect.structs.end(), [&](const ReShadeStruct& value) {
		return value.name == type;
	});
	if (structure == effect.structs.end())
		return {};
	std::vector<StructField> fields;
	const std::regex field(R"(([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]+\])?\s*(?::\s*([A-Za-z_][A-Za-z0-9_]*))?\s*;)");
	for (std::sregex_iterator it(structure->body.begin(), structure->body.end(), field), end; it != end; ++it)
		fields.push_back({ (*it)[1].str(), (*it)[2].str(), (*it)[3].str() });
	return fields;
}

std::string semanticValue(const std::string& type, const std::string& semantic, bool vertex)
{
	const std::string lower = lowercase(semantic);
	if (lower.rfind("texcoord", 0) == 0)
		return vertex ? "TexCoord" : "vTexCoord";
	if (lower.rfind("position", 0) == 0 || lower == "sv_position") {
		if (!vertex)
			return glslType(type) == "vec2" ? "vTexCoord * OutputSize" : "vec4(vTexCoord * OutputSize, 0.0, 1.0)";
		if (glslType(type) == "vec2")
			return "VertexCoord.xy";
		if (glslType(type) == "vec3")
			return "VertexCoord.xyz";
		return "vec4(VertexCoord.xyz, 1.0)";
	}
	if (lower.rfind("color", 0) == 0)
		return glslType(type) + "(1.0)";
	return glslType(type) + "(0.0)";
}

bool buildCall(const ReShadeEffect& effect, const ReShadeFunction& function, bool vertex,
		std::string& setup, std::string& arguments, std::string& error,
		std::vector<FunctionParameter> *reflectedParameters = nullptr,
		const std::unordered_map<std::string, std::string> *varyingInputs = nullptr)
{
	const auto varyingValue = [&](const std::string& semantic) -> const std::string * {
		if (varyingInputs == nullptr)
			return nullptr;
		const auto varying = varyingInputs->find(lowercase(semantic));
		return varying == varyingInputs->end() ? nullptr : &varying->second;
	};
	std::vector<FunctionParameter> parameters;
	if (!parseFunctionParameters(function, parameters, error))
		return false;
	for (size_t parameterIndex = 0; parameterIndex < parameters.size(); ++parameterIndex) {
		FunctionParameter& parameter = parameters[parameterIndex];
		const std::string lowerName = lowercase(parameter.name);
		if (parameter.output && (lowerName == "texcoord" || lowerName == "vertexcoord"
				|| lowerName == "vtexcoord" || lowerName == "gl_position" || lowerName == "fragcolor"))
			parameter.name = "fxOutputParameter" + std::to_string(parameterIndex);
		if (parameter.arrayCount != 0) {
			error = "ReShade FX entry function '" + function.name + "' cannot use array parameters.";
			return false;
		}
		if (!arguments.empty())
			arguments += ", ";
		const std::vector<StructField> fields = structFields(effect, parameter.type);
		if (parameter.output) {
			setup += "\t" + (fields.empty() ? glslType(parameter.type) : parameter.type) + " " + parameter.name;
			if (parameter.inout && fields.empty())
				setup += " = " + semanticValue(parameter.type, parameter.semantic, vertex);
			setup += ";\n";
			arguments += parameter.name;
			continue;
		}
		if (fields.empty()) {
			const std::string *varying = varyingValue(parameter.semantic);
			arguments += varying != nullptr ? *varying : semanticValue(parameter.type, parameter.semantic, vertex);
			continue;
		}
		setup += "\t" + parameter.type + " " + parameter.name + ";\n";
		for (const StructField& field : fields) {
			const std::string *varying = varyingValue(field.semantic);
			setup += "\t" + parameter.name + "." + field.name + " = "
					+ (varying != nullptr ? *varying : semanticValue(field.type, field.semantic, vertex)) + ";\n";
		}
		arguments += parameter.name;
	}
	if (reflectedParameters != nullptr)
		*reflectedParameters = parameters;
	return true;
}

std::string resultField(const ReShadeEffect& effect, const std::string& type, const std::regex& semantic)
{
	for (const StructField& field : structFields(effect, type))
		if (std::regex_match(field.semantic, semantic))
			return field.name;
	return {};
}

uint32_t semanticIndex(const std::string& semantic)
{
	const size_t firstDigit = semantic.find_first_of("0123456789");
	if (firstDigit == std::string::npos)
		return 0;
	uint32_t index;
	return parseShaderUnsigned(semantic.substr(firstDigit), index) ? index : InvalidShaderPass;
}

bool collectVertexVaryings(const ReShadeEffect& effect, const ReShadeFunction& vertex,
		const std::vector<FunctionParameter>& parameters, std::vector<VertexVarying>& varyings,
		std::unordered_map<std::string, std::string>& inputs, std::string& error)
{
	const auto add = [&](const std::string& type, const std::string& semantic,
			const std::string& source) -> bool {
		const std::string key = lowercase(semantic);
		if (key.empty() || key == "sv_position" || key.rfind("position", 0) == 0 || key == "texcoord"
				|| key == "texcoord0")
			return true;
		const std::string varyingType = glslType(type);
		if (varyingType != "float" && varyingType != "vec2" && varyingType != "vec3" && varyingType != "vec4") {
			error = "ReShade FX vertex varying '" + semantic + "' must be a float or float vector.";
			return false;
		}
		const auto existing = inputs.find(key);
		if (existing != inputs.end()) {
			const auto varying = std::find_if(varyings.begin(), varyings.end(), [&](const VertexVarying& value) {
				return value.name == existing->second;
			});
			if (varying == varyings.end() || varying->type != varyingType) {
				error = "ReShade FX vertex varying '" + semantic + "' has conflicting types.";
				return false;
			}
			return true;
		}
		VertexVarying varying { varyingType, key, "FxVarying" + std::to_string(varyings.size()), source };
		inputs.emplace(key, varying.name);
		varyings.push_back(std::move(varying));
		return true;
	};

	if (lowercase(vertex.returnType) != "void") {
		for (const StructField& field : structFields(effect, vertex.returnType))
			if (!add(field.type, field.semantic, "fxVertexResult." + field.name))
				return false;
	}
	for (const FunctionParameter& parameter : parameters) {
		if (!parameter.output)
			continue;
		const std::vector<StructField> fields = structFields(effect, parameter.type);
		if (fields.empty()) {
			if (!add(parameter.type, parameter.semantic, parameter.name))
				return false;
		}
		else {
			for (const StructField& field : fields)
				if (!add(field.type, field.semantic, parameter.name + "." + field.name))
					return false;
		}
	}
	return true;
}

bool outputExpression(const std::string& type, const std::string& value, std::string& expression)
{
	const std::string outputType = glslType(type);
	if (outputType == "vec4") expression = value;
	else if (outputType == "vec3") expression = "vec4(" + value + ", 1.0)";
	else if (outputType == "vec2") expression = "vec4(" + value + ", 0.0, 1.0)";
	else if (outputType == "float") expression = "vec4(vec3(" + value + "), 1.0)";
	else return false;
	return true;
}

std::vector<const ReShadeFunction *> reachableFunctions(const ReShadeEffect& effect,
		std::initializer_list<const ReShadeFunction *> roots)
{
	std::vector<const ReShadeFunction *> reachable;
	auto add = [&](const ReShadeFunction *function) {
		if (function != nullptr && std::find(reachable.begin(), reachable.end(), function) == reachable.end())
			reachable.push_back(function);
	};
	for (const ReShadeFunction *root : roots)
		add(root);
	for (size_t index = 0; index < reachable.size(); ++index) {
		const std::string& body = reachable[index]->body;
		for (const ReShadeFunction& candidate : effect.functions)
			if (std::regex_search(body, std::regex("\\b" + candidate.name + "\\s*\\(")))
				add(&candidate);
	}
	std::vector<const ReShadeFunction *> ordered;
	ordered.reserve(reachable.size());
	for (const ReShadeFunction& function : effect.functions)
		if (std::find(reachable.begin(), reachable.end(), &function) != reachable.end())
			ordered.push_back(&function);
	return ordered;
}

std::string loweredFunctionName(const ReShadeFunction& function)
{
	return lowercase(function.name) == "main" ? "fxMain" : function.name;
}

bool referencesIdentifier(const std::vector<const ReShadeFunction *>& functions, const std::string& name)
{
	const std::regex identifier("\\b" + name + "\\b");
	return std::any_of(functions.begin(), functions.end(), [&](const ReShadeFunction *function) {
		return std::regex_search(function->signature, identifier) || std::regex_search(function->body, identifier);
	});
}

void renameHostInterfaceShadows(std::string& signature, std::string& body)
{
	// Frame metadata is exposed through macros on GL/Vulkan. Rename effect-local
	// variables that legally shadow those names before macro expansion can turn
	// their declarations into malformed member expressions.
	static constexpr std::array<const char *, 5> Names {
		"SourceSize", "OutputSize", "OriginalSize", "FrameCount", "Rotation"
	};
	for (const char *name : Names) {
		const std::regex declaration("\\b(?:bool|int|uint|float|[biu]?vec[234]|mat(?:[234]|[234]x[234]))\\s+"
				+ std::string(name) + "\\b");
		if (!std::regex_search(signature, declaration) && !std::regex_search(body, declaration))
			continue;
		const std::regex identifier("\\b" + std::string(name) + "\\b");
		replaceAll(signature, identifier, "FxLocal" + std::string(name));
		replaceAll(body, identifier, "FxLocal" + std::string(name));
	}
}

std::string emitFunction(const ReShadeFunction& function)
{
	std::string signature = function.signature;
	std::string body = function.body;
	lowerLanguage(signature);
	lowerLanguage(body);
	renameHostInterfaceShadows(signature, body);
	const std::string returnType = glslType(function.returnType);
	if (returnType == "vec2" || returnType == "vec3" || returnType == "vec4"
			|| returnType == "ivec2" || returnType == "ivec3" || returnType == "ivec4"
			|| returnType == "uvec2" || returnType == "uvec3" || returnType == "uvec4")
		replaceAll(body, std::regex(R"(\breturn\s+([^;]+);)"), "return " + returnType + "($1);");
	return signature + "\n{\n" + body + "\n}\n";
}

void lowerBooleanArithmetic(std::string& source, const ReShadeEffect& effect)
{
	for (const ReShadeUniform& uniform : effect.uniforms) {
		if (uniform.valueType != ReShadeValueType::Bool)
			continue;
		const std::string value = "(" + uniform.name + " ? 1.0 : 0.0)";
		replaceAll(source, std::regex("\\b" + uniform.name + R"(\b\s*\*)"), value + " *");
		replaceAll(source, std::regex(R"(\*\s*\b)" + uniform.name + "\\b"), "* " + value);
		replaceAll(source, std::regex(R"(([+\-*/]=)\s*\b)" + uniform.name + "\\b"), "$1 " + value);
	}
	// HLSL implicitly converts a parenthesized boolean expression used in
	// arithmetic to 0/1. GLSL requires that conversion to be explicit.
	replaceAll(source, std::regex(R"((\*\s*)\(([^()]*(?:&&|\|\||==|!=|<=|>=|<|>)[^()]*)\))"),
			"$1(($2) ? 1.0 : 0.0)");
}

std::string samplerTexture(const ReShadeSampler& sampler)
{
	std::smatch match;
	if (std::regex_search(sampler.definition, match,
			std::regex(R"(\bTexture\s*=\s*(?:[A-Za-z_][A-Za-z0-9_]*\s*::\s*)*([A-Za-z_][A-Za-z0-9_]*))",
					std::regex::icase)))
		return match[1].str();
	return sampler.name;
}

bool isSourceTexture(const ReShadeEffect& effect, const std::string& texture, const std::string& sampler)
{
	const std::string lowerTexture = lowercase(texture);
	const std::string lowerSampler = lowercase(sampler);
	if (lowerTexture == "source" || lowerTexture == "sourcetex" || lowerTexture == "backbuffer"
			|| lowerTexture == "backbuffertex" || lowerSampler == "source" || lowerSampler == "sourcesampler"
			|| lowerSampler == "backbuffer")
		return true;
	const auto resource = std::find_if(effect.textures.begin(), effect.textures.end(), [&](const ReShadeTexture& value) {
		return value.name == texture;
	});
	if (resource == effect.textures.end())
		return false;
	return std::any_of(resource->annotations.begin(), resource->annotations.end(), [](const ReShadeAnnotation& annotation) {
		return lowercase(annotation.name) == "source" && lowercase(annotation.value) == "color";
	});
}

size_t renderTargetProducer(const ReShadeEffect& effect, size_t techniqueIndex, const std::string& texture)
{
	if (techniqueIndex >= effect.techniques.size())
		return InvalidShaderPass;
	const std::regex assignment(R"(\bRenderTarget[0-9]*\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)",
			std::regex::icase);
	const auto& passes = effect.techniques[techniqueIndex].passes;
	for (size_t passIndex = 0; passIndex < passes.size(); ++passIndex)
		for (std::sregex_iterator it(passes[passIndex].stateBlock.begin(), passes[passIndex].stateBlock.end(), assignment), end;
				it != end; ++it)
			if ((*it)[1].str() == texture)
				return passIndex;
	return InvalidShaderPass;
}

bool parseUiMaximum(const std::string& expression, SlangParameterType type, SlangParameterValue& result)
{
	if (parseValues(expression, type, result))
		return true;
	std::string compact = expression;
	compact.erase(std::remove_if(compact.begin(), compact.end(),
			[](unsigned char c) { return std::isspace(c); }), compact.end());
	if (compact != "OutputSize.x" && compact != "OutputSize.y")
		return false;
	result = {};
	result.componentCount = componentCount(type);
	std::fill_n(result.values.begin(), result.componentCount, static_cast<float>(MaxShaderTargetDimension));
	return true;
}

bool reShadeHostUniformExpression(const ReShadeUniform& uniform, bool& hostProvided,
		std::string& expression, std::string& error)
{
	const auto annotation = std::find_if(uniform.annotations.begin(), uniform.annotations.end(),
			[](const ReShadeAnnotation& value) { return lowercase(value.name) == "source"; });
	if (annotation == uniform.annotations.end()) {
		hostProvided = false;
		expression.clear();
		return true;
	}
	hostProvided = true;
	const std::string source = lowercase(annotation->value);
	const std::string type = glslType(uniform.typeName);
	const bool scalar = type == "float" || type == "int" || type == "uint" || type == "bool";
	const auto requireScalar = [&]() {
		if (scalar)
			return true;
		error = "ReShade FX host uniform '" + uniform.name + "' requires a scalar type.";
		return false;
	};
	const auto requireVec2 = [&]() {
		if (type == "vec2")
			return true;
		error = "ReShade FX host uniform '" + uniform.name + "' requires a float2 type.";
		return false;
	};
	if (source == "zero")
		expression = type + "(0.0)";
	else if (source == "has_depth")
		expression = type == "bool" ? "false" : type + "(0.0)";
	else if (source == "framecount") {
		if (!requireScalar()) return false;
		expression = type + "(FrameCount)";
	}
	else if (source == "timer") {
		if (!requireScalar()) return false;
		// Shader chains do not own wall-clock timing. Frame-derived time is stable
		// across pauses, deterministic replays, and every graphics backend.
		expression = type + "(float(FrameCount) * (1.0 / 60.0))";
	}
	else if (source == "random") {
		if (!requireScalar()) return false;
		expression = type + "(fract(sin(float(FrameCount) * 12.9898) * 43758.5453) * 32767.0)";
	}
	else if (source == "bufferwidth" || source == "viewportwidth") {
		if (!requireScalar()) return false;
		expression = type + "(OutputSize.x)";
	}
	else if (source == "bufferheight" || source == "viewportheight") {
		if (!requireScalar()) return false;
		expression = type + "(OutputSize.y)";
	}
	else if (source == "internalwidth" || source == "nativewidth") {
		if (!requireScalar()) return false;
		expression = type + "(OriginalSize.x)";
	}
	else if (source == "internalheight" || source == "nativeheight") {
		if (!requireScalar()) return false;
		expression = type + "(OriginalSize.y)";
	}
	else if (source == "upscale_multiplier") {
		if (!requireScalar()) return false;
		expression = type + "(SourceSize.x / max(OriginalSize.x, 1.0))";
	}
	else if (source == "viewportx" || source == "viewporty") {
		if (!requireScalar()) return false;
		expression = type + "(0.0)";
	}
	else if (source == "viewportsize") {
		if (!requireVec2()) return false;
		expression = "OutputSize";
	}
	else if (source == "buffer_to_viewport_ratio") {
		if (!requireVec2()) return false;
		expression = "vec2(1.0)";
	}
	else if (source == "internal_pixel_size" || source == "native_pixel_size") {
		if (!requireVec2()) return false;
		expression = "OutputSize / max(OriginalSize, vec2(1.0))";
	}
	else if (source == "normalized_internal_pixel_size" || source == "normalized_native_pixel_size") {
		if (!requireVec2()) return false;
		expression = "vec2(1.0) / max(OriginalSize, vec2(1.0))";
	}
	else {
		error = "ReShade FX uniform '" + uniform.name + "' uses unsupported host source '" + annotation->value + "'.";
		return false;
	}
	return true;
}

bool fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

} // namespace

bool compileReShadeFxPass(const std::string& effectText, size_t techniqueIndex, size_t passIndex,
		ShaderPassSource& source, std::string& error)
{
	ReShadeEffect effect;
	if (!parseReShadeEffect(effectText, effect, error))
		return false;
	if (techniqueIndex >= effect.techniques.size() || passIndex >= effect.techniques[techniqueIndex].passes.size())
		return fail(error, "ReShade FX selected an invalid technique pass.");
	const ReShadePass& pass = effect.techniques[techniqueIndex].passes[passIndex];
	const ReShadeFunction *pixel = findFunction(effect, pass.pixelShader);
	if (pixel == nullptr)
		return fail(error, "ReShade FX PixelShader entry point '" + pass.pixelShader + "' has no definition.");
	const ReShadeFunction *vertex = pass.vertexShader.empty() ? nullptr : findFunction(effect, pass.vertexShader);
	if (!pass.vertexShader.empty() && vertex == nullptr)
		return fail(error, "ReShade FX VertexShader entry point '" + pass.vertexShader + "' has no definition.");
	const std::vector<const ReShadeFunction *> functions = reachableFunctions(effect, { pixel, vertex });
	std::vector<const ReShadeSampler *> samplers;
	for (const ReShadeSampler& sampler : effect.samplers) {
		if (!referencesIdentifier(functions, sampler.name))
			continue;
		const std::string texture = samplerTexture(sampler);
		if (lowercase(sampler.name) == "depthbuffer" || lowercase(texture) == "depthbuffertex")
			return fail(error, "ReShade FX requires a depth buffer, which is unavailable to display shader chains.");
		samplers.push_back(&sampler);
	}

	ShaderPassSource compiled;
	std::string declarations;
	std::vector<const ReShadeUniform *> reflectedUniforms;
	uint32_t parameterComponents = 0;
	for (const ReShadeUniform& uniform : effect.uniforms) {
		if (uniform.valueType == ReShadeValueType::Unknown)
			return fail(error, "ReShade FX uniform '" + uniform.name + "' has an unsupported type.");
		bool hostProvided;
		std::string hostExpression;
		if (!reShadeHostUniformExpression(uniform, hostProvided, hostExpression, error))
			return false;
		if (hostProvided) {
			const std::string name = lowercase(uniform.name);
			// These names are already supplied by every backend preamble. Re-declaring
			// them as source annotations would create a recursive or conflicting macro.
			if (name != "sourcesize" && name != "outputsize" && name != "originalsize"
					&& name != "framecount" && name != "rotation")
				declarations += "#define " + uniform.name + " (" + hostExpression + ")\n";
			continue;
		}
		SlangParameter parameter;
		parameter.type = parameterType(uniform.valueType);
		parameter.name = uniform.name;
		parameter.label = uniform.name;
		if (!parseValues(uniform.defaultValue, parameter.type, parameter.defaultValue))
			return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid default value.");
		parameter.minimum = parameter.maximum = parameter.defaultValue;
		parameter.step.componentCount = componentCount(parameter.type);
		std::string uiType;
		std::string uiItems;
		for (const ReShadeAnnotation& annotation : uniform.annotations) {
			const std::string name = lowercase(annotation.name);
			if (name == "ui_label") parameter.label = annotation.value;
			else if (name == "ui_type") uiType = lowercase(annotation.value);
			else if (name == "ui_items") uiItems = annotation.value;
			else if (name == "ui_min" && !parseValues(annotation.value, parameter.type, parameter.minimum))
				return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid ui_min value.");
			else if (name == "ui_max" && !parseUiMaximum(annotation.value, parameter.type, parameter.maximum))
				return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid ui_max value.");
			else if (name == "ui_step" && !parseValues(annotation.value, parameter.type, parameter.step))
				return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid ui_step value.");
		}
		if (!validateSlangParameter(parameter, error))
			return false;
		parameterComponents += componentCount(parameter.type);
		if (parameterComponents > MaxShaderParameterComponents)
			return fail(error, "ReShade FX uses more than " + std::to_string(MaxShaderParameterComponents)
					+ " uniform components in one pass.");
		ShaderParameter visible;
		visible.name = parameter.name;
		visible.label = parameter.label;
		visible.value = parameter.defaultValue.values[0];
		visible.minimum = parameter.minimum.values[0];
		visible.maximum = parameter.maximum.values[0];
		visible.step = parameter.step.values[0];
		visible.values = parameter.defaultValue.values;
		visible.minimumValues = parameter.minimum.values;
		visible.maximumValues = parameter.maximum.values;
		visible.stepValues = parameter.step.values;
		visible.componentCount = componentCount(parameter.type);
		visible.type = parameter.type;
		if (visible.componentCount == 1 && parameter.type == SlangParameterType::Int
				&& (uiType == "combo" || uiType == "radio")) {
			visible.choiceLabels = splitChoiceLabels(uiItems);
			if (!visible.choiceLabels.empty()) {
				visible.minimum = visible.minimumValues[0] = 0.f;
				visible.maximum = visible.maximumValues[0] = static_cast<float>(visible.choiceLabels.size() - 1);
				visible.step = visible.stepValues[0] = 1.f;
				if (visible.value < visible.minimum || visible.value > visible.maximum)
					return fail(error, "ReShade FX uniform '" + uniform.name + "' has a choice default outside ui_items.");
			}
		}
		if (parameter.type == SlangParameterType::Bool) {
			visible.minimum = visible.minimumValues[0] = 0.f;
			visible.maximum = visible.maximumValues[0] = 1.f;
			visible.step = visible.stepValues[0] = 1.f;
		}
		compiled.parameters.push_back(std::move(visible));
		compiled.slangParameters.push_back(std::move(parameter));
		reflectedUniforms.push_back(&uniform);
	}

	for (size_t index = 0; index < compiled.slangParameters.size(); ++index)
		declarations += "uniform " + glslType(reflectedUniforms[index]->typeName)
				+ " " + compiled.slangParameters[index].name + ";\n";
	for (const ReShadeSampler *sampler : samplers) {
		const std::string texture = samplerTexture(*sampler);
		SlangResource resource;
		resource.type = SlangResourceType::Sampler2D;
		resource.name = sampler->name;
		resource.alias = texture;
		if (isSourceTexture(effect, texture, sampler->name))
			resource.convention = SlangResourceConvention::Source;
		else {
			const size_t producer = renderTargetProducer(effect, techniqueIndex, texture);
			resource.convention = producer != InvalidShaderPass && producer >= passIndex
					? SlangResourceConvention::Feedback : SlangResourceConvention::PassAlias;
		}
		compiled.slangResources.push_back(std::move(resource));
		declarations += "uniform sampler2D " + sampler->name + ";\n";
	}
	if (samplers.empty())
		declarations += "uniform sampler2D Source;\n";

	std::string common;
	for (const std::string& directive : effect.preprocessorDirectives) {
		if (!std::regex_match(directive, std::regex(R"(\s*#\s*(?:define|undef|if|ifdef|ifndef|elif|else|endif|line|pragma)\b.*)")))
			return fail(error, "ReShade FX uses an unsupported preprocessor directive: " + directive);
		// The clean-room preprocessor has already expanded definitions and selected
		// conditional branches. Only source-location directives remain meaningful.
		if (std::regex_match(directive, std::regex(R"(\s*#\s*line\b.*)")))
			common += directive + "\n";
	}
	common += "#define saturate(value) clamp((value), 0.0, 1.0)\n";
	// HLSL's mul(a, b) uses row/column order opposite GLSL for vector/matrix
	// operands. Typed overloads preserve that behavior, including CRT shaders'
	// common non-square float4x3 color matrices.
	common += "#define mul(left, right) FxMul((left), (right))\n"
			"float FxMul(float left, float right) { return left * right; }\n"
			"vec2 FxMul(vec2 left, float right) { return left * right; }\n"
			"vec3 FxMul(vec3 left, float right) { return left * right; }\n"
			"vec4 FxMul(vec4 left, float right) { return left * right; }\n"
			"vec2 FxMul(float left, vec2 right) { return left * right; }\n"
			"vec3 FxMul(float left, vec3 right) { return left * right; }\n"
			"vec4 FxMul(float left, vec4 right) { return left * right; }\n"
			"vec2 FxMul(vec2 left, mat2 right) { return right * left; }\n"
			"vec3 FxMul(vec3 left, mat3 right) { return right * left; }\n"
			"vec4 FxMul(vec4 left, mat4 right) { return right * left; }\n"
			"vec2 FxMul(mat2 left, vec2 right) { return right * left; }\n"
			"vec3 FxMul(mat3 left, vec3 right) { return right * left; }\n"
			"vec4 FxMul(mat4 left, vec4 right) { return right * left; }\n"
			"mat2 FxMul(mat2 left, mat2 right) { return right * left; }\n"
			"mat3 FxMul(mat3 left, mat3 right) { return right * left; }\n"
			"mat4 FxMul(mat4 left, mat4 right) { return right * left; }\n"
			"vec3 FxMul(vec4 left, mat4x3 right) { return right * left; }\n"
			"vec4 FxMul(mat4x3 left, vec3 right) { return right * left; }\n";
	common += "#define rcp(value) (1.0 / (value))\n";
	common += "float FxMod(float left, float right) { return mod(left, right); }\n"
			"vec2 FxMod(vec2 left, float right) { return mod(left, right); }\n"
			"vec3 FxMod(vec3 left, float right) { return mod(left, right); }\n"
			"vec4 FxMod(vec4 left, float right) { return mod(left, right); }\n"
			"vec2 FxMod(vec2 left, vec2 right) { return mod(left, right); }\n"
			"vec3 FxMod(vec3 left, vec3 right) { return mod(left, right); }\n"
			"vec4 FxMod(vec4 left, vec4 right) { return mod(left, right); }\n"
			"int FxMod(int left, int right) { return left % right; }\n"
			"uint FxMod(uint left, uint right) { return left % right; }\n";
	common += "float FxPow(float base, float exponent) { return pow(base, exponent); }\n"
			"vec2 FxPow(vec2 base, vec2 exponent) { return pow(base, exponent); }\n"
			"vec3 FxPow(vec3 base, vec3 exponent) { return pow(base, exponent); }\n"
			"vec4 FxPow(vec4 base, vec4 exponent) { return pow(base, exponent); }\n"
			"vec2 FxPow(vec2 base, float exponent) { return pow(base, vec2(exponent)); }\n"
			"vec3 FxPow(vec3 base, float exponent) { return pow(base, vec3(exponent)); }\n"
			"vec4 FxPow(vec4 base, float exponent) { return pow(base, vec4(exponent)); }\n"
			"vec2 FxPow(float base, vec2 exponent) { return pow(vec2(base), exponent); }\n"
			"vec3 FxPow(float base, vec3 exponent) { return pow(vec3(base), exponent); }\n"
			"vec4 FxPow(float base, vec4 exponent) { return pow(vec4(base), exponent); }\n"
			"float FxMix(float left, float right, float amount) { return mix(left, right, amount); }\n"
			"vec2 FxMix(vec2 left, vec2 right, float amount) { return mix(left, right, amount); }\n"
			"vec3 FxMix(vec3 left, vec3 right, float amount) { return mix(left, right, amount); }\n"
			"vec4 FxMix(vec4 left, vec4 right, float amount) { return mix(left, right, amount); }\n"
			"vec2 FxMix(vec2 left, vec2 right, vec2 amount) { return mix(left, right, amount); }\n"
			"vec3 FxMix(vec3 left, vec3 right, vec3 amount) { return mix(left, right, amount); }\n"
			"vec4 FxMix(vec4 left, vec4 right, vec4 amount) { return mix(left, right, amount); }\n"
			"vec2 FxMix(vec2 left, float right, float amount) { return mix(left, vec2(right), amount); }\n"
			"vec3 FxMix(vec3 left, float right, float amount) { return mix(left, vec3(right), amount); }\n"
			"vec4 FxMix(vec4 left, float right, float amount) { return mix(left, vec4(right), amount); }\n"
			"vec2 FxMix(float left, vec2 right, float amount) { return mix(vec2(left), right, amount); }\n"
			"vec3 FxMix(float left, vec3 right, float amount) { return mix(vec3(left), right, amount); }\n"
			"vec4 FxMix(float left, vec4 right, float amount) { return mix(vec4(left), right, amount); }\n"
			"float FxMix(float left, float right, bool amount) { return amount ? right : left; }\n"
			"vec2 FxMix(vec2 left, vec2 right, bool amount) { return amount ? right : left; }\n"
			"vec3 FxMix(vec3 left, vec3 right, bool amount) { return amount ? right : left; }\n"
			"vec4 FxMix(vec4 left, vec4 right, bool amount) { return amount ? right : left; }\n";
	common += "float FxDot(float left, float right) { return left * right; }\n"
			"float FxDot(vec2 left, vec2 right) { return dot(left, right); }\n"
			"float FxDot(vec3 left, vec3 right) { return dot(left, right); }\n"
			"float FxDot(vec4 left, vec4 right) { return dot(left, right); }\n"
			"float FxDot(vec2 left, float right) { return dot(left, vec2(right)); }\n"
			"float FxDot(vec3 left, float right) { return dot(left, vec3(right)); }\n"
			"float FxDot(vec4 left, float right) { return dot(left, vec4(right)); }\n"
			"float FxDot(float left, vec2 right) { return dot(vec2(left), right); }\n"
			"float FxDot(float left, vec3 right) { return dot(vec3(left), right); }\n"
			"float FxDot(float left, vec4 right) { return dot(vec4(left), right); }\n";
	std::string effectCode;
	for (const std::string& global : effect.globals) {
		std::string declaration = global;
		lowerLanguage(declaration);
		effectCode += declaration + "\n";
	}
	for (const ReShadeStruct& structure : effect.structs) {
		std::string body = structure.body;
		lowerLanguage(body);
		effectCode += "struct " + structure.name + " {\n" + body + "\n};\n";
	}
	// FX permits a function to call one declared later in the file. Emit GLSL
	// prototypes so source order never changes reachability or compilation.
	for (const ReShadeFunction *function : functions) {
		std::string signature = function->signature;
		lowerLanguage(signature);
		effectCode += signature + ";\n";
	}
	for (const ReShadeFunction *function : functions)
		effectCode += emitFunction(*function);
	lowerBooleanArithmetic(effectCode, effect);
	common += effectCode;

	std::vector<FunctionParameter> vertexParameters;
	std::vector<VertexVarying> vertexVaryings;
	std::unordered_map<std::string, std::string> varyingInputs;
	std::string vertexSetup;
	std::string vertexArguments;
	if (!pass.vertexShader.empty()) {
		if (!buildCall(effect, *vertex, true, vertexSetup, vertexArguments, error, &vertexParameters)
				|| !collectVertexVaryings(effect, *vertex, vertexParameters, vertexVaryings, varyingInputs, error))
			return false;
	}
	std::string fragmentVaryingDeclarations;
	std::string vertexVaryingDeclarations;
	for (const VertexVarying& varying : vertexVaryings) {
		fragmentVaryingDeclarations += "in " + varying.type + " " + varying.name + ";\n";
		vertexVaryingDeclarations += "out " + varying.type + " " + varying.name + ";\n";
	}

	std::string setup;
	std::string arguments;
	std::vector<FunctionParameter> pixelParameters;
	if (!buildCall(effect, *pixel, false, setup, arguments, error, &pixelParameters,
			varyingInputs.empty() ? nullptr : &varyingInputs))
		return false;
	std::string assignments;
	std::vector<StructField> outputs;
	std::vector<FunctionParameter> parameterOutputs;
	for (const FunctionParameter& parameter : pixelParameters)
		if (parameter.output
				&& std::regex_match(parameter.semantic, std::regex(R"((?:SV_Target|COLOR)[0-9]*)", std::regex::icase)))
			parameterOutputs.push_back(parameter);
	const bool returnsVoid = lowercase(pixel->returnType) == "void";
	if (returnsVoid) {
		if (parameterOutputs.empty())
			return fail(error, "ReShade FX void pixel shader has no out SV_Target or COLOR parameter.");
		std::sort(parameterOutputs.begin(), parameterOutputs.end(), [](const FunctionParameter& left,
				const FunctionParameter& right) {
			return semanticIndex(left.semantic) < semanticIndex(right.semantic);
		});
		assignments = "\t" + loweredFunctionName(*pixel) + "(" + arguments + ");\n";
		for (size_t index = 0; index < parameterOutputs.size(); ++index) {
			if (semanticIndex(parameterOutputs[index].semantic) != index || index >= 8)
				return fail(error, "ReShade FX pixel output semantics must be contiguous from SV_Target0 to SV_Target7.");
			std::string expression;
			if (!outputExpression(parameterOutputs[index].type, parameterOutputs[index].name, expression))
				return fail(error, "ReShade FX pixel shader has an unsupported out parameter type.");
			assignments += "\tFxOutput" + std::to_string(index) + " = " + expression + ";\n";
		}
	}
	else if (!parameterOutputs.empty()) {
		return fail(error, "ReShade FX pixel shaders cannot mix return values with out target parameters.");
	}
	for (const StructField& field : structFields(effect, pixel->returnType))
		if (std::regex_match(field.semantic, std::regex(R"((?:SV_Target|COLOR)[0-9]*)", std::regex::icase)))
			outputs.push_back(field);
	if (!returnsVoid && outputs.empty()) {
		if (!structFields(effect, pixel->returnType).empty())
			return fail(error, "ReShade FX pixel output struct has no SV_Target or COLOR field.");
		std::string expression;
		if (!outputExpression(pixel->returnType, loweredFunctionName(*pixel) + "(" + arguments + ")", expression))
			return fail(error, "ReShade FX pixel shader returns an unsupported output type.");
		assignments = "\tFxOutput0 = " + expression + ";\n";
	}
	else if (!returnsVoid) {
		std::sort(outputs.begin(), outputs.end(), [](const StructField& left, const StructField& right) {
			return semanticIndex(left.semantic) < semanticIndex(right.semantic);
		});
		const std::string result = "fxPixelResult";
		assignments = "\t" + pixel->returnType + " " + result + " = " + loweredFunctionName(*pixel)
				+ "(" + arguments + ");\n";
		for (size_t index = 0; index < outputs.size(); ++index) {
			if (semanticIndex(outputs[index].semantic) != index || index >= 8)
				return fail(error, "ReShade FX pixel output semantics must be contiguous from SV_Target0 to SV_Target7.");
			std::string expression;
			if (!outputExpression(outputs[index].type, result + "." + outputs[index].name, expression))
				return fail(error, "ReShade FX pixel shader has an unsupported output field type.");
			assignments += "\tFxOutput" + std::to_string(index) + " = " + expression + ";\n";
		}
	}
	compiled.outputCount = static_cast<uint32_t>(returnsVoid ? parameterOutputs.size() : (outputs.empty() ? 1 : outputs.size()));
	compiled.fragment = declarations + fragmentVaryingDeclarations + common
			+ "\nvoid main()\n{\n" + setup + assignments + "}\n";
	std::unordered_set<std::string> declaredSamplers { "Source" };
	for (const ReShadeSampler *sampler : samplers)
		declaredSamplers.insert(sampler->name);
	for (const ReShadeFunction *function : functions) {
		std::vector<FunctionParameter> parameters;
		if (!parseFunctionParameters(*function, parameters, error))
			return false;
		for (const FunctionParameter& parameter : parameters)
			if (lowercase(parameter.type) == "sampler" || lowercase(parameter.type) == "sampler2d")
				declaredSamplers.insert(parameter.name);
	}
	const std::regex textureCall(R"(\b(?:texture|textureLod|textureGrad)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*))");
	for (std::sregex_iterator it(compiled.fragment.begin(), compiled.fragment.end(), textureCall), end; it != end; ++it)
		if (declaredSamplers.count((*it)[1].str()) == 0)
			return fail(error, "ReShade FX samples undeclared sampler '" + (*it)[1].str() + "'.");

	if (!pass.vertexShader.empty()) {
		setup = vertexSetup;
		arguments = vertexArguments;
		const std::regex positionSemantic(R"((?:SV_Position|POSITION)[0-9]*)", std::regex::icase);
		const std::regex texcoordSemantic(R"(TEXCOORD[0-9]*)", std::regex::icase);
		std::string assignments;
		std::string invocation;
		if (lowercase(vertex->returnType) == "void") {
			const auto position = std::find_if(vertexParameters.begin(), vertexParameters.end(), [&](const auto& parameter) {
				return parameter.output && std::regex_match(parameter.semantic, positionSemantic);
			});
			if (position == vertexParameters.end())
				return fail(error, "ReShade FX void vertex shader has no position output semantic.");
			const auto texcoord = std::find_if(vertexParameters.begin(), vertexParameters.end(), [&](const auto& parameter) {
				return parameter.output && std::regex_match(parameter.semantic, texcoordSemantic);
			});
			const bool usesVertexId = std::any_of(vertexParameters.begin(), vertexParameters.end(), [](const auto& parameter) {
				return !parameter.output && lowercase(parameter.semantic) == "sv_vertexid";
			});
			if (usesVertexId && vertexVaryings.empty()) {
				// ReShade's shared vertex helper generates a three-vertex full-screen triangle,
				// while HollyCast presents post-processing passes with a four-vertex strip.
				// Preserve the equivalent host geometry instead of invoking it with incompatible IDs.
				setup.clear();
				assignments = "\tgl_Position = vec4(VertexCoord.xyz, 1.0);\n\tvTexCoord = TexCoord;\n";
			}
			else if (usesVertexId) {
				// The pass still needs its custom vertex outputs, but its full-screen triangle
				// position convention does not match HollyCast's four-vertex strip.
				invocation = "\t" + loweredFunctionName(*vertex) + "(" + arguments + ");\n";
				assignments = "\tgl_Position = vec4(VertexCoord.xyz, 1.0);\n\tvTexCoord = TexCoord;\n";
			}
			else {
				const std::string positionType = glslType(position->type);
				if (positionType == "vec4")
					assignments += "\tgl_Position = " + position->name + ";\n";
				else if (positionType == "vec3")
					assignments += "\tgl_Position = vec4(" + position->name + ", 1.0);\n";
				else if (positionType == "vec2")
					assignments += "\tgl_Position = vec4(" + position->name + ", 0.0, 1.0);\n";
				else
					return fail(error, "ReShade FX vertex position output must be a float vector.");

				if (texcoord == vertexParameters.end())
					assignments += "\tvTexCoord = TexCoord;\n";
				else if (glslType(texcoord->type) == "vec2")
					assignments += "\tvTexCoord = " + texcoord->name + ";\n";
				else if (glslType(texcoord->type) == "vec3" || glslType(texcoord->type) == "vec4")
					assignments += "\tvTexCoord = " + texcoord->name + ".xy;\n";
				else
					return fail(error, "ReShade FX vertex texture-coordinate output must be a float vector.");
				invocation = "\t" + loweredFunctionName(*vertex) + "(" + arguments + ");\n";
			}
		}
		else {
			const std::string position = resultField(effect, vertex->returnType, positionSemantic);
			const std::string texcoord = resultField(effect, vertex->returnType, texcoordSemantic);
			const std::string result = "fxVertexResult";
			if (position.empty() && structFields(effect, vertex->returnType).empty())
				assignments += "\tgl_Position = " + result + ";\n";
			else if (!position.empty())
				assignments += "\tgl_Position = " + result + "." + position + ";\n";
			else
				return fail(error, "ReShade FX vertex output has no position semantic.");
			assignments += texcoord.empty() ? "\tvTexCoord = TexCoord;\n"
					: "\tvTexCoord = " + result + "." + texcoord + ";\n";
			invocation = "\t" + glslType(vertex->returnType) + " " + result + " = "
					+ loweredFunctionName(*vertex) + "(" + arguments + ");\n";
		}
		for (const VertexVarying& varying : vertexVaryings)
			assignments += "\t" + varying.name + " = " + varying.source + ";\n";
		compiled.vertex = declarations + vertexVaryingDeclarations + common
				+ "\nvoid main()\n{\n" + setup + invocation + assignments + "}\n";
	}

	compiled.displayName = effect.techniques[techniqueIndex].name;
	source = std::move(compiled);
	error.clear();
	return true;
}

} // namespace postprocess
