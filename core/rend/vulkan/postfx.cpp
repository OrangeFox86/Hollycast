/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#include "postfx.h"

#ifndef LIBRETRO

#include "compiler.h"
#include "quad.h"
#include "texture.h"
#include "vulkan_context.h"

#include "rend/postfx/postfx.h"
#include "rend/transform_matrix.h"
#include "cfg/option.h"

#include <array>
#include <algorithm>
#include <cmath>

namespace
{

constexpr u32 kMaxParams = 64;
struct alignas(16) PostFxUbo
{
	float SourceSize[4] {};
	float NativeSize[4] {};
	float OutputSize[4] {};
	float WindowSize[4] {};
	float Frame[4] {};
	float Params[kMaxParams][4] {};
};

static vk::UniquePipeline createPipeline(vk::Device device, vk::PipelineCache cache,
		vk::RenderPass renderPass, vk::PipelineLayout layout,
		vk::ShaderModule vs, vk::ShaderModule fs, bool writeAlpha)
{
	vk::PipelineVertexInputStateCreateInfo vertexInput = GetQuadInputStateCreateInfo(true);
	vk::PipelineInputAssemblyStateCreateInfo inputAssembly({}, vk::PrimitiveTopology::eTriangleStrip);
	vk::PipelineViewportStateCreateInfo viewportState({}, 1, nullptr, 1, nullptr);
	vk::PipelineRasterizationStateCreateInfo raster;
	raster.lineWidth = 1.f;
	vk::PipelineMultisampleStateCreateInfo msaa;
	vk::PipelineDepthStencilStateCreateInfo depthStencil;

	vk::PipelineColorBlendAttachmentState blend;
	blend.blendEnable = false;
	blend.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
			| vk::ColorComponentFlagBits::eB;
	if (writeAlpha)
		blend.colorWriteMask |= vk::ColorComponentFlagBits::eA;
	vk::PipelineColorBlendStateCreateInfo blendState({}, false, vk::LogicOp::eNoOp, blend);

	std::array<vk::DynamicState, 2> dynamics { vk::DynamicState::eViewport, vk::DynamicState::eScissor };
	vk::PipelineDynamicStateCreateInfo dynamicState({}, dynamics);

	std::array<vk::PipelineShaderStageCreateInfo, 2> stages {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, vs, "main"),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, fs, "main"),
	};

	vk::GraphicsPipelineCreateInfo pipeInfo({}, stages, &vertexInput, &inputAssembly, nullptr,
			&viewportState, &raster, &msaa, &depthStencil, &blendState, &dynamicState,
			layout, renderPass, 0);

	return device.createGraphicsPipelineUnique(cache, pipeInfo).value;
}

static std::string buildVertexShader(bool rotate)
{
	return R"(
#version 450
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 vTexCoord;

void main()
{
)" + std::string(rotate ? "	gl_Position = vec4(in_pos.y, -in_pos.x, in_pos.z, 1.0);\n" : "	gl_Position = vec4(in_pos, 1.0);\n") + R"(
	vTexCoord = in_uv;
}
)";
}

