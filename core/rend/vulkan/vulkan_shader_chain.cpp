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

namespace {

constexpr char FragmentPreamble[] = R"(
layout (location = 0) in vec2 vTexCoord;
layout (push_constant) uniform ShaderFrame
{
	vec4 sizes;
	uvec4 frame;
	vec4 parameters[4];
} shaderFrame;
#define SourceSize shaderFrame.sizes.xy
#define OutputSize shaderFrame.sizes.zw
#define FrameCount int(shaderFrame.frame.x)
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
	vec4 parameters[4];
} shaderFrame;
#define SourceSize shaderFrame.sizes.xy
#define OutputSize shaderFrame.sizes.zw
#define FrameCount int(shaderFrame.frame.x)
#define VertexCoord in_pos
#define TexCoord in_uv
)";

constexpr uint32_t MaxShaderInputs = 16;

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
	uint32_t padding[3];
	float parameters[16];
};

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

void removeResourceDeclarations(std::string& source, const std::vector<postprocess::SlangParameter>& parameters)
{
	static const std::regex resource(R"(^\s*(?:layout\s*\([^\n]*\)\s*)?(?:uniform\s+)?(?:(?:lowp|mediump|highp)\s+)?(?:sampler|texture|image)(?:2DArray|1D|2D|3D|Cube)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\[\s*[1-9][0-9]*\s*\])?\s*;\s*$)");
	std::string cleaned;
	std::istringstream stream(source);
	for (std::string line; std::getline(stream, line);) {
		const bool parameter = std::any_of(parameters.begin(), parameters.end(), [&](const postprocess::SlangParameter& value) {
			return std::regex_match(line, std::regex("^\\s*uniform\\s+(?:lowp\\s+|mediump\\s+|highp\\s+)?(?:float|int|bool|vec[2-4])\\s+"
					+ value.name + R"(\s*;\s*$)", std::regex::icase));
		});
		if (!std::regex_match(line, resource) && !parameter)
			cleaned += line + '\n';
	}
	source = std::move(cleaned);
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
		if (parameterOffset + parameterComponentCount(parameter.type) > 16)
			return {};
		preamble += "#define " + parameter.name + " " + glslParameterValue(parameter, parameterOffset) + "\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

std::string vertexPreamble(const std::vector<postprocess::SlangParameter>& parameters,
		const std::vector<postprocess::ShaderPassInput>& bindings)
{
	std::string preamble = "layout (set = 0, binding = 0) uniform sampler2D Source;\n";
	for (size_t index = 0; index < bindings.size(); ++index)
		preamble += "layout (set = 0, binding = " + std::to_string(index + 1)
				+ ") uniform sampler2D " + bindings[index].bindingName + ";\n";
	preamble += VertexPreamble;
	uint32_t parameterOffset = 0;
	for (const postprocess::SlangParameter& parameter : parameters) {
		if (parameterOffset + parameterComponentCount(parameter.type) > 16)
			return {};
		preamble += "#define " + parameter.name + " " + glslParameterValue(parameter, parameterOffset) + "\n";
		parameterOffset += parameterComponentCount(parameter.type);
	}
	return preamble;
}

} // namespace

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
			vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
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
		std::string body = passSource->fragment;
		removeVersionDirective(body);
		removeResourceDeclarations(body, program.parameters);
		const std::string preamble = fragmentPreamble(program.parameters, shaderBindings, static_cast<uint32_t>(program.outputs.size()));
		if (preamble.empty()) {
			error = "Vulkan shader chains may use at most 16 parameter components per pass.";
			return false;
		}
		const std::string source = VulkanSource().addSource(preamble).addSource(body).generate();
		for (size_t index = 0; index < shaderBindings.size(); ++index)
			program.bindings.push_back({ shaderBindings[index], static_cast<uint32_t>(index + 1) });
		if (!passSource->vertex.empty()) {
			std::string vertex = passSource->vertex;
			removeVersionDirective(vertex);
			removeResourceDeclarations(vertex, program.parameters);
			const std::string vertexSource = vertexPreamble(program.parameters, shaderBindings);
			if (vertexSource.empty() || !ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex,
					VulkanSource().addSource(vertexSource).addSource(vertex).generate(), program.vertexShader, error))
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
	const std::string& requestedPath = config::PostProcessShader.get();
	const uint64_t generation = postprocess::shaderChainReloadGeneration();
	if (requestedPath == configuredPath && generation == configuredGeneration)
		return !programs.empty();
	configuredPath = requestedPath;
	configuredGeneration = generation;
	if (requestedPath.empty()) {
		programs.clear();
		parameters.clear();
		activePath.clear();
		postprocess::reportShaderChainStatus({}, {});
		return false;
	}
	std::string error;
	postprocess::ShaderChain chain;
	if (!postprocess::loadShaderChain(requestedPath, chain, error)) {
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not loaded: %s", requestedPath.c_str(), error.c_str());
		return !programs.empty();
	}
	postprocess::applyShaderChainParameterValues(chain, config::PostProcessShaderParameters.get());
	std::vector<Program> candidate;
	try {
		ensureResources();
		if (!compileCandidate(chain, candidate, error)) {
			postprocess::reportShaderChainStatus(activePath, error);
			WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedPath.c_str(), error.c_str());
			return !programs.empty();
		}
	} catch (const std::exception& exception) {
		error = exception.what();
		postprocess::reportShaderChainStatus(activePath, error);
		WARN_LOG(RENDERER, "Post-processing shader '%s' was not activated: %s", requestedPath.c_str(), error.c_str());
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
	for (size_t producer = 0; producer < candidate.size(); ++producer) {
		for (Program::Output& output : candidate[producer].outputs)
			output.needsMipmaps = output.needsMipmaps || output.config.useMipmaps;
		for (size_t consumer = producer + 1; consumer < candidate.size(); ++consumer) {
			if (consumer == producer + 1 && candidate[consumer].pass.useMipmaps)
				candidate[producer].outputs[0].needsMipmaps = true;
			for (const postprocess::ShaderPassInput& input : candidate[consumer].pass.inputs) {
				if (input.sourcePass != producer
						|| (input.kind != postprocess::ShaderInputKind::PassOutput
								&& input.kind != postprocess::ShaderInputKind::History
								&& input.kind != postprocess::ShaderInputKind::Feedback))
					continue;
				if (input.sourceOutput >= candidate[producer].outputs.size()) {
					error = "Vulkan post-processing input references an unavailable render target output.";
					postprocess::reportShaderChainStatus(activePath, error);
					return !programs.empty();
				}
				candidate[producer].outputs[input.sourceOutput].needsMipmaps |= candidate[consumer].pass.useMipmaps;
			}
		}
	}
	for (const Program& program : candidate)
		for (const Program::Output& output : program.outputs)
			if (output.needsMipmaps && !output.supportsMipmaps) {
				error = "Vulkan post-processing mipmaps require linear blit support for every sampled render target.";
				postprocess::reportShaderChainStatus(activePath, error);
				return !programs.empty();
			}
	programs = std::move(candidate);
	parameters = std::move(chain.parameters);
	historyDepth = 2;
	needsSourceHistory = false;
	for (const postprocess::ShaderPass& pass : chain.passes)
		for (const postprocess::ShaderPassInput& input : pass.inputs) {
			if (input.kind == postprocess::ShaderInputKind::History) {
				historyDepth = std::max(historyDepth, input.historyOffset + 1);
				needsSourceHistory = needsSourceHistory || input.sourcePass == postprocess::InvalidShaderPass;
			}
		}
	targets.clear();
	sourceTargets.clear();
	lookupTextures.clear();
	lookupTextures.reserve(chain.textures.size());
	for (const postprocess::ShaderPresetTexture& texture : chain.textures)
		lookupTextures.push_back({ texture });
	activePath = requestedPath;
	postprocess::reportShaderChainStatus(activePath, {}, parameters,
			chain.techniques, chain.selectedTechnique);
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
	targetSet.framebuffer.reset();
	std::vector<vk::ImageView> attachments;
	attachments.reserve(targetSet.outputs.size());
	for (const Target& target : targetSet.outputs)
		attachments.push_back(target.renderView.get());
	targetSet.framebuffer = VulkanContext::Instance()->GetDevice().createFramebufferUnique(vk::FramebufferCreateInfo({},
			program.renderPass.get(), attachments, extent.width, extent.height, 1));
	return true;
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
		commandBuffer.pipelineBarrier(existingContents ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
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
		commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
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
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
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
		const postprocess::ShaderFrameInfo& frame, vk::ImageView& outputView)
{
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
	commandBuffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
			{}, nullptr, nullptr, sourceBarrier);
	if (descriptorSets.size() <= imageIndex)
		descriptorSets.resize(imageIndex + 1);
	if (sourceDescriptorSets.size() <= imageIndex)
		sourceDescriptorSets.resize(imageIndex + 1);
	if (descriptorSets[imageIndex].size() < programs.size())
		descriptorSets[imageIndex].resize(programs.size());
	postprocess::ShaderFrameInfo passFrame = frame;
	const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
	if (status.activePath == activePath && status.error.empty())
		parameters = status.parameters;
	vk::ImageView inputView = sourceView;
	bool inputHasMipmaps = false;
	const size_t currentTarget = frame.frameCount % historyDepth;
	if (programs.front().pass.useMipmaps || needsSourceHistory) {
		if (sourceTargets[imageIndex].size() != historyDepth)
			sourceTargets[imageIndex].resize(historyDepth);
		Target& sourceTarget = sourceTargets[imageIndex][currentTarget];
		const bool sourceWasAllocated = sourceTarget.image && sourceTarget.extent == extent;
		try {
			ensureTarget(sourceTarget, extent, vk::Format::eR8G8B8A8Unorm, true);
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
		commandBuffer.pipelineBarrier(sourceWasAllocated ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
				vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr, nullptr, targetBarrier);
		if (!sourceDescriptorSets[imageIndex]) {
			sourceDescriptorSets[imageIndex] = std::move(context->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(context->GetDescriptorPool(), descriptorSetLayout.get())).front());
		}
		const vk::DescriptorImageInfo imageInfo(nearestSampler.get(), sourceView, vk::ImageLayout::eShaderReadOnlyOptimal);
		context->GetDevice().updateDescriptorSets(vk::WriteDescriptorSet(sourceDescriptorSets[imageIndex].get(), 0, 0,
				vk::DescriptorType::eCombinedImageSampler, imageInfo), nullptr);
		const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4> { 0.f, 0.f, 0.f, 1.f }));
		if (!sourceTarget.framebuffer) {
			const std::array<vk::ImageView, 1> sourceAttachments = { sourceTarget.renderView.get() };
			sourceTarget.framebuffer = context->GetDevice().createFramebufferUnique(
					vk::FramebufferCreateInfo({}, renderPass.get(), sourceAttachments, extent.width, extent.height, 1));
		}
		commandBuffer.beginRenderPass(vk::RenderPassBeginInfo(renderPass.get(), sourceTarget.framebuffer.get(), vk::Rect2D({}, extent), clearColor), vk::SubpassContents::eInline);
		commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, copyPipeline.get());
		commandBuffer.setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.f, 1.f));
		commandBuffer.setScissor(0, vk::Rect2D({}, extent));
		commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipelineLayout.get(), 0, sourceDescriptorSets[imageIndex].get(), nullptr);
		ShaderFrameUniforms uniforms {};
		uniforms.sizes[0] = uniforms.sizes[2] = static_cast<float>(extent.width);
		uniforms.sizes[1] = uniforms.sizes[3] = static_cast<float>(extent.height);
		uniforms.frameCount = frame.frameCount;
		commandBuffer.pushConstants(pipelineLayout.get(), vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
				0, sizeof(uniforms), &uniforms);
		quadBuffer->Update();
		quadBuffer->Bind(commandBuffer);
		quadBuffer->Draw(commandBuffer);
		commandBuffer.endRenderPass();
		generateMipmaps(commandBuffer, sourceTarget, sourceWasAllocated);
		sourceTarget.initialized = true;
		if (programs.front().pass.useMipmaps) {
			inputView = sourceTarget.sampleView.get();
			inputHasMipmaps = true;
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
		if (targetHistory.size() != historyDepth)
			targetHistory.resize(historyDepth);
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
		commandBuffer.pipelineBarrier(hasInitializedTarget ? vk::PipelineStageFlagBits::eFragmentShader : vk::PipelineStageFlagBits::eTopOfPipe,
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
				view = sourceView;
				break;
			case postprocess::ShaderInputKind::PassOutput:
				view = targets[imageIndex][binding.input.sourcePass][currentTarget].outputs[binding.input.sourceOutput].sampleView.get();
				resourceHasMipmaps = programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
				break;
			case postprocess::ShaderInputKind::History:
			case postprocess::ShaderInputKind::Feedback: {
				const uint32_t offset = binding.input.kind == postprocess::ShaderInputKind::Feedback ? 1 : binding.input.historyOffset;
				const size_t historyIndex = (currentTarget + historyDepth - offset % historyDepth) % historyDepth;
				if (binding.input.sourcePass == postprocess::InvalidShaderPass) {
					const Target& history = sourceTargets[imageIndex][historyIndex];
					view = history.initialized ? history.sampleView.get() : sourceView;
					resourceHasMipmaps = history.initialized;
					break;
				}
				const Target& history = targets[imageIndex][binding.input.sourcePass][historyIndex].outputs[binding.input.sourceOutput];
				// The first frame has no history. Source is deterministic and safer than
				// sampling a just-allocated image with undefined contents.
				view = history.initialized ? history.sampleView.get() : sourceView;
				resourceHasMipmaps = history.initialized && programs[binding.input.sourcePass].outputs[binding.input.sourceOutput].needsMipmaps;
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
		inputView = targetSet.outputs[0].sampleView.get();
		inputHasMipmaps = program.outputs[0].needsMipmaps;
		passFrame.sourceWidth = outputWidth;
		passFrame.sourceHeight = outputHeight;
	}
	outputView = inputView;
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
	copyPipeline.reset();
	copyFragmentShader.reset();
	mipNearestSampler.reset();
	mipLinearSampler.reset();
	nearestSampler.reset();
	linearSampler.reset();
	pipelineLayout.reset();
	descriptorSetLayout.reset();
	renderPass.reset();
	configuredPath.clear();
	configuredGeneration = 0;
	activePath.clear();
	postprocess::reportShaderChainStatus({}, {});
}
