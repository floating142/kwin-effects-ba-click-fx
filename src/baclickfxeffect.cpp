// SPDX-License-Identifier: GPL-3.0-or-later

#include "baclickfxeffect.h"
#include "unity/timing.h"
#include "applicationfilter.h"
#include "baclickfxdefaults.h"
#include "diagnostics.h"
#include "damageutils.h"
#include "outputscaleutils.h"
#include "mousebuttonutils.h"

#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <opengl/glshader.h>
#include <opengl/glvertexbuffer.h>
#include <opengl/glshadermanager.h>
#include <window.h>
#include <workspace.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QStandardPaths>
#include <QVector2D>
#include <QJsonDocument>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(KWIN_BA_CLICK_FX, "kwin_effect_ba_click_fx", QtInfoMsg)

namespace KWin
{

namespace
{

constexpr auto kAutoTrailIdleTimeout = std::chrono::milliseconds(50);

constexpr int kDamageTilePx = 64;

void unitePointBounds(QRectF &bounds, double x, double y)
{
    const QRectF point(x, y, 0.0001, 0.0001);
    bounds = bounds.isNull() ? point : bounds.united(point);
}

QRectF triBurstBounds(const QPointF &center, const TriBurstEmission &burst)
{
    QRectF bounds;
    for (const TriParticle &item : burst.particles) {
        const double progress = particleProgress(item.percentAge);
        if (progress >= 1.0) {
            continue;
        }

        // 与 GPURenderer::renderTriBurstGeometry 使用完全相同的位置和尺寸公式。
        const QPointF position = center + triParticleOffset(item, burst.params);
        const double x = position.x();
        const double y = position.y();
        const double half = baclickfx::billboardHalfSize(burst.params, item.startSize,
            float(baclickfx::subSizeCurve(burst.params, progress)), item.percentAge);

        // PS17/PS18 当前没有旋转；仍按渲染器公式保留通用旋转，避免参数以后变化
        // 时 damage 与实际几何脱节。
        const double rot = item.orientBase + item.spin * progress;
        const double cosR = std::cos(rot);
        const double sinR = std::sin(rot);
        for (const auto &[sx, sy] : {std::pair{-half, -half}, std::pair{half, -half},
                                    std::pair{-half, half}, std::pair{half, half}}) {
            unitePointBounds(bounds, x + sx * cosR - sy * sinR,
                             y + sx * sinR + sy * cosR);
        }
    }
    return bounds;
}

QRectF clickBaseBounds(const ClickInstance &inst)
{
    QRectF bounds;

    // Ring：与 renderRing 相同的旋转正方形。
    const double ringP = particleProgress(inst.ring.percentAge);
    if (ringP < 1.0) {
        const double half = baclickfx::billboardHalfSize(inst.ring.params, inst.ring.startSize,
            float(baclickfx::subSizeCurve(inst.ring.params, ringP)), inst.ring.percentAge);
        const double c = std::cos(inst.ring.startRotationRad);
        const double s = std::sin(inst.ring.startRotationRad);
        for (const auto &[sx, sy] : {std::pair{-half, -half}, std::pair{half, -half},
                                    std::pair{-half, half}, std::pair{half, half}}) {
            unitePointBounds(bounds, inst.center.x() + sx * c - sy * s,
                             inst.center.y() + sx * s + sy * c);
        }
    }

    return bounds;
}

QRectF clickMeshBounds(const ClickInstance &inst,
                       const baclickfx::CylinderProfile &profile)
{
    QRectF bounds;

    // MeshTri：逐弧段、逐 OBJ 顶点执行与 renderMeshTri 相同的缩放和旋转。
    const double meshP = particleProgress(inst.meshTri.percentAge);
    if (meshP < 1.0) {
        for (const MeshTriArc &arc : inst.meshTri.arcs) {
            const double localP = meshP;
            const double scale = inst.meshTri.params.sizePx * arc.radiusMul
                * std::max(0.0, baclickfx::subSizeCurve(inst.meshTri.params, localP));
            const double rotation = arc.angle;
            const double c = std::cos(rotation), s = std::sin(rotation);
            for (const baclickfx::Vertex &p : profile.mesh.vertices) {
                const auto point = baclickfx::unityObjPointOffset(p, scale, c, s);
                unitePointBounds(bounds, inst.center.x() + point[0], inst.center.y() + point[1]);
            }
        }
    }

    return bounds;
}

// 稀疏 damage 的量化网格。包围盒只向网格边界扩展，不会裁掉可见像素；64 像素网格
// 可保留长对角拖尾的窄带形状，同时限制 Region 的矩形数量和运算开销。
Rect damageTileRect(const QRectF &bounds)
{
    const QRect tiled = baclickfx::quantizeDamageRect(bounds);
    return tiled.isEmpty() ? Rect() : Rect(tiled.x(), tiled.y(), tiled.width(), tiled.height());
}

void appendDamageRect(QList<Rect> &rects, const QRectF &bounds)
{
    const Rect tiled = damageTileRect(bounds);
    // 相邻线段常量化到同一 tile；重复矩形对 Region 并集没有影响，只增加合并成本。
    if (!tiled.isEmpty() && (rects.isEmpty() || rects.back() != tiled)) {
        rects.append(tiled);
    }
}

void appendTrailDamage(QList<Rect> &rects, const TrailStream &stream, double pad)
{
    const std::deque<TrailPoint> &points = stream.points();
    if (points.size() < 2) {
        return;
    }
    for (std::size_t i = 1; i < points.size(); i++) {
        if (points[i].penUp) {
            continue;
        }
        const QPointF &a = points[i - 1].pos;
        const QPointF &b = points[i].pos;
        // 后端事件间隔可能产生很长的线段。仅在 damage 计算中按网格长度切段，避免
        // 单个大 AABB 覆盖大量空白；真实 Trail 点、UV、宽度和顶点数保持不变。
        const double dx = b.x() - a.x();
        const double dy = b.y() - a.y();
        const int pieces = std::max(1, int(std::ceil(
            std::max(std::abs(dx), std::abs(dy)) / kDamageTilePx)));
        for (int piece = 0; piece < pieces; piece++) {
            const double t0 = double(piece) / pieces;
            const double t1 = double(piece + 1) / pieces;
            const QPointF p0(a.x() + dx * t0, a.y() + dy * t0);
            const QPointF p1(a.x() + dx * t1, a.y() + dy * t1);
            const double left = std::min(p0.x(), p1.x()) - pad;
            const double top = std::min(p0.y(), p1.y()) - pad;
            const double right = std::max(p0.x(), p1.x()) + pad;
            const double bottom = std::max(p0.y(), p1.y()) + pad;
            appendDamageRect(rects, QRectF(left, top, right - left, bottom - top));
        }
    }
}

} // namespace

BaClickFxEffect::BaClickFxEffect()
{
    loadConfig();
    loadMeshes();

    connect(effects, &EffectsHandler::mouseChanged,
            this, &BaClickFxEffect::slotMouseChanged);
    connect(Workspace::self(), &Workspace::windowRemoved, this, [this](Window *window) {
        if (window) {
            m_processIdentityCache.remove(window->internalId());
        }
    });

    if (logsVerbose()) {
        qCInfo(KWIN_BA_CLICK_FX) << "BA Click FX 已加载" << debug(QStringLiteral("status"));
    }
}

BaClickFxEffect::~BaClickFxEffect() = default;

bool BaClickFxEffect::supported()
{
    // 本插件依赖 Shader、RGBA16F FBO 和顶点缓冲，仅支持 OpenGL 合成器。
    return effects->isOpenGLCompositing();
}

void BaClickFxEffect::reconfigure(ReconfigureFlags)
{
    loadConfig();

    // 配置重载会清空活动实例。清空发生在帧循环之外，因此显式重绘上一帧区域以清除
    // 已合成到屏幕的内容。
    if (m_paintedLastFrame) {
        effects->addRepaint(m_lastPainted);
        m_paintedLastFrame = false;
        if (logsVerbose()) {
            qCInfo(KWIN_BA_CLICK_FX) << "配置重载，已申请擦除上一帧脏区";
        }
    }

    if (logsVerbose()) {
        qCInfo(KWIN_BA_CLICK_FX) << "reconfigureEffect 完成" << debug(QStringLiteral("status"));
    }
}

void BaClickFxEffect::loadMeshes()
{
    // 资源装在 $XDG_DATA_DIRS/kwin/effects/ba-click-fx/assets/ 下。
    const auto locate = [](const char *name) {
        return QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                      QStringLiteral("kwin/effects/ba-click-fx/assets/")
                                          + QLatin1String(name))
            .toStdString();
    };

    const std::string cylinder = locate("Cylinder002.obj");
    m_meshes = baclickfx::loadMeshProfiles(cylinder);

    if (logsVerbose()) {
        qCInfo(KWIN_BA_CLICK_FX) << "网格已加载 cylinder 面数"
                             << m_meshes.cylinder002.mesh.faces.size();
    }
}

