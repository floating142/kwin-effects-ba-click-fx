// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ring3、Ring4 和拖尾共用的加法片元着色器，与 BaTouchAdditive.shader 对齐。
// _Color 已预乘到 v_color.rgb，透明度同时调制 RGB；输出透明度固定为 1。
// 渲染器为该图层设置 One/One，最终合成不将粒子透明度作为桌面覆盖度。
#version 140

uniform sampler2D u_texture;

in vec2 v_texcoord;
in vec4 v_color;
out vec4 out_color;

void main()
{
    vec4 tex = texture(u_texture, v_texcoord);
    vec3 rgb = tex.rgb * v_color.rgb * v_color.a * tex.a;
    out_color = vec4(rgb, 1.0);
}
