/*
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
#include "shader_chain.h"
#include "reshade_fx.h"

#include "cfg/ini.h"
#include "cfg/option.h"
#include "oslib/storage.h"
#include "stdclass.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace postprocess {
namespace {

constexpr int MaxPassCount = 32;
constexpr uint32_t MaxHistoryFrames = 32;

uint32_t parameterComponentCount(SlangParameterType type)
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

ShaderParameter projectParameter(const SlangParameter& parameter)
{
	ShaderParameter projected;
	projected.name = parameter.name;
	projected.label = parameter.label;
	projected.type = parameter.type;
	projected.componentCount = parameterComponentCount(parameter.type);
	projected.values = parameter.defaultValue.values;
	projected.minimumValues = parameter.minimum.values;
	projected.maximumValues = parameter.maximum.values;
	projected.stepValues = parameter.step.values;
	projected.value = projected.values[0];
	projected.minimum = projected.minimumValues[0];
	projected.maximum = projected.maximumValues[0];
	projected.step = projected.stepValues[0];
	return projected;
}

std::mutex statusMutex;
ShaderChainStatus status;
std::atomic<uint64_t> statusGeneration { 0 };
std::atomic<uint64_t> reloadGeneration { 1 };
std::mutex sourceCacheMutex;
std::unordered_map<std::string, uint64_t> sourceCacheRefreshGeneration;

bool isAbsolutePath(const std::string& path)
{
	return !path.empty()
			&& (path[0] == '/' || path[0] == '\\'
					|| (path.size() > 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':'));
}

bool isSafeRelativePath(const std::string& path)
{
	if (path.empty() || isAbsolutePath(path))
		return false;
	std::istringstream stream(path);
	for (std::string segment; std::getline(stream, segment, '/');) {
		std::istringstream nested(segment);
		for (std::string part; std::getline(nested, part, '\\');)
			if (part == "..")
				return false;
	}
	return true;
}

bool isValidIdentifier(const std::string& value)
{
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_'))
		return false;
	return std::all_of(value.begin() + 1, value.end(), [](char character) {
		return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
	});
}

bool startsWith(const std::string& value, std::string_view prefix)
{
	return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::string parentPath(const std::string& path)
{
	const size_t separator = path.find_last_of("/\\");
	return separator == std::string::npos ? std::string() : path.substr(0, separator);
}

bool parseScaleType(const std::string& value, ScaleType& type)
{
	if (value.empty() || value == "source") {
		type = ScaleType::Source;
		return true;
	}
	if (value == "viewport") {
		type = ScaleType::Viewport;
		return true;
	}
	if (value == "absolute") {
		type = ScaleType::Absolute;
		return true;
	}
	return false;
}

bool parseWrapMode(const std::string& value, ShaderWrapMode& mode)
{
	if (value.empty() || value == "clamp_to_edge") {
		mode = ShaderWrapMode::ClampToEdge;
		return true;
	}
	if (value == "clamp_to_border") {
		mode = ShaderWrapMode::ClampToBorder;
		return true;
	}
	if (value == "repeat") {
		mode = ShaderWrapMode::Repeat;
		return true;
	}
	if (value == "mirrored_repeat") {
		mode = ShaderWrapMode::MirroredRepeat;
		return true;
	}
	return false;
}

bool parseBlendFactor(const std::string& value, ShaderBlendFactor& factor)
{
	std::string upper = value;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
	if (upper == "ZERO") factor = ShaderBlendFactor::Zero;
	else if (upper == "ONE") factor = ShaderBlendFactor::One;
	else if (upper == "SRC_COLOR") factor = ShaderBlendFactor::SourceColor;
	else if (upper == "INV_SRC_COLOR") factor = ShaderBlendFactor::OneMinusSourceColor;
	else if (upper == "DEST_COLOR") factor = ShaderBlendFactor::DestinationColor;
	else if (upper == "INV_DEST_COLOR") factor = ShaderBlendFactor::OneMinusDestinationColor;
	else if (upper == "SRC_ALPHA") factor = ShaderBlendFactor::SourceAlpha;
	else if (upper == "INV_SRC_ALPHA") factor = ShaderBlendFactor::OneMinusSourceAlpha;
	else if (upper == "DEST_ALPHA") factor = ShaderBlendFactor::DestinationAlpha;
	else if (upper == "INV_DEST_ALPHA") factor = ShaderBlendFactor::OneMinusDestinationAlpha;
	else return false;
	return true;
}

bool parseReShadePassState(const ReShadePass& effectPass, ShaderPass& pass, std::string& error)
{
	std::smatch value;
	if (std::regex_search(effectPass.stateBlock, value,
			std::regex(R"(\bBlendEnable\s*=\s*(true|false)\s*;)", std::regex::icase))) {
		std::string enabled = value[1].str();
		std::transform(enabled.begin(), enabled.end(), enabled.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		pass.blendEnabled = enabled == "true";
	}
	if (std::regex_search(effectPass.stateBlock, value,
			std::regex(R"(\bSrcBlend\s*=\s*([A-Za-z_]+)\s*;)", std::regex::icase))) {
		if (!parseBlendFactor(value[1].str(), pass.sourceBlend)) {
			error = "ReShade FX pass has an unsupported source blend factor.";
			return false;
		}
		pass.blendEnabled = true;
	}
	if (std::regex_search(effectPass.stateBlock, value,
			std::regex(R"(\bDestBlend\s*=\s*([A-Za-z_]+)\s*;)", std::regex::icase))) {
		if (!parseBlendFactor(value[1].str(), pass.destinationBlend)) {
			error = "ReShade FX pass has an unsupported destination blend factor.";
			return false;
		}
		pass.blendEnabled = true;
	}
	if (std::regex_search(effectPass.stateBlock, value,
			std::regex(R"(\bBlendOp\s*=\s*([A-Za-z_]+)\s*;)", std::regex::icase))) {
		std::string operation = value[1].str();
		std::transform(operation.begin(), operation.end(), operation.begin(), [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
		if (operation == "ADD")
			pass.blendOperation = ShaderBlendOperation::Add;
		else if (operation == "SUBTRACT")
			pass.blendOperation = ShaderBlendOperation::Subtract;
		else if (operation == "REVSUBTRACT" || operation == "REVERSE_SUBTRACT")
			pass.blendOperation = ShaderBlendOperation::ReverseSubtract;
		else if (operation == "MIN")
			pass.blendOperation = ShaderBlendOperation::Minimum;
		else if (operation == "MAX")
			pass.blendOperation = ShaderBlendOperation::Maximum;
		else {
			error = "ReShade FX pass has an unsupported blend operation.";
			return false;
		}
	}
	return true;
}

bool configureReShadeOutputFormat(const ReShadeEffect& effect, ShaderPassOutput& output, std::string& error)
{
	const auto texture = std::find_if(effect.textures.begin(), effect.textures.end(), [&](const ReShadeTexture& value) {
		return value.name == output.alias;
	});
	if (texture == effect.textures.end())
		return true;
	std::smatch formatMatch;
	if (!std::regex_search(texture->definition, formatMatch,
			std::regex(R"(\bFormat\s*=\s*([A-Za-z0-9_]+)\s*;)", std::regex::icase)))
		return true;
	std::string format = formatMatch[1].str();
	std::transform(format.begin(), format.end(), format.begin(), [](unsigned char character) {
		return static_cast<char>(std::toupper(character));
	});
	if (format == "RGBA8" || format == "RGBA8_UNORM")
		output.targetFormatName = "R8G8B8A8_UNORM";
	else if (format == "RGBA8_SRGB" || format == "SRGBA8")
		output.targetFormatName = "R8G8B8A8_SRGB";
	else if (format == "RGBA16F" || format == "RGBA16_FLOAT")
		output.targetFormatName = "R16G16B16A16_SFLOAT";
	else if (format == "RGBA16" || format == "RGBA16_UNORM")
		output.targetFormatName = "R16G16B16A16_UNORM";
	else if (format == "RGBA32F" || format == "RGBA32_FLOAT")
		output.targetFormatName = "R32G32B32A32_SFLOAT";
	else {
		error = "ReShade FX render target '" + output.alias + "' uses unsupported format '" + formatMatch[1].str() + "'.";
		return false;
	}
	return true;
}

std::vector<std::string> splitParameters(const std::string& value)
{
	std::vector<std::string> result;
	std::string normalized = value;
	std::replace(normalized.begin(), normalized.end(), ',', ';');
	std::istringstream stream(normalized);
	for (std::string parameter; std::getline(stream, parameter, ';');) {
		parameter = trim_ws(parameter, " \t");
		if (!parameter.empty())
			result.push_back(std::move(parameter));
	}
	return result;
}

bool fail(std::string& error, const std::string& message)
{
	error = message;
	return false;
}

bool findPassOutput(const ShaderChain& chain, const std::string& alias, size_t lastPass,
		uint32_t& sourcePass, uint32_t& sourceOutput)
{
	for (size_t index = 0; index <= lastPass && index < chain.passes.size(); ++index) {
		const ShaderPass& pass = chain.passes[index];
		if (pass.alias == alias) {
			sourcePass = static_cast<uint32_t>(index);
			sourceOutput = 0;
			return true;
		}
		for (size_t output = 0; output < pass.outputs.size(); ++output)
			if (pass.outputs[output].alias == alias) {
				sourcePass = static_cast<uint32_t>(index);
				sourceOutput = static_cast<uint32_t>(output);
				return true;
			}
	}
	return false;
}

bool hasPresetTexture(const ShaderChain& chain, const std::string& name)
{
	return std::any_of(chain.textures.begin(), chain.textures.end(), [&](const ShaderPresetTexture& texture) {
		return texture.name == name;
	});
}

bool parseIndexedSemantic(const std::string& value, const char *prefix, uint32_t& index)
{
	const size_t prefixLength = std::char_traits<char>::length(prefix);
	if (value.compare(0, prefixLength, prefix) != 0)
		return false;
	const std::string suffix = value.substr(prefixLength);
	if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); }))
		return false;
	try {
		const unsigned long parsed = std::stoul(suffix);
		if (parsed > std::numeric_limits<uint32_t>::max())
			return false;
		index = static_cast<uint32_t>(parsed);
		return true;
	} catch (...) {
		return false;
	}
}

bool resolveSlangResourceInputs(ShaderChain& chain, size_t passIndex,
		const ShaderPassSource& source, std::string& error)
{
	ShaderPass& pass = chain.passes[passIndex];
	pass.inputs.clear();
	pass.inputs.reserve(source.slangResources.size());
	for (const SlangResource& resource : source.slangResources) {
		ShaderPassInput input;
		input.bindingName = resource.name;
		input.resourceName = resource.alias;
		if (source.retroArchGlsl) {
			if (resource.name == "OrigTexture") {
				input.kind = ShaderInputKind::Original;
				pass.inputs.push_back(std::move(input));
				continue;
			}
			if (resource.name == "PassPrevTexture") {
				input.kind = ShaderInputKind::Source;
				pass.inputs.push_back(std::move(input));
				continue;
			}
			std::smatch previousPass;
			if (std::regex_match(resource.name, previousPass, std::regex(R"(PassPrev([0-9]+)Texture)"))) {
				unsigned long long distance;
				try {
					distance = std::stoull(previousPass[1].str());
				} catch (...) {
					return fail(error, "RetroArch GLSL resource '" + resource.name + "' has an invalid pass distance.");
				}
				if (distance >= passIndex)
					input.kind = ShaderInputKind::Original;
				else {
					input.kind = ShaderInputKind::PassOutput;
					input.sourcePass = static_cast<uint32_t>(passIndex - distance - 1);
				}
				pass.inputs.push_back(std::move(input));
				continue;
			}
		}
		switch (resource.convention) {
		case SlangResourceConvention::Source:
			input.kind = ShaderInputKind::Source;
			break;
		case SlangResourceConvention::Original:
			input.kind = ShaderInputKind::Original;
			break;
		case SlangResourceConvention::PassAlias: {
			uint32_t semanticIndex;
			if (parseIndexedSemantic(resource.alias, "PassOutput", semanticIndex)) {
				if (semanticIndex >= passIndex)
					return fail(error, "Slang resource '" + resource.name + "' must reference an earlier PassOutput index.");
				input.kind = ShaderInputKind::PassOutput;
				input.sourcePass = semanticIndex;
				break;
			}
			if (parseIndexedSemantic(resource.alias, "User", semanticIndex)) {
				if (semanticIndex >= chain.textures.size())
					return fail(error, "Slang resource '" + resource.name + "' references an unavailable User texture.");
				input.kind = ShaderInputKind::LookupTexture;
				input.resourceName = chain.textures[semanticIndex].name;
				break;
			}
			if (hasPresetTexture(chain, resource.name) || hasPresetTexture(chain, resource.alias)) {
				input.kind = ShaderInputKind::LookupTexture;
				input.resourceName = hasPresetTexture(chain, resource.alias) ? resource.alias : resource.name;
				break;
			}
			input.kind = ShaderInputKind::PassOutput;
			if (!findPassOutput(chain, resource.alias, passIndex, input.sourcePass, input.sourceOutput)
					|| input.sourcePass >= passIndex)
				return fail(error, "Slang resource '" + resource.name + "' must reference an earlier pass alias or preset texture.");
			break;
		}
		case SlangResourceConvention::History:
			if (resource.historyOffset >= MaxHistoryFrames)
				return fail(error, "Slang history resources may retain at most 32 frames.");
			if ((resource.alias == "Original" || resource.alias == "Source") && resource.historyOffset == 0) {
				input.kind = ShaderInputKind::Original;
				break;
			}
			input.kind = ShaderInputKind::History;
			input.historyOffset = resource.historyOffset;
			if (resource.alias != "Original" && resource.alias != "Source") {
				if (!findPassOutput(chain, resource.alias, passIndex, input.sourcePass, input.sourceOutput)
						|| input.sourcePass >= passIndex)
					return fail(error, "Slang history resource '" + resource.name + "' must reference an earlier pass alias.");
			}
			break;
		case SlangResourceConvention::Feedback:
			input.kind = ShaderInputKind::Feedback;
			if (parseIndexedSemantic(resource.alias, "PassFeedback", input.sourcePass)) {
				if (input.sourcePass >= chain.passes.size())
					return fail(error, "Slang resource '" + resource.name + "' references an unavailable PassFeedback index.");
				break;
			}
			if (!findPassOutput(chain, resource.alias, chain.passes.size() - 1, input.sourcePass, input.sourceOutput))
				return fail(error, "Slang feedback resource '" + resource.name + "' has no matching pass alias.");
			break;
		}
		pass.inputs.push_back(std::move(input));
	}
	return true;
}

bool readTextFile(const std::string& path, std::string& text, std::string& error)
{
	// Cache only immutable source bytes. Compilation remains backend-owned,
	// while this avoids reparsing large include packages on every renderer start.
	// Size and mtime are part of the key, so edits invalidate entries safely.
	hostfs::FileInfo info;
	try {
		info = hostfs::storage().getFileInfo(path);
	} catch (...) {
		info = {};
	}
	const auto hash = [](const std::string& value, u64 size, u64 updateTime) {
		uint64_t result = 1469598103934665603ull;
		for (unsigned char byte : value) {
			result ^= byte;
			result *= 1099511628211ull;
		}
		for (u64 number : { size, updateTime })
			for (unsigned index = 0; index < sizeof(number); ++index) {
				result ^= static_cast<unsigned char>(number >> (index * 8));
				result *= 1099511628211ull;
			}
		return result;
	};
	const std::string cacheDirectory = get_writable_config_path("shader-cache/");
	const std::string cacheHeader = "HollycastShaderCacheV1\n\n";
	const uint64_t currentReloadGeneration = reloadGeneration.load(std::memory_order_relaxed);
	bool bypassCache = false;
	if (currentReloadGeneration > 1) {
		std::lock_guard<std::mutex> lock(sourceCacheMutex);
		bypassCache = sourceCacheRefreshGeneration[path] != currentReloadGeneration;
	}
	auto cachePath = [&](void) {
		std::ostringstream name;
		name << cacheDirectory << std::hex << hash(path, info.size, info.updateTime) << ".src";
		return name.str();
	};
	if (!bypassCache && !cacheDirectory.empty() && info.size != 0) {
		make_directory(cacheDirectory);
		std::unique_ptr<hostfs::File> cache(hostfs::storage().openFile(cachePath(), "rb"));
		if (cache != nullptr) {
			std::string cached(static_cast<size_t>(cache->size()), '\0');
			if (info.size <= std::numeric_limits<size_t>::max() - cacheHeader.size()
					&& cached.size() == cacheHeader.size() + static_cast<size_t>(info.size)
					&& cache->read(cached.data(), 1, cached.size()) == cached.size()
					&& cached.rfind(cacheHeader, 0) == 0) {
				text = cached.substr(cacheHeader.size());
				return true;
			}
		}
	}
	// Read raw bytes so Windows CRLF translation cannot make the byte-count check fail.
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rb"));
	if (file == nullptr)
		return fail(error, "Unable to open shader source: " + path);
	text.resize(static_cast<size_t>(file->size()));
	if (!text.empty() && file->read(text.data(), 1, text.size()) != text.size())
		return fail(error, "Unable to read shader source: " + path);
	if (!cacheDirectory.empty() && info.size != 0) {
		make_directory(cacheDirectory);
		if (std::unique_ptr<hostfs::File> cache(hostfs::storage().openFile(cachePath(), "wb")); cache != nullptr) {
			cache->write(cacheHeader.data(), 1, cacheHeader.size());
			if (!text.empty())
				cache->write(text.data(), 1, text.size());
		}
	}
	if (currentReloadGeneration > 1) {
		std::lock_guard<std::mutex> lock(sourceCacheMutex);
		sourceCacheRefreshGeneration[path] = currentReloadGeneration;
	}
	return true;
}

bool expandPackageIncludes(const std::string& path, const std::string& packageRoot, int depth,
		std::unordered_set<std::string>& includeStack, std::string& expanded, std::string& error)
{
	if (depth > 16)
		return fail(error, "Shader include nesting exceeds 16 files.");
	if (!includeStack.emplace(path).second)
		return fail(error, "Shader include cycle detected: " + path);

	std::string text;
	if (!readTextFile(path, text, error)) {
		includeStack.erase(path);
		return false;
	}

	const std::regex includePattern(R"include(^\s*#\s*include\s*"([^"]+)"\s*$)include");
	std::istringstream stream(text);
	for (std::string line; std::getline(stream, line);) {
		if (line.find("#include") == std::string::npos) {
			expanded += line + '\n';
			continue;
		}
		std::smatch match;
		if (!std::regex_match(line, match, includePattern) || !isSafeRelativePath(match[1].str())) {
			includeStack.erase(path);
			return fail(error, "Shader includes must use a package-relative quoted path.");
		}
		const std::string includePath = hostfs::storage().getSubPath(parentPath(path), match[1].str());
		// Paths containing '..' are rejected before joining, which is portable
		// across storage backends that do not canonicalize virtual paths.
		if (includePath.rfind(packageRoot, 0) != 0) {
			includeStack.erase(path);
			return fail(error, "Shader include escapes its package root.");
		}
		if (!expandPackageIncludes(includePath, packageRoot, depth + 1, includeStack, expanded, error)) {
			includeStack.erase(path);
			return false;
		}
	}
	includeStack.erase(path);
	return true;
}

void removePortableInterfaceDeclarations(std::string& source, const std::vector<ShaderParameter>& parameters)
{
	// Slang files usually declare the same interfaces supplied by HollyCast.
	// Strip only those exact declarations, leaving all author code untouched.
	const std::regex interfaceDeclaration(
			R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:in|out)\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?[^;\n]*\b(?:vTexCoord|FragColor)\s*;\s*$)",
			std::regex::icase);
	const std::regex frameDeclaration(
			R"(^\s*uniform\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?(?:sampler2D|vec2|int)\s+(?:Source|SourceSize|OriginalSize|OutputSize|FrameCount|Rotation)\s*;\s*$)",
			std::regex::icase);
	const std::regex parameterDeclaration(
			R"(^\s*uniform\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?(?:float|int|bool)\s+([^;]+)\s*;\s*$)",
			std::regex::icase);
	const auto declaresOnlyParameters = [&](const std::string& line) {
		std::smatch match;
		if (!std::regex_match(line, match, parameterDeclaration))
			return false;
		std::istringstream names(match[1].str());
		bool found = false;
		for (std::string name; std::getline(names, name, ',');) {
			const size_t begin = name.find_first_not_of(" \t\r\n");
			const size_t end = name.find_last_not_of(" \t\r\n");
			if (begin == std::string::npos || !isValidIdentifier(name.substr(begin, end - begin + 1)))
				return false;
			name = name.substr(begin, end - begin + 1);
			if (std::none_of(parameters.begin(), parameters.end(), [&](const ShaderParameter& parameter) {
				return parameter.name == name;
			}))
				return false;
			found = true;
		}
		return found;
	};
	std::string cleaned;
	std::istringstream stream(source);
	bool firstLine = true;
	for (std::string line; std::getline(stream, line);) {
		bool remove = (firstLine && startsWith(line, "#version"))
				|| std::regex_match(line, interfaceDeclaration) || std::regex_match(line, frameDeclaration)
				|| declaresOnlyParameters(line);
		if (!remove)
			cleaned += line + '\n';
		firstLine = false;
	}
	source = std::move(cleaned);
}

void replaceAll(std::string& source, const std::regex& pattern, const char *replacement)
{
	source = std::regex_replace(source, pattern, replacement);
}

bool resolveDimension(ScaleType scaleType, float scale, int sourceSize, int viewportSize,
		int& result, std::string& error)
{
	const int base = scaleType == ScaleType::Source ? sourceSize
			: scaleType == ScaleType::Viewport ? viewportSize : 1;
	if (base <= 0 || scale <= 0.f)
		return fail(error, "Shader pass dimensions must be positive.");

	const double scaled = static_cast<double>(base) * scale;
	if (scaled > std::numeric_limits<int>::max())
		return fail(error, "Shader pass dimensions are too large.");
	result = static_cast<int>(std::lround(scaled));
	return result > 0 || fail(error, "Shader pass dimensions must be positive.");
}

bool mergeShaderParameter(ShaderChain& chain, const ShaderParameter& reflected, std::string& error)
{
	auto existing = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const ShaderParameter& parameter) {
		return parameter.name == reflected.name;
	});
	if (existing == chain.parameters.end()) {
		chain.parameters.push_back(reflected);
		return true;
	}
	if (existing->componentCount != reflected.componentCount || existing->type != reflected.type)
		return fail(error, "Shader parameter '" + reflected.name + "' changes type between passes or layers.");

	const bool reflectedDefault = reflected.step <= 0.f && reflected.minimum >= reflected.maximum;
	const bool existingDefault = existing->step <= 0.f && existing->minimum >= existing->maximum;
	if (!reflectedDefault || existingDefault) {
		existing->label = reflected.label;
		existing->minimum = reflected.minimum;
		existing->maximum = reflected.maximum;
		existing->step = reflected.step;
		existing->values = reflected.values;
		existing->minimumValues = reflected.minimumValues;
		existing->maximumValues = reflected.maximumValues;
		existing->stepValues = reflected.stepValues;
		existing->choiceLabels = reflected.choiceLabels;
	}
	return true;
}

bool normalizeShaderParameterValue(const ShaderParameter& parameter, uint32_t component,
		float input, float& output)
{
	if (component >= parameter.componentCount || !std::isfinite(input))
		return false;
	const float minimum = parameter.componentCount == 1 ? parameter.minimum : parameter.minimumValues[component];
	const float maximum = parameter.componentCount == 1 ? parameter.maximum : parameter.maximumValues[component];
	const float step = parameter.componentCount == 1 ? parameter.step : parameter.stepValues[component];
	if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum
			|| input < minimum || input > maximum)
		return false;

	output = input;
	if (minimum < maximum && std::isfinite(step) && step > 0.f) {
		const float tolerance = std::max(1.f, std::fabs(maximum)) * 1e-6f;
		if (std::fabs(input - maximum) <= tolerance)
			output = maximum;
		else
			output = std::clamp(minimum + std::round((input - minimum) / step) * step, minimum, maximum);
	}
	if (parameter.type == SlangParameterType::Bool)
		output = output == 0.f ? 0.f : 1.f;
	else if (parameter.type == SlangParameterType::Int)
		output = std::clamp(std::round(output), minimum, maximum);
	return true;
}

} // namespace

ShaderFormat detectFormat(const std::string& path)
{
	const std::string extension = get_file_extension(path);
	if (extension == "glsl")
		return ShaderFormat::GLSL;
	if (extension == "glslp")
		return ShaderFormat::GlslPreset;
	if (extension == "fx")
		return ShaderFormat::ReShadeFX;
	if (extension == "slang")
		return ShaderFormat::Slang;
	if (extension == "slangp")
		return ShaderFormat::SlangPreset;
	return ShaderFormat::Unknown;
}

const char *formatName(ShaderFormat format)
{
	switch (format) {
	case ShaderFormat::GLSL:
		return "GLSL";
	case ShaderFormat::GlslPreset:
		return "GLSL preset";
	case ShaderFormat::ReShadeFX:
		return "ReShade FX";
	case ShaderFormat::Slang:
		return "Slang";
	case ShaderFormat::SlangPreset:
		return "Slang preset";
	default:
		return "Unknown";
	}
}

std::vector<std::string> configuredShaderPaths()
{
	std::vector<std::string> paths = config::PostProcessShaders.get();
	if (paths.empty() && !config::PostProcessShader.get().empty())
		paths.push_back(config::PostProcessShader.get());
	paths.erase(std::remove_if(paths.begin(), paths.end(), [](const std::string& path) { return path.empty(); }), paths.end());
	for (size_t index = 0; index < paths.size(); ++index)
		paths.erase(std::remove(paths.begin() + index + 1, paths.end(), paths[index]), paths.end());
	return paths;
}

std::string shaderSelectionLabel(const std::vector<std::string>& paths)
{
	if (paths.empty())
		return {};
	if (paths.size() == 1)
		return paths.front();
	return std::to_string(paths.size()) + " shader layers";
}

bool resolveTargetSize(const ShaderPass& pass, const ShaderFrameInfo& frame,
		int& width, int& height, std::string& error)
{
	if (!resolveDimension(pass.scaleTypeX, pass.scaleX, frame.sourceWidth, frame.viewportWidth, width, error)
			|| !resolveDimension(pass.scaleTypeY, pass.scaleY, frame.sourceHeight, frame.viewportHeight, height, error))
		return false;
	error.clear();
	return true;
}

std::vector<std::vector<bool>> resolveShaderPassOutputMipmaps(const ShaderChain& chain)
{
	std::vector<std::vector<bool>> requirements(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const ShaderPass& pass = chain.passes[passIndex];
		const size_t outputCount = pass.outputs.empty() ? 1 : pass.outputs.size();
		requirements[passIndex].assign(outputCount, false);
		for (size_t outputIndex = 0; outputIndex < pass.outputs.size(); ++outputIndex)
			requirements[passIndex][outputIndex] = pass.outputs[outputIndex].useMipmaps;
	}
	for (size_t consumerIndex = 0; consumerIndex < chain.passes.size(); ++consumerIndex) {
		const ShaderPass& consumer = chain.passes[consumerIndex];
		if (!consumer.useMipmaps)
			continue;
		if (consumerIndex != 0)
			requirements[consumerIndex - 1][0] = true;
		for (const ShaderPassInput& input : consumer.inputs) {
			if ((input.kind != ShaderInputKind::PassOutput && input.kind != ShaderInputKind::History
						&& input.kind != ShaderInputKind::Feedback)
					|| input.sourcePass >= requirements.size()
					|| input.sourceOutput >= requirements[input.sourcePass].size())
				continue;
			requirements[input.sourcePass][input.sourceOutput] = true;
		}
	}
	return requirements;
}

bool resolveShaderPassInputs(ShaderChain& chain, size_t passIndex,
		const ShaderPassSource& source, std::string& error)
{
	if (passIndex >= chain.passes.size())
		return fail(error, "Shader pass input resolution received an invalid pass index.");
	return resolveSlangResourceInputs(chain, passIndex, source, error);
}

ShaderChainStatus getShaderChainStatus()
{
	std::lock_guard<std::mutex> lock(statusMutex);
	return status;
}

uint64_t shaderChainStatusGeneration()
{
	return statusGeneration.load(std::memory_order_acquire);
}

void reportShaderChainStatus(const std::string& activePath, const std::string& error,
		const std::vector<ShaderParameter>& parameters,
		const std::vector<std::string>& techniques, int selectedTechnique)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	status.activePath = activePath;
	status.error = error;
	status.parameters = parameters;
	status.techniques = techniques;
	status.selectedTechnique = selectedTechnique;
	statusGeneration.fetch_add(1, std::memory_order_release);
}

bool setShaderChainParameter(const std::string& name, float value)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	auto parameter = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const ShaderParameter& candidate) {
		return candidate.name == name;
	});
	float normalized;
	if (parameter == status.parameters.end() || !normalizeShaderParameterValue(*parameter, 0, value, normalized))
		return false;
	parameter->value = normalized;
	parameter->values[0] = normalized;
	statusGeneration.fetch_add(1, std::memory_order_release);
	return true;
}

bool setShaderChainParameterComponents(const std::string& name, const float *values, uint32_t componentCount)
{
	if (values == nullptr || componentCount == 0 || componentCount > 16)
		return false;
	std::lock_guard<std::mutex> lock(statusMutex);
	auto parameter = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const ShaderParameter& candidate) {
		return candidate.name == name;
	});
	if (parameter == status.parameters.end() || parameter->componentCount != componentCount)
		return false;
	std::array<float, 16> normalized {};
	for (uint32_t index = 0; index < componentCount; ++index) {
		if (!normalizeShaderParameterValue(*parameter, index, values[index], normalized[index]))
			return false;
	}
	std::copy_n(normalized.begin(), componentCount, parameter->values.begin());
	parameter->value = parameter->values[0];
	statusGeneration.fetch_add(1, std::memory_order_release);
	return true;
}

void requestShaderChainReload()
{
	reloadGeneration.fetch_add(1, std::memory_order_relaxed);
}

uint64_t shaderChainReloadGeneration()
{
	return reloadGeneration.load(std::memory_order_relaxed);
}

void applyShaderChainParameterValues(ShaderChain& chain, const std::string& values)
{
	std::istringstream entries(values);
	for (std::string entry; std::getline(entries, entry, ';');) {
		const size_t separator = entry.find('=');
		if (separator == std::string::npos)
			continue;
		const std::string name = trim_ws(entry.substr(0, separator), " \t");
		if (!isValidIdentifier(name))
			continue;
		std::vector<float> parsedValues;
		std::istringstream valueStream(entry.substr(separator + 1));
		for (std::string component; std::getline(valueStream, component, ',');) {
			std::istringstream scalar(trim_ws(component, " \t"));
			float value;
			if (!(scalar >> value) || (scalar >> std::ws && !scalar.eof()) || !std::isfinite(value)) {
				parsedValues.clear();
				break;
			}
			parsedValues.push_back(value);
		}
		if (parsedValues.empty())
			continue;
		auto parameter = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const ShaderParameter& candidate) {
			return candidate.name == name;
		});
		if (parameter == chain.parameters.end() || parsedValues.size() != parameter->componentCount)
			continue;
		std::array<float, 16> normalized {};
		bool valid = true;
		for (uint32_t index = 0; index < parameter->componentCount; ++index) {
			valid = valid && normalizeShaderParameterValue(*parameter, index, parsedValues[index], normalized[index]);
		}
		if (!valid)
			continue;
		std::copy_n(normalized.begin(), parameter->componentCount, parameter->values.begin());
		parameter->value = parameter->values[0];
		for (ShaderPassSource& source : chain.sources)
			for (SlangParameter& reflected : source.slangParameters)
				if (reflected.name == name)
					std::copy_n(parameter->values.begin(), parameter->componentCount, reflected.defaultValue.values.begin());
	}
}

std::string serializeShaderChainParameterValues(const std::vector<ShaderParameter>& parameters)
{
	std::ostringstream values;
	values << std::setprecision(std::numeric_limits<float>::max_digits10);
	for (const ShaderParameter& parameter : parameters) {
		if (!isValidIdentifier(parameter.name) || parameter.componentCount == 0 || parameter.componentCount > 16)
			continue;
		if (values.tellp() > 0)
			values << ';';
		values << parameter.name << '=';
		for (uint32_t index = 0; index < parameter.componentCount; ++index) {
			if (index != 0)
				values << ',';
			const float component = parameter.componentCount == 1 ? parameter.value : parameter.values[index];
			if (!std::isfinite(component)) {
				values << 0.f;
				break;
			}
			values << component;
		}
	}
	return values.str();
}

static bool parsePreset(const std::string& presetText, const std::string& presetPath,
		ShaderFormat passFormat, const char *presetName, const char *passExtension,
		ShaderChain& chain, std::string& error)
{
	// RetroArch's repositories use CRLF presets. IniFile retains carriage returns,
	// which would otherwise become part of paths and enum-like values.
	std::string normalizedPreset = presetText;
	normalizedPreset.erase(std::remove(normalizedPreset.begin(), normalizedPreset.end(), '\r'), normalizedPreset.end());
	config::IniFile preset;
	preset.load(normalizedPreset);
	const int passCount = preset.getInt("", "shaders", -1);
	if (passCount < 1 || passCount > MaxPassCount)
		return fail(error, std::string(presetName) + " must declare between 1 and 32 shader passes.");

	const std::string basePath = parentPath(presetPath);
	if (basePath.empty())
		return fail(error, std::string("Unable to resolve the ") + presetName + " folder.");

	ShaderChain parsedChain;
	parsedChain.packagePath = presetPath;
	parsedChain.wantsUnscaledInput = true;
	parsedChain.passes.reserve(passCount);
	for (int index = 0; index < passCount; ++index) {
		const std::string suffix = std::to_string(index);
		const std::string& source = preset.getString("", "shader" + suffix);
		if (source.empty())
			return fail(error, std::string(presetName) + " is missing shader" + suffix + ".");
		if (!isSafeRelativePath(source))
			return fail(error, std::string(presetName) + " shader paths must be relative to the preset.");

		ShaderPass pass;
		pass.sourcePath = hostfs::storage().getSubPath(basePath, source);
		pass.packageRoot = basePath;
		pass.format = detectFormat(pass.sourcePath);
		if (pass.format != passFormat)
			return fail(error, std::string(presetName) + " pass " + suffix + " must reference a " + passExtension + " shader.");
		pass.alias = preset.getString("", "alias" + suffix);
		pass.linearFilter = preset.getBool("", "filter_linear" + suffix);
		pass.useMipmaps = preset.getBool("", "mipmap_input" + suffix);
		pass.frameCountModulo = static_cast<uint32_t>(std::max(0, preset.getInt("", "frame_count_mod" + suffix)));
		pass.scaleX = preset.getFloat("", "scale_x" + suffix, 1.f);
		pass.scaleY = preset.getFloat("", "scale_y" + suffix, pass.scaleX);
		const bool floatTarget = preset.getBool("", "float_framebuffer" + suffix);
		const bool srgbTarget = preset.getBool("", "srgb_framebuffer" + suffix);
		if (floatTarget && srgbTarget)
			return fail(error, std::string(presetName) + " pass " + suffix + " cannot request both float and sRGB framebuffers.");
		pass.targetFormat = floatTarget ? ShaderTargetFormat::Float
				: srgbTarget ? ShaderTargetFormat::Srgb : ShaderTargetFormat::Default;
		pass.targetFormatName = preset.getString("", "format" + suffix);
		if (!pass.targetFormatName.empty() && !isValidIdentifier(pass.targetFormatName))
			return fail(error, std::string(presetName) + " pass " + suffix + " has an invalid target format name.");

		if (pass.scaleX <= 0.f || pass.scaleY <= 0.f)
			return fail(error, std::string(presetName) + " scale values must be positive.");
		if (!parseScaleType(preset.getString("", "scale_type_x" + suffix,
				preset.getString("", "scale_type" + suffix)), pass.scaleTypeX)
				|| !parseScaleType(preset.getString("", "scale_type_y" + suffix,
				preset.getString("", "scale_type" + suffix)), pass.scaleTypeY))
			return fail(error, std::string(presetName) + " contains an unsupported scale type.");

		parsedChain.passes.push_back(std::move(pass));
	}

	std::unordered_set<std::string> aliases;
	for (const ShaderPass& pass : parsedChain.passes) {
		if (pass.alias.empty())
			continue;
		if (!isValidIdentifier(pass.alias))
			return fail(error, std::string(presetName) + " pass aliases must be valid identifiers.");
		if (!aliases.emplace(pass.alias).second)
			return fail(error, std::string(presetName) + " contains duplicate pass alias '" + pass.alias + "'.");
	}

	const int feedbackPass = preset.getInt("", "feedback_pass", -1);
	if (feedbackPass < -1 || feedbackPass >= passCount)
		return fail(error, std::string(presetName) + " feedback_pass must identify a declared shader pass.");
	if (feedbackPass >= 0) {
		parsedChain.feedbackPass = static_cast<uint32_t>(feedbackPass);
		parsedChain.passes[parsedChain.feedbackPass].feedbackTarget = true;
	}

	std::unordered_set<std::string> textureNames;
	for (const std::string& name : splitParameters(preset.getString("", "textures"))) {
		if (!isValidIdentifier(name))
			return fail(error, std::string(presetName) + " texture names must be valid identifiers.");
		if (!textureNames.emplace(name).second)
			return fail(error, std::string(presetName) + " contains duplicate texture '" + name + "'.");
		const std::string& texturePath = preset.getString("", name);
		if (!isSafeRelativePath(texturePath))
			return fail(error, std::string(presetName) + " texture paths must be relative to the preset.");
		ShaderPresetTexture texture;
		texture.name = name;
		texture.path = hostfs::storage().getSubPath(basePath, texturePath);
		texture.linearFilter = preset.getBool("", name + "_linear");
		texture.useMipmaps = preset.getBool("", name + "_mipmap");
		if (!parseWrapMode(preset.getString("", name + "_wrap_mode"), texture.wrapMode))
			return fail(error, std::string(presetName) + " texture '" + name + "' has an unsupported wrap mode.");
		parsedChain.textures.push_back(std::move(texture));
	}

	for (const std::string& name : splitParameters(preset.getString("", "parameters"))) {
		ShaderParameter parameter;
		parameter.name = name;
		parameter.value = preset.getFloat("", name);
		parsedChain.parameters.push_back(std::move(parameter));
	}

	chain = std::move(parsedChain);
	error.clear();
	return true;
}

bool shouldComposeDisplayInput(const ShaderFrameInfo& frame, bool wantsUnscaledInput)
{
	return !wantsUnscaledInput && frame.viewportWidth > 0 && frame.viewportHeight > 0
			&& (frame.rotationPending || frame.sourceWidth != frame.viewportWidth
					|| frame.sourceHeight != frame.viewportHeight);
}

bool parseSlangPreset(const std::string& presetText, const std::string& presetPath,
		ShaderChain& chain, std::string& error)
{
	return parsePreset(presetText, presetPath, ShaderFormat::Slang, "Slang preset", ".slang", chain, error);
}

bool parseGlslPreset(const std::string& presetText, const std::string& presetPath,
		ShaderChain& chain, std::string& error)
{
	return parsePreset(presetText, presetPath, ShaderFormat::GLSL, "GLSL preset", ".glsl", chain, error);
}

bool parseGlslShader(const std::string& shaderText, ShaderPassSource& source, std::string& error)
{
	if (shaderText.empty() || shaderText.find('\0') != std::string::npos)
		return fail(error, "GLSL source is empty or contains NUL bytes.");

	ShaderPassSource parsed;
	std::string cleaned;
	std::istringstream lines(shaderText);
	const std::regex parameterDirective(R"(^\s*#\s*pragma\s+parameter\b)", std::regex::icase);
	for (std::string line; std::getline(lines, line);) {
		// std::getline removes LF but retains CR. Normalize here so line-based
		// interface stripping works identically for upstream Windows shader packs.
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (std::regex_search(line, parameterDirective)) {
			std::istringstream declaration(line);
			std::string pragma;
			std::string keyword;
			ShaderParameter parameter;
			if (!(declaration >> pragma >> keyword >> parameter.name >> std::quoted(parameter.label)
					>> parameter.value >> parameter.minimum >> parameter.maximum >> parameter.step)
					|| pragma != "#pragma" || keyword != "parameter" || !isValidIdentifier(parameter.name)
					|| !std::isfinite(parameter.value) || !std::isfinite(parameter.minimum)
					|| !std::isfinite(parameter.maximum) || !std::isfinite(parameter.step)
					|| parameter.minimum > parameter.maximum || parameter.value < parameter.minimum
					|| parameter.value > parameter.maximum || parameter.step <= 0.f)
				return fail(error, "Malformed RetroArch GLSL #pragma parameter declaration.");
			declaration >> std::ws;
			if (!declaration.eof())
				return fail(error, "Malformed RetroArch GLSL #pragma parameter declaration.");
			if (std::any_of(parsed.parameters.begin(), parsed.parameters.end(), [&](const ShaderParameter& value) {
				return value.name == parameter.name;
			}))
				return fail(error, "Duplicate GLSL parameter '" + parameter.name + "'.");
			parameter.values[0] = parameter.value;
			parameter.minimumValues[0] = parameter.minimum;
			parameter.maximumValues[0] = parameter.maximum;
			parameter.stepValues[0] = parameter.step;
			parsed.parameters.push_back(std::move(parameter));
			continue;
		}
		if (std::regex_match(line, std::regex(R"(^\s*#\s*version\b.*$)", std::regex::icase)))
			continue;
		cleaned += line + '\n';
	}

	// Some official multi-pass GLSL packs declare a shared parameter in one
	// pass and only its uniform/default pair in later passes. Reflect the latter
	// as a read-only default so raw-stage selection also remains compilable.
	const std::regex scalarUniform(
			R"(^\s*uniform\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?float\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)",
			std::regex::icase);
	const std::regex scalarDefault(
			R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)\s*(?://.*)?$)");
	std::unordered_set<std::string> uniformNames;
	std::unordered_map<std::string, float> defaultValues;
	std::istringstream metadataLines(cleaned);
	for (std::string line; std::getline(metadataLines, line);) {
		std::smatch match;
		if (std::regex_match(line, match, scalarUniform))
			uniformNames.insert(match[1].str());
		else if (std::regex_match(line, match, scalarDefault)) {
			try {
				const float value = std::stof(match[2].str());
				if (std::isfinite(value))
					defaultValues.emplace(match[1].str(), value);
			} catch (...) {
				// This is optional compatibility metadata; the normal preprocessor
				// remains responsible for diagnosing malformed active directives.
			}
		}
	}
	for (const std::string& name : uniformNames) {
		const auto defaultValue = defaultValues.find(name);
		if (defaultValue == defaultValues.end() || std::any_of(parsed.parameters.begin(), parsed.parameters.end(),
				[&](const ShaderParameter& parameter) { return parameter.name == name; }))
			continue;
		ShaderParameter parameter;
		parameter.name = name;
		parameter.label = name;
		parameter.value = parameter.minimum = parameter.maximum = defaultValue->second;
		parameter.values[0] = parameter.minimumValues[0] = parameter.maximumValues[0] = defaultValue->second;
		parsed.parameters.push_back(std::move(parameter));
	}

	const bool hasVertexStage = std::regex_search(cleaned,
			std::regex(R"(#\s*(?:if|elif)\s+defined\s*\(\s*VERTEX\s*\)|#\s*ifdef\s+VERTEX\b)"));
	const bool hasFragmentStage = std::regex_search(cleaned,
			std::regex(R"(#\s*(?:if|elif)\s+defined\s*\(\s*FRAGMENT\s*\)|#\s*ifdef\s+FRAGMENT\b)"));
	const bool retroArchContract = hasVertexStage || hasFragmentStage
			|| std::regex_search(cleaned, std::regex(R"(\b(?:TextureSize|InputSize|FrameDirection|MVPMatrix|TEX0)\b)"));

	auto preprocessStage = [&](const char *stage, std::string& result) {
		return preprocessShaderSource(cleaned,
				{ { stage, "1" }, { "PARAMETER_UNIFORM", "1" }, { "__VERSION__", "130" }, { "GL_core_profile", "1" } },
				result, error);
	};
	if (hasVertexStage && !preprocessStage("VERTEX", parsed.vertex))
		return false;
	if ((hasVertexStage || hasFragmentStage) ? !preprocessStage("FRAGMENT", parsed.fragment)
			: (parsed.fragment = cleaned, false))
		return false;
	if (parsed.fragment.empty())
		return fail(error, "GLSL shader has no fragment stage.");

	if (retroArchContract) {
		const std::regex resourceDeclaration(
				R"(^\s*uniform\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?(?:sampler2D|vec2|int|mat4)\s+(?:Texture|InputSize|OrigInputSize|TextureSize|OutputSize|FrameDirection|FrameCount|Rotation|MVPMatrix)\s*;\s*$)",
				std::regex::icase);
		const std::regex interfaceDeclaration(
				R"(^\s*(?:layout\s*\([^)]*\)\s*)?(?:in|out|attribute|varying|COMPAT_ATTRIBUTE|COMPAT_VARYING)\s+(?:(?:lowp|mediump|highp|[A-Z][A-Z0-9_]*)\s+)?(?:vec2|vec3|vec4)\s+(?:VertexCoord|TexCoord|TEX0|vTexCoord|FragColor|COLOR|COL0)\s*;\s*$)",
				std::regex::icase);
		const std::regex compatibilityDefine(
				R"(^\s*#\s*define\s+(?:COMPAT_PRECISION|COMPAT_ATTRIBUTE|COMPAT_VARYING|COMPAT_TEXTURE|Source|SourceSize|OutSize|OutputSize|vTexCoord)\b.*$)",
				std::regex::icase);
		const std::regex sourceSizeDefine(R"(^\s*#\s*define\s+SourceSize\b.*$)", std::regex::icase);
		const std::regex outputSizeDefine(R"(^\s*#\s*define\s+(?:OutSize|OutputSize)\b.*$)", std::regex::icase);
		auto normalize = [&](std::string& stageSource, bool vertex) {
			std::string normalized;
			std::istringstream stageLines(stageSource);
			bool hasSourceSizeAlias = false;
			bool hasOutputSizeAlias = false;
			for (std::string line; std::getline(stageLines, line);) {
				hasSourceSizeAlias = hasSourceSizeAlias || std::regex_match(line, sourceSizeDefine);
				hasOutputSizeAlias = hasOutputSizeAlias || std::regex_match(line, outputSizeDefine);
				if (!std::regex_match(line, resourceDeclaration) && !std::regex_match(line, interfaceDeclaration)
						&& !std::regex_match(line, compatibilityDefine))
					normalized += line + '\n';
			}
			stageSource = std::move(normalized);
			if (hasSourceSizeAlias)
				replaceAll(stageSource, std::regex(R"(\bSourceSize\b)", std::regex::icase), "HollycastSourceSize4");
			if (hasOutputSizeAlias)
				replaceAll(stageSource, std::regex(R"(\bOutSize\b)", std::regex::icase),
						"HollycastOutputSize4");
			replaceAll(stageSource, std::regex(R"(\bCOMPAT_TEXTURE\b)"), "texture");
			replaceAll(stageSource, std::regex(R"(\bTexture\b)"), "Source");
			replaceAll(stageSource, std::regex(R"(\bOrigInputSize\b)"), "OriginalSize");
			replaceAll(stageSource, std::regex(R"(\b(?:InputSize|TextureSize)\b)"), "SourceSize");
			replaceAll(stageSource, std::regex(R"(\bFrameDirection\b)"), "(1)");
			if (vertex) {
				// COLOR/COL0 is legacy libretro fixed-function boilerplate. HollyCast's
				// display quad has no color stream, and the imported shaders do not read
				// COL0 in their fragment stages, so discard the dead passthrough safely.
				replaceAll(stageSource, std::regex(R"(\bCOL0\s*=\s*COLOR\s*;)"), "");
				replaceAll(stageSource, std::regex(R"(\bMVPMatrix\s*\*\s*VertexCoord\b)"), "VertexCoord");
				// HollyCast supplies clip-space quad coordinates directly. Preserve any
				// shader-side position adjustment while removing libretro's host MVP.
				replaceAll(stageSource, std::regex(R"(\bMVPMatrix\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\b)"), "$1");
				replaceAll(stageSource, std::regex(
						R"(\bVertexCoord\.x\s*\*\s*MVPMatrix\s*\[\s*0\s*\]\s*\+\s*VertexCoord\.y\s*\*\s*MVPMatrix\s*\[\s*1\s*\]\s*\+\s*VertexCoord\.z\s*\*\s*MVPMatrix\s*\[\s*2\s*\]\s*\+\s*VertexCoord\.w\s*\*\s*MVPMatrix\s*\[\s*3\s*\])"),
						"VertexCoord");
				replaceAll(stageSource, std::regex(R"(\bTEX0\b)"), "vTexCoord");
			}
			else {
				replaceAll(stageSource, std::regex(R"(\bTEX0\s*\.\s*xy\b)"), "vTexCoord");
				replaceAll(stageSource, std::regex(R"(\bTEX0\b)"), "vec4(vTexCoord, 0.0, 1.0)");
			}
			if (hasOutputSizeAlias)
				stageSource.insert(0, "#define HollycastOutputSize4 vec4(OutputSize, 1.0 / OutputSize)\n");
			if (hasSourceSizeAlias)
				stageSource.insert(0, "#define HollycastSourceSize4 vec4(SourceSize, 1.0 / SourceSize)\n");
			removePortableInterfaceDeclarations(stageSource, parsed.parameters);
		};
		if (!parsed.vertex.empty())
			normalize(parsed.vertex, true);
		normalize(parsed.fragment, false);
		const std::regex samplerDeclaration(
				R"(^\s*uniform\s+(?:(?:lowp|mediump|highp)\s+)?sampler2D\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)",
				std::regex::icase);
		auto reflectSamplers = [&](std::string& stageSource) {
			std::string normalized;
			std::istringstream lines(stageSource);
			for (std::string line; std::getline(lines, line);) {
				std::smatch declaration;
				if (!std::regex_match(line, declaration, samplerDeclaration)) {
					normalized += line + '\n';
					continue;
				}
				const std::string name = declaration[1].str();
				if (name == "Source" || std::any_of(parsed.slangResources.begin(), parsed.slangResources.end(),
						[&](const SlangResource& resource) { return resource.name == name; }))
					continue;
				SlangResource resource;
				resource.name = name;
				resource.alias = name;
				parsed.slangResources.push_back(std::move(resource));
			}
			stageSource = std::move(normalized);
		};
		reflectSamplers(parsed.vertex);
		reflectSamplers(parsed.fragment);
		parsed.retroArchGlsl = true;
	}
	else
		removePortableInterfaceDeclarations(parsed.fragment, parsed.parameters);

	source = std::move(parsed);
	error.clear();
	return true;
}

bool parseSlangShader(const std::string& shaderText, SlangShaderSource& shader, std::string& error)
{
	SlangShaderIR parsed;
	if (!parseSlangFrontend(shaderText, parsed, error))
		return false;
	SlangShaderSource projected;
	projected.vertex = std::move(parsed.vertex.source);
	projected.fragment = std::move(parsed.fragment.source);
	for (const SlangParameter& parameter : parsed.parameters) {
		projected.parameters.push_back(projectParameter(parameter));
	}
	shader = std::move(projected);
	error.clear();
	return true;
}

bool loadSlangPreset(const std::string& presetPath, ShaderChain& chain, std::string& error)
{
	if (detectFormat(presetPath) != ShaderFormat::SlangPreset)
		return fail(error, "Expected a .slangp preset.");

	std::string presetText;
	if (!readTextFile(presetPath, presetText, error))
		return false;
	return parseSlangPreset(presetText, presetPath, chain, error);
}

bool loadGlslPreset(const std::string& presetPath, ShaderChain& chain, std::string& error)
{
	if (detectFormat(presetPath) != ShaderFormat::GlslPreset)
		return fail(error, "Expected a .glslp preset.");

	std::string presetText;
	if (!readTextFile(presetPath, presetText, error))
		return false;
	return parseGlslPreset(presetText, presetPath, chain, error);
}

bool parseReShadeFx(const std::string& effectText, ShaderPassSource& source, std::string& error)
{
	ReShadeEffect effect;
	if (!parseReShadeEffect(effectText, effect, error))
		return false;
	if (!effect.selectedTechnique || !effect.selectedPass)
		return fail(error, "ReShade FX must declare a pass PixelShader entry point.");
	return compileReShadeFxPass(effectText, *effect.selectedTechnique, *effect.selectedPass, source, error);
}

bool loadShaderPassSource(const ShaderPass& pass, ShaderPassSource& source, std::string& error)
{
	source = {};
	std::string text;
	switch (pass.format) {
	case ShaderFormat::GLSL: {
		std::unordered_set<std::string> includeStack;
		const std::string root = pass.packageRoot.empty() ? parentPath(pass.sourcePath) : pass.packageRoot;
		if (!expandPackageIncludes(pass.sourcePath, root, 0, includeStack, text, error))
			return false;
		return parseGlslShader(text, source, error);
	}
	case ShaderFormat::Slang: {
		std::unordered_set<std::string> includeStack;
		const std::string root = pass.packageRoot.empty() ? parentPath(pass.sourcePath) : pass.packageRoot;
		if (!expandPackageIncludes(pass.sourcePath, root, 0, includeStack, text, error))
			return false;
		SlangShaderIR slang;
		if (!parseSlangFrontend(text, slang, error))
			return false;
		source.vertex = std::move(slang.vertex.source);
		source.fragment = std::move(slang.fragment.source);
		source.displayName = std::move(slang.displayName);
		source.targetFormatName = std::move(slang.format);
		source.slangParameters = std::move(slang.parameters);
		source.slangResources = std::move(slang.resources);
		for (const SlangParameter& parameter : source.slangParameters) {
			source.parameters.push_back(projectParameter(parameter));
		}
		removePortableInterfaceDeclarations(source.fragment, source.parameters);
		return true;
	}
	case ShaderFormat::ReShadeFX:
		{
			std::unordered_set<std::string> includeStack;
			const std::string root = pass.packageRoot.empty() ? parentPath(pass.sourcePath) : pass.packageRoot;
			if (!expandPackageIncludes(pass.sourcePath, root, 0, includeStack, text, error))
				return false;
		}
		if (text.empty())
			return fail(error, "ReShade FX source is empty.");
		if (pass.effectTechnique != InvalidShaderPass && pass.effectPass != InvalidShaderPass)
			return compileReShadeFxPass(text, pass.effectTechnique, pass.effectPass, source, error);
		return parseReShadeFx(text, source, error);
	default:
		return fail(error, "Unsupported shader pass format.");
	}
}

bool loadShaderChain(const std::string& path, ShaderChain& chain, std::string& error)
{
	chain = {};
	const ShaderFormat format = detectFormat(path);
	if (format == ShaderFormat::SlangPreset || format == ShaderFormat::GlslPreset) {
		if (!(format == ShaderFormat::SlangPreset ? loadSlangPreset(path, chain, error)
				: loadGlslPreset(path, chain, error)))
			return false;
	}
	else if (format == ShaderFormat::ReShadeFX) {
		std::string text;
		std::unordered_set<std::string> includeStack;
		const std::string packageRoot = parentPath(path);
		if (!expandPackageIncludes(path, packageRoot, 0, includeStack, text, error))
			return false;
		ReShadeEffect effect;
		if (!parseReShadeEffect(text, effect, error) || !effect.selectedTechnique)
			return false;
		ShaderChain parsed;
		parsed.packagePath = path;
		for (size_t index = 0; index < effect.techniques.size(); ++index)
			parsed.techniques.push_back(effect.techniques[index].name.empty()
					? "Technique" + std::to_string(index) : effect.techniques[index].name);
		size_t selectedTechnique = *effect.selectedTechnique;
		const std::string& requestedTechnique = config::PostProcessShaderTechnique.get();
		if (!requestedTechnique.empty()) {
			auto selected = std::find(parsed.techniques.begin(), parsed.techniques.end(), requestedTechnique);
			if (selected == parsed.techniques.end())
				return fail(error, "ReShade FX technique '" + requestedTechnique + "' was not found.");
			selectedTechnique = static_cast<size_t>(std::distance(parsed.techniques.begin(), selected));
		}
		parsed.selectedTechnique = static_cast<int>(selectedTechnique);
		const ReShadeTechnique& technique = effect.techniques[selectedTechnique];
		for (size_t passIndex = 0; passIndex < technique.passes.size(); ++passIndex) {
			const ReShadePass& effectPass = technique.passes[passIndex];
			if (effectPass.pixelShader.empty())
				continue;
			ShaderPass pass;
			pass.sourcePath = path;
			pass.packageRoot = packageRoot;
			pass.format = format;
			pass.effectTechnique = static_cast<uint32_t>(selectedTechnique);
			pass.effectPass = static_cast<uint32_t>(passIndex);
			const std::regex renderTarget(R"(\bRenderTarget([0-9]*)\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*;)", std::regex::icase);
			for (std::sregex_iterator it(effectPass.stateBlock.begin(), effectPass.stateBlock.end(), renderTarget), end; it != end; ++it) {
				const std::string indexText = (*it)[1].str();
				uint32_t output = 0;
				if (!indexText.empty()) {
					try {
						const unsigned long parsed = std::stoul(indexText);
						if (parsed >= 8)
							return fail(error, "ReShade FX supports at most eight color render targets per pass.");
						output = static_cast<uint32_t>(parsed);
					} catch (...) {
						return fail(error, "ReShade FX pass has an invalid RenderTarget index.");
					}
				}
				if (pass.outputs.size() <= output)
					pass.outputs.resize(output + 1);
				if (!pass.outputs[output].alias.empty())
					return fail(error, "ReShade FX pass assigns the same RenderTarget index more than once.");
				pass.outputs[output].alias = (*it)[2].str();
			}
			if (!pass.outputs.empty()) {
				if (pass.outputs.front().alias.empty())
					return fail(error, "ReShade FX MRT passes must define RenderTarget0.");
				for (ShaderPassOutput& output : pass.outputs)
					if (!configureReShadeOutputFormat(effect, output, error))
						return false;
				pass.alias = pass.outputs.front().alias;
			}
			else
				pass.alias = effectPass.name.empty() ? "FxPass" + std::to_string(passIndex) : effectPass.name;
			if (!parseReShadePassState(effectPass, pass, error))
				return false;
			parsed.passes.push_back(std::move(pass));
		}
		if (parsed.passes.empty())
			return fail(error, "ReShade FX technique has no runnable PixelShader passes.");
		std::unordered_set<std::string> outputAliases;
		for (const ShaderPass& pass : parsed.passes) {
			if (pass.outputs.empty()) {
				if (!outputAliases.emplace(pass.alias).second)
					return fail(error, "ReShade FX contains duplicate pass output alias '" + pass.alias + "'.");
				continue;
			}
			for (const ShaderPassOutput& output : pass.outputs)
				if (output.alias.empty() || !outputAliases.emplace(output.alias).second)
					return fail(error, "ReShade FX contains an empty or duplicate render target alias.");
		}
		for (const ReShadeTexture& texture : effect.textures) {
			const auto annotation = std::find_if(texture.annotations.begin(), texture.annotations.end(), [](const ReShadeAnnotation& value) {
				return value.name == "source";
			});
			if (annotation == texture.annotations.end() || annotation->value == "color")
				continue;
			if (!isSafeRelativePath(annotation->value))
				return fail(error, "ReShade FX lookup texture paths must remain inside the effect package.");
			ShaderPresetTexture lookup;
			lookup.name = texture.name;
			lookup.path = hostfs::storage().getSubPath(packageRoot, annotation->value);
			const auto sampler = std::find_if(effect.samplers.begin(), effect.samplers.end(), [&](const ReShadeSampler& value) {
				return value.definition.find("Texture = " + texture.name) != std::string::npos;
			});
			if (sampler != effect.samplers.end()) {
				lookup.linearFilter = std::regex_search(sampler->definition, std::regex(R"(\bFilter\s*=\s*[^;]*LINEAR)", std::regex::icase));
				lookup.useMipmaps = std::regex_search(sampler->definition, std::regex(R"(\bMipFilter\s*=\s*LINEAR)", std::regex::icase));
				if (std::regex_search(sampler->definition, std::regex(R"(\bAddress[UVW]\s*=\s*WRAP)", std::regex::icase)))
					lookup.wrapMode = ShaderWrapMode::Repeat;
				else if (std::regex_search(sampler->definition, std::regex(R"(\bAddress[UVW]\s*=\s*MIRROR)", std::regex::icase)))
					lookup.wrapMode = ShaderWrapMode::MirroredRepeat;
			}
			parsed.textures.push_back(std::move(lookup));
		}
		chain = std::move(parsed);
	}
	else if (format == ShaderFormat::GLSL || format == ShaderFormat::Slang) {
		ShaderChain parsed;
		parsed.packagePath = path;
		ShaderPass& pass = parsed.passes.emplace_back();
		pass.sourcePath = path;
		pass.packageRoot = parentPath(path);
		pass.format = format;
		chain = std::move(parsed);
	}
	else {
		return fail(error, "Select a .glsl, .glslp, .slang, .slangp, or .fx shader file.");
	}
	chain.wantsUnscaledInput = format == ShaderFormat::GlslPreset
			|| format == ShaderFormat::Slang || format == ShaderFormat::SlangPreset;

	chain.sources.clear();
	chain.sources.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		ShaderPass& pass = chain.passes[passIndex];
		ShaderPassSource passSource;
		if (!loadShaderPassSource(pass, passSource, error))
			return false;
		if (pass.alias.empty() && !passSource.displayName.empty()) {
			if (!isValidIdentifier(passSource.displayName))
				return fail(error, "Slang #pragma name must be a valid pass alias.");
			if (std::any_of(chain.passes.begin(), chain.passes.begin() + passIndex,
					[&](const ShaderPass& candidate) { return candidate.alias == passSource.displayName; }))
				return fail(error, "Slang #pragma name duplicates pass alias '" + passSource.displayName + "'.");
			pass.alias = passSource.displayName;
		}
		if (!passSource.targetFormatName.empty()) {
			if (!pass.targetFormatName.empty() && pass.targetFormatName != passSource.targetFormatName)
				return fail(error, "Slang preset target format conflicts with the shader #pragma format declaration.");
			pass.targetFormatName = passSource.targetFormatName;
		}
		for (ShaderPassOutput& output : pass.outputs) {
			if (output.targetFormat == ShaderTargetFormat::Default && output.targetFormatName.empty())
				output.targetFormat = pass.targetFormat;
			if (output.targetFormatName.empty())
				output.targetFormatName = pass.targetFormatName;
			output.useMipmaps = output.useMipmaps || pass.useMipmaps;
		}
		if (pass.outputs.empty()) {
			if (passSource.outputCount != 1)
				return fail(error, "A multi-output shader pass must declare RenderTarget0 through RenderTargetN.");
		}
		else if (pass.outputs.size() != passSource.outputCount) {
			return fail(error, "ReShade FX PixelShader outputs do not match its RenderTarget state block.");
		}
		if (!resolveShaderPassInputs(chain, passIndex, passSource, error))
			return false;
		for (const ShaderParameter& reflected : passSource.parameters)
			if (!mergeShaderParameter(chain, reflected, error))
				return false;
		chain.sources.push_back(std::move(passSource));
	}
	const uint32_t parameterComponents = std::accumulate(chain.parameters.begin(), chain.parameters.end(), 0u,
			[](uint32_t count, const ShaderParameter& parameter) { return count + parameter.componentCount; });
	if (parameterComponents > MaxShaderParameterComponents)
		return fail(error, "Shader chains may expose at most 24 parameter components.");
	for (const ShaderParameter& parameter : chain.parameters)
		if (!isValidIdentifier(parameter.name))
			return fail(error, "Shader parameter names must be valid identifiers.");
	error.clear();
	return true;
}

bool loadShaderChain(const std::vector<std::string>& paths, ShaderChain& chain, std::string& error)
{
	chain = {};
	if (paths.empty())
		return fail(error, "Select at least one shader layer.");
	if (paths.size() > MaxShaderLayers)
		return fail(error, "Shader stacks may contain at most " + std::to_string(MaxShaderLayers) + " selected layers.");
	if (paths.size() == 1)
		return loadShaderChain(paths.front(), chain, error);

	std::unordered_set<std::string> selectedPaths;
	chain.packagePath = shaderSelectionLabel(paths);
	for (size_t layerIndex = 0; layerIndex < paths.size(); ++layerIndex) {
		const std::string& path = paths[layerIndex];
		if (path.empty() || !selectedPaths.emplace(path).second)
			return fail(error, "Shader stacks cannot contain empty or duplicate layers.");

		ShaderChain layer;
		std::string layerError;
		if (!loadShaderChain(path, layer, layerError))
			return fail(error, "Shader layer " + std::to_string(layerIndex + 1) + " failed: " + layerError);
		const uint32_t passOffset = static_cast<uint32_t>(chain.passes.size());
		if (chain.passes.size() + layer.passes.size() > static_cast<size_t>(MaxPassCount))
			return fail(error, "Combined shader stacks may contain at most " + std::to_string(MaxPassCount) + " passes.");

		for (ShaderPass& pass : layer.passes)
			for (ShaderPassInput& input : pass.inputs)
				if (input.sourcePass != InvalidShaderPass)
					input.sourcePass += passOffset;
		if (chain.feedbackPass == InvalidShaderPass && layer.feedbackPass != InvalidShaderPass)
			chain.feedbackPass = layer.feedbackPass + passOffset;

		for (const ShaderPresetTexture& texture : layer.textures) {
			const auto existing = std::find_if(chain.textures.begin(), chain.textures.end(), [&](const ShaderPresetTexture& value) {
				return value.name == texture.name;
			});
			if (existing == chain.textures.end())
				chain.textures.push_back(texture);
			else if (existing->path != texture.path || existing->linearFilter != texture.linearFilter
					|| existing->useMipmaps != texture.useMipmaps || existing->wrapMode != texture.wrapMode)
				return fail(error, "Shader layers define incompatible lookup textures named '" + texture.name + "'.");
		}
		for (const ShaderParameter& parameter : layer.parameters)
			if (!mergeShaderParameter(chain, parameter, error))
				return false;

		chain.wantsUnscaledInput = chain.wantsUnscaledInput || layer.wantsUnscaledInput;
		chain.passes.insert(chain.passes.end(), std::make_move_iterator(layer.passes.begin()),
				std::make_move_iterator(layer.passes.end()));
		chain.sources.insert(chain.sources.end(), std::make_move_iterator(layer.sources.begin()),
				std::make_move_iterator(layer.sources.end()));
	}

	const uint32_t parameterComponents = std::accumulate(chain.parameters.begin(), chain.parameters.end(), 0u,
			[](uint32_t count, const ShaderParameter& parameter) { return count + parameter.componentCount; });
	if (parameterComponents > MaxShaderParameterComponents)
		return fail(error, "Combined shader layers may expose at most 24 parameter components.");
	error.clear();
	return true;
}

} // namespace postprocess
