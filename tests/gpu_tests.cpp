// SPDX-License-Identifier: GPL-3.0-or-later
#include "gpurenderer.h"
#include "gputexture.h"

#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <opengl/eglcontext.h>
#include <opengl/egldisplay.h>
#include <opengl/glshadermanager.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace KWin;

class GpuTests final : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void storageRetainsHdrValues();
    void srgbUploadDecodesAndPreservesState();
    void darkBackgroundRoundTrips_data();
    void darkBackgroundRoundTrips();
    void brightSourceProducesBloom();

private:
    QTemporaryDir m_data;
    std::unique_ptr<EglDisplay> m_display;
    std::shared_ptr<EglContext> m_context;
};

void GpuTests::initTestCase()
{
    if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        QSKIP("需要 Mesa surfaceless EGL；无需启动或修改桌面会话");
    }
    m_display = EglDisplay::create(
        eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr), nullptr);
    if (!m_display) {
        QSKIP("无法创建离屏 EGL display");
    }
    m_context = EglContext::create(m_display.get(), EGL_NO_CONFIG_KHR, nullptr);
    if (!m_context) {
        QSKIP("无法创建 KWin 离屏 GLES 上下文");
    }
    qInfo() << "GPU regression context:" << reinterpret_cast<const char *>(glGetString(GL_VERSION));

    // 只从当前源码复制资源，避免测试误用用户安装的旧 Shader 或贴图。
    QVERIFY(m_data.isValid());
    qputenv("XDG_DATA_HOME", m_data.path().toUtf8());
    for (const QString &dir : {QStringLiteral("assets"), QStringLiteral("shader")}) {
        const QString destination = m_data.path() + QStringLiteral("/kwin/effects/ba-click-fx/") + dir;
        QVERIFY(QDir().mkpath(destination));
        const QDir source(QStringLiteral(BA_CLICK_FX_SOURCE_DIR) + QLatin1Char('/') + dir);
        for (const QString &file : source.entryList(QDir::Files)) {
            QVERIFY(QFile::copy(source.filePath(file), destination + QLatin1Char('/') + file));
        }
    }
}

void GpuTests::cleanupTestCase()
{
    m_context.reset();
    m_display.reset();
}

static std::array<float, 4> readPixel(int x, int y)
{
    std::array<float, 4> pixel{};
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, pixel.data());
    return pixel;
}

void GpuTests::storageRetainsHdrValues()
{
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, QSize(8, 8));
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer::pushFramebuffer(&fbo);
    GLint componentType = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &componentType);
    const GLfloat color[] = {4.0f, 2.0f, 0.0001f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, color);
    const auto pixel = readPixel(0, 0);
    GLFramebuffer::popFramebuffer();
    QCOMPARE(componentType, GLint(GL_FLOAT));
    QCOMPARE(pixel[0], 4.0f);
    QCOMPARE(pixel[1], 2.0f);
    QVERIFY(std::abs(pixel[2] - color[2]) < 1e-7f);
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::srgbUploadDecodesAndPreservesState()
{
    auto output = baclickfx::allocateTexture(GL_RGBA16F, QSize(8, 8));
    output->bind();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 16);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 3);
    QImage image(3, 2, QImage::Format_RGBA8888);
    image.fill(QColor(255, 255, 255, 128));
    auto texture = baclickfx::uploadSrgbTexture(image);
    QVERIFY(texture);
    GLint binding = 0, alignment = 0, rowLength = 0, skipPixels = 0, skipRows = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    QCOMPARE(GLuint(binding), output->texture());
    QCOMPARE(alignment, 8);
    QCOMPARE(rowLength, 16);
    QCOMPARE(skipPixels, 2);
    QCOMPARE(skipRows, 3);

    GLFramebuffer fbo(output.get());
    QVERIFY(fbo.valid());
    GLFramebuffer::pushFramebuffer(&fbo);
    auto shader = ShaderManager::instance()->loadShaderFromCode(R"(
        void main() {
            vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
        }
    )", R"(
        uniform sampler2D source;
        out vec4 fragColor;
        void main() { fragColor = texture(source, vec2(0.5)); }
    )");
    QVERIFY(shader);
    ShaderManager::instance()->pushShader(shader.get());
    shader->setUniform("source", 0);
    glActiveTexture(GL_TEXTURE0);
    texture->bind();
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    const auto pixel = readPixel(4, 4);
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    ShaderManager::instance()->popShader();
    GLFramebuffer::popFramebuffer();
    // 与旧桌面 GL 路径相同：先预乘 Alpha，再由 sRGB 采样器解码 RGB。
    const float expected = float(baclickfx::srgbToLinearChannel(128.0 / 255.0));
    QVERIFY(std::abs(pixel[0] - expected) < 0.001f);
    QVERIFY(std::abs(pixel[3] - 128.0f / 255.0f) < 0.001f);
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::darkBackgroundRoundTrips_data()
{
    QTest::addColumn<bool>("direct");
    QTest::addColumn<double>("scale");
    QTest::newRow("direct") << true << 1.0;
    QTest::newRow("direct-fractional") << true << 1.6;
    QTest::newRow("blit") << false << 1.0;
    QTest::newRow("blit-fractional") << false << 1.6;
}