void BaClickFxEffect::loadConfig()
{
    namespace def = baclickfx::defaults;

    const KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                                   ->group(QLatin1String(def::kGroup));
    const int configuredLogLevel = group.readEntry(
        def::kLogLevel, int(def::kLogLevelDefault));
    m_logLevel = static_cast<def::LogLevel>(std::clamp(
        configuredLogLevel, int(def::LogLevel::Off), int(def::LogLevel::Verbose)));
    m_debugDamage = group.readEntry(def::kDebugDamage, def::kDebugDamageDefault);

    // 运行时限制与配置页控件共用 baclickfxdefaults.h 中的取值范围。
    m_timeScale = baclickfx::clamp(group.readEntry(def::kTimeScale, def::kTimeScaleDefault),
                                 def::kTimeScaleMin, def::kTimeScaleMax);
    m_globalScale = baclickfx::clamp(group.readEntry(def::kGlobalScale, def::kGlobalScaleDefault),
                                   def::kGlobalScaleMin, def::kGlobalScaleMax);
    const QJsonDocument overrideDoc = QJsonDocument::fromJson(
        group.readEntry(def::kOutputScaleOverrides, QByteArray()));
    m_outputScaleOverrides = overrideDoc.isObject() ? overrideDoc.object() : QJsonObject();
    m_outputScaleEnabled = group.readEntry(def::kOutputScaleEnabled,
                                           def::kOutputScaleEnabledDefault);

    // Unity 已定义的视觉参数保持固定；以下配置只控制对应图层是否绘制。
    m_enableTrail = group.readEntry(def::kEnableTrail, def::kEnableTrailDefault);
    m_alwaysTrail = group.readEntry(def::kAlwaysTrail, def::kAlwaysTrailDefault);
    m_enableDistanceEmitter = m_enableTrail
        && group.readEntry(def::kEnableDistanceEmitter, def::kEnableDistanceEmitterDefault);
    m_enableClick = group.readEntry(def::kEnableClick, def::kEnableClickDefault);
    setTriggerButtons(group.readEntry(def::kTriggerButtons, def::kTriggerButtonsDefault));
    m_disableOnFullscreen = group.readEntry(def::kDisableOnFullscreen,
                                             def::kDisableOnFullscreenDefault);
    m_desktopOnly = group.readEntry(def::kDesktopOnly, def::kDesktopOnlyDefault);
    m_excludeApplications = group.readEntry(def::kExcludeApplications,
                                             def::kExcludeApplicationsDefault);
    m_excludedApplications = baclickfx::parseExcludedApplications(
        group.readEntry(def::kExcludedApplications, QByteArray()));
    m_processIdentityCache.clear();

    // GPU timer 只在帧统计及以上级别启用。
    m_gpu.setLogLevel(m_logLevel);
    m_gpu.setProfiling(logsFrames());
    resetFrameStats();
    m_lastGpuSampleSerial = m_gpu.gpuSampleSerial();
    m_statFrameMsSum = m_statFrameMsMax = 0.0;
    m_statDevicePxSum = m_statRequestPxSum = m_statBloomSourcePxSum = 0.0;
    m_statDeviceRectsSum = m_statRequestRectsSum = m_statBloomSourceRectsSum = 0.0;
    m_lastFrameDelta = 0.0;

    // Instances own a snapshot of their visual parameters. Keep them alive so a
    // reconfigure affects only effects spawned afterwards.
    refreshSubsystems(effects->cursorPos());

    if (logsVerbose()) {
        qCInfo(KWIN_BA_CLICK_FX) << "配置已重载 timeScale" << m_timeScale
                             << "globalScale" << m_globalScale
                             << "worldUnitPx" << m_subsystems.ring.worldUnitPx
                             << debug(QStringLiteral("status"));
    }
}

bool BaClickFxEffect::logsInstances() const
{
    return m_logLevel >= baclickfx::defaults::LogLevel::Instance;
}

bool BaClickFxEffect::logsFrames() const
{
    return m_logLevel >= baclickfx::defaults::LogLevel::Frame;
}

bool BaClickFxEffect::logsVerbose() const
{
    return m_logLevel >= baclickfx::defaults::LogLevel::Verbose;
}

QSizeF BaClickFxEffect::outputSizeForPos(const QPointF &pos) const
{
    // Unity 固定 UI Pass 的垂直范围为两个世界单位，因此一个世界单位等于输出逻辑
    // 高度的一半，特效在不同分辨率下保持相同占屏比例。
    const QPoint pixel = baclickfx::logicalPixelAt(pos);
    const LogicalOutput *first = nullptr;
    for (const LogicalOutput *out : effects->screens()) {
        if (!out) {
            continue;
        }
        if (!first) {
            first = out;
        }
        const Rect g = out->geometry();
        if (g.contains(pixel)) {
            return QSizeF(g.width(), g.height());
        }
    }
    if (first) {
        return QSizeF(first->geometry().width(), first->geometry().height());
    }
    return QSizeF(1920, baclickfx::kReferenceHeightPx);
}

double BaClickFxEffect::outputScaleForPos(const QPointF &pos) const
{
    if (!m_outputScaleEnabled) return m_globalScale;
    const QPoint pixel = baclickfx::logicalPixelAt(pos);
    for (const LogicalOutput *out : effects->screens()) {
        if (!out || !out->geometry().contains(pixel)) continue;
        const QString fingerprint = baclickfx::outputScaleId(
            out->manufacturer(), out->model(), out->serialNumber(), out->name());
        for (const QString &key : {baclickfx::preferredOutputScaleId(
                                      out->uuid(), out->manufacturer(), out->model(),
                                      out->serialNumber(), out->name()), fingerprint}) {
            if (key.isEmpty() || !m_outputScaleOverrides.contains(key)) continue;
            return baclickfx::clamp(m_outputScaleOverrides.value(key).toDouble(
                                        baclickfx::defaults::kGlobalScaleDefault),
                                    baclickfx::defaults::kGlobalScaleMin,
                                    baclickfx::defaults::kGlobalScaleMax);
        }
    }
    // 单独缩放开启后不再回退到整体缩放。旧配置或新接入显示器尚无记录时，
    // 使用显示器倍率自身的默认值。
    return baclickfx::defaults::kGlobalScaleDefault;
}

void BaClickFxEffect::refreshSubsystems(const QPointF &pos)
{
    // 每次创建实例或拖尾会话都按所在输出重建，避免跨屏后沿用旧高度或逐屏缩放。
    const auto size = outputSizeForPos(pos);
    m_subsystems = baclickfx::buildSubsystemMap(m_timeScale, outputScaleForPos(pos), size.height(), size.width());
}

bool BaClickFxEffect::isActive() const
{
    // 拖动中即使屏上暂时空着也要保持活跃：光标位置是按帧轮询的，
    // 一旦停帧就再也收不到移动，距离发射器和拖尾都会停在原地。
    return m_dragging || !m_pendingClicks.empty()
        || !m_instances.empty() || !m_bursts.empty() || !m_trails.empty();
}

int BaClickFxEffect::requestedEffectChainPosition() const
{
    // 画在最上层，盖住窗口内容。
    return 99;
}

