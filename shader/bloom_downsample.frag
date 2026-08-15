// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden/MXFinalBloom 的逐级降采样阶段。PPv2 fastMode 使用四个对角样本组成
// DownsampleBox4Tap，每个样本权重为 0.25。
#version 140

uniform sampler2D u_texture;
uniform vec2 u_texelSize; // 源纹理的纹理像素尺寸。

in vec2 v_texcoord;
out vec4 out_color;

void main()
{
    vec2 t = u_texelSize;
    vec2 uv = v_texcoord;

    vec3 a = texture(u_texture, uv + t * vec2(-1.0, -1.0)).rgb;
    vec3 b = texture(u_texture, uv + t * vec2( 1.0, -1.0)).rgb;
    vec3 c = texture(u_texture, uv + t * vec2(-1.0,  1.0)).rgb;
    vec3 d = texture(u_texture, uv + t * vec2( 1.0,  1.0)).rgb;

    out_color = vec4((a + b + c + d) * 0.25, 1.0);
}
