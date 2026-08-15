// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden/MXFinalBloom 的逐级上采样阶段。对更低分辨率一级执行 UpsampleBox 后与
// 当前级直接相加；PPv2 使用能量累加，不使用 URP 的加权混合。
#version 140

uniform sampler2D u_texture;   // 更低一级，包含更宽的模糊范围。
uniform sampler2D u_hiTexture; // 当前一级，保留更清晰的高频内容。
uniform vec2 u_texelSize;      // 更低一级的纹理像素尺寸。
uniform float u_sampleScale;   // PPv2 根据 Diffusion 计算的采样尺度。

in vec2 v_texcoord;
out vec4 out_color;

void main()
{
    vec2 d = u_texelSize * (u_sampleScale * 0.5);
    vec2 uv = v_texcoord;

    vec3 lo = texture(u_texture, uv + vec2(-d.x, -d.y)).rgb;
    lo += texture(u_texture, uv + vec2( d.x, -d.y)).rgb;
    lo += texture(u_texture, uv + vec2(-d.x,  d.y)).rgb;
    lo += texture(u_texture, uv + vec2( d.x,  d.y)).rgb;
    lo *= 0.25;

    out_color = vec4(lo + texture(u_hiTexture, uv).rgb, 1.0);
}
