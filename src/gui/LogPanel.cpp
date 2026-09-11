#include "gui/LogPanel.h"

#include "core/Simulator.h"
#include "core/VirtualCamera.h"
#include "gui/GuiUtil.h"

#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

// 两次界面刷新之间的间隔。太短等于没批量，太长会让人觉得日志卡顿。
constexpr int kFlushIntervalMs = 150;
// 缓冲上限：事件风暴时一秒能来上万条，缓冲必须自己有个天花板。
constexpr int kPendingCap = 20000;

constexpr int kDetailRole = Qt::UserRole + 1;
constexpr int kDetailBuiltRole = Qt::UserRole + 2;

// 列
enum Column {
    ColTime = 0,
    ColLevel,
    ColCamera,
    ColCategory,
    ColPeer,
    ColDuration,
    ColSummary,
    ColCount,
};

const char *const kCategories[] = {
    logcat::Http,  logcat::Soap,  logcat::Rtsp,    logcat::Rtp,    logcat::Discovery,
    logcat::Event, logcat::Ptz,   logcat::Imaging, logcat::Media,  logcat::Vendor,
    logcat::Control, logcat::Core, logcat::Quirk,
};

QColor levelColor(LogLevel level, bool ok)
{
    // 这几个色在浅色和深色主题上都够对比度，不跟着调色板走。
    if (!ok || level == LogLevel::Error)
        return QColor(0xc2, 0x3b, 0x22);
    if (level == LogLevel::Warning)
        return QColor(0xc8, 0x86, 0x00);
    return QColor();
}

} // namespace

LogPanel::LogPanel(Simulator *simulator, QWidget *parent)
    : QWidget(parent)
    , m_simulator(simulator)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // ---- 过滤条 ----
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(tr("相机"), this));
    m_cameraFilter = new QComboBox(this);
    bar->addWidget(m_cameraFilter);

    bar->addWidget(new QLabel(tr("级别"), this));
    m_levelFilter = new QComboBox(this);
    m_levelFilter->addItem(util::levelName(LogLevel::Debug), static_cast<int>(LogLevel::Debug));
    m_levelFilter->addItem(util::levelName(LogLevel::Info), static_cast<int>(LogLevel::Info));
    m_levelFilter->addItem(util::levelName(LogLevel::Warning),
                           static_cast<int>(LogLevel::Warning));
    m_levelFilter->addItem(util::levelName(LogLevel::Error), static_cast<int>(LogLevel::Error));
    m_levelFilter->setCurrentIndex(1);
    bar->addWidget(m_levelFilter);

    bar->addWidget(new QLabel(tr("分类"), this));
    m_categoryFilter = new QComboBox(this);
    m_categoryFilter->addItem(tr("全部"), QString());
    for (const char *category : kCategories) {
        const QString name = QString::fromLatin1(category);
        m_categoryFilter->addItem(name, name);
    }
    bar->addWidget(m_categoryFilter);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索摘要 / 原文"));
    m_search->setClearButtonEnabled(true);
    bar->addWidget(m_search, 1);

    m_pause = new QCheckBox(tr("暂停"), this);
    m_pause->setToolTip(tr("暂停期间新记录仍在收集，恢复后一起显示。"));
    bar->addWidget(m_pause);
    m_follow = new QCheckBox(tr("自动滚动"), this);
    m_follow->setChecked(true);
    bar->addWidget(m_follow);

    bar->addWidget(new QLabel(tr("上限"), this));
    m_limit = new QSpinBox(this);
    m_limit->setRange(200, 100000);
    m_limit->setSingleStep(500);
    m_limit->setValue(2000);
    m_limit->setToolTip(tr("界面里最多保留多少条。超出的从最老的开始丢，"
                           "完整日志请用「导出」或命令行的 --log-file。"));
    bar->addWidget(m_limit);

    auto *clearButton = new QPushButton(tr("清空"), this);
    bar->addWidget(clearButton);
    auto *exportButton = new QPushButton(tr("导出…"), this);
    bar->addWidget(exportButton);
    layout->addLayout(bar);

    // ---- 表 ----
    m_view = new QTreeWidget(this);
    m_view->setColumnCount(ColCount);
    m_view->setHeaderLabels({ tr("时间"), tr("级别"), tr("相机"), tr("分类"), tr("客户端"),
                              tr("耗时"), tr("摘要") });
    m_view->setRootIsDecorated(true);
    m_view->setUniformRowHeights(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->header()->setStretchLastSection(true);
    m_view->setColumnWidth(ColTime, 100);
    m_view->setColumnWidth(ColLevel, 56);
    m_view->setColumnWidth(ColCamera, 70);
    m_view->setColumnWidth(ColCategory, 80);
    m_view->setColumnWidth(ColPeer, 150);
    m_view->setColumnWidth(ColDuration, 70);
    layout->addWidget(m_view, 1);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    connect(m_view, &QTreeWidget::itemExpanded, this, &LogPanel::onItemExpanded);
    connect(clearButton, &QPushButton::clicked, this, &LogPanel::clearLog);
    connect(exportButton, &QPushButton::clicked, this, &LogPanel::exportLog);
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildView(); });
    connect(m_cameraFilter, &QComboBox::currentIndexChanged, this, [this] { rebuildView(); });
    connect(m_levelFilter, &QComboBox::currentIndexChanged, this, [this] { rebuildView(); });
    connect(m_categoryFilter, &QComboBox::currentIndexChanged, this, [this] { rebuildView(); });
    connect(m_limit, &QSpinBox::valueChanged, this, [this](int value) { setDisplayLimit(value); });

    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(kFlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &LogPanel::flushPending);
    m_flushTimer->start();

    if (m_simulator) {
        connect(m_simulator->logBus(), &LogBus::recordPosted, this, &LogPanel::onRecordPosted);
        connect(m_simulator, &Simulator::cameraAdded, this,
                [this](VirtualCamera *) { reloadCameraFilter(); });
        connect(m_simulator, &Simulator::cameraRemoved, this,
                [this](const QString &) { reloadCameraFilter(); });
        reloadCameraFilter();

        // 启动时把 LogBus 的历史回填进来，GUI 起得比相机晚也不会漏掉开头那几条。
        m_records = m_simulator->logBus()->history();
        const int limit = m_limit->value();
        if (m_records.size() > limit)
            m_records.remove(0, m_records.size() - limit);
        rebuildView();
    }
}

