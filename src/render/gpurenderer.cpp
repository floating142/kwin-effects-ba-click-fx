// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpurenderer.h"
#include "gputexture.h"
#include "baclickfxdefaults.h"
#include "particles/clickinstance.h"
#include "particles/curveutils.h"
#include "meshprofiles.h"
#include "particles/subsystems.h"
#include "particles/trailstream.h"
#include "bloomutils.h"

#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <opengl/eglcontext.h>
#include <opengl/glplatform.h>
#include <opengl/glshadermanager.h>

#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <span>

namespace KWin
{

Q_LOGGING_CATEGORY(KWIN_BA_CLICK_FX_RENDERER, "kwin_effect_ba_click_fx")

namespace
{

bool s_logErrors = false;

#define GPU_ERROR() if (!s_logErrors) {} else qCWarning(KWIN_BA_CLICK_FX_RENDERER) \
    << "event=error component=gpu_renderer"

// 着色器源码安装在 shader/ 目录，并在初始化阶段读取和编译。

// 辉光参数来自 MXBloomSettings 和 Hidden/MXFinalBloom 编译产物。
constexpr float kBloomThreshold = 1.0f;
// SoftKnee 为 0 时使用近似硬阈值；1e-5 同时避免 QuadraticThreshold 除零。
constexpr float kBloomKnee = kBloomThreshold * 0.0f + 1e-5f;
// Intensity 配置值为 1.7。PPv2 的 BloomRenderer.Render 会先执行
//     intensity = Exp2(settings.intensity / 10) - 1
// 因此最终合成阶段使用约 0.1250 的 _Bloom_Settings.y。
const float kBloomIntensity = std::exp2(1.7f / 10.f) - 1.f;
// Clamp 为 65472。PPv2 在传入着色器前调用 GammaToLinearSpace：
//     pow(65472, 2.2) ≈ 3.9385e10
// 当前配置下首先生效的是半精度浮点上限 65504；保留独立 Clamp 值以维持 PPv2 的
// 参数语义。
//
constexpr float kBloomClampMax = 3.9385e10f;
// Color 为白色，但仍显式参与辉光染色公式。
constexpr float kBloomColor[3] = {1.0f, 1.0f, 1.0f};
constexpr auto kBloomRetryDelay = std::chrono::seconds(1);

void clearColorAttachment(GLFramebuffer *fbo)
{
    static constexpr GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GLFramebuffer::pushFramebuffer(fbo);
    glClearBufferfv(GL_COLOR, 0, clearColor);
    GLFramebuffer::popFramebuffer();
}

// 效果层 Region 使用全局逻辑坐标，离屏纹理使用左上角为原点的纹理像素坐标。
// 区域映射统一向外取整，避免裁掉辉光采样核的有效贡献。
Rect alignedLogicalRect(const QRectF &rect)
{
    const int left = int(std::floor(rect.x()));
    const int top = int(std::floor(rect.y()));
    const int right = int(std::ceil(rect.x() + rect.width()));
    const int bottom = int(std::ceil(rect.y() + rect.height()));
    return Rect(left, top, std::max(0, right - left), std::max(0, bottom - top));
}

Region mapGlobalRegionToTexture(const Region &globalRegion, const QRectF &outputRect,
                                const QSize &textureSize, int haloTexels = 0)
{
    if (globalRegion.isEmpty() || outputRect.isEmpty() || textureSize.isEmpty()) {
        return Region();
    }

    const Region clipped = globalRegion.intersected(alignedLogicalRect(outputRect));
    QList<Rect> mapped;
    mapped.reserve(clipped.rects().size());
    for (const Rect &r : clipped.rects()) {
        int left = int(std::floor((double(r.left()) - outputRect.x())
                                  / outputRect.width() * textureSize.width()))
            - haloTexels;
        int right = int(std::ceil((double(r.right()) - outputRect.x())
                                  / outputRect.width() * textureSize.width()))
            + haloTexels;
        int top = int(std::floor((double(r.top()) - outputRect.y())
                                 / outputRect.height() * textureSize.height()))
            - haloTexels;
        int bottom = int(std::ceil((double(r.bottom()) - outputRect.y())
                                   / outputRect.height() * textureSize.height()))
            + haloTexels;

        left = std::clamp(left, 0, textureSize.width());
        right = std::clamp(right, 0, textureSize.width());
        top = std::clamp(top, 0, textureSize.height());
        bottom = std::clamp(bottom, 0, textureSize.height());
        if (left < right && top < bottom) {
            mapped.append(Rect(left, top, right - left, bottom - top));
        }
    }
    return Region::fromUnsortedRects(mapped);
}

// Use one pixel grid for copying, background decoding and compositing. KWin's
// scaledRenderRect is in projection coordinates and can differ by a pixel from
// the real attachment when fractional output origins and extents are rounded.
class OutputPixelGrid
{
public:
    OutputPixelGrid(const RenderViewport &viewport, QSize size)
        : m_rect(viewport.scaledRenderRect())
        , m_size(size)
        , m_scale(viewport.scale())
    {
    }

    Region map(const Region &logical) const
    {
        const double sx = double(m_size.width()) / m_rect.width();
        const double sy = double(m_size.height()) / m_rect.height();
        const Rect bounds(QPoint(), m_size);
        QList<Rect> pixels;
        pixels.reserve(logical.rects().size());
        for (const Rect &area : logical.rects()) {
            const Rect relative = area.scaled(m_scale).rounded().translated(-m_rect.topLeft());
            const Rect rect = RectF(relative.x() * sx, relative.y() * sy,
                                    relative.width() * sx, relative.height() * sy)
                                  .rounded().intersected(bounds);
            if (!rect.isEmpty()) {
                pixels.append(rect);
            }
        }
        return Region::fromUnsortedRects(pixels);
    }

    QPointF project(int x, int y) const
    {
        return QPointF(m_rect.x() + double(x) * m_rect.width() / m_size.width(),
                       m_rect.y() + double(y) * m_rect.height() / m_size.height());
    }

private:
    Rect m_rect;
    QSize m_size;
    double m_scale;
};

Region mapTextureRegion(const Region &sourceRegion, const QSize &sourceSize,
                        const QSize &destinationSize, int haloTexels)
{
    if (sourceRegion.isEmpty() || sourceSize.isEmpty() || destinationSize.isEmpty()) {
        return Region();
    }

    QList<Rect> mapped;
    mapped.reserve(sourceRegion.rects().size());
    for (const Rect &r : sourceRegion.rects()) {
        int left = int(std::floor(double(r.left()) * destinationSize.width()
                                  / sourceSize.width()))
            - haloTexels;
        int right = int(std::ceil(double(r.right()) * destinationSize.width()
                                  / sourceSize.width()))
            + haloTexels;
        int top = int(std::floor(double(r.top()) * destinationSize.height()
                                 / sourceSize.height()))
            - haloTexels;
        int bottom = int(std::ceil(double(r.bottom()) * destinationSize.height()
                                   / sourceSize.height()))
            + haloTexels;

        left = std::clamp(left, 0, destinationSize.width());
        right = std::clamp(right, 0, destinationSize.width());
        top = std::clamp(top, 0, destinationSize.height());
        bottom = std::clamp(bottom, 0, destinationSize.height());
        if (left < right && top < bottom) {
            mapped.append(Rect(left, top, right - left, bottom - top));
        }
    }
    return Region::fromUnsortedRects(mapped);
}

void textureRegionQuads(const Region &region, const QSize &size,
                        std::vector<ParticleVertex> &vertices)
{
    vertices.clear();
    vertices.reserve(std::size_t(region.rects().size()) * 6);
    for (const Rect &r : region.rects()) {
        const float uLeft = float(r.left()) / size.width();
        const float uRight = float(r.right()) / size.width();
        const float vTop = 1.0f - float(r.top()) / size.height();
        const float vBottom = 1.0f - float(r.bottom()) / size.height();
        const float left = uLeft * 2.0f - 1.0f;
        const float right = uRight * 2.0f - 1.0f;
        const float top = vTop * 2.0f - 1.0f;
        const float bottom = vBottom * 2.0f - 1.0f;

        vertices.insert(vertices.end(), {
            {left, bottom, uLeft, vBottom, 1, 1, 1, 1},
            {right, bottom, uRight, vBottom, 1, 1, 1, 1},
            {left, top, uLeft, vTop, 1, 1, 1, 1},
            {right, bottom, uRight, vBottom, 1, 1, 1, 1},
            {right, top, uRight, vTop, 1, 1, 1, 1},
            {left, top, uLeft, vTop, 1, 1, 1, 1},
        });
    }
}

// 从标准数据目录定位已安装的资源文件。
QString locateAsset(const char *name)
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("kwin/effects/ba-click-fx/assets/")
                                      + QLatin1String(name));
}

// 从标准数据目录定位已安装的着色器文件。
QString locateShader(const char *name)
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                  QStringLiteral("kwin/effects/ba-click-fx/shader/")
                                      + QLatin1String(name));
}

QByteArray readShaderSource(const char *name)
{
    const QString path = locateShader(name);
    if (path.isEmpty()) {
        GPU_ERROR() << "code=shader_missing" << "name=" << name;
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        GPU_ERROR() << "code=shader_read_failed" << "path=" << path << file.errorString();
        return {};
    }
    return file.readAll();
}

// 时间和角度求值由 clickinstance.h 提供，使模拟、重绘区域计算和渲染共用同一实现。

} // namespace

// ===== GPURenderer 实现 ======================================================

GPURenderer::GPURenderer() = default;

