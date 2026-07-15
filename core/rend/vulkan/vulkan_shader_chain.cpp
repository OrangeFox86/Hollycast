/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#include "vulkan_shader_chain.h"

#include "cfg/option.h"
#include "compiler.h"
#include "log/LogManager.h"
#include "oslib/storage.h"
#include "stbi.h"
#include "utils.h"
#include "vulkan_context.h"

#include <algorithm>
#include <regex>
#include <unordered_map>

namespace {

constexpr char FragmentPreamble[] = R"(
layout (location = 0) in vec2 vTexCoord;
layout (push_constant) uniform ShaderFrame
{
	vec4 sizes;
	uvec4 frame;
	vec4 parameters[6];
} shaderFrame;
#define SourceSize shaderFrame.sizes.xy
#define OutputSize shaderFrame.sizes.zw
#define FrameCount int(shaderFrame.frame.x)
#define Rotation int(shaderFrame.frame.y)
#define OriginalSize vec2(shaderFrame.frame.zw)
layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor
)";

constexpr char VertexPreamble[] = R"(
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 0) out vec2 vTexCoord;
layout (push_constant) uniform ShaderFrame
{
	vec4 sizes;
	uvec4 frame;
	vec4 parameters[6];
} shaderFrame;
#define SourceSize shaderFrame.sizes.xy
#define OutputSize shaderFrame.sizes.zw
#define FrameCount int(shaderFrame.frame.x)
#define Rotation int(shaderFrame.frame.y)
#define OriginalSize vec2(shaderFrame.frame.zw)
#define VertexCoord in_pos
#define TexCoord in_uv
)";

constexpr uint32_t MaxShaderInputs = 16;
// Custom vertex stages may sample the same graph resources as fragment stages.
constexpr vk::PipelineStageFlags ShaderReadStages =
		vk::PipelineStageFlagBits::eVertexShader | vk::PipelineStageFlagBits::eFragmentShader;

void removeVersionDirective(std::string& source)
{
	const size_t lineEnd = source.find_first_of("\r\n");
	if (source.compare(0, 8, "#version") != 0)
		return;
	source.erase(0, lineEnd == std::string::npos ? source.size() : source.find_first_not_of("\r\n", lineEnd));
}

struct ShaderFrameUniforms {
	float sizes[4];
	uint32_t frameCount;
	uint32_t rotation;
	uint32_t originalWidth;
	uint32_t originalHeight;
	float parameters[postprocess::MaxShaderParameterComponents];
};

static_assert(postprocess::MaxShaderParameterComponents == 24,
		"Update the Vulkan shader preambles when changing the shared limit.");

bool isSampled2D(postprocess::SlangResourceType type)
{
	return type == postprocess::SlangResourceType::Sampler2D || type == postprocess::SlangResourceType::Texture2D;
}

uint32_t parameterComponentCount(postprocess::SlangParameterType type)
{
	switch (type) {
	case postprocess::SlangParameterType::Float2: return 2;
	case postprocess::SlangParameterType::Float3: return 3;
	case postprocess::SlangParameterType::Float4: return 4;
	case postprocess::SlangParameterType::Float2x2: return 4;
	case postprocess::SlangParameterType::Float3x3: return 9;
	case postprocess::SlangParameterType::Float4x4: return 16;
	default: return 1;
	}
}

vk::BlendFactor blendFactor(postprocess::ShaderBlendFactor factor)
{
	switch (factor) {
	case postprocess::ShaderBlendFactor::Zero: return vk::BlendFactor::eZero;
	case postprocess::ShaderBlendFactor::One: return vk::BlendFactor::eOne;
	case postprocess::ShaderBlendFactor::SourceColor: return vk::BlendFactor::eSrcColor;
	case postprocess::ShaderBlendFactor::OneMinusSourceColor: return vk::BlendFactor::eOneMinusSrcColor;
	case postprocess::ShaderBlendFactor::DestinationColor: return vk::BlendFactor::eDstColor;
	case postprocess::ShaderBlendFactor::OneMinusDestinationColor: return vk::BlendFactor::eOneMinusDstColor;
	case postprocess::ShaderBlendFactor::SourceAlpha: return vk::BlendFactor::eSrcAlpha;
	case postprocess::ShaderBlendFactor::OneMinusSourceAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
	case postprocess::ShaderBlendFactor::DestinationAlpha: return vk::BlendFactor::eDstAlpha;
	case postprocess::ShaderBlendFactor::OneMinusDestinationAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
	}
	return vk::BlendFactor::eOne;
}

vk::BlendOp blendOperation(postprocess::ShaderBlendOperation operation)
{
	switch (operation) {
	case postprocess::ShaderBlendOperation::Add: return vk::BlendOp::eAdd;
	case postprocess::ShaderBlendOperation::Subtract: return vk::BlendOp::eSubtract;
	case postprocess::ShaderBlendOperation::ReverseSubtract: return vk::BlendOp::eReverseSubtract;
	case postprocess::ShaderBlendOperation::Minimum: return vk::BlendOp::eMin;
	case postprocess::ShaderBlendOperation::Maximum: return vk::BlendOp::eMax;
	}
	return vk::BlendOp::eAdd;
}

std::string glslParameterValue(const postprocess::SlangParameter& parameter, uint32_t offset)
{
	const auto value = [](uint32_t index) {
		return "shaderFrame.parameters[" + std::to_string(index / 4) + "][" + std::to_string(index % 4) + "]";
	};
	const auto composite = [&](const char *type, uint32_t components) {
		std::string result = std::string(type) + "(";
		for (uint32_t index = 0; index < components; ++index) {
			if (index != 0)
				result += ", ";
			result += value(offset + index);
		}
		return result + ")";
	};
	switch (parameter.type) {
	case postprocess::SlangParameterType::Float: return value(offset);
	case postprocess::SlangParameterType::Int: return "int(" + value(offset) + ")";
	case postprocess::SlangParameterType::Bool: return "(" + value(offset) + " != 0.0)";
	case postprocess::SlangParameterType::Float2: return "vec2(" + value(offset) + ", " + value(offset + 1) + ")";
	case postprocess::SlangParameterType::Float3: return "vec3(" + value(offset) + ", " + value(offset + 1) + ", " + value(offset + 2) + ")";
	case postprocess::SlangParameterType::Float4: return "vec4(" + value(offset) + ", " + value(offset + 1) + ", " + value(offset + 2) + ", " + value(offset + 3) + ")";
	case postprocess::SlangParameterType::Float2x2: return composite("mat2", 4);
	case postprocess::SlangParameterType::Float3x3: return composite("mat3", 9);
	case postprocess::SlangParameterType::Float4x4: return composite("mat4", 16);
	}
	return value(offset);
}

