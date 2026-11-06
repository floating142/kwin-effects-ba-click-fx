// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/gpurenderer.h"
#include "render/gputexture.h"
#include "render/bloomutils.h"

#include <core/backendoutput.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <opengl/eglcontext.h>
#include <opengl/egldisplay.h>
#include <opengl/glshadermanager.h>

#include <QDir>
#include <QFile>
#include <QScopeGuard>
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
    void backgroundAllocationFailureCanRetry();
    void bloomAllocationFailureCanRetry();
    void outputTargetsReleaseIndependently();
    void gpuResourcesFollowEglContext();
    void failedGpuContextIsReleasedOnReplacement();
    void brightSourceProducesBloom_data();
    void brightSourceProducesBloom();
    void movingBloomMatchesFullRepaint_data();
    void movingBloomMatchesFullRepaint();
    void hdrToSdrClearsDesktopBloomOutsideParticleSources();
    void wideGamutSdrDesktopBloomMatchesFullRepaint_data();
    void wideGamutSdrDesktopBloomMatchesFullRepaint();
    void outputColorMatchesGameWorkingSpace_data();
    void outputColorMatchesGameWorkingSpace();
    void patternedBackgroundRoundTrips_data();
    void patternedBackgroundRoundTrips();
    void particleMaterialsMatchUnityBlending_data();
    void particleMaterialsMatchUnityBlending();
    void overlappingParticlesUseAdditiveBlendEquation();

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

void GpuTests::particleMaterialsMatchUnityBlending_data()
{
    QTest::addColumn<QString>("material");
    QTest::addColumn<float>("threshold");
    QTest::newRow("Ring") << QStringLiteral("alphablend_add") << 0.f;
    QTest::newRow("Triangle") << QStringLiteral("additive") << 0.f;
    QTest::newRow("Dissolve-visible") << QStringLiteral("dissolve") << .2f;
    QTest::newRow("Dissolve-discarded") << QStringLiteral("dissolve") << .3f;
}

