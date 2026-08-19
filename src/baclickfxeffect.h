// SPDX-License-Identifier: GPL-3.0-or-later
// BA Click FX 的 KWin 特效实现。

#pragma once

#include "baclickfxdefaults.h"
#include "clickinstance.h"
#include "curveutils.h"
#include "gpurenderer.h"
#include "meshprofiles.h"
#include "subsystems.h"
#include "trailstream.h"

#include <core/region.h>
#include <effect/effect.h>

#include <QLoggingCategory>
#include <QHash>
#include <QPointF>

#include <chrono>
#include <cstdint>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(KWIN_BA_CLICK_FX)

namespace KWin
{

/** KWin effect that renders the ported FX_Touch particle and trail systems. */
class BaClickFxEffect : public Effect
{
    Q_OBJECT

public:
    BaClickFxEffect();
    ~BaClickFxEffect() override;

    void reconfigure(ReconfigureFlags flags) override;
    bool isActive() const override;
    int requestedEffectChainPosition() const override;
    QString debug(const QString &parameter) const override;

    void prePaintScreen(ScreenPrePaintData &data) override;
    /// 绘制底层场景后，将粒子和拖尾合成到当前输出。
    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                     int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    void postPaintScreen() override;

    /// 使用输入事件采样拖动轨迹，避免高回报率设备的帧内路径信息丢失。
    void pointerMotion(PointerMotionEvent *event) override;

    static bool supported();

private Q_SLOTS:
    /// 处理全局鼠标按键状态跃迁；指针移动由 `pointerMotion()` 采样。
    void slotMouseChanged(const QPointF &pos, const QPointF &oldPos,
                          Qt::MouseButtons buttons, Qt::MouseButtons oldButtons,
                          Qt::KeyboardModifiers modifiers, Qt::KeyboardModifiers oldModifiers);

private:
    /// 读取 `[Effect-ba-click-fx]` 配置并重建运行时参数。
    void loadConfig();

    /// 定位并加载 Cylinder002 OBJ 网格。
    void loadMeshes();

    /// 推进实例状态并移除寿命结束的对象。
    void advance(double dt);

    /**
     * 渲染当前输出上的全部活动内容。
     *
     * @return 成功提交可见内容时返回 `true`；无内容或渲染目标初始化失败时返回
     * `false`。
     */
    bool renderGpu(const RenderTarget &renderTarget, const RenderViewport &viewport);

    /// 绘制 damage 诊断叠层；青色为插件申请区域。
    void drawDebugDamage(const RenderTarget &renderTarget, const RenderViewport &viewport);

    /// 返回不含 Bloom 外扩的保守稀疏内容区域，使用全局逻辑坐标。
    Region contentRegion() const;

    /// 按各输出 Bloom 最大传播半径扩展内容区域。
    Region dirtyRegion(const Region &content) const;

    /// 返回当前输出的 Bloom 逻辑像素外扩半径。
    double bloomPadding() const;

    /// 在全局逻辑坐标 `pos` 创建点击实例。
    void spawn(const QPointF &pos);

    /// 开始新的拖动会话并初始化距离累计状态。
    void startDrag(const QPointF &pos);

    /// 根据累计路径距离追加拖尾采样并触发 Ring4 发射。
    void updateDrag(const QPointF &pos);

    /// 结束拖动会话；已有拖尾点继续按寿命老化。
    void endDrag();

    /// 返回当前时间缩放是否满足 TrailRenderer 的启用阈值。
    bool trailEnabled() const;

    /**
     * 返回 `pos` 所在输出的逻辑高度。
     *
     * 坐标未命中输出时依次回退到首个输出和 1080 像素参考高度。
     */
    double outputHeightForPos(const QPointF &pos) const;

    /**
     * 确保子系统参数按指定输出高度换算。
     *
     * 参数在原对象中重建以保持成员地址稳定；跨越不同高度的输出时，活动粒子同步
     * 使用新输出尺度。
     */
    void ensureSubsystemsForHeight(double heightPx);

    /// 返回指定拖动会话中仍存活的 Ring4 粒子数。
    int liveDistanceParticles(std::uint64_t dragSerial) const;

    baclickfx::defaults::LogLevel m_logLevel = baclickfx::defaults::kLogLevelDefault;
    bool m_debugDamage = false;
    bool m_dragging = false;
    bool m_autoTrailSession = false;
    std::chrono::steady_clock::time_point m_lastAutoTrailMotion;
    double m_timeScale = 1.0;
    double m_globalScale = 1.0;
    // 当前参数表对应的输出逻辑高度；0 表示尚未构建。
    double m_subsystemHeightPx = 0.0;
    // Ring4 粒子上限作用于单次按下；0 保留为未关联会话。
    std::uint64_t m_dragSerial = 0;

    // 功能开关；Unity 已定义的视觉常量不提供配置项。
    bool m_enableTrail = true;
    bool m_alwaysTrail = false;
    bool m_enableDistanceEmitter = true;

    // 配置重建参数表时会先清空所有依赖旧参数的实例。
    baclickfx::SubsystemMap m_subsystems;
    baclickfx::MeshProfiles m_meshes;
    baclickfx::Rng m_rng;

    std::vector<ClickInstance> m_instances;
    std::vector<TriBurstInstance> m_bursts;
    struct TrailSession {
        TrailStream stream;
        baclickfx::Subsystem trailParams;
        baclickfx::Subsystem ring4Params;
        bool active = false;
    };
    std::vector<TrailSession> m_trails;