const char *glslParameterType(postprocess::SlangParameterType type)
{
	switch (type) {
	case postprocess::SlangParameterType::Int: return "int";
	case postprocess::SlangParameterType::Bool: return "bool";
	case postprocess::SlangParameterType::Float2: return "vec2";
	case postprocess::SlangParameterType::Float3: return "vec3";
	case postprocess::SlangParameterType::Float4: return "vec4";
	case postprocess::SlangParameterType::Float2x2: return "mat2";
	case postprocess::SlangParameterType::Float3x3: return "mat3";
	case postprocess::SlangParameterType::Float4x4: return "mat4";
	default: return "float";
	}
}

void removeResourceDeclarations(std::string& source, const std::vector<postprocess::SlangParameter>& parameters)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:sampler|texture|image)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)");
	static const std::regex numericUniform(
			R"(^\s*uniform\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|int|uint|bool|[iu]?vec[2-4]|mat(?:[2-4]|[2-4]x[2-4]))\s+[A-Za-z_][A-Za-z0-9_]*\s*;\s*$)",
			std::regex::icase);
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);) {
		const bool parameter = std::any_of(parameters.begin(), parameters.end(), [&](const postprocess::SlangParameter& value) {
			return std::regex_match(line, std::regex("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:float|int|bool|vec[2-4])\\s+"
					+ value.name + R"(\s*;\s*$)", std::regex::icase));
		});
		if (!std::regex_match(line, resource) && !std::regex_match(line, numericUniform) && !parameter)
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
}

void addVaryingLocations(std::string& source, const char *direction,
		std::unordered_map<std::string, uint32_t>& locations)
{
	const std::regex declaration("^(\\s*)(?:(flat)\\s+)?" + std::string(direction)
			+ R"(\s+(?:(?:lowp|mediump|highp)\s+)?(?:float|int|uint|[iu]?vec[2-4])\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
	std::istringstream lines(source);
	std::string normalized;
	uint32_t nextLocation = 1;
	for (const auto& location : locations)
		nextLocation = std::max(nextLocation, location.second + 1);
	for (std::string line; std::getline(lines, line);) {
		std::smatch match;
		if (std::regex_match(line, match, declaration)) {
			const std::string name = match[3].str();
			auto [location, inserted] = locations.emplace(name, nextLocation);
			if (inserted)
				++nextLocation;
			const size_t content = line.find_first_not_of(" \t");
			line = match[1].str() + "layout (location = " + std::to_string(location->second) + ") "
					+ line.substr(content);
		}
		normalized += line + '\n';
	}
	source = std::move(normalized);
}

vk::Format resolveTargetFormat(postprocess::ShaderTargetFormat targetFormat, const std::string& targetFormatName, std::string& error)
{
	if (!targetFormatName.empty()) {
		if (targetFormatName == "R8G8B8A8_UNORM")
			return vk::Format::eR8G8B8A8Unorm;
		if (targetFormatName == "R8G8B8A8_SRGB")
			return vk::Format::eR8G8B8A8Srgb;
		if (targetFormatName == "R16G16B16A16_SFLOAT")
			return vk::Format::eR16G16B16A16Sfloat;
		if (targetFormatName == "R16G16B16A16_UNORM")
			return vk::Format::eR16G16B16A16Unorm;
		if (targetFormatName == "R32G32B32A32_SFLOAT")
			return vk::Format::eR32G32B32A32Sfloat;
		error = "Vulkan post-processing does not support target format '" + targetFormatName + "'.";
		return vk::Format::eUndefined;
	}
	if (targetFormat == postprocess::ShaderTargetFormat::Float)
		return vk::Format::eR16G16B16A16Sfloat;
	if (targetFormat == postprocess::ShaderTargetFormat::Srgb)
		return vk::Format::eR8G8B8A8Srgb;
	return vk::Format::eR8G8B8A8Unorm;
}

std::string fragmentPreamble(const std::vector<postprocess::SlangParameter>& parameters,
		const std::vector<postprocess::ShaderPassInput>& bindings, uint32_t outputCount)
{
	std::string preamble = "layout (set = 0, binding = 0) uniform sampler2D Source;\n";
	for (size_t index = 0; index < bindings.size(); ++index)
		preamble += "layout (set = 0, binding = " + std::to_string(index + 1)
				+ ") uniform sampler2D " + bindings[index].bindingName + ";\n";
	preamble += FragmentPreamble;
	preamble += "#define FxOutput0 FragColor\n";
	for (uint32_t index = 1; index < outputCount; ++index)
		preamble += "layout (location = " + std::to_string(index) + ") out vec4 FxOutput" + std::to_string(index) + ";\n";
	uint32_t parameterOffset = 0;
	for (const postprocess::SlangParameter& parameter : parameters) {
		if (parameterOffset + parameterComponentCount(parameter.type) > postprocess::MaxShaderParameterComponents)
			return {};
		preamble += std::string(glslParameterType(parameter.type)) + " " + parameter.name + " = "
				+ glslParameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

std::string vertexPreamble(const std::vector<postprocess::SlangParameter>& parameters,
		const std::vector<postprocess::ShaderPassInput>& bindings, bool retroArchGlsl)
{
	std::string preamble = "layout (set = 0, binding = 0) uniform sampler2D Source;\n";
	for (size_t index = 0; index < bindings.size(); ++index)
		preamble += "layout (set = 0, binding = " + std::to_string(index + 1)
				+ ") uniform sampler2D " + bindings[index].bindingName + ";\n";
	preamble += VertexPreamble;
	if (retroArchGlsl)
		preamble += "#undef VertexCoord\n#define VertexCoord vec4(in_pos, 1.0)\n";
	uint32_t parameterOffset = 0;
	for (const postprocess::SlangParameter& parameter : parameters) {
		if (parameterOffset + parameterComponentCount(parameter.type) > postprocess::MaxShaderParameterComponents)
			return {};
		preamble += std::string(glslParameterType(parameter.type)) + " " + parameter.name + " = "
				+ glslParameterValue(parameter, parameterOffset) + ";\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

} // namespace

bool postprocess::buildVulkanShaderSources(const ShaderPassSource& source,
		const std::vector<SlangParameter>& parameters, const std::vector<ShaderPassInput>& bindings,
		uint32_t outputCount, std::string& fragmentSource, std::string& vertexSource, std::string& error)
{
	std::string fragment = source.fragment;
	removeVersionDirective(fragment);
	removeResourceDeclarations(fragment, parameters);
	std::string vertex = source.vertex;
	std::unordered_map<std::string, uint32_t> varyingLocations;
	if (!vertex.empty()) {
		removeVersionDirective(vertex);
		removeResourceDeclarations(vertex, parameters);
		addVaryingLocations(vertex, "out", varyingLocations);
	}
	addVaryingLocations(fragment, "in", varyingLocations);
	const std::string fragmentHeader = fragmentPreamble(parameters, bindings, outputCount);
	if (fragmentHeader.empty()) {
		error = "Vulkan shader chains may use at most 24 parameter components per pass.";
		return false;
	}
	fragmentSource = VulkanSource().addSource(fragmentHeader).addSource(fragment).generate();
	vertexSource.clear();
	if (!vertex.empty()) {
		const std::string vertexHeader = vertexPreamble(parameters, bindings, source.retroArchGlsl);
		if (vertexHeader.empty()) {
			error = "Vulkan shader chains may use at most 24 parameter components per pass.";
			return false;
		}
		vertexSource = VulkanSource().addSource(vertexHeader).addSource(vertex).generate();
	}
	error.clear();
	return true;
}

VulkanShaderChain::~VulkanShaderChain()
{
	term();
}

void VulkanShaderChain::ensureResources()
{
	if (renderPass)
		return;

	VulkanContext *context = VulkanContext::Instance();
	renderPass = createRenderPass({ vk::Format::eR8G8B8A8Unorm });

	std::array<vk::DescriptorSetLayoutBinding, MaxShaderInputs> bindings;
	for (uint32_t index = 0; index < bindings.size(); ++index)
		bindings[index] = vk::DescriptorSetLayoutBinding(index, vk::DescriptorType::eCombinedImageSampler, 1,
				vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);
	descriptorSetLayout = context->GetDevice().createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));
	const vk::PushConstantRange pushConstants(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0, sizeof(ShaderFrameUniforms));
	pipelineLayout = context->GetDevice().createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, descriptorSetLayout.get(), pushConstants));
	linearSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, 0.f));
	nearestSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge, 0.f, false, 1.f, false, vk::CompareOp::eNever, 0.f, 0.f));
	// A view with only level zero must not be sampled at a higher LOD. Mipmapped
	// targets use the matching unrestricted sampler below.
	mipLinearSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge));
	mipNearestSampler = context->GetDevice().createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eNearest, vk::Filter::eNearest,
			vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
			vk::SamplerAddressMode::eClampToEdge));
	quadBuffer = std::make_unique<QuadBuffer>();
	copyFragmentShader = ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addSource(fragmentPreamble({}, {}, 1)).addSource("void main() { gl_FragColor = texture(Source, vTexCoord); }").generate());
	const postprocess::ShaderPass copyPass;
	copyPipeline = createPipeline({}, copyFragmentShader.get(), renderPass.get(), copyPass, 1);
	copyRotatePipeline = createPipeline(VulkanContext::Instance()->GetShaderManager()->GetQuadVertexShader(true),
			copyFragmentShader.get(), renderPass.get(), copyPass, 1);
}