static std::string buildFragmentShader(const postfx::Preset& preset, const postfx::PassDef& pass,
		const std::vector<std::string>& paramIds, const std::vector<bool>& paramIsInt, bool forceOpaqueOutput)
{
	std::string s;
	s.reserve(pass.shaderBody.size() + 2048);
	s += R"(
#version 450

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D HC_Texture0;
layout(set = 0, binding = 1) uniform sampler2D HC_Texture1;
layout(set = 0, binding = 2) uniform sampler2D HC_Texture2;
layout(set = 0, binding = 3) uniform sampler2D HC_Texture3;
layout(set = 0, binding = 4) uniform sampler2D HC_Texture4;
layout(set = 0, binding = 5) uniform sampler2D HC_Texture5;
layout(set = 0, binding = 6) uniform sampler2D HC_Texture6;
layout(set = 0, binding = 7) uniform sampler2D HC_Texture7;
layout(set = 0, binding = 8, std140) uniform HC_PostFxUbo {
	vec4 SourceSize;
	vec4 NativeSize;
	vec4 OutputSize;
	vec4 WindowSize;
	vec4 Frame;
	vec4 Params[64];
} HC;

#define FragColor outColor

#define Texture HC_Texture0
#define Source Texture
#define Original Texture
#define SourceSize (HC.SourceSize.xy)
#define NativeSize (HC.NativeSize.xy)
#define OutputSize (HC.OutputSize.xy)
#define TextureSize SourceSize
#define InputSize SourceSize
#define OriginalSize SourceSize
#define FrameCount int(HC.Frame.x)
#define Time (HC.Frame.y)
#define TEX0 vTexCoord

#define float2 vec2
#define float3 vec3
#define float4 vec4
#define mat4x4 mat4
#define CONSTANT const
#define frac(x) fract(x)
#define saturate(x) clamp((x), 0.0, 1.0)

#define GetOption(x) (x)
#define texture2D texture
#define texture2DLod textureLod
#define texture2DProj textureProj
#define texture2DProjLod textureProjLod

vec2 GetCoordinates() { return vTexCoord; }
vec2 GetFragCoord() { return gl_FragCoord.xy; }
vec2 GetResolution() { return SourceSize; }
vec2 GetInvResolution() { return vec2(1.0) / max(SourceSize, vec2(1.0)); }
vec2 GetViewportSize() { return OutputSize; }
vec2 GetNativeSize() { return NativeSize; }
vec2 GetNativePixelSize() { return OutputSize / max(NativeSize, vec2(1.0)); }
vec2 GetInvNativeSize() { return vec2(1.0) / max(NativeSize, vec2(1.0)); }
vec2 GetInvNativePixelSize() { return GetNativePixelSize() / max(WindowSize, vec2(1.0)); }
vec2 GetWindowToViewportRatio() { return WindowSize / max(OutputSize, vec2(1.0)); }
vec2 GetWindowSize() { return WindowSize; }
vec2 GetInvWindowSize() { return vec2(1.0) / max(WindowSize, vec2(1.0)); }
vec2 GetOriginalSize() { return SourceSize; }
vec2 GetPaddedOriginalSize() { return SourceSize * GetWindowToViewportRatio(); }
vec2 GetWindowResolution() { return WindowSize; }
vec2 GetInternalSize() { return SourceSize; }
vec2 GetInternalPixelSize() { return OutputSize / max(SourceSize, vec2(1.0)); }
vec2 GetInvInternalPixelSize() { return GetInternalPixelSize() / max(WindowSize, vec2(1.0)); }
float GetUpscaleMultiplier() { return SourceSize.x / max(NativeSize.x, 1.0); }
float GetTime() { return Time; }
int GetFrameCount() { return FrameCount; }

#define HC_Sample(s, uv) texture((s), (uv))
vec4 SampleLocation(vec2 uv) { return texture(Texture, uv); }
vec4 Sample() { return SampleLocation(vTexCoord); }
#define SampleOffset(offset) textureOffset(Texture, vTexCoord, offset)
vec4 HCSanitizeColor(vec4 c) { return vec4(
	(isnan(c.x) || isinf(c.x)) ? 0.0 : c.x,
	(isnan(c.y) || isinf(c.y)) ? 0.0 : c.y,
	(isnan(c.z) || isinf(c.z)) ? 0.0 : c.z,
	(isnan(c.w) || isinf(c.w)) ? 1.0 : c.w); }
void SetOutput(vec4 c) { FragColor = HCSanitizeColor(c); }

#define OptionEnabled(x) ((x) != 0)
)";
	s += std::string("#define HC_FORCE_OUTPUT_OPAQUE ") + (forceOpaqueOutput ? "1\n" : "0\n");
	s += R"(
void HC_SetOutputImpl(vec4 c) {
	vec4 hc = HCSanitizeColor(c);
#if HC_FORCE_OUTPUT_OPAQUE == 1
	FragColor = vec4(hc.rgb, 1.0);
#else
	FragColor = hc;
#endif
}
#undef SetOutput
#define SetOutput(c) HC_SetOutputImpl(c)
)";
	for (size_t i = 0; i < pass.samplers.size() && i < VulkanPostFx::MaxSamplers; i++)
	{
		const auto& sb = pass.samplers[i];
		if (sb.name.empty() || sb.name == "Texture")
			continue;
		s += "#define ";
		s += sb.name;
		s += " HC_Texture";
		s += std::to_string(i);
		s += "\n";
	}
	for (size_t i = 0; i < paramIds.size() && i < kMaxParams; i++)
	{
		s += "#define ";
		s += paramIds[i];
		if (i < paramIsInt.size() && paramIsInt[i])
			s += " int(HC.Params[" + std::to_string(i) + "].x + 0.5)\n";
		else
			s += " (HC.Params[" + std::to_string(i) + "].x)\n";
	}
	s += "\n";
	std::string body = pass.shaderBody;
	size_t p = 0;
	while ((p = body.find("gl_FragColor", p)) != std::string::npos)
	{
		body.replace(p, 12, "FragColor");
		p += 9;
	}
	s += body;
	return s;
}

