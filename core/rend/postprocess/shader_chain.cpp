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
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace postprocess {
namespace {

constexpr int MaxPassCount = 32;
constexpr size_t MaxParameterCount = 16;

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
std::atomic<uint64_t> reloadGeneration { 1 };

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
	std::istringstream stream(value);
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
		switch (resource.convention) {
		case SlangResourceConvention::Source:
			input.kind = ShaderInputKind::Source;
			break;
		case SlangResourceConvention::Original:
			input.kind = ShaderInputKind::Original;
			break;
		case SlangResourceConvention::PassAlias:
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
		case SlangResourceConvention::History:
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
			if (!findPassOutput(chain, resource.alias, passIndex, input.sourcePass, input.sourceOutput))
				return fail(error, "Slang feedback resource '" + resource.name + "' has no matching pass alias.");
			if (chain.feedbackPass != input.sourcePass)
				return fail(error, "Slang feedback resource '" + resource.name + "' does not match the preset feedback_pass target.");
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
	auto cachePath = [&](void) {
		std::ostringstream name;
		name << cacheDirectory << std::hex << hash(path, info.size, info.updateTime) << ".src";
		return name.str();
	};
	if (!cacheDirectory.empty() && info.size != 0) {
		make_directory(cacheDirectory);
		std::unique_ptr<hostfs::File> cache(hostfs::storage().openFile(cachePath(), "rb"));
		if (cache != nullptr) {
			std::string cached(static_cast<size_t>(cache->size()), '\0');
			if (cached.size() >= cacheHeader.size() && cache->read(cached.data(), 1, cached.size()) == cached.size()
					&& cached.rfind(cacheHeader, 0) == 0) {
				text = cached.substr(cacheHeader.size());
				return true;
			}
		}
	}
	std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(path, "rt"));
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
	return true;
}

bool expandSlangIncludes(const std::string& path, const std::string& packageRoot, int depth,
		std::unordered_set<std::string>& includeStack, std::string& expanded, std::string& error)
{
	if (depth > 16)
		return fail(error, "Slang include nesting exceeds 16 files.");
	if (!includeStack.emplace(path).second)
		return fail(error, "Slang include cycle detected: " + path);

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
			return fail(error, "Slang includes must use a package-relative quoted path.");
		}
		const std::string includePath = hostfs::storage().getSubPath(parentPath(path), match[1].str());
		// Paths containing '..' are rejected before joining, which is portable
		// across storage backends that do not canonicalize virtual paths.
		if (includePath.rfind(packageRoot, 0) != 0) {
			includeStack.erase(path);
			return fail(error, "Slang include escapes its package root.");
		}
		if (!expandSlangIncludes(includePath, packageRoot, depth + 1, includeStack, expanded, error)) {
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
	const std::regex interfaceDeclaration(R"(^\s*(layout\s*\([^\n]*\)\s*)?(in|out)\s+[^;\n]*\b(vTexCoord|FragColor)\s*;\s*$)", std::regex::icase);
	const std::regex frameDeclaration(R"(^\s*uniform\s+(sampler2D|vec2|int)\s+(Source|SourceSize|OutputSize|FrameCount)\s*;\s*$)", std::regex::icase);
	std::string cleaned;
	std::istringstream stream(source);
	bool firstLine = true;
	for (std::string line; std::getline(stream, line);) {
		bool remove = (firstLine && startsWith(line, "#version"))
				|| std::regex_match(line, interfaceDeclaration) || std::regex_match(line, frameDeclaration);
		for (const ShaderParameter& parameter : parameters) {
			const std::regex declaration("^\\s*uniform\\s+(float|int|bool)\\s+" + parameter.name + R"(\s*;\s*$)", std::regex::icase);
			remove = remove || std::regex_match(line, declaration);
		}
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

} // namespace

ShaderFormat detectFormat(const std::string& path)
{
	const std::string extension = get_file_extension(path);
	if (extension == "glsl")
		return ShaderFormat::GLSL;
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

bool resolveTargetSize(const ShaderPass& pass, const ShaderFrameInfo& frame,
		int& width, int& height, std::string& error)
{
	if (!resolveDimension(pass.scaleTypeX, pass.scaleX, frame.sourceWidth, frame.viewportWidth, width, error)
			|| !resolveDimension(pass.scaleTypeY, pass.scaleY, frame.sourceHeight, frame.viewportHeight, height, error))
		return false;
	error.clear();
	return true;
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
}

bool setShaderChainParameter(const std::string& name, float value)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	auto parameter = std::find_if(status.parameters.begin(), status.parameters.end(), [&](const ShaderParameter& candidate) {
		return candidate.name == name;
	});
	if (parameter == status.parameters.end() || value < parameter->minimum || value > parameter->maximum)
		return false;
	parameter->value = value;
	parameter->values[0] = value;
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
	for (uint32_t index = 0; index < componentCount; ++index)
	{
		const float minimum = parameter->componentCount == 1 ? parameter->minimum : parameter->minimumValues[index];
		const float maximum = parameter->componentCount == 1 ? parameter->maximum : parameter->maximumValues[index];
		if (!std::isfinite(values[index]) || values[index] < minimum || values[index] > maximum)
			return false;
	}
	std::copy_n(values, componentCount, parameter->values.begin());
	parameter->value = parameter->values[0];
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
		bool valid = true;
		for (uint32_t index = 0; index < parameter->componentCount; ++index) {
			const float minimum = parameter->componentCount == 1 ? parameter->minimum : parameter->minimumValues[index];
			const float maximum = parameter->componentCount == 1 ? parameter->maximum : parameter->maximumValues[index];
			valid = valid && parsedValues[index] >= minimum && parsedValues[index] <= maximum;
		}
		if (!valid)
			continue;
		std::copy(parsedValues.begin(), parsedValues.end(), parameter->values.begin());
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

bool parseSlangPreset(const std::string& presetText, const std::string& presetPath,
		ShaderChain& chain, std::string& error)
{
	config::IniFile preset;
	preset.load(presetText);
	const int passCount = preset.getInt("", "shaders", -1);
	if (passCount < 1 || passCount > MaxPassCount)
		return fail(error, "Slang preset must declare between 1 and 32 shader passes.");

	const std::string basePath = parentPath(presetPath);
	if (basePath.empty())
		return fail(error, "Unable to resolve the Slang preset folder.");

	ShaderChain parsedChain;
	parsedChain.packagePath = presetPath;
	parsedChain.passes.reserve(passCount);
	for (int index = 0; index < passCount; ++index) {
		const std::string suffix = std::to_string(index);
		const std::string& source = preset.getString("", "shader" + suffix);
		if (source.empty())
			return fail(error, "Slang preset is missing shader" + suffix + ".");
		if (!isSafeRelativePath(source))
			return fail(error, "Slang preset shader paths must be relative to the preset.");

		ShaderPass pass;
		pass.sourcePath = hostfs::storage().getSubPath(basePath, source);
		pass.packageRoot = basePath;
		pass.format = detectFormat(pass.sourcePath);
		if (pass.format != ShaderFormat::Slang)
			return fail(error, "Slang preset pass " + suffix + " must reference a .slang shader.");
		pass.alias = preset.getString("", "alias" + suffix);
		pass.linearFilter = preset.getBool("", "filter_linear" + suffix);
		pass.useMipmaps = preset.getBool("", "mipmap_input" + suffix);
		pass.frameCountModulo = static_cast<uint32_t>(std::max(0, preset.getInt("", "frame_count_mod" + suffix)));
		pass.scaleX = preset.getFloat("", "scale_x" + suffix, 1.f);
		pass.scaleY = preset.getFloat("", "scale_y" + suffix, pass.scaleX);
		const bool floatTarget = preset.getBool("", "float_framebuffer" + suffix);
		const bool srgbTarget = preset.getBool("", "srgb_framebuffer" + suffix);
		if (floatTarget && srgbTarget)
			return fail(error, "Slang preset pass " + suffix + " cannot request both float and sRGB framebuffers.");
		pass.targetFormat = floatTarget ? ShaderTargetFormat::Float
				: srgbTarget ? ShaderTargetFormat::Srgb : ShaderTargetFormat::Default;
		pass.targetFormatName = preset.getString("", "format" + suffix);
		if (!pass.targetFormatName.empty() && !isValidIdentifier(pass.targetFormatName))
			return fail(error, "Slang preset pass " + suffix + " has an invalid target format name.");

		if (pass.scaleX <= 0.f || pass.scaleY <= 0.f)
			return fail(error, "Slang preset scale values must be positive.");
		if (!parseScaleType(preset.getString("", "scale_type_x" + suffix,
				preset.getString("", "scale_type" + suffix)), pass.scaleTypeX)
				|| !parseScaleType(preset.getString("", "scale_type_y" + suffix,
				preset.getString("", "scale_type" + suffix)), pass.scaleTypeY))
			return fail(error, "Slang preset contains an unsupported scale type.");

		parsedChain.passes.push_back(std::move(pass));
	}

	std::unordered_set<std::string> aliases;
	for (const ShaderPass& pass : parsedChain.passes) {
		if (pass.alias.empty())
			continue;
		if (!isValidIdentifier(pass.alias))
			return fail(error, "Slang preset pass aliases must be valid identifiers.");
		if (!aliases.emplace(pass.alias).second)
			return fail(error, "Slang preset contains duplicate pass alias '" + pass.alias + "'.");
	}

	const int feedbackPass = preset.getInt("", "feedback_pass", -1);
	if (feedbackPass < -1 || feedbackPass >= passCount)
		return fail(error, "Slang preset feedback_pass must identify a declared shader pass.");
	if (feedbackPass >= 0) {
		parsedChain.feedbackPass = static_cast<uint32_t>(feedbackPass);
		parsedChain.passes[parsedChain.feedbackPass].feedbackTarget = true;
	}

	std::unordered_set<std::string> textureNames;
	for (const std::string& name : splitParameters(preset.getString("", "textures"))) {
		if (!isValidIdentifier(name))
			return fail(error, "Slang preset texture names must be valid identifiers.");
		if (!textureNames.emplace(name).second)
			return fail(error, "Slang preset contains duplicate texture '" + name + "'.");
		const std::string& texturePath = preset.getString("", name);
		if (!isSafeRelativePath(texturePath))
			return fail(error, "Slang preset texture paths must be relative to the preset.");
		ShaderPresetTexture texture;
		texture.name = name;
		texture.path = hostfs::storage().getSubPath(basePath, texturePath);
		texture.linearFilter = preset.getBool("", name + "_linear");
		texture.useMipmaps = preset.getBool("", name + "_mipmap");
		if (!parseWrapMode(preset.getString("", name + "_wrap_mode"), texture.wrapMode))
			return fail(error, "Slang preset texture '" + name + "' has an unsupported wrap mode.");
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
	case ShaderFormat::GLSL:
		if (!readTextFile(pass.sourcePath, text, error))
			return false;
		source.fragment = std::move(text);
		source.parameters.clear();
		return true;
	case ShaderFormat::Slang: {
		std::unordered_set<std::string> includeStack;
		const std::string root = pass.packageRoot.empty() ? parentPath(pass.sourcePath) : pass.packageRoot;
		if (!expandSlangIncludes(pass.sourcePath, root, 0, includeStack, text, error))
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
			if (!expandSlangIncludes(pass.sourcePath, root, 0, includeStack, text, error))
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
	if (format == ShaderFormat::SlangPreset) {
		if (!loadSlangPreset(path, chain, error))
			return false;
	}
	else if (format == ShaderFormat::ReShadeFX) {
		std::string text;
		std::unordered_set<std::string> includeStack;
		const std::string packageRoot = parentPath(path);
		if (!expandSlangIncludes(path, packageRoot, 0, includeStack, text, error))
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
		return fail(error, "Select a .glsl, .slang, .slangp, or .fx shader file.");
	}

	chain.sources.clear();
	chain.sources.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		ShaderPass& pass = chain.passes[passIndex];
		ShaderPassSource passSource;
		if (!loadShaderPassSource(pass, passSource, error))
			return false;
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
		for (const ShaderParameter& reflected : passSource.parameters) {
			auto existing = std::find_if(chain.parameters.begin(), chain.parameters.end(), [&](const ShaderParameter& parameter) {
				return parameter.name == reflected.name;
			});
			if (existing == chain.parameters.end())
				chain.parameters.push_back(reflected);
			else {
				if (existing->componentCount != reflected.componentCount || existing->type != reflected.type)
					return fail(error, "Shader parameter '" + reflected.name + "' changes type between passes.");
				existing->label = reflected.label;
				existing->minimum = reflected.minimum;
				existing->maximum = reflected.maximum;
				existing->step = reflected.step;
				existing->values = reflected.values;
				existing->minimumValues = reflected.minimumValues;
				existing->maximumValues = reflected.maximumValues;
				existing->stepValues = reflected.stepValues;
			}
		}
		chain.sources.push_back(std::move(passSource));
	}
	if (chain.parameters.size() > MaxParameterCount)
		return fail(error, "Shader chains may expose at most 16 parameters.");
	for (const ShaderParameter& parameter : chain.parameters)
		if (!isValidIdentifier(parameter.name))
			return fail(error, "Shader parameter names must be valid identifiers.");
	error.clear();
	return true;
}

} // namespace postprocess