vk::UniqueRenderPass VulkanShaderChain::createRenderPass(const std::vector<vk::Format>& formats) const
{
	VulkanContext *context = VulkanContext::Instance();
	std::vector<vk::AttachmentDescription> attachments;
	std::vector<vk::AttachmentReference> colorAttachments;
	attachments.reserve(formats.size());
	colorAttachments.reserve(formats.size());
	for (size_t index = 0; index < formats.size(); ++index) {
		attachments.emplace_back(vk::AttachmentDescription({}, formats[index], vk::SampleCountFlagBits::e1,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
				vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
				vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal));
		colorAttachments.emplace_back(static_cast<uint32_t>(index), vk::ImageLayout::eColorAttachmentOptimal);
	}
	const vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, 0, nullptr,
			static_cast<uint32_t>(colorAttachments.size()), colorAttachments.data(),
			nullptr, nullptr, 0, nullptr);
	const vk::SubpassDependency dependency(0, VK_SUBPASS_EXTERNAL,
			vk::PipelineStageFlagBits::eColorAttachmentOutput, ShaderReadStages,
			vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);
	return context->GetDevice().createRenderPassUnique(vk::RenderPassCreateInfo({}, static_cast<uint32_t>(attachments.size()),
			attachments.data(), 1, &subpass, 1, &dependency));
}

vk::UniquePipeline VulkanShaderChain::createPipeline(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader,
		vk::RenderPass renderPass, const postprocess::ShaderPass& pass, size_t colorAttachmentCount) const
{
	VulkanContext *context = VulkanContext::Instance();
	const vk::PipelineVertexInputStateCreateInfo vertexInput = GetQuadInputStateCreateInfo(true);
	const vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, vk::PrimitiveTopology::eTriangleStrip);
	const vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo rasterization;
	rasterization.lineWidth = 1.f;
	const vk::PipelineMultisampleStateCreateInfo multisample;
	const vk::PipelineDepthStencilStateCreateInfo depthStencil;
	const vk::PipelineColorBlendAttachmentState colorBlend(pass.blendEnabled, blendFactor(pass.sourceBlend), blendFactor(pass.destinationBlend),
			blendOperation(pass.blendOperation), vk::BlendFactor::eOne, vk::BlendFactor::eZero, blendOperation(pass.blendOperation),
			vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	const std::vector<vk::PipelineColorBlendAttachmentState> colorBlends(colorAttachmentCount, colorBlend);
	const vk::PipelineColorBlendStateCreateInfo blendState({}, false, vk::LogicOp::eNoOp, colorBlends);
	const std::array<vk::DynamicState, 2> dynamicStates = { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	const vk::PipelineDynamicStateCreateInfo dynamicState({}, dynamicStates);
	const std::array<vk::PipelineShaderStageCreateInfo, 2> stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex,
				vertexShader ? vertexShader : VulkanContext::Instance()->GetShaderManager()->GetQuadVertexShader(false), "main"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fragmentShader, "main"),
	};
	return context->GetDevice().createGraphicsPipelineUnique(context->GetPipelineCache(),
			vk::GraphicsPipelineCreateInfo({}, stages, &vertexInput, &inputAssembly, nullptr, &viewportState,
					&rasterization, &multisample, &depthStencil, &blendState, &dynamicState, pipelineLayout.get(), renderPass)).value;
}