static vk::UniqueRenderPass createOffscreenRenderPass(vk::Device device, vk::Format format)
{
	vk::AttachmentDescription colorAttachment({}, format, vk::SampleCountFlagBits::e1,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
			vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
			vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
	vk::AttachmentReference colorRef(0, vk::ImageLayout::eColorAttachmentOptimal);
	vk::SubpassDescription subpass({}, vk::PipelineBindPoint::eGraphics, nullptr, colorRef);
	vk::SubpassDependency dep(
		VK_SUBPASS_EXTERNAL, 0,
		vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eColorAttachmentOutput,
		vk::AccessFlagBits::eShaderRead, vk::AccessFlagBits::eColorAttachmentWrite);
	vk::SubpassDependency dep2(
		0, VK_SUBPASS_EXTERNAL,
		vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eFragmentShader,
		vk::AccessFlagBits::eColorAttachmentWrite, vk::AccessFlagBits::eShaderRead);

	std::array<vk::SubpassDependency, 2> deps { dep, dep2 };
	return device.createRenderPassUnique(vk::RenderPassCreateInfo({}, colorAttachment, subpass, deps));
}

static postfx::ScaleType scaleTypeForPass(const postfx::PassDef& pass)
{
	return pass.scaleType;
}

static std::string summarizeCompilerError(const std::string& err)
{
	if (err.empty())
		return {};
	size_t start = 0;
	while (start < err.size() && (err[start] == '\n' || err[start] == '\r' || err[start] == ' ' || err[start] == '\t'))
		start++;
	size_t end = err.find_first_of("\r\n", start);
	std::string line = (end == std::string::npos) ? err.substr(start) : err.substr(start, end - start);
	if (line.size() > 220)
		line.resize(220);
	return line;
}

} // namespace

void VulkanPostFx::OnSwapchainRecreated(vk::Format format, u32 swapW, u32 swapH, u32 swapchainSize, vk::RenderPass swapchainRenderPass)
{
	this->format = format;
	this->swapW = swapW;
	this->swapH = swapH;
	this->swapchainRenderPass = swapchainRenderPass;

	offscreenRenderPass.reset();
	descSetLayout.reset();
	pipelineLayout.reset();
	linearSampler.reset();
	nearestSampler.reset();
	vertexShader.reset();
	vertexShaderRotate.reset();
	quadBuffer.reset();

	releaseSwapchainResources();
	configVersion = 0;
	cachedViewportW = cachedViewportH = 0;
	prepared = false;
}

void VulkanPostFx::releaseSwapchainResources()
{
	frames.clear();
	passes.clear();
}

void VulkanPostFx::Term()
{
	offscreenRenderPass.reset();
	descSetLayout.reset();
	pipelineLayout.reset();
	linearSampler.reset();
	nearestSampler.reset();
	vertexShader.reset();
	vertexShaderRotate.reset();
	quadBuffer.reset();

	releaseSwapchainResources();
	configVersion = 0;
	prepared = false;
}

bool VulkanPostFx::ensurePipelinesAndResources(vk::RenderPass swapchainRenderPass, int viewportW, int viewportH)
{
	postfx::Tick();
	const postfx::Preset* preset = postfx::GetActivePreset();
	if (!postfx::IsEnabled() || preset == nullptr || preset->passes.empty())
		return false;
	if (format == vk::Format::eUndefined)
	{
		postfx::SetRuntimeError("Vulkan PostFX is waiting for a valid swapchain format.");
		return false;
	}

	if (configVersion == postfx::GetConfigVersion() && cachedViewportW == viewportW && cachedViewportH == viewportH
			&& this->swapchainRenderPass == swapchainRenderPass && !passes.empty() && frames.size() == VulkanContext::Instance()->GetSwapChainSize())
		return true;

	rebuildPassChain(swapchainRenderPass, viewportW, viewportH);
	if (passes.empty() && postfx::GetLastError().empty())
		postfx::SetRuntimeError("Vulkan PostFX pass chain is empty.");
	return !passes.empty();
}

