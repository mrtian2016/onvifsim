#include "ptz/PtzState.h"

#include "core/VirtualCamera.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QTimer>
#include <QtCore/QUrl>

#include <cmath>

namespace onvifsim {
namespace {

// 归一化坐标区间：ONVIF 的 PositionGenericSpace 是 [-1, 1]，ZoomSpace 是 [0, 1]。
// 服务层往 XML 里写的就是这套数，所以状态机内部也直接用它，不再做一次换算。
constexpr double kPanTiltMin = -1.0;
constexpr double kPanTiltMax = 1.0;
constexpr double kZoomMin = 0.0;
constexpr double kZoomMax = 1.0;

// 满速下每秒走过的归一化距离。取值只影响「转得多快」的手感：云台横扫全程 2 秒、
// 变焦走完全程 2 秒，接近常见球机；也让单测能用几百毫秒的时间窗测出可辨认的位移。
constexpr double kPanTiltRate = 1.0;
constexpr double kZoomRate = 0.5;

// 位置量级是 1，1e-9 足够区分「顶到限位」与「还差一点」，不会把真实位移吃掉。
constexpr double kEps = 1e-9;

// 只在移动期间跑的信号节拍。位置本身是惰性积分的，这个定时器纯粹为了让 GUI
// 的十字准星有得刷；停下就关掉，空转的相机不占 CPU。
constexpr int kTickIntervalMs = 100;

// 没有相机（单测直接 new PtzState(nullptr)）时的预置位容量，与 PtzNodeConfig 的默认值一致。
constexpr int kDefaultMaxPresets = 300;

double clampRange(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 速度分量归一到 (0, 1]。ONVIF 允许 AbsoluteMove / RelativeMove 不带 Speed，
// 客户端也常发 0；这两种都按满速处理，否则速率为 0 的定点移动永远到不了目标。
double speedFactor(double raw)
{
    const double s = std::fabs(raw);
    if (s < kEps)
        return 1.0;
    return s > 1.0 ? 1.0 : s;
}

// 出厂预填里掺着的功能槽：真机（海康系）把一批固定编号留给巡航、扫描、重启这类动作，
// 名字不是「预置点 N」。客户端的占位名过滤要同时见到两种名字才算被压到位
//（reference-client-facts §5.3：占位名正则 + 同位置聚类两条启发式）。
const QHash<int, QString> &factoryFunctionSlots()
{
    static const QHash<int, QString> table = {
        { 33, QStringLiteral("自动翻转") },
        { 34, QStringLiteral("回到零点位置") },
        { 35, QStringLiteral("调用巡航 1") },
        { 36, QStringLiteral("调用巡航 2") },
        { 46, QStringLiteral("快速巡航") },
        { 90, QStringLiteral("巡航扫描") },
        { 92, QStringLiteral("开启限位") },
        { 94, QStringLiteral("远程重启") },
        { 95, QStringLiteral("调出菜单") },
        { 96, QStringLiteral("停止扫描") },
        { 99, QStringLiteral("开始自动扫描") },
    };
    return table;
}

} // namespace

struct PtzState::Private
{
    enum class Mode { Idle, Continuous, Target };

    struct Axis {
        double value = 0.0;
        double lo = kPanTiltMin;
        double hi = kPanTiltMax;
        double vel = 0.0;      // 连续模式：带符号速度；定点模式：正的速率
        double target = 0.0;
    };

    // 一起启停的一组轴。PanTilt 与 Zoom 必须分开：ContinuousMove 可以只带其中一个，
    // Stop 也能只停一个（客户端手写栈就是只发被命令的那个轴）。
    struct Group {
        Mode mode = Mode::Idle;
        qint64 deadlineMs = -1;   // ContinuousMove 的 Timeout，-1 表示不限
        qint64 minEndMs = -1;     // 最短运动时长，goto_preset_slow 用
    };

    PtzState *q = nullptr;
    VirtualCamera *camera = nullptr;

    // 单调时钟。位置靠它按「上次积分到现在」补算，所以 GetStatus 无论什么时候来都准，
    // 也不受系统时间被调整的影响。
    QElapsedTimer clock;
    qint64 lastTickMs = 0;

