// The sea of clouds beneath the floating islands.
shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled, fog_disabled;

uniform vec3 cloud_color : source_color = vec3(0.93, 0.95, 1.0);
uniform vec3 shadow_color : source_color = vec3(0.55, 0.62, 0.78);
uniform float scale = 0.004;
uniform float speed = 0.004;

varying vec3 wpos;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}
float fbm(vec2 p) {
	float s = 0.0, a = 0.5;
	for (int i = 0; i < 5; i++) { s += a * noise(p); p *= 2.03; a *= 0.5; }
	return s;
}

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 p = wpos.xz * scale + vec2(TIME * speed, TIME * speed * 0.6);
	float d = fbm(p) * 0.65 + fbm(p * 3.1 - TIME * speed * 2.0) * 0.35;
	float dist = length(wpos.xz - CAMERA_POSITION_WORLD.xz);
	float fade = 1.0 - smoothstep(1200.0, 2600.0, dist);
	vec3 c = mix(shadow_color, cloud_color, smoothstep(0.35, 0.75, d));
	ALBEDO = c;
	ALPHA = smoothstep(0.28, 0.55, d) * fade * 0.97;
}
