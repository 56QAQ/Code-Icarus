// Leaves, glass, crops and bushes: textured like terrain, double sided, lit through
// from behind, with a gentle wind sway (plants only).
shader_type spatial;
render_mode cull_disabled, diffuse_burley;

#include "res://shaders/voxel_common.gdshaderinc"

uniform float sway = 0.035;

varying vec3 wpos;
varying vec3 lpos;
varying vec3 lnrm;

void vertex() {
	lpos = VERTEX;
	lnrm = NORMAL;
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// Wind as a smooth field over world space, so neighbouring leaf cubes move together
	// (no cracks); glass does not sway.
	float k = max(0.6, fract(VERTEX.y));
	int mat = int(round(UV.x));
	if (mat >= 0) {
		int flags = int(round(texelFetch(layer_map, ivec2(mat, 0), 0).a * 255.0));
		if ((flags & FLAG_SHINY) != 0) {
			k = 0.0;
		}
	}
	float w = sin(TIME * 1.7 + wpos.x * 0.35 + wpos.z * 0.27) * sway
			+ sin(TIME * 4.3 + wpos.x * 1.3 + wpos.y * 0.7) * sway * 0.25;
	VERTEX.x += w * k;
	VERTEX.z += w * 0.6 * k;
}

void fragment() {
	vec2 uvc = face_coords(lpos, lnrm);
	vec2 gx = dFdx(uvc);
	vec2 gy = dFdy(uvc);
	VoxelSurface s = voxel_surface(UV, COLOR, lpos, lnrm, wpos, gx, gy);
	ALBEDO = s.albedo * s.edge;
	ROUGHNESS = mix(0.85, 0.08, s.gloss);
	SPECULAR = mix(0.3, 0.8, s.gloss);
	BACKLIGHT = s.albedo * 0.45;
	if (COLOR.a > 0.01) {
		EMISSION = s.albedo * COLOR.a * 3.0 * (0.7 + 0.3 * sin(TIME * 10.0 + wpos.x));
	}
}