    Axis pan;
    Axis tilt;
    Axis zoom;
    Group panTilt;
    Group zoomGroup;

    QList<PtzPreset> presets;
    PtzVector home;
    bool homeSet = false;
    QString lastAux;
    QDateTime lastUpdate;
    QTimer *ticker = nullptr;
    bool lastNotifiedMoving = false;

    const Quirks &quirks() const;
    int maxPresets() const;
    bool statusFrozen() const;

    qint64 nowMs() const { return clock.elapsed(); }
    bool moving() const { return panTilt.mode != Mode::Idle || zoomGroup.mode != Mode::Idle; }

    bool advance(qint64 now);
    bool stepGroup(Group &g, Axis **axes, int count, qint64 prevMs, qint64 now);
    void startTarget(const PtzVector &target, const PtzVector &speed, int minDurationMs);
    void notifyStatusIfChanged();
    void syncTicker();

    PtzVector snapshot() const;
    QString storedName(const QString &name) const;
    QString allocateToken() const;
    int indexOf(const QString &token) const;
};

const Quirks &PtzState::Private::quirks() const
{
    // 单测直接构造 PtzState(nullptr)；没有相机时退回一份全关的默认开关集。
    static const Quirks none;
    return camera ? camera->quirks() : none;
}

int PtzState::Private::maxPresets() const
{
    return camera ? camera->model().ptzNode.maxPresets : kDefaultMaxPresets;
}

bool PtzState::Private::statusFrozen() const
{
    return quirks().isEnabled(QuirkId::PtzMoveWithoutStatusChange);
}

PtzVector PtzState::Private::snapshot() const
{
    PtzVector v;
    v.pan = pan.value;
    v.tilt = tilt.value;
    v.zoom = zoom.value;
    return v;
}

int PtzState::Private::indexOf(const QString &token) const
{
    for (int i = 0; i < presets.size(); ++i) {
        if (presets.at(i).token == token)
            return i;
    }
    return -1;
}

QString PtzState::Private::storedName(const QString &name) const
{
    // C7：真机把名字百分号编码后原样吐回，客户端不 unquote 就显示成一串 %XX。
    // 编码在写入时做一次，之后读写都是同一份字节，「原样存、原样吐」才成立。
    if (name.isEmpty() || !quirks().isEnabled(QuirkId::PtzPresetPercentEncoded))
        return name;
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

QString PtzState::Private::allocateToken() const
{
    // 真机的 token 就是槽位号，而且客户端的占位名过滤要求名字里的数字与 token 一致，
    // 所以这里取最小的空闲正整数，而不是 UUID 之类。
    for (int i = 1;; ++i) {
        const QString candidate = QString::number(i);
        if (indexOf(candidate) < 0)
            return candidate;
    }
}

bool PtzState::Private::stepGroup(Group &g, Axis **axes, int count, qint64 prevMs, qint64 now)
{
    if (g.mode == Mode::Idle)
        return false;

    double dt = static_cast<double>(now - prevMs) / 1000.0;
    if (g.deadlineMs >= 0) {
        // 超时点之后的那段时间不能再积分，否则 Timeout 形同虚设。
        const double allowed = static_cast<double>(g.deadlineMs - prevMs) / 1000.0;
        dt = qMin(dt, qMax(0.0, allowed));
    }

    bool changed = false;
    bool finished = true;
    for (int i = 0; i < count; ++i) {
        Axis &a = *axes[i];
        const double before = a.value;
        if (g.mode == Mode::Continuous) {
            a.value = clampRange(a.value + a.vel * dt, a.lo, a.hi);
            // 「到限位就停」：只要还有一个轴能继续往它的方向走，整组就还在动。
            const bool canMove = (a.vel > kEps && a.value < a.hi - kEps)
                    || (a.vel < -kEps && a.value > a.lo + kEps);
            if (canMove)
                finished = false;
        } else {
            const double remain = a.target - a.value;
            const double step = a.vel * dt;
            if (std::fabs(remain) <= step + kEps) {
                a.value = a.target;   // 吸附到目标，避免浮点残差让状态永远回不到 Idle
            } else {
                a.value += remain > 0.0 ? step : -step;
                finished = false;
            }
        }
        if (before != a.value)
            changed = true;
    }

    // 慢速 GotoPreset：位置早就到了，但机械动作还没结束，状态要继续报 MOVING。
    if (g.minEndMs >= 0 && now < g.minEndMs)
        finished = false;
    if (g.deadlineMs >= 0 && now >= g.deadlineMs)
        finished = true;

    if (finished) {
        g.mode = Mode::Idle;
        g.deadlineMs = -1;
        g.minEndMs = -1;
        for (int i = 0; i < count; ++i)
            axes[i]->vel = 0.0;
    }
    return changed;
}

bool PtzState::Private::advance(qint64 now)
{
    if (now < lastTickMs) {
        // 单调时钟理论上不会回退，真回退了就重新对时，别把位置算飞。
        lastTickMs = now;
        return false;
    }
    const qint64 prev = lastTickMs;
    lastTickMs = now;

    Axis *ptAxes[2] = { &pan, &tilt };
    Axis *zoomAxes[1] = { &zoom };
    const bool ptChanged = stepGroup(panTilt, ptAxes, 2, prev, now);
    const bool zoomChanged = stepGroup(zoomGroup, zoomAxes, 1, prev, now);
    if (ptChanged || zoomChanged)
        lastUpdate = QDateTime::currentDateTimeUtc();
    return ptChanged || zoomChanged;
}

void PtzState::Private::startTarget(const PtzVector &target, const PtzVector &speed,
                                    int minDurationMs)
{
    if (statusFrozen())
        return;

    const qint64 now = nowMs();
    advance(now);
    const qint64 minEnd = minDurationMs > 0 ? now + minDurationMs : -1;
    const double stretch = minDurationMs > 0 ? minDurationMs / 1000.0 : 0.0;

    // 慢速模式下速率按「走完全程正好用掉这么久」反推，位置一路慢慢爬；
    // 否则按 Speed 分量取满速的百分比。
    const auto rateFor = [&](const Axis &a, double baseRate, double sp) {
        if (stretch > 0.0)
            return qMax(std::fabs(a.target - a.value) / stretch, kEps);
        return baseRate * speedFactor(sp);
    };

    if (target.hasPanTilt) {
        pan.target = clampRange(target.pan, pan.lo, pan.hi);
        tilt.target = clampRange(target.tilt, tilt.lo, tilt.hi);
        const double sp = speed.hasPanTilt ? speed.pan : 0.0;
        const double st = speed.hasPanTilt ? speed.tilt : 0.0;
        pan.vel = rateFor(pan, kPanTiltRate, sp);
        tilt.vel = rateFor(tilt, kPanTiltRate, st);
        panTilt.mode = Mode::Target;
        panTilt.deadlineMs = -1;
        panTilt.minEndMs = minEnd;
    }
    if (target.hasZoom) {
        zoom.target = clampRange(target.zoom, zoom.lo, zoom.hi);
        const double sz = speed.hasZoom ? speed.zoom : 0.0;
        zoom.vel = rateFor(zoom, kZoomRate, sz);
        zoomGroup.mode = Mode::Target;
        zoomGroup.deadlineMs = -1;
        zoomGroup.minEndMs = minEnd;
    }

    // 再走一次零时长的积分：目标就是当前位置（很常见，出厂预置位全指向同一个点）
    // 时立刻收敛回 Idle，不留一个假的 MOVING。
    const bool changed = advance(now);
    lastUpdate = QDateTime::currentDateTimeUtc();
    if (changed)
        emit q->positionChanged(snapshot());
    notifyStatusIfChanged();
    syncTicker();
}

void PtzState::Private::notifyStatusIfChanged()
{
    // 只跟「上一次发出去的状态」比，不看瞬时值：position() 这类 const 访问也会推进积分，
    // 运动可能在两拍之间悄悄结束，只比瞬时值就会漏掉 MOVING → IDLE 那一次信号。
    const bool now = moving();
    if (now == lastNotifiedMoving)
        return;
    lastNotifiedMoving = now;
    emit q->moveStatusChanged();
}

void PtzState::Private::syncTicker()
{
    if (!ticker)
        return;
    if (moving()) {
        if (!ticker->isActive())
            ticker->start();
    } else if (ticker->isActive()) {
        ticker->stop();
    }
}

PtzState::PtzState(VirtualCamera *camera, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->q = this;
    d->camera = camera;
    d->zoom.lo = kZoomMin;
    d->zoom.hi = kZoomMax;
    d->clock.start();
    d->lastTickMs = d->clock.elapsed();
    d->lastUpdate = QDateTime::currentDateTimeUtc();

    d->ticker = new QTimer(this);
    d->ticker->setInterval(kTickIntervalMs);
    d->ticker->setTimerType(Qt::CoarseTimer);
    connect(d->ticker, &QTimer::timeout, this, [this] {
        const bool wasMoving = d->moving();
        const bool changed = d->advance(d->nowMs());
        // 位置可能刚被 position() 惰性积分吃掉一段，所以只要还在动就照发，
        // 免得 GUI 因为「这一拍没变化」而卡住不刷新。
        if (changed || wasMoving)
            emit positionChanged(d->snapshot());
        d->notifyStatusIfChanged();
        d->syncTicker();
    });

    // C6：出厂预填的槽位在开机时就已经在相机里了，所以构造时按 quirk 直接铺好。
    // quirk 是运行期可改的，REST / GUI 改完要再调一次 generateFactoryPresets()。
    if (d->quirks().isEnabled(QuirkId::PtzFactory300Presets)) {
        generateFactoryPresets(
                d->quirks().paramInt(QuirkId::PtzFactory300Presets, QStringLiteral("count")));
    }
}

PtzState::~PtzState()
{
    delete d;
}

PtzVector PtzState::position() const
{
    // GetStatus 随时可能进来，位置在这里现算，不指望定时器把每一帧都刷准。
    d->advance(d->nowMs());
    return d->snapshot();
}

void PtzState::setPosition(const PtzVector &position)
{
    const qint64 now = d->nowMs();
    d->advance(now);

    // 外部直接摆位（REST / GUI / 场景恢复）：摆哪一组就停哪一组，
    // 否则残留的速度会立刻把刚设好的位置又推走。
    if (position.hasPanTilt) {
        d->pan.value = clampRange(position.pan, d->pan.lo, d->pan.hi);
        d->tilt.value = clampRange(position.tilt, d->tilt.lo, d->tilt.hi);
        d->panTilt.mode = Private::Mode::Idle;
        d->pan.vel = 0.0;
        d->tilt.vel = 0.0;
    }
    if (position.hasZoom) {
        d->zoom.value = clampRange(position.zoom, d->zoom.lo, d->zoom.hi);
        d->zoomGroup.mode = Private::Mode::Idle;
        d->zoom.vel = 0.0;
    }
    d->lastUpdate = QDateTime::currentDateTimeUtc();

    emit positionChanged(d->snapshot());
    d->notifyStatusIfChanged();
    d->syncTicker();
}

PtzMoveStatus PtzState::panTiltStatus() const
{
    d->advance(d->nowMs());
    return d->panTilt.mode == Private::Mode::Idle ? PtzMoveStatus::Idle : PtzMoveStatus::Moving;
}

PtzMoveStatus PtzState::zoomStatus() const
{
    d->advance(d->nowMs());
    return d->zoomGroup.mode == Private::Mode::Idle ? PtzMoveStatus::Idle : PtzMoveStatus::Moving;
}

QDateTime PtzState::lastUpdate() const
{
    d->advance(d->nowMs());
    return d->lastUpdate;
}

void PtzState::continuousMove(const PtzVector &velocity, int timeoutMs)
{
    // quirk：命令照收照回成功，但位置永远不动，客户端闭环不了。
    if (d->statusFrozen())
        return;

    const qint64 now = d->nowMs();
    d->advance(now);
    const qint64 deadline = timeoutMs > 0 ? now + timeoutMs : -1;

    // 只更新出现的那一组：客户端手写栈只发被命令的那个轴（非零 Zoom 会让某些
    // 固件吐畸形 HTTP，见 C5），zeep 栈两个一起发，两种都得接。
    if (velocity.hasPanTilt) {
        d->pan.vel = clampRange(velocity.pan, -1.0, 1.0) * kPanTiltRate;
        d->tilt.vel = clampRange(velocity.tilt, -1.0, 1.0) * kPanTiltRate;
        d->panTilt.mode = Private::Mode::Continuous;
        d->panTilt.deadlineMs = deadline;
        d->panTilt.minEndMs = -1;
    }
    if (velocity.hasZoom) {
        d->zoom.vel = clampRange(velocity.zoom, -1.0, 1.0) * kZoomRate;
        d->zoomGroup.mode = Private::Mode::Continuous;
        d->zoomGroup.deadlineMs = deadline;
        d->zoomGroup.minEndMs = -1;
    }

    // 零速度的 ContinuousMove 等价于 Stop；零时长积分会把这种组直接收敛回 Idle。
    const bool changed = d->advance(now);
    d->lastUpdate = QDateTime::currentDateTimeUtc();
    if (changed)
        emit positionChanged(d->snapshot());
    d->notifyStatusIfChanged();
    d->syncTicker();
}

void PtzState::relativeMove(const PtzVector &translation, const PtzVector &speed)
{
    // 相对移动就是「当前位置 + 位移」的定点移动，限位交给 absoluteMove 统一 clamp。
    const PtzVector current = position();
    PtzVector target = translation;
    target.pan = current.pan + translation.pan;
    target.tilt = current.tilt + translation.tilt;
    target.zoom = current.zoom + translation.zoom;
    absoluteMove(target, speed);
}

void PtzState::absoluteMove(const PtzVector &target, const PtzVector &speed)
{
    // 按 speed 走一段时间才到位，不是瞬移：期间 GetStatus 必须显示 MOVING，
    // 客户端就是靠这个判断命令有没有被执行。
    d->startTarget(target, speed, 0);
}

void PtzState::stop(bool panTilt, bool zoom)
{
    const qint64 now = d->nowMs();
    const bool changed = d->advance(now);

    if (panTilt) {
        d->panTilt.mode = Private::Mode::Idle;
        d->panTilt.deadlineMs = -1;
        d->panTilt.minEndMs = -1;
        d->pan.vel = 0.0;
        d->tilt.vel = 0.0;
    }
    if (zoom) {
        d->zoomGroup.mode = Private::Mode::Idle;
        d->zoomGroup.deadlineMs = -1;
        d->zoomGroup.minEndMs = -1;
        d->zoom.vel = 0.0;
    }

    if (changed)
        emit positionChanged(d->snapshot());
    d->notifyStatusIfChanged();
    d->syncTicker();
}

QList<PtzPreset> PtzState::presets() const
{
    return d->presets;
}

const PtzPreset *PtzState::preset(const QString &token) const
{
    const int i = d->indexOf(token);
    return i < 0 ? nullptr : &d->presets.at(i);
}

QString PtzState::setPreset(const QString &name, const QString &token, QString *errorOut)
{
    if (errorOut)
        errorOut->clear();

    const PtzVector here = position();
    const int existing = token.isEmpty() ? -1 : d->indexOf(token);
    if (existing >= 0) {
        // 覆盖已有槽位：名字给空就保留原名（真机普遍如此），位置一律刷成当前位置。
        PtzPreset &p = d->presets[existing];
        if (!name.isEmpty())
            p.name = d->storedName(name);
        p.position = here;
        p.hasPosition = true;
        // token 要在 emit **之前**取出来：presetsChanged 的槽里只要动了
        // d->presets（界面的刷新就在读它），p 这个引用当场失效。
        const QString existingToken = p.token;
        emit presetsChanged();
        return existingToken;
    }

    // 容量按相机模型的 maxPresets 卡。满了要让服务层回 ter:TooManyPresets，
    // 这是真机上「出厂预填 300 槽」之后必然会撞到的错误。
    if (d->presets.size() >= d->maxPresets()) {
        if (errorOut)
            *errorOut = QStringLiteral("ter:TooManyPresets");
        return QString();
    }

    PtzPreset p;
    // 带 token 但槽位不存在时按新建处理：真机行为不一，客户端两种都接，
    // 新建比回 Fault 更不容易把客户端的预置位流程卡死。
    p.token = token.isEmpty() ? d->allocateToken() : token;
    p.name = d->storedName(name.isEmpty() ? QStringLiteral("Preset %1").arg(p.token) : name);
    p.position = here;
    p.hasPosition = true;
    d->presets.append(p);
    emit presetsChanged();
    return p.token;
}

bool PtzState::removePreset(const QString &token)
{
    const int i = d->indexOf(token);
    if (i < 0)
        return false;
    d->presets.removeAt(i);
    emit presetsChanged();
    return true;
}

bool PtzState::gotoPreset(const QString &token, const PtzVector &speed)
{
    const int i = d->indexOf(token);
    if (i < 0)
        return false;

    // quirk：响应正常，但机械动作要好几秒，期间 GetStatus 一直 MOVING。
    const int slowMs = d->quirks().isEnabled(QuirkId::PtzGotoPresetSlow)
            ? d->quirks().paramInt(QuirkId::PtzGotoPresetSlow, QStringLiteral("ms"))
            : 0;

    const PtzPreset &p = d->presets.at(i);
    // 功能槽（巡航、重启这类）没有位置，命令收下但不该乱动；慢 quirk 仍然要装忙。
    const PtzVector target = p.hasPosition ? p.position : d->snapshot();
    d->startTarget(target, speed, slowMs);
    return true;
}

int PtzState::maxPresets() const
{
    return d->maxPresets();
}

void PtzState::generateFactoryPresets(int count)
{
    d->presets.clear();

    int n = qMax(0, count);
    const int cap = d->maxPresets();
    if (cap > 0)
        n = qMin(n, cap);   // 生成数不能突破自己声明的容量，否则 SetPreset 的判据自相矛盾

    // C6：这些槽在真机上全部指向同一个假位置——正是客户端那两条启发式
    //（同一精确位置聚 ≥ 8 且过半占位名整组丢、占位名 ≥ 30 整组丢）要对付的东西。
    PtzVector shared;
    shared.pan = 0.0;
    shared.tilt = 1.0;
    shared.zoom = 0.0;

    const QHash<int, QString> &functional = factoryFunctionSlots();
    d->presets.reserve(n);
    for (int i = 1; i <= n; ++i) {
        PtzPreset p;
        p.token = QString::number(i);
        const auto it = functional.constFind(i);
        // 占位名里的数字必须与 token 一致，客户端的正则是连着 token 一起校验的。
        p.name = d->storedName(it != functional.constEnd()
                                       ? it.value()
                                       : QStringLiteral("预置点 %1").arg(i));
        p.position = shared;
        p.hasPosition = true;
        d->presets.append(p);
    }
    emit presetsChanged();
}

void PtzState::clearPresets()
{
    if (d->presets.isEmpty())
        return;
    d->presets.clear();
    emit presetsChanged();
}

PtzVector PtzState::homePosition() const
{
    return d->home;
}

void PtzState::setHomePosition()
{
    d->home = position();
    d->homeSet = true;
}

bool PtzState::gotoHome(const PtzVector &speed)
{
    // 没设过 Home 就回 false，服务层据此吐 ter:NoHomePosition；
    // 真机在没设过的时候也是拒绝，而不是偷偷回原点。
    if (!d->homeSet)
        return false;
    d->startTarget(d->home, speed, 0);
    return true;
}

bool PtzState::isHomeSet() const
{
    return d->homeSet;
}

QString PtzState::lastAuxiliaryCommand() const
{
    return d->lastAux;
}

void PtzState::sendAuxiliaryCommand(const QString &command)
{
    // 雨刷、红外灯这类辅助命令没有可观察的状态，只记下最后一条供 GUI / 测试断言。
    d->lastAux = command;
}

} // namespace onvifsim
