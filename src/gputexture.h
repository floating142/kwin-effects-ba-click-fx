// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <opengl/gltexture.h>

namespace baclickfx
{

/// 分配实际存储格式与 internalFormat 一致的单级纹理。
std::unique_ptr<KWin::GLTexture> allocateTexture(GLenum internalFormat, const QSize &size);

/// 上传 Unity 的直通 sRGB 贴图；RGB 由采样器解码，Alpha 保持独立。
std::unique_ptr<KWin::GLTexture> uploadSrgbTexture(const QImage &image);

}
