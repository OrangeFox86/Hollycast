/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include "rend/postprocess/shader_chain.h"
#include "quad.h"
#include "texture.h"

#include <array>
#include <memory>
#include <vector>

class VulkanShaderChain
{
public:
	~VulkanShaderChain();

	// GLSL files are fragment bodies. HollyCast supplies Source, SourceSize,
	// OutputSize and FrameCount, matching the OpenGL executor contract.
	bool render(vk::Image sourceImage, vk::ImageView sourceView, const vk::Extent2D& extent,
			const postprocess::ShaderFrameInfo& frame, vk::ImageView& outputView);
	void term();

private:
	struct Target {
		// Allocation must outlive the image it backs, so it is declared first.
		Allocation allocation;
		vk::UniqueImage image;
		vk::UniqueImageView renderView;
		vk::UniqueImageView sampleView;
		// Source-history copies remain single-target and use this framebuffer.
		vk::UniqueFramebuffer framebuffer;
		vk::Extent2D extent {};
		vk::Format format = vk::Format::eUndefined;
		uint32_t mipLevels = 1;
		bool initialized = false;
	};
	struct TargetSet {
		std::vector<Target> outputs;
		vk::UniqueFramebuffer framebuffer;
	};
	struct Binding {
		postprocess::ShaderPassInput input;
		uint32_t descriptorBinding = 0;
	};
	struct Program {
		struct Output {
			postprocess::ShaderPassOutput config;
			vk::Format format = vk::Format::eR8G8B8A8Unorm;
			bool needsMipmaps = false;
			bool supportsMipmaps = false;
		};
		postprocess::ShaderPass pass;
		vk::UniqueShaderModule vertexShader;
		vk::UniqueShaderModule fragmentShader;
		vk::UniqueRenderPass renderPass;
		vk::UniquePipeline pipeline;
		std::vector<Output> outputs;
		std::vector<Binding> bindings;
		std::vector<postprocess::SlangParameter> parameters;
	};
	struct LookupTexture {
		postprocess::ShaderPresetTexture config;
		std::unique_ptr<Texture> texture;
		vk::UniqueSampler sampler;
	};

	bool updateConfiguredChain();
	bool compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate, std::string& error);
	bool ensureTarget(Target& target, const vk::Extent2D& extent, vk::Format format, bool mipmapped);
	bool ensureTargetSet(TargetSet& targets, const Program& program, const vk::Extent2D& extent);
	void generateMipmaps(vk::CommandBuffer commandBuffer, const Target& target, bool existingContents) const;
	vk::UniquePipeline createPipeline(vk::ShaderModule vertexShader, vk::ShaderModule fragmentShader, vk::RenderPass renderPass,
			const postprocess::ShaderPass& pass, size_t colorAttachmentCount) const;
	vk::UniqueRenderPass createRenderPass(const std::vector<vk::Format>& formats) const;
	bool ensureLookupTextures(vk::CommandBuffer commandBuffer, std::string& error);
	void ensureResources();

	vk::UniqueRenderPass renderPass;
	vk::UniqueDescriptorSetLayout descriptorSetLayout;
	vk::UniquePipelineLayout pipelineLayout;
	vk::UniqueSampler linearSampler;
	vk::UniqueSampler nearestSampler;
	vk::UniqueSampler mipLinearSampler;
	vk::UniqueSampler mipNearestSampler;
	vk::UniqueShaderModule copyFragmentShader;
	vk::UniquePipeline copyPipeline;
	std::unique_ptr<QuadBuffer> quadBuffer;
	std::vector<std::vector<vk::UniqueDescriptorSet>> descriptorSets;
	std::vector<vk::UniqueDescriptorSet> sourceDescriptorSets;
	std::vector<std::vector<std::vector<TargetSet>>> targets;
	std::vector<std::vector<Target>> sourceTargets;
	uint32_t historyDepth = 2;
	bool needsSourceHistory = false;
	std::vector<Program> programs;
	std::vector<LookupTexture> lookupTextures;
	std::vector<postprocess::ShaderParameter> parameters;
	std::string configuredPath;
	uint64_t configuredGeneration = 0;
	std::string activePath;
};
