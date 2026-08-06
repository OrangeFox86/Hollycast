/*
	Copyright 2018 flyinghead
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
#include "gl4.h"
#include "rend/gles/glcache.h"
#include "rend/transform_matrix.h"
#include "glsl.h"
#include "gl4naomi2.h"
#include "rend/gles/naomi2.h"
#include "rend/gles/quad.h"

#ifdef LIBRETRO
#include "rend/gles/postprocess.h"
#endif

#include <memory>

//Fragment and vertex shaders code

const char* ShaderHeader = R"(
layout(r32ui, binding = 4) uniform coherent restrict highp uimage2D abufferPointerImg;

layout(binding = 0, offset = 0) uniform atomic_uint buffer_index;
)"
OIT_POLY_PARAM
R"(
layout (binding = 0, std430) coherent restrict buffer PixelBuffer {
	Pixel pixels[];
};

layout (binding = 1, std430) readonly buffer TrPolyParamBuffer {
	PolyParam tr_poly_params[];
};
)";

static const char* VertexShaderSource = R"(
#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION
#endif

// Uniforms 
uniform mat4 ndcMat;
uniform int pp_Number;

// Input
in vec4 in_pos;
in vec4 in_base;
in vec4 in_offs;
in vec2 in_uv;
#if pp_TwoVolumes == 1
in vec4 in_base1;
in vec4 in_offs1;
in vec2 in_uv1;
#endif

// Output
INTERPOLATION out vec4 vtx_base;
INTERPOLATION out vec4 vtx_offs;
out vec3 vtx_uv;
#if pp_TwoVolumes == 1
INTERPOLATION out vec4 vtx_base1;
INTERPOLATION out vec4 vtx_offs1;
out vec2 vtx_uv1;
#endif
flat out uint vtx_index;

void main()
{
	vec4 vpos = ndcMat * in_pos;
	#if DIV_POS_Z == 1
		vpos /= vpos.z;
		vpos.z = vpos.w;
	#endif
	vtx_base = in_base;
	vtx_offs = in_offs;
	vtx_uv = vec3(in_uv, vpos.z);
	#if pp_TwoVolumes == 1
		vtx_base1 = in_base1;
		vtx_offs1 = in_offs1;
		vtx_uv1 = in_uv1;
	#endif
	vtx_index = uint(pp_Number) + uint(gl_VertexID);
	#if pp_Gouraud == 1 && DIV_POS_Z != 1
		vtx_base *= vpos.z;
		vtx_offs *= vpos.z;
		#if pp_TwoVolumes == 1
			vtx_base1 *= vpos.z;
			vtx_offs1 *= vpos.z;
		#endif
	#endif
	
	#if DIV_POS_Z != 1
		vtx_uv.xy *= vpos.z;
		#if pp_TwoVolumes == 1
			vtx_uv1 *= vpos.z;
		#endif
		vpos.w = 1.0;
		vpos.z = 0.0;
	#endif
	gl_Position = vpos;
}
)";

const char* gl4PixelPipelineShader = R"(
#define PI 3.1415926

#define PASS_DEPTH 0
#define PASS_COLOR 1
#define PASS_OIT 2

#if PASS == PASS_DEPTH || PASS == PASS_COLOR || NOUVEAU == 1
out vec4 FragColor;
#endif

#if pp_TwoVolumes == 1
#define IF(x) if (x)
#else
#define IF(x)
#endif

#if pp_Gouraud == 0
#define INTERPOLATION flat
#else
#define INTERPOLATION
#endif

// Uniforms
uniform float cp_AlphaTestValue;
uniform vec4 pp_ClipTest;
uniform vec3 sp_FOG_COL_RAM,sp_FOG_COL_VERT;
uniform float sp_FOG_DENSITY;
uniform float shade_scale_factor;
uniform sampler2D tex0, tex1;
layout(binding = 5) uniform sampler2D fog_table;
uniform highp usampler2D shadow_stencil;
uniform sampler2D DepthTex;
uniform float trilinear_alpha;
uniform vec4 fog_clamp_min;
uniform vec4 fog_clamp_max;
#if pp_Palette != 0
uniform sampler2D palette;
uniform int palette_index;
#endif

uniform ivec2 blend_mode[2];
#if pp_TwoVolumes == 1
uniform bool use_alpha[2];
uniform bool ignore_tex_alpha[2];
uniform int shading_instr[2];
uniform int fog_control[2];
#endif

// Input
INTERPOLATION in vec4 vtx_base;
INTERPOLATION in vec4 vtx_offs;
in vec3 vtx_uv;
#if pp_TwoVolumes == 1
INTERPOLATION in vec4 vtx_base1;
INTERPOLATION in vec4 vtx_offs1;
in vec2 vtx_uv1;
#endif
flat in uint vtx_index;

float fog_mode2(float w)
{
	float z = clamp(
#if DIV_POS_Z == 1
					sp_FOG_DENSITY / w
#else
					sp_FOG_DENSITY * w
#endif
									  , 1.0, 255.9999);
	float exp = floor(log2(z));
	float m = z * 16.0 / pow(2.0, exp) - 16.0;
	float idx = floor(m) + exp * 16.0 + 0.5;
	vec4 fog_coef = texture(fog_table, vec2(idx / 128.0, 0.75 - (m - floor(m)) / 2.0));
	return fog_coef.r;
}

vec4 fog_clamp(vec4 col)
{
#if FogClamping == 1
	return clamp(col, fog_clamp_min, fog_clamp_max);
#else
	return col;
#endif
}

#if pp_Palette != 0

vec4 getPaletteEntry(float colIdx)
{
	int color_idx = int(floor(colIdx * 255.0 + 0.5)) + palette_index;
	ivec2 c = ivec2(color_idx % 32, color_idx / 32);
	return texelFetch(palette, c, 0);
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
	// Match the xBRZ width buckets: 8x16 matches 8x8, while 16x8 matches 16x16.
	return pp_MaxFilteredTextureSize >= 1024 || texSize.x <= float(pp_MaxFilteredTextureSize);
}

float jincDistance(vec2 a, vec2 b)
{
	vec2 v = b - a;
	return sqrt(dot(v, v));
}

vec4 jincResampler(vec4 x)
{
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
	vec4 color = mix(mix(vec4(c00.rgb * c00.a, c00.a), vec4(c10.rgb * c10.a, c10.a), weight.x),
			mix(vec4(c01.rgb * c01.a, c01.a), vec4(c11.rgb * c11.a, c11.a), weight.x), weight.y);

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

	color = mix(color, clamp(color, min(min(c11, c21), min(c12, c22)), max(max(c11, c21), max(c12, c22))), arStrength);
	if (color.a > 0.0)
		color.rgb /= color.a;
	if (noEdgeBlend)
		color.a = color.a >= 0.5 ? 1.0 : 0.0;
	return color;
}

bool colorEq(vec4 a, vec4 b) { return distance(a, b) < 0.001; }
bool mmpxAllEq2(vec4 value, vec4 a, vec4 b) { return colorEq(value, a) && colorEq(value, b); }
bool mmpxAllEq3(vec4 value, vec4 a, vec4 b, vec4 c) { return colorEq(value, a) && colorEq(value, b) && colorEq(value, c); }
bool mmpxAllEq4(vec4 value, vec4 a, vec4 b, vec4 c, vec4 d) { return colorEq(value, a) && colorEq(value, b) && colorEq(value, c) && colorEq(value, d); }
bool mmpxAnyEq3(vec4 value, vec4 a, vec4 b, vec4 c) { return colorEq(value, a) || colorEq(value, b) || colorEq(value, c); }
bool mmpxNoneEq4(vec4 value, vec4 a, vec4 b, vec4 c, vec4 d) { return !colorEq(value, a) && !colorEq(value, b) && !colorEq(value, c) && !colorEq(value, d); }
float mmpxLuma(vec4 color) { return dot(color.rgb, vec3(255.0)) * (1.0 - color.a); }
vec4 mmpxResultColor(vec4 result, vec4 center) { return pp_SampleTextureFilter == 6 && !colorEq(result, center) ? mix(result, center, 0.25) : result; }

vec4 mmpxSample(sampler2D sourceTex, vec2 uv)
{
	vec2 texSize = vec2(textureSize(sourceTex, 0));
	vec2 bcoords = floor(uv * texSize);
	vec2 baseUV = (bcoords + 0.5) / texSize;
	vec2 invTexSize = vec2(1.0) / texSize;

	// MMPX is a MIT-licensed style-preserving pixel-art magnifier by Morgan McGuire and Mara Gagiu.
	vec4 A = sampleSourceTexel(sourceTex, baseUV + vec2(-1.0, -1.0) * invTexSize);
	vec4 B = sampleSourceTexel(sourceTex, baseUV + vec2( 0.0, -1.0) * invTexSize);
	vec4 C = sampleSourceTexel(sourceTex, baseUV + vec2( 1.0, -1.0) * invTexSize);
	vec4 D = sampleSourceTexel(sourceTex, baseUV + vec2(-1.0,  0.0) * invTexSize);
	vec4 E = sampleSourceTexel(sourceTex, baseUV);
	vec4 F = sampleSourceTexel(sourceTex, baseUV + vec2( 1.0,  0.0) * invTexSize);
	vec4 G = sampleSourceTexel(sourceTex, baseUV + vec2(-1.0,  1.0) * invTexSize);
	vec4 H = sampleSourceTexel(sourceTex, baseUV + vec2( 0.0,  1.0) * invTexSize);
	vec4 I = sampleSourceTexel(sourceTex, baseUV + vec2( 1.0,  1.0) * invTexSize);
	vec4 J = E, K = E, L = E, M = E;

	if (!mmpxAllEq4(E, A, B, C, D) || !mmpxAllEq4(E, F, G, H, I))
	{
		vec4 P = sampleSourceTexel(sourceTex, baseUV + vec2( 0.0, -2.0) * invTexSize);
		vec4 Q = sampleSourceTexel(sourceTex, baseUV + vec2(-2.0,  0.0) * invTexSize);
		vec4 R = sampleSourceTexel(sourceTex, baseUV + vec2( 2.0,  0.0) * invTexSize);
		vec4 S = sampleSourceTexel(sourceTex, baseUV + vec2( 0.0,  2.0) * invTexSize);
		float Bl = mmpxLuma(B), Dl = mmpxLuma(D), El = mmpxLuma(E), Fl = mmpxLuma(F), Hl = mmpxLuma(H);

		if ((colorEq(D, B) && !colorEq(D, H) && !colorEq(D, F)) && (El >= Dl || colorEq(E, A)) && mmpxAnyEq3(E, A, C, G) && ((El < Dl) || !colorEq(A, D) || !colorEq(E, P) || !colorEq(E, Q))) J = D;
		if ((colorEq(B, F) && !colorEq(B, D) && !colorEq(B, H)) && (El >= Bl || colorEq(E, C)) && mmpxAnyEq3(E, A, C, I) && ((El < Bl) || !colorEq(C, B) || !colorEq(E, P) || !colorEq(E, R))) K = B;
		if ((colorEq(H, D) && !colorEq(H, F) && !colorEq(H, B)) && (El >= Hl || colorEq(E, G)) && mmpxAnyEq3(E, A, G, I) && ((El < Hl) || !colorEq(G, H) || !colorEq(E, S) || !colorEq(E, Q))) L = H;
		if ((colorEq(F, H) && !colorEq(F, B) && !colorEq(F, D)) && (El >= Fl || colorEq(E, I)) && mmpxAnyEq3(E, C, G, I) && ((El < Fl) || !colorEq(I, H) || !colorEq(E, R) || !colorEq(E, S))) M = F;

		if (pp_SampleTextureFilter == 7)
		{
			if ((!colorEq(E, F) && mmpxAllEq4(E, C, I, D, Q) && mmpxAllEq2(F, B, H)) && !colorEq(F, sampleSourceTexel(sourceTex, baseUV + vec2( 3.0,  0.0) * invTexSize))) K = M = F;
			if ((!colorEq(E, D) && mmpxAllEq4(E, A, G, F, R) && mmpxAllEq2(D, B, H)) && !colorEq(D, sampleSourceTexel(sourceTex, baseUV + vec2(-3.0,  0.0) * invTexSize))) J = L = D;
			if ((!colorEq(E, H) && mmpxAllEq4(E, G, I, B, P) && mmpxAllEq2(H, D, F)) && !colorEq(H, sampleSourceTexel(sourceTex, baseUV + vec2( 0.0,  3.0) * invTexSize))) L = M = H;
			if ((!colorEq(E, B) && mmpxAllEq4(E, A, C, H, S) && mmpxAllEq2(B, D, F)) && !colorEq(B, sampleSourceTexel(sourceTex, baseUV + vec2( 0.0, -3.0) * invTexSize))) J = K = B;
			if (Bl < El && mmpxAllEq4(E, G, H, I, S) && mmpxNoneEq4(E, A, D, C, F)) J = K = B;
			if (Hl < El && mmpxAllEq4(E, A, B, C, P) && mmpxNoneEq4(E, D, G, I, F)) L = M = H;
			if (Fl < El && mmpxAllEq4(E, A, D, G, Q) && mmpxNoneEq4(E, B, C, I, H)) K = M = F;
			if (Dl < El && mmpxAllEq4(E, C, F, I, R) && mmpxNoneEq4(E, B, A, G, H)) J = L = D;
		}
	}

	vec2 fpart = fract(uv * texSize);
	vec4 result = (fpart.x < 0.5) ? ((fpart.y < 0.5) ? J : L) : ((fpart.y < 0.5) ? K : M);
	return mmpxResultColor(result, E);
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
		for (int x = -1; x <= 1; x++)
		{
			vec4 sampleColor = sampleSourceTexel(sourceTex, (base + vec2(x, y) + 0.5) * invTexSize);
			minColor = min(minColor, sampleColor);
			maxColor = max(maxColor, sampleColor);
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
		return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
	if (x < 2.0)
		return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x + (8.0 * b + 24.0 * c)) / 6.0;
	return 0.0;
}

float reconstructionWeight(float x, int filterMode)
{
	if (filterMode == 0) return lanczosWeight(x, 2.0);
	if (filterMode == 1) return lanczosWeight(x, 3.0);
	if (filterMode == 2) return spline36Weight(x);
	if (filterMode == 3) return cubicWeight(x, 0.378215755, 0.310892122);
	if (filterMode == 4) return cubicWeight(x, 0.262014512, 0.368992744);
	if (filterMode == 5) return cubicWeight(x, 0.0, 0.5);
	return cubicWeight(x, 1.0 / 3.0, 1.0 / 3.0);
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

vec4 filteredTextureSample(sampler2D sourceTex, vec3 coords)
{
	if (!sampleTextureFilterEligible(sourceTex))
		return legacyTextureSample(sourceTex, coords);

	vec2 uv = projectTextureCoords(coords);
	if (pp_SampleTextureFilter == 1) return alphaWeightedBilinear(sourceTex, uv, false);
	if (pp_SampleTextureFilter == 2) return alphaWeightedBilinear(sourceTex, uv, true);
	if (pp_SampleTextureFilter == 3) return jinc2Sample(sourceTex, uv, false);
	if (pp_SampleTextureFilter == 4) return jinc2Sample(sourceTex, uv, true);
	if (pp_SampleTextureFilter == 5 || pp_SampleTextureFilter == 6 || pp_SampleTextureFilter == 7) return mmpxSample(sourceTex, uv);
	if (pp_SampleTextureFilter == 8) return jinc2AntiRingSample(sourceTex, uv, 0.0);
	if (pp_SampleTextureFilter == 9) return jinc2AntiRingSample(sourceTex, uv, 0.18);
	if (pp_SampleTextureFilter == 10) return jinc2AntiRingSample(sourceTex, uv, 0.08);
	if (pp_SampleTextureFilter == 11) return jinc2AntiRingSample(sourceTex, uv, 0.22);
	if (pp_SampleTextureFilter == 12) return separableReconstructionSample(sourceTex, uv, 0, true);
	if (pp_SampleTextureFilter == 13) return separableReconstructionSample(sourceTex, uv, 1, true);
	if (pp_SampleTextureFilter == 14) return separableReconstructionSample(sourceTex, uv, 2, false);
	if (pp_SampleTextureFilter == 15) return separableReconstructionSample(sourceTex, uv, 3, false);
	if (pp_SampleTextureFilter == 16) return separableReconstructionSample(sourceTex, uv, 4, false);
	if (pp_SampleTextureFilter == 17) return separableReconstructionSample(sourceTex, uv, 5, false);
	if (pp_SampleTextureFilter == 18) return separableReconstructionSample(sourceTex, uv, 6, false);
	return legacyTextureSample(sourceTex, coords);
}

)" R"(

#endif

#if PASS == PASS_OIT

uint getNextPixelIndex()
{
	uint index = atomicCounterIncrement(buffer_index);
	if (index >= uint(pixels.length()))
		// Buffer overflow
		discard;

	return index;
}

#endif

void main()
{
	setFragDepth(vtx_uv.z);
	
	#if PASS == PASS_OIT
		// Manual depth testing
		float frontDepth = texture(DepthTex, gl_FragCoord.xy / vec2(textureSize(DepthTex, 0))).r;
		if (gl_FragDepth < frontDepth)
			discard;
	#endif
	
	// Clip inside the box
	#if pp_ClipInside == 1
		if (gl_FragCoord.x >= pp_ClipTest.x && gl_FragCoord.x <= pp_ClipTest.z
				&& gl_FragCoord.y >= pp_ClipTest.y && gl_FragCoord.y <= pp_ClipTest.w)
			discard;
	#endif
	
	vec4 color = vtx_base;
	vec4 offset = vtx_offs;
	bool area1 = false;
	ivec2 cur_blend_mode = blend_mode[0];
	
	#if pp_TwoVolumes == 1
		bool cur_use_alpha = use_alpha[0];
		bool cur_ignore_tex_alpha = ignore_tex_alpha[0];
		int cur_shading_instr = shading_instr[0];
		int cur_fog_control = fog_control[0];
		#if PASS == PASS_COLOR
			uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / vec2(textureSize(shadow_stencil, 0)));
			if (stencil.r == 0x81u) {
				color = vtx_base1;
				offset = vtx_offs1;
				area1 = true;
				cur_blend_mode = blend_mode[1];
				cur_use_alpha = use_alpha[1];
				cur_ignore_tex_alpha = ignore_tex_alpha[1];
				cur_shading_instr = shading_instr[1];
				cur_fog_control = fog_control[1];
			}
		#endif
	#endif
	#if pp_Gouraud == 1 && DIV_POS_Z != 1
		color /= vtx_uv.z;
		offset /= vtx_uv.z;
	#endif
	
	#if pp_UseAlpha==0 || pp_TwoVolumes == 1
		IF(!cur_use_alpha)
			color.a=1.0;
	#endif
	#if pp_FogCtrl==3 || pp_TwoVolumes == 1 // LUT Mode 2
		IF(cur_fog_control == 3)
			color = vec4(sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z));
	#endif
	#if pp_Texture==1
	{
		vec4 texcol;
		#if pp_TwoVolumes == 1
			if (area1) {
				#if pp_SampleTextureFilter != 0
					texcol = filteredTextureSample(tex1, vec3(vtx_uv1.xy, vtx_uv.z));
				#else
				#if DIV_POS_Z == 1
					texcol = texture(tex1, vtx_uv1);
				#else
					texcol = textureProj(tex1, vec3(vtx_uv1.xy, vtx_uv.z));
				#endif
				#endif
			}
			else
		#endif
		#if pp_SampleTextureFilter != 0
			texcol = filteredTextureSample(tex0, vtx_uv);
		#elif pp_Palette == 0
			#if DIV_POS_Z == 1
				texcol = texture(tex0, vtx_uv.xy);
			#else
				texcol = textureProj(tex0, vtx_uv);
			#endif
		#elif pp_Palette == 1
			texcol = palettePixel(tex0, vtx_uv);
		#else
			texcol = palettePixelBilinear(tex0, vtx_uv);
		#endif

		#if pp_BumpMap == 1
			float s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;
			float r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;
			texcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);
			texcol.rgb = vec3(1.0, 1.0, 1.0);	
		#else
			#if pp_IgnoreTexA==1 || pp_TwoVolumes == 1
				IF(cur_ignore_tex_alpha)
					texcol.a=1.0;	
			#endif
		#endif
		#if pp_ShadInstr==0 || pp_TwoVolumes == 1 // DECAL
		IF(cur_shading_instr == 0)
		{
			color=texcol;
		}
		#endif
		#if pp_ShadInstr==1 || pp_TwoVolumes == 1 // MODULATE
		IF(cur_shading_instr == 1)
		{
			color.rgb*=texcol.rgb;
			color.a=texcol.a;
		}
		#endif
		#if pp_ShadInstr==2 || pp_TwoVolumes == 1 // DECAL ALPHA
		IF(cur_shading_instr == 2)
		{
			color.rgb=mix(color.rgb,texcol.rgb,texcol.a);
		}
		#endif
		#if  pp_ShadInstr==3 || pp_TwoVolumes == 1 // MODULATE ALPHA
		IF(cur_shading_instr == 3)
		{
			color*=texcol;
		}
		#endif
		
		#if pp_Offset==1 && pp_BumpMap == 0
		{
			color.rgb += offset.rgb;
		}
		#endif
	}
	#endif
	#if PASS == PASS_COLOR && pp_TwoVolumes == 0
		uvec4 stencil = texture(shadow_stencil, gl_FragCoord.xy / vec2(textureSize(shadow_stencil, 0)));
		if (stencil.r == 0x81u)
			color.rgb *= shade_scale_factor;
	#endif
	
	color = fog_clamp(color);
	
	#if pp_FogCtrl==0 || pp_TwoVolumes == 1 // LUT
		IF(cur_fog_control == 0)
		{
			color.rgb = mix(color.rgb, sp_FOG_COL_RAM.rgb, fog_mode2(vtx_uv.z)); 
		}
	#endif
	#if pp_Offset==1 && pp_BumpMap == 0 && (pp_FogCtrl == 1 || pp_TwoVolumes == 1)  // Per vertex
		IF(cur_fog_control == 1)
		{
			color.rgb=mix(color.rgb, sp_FOG_COL_VERT.rgb, offset.a);
		}
	#endif
	
	color *= trilinear_alpha;
	
	#if cp_AlphaTest == 1
		color.a = round(color.a * 255.0) / 255.0;
		if (cp_AlphaTestValue > color.a)
			discard;
		color.a = 1.0;
	#endif

	//color.rgb=vec3(gl_FragCoord.w * sp_FOG_DENSITY / 128.0);
	
	#if PASS == PASS_COLOR 
		FragColor = color;
	#elif PASS == PASS_OIT
		ivec2 coords = ivec2(gl_FragCoord.xy);
		uint idx =  getNextPixelIndex();
		
		Pixel pixel;
		pixel.color = packColors(clamp(color, vec4(0.0), vec4(1.0)));
		pixel.depth = gl_FragDepth;
		pixel.seq_num = vtx_index;
		pixel.next = imageAtomicExchange(abufferPointerImg, coords, idx);
		pixels[idx] = pixel;
		
#if NOUVEAU == 0
		discard;
#else
		// nouveau may be optimizing a bit too aggressively here
		FragColor = vec4(0.0);
#endif
		
	#endif
}
)";

static const char* ModifierVolumeShader = R"(
in vec3 vtx_uv;

void main()
{
	setFragDepth(vtx_uv.z);
}
)";

class Vertex4Source : public OpenGl4Source
{
public:
	Vertex4Source(bool gouraud, bool divPosZ, bool twoVolumes) : OpenGl4Source()
	{
		addConstant("pp_TwoVolumes", twoVolumes);
		addConstant("pp_Gouraud", gouraud);
		addConstant("DIV_POS_Z", divPosZ);

		addSource(VertexShaderSource);
	}
};

class Fragment4ShaderSource : public OpenGl4Source
{
public:
	Fragment4ShaderSource(const gl4PipelineShader* s) : OpenGl4Source()
	{
		addConstant("cp_AlphaTest", s->cp_AlphaTest);
		addConstant("pp_ClipInside", s->pp_InsideClipping);
		addConstant("pp_UseAlpha", s->pp_UseAlpha);
		addConstant("pp_Texture", s->pp_Texture);
		addConstant("pp_IgnoreTexA", s->pp_IgnoreTexA);
		addConstant("pp_ShadInstr", s->pp_ShadInstr);
		addConstant("pp_Offset", s->pp_Offset);
		addConstant("pp_FogCtrl", s->pp_FogCtrl);
		addConstant("pp_TwoVolumes", s->pp_TwoVolumes);
		addConstant("pp_Gouraud", s->pp_Gouraud);
		addConstant("pp_BumpMap", s->pp_BumpMap);
		addConstant("FogClamping", s->fog_clamping);
		addConstant("pp_Palette", s->palette);
		addConstant("pp_SampleTextureFilter", s->sampleTextureFilter);
		addConstant("pp_MaxFilteredTextureSize", s->maxFilteredTextureSize);
		addConstant("NOUVEAU", gl.mesa_nouveau);
		addConstant("PASS", (int)s->pass);
		addConstant("DIV_POS_Z", s->divPosZ);

		addSource(ShaderHeader);
		addSource(gl4PixelPipelineShader);
	}
};

gl4_ctx gl4;

struct gl4ShaderUniforms_t gl4ShaderUniforms;
int max_image_width;
int max_image_height;

bool gl4CompilePipelineShader(gl4PipelineShader* s, const char *fragment_source /* = nullptr */,
		const char *vertex_source /* = nullptr */)
{
	std::string vertexSource;
	if (s->naomi2)
		vertexSource = N2Vertex4Source(s).generate();
	else
		vertexSource = Vertex4Source(s->pp_Gouraud, s->divPosZ, s->pp_TwoVolumes).generate();
	Fragment4ShaderSource fragmentSource(s);

	s->program = gl_CompileAndLink(vertex_source != nullptr ? vertex_source : vertexSource.c_str(),
			fragment_source != nullptr ? fragment_source : fragmentSource.generate().c_str());

	//setup texture 0 as the input for the shader
	GLint gu = glGetUniformLocation(s->program, "tex0");
	if (s->pp_Texture == 1 && gu != -1)
		glUniform1i(gu, 0);
	// Setup texture 1 as the input for area 1 in two volume mode
	gu = glGetUniformLocation(s->program, "tex1");
	if (s->pp_Texture == 1 && gu != -1)
		glUniform1i(gu, 1);

	//get the uniform locations
	s->pp_ClipTest      = glGetUniformLocation(s->program, "pp_ClipTest");

	s->sp_FOG_DENSITY   = glGetUniformLocation(s->program, "sp_FOG_DENSITY");

	s->cp_AlphaTestValue= glGetUniformLocation(s->program, "cp_AlphaTestValue");

	//FOG_COL_RAM,FOG_COL_VERT,FOG_DENSITY;
	if (s->pp_FogCtrl==1 && s->pp_Texture==1)
		s->sp_FOG_COL_VERT=glGetUniformLocation(s->program, "sp_FOG_COL_VERT");
	else
		s->sp_FOG_COL_VERT=-1;
	if (s->pp_FogCtrl==0 || s->pp_FogCtrl==3)
	{
		s->sp_FOG_COL_RAM=glGetUniformLocation(s->program, "sp_FOG_COL_RAM");
	}
	else
	{
		s->sp_FOG_COL_RAM=-1;
	}
	s->shade_scale_factor = glGetUniformLocation(s->program, "shade_scale_factor");

	// Use texture 1 for depth texture
	gu = glGetUniformLocation(s->program, "DepthTex");
	if (gu != -1)
		glUniform1i(gu, 2);		// GL_TEXTURE2

	s->trilinear_alpha = glGetUniformLocation(s->program, "trilinear_alpha");
	
	if (s->fog_clamping)
	{
		s->fog_clamp_min = glGetUniformLocation(s->program, "fog_clamp_min");
		s->fog_clamp_max = glGetUniformLocation(s->program, "fog_clamp_max");
	}
	else
	{
		s->fog_clamp_min = -1;
		s->fog_clamp_max = -1;
	}
	s->ndcMat = glGetUniformLocation(s->program, "ndcMat");

	// Shadow stencil for OP/PT rendering pass
	gu = glGetUniformLocation(s->program, "shadow_stencil");
	if (gu != -1)
		glUniform1i(gu, 3);		// GL_TEXTURE3

	s->pp_Number = glGetUniformLocation(s->program, "pp_Number");

	s->blend_mode = glGetUniformLocation(s->program, "blend_mode");
	s->use_alpha = glGetUniformLocation(s->program, "use_alpha");
	s->ignore_tex_alpha = glGetUniformLocation(s->program, "ignore_tex_alpha");
	s->shading_instr = glGetUniformLocation(s->program, "shading_instr");
	s->fog_control = glGetUniformLocation(s->program, "fog_control");

	gu = glGetUniformLocation(s->program, "palette");
	if (gu != -1)
		glUniform1i(gu, 6);		// GL_TEXTURE6
	s->palette_index = glGetUniformLocation(s->program, "palette_index");
	s->ditherDivisor = glGetUniformLocation(s->program, "ditherDivisor");

	if (s->naomi2)
		initN2Uniforms(s);

	return glIsProgram(s->program)==GL_TRUE;
}

