#pragma parameter SATURATION "Saturation" 1.10 0.00 2.00 0.01
#pragma parameter CONTRAST "Contrast" 1.05 0.50 1.50 0.01

vec3 applyContrast(vec3 c, float contrast)
{
	return (c - 0.5) * contrast + 0.5;
}

vec3 applySaturation(vec3 c, float saturation)
{
	float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
	return mix(vec3(luma), c, saturation);
}

void main()
{
	vec4 c = texture(Source, vTexCoord);
	vec3 rgb = c.rgb;
	rgb = applyContrast(rgb, CONTRAST);
	rgb = applySaturation(rgb, SATURATION);
	gl_FragColor = vec4(clamp(rgb, 0.0, 1.0), c.a);
}
