$input v_texcoord0, v_color0

/*
 * Blitzkrieg 2 Android material compatibility shader.
 *
 * The desktop AM_ALPHA_TEST material mode keeps depth writes enabled and
 * rejects texels below the material alpha reference. This shader uses bgfx's
 * predefined u_alphaRef uniform, populated from BGFX_STATE_ALPHA_REF.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    vec4 color = texture2D(s_texColor, v_texcoord0) * v_color0;
    if (color.a < u_alphaRef)
    {
        discard;
    }
    gl_FragColor = color;
}
