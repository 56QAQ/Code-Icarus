// Character voxel bodies: vertex colour per body voxel, a faint bevel and variation
// per voxel (cloth and skin, not tiles), underside shadow and a rim of light so figures
// read against the ground.
shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

uniform float voxel = 0.125;
uniform vec4 tint : source_color = vec4(1.0);
uniform float rim_strength = 0.35;

varying vec3 lpos;
varying vec3 lnrm;

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

void vertex() {
	lpos = VERTEX / voxel;
	lnrm = NORMAL;
}

void fragment() {
	vec3 c = srgb_to_linear(COLOR.rgb) * tint.rgb;
	vec3 an = abs(lnrm);
	vec2 f = an.y > 0.5 ? lpos.xz : (an.x > 0.5 ? lpos.zy : lpos.xy);
	vec2 uv = fract(f + 1e-3);
	vec2 e = min(uv, 1.0 - uv);
	c *= mix(0.95, 1.0, smoothstep(0.0, 0.07, min(e.x, e.y)));
	vec3 cell = floor(lpos - lnrm * 0.5 + 1e-3);
	c *= 0.97 + 0.05 * hash13(cell);
	// Faces pointing down are in their own shadow.
	c *= mix(1.0, 0.72, step(0.5, -lnrm.y));
	ALBEDO = c;
	ROUGHNESS = 0.78;
	SPECULAR = 0.3;
	RIM = rim_strength;
	RIM_TINT = 0.6;
}