void GpuTests::particleMaterialsMatchUnityBlending()
{
    QFETCH(QString, material);
    QFETCH(float, threshold);
    auto output = baclickfx::allocateTexture(GL_RGBA16F, QSize(8, 8));
    GLFramebuffer fbo(output.get());
    QVERIFY(fbo.valid());
    QImage image(2, 2, QImage::Format_RGBA8888);
    image.fill(QColor(128, 64, 192, 128));
    auto texture = baclickfx::uploadSrgbTexture(image);
    texture->setFilter(GL_LINEAR);
    QFile fragment(QStringLiteral(BA_CLICK_FX_SOURCE_DIR "/shader/") + material + QStringLiteral(".frag"));
    QVERIFY(fragment.open(QIODevice::ReadOnly));
    const QByteArray fragmentSource = fragment.readAll().replace("#version 140", "");
    auto shader = ShaderManager::instance()->loadShaderFromCode(R"(
        out vec2 v_texcoord;
        out vec4 v_color;
        out float v_custom0;
        uniform float threshold;
        void main() {
            vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
            gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
            v_texcoord = vec2(.5); v_color = vec4(2, 3, 4, .5); v_custom0 = threshold;
        }
    )", fragmentSource);
    QVERIFY(shader);
    GLFramebuffer::pushFramebuffer(&fbo);
    const GLfloat background[]{.25f, .125f, .0625f, 1};
    glClearBufferfv(GL_COLOR, 0, background);
    glEnable(GL_BLEND);
    const bool ring = material == QStringLiteral("alphablend_add");
    const bool dissolve = material == QStringLiteral("dissolve");
    glBlendFuncSeparate(ring ? GL_ONE : dissolve ? GL_SRC_ALPHA : GL_ONE,
        ring ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE, GL_ONE, ring ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
    ShaderManager::instance()->pushShader(shader.get());
    shader->setUniform("u_texture", 0);
    shader->setUniform("threshold", threshold);
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
    glDisable(GL_BLEND);
    GLFramebuffer::popFramebuffer();
    const double tex[]{baclickfx::srgbToLinearChannel(128.0 / 255),
        baclickfx::srgbToLinearChannel(64.0 / 255), baclickfx::srgbToLinearChannel(192.0 / 255)};
    const double alpha = 128.0 / 255;
    for (int ch = 0; ch < 3; ++ch) {
        double expected = background[ch];
        if (ring) {
            expected = tex[ch] * (ch + 2) * tex[0] + expected * (1 - tex[0] * .5);
        } else if (!dissolve || alpha * .5 >= threshold) {
            expected += tex[ch] * (ch + 2) * .5 * alpha;
        }
        QVERIFY2(std::abs(pixel[ch] - expected) < .002,
            qPrintable(QStringLiteral("%1 channel=%2 actual=%3 expected=%4")
                .arg(material).arg(ch).arg(pixel[ch]).arg(expected)));
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
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
    // Unity RGB 不受独立 Alpha 通道影响；着色器负责应用透明度。
    const float expected = 1.0f;
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
        QVERIFY(renderer.prepareTargets(target, viewport));
        const Region region(220, 120, 20, 20);
        QVERIFY(renderer.beginFrame(target, viewport, region));
        renderer.endFrame(target, viewport, region, renderer.updateBloom(target, region));
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

void GpuTests::backgroundAllocationFailureCanRetry()
{
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer wrapper(fbo.handle(), size);
    RenderTarget target(&wrapper, ColorDescription::sRGB);
    const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
    GLFramebuffer::pushFramebuffer(&wrapper);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });

    GPURenderer renderer;
    QVERIFY(renderer.initialize());
    QVERIFY(renderer.prepareTargets(target, viewport));
    const Region region(0, 0, size.width(), size.height());

    // PBO 容量不足，纹理名称分配成功，但像素存储和 FBO 创建失败。
    GLuint unpackBuffer = 0;
    glGenBuffers(1, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 4, nullptr, GL_STREAM_DRAW);
    const bool succeeded = renderer.beginFrame(target, viewport, region);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &unpackBuffer);
    QVERIFY(!succeeded);
    QCOMPARE(glGetError(), GLenum(GL_INVALID_OPERATION));
    QVERIFY(renderer.diagnosticStatus().contains(QStringLiteral("bg=0x0")));

    QVERIFY(renderer.beginFrame(target, viewport, region));
    renderer.endFrame(target, viewport, region, renderer.updateBloom(target, region));
    QVERIFY(renderer.diagnosticStatus().contains(QStringLiteral("bg=128x96")));
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::bloomAllocationFailureCanRetry()
{
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    const auto hdr = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 320), 80, 0, 320, 320);
    RenderTarget sdrTarget(&fbo, ColorDescription::sRGB);
    RenderTarget hdrTarget(&fbo, hdr);
    const RectF rect(0, 0, size.width(), size.height());
    const RenderViewport sdrView(rect, 1.0, sdrTarget, QPoint());
    const RenderViewport hdrView(rect, 1.0, hdrTarget, QPoint());
    const Region damage(0, 0, size.width(), size.height());
    const GLfloat dark[]{.04f, .05f, .06f, 1};
    const GLfloat bright[]{.75f, .75f, .75f, 1};

    GPURenderer renderer;
    QVERIFY(renderer.initialize());
    QVERIFY(renderer.prepareTargets(sdrTarget, sdrView));
    glClearBufferfv(GL_COLOR, 0, dark);
    QVERIFY(renderer.beginFrame(sdrTarget, sdrView, damage));
    const bool sdrBloom = renderer.updateBloom(sdrTarget, {});
    renderer.endFrame(sdrTarget, sdrView, damage, sdrBloom);
    QVERIFY(!sdrBloom);
    QVERIFY(std::abs(readPixel(64, 48)[0] - dark[0]) < .001f);

    // HDR 背景需要辉光。用容量不足的 PBO 只让这次金字塔分配失败。
    glClearBufferfv(GL_COLOR, 0, bright);
    QVERIFY(renderer.beginFrame(hdrTarget, hdrView, damage));
    GLuint unpackBuffer = 0;
    glGenBuffers(1, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 4, nullptr, GL_STREAM_DRAW);
    const bool failedBloom = renderer.updateBloom(hdrTarget, {});
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(1, &unpackBuffer);
    renderer.endFrame(hdrTarget, hdrView, damage, failedBloom);
    QVERIFY(!failedBloom);
    QCOMPARE(glGetError(), GLenum(GL_INVALID_OPERATION));
    QVERIFY(std::abs(readPixel(64, 48)[0] - bright[0]) < .001f);

    const auto repaint = [&]() {
        glClearBufferfv(GL_COLOR, 0, bright);
        if (!renderer.beginFrame(hdrTarget, hdrView, damage)) {
            return false;
        }
        const bool bloom = renderer.updateBloom(hdrTarget, {});
        renderer.endFrame(hdrTarget, hdrView, damage, bloom);
        return bloom;
    };
    QVERIFY(!repaint()); // 冷却期间仍复用场景目标。
    QTRY_VERIFY_WITH_TIMEOUT(repaint(), 2000);
    QVERIFY(readPixel(64, 48)[0] > bright[0] + .01f);
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::outputTargetsReleaseIndependently()
{
    struct TestOutput : BackendOutput {
        std::expected<void, OutputError> testPresentation(const std::shared_ptr<OutputFrame> &) override
        {
            return {};
        }
        std::expected<void, OutputError> present(const QList<OutputLayer *> &,
                                                  const std::shared_ptr<OutputFrame> &) override
        {
            return {};
        }
    };
    TestOutput firstBackend;
    TestOutput secondBackend;
    const LogicalOutput first(&firstBackend);
    const LogicalOutput second(&secondBackend);
    GPURenderer renderer;
    const auto prepare = [&renderer](const LogicalOutput &output, const QSize &size) {
        GLFramebuffer framebuffer(0, size);
        RenderTarget target(&framebuffer, ColorDescription::sRGB);
        const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
        return renderer.prepareTargets(target, viewport, &output);
    };

    QVERIFY(prepare(first, QSize(128, 96)));
    QVERIFY(prepare(second, QSize(96, 64)));
    QVERIFY(prepare(first, QSize(160, 100)));
    const auto resized = renderer.diagnosticStatus();
    QVERIFY(!resized.contains(QStringLiteral("hdr=128x96")));
    QVERIFY(resized.contains(QStringLiteral("hdr=160x100")));
    QVERIFY(resized.contains(QStringLiteral("hdr=96x64")));

    QVERIFY(renderer.invalidateOutput(&second));
    QVERIFY(!renderer.invalidateOutput(&second));
    renderer.releaseInvalidatedTargets();
    const auto removed = renderer.diagnosticStatus();
    QVERIFY(!removed.contains(QStringLiteral("hdr=96x64")));
    QVERIFY(removed.contains(QStringLiteral("hdr=160x100")));
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::gpuResourcesFollowEglContext()
{
    auto second = EglContext::create(m_display.get(), EGL_NO_CONFIG_KHR, nullptr);
    QVERIFY(second);
    const auto restore = qScopeGuard([&]() {
        second.reset();
        (void)m_context->makeCurrent();
    });
    QVERIFY(!second->isCompatibleWith(m_context.get()));
    GpuRendererCache cache;
    const auto paint = [](GPURenderer &renderer, const QSize &size) {
        auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
        QVERIFY(texture);
        GLFramebuffer fbo(texture.get());
        QVERIFY(fbo.valid());
        RenderTarget target(&fbo, ColorDescription::sRGB);
        const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
        GLFramebuffer::pushFramebuffer(&fbo);
        const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
        const GLfloat dark[]{.04f, .05f, .06f, 1};
        glClearBufferfv(GL_COLOR, 0, dark);
        const Region region(0, 0, size.width(), size.height());
        QVERIFY(renderer.prepareTargets(target, viewport));
        QVERIFY(renderer.beginFrame(target, viewport, region));
        renderer.endFrame(target, viewport, region, renderer.updateBloom(target, {}));
        QVERIFY(std::abs(readPixel(16, 16)[0] - dark[0]) < .001f);
        QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
    };

    QVERIFY(m_context->makeCurrent());
    auto *firstEntry = cache.current();
    QVERIFY(firstEntry && firstEntry->ready);
    paint(firstEntry->renderer, QSize(64, 64));
    QVERIFY(second->makeCurrent());
    auto *secondEntry = cache.current();
    QVERIFY(secondEntry && secondEntry->ready);
    QVERIFY(firstEntry != secondEntry);
    paint(secondEntry->renderer, QSize(96, 64));
    QVERIFY(firstEntry->renderer.diagnosticStatus().contains(QStringLiteral("hdr=64x64")));
    QVERIFY(secondEntry->renderer.diagnosticStatus().contains(QStringLiteral("hdr=96x64")));

    // 在第二个上下文触发失效，两个上下文的资源都应在各自的归属中释放。
    cache.invalidateOutput(nullptr);
    QCOMPARE(EglContext::currentContext(), second.get());
    QVERIFY(!cache.diagnosticStatus().contains(QStringLiteral(":hdr=")));
    paint(secondEntry->renderer, QSize(96, 64));
    QVERIFY(m_context->makeCurrent());
    QCOMPARE(cache.current(), firstEntry);
    paint(firstEntry->renderer, QSize(64, 64));
    cache.releaseDevice(nullptr);
    QCOMPARE(EglContext::currentContext(), m_context.get());
    QVERIFY(!cache.ready());
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::failedGpuContextIsReleasedOnReplacement()
{
    auto failed = EglContext::create(m_display.get(), EGL_NO_CONFIG_KHR, nullptr);
    QVERIFY(failed);
    QVERIFY(failed->makeCurrent());
    const auto restore = qScopeGuard([this] { (void)m_context->makeCurrent(); });
    GpuRendererCache cache;
    auto *original = cache.current();
    QVERIFY(original && original->ready);
    failed->setFailed();
    QVERIFY(!cache.current());
    std::weak_ptr<EglContext> old = failed;
    failed.reset();

    QVERIFY(m_context->makeCurrent());
    const QSize size(64, 64);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    auto *replacement = cache.current();
    QVERIFY(replacement && replacement->ready);
    QVERIFY(old.expired());
    QCOMPARE(EglContext::currentContext(), m_context.get());
    QCOMPARE(GLFramebuffer::currentFramebuffer(), &fbo);
    RenderTarget target(&fbo, ColorDescription::sRGB);
    const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
    const Region region(0, 0, size.width(), size.height());
    const GLfloat dark[]{.04f, .05f, .06f, 1};
    glClearBufferfv(GL_COLOR, 0, dark);
    auto &renderer = replacement->renderer;
    QVERIFY(renderer.prepareTargets(target, viewport));
    QVERIFY(renderer.beginFrame(target, viewport, region));
    renderer.endFrame(target, viewport, region, renderer.updateBloom(target, {}));
    QVERIFY(std::abs(readPixel(16, 16)[0] - dark[0]) < .001f);
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::brightSourceProducesBloom_data()
{
    QTest::addColumn<bool>("beforeBloom");
    QTest::newRow("UIAlwaysVisible") << true;
    QTest::newRow("LetterBox") << false;
}

void GpuTests::brightSourceProducesBloom()
{
    QFETCH(bool, beforeBloom);
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
        QVERIFY(renderer.prepareTargets(target, viewport));
        const Region output(0, 0, 128, 96);
        QVERIFY(renderer.beginFrame(target, viewport, output));
        const Region source(56, 40, 16, 16);
        const auto drawBrightSource = [] {
            glEnable(GL_SCISSOR_TEST);
            glScissor(56, 40, 16, 16);
            const GLfloat bright[] = {4, 2, 1, 1};
            glClearBufferfv(GL_COLOR, 0, bright);
            glDisable(GL_SCISSOR_TEST);
        };
        if (beforeBloom) {
            drawBrightSource();
        }
        const bool bloomOk = renderer.updateBloom(target, beforeBloom ? source : Region());
        if (!beforeBloom) {
            drawBrightSource();
        }
        renderer.endFrame(target, viewport, output, bloomOk);
    }
    const auto halo = readPixel(54, 48); // 亮源几何以外，只能由辉光照亮。
    const auto core = readPixel(64, 48);
    GLFramebuffer::popFramebuffer();
    QVERIFY(core[0] > .99f);
    if (beforeBloom) {
        QVERIFY2(halo[0] > .01f, "UIAlwaysVisible 亮源外应有辉光");
    } else {
        QVERIFY2(halo[0] < .001f, "LetterBox 亮源不应进入辉光金字塔");
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::movingBloomMatchesFullRepaint_data()
{
    QTest::addColumn<QSize>("size");
    QTest::newRow("even") << QSize(640, 360);
    QTest::newRow("odd") << QSize(641, 359);
    QTest::newRow("portrait") << QSize(360, 640);
    QTest::newRow("short-side") << QSize(1024, 64);
}

void GpuTests::movingBloomMatchesFullRepaint()
{
    QFETCH(QSize, size);
    const QRect output(QPoint(), size);
    const auto cd = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    // Reference redraws the whole scene. The second run preserves all pixels outside
    // damage, as KWin does. A moving/disappearing source must leave the same image.
    const std::array<QRect, 7> sources{{QRect(size.width()/8, size.height()/6, 16, 16),
        QRect(size.width()/8+15, size.height()/6+7, 16, 16),
        QRect(size.width()/2, size.height()/2, 16, 16),
        QRect(size.width()-12, size.height()-12, 12, 12), {}, {}, {}}};
    const std::array<QRect, sources.size()> postBloom{{{}, {},
        QRect(size.width()/4, size.height()/4, 8, 8), {},
        QRect(5, 5, 8, 8), QRect(size.width()/2, 5, 8, 8), {}}};
    std::array<std::vector<float>, sources.size()> reference;
    const GLfloat background[]{.04f, .06f, .08f, 1};
    const GLfloat bright[]{4, 2, 1, 1};
    const GLfloat triangle[]{.4f, .8f, 1.2f, 1};
    const int reach = int(std::ceil(baclickfx::bloomReachPx(size, 1.0))) + 2;
    const auto region = [](const QRect &rect) {
        return Region(Rect(rect.x(), rect.y(), rect.width(), rect.height()));
    };
    const auto clearRect = [&](const Rect &rect, const GLfloat *color) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(rect.x(), size.height() - rect.bottom(), rect.width(), rect.height());
        glClearBufferfv(GL_COLOR, 0, color);
        glDisable(GL_SCISSOR_TEST);
    };
    for (bool partial : {false, true}) {
        auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
        GLFramebuffer fbo(texture.get());
        QVERIFY(fbo.valid());
        RenderTarget target(&fbo, cd);
        RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
        GLFramebuffer::pushFramebuffer(&fbo);
        const auto popFramebuffer = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
        glClearBufferfv(GL_COLOR, 0, background);
        {
            GPURenderer renderer;
            QVERIFY(renderer.initialize());
            QVERIFY(renderer.prepareTargets(target, viewport));
            Region previous;
            for (std::size_t frame = 0; frame < sources.size(); ++frame) {
                const QRect source = sources[frame];
                const QRect glow = source.isEmpty() ? QRect()
                    : source.adjusted(-reach, -reach, reach, reach).intersected(output);
                const QRect post = postBloom[frame];
                const QRect postDamage = post.isEmpty() ? QRect()
                    : post.adjusted(-1, -1, 1, 1).intersected(output);
                const Region damage = partial ? region(glow).united(region(postDamage)) : region(output);
                const Region repaint = damage.united(previous);
                for (const Rect &rect : repaint.rects()) {
                    clearRect(rect, background);
                }
                QVERIFY(renderer.beginFrame(target, viewport, repaint));
                if (!source.isEmpty()) {
                    clearRect(Rect(source.x(), source.y(), source.width(), source.height()), bright);
                }
                const Region bloomUpdate = region(partial ? source : output);
                const bool bloomOk = renderer.updateBloom(target, bloomUpdate);
                if (!post.isEmpty()) {
                    clearRect(Rect(post.x(), post.y(), post.width(), post.height()), triangle);
                }
                renderer.endFrame(target, viewport, damage, bloomOk);
                std::vector<float> pixels(std::size_t(size.width()) * size.height() * 4);
                glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, pixels.data());
                if (!partial) {
                    reference[frame] = std::move(pixels);
                } else {
                    float maxError = 0;
                    std::size_t maxIndex = 0;
                    for (std::size_t i = 0; i < pixels.size(); ++i) {
                        if (const float error = std::abs(pixels[i] - reference[frame][i]); error > maxError) {
                            maxError = error;
                            maxIndex = i;
                        }
                    }
                    QVERIFY2(maxError < .002f, qPrintable(QStringLiteral("frame=%1 maxError=%2 pixel=(%3,%4)")
                        .arg(frame).arg(maxError).arg((maxIndex/4)%size.width()).arg((maxIndex/4)/size.width())));
                }
                previous = damage;
            }
        }
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::hdrToSdrClearsDesktopBloomOutsideParticleSources()
{
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    const auto hdr = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 320), 80, 0, 320, 320);
    const auto sdr = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    RenderTarget hdrTarget(&fbo, hdr);
    RenderTarget sdrTarget(&fbo, sdr);
    const RectF rect(0, 0, size.width(), size.height());
    const RenderViewport hdrView(rect, 1.0, hdrTarget, QPoint());
    const RenderViewport sdrView(rect, 1.0, sdrTarget, QPoint());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    GPURenderer renderer;
    QVERIFY(renderer.initialize());

    // HDR 桌面本身是亮源，粒子亮源区域为空。
    const Region output(0, 0, size.width(), size.height());
    const GLfloat bright[]{.75f, .75f, .75f, 1};
    glClearBufferfv(GL_COLOR, 0, bright);
    QVERIFY(renderer.prepareTargets(hdrTarget, hdrView));
    QVERIFY(renderer.beginFrame(hdrTarget, hdrView, output));
    const bool hdrBloom = renderer.updateBloom(hdrTarget, {});
    QVERIFY(hdrBloom);
    renderer.endFrame(hdrTarget, hdrView, output, hdrBloom);
    QVERIFY(readPixel(64, 48)[0] > bright[0] + .01f);

    // 复用同尺寸目标。SDR 帧只合成一块小区域，且仍然没有 Bloom 前的粒子。
    const GLfloat dark[]{.04f, .05f, .06f, 1};
    glClearBufferfv(GL_COLOR, 0, dark);
    const Region damage(60, 44, 8, 8);
    QVERIFY(renderer.prepareTargets(sdrTarget, sdrView));
    QVERIFY(renderer.beginFrame(sdrTarget, sdrView, damage));
    const bool sdrBloom = renderer.updateBloom(sdrTarget, {});
    QVERIFY(sdrBloom);
    renderer.endFrame(sdrTarget, sdrView, damage, sdrBloom);
    const auto pixel = readPixel(64, 48);
    for (int channel = 0; channel < 3; ++channel) {
        QVERIFY(std::abs(pixel[channel] - dark[channel]) < .001f);
    }
}

void GpuTests::wideGamutSdrDesktopBloomMatchesFullRepaint_data()
{
    QTest::addColumn<bool>("p3");
    QTest::newRow("P3") << true;
    QTest::newRow("BT2020") << false;
}

void GpuTests::wideGamutSdrDesktopBloomMatchesFullRepaint()
{
    QFETCH(bool, p3);
    const QSize size(128, 96);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    QVERIFY(texture);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    const auto color = std::make_shared<ColorDescription>(
        p3 ? Colorimetry::DisplayP3 : Colorimetry::BT2020,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    RenderTarget target(&fbo, color);
    const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
    const Region output(0, 0, size.width(), size.height());
    const Region damage(64, 0, 8, size.height());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    const GLfloat dark[]{.04f, .05f, .06f, 1};
    const GLfloat red[]{1, 0, 0, 1};
    std::vector<float> reference;
    for (bool full : {true, false}) {
        glClearBufferfv(GL_COLOR, 0, dark);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, 64, size.height());
        glClearBufferfv(GL_COLOR, 0, red);
        glDisable(GL_SCISSOR_TEST);
        GPURenderer renderer;
        QVERIFY(renderer.initialize());
        QVERIFY(renderer.prepareTargets(target, viewport));
        QVERIFY(renderer.beginFrame(target, viewport, full ? output : damage));
        // 亮源来自局部重绘区域以外的桌面，当前帧没有粒子亮源。
        const bool bloom = renderer.updateBloom(target, full ? output : Region());
        renderer.endFrame(target, viewport, full ? output : damage, bloom);
        QVERIFY(bloom);
        QVERIFY(readPixel(64, 48)[0] > dark[0] + .01f);
        std::vector<float> pixels(std::size_t(8 * size.height() * 4));
        glReadPixels(64, 0, 8, size.height(), GL_RGBA, GL_FLOAT, pixels.data());
        if (full) {
            reference = std::move(pixels);
        } else {
            QCOMPARE(pixels, reference);
        }
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::outputColorMatchesGameWorkingSpace_data()
{
    QTest::addColumn<int>("gamut");
    QTest::addColumn<int>("transfer");
    QTest::addColumn<double>("peak");
    QTest::newRow("709-gamma22") << 0 << int(TransferFunction::gamma22) << 80.0;
    QTest::newRow("P3-gamma22") << 1 << int(TransferFunction::gamma22) << 80.0;
    QTest::newRow("2020-gamma22") << 2 << int(TransferFunction::gamma22) << 80.0;
    QTest::newRow("2020-PQ") << 2 << int(TransferFunction::PerceptualQuantizer) << 10000.0;
    QTest::newRow("2020-HLG-1000") << 2 << int(TransferFunction::HLG) << 1000.0;
    QTest::newRow("2020-HLG-600") << 2 << int(TransferFunction::HLG) << 600.0;
    QTest::newRow("709-sRGB") << 0 << int(TransferFunction::sRGB) << 80.0;
    QTest::newRow("709-linear") << 0 << int(TransferFunction::linear) << 80.0;
    QTest::newRow("709-BT1886") << 0 << int(TransferFunction::BT1886) << 80.0;
}

void GpuTests::outputColorMatchesGameWorkingSpace()
{
    QFETCH(int, gamut);
    QFETCH(int, transfer);
    QFETCH(double, peak);
    const auto &primaries = gamut == 1 ? Colorimetry::DisplayP3
        : gamut == 2 ? Colorimetry::BT2020 : Colorimetry::BT709;
    const bool hlg = transfer == TransferFunction::HLG;
    const TransferFunction tf(TransferFunction::Type(transfer), hlg ? .05 : 0, peak);
    const auto cd = std::make_shared<ColorDescription>(primaries, tf, 80, 0, 80, tf.maxLuminance);
    const QSize size(32, 32);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    RenderTarget target(&fbo, cd);
    RenderViewport viewport(RectF(0, 0, 32, 32), 1, target, QPoint());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto popFramebuffer = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    const QVector3D background = tf.nitsToEncoded(QVector3D(40, .1f, 2));
    const GLfloat clear[]{background.x(), background.y(), background.z(), 1};
    glClearBufferfv(GL_COLOR, 0, clear);
    const auto before = readPixel(1, 1);
    {
        GPURenderer renderer;
        QVERIFY(renderer.initialize());
        QVERIFY(renderer.prepareTargets(target, viewport));
        const Region output(0, 0, 32, 32);
        QVERIFY(renderer.beginFrame(target, viewport, output));
        if (hlg) {
            const auto decoded = readPixel(1, 1);
            const auto expected = primaries.relativeColorimetricTo(Colorimetry::BT709)
                .map(tf.encodedToNits(QVector3D(before[0], before[1], before[2])) / 80.f);
            for (int ch = 0; ch < 3; ++ch) {
                QVERIFY(std::abs(decoded[ch] - expected[ch]) < .001f);
            }
        }
        glEnable(GL_SCISSOR_TEST);
        glScissor(8, 8, 16, 16);
        // Source color is in the original game's linear BT.709 working space.
        // Keep it below the Bloom threshold to isolate final color conversion.
        const GLfloat source[]{.125f, .25f, .5f, 1};
        glClearBufferfv(GL_COLOR, 0, source);
        glDisable(GL_SCISSOR_TEST);
        renderer.endFrame(target, viewport, output, renderer.updateBloom(target, Region(8, 8, 16, 16)));
    }
    const auto actual = readPixel(16, 16);
    const auto outside = readPixel(1, 1);
    const auto linear = Colorimetry::BT709.relativeColorimetricTo(primaries)
        .map(QVector3D(.125f, .25f, .5f));
    const auto expected = tf.nitsToEncoded(linear * 80);
    for (int ch = 0; ch < 3; ++ch) {
        QVERIFY2(std::abs(actual[ch] - expected[ch]) < .002f,
            qPrintable(QStringLiteral("channel=%1 actual=%2 expected=%3")
                .arg(ch).arg(actual[ch]).arg(expected[ch])));
        QVERIFY(std::abs(outside[ch] - before[ch]) < .002f);
    }
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::patternedBackgroundRoundTrips_data()
{
    QTest::addColumn<int>("transform");
    QTest::addColumn<double>("scale");
    QTest::addColumn<bool>("direct");
    for (int transform = 0; transform < 8; ++transform) {
        for (double scale : {1.0, 1.25, 1.5, 1.6}) {
            const QByteArray name = QByteArray::number(transform) + '-' + QByteArray::number(scale);
            QTest::newRow(name.constData()) << transform << scale << true;
        }
    }
    QTest::newRow("blit") << 0 << 1.0 << false;
    QTest::newRow("blit-125") << 0 << 1.25 << false;
    QTest::newRow("blit-150") << 0 << 1.5 << false;
    QTest::newRow("blit-fractional") << 0 << 1.6 << false;
}

void GpuTests::patternedBackgroundRoundTrips()
{
    QFETCH(int, transform);
    QFETCH(double, scale);
    QFETCH(bool, direct);
    const QSize size(129, 97);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    texture->setContentTransform(OutputTransform(OutputTransform::Kind(transform)));
    texture->setFilter(GL_LINEAR);
    std::vector<float> pattern(std::size_t(size.width()) * size.height() * 4);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            const std::size_t i = std::size_t(y * size.width() + x) * 4;
            pattern[i] = .02f + .2f * x / size.width();
            pattern[i+1] = .03f + .15f * y / size.height();
            pattern[i+2] = (x/11 + y/7) % 2 ? .08f : .18f;
            pattern[i+3] = 1;
        }
    }
    texture->bind();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, pattern.data());
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    GLFramebuffer wrapper(fbo.handle(), size);
    const auto cd = std::make_shared<ColorDescription>(Colorimetry::BT709,
        TransferFunction(TransferFunction::gamma22, 0, 80), 80, 0, 80, 80);
    RenderTarget target(direct ? &fbo : &wrapper, cd);
    const QSize transformedSize = target.transformedSize();
    RenderViewport viewport(RectF(-211, 37, transformedSize.width()/scale, transformedSize.height()/scale),
        scale, target, QPoint());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto popFramebuffer = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    std::vector<float> before(pattern.size()), after(pattern.size());
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, before.data());
    {
        GPURenderer renderer;
        QVERIFY(renderer.initialize());
        QVERIFY(renderer.prepareTargets(target, viewport));
        const Region dirty(-202, 44, 37, 29);
        QVERIFY(renderer.beginFrame(target, viewport, dirty));
        renderer.endFrame(target, viewport, dirty, renderer.updateBloom(target, Region()));
    }
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, after.data());
    float maxError = 0;
    std::size_t maxIndex = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (const float error = std::abs(before[i] - after[i]); error > maxError) {
            maxError = error;
            maxIndex = i;
        }
    }
    QVERIFY2(maxError < .0005f, qPrintable(QStringLiteral("maxError=%1 pixel=(%2,%3)")
        .arg(maxError).arg((maxIndex/4)%size.width()).arg((maxIndex/4)/size.width())));
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

