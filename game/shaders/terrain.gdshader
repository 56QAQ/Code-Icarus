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
	ALBEDO = s.albedo * s.edge;
	ROUGHNESS = mix(0.92, 0.28, s.gloss);
	SPECULAR = mix(0.3, 0.65, s.gloss);
	METALLIC = s.gloss * 0.35;
	float pulse = 0.85 + 0.15 * sin(TIME * 1.7 + wpos.x * 0.7 + wpos.z * 0.5);
	EMISSION = s.albedo * s.glow * 2.2 * pulse;
	float em = COLOR.a;
	if (em > 0.01) {
		float flick = 0.7 + 0.3 * sin(TIME * 11.0 + wpos.x * 3.1 + wpos.z * 2.3 + wpos.y);
		EMISSION += s.albedo * em * 3.0 * flick;
	}
}
