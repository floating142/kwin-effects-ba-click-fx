// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief OpenGL 高动态范围粒子、拖尾与辉光渲染器。
 *
 * 三个粒子着色器分别对应 Unity 编译产物中的材质实现：
 * - alphablend_add.frag：Ring（FX_SHADER_AlphaBlend_Add）；
 * - dissolve.frag：MeshTri（FX_SHADER_Dissolve_GachaGauge_P）；
 * - additive.frag：Ring3/Ring4 三角形（FX_SHADER_Additive_0）。
 *
 * 帧处理流程与 Unity UIRenderPass 保持一致：
 * 1. 将屏幕缓冲导入线性 RGBA16F 场景纹理；
 * 2. 按原始混合状态在场景纹理上绘制粒子；
 * 3. 执行 PPv2 MXFinalBloom 金字塔；
 * 4. 计算 `main + bloom * intensity * color`，再按输出传递函数编码。
 *
 * 场景纹理必须包含真实桌面颜色，因为 Ring 的 `One OneMinusSrcAlpha` 和 Dissolve
 * 的 `SrcAlpha One` 都依赖目标颜色。
 */

#pragma once

#include "clickinstance.h"
#include "baclickfxdefaults.h"
#include "curveutils.h"
#include "meshprofiles.h"
#include "subsystems.h"
#include "trailstream.h"

#include <effect/effect.h>
#include <opengl/glframebuffer.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>
#include <epoxy/gl.h>

#include <QRectF>
#include <QSize>
#include <QString>

#include <array>
#include <memory>
#include <vector>

namespace KWin
{

class RenderTarget;
class RenderViewport;

/// 粒子顶点格式，对应 Unity 的 POSITION、TEXCOORD0、COLOR 和 CustomData0。
struct ParticleVertex {
    float x, y;       // 输出局部逻辑坐标。
    float u, v;       // 纹理坐标。
    float r, g, b, a; // 线性 HDR 颜色，可大于 1。

    // Dissolve 着色器将 CustomData0 用作丢弃阈值；其他图层保持为 0。
    float custom0 = 0.0f;
};

/// 管理各输出的 HDR 场景纹理、粒子绘制、辉光处理和最终合成。
class GPURenderer
{
public:
    GPURenderer();
    ~GPURenderer();

    /// 加载着色器、纹理和顶点缓冲资源。
    bool initialize();

    /**
     * 准备当前输出的 HDR 场景纹理和辉光金字塔。
     *
     * 资源按输出逻辑矩形和缩放缓存。OpenGL 上下文无效或资源分配失败时返回
     * `false`。
     */
    bool prepareTargets(const RenderViewport &viewport);

    /**
     * 导入桌面背景并开始渲染当前输出的 HDR 帧。
     *
     * @param renderTarget KWin 输出目标，同时提供源像素和颜色传递函数。
     * @param viewport 当前输出的几何、缩放和投影信息。
     * @param importRegion 需要从桌面导入的全局逻辑区域。
     * @return 背景导入且 HDR 目标绑定成功时返回 `true`。
     *
     * `importRegion` 必须包含传给 `endFrame()` 的合成区域，以保证覆盖写入使用当前
     * 桌面内容。调用方还应包含上一帧的绘制区域，用于清除私有场景纹理中的旧亮源。
     * HDR 输出允许桌面像素超过辉光阈值，因此该模式下始终导入完整输出。
     */
    bool beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport,
                    const Region &importRegion);

    /// 绘制材质队列 3000 中的 Ring 和 Ring3。
    void renderClickBase(const ClickInstance &inst, const QPointF &outputOrigin);

    /// 在队列 3000 图层之后绘制材质队列 4499 的 MeshTri。
    void renderClickMeshTri(const ClickInstance &inst,
                            const baclickfx::CylinderProfile &mesh, const QPointF &outputOrigin);

    /// 绘制一次 Ring4 距离发射粒子簇。
    void renderTriBurst(const TriBurstInstance &inst, const QPointF &outputOrigin);

    /// 一次性提交此前由 renderTriBurst() 追加的全部 Ring4 几何。
    void flushTriBursts();

    /// 绘制拖尾；宽度和纹理重复间距从 `params.worldUnitPx` 派生。
    void renderTrail(const std::vector<StrokeData> &strokes,
                     const baclickfx::Subsystem &params,
                     const QPointF &outputOrigin);

