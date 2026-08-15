// SPDX-License-Identifier: GPL-3.0-or-later
//
// MeshTri 使用的 Dissolve 片元着色器，与
// Shader_GraphsFX_SHADER_Dissolve_GachaGauge_P.gles3 对齐。
// FX_TEX_Grad_Ring3.png 的透明度沿 u 呈钟形，同时用于 SrcAlpha 源因子和丢弃判据。
//
// 渲染器为 RGB 设置 SrcAlpha/One，为透明度设置 One/One。
#version 140

uniform sampler2D u_texture;

in vec2 v_texcoord;
in vec4 v_color;
in float v_custom0;
out vec4 out_color;

void main()
{
    vec4 tex = texture(u_texture, v_texcoord);
    float a = tex.a;

    // custom0 驱动 1 → 0 → 1 的溶解范围；钟形透明度使弧段从两端同时被裁剪。
    if (v_color.a * a - v_custom0 < 0.0) {
        discard;
    }

    // 保留完整纹理颜色公式；HDR 增益已预乘到 v_color.rgb。
    vec3 rgb = tex.rgb * v_color.rgb;
    out_color = vec4(rgb, a * v_color.a);
}
