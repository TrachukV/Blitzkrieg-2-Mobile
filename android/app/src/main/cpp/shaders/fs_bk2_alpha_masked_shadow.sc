$input v_texcoord0, v_color0

/*
 * Textured projected-shadow shader for alpha-tested map geometry.
 *
 * The source texture supplies only the silhouette. The projected vertices
 * carry the desktop-style translucent shadow color.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

void main()
{
    float texture_alpha = texture2D(s_texColor, v_texcoord0).a;
    if (texture_alpha < u_alphaRef)
    {
        discard;
    }
    gl_FragColor = vec4(v_color0.rgb, v_color0.a * texture_alpha);
}