GPURenderer::~GPURenderer()
{
    // 上下文丢失时 KWin 的包装对象也会跳过 GL 删除，资源由 EGL 回收。
    if (!EglContext::currentContext()) {
        return;
    }
    const auto deleteVertexArray = [](ShaderSet &set) {
        if (set.vao) {
            glDeleteVertexArrays(1, &set.vao);
            set.vao = 0;
        }
    };
    deleteVertexArray(m_shaderBackground);
    deleteVertexArray(m_shaderAlphablendAdd);
    deleteVertexArray(m_shaderDissolve);
    deleteVertexArray(m_shaderAdditive);
    deleteVertexArray(m_shaderComposite);
    deleteVertexArray(m_shaderBloomPrefilter);
    deleteVertexArray(m_shaderBloomDownsample);
    deleteVertexArray(m_shaderBloomUpsample);
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_activeTimerPhase >= 0) {
        glEndQuery(GL_TIME_ELAPSED);
        m_activeTimerPhase = -1;
    }
    for (GpuTimerFrame &frame : m_timerFrames) {
        for (GLuint &query : frame.queries) {
            if (query) {
                glDeleteQueries(1, &query);
                query = 0;
            }
        }
    }
}

bool GPURenderer::initialize()
{
    const auto glString = [](GLenum name) {
        const GLubyte *value = glGetString(name);
        return value ? QString::fromLatin1(reinterpret_cast<const char *>(value))
                     : QStringLiteral("unknown");
    };
    m_gpuVendor = glString(GL_VENDOR);
    m_gpuRenderer = glString(GL_RENDERER);
    m_gpuVersion = glString(GL_VERSION);

    // 由着色器编译结果判断当前 OpenGL 上下文是否满足要求，避免重复维护版本判据。

    // 按 Unity .meta 中的 wrapU/wrapV 设置各纹理的环绕模式
    // （Unity TextureWrapMode：0 = Repeat，1 = Clamp）：
    //   FX_TEX_Circle_01     wrapU/V: 0 → Repeat
    //   FX_TEX_Grad_Ring3    wrapU/V: 1 → Clamp
    //   FX_TEX_Trail_03      wrapU/V: 0 → Repeat
    //   FX_TEX_Triangle_02_1 wrapU/V: 1 → Clamp
    m_texCircle = loadTexture("FX_TEX_Circle_01.png", GL_REPEAT);
    m_texGradRing3 = loadTexture("FX_TEX_Grad_Ring3.png", GL_CLAMP_TO_EDGE);
    m_texTriangle = loadTexture("FX_TEX_Triangle_02_1.png", GL_CLAMP_TO_EDGE);
    m_texTrail = loadTexture("FX_TEX_Trail_03.png", GL_REPEAT);

    if (!m_texCircle || !m_texGradRing3 || !m_texTriangle || !m_texTrail) {
        GPU_ERROR() << "code=texture_init_failed";
        return false;
    }

    // 编译背景导入、三个粒子和最终合成着色器。
    m_shaderBackground = loadShader("particle.vert", "background.frag");
    m_shaderAlphablendAdd = loadShader("particle.vert", "alphablend_add.frag");
    m_shaderDissolve = loadShader("particle.vert", "dissolve.frag");
    m_shaderAdditive = loadShader("particle.vert", "additive.frag");
    m_shaderComposite = loadShader("particle.vert", "composite.frag");

    if (!m_shaderBackground || !m_shaderAlphablendAdd || !m_shaderDissolve || !m_shaderAdditive
        || !m_shaderComposite) {
        GPU_ERROR() << "code=shader_init_failed";
        return false;
    }

    // 辉光着色器失败时保留粒子渲染，并在最终合成中将辉光强度设为 0。
    m_shaderBloomPrefilter = loadShader("particle.vert", "bloom_prefilter.frag");
    m_shaderBloomDownsample = loadShader("particle.vert", "bloom_downsample.frag");
    m_shaderBloomUpsample = loadShader("particle.vert", "bloom_upsample.frag");
    if (!m_shaderBloomPrefilter || !m_shaderBloomDownsample || !m_shaderBloomUpsample) {
        GPU_ERROR() << "code=bloom_shader_failed" << "action=bloom_disabled";
    }

    // 所有布局共享一个流式 VBO；每个已链接的 shader 使用独立 VAO 缓存属性位置。
    glGenBuffers(1, &m_vbo);
    if (m_vbo == 0) {
        GPU_ERROR() << "code=buffer_init_failed";
        return false;
    }

    if (!initializeVertexArray(m_shaderBackground)
        || !initializeVertexArray(m_shaderAlphablendAdd)
        || !initializeVertexArray(m_shaderDissolve)
        || !initializeVertexArray(m_shaderAdditive)
        || !initializeVertexArray(m_shaderComposite)) {
        GPU_ERROR() << "code=vertex_array_init_failed";
        return false;
    }
    // Bloom shader 是可选链路；仅为成功链接的程序创建布局。
    if ((m_shaderBloomPrefilter && !initializeVertexArray(m_shaderBloomPrefilter))
        || (m_shaderBloomDownsample && !initializeVertexArray(m_shaderBloomDownsample))
        || (m_shaderBloomUpsample && !initializeVertexArray(m_shaderBloomUpsample))) {
        GPU_ERROR() << "code=bloom_vertex_array_init_failed" << "action=bloom_disabled";
        const auto deleteVertexArray = [](ShaderSet &set) {
            if (set.vao) {
                glDeleteVertexArrays(1, &set.vao);
                set.vao = 0;
            }
        };
        deleteVertexArray(m_shaderBloomPrefilter);
        deleteVertexArray(m_shaderBloomDownsample);
        deleteVertexArray(m_shaderBloomUpsample);
        m_shaderBloomPrefilter.shader.reset();
        m_shaderBloomDownsample.shader.reset();
        m_shaderBloomUpsample.shader.reset();
    }

    if (m_logVerbose) {
        qCInfo(KWIN_BA_CLICK_FX_RENDERER) << "event=initialized component=gpu_renderer";
    }
    return true;
}

QString GPURenderer::diagnosticStatus() const
{
    QStringList outputs;
    outputs.reserve(qsizetype(m_outputs.size()));
    for (const auto &target : m_outputs) {
        QStringList bloomSizes;
        for (const BloomLevel &level : target->bloomLevels) {
            bloomSizes.append(QStringLiteral("%1x%2")
                                  .arg(level.size.width())
                                  .arg(level.size.height()));
        }
        outputs.append(QStringLiteral("%1x%2@%3:bloom=%4")
                           .arg(target->devicePx.width())
                           .arg(target->devicePx.height())
                           .arg(target->scale, 0, 'f', 2)
                           .arg(target->bloomLevels.size())
                       + QStringLiteral(":hdr=%1x%2:bg=%3x%4:bloom_sizes=%5")
                             .arg(target->hdrTexture ? target->devicePx.width() : 0)
                             .arg(target->hdrTexture ? target->devicePx.height() : 0)
                             .arg(target->bgTexture ? target->devicePx.width() : 0)
                             .arg(target->bgTexture ? target->devicePx.height() : 0)
                             .arg(bloomSizes.join(QLatin1Char('|'))));
    }
    return QStringLiteral("gpu_vendor=\"%1\" gpu_renderer=\"%2\" gpu_version=\"%3\" "
                          "textures_ready=%4 shaders_ready=%5 import_mode=%6 targets=\"%7\"")
        .arg(m_gpuVendor.isEmpty() ? QStringLiteral("uninitialized") : m_gpuVendor)
        .arg(m_gpuRenderer.isEmpty() ? QStringLiteral("uninitialized") : m_gpuRenderer)
        .arg(m_gpuVersion.isEmpty() ? QStringLiteral("uninitialized") : m_gpuVersion)
        .arg(bool(m_texCircle && m_texGradRing3 && m_texTriangle && m_texTrail))
        .arg(bool(m_shaderBackground && m_shaderAlphablendAdd && m_shaderDissolve
                  && m_shaderAdditive && m_shaderComposite && m_shaderBloomPrefilter
                  && m_shaderBloomDownsample && m_shaderBloomUpsample))
        .arg(m_lastImportDirect ? QStringLiteral("direct") : QStringLiteral("blit"))
        .arg(outputs.join(QLatin1Char(',')));
}

GPURenderer::ShaderSet GPURenderer::loadShader(const char *vertexFile, const char *fragmentFile)
{
    const QByteArray vertexSource = readShaderSource(vertexFile);
    QByteArray fragmentSource = readShaderSource(fragmentFile);
    if (vertexSource.isEmpty() || fragmentSource.isEmpty()) {
        return {};
    }

    static const QByteArray includeDirective = QByteArrayLiteral("#include \"transfer.glsl\"");
    if (fragmentSource.contains(includeDirective)) {
        const QByteArray transferSource = readShaderSource("transfer.glsl");
        if (transferSource.isEmpty()) {
            return {};
        }
        fragmentSource.replace(includeDirective, transferSource);
    }

    ShaderSet set;
    set.shader = ShaderManager::instance()->loadShaderFromCode(vertexSource, fragmentSource);

    // 附加逻辑名称便于区分失败阶段；详细 GLSL 日志由 KWin 的 kwin_opengl 分类输出。
    if (!set.shader) {
        GPU_ERROR() << "code=shader_compile_failed" << "vertex=" << vertexFile
                    << "fragment=" << fragmentFile;
        return {};
    }

    // 顶点属性位置只能在链接后查询。被驱动优化掉的属性返回 -1，
    // setVertexLayout() 会跳过对应槽位。
    set.positionLocation = set.shader->attributeLocation("position");
    set.texcoordLocation = set.shader->attributeLocation("texcoord");
    set.colorLocation = set.shader->attributeLocation("color");
    set.custom0Location = set.shader->attributeLocation("custom0");
    return set;
}

bool GPURenderer::initializeVertexArray(ShaderSet &set)
{
    Q_ASSERT(set.shader);
    Q_ASSERT(m_vbo);

    GLint previousVao = 0;
    GLint previousVbo = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousVbo);

    glGenVertexArrays(1, &set.vao);
    if (set.vao) {
        glBindVertexArray(set.vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        setVertexLayout(set);
    }

    glBindVertexArray(static_cast<GLuint>(previousVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousVbo));
    return set.vao != 0;
}

