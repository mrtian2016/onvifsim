#include "gui/tabs/EventsTab.h"

#include "gui/I18n.h"

#include "core/VirtualCamera.h"
#include "events/SubscriptionManager.h"
#include "gui/GuiUtil.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

// 表格里存的是 EventKind 的整数值，取回来时要还原。
constexpr int kKindRole = Qt::UserRole + 1;
} // namespace

EventsTab::EventsTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *vertical = new QSplitter(Qt::Vertical, this);
    layout->addWidget(vertical);

    auto *top = new QSplitter(Qt::Horizontal, vertical);

    // ---- topic 列表与手动触发 ----
    auto *topicBox = new QGroupBox(tr("Topic"), top);
    auto *topicLayout = new QVBoxLayout(topicBox);
    m_topics = new QTableWidget(0, 4, topicBox);
    m_topics->setHorizontalHeaderLabels({ tr("名称"), tr("Topic"), tr("类型"), tr("当前状态") });
    m_topics->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_topics->setSelectionMode(QAbstractItemView::SingleSelection);
    m_topics->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_topics->verticalHeader()->setVisible(false);
    m_topics->horizontalHeader()->setStretchLastSection(true);
    topicLayout->addWidget(m_topics, 1);

    auto *triggerRow = new QHBoxLayout;
    m_trigger = new QPushButton(tr("触发选中的事件"), topicBox);
    triggerRow->addWidget(m_trigger);
    triggerRow->addWidget(new QLabel(tr("持续"), topicBox));
    m_duration = new QSpinBox(topicBox);
    m_duration->setRange(0, 600000);
    m_duration->setSingleStep(500);
    m_duration->setValue(5000);
    m_duration->setSuffix(tr(" 毫秒"));
    m_duration->setToolTip(tr("属性型事件会先发 true，过了这个时长再发配对的 false；"
                              "瞬时事件忽略这个值。"));
    triggerRow->addWidget(m_duration);
    triggerRow->addStretch(1);
    topicLayout->addLayout(triggerRow);
    top->addWidget(topicBox);
    connect(m_trigger, &QPushButton::clicked, this, &EventsTab::triggerSelected);
    connect(m_topics, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { triggerSelected(); });

    // ---- 自动触发计划与事件风暴 ----
    auto *scheduleBox = new QGroupBox(tr("自动触发"), top);
    auto *scheduleLayout = new QVBoxLayout(scheduleBox);
    m_schedules = new QTableWidget(0, 5, scheduleBox);
    m_schedules->setHorizontalHeaderLabels(
        { tr("事件"), tr("间隔"), tr("持续"), tr("抖动"), tr("启用") });
    m_schedules->verticalHeader()->setVisible(false);
    m_schedules->horizontalHeader()->setStretchLastSection(true);
    scheduleLayout->addWidget(m_schedules, 1);

    auto *scheduleButtons = new QHBoxLayout;
    auto *addSchedule = new QPushButton(tr("添加"), scheduleBox);
    auto *removeSchedule = new QPushButton(tr("删除"), scheduleBox);
    auto *applySchedule = new QPushButton(tr("应用"), scheduleBox);
    scheduleButtons->addWidget(addSchedule);
    scheduleButtons->addWidget(removeSchedule);
    scheduleButtons->addWidget(applySchedule);
    scheduleButtons->addStretch(1);
    scheduleLayout->addLayout(scheduleButtons);

    auto *stormRow = new QHBoxLayout;
    stormRow->addWidget(new QLabel(tr("事件风暴"), scheduleBox));
    m_stormRate = new QSpinBox(scheduleBox);
    m_stormRate->setRange(1, 1000);
    m_stormRate->setValue(20);
    m_stormRate->setSuffix(tr(" 条/秒"));
    stormRow->addWidget(m_stormRate);
    m_stormButton = new QPushButton(tr("开始"), scheduleBox);
    m_stormButton->setToolTip(tr("压测客户端的事件处理与队列，看它会不会被淹。"));
    stormRow->addWidget(m_stormButton);
    stormRow->addStretch(1);
    scheduleLayout->addLayout(stormRow);
    top->addWidget(scheduleBox);
    vertical->addWidget(top);

    connect(addSchedule, &QPushButton::clicked, this, [this] {
        EventSchedule schedule;
        schedule.enabled = true;
        addScheduleRow(schedule);
        applySchedules();
    });
    connect(removeSchedule, &QPushButton::clicked, this, [this] {
        const int row = m_schedules->currentRow();
        if (row < 0)
            return;
        m_schedules->removeRow(row);
        applySchedules();
    });
    connect(applySchedule, &QPushButton::clicked, this, &EventsTab::applySchedules);
    connect(m_stormButton, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        if (!cam)
            return;
        if (cam->events()->isStorming())
            cam->events()->stopStorm();
        else
            cam->events()->startStorm(m_stormRate->value());
        refresh();
    });

    // ---- 订阅 ----
    auto *bottomBox = new QGroupBox(tr("当前订阅"), vertical);
    auto *bottomLayout = new QVBoxLayout(bottomBox);
    m_subscriptions = new QTableWidget(0, 8, bottomBox);
    m_subscriptions->setHorizontalHeaderLabels({ tr("id"), tr("地址"), tr("过滤器"), tr("创建"),
                                                 tr("到期"), tr("队列"), tr("已拉取"),
                                                 tr("来源") });
    m_subscriptions->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_subscriptions->verticalHeader()->setVisible(false);
    m_subscriptions->horizontalHeader()->setStretchLastSection(true);
    bottomLayout->addWidget(m_subscriptions, 1);

    m_stats = new QLabel(bottomBox);
    m_stats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottomLayout->addWidget(m_stats);
    vertical->addWidget(bottomBox);
    vertical->setStretchFactor(0, 3);
    vertical->setStretchFactor(1, 2);
}