bool VulkanShaderChain::compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate,
		std::string& error)
{
	VulkanContext *context = VulkanContext::Instance();
	candidate.reserve(chain.passes.size());
	for (size_t passIndex = 0; passIndex < chain.passes.size(); ++passIndex) {
		const postprocess::ShaderPass& pass = chain.passes[passIndex];
		postprocess::ShaderPassSource loadedSource;
		const postprocess::ShaderPassSource *passSource = passIndex < chain.sources.size()
				? &chain.sources[passIndex] : &loadedSource;
		if (passSource == &loadedSource && !postprocess::loadShaderPassSource(pass, loadedSource, error))
			return false;
		std::vector<postprocess::SlangParameter> typedParameters = passSource->slangParameters;
		if (typedParameters.empty())
			for (const postprocess::ShaderParameter& parameter : chain.parameters) {
				postprocess::SlangParameter typed;
				typed.name = parameter.name;
				typed.type = parameter.type;
				typed.defaultValue.values = parameter.values;
				typed.defaultValue.componentCount = parameter.componentCount;
				typedParameters.push_back(std::move(typed));
			}
		std::vector<postprocess::ShaderPassOutput> outputs = pass.outputs;
		if (outputs.empty())
			outputs.push_back({ pass.alias, pass.targetFormat, pass.targetFormatName, false });
		if (passSource->outputCount != outputs.size()) {
			error = "Vulkan post-processing shader output count does not match its configured render targets.";
			return false;
		}
		const vk::PhysicalDeviceLimits& limits = context->GetPhysicalDevice().getProperties().limits;
		if (outputs.size() > limits.maxColorAttachments || outputs.size() > limits.maxFragmentOutputAttachments) {
			error = "Vulkan post-processing shader uses more color targets than the device supports.";
			return false;
		}
		Program program;
		program.pass = pass;
		program.parameters = std::move(typedParameters);
		program.outputs.reserve(outputs.size());
		std::vector<vk::Format> targetFormats;
		targetFormats.reserve(outputs.size());
		for (const postprocess::ShaderPassOutput& output : outputs) {
			const vk::Format targetFormat = resolveTargetFormat(output.targetFormat, output.targetFormatName, error);
			if (targetFormat == vk::Format::eUndefined)
				return false;
			const vk::FormatFeatureFlags formatFeatures = context->GetPhysicalDevice().getFormatProperties(targetFormat).optimalTilingFeatures;
			if (!(formatFeatures & vk::FormatFeatureFlagBits::eColorAttachment) || !(formatFeatures & vk::FormatFeatureFlagBits::eSampledImage)) {
				error = "Vulkan does not support a requested post-processing target format.";
				return false;
			}
			const bool supportsMipmaps = (formatFeatures & vk::FormatFeatureFlagBits::eBlitSrc)
					&& (formatFeatures & vk::FormatFeatureFlagBits::eBlitDst)
					&& (formatFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear);
			if (output.useMipmaps && !supportsMipmaps) {
				error = "Vulkan post-processing mipmaps require linear blit support for every requested target format.";
				return false;
			}
			program.outputs.push_back({ output, targetFormat, output.useMipmaps, supportsMipmaps });
			targetFormats.push_back(targetFormat);
		}
		std::vector<postprocess::ShaderPassInput> shaderBindings;
		for (const postprocess::SlangResource& resource : passSource->slangResources) {
			if (!isSampled2D(resource.type) || resource.arrayCount != 1) {
				error = "Vulkan post-processing currently supports only non-array sampler2D Slang resources.";
				return false;
			}
			// Binding zero is the canonical Source name. SourceTexture and FX
			// SourceSampler still need their declared binding preserved.
			if (resource.convention == postprocess::SlangResourceConvention::Source && resource.name == "Source")
				continue;
			auto input = std::find_if(pass.inputs.begin(), pass.inputs.end(), [&](const postprocess::ShaderPassInput& candidateInput) {
				return candidateInput.bindingName == resource.name;
			});
			if (input == pass.inputs.end()) {
				error = "Vulkan post-processing could not resolve Slang resource '" + resource.name + "'.";
				return false;
			}
			shaderBindings.push_back(*input);
		}
		if (shaderBindings.size() + 1 > MaxShaderInputs) {
			error = "Vulkan post-processing supports at most 16 sampled inputs per pass.";
			return false;
		}
		std::string source;
		std::string vertexSource;
		if (!postprocess::buildVulkanShaderSources(*passSource, program.parameters, shaderBindings,
				static_cast<uint32_t>(program.outputs.size()), source, vertexSource, error))
			return false;
		for (size_t index = 0; index < shaderBindings.size(); ++index)
			program.bindings.push_back({ shaderBindings[index], static_cast<uint32_t>(index + 1) });
		if (!vertexSource.empty()) {
			if (!ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex,
					vertexSource, program.vertexShader, error))
				return false;
		}
		if (!ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment, source, program.fragmentShader, error))
			return false;
		try {
			program.renderPass = createRenderPass(targetFormats);
			program.pipeline = createPipeline(program.vertexShader.get(), program.fragmentShader.get(), program.renderPass.get(),
					program.pass, program.outputs.size());
		} catch (const vk::SystemError& exception) {
			error = exception.what();
			return false;
		}
		candidate.push_back(std::move(program));
	}
	return true;
}

bool VulkanShaderChain::updateConfiguredChain()
{
	const std::vector<std::string> requestedPaths = postprocess::configuredShaderPaths();
	const std::string requestedLabel = postprocess::shaderSelectionLabel(requestedPaths);
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPaths == configuredPaths && generation == configuredGeneration)
		return !programs.empty();
	auto releaseActiveResources = [&] {
		if (programs.empty() && targets.empty() && sourceTargets.empty() && lookupTextures.empty()
				&& descriptorSets.empty() && sourceDescriptorSets.empty())
			return;
		// A live reload can retire resources referenced by any swapchain image.
		// Waiting only on this rare transition keeps normal frames stall-free.
		VulkanContext::Instance()->GetDevice().waitIdle();
		targets.clear();
		sourceTargets.clear();
		lookupTextures.clear();
		descriptorSets.clear();
		sourceDescriptorSets.clear();
		programs.clear();
	};
	configuredPaths = requestedPaths;
	configuredGeneration = generation;
	if (requestedPaths.empty()) {
		releaseActiveResources();
		parameters.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return false;
	}
	std::string error;
	postprocess::ShaderChain chain;
	if (!postprocess::loadShaderChain(requestedPaths, chain, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	postprocess::applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
	std::vector<Program> candidate;
	try {
		ensureResources();
		if (!compileCandidate(chain, candidate, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
			return !programs.empty();
		}
	} catch (const std::exception& exception) {
		error = exception.what();
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedLabel.c_str(), error.c_str());
		return !programs.empty();
	}
	for (const Program& program : candidate)
		for (const Binding& binding : program.bindings)
			if (binding.input.sourcePass != postprocess::InvalidShaderPass
					&& binding.input.kind != postprocess::ShaderInputKind::LookupTexture
					&& (binding.input.sourcePass >= candidate.size()
							|| binding.input.sourceOutput >= candidate[binding.input.sourcePass].outputs.size())) {
				error = "Vulkan post-processing input references an unavailable render target output.";
				postprocess::reportShaderChainStatus(activePath, error);
				return !programs.empty();
			}
	const std::vector<std::vector<bool>> mipmapRequirements = postprocess::resolveShaderPassOutputMipmaps(chain);
	for (size_t passIndex = 0; passIndex < candidate.size(); ++passIndex)
		for (size_t outputIndex = 0; outputIndex < candidate[passIndex].outputs.size(); ++outputIndex)
			candidate[passIndex].outputs[outputIndex].needsMipmaps = mipmapRequirements[passIndex][outputIndex];
	for (const Program& program : candidate)
		for (const Program::Output& output : program.outputs)
			if (output.needsMipmaps && !output.supportsMipmaps) {
				error = "Vulkan post-processing mipmaps require linear blit support for every sampled render target.";
				postprocess::reportShaderChainStatus(activePath, error);
				return !programs.empty();
			}
	releaseActiveResources();
	programs = std::move(candidate);
	parameters = std::move(chain.parameters);
	wantsUnscaledInput = chain.wantsUnscaledInput;
	sourceHistoryDepth = 1;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			const uint32_t requiredDepth = input.kind == postprocess::ShaderInputKind::History
					? input.historyOffset + 1
					: input.kind == postprocess::ShaderInputKind::Feedback ? 2u : 1u;
			if (requiredDepth == 1)
				continue;
			if (input.sourcePass == postprocess::InvalidShaderPass)
				sourceHistoryDepth = std::max(sourceHistoryDepth, requiredDepth);
			else
				programs[input.sourcePass].historyDepth = std::max(programs[input.sourcePass].historyDepth, requiredDepth);
		}
	lookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		lookupTextures.push_back({ texture });
	activePath = requestedLabel;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
	parameterGeneration = postprocess::shaderChainStatusGeneration();
	return true;
}

