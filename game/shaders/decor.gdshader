// Grass tufts and flowers: alpha-cut sprites that sway in the wind and take on the same
// large-scale colour drift as the meadow they grow from.
shader_type spatial;
render_mode cull_disabled, diffuse_burley;

#include "res://shaders/voxel_common.gdshaderinc"

uniform sampler2DArray decor : source_color, filter_nearest_mipmap, repeat_disable;

varying vec3 wpos;

void vertex() {
	wpos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	float top = 1.0 - UV.y;  // 1 at the tips, 0 at the root
	float w = sin(TIME * 2.1 + wpos.x * 0.45 + wpos.z * 0.31) * 0.06 + sin(TIME * 5.3 + wpos.x * 1.7) * 0.015;
	VERTEX.x += w * top;
	VERTEX.z += w * 0.7 * top;
}

void fragment() {
	float layer = floor(UV.x);
	vec4 t = texture(decor, vec3(fract(UV.x), UV.y, layer));
	ALPHA = t.a;
	ALPHA_SCISSOR_THRESHOLD = 0.5;
	vec3 c = t.rgb * COLOR.rgb;
	if (layer < 2.5) {
		c *= natural_tint(wpos);
	}
	// Tips catch more light than roots.
	c *= mix(0.72, 1.08, 1.0 - UV.y);
	ALBEDO = c;
	ROUGHNESS = 0.9;
	SPECULAR = 0.2;
	BACKLIGHT = c * 0.4;
}
