/*
	Copyright 2021 flyinghead
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
#include "dx11_shaders.h"
#include "dx11context.h"
#include "stdclass.h"
#include "dx11_naomi2.h"
#include <cstdio>
#include <xxhash.h>

const char * const VertexShader = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif

struct VertexIn
{
	float4 pos : POSITION;
	float4 col : COLOR0;
	float4 spec : COLOR1;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	float4 uv : TEXCOORD0;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
};

cbuffer constantBuffer : register(b0)
{
	float4x4 transMatrix;
	float4 leftPlane;
	float4 topPlane;
	float4 rightPlane;
	float4 bottomPlane;
};

[clipplanes(leftPlane, topPlane, rightPlane, bottomPlane)]
VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = mul(transMatrix, float4(vin.pos.xyz, 1.f));
#if DIV_POS_Z == 1
	vo.pos /= vo.pos.z;
	vo.pos.z = vo.pos.w;
#endif
	vo.col = vin.col;
	vo.spec = vin.spec;
#if pp_Gouraud == 1 && DIV_POS_Z != 1
	vo.col *= vo.pos.z;
	vo.spec *= vo.pos.z;
#endif
	vo.uv.xyz = float3(vin.uv, 0.f);

#if DIV_POS_Z == 1
	vo.uv.w = vo.pos.w;
#else
	vo.uv.xy *= vo.pos.z;
	vo.uv.w = vo.pos.z;
	vo.pos.w = 1.f;
	vo.pos.z = 0.f;
#endif

	return vo;
}

)";

const char *ModVolVertexShader = R"(
struct VertexIn
{
	float4 pos : POSITION;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	float4 uv : TEXCOORD0;
};

cbuffer constantBuffer : register(b0)
{
	float4x4 transMatrix;
	float4 leftPlane;
	float4 topPlane;
	float4 rightPlane;
	float4 bottomPlane;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
	vo.pos = mul(transMatrix, float4(vin.pos.xyz, 1.f));
#if DIV_POS_Z == 1
	vo.pos /= vo.pos.z;
	vo.pos.z = vo.pos.w;
	vo.uv = float4(0.f, 0.f, 0.f, vo.pos.w);
#else
	vo.uv = float4(0.f, 0.f, 0.f, vo.pos.z);
	vo.pos.w = 1.f;
	vo.pos.z = 0.f;
#endif
	return vo;
}

)";

const char * const PixelShaderCommonPart1 = R"(
#if pp_Gouraud == 1
#define INTERPOLATION
#else
#define INTERPOLATION nointerpolation
#endif

#define PI 3.1415926f

Texture2D texture0 : register(t0);
sampler sampler0 : register(s0);

Texture2D paletteTexture : register(t1);
sampler paletteSampler : register(s1);

Texture2D fogTexture : register(t2);
sampler fogSampler : register(s2);

float fog_mode2(float w)
{
	float z = clamp(
#if DIV_POS_Z == 1
					fogDensity / w
#else
					fogDensity * w
#endif
									, 1.0f, 255.9999f);
	float exp = floor(log2(z));
	float m = z * 16.0f / pow(2.0, exp) - 16.0f;
	float idx = floor(m) + exp * 16.0f + 0.5f;
	float4 fogCoef = fogTexture.Sample(fogSampler, float2(idx / 128.0f, 0.75f - (m - floor(m)) / 2.0f));
	return fogCoef.a;
}

float4 clampColor(float4 color)
{
#if FogClamping == 1
	return clamp(color, colorClampMin, colorClampMax);
#else
	return color;
#endif
}

#if pp_Palette != 0

float4 getPaletteEntry(float colIdx)
{
	uint colorIdx = int(floor(colIdx * 255.0f + 0.5f) + paletteIndex);
    float2 c = float2((fmod(float(colorIdx), 32.0f) * 2.0f + 1.0f) / 64.0f, (float(colorIdx / 32) * 2.0f + 1.0f) / 64.0f);
	return paletteTexture.Sample(paletteSampler, c);
}

#endif

#if pp_Palette == 1

float4 palettePixel(Texture2D tex, sampler texSampler, float2 coords)
{
	return getPaletteEntry(tex.Sample(texSampler, coords).a);
}

#elif pp_Palette == 2

float4 palettePixelBilinear(Texture2D tex, sampler texSampler, float2 coords)
{
	float2 textureSize;
	tex.GetDimensions(textureSize.x, textureSize.y);
	float2 pixCoord = coords * textureSize - 0.5f;				// coordinates of top left pixel
	float2 originPixCoord = floor(pixCoord);

	float2 sampleUV = (originPixCoord + 0.5f) / textureSize;	// UV coordinates of center of top left pixel

    // Sample from all surrounding texels
    float4 c00 = getPaletteEntry(tex.Sample(texSampler, sampleUV).a);
    float4 c01 = getPaletteEntry(tex.Sample(texSampler, sampleUV, int2(0, 1)).a);
    float4 c11 = getPaletteEntry(tex.Sample(texSampler, sampleUV, int2(1, 1)).a);
    float4 c10 = getPaletteEntry(tex.Sample(texSampler, sampleUV, int2(1, 0)).a);

	float2 weight = pixCoord - originPixCoord;

    // Bi-linear mixing
    float4 temp0 = lerp(c00, c10, weight.x);
    float4 temp1 = lerp(c01, c11, weight.x);
    return lerp(temp0, temp1, weight.y);
}

#endif

#if pp_Texture == 1 && pp_SampleTextureFilter != 0

float4 sampleSourceTexel(Texture2D sourceTex, sampler sourceSampler, float2 uv)
{
#if pp_Palette == 0
	return sourceTex.Sample(sourceSampler, uv);
#else
	return getPaletteEntry(sourceTex.Sample(sourceSampler, uv).a);
#endif
}

float4 legacyTextureSample(Texture2D sourceTex, sampler sourceSampler, float2 uv)
{
#if pp_Palette == 0
	return sourceTex.Sample(sourceSampler, uv);
#else
#if pp_Palette == 1
	return palettePixel(sourceTex, sourceSampler, uv);
#else
	return palettePixelBilinear(sourceTex, sourceSampler, uv);
#endif
#endif
}

bool sampleTextureFilterEligible(Texture2D sourceTex)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float maxFilteredTextureSize = float(pp_MaxFilteredTextureSize);
	// Match the xBRZ size buckets by texture width: 8x16 matches 8x8, while 16x8 matches 16x16.
	return pp_MaxFilteredTextureSize >= 1024 || (float)texWidth <= maxFilteredTextureSize;
}

float jincDistance(float2 a, float2 b)
{
	float2 v = b - a;
	return sqrt(dot(v, v));
}

float4 jincResampler(float4 x)
{
	// Windowed Jinc2 constants adapted from Hyllian's MIT-licensed shader.
	const float wa = 1.382300768f;
	const float wb = 2.576105976f;
	const float wawb = wa * wb;
	const float4 a = sin(x * wa) * sin(x * wb) / (x * x);
	bool4 zero = x == float4(0.0f, 0.0f, 0.0f, 0.0f);
	return float4(zero.x ? wawb : a.x, zero.y ? wawb : a.y, zero.z ? wawb : a.z, zero.w ? wawb : a.w);
}

float4 alphaWeightedBilinear(Texture2D sourceTex, sampler sourceSampler, float2 uv, bool noEdgeBlend)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 pixCoord = uv * texSize - 0.5f;
	float2 originPixCoord = floor(pixCoord);
	float2 sampleUV = (originPixCoord + 0.5f) / texSize;
	float2 texel = float2(1.0f, 1.0f) / texSize;

	float4 c00 = sampleSourceTexel(sourceTex, sourceSampler, sampleUV);
	float4 c10 = sampleSourceTexel(sourceTex, sourceSampler, sampleUV + float2(texel.x, 0.0f));
	float4 c01 = sampleSourceTexel(sourceTex, sourceSampler, sampleUV + float2(0.0f, texel.y));
	float4 c11 = sampleSourceTexel(sourceTex, sourceSampler, sampleUV + texel);

	float2 weight = pixCoord - originPixCoord;
	float4 p00 = float4(c00.rgb * c00.a, c00.a);
	float4 p10 = float4(c10.rgb * c10.a, c10.a);
	float4 p01 = float4(c01.rgb * c01.a, c01.a);
	float4 p11 = float4(c11.rgb * c11.a, c11.a);
	float4 color = lerp(lerp(p00, p10, weight.x), lerp(p01, p11, weight.x), weight.y);

	if (color.a > 0.0f)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5f ? 1.0f : 0.0f;
	return color;
}

float4 jinc2Sample(Texture2D sourceTex, sampler sourceSampler, float2 uv, bool noEdgeBlend)
{
	const float arStrength = 0.8f;
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 pc = uv * texSize;
	float2 tc = floor(pc - float2(0.5f, 0.5f)) + float2(0.5f, 0.5f);
	float2 dx = float2(1.0f, 0.0f);
	float2 dy = float2(0.0f, 1.0f);

	float4 w0 = jincResampler(float4(
		jincDistance(pc, tc - dx - dy),
		jincDistance(pc, tc - dy),
		jincDistance(pc, tc + dx - dy),
		jincDistance(pc, tc + dx * 2.0f - dy)));
	float4 w1 = jincResampler(float4(
		jincDistance(pc, tc - dx),
		jincDistance(pc, tc),
		jincDistance(pc, tc + dx),
		jincDistance(pc, tc + dx * 2.0f)));
	float4 w2 = jincResampler(float4(
		jincDistance(pc, tc - dx + dy),
		jincDistance(pc, tc + dy),
		jincDistance(pc, tc + dx + dy),
		jincDistance(pc, tc + dx * 2.0f + dy)));
	float4 w3 = jincResampler(float4(
		jincDistance(pc, tc - dx + dy * 2.0f),
		jincDistance(pc, tc + dy * 2.0f),
		jincDistance(pc, tc + dx + dy * 2.0f),
		jincDistance(pc, tc + dx * 2.0f + dy * 2.0f)));

	float2 baseUV = tc / texSize;
	dx = float2(1.0f / texSize.x, 0.0f);
	dy = float2(0.0f, 1.0f / texSize.y);

	float4 c00 = sampleSourceTexel(sourceTex, sourceSampler, baseUV - dx - dy);
	float4 c10 = sampleSourceTexel(sourceTex, sourceSampler, baseUV - dy);
	float4 c20 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx - dy);
	float4 c30 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx * 2.0f - dy);
	float4 c01 = sampleSourceTexel(sourceTex, sourceSampler, baseUV - dx);
	float4 c11 = sampleSourceTexel(sourceTex, sourceSampler, baseUV);
	float4 c21 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx);
	float4 c31 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx * 2.0f);
	float4 c02 = sampleSourceTexel(sourceTex, sourceSampler, baseUV - dx + dy);
	float4 c12 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dy);
	float4 c22 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx + dy);
	float4 c32 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx * 2.0f + dy);
	float4 c03 = sampleSourceTexel(sourceTex, sourceSampler, baseUV - dx + dy * 2.0f);
	float4 c13 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dy * 2.0f);
	float4 c23 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx + dy * 2.0f);
	float4 c33 = sampleSourceTexel(sourceTex, sourceSampler, baseUV + dx * 2.0f + dy * 2.0f);

	c00.rgb *= c00.a; c10.rgb *= c10.a; c20.rgb *= c20.a; c30.rgb *= c30.a;
	c01.rgb *= c01.a; c11.rgb *= c11.a; c21.rgb *= c21.a; c31.rgb *= c31.a;
	c02.rgb *= c02.a; c12.rgb *= c12.a; c22.rgb *= c22.a; c32.rgb *= c32.a;
	c03.rgb *= c03.a; c13.rgb *= c13.a; c23.rgb *= c23.a; c33.rgb *= c33.a;

	float wsum = max(
		dot(w0, float4(1.0f, 1.0f, 1.0f, 1.0f)) + dot(w1, float4(1.0f, 1.0f, 1.0f, 1.0f))
		+ dot(w2, float4(1.0f, 1.0f, 1.0f, 1.0f)) + dot(w3, float4(1.0f, 1.0f, 1.0f, 1.0f)),
		0.000001f);
	float4 color = float4(
		dot(w0, float4(c00.r, c10.r, c20.r, c30.r)) + dot(w1, float4(c01.r, c11.r, c21.r, c31.r)) + dot(w2, float4(c02.r, c12.r, c22.r, c32.r)) + dot(w3, float4(c03.r, c13.r, c23.r, c33.r)),
		dot(w0, float4(c00.g, c10.g, c20.g, c30.g)) + dot(w1, float4(c01.g, c11.g, c21.g, c31.g)) + dot(w2, float4(c02.g, c12.g, c22.g, c32.g)) + dot(w3, float4(c03.g, c13.g, c23.g, c33.g)),
		dot(w0, float4(c00.b, c10.b, c20.b, c30.b)) + dot(w1, float4(c01.b, c11.b, c21.b, c31.b)) + dot(w2, float4(c02.b, c12.b, c22.b, c32.b)) + dot(w3, float4(c03.b, c13.b, c23.b, c33.b)),
		dot(w0, float4(c00.a, c10.a, c20.a, c30.a)) + dot(w1, float4(c01.a, c11.a, c21.a, c31.a)) + dot(w2, float4(c02.a, c12.a, c22.a, c32.a)) + dot(w3, float4(c03.a, c13.a, c23.a, c33.a))) / wsum;

	float4 unclamped = color;
	color = lerp(unclamped, clamp(color, min(min(c11, c21), min(c12, c22)), max(max(c11, c21), max(c12, c22))), arStrength);
	if (color.a > 0.0f)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5f ? 1.0f : 0.0f;
	return color;
}

float4 clampToSourceNeighborhood(Texture2D sourceTex, sampler sourceSampler, float2 uv, float4 color)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 base = floor(uv * texSize);
	float2 invTexSize = float2(1.0f, 1.0f) / texSize;
	float4 minColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
	float4 maxColor = float4(0.0f, 0.0f, 0.0f, 0.0f);

	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			float4 sampleColor = sampleSourceTexel(sourceTex, sourceSampler, (base + float2((float)x, (float)y) + 0.5f) * invTexSize);
			minColor = min(minColor, sampleColor);
			maxColor = max(maxColor, sampleColor);
		}
	}
	return clamp(color, minColor, maxColor);
}

float4 sharpenAgainstCenter(Texture2D sourceTex, sampler sourceSampler, float2 uv, float4 color, float strength)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 centerUV = (floor(uv * texSize) + 0.5f) / texSize;
	float4 center = sampleSourceTexel(sourceTex, sourceSampler, centerUV);
	color.rgb += (color.rgb - center.rgb) * strength;
	return color;
}

float4 jinc2AntiRingSample(Texture2D sourceTex, sampler sourceSampler, float2 uv, float sharpness)
{
	float4 color = jinc2Sample(sourceTex, sourceSampler, uv, false);
	if (sharpness > 0.0f)
		color = sharpenAgainstCenter(sourceTex, sourceSampler, uv, color, sharpness);
	return clampToSourceNeighborhood(sourceTex, sourceSampler, uv, color);
}

float sincWeight(float x)
{
	x = abs(x);
	if (x < 0.00001f)
		return 1.0f;
	float pix = PI * x;
	return sin(pix) / pix;
}
)";
const char * const PixelShaderCommonPart2 = R"(
float lanczosWeight(float x, float radius)
{
	x = abs(x);
	if (x >= radius)
		return 0.0f;
	return sincWeight(x) * sincWeight(x / radius);
}

float spline36Weight(float x)
{
	x = abs(x);
	if (x < 1.0f)
		return ((13.0f / 11.0f * x - 453.0f / 209.0f) * x) * x + 1.0f;
	if (x < 2.0f)
		return ((-6.0f / 11.0f * x + 270.0f / 209.0f) * x - 156.0f / 209.0f) * x - 3.0f / 209.0f;
	if (x < 3.0f)
		return ((1.0f / 11.0f * x - 45.0f / 209.0f) * x + 26.0f / 209.0f) * x - 1.0f / 209.0f;
	return 0.0f;
}

float cubicWeight(float x, float b, float c)
{
	x = abs(x);
	if (x < 1.0f)
		return ((12.0f - 9.0f * b - 6.0f * c) * x * x * x
			+ (-18.0f + 12.0f * b + 6.0f * c) * x * x
			+ (6.0f - 2.0f * b)) / 6.0f;
	if (x < 2.0f)
		return ((-b - 6.0f * c) * x * x * x
			+ (6.0f * b + 30.0f * c) * x * x
			+ (-12.0f * b - 48.0f * c) * x
			+ (8.0f * b + 24.0f * c)) / 6.0f;
	return 0.0f;
}

float reconstructionWeight(float x, int filterMode)
{
	if (filterMode == 0)
		return lanczosWeight(x, 2.0f);
	if (filterMode == 1)
		return lanczosWeight(x, 3.0f);
	if (filterMode == 2)
		return spline36Weight(x);
	if (filterMode == 3)
		return cubicWeight(x, 0.378215755f, 0.310892122f); // Robidoux
	if (filterMode == 4)
		return cubicWeight(x, 0.262014512f, 0.368992744f); // Robidoux Sharp
	if (filterMode == 5)
		return cubicWeight(x, 0.0f, 0.5f); // Catmull-Rom
	return cubicWeight(x, 1.0f / 3.0f, 1.0f / 3.0f); // Mitchell-Netravali
}

float4 separableReconstructionSample(Texture2D sourceTex, sampler sourceSampler, float2 uv, int filterMode, bool antiRing)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 pc = uv * texSize - 0.5f;
	float2 base = floor(pc);
	float2 f = pc - base;
	float2 invTexSize = float2(1.0f, 1.0f) / texSize;
	float4 color = float4(0.0f, 0.0f, 0.0f, 0.0f);
	float weightSum = 0.0f;

	for (int y = -2; y <= 3; y++)
	{
		float wy = reconstructionWeight(float(y) - f.y, filterMode);
		for (int x = -2; x <= 3; x++)
		{
			float weight = reconstructionWeight(float(x) - f.x, filterMode) * wy;
			float4 sampleColor = sampleSourceTexel(sourceTex, sourceSampler, (base + float2((float)x, (float)y) + 0.5f) * invTexSize);
			sampleColor.rgb *= sampleColor.a;
			color += sampleColor * weight;
			weightSum += weight;
		}
	}

	if (abs(weightSum) > 0.00001f)
		color /= weightSum;
	if (color.a > 0.0f)
		color.rgb /= color.a;
	color = clamp(color, 0.0f, 1.0f);
	if (antiRing)
		color = clampToSourceNeighborhood(sourceTex, sourceSampler, uv, color);
	return color;
}

uint packSourceTexel(Texture2D sourceTex, sampler sourceSampler, float2 baseUV, float2 texelOffset, float2 invTexSize)
{
	float4 value = clamp(sampleSourceTexel(sourceTex, sourceSampler, baseUV + texelOffset * invTexSize), 0.0f, 1.0f);
	return (uint(round(value.r * 255.f))) | (uint(round(value.g * 255.f)) << 8) | (uint(round(value.b * 255.f)) << 16) | (uint(round(value.a * 255.f)) << 24);
}

uint mmpxLuma(uint color)
{
	uint alpha = (color & 0xFF000000u) >> 24;
	return (((color & 0x000000FFu) + ((color & 0x0000FF00u) >> 8) + ((color & 0x00FF0000u) >> 16) + 1u) * (256u - alpha));
}

bool mmpxAllEq2(uint value, uint a, uint b) { return ((value ^ a) | (value ^ b)) == 0u; }
bool mmpxAllEq3(uint value, uint a, uint b, uint c) { return ((value ^ a) | (value ^ b) | (value ^ c)) == 0u; }
bool mmpxAllEq4(uint value, uint a, uint b, uint c, uint d) { return ((value ^ a) | (value ^ b) | (value ^ c) | (value ^ d)) == 0u; }
bool mmpxAnyEq3(uint value, uint a, uint b, uint c) { return value == a || value == b || value == c; }
bool mmpxNoneEq4(uint value, uint a, uint b, uint c, uint d) { return value != a && value != b && value != c && value != d; }
bool mmpxNoneEq2(uint value, uint a, uint b) { return value != a && value != b; }

float4 unpackSourceTexel(in uint color)
{
	return float4(
		float(color & 0x000000FFu) / 255.0f,
		float((color & 0x0000FF00u) >> 8) / 255.0f,
		float((color & 0x00FF0000u) >> 16) / 255.0f,
		float((color & 0xFF000000u) >> 24) / 255.0f);
}

float4 mmpxResultColor(uint result, uint center)
{
	float4 color = unpackSourceTexel(result);
	if (pp_SampleTextureFilter == 6 && result != center)
		color = lerp(color, unpackSourceTexel(center), 0.25f);
	return color;
}

float4 mmpxSample(Texture2D sourceTex, sampler sourceSampler, float2 uv)
{
	uint texWidth, texHeight;
	sourceTex.GetDimensions(texWidth, texHeight);
	float2 texSize = float2((float)texWidth, (float)texHeight);
	float2 bcoords = floor(uv * texSize);
	float2 baseUV = (bcoords + 0.5f) / texSize;
	float2 invTexSize = float2(1.0f, 1.0f) / texSize;

	// MMPX is a MIT-licensed style-preserving pixel-art magnifier by Morgan McGuire and Mara Gagiu.
	uint A = packSourceTexel(sourceTex, sourceSampler, baseUV, float2(-1.0f, -1.0f), invTexSize);
	uint B = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f, -1.0f), invTexSize);
	uint C = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 1.0f, -1.0f), invTexSize);
	uint D = packSourceTexel(sourceTex, sourceSampler, baseUV, float2(-1.0f,  0.0f), invTexSize);
	uint E = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f,  0.0f), invTexSize);
	uint F = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 1.0f,  0.0f), invTexSize);
	uint G = packSourceTexel(sourceTex, sourceSampler, baseUV, float2(-1.0f,  1.0f), invTexSize);
	uint H = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f,  1.0f), invTexSize);
	uint I = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 1.0f,  1.0f), invTexSize);
	uint J = E, K = E, L = E, M = E;

	if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0u)
	{
)";
const char * const PixelShaderCommonPart3 = R"(
		uint P = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f, -2.0f), invTexSize);
		uint Q = packSourceTexel(sourceTex, sourceSampler, baseUV, float2(-2.0f,  0.0f), invTexSize);
		uint R = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 2.0f,  0.0f), invTexSize);
		uint S = packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f,  2.0f), invTexSize);
		uint Bl = mmpxLuma(B), Dl = mmpxLuma(D), El = mmpxLuma(E), Fl = mmpxLuma(F), Hl = mmpxLuma(H);

		if ((D == B && D != H && D != F) && (El >= Dl || E == A) && mmpxAnyEq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
		if ((B == F && B != D && B != H) && (El >= Bl || E == C) && mmpxAnyEq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
		if ((H == D && H != F && H != B) && (El >= Hl || E == G) && mmpxAnyEq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
		if ((F == H && F != B && F != D) && (El >= Fl || E == I) && mmpxAnyEq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;

		if (pp_SampleTextureFilter == 7)
		{
			if ((E != F && mmpxAllEq4(E, C, I, D, Q) && mmpxAllEq2(F, B, H)) && (F != packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 3.0f,  0.0f), invTexSize))) K = M = F;
			if ((E != D && mmpxAllEq4(E, A, G, F, R) && mmpxAllEq2(D, B, H)) && (D != packSourceTexel(sourceTex, sourceSampler, baseUV, float2(-3.0f,  0.0f), invTexSize))) J = L = D;
			if ((E != H && mmpxAllEq4(E, G, I, B, P) && mmpxAllEq2(H, D, F)) && (H != packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f,  3.0f), invTexSize))) L = M = H;
			if ((E != B && mmpxAllEq4(E, A, C, H, S) && mmpxAllEq2(B, D, F)) && (B != packSourceTexel(sourceTex, sourceSampler, baseUV, float2( 0.0f, -3.0f), invTexSize))) J = K = B;
			if (Bl < El && mmpxAllEq4(E, G, H, I, S) && mmpxNoneEq4(E, A, D, C, F)) J = K = B;
			if (Hl < El && mmpxAllEq4(E, A, B, C, P) && mmpxNoneEq4(E, D, G, I, F)) L = M = H;
			if (Fl < El && mmpxAllEq4(E, A, D, G, Q) && mmpxNoneEq4(E, B, C, I, H)) K = M = F;
			if (Dl < El && mmpxAllEq4(E, C, F, I, R) && mmpxNoneEq4(E, B, A, G, H)) J = L = D;
		}
	}

	float2 fpart = frac(uv * texSize);
	uint result = (fpart.x < 0.5f) ? ((fpart.y < 0.5f) ? J : L) : ((fpart.y < 0.5f) ? K : M);
	return mmpxResultColor(result, E);
}

float4 filteredTextureSample(Texture2D sourceTex, sampler sourceSampler, float2 uv)
{
	if (!sampleTextureFilterEligible(sourceTex))
		return legacyTextureSample(sourceTex, sourceSampler, uv);

	if (pp_SampleTextureFilter == 1)
		return alphaWeightedBilinear(sourceTex, sourceSampler, uv, false);
	if (pp_SampleTextureFilter == 2)
		return alphaWeightedBilinear(sourceTex, sourceSampler, uv, true);
	if (pp_SampleTextureFilter == 3)
		return jinc2Sample(sourceTex, sourceSampler, uv, false);
	if (pp_SampleTextureFilter == 4)
		return jinc2Sample(sourceTex, sourceSampler, uv, true);
	if (pp_SampleTextureFilter == 5 || pp_SampleTextureFilter == 6 || pp_SampleTextureFilter == 7)
		return mmpxSample(sourceTex, sourceSampler, uv);
	if (pp_SampleTextureFilter == 8)
		return jinc2AntiRingSample(sourceTex, sourceSampler, uv, 0.0f);
	if (pp_SampleTextureFilter == 9)
		return jinc2AntiRingSample(sourceTex, sourceSampler, uv, 0.18f);
	if (pp_SampleTextureFilter == 10)
		return jinc2AntiRingSample(sourceTex, sourceSampler, uv, 0.08f);
	if (pp_SampleTextureFilter == 11)
		return jinc2AntiRingSample(sourceTex, sourceSampler, uv, 0.22f);
	if (pp_SampleTextureFilter == 12)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 0, true);
)";
const char * const PixelShaderCommonPart4 = R"(
	if (pp_SampleTextureFilter == 13)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 1, true);
	if (pp_SampleTextureFilter == 14)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 2, false);
	if (pp_SampleTextureFilter == 15)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 3, false);
	if (pp_SampleTextureFilter == 16)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 4, false);
	if (pp_SampleTextureFilter == 17)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 5, false);
	if (pp_SampleTextureFilter == 18)
		return separableReconstructionSample(sourceTex, sourceSampler, uv, 6, false);
	return legacyTextureSample(sourceTex, sourceSampler, uv);
}
)";
const char * const PixelShaderCommonPart5 = R"(

#endif

)";

extern const std::string PixelShaderCommon =
	std::string(PixelShaderCommonPart1)
	+ PixelShaderCommonPart2 + PixelShaderCommonPart3 + PixelShaderCommonPart4 + PixelShaderCommonPart5;

const char * const PixelShader = R"(

cbuffer constantBuffer : register(b0)
{
	float4 colorClampMin;
	float4 colorClampMax;
	float4 FOG_COL_VERT;
	float4 FOG_COL_RAM;
	float4 ditherDivisor;
	float fogDensity;
	float shadowScale;
	float alphaTestValue;
};

cbuffer polyConstantBuffer : register(b1)
{
	float4 clipTest;
	float paletteIndex;
	float trilinearAlpha;
};

#include "pixel_common.hlsl"

struct Pixel 
{
	float4 pos : SV_POSITION;
	float4 uv : TEXCOORD0;
	INTERPOLATION float4 col : COLOR0;
	INTERPOLATION float4 spec : COLOR1;
};

struct PSO
{
	float4 col : SV_TARGET;
	float z : SV_DEPTH;
};

PSO main(in Pixel inpix)
{
#if pp_ClipInside == 1
	// Clip inside the box
	if (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z
			&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)
		discard;
#endif
	float4 color = inpix.col;
	#if pp_BumpMap == 1 || pp_Offset == 1
		float4 specular = inpix.spec;
	#endif
	#if pp_Gouraud == 1 && DIV_POS_Z != 1
		color /= inpix.uv.w;
		#if pp_BumpMap == 1 || pp_Offset == 1
			specular /= inpix.uv.w;
		#endif
	#endif
	#if pp_UseAlpha == 0
		color.a = 1.0f;
	#endif
	#if pp_FogCtrl == 3
		color = float4(FOG_COL_RAM.rgb, fog_mode2(inpix.uv.w));
	#endif
	#if pp_Texture == 1
	{
		float2 uv = inpix.uv.xy;
		#if DIV_POS_Z != 1
			uv /= inpix.uv.w;
		#endif
		#if pp_SampleTextureFilter != 0
			float4 texcol = filteredTextureSample(texture0, sampler0, uv);
		#else
			#if pp_Palette == 0
				float4 texcol = texture0.Sample(sampler0, uv);
			#elif pp_Palette == 1
				float4 texcol = palettePixel(texture0, sampler0, uv);
			#else
				float4 texcol = palettePixelBilinear(texture0, sampler0, uv);
			#endif
		#endif
		
		#if pp_BumpMap == 1
			float s = PI / 2.0f * (texcol.a * 15.0f * 16.0f + texcol.r * 15.0f) / 255.0f;
			float r = 2.0f * PI * (texcol.g * 15.0f * 16.0f + texcol.b * 15.0f) / 255.0f;
			texcol[3] = clamp(specular.a + specular.r * sin(s) + specular.g * cos(s) * cos(r - 2.0f * PI * specular.b), 0.0f, 1.0f);
			texcol.rgb = float3(1.0f, 1.0f, 1.0f);	
		#else
			#if pp_IgnoreTexA == 1
				texcol.a = 1.0f;
			#endif
		#endif
		#if pp_ShadInstr == 0
			color = texcol;
		#endif
		#if pp_ShadInstr == 1
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		#endif
		#if pp_ShadInstr == 2
			color.rgb = lerp(color.rgb, texcol.rgb, texcol.a);
		#endif
		#if  pp_ShadInstr == 3
			color *= texcol;
		#endif
		
		#if pp_Offset == 1 && pp_BumpMap == 0
			color.rgb += specular.rgb;
		#endif
	}
	#endif
	
	color = clampColor(color);
	
	#if pp_FogCtrl == 0
		color.rgb = lerp(color.rgb, FOG_COL_RAM.rgb, fog_mode2(inpix.uv.w)); 
	#endif
	#if pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0
		color.rgb = lerp(color.rgb, FOG_COL_VERT.rgb, specular.a);
	#endif
	
	#if pp_TriLinear == 1
	color *= trilinearAlpha;
	#endif

	#if cp_AlphaTest == 1
		color.a = round(color.a * 255.0f) * 0.0039215688593685626983642578125; // 1 / 255
		if (alphaTestValue > color.a)
			discard;
		color.a = 1.0f;
	#endif

#if DITHERING == 1
	static const float ditherTable[16] = {
		5.0f, 13.0f,  7.0f, 15.0f,
		9.0f,  1.0f, 11.0f,  3.0f,
		6.0f, 14.0f,  4.0f, 12.0f,
		10.0f, 2.0f,  8.0f,  0.0f
	};
	float r = ditherTable[int(inpix.pos.y % 4.0f) * 4 + int(inpix.pos.x % 4.0f)];
	float4 dv = float4(r, r, r, 1.0f) / ditherDivisor;
	color = clamp(floor(color * 255.0f + dv) / 255.0f, 0.0f, 1.0f);
#endif
	PSO pso;
#if DIV_POS_Z == 1
	float w = 100000.0f / inpix.uv.w;
#else
	float w = 100000.0f * inpix.uv.w;
#endif
	pso.z = log2(1.0f + max(w, -0.999999f)) / 34.0f;
	pso.col = color;

	return pso;
}

struct MVPixel 
{
	float4 pos : SV_POSITION;
	float4 uv : TEXCOORD0;
};

PSO modifierVolume(in MVPixel inpix)
{
	PSO pso;
#if DIV_POS_Z == 1
	float w = 100000.0f / inpix.uv.w;
#else
	float w = 100000.0f * inpix.uv.w;
#endif
	pso.z = log2(1.0f + max(w, -0.999999f)) / 34.0f;
	pso.col = float4(0, 0, 0, 1.f - shadowScale);

	return pso;
}
)";

const char * const QuadVertexShader = R"(
struct VertexIn
{
	float2 pos : POSITION;
	float2 uv : TEXCOORD0;
};

struct VertexOut
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

VertexOut main(in VertexIn vin)
{
	VertexOut vo;
#if ROTATE == 0
	vo.pos = float4(vin.pos, 0.f, 1.f);
#else
	vo.pos = float4(-vin.pos.y, vin.pos.x, 0.f, 1.f);
#endif
	vo.uv = vin.uv;

	return vo;
}
)";

const char * const QuadPixelShader = R"(
cbuffer constantBuffer : register(b0)
{
	float4 color;
};

struct VertexIn
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
};

sampler sampler0;
Texture2D texture0;

float4 main(in VertexIn vin) : SV_Target
{
	return color * texture0.Sample(sampler0, vin.uv);
}

)";

struct IncludeManager : public ID3DInclude
{
	HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID *ppData, UINT *pBytes) override
	{
		if (!strcmp(pFileName, "pixel_common.hlsl"))
		{
			*ppData = PixelShaderCommon.c_str();
			*pBytes = (UINT)PixelShaderCommon.size();
			return S_OK;
		}
		return E_FAIL;
	}

	HRESULT STDMETHODCALLTYPE Close(LPCVOID pData) override {
		return S_OK;
	}
};

const char * const MacroValues[] { "0", "1", "2", "3" };

enum VertexMacroEnum {
	MacroGouraud,
	MacroDivPosZ,
	MacroPositionOnly,
	MacroTwoVolumes,
	MacroLightOn,
	MacroModifierVolume,
};

static D3D_SHADER_MACRO VertexMacros[]
{
	{ "pp_Gouraud", "1" },
	{ "DIV_POS_Z", "0" },
	{ "POSITION_ONLY", "0" },
	{ "pp_TwoVolumes", "0" },
	{ "LIGHT_ON", "1" },
	{ "MODIFIER_VOLUME", "0" },
	{ nullptr, nullptr }
};

enum PixelMacroEnum {
	MacroTexture = 2,
	MacroUseAlpha,
	MacroIgnoreTexA,
	MacroShadInstr,
	MacroOffset,
	MacroFogCtrl,
	MacroBumpMap,
	MacroFogClamping,
	MacroTriLinear,
	MacroPalette,
	MacroAlphaTest,
	MacroClipInside,
	MacroDithering,
	MacroSampleTextureFilter,
	MacroMaxFilteredTextureSize
};

static D3D_SHADER_MACRO PixelMacros[]
{
	{ "pp_Gouraud", "1" },
	{ "DIV_POS_Z", "0" },
	{ "pp_Texture", "0" },
	{ "pp_UseAlpha", "0" },
	{ "pp_IgnoreTexA", "0" },
	{ "pp_ShadInstr", "0" },
	{ "pp_Offset", "0" },
	{ "pp_FogCtrl", "0" },
	{ "pp_BumpMap", "0" },
	{ "FogClamping", "0" },
	{ "pp_TriLinear", "0" },
	{ "pp_Palette", "0" },
	{ "cp_AlphaTest", "0" },
	{ "pp_ClipInside", "0" },
	{ "DITHERING", "0" },
	{ "pp_SampleTextureFilter", "0" },
	{ "pp_MaxFilteredTextureSize", "0" },
	{ nullptr, nullptr }
};

const ComPtr<ID3D11PixelShader>& DX11Shaders::getShader(bool pp_Texture, bool pp_UseAlpha, bool pp_IgnoreTexA, u32 pp_ShadInstr,
		bool pp_Offset, u32 pp_FogCtrl, bool pp_BumpMap, bool fog_clamping,
		bool trilinear, int palette, bool gouraud, bool alphaTest, bool clipInside, bool dithering,
		int sampleTextureFilter, int maxFilteredTextureSize)
{
	bool divPosZ = !settings.platform.isNaomi2() && config::NativeDepthInterpolation;
	u64 hash = (u64)(int)pp_Texture
			| (pp_UseAlpha << 1)
			| (pp_IgnoreTexA << 2)
			| (pp_ShadInstr << 3)
			| (pp_Offset << 5)
			| (pp_FogCtrl << 6)
			| (pp_BumpMap << 8)
			| (fog_clamping << 9)
			| (trilinear << 10)
			| (palette << 11)
			| (gouraud << 13)
			| (alphaTest << 14)
			| (clipInside << 15)
			| (divPosZ << 16)
			| (dithering << 17)
			| ((u64)sampleTextureFilter << 18);
	hash ^= (u64)(u32)maxFilteredTextureSize + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
	auto& shader = shaders[hash];
	if (shader == nullptr)
	{
		verify(pp_ShadInstr < std::size(MacroValues));
		verify(pp_FogCtrl < std::size(MacroValues));
		verify(sampleTextureFilter >= 0 && sampleTextureFilter <= 19);
		PixelMacros[MacroGouraud].Definition = MacroValues[gouraud];
		PixelMacros[MacroTexture].Definition = MacroValues[pp_Texture];
		PixelMacros[MacroUseAlpha].Definition = MacroValues[pp_UseAlpha];
		PixelMacros[MacroIgnoreTexA].Definition = MacroValues[pp_IgnoreTexA];
		PixelMacros[MacroShadInstr].Definition = MacroValues[pp_ShadInstr];
		PixelMacros[MacroOffset].Definition = MacroValues[pp_Offset];
		PixelMacros[MacroFogCtrl].Definition = MacroValues[pp_FogCtrl];
		PixelMacros[MacroBumpMap].Definition = MacroValues[pp_BumpMap];
		PixelMacros[MacroFogClamping].Definition = MacroValues[fog_clamping];
		PixelMacros[MacroTriLinear].Definition = MacroValues[trilinear];
		PixelMacros[MacroPalette].Definition = MacroValues[palette];
		PixelMacros[MacroAlphaTest].Definition = MacroValues[alphaTest];
		PixelMacros[MacroClipInside].Definition = MacroValues[clipInside];
		PixelMacros[MacroDivPosZ].Definition = MacroValues[divPosZ];
		PixelMacros[MacroDithering].Definition = MacroValues[dithering];
		char sampleFilterMacro[12];
		char maxFilteredSizeMacro[12];
		snprintf(sampleFilterMacro, sizeof(sampleFilterMacro), "%d", sampleTextureFilter);
		snprintf(maxFilteredSizeMacro, sizeof(maxFilteredSizeMacro), "%d", maxFilteredTextureSize);
		PixelMacros[MacroSampleTextureFilter].Definition = sampleFilterMacro;
		PixelMacros[MacroMaxFilteredTextureSize].Definition = maxFilteredSizeMacro;

		shader = compilePS(PixelShader, "main", PixelMacros);
		verify(shader != nullptr);
	}
	return shader;
}

const ComPtr<ID3D11VertexShader>& DX11Shaders::getVertexShader(bool gouraud, bool naomi2)
{
	bool divPosZ = !settings.platform.isNaomi2() && config::NativeDepthInterpolation;
	int index = (int)gouraud | ((int)naomi2 << 1) | ((int)divPosZ << 2);
	ComPtr<ID3D11VertexShader>& vertexShader = vertexShaders[index];
	if (!vertexShader)
	{
		VertexMacros[MacroGouraud].Definition = MacroValues[gouraud];
		if (!naomi2)
		{
			VertexMacros[MacroDivPosZ].Definition = MacroValues[divPosZ];
			vertexShader = compileVS(VertexShader, "main", VertexMacros);
		}
		else
		{
			VertexMacros[MacroPositionOnly].Definition = MacroValues[false];
			VertexMacros[MacroTwoVolumes].Definition = MacroValues[false];
			VertexMacros[MacroLightOn].Definition = MacroValues[true];
			VertexMacros[MacroModifierVolume].Definition = MacroValues[false];
			std::string source(DX11N2VertexShader);
			source += std::string("\n") + DX11N2ColorShader;
			vertexShader = compileVS(source.c_str(), "main", VertexMacros);
		}
	}

	return vertexShader;
}

const ComPtr<ID3D11VertexShader>& DX11Shaders::getMVVertexShader(bool naomi2)
{
	bool divPosZ = !settings.platform.isNaomi2() && config::NativeDepthInterpolation;
	int index = (int)naomi2 | ((int)divPosZ << 1);
	if (!modVolVertexShaders[index])
	{
		if (!naomi2)
		{
			VertexMacros[MacroDivPosZ].Definition = MacroValues[divPosZ];
			modVolVertexShaders[index] = compileVS(ModVolVertexShader, "main", VertexMacros);
		}
		else
		{
			VertexMacros[MacroGouraud].Definition = MacroValues[false];
			VertexMacros[MacroPositionOnly].Definition = MacroValues[true];
			VertexMacros[MacroTwoVolumes].Definition = MacroValues[false];
			VertexMacros[MacroLightOn].Definition = MacroValues[false];
			VertexMacros[MacroModifierVolume].Definition = MacroValues[true];
			modVolVertexShaders[index] = compileVS(DX11N2VertexShader, "main", VertexMacros);
		}
	}

	return modVolVertexShaders[index];
}

const ComPtr<ID3D11PixelShader>& DX11Shaders::getModVolShader()
{
	if (!modVolShader)
		modVolShader = compilePS(PixelShader, "modifierVolume", PixelMacros);

	return modVolShader;
}

const ComPtr<ID3D11VertexShader>& DX11Shaders::getQuadVertexShader(bool rotate)
{
	ComPtr<ID3D11VertexShader>& shader = rotate ? quadRotateVertexShader : quadVertexShader;
	if (!shader)
	{
		D3D_SHADER_MACRO macros[]
		{
			{ "ROTATE", rotate ? "1" : "0" },
			{ nullptr, nullptr }
		};
		shader = compileVS(QuadVertexShader, "main", macros);
	}

	return shader;
}

const ComPtr<ID3D11PixelShader>& DX11Shaders::getQuadPixelShader()
{
	if (!quadPixelShader)
		quadPixelShader = compilePS(QuadPixelShader, "main", nullptr);

	return quadPixelShader;
}

ComPtr<ID3DBlob> DX11Shaders::compileShader(const char* source, const char* function, const char* profile, const D3D_SHADER_MACRO *pDefines)
{
	u64 hash = hashShader(source, function, profile, pDefines);

	ComPtr<ID3DBlob> shaderBlob;
	if (!lookupShader(hash, shaderBlob))
	{
		ComPtr<ID3DBlob> errorBlob;
		IncludeManager includeManager;

		if (FAILED(this->D3DCompile(source, strlen(source), nullptr, pDefines, &includeManager, function, profile, 0, 0, &shaderBlob.get(), &errorBlob.get())))
			ERROR_LOG(RENDERER, "Shader compilation failed: %s", errorBlob->GetBufferPointer());
		else
			cacheShader(hash, shaderBlob);
	}

	return shaderBlob;
}

ComPtr<ID3D11VertexShader> DX11Shaders::compileVS(const char* source, const char* function, const D3D_SHADER_MACRO *pDefines)
{
	ComPtr<ID3DBlob> blob = compileShader(source, function, "vs_4_0", pDefines);
	ComPtr<ID3D11VertexShader> shader;
	if (blob)
	{
		if (FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader.get())))
			ERROR_LOG(RENDERER, "Vertex shader creation failed");
	}

	return shader;
}

ComPtr<ID3D11PixelShader> DX11Shaders::compilePS(const char* source, const char* function, const D3D_SHADER_MACRO *pDefines)
{
	ComPtr<ID3DBlob> blob = compileShader(source, function, "ps_4_0", pDefines);
	ComPtr<ID3D11PixelShader> shader;
	if (blob)
	{
		if (FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader.get())))
			ERROR_LOG(RENDERER, "Pixel shader creation failed");
	}

	return shader;
}

ComPtr<ID3DBlob> DX11Shaders::getVertexShaderBlob()
{
	VertexMacros[MacroGouraud].Definition = MacroValues[true];
	// FIXME code dup
	VertexMacros[MacroPositionOnly].Definition = MacroValues[false];
	VertexMacros[MacroTwoVolumes].Definition = MacroValues[false];
	VertexMacros[MacroModifierVolume].Definition = MacroValues[false];
	std::string source(DX11N2VertexShader);
	source += std::string("\n") + DX11N2ColorShader;
	return compileShader(source.c_str(), "main", "vs_4_0", VertexMacros);
}

ComPtr<ID3DBlob> DX11Shaders::getMVVertexShaderBlob()
{
	// FIXME code dup
	VertexMacros[MacroGouraud].Definition = MacroValues[false];
	VertexMacros[MacroPositionOnly].Definition = MacroValues[true];
	VertexMacros[MacroTwoVolumes].Definition = MacroValues[false];
	VertexMacros[MacroModifierVolume].Definition = MacroValues[true];
	return compileShader(DX11N2VertexShader, "main", "vs_4_0", VertexMacros);
}

ComPtr<ID3DBlob> DX11Shaders::getQuadVertexShaderBlob()
{
	return compileShader(QuadVertexShader, "main", "vs_4_0", nullptr);
}

void DX11Shaders::init(const ComPtr<ID3D11Device>& device, pD3DCompile D3DCompile)
{
	this->device = device;
	this->D3DCompile = D3DCompile;
	enableCache(!DX11Context::Instance()->hasShaderCache());
	loadCache(CacheFile);
}

void DX11Shaders::term()
{
	saveCache(CacheFile);
	shaders.clear();
	for (auto& shader : vertexShaders)
		shader.reset();
	modVolShader.reset();
	for (auto& shader : modVolVertexShaders)
		shader.reset();
	quadVertexShader.reset();
	quadRotateVertexShader.reset();
	quadPixelShader.reset();
	device.reset();
}

void CachedDX11Shaders::saveCache(const std::string& filename)
{
	if (!enabled)
		return;
	std::string path = hostfs::getShaderCachePath(filename);
	FILE *fp = nowide::fopen(path.c_str(), "wb");
	if (fp == nullptr)
	{
		WARN_LOG(RENDERER, "Cannot save shader cache to %s", path.c_str());
		return;
	}
	for (const auto& pair : shaderCache)
	{
		if (std::fwrite(&pair.first, sizeof(pair.first), 1, fp) != 1
				|| std::fwrite(&pair.second.size, sizeof(pair.second.size), 1, fp) != 1
				|| std::fwrite(&pair.second.blob[0], 1, pair.second.size, fp) != pair.second.size)
		{
			WARN_LOG(RENDERER, "Error saving shader cache to %s", path.c_str());
			break;
		}
	}
	NOTICE_LOG(RENDERER, "Saved %d shaders to %s", (int)shaderCache.size(), path.c_str());
	std::fclose(fp);
}

void CachedDX11Shaders::loadCache(const std::string& filename)
{
	if (!enabled)
		return;
	std::string path = hostfs::getShaderCachePath(filename);
	FILE *fp = nowide::fopen(path.c_str(), "rb");
	if (fp != nullptr)
	{
		u64 hash;
		u32 size;
		while (true)
		{
			if (std::fread(&hash, sizeof(hash), 1, fp) != 1)
				break;
			if (std::fread(&size, sizeof(size), 1, fp) != 1)
				break;
			std::unique_ptr<u8[]> blob = std::make_unique<u8[]>(size);
			if (std::fread(&blob[0], 1, size, fp) != size)
				break;
			shaderCache[hash] = { size, std::move(blob) };
		}
		std::fclose(fp);
		NOTICE_LOG(RENDERER, "Loaded %d shaders from %s", (int)shaderCache.size(), path.c_str());
	}

}
bool CachedDX11Shaders::lookupShader(u64 hash, ComPtr<ID3DBlob>& blob)
{
	if (!enabled)
		return false;

	auto it = shaderCache.find(hash);
	if (it == shaderCache.end())
		return false;

	D3DCreateBlob(it->second.size, &blob.get());
	memcpy(blob->GetBufferPointer(), &it->second.blob[0], it->second.size);

	return true;
}

void CachedDX11Shaders::cacheShader(u64 hash, const ComPtr<ID3DBlob>& blob)
{
	if (!enabled)
		return;
	u32 size = (u32)blob->GetBufferSize();
	std::unique_ptr<u8[]> data = std::make_unique<u8[]>(size);
	memcpy(&data[0], blob->GetBufferPointer(), size);
	shaderCache[hash] = { size, std::move(data) };
}

u64 CachedDX11Shaders::hashShader(const char* source, const char* function, const char* profile, const D3D_SHADER_MACRO *pDefines, const char *includeFile)
{
	if (!enabled)
		return 0;

	XXH3_state_t *xxh = XXH3_createState();
	XXH3_64bits_reset(xxh);
	XXH3_64bits_update(xxh, source, strlen(source));
	XXH3_64bits_update(xxh, function, strlen(function));
	if (pDefines != nullptr)
		for (const D3D_SHADER_MACRO *pDef = pDefines; pDef->Name != nullptr; pDef++)
		{
			XXH3_64bits_update(xxh, pDef->Name, strlen(pDef->Name));
			XXH3_64bits_update(xxh, pDef->Definition, strlen(pDef->Definition));
		}
	if (includeFile != nullptr)
		XXH3_64bits_update(xxh, includeFile, strlen(includeFile));
	u64 hash = XXH3_64bits_digest(xxh);
	XXH3_freeState(xxh);

	return hash;
}