void GpuTests::overlappingParticlesUseAdditiveBlendEquation()
{
    const QSize size(640, 360);
    auto texture = baclickfx::allocateTexture(GL_RGBA16F, size);
    GLFramebuffer fbo(texture.get());
    QVERIFY(fbo.valid());
    RenderTarget target(&fbo);
    const RenderViewport viewport(RectF(0, 0, size.width(), size.height()), 1.0, target, QPoint());
    const Region output(0, 0, size.width(), size.height());
    GLFramebuffer::pushFramebuffer(&fbo);
    const auto pop = qScopeGuard([] { GLFramebuffer::popFramebuffer(); });
    const auto resetBlend = qScopeGuard([] { glBlendEquation(GL_FUNC_ADD); });

    GPURenderer renderer;
    QVERIFY(renderer.initialize());
    QVERIFY(renderer.prepareTargets(target, viewport));
    const auto params = baclickfx::buildSubsystemMap(1, 1, size.height(), size.width());
    baclickfx::Rng rng(42);
    auto first = makeClickInstance(QPointF(316, 180), params, 1, rng);
    auto second = makeClickInstance(QPointF(324, 180), params, 1, rng);
    initializeClickBirth(first, 1.0 / 60);
    initializeClickBirth(second, 1.0 / 60);
    advanceClickInstance(first, .065);
    advanceClickInstance(second, .023);
    TrailStream trail;
    trail.beginStroke(QPointF(285, 180.5));
    trail.addSegment(QPointF(285, 180.5), QPointF(315, 180.5));
    trail.addSegment(QPointF(315, 180.5), QPointF(350, 180.5));
    const auto strokes = buildTrailStrokes(trail, params.trail);
    QVERIFY(!strokes.empty());
    const auto mesh = baclickfx::loadMeshProfiles(BA_CLICK_FX_SOURCE_DIR "/assets/Cylinder002.obj");
    QVERIFY(!mesh.cylinder002.mesh.faces.empty());

    std::array<std::vector<float>, 3> frames;
    std::array<std::vector<float>, 2> composited;
    const GLfloat black[]{0, 0, 0, 1};
    for (std::size_t i = 0; i < frames.size(); ++i) {
        glClearBufferfv(GL_COLOR, 0, black);
        const GLenum hostRgb = i == 2 ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        const GLenum hostAlpha = i == 2 ? GL_MAX : GL_FUNC_ADD;
        glBlendEquationSeparate(hostRgb, hostAlpha);
        QVERIFY(renderer.beginFrame(target, viewport, output));
        if (i != 1) {
            renderer.renderTrail(strokes, params.trail, {});
        }
        renderer.renderClickBase(first, {});
        renderer.renderClickBase(second, {});
        renderer.renderClickMeshTri(first, mesh.cylinder002, {});
        renderer.renderClickMeshTri(second, mesh.cylinder002, {});
        if (i == 1) {
            renderer.renderTrail(strokes, params.trail, {});
        }
        frames[i].resize(std::size_t(size.width()) * size.height() * 4);
        glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, frames[i].data());
        const bool bloomOk = renderer.updateBloom(target, output);
        renderer.endFrame(target, viewport, output, bloomOk);
        if (i < composited.size()) {
            composited[i].resize(frames[i].size());
            glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_FLOAT, composited[i].data());
        }
        GLint restoredRgb = 0, restoredAlpha = 0;
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &restoredRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &restoredAlpha);
        QCOMPARE(restoredRgb, GLint(hostRgb));
        QCOMPARE(restoredAlpha, GLint(hostAlpha));
    }

    float orderDifference = 0;
    float outputDifference = 0;
    float hostDifference = 0;
    for (std::size_t i = 0; i < frames[0].size(); ++i) {
        if (i % 4 == 3) {
            continue;
        }
        orderDifference = std::max(orderDifference, std::abs(frames[0][i] - frames[1][i]));
        hostDifference = std::max(hostDifference, std::abs(frames[0][i] - frames[2][i]));
        outputDifference = std::max(outputDifference, std::abs(composited[0][i] - composited[1][i]));
    }
    qInfo() << "Peak RGB delta from moving Trail after Ring, linear and composited:"
            << orderDifference << outputDifference;
    QVERIFY(orderDifference > .01f);
    QVERIFY(outputDifference > 1.f / 255);
    QVERIFY2(hostDifference < .001f, qPrintable(QStringLiteral("Inherited blend equation changed RGB by %1").arg(hostDifference)));
    QCOMPARE(glGetError(), GLenum(GL_NO_ERROR));
}

QTEST_GUILESS_MAIN(GpuTests)
#include "gpu_tests.moc"
