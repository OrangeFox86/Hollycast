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
#include <cctype>
#include <cmath>
#include <regex>
#include <sstream>
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
	replaceAll(source, std::regex(R"(\bfloat2x2\b)"), "mat2");
	replaceAll(source, std::regex(R"(\bfloat3x3\b)"), "mat3");
	replaceAll(source, std::regex(R"(\bfloat4x4\b)"), "mat4");
	replaceAll(source, std::regex(R"(\bfloat2\b)"), "vec2");
	replaceAll(source, std::regex(R"(\bfloat3\b)"), "vec3");
	replaceAll(source, std::regex(R"(\bfloat4\b)"), "vec4");
	replaceAll(source, std::regex(R"(\buint2\b)"), "uvec2");
	replaceAll(source, std::regex(R"(\buint3\b)"), "uvec3");
	replaceAll(source, std::regex(R"(\buint4\b)"), "uvec4");
	replaceAll(source, std::regex(R"(\bint2\b)"), "ivec2");
	replaceAll(source, std::regex(R"(\bint3\b)"), "ivec3");
	replaceAll(source, std::regex(R"(\bint4\b)"), "ivec4");
	replaceAll(source, std::regex(R"(\blerp\s*\()"), "mix(");
	replaceAll(source, std::regex(R"(\bfrac\s*\()"), "fract(");
	replaceAll(source, std::regex(R"(\brsqrt\s*\()"), "inversesqrt(");
	replaceAll(source, std::regex(R"(\btex2D\s*\()"), "texture(");
	replaceAll(source, std::regex(R"(\btex2Dgrad\s*\()"), "textureGrad(");
	replaceAll(source, std::regex(R"(\btex2Dlod\s*\()"), "textureLod(");
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

bool parseValues(const std::string& expression, SlangParameterType type, SlangParameterValue& result)
{
	result = {};
	result.componentCount = componentCount(type);
	if (expression.empty())
		return true;
	const std::string lower = lowercase(expression);
	if (type == SlangParameterType::Bool && (lower == "true" || lower == "false")) {
		result.values[0] = lower == "true" ? 1.f : 0.f;
		return true;
	}
	std::string values = expression;
	const size_t open = values.find('(');
	if (open != std::string::npos && values.back() == ')'
			&& std::all_of(values.begin(), values.begin() + open, [](unsigned char c) {
				return std::isalnum(c) || c == '_';
			}))
		values = values.substr(open + 1, values.size() - open - 2);
	static const std::regex number(R"([-+]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)");
	uint32_t index = 0;
	for (std::sregex_iterator it(values.begin(), values.end(), number), end; it != end; ++it) {
		if (index >= result.componentCount)
			return false;
		try {
			result.values[index++] = std::stof((*it).str());
		} catch (...) {
			return false;
		}
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
	const std::string list = function.signature.substr(open + 1, close - open - 1);
	if (std::regex_match(list, std::regex(R"(\s*(?:void)?\s*)")))
		return true;
	for (std::string argument : splitArguments(list)) {
		replaceAll(argument, std::regex(R"(\b(?:in|out|inout|const|static)\b)"), " ");
		std::smatch match;
		const std::regex declaration(R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*([A-Za-z_][A-Za-z0-9_]*))?\s*$)");
		if (!std::regex_match(argument, match, declaration)) {
			error = "ReShade FX function '" + function.name + "' has an unsupported parameter declaration.";
			return false;
		}
		parameters.push_back({ match[1].str(), match[2].str(), match[3].str() });
	}
	return true;
}

struct StructField {
	std::string type;
	std::string name;
	std::string semantic;
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
	if (lower.rfind("position", 0) == 0 || lower == "sv_position")
		return vertex ? (glslType(type) == "vec2" ? "VertexCoord.xy" : "VertexCoord")
				: (glslType(type) == "vec2" ? "vTexCoord * OutputSize" : "vec4(vTexCoord * OutputSize, 0.0, 1.0)");
	if (lower.rfind("color", 0) == 0)
		return glslType(type) + "(1.0)";
	return glslType(type) + "(0.0)";
}

