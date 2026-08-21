// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpurenderer.h"
#include "baclickfxdefaults.h"
#include "clickinstance.h"
#include "curveutils.h"
#include "meshprofiles.h"
#include "subsystems.h"
#include "trailstream.h"
#include "bloomutils.h"

#include <core/rendertarget.h>
#include <core/renderviewport.h>
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
constexpr float kBloomIntensity = 0.12503f;
// Clamp 为 65472。PPv2 在传入着色器前调用 GammaToLinearSpace：
//     pow(65472, 2.2) ≈ 3.9385e10
// 当前配置下首先生效的是半精度浮点上限 65504；保留独立 Clamp 值以维持 PPv2 的
// 参数语义。
//
constexpr float kBloomClampMax = 3.9385e10f;
// Color 为白色，但仍显式参与辉光染色公式。
constexpr float kBloomColor[3] = {1.0f, 1.0f, 1.0f};
// 金字塔最小边长取 2；宽度为 1 时四点采样会退化为同一纹理像素，不再增加信息。
constexpr int kBloomMinSize = 2;

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
    // KWin 在有效 OpenGL 上下文中卸载效果，可在此直接释放自有 OpenGL 对象。
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
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

    // 使用私有 VAO/VBO；顶点属性位置随着色器变化，在每次绘制前重新配置。
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    if (m_vao == 0 || m_vbo == 0) {
        GPU_ERROR() << "code=buffer_init_failed";
        return false;
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

    // Unity 在线性颜色空间中采样 sRGB 纹理。显式分配 GL_SRGB8_ALPHA8，使 GPU
    // 采样时自动解码；统一转换为 RGBA8888，保证 glTexSubImage2D 的通道排列一致。
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    auto tex = GLTexture::allocate(GL_SRGB8_ALPHA8, rgba.size(), 1);
    if (!tex || tex->isNull()) {
        GPU_ERROR() << "code=texture_alloc_failed" << "path=" << path;
        return nullptr;
    }
    tex->update(rgba, Region(0, 0, rgba.width(), rgba.height()));

    // Unity .meta 中 filterMode=1 且 enableMipMap=0，对应线性过滤且不生成多级纹理。
    tex->setFilter(GL_LINEAR);
    tex->setWrapMode(wrapMode);
    return tex;
}