std::unique_ptr<GLTexture> GPURenderer::loadTexture(const char *assetName, GLenum wrapMode)
{
    const QString path = locateAsset(assetName);
    if (path.isEmpty()) {
        GPU_ERROR() << "code=asset_missing" << "name=" << assetName;
        return nullptr;
    }

    QImage img(path);
    if (img.isNull()) {
        GPU_ERROR() << "code=image_load_failed" << "path=" << path;
        return nullptr;
    }

    // 直通 Alpha 与 Unity 相同，采样时由硬件执行 sRGB 解码。
    auto tex = baclickfx::uploadSrgbTexture(img);
    if (!tex || tex->isNull()) {
        GPU_ERROR() << "code=texture_alloc_failed" << "path=" << path;
        return nullptr;
    }

    // 原 PC Texture2D 的 FilterMode=1、MipCount=1：线性过滤且不生成多级纹理。
    tex->setFilter(GL_LINEAR);
    tex->setWrapMode(wrapMode);
    return tex;
}

bool GPURenderer::invalidateOutput(const LogicalOutput *output)
{
    for (const auto &targets : m_outputs) {
        if (targets->output == output && !targets->invalidated) {
            targets->invalidated = true;
            return true;
        }
    }
    return false;
}

void GPURenderer::releaseInvalidatedTargets()
{
    std::erase_if(m_outputs, [this](const auto &targets) {
        if (!targets->invalidated) {
            return false;
        }
        if (m_current == targets.get()) {
            m_current = nullptr;
        }
        return true;
    });
}

bool GPURenderer::prepareTargets(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                 const LogicalOutput *output)
{
    m_current = nullptr;
    releaseInvalidatedTargets();
    // 显式按分量转换 KWin::RectF，后续几何计算统一使用 QRectF 的边界语义。
    const RectF rr = viewport.renderRect();
    const QRectF outputRect(rr.x(), rr.y(), rr.width(), rr.height());
    const double scale = viewport.scale();

    // 投影矩形采用 KWin 的缩放取整坐标，采样网格则以实际附件像素为准。
    // RenderViewport 的投影在两个方向各留出 renderOffset 指定的边距。
    const Rect sr = viewport.scaledRenderRect();
    const QPoint offset = viewport.renderOffset();
    const QSize devicePx = renderTarget.transformedSize() - QSize(2 * offset.x(), 2 * offset.y());
    const QSize renderTargetDevicePx = renderTarget.size();
    const int transformKind = int(viewport.transform().kind());
    if (devicePx.isEmpty() || sr.isEmpty()) {
        return false;
    }

    // 同一输出切换视口时先释放旧资源，避免新旧全尺寸纹理同时占用显存。
    const auto cached = std::find_if(m_outputs.begin(), m_outputs.end(), [output](const auto &targets) {
        return targets->output == output;
    });
    if (cached != m_outputs.end()) {
        const auto &t = *cached;
        if (t->renderRect == outputRect && qFuzzyCompare(t->scale, scale)
            && t->devicePx == devicePx
            && t->renderTargetDevicePx == renderTargetDevicePx
            && t->transformKind == transformKind) {
            m_current = t.get();
            return true;
        }
        m_outputs.erase(cached);
    }

    auto targets = std::make_unique<OutputTargets>();
    targets->output = output;
    targets->renderRect = outputRect;
    targets->textureRect = QRectF(sr.x() / scale, sr.y() / scale,
                                  sr.width() / scale, sr.height() / scale);
    targets->scale = scale;
    targets->devicePx = devicePx;
    targets->renderTargetDevicePx = renderTargetDevicePx;
    targets->transformKind = transformKind;

    // 创建线性 RGBA16F 场景纹理及其帧缓冲。
    targets->hdrTexture = baclickfx::allocateTexture(GL_RGBA16F, devicePx);
    if (!targets->hdrTexture || targets->hdrTexture->isNull()) {
        GPU_ERROR() << "code=hdr_texture_alloc_failed" << devicePx;
        return false;
    }
    targets->hdrTexture->setFilter(GL_LINEAR);
    targets->hdrTexture->setWrapMode(GL_CLAMP_TO_EDGE);

    targets->hdrFbo = std::make_unique<GLFramebuffer>(targets->hdrTexture.get());
    if (!targets->hdrFbo->valid()) {
        GPU_ERROR() << "code=hdr_fbo_failed";
        return false;
    }

    // OpenGL 新分配纹理的内容未定义，而局部重绘和辉光采样可能读取尚未写入的边缘。
    // 创建资源时完整清零一次，避免未初始化显存进入辉光链。glClearBufferfv 不改变
    // 清除颜色或颜色掩码；临时关闭裁剪以覆盖整个附件。
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorWasEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }
    clearColorAttachment(targets->hdrFbo.get());
    if (scissorWasEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    if (m_logVerbose) {
        qCInfo(KWIN_BA_CLICK_FX_RENDERER)
            << "event=targets_created size=" << devicePx << " scale=" << scale;
    }

    m_outputs.push_back(std::move(targets));
    m_current = m_outputs.back().get();
    return true;
}

double GPURenderer::bloomReachPx(const QSize &devicePx, double scale)
{
    return baclickfx::bloomReachPx(devicePx, scale);
}

bool GPURenderer::prepareBloomPyramid(OutputTargets &targets)
{
    // 级数和上采样尺度由第 0 级尺寸及 Diffusion 按 PPv2 公式确定。
    QSize s = baclickfx::bloomBaseSize(targets.devicePx);
    const baclickfx::BloomPyramidParams params = baclickfx::bloomPyramidParams(s);
    std::vector<BloomLevel> levels;
    levels.reserve(params.iterations);

    // Unity 即使短边降到 1 也保留 Diffusion 指定的层数。
    for (int i = 0; i < params.iterations; i++) {

        BloomLevel level;
        level.size = s;

        // down 保存降采样结果，up 保存上采样结果。分离读写目标可避免 OpenGL
        // 反馈环；该结构与 PPv2 的 m_Pyramid[i].down/up 一致。
        const auto makeLevel = [&s](std::unique_ptr<GLTexture> &tex,
                                    std::unique_ptr<GLFramebuffer> &fbo) {
            tex = baclickfx::allocateTexture(GL_RGBA16F, s);
            if (!tex || tex->isNull()) {
                GPU_ERROR() << "code=bloom_texture_alloc_failed" << s;
                return false;
            }
            // 降采样和盒式上采样依赖硬件双线性过滤减少采样次数。
            tex->setFilter(GL_LINEAR);
            // 边缘钳制防止输出边缘的亮点从纹理对侧重复采样。
            tex->setWrapMode(GL_CLAMP_TO_EDGE);

            fbo = std::make_unique<GLFramebuffer>(tex.get());
            if (!fbo->valid()) {
                GPU_ERROR() << "code=bloom_fbo_failed" << s;
                return false;
            }
            return true;
        };

        if (!makeLevel(level.downTexture, level.downFbo)
            || !makeLevel(level.upTexture, level.upFbo)) {
            return false;
        }

        levels.push_back(std::move(level));
        s = QSize(s.width() >= 4 ? s.width() / 2 : 1,
                  s.height() >= 4 ? s.height() / 2 : 1);
    }

    // beginFrame() 已关闭裁剪。清零未覆盖的像素，保证首次局部更新可安全采样。
    for (const BloomLevel &level : levels) {
        clearColorAttachment(level.downFbo.get());
        clearColorAttachment(level.upFbo.get());
    }
    targets.bloomSampleScale = params.sampleScale;
    targets.bloomLevels = std::move(levels);
    return true;
}

void GPURenderer::setProfiling(bool on)
{
    if (m_profiling == on) {
        return;
    }
    m_profiling = on;
    m_lastGpuMs = -1.0;
    m_lastGpuImportMs = -1.0;
    m_lastGpuParticleMs = -1.0;
    m_lastGpuBloomMs = -1.0;
    m_lastGpuCompositeMs = -1.0;
    // 配置重载路径不保证存在当前 OpenGL 上下文，因此这里只记录清理请求。查询对象
    // 统一在下一次 beginFrame() 中释放，避免无上下文调用及复用旧采样结果。
    m_timerNeedsCleanup = true;
}

void GPURenderer::setLogLevel(baclickfx::defaults::LogLevel level)
{
    s_logErrors = level >= baclickfx::defaults::LogLevel::Error;
    m_logVerbose = level >= baclickfx::defaults::LogLevel::Verbose;
}

void GPURenderer::collectGpuTimings()
{
    for (GpuTimerFrame &frame : m_timerFrames) {
        if (!frame.pending || frame.issuedPhases <= 0) {
            continue;
        }

        bool allReady = true;
        for (int phase = 0; phase < frame.issuedPhases; phase++) {
            GLint available = 0;
            glGetQueryObjectiv(frame.queries[phase], GL_QUERY_RESULT_AVAILABLE, &available);
            if (!available) {
                allReady = false;
                break;
            }
        }
        if (!allReady) {
            continue;
        }

        std::array<double, kGpuTimerPhaseCount> phaseMs{};
        for (int phase = 0; phase < frame.issuedPhases; phase++) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(frame.queries[phase], GL_QUERY_RESULT, &ns);
            phaseMs[phase] = double(ns) / 1.0e6;
        }

        if (frame.publishable && frame.issuedPhases == kGpuTimerPhaseCount) {
            m_lastGpuImportMs = phaseMs[int(GpuTimerPhase::Import)];
            m_lastGpuParticleMs = phaseMs[int(GpuTimerPhase::Particle)];
            m_lastGpuBloomMs = phaseMs[int(GpuTimerPhase::Bloom)];
            m_lastGpuCompositeMs = phaseMs[int(GpuTimerPhase::Composite)];
            m_lastGpuMs = m_lastGpuImportMs + m_lastGpuParticleMs
                + m_lastGpuBloomMs + m_lastGpuCompositeMs;
            m_lastGpuOutput = frame.output;
            m_lastGpuOutputId = frame.outputId;
            m_gpuSampleSerial++;
        }

        frame.pending = false;
        frame.publishable = false;
        frame.issuedPhases = 0;
    }
}

