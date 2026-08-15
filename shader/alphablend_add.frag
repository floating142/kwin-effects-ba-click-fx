// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ring 使用的 AlphaBlend_Add 片元着色器，与 BaTouchAlphaBlendAdd.shader 对齐。
// FX_TEX_Circle_01.png 的透明度恒为 1，圆形边缘衰减保存在红色通道，因此
// _RGBRGBA=0 时从红色通道读取强度。_Color 和 _Intensity 已预乘到 v_color.rgb。
//
// 渲染器为该图层设置 One/OneMinusSrcAlpha，RGB 和透明度使用相同混合因子。
#version 140

uniform sampler2D u_texture;

in vec2 v_texcoord;
in vec4 v_color;
out vec4 out_color;

void main()
{
    vec4 tex = texture(u_texture, v_texcoord);
    // RGB 已乘以 m，符合预乘输入约定。
    float m = tex.r;
    vec3 rgb = tex.rgb * v_color.rgb * m;
    float a = clamp(m * v_color.a, 0.0, 1.0);
    out_color = vec4(rgb, a);
}
