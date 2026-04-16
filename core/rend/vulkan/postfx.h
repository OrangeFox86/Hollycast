/*
	Copyright 2026

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
*/
#pragma once

#ifndef LIBRETRO
#include "vulkan.h"
#include "buffer.h"
#include "texture.h"
#include "rend/postfx/postfx_preset.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class VulkanPostFx
{
public:
	static constexpr size_t MaxSamplers = 8;
	void OnSwapchainRecreated(vk::Format format, u32 swapW, u32 swapH, u32 swapchainSize, vk::RenderPass swapchainRenderPass);
	void Term();

	// Records offscreen passes (if needed) and prepares the final-pass state.
	// Returns true if the final pass should be drawn via DrawFinalPass().
	bool Prepare(vk::CommandBuffer cmd, vk::ImageView srcView, const vk::Extent2D& srcExtent,
			int viewportW, int viewportH);

	// Draws the last pass into the currently active swapchain render pass.
	void DrawFinalPass(vk::CommandBuffer cmd, int dx, int dy, int viewportW, int viewportH, bool rotate90, bool nearestFilter);

private:
	struct PassRuntime
	{
		postfx::Filter filter = postfx::Filter::Linear;
		std::string renderTargetName;
		std::vector<postfx::PassDef::SamplerBinding> samplers;
		int outW = 0;
		int outH = 0;

		vk::UniqueShaderModule frag;
		vk::UniquePipeline pipeline;
		vk::UniquePipeline pipelineRotate;
	};
	struct SwapchainFrame
	{
		std::unique_ptr<BufferData> ubo;
		std::vector<vk::UniqueDescriptorSet> descSets;
		std::vector<FramebufferAttachment> attachments;
		std::vector<bool> attachmentReady;
		std::vector<vk::UniqueFramebuffer> framebuffers;
	};

	bool ensurePipelinesAndResources(vk::RenderPass swapchainRenderPass, int viewportW, int viewportH);
	void releaseSwapchainResources();
	void rebuildPassChain(vk::RenderPass swapchainRenderPass, int viewportW, int viewportH);

	vk::Format format = vk::Format::eUndefined;
	u32 swapW = 0;
	u32 swapH = 0;

	vk::UniqueRenderPass offscreenRenderPass;
	vk::RenderPass swapchainRenderPass = vk::RenderPass();

	vk::UniqueDescriptorSetLayout descSetLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueSampler linearSampler;
	vk::UniqueSampler nearestSampler;

	vk::UniqueShaderModule vertexShader;
	vk::UniqueShaderModule vertexShaderRotate;
	std::unique_ptr<class QuadBuffer> quadBuffer;

	std::vector<PassRuntime> passes;
	std::vector<SwapchainFrame> frames;

	uint64_t configVersion = 0;
	int cachedViewportW = 0;
	int cachedViewportH = 0;

	// Prepared for DrawFinalPass
	bool prepared = false;
	vk::ImageView preparedInputView;
	vk::Extent2D preparedInputExtent;
	vk::Extent2D preparedNativeExtent;
	std::unordered_map<std::string, vk::ImageView> preparedNamedViews;
};

#endif // !LIBRETRO
