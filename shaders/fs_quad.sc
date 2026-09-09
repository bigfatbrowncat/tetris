$input v_uv, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);

void main()
{
	float a = texture2D(s_texture, v_uv).a;
	// Straight alpha: the blend (src* a + dst*(1-a)) composites the glyph over
	// whatever is behind it. Opaque geometry (a=1) is unaffected; the
	// non-glyph part of a text quad (a=0) shows the background instead of black.
	gl_FragColor = vec4(v_color0, a);
}