bool GPURenderer::prepareTargets(const RenderViewport &viewport)
{
    // 显式按分量转换 KWin::RectF，后续几何计算统一使用 QRectF 的边界语义。
    const RectF rr = viewport.renderRect();
    const QRectF outputRect(rr.x(), rr.y(), rr.width(), rr.height());
    const double scale = viewport.scale();

    // 使用 KWin 已缩放并取整的 scaledRenderRect()，确保纹理尺寸与投影矩阵一致。
    const Rect sr = viewport.scaledRenderRect();
    const QSize devicePx(sr.width(), sr.height());
    const QSize renderTargetDevicePx = viewport.deviceSize();
    const int transformKind = int(viewport.transform().kind());
    if (devicePx.isEmpty()) {
        return false;
    }

    // 输出数量有限，线性查找可避免额外缓存结构。
    for (auto it = m_outputs.begin(); it != m_outputs.end(); ++it) {
        const auto &t = *it;
        if (t->renderRect == outputRect && qFuzzyCompare(t->scale, scale)
            && t->renderTargetDevicePx == renderTargetDevicePx
            && t->transformKind == transformKind) {
            m_current = t.get();
            // 队尾为最近使用项。unique_ptr 移动不改变 OutputTargets 的对象地址。
            if (std::next(it) != m_outputs.end()) {
                auto hit = std::move(*it);
                m_outputs.erase(it);
                m_outputs.push_back(std::move(hit));
            }
            return true;
        }
    }

    // 限制逐输出缓存数量，避免分辨率或缩放反复变化时保留过多陈旧资源。
    constexpr std::size_t kMaxCachedOutputs = 4;
    if (m_outputs.size() >= kMaxCachedOutputs) {
        m_outputs.erase(m_outputs.begin());
    }

    auto targets = std::make_unique<OutputTargets>();
    targets->renderRect = outputRect;
    targets->scale = scale;
    targets->devicePx = devicePx;
    targets->renderTargetDevicePx = renderTargetDevicePx;
    targets->transformKind = transformKind;

    // 创建线性 RGBA16F 场景纹理及其帧缓冲。
    targets->hdrTexture = GLTexture::allocate(GL_RGBA16F, devicePx, 1);
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

    // 辉光金字塔分配失败时仅关闭辉光，保留粒子渲染。
    if (!prepareBloomPyramid(*targets, devicePx)) {
        targets->bloomLevels.clear();
    }

    // OpenGL 新分配纹理的内容未定义，而局部重绘和辉光采样可能读取尚未写入的边缘。
    // 创建资源时完整清零一次，避免未初始化显存进入辉光链。glClearBufferfv 不改变
    // 清除颜色或颜色掩码；临时关闭裁剪以覆盖整个附件。
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissorWasEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }
    static constexpr GLfloat clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto clearFbo = [](GLFramebuffer *fbo) {
        GLFramebuffer::pushFramebuffer(fbo);
        glClearBufferfv(GL_COLOR, 0, clearColor);
        GLFramebuffer::popFramebuffer();
    };
    clearFbo(targets->hdrFbo.get());
    if (targets->bgFbo) {
        clearFbo(targets->bgFbo.get());
    }
    for (BloomLevel &level : targets->bloomLevels) {
        clearFbo(level.downFbo.get());
        clearFbo(level.upFbo.get());
    }
    if (scissorWasEnabled) {
        glEnable(GL_SCISSOR_TEST);
    }

    if (m_logVerbose) {
        qCInfo(KWIN_BA_CLICK_FX_RENDERER)
            << "event=targets_created size=" << devicePx << " scale=" << scale
            << " bloom_levels=" << targets->bloomLevels.size();
    }

    m_outputs.push_back(std::move(targets));
    m_current = m_outputs.back().get();
    return true;
}

double GPURenderer::bloomReachPx(const QSize &devicePx, double scale)
{
    return baclickfx::bloomReachPx(devicePx, scale);
}

bool GPURenderer::prepareBloomPyramid(OutputTargets &targets, const QSize &devicePx)
{
    targets.bloomLevels.clear();

    // 级数和上采样尺度由第 0 级尺寸及 Diffusion 按 PPv2 公式确定。
    QSize s = baclickfx::bloomBaseSize(devicePx);
    const baclickfx::BloomPyramidParams params = baclickfx::bloomPyramidParams(s);
    targets.bloomSampleScale = params.sampleScale;

    // 各级尺寸逐次减半；kBloomMinSize 作为极小输出的保护条件。
    for (int i = 0; i < params.iterations; i++) {
        if (s.width() < kBloomMinSize || s.height() < kBloomMinSize) {
            break;
        }

        BloomLevel level;
        level.size = s;

        // down 保存降采样结果，up 保存上采样结果。分离读写目标可避免 OpenGL
        // 反馈环；该结构与 PPv2 的 m_Pyramid[i].down/up 一致。
        const auto makeLevel = [&s](std::unique_ptr<GLTexture> &tex,
                                    std::unique_ptr<GLFramebuffer> &fbo) {
            tex = GLTexture::allocate(GL_RGBA16F, s, 1);
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

        targets.bloomLevels.push_back(std::move(level));
        s = QSize(s.width() / 2, s.height() / 2);
    }

    return !targets.bloomLevels.empty();
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
            m_gpuSampleSerial++;
        }

        frame.pending = false;
        frame.publishable = false;
        frame.issuedPhases = 0;
    }
}

