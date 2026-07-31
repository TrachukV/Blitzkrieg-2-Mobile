$input a_position, a_normal, a_color0, a_texcoord0, a_indices, a_weight
$output v_texcoord0, v_color0

/*
 * Runtime Granny skinning for Android.
 *
 * BK2MSH1 v5 stores bind-pose vertices plus four normalized influences and
 * sampled column-major skin matrices. The 48-bone palette covers the shipped
 * infantry skeletons; larger or older meshes remain on the CPU path.
 */

#include <bgfx_shader.sh>

uniform mat4 u_bones[48];
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
    vec4 source = vec4(a_position, 1.0);
    vec4 source_normal = vec4(a_normal, 0.0);
    vec4 skinned =
            mul(u_bones[int(a_indices.x)], source) * a_weight.x +
            mul(u_bones[int(a_indices.y)], source) * a_weight.y +
            mul(u_bones[int(a_indices.z)], source) * a_weight.z +
            mul(u_bones[int(a_indices.w)], source) * a_weight.w;
    vec4 skinned_normal =
            mul(u_bones[int(a_indices.x)], source_normal) * a_weight.x +
            mul(u_bones[int(a_indices.y)], source_normal) * a_weight.y +
            mul(u_bones[int(a_indices.z)], source_normal) * a_weight.z +
            mul(u_bones[int(a_indices.w)], source_normal) * a_weight.w;
    float total_weight =
            a_weight.x + a_weight.y + a_weight.z + a_weight.w;
    vec4 model_position = total_weight > 0.00000001
            ? skinned / total_weight
            : source;
    vec3 model_normal = total_weight > 0.00000001
            ? normalize((skinned_normal / total_weight).xyz)
            : a_normal;
    gl_Position = mul(u_modelViewProj, model_position);
    v_texcoord0 = a_texcoord0;
    v_color0 = vec4(
            a_color0.rgb * legacyLighting(model_normal),
            a_color0.a);
}