void LogPanel::reloadCameraFilter()
{
    const QString previous = m_cameraFilter->currentData().toString();
    m_cameraFilter->blockSignals(true);
    m_cameraFilter->clear();
    m_cameraFilter->addItem(tr("全部"), QString());
    if (m_simulator) {
        const QList<VirtualCamera *> cameras = m_simulator->cameras();
        for (VirtualCamera *cam : cameras)
            m_cameraFilter->addItem(QStringLiteral("%1 · %2").arg(cam->id(),
                                                                  cam->model().displayName),
                                    cam->id());
    }
    const int index = m_cameraFilter->findData(previous);
    m_cameraFilter->setCurrentIndex(index >= 0 ? index : 0);
    m_cameraFilter->blockSignals(false);
}

void LogPanel::setDisplayLimit(int records)
{
    // setValue() 会发 valueChanged，而那个信号又连回这里 —— 靠「值相等就不再设」
    // 截断递归确实能挡住，但那是巧合不是设计：spinbox 一旦做了取整或钳位，
    // 两个值就永远不相等，于是无限互调。显式 blockSignals 把环断掉。
    if (m_limit->value() != records) {
        const QSignalBlocker blocker(m_limit);
        m_limit->setValue(records);
    }
    // 用 spinbox 钳过之后的值，别用调用方给的原值。
    const int effective = m_limit->value();
    if (m_records.size() > effective)
        m_records.remove(0, m_records.size() - effective);
    rebuildView();
}

int LogPanel::displayLimit() const
{
    return m_limit->value();
}

