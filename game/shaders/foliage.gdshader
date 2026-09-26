// Leaves, crops and bushes: like terrain, double sided, with a gentle wind sway.
shader_type spatial;
render_mode cull_disabled, diffuse_burley;

uniform float sway = 0.035;

varying vec3 wpos;

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float w = sin(TIME * 1.7 + wpos.x * 0.35 + wpos.z * 0.27) * sway;
	VERTEX.x += w * fract(VERTEX.y) ;
	VERTEX.z += w * 0.6 * fract(VERTEX.y);
}

void fragment() {
	vec3 c = srgb_to_linear(COLOR.rgb);
	ALBEDO = c;
	ROUGHNESS = 0.85;
	BACKLIGHT = c * 0.35;
	if (COLOR.a > 0.01) {
		EMISSION = c * COLOR.a * 3.0 * (0.7 + 0.3 * sin(TIME * 10.0 + wpos.x));
	}
}