    /**
     * 执行辉光处理，并将线性 HDR 场景纹理编码回 KWin 输出。
     *
     * @param compositeRegion 最终覆盖写入的全局逻辑区域。
     * @param bloomSourceRegion 本帧粒子写入的原始区域，不包含辉光外扩。
     *
     * `compositeRegion` 必须与 `prePaintScreen()` 申请的区域一致，并被
     * `beginFrame()` 的导入区域包含。辉光从 `bloomSourceRegion` 按各级采样核传播；
     * 最终合成使用已外扩的 `compositeRegion`。FBO 和金字塔保持输出级完整尺寸，
     * `Region` 仅用于跳过确定不变的纹理像素。
     */
    void endFrame(const RenderTarget &renderTarget, const RenderViewport &viewport,
                  const Region &compositeRegion, const Region &bloomSourceRegion);

    /// 启用或禁用背景导入、粒子、辉光和最终合成四个阶段的异步 GPU 计时。
    void setProfiling(bool on);

    /// 同步插件日志级别；错误日志使用 Error+，普通状态日志只使用 Verbose。
    void setLogLevel(baclickfx::defaults::LogLevel level);

    // GPU 查询结果采用异步读取，不会阻塞合成线程。每发布一组新结果，
    // gpuSampleSerial() 增加一次；尚无结果时各阶段时长返回 -1。
    double lastGpuMs() const { return m_lastGpuMs; }
    double lastGpuImportMs() const { return m_lastGpuImportMs; }
    double lastGpuParticleMs() const { return m_lastGpuParticleMs; }
    double lastGpuBloomMs() const { return m_lastGpuBloomMs; }
    double lastGpuCompositeMs() const { return m_lastGpuCompositeMs; }
    QString lastGpuOutput() const { return m_lastGpuOutput; }
    std::uint64_t gpuSampleSerial() const { return m_gpuSampleSerial; }

    /// 返回 GPU、已加载资源和逐输出渲染目标的稳定诊断摘要。
    QString diagnosticStatus() const;

    /**
     * 计算辉光在单个输出上的最大逻辑像素传播半径。
     *
     * 该值只用于重绘区域和最终合成区域的外扩。`devicePx` 必须是单块输出的设备像素
     * 尺寸，不能使用虚拟桌面并集。
     */
    static double bloomReachPx(const QSize &devicePx, double scale);

private:
    // 链接后按名称查询顶点属性位置。未使用的属性可能被编译器优化掉，同时本类不依赖
    // KWin 对 position 和 texcoord 的隐式绑定约定。
    struct ShaderSet
    {
        std::unique_ptr<GLShader> shader;
        // 顶点布局在 shader 链接后固定，初始化一次即可复用。
        GLuint vao = 0;
        int positionLocation = -1;
        int texcoordLocation = -1;
        int colorLocation = -1;
        // 只有 Dissolve 着色器使用 CustomData0；其他程序的属性位置为 -1。
        int custom0Location = -1;

        explicit operator bool() const { return shader != nullptr; }
        GLShader *get() const { return shader.get(); }
    };

    /// 从 shader 目录加载、编译并链接着色器；失败时返回空 `ShaderSet`。
    ShaderSet loadShader(const char *vertexFile, const char *fragmentFile);

    /// 为已链接的 shader 创建并初始化专用 VAO。
    bool initializeVertexArray(ShaderSet &set);

    /// 从 assets 目录加载纹理，并应用对应的 OpenGL 环绕模式。
    std::unique_ptr<GLTexture> loadTexture(const char *assetName, GLenum wrapMode);

    /// 将输出传递函数参数写入已绑定的背景导入或合成着色器。
    static void setTransferFunctionUniforms(GLShader *shader, const RenderTarget &renderTarget);

    /// 将顶点追加到当前 CPU 批次。
    void appendVertex(const ParticleVertex &vertex);
    void appendVertices(const std::vector<ParticleVertex> &vertices);

    /// 为已绑定的 VBO 配置当前着色器使用的顶点属性。
    void setVertexLayout(const ShaderSet &set);

    /// 上传并绘制指定顶点；调用方负责绑定着色器和统一变量。
    void drawVertices(const ShaderSet &set, const std::vector<ParticleVertex> &verts);

    /// 使用指定着色器和纹理绘制当前批次，并清空 CPU 顶点缓冲。
    void flushVertices(const ShaderSet &set, GLTexture *texture);

