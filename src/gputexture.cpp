// SPDX-License-Identifier: GPL-3.0-or-later
#include "gputexture.h"

#include <QImage>

namespace baclickfx
{
using KWin::GLTexture;
using KWin::OutputTransform;

namespace
{

// KWin 6.8 的 GLTexture::allocate() 使用 GLES 的普通 8 位格式，即使参数是
// GL_RGBA16F 或 GL_SRGB8_ALPHA8。自行分配存储，仍由 GLTexture 管理生命周期、
// 过滤和坐标变换，避免依赖该便捷函数对格式的处理。
class SizedTexture final : public GLTexture
{
public:
    SizedTexture(GLuint texture, GLenum format, const QSize &size)
        : GLTexture(GL_TEXTURE_2D, texture, format, size, 1, true, OutputTransform{})
    {
    }
};

std::unique_ptr<GLTexture> createTexture(GLenum format, const QSize &size,
                                       const void *rgba = nullptr)
{
    if (size.isEmpty()) {
        return nullptr;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) {
        return nullptr;
    }
    GLint previousBinding = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousBinding);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, format, size.width(), size.height(), 0,
                 GL_RGBA, format == GL_RGBA16F ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, previousBinding);
    return std::make_unique<SizedTexture>(texture, format, size);
}

}

std::unique_ptr<GLTexture> allocateTexture(GLenum internalFormat, const QSize &size)
{
    return createTexture(internalFormat, size);
}

std::unique_ptr<GLTexture> uploadSrgbTexture(const QImage &image)
{
    if (image.isNull()) {
        return nullptr;
    }
    // 保留旧 GLTexture::update() 格式回退所用的预乘 Alpha，但显式使用 RGBA：
    // GLES 上的 BGRA 上传不适用于这里的 sized sRGB 存储。
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    GLint alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rgba.bytesPerLine() / 4);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    auto texture = createTexture(GL_SRGB8_ALPHA8, rgba.size(), rgba.constBits());
    glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
    return texture;
}

}