bool VulkanShaderChain::ensureTarget(Target& target, const vk::Extent2D& extent, vk::Format format,
		bool mipmapped)
{
	uint32_t mipLevels = 1;
	for (uint32_t dimension = std::max(extent.width, extent.height); mipmapped && dimension > 1; dimension >>= 1)
		++mipLevels;
	if (target.image && target.extent == extent && target.format == format && target.mipLevels == mipLevels)
		return false;
	target.framebuffer.reset();
	target.sampleView.reset();
	target.renderView.reset();
	target.image.reset();
	target.allocation = {};

	VulkanContext *context = VulkanContext::Instance();
	vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
	if (mipLevels > 1)
		usage |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
	target.image = context->GetDevice().createImageUnique(vk::ImageCreateInfo({}, vk::ImageType::e2D,
			format, vk::Extent3D(extent, 1), mipLevels, 1, vk::SampleCountFlagBits::e1,
			vk::ImageTiling::eOptimal, usage));
	VmaAllocationCreateInfo allocationInfo = { VmaAllocationCreateFlags(), VmaMemoryUsage::VMA_MEMORY_USAGE_GPU_ONLY };
	target.allocation = context->GetAllocator().AllocateForImage(target.image.get(), allocationInfo);
	const vk::ImageSubresourceRange renderRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
	target.renderView = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, target.image.get(),
			vk::ImageViewType::e2D, format, {}, renderRange));
	const vk::ImageSubresourceRange sampleRange(vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1);
	target.sampleView = context->GetDevice().createImageViewUnique(vk::ImageViewCreateInfo({}, target.image.get(),
			vk::ImageViewType::e2D, format, {}, sampleRange));
	target.extent = extent;
	target.format = format;
	target.mipLevels = mipLevels;
	target.renderedFrame = 0;
	target.initialized = false;
	return true;
}

bool VulkanShaderChain::ensureTargetSet(TargetSet& targetSet, const Program& program, const vk::Extent2D& extent)
{
	bool changed = targetSet.outputs.size() != program.outputs.size();
	if (changed)
		targetSet.outputs.resize(program.outputs.size());
	for (size_t index = 0; index < program.outputs.size(); ++index)
		changed = ensureTarget(targetSet.outputs[index], extent, program.outputs[index].format,
				program.outputs[index].needsMipmaps) || changed;
	if (!changed)
		return false;
	targetSet.renderedFrame = 0;
	targetSet.initialized = false;
	targetSet.framebuffer.reset();
	std::vector<vk::ImageView> attachments;
	attachments.reserve(targetSet.outputs.size());
	for (const Target& target : targetSet.outputs)
		attachments.push_back(target.renderView.get());
	targetSet.framebuffer = VulkanContext::Instance()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo({},
			program.renderPass.get(), attachments, extent.width, extent.height, 1));
	return true;
}

const VulkanShaderChain::Target *VulkanShaderChain::findSourceHistoryTarget(uint32_t frameCount) const
{
	// Resources stay partitioned by swapchain image for flight safety, so frame
	// metadata—not the current partition—identifies the requested history slot.
	for (const std::vector<Target>& partition : sourceTargets)
		for (const Target& target : partition)
			if (target.initialized && target.renderedFrame == frameCount)
				return &target;
	return nullptr;
}

const VulkanShaderChain::TargetSet *VulkanShaderChain::findPassHistoryTarget(uint32_t passIndex,
		uint32_t frameCount) const
{
	for (const std::vector<std::vector<TargetSet>>& partition : targets) {
		if (passIndex >= partition.size())
			continue;
		for (const TargetSet& targetSet : partition[passIndex])
			if (targetSet.initialized && targetSet.renderedFrame == frameCount)
				return &targetSet;
	}
	return nullptr;
}

void VulkanShaderChain::generateMipmaps(vk::CommandBuffer commandBuffer, const Target& target, bool existingContents) const
{
	if (target.mipLevels == 1)
		return;
	const vk::ImageSubresourceRange baseRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
	vk::ImageMemoryBarrier baseBarrier(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eTransferSrcOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), baseRange);
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
			{}, nullptr, nullptr, baseBarrier);

	uint32_t sourceWidth = target.extent.width;
	uint32_t sourceHeight = target.extent.height;
	for (uint32_t level = 1; level < target.mipLevels; ++level) {
		const vk::ImageSubresourceRange destinationRange(vk::ImageAspectFlagBits::eColor, level, 1, 0, 1);
		const vk::ImageMemoryBarrier destinationBarrier(existingContents ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
				vk::AccessFlagBits::eTransferWrite, existingContents ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
				vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), destinationRange);
		commandBuffer.pipelineBarrier(existingContents ? ShaderReadStages : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eTransfer, {}, nullptr, nullptr, destinationBarrier);
		const uint32_t destinationWidth = std::max(sourceWidth >> 1, 1u);
		const uint32_t destinationHeight = std::max(sourceHeight >> 1, 1u);
		const vk::ImageBlit blit(vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level - 1, 0, 1),
				{ { vk::Offset3D(), vk::Offset3D(static_cast<int>(sourceWidth), static_cast<int>(sourceHeight), 1) } },
				vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, level, 0, 1),
				{ { vk::Offset3D(), vk::Offset3D(static_cast<int>(destinationWidth), static_cast<int>(destinationHeight), 1) } });
		commandBuffer.blitImage(target.image.get(), vk::ImageLayout::eTransferSrcOptimal, target.image.get(),
				vk::ImageLayout::eTransferDstOptimal, blit, vk::Filter::eLinear);
		const vk::ImageSubresourceRange sourceRange(vk::ImageAspectFlagBits::eColor, level - 1, 1, 0, 1);
		const vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
				vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), sourceRange);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, ShaderReadStages,
				{}, nullptr, nullptr, sourceBarrier);
		baseBarrier = vk::ImageMemoryBarrier(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eTransferRead,
				vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eTransferSrcOptimal,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), destinationRange);
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
				{}, nullptr, nullptr, baseBarrier);
		sourceWidth = destinationWidth;
		sourceHeight = destinationHeight;
	}
	const vk::ImageSubresourceRange lastRange(vk::ImageAspectFlagBits::eColor, target.mipLevels - 1, 1, 0, 1);
	const vk::ImageMemoryBarrier lastBarrier(vk::AccessFlagBits::eTransferRead, vk::AccessFlagBits::eShaderRead,
			vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, target.image.get(), lastRange);
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, ShaderReadStages,
			{}, nullptr, nullptr, lastBarrier);
}