bool buildCall(const ReShadeEffect& effect, const ReShadeFunction& function, bool vertex,
		std::string& setup, std::string& arguments, std::string& error)
{
	std::vector<FunctionParameter> parameters;
	if (!parseFunctionParameters(function, parameters, error))
		return false;
	for (const FunctionParameter& parameter : parameters) {
		if (!arguments.empty())
			arguments += ", ";
		const std::vector<StructField> fields = structFields(effect, parameter.type);
		if (fields.empty()) {
			arguments += semanticValue(parameter.type, parameter.semantic, vertex);
			continue;
		}
		setup += "\t" + parameter.type + " " + parameter.name + ";\n";
		for (const StructField& field : fields)
			setup += "\t" + parameter.name + "." + field.name + " = "
					+ semanticValue(field.type, field.semantic, vertex) + ";\n";
		arguments += parameter.name;
	}
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
	try {
		return static_cast<uint32_t>(std::stoul(semantic.substr(firstDigit)));
	} catch (...) {
		return InvalidShaderPass;
	}
}

std::string emitFunction(const ReShadeFunction& function)
{
	std::string signature = function.signature;
	std::string body = function.body;
	lowerLanguage(signature);
	lowerLanguage(body);
	return signature + "\n{\n" + body + "\n}\n";
}

std::string samplerTexture(const ReShadeSampler& sampler)
{
	std::smatch match;
	if (std::regex_search(sampler.definition, match,
			std::regex(R"(\bTexture\s*=\s*([A-Za-z_][A-Za-z0-9_]*))", std::regex::icase)))
		return match[1].str();
	return sampler.name;
}