void LogPanel::onRecordPosted(const LogRecord &record)
{
    // 这个槽是在网络处理链上被同步调用的，只能做「塞进缓冲」这一件事。
    //
    // 溢出时按块丢，不要一条一条 remove(0, 1)：勾上「暂停」之后 flushPending()
    // 直接 return，m_pending 会稳定卡在 kPendingCap，于是**每来一条日志**都要
    // 做一次 20000 个 LogRecord 的 memmove。这段开销直接算进 RTSP / RTP / SOAP
    // 的处理时延（单进程单事件循环），表现是「一暂停日志，拉流就卡」——
    // 没人会怀疑到日志面板头上。
    if (m_pending.size() >= kPendingCap) {
        const int drop = qMax(1, kPendingCap / 4);
        m_pending.remove(0, qMin(drop, m_pending.size()));
        m_droppedRecords += drop;
    }
    m_pending.append(record);
}

void LogPanel::flushPending()
{
    if (m_pending.isEmpty() || m_pause->isChecked()) {
        if (!m_pending.isEmpty())
            updateStatus();
        return;
    }

    const int limit = m_limit->value();
    QVector<LogRecord> batch;
    batch.swap(m_pending);

    // 一批就超过上限时，老的那些反正马上会被砍掉，先扔了省得白建 item。
    if (batch.size() > limit)
        batch.remove(0, batch.size() - limit);

    m_records.append(batch);
    if (m_records.size() > limit)
        m_records.remove(0, m_records.size() - limit);

    QList<QTreeWidgetItem *> items;
    items.reserve(batch.size());
    for (const LogRecord &record : batch) {
        if (passesFilter(record))
            items.append(makeItem(record));
    }
    if (items.isEmpty()) {
        updateStatus();
        return;
    }

    QScrollBar *scrollBar = m_view->verticalScrollBar();
    const bool atBottom = scrollBar->value() >= scrollBar->maximum() - 4;

    m_view->setUpdatesEnabled(false);
    m_view->addTopLevelItems(items);
    // 顶部超出的部分整块摘掉。takeTopLevelItem 是 O(1) 摘头，逐条删也不贵。
    int excess = m_view->topLevelItemCount() - limit;
    while (excess-- > 0)
        delete m_view->takeTopLevelItem(0);
    m_view->setUpdatesEnabled(true);

    if (atBottom && m_follow->isChecked())
        m_view->scrollToBottom();

    updateStatus();
}

bool LogPanel::passesFilter(const LogRecord &record) const
{
    const QString camera = m_cameraFilter->currentData().toString();
    if (!camera.isEmpty() && record.cameraId != camera)
        return false;

    const LogLevel minimum = static_cast<LogLevel>(m_levelFilter->currentData().toInt());
    if (static_cast<int>(record.level) < static_cast<int>(minimum))
        return false;

    const QString category = m_categoryFilter->currentData().toString();
    if (!category.isEmpty() && record.category != category)
        return false;

    const QString needle = m_search->text().trimmed();
    if (!needle.isEmpty()) {
        if (!record.summary.contains(needle, Qt::CaseInsensitive)
            && !record.peer.contains(needle, Qt::CaseInsensitive)
            && !record.detail.contains(needle, Qt::CaseInsensitive)) {
            return false;
        }
    }
    return true;
}

