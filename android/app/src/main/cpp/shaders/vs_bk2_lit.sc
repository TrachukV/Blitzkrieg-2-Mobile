$input a_position, a_normal, a_color0, a_texcoord0
$output v_texcoord0, v_color0

/*
 * Blitzkrieg 2 directional per-vertex lighting.
 *
 * u_legacyLight stores the original SAmbientLight values:
 *   0: light direction
 *   1: ambient colour
 *   2: colour of a plane facing the light
 *   3: colour of a plane facing away from the light
 *   4: ambient colour at the negative-side horizon
 */

#include <bgfx_shader.sh>

uniform vec4 u_legacyLight[5];

vec3 legacyLighting(vec3 model_normal)
{
    vec3 world_normal = normalize(
            mul(u_model[0], vec4(model_normal, 0.0)).xyz);
    float n_dot_l = clamp(
            dot(world_normal, u_legacyLight[0].xyz),
            -1.0,
            1.0);
    vec3 ambient = u_legacyLight[1].xyz;
    return n_dot_l >= 0.0
            ? ambient +
                    n_dot_l * (u_legacyLight[2].xyz - ambient)
            : u_legacyLight[4].xyz +
                    (-n_dot_l) *
                            (u_legacyLight[3].xyz -
                             u_legacyLight[4].xyz);
}

void main()
{
    gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
    v_texcoord0 = a_texcoord0;
    v_color0 = vec4(
            a_color0.rgb * legacyLighting(a_normal),
            a_color0.a);
}