void EventsTab::bindCamera()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_topics->setRowCount(0);
        m_schedules->setRowCount(0);
        m_subscriptions->setRowCount(0);
        return;
    }

    trackConnection(connect(cam->events(), &EventEngine::topicsChanged, this,
                            &EventsTab::reloadTopics));
    trackConnection(connect(cam->events(), &EventEngine::statisticsChanged, this,
                            &CameraTab::refreshNow));
    trackConnection(connect(cam->subscriptions(), &SubscriptionManager::countChanged, this,
                            [this](int) { refreshNow(); }));
    trackConnection(connect(cam, &VirtualCamera::quirksChanged, this, &EventsTab::reloadTopics));
    trackConnection(connect(cam->events(), &EventEngine::schedulesChanged, this,
                            &EventsTab::reloadSchedules));

    reloadTopics();
    reloadSchedules();
}

void EventsTab::reloadTopics()
{
    VirtualCamera *cam = camera();
    m_topics->setRowCount(0);
    if (!cam)
        return;

    const QVector<TopicDef> topics = cam->events()->topics();
    m_topics->setRowCount(topics.size());
    for (int row = 0; row < topics.size(); ++row) {
        const TopicDef &t = topics.at(row);
        auto *first = new QTableWidgetItem(i18n::topicName(t));
        first->setData(kKindRole, static_cast<int>(t.kind));
        m_topics->setItem(row, 0, first);
        m_topics->setItem(row, 1, new QTableWidgetItem(t.topic));
        m_topics->setItem(row, 2,
                          new QTableWidgetItem(t.isProperty ? tr("属性型") : tr("瞬时")));
        m_topics->setItem(row, 3, new QTableWidgetItem(QString()));
    }
    m_topics->resizeColumnsToContents();
    if (!topics.isEmpty())
        m_topics->selectRow(0);
}

void EventsTab::addScheduleRow(const EventSchedule &schedule)
{
    const int row = m_schedules->rowCount();
    m_schedules->insertRow(row);

    auto *kind = new QComboBox(m_schedules);
    for (int i = 0; i < static_cast<int>(EventKind::Count); ++i)
        kind->addItem(TopicCatalog::kindName(static_cast<EventKind>(i)), i);
    kind->setCurrentIndex(static_cast<int>(schedule.kind));
    m_schedules->setCellWidget(row, 0, kind);

    auto *interval = new QSpinBox(m_schedules);
    interval->setRange(1, 86400);
    interval->setValue(schedule.intervalSec);
    interval->setSuffix(tr(" 秒"));
    m_schedules->setCellWidget(row, 1, interval);

    auto *duration = new QSpinBox(m_schedules);
    duration->setRange(0, 3600);
    duration->setValue(schedule.durationSec);
    duration->setSuffix(tr(" 秒"));
    m_schedules->setCellWidget(row, 2, duration);

    auto *jitter = new QCheckBox(m_schedules);
    jitter->setChecked(schedule.randomJitter);
    m_schedules->setCellWidget(row, 3, jitter);

    auto *enabled = new QCheckBox(m_schedules);
    enabled->setChecked(schedule.enabled);
    m_schedules->setCellWidget(row, 4, enabled);
    connect(enabled, &QCheckBox::toggled, this, [this] { applySchedules(); });
}