QString BaClickFxEffect::debug(const QString &parameter) const
{
    if (parameter.startsWith(QStringLiteral("preview:"))) {
        const QJsonDocument doc = QJsonDocument::fromJson(parameter.mid(8).toUtf8());
        if (doc.isObject()) {
            const_cast<BaClickFxEffect *>(this)->applyPreview(doc.object());
        }
        return QStringLiteral("preview-applied");
    }
    const QString status = QStringLiteral(
        "build=%1 logLevel=%2 debugDamage=%3 gpuReady=%4 active=%5 "
        "enableClick=%6 disableOnFullscreen=%7 desktopOnly=%8 "
        "excludeApplications=%9 excludedApplicationCount=%10")
        .arg(QStringLiteral(BA_CLICK_FX_BUILD_ID))
        .arg(int(m_logLevel))
        .arg(m_debugDamage)
        .arg(m_gpuReady)
        .arg(isActive())
        .arg(m_enableClick)
        .arg(m_disableOnFullscreen)
        .arg(m_desktopOnly)
        .arg(m_excludeApplications)
        .arg(m_excludedApplications.size())
        + QStringLiteral(" triggerButtons=%1").arg(m_triggerButtons.toInt());
    if (parameter.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0
        && logsInstances()) {
        qCInfo(KWIN_BA_CLICK_FX) << "日志测试" << status;
    }
    if (parameter.compare(QStringLiteral("outputs-json"), Qt::CaseInsensitive) == 0) {
        QJsonArray outputs;
        for (const LogicalOutput *out : effects->screens()) {
            if (!out) continue;
            outputs.append(QJsonObject{{QStringLiteral("id"), baclickfx::preferredOutputScaleId(
                out->uuid(), out->manufacturer(), out->model(), out->serialNumber(), out->name())},
                {QStringLiteral("uuid"), out->uuid()}, {QStringLiteral("name"), out->name()},
                {QStringLiteral("pixelWidth"), out->pixelSize().width()},
                {QStringLiteral("pixelHeight"), out->pixelSize().height()}});
        }
        return QString::fromUtf8(QJsonDocument(outputs).toJson(QJsonDocument::Compact));
    }
    if (parameter.compare(QStringLiteral("diagnostics"), Qt::CaseInsensitive) == 0) {
        QStringList outputs;
        for (const LogicalOutput *out : effects->screens()) {
            if (!out) continue;
            const Rect geometry = out->geometry();
            outputs.append(QStringLiteral("%1x%2+%3+%4@%5")
                               .arg(geometry.width()).arg(geometry.height())
                               .arg(geometry.x()).arg(geometry.y())
                               .arg(out->scale(), 0, 'f', 2));
        }
        const QString assetRoot = QStandardPaths::locate(
            QStandardPaths::GenericDataLocation,
            QStringLiteral("kwin/effects/ba-click-fx/assets"),
            QStandardPaths::LocateDirectory);
        const QString shaderRoot = QStandardPaths::locate(
            QStandardPaths::GenericDataLocation,
            QStringLiteral("kwin/effects/ba-click-fx/shader"),
            QStandardPaths::LocateDirectory);
        return QStringLiteral("event=diagnostics build=%1 kwin_version=\"%2\" "
                              "log_level=%3 gpu_ready=%4 active=%5 outputs=\"%6\" "
                              "asset_root=\"%7\" shader_root=\"%8\" %9 "
                              "skip_no_activity=%10 skip_no_damage=%11 skip_gpu=%12 "
                              "skip_target=%13 skip_import=%14 enable_click=%15 "
                              "disable_on_fullscreen=%16 desktop_only=%17 "
                              "exclude_applications=%18 excluded_application_count=%19")
            .arg(QStringLiteral(BA_CLICK_FX_BUILD_ID))
            .arg(QStringLiteral(BA_CLICK_FX_KWIN_VERSION))
            .arg(int(m_logLevel)).arg(m_gpuReady).arg(isActive())
            .arg(outputs.join(QLatin1Char(','))).arg(assetRoot).arg(shaderRoot)
            .arg(m_gpu.diagnosticStatus()).arg(m_skipNoActivity).arg(m_skipNoDamage)
            .arg(m_skipGpu).arg(m_skipTarget).arg(m_skipImport)
            .arg(m_enableClick).arg(m_disableOnFullscreen)
            .arg(m_desktopOnly).arg(m_excludeApplications).arg(m_excludedApplications.size())
            + QStringLiteral(" trigger_buttons=%1").arg(m_triggerButtons.toInt());
    }
    return status;
}

void BaClickFxEffect::applyPreview(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("timeScale"))) {
        m_timeScale = baclickfx::clamp(
            obj.value(QStringLiteral("timeScale")).toDouble(),
            baclickfx::defaults::kTimeScaleMin, baclickfx::defaults::kTimeScaleMax);
    }
    if (obj.contains(QStringLiteral("globalScale"))) {
        m_globalScale = baclickfx::clamp(
            obj.value(QStringLiteral("globalScale")).toDouble(),
            baclickfx::defaults::kGlobalScaleMin,
            baclickfx::defaults::kGlobalScaleMax);
    }
    if (obj.contains(QStringLiteral("desktopOnly"))) {
        m_desktopOnly = obj.value(QStringLiteral("desktopOnly")).toBool();
    }
    if (obj.contains(QStringLiteral("excludeApplications"))) {
        m_excludeApplications = obj.value(QStringLiteral("excludeApplications")).toBool();
    }
    if (obj.contains(QStringLiteral("excludedApplications"))
        && obj.value(QStringLiteral("excludedApplications")).isArray()) {
        m_excludedApplications = baclickfx::excludedApplicationsFromJson(
            obj.value(QStringLiteral("excludedApplications")).toArray());
    }
    if (obj.contains(QStringLiteral("outputScaleEnabled"))) {
        m_outputScaleEnabled = obj.value(QStringLiteral("outputScaleEnabled")).toBool();
    }
    if (obj.contains(QStringLiteral("outputScaleOverrides"))
        && obj.value(QStringLiteral("outputScaleOverrides")).isObject()) {
        m_outputScaleOverrides = obj.value(QStringLiteral("outputScaleOverrides")).toObject();
    }
    if (obj.contains(QStringLiteral("enableTrail"))) {
        m_enableTrail = obj.value(QStringLiteral("enableTrail")).toBool();
    }
    if (obj.contains(QStringLiteral("alwaysTrail"))) {
        m_alwaysTrail = obj.value(QStringLiteral("alwaysTrail")).toBool();
    }
    if (obj.contains(QStringLiteral("enableDistanceEmitter"))) {
        m_enableDistanceEmitter = m_enableTrail
            && obj.value(QStringLiteral("enableDistanceEmitter")).toBool();
    }
    if (obj.contains(QStringLiteral("enableClick"))) {
        m_enableClick = obj.value(QStringLiteral("enableClick")).toBool();
    }
    if (obj.contains(QStringLiteral("triggerButtons"))) {
        setTriggerButtons(obj.value(QStringLiteral("triggerButtons")).toInt(
            baclickfx::defaults::kTriggerButtonsDefault));
    }
    if (obj.contains(QStringLiteral("disableOnFullscreen"))) {
        m_disableOnFullscreen = obj.value(QStringLiteral("disableOnFullscreen")).toBool();
    }
    refreshSubsystems(effects->cursorPos());
}

void BaClickFxEffect::setTriggerButtons(int buttons)
{
    const auto triggers = baclickfx::normalizedTriggerButtons(buttons);
    if (triggers != m_triggerButtons) {
        // 实时预览改键时结束旧笔划，避免已取消的按键继续维持拖尾。
        endDrag();
        m_pressSuppressed = false;
        m_triggerButtons = triggers;
    }
}

void BaClickFxEffect::spawn(const QPointF &pos)
{
    refreshSubsystems(pos);
    ClickInstance inst = makeClickInstance(pos, m_subsystems, m_timeScale, m_rng);
    if (logsInstances()) {
        qCInfo(KWIN_BA_CLICK_FX) << "起实例" << pos << "时长" << inst.life
                             << "秒 ring3 粒子" << inst.ring3.particles.size()
                             << "meshTri 弧段" << inst.meshTri.arcs.size();
    }
    m_instances.push_back(std::move(inst));
}

void BaClickFxEffect::advance(double dt)
{
    for (ClickInstance &inst : m_instances) {
        advanceClickFrame(inst, dt);
    }
    std::erase_if(m_instances, [](const ClickInstance &inst) {
        return !clickInstanceAlive(inst);
    });

    for (TriBurstInstance &inst : m_bursts) {
        advanceTriBurstFrame(inst.burst, dt);
        inst.age += dt;
    }
    std::erase_if(m_bursts, [](const TriBurstInstance &inst) {
        return inst.burst.particles.empty();
    });

}

void BaClickFxEffect::retireTrails()
{
    // 每次按下保存独立参数，使跨屏后的新拖动不改变已有拖尾寿命。
    for (TrailSession &session : m_trails) {
        session.stream.retireExpired(
            baclickfx::clamp(session.trailParams.lifetimeSec / session.timeScale, 0.001, 60.0));
    }
    std::erase_if(m_trails, [](const TrailSession &session) {
        return !session.active && session.stream.empty();
    });
}

bool BaClickFxEffect::trailEnabled() const
{
    // Unity 侧 FxTrailTimeScale 在 timeScale 低于阈值时直接把拖尾停掉。
    return m_enableTrail && m_timeScale > m_subsystems.trail.killUnderTimeScale;
}

Window *BaClickFxEffect::inputWindowAt(const QPointF &pos) const
{
    // EffectWindow 没有公开的坐标命中接口。本插件本就与 KWin 版本绑定，
    // 因此直接使用 KWin 输入分发的 Window::hitTest()，以正确处理 Wayland
    // input region、子表面和窗口装饰。
    const QList<Window *> &stacking = Workspace::self()->stackingOrder();
    for (auto it = stacking.crbegin(); it != stacking.crend(); ++it) {
        Window *window = *it;
        if (!window || window->isDeleted()
            || !window->isOnCurrentActivity()
            || !window->isOnCurrentDesktop()
            || window->isMinimized()
            || window->isHidden()
            || window->isHiddenByShowDesktop()
            || !window->readyForPainting()) {
            continue;
        }
        if (!window->hitTest(pos)) {
            continue;
        }

        if (logsVerbose()) {
            qCInfo(KWIN_BA_CLICK_FX)
                << "input hit test" << pos
                << "class" << window->resourceClass()
                << "geometry" << window->frameGeometry()
                << "type" << window->windowType()
                << "layer" << window->layer();
        }
        return window;
    }
    return nullptr;
}

