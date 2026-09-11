#pragma once

// 事件 Tab：topic 列表 + 手动触发 + 自动触发计划 + 当前订阅。
//
// 订阅那张表是这一页最有价值的东西：参照客户端每次建连都建一条 PullPoint
// 订阅且从不退订，槽位泄漏是真实高发故障 —— 泄漏发生时这里的行数会一路涨，
// 一眼就能看见。

#include "events/EventEngine.h"
#include "gui/tabs/CameraTab.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace onvifsim {
namespace gui {

class EventsTab : public CameraTab
{
    Q_OBJECT
public:
    explicit EventsTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    void reloadTopics();
    void reloadSchedules();
    void applySchedules();
    void addScheduleRow(const EventSchedule &schedule);
    void triggerSelected();

    QTableWidget *m_topics = nullptr;
    QSpinBox *m_duration = nullptr;
    QPushButton *m_trigger = nullptr;

    QTableWidget *m_schedules = nullptr;
    QSpinBox *m_stormRate = nullptr;
    QPushButton *m_stormButton = nullptr;

    QTableWidget *m_subscriptions = nullptr;
    QLabel *m_stats = nullptr;
};

} // namespace gui
} // namespace onvifsim