bool VulkanShaderChain::ensureLookupTextures(vk::CommandBuffer commandBuffer, std::string& error)
{
	for (LookupTexture& lookup : lookupTextures) {
		if (lookup.texture)
			continue;
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(lookup.config.path, "rb"));
		if (!file) {
			error = "Unable to open post-processing lookup texture: " + lookup.config.path;
			return false;
		}
		int width;
		int height;
		int channels;
		u8 *pixels = stbi_load_from_file(file.get(), &width, &height, &channels, STBI_rgb_alpha);
		if (!pixels || width <= 0 || height <= 0) {
			if (pixels)
				stbi_image_free(pixels);
			error = "Unable to decode post-processing lookup texture: " + lookup.config.path;
			return false;
		}
		try {
			lookup.texture = std::make_unique<Texture>();
			lookup.texture->tex_type = TextureType::_8888;
			lookup.texture->SetCommandBuffer(commandBuffer);
			lookup.texture->UploadToGPU(width, height, pixels, lookup.config.useMipmaps);
			lookup.texture->SetCommandBuffer(nullptr);
			vk::SamplerCreateInfo samplerInfo;
			samplerInfo.magFilter = lookup.config.linearFilter ? vk::Filter::eLinear : vk::Filter::eNearest;
			samplerInfo.minFilter = samplerInfo.magFilter;
			samplerInfo.mipmapMode = lookup.config.useMipmaps ? vk::SamplerMipmapMode::eLinear : vk::SamplerMipmapMode::eNearest;
			samplerInfo.addressModeU = lookup.config.wrapMode == postprocess::ShaderWrapMode::Repeat ? vk::SamplerAddressMode::eRepeat
					: lookup.config.wrapMode == postprocess::ShaderWrapMode::MirroredRepeat ? vk::SamplerAddressMode::eMirroredRepeat
					: lookup.config.wrapMode == postprocess::ShaderWrapMode::ClampToBorder ? vk::SamplerAddressMode::eClampToBorder : vk::SamplerAddressMode::eClampToEdge;
			samplerInfo.addressModeV = samplerInfo.addressModeW = samplerInfo.addressModeU;
			samplerInfo.borderColor = vk::BorderColor::eFloatTransparentBlack;
			samplerInfo.maxLod = lookup.config.useMipmaps ? vk::LodClampNone : 0.f;
			lookup.sampler = VulkanContext::Instance()->GetDevice().createSamplerUnique(samplerInfo);
		} catch (const std::exception& exception) {
			stbi_image_free(pixels);
			lookup.texture.reset();
			error = exception.what();
			return false;
		}
		stbi_image_free(pixels);
	}
	return true;
}