void EventsTab::reloadSchedules()
{
    // 自己刚写下去引发的回调直接忽略：否则用户每勾一个框、每改一个数字，
    // 整张表就被重建一次，正在编辑的那一格当场被换掉。
    // 外部（REST）改的计划表不带这个标记，照常回填。
    if (isUpdatingUi())
        return;
    VirtualCamera *cam = camera();
    const UiUpdateGuard guard(this);
    m_schedules->setRowCount(0);
    if (cam) {
        for (const EventSchedule &s : cam->events()->schedules())
            addScheduleRow(s);
    }
    m_schedules->resizeColumnsToContents();
}

void EventsTab::applySchedules()
{
    VirtualCamera *cam = camera();
    if (!cam || isUpdatingUi())
        return;

    QVector<EventSchedule> schedules;
    for (int row = 0; row < m_schedules->rowCount(); ++row) {
        EventSchedule s;
        if (auto *kind = qobject_cast<QComboBox *>(m_schedules->cellWidget(row, 0)))
            s.kind = static_cast<EventKind>(kind->currentData().toInt());
        if (auto *interval = qobject_cast<QSpinBox *>(m_schedules->cellWidget(row, 1)))
            s.intervalSec = interval->value();
        if (auto *duration = qobject_cast<QSpinBox *>(m_schedules->cellWidget(row, 2)))
            s.durationSec = duration->value();
        if (auto *jitter = qobject_cast<QCheckBox *>(m_schedules->cellWidget(row, 3)))
            s.randomJitter = jitter->isChecked();
        if (auto *enabled = qobject_cast<QCheckBox *>(m_schedules->cellWidget(row, 4)))
            s.enabled = enabled->isChecked();
        schedules.append(s);
    }
    // 带上标记：setSchedules 会发 schedulesChanged，而那个信号连回
    // reloadSchedules —— 不标一下就会把用户正在编辑的那一行重建掉。
    const UiUpdateGuard guard(this);
    cam->events()->setSchedules(schedules);
}

void EventsTab::triggerSelected()
{
    VirtualCamera *cam = camera();
    const int row = m_topics->currentRow();
    if (!cam || row < 0)
        return;
    QTableWidgetItem *item = m_topics->item(row, 0);
    if (!item)
        return;
    const EventKind kind = static_cast<EventKind>(item->data(kKindRole).toInt());
    cam->events()->trigger(kind, m_duration->value());
}

void EventsTab::refresh()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_stats->clear();
        return;
    }

    // topic 的当前状态
    const QMap<QString, bool> states = cam->events()->propertyStates();
    for (int row = 0; row < m_topics->rowCount(); ++row) {
        QTableWidgetItem *topicItem = m_topics->item(row, 1);
        QTableWidgetItem *stateItem = m_topics->item(row, 3);
        if (!topicItem || !stateItem)
            continue;
        const auto it = states.constFind(topicItem->text());
        stateItem->setText(it == states.constEnd() ? QString()
                                                   : (*it ? tr("true") : tr("false")));
    }

    // 订阅表。行数一般是个位数，整表重建最省心。
    const QList<Subscription *> subscriptions = cam->subscriptions()->subscriptions();
    m_subscriptions->setRowCount(subscriptions.size());
    for (int row = 0; row < subscriptions.size(); ++row) {
        const Subscription *s = subscriptions.at(row);
        const QStringList cells = {
            s->id,
            s->address,
            s->filter.expression().isEmpty() ? tr("（全收）") : s->filter.expression(),
            util::formatTime(s->createdAt),
            util::formatTime(s->terminationTime),
            QStringLiteral("%1 / %2").arg(s->queue.size()).arg(s->maxQueue),
            QString::number(s->pulledCount),
            s->creatorPeer,
        };
        for (int col = 0; col < cells.size(); ++col)
            m_subscriptions->setItem(row, col, new QTableWidgetItem(cells.at(col)));
    }

    const EventEngine *engine = cam->events();
    m_stats->setText(tr("订阅 %1 · 挂起的 PullMessages %2 · 累计事件 %3 · 最近 %4 %5")
                         .arg(cam->subscriptions()->count())
                         .arg(cam->subscriptions()->pendingPullCount())
                         .arg(engine->totalEventsSent())
                         .arg(engine->lastEventTopic().isEmpty() ? tr("无")
                                                                 : engine->lastEventTopic(),
                              util::formatRelative(engine->lastEventTime())));
    m_stormButton->setText(engine->isStorming() ? tr("停止") : tr("开始"));
}
} // namespace gui
} // namespace onvifsim