void GPURenderer::beginGpuTimingFrame()
{
    if (!m_profiling) {
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

void GPURenderer::setTransferFunctionUniforms(GLShader *shader, const RenderTarget &renderTarget)
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
    glGetIntegerv(GL_VIEWPORT, m_savedViewport);
    m_savedScissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, m_savedScissorBox);
    m_blendStateCaptured = true;

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

    // SDR 桌面像素不会有效超过辉光阈值，可仅导入调用方请求的区域。HDR 输出允许
    // 桌面高光超过参考白，辉光预筛选会读取这些像素，因此必须导入完整输出。
    const ColorDescription &cd = *renderTarget.colorDescription();
    const bool hasHdrHeadroom =
        cd.transferFunction().maxLuminance > cd.referenceLuminance() * 1.0001;
    Region importRegion = hasHdrHeadroom
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

    if (!directImport && !m_current->bgTexture) {
        m_current->bgTexture = GLTexture::allocate(GL_RGBA16F, m_current->devicePx, 1);
        if (!m_current->bgTexture || m_current->bgTexture->isNull()) {
            GPU_ERROR() << "code=background_texture_alloc_failed" << m_current->devicePx;
            finishGpuTimingFrame(false);
            restoreBlendState();
            return false;
        }
        m_current->bgTexture->setFilter(GL_LINEAR);
        m_current->bgTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_current->bgFbo = std::make_unique<GLFramebuffer>(m_current->bgTexture.get());
        if (!m_current->bgFbo->valid()) {
            GPU_ERROR() << "code=background_fbo_failed";
            finishGpuTimingFrame(false);
            restoreBlendState();
            return false;
        }
    }

    if (!directImport) {
        // 无可采样纹理时保留 framebuffer blit fallback。复制必须发生在绑定私有 HDR
        // 帧缓冲之前，避免从错误的 framebuffer 读取。
        QList<Rect> importedRects;
        importedRects.reserve(importRegion.rects().size());
        for (const Rect &logicalRect : importRegion.rects()) {
            const QRect aligned = QRect(logicalRect.x(), logicalRect.y(),
                                        logicalRect.width(), logicalRect.height())
                                      .intersected(outRect.toAlignedRect());
            if (aligned.isEmpty()) {
                continue;
            }
            const Rect srcLogical(aligned.x(), aligned.y(), aligned.width(), aligned.height());
            const Rect scaledSource = srcLogical.scaled(viewport.scale()).rounded();
            const Rect dstDevice = scaledSource.translated(-viewport.scaledRenderRect().topLeft());
            if (!m_current->bgFbo->blitFromRenderTarget(renderTarget, viewport,
                                                        srcLogical, dstDevice)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    GPU_ERROR() << "code=desktop_import_failed"
                                << "无法取得桌面像素，插件不会绘制";
                }
                finishGpuTimingFrame(false);
                restoreBlendState();
                glViewport(m_savedViewport[0], m_savedViewport[1],
                           m_savedViewport[2], m_savedViewport[3]);
                return false;
            }
            importedRects.append(srcLogical);
        }
        importRegion = Region::fromUnsortedRects(importedRects);
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
    const RectF rr = viewport.renderRect();
    m_projection.setToIdentity();
    m_projection.ortho(0.0f, static_cast<float>(rr.width()),
                       static_cast<float>(rr.height()), 0.0f,
                       -1.0f, 1.0f);

    // ── 第 2 步：将背景解码到线性 HDR 场景纹理 ─────────────────────────────
    //
    // SDR 输出仅解码 importRegion，以使带宽开销随重绘区域变化。合成区域始终包含在
    // 导入区域内，旧粒子区域也已由当前桌面覆盖；HDR 输出则在前面扩展为完整输出。
    if (!importRegion.isEmpty()) {
        GLShader *bg = m_shaderBackground.get();
        ShaderManager::instance()->pushShader(bg);
        bg->setUniform("u_projection", m_projection);
        setTransferFunctionUniforms(bg, renderTarget);

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
        quads.reserve(std::size_t(importRegion.rects().size()) * 6);
        const QSize sourceSize = backgroundSource->size();
        const auto directUv = [&](const QPointF &logical) {
            // 旋转输出的实际 RenderTarget 可能交换宽高。先映射到实际目标，
            // 再显式转换为 OpenGL 左下原点 UV，避免同时叠加 KWin 变换和纹理矩阵。
            const QPointF p = viewport.mapToRenderTarget(logical);
            return QPointF(p.x() / sourceSize.width(),
                           1.0 - p.y() / sourceSize.height());
        };
        for (const Rect &rect : importRegion.rects()) {
            const float l = static_cast<float>(rect.left() - outRect.left());
            const float r = static_cast<float>(rect.right() - outRect.left());
            const float t = static_cast<float>(rect.top() - outRect.top());
            const float b = static_cast<float>(rect.bottom() - outRect.top());
            const QPointF uvLT = directImport
                ? directUv(QPointF(rect.left(), rect.top()))
                : QPointF(l / outRect.width(), 1.0 - t / outRect.height());
            const QPointF uvRT = directImport
                ? directUv(QPointF(rect.right(), rect.top()))
                : QPointF(r / outRect.width(), 1.0 - t / outRect.height());
            const QPointF uvLB = directImport
                ? directUv(QPointF(rect.left(), rect.bottom()))
                : QPointF(l / outRect.width(), 1.0 - b / outRect.height());
            const QPointF uvRB = directImport
                ? directUv(QPointF(rect.right(), rect.bottom()))
                : QPointF(r / outRect.width(), 1.0 - b / outRect.height());
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

    // 保存并恢复 KWin 原有 VAO/VBO。核心配置文件中 VAO 0 不能承载顶点属性，
    // 因此不能以绑定 VAO 0 代替状态恢复。
    GLint prevVao = 0;
    GLint prevVbo = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevVbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    // 顶点数据每帧重填且只绘制一次，使用 GL_STREAM_DRAW。
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(ParticleVertex)),
                 verts.data(), GL_STREAM_DRAW);

    setVertexLayout(set);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));

    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevVbo));
}