    // 几何生成。

    /// 生成 Ring 圆盘几何并追加到当前批次。
    void renderRing(double cx, double cy, const RingEmission &sub, double ageSec);

    /// 生成 MeshTri 圆环弧段并追加到当前批次。
    void renderMeshTri(double cx, double cy, const MeshTriEmission &sub, double ageSec,
                       const baclickfx::CylinderProfile &profile);

    /// 生成始终面向屏幕的 Ring3/Ring4 几何，并追加到当前批次。
    void renderTriBurstGeometry(double cx, double cy, const TriBurstEmission &sub,
                                double ageSec, bool flush);

    // 渲染资源。
    // Unity 原始贴图。
    std::unique_ptr<GLTexture> m_texCircle;     // Ring：FX_TEX_Circle_01.png
    std::unique_ptr<GLTexture> m_texGradRing3;  // MeshTri：FX_TEX_Grad_Ring3.png
    std::unique_ptr<GLTexture> m_texTriangle;   // Ring3/Ring4：FX_TEX_Triangle_02_1.png
    std::unique_ptr<GLTexture> m_texTrail;      // 拖尾：FX_TEX_Trail_03.png

    // 背景导入、粒子和最终合成着色器。
    ShaderSet m_shaderBackground;    // 屏幕缓冲 → 线性 HDR（Unity UIRenderPass 第 1 步）
    ShaderSet m_shaderAlphablendAdd; // Ring
    ShaderSet m_shaderDissolve;      // MeshTri
    ShaderSet m_shaderAdditive;      // Ring3/Ring4 三角
    ShaderSet m_shaderComposite;     // MXFinalBloom 合成阶段

    // 辉光处理各阶段的着色器。
    ShaderSet m_shaderBloomPrefilter;
    ShaderSet m_shaderBloomDownsample;
    ShaderSet m_shaderBloomUpsample;

    // 辉光金字塔层级。第 0 级为半分辨率，后续层级依次减半直至 kBloomMinSize。
    // 降采样与上采样使用独立目标，避免上采样时同时读写同一附件形成 OpenGL 反馈环。
    struct BloomLevel {
        std::unique_ptr<GLTexture> downTexture;
        std::unique_ptr<GLFramebuffer> downFbo;
        std::unique_ptr<GLTexture> upTexture;
        std::unique_ptr<GLFramebuffer> upFbo;
        QSize size;
    };

    // 每块输出独立保存渲染目标。KWin 按输出绘制，且各输出的几何和缩放可能不同，
    // 因此每块输出长期复用一套完整尺寸的资源。
    struct OutputTargets
    {
        // 使用 QRectF 以确保边界语义与后续 Qt 几何计算一致。
        QRectF renderRect;  // 全局逻辑矩形和缓存键。
        double scale = 1.0; // 输出缩放和缓存键。
        QSize devicePx;     // 私有纹理尺寸，保持输出的逻辑方向。
        QSize renderTargetDevicePx; // 旋转后的目标设备尺寸和缓存键。
        int transformKind = 0;       // OutputTransform::Kind 和缓存键。

        // 纹理声明在 FBO 之前，确保成员逆序析构时颜色附件最后释放。
        std::unique_ptr<GLTexture> hdrTexture;
        std::unique_ptr<GLFramebuffer> hdrFbo;

        // 编码后的桌面像素先复制到该纹理，再由背景着色器解码到 hdrFbo。
        // RGBA16F 可保留 HDR 输入，并避免解码前发生定点量化。
        std::unique_ptr<GLTexture> bgTexture;
        std::unique_ptr<GLFramebuffer> bgFbo;

        std::vector<BloomLevel> bloomLevels;
        std::vector<Region> downRegions;
        std::vector<Region> upRegions;

        // 跨非活动时段保留，确保下次激活时刷新私有 FBO 中的旧内容。
        Region previousCompositeRegion;

        // 上一帧的原始亮源区域。辉光更新使用它与当前亮源区域的并集，使消失的亮源
        // 同样能够使金字塔失效并被清除。
        Region previousBloomSourceRegion;

        // PPv2 的 `_SampleScale`，由 Diffusion 和输出尺寸计算。
        float bloomSampleScale = 1.0f;
    };

