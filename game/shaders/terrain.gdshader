// Terrain and debris cubes: textured per material face (voxel_common); the vertex
// colour carries ambient occlusion and per-cube variation, alpha = emission (burning).
shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

#include "res://shaders/voxel_common.gdshaderinc"

varying vec3 wpos;
varying vec3 lpos;
varying vec3 lnrm;

void vertex() {
	lpos = VERTEX;
	lnrm = NORMAL;
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
	vec2 uvc = face_coords(lpos, lnrm);
	vec2 gx = dFdx(uvc);
	vec2 gy = dFdy(uvc);
	VoxelSurface s = voxel_surface(UV, COLOR, lpos, lnrm, wpos, gx, gy);
	ALBEDO = safe_color(s.albedo * s.edge);
	ROUGHNESS = mix(0.92, 0.28, s.gloss);
	SPECULAR = mix(0.3, 0.65, s.gloss);
	// Rain: puddles gather in hollows of flat ground, dark and mirror-smooth, rippling.
	if (wetness > 0.02 && lnrm.y > 0.5) {
		vec2 cell = floor(wpos.xz * 0.45);
		vec2 f = fract(wpos.xz * 0.45);
		vec2 u = f * f * (3.0 - 2.0 * f);
		float n = mix(mix(hash12(cell), hash12(cell + vec2(1.0, 0.0)), u.x),
				mix(hash12(cell + vec2(0.0, 1.0)), hash12(cell + vec2(1.0, 1.0)), u.x), u.y);
		float puddle = smoothstep(0.7, 0.76, n) * wetness;
		if (puddle > 0.01) {
			ALBEDO = mix(ALBEDO, ALBEDO * 0.45, puddle);
			ROUGHNESS = mix(ROUGHNESS, 0.04, puddle);
			SPECULAR = mix(SPECULAR, 0.9, puddle);
			vec2 g = rain_ripples(wpos.xz, TIME) * puddle;
			vec3 nw = normalize(vec3(-g.x, 1.0, -g.y));
			NORMAL = normalize(mix(NORMAL, (VIEW_MATRIX * vec4(nw, 0.0)).xyz, puddle));
		}
	}
	METALLIC = s.gloss * 0.35;
	float pulse = 0.85 + 0.15 * sin(TIME * 1.7 + wpos.x * 0.7 + wpos.z * 0.5);
	EMISSION = s.albedo * s.glow * 2.2 * pulse + s.light;
	float em = COLOR.a;
	if (em > 0.01) {
		float flick = 0.7 + 0.3 * sin(TIME * 11.0 + wpos.x * 3.1 + wpos.z * 2.3 + wpos.y);
		EMISSION += s.albedo * em * 3.0 * flick;
	}
}