void GPURenderer::setVertexLayout(const ShaderSet &set)
{
    // 调用前要求 m_vao 和 m_vbo 已绑定；glVertexAttribPointer 使用当前 ARRAY_BUFFER。
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
    const double ageSec = inst.age;
    const QPointF local = inst.center - outputOrigin;
    const double cx = local.x();
    const double cy = local.y();

    // 两个图层的材质队列均为 3000，同队列内保持 Prefab 子对象顺序。
    renderRing(cx, cy, inst.ring, ageSec);
    renderTriBurstGeometry(cx, cy, inst.ring3, ageSec);
}

void GPURenderer::renderClickMeshTri(const ClickInstance &inst,
                                     const baclickfx::CylinderProfile &mesh,
                                     const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    renderMeshTri(local.x(), local.y(), inst.meshTri, inst.age, mesh);
}

void GPURenderer::renderTriBurst(const TriBurstInstance &inst, const QPointF &outputOrigin)
{
    const QPointF local = inst.center - outputOrigin;
    renderTriBurstGeometry(local.x(), local.y(), inst.burst, inst.age);
}

void GPURenderer::renderTrail(const TrailStream &trail, const baclickfx::Subsystem &params,
                              const QPointF &outputOrigin)
{
    if (trail.empty()) {
        return;
    }

    const double hdrGain = std::max(1.0, params.hdrGain);

    // TrailRenderer.colorGradient 沿轨迹求值：t=0 为笔头，t=1 为笔尾。

    buildTrailStrokes(trail, params, outputOrigin, m_trailStrokes);
    for (const StrokeData &stroke : m_trailStrokes) {
        const std::vector<StrokeSample> &s = stroke.samples;
        const std::size_t n = s.size();

        // 使用角平分法线构造主带；转角外侧由 numCornerVertices 扇面补齐，且仅与
        // 主带共边，避免加法混合区域重叠。
        m_trailNormals.resize(n);
        m_trailDirs.resize(n > 1 ? n - 1 : 0);
        std::vector<QPointF> &normals = m_trailNormals;
        std::vector<QPointF> &dirs = m_trailDirs;
        QPointF lastDir(1, 0);
        for (std::size_t i = 0; i + 1 < n; i++) {
            const double dx = s[i + 1].pos.x() - s[i].pos.x();
            const double dy = s[i + 1].pos.y() - s[i].pos.y();
            const double len = std::hypot(dx, dy);
            if (len > 1e-6) {
                lastDir = QPointF(dx / len, dy / len);
            }
            dirs[i] = lastDir;
        }
        for (std::size_t i = 0; i < n; i++) {
            const QPointF dIn = dirs.empty() ? QPointF(1, 0) : dirs[i == 0 ? 0 : i - 1];
            const QPointF dOut = dirs.empty() ? QPointF(1, 0) : dirs[std::min(dirs.size() - 1, i)];
            const double ax = -dIn.y() - dOut.y();
            const double ay = dIn.x() + dOut.x();
            const double inv = 1.0 / std::max(1e-6, std::hypot(ax, ay));
            normals[i] = QPointF(ax * inv, ay * inv);
        }

        // 每段由两个三角形组成。顶点色沿轨迹渐变，HDR 倍率已预乘到 RGB。
        std::vector<ParticleVertex> &strip = m_trailStrip;
        strip.clear();
        strip.reserve((n - 1) * 6);

        const auto vertexAtOffset = [&](std::size_t i, const QPointF &offset,
                                        double textureV) {
            // 宽度使用 Unity 参数的直接换算值：
            // widthMultiplier 0.005 × widthCurve(恒 1.0) × kUnitySizeToPx = 0.9 逻辑像素，
            // TrailRenderer_13 的 Transform_12 localScale 也是 1.0，不存在其他缩放因子。
            const double half = s[i].width * 0.5;
            const double alpha = evalTrailGradientAlpha(params, s[i].headT);
            // 颜色由 Unity 渐变从笔头亮蓝过渡到笔尾黑色，透明度沿全长保持为 1。
            const baclickfx::Rgb rgb = evalTrailGradientColor(params, s[i].headT);
            return ParticleVertex{
                static_cast<float>(s[i].pos.x() + offset.x() * half),
                static_cast<float>(s[i].pos.y() + offset.y() * half),
                // u 沿轨迹长度，v 横跨拖尾宽度；纹理自身提供横向软边。
                static_cast<float>(s[i].textureU), static_cast<float>(textureV),
                static_cast<float>(rgb[0] * hdrGain),
                static_cast<float>(rgb[1] * hdrGain),
                static_cast<float>(rgb[2] * hdrGain),
                static_cast<float>(alpha),
            };
        };

        const auto vertexAtNormal = [&](std::size_t i, const QPointF &normal, double side) {
            return vertexAtOffset(i, normal * side, side > 0 ? 0.0 : 1.0);
        };

        const auto vertexAt = [&](std::size_t i, double side) {
            return vertexAtNormal(i, normals[i], side);
        };

        for (std::size_t i = 0; i + 1 < n; i++) {
            const ParticleVertex a = vertexAt(i, 1.0);
            const ParticleVertex b = vertexAt(i, -1.0);
            const ParticleVertex c = vertexAt(i + 1, 1.0);
            const ParticleVertex d = vertexAt(i + 1, -1.0);
            strip.push_back(a);
            strip.push_back(b);
            strip.push_back(c);
            strip.push_back(b);
            strip.push_back(d);
            strip.push_back(c);
        }

        // TrailRenderer_13 的 numCornerVertices 为 4。转角外侧使用以角平分外点
        // 为锚的三角扇补齐，边界仅与主带共边。
        const int cornerVertices = std::max(0, params.numCornerVertices);
        for (std::size_t i = 1; i + 1 < n && cornerVertices > 0; i++) {
            const QPointF dIn = dirs[i - 1];
            const QPointF dOut = dirs[i];
            const QPointF nIn(-dIn.y(), dIn.x());
            const QPointF nOut(-dOut.y(), dOut.x());
            const double cross = dIn.x() * dOut.y() - dIn.y() * dOut.x();
            if (std::abs(cross) < 1e-6) {
                continue;
            }
            const double side = cross > 0.0 ? -1.0 : 1.0;
            const double theta = std::atan2(nIn.x() * nOut.y() - nIn.y() * nOut.x(),
                                            nIn.x() * nOut.x() + nIn.y() * nOut.y());
            const ParticleVertex anchor = vertexAt(i, side);
            QPointF prev = nIn;
            for (int j = 1; j <= cornerVertices + 1; j++) {
                const double a = theta * (double(j) / (cornerVertices + 1));
                const double ca = std::cos(a);
                const double sa = std::sin(a);
                const QPointF next(nIn.x() * ca - nIn.y() * sa,
                                   nIn.x() * sa + nIn.y() * ca);
                strip.push_back(anchor);
                strip.push_back(vertexAtNormal(i, prev, side));
                strip.push_back(vertexAtNormal(i, next, side));
                prev = next;
            }
        }

        // numCapVertices 为 1：首尾分别沿反向和正向切线生成半圆端帽。
        const int capVertices = std::max(0, params.numCapVertices);
        if (capVertices > 0 && !dirs.empty()) {
            const auto addCap = [&](std::size_t i, const QPointF &dir, bool startCap) {
                const QPointF normal(-dir.y(), dir.x());
                const double baseAngle = std::atan2(normal.y(), normal.x());
                const double sweep = startCap ? M_PI : -M_PI;
                const ParticleVertex center = vertexAtOffset(i, QPointF(0, 0), 0.5);
                QPointF previous = normal;
                for (int j = 1; j <= capVertices + 1; j++) {
                    const double angle = baseAngle
                        + sweep * (static_cast<double>(j) / (capVertices + 1));
                    const QPointF next(std::cos(angle), std::sin(angle));
                    const double prevV = 0.5 - 0.5
                        * (previous.x() * normal.x() + previous.y() * normal.y());
                    const double nextV = 0.5 - 0.5
                        * (next.x() * normal.x() + next.y() * normal.y());
                    strip.push_back(center);
                    strip.push_back(vertexAtOffset(i, previous, prevV));
                    strip.push_back(vertexAtOffset(i, next, nextV));
                    previous = next;
                }
            };
            addCap(0, dirs.front(), true);
            addCap(n - 1, dirs.back(), false);
        }

        appendVertices(strip);
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

bool GPURenderer::renderBloom(const Region &globalChangedRegion)
{
    if (!m_current || m_current->bloomLevels.empty() || !m_shaderBloomPrefilter
        || !m_shaderBloomDownsample || !m_shaderBloomUpsample) {
        return false;
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
                                                    m_current->renderRect,
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

void GPURenderer::endFrame(const RenderTarget &renderTarget, const RenderViewport &viewport,
                           const Region &requestedCompositeRegion,
                           const Region &requestedBloomSourceRegion)
{
    Q_ASSERT(m_current);

    // 粒子提交完成后闭合粒子阶段；辉光查询在区域运算完成且即将提交时开始。
    endGpuTimerPhase();

    // 返回 KWin 输出帧缓冲。
    GLFramebuffer::popFramebuffer();
    // 私有 HDR 帧缓冲使用输出局部视口，返回 KWin 输出目标后恢复入口视口。
    glViewport(m_savedViewport[0], m_savedViewport[1],
               m_savedViewport[2], m_savedViewport[3]);

    // 合成区域使用全局逻辑坐标，先与当前输出求交；空区域可跳过整条辉光和合成链。
    const QRectF &out = m_current->renderRect;
    const Rect outputLogical = alignedLogicalRect(out);
    const Region compositeRegion = requestedCompositeRegion.intersected(outputLogical);
    if (compositeRegion.isEmpty()) {
        // 每条返回路径都必须闭合计时查询，避免下一帧 glBeginQuery() 失败。
        finishGpuTimingFrame(false);
        restoreBlendState();
        return;
    }

    // 辉光在最终合成前执行，并从当前与上一帧原始亮源区域的并集开始传播。当前区域
    // 更新新增或移动的亮源，上一帧区域清除消失的亮源；各级采样核在 renderBloom()
    // 内继续外扩。
    const Region currentBloomSource = requestedBloomSourceRegion.intersected(outputLogical);
    Region bloomUpdateRegion = currentBloomSource;
    bloomUpdateRegion += m_current->previousBloomSourceRegion.intersected(outputLogical);
    const ColorDescription &cd = *renderTarget.colorDescription();
    const bool hasHdrHeadroom =
        cd.transferFunction().maxLuminance > cd.referenceLuminance() * 1.0001;
    if (hasHdrHeadroom) {
        // HDR 桌面本身可超过辉光阈值，因此金字塔需与背景导入一样更新完整输出。
        bloomUpdateRegion = Region(outputLogical);
    }
    beginGpuTimerPhase(GpuTimerPhase::Bloom);
    const bool bloomOk = renderBloom(bloomUpdateRegion);
    switchGpuTimerPhase(GpuTimerPhase::Composite);

    // 最终合成执行 main + bloom * intensity * color，并按输出传递函数编码。HDR
    // 场景纹理已包含桌面和全部粒子，因此使用覆盖写。

    GLShader *composite = m_shaderComposite.get();
    ShaderManager::instance()->pushShader(composite);
    composite->setUniform("u_projection", viewport.projectionMatrix());
    // 编码参数必须与 beginFrame() 的解码参数一致，以保持桌面颜色往返不变。
    setTransferFunctionUniforms(composite, renderTarget);

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
    const float scale = static_cast<float>(viewport.scale());

    // 投影坐标 y 轴向下，而私有帧缓冲纹理坐标 y 轴向上，因此逻辑顶边对应 v=1。
    std::vector<ParticleVertex> &quads = m_regionQuads;
    quads.clear();
    quads.reserve(std::size_t(compositeRegion.rects().size()) * 6);
    for (const Rect &area : compositeRegion.rects()) {
        const float l = static_cast<float>(area.left()) * scale;
        const float r = static_cast<float>(area.right()) * scale;
        const float t = static_cast<float>(area.top()) * scale;
        const float b = static_cast<float>(area.bottom()) * scale;
        const float uL = static_cast<float>((area.left() - out.left()) / out.width());
        const float uR = static_cast<float>((area.right() - out.left()) / out.width());
        const float vT = static_cast<float>(1.0 - (area.top() - out.top()) / out.height());
        const float vB = static_cast<float>(1.0 - (area.bottom() - out.top()) / out.height());
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
    m_current->previousBloomSourceRegion = currentBloomSource;

    finishGpuTimingFrame(true);
    restoreBlendState();
    glViewport(m_savedViewport[0], m_savedViewport[1],
               m_savedViewport[2], m_savedViewport[3]);
}

// ===== 粒子几何生成 ==========================================================

void GPURenderer::renderRing(double cx, double cy, const RingEmission &sub, double ageSec)
{
    const double progress = layerProgress(sub.durationSec, ageSec);
    if (progress >= 1.0) {
        return;
    }

    const double curve = baclickfx::subSizeCurve(sub.params, progress);
    const baclickfx::Rgba col = baclickfx::subColor(sub.params, progress);
    // SizeModule multiplier=2 与 Billboard 边长到半宽的除 2 相抵。
    const double radius = sub.params.sizePx * std::max(0.0, curve);

    // Unity 着色器公式：tex.rgb * v_color.rgb * v_color.a。
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
                                double ageSec, const baclickfx::CylinderProfile &profile)
{
    const double progress = layerProgress(sub.durationSec, ageSec);
    if (progress >= 1.0) {
        return;
    }

    // MeshTri 使用 FX_TEX_Grad_Ring3.png 和 Cylinder002.obj。直接使用 OBJ 面、三角
    // 剖分及逐角纹理坐标，以保留 Unity Mesh Particle Renderer 的接缝和插值行为。

    for (const MeshTriArc &item : sub.arcs) {
        const double localP = baclickfx::clamp(progress + item.phase * 0.08, 0.0, 1.0);
        const double curve = baclickfx::subSizeCurve(sub.params, localP);
        const baclickfx::Rgba col = baclickfx::subColor(sub.params, localP);

        const double baseRadius = sub.params.sizePx * item.radiusMul * std::max(0.0, curve);
        // custom0 为 Dissolve 的片元丢弃阈值：
        //   vertexColor.a * texAlpha < custom0  →  discard
        // Grad_Ring3 的透明度沿 u 呈钟形，因此溶解从两端同时推进。此处仅写入顶点
        // 数据，实际裁剪由 dissolve.frag 完成。
        const double custom0 = meshTriCustom0(sub, localP);
        // 旋转角由角速度曲线的分段埃尔米特解析积分得到。
        const double start = item.angle + meshTriRotation(sub, localP, item.rotMix);

        // Unity 着色器公式：rgb = tex.rgb * Color_D20A2F66.rgb * vertexColor.rgb。
        // Color_D20A2F66 = 5.992157，预乘进顶点色。
        const double hdrGain = sub.params.hdrGain;

        // MeshTri 的透明度曲线恒为 1，消退完全由着色器丢弃阈值控制。
        const double energyBase = col.a;

        const float cr = static_cast<float>(col.r * hdrGain * energyBase);
        const float cg = static_cast<float>(col.g * hdrGain * energyBase);
        const float cb = static_cast<float>(col.b * hdrGain * energyBase);
        const float ca = static_cast<float>(col.a);
        const float dissolve = static_cast<float>(custom0);
        const double cosR = std::cos(start);
        const double sinR = std::sin(start);

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
                const double sx = p[0] * baseRadius;
                const double sy = p[1] * baseRadius;
                const auto uv = profile.mesh.faceUv(fi, corner);
                if (!uv) {
                    valid = false;
                    break;
                }
                appendVertex({
                    static_cast<float>(cx + sx * cosR - sy * sinR),
                    static_cast<float>(cy + sx * sinR + sy * cosR),
                    static_cast<float>((*uv)[0]), static_cast<float>((*uv)[1]),
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

void GPURenderer::renderTriBurstGeometry(double cx, double cy, const TriBurstEmission &sub,
                                         double ageSec)
{
    for (const TriParticle &item : sub.particles) {
        const double progress = layerProgress(item.durationSec, ageSec);
        if (progress >= 1.0) {
            continue;
        }

        const double curve = baclickfx::subSizeCurve(sub.params, progress);
        const baclickfx::Rgba col = baclickfx::subColor(sub.params, progress);

        // 出生位置与速度都沿 item.angle 的径向。
        const double dist = item.speedPxPerSec * item.lifetimeSec * progress;
        const double radius = sub.params.localScalePx * (sub.params.shapeScale + dist);
        const double x = cx + std::cos(item.angle) * radius;
        const double y = cy + std::sin(item.angle) * radius;

        // Unity Billboard 的 startSize 表示四边形边长。PSR_20/21 的
        // m_MaxParticleSize=0.5 远高于该图层可能达到的屏幕尺寸，因此无需额外钳制。
        const double side = sub.params.sizePx * item.sizeMul * std::max(0.0, curve);
        const double half = side * 0.5;
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

    // Ring3/Ring4 使用 BaTouchAdditive.shader 的 One/One 加法混合。
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
    flushVertices(m_shaderAdditive, m_texTriangle.get());
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // 恢复帧内默认状态。
}

} // namespace KWin