bool BaClickFxEffect::isWindowExcluded(const Window *window) const
{
    // 对话框和弹出窗口通常继承主窗口的应用身份。深度上限防御
    // 异常 transient 链环，正常窗口只需要一两次迭代。
    const bool hasProcessRules = std::ranges::any_of(
        m_excludedApplications, [](const baclickfx::ExcludedApplication &rule) {
            return rule.identity.kind == baclickfx::ApplicationIdentityKind::Launcher
                || rule.identity.kind == baclickfx::ApplicationIdentityKind::Process;
        });
    int depth = 0;
    for (const Window *candidate = window;
         candidate && depth < 16;
         candidate = candidate->transientFor(), ++depth) {
        const baclickfx::ApplicationIdentity inexpensiveIdentity =
            baclickfx::identifyApplication(candidate->desktopFileName(),
                                            candidate->resourceClass(),
                                            candidate->resourceName());
        if (baclickfx::isApplicationExcluded(m_excludedApplications, inexpensiveIdentity)) {
            return true;
        }
        // desktop-file 是最高优先级身份；存在时绝不读取或匹配进程规则。
        if (!hasProcessRules || !candidate->desktopFileName().isEmpty()) {
            continue;
        }

        const QUuid windowId = candidate->internalId();
        auto identity = m_processIdentityCache.constFind(windowId);
        if (identity == m_processIdentityCache.cend()) {
            m_processIdentityCache.insert(windowId,
                                          baclickfx::processIdentity(candidate->pid()));
            identity = m_processIdentityCache.constFind(windowId);
        }
        const baclickfx::ApplicationIdentity processIdentity =
            baclickfx::identifyApplication({}, candidate->resourceClass(),
                                            candidate->resourceName(), *identity);
        if (baclickfx::isApplicationExcluded(m_excludedApplications, processIdentity)) {
            return true;
        }
    }
    return false;
}

bool BaClickFxEffect::isEffectAllowedAt(const QPointF &pos, InputPhase phase) const
{
    const bool heldMotion = phase == InputPhase::HeldMotion;
    if (!m_disableOnFullscreen && (heldMotion || (!m_desktopOnly && !m_excludeApplications))) {
        return true;
    }

    Window *window = inputWindowAt(pos);
    if (m_disableOnFullscreen && window && window->isFullScreen()) {
        return false;
    }
    // 桌面/应用过滤仍在按下时确定，保留跨普通窗口拖动的连续性。
    if (heldMotion) {
        return true;
    }

    bool onOutput = false;
    const QPoint pixel = baclickfx::logicalPixelAt(pos);
    for (const LogicalOutput *output : effects->screens()) {
        if (output && output->geometry().contains(pixel)) {
            onOutput = true;
            break;
        }
    }

    // X11 桌面窗口具有 Desktop 类型；Wayland layer-shell 背景层由
    // KWin 放入 DesktopLayer。无窗口的输出区域同样是露出的桌面。
    const bool desktop = onOutput
        && (!window || window->isDesktop() || window->layer() == DesktopLayer);

    if (m_desktopOnly && !desktop) {
        return false;
    }
    // 应用排除不覆盖真实桌面，避免排除 plasmashell 后连壁纸也失效。
    if (m_excludeApplications && window && !desktop && isWindowExcluded(window)) {
        return false;
    }

    return true;
}

void BaClickFxEffect::startDrag(const QPointF &pos, bool awaitFirstSample)
{
    refreshSubsystems(pos);
    m_dragging = true;
    m_motionEventsWork = false;
    m_autoTrailSession = false;
    m_lastDrag = pos;
    // 新的一次按下 = 新的一个 FX_Touch 实例，50 片的名额重新起算。
    m_dragSerial++;

    TrailSession session;
    session.timeScale = m_timeScale;
    session.trailParams = m_subsystems.trail;
    session.ring4Params = m_subsystems.ring4;
    session.emitter = ParticleEmitterState(m_rng.nextSeed());
    session.active = true;
    session.firstSamplePending = awaitFirstSample;
    if (trailEnabled() && !awaitFirstSample) {
        session.stream.beginStroke(pos);
    }
    m_trails.push_back(std::move(session));
}

int BaClickFxEffect::liveDistanceParticles(std::uint64_t dragSerial) const
{
    // maxNumParticles 限制同时存活的粒子数；粒子到期后释放对应名额。
    int live = 0;
    for (const TriBurstInstance &inst : m_bursts) {
        if (inst.dragSerial != dragSerial) {
            continue;
        }
        for (const TriParticle &p : inst.burst.particles) {
            // burst 里所有粒子共用 inst.age，各自寿命不同（startLifetime 是区间随机）。
            if (baclickfx::unity::survives(p.percentAge)) {
                live++;
            }
        }
    }
    return live;
}

void BaClickFxEffect::updateDrag(const QPointF &pos, double elapsed)
{
    if (!m_dragging) {
        return;
    }
    // 呈现帧也要检查过滤规则，窗口可能在两次指针事件之间进入全屏。
    const InputPhase phase = m_autoTrailSession ? InputPhase::Trigger : InputPhase::HeldMotion;
    if (m_pressSuppressed || !isEffectAllowedAt(pos, phase)) {
        endDrag();
        return;
    }

    TrailSession *session = !m_trails.empty() && m_trails.back().active
        ? &m_trails.back()
        : nullptr;
    if (session && session->firstSamplePending) {
        // Unity 在创建特效的 Update 内先定位再播放，首个位置直接作为轨迹起点。
        refreshSubsystems(pos);
        session->trailParams = m_subsystems.trail;
        session->ring4Params = m_subsystems.ring4;
        session->firstSamplePending = false;
        m_lastDrag = pos;
        if (trailEnabled()) {
            session->stream.beginStroke(pos);
        }
    }

    // 保留 KWin 的分数逻辑坐标，避免降低短距离轨迹的采样精度。
    const double dx = pos.x() - m_lastDrag.x();
    const double dy = pos.y() - m_lastDrag.y();
    const double dist = std::hypot(dx, dy);
    const bool moved = dist > 0;
    if (moved) ++m_inputAccepted;
    else ++m_inputDiscarded;
    if (moved && logsFrames()) {
        auto outputAt = [this](const QPointF &point) {
            const QPoint pixel = baclickfx::logicalPixelAt(point);
            for (const LogicalOutput *out : effects->screens()) {
                if (out && out->geometry().contains(pixel)) {
                    return out;
                }
            }
            return static_cast<const LogicalOutput *>(nullptr);
        };
        if (outputAt(m_lastDrag) != outputAt(pos)) {
            ++m_inputCrossScreen;
        }
    }
    if (session && trailEnabled()) {
        TrailStream &stream = session->stream;
        // Unity 每帧至多追加一个端点，阈值比较基于最近存储点的直线距离。
        if (stream.empty()) {
            if (moved || !m_autoTrailSession) {
                stream.beginStroke(pos);
            }
        } else if (stream.head() != pos) {
            const QPointF last = stream.points().back().pos;
            const QPointF delta = pos - last;
            const double minStep = std::max(1e-3, session->trailParams.minVertexDistancePx);
            if (QPointF::dotProduct(delta, delta) > minStep * minStep) {
                stream.addSegment(last, pos);
            } else {
                ++m_inputMerged;
            }
            stream.setHead(pos);
        }
    }

    if (m_enableDistanceEmitter && session && elapsed > 0) {
        const float wallStep = float(elapsed);
        const float simulationStep = float(elapsed * session->timeScale);
        const double unit = session->ring4Params.worldUnitPx;
        const QPointF delta = pos - m_lastDrag;
        const baclickfx::unity::MotionVector emitterVelocity{
            float(delta.x() / unit) / wallStep, -float(delta.y() / unit) / wallStep, 0};
        const auto amount = baclickfx::unity::advanceDistanceEmission(session->emitter.emission,
            float(session->ring4Params.rateOverDistance), emitterVelocity, simulationStep);
        ParticleBirthFrame frame;
        frame.emission = session->emitter.emission;
        frame.rateCount = amount;
        frame.step = simulationStep;
        frame.simulationSpeed = float(session->timeScale);
        frame.emitterVelocity = emitterVelocity;
        const int cap = session->ring4Params.maxParticles;
        const int available = cap > 0 ? std::max(0, cap - liveDistanceParticles(m_dragSerial))
                                     : int(amount);
        const int accepted = int(std::min<std::uint64_t>(std::uint64_t(available), amount));
        if (accepted > 0) {
            auto existing = std::find_if(m_bursts.begin(), m_bursts.end(), [this](const auto &burst) {
                return burst.dragSerial == m_dragSerial;
            });
            if (existing == m_bursts.end()) {
                TriBurstInstance inst;
                inst.center = pos;
                inst.dragSerial = m_dragSerial;
                m_bursts.push_back(std::move(inst));
                existing = std::prev(m_bursts.end());
            }
            const baclickfx::unity::MotionVector offset{
                float((pos.x() - existing->center.x()) / unit),
                -float((pos.y() - existing->center.y()) / unit), 0};
            frame.offset = simulationStep + existing->burst.simulationRemainder;
            appendTriBurst(existing->burst, session->ring4Params, session->timeScale,
                accepted, m_rng, session->emitter, frame, offset);
            existing->life = existing->burst.durationSec;
        }
        // 距离出生和时间发射共用余量 / RNG；源时间 rate 为零，仍逐模拟步更新。
        const float tick = baclickfx::unity::simulationStep(simulationStep,
            baclickfx::unity::sourceMaximumParticleTimestep);
        baclickfx::unity::catchup(simulationStep, tick, 1.f, false, [&](float dt) {
            baclickfx::unity::advanceConstantEmission(session->emitter.emission, {}, 0, dt, 1);
        });
    }

    m_lastDrag = pos;
}

