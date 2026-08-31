$input v_uv, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_texture, 0);

void main()
{
	float a = texture2D(s_texture, v_uv).a;
	gl_FragColor = vec4(v_color0 * a, 1.0);
}