void VulkanPostFx::rebuildPassChain(vk::RenderPass swapchainRenderPass, int viewportW, int viewportH)
{
	releaseSwapchainResources();
	prepared = false;

	configVersion = postfx::GetConfigVersion();
	cachedViewportW = viewportW;
	cachedViewportH = viewportH;
	this->swapchainRenderPass = swapchainRenderPass;

	const postfx::Preset* preset = postfx::GetActivePreset();
	if (preset == nullptr)
		return;

	try
	{
		VulkanContext* ctx = VulkanContext::Instance();
		vk::Device device = ctx->GetDevice();

	if (!offscreenRenderPass)
		offscreenRenderPass = createOffscreenRenderPass(device, format);

	if (!descSetLayout)
	{
		std::array<vk::DescriptorSetLayoutBinding, VulkanPostFx::MaxSamplers + 1> bindings {};
		for (u32 i = 0; i < (u32)VulkanPostFx::MaxSamplers; i++)
			bindings[i] = vk::DescriptorSetLayoutBinding(i, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment);
		bindings[VulkanPostFx::MaxSamplers] = vk::DescriptorSetLayoutBinding((u32)VulkanPostFx::MaxSamplers,
				vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eFragment);
		descSetLayout = device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo({}, bindings));
		pipelineLayout = device.createPipelineLayoutUnique(vk::PipelineLayoutCreateInfo({}, descSetLayout.get()));

		linearSampler = device.createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eLinear, vk::Filter::eLinear,
				vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				vk::SamplerAddressMode::eClampToEdge));
		nearestSampler = device.createSamplerUnique(vk::SamplerCreateInfo({}, vk::Filter::eNearest, vk::Filter::eNearest,
				vk::SamplerMipmapMode::eNearest, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
				vk::SamplerAddressMode::eClampToEdge));
	}

	if (!vertexShader)
		vertexShader = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex, buildVertexShader(false));
	if (!vertexShaderRotate)
		vertexShaderRotate = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eVertex, buildVertexShader(true));
	if (!vertexShader || !vertexShaderRotate)
	{
		const std::string err = summarizeCompilerError(ShaderCompiler::GetLastError());
		if (!err.empty())
			postfx::SetRuntimeError(std::string("Vulkan vertex shader compile failed for PostFX: ") + err);
		else
			postfx::SetRuntimeError("Vulkan vertex shader compile failed for PostFX.");
		return;
	}
	if (!quadBuffer)
		quadBuffer = std::make_unique<QuadBuffer>();

	std::vector<std::string> paramIds;
	std::vector<bool> paramIsInt;
	for (const auto& p : preset->params)
	{
		paramIds.push_back(p.id);
		paramIsInt.push_back(p.integer);
	}

	passes.resize(preset->passes.size());

	int curW = viewportW;
	int curH = viewportH;
	for (size_t i = 0; i < preset->passes.size(); i++)
	{
		const auto& passDef = preset->passes[i];
		PassRuntime& pr = passes[i];
		pr.filter = passDef.filter;
		pr.samplers = passDef.samplers;
		pr.renderTargetName = passDef.renderTargetName;

		int baseW = viewportW;
		int baseH = viewportH;
		switch (scaleTypeForPass(passDef))
		{
		case postfx::ScaleType::Source:
			baseW = curW;
			baseH = curH;
			break;
		case postfx::ScaleType::Absolute:
			baseW = (int)std::round(passDef.scaleX);
			baseH = (int)std::round(passDef.scaleY);
			break;
		case postfx::ScaleType::Viewport:
		default:
			break;
		}
		if (passDef.scaleType != postfx::ScaleType::Absolute)
		{
			baseW = (int)std::round(baseW * passDef.scaleX);
			baseH = (int)std::round(baseH * passDef.scaleY);
		}
		pr.outW = std::max(1, baseW);
		pr.outH = std::max(1, baseH);
		if ((i + 1) == preset->passes.size())
		{
			// Final pass always draws into the current viewport region.
			pr.outW = viewportW;
			pr.outH = viewportH;
		}

		const bool isFinal = ((i + 1) == preset->passes.size());
		const std::string fragText = buildFragmentShader(*preset, passDef, paramIds, paramIsInt, isFinal);
		pr.frag = ShaderCompiler::TryCompile(vk::ShaderStageFlagBits::eFragment, fragText);
		if (!pr.frag)
		{
			const std::string err = summarizeCompilerError(ShaderCompiler::GetLastError());
			if (!err.empty())
				postfx::SetRuntimeError(std::string("Vulkan fragment compile failed for PostFX pass: ") + err);
			else
				postfx::SetRuntimeError("Vulkan fragment compile failed for PostFX pass.");
			passes.clear();
			return;
		}

		const vk::RenderPass rp = isFinal ? swapchainRenderPass : *offscreenRenderPass;
		pr.pipeline = createPipeline(device, ctx->GetPipelineCache(), rp, *pipelineLayout, vertexShader.get(), pr.frag.get(), !isFinal);
		if (isFinal)
			pr.pipelineRotate = createPipeline(device, ctx->GetPipelineCache(), rp, *pipelineLayout, vertexShaderRotate.get(), pr.frag.get(), false);

		curW = pr.outW;
		curH = pr.outH;
	}

		const u32 swapSize = (u32)ctx->GetSwapChainSize();
		frames.resize(swapSize);

		const u32 uboAlignment = (u32)ctx->GetUniformBufferAlignment();
		const vk::DeviceSize uboStride = (u32)sizeof(PostFxUbo) + align((u32)sizeof(PostFxUbo), uboAlignment);
		for (u32 i = 0; i < swapSize; i++)
		{
			SwapchainFrame& f = frames[i];
			f.descSets.resize(passes.size());
			for (auto& ds : f.descSets)
				ds.reset();

			f.ubo = std::make_unique<BufferData>(uboStride * passes.size(), vk::BufferUsageFlagBits::eUniformBuffer);

			const size_t offscreenPassCount = passes.size() > 0 ? passes.size() - 1 : 0;
			f.attachments.clear();
			f.attachmentReady.clear();
			f.framebuffers.clear();
			f.attachments.reserve(offscreenPassCount);
			f.attachmentReady.reserve(offscreenPassCount);
			f.framebuffers.reserve(offscreenPassCount);
			for (size_t p = 0; p < offscreenPassCount; p++)
			{
				f.attachments.emplace_back(ctx->GetPhysicalDevice(), device);
				f.attachments.back().Init((u32)passes[p].outW, (u32)passes[p].outH, format,
						vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled, "PostFx");
				f.attachmentReady.push_back(false);
				const vk::ImageView view = f.attachments.back().GetImageView();
				f.framebuffers.push_back(device.createFramebufferUnique(vk::FramebufferCreateInfo({}, *offscreenRenderPass,
						view, (u32)passes[p].outW, (u32)passes[p].outH, 1)));
			}
		}

		postfx::ClearRuntimeError();
	}
	catch (const std::exception& e)
	{
		WARN_LOG(RENDERER, "PostFX Vulkan rebuild failed: %s", e.what());
		postfx::SetRuntimeError(std::string("Vulkan PostFX rebuild failed: ") + e.what());
		releaseSwapchainResources();
		passes.clear();
	}
}