QTreeWidgetItem *LogPanel::makeItem(const LogRecord &record) const
{
    auto *item = new QTreeWidgetItem;
    item->setText(ColTime, util::formatTime(record.timestamp));
    item->setText(ColLevel, util::levelName(record.level));
    item->setText(ColCamera, record.cameraId);
    item->setText(ColCategory, record.category);
    item->setText(ColPeer, record.peer);
    item->setText(ColDuration, util::formatDurationUs(record.durationUs));
    // 这条行为是被哪条 quirk 促成的，直接写在摘要后面 ——
    // 「怪异行为到底是我自己开的还是真 bug」是排查时的第一个问题。
    item->setText(ColSummary, record.quirkKey.isEmpty()
                                  ? record.summary
                                  : QStringLiteral("%1  [quirk: %2]").arg(record.summary,
                                                                          record.quirkKey));

    const QColor color = levelColor(record.level, record.ok);
    if (color.isValid()) {
        for (int col = 0; col < ColCount; ++col)
            item->setForeground(col, color);
    }

    if (!record.detail.isEmpty()) {
        item->setData(0, kDetailRole, record.detail);
        // 先只挂一个展开箭头，真正的子项等用户点开再建。
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    return item;
}

void LogPanel::onItemExpanded(QTreeWidgetItem *item)
{
    if (!item || item->parent() || item->data(0, kDetailBuiltRole).toBool())
        return;
    const QString detail = item->data(0, kDetailRole).toString();
    if (detail.isEmpty()) {
        item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
        return;
    }

    auto *child = new QTreeWidgetItem(item);
    child->setText(0, detail);
    child->setFont(0, QFontDatabase::systemFont(QFontDatabase::FixedFont));
    child->setFirstColumnSpanned(true);
    item->setData(0, kDetailBuiltRole, true);
}

void LogPanel::rebuildView()
{
    const int limit = m_limit->value();
    m_view->setUpdatesEnabled(false);
    m_view->clear();

    QList<QTreeWidgetItem *> items;
    items.reserve(m_records.size());
    for (const LogRecord &record : m_records) {
        if (passesFilter(record))
            items.append(makeItem(record));
    }
    if (items.size() > limit) {
        // 只留最新的一批
        QList<QTreeWidgetItem *> trimmed = items.mid(items.size() - limit);
        qDeleteAll(items.begin(), items.begin() + (items.size() - limit));
        items = trimmed;
    }
    m_view->addTopLevelItems(items);
    m_view->setUpdatesEnabled(true);
    if (m_follow->isChecked())
        m_view->scrollToBottom();
    updateStatus();
}

void LogPanel::updateStatus()
{
    QString text = tr("显示 %1 / 保留 %2 条").arg(m_view->topLevelItemCount())
                       .arg(m_records.size());
    if (!m_pending.isEmpty())
        text += tr(" · 待刷新 %1").arg(m_pending.size());
    if (m_droppedRecords > 0)
        text += tr(" · 因刷屏丢弃 %1").arg(m_droppedRecords);
    m_status->setText(text);
}

void LogPanel::clearLog()
{
    m_records.clear();
    m_pending.clear();
    m_droppedRecords = 0;
    m_view->clear();
    // 总线上的历史也要清。只清界面的话，重开程序（或者新接一个 SSE 订阅者）
    // 会把刚「清空」掉的记录原样回填回来 —— 用户的预期是「这些日志没了」。
    if (m_simulator && m_simulator->logBus())
        m_simulator->logBus()->clearHistory();
    updateStatus();
}

void LogPanel::exportLog()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("导出日志"),
                                                      QStringLiteral("onvifsim.log"),
                                                      tr("日志文件 (*.log *.txt)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("导出失败"), file.errorString());
        return;
    }

    QTextStream out(&file);
    int count = 0;
    for (const LogRecord &record : m_records) {
        if (!passesFilter(record))
            continue;
        out << record.timestamp.toString(Qt::ISODateWithMs) << QLatin1Char(' ')
            << record.levelName() << QLatin1Char(' ')
            << (record.cameraId.isEmpty() ? QStringLiteral("-") : record.cameraId)
            << QLatin1Char(' ') << record.category << QLatin1Char(' ')
            << (record.peer.isEmpty() ? QStringLiteral("-") : record.peer) << QLatin1Char(' ')
            << record.summary << QLatin1Char('\n');
        if (!record.detail.isEmpty())
            out << record.detail << QLatin1Char('\n');
        ++count;
    }
    out.flush();
    // 写失败也要说。磁盘满 / 只读挂载下不查状态的话，用户拿到的是一句
    // 「已导出 N 条记录」和一个空文件。
    if (out.status() != QTextStream::Ok || file.error() != QFileDevice::NoError) {
        QMessageBox::warning(this, tr("导出失败"),
                             tr("写入 %1 失败：%2").arg(path, file.errorString()));
        return;
    }
    QMessageBox::information(this, tr("导出完成"),
                             tr("已导出 %1 条记录到 %2").arg(count).arg(path));
}

} // namespace gui
} // namespace onvifsim
