// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden/MXFinalBloom 的最终合成阶段。输入场景纹理已经包含桌面和全部粒子图层；
// 第 0 级辉光通过最后一次 UpsampleBox 恢复到全分辨率，再按
// main + bloom * intensity * color 在线性空间合成，最后编码回 KWin 输出。
#version 140

uniform sampler2D u_hdrTexture;
uniform sampler2D u_bloomTexture;
uniform vec2 u_bloomTexelSize;
uniform float u_sampleScale;
uniform float u_bloomIntensity;
uniform vec3 u_bloomColor;

#include "transfer.glsl"

in vec2 v_texcoord;
out vec4 out_color;

void main()
{
    vec3 color = texture(u_hdrTexture, v_texcoord).rgb;

    // 第 0 级为半分辨率，不能以单次双线性采样代替 PPv2 的四点 UpsampleBox。
    vec2 d = u_bloomTexelSize * (u_sampleScale * 0.5);
    vec3 bloom = texture(u_bloomTexture, v_texcoord + vec2(-d.x, -d.y)).rgb * 0.25;
    bloom += texture(u_bloomTexture, v_texcoord + vec2( d.x, -d.y)).rgb * 0.25;
    bloom += texture(u_bloomTexture, v_texcoord + vec2(-d.x,  d.y)).rgb * 0.25;
    bloom += texture(u_bloomTexture, v_texcoord + vec2( d.x,  d.y)).rgb * 0.25;

    // 辉光颜色和强度仅作用于辉光项，不修改主场景颜色。
    color += bloom * u_bloomIntensity * u_bloomColor;

    // 输出目标为不透明桌面；粒子累计透明度不具备覆盖度语义。
    out_color = vec4(tfEncode(color), 1.0);
}
