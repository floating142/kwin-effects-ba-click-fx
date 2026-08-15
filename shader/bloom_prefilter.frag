// SPDX-License-Identifier: GPL-3.0-or-later
//
// Hidden/MXFinalBloom 的预筛选阶段。使用四点盒式滤波降到半分辨率，依次执行半精度
// 上限钳制、材质 Clamp 和 PPv2 QuadraticThreshold。SoftKnee 为 0 时近似硬阈值，
// 但仍保留完整公式。
#version 140

uniform sampler2D u_texture;
uniform vec2 u_texelSize; // HDR 源纹理的纹理像素尺寸。
// x=threshold，y=threshold-knee，z=2*knee，w=0.25/knee。
uniform vec4 u_filter;
uniform float u_clampMax;

in vec2 v_texcoord;
out vec4 out_color;

void main()
{
    vec2 d = u_texelSize;
    vec3 c = texture(u_texture, v_texcoord + vec2(-d.x, -d.y)).rgb * 0.25;
    c += texture(u_texture, v_texcoord + vec2( d.x, -d.y)).rgb * 0.25;
    c += texture(u_texture, v_texcoord + vec2(-d.x,  d.y)).rgb * 0.25;
    c += texture(u_texture, v_texcoord + vec2( d.x,  d.y)).rgb * 0.25;

    // 钳位必须发生在阈值贡献计算之前，与 PPv2 的运算顺序一致。
    c = min(c, vec3(65504.0));
    c = min(c, vec3(u_clampMax));

    // 使用 RGB 最大分量，使高亮单色像素同样产生辉光。
    float br = max(c.r, max(c.g, c.b));
    float soft = br - u_filter.y;
    soft = clamp(soft, 0.0, u_filter.z);
    soft = soft * soft * u_filter.w;
    float contrib = max(soft, br - u_filter.x) / max(br, 1e-4);

    out_color = vec4(c * contrib, 1.0);
}
