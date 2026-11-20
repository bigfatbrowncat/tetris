$input v_uv, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);

void main()
{
	vec4 t = texture2D(s_texture, v_uv);
	// The texture supplies the color (the field image, or the white strip /
	// glyph ink of the font atlas) and the straight alpha (glyph coverage).
	// The blend (src* a + dst*(1-a)) composites over whatever is behind:
	// opaque geometry (a=1) is unaffected; the non-glyph part of a text
	// quad (a=0) shows the background instead of black.
	gl_FragColor = vec4(v_color0 * t.rgb, t.a);
}