static void gl4_delete_shaders()
{
	for (auto it : gl4.shaders)
	{
		if (it.second.program != 0)
			glcache.DeleteProgram(it.second.program);
	}
	gl4.shaders.clear();
	glcache.DeleteProgram(gl4.modvol_shader.program);
	gl4.modvol_shader.program = 0;
	glcache.DeleteProgram(gl4.n2ModVolShader.program);
	gl4.n2ModVolShader.program = 0;
}

static void gl4_term()
{
	for (auto& buffer : gl4.vbo.geometry)
		buffer.reset();
	for (auto& buffer : gl4.vbo.modvols)
		buffer.reset();
	for (auto& buffer : gl4.vbo.idxs)
		buffer.reset();
	for (auto& buffer : gl4.vbo.tr_poly_params)
		buffer.reset();
	gl4_delete_shaders();
	for (auto& vao : gl4.vbo.main_vao)
		vao.term();
	for (auto& vao : gl4.vbo.modvol_vao)
		vao.term();
	// Restore the gl context to a decent state in case of exception
	if (gl.ofbo.origFbo != 0)
		glBindFramebuffer(GL_FRAMEBUFFER, gl.ofbo.origFbo);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
}

static void create_modvol_shader()
{
	if (gl4.modvol_shader.program != 0)
		return;
	Vertex4Source vertexShader(false, config::NativeDepthInterpolation, false);
	OpenGl4Source fragmentShader;
	fragmentShader.addConstant("DIV_POS_Z", config::NativeDepthInterpolation)
		.addSource(ShaderHeader)
		.addSource(ModifierVolumeShader);

	gl4.modvol_shader.program = gl_CompileAndLink(vertexShader.generate().c_str(), fragmentShader.generate().c_str());
	gl4.modvol_shader.ndcMat = glGetUniformLocation(gl4.modvol_shader.program, "ndcMat");

	N2Vertex4Source n2VertexShader;
	n2VertexShader.setConstant("MODIFIER_VOLUME", true);
	fragmentShader.setConstant("DIV_POS_Z", false);
	gl4.n2ModVolShader.program = gl_CompileAndLink(n2VertexShader.generate().c_str(), fragmentShader.generate().c_str());
	gl4.n2ModVolShader.ndcMat = glGetUniformLocation(gl4.n2ModVolShader.program, "ndcMat");
	gl4.n2ModVolShader.mvMat = glGetUniformLocation(gl4.n2ModVolShader.program, "mvMat");
	gl4.n2ModVolShader.projMat = glGetUniformLocation(gl4.n2ModVolShader.program, "projMat");
}

