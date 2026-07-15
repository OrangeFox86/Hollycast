/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include "gles.h"
#include "rend/postprocess/shader_chain.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// Executes the portable chain data on OpenGL. A GLSL pass is a fragment-shader
// body: HollyCast supplies Source, SourceSize, OutputSize, FrameCount, and the
// fullscreen vertex stage so package authors do not need backend boilerplate.
class GlShaderChain
{
public:
	~GlShaderChain();

	// A candidate replaces the active chain only after every pass has compiled.
	// The previous chain remains available when loading or compilation fails.
	bool activate(const postprocess::ShaderChain& chain, std::string& error);
	GlFramebuffer *render(GLuint sourceTexture, const postprocess::ShaderFrameInfo& frame,
			bool& presentationTransformApplied);
	bool hasActiveChain() const { return !programs.empty(); }
	void term();

private:
	class VertexArray final : public GlVertexArray
	{
	protected:
		void defineVtxAttribs() override;
	};

	struct ParameterUniform {
		GLint location = -1;
		std::string name;
		postprocess::SlangParameterType type = postprocess::SlangParameterType::Float;
		std::array<float, 16> values {};
	};
	struct ResourceUniform {
		GLint location = -1;
		postprocess::ShaderInputKind kind = postprocess::ShaderInputKind::Source;
		uint32_t sourcePass = postprocess::InvalidShaderPass;
		uint32_t sourceOutput = 0;
		uint32_t historyOffset = 1;
		std::string resourceName;
		GLuint unit = 0;
	};
	struct LookupTexture {
		postprocess::ShaderPresetTexture config;
		GLuint texture = 0;
	};
	struct Program {
		GLuint handle = 0;
		postprocess::ShaderPass pass;
		GLint sourceUniform = -1;
		GLint sourceSizeUniform = -1;
		GLint originalSizeUniform = -1;
		GLint outputSizeUniform = -1;
		GLint frameCountUniform = -1;
		GLint rotationUniform = -1;
		std::vector<ParameterUniform> parameters;
		std::vector<ResourceUniform> resources;
		std::vector<bool> outputNeedsMipmaps;
		uint32_t historyDepth = 1;
	};
	struct MrtTarget {
		std::vector<std::unique_ptr<GlFramebuffer>> outputs;
		GLuint framebuffer = 0;
		~MrtTarget() {
			if (framebuffer != 0)
				glDeleteFramebuffers(1, &framebuffer);
		}
	};
	using TargetRing = std::vector<std::unique_ptr<GlFramebuffer>>;
	using MrtTargetRing = std::vector<std::unique_ptr<MrtTarget>>;

	bool ensureResources();
	bool ensureMrtTarget(MrtTarget& target, const postprocess::ShaderPass& pass, int width, int height, std::string& error);
	bool ensureLookupTextures(std::string& error);
	bool ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error);
	void deleteLookupTextures();
	void deleteLookupTextures(std::vector<LookupTexture>& textures);
	void updateConfiguredChain();
	bool compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate, std::string& error);
	static void deletePrograms(std::vector<Program>& programs);

	std::vector<Program> programs;
	std::vector<std::string> configuredPaths;
	uint64_t configuredGeneration = 0;
	std::string activePath;
	std::vector<MrtTargetRing> targets;
	std::unique_ptr<GlFramebuffer> displayInput;
	TargetRing sourceHistory;
	uint32_t sourceHistoryDepth = 1;
	bool needsSourceHistory = false;
	bool wantsUnscaledInput = false;
	uint64_t parameterGeneration = 0;
	std::vector<LookupTexture> lookupTextures;
	std::unique_ptr<GlBuffer> vertexBuffer;
	GLuint copyProgram = 0;
	GLuint copyRotateProgram = 0;
	GLint copySourceUniform = -1;
	GLint copyRotateSourceUniform = -1;
	VertexArray vertexArray;
};