void BaClickFxEffect::handlePointerMotion(const QPointF &position)
{
    // 原生事件及时更新全屏过滤；几何和粒子发射统一在输出帧中处理。
    if (!m_dragging) {
        return;
    }

    const auto t0 = logsFrames() ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    m_motionEventsWork = true;
    ++m_pointerMotionEvents;
    ++m_inputEvents;
    if (!isEffectAllowedAt(position, m_autoTrailSession ? InputPhase::Trigger : InputPhase::HeldMotion))
        endDrag();
    if (logsFrames()) {
        m_inputCpuMsSinceLog += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }
}

void BaClickFxEffect::endDrag()
{
    if (m_dragging) {
        const std::size_t points = !m_trails.empty() ? m_trails.back().stream.points().size() : 0;
        const QRectF bounds = !m_trails.empty()
            ? m_trails.back().stream.boundingBox(6.0)
            : QRectF();
        if (logsInstances()) {
            qCInfo(KWIN_BA_CLICK_FX) << "拖动结束 拖尾点数" << points
                                 << "存活连发" << m_bursts.size()
                                 << "包围盒" << bounds;
        }
    }
    m_dragging = false;
    m_autoTrailSession = false;
    // 保留已有拖尾点自然淡出。
    if (!m_trails.empty()) {
        m_trails.back().active = false;
    }
}

Region BaClickFxEffect::contentRegion()
{
    QList<Rect> bloomRects;
    QList<Rect> letterBoxRects;
    qsizetype trailPointCount = 0;
    for (const TrailSession &session : m_trails) {
        trailPointCount += qsizetype(session.stream.points().size());
    }
    bloomRects.reserve(qsizetype(m_instances.size()) + trailPointCount + 64);
    letterBoxRects.reserve(qsizetype(m_instances.size() + m_bursts.size()));

    m_clickBaseBounds.clear();
    m_clickMeshBounds.clear();
    m_clickTriBounds.clear();
    m_burstBounds.clear();
    m_trailBounds.clear();
    m_clickBaseBounds.reserve(m_instances.size());
    m_clickMeshBounds.reserve(m_instances.size());
    m_clickTriBounds.reserve(m_instances.size());
    m_burstBounds.reserve(m_bursts.size());
    m_trailBounds.reserve(m_trails.size());

    for (const ClickInstance &inst : m_instances) {
        const QRectF base = clickBaseBounds(inst);
        const QRectF mesh = clickMeshBounds(inst, m_meshes.cylinder002);
        m_clickBaseBounds.push_back(base);
        m_clickMeshBounds.push_back(mesh);
        m_clickTriBounds.push_back(triBurstBounds(inst.center, inst.ring3));
        appendDamageRect(letterBoxRects, m_clickTriBounds.back());
        if (base.isNull()) {
            appendDamageRect(bloomRects, mesh);
        } else if (mesh.isNull()) {
            appendDamageRect(bloomRects, base);
        } else {
            appendDamageRect(bloomRects, base.united(mesh));
        }
    }
    for (const TriBurstInstance &inst : m_bursts) {
        m_burstBounds.push_back(triBurstBounds(inst.center, inst.burst));
        appendDamageRect(letterBoxRects, m_burstBounds.back());
    }
    // 相邻线段分别加入 Region，避免整条轨迹退化为总包围盒。pad 覆盖 Ribbon、圆角
    // 和端帽的最大半径，因此仍为保守区域。
    for (TrailSession &session : m_trails) {
        const double pad = std::max(6.0, session.trailParams.widthPx * 5.0);
        m_trailBounds.push_back(session.stream.boundingBox(pad));
        buildTrailStrokes(session.stream, session.trailParams, session.strokes);
        appendTrailDamage(bloomRects, session.stream, pad);
    }

    m_lastBloomSource = Region::fromUnsortedRects(bloomRects);
    return m_lastBloomSource.united(Region::fromUnsortedRects(letterBoxRects));
}

double BaClickFxEffect::bloomPadding() const
{
    // 各输出的 Bloom 半径可能不同；全局重绘区域使用最大值，避免在任一输出上裁短光晕。
    double pad = 0.0;
    for (const LogicalOutput *out : effects->screens()) {
        if (!out) {
            continue;
        }
        const Rect g = out->geometry();
        const double scale = out->scale();
        const QSize devicePx(int(std::lround(g.width() * scale)),
                             int(std::lround(g.height() * scale)));
        if (devicePx.isEmpty()) {
            continue;
        }
        pad = std::max(pad, GPURenderer::bloomReachPx(devicePx, scale));
    }
    // 无可用输出时使用保守回退值。
    return pad > 0.0 ? pad : 340.0;
}

Region BaClickFxEffect::dirtyRegion(const Region &content) const
{
    if (content.isEmpty()) {
        return Region();
    }
    Region dirty;
    Region assigned;
    for (const LogicalOutput *out : effects->screens()) {
        if (!out) {
            continue;
        }
        const Rect geometry = out->geometry();
        const Region onOutput = content.intersected(geometry);
        if (onOutput.isEmpty()) {
            continue;
        }
        const QSize devicePx(int(std::lround(geometry.width() * out->scale())),
                             int(std::lround(geometry.height() * out->scale())));
        if (devicePx.isEmpty()) {
            continue;
        }
        const int pad = int(std::ceil(GPURenderer::bloomReachPx(devicePx, out->scale()))) + 1;
        for (const Rect &rect : onOutput.rects()) {
            dirty += rect.grownBy(QMargins(pad, pad, pad, pad)).intersected(geometry);
        }
        assigned += onOutput;
    }
    const Region unassigned = content.subtracted(assigned);
    if (!unassigned.isEmpty()) {
        const int pad = int(std::ceil(bloomPadding())) + 1;
        for (const Rect &rect : unassigned.rects()) {
            dirty += rect.grownBy(QMargins(pad, pad, pad, pad));
        }
    }
    return dirty;
}

void BaClickFxEffect::resetFrameStats()
{
    m_statFrames = 0;
    m_statCpuMsSum = m_statCpuMsMax = 0.0;
    m_statPrePaintCpuMsSum = m_statSetupCpuMsSum = 0.0;
    m_statTrailCpuMsSum = m_statParticleCpuMsSum = m_statFinishCpuMsSum = 0.0;
    m_inputCpuMsSinceLog = 0.0;
    m_inputEvents = m_inputAccepted = m_inputMerged = m_inputDiscarded = m_inputCrossScreen = 0;
    m_mouseChangedEvents = m_mouseChangedMotion = m_pointerMotionEvents = m_fallbackSamples = 0;
    m_skipNoActivity = m_skipNoDamage = m_skipGpu = m_skipTarget = m_skipImport = 0;
    m_statGpuMsSum = m_statGpuMsMax = 0.0;
    m_statGpuImportMsSum = m_statGpuParticleMsSum = 0.0;
    m_statGpuBloomMsSum = m_statGpuCompositeMsSum = 0.0;
    m_statGpuSamples = 0;
    m_statFrameMsSum = m_statFrameMsMax = 0.0;
    m_statDevicePxSum = m_statRequestPxSum = m_statBloomSourcePxSum = 0.0;
    m_statDeviceRectsSum = m_statRequestRectsSum = m_statBloomSourceRectsSum = 0.0;
    m_outputFrameStats.clear();
}

