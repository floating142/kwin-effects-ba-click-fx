// SPDX-License-Identifier: GPL-3.0-or-later

#include "baclickfxeffect.h"
#include "baclickfxdefaults.h"

#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <input_event.h>
#include <opengl/glshader.h>
#include <opengl/glvertexbuffer.h>
#include <opengl/glshadermanager.h>
// prePaintScreen() 使用 RenderView 的呈现时间戳，因此需要完整类型定义。
#include <scene/scene.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QStandardPaths>
#include <QVector2D>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(KWIN_BA_CLICK_FX, "kwin_effect_ba_click_fx", QtInfoMsg)

namespace KWin
{

namespace
{

void unitePointBounds(QRectF &bounds, double x, double y)
{
    const QRectF point(x, y, 0.0001, 0.0001);
    bounds = bounds.isNull() ? point : bounds.united(point);
}

QRectF triBurstBounds(const QPointF &center, const TriBurstEmission &burst, double ageSec)
{
    QRectF bounds;
    for (const TriParticle &item : burst.particles) {
        const double progress = layerProgress(item.durationSec, ageSec);
        if (progress >= 1.0) {
            continue;
        }

        // 与 GPURenderer::renderTriBurstGeometry 使用完全相同的位置和尺寸公式。
        const double dist = item.speedPxPerSec * item.lifetimeSec * progress;
        const double radius = burst.params.localScalePx * (burst.params.shapeScale + dist);
        const double x = center.x() + std::cos(item.angle) * radius;
        const double y = center.y() + std::sin(item.angle) * radius;
        const double side = burst.params.sizePx * item.sizeMul
            * std::max(0.0, baclickfx::subSizeCurve(burst.params, progress));
        const double half = side * 0.5;

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

QRectF clickBounds(const ClickInstance &inst, const baclickfx::CylinderProfile &profile)
{
    QRectF bounds;

    // Ring：与 renderRing 相同的旋转正方形。
    const double ringP = layerProgress(inst.ring.durationSec, inst.age);
    if (ringP < 1.0) {
        const double half = inst.ring.params.sizePx
            * std::max(0.0, baclickfx::subSizeCurve(inst.ring.params, ringP));
        const double c = std::cos(inst.ring.startRotationRad);
        const double s = std::sin(inst.ring.startRotationRad);
        for (const auto &[sx, sy] : {std::pair{-half, -half}, std::pair{half, -half},
                                    std::pair{-half, half}, std::pair{half, half}}) {
            unitePointBounds(bounds, inst.center.x() + sx * c - sy * s,
                             inst.center.y() + sx * s + sy * c);
        }
    }

    const QRectF ring3 = triBurstBounds(inst.center, inst.ring3, inst.age);
    if (!ring3.isNull()) {
        bounds = bounds.isNull() ? ring3 : bounds.united(ring3);
    }

    // MeshTri：逐弧段、逐 OBJ 顶点执行与 renderMeshTri 相同的缩放和旋转。
    const double meshP = layerProgress(inst.meshTri.durationSec, inst.age);
    if (meshP < 1.0) {
        for (const MeshTriArc &arc : inst.meshTri.arcs) {
            const double localP = baclickfx::clamp(meshP + arc.phase * 0.08, 0.0, 1.0);
            const double scale = inst.meshTri.params.sizePx * arc.radiusMul
                * std::max(0.0, baclickfx::subSizeCurve(inst.meshTri.params, localP));
            const double rotation = arc.angle
                + meshTriRotation(inst.meshTri, localP, arc.rotMix);
            const double c = std::cos(rotation);
            const double s = std::sin(rotation);
            for (const baclickfx::Vertex &p : profile.mesh.vertices) {
                const double x = p[0] * scale;
                const double y = p[1] * scale;
                unitePointBounds(bounds, inst.center.x() + x * c - y * s,
                                 inst.center.y() + x * s + y * c);
            }
        }
    }

    return bounds;
}

// 稀疏 damage 的量化网格。包围盒只向网格边界扩展，不会裁掉可见像素；64 像素网格
// 可保留长对角拖尾的窄带形状，同时限制 Region 的矩形数量和运算开销。
constexpr int kDamageTilePx = 64;

Rect damageTileRect(const QRectF &bounds)
{
    if (bounds.isNull() || bounds.isEmpty()) {
        return Rect();
    }
    const int left = int(std::floor(bounds.left() / kDamageTilePx)) * kDamageTilePx;
    const int top = int(std::floor(bounds.top() / kDamageTilePx)) * kDamageTilePx;
    const int right = int(std::ceil(bounds.right() / kDamageTilePx)) * kDamageTilePx;
    const int bottom = int(std::ceil(bounds.bottom() / kDamageTilePx)) * kDamageTilePx;
    return Rect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

void appendDamageRect(QList<Rect> &rects, const QRectF &bounds)
{
    const Rect tiled = damageTileRect(bounds);
    if (!tiled.isEmpty()) {
        rects.append(tiled);
    }
}

void appendTrailDamage(QList<Rect> &rects, const TrailStream &stream, double pad)
{
    const std::vector<TrailPoint> &points = stream.points();
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

    if (m_debugLog) {
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
        if (m_debugLog) {
            qCInfo(KWIN_BA_CLICK_FX) << "配置重载，已申请擦除上一帧脏区";
        }
    }

    if (m_debugLog) {
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

    if (m_debugLog) {
        qCInfo(KWIN_BA_CLICK_FX) << "网格已加载 cylinder 面数"
                             << m_meshes.cylinder002.mesh.faces.size();
    }
}

void BaClickFxEffect::loadConfig()
{
    namespace def = baclickfx::defaults;

    const KConfigGroup group = KSharedConfig::openConfig(QStringLiteral("kwinrc"))
                                   ->group(QLatin1String(def::kGroup));
    m_debugLog = group.readEntry(def::kDebugLog, def::kDebugLogDefault);
    m_debugDamage = group.readEntry(def::kDebugDamage, def::kDebugDamageDefault);

    // 运行时限制与配置页控件共用 baclickfxdefaults.h 中的取值范围。
    m_timeScale = baclickfx::clamp(group.readEntry(def::kTimeScale, def::kTimeScaleDefault),
                                 def::kTimeScaleMin, def::kTimeScaleMax);
    m_globalScale = baclickfx::clamp(group.readEntry(def::kGlobalScale, def::kGlobalScaleDefault),
                                   def::kGlobalScaleMin, def::kGlobalScaleMax);

    // Unity 已定义的视觉参数保持固定；以下配置只控制对应图层是否绘制。
    m_enableTrail = group.readEntry(def::kEnableTrail, def::kEnableTrailDefault);
    m_enableDistanceEmitter =
        group.readEntry(def::kEnableDistanceEmitter, def::kEnableDistanceEmitterDefault);

    // GPU 计时随 DebugLog 开关。查询对象需要 OpenGL 上下文，因此在首次 beginFrame()
    // 中延迟创建。
    m_gpu.setProfiling(m_debugLog);
    m_statFrames = 0;
    m_statCpuMsSum = m_statCpuMsMax = 0.0;
    m_statPrePaintCpuMsSum = 0.0;
    m_statSetupCpuMsSum = 0.0;
    m_statTrailCpuMsSum = 0.0;
    m_statParticleCpuMsSum = 0.0;
    m_statFinishCpuMsSum = 0.0;
    m_inputCpuMsSinceLog = 0.0;
    m_statGpuMsSum = m_statGpuMsMax = 0.0;
    m_statGpuImportMsSum = 0.0;
    m_statGpuParticleMsSum = 0.0;
    m_statGpuBloomMsSum = 0.0;
    m_statGpuCompositeMsSum = 0.0;
    m_statGpuSamples = 0;
    m_lastGpuSampleSerial = m_gpu.gpuSampleSerial();
    m_statFrameMsSum = m_statFrameMsMax = 0.0;
    m_statDevicePxSum = m_statRequestPxSum = m_statBloomSourcePxSum = 0.0;
    m_statDeviceRectsSum = m_statRequestRectsSum = m_statBloomSourceRectsSum = 0.0;
    m_lastFrameDelta = 0.0;

    // 活动实例依赖当前参数表，重建前先清空以避免引用失效。
    m_instances.clear();
    m_bursts.clear();
    // 参数变化会改变拖尾宽度和寿命，因此同时清空现有拖尾。
    m_trails.clear();
    m_trailEmitValid = false;
    m_trailAccum = 0;
    m_distAccum = 0;
    m_subsystemHeightPx = 0.0;
    ensureSubsystemsForHeight(outputHeightForPos(effects->cursorPos()));

    if (m_debugLog) {
        qCInfo(KWIN_BA_CLICK_FX) << "配置已重载 timeScale" << m_timeScale
                             << "globalScale" << m_globalScale
                             << "worldUnitPx" << m_subsystems.ring.worldUnitPx
                             << debug(QStringLiteral("status"));
    }
}

double BaClickFxEffect::outputHeightForPos(const QPointF &pos) const
{
    // Unity 固定 UI Pass 的垂直范围为两个世界单位，因此一个世界单位等于输出逻辑
    // 高度的一半，特效在不同分辨率下保持相同占屏比例。
    const LogicalOutput *first = nullptr;
    for (const LogicalOutput *out : effects->screens()) {
        if (!out) {
            continue;
        }
        if (!first) {
            first = out;
        }
        const Rect g = out->geometry();
        const QRectF geo(g.x(), g.y(), g.width(), g.height());
        if (geo.contains(pos)) {
            return geo.height();
        }
    }
    if (first) {
        return first->geometry().height();
    }
    return baclickfx::kReferenceHeightPx;
}

void BaClickFxEffect::ensureSubsystemsForHeight(double heightPx)
{
    const double h = heightPx > 0.0 ? heightPx : baclickfx::kReferenceHeightPx;
    if (std::abs(h - m_subsystemHeightPx) < 0.5) {
        return;
    }
    m_subsystemHeightPx = h;
    // 就地重建保持 Subsystem 成员地址稳定；曲线视图仍指向静态数据。
    m_subsystems = baclickfx::buildSubsystemMap(m_timeScale, m_globalScale, h);
}

bool BaClickFxEffect::isActive() const
{
    // 拖动中即使屏上暂时空着也要保持活跃：光标位置是按帧轮询的，
    // 一旦停帧就再也收不到移动，距离发射器和拖尾都会停在原地。
    return m_dragging || !m_instances.empty() || !m_bursts.empty() || !m_trails.empty();
}

int BaClickFxEffect::requestedEffectChainPosition() const
{
    // 画在最上层，盖住窗口内容。
    return 99;
}

QString BaClickFxEffect::debug(const QString &parameter) const
{
    const QString status = QStringLiteral("build=%1 debugLog=%2 debugDamage=%3 gpuReady=%4 active=%5")
        .arg(QStringLiteral(BA_CLICK_FX_BUILD_ID))
        .arg(m_debugLog)
        .arg(m_debugDamage)
        .arg(m_gpuReady)
        .arg(isActive());
    if (parameter.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0 && m_debugLog) {
        qCInfo(KWIN_BA_CLICK_FX) << "日志测试" << status;
    }
    return status;
}

void BaClickFxEffect::spawn(const QPointF &pos)
{
    // 点击落在哪块屏，就按那块屏的高度换算世界单位。
    ensureSubsystemsForHeight(outputHeightForPos(pos));
    ClickInstance inst = makeClickInstance(pos, m_subsystems, m_timeScale, m_rng);
    if (m_debugLog) {
        qCInfo(KWIN_BA_CLICK_FX) << "起实例" << pos << "时长" << inst.life
                             << "秒 ring3 粒子" << inst.ring3.particles.size()
                             << "meshTri 弧段" << inst.meshTri.arcs.size();
    }
    m_instances.push_back(std::move(inst));
}

void BaClickFxEffect::advance(double dt)
{
    for (ClickInstance &inst : m_instances) {
        // 还没上过屏的实例先欠一帧，让它的第一帧落在 age=0，见 ClickInstance::drawn。
        if (!inst.drawn) {
            continue;
        }
        inst.age += dt;
    }
    std::erase_if(m_instances, [](const ClickInstance &inst) {
        return inst.age >= inst.life;
    });

    for (TriBurstInstance &inst : m_bursts) {
        if (!inst.drawn) {
            continue;
        }
        inst.age += dt;
    }
    std::erase_if(m_bursts, [](const TriBurstInstance &inst) {
        return inst.age >= inst.life;
    });

    // 每次按下保存独立参数，使跨屏后的新拖动不改变已有拖尾寿命。
    for (TrailSession &session : m_trails) {
        session.stream.advance(
            dt, baclickfx::clamp(session.trailParams.lifetimeSec / m_timeScale, 0.001, 60.0));
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

void BaClickFxEffect::startDrag(const QPointF &pos)
{
    m_dragging = true;
    m_lastDrag = pos;
    m_trailEmit = pos;
    m_trailEmitValid = true;
    m_trailAccum = 0;
    m_distAccum = 0;
    // 新的一次按下 = 新的一个 FX_Touch 实例，50 片的名额重新起算。
    m_dragSerial++;

    TrailSession session;
    session.trailParams = m_subsystems.trail;
    session.ring4Params = m_subsystems.ring4;
    session.active = true;
    if (trailEnabled()) {
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
            if (inst.age < p.durationSec) {
                live++;
            }
        }
    }
    return live;
}

void BaClickFxEffect::updateDrag(const QPointF &pos)
{
    if (!m_dragging) {
        return;
    }

    // 保留 KWin 的分数逻辑坐标，避免降低短距离轨迹的采样精度。
    const double dx = pos.x() - m_lastDrag.x();
    const double dy = pos.y() - m_lastDrag.y();
    const double dist = std::hypot(dx, dy);
    if (dist <= 0) {
        return;
    }
    const double dirX = dx / dist;
    const double dirY = dy / dist;

    TrailSession *session = !m_trails.empty() && m_trails.back().active
        ? &m_trails.back()
        : nullptr;

    if (trailEnabled() && session) {
        // 按 TrailRenderer.m_MinVertexDistance 的累计路程离散采样。
        const double minStep = std::max(1e-3, session->trailParams.minVertexDistancePx);
        double remain = dist;
        QPointF walk = m_lastDrag;

        while (m_trailAccum + remain >= minStep) {
            const double step = minStep - m_trailAccum;
            const QPointF next(walk.x() + dirX * step, walk.y() + dirY * step);
            if (m_trailEmitValid) {
                session->stream.addSegment(m_trailEmit, next);
            }
            m_trailEmit = next;
            m_trailEmitValid = true;
            walk = next;
            remain -= step;
            m_trailAccum = 0;
        }

        m_trailAccum += remain;
    }

    if (m_enableDistanceEmitter && session) {
        const double stepPx = distanceStepFromRing4(session->ring4Params);
        if (std::isfinite(stepPx)) {
            // rateOverDistance 的每次发射发生在路径跨过阈值的那个位置，不是本次
            // pointer event 的终点。逐个求线段交点，保证低频长事件和高频短事件
            // 得到相同的空间分布。
            double walked = 0.0;
            double remaining = dist;
            while (m_distAccum + remaining >= stepPx) {
                const double toEmission = stepPx - m_distAccum;
                walked += toEmission;
                remaining -= toEmission;
                m_distAccum = 0.0;
                const QPointF emissionPos(m_lastDrag.x() + dirX * walked,
                                          m_lastDrag.y() + dirY * walked);

                // maxNumParticles 的作用域是本次按下创建的 ParticleSystem；满员时跳过发射。
                const int cap = session->ring4Params.maxParticles;
                const int budget = cap > 0
                    ? std::max(0, cap - liveDistanceParticles(m_dragSerial))
                    : -1;
                if (budget != 0) {
                    TriBurstInstance inst = makeTriBurstInstance(emissionPos, session->ring4Params,
                                                                 1, m_timeScale,
                                                                 budget, m_rng);
                    inst.dragSerial = m_dragSerial;
                    m_bursts.push_back(std::move(inst));
                }
            }
            m_distAccum += remaining;
        }
    }

    m_lastDrag = pos;
}

void BaClickFxEffect::pointerMotion(PointerMotionEvent *event)
{
    // 输入事件采样频率可高于帧率，因此能够保留快速拖动中的帧内转折点。
    if (!m_dragging) {
        return;
    }

    const auto t0 = m_debugLog ? std::chrono::steady_clock::now()
                               : std::chrono::steady_clock::time_point{};
    m_pointerMotionWorks = true;
    updateDrag(event->position);
    if (m_debugLog) {
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
        if (m_debugLog) {
            qCInfo(KWIN_BA_CLICK_FX) << "拖动结束 拖尾点数" << points
                                 << "存活连发" << m_bursts.size()
                                 << "包围盒" << bounds;
        }
    }
    m_dragging = false;
    // 保留已有拖尾点自然淡出，只清除后续路径连接状态。
    m_trailAccum = 0;
    m_distAccum = 0;
    m_trailEmitValid = false;
    if (!m_trails.empty()) {
        m_trails.back().active = false;
    }
}

Region BaClickFxEffect::contentRegion() const
{
    QList<Rect> rects;
    rects.reserve(qsizetype(m_instances.size() + m_bursts.size() + 64));

    for (const ClickInstance &inst : m_instances) {
        appendDamageRect(rects, clickBounds(inst, m_meshes.cylinder002));
    }
    for (const TriBurstInstance &inst : m_bursts) {
        appendDamageRect(rects, triBurstBounds(inst.center, inst.burst, inst.age));
    }
    // 相邻线段分别加入 Region，避免整条轨迹退化为总包围盒。pad 覆盖 Ribbon、圆角
    // 和端帽的最大半径，因此仍为保守区域。
    for (const TrailSession &session : m_trails) {
        appendTrailDamage(rects, session.stream,
                          std::max(6.0, session.trailParams.widthPx * 5.0));
    }

    return Region::fromUnsortedRects(rects);
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
    // Region::grownBy 会逐矩形外扩后重新求并集。向上取整确保设备/逻辑换算中的
    // 小数 Bloom 半径不被截短；再多 1px 覆盖双线性采样边界和 toAlignedRect。
    const int pad = int(std::ceil(bloomPadding())) + 1;
    return content.grownBy(QMargins(pad, pad, pad, pad));
}

void BaClickFxEffect::prePaintScreen(ScreenPrePaintData &data)
{
    const auto cpuStart = m_debugLog ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    // 使用合成器的呈现时间戳推进动画。
    const std::chrono::nanoseconds now = data.view
        ? data.view->nextPresentationTimestamp()
        : std::chrono::nanoseconds{0};

    if (m_lastFrame.count() != 0 && now > m_lastFrame) {
        // 限制会话恢复后的异常长时间步，避免实例在单帧内跳过全部生命周期。
        const double dt = std::min(std::chrono::duration<double>(now - m_lastFrame).count(), 0.1);
        // 保存真实呈现间隔供调试日志使用。
        m_lastFrameDelta = std::chrono::duration<double, std::milli>(now - m_lastFrame).count();
        advance(dt);
    }
    m_lastFrame = now;

    // pointerMotion 可用时避免重复轮询；不转发该事件的后端使用 cursorPos() 回退。
    if (m_dragging && !m_pointerMotionWorks) {
        updateDrag(effects->cursorPos());
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

    if (m_debugLog) {
        m_lastPrePaintCpuMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpuStart).count();
    }

    effects->prePaintScreen(data);
}

void BaClickFxEffect::logFrameStats(const Region &deviceRegion, double cpuMs)
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
        const Region sourceOnOutput = m_lastContent.intersected(out->geometry());
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
    qCInfo(KWIN_BA_CLICK_FX).nospace()
        << "帧统计 " << m_statFrames << " 帧均值 | "
        << "CPU " << (m_statCpuMsSum / n) << "ms(峰 " << m_statCpuMsMax << ") | "
        << "分段 pre " << (m_statPrePaintCpuMsSum / n)
        << " setup " << (m_statSetupCpuMsSum / n)
        << " trail " << (m_statTrailCpuMsSum / n)
        << " particle " << (m_statParticleCpuMsSum / n)
        << " finish " << (m_statFinishCpuMsSum / n)
        << " input/30帧 " << m_inputCpuMsSinceLog << "ms | "
        << "GPU " << avgGpuMs << "ms(峰 " << m_statGpuMsMax
        << ", 样本 " << m_statGpuSamples << ") | "
        << "GPU分段 import " << (m_statGpuImportMsSum / gpuN)
        << " particle " << (m_statGpuParticleMsSum / gpuN)
        << " bloom " << (m_statGpuBloomMsSum / gpuN)
        << " composite " << (m_statGpuCompositeMsSum / gpuN) << " | "
        << "帧间隔 " << (m_statFrameMsSum / n) << "ms(峰 " << m_statFrameMsMax << ") | "
        << "KWin输出渲染 " << (m_statDevicePxSum / n / 1e6) << "Mpx/"
        << (m_statDeviceRectsSum / n) << "矩形"
        << " 我们申请 " << (m_statRequestPxSum / n / 1e6) << "Mpx/"
        << (m_statRequestRectsSum / n) << "矩形"
        << " Bloom源 " << (m_statBloomSourcePxSum / n / 1e6) << "Mpx/"
        << (m_statBloomSourceRectsSum / n) << "矩形";

    m_statFrames = 0;
    m_statCpuMsSum = m_statCpuMsMax = 0.0;
    m_statPrePaintCpuMsSum = 0.0;
    m_statSetupCpuMsSum = 0.0;
    m_statTrailCpuMsSum = 0.0;
    m_statParticleCpuMsSum = 0.0;
    m_statFinishCpuMsSum = 0.0;
    m_inputCpuMsSinceLog = 0.0;
    m_statGpuMsSum = m_statGpuMsMax = 0.0;
    m_statGpuImportMsSum = 0.0;
    m_statGpuParticleMsSum = 0.0;
    m_statGpuBloomMsSum = 0.0;
    m_statGpuCompositeMsSum = 0.0;
    m_statGpuSamples = 0;
    m_statFrameMsSum = m_statFrameMsMax = 0.0;
    m_statDevicePxSum = m_statRequestPxSum = m_statBloomSourcePxSum = 0.0;
    m_statDeviceRectsSum = m_statRequestRectsSum = m_statBloomSourceRectsSum = 0.0;
}

void BaClickFxEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // 先让底下的窗口/桌面画完，粒子是叠在最上面的。
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (m_instances.empty() && m_bursts.empty() && m_trails.empty()) {
        return;
    }

    // Shader 和纹理需要有效 OpenGL 上下文，因此在首次绘制时初始化。失败后不在每帧
    // 重试；插件没有 CPU 渲染回退。
    if (!m_gpuTried) {
        m_gpuTried = true;
        m_gpuReady = m_gpu.initialize();
        if (!m_gpuReady) {
            qCWarning(KWIN_BA_CLICK_FX) << "GPU 路径初始化失败，本插件不会绘制任何内容";
        }
    }
    if (!m_gpuReady) {
        return;
    }

    if (!m_debugLog) {
        renderGpu(renderTarget, viewport);
        return;
    }

    // DebugLog 下测量 paintScreen() 的 CPU 墙钟时间；顶点生成和绘制提交都位于合成线程。
    const auto t0 = std::chrono::steady_clock::now();
    renderGpu(renderTarget, viewport);
    const double cpuMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logFrameStats(deviceRegion, cpuMs);
    if (m_debugDamage) {
        drawDebugDamage(renderTarget, viewport, deviceRegion);
    }
}

void BaClickFxEffect::drawDebugDamage(const RenderTarget &renderTarget,
                                    const RenderViewport &viewport,
                                    const Region &deviceRegion)
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

    // deviceRegion 已经是 KWin 本帧实际使用的设备像素区域，逐矩形画，不能取
    // boundingRect，否则恰好会掩盖我们想诊断的 Region 是否被扩大/合并的问题。
    for (const Rect &r : deviceRegion.rects()) {
        drawRect(float(r.x()), float(r.y()), float(r.x() + r.width()),
                 float(r.y() + r.height()), QVector4D(1.0f, 0.0f, 1.0f, 0.8f));
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
    auto phaseStart = m_debugLog ? CpuClock::now() : CpuClock::time_point{};
    m_lastSetupCpuMs = 0.0;
    m_lastTrailCpuMs = 0.0;
    m_lastParticleCpuMs = 0.0;
    m_lastFinishCpuMs = 0.0;
    const auto phaseMs = [&]() {
        if (!m_debugLog) {
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
        m_lastSetupCpuMs = phaseMs();
        return false;
    }

    // HDR FBO 与 Bloom 金字塔按整块输出分配并逐输出复用。
    if (!m_gpu.prepareTargets(viewport)) {
        m_lastSetupCpuMs = phaseMs();
        return false;
    }

    const QPointF origin = outputRect.topLeft();

    // 背景导入使用 m_lastClearArea，以覆盖当前 dirty 之外的上一帧粒子。导入失败时
    // 跳过当前帧，避免将未初始化的私有 HDR 内容覆盖到屏幕。
    if (!m_gpu.beginFrame(renderTarget, viewport, m_lastClearArea)) {
        m_lastSetupCpuMs = phaseMs();
        return false;
    }
    m_lastSetupCpuMs = phaseMs();

    // 队列 3000 的固定提交顺序：Trail、Ring/Ring3、Ring4。
    for (const TrailSession &session : m_trails) {
        if (!session.stream.empty()) {
            m_gpu.renderTrail(session.stream, session.trailParams, origin);
        }
    }
    m_lastTrailCpuMs = phaseMs();

    for (ClickInstance &inst : m_instances) {
        m_gpu.renderClickBase(inst, origin);
        inst.drawn = true;
    }

    for (TriBurstInstance &inst : m_bursts) {
        m_gpu.renderTriBurst(inst, origin);
        inst.drawn = true;
    }

    // Unity 按材质 Render Queue 全局排序：MeshTri=4499，其余粒子=3000。
    // 因此所有点击实例与 Ring4 的 3000 层都结束后，才统一提交 MeshTri。
    for (const ClickInstance &inst : m_instances) {
        m_gpu.renderClickMeshTri(inst, m_meshes.cylinder002, origin);
    }
    m_lastParticleCpuMs = phaseMs();

    // 最终合成使用已按 Bloom 半径外扩的 m_lastDirty；Bloom 金字塔从未外扩的
    // m_lastContent 开始传播，避免重复计算影响半径。
    m_gpu.endFrame(renderTarget, viewport, m_lastDirty, m_lastContent);
    m_lastFinishCpuMs = phaseMs();
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
        // 还有内容存活（或仍在拖动），要下一帧。
        if (!m_lastDirty.isEmpty()) {
            requestRepaint(m_lastDirty);
        } else if (m_dragging) {
            // 拖动中但屏上暂时空着（例如关了拖尾、还没攒够一次连发）。
            // 这时没有任何区域需要重画，但必须让帧循环继续：光标位置是在
            // prePaintScreen 里按帧轮询的（m_pointerMotionWorks 为 false 的后端），
            // 一旦停帧就再也收不到移动，距离发射器和拖尾都会停在原地。
            // 请求 1×1 最小区域以维持下一帧输入轮询。
            const QPointF c = effects->cursorPos();
            effects->addRepaint(Rect(int(c.x()), int(c.y()), 1, 1));
        }
    } else if (m_paintedLastFrame) {
        // 最后一帧：再要一次重绘，把残影擦掉。范围是上一帧真正铺过的那块。
        requestRepaint(m_lastPainted);
        m_paintedLastFrame = false;
        m_lastPainted = Region();
        m_lastDirty = Region();
        m_lastContent = Region();
        m_lastClearArea = Region();
        m_lastFrame = std::chrono::nanoseconds{0};
    }

    effects->postPaintScreen();
}

void BaClickFxEffect::slotMouseChanged(const QPointF &pos, const QPointF &,
                                     Qt::MouseButtons buttons, Qt::MouseButtons oldButtons,
                                     Qt::KeyboardModifiers, Qt::KeyboardModifiers)
{
    const bool wasDown = oldButtons & Qt::LeftButton;
    const bool isDown = buttons & Qt::LeftButton;

    // 只处理按下/松开两个跃迁；移动交给 prePaintScreen 里的按帧轮询。
    if (!wasDown && isDown) {
        spawn(pos);
        startDrag(pos);
        // 从非活动状态切换后请求一个最小区域以启动帧循环；完整 Region 将由随后的
        // prePaintScreen() 加入 data.paint。
        effects->addRepaint(Rect(int(std::floor(pos.x())), int(std::floor(pos.y())), 1, 1));
    } else if (wasDown && !isDown) {
        endDrag();
    }
}

} // namespace KWin

#include "moc_baclickfxeffect.cpp"