void GpuTests::darkBackgroundRoundTrips()
{
    QFETCH(bool, direct);
    QFETCH(double, scale);
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer wrapper(fbo.handle(), size); // 无可采样附件，覆盖 blit 回退路径。
    const auto cd = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    RenderTarget target(direct ? &fbo : &wrapper, cd);
    RenderViewport viewport(RectF(200, 100, size.width() / scale, size.height() / scale), scale, target, QPoint());
    GLFramebuffer::pushFramebuffer(target.framebuffer());
    const GLfloat color[] = {0.04f, 0.07f, 0.09f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, color);
    const auto before = readPixel(64, 48);
    {
        GPURenderer renderer;
        QVERIFY(renderer.initialize());
        QVERIFY(renderer.prepareTargets(viewport));
        const Region region(220, 120, 20, 20);
        QVERIFY(renderer.beginFrame(target, viewport, region));
        renderer.endFrame(target, viewport, region, region);
    }
    // 无粒子时，暗背景穿过导入、Bloom、合成后必须不变，不能随重绘区域出现黑块。
    const auto inside = readPixel(int(30 * scale), size.height() - int(30 * scale));
    const auto outside = readPixel(0, 0);
    GLFramebuffer::popFramebuffer();
    for (int i = 0; i < 3; ++i) {
        QVERIFY2(std::abs(inside[i] - before[i]) < 0.0002f, qPrintable(
            QStringLiteral("channel %1: before=%2 after=%3").arg(i).arg(before[i]).arg(inside[i])));
        QCOMPARE(outside[i], before[i]);
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::brightSourceProducesBloom()
{
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    const auto cd = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    RenderTarget target(&fbo, cd);
    RenderViewport viewport(RectF(0, 0, 128, 96), 1.0, target, QPoint());
    GLFramebuffer::pushFramebuffer(&fbo);
    const GLfloat black[] = {0, 0, 0, 1};
    glClearBufferfv(GL_COLOR, 0, black);
    {
        GPURenderer renderer;
        QVERIFY(renderer.initialize());
        QVERIFY(renderer.prepareTargets(viewport));
        const Region output(0, 0, 128, 96);
        QVERIFY(renderer.beginFrame(target, viewport, output));
        // 确定的 HDR 亮源替代随机粒子，验证能量穿过真实的 Bloom 金字塔。
        glEnable(GL_SCISSOR_TEST);
        glScissor(56, 40, 16, 16);
        const GLfloat bright[] = {4, 2, 1, 1};
        glClearBufferfv(GL_COLOR, 0, bright);
        glDisable(GL_SCISSOR_TEST);
        renderer.endFrame(target, viewport, output, Region(56, 40, 16, 16));
    }
    const auto halo = readPixel(54, 48); // 亮源几何以外，只能由辉光照亮。
    GLFramebuffer::popFramebuffer();
    QVERIFY2(halo[0] > 0.01f, "HDR 能量被截断，亮源外没有辉光");
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

QTEST_GUILESS_MAIN(GpuTests)
#include "gpu_tests.moc"
