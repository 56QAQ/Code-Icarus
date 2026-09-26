// Shared by the terrain and foliage shaders: cube faces are textured from the material
// texture array (see scripts/texture_forge.gd). UV.x = material id (-1: plain vertex
// colour), UV.y = face kind (0 top, 1 side, 2 bottom) + 3 * damage.

uniform sampler2DArray atlas : source_color, filter_nearest_mipmap_anisotropic, repeat_enable;
uniform sampler2D layer_map : filter_nearest, repeat_disable;
uniform int crack_base = 0;

const int FLAG_TINTED = 1;
const int FLAG_NATURAL = 2;
const int FLAG_GLOW = 4;
const int FLAG_SHINY = 8;

vec3 srgb_to_linear(vec3 c) {
	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

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

// Meadows and rock drift between lush and dry over tens of cubes, so patterns never
// tile and the land reads as a landscape rather than a carpet.
vec3 natural_tint(vec3 wpos) {
	float n1 = vnoise(wpos.xz * 0.035);
	float n2 = vnoise(wpos.xz * 0.11 + 7.3);
	float n3 = vnoise(wpos.xz * 0.4 - 3.1);
	float m = clamp(n1 * 0.62 + n2 * 0.28 + n3 * 0.1, 0.0, 1.0);
	vec3 lush = vec3(0.74, 0.9, 0.86);
	vec3 dry = vec3(1.2, 1.06, 0.66);
	return mix(lush, dry, smoothstep(0.2, 0.85, m)) * mix(0.9, 1.04, n3);
}

// Continuous face coordinates (for gradients) from a model-space position and normal;
// v runs downwards on the sides so textures stand upright.
vec2 face_coords(vec3 p, vec3 n) {
	vec3 an = abs(n);
	if (an.y > 0.5) {
		return p.xz;
	} else if (an.x > 0.5) {
		return vec2(p.z * sign(n.x), -p.y);
	}
	return vec2(-p.x * sign(n.z), -p.y);
}

struct VoxelSurface {
	vec3 albedo;
	float gloss;
	float glow;
	float edge;
};

VoxelSurface voxel_surface(vec2 uv_in, vec4 color, vec3 lpos, vec3 lnrm, vec3 wpos, vec2 gx, vec2 gy) {
	VoxelSurface s;
	s.gloss = 0.0;
	s.glow = 0.0;
	s.edge = 1.0;
	int mat = int(round(uv_in.x));
	if (mat < 0) {
		s.albedo = srgb_to_linear(color.rgb);
		return s;
	}
	vec2 uvc = face_coords(lpos, lnrm);
	vec2 uv = clamp(fract(uvc), vec2(0.001), vec2(0.999));
	int code = int(round(uv_in.y));
	int face = code % 3;
	int dmg = code / 3;
	ivec4 li = ivec4(round(texelFetch(layer_map, ivec2(mat, 0), 0) * 255.0));
	int layer = face == 0 ? li.r : (face == 1 ? li.g : li.b);
	vec4 t = textureGrad(atlas, vec3(uv, float(layer)), gx, gy);
	int flags = li.a;
	vec3 c = t.rgb;
	if ((flags & FLAG_TINTED) != 0) {
		c *= srgb_to_linear(color.rgb) * 1.6;
	} else {
		c *= color.rgb;
	}
	if ((flags & FLAG_NATURAL) != 0) {
		// Meadows and rock drift in colour over tens of cubes, so the pattern never tiles.
		c *= natural_tint(wpos);
	}
	if ((flags & FLAG_SHINY) != 0) {
		s.gloss = 1.0 - t.a;
	}
	if ((flags & FLAG_GLOW) != 0) {
		s.glow = smoothstep(0.45, 0.85, dot(t.rgb, vec3(0.3, 0.59, 0.11)));
	}
	if (dmg > 0) {
		int stage = min(3, (dmg - 1) / 2);
		float crack = textureGrad(atlas, vec3(uv, float(crack_base + stage)), gx, gy).a;
		c *= 1.0 - crack * 0.72;
	}
	// A soft bevel on each cube face keeps the voxel grid readable.
	vec2 e = min(uv, 1.0 - uv);
	s.edge = mix(0.93, 1.0, smoothstep(0.0, 0.06, min(e.x, e.y)));
	s.albedo = c;
	return s;
}