bool isSourceTexture(const ReShadeEffect& effect, const std::string& texture, const std::string& sampler)
{
	const std::string lowerTexture = lowercase(texture);
	const std::string lowerSampler = lowercase(sampler);
	if (lowerTexture == "source" || lowerTexture == "sourcetex" || lowerTexture == "backbuffer"
			|| lowerSampler == "source" || lowerSampler == "sourcesampler")
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

	ShaderPassSource compiled;
	uint32_t parameterComponents = 0;
	for (const ReShadeUniform& uniform : effect.uniforms) {
		if (uniform.valueType == ReShadeValueType::Unknown)
			return fail(error, "ReShade FX uniform '" + uniform.name + "' has an unsupported type.");
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
			else if (name == "ui_max" && !parseValues(annotation.value, parameter.type, parameter.maximum))
				return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid ui_max value.");
			else if (name == "ui_step" && !parseValues(annotation.value, parameter.type, parameter.step))
				return fail(error, "ReShade FX uniform '" + uniform.name + "' has an invalid ui_step value.");
		}
		parameterComponents += componentCount(parameter.type);
		if (parameterComponents > 16)
			return fail(error, "ReShade FX uses more than 16 uniform components in one pass.");
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
	}

	std::string declarations;
	for (size_t index = 0; index < compiled.slangParameters.size(); ++index)
		declarations += "uniform " + glslType(effect.uniforms[index].typeName)
				+ " " + compiled.slangParameters[index].name + ";\n";
	for (const ReShadeSampler& sampler : effect.samplers) {
		const std::string texture = samplerTexture(sampler);
		SlangResource resource;
		resource.type = SlangResourceType::Sampler2D;
		resource.name = sampler.name;
		resource.alias = texture;
		resource.convention = isSourceTexture(effect, texture, sampler.name)
				? SlangResourceConvention::Source : SlangResourceConvention::PassAlias;
		compiled.slangResources.push_back(std::move(resource));
		declarations += "uniform sampler2D " + sampler.name + ";\n";
	}
	if (effect.samplers.empty())
		declarations += "uniform sampler2D Source;\n";

	std::string common;
	for (const std::string& directive : effect.preprocessorDirectives) {
		if (!std::regex_match(directive, std::regex(R"(\s*#\s*(?:define|undef|if|ifdef|ifndef|elif|else|endif|line|pragma)\b.*)")))
			return fail(error, "ReShade FX uses an unsupported preprocessor directive.");
		common += directive + "\n";
	}
	common += "#define saturate(value) clamp((value), 0.0, 1.0)\n";
	common += "#define mul(left, right) ((left) * (right))\n";
	for (const ReShadeStruct& structure : effect.structs) {
		std::string body = structure.body;
		lowerLanguage(body);
		common += "struct " + structure.name + " {\n" + body + "\n};\n";
	}
	for (const ReShadeFunction& function : effect.functions)
		common += emitFunction(function);

	std::string setup;
	std::string arguments;
	if (!buildCall(effect, *pixel, false, setup, arguments, error))
		return false;
	const std::string result = pixel->name + "(" + arguments + ")";
	std::string assignments;
	std::vector<StructField> outputs;
	for (const StructField& field : structFields(effect, pixel->returnType))
		if (std::regex_match(field.semantic, std::regex(R"((?:SV_Target|COLOR)[0-9]*)", std::regex::icase)))
			outputs.push_back(field);
	if (outputs.empty()) {
		if (!structFields(effect, pixel->returnType).empty())
			return fail(error, "ReShade FX pixel output struct has no SV_Target or COLOR field.");
		assignments = "\tFxOutput0 = " + result + ";\n";
	}
	else {
		std::sort(outputs.begin(), outputs.end(), [](const StructField& left, const StructField& right) {
			return semanticIndex(left.semantic) < semanticIndex(right.semantic);
		});
		for (size_t index = 0; index < outputs.size(); ++index) {
			if (semanticIndex(outputs[index].semantic) != index || index >= 8)
				return fail(error, "ReShade FX pixel output semantics must be contiguous from SV_Target0 to SV_Target7.");
			assignments += "\tFxOutput" + std::to_string(index) + " = " + result + "." + outputs[index].name + ";\n";
		}
	}
	compiled.outputCount = static_cast<uint32_t>(outputs.empty() ? 1 : outputs.size());
	compiled.fragment = declarations + common + "\nvoid main()\n{\n" + setup + assignments + "}\n";
	std::unordered_set<std::string> declaredSamplers { "Source" };
	for (const ReShadeSampler& sampler : effect.samplers)
		declaredSamplers.insert(sampler.name);
	const std::regex textureCall(R"(\b(?:texture|textureLod|textureGrad)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*))");
	for (std::sregex_iterator it(compiled.fragment.begin(), compiled.fragment.end(), textureCall), end; it != end; ++it)
		if (declaredSamplers.count((*it)[1].str()) == 0)
			return fail(error, "ReShade FX samples undeclared sampler '" + (*it)[1].str() + "'.");

	if (!pass.vertexShader.empty()) {
		const ReShadeFunction *vertex = findFunction(effect, pass.vertexShader);
		if (vertex == nullptr)
			return fail(error, "ReShade FX VertexShader entry point '" + pass.vertexShader + "' has no definition.");
		setup.clear();
		arguments.clear();
		if (!buildCall(effect, *vertex, true, setup, arguments, error))
			return false;
		const std::string position = resultField(effect, vertex->returnType,
				std::regex(R"((?:SV_Position|POSITION)[0-9]*)", std::regex::icase));
		const std::string texcoord = resultField(effect, vertex->returnType,
				std::regex(R"(TEXCOORD[0-9]*)", std::regex::icase));
		const std::string result = "fxVertexResult";
		std::string assignments;
		if (position.empty() && structFields(effect, vertex->returnType).empty())
			assignments += "\tgl_Position = " + result + ";\n";
		else if (!position.empty())
			assignments += "\tgl_Position = " + result + "." + position + ";\n";
		else
			return fail(error, "ReShade FX vertex output has no position semantic.");
		assignments += texcoord.empty() ? "\tvTexCoord = TexCoord;\n"
				: "\tvTexCoord = " + result + "." + texcoord + ";\n";
		compiled.vertex = declarations + common + "\nvoid main()\n{\n" + setup + "\t" + glslType(vertex->returnType)
				+ " " + result + " = " + vertex->name + "(" + arguments + ");\n" + assignments + "}\n";
	}

	compiled.displayName = effect.techniques[techniqueIndex].name;
	source = std::move(compiled);
	error.clear();
	return true;
}

} // namespace postprocess