void BaClickFxEffect::prepareScreen(ScreenPrePaintData &data, std::chrono::nanoseconds now)
{
    const auto cpuStart = logsFrames() ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    // 光标所在输出驱动模拟和采样，混合刷新率屏幕的回调不会插入额外发射批次。
    const QPointF cursor = effects->cursorPos();
    const LogicalOutput *driver = effects->screenAt(baclickfx::logicalPixelAt(cursor));
    const auto tick = m_frameClock.take(reinterpret_cast<std::uintptr_t>(data.screen),
        reinterpret_cast<std::uintptr_t>(driver), now);
    if (tick.accepted) {
        const double elapsed = std::chrono::duration<double>(tick.elapsed).count();
        m_lastFrameDelta = elapsed * 1000.0;
        // TrailRenderer 读取受 Maximum Allowed Timestep 限制的游戏时钟。
        // FX_Touch 粒子系统读取 unscaledDeltaTime，长帧按实际间隔追赶。
        const double trailElapsed = std::min(elapsed,
            double(baclickfx::unity::sourceMaximumAllowedTimestep));
        for (TrailSession &session : m_trails) {
            session.stream.advanceTime(trailElapsed);
        }

        if (m_autoTrailSession && !m_alwaysTrail
            && std::chrono::steady_clock::now() - m_lastAutoTrailMotion > kAutoTrailIdleTimeout) {
            endDrag();
        }
        // 每帧按最终光标位置执行发射；输入回调仍会立即停止跨入全屏的手势。
        if (m_dragging && !isEffectAllowedAt(cursor,
                m_autoTrailSession ? InputPhase::Trigger : InputPhase::HeldMotion)) {
            endDrag();
        }
        const bool firstDragSample = m_dragging && !m_trails.empty()
            && m_trails.back().firstSamplePending;
        for (const PendingClick &click : m_pendingClicks) {
            const bool followsFrameCursor = firstDragSample && !m_autoTrailSession
                && click.firstSampleSerial == m_dragSerial
                && isEffectAllowedAt(cursor);
            spawn(followsFrameCursor ? cursor : click.position);
        }
        m_pendingClicks.clear();
        if (m_dragging && (elapsed > 0 || firstDragSample)) {
            if (!m_motionEventsWork) {
                ++m_inputEvents;
                ++m_fallbackSamples;
            }
            updateDrag(cursor, elapsed);
        }
        // 原生 TrailRenderer 先以本帧时刻落点，再回收过期点。
        retireTrails();
        // 原生距离出生先占用容量，再推进已有及新生粒子；新生年龄先扣掉待推进时间。
        advance(elapsed);
    }

    // 仅申请特效覆盖区域，避免无关窗口参与重合成。
    //
    // 缓存本帧区域供 paintScreen() 使用，确保 KWin 重绘申请与最终合成基于同一快照。
    m_lastContent = contentRegion();
    m_lastDirty = dirtyRegion(m_lastContent);

    // KWin 重绘区为当前区域与上一帧区域的并集。当前区域收缩时，并集中的差集由场景
    // 重绘为背景，而插件仅在 m_lastDirty 内执行最终合成。
    Region repaint = m_lastDirty;
    if (m_paintedLastFrame && !m_lastPainted.isEmpty()) {
        repaint += m_lastPainted;
    }

    // beginFrame 的稀疏背景导入要用这个并集（见 m_lastClearArea 的注释）。
    m_lastClearArea = repaint;

    if (!repaint.isEmpty()) {
        data.paint += repaint;
    }

    if (logsFrames()) {
        m_lastPrePaintCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count();
    }
}

void BaClickFxEffect::logFrameStats(const RenderViewport &viewport,
                                    const Region &deviceRegion, double cpuMs)
{
    // deviceRegion 使用设备像素坐标；逐矩形累加可排除分离区域之间的空白。
    double devicePx = 0.0;
    for (const Rect &r : deviceRegion.rects()) {
        devicePx += double(r.width()) * double(r.height());
    }

    // 申请面积同样逐输出、逐矩形换算，以保留稀疏 Region 排除空白区域的收益。
    double requestPx = 0.0;
    double requestRects = 0.0;
    double bloomSourcePx = 0.0;
    double bloomSourceRects = 0.0;
    for (const LogicalOutput *out : effects->screens()) {
        if (!out) {
            continue;
        }
        const Region onOutput = m_lastDirty.intersected(out->geometry());
        const Region sourceOnOutput = m_lastBloomSource.intersected(out->geometry());
        const double scale2 = out->scale() * out->scale();
        requestRects += onOutput.rects().size();
        bloomSourceRects += sourceOnOutput.rects().size();
        for (const Rect &r : onOutput.rects()) {
            requestPx += double(r.width()) * double(r.height()) * scale2;
        }
        for (const Rect &r : sourceOnOutput.rects()) {
            bloomSourcePx += double(r.width()) * double(r.height()) * scale2;
        }
    }

    const RectF renderRect = viewport.renderRect();
    const QRect alignedOutput = QRectF(renderRect.x(), renderRect.y(),
                                       renderRect.width(), renderRect.height()).toAlignedRect();
    const Rect outputLogical(alignedOutput.x(), alignedOutput.y(),
                             alignedOutput.width(), alignedOutput.height());
    const Region outputRequest = m_lastDirty.intersected(outputLogical);
    const Region outputSource = m_lastBloomSource.intersected(outputLogical);
    const double outputScale2 = viewport.scale() * viewport.scale();
    const Rect scaledOutput = viewport.scaledRenderRect();
    const QString outputLabel = QStringLiteral("%1x%2@%3")
        .arg(scaledOutput.width()).arg(scaledOutput.height()).arg(viewport.scale(), 0, 'f', 2);
    OutputFrameStats &outputStats = m_outputFrameStats[outputLabel];
    outputStats.frames++;
    outputStats.devicePxSum += devicePx;
    outputStats.deviceRects += deviceRegion.rects().size();
    outputStats.requestRects += outputRequest.rects().size();
    outputStats.sourceRects += outputSource.rects().size();
    for (const Rect &r : outputRequest.rects()) {
        outputStats.requestPxSum += double(r.width()) * double(r.height()) * outputScale2;
    }
    for (const Rect &r : outputSource.rects()) {
        outputStats.sourcePxSum += double(r.width()) * double(r.height()) * outputScale2;
    }

    const double gpuMs = m_gpu.lastGpuMs();
    const std::uint64_t gpuSerial = m_gpu.gpuSampleSerial();
    const double frameMs = m_statFrames > 0 && m_lastFrameDelta > 0.0 ? m_lastFrameDelta : 0.0;

    m_statFrames++;
    m_statCpuMsSum += cpuMs;
    m_statCpuMsMax = std::max(m_statCpuMsMax, cpuMs);
    m_statPrePaintCpuMsSum += m_lastPrePaintCpuMs;
    m_statSetupCpuMsSum += m_lastSetupCpuMs;
    m_statTrailCpuMsSum += m_lastTrailCpuMs;
    m_statParticleCpuMsSum += m_lastParticleCpuMs;
    m_statFinishCpuMsSum += m_lastFinishCpuMs;
    if (gpuMs >= 0.0 && gpuSerial != m_lastGpuSampleSerial) {
        m_statGpuMsSum += gpuMs;
        m_statGpuMsMax = std::max(m_statGpuMsMax, gpuMs);
        m_statGpuImportMsSum += m_gpu.lastGpuImportMs();
        m_statGpuParticleMsSum += m_gpu.lastGpuParticleMs();
        m_statGpuBloomMsSum += m_gpu.lastGpuBloomMs();
        m_statGpuCompositeMsSum += m_gpu.lastGpuCompositeMs();
        m_statGpuSamples++;
        OutputFrameStats &sampleOutput = m_outputFrameStats[m_gpu.lastGpuOutput()];
        sampleOutput.gpuImportMsSum += m_gpu.lastGpuImportMs();
        sampleOutput.gpuBloomMsSum += m_gpu.lastGpuBloomMs();
        sampleOutput.gpuCompositeMsSum += m_gpu.lastGpuCompositeMs();
        sampleOutput.gpuSamples++;
        m_lastGpuSampleSerial = gpuSerial;
    }
    m_statFrameMsSum += frameMs;
    m_statFrameMsMax = std::max(m_statFrameMsMax, frameMs);
    m_statDevicePxSum += devicePx;
    m_statRequestPxSum += requestPx;
    m_statBloomSourcePxSum += bloomSourcePx;
    m_statDeviceRectsSum += deviceRegion.rects().size();
    m_statRequestRectsSum += requestRects;
    m_statBloomSourceRectsSum += bloomSourceRects;

    // 每 30 帧汇总一次，避免日志开销明显干扰合成线程。
    if (m_statFrames < 30) {
        return;
    }
    const double n = double(m_statFrames);
    const double avgGpuMs = m_statGpuSamples > 0
        ? m_statGpuMsSum / double(m_statGpuSamples)
        : -1.0;
    const double gpuN = m_statGpuSamples > 0 ? double(m_statGpuSamples) : 1.0;
    const baclickfx::FrameStatsSummary baseSummary{
        .frames = m_statFrames,
        .gpuSamples = m_statGpuSamples,
        .cpuAvgMs = m_statCpuMsSum / n,
        .cpuMaxMs = m_statCpuMsMax,
        .prePaintAvgMs = m_statPrePaintCpuMsSum / n,
        .setupAvgMs = m_statSetupCpuMsSum / n,
        .trailAvgMs = m_statTrailCpuMsSum / n,
        .particleAvgMs = m_statParticleCpuMsSum / n,
        .finishAvgMs = m_statFinishCpuMsSum / n,
        .inputTotalMs = m_inputCpuMsSinceLog,
        .gpuAvgMs = avgGpuMs,
        .gpuMaxMs = m_statGpuSamples > 0 ? m_statGpuMsMax : -1.0,
        .gpuImportAvgMs = m_statGpuSamples > 0 ? m_statGpuImportMsSum / gpuN : -1.0,
        .gpuParticleAvgMs = m_statGpuSamples > 0 ? m_statGpuParticleMsSum / gpuN : -1.0,
        .gpuBloomAvgMs = m_statGpuSamples > 0 ? m_statGpuBloomMsSum / gpuN : -1.0,
        .gpuCompositeAvgMs = m_statGpuSamples > 0 ? m_statGpuCompositeMsSum / gpuN : -1.0,
        .frameIntervalAvgMs = m_statFrameMsSum / n,
        .frameIntervalMaxMs = m_statFrameMsMax,
        .deviceAvgMpx = m_statDevicePxSum / n / 1e6,
        .requestAvgMpx = m_statRequestPxSum / n / 1e6,
        .bloomSourceAvgMpx = m_statBloomSourcePxSum / n / 1e6,
        .deviceAvgRects = m_statDeviceRectsSum / n,
        .requestAvgRects = m_statRequestRectsSum / n,
        .bloomSourceAvgRects = m_statBloomSourceRectsSum / n,
        .inputEvents = m_inputEvents,
        .inputAccepted = m_inputAccepted,
        .inputMerged = m_inputMerged,
        .inputDiscarded = m_inputDiscarded,
        .inputCrossScreen = m_inputCrossScreen,
        .mouseChangedEvents = m_mouseChangedEvents,
        .mouseChangedMotion = m_mouseChangedMotion,
        .pointerMotionEvents = m_pointerMotionEvents,
        .fallbackSamples = m_fallbackSamples,
        .output = QString(),
        .outputGpuImportMs = -1.0,
        .outputGpuBloomMs = -1.0,
        .outputGpuCompositeMs = -1.0,
        .outputDeviceMpx = 0.0,
        .outputRequestMpx = 0.0,
        .outputSourceMpx = 0.0,
        .outputDeviceRects = 0.0,
        .outputRequestRects = 0.0,
        .outputSourceRects = 0.0,
        .skipNoActivity = m_skipNoActivity,
        .skipNoDamage = m_skipNoDamage,
        .skipGpu = m_skipGpu,
        .skipTarget = m_skipTarget,
        .skipImport = m_skipImport,
    };
    for (auto it = m_outputFrameStats.cbegin(); it != m_outputFrameStats.cend(); ++it) {
        baclickfx::FrameStatsSummary summary = baseSummary;
        const OutputFrameStats &output = it.value();
        const double outputN = output.frames > 0 ? double(output.frames) : 1.0;
        const double outputGpuN = output.gpuSamples > 0 ? double(output.gpuSamples) : 1.0;
        summary.output = it.key();
        summary.outputGpuImportMs = output.gpuSamples > 0
            ? output.gpuImportMsSum / outputGpuN : -1.0;
        summary.outputGpuBloomMs = output.gpuSamples > 0
            ? output.gpuBloomMsSum / outputGpuN : -1.0;
        summary.outputGpuCompositeMs = output.gpuSamples > 0
            ? output.gpuCompositeMsSum / outputGpuN : -1.0;
        summary.outputDeviceMpx = output.devicePxSum / outputN / 1e6;
        summary.outputRequestMpx = output.requestPxSum / outputN / 1e6;
        summary.outputSourceMpx = output.sourcePxSum / outputN / 1e6;
        summary.outputDeviceRects = double(output.deviceRects) / outputN;
        summary.outputRequestRects = double(output.requestRects) / outputN;
        summary.outputSourceRects = double(output.sourceRects) / outputN;
        qCInfo(KWIN_BA_CLICK_FX).noquote() << baclickfx::formatFrameStats(summary);
    }

    resetFrameStats();
}

