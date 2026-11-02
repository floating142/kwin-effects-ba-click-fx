// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <opengl/gltexture.h>

namespace baclickfx
{

/// 分配实际存储格式与 internalFormat 一致的单级纹理。
std::unique_ptr<KWin::GLTexture> allocateTexture(GLenum internalFormat, const QSize &size);

/// 上传 sRGB 贴图；保留旧桌面 GL 路径的预乘 Alpha，RGB 由采样器解码。
std::unique_ptr<KWin::GLTexture> uploadSrgbTexture(const QImage &image);

}
