// SPDX-License-Identifier: GPL-3.0-or-later
//
// 将 KWin 输出缓冲中的桌面颜色解码到线性 HDR 场景纹理，对应 Unity UIRenderPass
// 复制 cameraTarget 的阶段。Ring 和 Dissolve 的混合公式依赖真实目标颜色，因此
// 粒子必须绘制在该背景上，而不是透明黑色上。
#version 140

uniform sampler2D u_texture;

#include "transfer.glsl"

in vec2 v_texcoord;
out vec4 out_color;

void main()
{
    // 桌面和 Unity cameraTarget 均视为不透明。
    out_color = vec4(tfDecode(texture(u_texture, v_texcoord).rgb), 1.0);
}