void BaClickFxEffect::paintOverlay(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                  const Region &deviceRegion)
{
    if (m_instances.empty() && m_bursts.empty() && m_trails.empty()) {
        ++m_skipNoActivity;
        return;
    }

    // Shader 和纹理需要有效 OpenGL 上下文，因此在首次绘制时初始化。失败后不在每帧
    // 重试；插件没有 CPU 渲染回退。
    if (!m_gpuTried) {
        m_gpuTried = true;
        m_gpuReady = m_gpu.initialize();
        if (!m_gpuReady) {
            qCWarning(KWIN_BA_CLICK_FX)
                << "event=error component=effect code=gpu_renderer_init_failed"
                << "action=rendering_disabled";
        }
    }
    if (!m_gpuReady) {
        ++m_skipGpu;
        return;
    }

    if (!logsFrames()) {
        renderGpu(renderTarget, viewport);
        if (m_debugDamage) {
            drawDebugDamage(renderTarget, viewport);
        }
        return;
    }

    // 帧统计级别下测量 paintScreen() 的 CPU 墙钟时间。
    const auto t0 = std::chrono::steady_clock::now();
    renderGpu(renderTarget, viewport);
    const double cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logFrameStats(viewport, deviceRegion, cpuMs);
    if (m_debugDamage) {
        drawDebugDamage(renderTarget, viewport);
    }
}

void BaClickFxEffect::drawDebugDamage(const RenderTarget &renderTarget,
                                      const RenderViewport &viewport)
{
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    GLShader *shader = ShaderManager::instance()->pushShader(
        ShaderTrait::UniformColor | ShaderTrait::TransformColorspace);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix,
                       viewport.projectionMatrix());
    shader->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(),
                                  RenderingIntent::Perceptual);

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldBlendSrcRgb = GL_ONE;
    GLint oldBlendDstRgb = GL_ZERO;
    GLint oldBlendSrcAlpha = GL_ONE;
    GLint oldBlendDstAlpha = GL_ZERO;
    GLint oldBlendEquationRgb = GL_FUNC_ADD;
    GLint oldBlendEquationAlpha = GL_FUNC_ADD;
    glGetIntegerv(GL_BLEND_SRC_RGB, &oldBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &oldBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &oldBlendEquationRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &oldBlendEquationAlpha);
    GLfloat oldLineWidth = 1.0f;
    glGetFloatv(GL_LINE_WIDTH, &oldLineWidth);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(2.0f);

    const auto drawRect = [&](float left, float top, float right, float bottom,
                              const QVector4D &color) {
        // 向内缩 1 设备像素，避免刚好落在裁剪边界上的线有一半被切掉。
        if (right - left > 2.0f && bottom - top > 2.0f) {
            left += 1.0f;
            top += 1.0f;
            right -= 1.0f;
            bottom -= 1.0f;
        }
        const QList<QVector2D> vertices = {
            {left, top}, {right, top}, {right, bottom}, {left, bottom},
        };
        vbo->reset();
        vbo->setVertices(vertices);
        shader->setUniform(GLShader::ColorUniform::Color, color);
        vbo->render(GL_LINE_LOOP);
    };

    // m_lastDirty 是全局逻辑坐标；RenderViewport 的投影吃设备像素坐标。
    const RectF rr = viewport.renderRect();
    const QRectF outputRect(rr.x(), rr.y(), rr.width(), rr.height());
    const QRect alignedOutput = outputRect.toAlignedRect();
    const Rect outputLogical(alignedOutput.x(), alignedOutput.y(),
                             alignedOutput.width(), alignedOutput.height());
    const Region requested = m_lastDirty.intersected(outputLogical);
    const float scale = float(viewport.scale());
    for (const Rect &r : requested.rects()) {
        drawRect(float(r.left()) * scale, float(r.top()) * scale,
                 float(r.right()) * scale, float(r.bottom()) * scale,
                 QVector4D(0.0f, 1.0f, 1.0f, 0.9f));
    }

    glLineWidth(oldLineWidth);
    glBlendEquationSeparate(oldBlendEquationRgb, oldBlendEquationAlpha);
    glBlendFuncSeparate(oldBlendSrcRgb, oldBlendDstRgb,
                        oldBlendSrcAlpha, oldBlendDstAlpha);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
    ShaderManager::instance()->popShader();
}