void GPURenderer::beginGpuTimingFrame()
{
    if (!m_profiling || !EglContext::currentContext()->supportsTimerQueries()) {
        return;
    }

    for (GpuTimerFrame &frame : m_timerFrames) {
        if (!frame.queries[0]) {
            glGenQueries(kGpuTimerPhaseCount, frame.queries.data());
        }
    }
    collectGpuTimings();

    int slot = -1;
    for (int attempt = 0; attempt < kGpuTimerFrameCount; attempt++) {
        const int candidate = (m_nextTimerFrame + attempt) % kGpuTimerFrameCount;
        if (!m_timerFrames[candidate].pending) {
            slot = candidate;
            break;
        }
    }
    if (slot < 0) {
        return;
    }

    GpuTimerFrame &frame = m_timerFrames[slot];
    frame.pending = true;
    frame.publishable = true;
    frame.issuedPhases = 0;
    frame.output = QStringLiteral("%1x%2@%3")
        .arg(m_current->devicePx.width())
        .arg(m_current->devicePx.height())
        .arg(m_current->scale, 0, 'f', 2);
    frame.outputId = statsOutputId(m_current->output, m_current->renderRect.topLeft());
    m_activeTimerFrame = slot;
    m_activeTimerPhase = -1;
    m_nextTimerFrame = (slot + 1) % kGpuTimerFrameCount;
}

void GPURenderer::beginGpuTimerPhase(GpuTimerPhase phase)
{
    if (m_activeTimerFrame < 0) {
        return;
    }
    Q_ASSERT(m_activeTimerPhase < 0);

    const int phaseIndex = int(phase);
    GpuTimerFrame &frame = m_timerFrames[m_activeTimerFrame];
    frame.issuedPhases = std::max(frame.issuedPhases, phaseIndex + 1);
    m_activeTimerPhase = phaseIndex;
    glBeginQuery(GL_TIME_ELAPSED, frame.queries[phaseIndex]);
}

void GPURenderer::endGpuTimerPhase()
{
    if (m_activeTimerPhase < 0) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    m_activeTimerPhase = -1;
}

void GPURenderer::switchGpuTimerPhase(GpuTimerPhase phase)
{
    endGpuTimerPhase();
    beginGpuTimerPhase(phase);
}

void GPURenderer::finishGpuTimingFrame(bool publishable)
{
    if (m_activeTimerFrame < 0) {
        return;
    }
    endGpuTimerPhase();
    GpuTimerFrame &frame = m_timerFrames[m_activeTimerFrame];
    frame.publishable = publishable && frame.issuedPhases == kGpuTimerPhaseCount;
    m_activeTimerFrame = -1;
    m_activeTimerPhase = -1;
}

void GPURenderer::setColorSpaceUniforms(GLShader *shader, const RenderTarget &renderTarget, bool toWorkingSpace)
{
    // KWin 的颜色空间统一变量仅适用于 ShaderManager::generateShader() 生成的程序。
    // 本项目直接加载 GLSL 源码，因此显式传入所需的传递函数参数。
    const ColorDescription &cd = *renderTarget.colorDescription();
    const TransferFunction tf = cd.transferFunction();

    shader->setUniform("u_tfType", int(tf.type));
    // 第三个分量为参考白亮度，用于将绝对亮度归一化为 1.0 表示参考白。
    shader->setUniform("u_tfLum",
                       QVector3D(float(tf.minLuminance), float(tf.maxLuminance),
                                 float(cd.referenceLuminance())));
    // BT.1886 参数始终初始化；其他传递函数不会执行对应分支。
    shader->setUniform("u_tfBt1886", QVector2D(float(tf.bt1886A()), float(tf.bt1886B())));
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    if (tf.type == TransferFunction::HLG) {
        const double gamma = 1.2 + 0.42 * std::log10(tf.maxLuminance / 1000.0);
        const double alpha = std::pow(tf.maxLuminance, 1.0 - gamma);
        shader->setUniform("u_tfHlg", QVector3D(float(gamma), float(alpha), float(tf.hlgBeta())));
    }
#endif
    QMatrix4x4 colorMatrix; // BT.709 输出保持严格单位矩阵。
    if (cd.containerColorimetry() != Colorimetry::BT709) {
        colorMatrix = toWorkingSpace
            ? cd.containerColorimetry().relativeColorimetricTo(Colorimetry::BT709)
            : Colorimetry::BT709.relativeColorimetricTo(cd.containerColorimetry());
    }
    shader->setUniform("u_colorMatrix", colorMatrix);
}

QString GPURenderer::statsOutputId(const LogicalOutput *output, const QPointF &viewportOrigin)
{
    if (output) {
        const auto uuid = output->uuid();
        return uuid.isEmpty() ? output->name() : uuid;
    }
    return QStringLiteral("viewport@%1,%2").arg(viewportOrigin.x()).arg(viewportOrigin.y());
}

bool GPURenderer::desktopCanContributeBloom(const ColorDescription &color)
{
    // 饱和广色域颜色转换到线性 BT.709 时，分量可能超过参考白，即使输出是 SDR。
    return color.containerColorimetry() != Colorimetry::BT709
        || color.transferFunction().maxLuminance > color.referenceLuminance() * 1.0001;
}

bool GPURenderer::needsFullBloomRefresh(const ColorDescription &color) const
{
    if (desktopCanContributeBloom(color)) {
        return true;
    }
    const auto &previous = m_current->previousBloomColor;
    if (!previous || previous.get() == &color) {
        return false;
    }
    const auto &tf = color.transferFunction();
    const auto &oldTf = previous->transferFunction();
    return oldTf.type != tf.type || oldTf.minLuminance != tf.minLuminance
        || oldTf.maxLuminance != tf.maxLuminance
        || previous->referenceLuminance() != color.referenceLuminance()
        || previous->containerColorimetry() != color.containerColorimetry();
}

bool GPURenderer::prepareBackgroundTargets()
{
    if (m_current->bgFbo) {
        return true;
    }

    auto texture = baclickfx::allocateTexture(GL_RGBA16F, m_current->devicePx);
    if (!texture || texture->isNull()) {
        GPU_ERROR() << "code=background_texture_alloc_failed" << m_current->devicePx;
        return false;
    }
    texture->setFilter(GL_LINEAR);
    texture->setWrapMode(GL_CLAMP_TO_EDGE);
    auto fbo = std::make_unique<GLFramebuffer>(texture.get());
    if (!fbo->valid()) {
        GPU_ERROR() << "code=background_fbo_failed";
        return false;
    }

    m_current->bgTexture = std::move(texture);
    m_current->bgFbo = std::move(fbo);
    return true;
}