bool VulkanPostFx::Prepare(vk::CommandBuffer cmd, vk::ImageView srcView, const vk::Extent2D& srcExtent,
		int viewportW, int viewportH)
{
	prepared = false;
	preparedNamedViews.clear();
	postfx::Tick();
	const postfx::Preset* preset = postfx::GetActivePreset();
	if (!postfx::IsEnabled() || preset == nullptr || preset->passes.empty())
		return false;
	if (!ensurePipelinesAndResources(VulkanContext::Instance()->GetRenderPass(), viewportW, viewportH))
		return false;

	VulkanContext* ctx = VulkanContext::Instance();
	const u32 imageIndex = (u32)ctx->GetCurrentImageIndex();
	if (imageIndex >= frames.size())
	{
		postfx::SetRuntimeError("Vulkan PostFX frame resources are not ready for this swapchain image.");
		return false;
	}

	SwapchainFrame& frame = frames[imageIndex];
	const u32 uboAlignment = (u32)ctx->GetUniformBufferAlignment();
	const vk::DeviceSize uboStride = (u32)sizeof(PostFxUbo) + align((u32)sizeof(PostFxUbo), uboAlignment);

	vk::ImageView inputView = srcView;
	vk::Extent2D inputExtent = srcExtent;

	try
	{
		const u32 frameCount = FrameCount;
		const float timeSeconds = (float)(frameCount) / 60.f;
		const float renderScale = std::max(1.f, config::RenderResolution.get() / 480.f);
		const float nativeW = std::max(1.f, (float)srcExtent.width / renderScale);
		const float nativeH = std::max(1.f, (float)srcExtent.height / renderScale);
		preparedNativeExtent = vk::Extent2D { (u32)std::lround(nativeW), (u32)std::lround(nativeH) };

		const size_t offscreenPassCount = passes.size() > 0 ? passes.size() - 1 : 0;
		for (size_t passIndex = 0; passIndex < offscreenPassCount; passIndex++)
		{
			const PassRuntime& pr = passes[passIndex];
			const bool targetReady = passIndex < frame.attachmentReady.size() && frame.attachmentReady[passIndex];

			PostFxUbo ubo {};
			ubo.SourceSize[0] = (float)inputExtent.width;
			ubo.SourceSize[1] = (float)inputExtent.height;
			ubo.NativeSize[0] = nativeW;
			ubo.NativeSize[1] = nativeH;
			ubo.OutputSize[0] = (float)pr.outW;
			ubo.OutputSize[1] = (float)pr.outH;
			ubo.WindowSize[0] = (float)swapW;
			ubo.WindowSize[1] = (float)swapH;
			ubo.Frame[0] = (float)frameCount;
			ubo.Frame[1] = timeSeconds;

			for (size_t i = 0; i < preset->params.size() && i < kMaxParams; i++)
			{
				float v = preset->params[i].defaultValue;
				(void)postfx::GetParamValue(preset->params[i].id, v);
				ubo.Params[i][0] = v;
			}
			frame.ubo->upload(sizeof(ubo), &ubo, (u32)(uboStride * passIndex));

			if (!frame.descSets[passIndex])
			{
				frame.descSets[passIndex] = std::move(ctx->GetDevice().allocateDescriptorSetsUnique(
						vk::DescriptorSetAllocateInfo(ctx->GetDescriptorPool(), descSetLayout.get())).front());
			}

			std::array<vk::DescriptorImageInfo, VulkanPostFx::MaxSamplers> imageInfos;
			std::array<vk::WriteDescriptorSet, VulkanPostFx::MaxSamplers + 1> writes;
			for (size_t unit = 0; unit < VulkanPostFx::MaxSamplers; unit++)
			{
				vk::ImageView view = inputView;
				postfx::Filter filter = pr.filter;
				if (unit < pr.samplers.size())
				{
					const auto& sb = pr.samplers[unit];
					filter = sb.filter;
					if (sb.source != "$prev")
					{
						auto it = preparedNamedViews.find(sb.source);
						if (it != preparedNamedViews.end())
							view = it->second;
					}
				}
				const vk::Sampler sampler = (filter == postfx::Filter::Linear) ? linearSampler.get() : nearestSampler.get();
				imageInfos[unit] = vk::DescriptorImageInfo(sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
				writes[unit] = vk::WriteDescriptorSet(frame.descSets[passIndex].get(), (u32)unit, 0,
						vk::DescriptorType::eCombinedImageSampler, imageInfos[unit]);
			}
			vk::DescriptorBufferInfo bufferInfo(frame.ubo->buffer.get(), uboStride * passIndex, sizeof(PostFxUbo));
			writes[VulkanPostFx::MaxSamplers] = vk::WriteDescriptorSet(frame.descSets[passIndex].get(),
					(u32)VulkanPostFx::MaxSamplers, 0, vk::DescriptorType::eUniformBuffer, nullptr, bufferInfo);
			ctx->GetDevice().updateDescriptorSets(writes, nullptr);

			// Transition target image to color attachment, render, then it ends in shader-read layout.
			setImageLayout(cmd, frame.attachments[passIndex].GetImage(), format, 1,
					targetReady ? vk::ImageLayout::eShaderReadOnlyOptimal : vk::ImageLayout::eUndefined,
					vk::ImageLayout::eColorAttachmentOptimal);

			const vk::ClearValue clearColor(vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 1.f}));
			cmd.beginRenderPass(vk::RenderPassBeginInfo(*offscreenRenderPass, *frame.framebuffers[passIndex],
					vk::Rect2D({0, 0}, {(u32)pr.outW, (u32)pr.outH}), clearColor), vk::SubpassContents::eInline);

			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pr.pipeline.get());
			cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, frame.descSets[passIndex].get(), nullptr);
			cmd.setViewport(0, vk::Viewport(0.f, 0.f, (float)pr.outW, (float)pr.outH, 0.f, 1.f));
			cmd.setScissor(0, vk::Rect2D({0, 0}, {(u32)pr.outW, (u32)pr.outH}));

			if (passIndex == 0)
			{
				float shiftX = 0.f;
				float shiftY = 0.f;
				getVideoShift(shiftX, shiftY);
				QuadVertex vtx[4] {
					{ -1, -1, 0, 0, 0 },
					{  1, -1, 0, 1, 0 },
					{ -1,  1, 0, 0, 1 },
					{  1,  1, 0, 1, 1 },
				};
				vtx[0].x = vtx[2].x = -1.f + shiftX * 2.f / (float)std::max(1u, inputExtent.width);
				vtx[1].x = vtx[3].x = vtx[0].x + 2.f;
				vtx[0].y = vtx[1].y = -1.f + shiftY * 2.f / (float)std::max(1u, inputExtent.height);
				vtx[2].y = vtx[3].y = vtx[0].y + 2.f;
				quadBuffer->Update(vtx);
			}
			else
			{
				quadBuffer->Update(nullptr);
			}
			quadBuffer->Bind(cmd);
			quadBuffer->Draw(cmd);
			cmd.endRenderPass();
			frame.attachmentReady[passIndex] = true;
			if (!pr.renderTargetName.empty())
				preparedNamedViews[pr.renderTargetName] = frame.attachments[passIndex].GetImageView();

			inputView = frame.attachments[passIndex].GetImageView();
			inputExtent = vk::Extent2D { (u32)pr.outW, (u32)pr.outH };
		}

		prepared = true;
		preparedInputView = inputView;
		preparedInputExtent = inputExtent;
		return true;
	}
	catch (const std::exception& e)
	{
		WARN_LOG(RENDERER, "PostFX Vulkan prepare failed: %s", e.what());
		postfx::SetRuntimeError(std::string("Vulkan PostFX prepare failed: ") + e.what());
		prepared = false;
		preparedNamedViews.clear();
		return false;
	}
}

