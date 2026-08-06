/*
 *  Created on: Oct 3, 2019

	Copyright 2019 flyinghead
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
#include "vulkan.h"
#include "shaders.h"
#include "compiler.h"
#include "utils.h"

static const char VertexShaderSource[] = R"(
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;
layout (location = 1) in vec4        in_base;
layout (location = 2) in vec4        in_offs;
layout (location = 3) in mediump vec2 in_uv;

layout (location = 0) INTERPOLATION out highp vec4 vtx_base;
layout (location = 1) INTERPOLATION out highp vec4 vtx_offs;
layout (location = 2) out highp vec3 vtx_uv;

void main()
{
	vec4 vpos = uniformBuffer.ndcMat * in_pos;
#if DIV_POS_Z == 1
	vpos /= vpos.z;
	vpos.z = vpos.w;
#endif
	vtx_base = in_base;
	vtx_offs = in_offs;
	vtx_uv = vec3(in_uv, vpos.z);
#if pp_Gouraud == 1 && DIV_POS_Z != 1
	vtx_base *= vpos.z;
	vtx_offs *= vpos.z;
#endif

#if DIV_POS_Z != 1
	vtx_uv.xy *= vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
#endif
	gl_Position = vpos;
}
)";

static const char FragmentShaderTop[] = R"(
layout (location = 0) out vec4 FragColor;
#define gl_FragColor FragColor

layout (std140, set = 0, binding = 1) uniform FragmentShaderUniforms
{
	vec4 colorClampMin;
	vec4 colorClampMax;
	vec4 sp_FOG_COL_RAM;
	vec4 sp_FOG_COL_VERT;
	vec4 ditherDivisor;
	float cp_AlphaTestValue;
	float sp_FOG_DENSITY;
} uniformBuffer;

layout (push_constant) uniform pushBlock
{
	vec4 clipTest;
	float trilinearAlpha;
	float palette_index;
} pushConstants;

#if pp_Texture == 1
layout (set = 1, binding = 0) uniform sampler2D tex;
#endif
#if pp_FogCtrl != 2
layout (set = 0, binding = 2) uniform sampler2D fog_table;
#endif
#if pp_Palette != 0
layout (set = 0, binding = 3) uniform sampler2D palette;
#endif

// Vertex input
layout (location = 0) INTERPOLATION in highp vec4 vtx_base;
layout (location = 1) INTERPOLATION in highp vec4 vtx_offs;
layout (location = 2) in highp vec3 vtx_uv;
)";

const char *FragmentShaderCommon = R"(
#define PI 3.1415926

#if pp_FogCtrl != 2 || pp_TwoVolumes == 1
float fog_mode2(float w)
{
	float z = clamp(
#if DIV_POS_Z == 1
					uniformBuffer.sp_FOG_DENSITY / w
#else
					uniformBuffer.sp_FOG_DENSITY * w
#endif
													, 1.0, 255.9999);
	float exp = floor(log2(z));
	float m = z * 16.0 / pow(2.0, exp) - 16.0;
	float idx = floor(m) + exp * 16.0 + 0.5;
	vec4 fog_coef = texture(fog_table, vec2(idx / 128.0, 0.75 - (m - floor(m)) / 2.0));
	return fog_coef.r;
}
#endif

vec4 colorClamp(vec4 col)
{
#if ColorClamping == 1
	return clamp(col, uniformBuffer.colorClampMin, uniformBuffer.colorClampMax);
#else
	return col;
#endif
}

#if pp_Palette != 0

vec4 getPaletteEntry(float colIdx)
{
	vec2 c = vec2(colIdx * 255.0 / 1023.0 + pushConstants.palette_index, 0.5);
	return texture(palette, c);
}

#endif

#if pp_Palette == 1

vec4 palettePixel(sampler2D tex, vec3 coords)
{
#if DIV_POS_Z == 1
	return getPaletteEntry(texture(tex, coords.xy).r);
#else
	return getPaletteEntry(textureProj(tex, coords).r);
#endif
}

#elif pp_Palette == 2

vec4 palettePixelBilinear(sampler2D tex, vec3 coords)
{
#if DIV_POS_Z == 0
	coords.xy /= coords.z;
#endif
	vec2 texSize = vec2(textureSize(tex, 0));
	vec2 pixCoord = coords.xy * texSize - 0.5;			// coordinates of top left pixel
	vec2 originPixCoord = floor(pixCoord);

	vec2 sampleUV = (originPixCoord + 0.5) / texSize;	// UV coordinates of center of top left pixel

    // Sample from all surrounding texels
    vec4 c00 = getPaletteEntry(texture(tex, sampleUV).r);
    vec4 c01 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(0, 1)).r);
    vec4 c11 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(1, 1)).r);
    vec4 c10 = getPaletteEntry(textureOffset(tex, sampleUV, ivec2(1, 0)).r);

	vec2 weight = pixCoord - originPixCoord;

    // Bi-linear mixing
    vec4 temp0 = mix(c00, c10, weight.x);
    vec4 temp1 = mix(c01, c11, weight.x);
    return mix(temp0, temp1, weight.y);
}

#endif

#if pp_Texture == 1 && pp_SampleTextureFilter != 0

#ifndef PI
#define PI 3.1415926
#endif

vec2 projectTextureCoords(vec3 coords)
{
#if DIV_POS_Z == 0
	return coords.xy / coords.z;
#else
	return coords.xy;
#endif
}

vec4 sampleSourceTexel(sampler2D sourceTex, vec2 uv)
{
#if pp_Palette == 0
	return texture(sourceTex, uv);
#else
	return getPaletteEntry(texture(sourceTex, uv).r);
#endif
}

vec4 legacyTextureSample(sampler2D sourceTex, vec3 coords)
{
#if pp_Palette == 0
	#if DIV_POS_Z == 1
		return texture(sourceTex, coords.xy);
	#else
		return textureProj(sourceTex, coords);
	#endif
#elif pp_Palette == 1
	return palettePixel(sourceTex, coords);
#else
	return palettePixelBilinear(sourceTex, coords);
#endif
}

bool sampleTextureFilterEligible(sampler2D sourceTex)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	float maxFilteredTextureSize = float(pp_MaxFilteredTextureSize);
	// Match the xBRZ size buckets by texture width: 8x16 matches 8x8, while 16x8 matches 16x16.
	return pp_MaxFilteredTextureSize >= 1024 || texSize.x <= maxFilteredTextureSize;
}

float jincDistance(vec2 a, vec2 b)
{
	vec2 v = b - a;
	return sqrt(dot(v, v));
}

vec4 jincResampler(vec4 x)
{
	// Windowed Jinc2 constants adapted from Hyllian's MIT-licensed shader.
	const float wa = 1.382300768;
	const float wb = 2.576105976;
	const float wawb = wa * wb;
	vec4 a = sin(x * wa) * sin(x * wb) / (x * x);
	bvec4 zero = equal(x, vec4(0.0));
	return vec4(zero.x ? wawb : a.x, zero.y ? wawb : a.y, zero.z ? wawb : a.z, zero.w ? wawb : a.w);
}

vec4 alphaWeightedBilinear(sampler2D sourceTex, vec2 uv, bool noEdgeBlend)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 pixCoord = uv * texSize - 0.5;
	vec2 originPixCoord = floor(pixCoord);
	vec2 sampleUV = (originPixCoord + 0.5) / texSize;
	vec2 texel = vec2(1.0) / texSize;

	vec4 c00 = sampleSourceTexel(sourceTex, sampleUV);
	vec4 c10 = sampleSourceTexel(sourceTex, sampleUV + vec2(texel.x, 0.0));
	vec4 c01 = sampleSourceTexel(sourceTex, sampleUV + vec2(0.0, texel.y));
	vec4 c11 = sampleSourceTexel(sourceTex, sampleUV + texel);

	vec2 weight = pixCoord - originPixCoord;
	vec4 p00 = vec4(c00.rgb * c00.a, c00.a);
	vec4 p10 = vec4(c10.rgb * c10.a, c10.a);
	vec4 p01 = vec4(c01.rgb * c01.a, c01.a);
	vec4 p11 = vec4(c11.rgb * c11.a, c11.a);
	vec4 color = mix(mix(p00, p10, weight.x), mix(p01, p11, weight.x), weight.y);

	if (color.a > 0.0)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5 ? 1.0 : 0.0;
	return color;
}

vec4 jinc2Sample(sampler2D sourceTex, vec2 uv, bool noEdgeBlend)
{
	const float arStrength = 0.8;
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 pc = uv * texSize;
	vec2 tc = floor(pc - vec2(0.5)) + vec2(0.5);
	vec2 dx = vec2(1.0, 0.0);
	vec2 dy = vec2(0.0, 1.0);

	vec4 w0 = jincResampler(vec4(jincDistance(pc, tc - dx - dy), jincDistance(pc, tc - dy), jincDistance(pc, tc + dx - dy), jincDistance(pc, tc + dx * 2.0 - dy)));
	vec4 w1 = jincResampler(vec4(jincDistance(pc, tc - dx), jincDistance(pc, tc), jincDistance(pc, tc + dx), jincDistance(pc, tc + dx * 2.0)));
	vec4 w2 = jincResampler(vec4(jincDistance(pc, tc - dx + dy), jincDistance(pc, tc + dy), jincDistance(pc, tc + dx + dy), jincDistance(pc, tc + dx * 2.0 + dy)));
	vec4 w3 = jincResampler(vec4(jincDistance(pc, tc - dx + dy * 2.0), jincDistance(pc, tc + dy * 2.0), jincDistance(pc, tc + dx + dy * 2.0), jincDistance(pc, tc + dx * 2.0 + dy * 2.0)));

	vec2 baseUV = tc / texSize;
	dx /= texSize;
	dy /= texSize;

	vec4 c00 = sampleSourceTexel(sourceTex, baseUV - dx - dy);
	vec4 c10 = sampleSourceTexel(sourceTex, baseUV - dy);
	vec4 c20 = sampleSourceTexel(sourceTex, baseUV + dx - dy);
	vec4 c30 = sampleSourceTexel(sourceTex, baseUV + dx * 2.0 - dy);
	vec4 c01 = sampleSourceTexel(sourceTex, baseUV - dx);
	vec4 c11 = sampleSourceTexel(sourceTex, baseUV);
	vec4 c21 = sampleSourceTexel(sourceTex, baseUV + dx);
	vec4 c31 = sampleSourceTexel(sourceTex, baseUV + dx * 2.0);
	vec4 c02 = sampleSourceTexel(sourceTex, baseUV - dx + dy);
	vec4 c12 = sampleSourceTexel(sourceTex, baseUV + dy);
	vec4 c22 = sampleSourceTexel(sourceTex, baseUV + dx + dy);
	vec4 c32 = sampleSourceTexel(sourceTex, baseUV + dx * 2.0 + dy);
	vec4 c03 = sampleSourceTexel(sourceTex, baseUV - dx + dy * 2.0);
	vec4 c13 = sampleSourceTexel(sourceTex, baseUV + dy * 2.0);
	vec4 c23 = sampleSourceTexel(sourceTex, baseUV + dx + dy * 2.0);
	vec4 c33 = sampleSourceTexel(sourceTex, baseUV + dx * 2.0 + dy * 2.0);

	c00.rgb *= c00.a; c10.rgb *= c10.a; c20.rgb *= c20.a; c30.rgb *= c30.a;
	c01.rgb *= c01.a; c11.rgb *= c11.a; c21.rgb *= c21.a; c31.rgb *= c31.a;
	c02.rgb *= c02.a; c12.rgb *= c12.a; c22.rgb *= c22.a; c32.rgb *= c32.a;
	c03.rgb *= c03.a; c13.rgb *= c13.a; c23.rgb *= c23.a; c33.rgb *= c33.a;

	float wsum = max(dot(w0, vec4(1.0)) + dot(w1, vec4(1.0)) + dot(w2, vec4(1.0)) + dot(w3, vec4(1.0)), 0.000001);
	vec4 color = vec4(
		dot(w0, vec4(c00.r, c10.r, c20.r, c30.r)) + dot(w1, vec4(c01.r, c11.r, c21.r, c31.r)) + dot(w2, vec4(c02.r, c12.r, c22.r, c32.r)) + dot(w3, vec4(c03.r, c13.r, c23.r, c33.r)),
		dot(w0, vec4(c00.g, c10.g, c20.g, c30.g)) + dot(w1, vec4(c01.g, c11.g, c21.g, c31.g)) + dot(w2, vec4(c02.g, c12.g, c22.g, c32.g)) + dot(w3, vec4(c03.g, c13.g, c23.g, c33.g)),
		dot(w0, vec4(c00.b, c10.b, c20.b, c30.b)) + dot(w1, vec4(c01.b, c11.b, c21.b, c31.b)) + dot(w2, vec4(c02.b, c12.b, c22.b, c32.b)) + dot(w3, vec4(c03.b, c13.b, c23.b, c33.b)),
		dot(w0, vec4(c00.a, c10.a, c20.a, c30.a)) + dot(w1, vec4(c01.a, c11.a, c21.a, c31.a)) + dot(w2, vec4(c02.a, c12.a, c22.a, c32.a)) + dot(w3, vec4(c03.a, c13.a, c23.a, c33.a))) / wsum;

	vec4 unclamped = color;
	color = mix(unclamped, clamp(color, min(min(c11, c21), min(c12, c22)), max(max(c11, c21), max(c12, c22))), arStrength);
	if (color.a > 0.0)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5 ? 1.0 : 0.0;
	return color;
}

)" R"(

vec4 clampToSourceNeighborhood(sampler2D sourceTex, vec2 uv, vec4 color)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 base = floor(uv * texSize);
	vec2 invTexSize = vec2(1.0) / texSize;
	vec4 minColor = vec4(1.0);
	vec4 maxColor = vec4(0.0);

	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			vec4 sampleColor = sampleSourceTexel(sourceTex, (base + vec2(x, y) + 0.5) * invTexSize);
			minColor = min(minColor, sampleColor);
			maxColor = max(maxColor, sampleColor);
		}
	}
	return clamp(color, minColor, maxColor);
}

vec4 sharpenAgainstCenter(sampler2D sourceTex, vec2 uv, vec4 color, float strength)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 centerUV = (floor(uv * texSize) + 0.5) / texSize;
	vec4 center = sampleSourceTexel(sourceTex, centerUV);
	color.rgb += (color.rgb - center.rgb) * strength;
	return color;
}

vec4 jinc2AntiRingSample(sampler2D sourceTex, vec2 uv, float sharpness)
{
	vec4 color = jinc2Sample(sourceTex, uv, false);
	if (sharpness > 0.0)
		color = sharpenAgainstCenter(sourceTex, uv, color, sharpness);
	return clampToSourceNeighborhood(sourceTex, uv, color);
}

float sincWeight(float x)
{
	x = abs(x);
	if (x < 0.00001)
		return 1.0;
	float pix = PI * x;
	return sin(pix) / pix;
}

float lanczosWeight(float x, float radius)
{
	x = abs(x);
	if (x >= radius)
		return 0.0;
	return sincWeight(x) * sincWeight(x / radius);
}

float spline36Weight(float x)
{
	x = abs(x);
	if (x < 1.0)
		return ((13.0 / 11.0 * x - 453.0 / 209.0) * x) * x + 1.0;
	if (x < 2.0)
		return ((-6.0 / 11.0 * x + 270.0 / 209.0) * x - 156.0 / 209.0) * x - 3.0 / 209.0;
	if (x < 3.0)
		return ((1.0 / 11.0 * x - 45.0 / 209.0) * x + 26.0 / 209.0) * x - 1.0 / 209.0;
	return 0.0;
}

float cubicWeight(float x, float b, float c)
{
	x = abs(x);
	if (x < 1.0)
		return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x
			+ (-18.0 + 12.0 * b + 6.0 * c) * x * x
			+ (6.0 - 2.0 * b)) / 6.0;
	if (x < 2.0)
		return ((-b - 6.0 * c) * x * x * x
			+ (6.0 * b + 30.0 * c) * x * x
			+ (-12.0 * b - 48.0 * c) * x
			+ (8.0 * b + 24.0 * c)) / 6.0;
	return 0.0;
}

float reconstructionWeight(float x, int filterMode)
{
	if (filterMode == 0)
		return lanczosWeight(x, 2.0);
	if (filterMode == 1)
		return lanczosWeight(x, 3.0);
	if (filterMode == 2)
		return spline36Weight(x);
	if (filterMode == 3)
		return cubicWeight(x, 0.378215755, 0.310892122); // Robidoux
	if (filterMode == 4)
		return cubicWeight(x, 0.262014512, 0.368992744); // Robidoux Sharp
	if (filterMode == 5)
		return cubicWeight(x, 0.0, 0.5); // Catmull-Rom
	return cubicWeight(x, 1.0 / 3.0, 1.0 / 3.0); // Mitchell-Netravali
}

vec4 separableReconstructionSample(sampler2D sourceTex, vec2 uv, int filterMode, bool antiRing)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 pc = uv * texSize - 0.5;
	vec2 base = floor(pc);
	vec2 f = pc - base;
	vec2 invTexSize = vec2(1.0) / texSize;
	vec4 color = vec4(0.0);
	float weightSum = 0.0;

	for (int y = -2; y <= 3; y++)
	{
		float wy = reconstructionWeight(float(y) - f.y, filterMode);
		for (int x = -2; x <= 3; x++)
		{
			float weight = reconstructionWeight(float(x) - f.x, filterMode) * wy;
			vec4 sampleColor = sampleSourceTexel(sourceTex, (base + vec2(x, y) + 0.5) * invTexSize);
			sampleColor.rgb *= sampleColor.a;
			color += sampleColor * weight;
			weightSum += weight;
		}
	}

	if (abs(weightSum) > 0.00001)
		color /= weightSum;
	if (color.a > 0.0)
		color.rgb /= color.a;
	color = clamp(color, 0.0, 1.0);
	if (antiRing)
		color = clampToSourceNeighborhood(sourceTex, uv, color);
	return color;
}

)" R"(

uint packSourceTexel(sampler2D sourceTex, vec2 baseUV, vec2 texelOffset, vec2 invTexSize)
{
	return packUnorm4x8(clamp(sampleSourceTexel(sourceTex, baseUV + texelOffset * invTexSize), 0.0, 1.0));
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
bool mmpxNoneEq2(uint value, uint a, uint b) { return value != a && value != b; }
bool mmpxNoneEq4(uint value, uint a, uint b, uint c, uint d) { return value != a && value != b && value != c && value != d; }

vec4 mmpxResultColor(uint result, uint center)
{
	vec4 color = unpackUnorm4x8(result);
	if (pp_SampleTextureFilter == 6 && result != center)
		color = mix(color, unpackUnorm4x8(center), 0.25);
	return color;
}

vec4 mmpxSample(sampler2D sourceTex, vec2 uv)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 bcoords = floor(uv * texSize);
	vec2 baseUV = (bcoords + 0.5) / texSize;
	vec2 invTexSize = vec2(1.0) / texSize;

	// MMPX is a MIT-licensed style-preserving pixel-art magnifier by Morgan McGuire and Mara Gagiu.
	uint A = packSourceTexel(sourceTex, baseUV, vec2(-1.0, -1.0), invTexSize);
	uint B = packSourceTexel(sourceTex, baseUV, vec2( 0.0, -1.0), invTexSize);
	uint C = packSourceTexel(sourceTex, baseUV, vec2( 1.0, -1.0), invTexSize);
	uint D = packSourceTexel(sourceTex, baseUV, vec2(-1.0,  0.0), invTexSize);
	uint E = packSourceTexel(sourceTex, baseUV, vec2( 0.0,  0.0), invTexSize);
	uint F = packSourceTexel(sourceTex, baseUV, vec2( 1.0,  0.0), invTexSize);
	uint G = packSourceTexel(sourceTex, baseUV, vec2(-1.0,  1.0), invTexSize);
	uint H = packSourceTexel(sourceTex, baseUV, vec2( 0.0,  1.0), invTexSize);
	uint I = packSourceTexel(sourceTex, baseUV, vec2( 1.0,  1.0), invTexSize);
	uint J = E, K = E, L = E, M = E;

	if (((A ^ E) | (B ^ E) | (C ^ E) | (D ^ E) | (F ^ E) | (G ^ E) | (H ^ E) | (I ^ E)) != 0u)
	{
		uint P = packSourceTexel(sourceTex, baseUV, vec2( 0.0, -2.0), invTexSize);
		uint Q = packSourceTexel(sourceTex, baseUV, vec2(-2.0,  0.0), invTexSize);
		uint R = packSourceTexel(sourceTex, baseUV, vec2( 2.0,  0.0), invTexSize);
		uint S = packSourceTexel(sourceTex, baseUV, vec2( 0.0,  2.0), invTexSize);
		uint Bl = mmpxLuma(B), Dl = mmpxLuma(D), El = mmpxLuma(E), Fl = mmpxLuma(F), Hl = mmpxLuma(H);

		if ((D == B && D != H && D != F) && (El >= Dl || E == A) && mmpxAnyEq3(E, A, C, G) && ((El < Dl) || A != D || E != P || E != Q)) J = D;
		if ((B == F && B != D && B != H) && (El >= Bl || E == C) && mmpxAnyEq3(E, A, C, I) && ((El < Bl) || C != B || E != P || E != R)) K = B;
		if ((H == D && H != F && H != B) && (El >= Hl || E == G) && mmpxAnyEq3(E, A, G, I) && ((El < Hl) || G != H || E != S || E != Q)) L = H;
		if ((F == H && F != B && F != D) && (El >= Fl || E == I) && mmpxAnyEq3(E, C, G, I) && ((El < Fl) || I != H || E != R || E != S)) M = F;

		if (pp_SampleTextureFilter == 7)
		{
			if ((E != F && mmpxAllEq4(E, C, I, D, Q) && mmpxAllEq2(F, B, H)) && (F != packSourceTexel(sourceTex, baseUV, vec2( 3.0,  0.0), invTexSize))) K = M = F;
			if ((E != D && mmpxAllEq4(E, A, G, F, R) && mmpxAllEq2(D, B, H)) && (D != packSourceTexel(sourceTex, baseUV, vec2(-3.0,  0.0), invTexSize))) J = L = D;
			if ((E != H && mmpxAllEq4(E, G, I, B, P) && mmpxAllEq2(H, D, F)) && (H != packSourceTexel(sourceTex, baseUV, vec2( 0.0,  3.0), invTexSize))) L = M = H;
			if ((E != B && mmpxAllEq4(E, A, C, H, S) && mmpxAllEq2(B, D, F)) && (B != packSourceTexel(sourceTex, baseUV, vec2( 0.0, -3.0), invTexSize))) J = K = B;
			if (Bl < El && mmpxAllEq4(E, G, H, I, S) && mmpxNoneEq4(E, A, D, C, F)) J = K = B;
			if (Hl < El && mmpxAllEq4(E, A, B, C, P) && mmpxNoneEq4(E, D, G, I, F)) L = M = H;
			if (Fl < El && mmpxAllEq4(E, A, D, G, Q) && mmpxNoneEq4(E, B, C, I, H)) K = M = F;
			if (Dl < El && mmpxAllEq4(E, C, F, I, R) && mmpxNoneEq4(E, B, A, G, H)) J = L = D;
		}
	}

	vec2 fpart = fract(uv * texSize);
	uint result = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);
	return mmpxResultColor(result, E);
}

vec4 filteredTextureSample(sampler2D sourceTex, vec3 coords)
{
	if (!sampleTextureFilterEligible(sourceTex))
		return legacyTextureSample(sourceTex, coords);

	vec2 uv = projectTextureCoords(coords);
	if (pp_SampleTextureFilter == 1)
		return alphaWeightedBilinear(sourceTex, uv, false);
	if (pp_SampleTextureFilter == 2)
		return alphaWeightedBilinear(sourceTex, uv, true);
	if (pp_SampleTextureFilter == 3)
		return jinc2Sample(sourceTex, uv, false);
	if (pp_SampleTextureFilter == 4)
		return jinc2Sample(sourceTex, uv, true);
	if (pp_SampleTextureFilter == 5 || pp_SampleTextureFilter == 6 || pp_SampleTextureFilter == 7)
		return mmpxSample(sourceTex, uv);
	if (pp_SampleTextureFilter == 8)
		return jinc2AntiRingSample(sourceTex, uv, 0.0);
	if (pp_SampleTextureFilter == 9)
		return jinc2AntiRingSample(sourceTex, uv, 0.18);
	if (pp_SampleTextureFilter == 10)
		return jinc2AntiRingSample(sourceTex, uv, 0.08);
	if (pp_SampleTextureFilter == 11)
		return jinc2AntiRingSample(sourceTex, uv, 0.22);
	if (pp_SampleTextureFilter == 12)
		return separableReconstructionSample(sourceTex, uv, 0, true);
	if (pp_SampleTextureFilter == 13)
		return separableReconstructionSample(sourceTex, uv, 1, true);
	if (pp_SampleTextureFilter == 14)
		return separableReconstructionSample(sourceTex, uv, 2, false);
	if (pp_SampleTextureFilter == 15)
		return separableReconstructionSample(sourceTex, uv, 3, false);
	if (pp_SampleTextureFilter == 16)
		return separableReconstructionSample(sourceTex, uv, 4, false);
	if (pp_SampleTextureFilter == 17)
		return separableReconstructionSample(sourceTex, uv, 5, false);
	if (pp_SampleTextureFilter == 18)
		return separableReconstructionSample(sourceTex, uv, 6, false);
	return legacyTextureSample(sourceTex, coords);
}

#endif
)";

static const char FragmentShaderMain[] = R"(
void main()
{
	// Clip inside the box
	#if pp_ClipInside == 1
		if (gl_FragCoord.x >= pushConstants.clipTest.x && gl_FragCoord.x <= pushConstants.clipTest.z
				&& gl_FragCoord.y >= pushConstants.clipTest.y && gl_FragCoord.y <= pushConstants.clipTest.w)
			discard;
	#endif
	
	highp vec4 color = vtx_base;
	highp vec4 offset = vtx_offs;
	#if pp_Gouraud == 1 && DIV_POS_Z != 1
		color /= vtx_uv.z;
		offset /= vtx_uv.z;
	#endif
	#if pp_UseAlpha == 0
		color.a = 1.0;
	#endif
	#if pp_FogCtrl == 3
		color = vec4(uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z));
	#endif
	#if pp_Texture == 1
	{
		#if pp_SampleTextureFilter != 0
			vec4 texcol = filteredTextureSample(tex, vtx_uv);
		#else
		#if pp_Palette == 0
			#if DIV_POS_Z == 1
				vec4 texcol = texture(tex, vtx_uv.xy);
			#else
				vec4 texcol = textureProj(tex, vtx_uv);
			#endif
		#else
			#if pp_Palette == 1
				vec4 texcol = palettePixel(tex, vtx_uv);
			#else
				vec4 texcol = palettePixelBilinear(tex, vtx_uv);
			#endif
		#endif
		#endif
		
		#if pp_BumpMap == 1
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
		#else
			#if pp_IgnoreTexA == 1
				texcol.a = 1.0;
			#endif
		#endif
		#if pp_ShadInstr == 0
		{
			color = texcol;
		}
		#endif
		#if pp_ShadInstr == 1
		{
			color.rgb *= texcol.rgb;
			color.a = texcol.a;
		}
		#endif
		#if pp_ShadInstr == 2
		{
			color.rgb = mix(color.rgb, texcol.rgb, texcol.a);
		}
		#endif
		#if  pp_ShadInstr == 3
		{
			color *= texcol;
		}
		#endif
		
		#if pp_Offset == 1 && pp_BumpMap == 0
		{
			color.rgb += offset.rgb;
		}
		#endif
	}
	#endif
	
	color = colorClamp(color);
	
	#if pp_FogCtrl == 0
	{
		color.rgb = mix(color.rgb, uniformBuffer.sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z)); 
	}
	#endif
	#if pp_FogCtrl == 1 && pp_Offset==1 && pp_BumpMap == 0
	{
		color.rgb = mix(color.rgb, uniformBuffer.sp_FOG_COL_VERT.rgb, offset.a);
	}
	#endif
	
	#if pp_TriLinear == 1
	color *= pushConstants.trilinearAlpha;
	#endif
	
	#if cp_AlphaTest == 1
		color.a = round(color.a * 255.0) / 255.0;
		if (uniformBuffer.cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	#endif

	//color.rgb = vec3(gl_FragCoord.w * uniformBuffer.sp_FOG_DENSITY / 128.0);

#if DIV_POS_Z == 1
	highp float w = 100000.0 / vtx_uv.z;
#else
	highp float w = 100000.0 * vtx_uv.z;
#endif
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;

#if DITHERING == 1
	float ditherTable[16] = float[](
		5., 13.,  7., 15.,
		9.,  1., 11.,  3.,
		6., 14.,  4., 12.,
		10., 2.,  8.,  0.
	);
	float r = ditherTable[int(mod(gl_FragCoord.y, 4.)) * 4 + int(mod(gl_FragCoord.x, 4.))];
	vec4 dv = vec4(r, r, r, 1.) / uniformBuffer.ditherDivisor;
	color = clamp(floor(color * 255. + dv) / 255., 0., 1.);
#endif
	gl_FragColor = color;
}
)";

extern const char ModVolVertexShaderSource[] = R"(
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (location = 0) in vec4 in_pos;
layout (location = 0) out highp float depth;

void main()
{
	vec4 vpos = uniformBuffer.ndcMat * in_pos;
#if DIV_POS_Z == 1
	vpos /= vpos.z;
	vpos.z = vpos.w;
	depth = vpos.w;
#else
	depth = vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
#endif
	gl_Position = vpos;
}
)";

static const char ModVolFragmentShaderSource[] = R"(
layout (location = 0) in highp float depth;
layout (location = 0) out vec4 FragColor;

layout (push_constant) uniform pushBlock
{
	float sp_ShaderColor;
} pushConstants;

void main()
{
#if DIV_POS_Z == 1
	highp float w = 100000.0 / depth;
#else
	highp float w = 100000.0 * depth;
#endif
	gl_FragDepth = log2(1.0 + max(w, -0.999999)) / 34.0;
	FragColor = vec4(0.0, 0.0, 0.0, pushConstants.sp_ShaderColor);
}
)";

static const char QuadVertexShaderSource[] = R"(
layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;

layout (location = 0) out vec2 outUV;

void main()
{
#if ROTATE == 0
	gl_Position = vec4(in_pos, 1.0);
#else
	gl_Position = vec4(in_pos.y, -in_pos.x, in_pos.z, 1.0);
#endif
	outUV = in_uv;
}
)";

static const char QuadFragmentShaderSource[] = R"(
layout (set = 0, binding = 0) uniform sampler2D tex;

layout (push_constant) uniform pushBlock
{
	vec4 color;
} pushConstants;

layout (location = 0) in vec2 inUV;
layout (location = 0) out vec4 FragColor;

void main() 
{
#if IGNORE_TEX_ALPHA == 1
	FragColor.rgb = pushConstants.color.rgb * texture(tex, inUV).rgb;
	FragColor.a = pushConstants.color.a;
#else
	FragColor = pushConstants.color * texture(tex, inUV);
#endif
}
)";

extern const char N2LightShaderSource[] = R"(

layout (std140, set = 1, binding = 2) uniform N2VertexShaderUniforms
{
	mat4 mvMat;
	mat4 normalMat;
	mat4 projMat;
	ivec2 envMapping;
	int bumpMapping;
	int polyNumber;

	vec2 glossCoef;
	ivec2 constantColor;
} n2Uniform;

#define PI 3.1415926

#define LMODE_SINGLE_SIDED 0
#define LMODE_DOUBLE_SIDED 1
#define LMODE_DOUBLE_SIDED_WITH_TOLERANCE 2
#define LMODE_SPECIAL_EFFECT 3
#define LMODE_THIN_SURFACE 4
#define LMODE_BUMP_MAP 5

#define ROUTING_SPEC_TO_OFFSET 1
#define ROUTING_DIFF_TO_OFFSET 2
#define ROUTING_ATTENUATION 1	// not handled
#define ROUTING_FOG 2			// not handled
#define ROUTING_ALPHA 4
#define ROUTING_SUB 8

struct N2Light
{
	vec4 color;
	vec4 direction;	// For parallel/spot
	vec4 position;	// For spot/point

	int parallel;
	int routing;
	int dmode;
	int smode;

	ivec2 diffuse;
	ivec2 specular;

	float attnDistA;
	float attnDistB;
	float attnAngleA;	// For spot
	float attnAngleB;

	int distAttnMode;	// For spot/point
	int _pad1;
	int _pad2;
	int _pad3;
};

layout (std140, set = 1, binding = 3) uniform N2Lights
{
	N2Light lights[16];
	vec4 ambientBase[2];
	vec4 ambientOffset[2];
	ivec2 ambientMaterialBase;
	ivec2 ambientMaterialOffset;
	int lightCount;
	int useBaseOver;
	int bumpId0;
	int bumpId1;
} n2Lights;

void computeColors(inout vec4 baseCol, inout vec4 offsetCol, in int volIdx, in vec3 position, in vec3 normal)
{
	if (n2Uniform.constantColor[volIdx] == 1)
		return;
	vec3 diffuse = vec3(0.0);
	vec3 specular = vec3(0.0);
	float diffuseAlpha = 0.0;
	float specularAlpha = 0.0;
	vec3 reflectDir = reflect(normalize(position), normal);
	const float BASE_FACTOR = 2.0;

	for (int i = 0; i < n2Lights.lightCount; i++)
	{
		vec3 lightDir; // direction to the light
		vec3 lightColor = n2Lights.lights[i].color.rgb;
		if (n2Lights.lights[i].parallel == 1)
		{
			lightDir = normalize(n2Lights.lights[i].direction.xyz);
		}
		else
		{
			lightDir = normalize(n2Lights.lights[i].position.xyz - position);
			if (n2Lights.lights[i].attnDistA != 1.0 || n2Lights.lights[i].attnDistB != 0.0)
			{
				float distance = length(n2Lights.lights[i].position.xyz - position);
				if (n2Lights.lights[i].distAttnMode == 0)
					distance = 1.0 / distance;
				lightColor *= clamp(n2Lights.lights[i].attnDistB * distance + n2Lights.lights[i].attnDistA, 0.0, 1.0);
			}
			if (n2Lights.lights[i].attnAngleA != 1.0 || n2Lights.lights[i].attnAngleB != 0.0)
			{
				vec3 spotDir = n2Lights.lights[i].direction.xyz;
				float cosAngle = 1.0 - max(0.0, dot(lightDir, spotDir));
				lightColor *= clamp(cosAngle * n2Lights.lights[i].attnAngleB + n2Lights.lights[i].attnAngleA, 0.0, 1.0);
			}
		}
		if (n2Lights.lights[i].diffuse[volIdx] == 1)
		{
			float factor = (n2Lights.lights[i].routing & ROUTING_SUB) != 0 ? -BASE_FACTOR : BASE_FACTOR;
			if (n2Lights.lights[i].dmode == LMODE_SINGLE_SIDED)
				factor *= max(dot(normal, lightDir), 0.0);
			else if (n2Lights.lights[i].dmode == LMODE_DOUBLE_SIDED)
				factor *= abs(dot(normal, lightDir));

			if ((n2Lights.lights[i].routing & ROUTING_ALPHA) != 0)
				diffuseAlpha += lightColor.r * factor;
			else
			{
				if ((n2Lights.lights[i].routing & ROUTING_DIFF_TO_OFFSET) == 0)
					diffuse += lightColor * factor * baseCol.rgb;
				else
					specular += lightColor * factor * baseCol.rgb;
			}
		}
		if (n2Lights.lights[i].specular[volIdx] == 1)
		{
			float factor = (n2Lights.lights[i].routing & ROUTING_SUB) != 0 ? -BASE_FACTOR : BASE_FACTOR;
			if (n2Lights.lights[i].smode == LMODE_SINGLE_SIDED)
				factor *= clamp(pow(max(dot(lightDir, reflectDir), 0.0), n2Uniform.glossCoef[volIdx]), 0.0, 1.0);
			else if (n2Lights.lights[i].smode == LMODE_DOUBLE_SIDED)
				factor *= clamp(pow(abs(dot(lightDir, reflectDir)), n2Uniform.glossCoef[volIdx]), 0.0, 1.0);

			if ((n2Lights.lights[i].routing & ROUTING_ALPHA) != 0)
				specularAlpha += lightColor.r * factor;
			else
			{
				if ((n2Lights.lights[i].routing & ROUTING_SPEC_TO_OFFSET) == 0)
					diffuse += lightColor * factor * offsetCol.rgb;
				else
					specular += lightColor * factor * offsetCol.rgb;
			}
		}
	}
	// ambient light
	if (n2Lights.ambientMaterialBase[volIdx] == 1)
		diffuse += n2Lights.ambientBase[volIdx].rgb * baseCol.rgb;
	else
		diffuse += n2Lights.ambientBase[volIdx].rgb;
	if (n2Lights.ambientMaterialOffset[volIdx] == 1)
		specular += n2Lights.ambientOffset[volIdx].rgb * offsetCol.rgb;
	else
		specular += n2Lights.ambientOffset[volIdx].rgb;
	baseCol.rgb = diffuse;
	offsetCol.rgb = specular;

	baseCol.a += diffuseAlpha;
	offsetCol.a += specularAlpha;
	if (n2Lights.useBaseOver == 1)
	{
		vec4 overflow = max(baseCol - vec4(1.0), 0.0);
		offsetCol += overflow;
	}
	baseCol = clamp(baseCol, 0.0, 1.0);
	offsetCol = clamp(offsetCol, 0.0, 1.0);
}

void computeEnvMap(inout vec2 uv, in vec3 position, in vec3 normal)
{
	// Spherical mapping
	//vec3 r = reflect(normalize(position), normal);
	//float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
	//uv += r.xy / m + 0.5;

	// Cheap env mapping
	uv += normal.xy / 2.0 + 0.5;
	uv = clamp(uv, 0.0, 1.0);
}

void computeBumpMap(inout vec4 color0, in vec4 color1, in vec3 position, in vec3 normal, in mat4 normalMat)
{
	// TODO
	//if (n2Lights.bumpId0 == -1)
		return;
	normal = normalize(normal);
	vec3 tangent = color0.xyz;
	if (tangent.x > 0.5)
		tangent.x -= 1.0;
	if (tangent.y > 0.5)
		tangent.y -= 1.0;
	if (tangent.z > 0.5)
		tangent.z -= 1.0;
	tangent = normalize(tangent);
	vec3 bitangent = color1.xyz;
	if (bitangent.x > 0.5)
		bitangent.x -= 1.0;
	if (bitangent.y > 0.5)
		bitangent.y -= 1.0;
	if (bitangent.z > 0.5)
		bitangent.z -= 1.0;
	bitangent = normalize(bitangent);

	float scaleDegree = color0.w;
	float scaleOffset = color1.w;

	vec3 lightDir; // direction to the light
	if (n2Lights.lights[n2Lights.bumpId0].parallel == 1)
		lightDir = n2Lights.lights[n2Lights.bumpId0].direction.xyz;
	else
		lightDir = n2Lights.lights[n2Lights.bumpId0].position.xyz - position;
	lightDir = normalize(lightDir * mat3(normalMat));

	float n = dot(lightDir, normal);
	float cosQ = dot(lightDir, tangent);
	float sinQ = dot(lightDir, bitangent);

	float sinT = clamp(n, 0.0, 1.0);
	float k1 = 1.0 - scaleDegree;
	float k2 = scaleDegree * sinT;
	float k3 = scaleDegree * sqrt(1.0 - sinT * sinT); // cos T

	float q = acos(cosQ);
	if (sinQ < 0.0)
		q = 2.0 * PI - q;

	color0.r = k2;
	color0.g = k3;
	color0.b = q / PI / 2.0;
	color0.a = k1;
	color0 = clamp(color0, 0.0, 1.0);
}
)";

static const char N2VertexShaderSource[] = R"(
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (location = 0) in vec4         in_pos;
layout (location = 1) in vec4         in_base;
layout (location = 2) in vec4         in_offs;
layout (location = 3) in mediump vec2 in_uv;
layout (location = 4) in vec3         in_normal;

layout (location = 0) INTERPOLATION out highp vec4 vtx_base;
layout (location = 1) INTERPOLATION out highp vec4 vtx_offs;
layout (location = 2) out highp vec3 vtx_uv;

void wDivide(inout vec4 vpos)
{
	vpos = vec4(vpos.xy / vpos.w, 1.0 / vpos.w, 1.0);
	vpos = uniformBuffer.ndcMat * vpos;
#if pp_Gouraud == 1
	vtx_base *= vpos.z;
	vtx_offs *= vpos.z;
#endif
	vtx_uv = vec3(vtx_uv.xy * vpos.z, vpos.z);
	vpos.w = 1.0;
	vpos.z = 0.0;
}

void main()
{
	vec4 vpos = n2Uniform.mvMat * in_pos;
	vtx_base = in_base;
	vtx_offs = in_offs;

	vec3 vnorm = normalize(mat3(n2Uniform.normalMat) * in_normal);

	// TODO bump mapping
	if (n2Uniform.bumpMapping == 0)
	{
		computeColors(vtx_base, vtx_offs, 0, vpos.xyz, vnorm);
		#if pp_Texture == 0
				vtx_base += vtx_offs;
		#endif
	}

	vtx_uv.xy = in_uv;
	if (n2Uniform.envMapping[0] == 1)
		computeEnvMap(vtx_uv.xy, vpos.xyz, vnorm);

	vpos = n2Uniform.projMat * vpos;
	wDivide(vpos);

	gl_Position = vpos;
}
)";

extern const char N2ModVolVertexShaderSource[] = R"(
layout (std140, set = 0, binding = 0) uniform VertexShaderUniforms
{
	mat4 ndcMat;
} uniformBuffer;

layout (std140, set = 1, binding = 2) uniform N2VertexShaderUniforms
{
	mat4 mvMat;
	mat4 normalMat;
	mat4 projMat;
	ivec2 envMapping;
	int bumpMapping;
	int polyNumber;

	vec2 glossCoef;
	ivec2 constantColor;
} n2Uniform;

layout (location = 0) in vec4 in_pos;
layout (location = 0) out highp float depth;

void wDivide(inout vec4 vpos)
{
	vpos = vec4(vpos.xy / vpos.w, 1.0 / vpos.w, 1.0);
	vpos = uniformBuffer.ndcMat * vpos;
	depth = vpos.z;
	vpos.w = 1.0;
	vpos.z = 0.0;
}

void main()
{
	vec4 vpos = n2Uniform.mvMat * in_pos;
	vpos.z = min(vpos.z, -0.001);
	vpos = n2Uniform.projMat * vpos;
	wDivide(vpos);

	gl_Position = vpos;
}
)";

vk::UniqueShaderModule ShaderManager::compileShader(const VertexShaderParams& params)
{
	VulkanSource src;
	if (!params.naomi2)
	{
		src.addConstant("pp_Gouraud", (int)params.gouraud)
				.addConstant("DIV_POS_Z", (int)params.divPosZ)
				.addSource(GouraudSource)
				.addSource(VertexShaderSource);
	}
	else
	{
		src.addConstant("pp_Gouraud", (int)params.gouraud)
				.addConstant("pp_Texture", (int)params.texture)
				.addSource(GouraudSource)
				.addSource(N2LightShaderSource)
				.addSource(N2VertexShaderSource);
	}
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, src.generate());
}

vk::UniqueShaderModule ShaderManager::compileShader(const FragmentShaderParams& params)
{
	VulkanSource src;
	src.addConstant("cp_AlphaTest", (int)params.alphaTest)
		.addConstant("pp_ClipInside", (int)params.insideClipTest)
		.addConstant("pp_UseAlpha", (int)params.useAlpha)
		.addConstant("pp_Texture", (int)params.texture)
		.addConstant("pp_IgnoreTexA", (int)params.ignoreTexAlpha)
		.addConstant("pp_ShadInstr", params.shaderInstr)
		.addConstant("pp_Offset", (int)params.offset)
		.addConstant("pp_FogCtrl", params.fog)
		.addConstant("pp_Gouraud", (int)params.gouraud)
		.addConstant("pp_BumpMap", (int)params.bumpmap)
		.addConstant("ColorClamping", (int)params.clamping)
		.addConstant("pp_TriLinear", (int)params.trilinear)
		.addConstant("pp_Palette", params.palette)
		.addConstant("pp_SampleTextureFilter", params.sampleTextureFilter)
		.addConstant("pp_MaxFilteredTextureSize", params.maxFilteredTextureSize)
		.addConstant("DIV_POS_Z", (int)params.divPosZ)
		.addConstant("DITHERING", (int)params.dithering)
		.addSource(GouraudSource)
		.addSource(FragmentShaderTop)
		.addSource(FragmentShaderCommon)
		.addSource(FragmentShaderMain);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment, src.generate());
}

vk::UniqueShaderModule ShaderManager::compileShader(const ModVolShaderParams& params)
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex,
			VulkanSource().addConstant("DIV_POS_Z", (int)params.divPosZ)
				.addSource(params.naomi2 ? N2ModVolVertexShaderSource : ModVolVertexShaderSource).generate());
}

vk::UniqueShaderModule ShaderManager::compileModVolFragmentShader(bool divPosZ)
{
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,
			VulkanSource().addConstant("DIV_POS_Z", (int)divPosZ)
				.addSource(ModVolFragmentShaderSource).generate());
}

vk::UniqueShaderModule ShaderManager::compileQuadVertexShader(bool rotate)
{
	VulkanSource src;
	src.addConstant("ROTATE", (int)rotate)
			.addSource(QuadVertexShaderSource);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eVertex, src.generate());
}

vk::UniqueShaderModule ShaderManager::compileQuadFragmentShader(bool ignoreTexAlpha)
{
	VulkanSource src;
	src.addConstant("IGNORE_TEX_ALPHA", (int)ignoreTexAlpha)
			.addSource(QuadFragmentShaderSource);
	return ShaderCompiler::Compile(vk::ShaderStageFlagBits::eFragment,src.generate());
}