bool VulkanShaderChain::render(vk::Image sourceImage, vk::ImageView sourceView, const vk::Extent2D& extent,
		const postprocess::ShaderFrameInfo& frame, vk::ImageView& outputView,
		bool& presentationTransformApplied)
{
	presentationTransformApplied = false;
	if (!updateConfiguredChain() || !sourceImage || !sourceView || extent.width == 0 || extent.height == 0)
		return false;
	VulkanContext *context = VulkanContext::Instance();
	const size_t imageIndex = static_cast<size_t>(context->GetCurrentImageIndex());
	if (targets.size() <= imageIndex)
		targets.resize(imageIndex + 1);
	if (targets[imageIndex].size() < programs.size())
		targets[imageIndex].resize(programs.size());
	if (sourceTargets.size() <= imageIndex)
		sourceTargets.resize(imageIndex + 1);
	vk::CommandBuffer commandBuffer = context->GetCurrentCommandBuffer();
	std::string lookupError;
	if (!ensureLookupTextures(commandBuffer, lookupError)) {
		postprocess::reportShaderChainStatus(activePath, lookupError);
		WARN_LOG(RENDERER, "Post-processing lookup texture failed: %s", lookupError.c_str());
		return false;
	}
	const vk::ImageMemoryBarrier sourceBarrier(vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
			VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, sourceImage,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, ShaderReadStages,
			{}, nullptr, nullptr, sourceBarrier);
	if (descriptorSets.size() <= imageIndex)
		descriptorSets.resize(imageIndex + 1);
	if (sourceDescriptorSets.size() <= imageIndex)
		sourceDescriptorSets.resize(imageIndex + 1);
	if (descriptorSets[imageIndex].size() < programs.size())
		descriptorSets[imageIndex].resize(programs.size());
	postprocess::ShaderFrameInfo passFrame = frame;
	const uint64_t statusGeneration = postprocess::shaderChainStatusGeneration();
	if (statusGeneration != parameterGeneration) {
		const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
		parameterGeneration = statusGeneration;
		if (status.activePath == activePath && status.error.empty())
			parameters = status.parameters;
	}
	vk::ImageView inputView = sourceView;
	vk::ImageView originalView = sourceView;
	bool inputHasMipmaps = false;
	const size_t currentSourceTarget = frame.frameCount % sourceHistoryDepth;
	const bool composeDisplayInput = postprocess::shouldComposeDisplayInput(frame, wantsUnscaledInput);
	const vk::Extent2D chainInputExtent = composeDisplayInput
			? vk::Extent2D(static_cast<uint32_t>(frame.viewportWidth), static_cast<uint32_t>(frame.viewportHeight)) : extent;
	const bool sourceNeedsMipmaps = programs.front().pass.useMipmaps;
	if (composeDisplayInput || sourceNeedsMipmaps || sourceHistoryDepth > 1) {
		if (sourceTargets[imageIndex].size() != sourceHistoryDepth)
			sourceTargets[imageIndex].resize(sourceHistoryDepth);
		Target& sourceTarget = sourceTargets[imageIndex][currentSourceTarget];
		bool sourceWasAllocated = false;
		try {
			sourceWasAllocated = !ensureTarget(sourceTarget, chainInputExtent,
					vk::Format::eR8G8B8A8Unorm, sourceNeedsMipmaps);
		} catch (const std::exception& exception) {
			postprocess::reportShaderChainStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing source target allocation failed: %s", exception.what());
			return false;
		}
		const vk::ImageMemoryBarrier targetBarrier(sourceWasAllocated ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
				vk::AccessFlagBits::eColorAttachmentWrite,
				sourceWasAllocated ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
				vk::ImageLayout::eColorAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
				sourceTarget.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		commandBuffer.pipelineBarrier(sourceWasAllocated ? ShaderReadStages : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, targetBarrier);
		if (!sourceDescriptorSets[imageIndex]) {
			sourceDescriptorSets[imageIndex] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
		}
		const vk::Sampler copySampler = composeDisplayInput && config::LinearInterpolation
				? linearSampler.get() : nearestSampler.get();
		const vk::DescriptorImageInfo imageInfo(copySampler, sourceView, vk::ImageLayout::eShaderReadOnlyOptimal);
		context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(sourceDescriptorSets[imageIndex].get(), 0, 0,
				vk::DescriptorType::eCombinedImageSampler, imageInfo), nullptr);
		const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }));
		if (!sourceTarget.framebuffer) {
			const std::array<vk::ImageView, 1> sourceAttachments = { sourceTarget.renderView.get() };
			sourceTarget.framebuffer = context->GetDevice().createFramebufferUnique(
					vk::FramebufferCreateInfo({}, renderPass.get(), sourceAttachments,
							chainInputExtent.width, chainInputExtent.height, 1));
		}
		commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(renderPass.get(), sourceTarget.framebuffer.get(),
				vk::Rect2D({}, chainInputExtent), clearColor), vk::SubpassContents::eInline);
		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics,
				frame.rotationPending && composeDisplayInput ? copyRotatePipeline.get() : copyPipeline.get());
		commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(chainInputExtent.width),
				static_cast<float>(chainInputExtent.height), 0.f, 1.f));
		commandBuffer.setScissor(0, vk::Rect2D({}, chainInputExtent));
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0, sourceDescriptorSets[imageIndex].get(), nullptr);
		ShaderFrameUniforms uniforms {};
		uniforms.sizes[0] = static_cast<float>(extent.width);
		uniforms.sizes[1] = static_cast<float>(extent.height);
		uniforms.sizes[2] = static_cast<float>(chainInputExtent.width);
		uniforms.sizes[3] = static_cast<float>(chainInputExtent.height);
		uniforms.frameCount = frame.frameCount;
		uniforms.rotation = frame.rotationPending ? 1u : 0u;
		uniforms.originalWidth = chainInputExtent.width;
		uniforms.originalHeight = chainInputExtent.height;
		commandBuffer.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
				0, sizeof(uniforms), &uniforms);
		quadBuffer->Update();
		quadBuffer->Bind(commandBuffer);
		quadBuffer->Draw(commandBuffer);
		commandBuffer.endRenderPass();
		if (sourceNeedsMipmaps)
			generateMipmaps(commandBuffer, sourceTarget, sourceWasAllocated);
		sourceTarget.renderedFrame = frame.frameCount;
		sourceTarget.initialized = true;
		if (composeDisplayInput || sourceNeedsMipmaps) {
			inputView = sourceTarget.sampleView.get();
			inputHasMipmaps = sourceNeedsMipmaps;
		}
		if (composeDisplayInput) {
			originalView = inputView;
			passFrame.sourceWidth = static_cast<int>(chainInputExtent.width);
			passFrame.sourceHeight = static_cast<int>(chainInputExtent.height);
		}
	}
	for (size_t index = 0; index < programs.size(); ++index) {
		Program& program = programs[index];
		int outputWidth;
		int outputHeight;
		std::string error;
		if (!postprocess::resolveTargetSize(program.pass, passFrame, outputWidth, outputHeight, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			return false;
		}
		const vk::Extent2D targetExtent(static_cast<uint32_t>(outputWidth), static_cast<uint32_t>(outputHeight));
		auto& targetHistory = targets[imageIndex][index];
		if (targetHistory.size() != program.historyDepth)
			targetHistory.resize(program.historyDepth);
		const size_t currentTarget = frame.frameCount % program.historyDepth;
		TargetSet& targetSet = targetHistory[currentTarget];
		try {
			// History and feedback sample a previous target set. Allocate every
			// attachment now so the first-frame fallback never exposes undefined memory.
			for (TargetSet& historyTarget : targetHistory)
				ensureTargetSet(historyTarget, program, targetExtent);
		} catch (const std::exception& exception) {
			postprocess::reportShaderChainStatus(activePath, exception.what());
			WARN_LOG(RENDERER, "Post-processing target allocation failed: %s", exception.what());
			return false;
		}
		std::vector<bool> targetWasInitialized;
		targetWasInitialized.reserve(targetSet.outputs.size());
		for (const Target& target : targetSet.outputs)
			targetWasInitialized.push_back(target.initialized);
		std::vector<vk::ImageMemoryBarrier> targetBarriers;
		targetBarriers.reserve(targetSet.outputs.size());
		bool hasInitializedTarget = false;
		for (size_t outputIndex = 0; outputIndex < targetSet.outputs.size(); ++outputIndex) {
			const Target& target = targetSet.outputs[outputIndex];
			const bool initialized = targetWasInitialized[outputIndex];
			hasInitializedTarget = hasInitializedTarget || initialized;
			targetBarriers.emplace_back(initialized ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(),
					vk::AccessFlagBits::eColorAttachmentWrite,
					initialized ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
					vk::ImageLayout::eColorAttachmentOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
					target.image.get(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		}
		commandBuffer.pipelineBarrier(hasInitializedTarget ? ShaderReadStages : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, targetBarriers);
		if (!descriptorSets[imageIndex][index]) {
			descriptorSets[imageIndex][index] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
		}
		const vk::Sampler sampler = program.pass.linearFilter
				? (inputHasMipmaps ? mipLinearSampler.get() : linearSampler.get())
				: (inputHasMipmaps ? mipNearestSampler.get() : nearestSampler.get());
		std::vector<vk::DescriptorImageInfo> imageInfos;
		std::vector<vk::WriteDescriptorSet> descriptorWrites;
		imageInfos.reserve(program.bindings.size() + 1);
		descriptorWrites.reserve(program.bindings.size() + 1);
		auto bindImage = [&](uint32_t binding, vk::Sampler imageSampler, vk::ImageView view) {
			imageInfos.emplace_back(imageSampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
			descriptorWrites.emplace_back(descriptorSets[imageIndex][index].get(), binding, 0, 1,
					vk::DescriptorType::eCombinedImageSampler, &imageInfos.back());
		};
		bindImage(0, sampler, inputView);
		for (const Binding& binding : program.bindings) {
			vk::ImageView view {};
			vk::Sampler resourceSampler = sampler;
			bool resourceHasMipmaps = false;
			switch (binding.input.kind) {
			case postprocess::ShaderInputKind::Source:
				view = inputView;
				resourceHasMipmaps = inputHasMipmaps;
				break;
			case postprocess::ShaderInputKind::Original:
				view = originalView;
				break;
			case postprocess::ShaderInputKind::PassOutput: {
				const Program& sourceProgram = programs[binding.input.sourcePass];
				const size_t sourceTarget = frame.frameCount % sourceProgram.historyDepth;
				view = targets[imageIndex][binding.input.sourcePass][sourceTarget].outputs[binding.input.sourceOutput].sampleView.get();
				resourceHasMipmaps = programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
				break;
			}
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback: {
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				const bool requestedFrameAvailable = frame.frameCount >= offset;
				const uint32_t requestedFrame = requestedFrameAvailable ? frame.frameCount - offset : 0;
				if (binding.input.sourcePass == postprocess::InvalidShaderPass) {
					const Target *history = requestedFrameAvailable ? findSourceHistoryTarget(requestedFrame) : nullptr;
					view = history != nullptr ? history->sampleView.get() : originalView;
					resourceHasMipmaps = history != nullptr && history->mipLevels > 1;
					break;
				}
				const TargetSet *historySet = requestedFrameAvailable
						? findPassHistoryTarget(binding.input.sourcePass, requestedFrame) : nullptr;
				const Target *history = historySet != nullptr && binding.input.sourceOutput < historySet->outputs.size()
						? &historySet->outputs[binding.input.sourceOutput] : nullptr;
				// Startup or a missing exact frame falls back to Original deterministically.
				view = history != nullptr && history->initialized ? history->sampleView.get() : originalView;
				resourceHasMipmaps = history != nullptr && history->initialized
						&& programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
				break;
			}
			case postprocess::ShaderInputKind::LookupTexture: {
				auto lookup = std::find_if(lookupTextures.begin(), lookupTextures.end(), [&](const LookupTexture& value) {
					return value.config.name == binding.input.resourceName;
				});
				if (lookup == lookupTextures.end() || !lookup->texture || !lookup->sampler) {
					error = "Vulkan lookup texture '" + binding.input.resourceName + "' is unavailable.";
					postprocess::reportShaderChainStatus(activePath, error);
					return false;
				}
				view = lookup->texture->GetImageView();
				resourceSampler = lookup->sampler.get();
				break;
			}
			}
			if (!view) {
				error = "Vulkan shader resource '" + binding.input.bindingName + "' is unavailable.";
				postprocess::reportShaderChainStatus(activePath, error);
				return false;
			}
			if (binding.input.kind != postprocess::ShaderInputKind::LookupTexture)
				resourceSampler = program.pass.linearFilter
						? (resourceHasMipmaps ? mipLinearSampler.get() : linearSampler.get())
						: (resourceHasMipmaps ? mipNearestSampler.get() : nearestSampler.get());
			bindImage(binding.descriptorBinding, resourceSampler, view);
		}
		context->GetDevice().updateDescriptorSets(descriptorWrites, nullptr);
		const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }));
		const std::vector<vk::ClearValue> clearColors(targetSet.outputs.size(), clearColor);
		commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(program.renderPass.get(), targetSet.framebuffer.get(),
				vk::Rect2D({}, targetExtent), clearColors), vk::SubpassContents::eInline);
		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, program.pipeline.get());
		commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(outputWidth), static_cast<float>(outputHeight), 0.f, 1.f));
		commandBuffer.setScissor(0, vk::Rect2D({}, targetExtent));
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0, descriptorSets[imageIndex][index].get(), nullptr);
		ShaderFrameUniforms uniforms {};
		uniforms.sizes[0] = static_cast<float>(passFrame.sourceWidth);
		uniforms.sizes[1] = static_cast<float>(passFrame.sourceHeight);
		uniforms.sizes[2] = static_cast<float>(outputWidth);
		uniforms.sizes[3] = static_cast<float>(outputHeight);
		uniforms.frameCount = program.pass.frameCountModulo == 0 ? frame.frameCount : frame.frameCount % program.pass.frameCountModulo;
		uniforms.rotation = frame.rotationPending ? 1u : 0u;
		uniforms.originalWidth = chainInputExtent.width;
		uniforms.originalHeight = chainInputExtent.height;
		uint32_t parameterOffset = 0;
		for (const postprocess::SlangParameter& parameter : program.parameters) {
			const uint32_t components = parameterComponentCount(parameter.type);
			std::copy_n(parameter.defaultValue.values.begin(), components, uniforms.parameters + parameterOffset);
			const auto configured = std::find_if(parameters.begin(), parameters.end(), [&](const postprocess::ShaderParameter& value) {
				return value.name == parameter.name;
			});
			if (configured != parameters.end())
				std::copy_n(configured->values.begin(), components, uniforms.parameters + parameterOffset);
			parameterOffset += components;
		}
		commandBuffer.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
				0, sizeof(uniforms), &uniforms);
		quadBuffer->Update();
		quadBuffer->Bind(commandBuffer);
		quadBuffer->Draw(commandBuffer);
		commandBuffer.endRenderPass();
		for (size_t outputIndex = 0; outputIndex < targetSet.outputs.size(); ++outputIndex) {
			Target& target = targetSet.outputs[outputIndex];
			if (program.outputs[outputIndex].needsMipmaps)
				generateMipmaps(commandBuffer, target, targetWasInitialized[outputIndex]);
			target.initialized = true;
		}
		targetSet.renderedFrame = frame.frameCount;
		targetSet.initialized = true;
		inputView = targetSet.outputs[0].sampleView.get();
		inputHasMipmaps = program.outputs[0].needsMipmaps;
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
	}
	outputView = inputView;
	presentationTransformApplied = composeDisplayInput;
	return true;
}

void VulkanShaderChain::term()
{
	sourceTargets.clear();
	targets.clear();
	sourceDescriptorSets.clear();
	descriptorSets.clear();
	quadBuffer.reset();
	programs.clear();
	parameters.clear();
	copyRotatePipeline.reset();
	copyPipeline.reset();
	copyFragmentShader.reset();
	mipNearestSampler.reset();
	mipLinearSampler.reset();
	nearestSampler.reset();
	linearSampler.reset();
	pipelineLayout.reset();
	descriptorSetLayout.reset();
	renderPass.reset();
	configuredPaths.clear();
	configuredGeneration = 0;
	parameterGeneration = 0;
	wantsUnscaledInput = false;
	activePath.clear();
	sourceHistoryDepth = 1;
	postprocess::reportShaderChainStatus({}, {});
}