void VulkanPostFx::DrawFinalPass(vk::CommandBuffer cmd, int dx, int dy, int viewportW, int viewportH, bool rotate90, bool nearestFilter)
{
	if (!prepared || passes.empty())
		return;
	postfx::Tick();
	const postfx::Preset* preset = postfx::GetActivePreset();
	if (preset == nullptr)
		return;

	VulkanContext* ctx = VulkanContext::Instance();
	const u32 imageIndex = (u32)ctx->GetCurrentImageIndex();
	if (imageIndex >= frames.size())
		return;

	try
	{
		SwapchainFrame& frame = frames[imageIndex];
		const size_t lastIndex = passes.size() - 1;
		const PassRuntime& pr = passes[lastIndex];
		const u32 uboAlignment = (u32)ctx->GetUniformBufferAlignment();
		const vk::DeviceSize uboStride = (u32)sizeof(PostFxUbo) + align((u32)sizeof(PostFxUbo), uboAlignment);

		PostFxUbo ubo {};
		ubo.SourceSize[0] = (float)preparedInputExtent.width;
		ubo.SourceSize[1] = (float)preparedInputExtent.height;
		ubo.NativeSize[0] = (float)preparedNativeExtent.width;
		ubo.NativeSize[1] = (float)preparedNativeExtent.height;
		ubo.OutputSize[0] = (float)viewportW;
		ubo.OutputSize[1] = (float)viewportH;
		ubo.WindowSize[0] = (float)swapW;
		ubo.WindowSize[1] = (float)swapH;
		const u32 frameCount = FrameCount;
		ubo.Frame[0] = (float)frameCount;
		ubo.Frame[1] = (float)frameCount / 60.f;
		for (size_t i = 0; i < preset->params.size() && i < kMaxParams; i++)
		{
			float v = preset->params[i].defaultValue;
			(void)postfx::GetParamValue(preset->params[i].id, v);
			ubo.Params[i][0] = v;
		}
		frame.ubo->upload(sizeof(ubo), &ubo, (u32)(uboStride * lastIndex));

		if (!frame.descSets[lastIndex])
		{
			frame.descSets[lastIndex] = std::move(ctx->GetDevice().allocateDescriptorSetsUnique(
					vk::DescriptorSetAllocateInfo(ctx->GetDescriptorPool(), descSetLayout.get())).front());
		}

		std::array<vk::DescriptorImageInfo, VulkanPostFx::MaxSamplers> imageInfos;
		std::array<vk::WriteDescriptorSet, VulkanPostFx::MaxSamplers + 1> writes;
		for (size_t unit = 0; unit < VulkanPostFx::MaxSamplers; unit++)
		{
			vk::ImageView view = preparedInputView;
			postfx::Filter filter = (nearestFilter || pr.filter == postfx::Filter::Nearest)
					? postfx::Filter::Nearest : postfx::Filter::Linear;
			if (unit < pr.samplers.size())
			{
				const auto& sb = pr.samplers[unit];
				filter = sb.filter;
				if (nearestFilter)
					filter = postfx::Filter::Nearest;
				if (sb.source != "$prev")
				{
					auto it = preparedNamedViews.find(sb.source);
					if (it != preparedNamedViews.end())
						view = it->second;
				}
			}
			const vk::Sampler sampler = (filter == postfx::Filter::Linear) ? linearSampler.get() : nearestSampler.get();
			imageInfos[unit] = vk::DescriptorImageInfo(sampler, view, vk::ImageLayout::eShaderReadOnlyOptimal);
			writes[unit] = vk::WriteDescriptorSet(frame.descSets[lastIndex].get(), (u32)unit, 0,
					vk::DescriptorType::eCombinedImageSampler, imageInfos[unit]);
		}
		vk::DescriptorBufferInfo bufferInfo(frame.ubo->buffer.get(), uboStride * lastIndex, sizeof(PostFxUbo));
		writes[VulkanPostFx::MaxSamplers] = vk::WriteDescriptorSet(frame.descSets[lastIndex].get(),
				(u32)VulkanPostFx::MaxSamplers, 0, vk::DescriptorType::eUniformBuffer, nullptr, bufferInfo);
		ctx->GetDevice().updateDescriptorSets(writes, nullptr);

		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pr.pipeline.get());
		if (rotate90 && pr.pipelineRotate)
			cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pr.pipelineRotate.get());
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipelineLayout, 0, frame.descSets[lastIndex].get(), nullptr);

		cmd.setViewport(0, vk::Viewport((float)dx, (float)dy, (float)viewportW, (float)viewportH, 0.f, 1.f));
		cmd.setScissor(0, vk::Rect2D(vk::Offset2D(dx, dy), vk::Extent2D((u32)viewportW, (u32)viewportH)));

		if (passes.size() == 1)
		{
			float shiftX = 0.f;
			float shiftY = 0.f;
			getVideoShift(shiftX, shiftY);
			QuadVertex vtx[4] {
				{ -1, -1, 0, 0, 0 },
				{  1, -1, 0, 1, 0 },
				{ -1,  1, 0, 0, 1 },
				{  1,  1, 0, 1, 1 },
			};
			vtx[0].x = vtx[2].x = -1.f + shiftX * 2.f / (float)std::max(1u, preparedInputExtent.width);
			vtx[1].x = vtx[3].x = vtx[0].x + 2.f;
			vtx[0].y = vtx[1].y = -1.f + shiftY * 2.f / (float)std::max(1u, preparedInputExtent.height);
			vtx[2].y = vtx[3].y = vtx[0].y + 2.f;
			quadBuffer->Update(vtx);
		}
		else
		{
			quadBuffer->Update(nullptr);
		}
		quadBuffer->Bind(cmd);
		quadBuffer->Draw(cmd);
	}
	catch (const std::exception& e)
	{
		WARN_LOG(RENDERER, "PostFX Vulkan draw failed: %s", e.what());
		postfx::SetRuntimeError(std::string("Vulkan PostFX draw failed: ") + e.what());
		prepared = false;
		return;
	}
}

#endif // !LIBRETRO
