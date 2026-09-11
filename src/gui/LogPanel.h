#pragma once

// 底部日志面板。
//
// 日志量可能极大（一次事件风暴就能刷出上万条），所以这里有三道防线：
//   1. LogBus 的信号只往内存缓冲里塞，绝不直接动界面；
//   2. 每 150 ms 批量刷一次，用 addTopLevelItems 一次性插入；
//   3. 显示条数有上限，超了从头砍，缓冲本身也有上限、溢出只计数不占内存。
// SOAP 原文（LogRecord::detail）不预先建子项，等用户展开那一行才生成 ——
// 上千条记录每条都挂一个子项的话，内存和构建时间都会翻倍。

#include "core/LogBus.h"

#include <QtCore/QVector>
#include <QtWidgets/QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace onvifsim {

class Simulator;

namespace gui {

class LogPanel : public QWidget
{
    Q_OBJECT
public:
    explicit LogPanel(Simulator *simulator, QWidget *parent = nullptr);

    // 显示上限（同时也是内存里保留的条数）。
    void setDisplayLimit(int records);
    int displayLimit() const;

public slots:
    void clearLog();
    void exportLog();

private slots:
    void onRecordPosted(const onvifsim::LogRecord &record);
    void flushPending();
    void onItemExpanded(QTreeWidgetItem *item);

private:
    bool passesFilter(const LogRecord &record) const;
    QTreeWidgetItem *makeItem(const LogRecord &record) const;
    void rebuildView();
    void reloadCameraFilter();
    void updateStatus();

    Simulator *m_simulator = nullptr;

    QTreeWidget *m_view = nullptr;
    QComboBox *m_cameraFilter = nullptr;
    QComboBox *m_levelFilter = nullptr;
    QComboBox *m_categoryFilter = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_pause = nullptr;
    QCheckBox *m_follow = nullptr;
    QSpinBox *m_limit = nullptr;
    QLabel *m_status = nullptr;
    QTimer *m_flushTimer = nullptr;

    QVector<LogRecord> m_records;   // 环形缓冲：界面上能看到的全部记录
    QVector<LogRecord> m_pending;   // 两次刷新之间攒下的
    qint64 m_droppedRecords = 0;    // 缓冲溢出丢掉的条数，只在状态栏里报个数
};

} // namespace gui
} // namespace onvifsim
