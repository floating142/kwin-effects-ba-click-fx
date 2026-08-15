// SPDX-License-Identifier: GPL-3.0-or-later
//
// 所有粒子和全屏四边形共用的顶点着色器。输入包含输出局部坐标、纹理坐标、线性
// HDR 顶点色和 Unity CustomData0，u_projection 将坐标转换到裁剪空间。
#version 140

uniform mat4 u_projection;

// GLSL 1.40 不保证支持 layout(location=)。position 和 texcoord 由 KWin 的
// ShaderManager 在链接前绑定；color 和 custom0 由驱动分配，并在链接后查询位置。
// 不使用 custom0 的片元着色器允许链接器将该属性优化掉。
in vec2 position;
in vec2 texcoord;
in vec4 color;
in float custom0;

out vec2 v_texcoord;
out vec4 v_color;
out float v_custom0;

void main()
{
    gl_Position = u_projection * vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
    v_color = color;
    v_custom0 = custom0;
}
