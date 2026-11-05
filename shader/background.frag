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
    vec3 linearOutput = tfDecode(texture(u_texture, v_texcoord).rgb);
    // 粒子贴图及顶点颜色采用游戏的线性 BT.709；背景必须进入同一空间再混合。
    // 不裁剪负分量，保留广色域背景往返转换所需的信息。
    out_color = vec4((u_colorMatrix * vec4(linearOutput, 0.0)).rgb, 1.0);
}