    // 渲染器状态；OpenGL 资源在 paintScreen() 中延迟初始化。
    GPURenderer m_gpu;
    bool m_gpuReady = false;
    bool m_gpuTried = false;

    // 拖动采样状态
    // 上一次采样到的指针位置。
    QPointF m_lastDrag;
    // 上一个拖尾落点；未起笔时由 m_trailEmitValid 标记无效。
    QPointF m_trailEmit;
    bool m_trailEmitValid = false;
    // 尚未达到 minVertexDistance 的跨帧累计距离。
    double m_trailAccum = 0.0;
    // 尚未达到 Ring4 发射间距的跨帧累计距离。
    double m_distAccum = 0.0;

    // 帧时钟；0 表示第一帧仅建立时间基准。
    std::chrono::nanoseconds m_lastFrame{0};

    // 收到 pointerMotion 或 mouseChanged 移动通知后停止帧级 cursorPos() 回退采样。
    bool m_motionEventsWork = false;

    // 以下区域均使用全局逻辑坐标。缓存结果可确保预绘制、背景导入、Bloom 和最终
    // 合成使用同一个模拟快照。
    Region m_lastDirty;
    // 未做 Bloom 外扩的内容区域。
    Region m_lastContent;

    // 上一帧合成区域用于在内容消失后额外重绘一次，清除残留像素。
    bool m_paintedLastFrame = false;
    Region m_lastPainted;

    // 背景导入区域为当前 dirty 与上一帧绘制区域的并集，用于覆盖私有 HDR 目标中的
    // 旧粒子。最终 composite 仍只使用当前 dirty 区域。
    Region m_lastClearArea;

    bool logsInstances() const;
    bool logsFrames() const;
    bool logsVerbose() const;

    // 性能统计状态，仅在帧统计级别及以上更新。记录 CPU 分段、异步 GPU 计时、呈现
    // 间隔，以及插件申请区域与 KWin 实际重绘区域的差异。
    void logFrameStats(const RenderViewport &viewport, const Region &deviceRegion, double cpuMs);

    struct OutputFrameStats
    {
        int frames = 0;
        int gpuSamples = 0;
        double gpuImportMsSum = 0.0;
        double gpuBloomMsSum = 0.0;
        double gpuCompositeMsSum = 0.0;
        double devicePxSum = 0.0;
        double requestPxSum = 0.0;
        double sourcePxSum = 0.0;
        int deviceRects = 0;
        int requestRects = 0;
        int sourceRects = 0;
    };

    // 上一次呈现间隔，单位为毫秒。
    double m_lastFrameDelta = 0.0;

    // 最近一帧各 CPU 阶段的墙钟时间，单位为毫秒。
    double m_lastPrePaintCpuMs = 0.0;
    double m_lastSetupCpuMs = 0.0;
    double m_lastTrailCpuMs = 0.0;
    double m_lastParticleCpuMs = 0.0;
    double m_lastFinishCpuMs = 0.0;
    double m_inputCpuMsSinceLog = 0.0;
    QHash<QString, OutputFrameStats> m_outputFrameStats;
    std::uint64_t m_inputEvents = 0;
    std::uint64_t m_inputAccepted = 0;
    std::uint64_t m_inputMerged = 0;
    std::uint64_t m_inputDiscarded = 0;
    std::uint64_t m_inputCrossScreen = 0;
    std::uint64_t m_mouseChangedEvents = 0;
    std::uint64_t m_mouseChangedMotion = 0;
    std::uint64_t m_pointerMotionEvents = 0;
    std::uint64_t m_fallbackSamples = 0;
    std::uint64_t m_skipNoActivity = 0;
    std::uint64_t m_skipNoDamage = 0;
    std::uint64_t m_skipGpu = 0;
    std::uint64_t m_skipTarget = 0;
    std::uint64_t m_skipImport = 0;

    int m_statFrames = 0;
    double m_statCpuMsSum = 0.0;
    double m_statCpuMsMax = 0.0;
    double m_statPrePaintCpuMsSum = 0.0;
    double m_statSetupCpuMsSum = 0.0;
    double m_statTrailCpuMsSum = 0.0;
    double m_statParticleCpuMsSum = 0.0;
    double m_statFinishCpuMsSum = 0.0;
    double m_statGpuMsSum = 0.0;
    double m_statGpuMsMax = 0.0;
    double m_statGpuImportMsSum = 0.0;
    double m_statGpuParticleMsSum = 0.0;
    double m_statGpuBloomMsSum = 0.0;
    double m_statGpuCompositeMsSum = 0.0;
    int m_statGpuSamples = 0;
    std::uint64_t m_lastGpuSampleSerial = 0;
    double m_statFrameMsSum = 0.0;
    double m_statFrameMsMax = 0.0;
    double m_statDevicePxSum = 0.0;   // KWin 实际重画的设备像素数
    double m_statRequestPxSum = 0.0;  // 我们申请的（m_lastDirty 折算设备像素）
    double m_statBloomSourcePxSum = 0.0; // Bloom 原始输入（m_lastContent）设备像素数
    double m_statDeviceRectsSum = 0.0;
    double m_statRequestRectsSum = 0.0;
    double m_statBloomSourceRectsSum = 0.0;
};

} // namespace KWin
