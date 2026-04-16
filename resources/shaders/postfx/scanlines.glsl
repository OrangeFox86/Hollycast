#pragma parameter SCANLINE_STRENGTH "Scanline Strength" 0.30 0.00 1.00 0.01
#pragma parameter SCANLINE_THICKNESS "Scanline Thickness" 1.00 0.50 3.00 0.01

void main()
{
	vec2 uv = vTexCoord;
	vec4 c = texture(Source, uv);

	float y = uv.y * OutputSize.y * 3.14159265 * SCANLINE_THICKNESS;
	float line = 0.5 - 0.5 * cos(y);
	float mask = 1.0 - SCANLINE_STRENGTH * line;

	c.rgb *= mask;
	gl_FragColor = c;
}

