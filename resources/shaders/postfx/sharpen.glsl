#pragma parameter SHARPEN "Sharpen" 0.25 0.00 1.00 0.01

void main()
{
	vec2 uv = vTexCoord;
	vec2 texel = 1.0 / SourceSize;

	vec4 center = texture(Source, uv);
	vec3 n = texture(Source, uv + vec2(0.0, texel.y)).rgb;
	vec3 s = texture(Source, uv - vec2(0.0, texel.y)).rgb;
	vec3 e = texture(Source, uv + vec2(texel.x, 0.0)).rgb;
	vec3 w = texture(Source, uv - vec2(texel.x, 0.0)).rgb;
	vec3 blur = (n + s + e + w) * 0.25;

	vec3 sharpened = center.rgb + (center.rgb - blur);
	vec3 outRgb = mix(center.rgb, sharpened, SHARPEN);

	gl_FragColor = vec4(outRgb, center.a);
}

