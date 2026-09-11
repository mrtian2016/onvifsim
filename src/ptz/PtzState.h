#pragma once

// PTZ 位置积分模型与预置位。
// pan / tilt ∈ [-1, 1]，zoom ∈ [0, 1]。连续移动按速度积分，到限位停，
// GetStatus 返回随时间变化的真实位置。

#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>

namespace onvifsim {

class VirtualCamera;

struct PtzVector {
    double pan = 0.0;
    double tilt = 0.0;
    double zoom = 0.0;
    bool hasPanTilt = true;
    bool hasZoom = true;
};

struct PtzPreset {
    QString token;
    QString name;
    PtzVector position;
    bool hasPosition = true;
};

enum class PtzMoveStatus { Idle, Moving, Unknown };

// 注意：这个类的若干 const 方法（position() / settings() 一类）在内部会**推进
// 积分状态**再返回 —— 语义上是「读」，实现上会改 d 里的时间戳与当前值。
// 因此它**不是线程安全的**，也不要在同一个表达式里对同一对象调两次。
class PtzState : public QObject
{
    Q_OBJECT
public:
    explicit PtzState(VirtualCamera *camera, QObject *parent = nullptr);
    ~PtzState() override;

    PtzVector position() const;
    void setPosition(const PtzVector &position);

    PtzMoveStatus panTiltStatus() const;
    PtzMoveStatus zoomStatus() const;
    QDateTime lastUpdate() const;

    // ---- 移动 ----
    // ContinuousMove 里 PanTilt 与 Zoom 同时出现或只出现一个都要接。
    void continuousMove(const PtzVector &velocity, int timeoutMs = 0);
    void relativeMove(const PtzVector &translation, const PtzVector &speed);
    void absoluteMove(const PtzVector &target, const PtzVector &speed);
    void stop(bool panTilt = true, bool zoom = true);

    // ---- 预置位 ----
    QList<PtzPreset> presets() const;
    const PtzPreset *preset(const QString &token) const;
    QString setPreset(const QString &name, const QString &token = QString(), QString *errorOut = nullptr);
    bool removePreset(const QString &token);
    bool gotoPreset(const QString &token, const PtzVector &speed);
    int maxPresets() const;

    // quirk C6：出厂预填 N 个槽，含巡航扫描 / 远程重启这类功能槽。
    void generateFactoryPresets(int count);
    void clearPresets();

    // ---- Home ----
    PtzVector homePosition() const;
    void setHomePosition();
    bool gotoHome(const PtzVector &speed);
    bool isHomeSet() const;

    // 辅助命令（雨刷、红外灯等），只记录不做事。
    QString lastAuxiliaryCommand() const;
    void sendAuxiliaryCommand(const QString &command);

signals:
    void positionChanged(const onvifsim::PtzVector &position);
    void moveStatusChanged();
    void presetsChanged();

private:
    struct Private;
    Private *d;
};

} // namespace onvifsim

// 让 positionChanged 能走队列连接（RtspServer 将来 moveToThread 时用得上）。
Q_DECLARE_METATYPE(onvifsim::PtzVector)
