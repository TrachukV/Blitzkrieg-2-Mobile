$input a_position, a_color0, a_texcoord0, a_indices, a_weight
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

void main()
{
    vec4 source = vec4(a_position, 1.0);
    vec4 skinned =
            mul(u_bones[int(a_indices.x)], source) * a_weight.x +
            mul(u_bones[int(a_indices.y)], source) * a_weight.y +
            mul(u_bones[int(a_indices.z)], source) * a_weight.z +
            mul(u_bones[int(a_indices.w)], source) * a_weight.w;
    float total_weight =
            a_weight.x + a_weight.y + a_weight.z + a_weight.w;
    vec4 model_position = total_weight > 0.00000001
            ? skinned / total_weight
            : source;
    gl_Position = mul(u_modelViewProj, model_position);
    v_texcoord0 = a_texcoord0;
    v_color0 = a_color0;
}
