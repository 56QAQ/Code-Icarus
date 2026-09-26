// Water surfaces: translucent, glossy, with small animated ripples.
shader_type spatial;
render_mode blend_mix, depth_draw_always, cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec3 shallow_color : source_color = vec3(0.30, 0.62, 0.78);
uniform vec3 deep_color : source_color = vec3(0.07, 0.22, 0.42);
uniform float opacity = 0.72;

varying vec3 wpos;
varying float is_top;

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	is_top = step(0.5, NORMAL.y);
	if (is_top > 0.5) {
		VERTEX.y += (sin(TIME * 1.3 + wpos.x * 0.8) + cos(TIME * 1.1 + wpos.z * 0.7)) * 0.025;
	}
}

void fragment() {
	float ripple = sin(TIME * 2.0 + wpos.x * 2.3 + sin(wpos.z * 1.7 + TIME)) * 0.5 + 0.5;
	vec3 base = mix(deep_color, shallow_color, 0.35 + 0.2 * ripple);
	ALBEDO = base;
	ROUGHNESS = 0.06;
	SPECULAR = 0.7;
	METALLIC = 0.0;
	vec3 n = NORMAL;
	if (is_top > 0.5) {
		n.x += (sin(TIME * 2.3 + wpos.x * 3.7) * 0.05);
		n.z += (cos(TIME * 1.9 + wpos.z * 3.3) * 0.05);
		NORMAL = normalize(n);
	}
	ALPHA = opacity;
}