    // 使用 unique_ptr 保证输出列表扩容时 m_current 指向的对象地址保持稳定。
    std::vector<std::unique_ptr<OutputTargets>> m_outputs;
    OutputTargets *m_current = nullptr; // 由 prepareTargets() 选择的当前输出。

    /// 按 PPv2 Diffusion 公式为输出创建辉光金字塔并计算采样半径。
    bool prepareBloomPyramid(OutputTargets &targets, const QSize &devicePx);

    /// 执行辉光预筛选、降采样和上采样链。
    bool renderBloom(const Region &globalChangedRegion);

    /// 恢复 beginFrame() 入口处的混合、视口和裁剪状态。
    void restoreBlendState();

    /// 保存和恢复 beginFrame() 入口处的 VAO/VBO 绑定。
    void captureVertexState();
    void restoreVertexState();

    // KWin GLVertexBuffer 只提供两个属性槽，而本渲染器需要 position、texcoord、color
    // 和可选的 CustomData0，因此使用共享 VBO 和每个 ShaderSet 的专用 VAO。
    GLuint m_vbo = 0;
    std::vector<ParticleVertex> m_vertices;
    struct TrailVertexStyle {
        double halfWidth = 0.0;
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 0.0f;
    };
    std::vector<QPointF> m_trailLocalSamples;
    std::vector<TrailVertexStyle> m_trailStyles;
    std::vector<QPointF> m_trailNormals;
    std::vector<QPointF> m_trailDirs;
    std::vector<ParticleVertex> m_trailStrip;
    std::vector<ParticleVertex> m_regionQuads;

    // 当前帧从输出局部坐标到标准化设备坐标的投影矩阵。
    QMatrix4x4 m_projection;

    // beginFrame() 到 endFrame() 期间保存的 KWin GL 状态。
    bool m_blendStateCaptured = false;
    GLboolean m_savedBlendEnabled = GL_FALSE;
    GLint m_savedBlendSrcRgb = GL_ONE;
    GLint m_savedBlendDstRgb = GL_ZERO;
    GLint m_savedBlendSrcAlpha = GL_ONE;
    GLint m_savedBlendDstAlpha = GL_ZERO;
    GLint m_savedViewport[4] = {0, 0, 0, 0};
    GLboolean m_savedScissorEnabled = GL_FALSE;
    GLint m_savedScissorBox[4] = {0, 0, 0, 0};
    bool m_vertexStateCaptured = false;
    GLint m_savedVao = 0;
    GLint m_savedVbo = 0;

    // GPU 性能计时状态。每个采样帧包含四个连续且互不嵌套的 GL_TIME_ELAPSED 查询；
    // OpenGL 不允许同一查询目标发生嵌套。
    enum class GpuTimerPhase : int {
        Import = 0,
        Particle,
        Bloom,
        Composite,
        Count,
    };
    static constexpr int kGpuTimerFrameCount = 2;
    static constexpr int kGpuTimerPhaseCount = int(GpuTimerPhase::Count);
    struct GpuTimerFrame {
        std::array<GLuint, kGpuTimerPhaseCount> queries{};
        bool pending = false;
        bool publishable = false;
        int issuedPhases = 0;
        QString output;
    };

    void collectGpuTimings();
    void beginGpuTimingFrame();
    void beginGpuTimerPhase(GpuTimerPhase phase);
    void endGpuTimerPhase();
    void switchGpuTimerPhase(GpuTimerPhase phase);
    void finishGpuTimingFrame(bool publishable);

    // 两组查询帧异步轮换。若两组结果都未就绪，则跳过当前帧计时，避免阻塞合成线程。
    bool m_profiling = false;
    bool m_logVerbose = false;
    bool m_timerNeedsCleanup = false;
    std::array<GpuTimerFrame, kGpuTimerFrameCount> m_timerFrames;
    int m_nextTimerFrame = 0;
    int m_activeTimerFrame = -1;
    int m_activeTimerPhase = -1;
    double m_lastGpuMs = -1.0;
    double m_lastGpuImportMs = -1.0;
    double m_lastGpuParticleMs = -1.0;
    double m_lastGpuBloomMs = -1.0;
    double m_lastGpuCompositeMs = -1.0;
    QString m_lastGpuOutput;
    std::uint64_t m_gpuSampleSerial = 0;
    QString m_gpuVendor;
    QString m_gpuRenderer;
    QString m_gpuVersion;
    bool m_lastImportDirect = false;
};

} // namespace KWin