static void gl_create_resources()
{
	if (gl4.vbo.geometry[0] != nullptr)
		// Assume the resources have already been created
		return;

	//create vbos
	for (u32 i = 0; i < std::size(gl4.vbo.geometry); i++)
	{
		gl4.vbo.geometry[i] = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER);
		gl4.vbo.modvols[i] = std::make_unique<GlBuffer>(GL_ARRAY_BUFFER);
		gl4.vbo.idxs[i] = std::make_unique<GlBuffer>(GL_ELEMENT_ARRAY_BUFFER);
		// Create the buffer for Translucent poly params
		gl4.vbo.tr_poly_params[i] = std::make_unique<GlBuffer>(GL_SHADER_STORAGE_BUFFER);
		gl4.vbo.bufferIndex = i;
		gl4SetupMainVBO();
		gl4SetupModvolVBO();
	}
	GlVertexArray::unbind();

	gl.quadDrawer = std::make_unique<GlQuadDrawer>();
	glCheck();
}

struct OpenGL4Renderer : OpenGLRenderer
{
	bool Init() override;

	void Term() override
	{
		termABuffer();
		glcache.DeleteTextures(1, &stencilTexId);
		stencilTexId = 0;
		glcache.DeleteTextures(1, &depthTexId);
		depthTexId = 0;
		glcache.DeleteTextures(2, opaqueTexId);
		opaqueTexId[0] = opaqueTexId[1] = 0;
		glDeleteFramebuffers(2, geom_fbo);
		geom_fbo[0] = geom_fbo[1] = 0;
		glDeleteSamplers(2, texSamplers);
		texSamplers[0] = texSamplers[1] = 0;

		TexCache.Clear();
		termGLCommon();
		gl4_term();
	}