bool BaClickFxEffect::renderGpu(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
    using CpuClock = std::chrono::steady_clock;
    auto phaseStart = logsFrames() ? CpuClock::now() : CpuClock::time_point{};
    m_lastSetupCpuMs = 0.0;
    m_lastTrailCpuMs = 0.0;
    m_lastParticleCpuMs = 0.0;
    m_lastFinishCpuMs = 0.0;
    const auto phaseMs = [&]() {
        if (!logsFrames()) {
            return 0.0;
        }
        const auto now = CpuClock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - phaseStart).count();
        phaseStart = now;
        return ms;
    };

    // KWin::RectF 不是 QRectF，而且它的 right()/bottom() 等于 x()+width()
    // （与 QRect 相反，见 core/rect.h 的类注释）。按分量显式构造一个 QRectF，
    // 后面全按 Qt 语义用。
    const RectF rr = viewport.renderRect();
    const QRectF outputRect(rr.x(), rr.y(), rr.width(), rr.height());

    // 内容未与当前输出相交时跳过整条渲染链。判定使用原始几何区域即可，因为空输出
    // 不会产生 Bloom 亮源。
    const QRect alignedOutput = outputRect.toAlignedRect();
    const Rect outputLogical(alignedOutput.x(), alignedOutput.y(),
                             alignedOutput.width(), alignedOutput.height());
    if (!m_lastContent.intersects(outputLogical)) {
        ++m_skipNoDamage;
        m_lastSetupCpuMs = phaseMs();
        return false;
    }

    // HDR FBO 与 Bloom 金字塔按整块输出分配并逐输出复用。
    if (!m_gpu.prepareTargets(renderTarget, viewport)) {
        ++m_skipTarget;
        m_lastSetupCpuMs = phaseMs();
        return false;
    }

    const QPointF origin = outputRect.topLeft();

    Q_ASSERT(m_trailBounds.size() == m_trails.size());
    Q_ASSERT(m_clickBaseBounds.size() == m_instances.size());
    Q_ASSERT(m_clickMeshBounds.size() == m_instances.size());
    Q_ASSERT(m_clickTriBounds.size() == m_instances.size());
    Q_ASSERT(m_burstBounds.size() == m_bursts.size());

    // 背景导入使用 m_lastClearArea，以覆盖当前 dirty 之外的上一帧粒子。导入失败时
    // 跳过当前帧，避免将未初始化的私有 HDR 内容覆盖到屏幕。
    if (!m_gpu.beginFrame(renderTarget, viewport, m_lastClearArea)) {
        ++m_skipImport;
        m_lastSetupCpuMs = phaseMs();
        return false;
    }
    m_lastSetupCpuMs = phaseMs();

    // 原始材质队列：Trail / Ring / MeshTri = 4499；Ring3 / Ring4 = 4550。
    for (std::size_t i = 0; i < m_trails.size(); i++) {
        const TrailSession &session = m_trails[i];
        if (session.stream.empty()) {
            continue;
        }
        if (!m_trailBounds[i].intersects(outputRect)) {
            continue;
        }
        m_gpu.renderTrail(session.strokes, session.trailParams, origin);
    }
    m_lastTrailCpuMs = phaseMs();

    for (std::size_t i = 0; i < m_instances.size(); i++) {
        ClickInstance &inst = m_instances[i];
        if (m_clickBaseBounds[i].intersects(outputRect)) {
            m_gpu.renderClickBase(inst, origin);
        }
    }

    // UIAlwaysVisible 的同队列元素采用稳定的宿主顺序。
    for (std::size_t i = 0; i < m_instances.size(); i++) {
        const ClickInstance &inst = m_instances[i];
        if (m_clickMeshBounds[i].intersects(outputRect)) {
            m_gpu.renderClickMeshTri(inst, m_meshes.cylinder002, origin);
        }
    }
    // 原版 LetterBox（Ring3 / Ring4）在 UI Bloom 之后绘制。
    m_lastParticleCpuMs = phaseMs();
    const bool bloomOk = m_gpu.updateBloom(renderTarget, m_lastBloomSource);
    m_lastFinishCpuMs = phaseMs();
    for (std::size_t i = 0; i < m_instances.size(); i++) {
        const ClickInstance &inst = m_instances[i];
        if (m_clickTriBounds[i].intersects(outputRect)) {
            m_gpu.renderClickAdditive(inst, origin);
        }
    }
    for (std::size_t i = 0; i < m_bursts.size(); i++) {
        TriBurstInstance &inst = m_bursts[i];
        if (m_burstBounds[i].intersects(outputRect)) {
            m_gpu.renderTriBurst(inst, origin);
        }
    }
    m_gpu.flushTriBursts();
    m_lastParticleCpuMs += phaseMs();

    m_gpu.endFrame(renderTarget, viewport, m_lastDirty, bloomOk);
    m_lastFinishCpuMs += phaseMs();
    m_paintedLastFrame = true;
    // 保存本帧合成区域，下一帧将其并入 KWin 重绘区以清除收缩后的差集。只保存最近
    // 一帧，避免历史区域持续扩大最终合成范围。
    m_lastPainted = m_lastDirty;
    return true;
}

void BaClickFxEffect::postPaintScreen()
{
    // postPaintScreen() 只请求下一帧所需的最小区域；下一帧的完整 dirty 区域在
    // prePaintScreen() 推进模拟后加入 ScreenPrePaintData::paint。
    const auto requestRepaint = [](const Region &r) {
        if (r.isEmpty()) {
            return;
        }
        effects->addRepaint(r);
    };

    if (isActive()) {
        requestRepaint(m_lastDirty);
        // 旧粒子可能只留在另一块屏幕；光标所在输出仍须继续提供呈现时钟。
        // 已覆盖光标时该点会合并进已有重绘区域。
        const QPointF cursor = effects->cursorPos();
        effects->addRepaint(Rect(baclickfx::logicalPixelAt(cursor), QSize(1, 1)));
    } else if (m_paintedLastFrame) {
        // 最后一帧：再要一次重绘，把残影擦掉。范围是上一帧真正铺过的那块。
        requestRepaint(m_lastPainted);
        m_paintedLastFrame = false;
        m_lastPainted = Region();
        m_lastDirty = Region();
        m_lastContent = Region();
        m_lastBloomSource = Region();
        m_lastClearArea = Region();
    }

    if (!isActive()) m_frameClock.reset();
    effects->postPaintScreen();
}

void BaClickFxEffect::slotMouseChanged(const QPointF &pos, const QPointF &oldPos,
                                       Qt::MouseButtons buttons, Qt::MouseButtons oldButtons,
                                       Qt::KeyboardModifiers, Qt::KeyboardModifiers)
{
    ++m_mouseChangedEvents;
    if (pos != oldPos) {
        ++m_mouseChangedMotion;
    }
    const auto transition = baclickfx::mouseButtonTransitions(buttons, oldButtons, m_triggerButtons);
    const bool isDown = transition.held;

    if (transition.startsGesture()) {
        // 桌面/应用许可在按下时锁定，全屏抑制还会在拖动过程中实时检查。
        m_pressSuppressed = !isEffectAllowedAt(pos);
        if (m_pressSuppressed) {
            // 配置变更或窗口在光标下出现时，可能仍留有 AlwaysTrail 会话。
            // 结束它可确保后续 pointerMotion 不会绕过本次按下的过滤结果。
            if (m_dragging) {
                endDrag();
            }
            return;
        }
        // AlwaysTrail 会话已经在鼠标移动时建立；首个触发键按下只改变会话模式，
        // 不要结束并重新创建拖尾，否则一次点击或切换输入模式会产生断裂的新轨迹。
        if (!m_dragging) {
            startDrag(pos, true);
        }
        m_autoTrailSession = false;
    } else if (transition.endsGesture()) {
        const bool suppressed = m_pressSuppressed;
        m_pressSuppressed = false;
        if (suppressed) {
            return;
        }
        if (m_alwaysTrail && trailEnabled() && m_dragging) {
            // 全部触发键释放后继续复用同一会话，切回自动拖尾模式。静止期间保留会话，
            // 下一次移动继续同一笔划，不产生断点或新的 TrailSession。
            m_autoTrailSession = true;
            m_lastAutoTrailMotion = std::chrono::steady_clock::now();
        } else {
            endDrag();
        }
        return;
    }

    if (transition.pressed && !m_pressSuppressed) {
        if (!isEffectAllowedAt(pos, InputPhase::HeldMotion)) {
            endDrag();
            return;
        }
        // 第二个触发键不重建活动拖尾；若此前因全屏停止，则从当前位置起笔。
        if (!m_dragging) {
            startDrag(pos);
        }
        m_autoTrailSession = false;
        if (m_enableClick && isEffectAllowedAt(pos)) {
            for (const auto button : {Qt::LeftButton, Qt::RightButton, Qt::MiddleButton}) {
                if (transition.pressed.testFlag(button)) {
                    const auto serial = !m_trails.empty() && m_trails.back().firstSamplePending
                        ? m_dragSerial : std::uint64_t(0);
                    m_pendingClicks.push_back({pos, serial});
                }
            }
        }
        // 从非活动状态切换后启动帧循环；完整区域由 prePaintScreen() 申请。
        effects->addRepaint(Rect(baclickfx::logicalPixelAt(pos), QSize(1, 1)));
    }

    if (!transition.startsGesture() && pos != oldPos
        && (isDown || (m_alwaysTrail && trailEnabled()))) {
        // 保留按下时的桌面/应用许可；全屏抑制对按住和悬停两种轨迹都实时生效。
        if (isDown && m_pressSuppressed) {
            return;
        }
        const InputPhase phase = isDown ? InputPhase::HeldMotion : InputPhase::Trigger;
        if (!isEffectAllowedAt(pos, phase)) {
            // 结束而不是暂停会话，避免回到桌面时连接一条跨窗口的长线。
            if (m_dragging) {
                endDrag();
            }
            return;
        }
        if (!m_dragging) {
            // 从非桌面区域进入桌面时从当前位置起笔，避免跨过窗口连线。
            // 普通起笔仍保留 oldPos 到 pos 的首段移动。
            const QPointF start = !isEffectAllowedAt(oldPos, phase) ? pos : oldPos;
            startDrag(start);
            m_autoTrailSession = !isDown;
            effects->addRepaint(Rect(baclickfx::logicalPixelAt(pos), QSize(1, 1)));
        }
        if (!isDown) {
            m_autoTrailSession = true;
            m_lastAutoTrailMotion = std::chrono::steady_clock::now();
        }
        const auto t0 = logsFrames() ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
        m_motionEventsWork = true;
        ++m_inputEvents;
        // 只记录输入活动；prepareScreen 每帧读取一次最新光标位置。
        if (logsFrames()) {
            m_inputCpuMsSinceLog += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        }
    }
}

} // namespace KWin

#include "moc_baclickfxeffect.cpp"
