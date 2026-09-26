// Terrain cubes: vertex colour carries material colour * baked AO; alpha = emission.
shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

uniform float edge_darken = 0.10;
uniform float grain = 0.035;

varying vec3 wpos;
varying vec3 wnrm;

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

float hash3(vec3 p) {
	p = fract(p * 0.3183099 + 0.1);
	p *= 17.0;
	return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	wnrm = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}

void fragment() {
	vec3 c = srgb_to_linear(COLOR.rgb);
	// Soft bevel: darken near cube edges on each face for a crisp voxel read.
	vec3 f = fract(wpos + 1e-4);
	vec3 an = abs(wnrm);
	vec2 uv = an.y > 0.5 ? f.xz : (an.x > 0.5 ? f.zy : f.xy);
	vec2 e = min(uv, 1.0 - uv);
	float edge = smoothstep(0.0, 0.07, min(e.x, e.y));
	c *= mix(1.0 - edge_darken, 1.0, edge);
	// Sub-cube grain so large flat areas are not plastic-looking.
	vec3 cell = floor(wpos * 4.0 + wnrm * 0.01);
	c *= 1.0 - grain + grain * 2.0 * hash3(cell);
	ALBEDO = c;
	ROUGHNESS = 0.9;
	SPECULAR = 0.25;
	float em = COLOR.a;
	if (em > 0.01) {
		float flick = 0.7 + 0.3 * sin(TIME * 11.0 + wpos.x * 3.1 + wpos.z * 2.3 + wpos.y);
		EMISSION = c * em * 3.0 * flick;
	}
}