bool GPURenderer::beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport,
                             const Region &requestedImportRegion)
{
    Q_ASSERT(m_current); // prepareTargets() 必须先成功。

    // 粒子图层和最终合成会修改混合、视口与裁剪状态，入口值必须完整保存并在
    // endFrame() 中恢复。
    m_savedBlendEnabled = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &m_savedBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &m_savedBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_savedBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &m_savedBlendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_savedBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_savedBlendEquationAlpha);
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);
    m_savedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, m_savedScissorBox);
    m_blendStateCaptured = true;
    captureVertexState();

    // 在有效 OpenGL 上下文中回收 setProfiling() 标记的查询对象。
    if (m_timerNeedsCleanup) {
        m_timerNeedsCleanup = false;
        if (m_activeTimerPhase >= 0) {
            glEndQuery(GL_TIME_ELAPSED);
        }
        m_activeTimerFrame = -1;
        m_activeTimerPhase = -1;
        for (GpuTimerFrame &frame : m_timerFrames) {
            for (GLuint &query : frame.queries) {
                if (query) {
                    glDeleteQueries(1, &query);
                    query = 0;
                }
            }
            frame.pending = false;
            frame.publishable = false;
            frame.issuedPhases = 0;
        }
        m_nextTimerFrame = 0;
        m_lastGpuMs = -1.0;
        m_lastGpuImportMs = -1.0;
        m_lastGpuParticleMs = -1.0;
        m_lastGpuBloomMs = -1.0;
        m_lastGpuCompositeMs = -1.0;
        m_gpuSampleSerial = 0;
    }

    // 四个计时阶段连续提交，结果异步读取，不等待 GPU。
    beginGpuTimingFrame();

    // ── 第 1 步：导入桌面背景 ────────────────────────────────────────────────
    //
    // 对齐 Unity UIRenderPass 的第一条命令：
    //     commandBuffer.Blit(cameraTarget, UiRenderTargetId)
    // Ring 和 Dissolve 的混合公式依赖真实目标颜色，因此粒子必须绘制在已导入的桌面
    // 背景上，而不是透明黑色上。
    //
    // 背景先复制到 bgTexture，再由着色器解码到 hdrFbo；帧缓冲复制无法执行颜色
    // 传递函数解码。
    const QRectF &outRect = m_current->renderRect;

    // 普通 BT.709 SDR 可仅导入调用方请求的区域。HDR 或广色域桌面可能贡献辉光，
    // 辉光预筛选需要完整输出。
    // 色彩切换后的首帧同样需要完整输入，以重算该输出的辉光。
    Region importRegion = needsFullBloomRefresh(*renderTarget.colorDescription())
        ? Region(alignedLogicalRect(outRect))
        : requestedImportRegion.intersected(alignedLogicalRect(outRect));

    // 合并上一次合成区域，以清除跨非活动时段保留在私有场景纹理中的旧亮源。
    importRegion += m_current->previousCompositeRegion.intersected(alignedLogicalRect(outRect));

    // 区域和颜色空间判据计算完成后再开始 GPU 查询，避免计入 CPU 区域运算。
    beginGpuTimerPhase(GpuTimerPhase::Import);

    GLTexture *sourceTexture = renderTarget.texture();
    const bool directImport = sourceTexture && !sourceTexture->isNull()
        && sourceTexture->target() == GL_TEXTURE_2D && !sourceTexture->size().isEmpty();
    m_lastImportDirect = directImport;

    if (!directImport && !prepareBackgroundTargets()) {
        finishGpuTimingFrame(false);
        restoreVertexState();
        restoreBlendState();
        return false;
    }

    const OutputPixelGrid pixelGrid(viewport, m_current->devicePx);
    const Region importPixels = pixelGrid.map(importRegion);
    if (!directImport) {
        // 使用已经对齐的设备像素做一对一复制，避免从逻辑坐标再次取整。
        const RenderViewport copyViewport(
            RectF(0, 0, m_current->devicePx.width(), m_current->devicePx.height()),
            1.0, renderTarget, viewport.renderOffset());
        for (const Rect &pixels : importPixels.rects()) {
            if (!m_current->bgFbo->blitFromRenderTarget(renderTarget, copyViewport, pixels, pixels)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    GPU_ERROR() << "code=desktop_import_failed"
                                << "无法取得桌面像素，插件不会绘制";
                }
                finishGpuTimingFrame(false);
                restoreVertexState();
                restoreBlendState();
                glViewport(m_savedViewport[0], m_savedViewport[1],
                           m_savedViewport[2], m_savedViewport[3]);
                return false;
            }
        }
    }

    // 绑定输出局部 HDR 帧缓冲。
    GLFramebuffer::pushFramebuffer(m_current->hdrFbo.get());

    // KWin 裁剪框使用输出目标设备坐标，不能直接用于输出局部的私有帧缓冲。离屏阶段
    // 通过显式区域几何限制绘制，返回输出帧缓冲后再恢复原裁剪状态。
    glDisable(GL_SCISSOR_TEST);

    // 显式设置私有帧缓冲视口，不依赖 pushFramebuffer() 修改视口。
    glViewport(0, 0, m_current->devicePx.width(), m_current->devicePx.height());

    // 投影矩阵将输出局部逻辑坐标映射到标准化设备坐标。交换 bottom/top 使 y 轴
    // 向下，与 Qt 坐标系一致；背景解码和粒子几何共用该矩阵。
    const QRectF grid = m_current->textureRect.translated(-outRect.topLeft());
    m_projection.setToIdentity();
    m_projection.ortho(float(grid.left()), float(grid.right()),
                       float(grid.bottom()), float(grid.top()),
                       -1.0f, 1.0f);

    // ── 第 2 步：将背景解码到线性 HDR 场景纹理 ─────────────────────────────
    //
    // SDR 输出仅解码 importRegion，以使带宽开销随重绘区域变化。合成区域始终包含在
    // 导入区域内，旧粒子区域也已由当前桌面覆盖；HDR 输出则在前面扩展为完整输出。
    if (!importRegion.isEmpty()) {
        GLShader *bg = m_shaderBackground.get();
        ShaderManager::instance()->pushShader(bg);
        bg->setUniform("u_projection", m_projection);
        setColorSpaceUniforms(bg, renderTarget, true);

        glActiveTexture(GL_TEXTURE0);
        GLTexture *backgroundSource = directImport ? sourceTexture : m_current->bgTexture.get();
        backgroundSource->bind();
        bg->setUniform("u_texture", 0);

        // 背景是本帧对该区域的首次写入，使用覆盖写。
        const GLboolean prevBlend = glIsEnabled(GL_BLEND);
        glDisable(GL_BLEND);

        // 四边形使用输出局部逻辑坐标，纹理坐标对应各导入矩形在完整输出中的位置。
        // 所有矩形合并到一次 VBO 提交，避免绘制调用数量随 Region 矩形数增长。
        //
        // 投影矩阵的 y 轴向下，而 OpenGL 纹理原点位于左下，因此输出顶边对应 v=1。
        // 背景导入和最终合成必须使用相同映射。
        std::vector<ParticleVertex> &quads = m_regionQuads;
        quads.clear();
        quads.reserve(std::size_t(importPixels.rects().size()) * 6);
        const QSize sourceSize = backgroundSource->size();
        const double scale = viewport.scale();
        const QMatrix4x4 sourceProjection = viewport.projectionMatrix();
        const auto backgroundUv = [&](int x, int y) {
            if (!directImport) {
                return QPointF(double(x) / sourceSize.width(), 1.0 - double(y) / sourceSize.height());
            }
            // 直接使用最终合成的投影和视口，将实际输出像素反向采回私有纹理。
            const QPointF p = pixelGrid.project(x, y);
            const QVector4D clip = sourceProjection * QVector4D(float(p.x()), float(p.y()), 0, 1);
            return QPointF(((clip.x() + 1.0) * .5 * m_savedViewport[2] + m_savedViewport[0]) / sourceSize.width(),
                           ((clip.y() + 1.0) * .5 * m_savedViewport[3] + m_savedViewport[1]) / sourceSize.height());
        };
        for (const Rect &pixels : importPixels.rects()) {
            const QPointF tl = pixelGrid.project(pixels.left(), pixels.top()) / scale - outRect.topLeft();
            const QPointF br = pixelGrid.project(pixels.right(), pixels.bottom()) / scale - outRect.topLeft();
            const float l = float(tl.x()), r = float(br.x());
            const float t = float(tl.y()), b = float(br.y());
            const QPointF uvLT = backgroundUv(pixels.left(), pixels.top());
            const QPointF uvRT = backgroundUv(pixels.right(), pixels.top());
            const QPointF uvLB = backgroundUv(pixels.left(), pixels.bottom());
            const QPointF uvRB = backgroundUv(pixels.right(), pixels.bottom());
            quads.insert(quads.end(), {
                {l, b, float(uvLB.x()), float(uvLB.y()), 1, 1, 1, 1},
                {r, b, float(uvRB.x()), float(uvRB.y()), 1, 1, 1, 1},
                {l, t, float(uvLT.x()), float(uvLT.y()), 1, 1, 1, 1},
                {r, b, float(uvRB.x()), float(uvRB.y()), 1, 1, 1, 1},
                {r, t, float(uvRT.x()), float(uvRT.y()), 1, 1, 1, 1},
                {l, t, float(uvLT.x()), float(uvLT.y()), 1, 1, 1, 1},
            });
        }
        drawVertices(m_shaderBackground, quads);

        ShaderManager::instance()->popShader();
        if (prevBlend) {
            glEnable(GL_BLEND);
        }
    }

    // 启用混合；各粒子图层在提交前设置其 Unity 材质对应的混合因子。
    //
    // 默认使用首个 Ring 图层的 One/OneMinusSrcAlpha 状态。
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    switchGpuTimerPhase(GpuTimerPhase::Particle);
    return true;
}

void GPURenderer::appendVertex(const ParticleVertex &vertex)
{
    m_vertices.push_back(vertex);
}

void GPURenderer::appendVertices(const std::vector<ParticleVertex> &vertices)
{
    m_vertices.insert(m_vertices.end(), vertices.begin(), vertices.end());
}

void GPURenderer::flushVertices(const ShaderSet &set, GLTexture *texture)
{
    if (m_vertices.empty() || !set) {
        m_vertices.clear();
        return;
    }

    GLShader *shader = set.get();
    ShaderManager::instance()->pushShader(shader);
    // 自定义着色器的统一变量不在 KWin 内建枚举中，需按名称查询位置。
    shader->setUniform("u_projection", m_projection);

    if (texture) {
        glActiveTexture(GL_TEXTURE0);
        texture->bind();
        shader->setUniform("u_texture", 0);
    }

    drawVertices(set, m_vertices);

    ShaderManager::instance()->popShader();
    m_vertices.clear();
}

void GPURenderer::drawVertices(const ShaderSet &set, const std::vector<ParticleVertex> &verts)
{
    if (verts.empty()) {
        return;
    }

    glBindVertexArray(set.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // 顶点数据每帧重填且只绘制一次，使用 GL_STREAM_DRAW。
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(ParticleVertex)),
                 verts.data(), GL_STREAM_DRAW);

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
}

void GPURenderer::setVertexLayout(const ShaderSet &set)
{
    // 调用前要求目标 ShaderSet 的 VAO 和 m_vbo 已绑定；glVertexAttribPointer 使用
    // 当前 ARRAY_BUFFER，并由 VAO 持久保存缓冲关联、格式和属性启用状态。
    const auto bind = [](int location, int components, std::size_t offset) {
        if (location < 0) {
            return; // 跳过已被驱动优化的属性。
        }
        const GLuint idx = static_cast<GLuint>(location);
        glEnableVertexAttribArray(idx);
        glVertexAttribPointer(idx, components, GL_FLOAT, GL_FALSE, sizeof(ParticleVertex),
                              reinterpret_cast<const void *>(offset));
    };

    bind(set.positionLocation, 2, offsetof(ParticleVertex, x));
    bind(set.texcoordLocation, 2, offsetof(ParticleVertex, u));
    bind(set.colorLocation, 4, offsetof(ParticleVertex, r));
    bind(set.custom0Location, 1, offsetof(ParticleVertex, custom0));
}

void GPURenderer::renderClickBase(const ClickInstance &inst, const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    const double cx = local.x();
    const double cy = local.y();

    renderRing(cx, cy, inst.ring);
}

void GPURenderer::renderClickAdditive(const ClickInstance &inst, const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    const double cx = local.x(), cy = local.y();
    renderTriBurstGeometry(cx, cy, inst.ring3, true);
}

void GPURenderer::renderClickMeshTri(const ClickInstance &inst,
                                     const baclickfx::CylinderProfile &mesh,
                                     const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    renderMeshTri(local.x(), local.y(), inst.meshTri, mesh);
}

void GPURenderer::renderTriBurst(const TriBurstInstance &inst, const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    renderTriBurstGeometry(local.x(), local.y(), inst.burst, false);
}

