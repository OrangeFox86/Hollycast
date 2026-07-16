/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

	Flycast is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
*/
#pragma once

#include "types.h"
#include "rend/postprocess/shader_chain.h"

#include <d3d11.h>

#include "windows/comptr.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class Quad;

class Dx11ShaderChain
{
public:
	void init(const ComPtr<ID3D11Device>& device, const ComPtr<ID3D11DeviceContext>& deviceContext, Quad *quad);
	bool render(ComPtr<ID3D11ShaderResourceView>& sourceView, const postprocess::ShaderFrameInfo& frame,
			ComPtr<ID3D11ShaderResourceView>& outputView, bool& presentationTransformApplied);
	void term();

private:
	struct ShaderFrameUniforms {
		float sourceSize[2];
		float outputSize[2];
		int frameCount;
		int rotation;
		float originalSize[2];
		float parameters[postprocess::MaxShaderParameterComponents];
	};
	struct Target {
		ComPtr<ID3D11Texture2D> texture;
		ComPtr<ID3D11RenderTargetView> view;
		ComPtr<ID3D11ShaderResourceView> shaderView;
		int width = 0;
		int height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		bool mipmapped = false;
		uint32_t renderedFrame = 0;
		bool initialized = false;
	};
	struct ResourceBinding {
		postprocess::ShaderPassInput input;
		UINT slot = 0;
		uint32_t lookupIndex = postprocess::InvalidShaderPass;
		ComPtr<ID3D11SamplerState> sampler;
		ComPtr<ID3D11SamplerState> mipSampler;
		bool useMipmaps = false;
	};
	struct LookupTexture {
		postprocess::ShaderPresetTexture config;
		ComPtr<ID3D11ShaderResourceView> view;
		ComPtr<ID3D11SamplerState> sampler;
	};
	struct Program {
		postprocess::ShaderPass pass;
		ComPtr<ID3D11VertexShader> vertexShader;
		ComPtr<ID3D11PixelShader> pixelShader;
		ComPtr<ID3D11BlendState> blendState;
		std::vector<ResourceBinding> resources;
		std::vector<postprocess::SlangParameter> parameters;
		std::vector<uint32_t> parameterIndices;
		std::vector<ComPtr<ID3D11ShaderResourceView>> resourceViews;
		std::vector<ComPtr<ID3D11SamplerState>> samplers;
		std::vector<bool> outputNeedsMipmaps;
		std::vector<DXGI_FORMAT> outputFormats;
		bool duckStationGlsl = false;
		uint32_t historyDepth = 1;
		int targetWidth = 0;
		int targetHeight = 0;
	};
	using TargetRing = std::vector<Target>;
	using PassTargets = std::vector<TargetRing>;

	bool updateConfiguredChain();
	bool compileCandidate(const postprocess::ShaderChain& chain, std::vector<Program>& candidate, std::string& error) const;
	bool ensureTarget(Target& target, int width, int height, DXGI_FORMAT format, bool mipmapped);
	bool prepareMipmappedSource(ComPtr<ID3D11ShaderResourceView>& sourceView, int width, int height,
			ComPtr<ID3D11ShaderResourceView>& mipmappedView);
	bool ensureLookupTextures(std::string& error);
	bool ensureLookupTextures(std::vector<LookupTexture>& textures, std::string& error);
	bool ensurePassSamplers(std::string& error);
	void unbindPixelShaderSource(UINT count = 1) const;

	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> deviceContext;
	ComPtr<ID3D11SamplerState> linearSampler;
	ComPtr<ID3D11SamplerState> nearestSampler;
	ComPtr<ID3D11SamplerState> mipLinearSampler;
	ComPtr<ID3D11SamplerState> mipNearestSampler;
	std::vector<PassTargets> targets;
	Target displayInput;
	Target mipmapSource;
	TargetRing sourceHistory;
	uint32_t sourceHistoryDepth = 1;
	bool needsSourceHistory = false;
	std::vector<Program> programs;
	std::vector<LookupTexture> lookupTextures;
	std::vector<postprocess::ShaderParameter> parameters;
	Quad *quad = nullptr;
	std::vector<std::string> configuredPaths;
	uint64_t configuredGeneration = 0;
	uint64_t parameterGeneration = 0;
	std::string activePath;
};
