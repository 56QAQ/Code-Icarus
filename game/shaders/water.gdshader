// Water: coloured by how much water the eye looks through (clear at the shore, deep
// blue in the middle), with refraction of what lies beneath, foam where it meets the
// shore, rolling waves on the surface and streaks running down falling water.
// UV.y = 1 on water that keeps falling (a cascade).
shader_type spatial;
render_mode blend_mix, depth_draw_always, cull_back, diffuse_burley, specular_schlick_ggx;

uniform vec3 shallow_color : source_color = vec3(0.30, 0.66, 0.72);
uniform vec3 deep_color : source_color = vec3(0.05, 0.19, 0.38);
uniform float clarity = 2.6;  // cubes of water until it shows mostly its own colour
uniform float rain = 0.0;     // 0 dry .. 1 pouring: raindrops ring the surface
uniform sampler2D depth_tex : hint_depth_texture, filter_linear_mipmap;
uniform sampler2D screen_tex : hint_screen_texture, filter_linear_mipmap;

varying vec3 wpos;
varying float is_top;
varying float falling;

float hash12(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
			mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Slope of the wave field at p (a few travelling waves and a little noise).
// Raindrops on a still surface: rings spreading from random points, one drop per cell
// of about half a cube at a time. Returns the slope of the surface (xz) at p.
vec2 rain_ripples(vec2 p, float t) {
	vec2 g = vec2(0.0);
	for (int k = 0; k < 2; k++) {
		vec2 q = p * 2.2 + vec2(float(k) * 0.37, float(k) * 0.71);
		vec2 cell = floor(q);
		for (int j = -1; j <= 1; j++) {
			for (int i = -1; i <= 1; i++) {
				vec2 c = cell + vec2(float(i), float(j));
				float h = hash12(c + float(k) * 17.0);
				if (h > 0.55) continue;  // not every spot has a drop just now
				vec2 centre = c + vec2(hash12(c * 1.7 + 3.1), hash12(c * 2.3 + 7.7));
				float age = fract(t * 0.9 + h);
				vec2 d = q - centre;
				float dist = length(d);
				float r = age * 0.9;
				float ring = exp(-pow((dist - r) / 0.07, 2.0)) * (1.0 - age) * (1.0 - age);
				g += (dist > 0.001 ? d / dist : vec2(0.0)) * ring * sin((dist - r) * 40.0);
			}
		}
	}
	return g * 0.5;
}

vec2 wave_slope(vec2 p, float t) {
	vec2 g = vec2(0.0);
	vec2 d1 = vec2(0.8, 0.3);
	vec2 d2 = vec2(-0.4, 0.9);
	vec2 d3 = vec2(0.25, -0.97);
	g += d1 * cos(dot(p, d1) * 2.3 + t * 1.7) * 0.07;
	g += d2 * cos(dot(p, d2) * 3.3 + t * 2.2) * 0.05;
	g += d3 * cos(dot(p, d3) * 5.7 + t * 3.1) * 0.03;
	float e = 0.15;
	float n0 = vnoise(p * 2.0 + t * 0.4);
	g += vec2(vnoise(p * 2.0 + vec2(e, 0.0) + t * 0.4) - n0, vnoise(p * 2.0 + vec2(0.0, e) + t * 0.4) - n0) * 0.35;
	return g;
}

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	is_top = step(0.5, NORMAL.y);
	falling = UV.y;
	if (is_top > 0.5) {
		VERTEX.y += (sin(TIME * 1.3 + wpos.x * 0.8) + cos(TIME * 1.1 + wpos.z * 0.7)) * 0.02 - 0.04;
	}
}

void fragment() {
	// World-space normal with waves on the surface; the engine wants view space.
	vec3 nw = (INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz;
	float rings = 0.0;
	if (is_top > 0.5) {
		vec2 g = wave_slope(wpos.xz, TIME);
		if (rain > 0.01) {
			vec2 rr = rain_ripples(wpos.xz, TIME);
			g += rr * rain * 2.2;
			rings = clamp(length(rr) * 0.5, 0.0, 0.22) * rain;
		}
		nw = normalize(vec3(-g.x, 1.0, -g.y));
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(nw, 0.0)).xyz);
	// How much water lies between the surface and whatever is behind it.
	float depth = texture(depth_tex, SCREEN_UV).r;
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
	depth = depth * 2.0 - 1.0;  // OpenGL's depth range
#endif
	vec4 view = INV_PROJECTION_MATRIX * vec4(SCREEN_UV * 2.0 - 1.0, depth, 1.0);
	// Nothing behind (the sky) or a degenerate depth: treat as deep water, never divide
	// by zero (a NaN here would turn the pixel black).
	float thickness = 50.0;
	if (abs(view.w) > 1e-6) {
		view.xyz /= view.w;
		thickness = clamp(-view.z + VERTEX.z, 0.0, 50.0);
	}
	float murk = 1.0 - exp(-thickness / clarity);
	// Refraction: the bed seen through the rippling surface, tinted by the water.
	vec2 offs = nw.xz * 0.03 * clamp(thickness, 0.0, 1.0);
	vec3 below = texture(screen_tex, SCREEN_UV + offs).rgb;
	vec3 water = mix(shallow_color, deep_color, smoothstep(0.0, 1.0, murk));
	vec3 col = mix(below * shallow_color * 1.45, water, clamp(murk * 1.15 + 0.12, 0.0, 1.0));
	// Foam where the water is thin against the shore, broken up by noise.
	float foam = (1.0 - smoothstep(0.0, 0.3, thickness)) * is_top;
	foam *= smoothstep(0.35, 0.7, vnoise(wpos.xz * 3.0 + TIME * 0.6));
	// Cascades: white streaks running down.
	if (falling > 0.5) {
		float s = vnoise(vec2((wpos.x + wpos.z) * 3.0, wpos.y * 1.5 + TIME * 5.0));
		foam = max(foam, smoothstep(0.45, 0.85, s) * 0.8);
		col = mix(col, shallow_color * 1.3, 0.35);
	}
	col = mix(col, vec3(0.92, 0.96, 1.0), foam * 0.85);
	col = mix(col, col * vec3(0.8, 0.85, 0.9) + vec3(0.03), rain * 0.4);
	col = mix(col, vec3(0.85, 0.9, 0.96), rings);
	ALBEDO = col;
	ALPHA = mix(0.88, 1.0, murk);
	ROUGHNESS = mix(0.05, 0.6, foam);
	SPECULAR = 0.55;
}