void GPURenderer::flushTriBursts()
{
    // Ring4 全部使用同一材质和 One/One 加法混合，可保持顶点顺序后一次性提交。
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    flushVertices(m_shaderAdditive, m_texTriangle.get());
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void GPURenderer::renderTrail(const std::vector<StrokeData> &strokes,
                              const baclickfx::Subsystem &params,
                              const QPointF &outputOrigin)
{
    if (strokes.empty()) {
        return;
    }

    const double hdrGain = std::max(1.0, params.hdrGain);

    // 原生带状网格包含端帽、转角及退化连接点，统一展开为三角形。
    // 不额外叠加转角扇面，否则加法混合会把连接处重复提亮。
    for (const StrokeData &stroke : strokes) {
        const auto &ribbon = stroke.ribbon;
        if (ribbon.size() < 3) continue;
        auto &vertices = m_trailStrip;
        vertices.clear();
        vertices.reserve((ribbon.size() - 2) * 3);
        const QPointF origin = stroke.ribbonOrigin - outputOrigin;
        const auto vertex = [&](std::size_t index) {
            const auto &v = ribbon[index];
            const auto &sample = stroke.samples[stroke.samples.size() - 1 - v.sample];
            return ParticleVertex{
                float(origin.x() + double(v.position[0]) * stroke.worldUnitPx),
                float(origin.y() - double(v.position[1]) * stroke.worldUnitPx), v.u, v.v,
                float(sample.color[0] * hdrGain), float(sample.color[1] * hdrGain),
                float(sample.color[2] * hdrGain), float(sample.alpha)};
        };
        ParticleVertex first = vertex(0);
        ParticleVertex second = vertex(1);
        for (std::size_t i = 2; i < ribbon.size(); ++i) {
            const ParticleVertex third = vertex(i);
            vertices.push_back(first);
            vertices.push_back(i % 2 ? third : second);
            vertices.push_back(i % 2 ? second : third);
            first = second;
            second = third;
        }
        appendVertices(vertices);
    }

    // 拖尾材质使用 BaTouchAdditive.shader 的 One/One 加法混合，RGB 和透明度使用
    // 相同因子。
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    flushVertices(m_shaderAdditive, m_texTrail.get());
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void GPURenderer::restoreBlendState()
{
    if (!m_blendStateCaptured) {
        return;
    }

    glBlendFuncSeparate(m_savedBlendSrcRgb, m_savedBlendDstRgb,
                        m_savedBlendSrcAlpha, m_savedBlendDstAlpha);
    glBlendEquationSeparate(m_savedBlendEquationRgb, m_savedBlendEquationAlpha);
    if (m_savedBlendEnabled) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    m_blendStateCaptured = false;

    glScissor(m_savedScissorBox[0], m_savedScissorBox[1],
              m_savedScissorBox[2], m_savedScissorBox[3]);
    if (m_savedScissorEnabled) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void GPURenderer::captureVertexState()
{
    Q_ASSERT(!m_vertexStateCaptured);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_savedVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_savedVbo);
    m_vertexStateCaptured = true;
}

void GPURenderer::restoreVertexState()
{
    if (!m_vertexStateCaptured) {
        return;
    }

    // 核心配置中 VAO 0 不能承载属性状态，因此必须恢复入口处的真实绑定。
    glBindVertexArray(static_cast<GLuint>(m_savedVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_savedVbo));
    m_vertexStateCaptured = false;
}

bool GPURenderer::renderBloom(const Region &globalChangedRegion)
{
    if (!m_current || m_current->bloomLevels.empty() || !m_shaderBloomPrefilter
        || !m_shaderBloomDownsample || !m_shaderBloomUpsample) {
        return false;
    }
    if (globalChangedRegion.isEmpty()) {
        return true;
    }

    std::vector<BloomLevel> &levels = m_current->bloomLevels;

    // 使用单位投影，并仅为受输入变化影响的区域生成四边形。Region 只剔除确定不会
    // 变化的目标纹理像素，不改变着色器、采样核或金字塔尺寸。
    QMatrix4x4 identity;

    // 辉光链各阶段均使用覆盖写。保存入口混合状态并在结束后恢复。
    const GLboolean prevBlend = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    // 每级帧缓冲尺寸不同，需显式设置视口，并在结束后恢复 KWin 的入口视口。
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    const auto blitTo = [&](const GLFramebuffer *dstFbo, const QSize &dstSize,
                            const ShaderSet &set, const Region &dstRegion) {
        if (dstRegion.isEmpty()) {
            return;
        }
        GLFramebuffer::pushFramebuffer(const_cast<GLFramebuffer *>(dstFbo));
        glViewport(0, 0, dstSize.width(), dstSize.height());
        textureRegionQuads(dstRegion, dstSize, m_regionQuads);
        drawVertices(set, m_regionQuads);
        GLFramebuffer::popFramebuffer();
    };

    // 预筛选和降采样的四点核向外扩展一个源纹理像素。上采样同时依赖低分辨率
    // 四点核和本级同位置像素，因此使用两条传播区域的并集。
    m_current->downRegions.resize(levels.size());
    m_current->upRegions.resize(levels.size());
    std::vector<Region> &downRegions = m_current->downRegions;
    std::vector<Region> &upRegions = m_current->upRegions;
    Region changedInHdr = mapGlobalRegionToTexture(globalChangedRegion,
                                                    m_current->textureRect,
                                                    m_current->devicePx, 0);
    downRegions[0] = mapTextureRegion(changedInHdr, m_current->devicePx,
                                      levels[0].size, 1);
    for (std::size_t i = 1; i < levels.size(); i++) {
        downRegions[i] = mapTextureRegion(downRegions[i - 1], levels[i - 1].size,
                                          levels[i].size, 1);
    }

    // ── 阶段 1：预筛选，HDR 全分辨率到第 0 级降采样目标 ───────────────────
    {
        GLShader *sh = m_shaderBloomPrefilter.get();
        ShaderManager::instance()->pushShader(sh);
        sh->setUniform("u_projection", identity);
        sh->setUniform("u_texture", 0);
        // 软膝曲线系数采用 PPv2 的 _Threshold 打包方式
        // （Bloom.cs：`new Vector4(lthresh, lthresh - knee, knee * 2f, 0.25f / knee)`）。
        sh->setUniform("u_filter", QVector4D(kBloomThreshold,
                                            kBloomThreshold - kBloomKnee,
                                            kBloomKnee * 2.0f,
                                            0.25f / kBloomKnee));
        sh->setUniform("u_clampMax", kBloomClampMax);
        // 预筛选阶段使用 HDR 源纹理的纹理像素尺寸执行四点采样。
        sh->setUniform("u_texelSize",
                       QVector2D(1.0f / m_current->devicePx.width(),
                                 1.0f / m_current->devicePx.height()));

        glActiveTexture(GL_TEXTURE0);
        m_current->hdrTexture->bind();
        blitTo(levels[0].downFbo.get(), levels[0].size,
               m_shaderBloomPrefilter, downRegions[0]);
        ShaderManager::instance()->popShader();
    }

    // ── 阶段 2：逐级降采样 ─────────────────────────────────────────────────
    if (levels.size() > 1) {
        GLShader *sh = m_shaderBloomDownsample.get();
        ShaderManager::instance()->pushShader(sh);
        sh->setUniform("u_projection", identity);
        sh->setUniform("u_texture", 0);

        for (std::size_t i = 0; i + 1 < levels.size(); i++) {
            const BloomLevel &src = levels[i];
            // 四点采样偏移使用源纹理的纹理像素尺寸。
            sh->setUniform("u_texelSize",
                           QVector2D(1.0f / src.size.width(), 1.0f / src.size.height()));
            glActiveTexture(GL_TEXTURE0);
            src.downTexture->bind();
            blitTo(levels[i + 1].downFbo.get(), levels[i + 1].size,
                   m_shaderBloomDownsample, downRegions[i + 1]);
        }
        ShaderManager::instance()->popShader();
    }

    // ── 阶段 3：逐级盒式上采样并累加 ───────────────────────────────────────
    //
    // 从最小级向上遍历，将低分辨率结果执行 UpsampleBox 后与当前级降采样结果相加，
    // 最终在第 0 级上采样目标中得到完整辉光。
    //
    // PPv2 Combine 使用直接相加，而不是 URP 的加权混合；该差异会影响辉光能量和
    // 外圈宽度，必须保留。
    //
    // 降采样和上采样使用独立纹理，避免在同一绘制中读写同一附件形成 OpenGL 反馈环。
    if (levels.size() > 1) {
        GLShader *sh = m_shaderBloomUpsample.get();
        ShaderManager::instance()->pushShader(sh);
        sh->setUniform("u_projection", identity);
        sh->setUniform("u_texture", 0);
        sh->setUniform("u_hiTexture", 1);
        sh->setUniform("u_sampleScale", m_current->bloomSampleScale);

        Region loChanged = downRegions.back();
        for (std::size_t i = levels.size() - 1; i > 0; i--) {
            const BloomLevel &lo = levels[i];
            const BloomLevel &hi = levels[i - 1];
            // 采样偏移使用被上采样的低分辨率纹理尺寸。
            sh->setUniform("u_texelSize",
                           QVector2D(1.0f / lo.size.width(), 1.0f / lo.size.height()));

            glActiveTexture(GL_TEXTURE0);
            // 最低级从 down 读取，其余级从前一次迭代生成的 up 读取，以形成累加链。
            if (i == levels.size() - 1) {
                lo.downTexture->bind();
            } else {
                lo.upTexture->bind();
            }
            glActiveTexture(GL_TEXTURE1);
            hi.downTexture->bind();

            Region affectedByLo = mapTextureRegion(loChanged, lo.size, hi.size,
                                                   int(std::ceil(m_current->bloomSampleScale)) + 1);
            upRegions[i - 1] = affectedByLo.united(downRegions[i - 1]);
            blitTo(hi.upFbo.get(), hi.size, m_shaderBloomUpsample, upRegions[i - 1]);
            loChanged = upRegions[i - 1];
        }
        ShaderManager::instance()->popShader();

        // 将活动纹理单元恢复为 0，满足后续合成和 KWin 绘制的状态约定。
        glActiveTexture(GL_TEXTURE0);
    }

    // 恢复入口视口和混合开关。
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    return true;
}

bool GPURenderer::updateBloom(const RenderTarget &renderTarget, const Region &requestedBloomSourceRegion)
{
    Q_ASSERT(m_current);
    endGpuTimerPhase();
    const Rect outputLogical = alignedLogicalRect(m_current->renderRect);
    // 辉光在最终合成前执行，并从当前与上一帧原始亮源区域的并集开始传播。当前区域
    // 更新新增或移动的亮源，上一帧区域清除消失的亮源；各级采样核在 renderBloom()
    // 内继续外扩。
    const Region currentBloomSource = requestedBloomSourceRegion.intersected(outputLogical);
    Region bloomUpdateRegion = currentBloomSource;
    bloomUpdateRegion += m_current->previousBloomSourceRegion.intersected(outputLogical);
    if (needsFullBloomRefresh(*renderTarget.colorDescription())) {
        // 色彩切换时重算整屏，清除局部亮源区域之外的旧辉光。
        bloomUpdateRegion = Region(outputLogical);
    }
    beginGpuTimerPhase(GpuTimerPhase::Bloom);
    if (m_current->bloomLevels.empty() && !bloomUpdateRegion.isEmpty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_current->bloomRetryAt && !prepareBloomPyramid(*m_current)) {
            // 保留本帧的粒子输出，活动期间最多每秒重试一次。
            m_current->bloomRetryAt = now + kBloomRetryDelay;
        }
    }
    const bool bloomOk = renderBloom(bloomUpdateRegion);
    m_current->previousBloomColor = renderTarget.colorDescription();
    m_current->previousBloomSourceRegion = currentBloomSource;
    switchGpuTimerPhase(GpuTimerPhase::Composite);
    return bloomOk;
}

void GPURenderer::endFrame(const RenderTarget &renderTarget, const RenderViewport &viewport,
                           const Region &requestedCompositeRegion,
                           bool bloomOk)
{
    Q_ASSERT(m_current);

    // 返回 KWin 输出帧缓冲。
    GLFramebuffer::popFramebuffer();
    // 私有 HDR 帧缓冲使用输出局部视口，返回 KWin 输出目标后恢复入口视口。
    glViewport(m_savedViewport[0], m_savedViewport[1],
               m_savedViewport[2], m_savedViewport[3]);

    // 合成区域使用全局逻辑坐标，先与当前输出求交；空区域可跳过合成。
    const QRectF &out = m_current->renderRect;
    const Rect outputLogical = alignedLogicalRect(out);
    const Region compositeRegion = requestedCompositeRegion.intersected(outputLogical);
    if (compositeRegion.isEmpty()) {
        // 每条返回路径都必须闭合计时查询，避免下一帧 glBeginQuery() 失败。
        finishGpuTimingFrame(false);
        restoreVertexState();
        restoreBlendState();
        return;
    }

    // 最终合成执行 main + bloom * intensity * color，并按输出传递函数编码。HDR
    // 场景纹理已包含桌面和全部粒子，因此使用覆盖写。

    GLShader *composite = m_shaderComposite.get();
    ShaderManager::instance()->pushShader(composite);
    composite->setUniform("u_projection", viewport.projectionMatrix());
    // 编码参数必须与 beginFrame() 的解码参数一致，以保持桌面颜色往返不变。
    setColorSpaceUniforms(composite, renderTarget, false);

    glActiveTexture(GL_TEXTURE0);
    m_current->hdrTexture->bind();
    composite->setUniform("u_hdrTexture", 0);

    // 辉光通常位于第 0 级 up；只有一级时位于 down。链路不可用时将强度设为 0，
    // 但仍绑定合法纹理，避免悬空采样器产生未定义行为。
    glActiveTexture(GL_TEXTURE1);
    QSizeF bloomTexel(1.0, 1.0);
    if (bloomOk) {
        const BloomLevel &top = m_current->bloomLevels[0];
        if (m_current->bloomLevels.size() > 1) {
            top.upTexture->bind();
        } else {
            top.downTexture->bind();
        }
        const QSize s = top.size;
        bloomTexel = QSizeF(1.0 / s.width(), 1.0 / s.height());
    } else {
        m_current->hdrTexture->bind();
    }
    composite->setUniform("u_bloomTexture", 1);
    composite->setUniform("u_bloomIntensity", bloomOk ? kBloomIntensity : 0.0f);
    // _Bloom_Color 与强度共同作用于辉光项。
    composite->setUniform("u_bloomColor",
                          QVector3D(kBloomColor[0], kBloomColor[1], kBloomColor[2]));
    composite->setUniform("u_bloomTexelSize",
                        QVector2D(float(bloomTexel.width()), float(bloomTexel.height())));
    composite->setUniform("u_sampleScale", m_current->bloomSampleScale);
    glActiveTexture(GL_TEXTURE0);

    // RenderViewport::projectionMatrix() 已经包含 OutputTransform，顶点保持缩放后的
    // 全局逻辑坐标即可。这里若再调用 mapToRenderTarget()，旋转会被应用两次，90°
    // 输出最终表现为特效位置与鼠标上下镜像。
    const OutputPixelGrid pixelGrid(viewport, m_current->devicePx);
    const Region compositePixels = pixelGrid.map(compositeRegion);
    const QSize size = m_current->devicePx;

    // 投影坐标 y 轴向下，私有帧缓冲纹理坐标 y 轴向上。
    std::vector<ParticleVertex> &quads = m_regionQuads;
    quads.clear();
    quads.reserve(std::size_t(compositePixels.rects().size()) * 6);
    for (const Rect &pixels : compositePixels.rects()) {
        const QPointF tl = pixelGrid.project(pixels.left(), pixels.top());
        const QPointF br = pixelGrid.project(pixels.right(), pixels.bottom());
        const float l = float(tl.x()), r = float(br.x());
        const float t = float(tl.y()), b = float(br.y());
        const float uL = float(pixels.left()) / size.width();
        const float uR = float(pixels.right()) / size.width();
        const float vT = 1.0f - float(pixels.top()) / size.height();
        const float vB = 1.0f - float(pixels.bottom()) / size.height();
        quads.insert(quads.end(), {
            {l, b, uL, vB, 1, 1, 1, 1},
            {r, b, uR, vB, 1, 1, 1, 1},
            {l, t, uL, vT, 1, 1, 1, 1},
            {r, b, uR, vB, 1, 1, 1, 1},
            {r, t, uR, vT, 1, 1, 1, 1},
            {l, t, uL, vT, 1, 1, 1, 1},
        });
    }

    // 场景纹理已经包含桌面和粒子，最终合成必须关闭混合并覆盖写入。调用方保证
    // compositeRegion 被 beginFrame() 的导入区域包含，避免写回未更新的场景内容。
    glDisable(GL_BLEND);

    // 最终合成复用同一 VAO/VBO 路径；未使用的顶点色属性会返回 -1 并被跳过。
    drawVertices(m_shaderComposite, quads);

    ShaderManager::instance()->popShader();

    m_current->previousCompositeRegion = compositeRegion;

    finishGpuTimingFrame(true);
    restoreVertexState();
    restoreBlendState();
    glViewport(m_savedViewport[0], m_savedViewport[1],
               m_savedViewport[2], m_savedViewport[3]);
}

// ===== 粒子几何生成 ==========================================================

void GPURenderer::renderRing(double cx, double cy, const RingEmission &sub)
{
    const double progress = particleProgress(sub.percentAge);
    if (progress >= 1.0) {
        return;
    }

    const double curve = baclickfx::subSizeCurve(sub.params, progress);
    const baclickfx::Rgba col = baclickfx::subColor(sub.params, progress);
    // SizeModule multiplier 已包含在 curve 中；Billboard 边长转半宽。
    const double radius = baclickfx::billboardHalfSize(sub.params, sub.startSize, float(curve), sub.percentAge);

    // Unity RGB 公式：tex.rgb * v_color.rgb * tex.r；顶点 Alpha 仅控制目标衰减。
    // _Color HDR = 2.0，已预乘进 v_color。
    const double hdrGain = sub.params.hdrGain;
    const float r = col.r * hdrGain;
    const float g = col.g * hdrGain;
    const float b = col.b * hdrGain;
    const float a = col.a;

    // 使用覆盖完整纹理坐标的正方形承载圆盘纹理，并绕中心应用起始旋转。
    const float cosR = std::cos(sub.startRotationRad);
    const float sinR = std::sin(sub.startRotationRad);
    const float hw = radius;  // 半宽。
    const float hh = radius;  // 半高。

    // 旋转前的四个局部顶点。
    const float corners[4][2] = {
        {-hw, -hh}, // 左下。
        { hw, -hh}, // 右下。
        {-hw,  hh}, // 左上。
        { hw,  hh}, // 右上。
    };
    const float uvs[4][2] = {
        {0, 1}, // 左下。
        {1, 1}, // 右下。
        {0, 0}, // 左上。
        {1, 0}, // 右上。
    };

    // 旋转并平移到圆心；两个三角形使用索引 (0,1,2) 和 (1,3,2)。
    static constexpr int kQuadIndices[6] = {0, 1, 2, 1, 3, 2};
    for (int i = 0; i < 6; i++) {
        const int idx = kQuadIndices[i];
        const float lx = corners[idx][0];
        const float ly = corners[idx][1];
        const float rx = lx * cosR - ly * sinR;
        const float ry = lx * sinR + ly * cosR;
        appendVertex({
            static_cast<float>(cx + rx), static_cast<float>(cy + ry),
            uvs[idx][0], uvs[idx][1],
            r, g, b, a
        });
    }
    flushVertices(m_shaderAlphablendAdd, m_texCircle.get());
}

void GPURenderer::renderMeshTri(double cx, double cy, const MeshTriEmission &sub,
                                const baclickfx::CylinderProfile &profile)
{
    const double progress = particleProgress(sub.percentAge);
    if (progress >= 1.0) {
        return;
    }

    // MeshTri 使用 FX_TEX_Grad_Ring3.png 和 Cylinder002.obj。直接使用 OBJ 面、三角
    // 剖分及逐角纹理坐标，以保留 Unity Mesh Particle Renderer 的接缝和插值行为。

    for (const MeshTriArc &item : sub.arcs) {
        const double localP = progress;
        const double curve = baclickfx::subSizeCurve(sub.params, localP);
        const baclickfx::Rgba col = baclickfx::subColor(sub.params, localP);

        const double baseRadius = sub.params.sizePx * item.radiusMul * std::max(0.0, curve);
        // custom0 为 Dissolve 的片元丢弃阈值：
        //   vertexColor.a * texAlpha < custom0  →  discard
        // Grad_Ring3 的透明度沿 u 呈钟形，因此溶解从两端同时推进。此处仅写入顶点
        // 数据，实际裁剪由 dissolve.frag 完成。
        const double custom0 = meshTriCustom0(sub, localP);
        // World 模拟空间的旋转角由 advanceMeshTri 逐帧推进。
        const double start = item.angle;

        // Unity 着色器公式：rgb = tex.rgb * Color_D20A2F66.rgb * vertexColor.rgb。
        // Color_D20A2F66 = 5.992157，预乘进顶点色。
        const double hdrGain = sub.params.hdrGain;

        // RGB 保留直通颜色；源透明度只由 Dissolve 输出和 SrcAlpha 混合应用一次。
        const float cr = static_cast<float>(col.r * hdrGain);
        const float cg = static_cast<float>(col.g * hdrGain);
        const float cb = static_cast<float>(col.b * hdrGain);
        const float ca = static_cast<float>(col.a);
        const float dissolve = static_cast<float>(custom0);
        const double cosR = std::cos(start), sinR = std::sin(start);

        for (std::size_t fi = 0; fi < profile.mesh.faces.size(); fi++) {
            const baclickfx::Face &face = profile.mesh.faces[fi];
            const std::size_t vertexStart = m_vertices.size();
            bool valid = true;
            for (int corner = 0; corner < 3; corner++) {
                const int vi = face[corner];
                if (vi < 0 || static_cast<std::size_t>(vi) >= profile.mesh.vertices.size()) {
                    valid = false;
                    break;
                }
                const baclickfx::Vertex &p = profile.mesh.vertices[static_cast<std::size_t>(vi)];
                const auto offset = baclickfx::unityObjPointOffset(p, baseRadius, cosR, sinR);
                const auto uv = profile.mesh.faceUv(fi, corner);
                if (!uv) {
                    valid = false;
                    break;
                }
                appendVertex({
                    static_cast<float>(cx + offset[0]),
                    static_cast<float>(cy + offset[1]),
                    static_cast<float>((*uv)[0]), static_cast<float>(1.0 - (*uv)[1]),
                    cr, cg, cb, ca, dissolve,
                });
            }
            if (!valid) {
                m_vertices.resize(vertexStart);
            }
        }
    }

    // Dissolve 的编译材质状态为 RGB 使用 SrcAlpha/One，透明度使用 One/One。
    // 输出透明度为 texAlpha * vertexColor.a，因此源透明度因子会保留纹理两端的
    // 钟形衰减。最终合成不将该透明度作为桌面覆盖度。
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
    flushVertices(m_shaderDissolve, m_texGradRing3.get());
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // 恢复帧内默认状态。
}

void GPURenderer::renderTriBurstGeometry(double cx, double cy, const TriBurstEmission &sub, bool flush)
{
    for (const TriParticle &item : sub.particles) {
        const double progress = particleProgress(item.percentAge);
        if (progress >= 1.0) {
            continue;
        }

        const double curve = baclickfx::subSizeCurve(sub.params, progress);
        const baclickfx::Rgba col = baclickfx::subColor(sub.params, progress);

        const QPointF offset = triParticleOffset(item);
        const double x = cx + offset.x();
        const double y = cy + offset.y();

        const double half = baclickfx::billboardHalfSize(sub.params, item.startSize,
            float(curve), item.percentAge, item.billboard);
        // PS17/PS18 的起始旋转为 0 且未启用 RotationModule，朝向仅由图集帧决定。
        const double rot = item.orientBase + item.spin * progress;

        // Unity 着色器公式：tex.rgb * v_color.rgb * v_color.a * tex.a。
        // _Color HDR = 5.992157。
        const double hdrGain = sub.params.hdrGain;
        const float r = static_cast<float>(col.r * hdrGain);
        const float g = static_cast<float>(col.g * hdrGain);
        const float b = static_cast<float>(col.b * hdrGain);
        const float a = static_cast<float>(col.a);

        // FX_TEX_Triangle_02_1.png 为 2×1 图集，两帧仅改变三角方向，不改变亮度。
        // frameOverTime 为常量，因此粒子生命周期内帧索引保持不变。
        const baclickfx::UvTile tile = baclickfx::uvFrameTile(sub.params, item.uvStartFrameNorm,
                                                          item.uvFrameOverTimeNorm);

        // ParticleSystemRenderer_20/21 使用 Billboard 模式且不绑定网格，因此几何为
        // 始终面向屏幕的正方形；三角形状和方向全部由图集纹理决定。
        const float cosR = static_cast<float>(std::cos(rot));
        const float sinR = static_cast<float>(std::sin(rot));

        // 投影坐标 y 轴向下，QImage 的首行对应 v=0，因此上侧顶点使用 v=0。
        const float corners[4][2] = {
            {-static_cast<float>(half), -static_cast<float>(half)}, // 屏幕左上。
            { static_cast<float>(half), -static_cast<float>(half)}, // 屏幕右上。
            {-static_cast<float>(half),  static_cast<float>(half)}, // 屏幕左下。
            { static_cast<float>(half),  static_cast<float>(half)}, // 屏幕右下。
        };
        const double uvCorners[4][2] = {
            {0.0, 0.0},
            {1.0, 0.0},
            {0.0, 1.0},
            {1.0, 1.0},
        };
        static constexpr int kQuadIndices[6] = {0, 1, 2, 1, 3, 2};

        for (int i = 0; i < 6; i++) {
            const int idx = kQuadIndices[i];
            const float lx = corners[idx][0];
            const float ly = corners[idx][1];
            appendVertex({
                static_cast<float>(x + lx * cosR - ly * sinR),
                static_cast<float>(y + lx * sinR + ly * cosR),
                static_cast<float>(tile.mapU(uvCorners[idx][0])),
                static_cast<float>(tile.mapV(uvCorners[idx][1])),
                r, g, b, a,
            });
        }
    }

    if (flush) {
        // Ring3 在队列 4499 完成后，以队列 4550 的纯加法材质提交。
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
        flushVertices(m_shaderAdditive, m_texTriangle.get());
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
    }
}

namespace
{
class RestoreEglContext
{
public:
    ~RestoreEglContext()
    {
        auto *current = EglContext::currentContext();
        if (current == m_previous) {
            return;
        }
        if (m_previous) {
            (void)m_previous->makeCurrent();
        } else if (current) {
            current->doneCurrent();
        }
    }

private:
    EglContext *const m_previous = EglContext::currentContext();
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    // 待释放的缓存也可能持有进入清理前最后一份上下文引用。
    const std::shared_ptr<EglContext> m_keepAlive = m_previous
        ? m_previous->weak_from_this().lock() : nullptr;
#endif
};
} // namespace

GpuRendererCache::~GpuRendererCache()
{
    releaseDevice(nullptr);
}

GpuRendererCache::Entry *GpuRendererCache::current(const RenderDevice *device)
{
    auto *context = EglContext::currentContext();
    if (!context) {
        return nullptr;
    }
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    if (context->isFailed()) {
        return nullptr;
    }
#endif
    for (const auto &entry : m_entries) {
        if (entry->context == context) {
            return entry.get();
        }
    }

    auto entry = std::make_unique<Entry>();
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
    entry->contextRef = context->weak_from_this().lock();
    if (!entry->contextRef) {
        return nullptr;
    }
    // KWin 可以在设备保留的情况下重建 EGL 上下文。回收旧资源后继续在新上下文绘制。
    releaseEntries(nullptr, true);
    if (EglContext::currentContext() != context) {
        return nullptr;
    }
#else
    // 旧版沿用合成器持有的单个上下文，生命周期覆盖效果实例。
    if (!m_entries.empty()) {
        return nullptr;
    }
#endif
    entry->context = context;
    entry->device = device;
    entry->renderer.setLogLevel(m_logLevel);
    entry->renderer.setProfiling(m_profiling);
    entry->ready = entry->renderer.initialize();
    if (!entry->ready) {
        qCWarning(KWIN_BA_CLICK_FX_RENDERER)
            << "event=error component=effect code=gpu_renderer_init_failed"
            << "action=rendering_disabled_for_context";
    }
    return m_entries.emplace_back(std::move(entry)).get();
}

void GpuRendererCache::configure(baclickfx::defaults::LogLevel level, bool profiling)
{
    m_logLevel = level;
    m_profiling = profiling;
    for (const auto &entry : m_entries) {
        entry->renderer.setLogLevel(level);
        entry->renderer.setProfiling(profiling);
        entry->lastGpuSampleSerial = entry->renderer.gpuSampleSerial();
    }
}

void GpuRendererCache::invalidateOutput(const LogicalOutput *output)
{
    const RestoreEglContext restore;
    for (const auto &entry : m_entries) {
        if (entry->renderer.invalidateOutput(output)
            && (entry->context == EglContext::currentContext() || entry->context->makeCurrent())) {
            entry->renderer.releaseInvalidatedTargets();
        }
    }
}

void GpuRendererCache::releaseDevice(const RenderDevice *device)
{
    releaseEntries(device, false);
}

void GpuRendererCache::releaseEntries(const RenderDevice *device, [[maybe_unused]] bool failedOnly)
{
    const RestoreEglContext restore;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if ((device && (*it)->device != device)
#ifndef BA_CLICK_FX_KWIN_LEGACY_PAINTSCREEN
            || (failedOnly && !(*it)->context->isFailed())
#endif
        ) {
            ++it;
            continue;
        }
        if ((*it)->context != EglContext::currentContext() && !(*it)->context->makeCurrent()) {
            // GL 名称可能在其他上下文中重用，失败时也不能让析构访问那个上下文。
            if (auto *current = EglContext::currentContext()) {
                current->doneCurrent();
            }
        }
        it = m_entries.erase(it);
    }
}

bool GpuRendererCache::ready() const
{
    return std::ranges::any_of(m_entries, [](const auto &entry) { return entry->ready; });
}

QString GpuRendererCache::diagnosticStatus() const
{
    QStringList summaries;
    for (const auto &entry : m_entries) {
        summaries.append(entry->renderer.diagnosticStatus());
    }
    return summaries.join(QStringLiteral(" | "));
}

} // namespace KWin