	bool Render() override
	{
		saveCurrentFramebuffer();
		renderFrame(gl.rendContext->framebufferWidth, gl.rendContext->framebufferHeight);
		if (gl.rendContext->isRTT) {
			restoreCurrentFramebuffer();
			return false;
		}

		if (!config::EmulateFramebuffer)
		{
			frameRendered = true;
			clearLastFrame = false;
			drawOSD();
			renderVideoRouting();
		}
		restoreCurrentFramebuffer();

		return true;
	}

	GLenum getFogTextureSlot() const override {
		return GL_TEXTURE5;
	}
	GLenum getPaletteTextureSlot() const override {
		return GL_TEXTURE6;
	}

	bool renderFrame(int width, int height);
};

//setup
void gl_DebugOutput(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
		const GLchar *message, const void *userParam);

bool OpenGL4Renderer::Init()
{
	findGLVersion();
	if ((!gl.is_gles && (gl.gl_major < 4 || (gl.gl_major == 4 && gl.gl_minor < 3)))
			|| (gl.is_gles && (gl.gl_major < 3 || (gl.gl_major == 3 && gl.gl_minor < 2))))
	{
		WARN_LOG(RENDERER, "Warning: OpenGL version doesn't support per-pixel sorting.");
		return false;
	}
	INFO_LOG(RENDERER, "Per-pixel sorting enabled");

	glcache.DisableCache();

    //glEnable(GL_DEBUG_OUTPUT);
    //glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    //glDebugMessageCallback(gl_DebugOutput, NULL);
    //glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);

	gl_create_resources();

	initABuffer();

	if (config::TextureUpscale > 1)
	{
		// Trick to preload the tables used by xBRZ
		u32 src[] { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
		u32 dst[16];
		UpscalexBRZ(2, src, dst, 2, 2, false);
	}
	updateFogTable = true;
	TextureCacheData::SetDirectXColorOrder(false);
	TextureCacheData::setUploadToGPUFlavor();

	return true;
}

static void resize(int w, int h)
{
	if (w > max_image_width || h > max_image_height || stencilTexId == 0)
	{
		if (w > max_image_width)
			max_image_width = w;
		if (h > max_image_height)
			max_image_height = h;

		if (stencilTexId != 0)
		{
			glcache.DeleteTextures(1, &stencilTexId);
			stencilTexId = 0;
		}
		if (depthTexId != 0)
		{
			glcache.DeleteTextures(1, &depthTexId);
			depthTexId = 0;
		}
		if (opaqueTexId[0] != 0)
		{
			glcache.DeleteTextures(2, opaqueTexId);
			opaqueTexId[0] = 0;
		}
		gl4CreateTextures(max_image_width, max_image_height);
		reshapeABuffer(max_image_width, max_image_height);
	}
}

bool OpenGL4Renderer::renderFrame(int rendering_width, int rendering_height)
{
	if (!config::EmulateFramebuffer)
		initVideoRoutingFrameBuffer();
	
	const bool is_rtt = gl.rendContext->isRTT;

	gl.matrices.CalcMatrices(gl.rendContext, rendering_width, rendering_height);
	gl4ShaderUniforms.ndcMat = gl.matrices.GetNormalMatrix();
	
	/*
		Handle Dc to screen scaling
	*/
	if (!is_rtt)
		getVideoShift(gl.ofbo.shiftX, gl.ofbo.shiftY);
	resize(rendering_width, rendering_height);
	
	//VERT and RAM fog color constants
	FOG_COL_VERT.getRGBColor(gl4ShaderUniforms.ps_FOG_COL_VERT);
	FOG_COL_RAM.getRGBColor(gl4ShaderUniforms.ps_FOG_COL_RAM);

	//Fog density constant
	gl4ShaderUniforms.fog_den_float = FOG_DENSITY.get() * config::ExtraDepthScale;

	gl.rendContext->fog_clamp_min.getRGBAColor(gl4ShaderUniforms.fog_clamp_min);
	gl.rendContext->fog_clamp_max.getRGBAColor(gl4ShaderUniforms.fog_clamp_max);
	
	if (config::ModifierVolumes)
	{
		create_modvol_shader();
		glcache.UseProgram(gl4.modvol_shader.program);
		glUniformMatrix4fv(gl4.modvol_shader.ndcMat, 1, GL_FALSE, &gl4ShaderUniforms.ndcMat[0][0]);

		glcache.UseProgram(gl4.n2ModVolShader.program);
		glUniformMatrix4fv(gl4.n2ModVolShader.ndcMat, 1, GL_FALSE, &gl4ShaderUniforms.ndcMat[0][0]);
	}
	for (auto& it : gl4.shaders)
		resetN2UniformCache(&it.second);
	gl4ShaderUniforms.PT_ALPHA=(PT_ALPHA_REF&0xFF)/255.0f;

	glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);

	GLuint output_fbo;
	//setup render target first
	if (is_rtt)
		output_fbo = BindRTT(false);
	else
	{
		this->width = rendering_width;
		this->height = rendering_height;
#ifdef LIBRETRO
		if (config::EmulateFramebuffer)
			output_fbo = init_output_framebuffer(rendering_width, rendering_height);
		else
			output_fbo = postProcessor.getFramebuffer(rendering_width, rendering_height);
		glViewport(0, 0, rendering_width, rendering_height);
#else
		output_fbo = init_output_framebuffer(rendering_width, rendering_height);
#endif
	}
	if (output_fbo == 0)
		return false;

	gl4.vbo.nextBuffer();
	glcache.Disable(GL_SCISSOR_TEST);
	if (!is_rtt)
		glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);

	//Main VBO
	//move vertex to gpu
	gl4.vbo.getVertexBuffer()->update(gl.rendContext->verts.data(), gl.rendContext->verts.size() * sizeof(decltype(*gl.rendContext->verts.data())));
	gl4.vbo.getIndexBuffer()->update(gl.rendContext->idx.data(), gl.rendContext->idx.size() * sizeof(decltype(*gl.rendContext->idx.data())));

	//Modvol VBO
	if (!gl.rendContext->modtrig.empty())
		gl4.vbo.getModVolBuffer()->update(gl.rendContext->modtrig.data(), gl.rendContext->modtrig.size() * sizeof(decltype(*gl.rendContext->modtrig.data())));

	// TR PolyParam data
	if (!gl.rendContext->global_param_tr.empty())
	{
		std::vector<u32> trPolyParams(gl.rendContext->global_param_tr.size() * 2);
		int i = 0;
		for (const PolyParam& pp : gl.rendContext->global_param_tr)
		{
			trPolyParams[i++] = (pp.tsp.full & 0xffff00c0) | ((pp.isp.full >> 16) & 0xe400) | ((pp.pcw.full >> 7) & 1);
			trPolyParams[i++] = pp.tsp1.full;
		}
		gl4.vbo.getPolyParamBuffer()->update(trPolyParams.data(), trPolyParams.size() * sizeof(u32));
	}
	else {
		// Fake one if no TR geometry to avoid a crash
		u32 dummy;
		gl4.vbo.getPolyParamBuffer()->update(&dummy, sizeof(dummy));
	}
	// Declare storage
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, gl4.vbo.getPolyParamBuffer()->getName());
	glCheck();

	Rect scissor = gl.matrices.getBaseScissor();
	gl4ShaderUniforms.base_clipping.x = scissor.origin.x;
	gl4ShaderUniforms.base_clipping.y = scissor.origin.y;
	gl4ShaderUniforms.base_clipping.width = scissor.size.x;
	gl4ShaderUniforms.base_clipping.height = scissor.size.y;
	if (is_rtt) {
		// Render to texture
		gl4ShaderUniforms.base_clipping.enabled = true;
	}
	else
	{
		// Render to screen
		if (scissor.origin.x != 0 || scissor.origin.y != 0
				|| scissor.size.x < (int)gl.rendContext->framebufferWidth
				|| scissor.size.y < (int)gl.rendContext->framebufferHeight)
			gl4ShaderUniforms.base_clipping.enabled = true;
		else
			gl4ShaderUniforms.base_clipping.enabled = false;
	}
	if (gl4ShaderUniforms.base_clipping.enabled) {
		glcache.Scissor(gl4ShaderUniforms.base_clipping.x, gl4ShaderUniforms.base_clipping.y,
				gl4ShaderUniforms.base_clipping.width, gl4ShaderUniforms.base_clipping.height);
		glcache.Enable(GL_SCISSOR_TEST);
	}

	gl4DrawStrips(output_fbo, rendering_width, rendering_height);
#ifdef LIBRETRO
	if (!is_rtt && !config::EmulateFramebuffer)
		postProcessor.render(glsm_get_current_framebuffer());
#endif

	if (is_rtt)
		ReadRTTBuffer();
	else if (config::EmulateFramebuffer)
		writeFramebufferToVRAM();
	else
	{
		gl.ofbo.aspectRatio = getOutputFramebufferAspectRatio();
#ifndef LIBRETRO
		gl.ofbo2.ready = false;
		renderLastFrame();
#endif
	}
	GlVertexArray::unbind();

	return !is_rtt;
}

Renderer* rend_GL4()
{
	return new OpenGL4Renderer();
}
