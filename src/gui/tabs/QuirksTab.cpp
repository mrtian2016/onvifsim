#include "gui/tabs/QuirksTab.h"

#include "gui/I18n.h"

#include "core/Simulator.h"
#include "core/VirtualCamera.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <limits>

namespace onvifsim {
namespace gui {

namespace {

// 参数控件里存的是参数名，写回时按名字取。
constexpr char kParamNameProperty[] = "onvifsim_param";

QVariant widgetValue(QWidget *widget)
{
    if (auto *combo = qobject_cast<QComboBox *>(widget))
        return combo->currentText();
    if (auto *check = qobject_cast<QCheckBox *>(widget))
        return check->isChecked();
    if (auto *spin = qobject_cast<QSpinBox *>(widget))
        return spin->value();
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget))
        return spin->value();
    if (auto *edit = qobject_cast<QLineEdit *>(widget))
        return edit->text();
    return QVariant();
}

void setWidgetValue(QWidget *widget, const QVariant &value)
{
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        const int index = combo->findText(value.toString());
        combo->setCurrentIndex(index >= 0 ? index : 0);
        return;
    }
    if (auto *check = qobject_cast<QCheckBox *>(widget)) {
        check->setChecked(value.toBool());
        return;
    }
    if (auto *spin = qobject_cast<QSpinBox *>(widget)) {
        spin->setValue(value.toInt());
        return;
    }
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget)) {
        spin->setValue(value.toDouble());
        return;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(widget))
        edit->setText(value.toString());
}
} // namespace

QuirksTab::QuirksTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    buildUi();
}

void QuirksTab::buildUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    // ---- 顶部：目标、过滤、批量操作 ----
    auto *header = new QHBoxLayout;
    header->addWidget(new QLabel(tr("作用于"), this));
    m_target = new QComboBox(this);
    m_target->addItem(tr("当前相机"), false);
    m_target->addItem(tr("全局默认（新建相机的初值）"), true);
    header->addWidget(m_target);

    m_search = new QLineEdit(this);
    m_search->setPlaceholderText(tr("搜索标题 / 说明 / 出处编号 / key"));
    m_search->setClearButtonEnabled(true);
    header->addWidget(m_search, 1);

    m_onlyEnabled = new QCheckBox(tr("只看已开启"), this);
    header->addWidget(m_onlyEnabled);

    m_clearAll = new QPushButton(tr("全部关闭"), this);
    header->addWidget(m_clearAll);
    m_applyGlobal = new QPushButton(tr("把全局默认应用到全部相机"), this);
    header->addWidget(m_applyGlobal);
    layout->addLayout(header);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    connect(m_target, &QComboBox::currentIndexChanged, this, [this] { reloadValues(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(m_onlyEnabled, &QCheckBox::toggled, this, [this] { applyFilter(); });
    connect(m_clearAll, &QPushButton::clicked, this, [this] {
        Quirks quirks = targetQuirks();
        quirks.clear();
        setTargetQuirks(quirks);
        reloadValues();
    });
    connect(m_applyGlobal, &QPushButton::clicked, this, [this] {
        if (simulator())
            simulator()->applyGlobalQuirksToAll();
        reloadValues();
    });

    // ---- 主体：按分组现场生成 ----
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);

    auto *content = new QWidget;
    auto *contentLayout = new QVBoxLayout(content);
    scroll->setWidget(content);

    // 分组顺序照 QuirkRegistry 表里出现的先后，不另外排 —— 表怎么写界面就怎么排。
    QVector<QuirkGroup> groupOrder;
    for (const QuirkDef &def : QuirkRegistry::all()) {
        if (!groupOrder.contains(def.group))
            groupOrder.append(def.group);
    }

    for (const QuirkGroup group : groupOrder) {
        // 分组标题带上这一组里出现过的出处字母，方便和 reference-client-facts.md 对照。
        QStringList letters;
        for (const QuirkDef &def : QuirkRegistry::all()) {
            if (def.group != group || def.sourceId.isEmpty())
                continue;
            const QString letter = def.sourceId.left(1);
            if (!letters.contains(letter))
                letters.append(letter);
        }
        const QString title = letters.isEmpty()
                                  ? i18n::quirkGroupTitle(QuirkRegistry::groupTitle(group))
                                  : QStringLiteral("%1 · %2").arg(letters.join(QLatin1Char('/')),
                                                                  i18n::quirkGroupTitle(QuirkRegistry::groupTitle(group)));

        auto *box = new QGroupBox(title, content);
        auto *boxLayout = new QVBoxLayout(box);
        contentLayout->addWidget(box);
        const int groupIndex = m_groups.size();
        m_groups.append(box);

        for (const QuirkDef &def : QuirkRegistry::all()) {
            if (def.group != group)
                continue;

            Row row;
            row.id = def.id;
            row.groupIndex = groupIndex;

            row.container = new QWidget(box);
            auto *rowLayout = new QVBoxLayout(row.container);
            rowLayout->setContentsMargins(0, 0, 0, 4);
            rowLayout->setSpacing(2);

            const QString label = def.sourceId.isEmpty()
                                      ? i18n::quirkTitle(def)
                                      : QStringLiteral("%1 · %2").arg(def.sourceId, i18n::quirkTitle(def));
            row.check = new QCheckBox(label, row.container);
            // 说明里写着这条 quirk 是从哪台真机上抓下来的，鼠标悬停就能看到。
            row.check->setToolTip(QStringLiteral("%1\n\nkey: %2").arg(i18n::quirkDescription(def), def.key));
            rowLayout->addWidget(row.check);

            if (!def.params.isEmpty()) {
                row.paramBox = new QWidget(row.container);
                auto *form = new QFormLayout(row.paramBox);
                form->setContentsMargins(24, 0, 0, 0);
                form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
                for (const QuirkParamDef &param : def.params) {
                    QWidget *widget = createParamWidget(param);
                    if (!widget)
                        continue;
                    widget->setParent(row.paramBox);
                    widget->setProperty(kParamNameProperty, param.name);
                    widget->setToolTip(i18n::quirkParamDescription(param));
                    form->addRow(param.name, widget);
                    row.params.append(qMakePair(param.name, widget));
                }
                rowLayout->addWidget(row.paramBox);
            }

            row.searchText = (def.sourceId + QLatin1Char(' ') + def.key + QLatin1Char(' ')
                              + i18n::quirkTitle(def) + QLatin1Char(' ') + i18n::quirkDescription(def))
                                 .toLower();
            boxLayout->addWidget(row.container);
            m_rows.append(row);
        }
    }
    contentLayout->addStretch(1);

    // 连接放在最后统一做：建界面时 setChecked / setValue 会发信号，
    // 但那时 m_rows 还没填完，接早了会写回半成品。
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        connect(row.check, &QCheckBox::toggled, this, [this, i](bool on) {
            if (m_rows.at(i).paramBox)
                m_rows.at(i).paramBox->setEnabled(on);
            if (isUpdatingUi())
                return;
            writeRow(m_rows.at(i));
            updateSummary();
            applyFilter();
        });

        for (const auto &param : row.params) {
            QWidget *widget = param.second;
            const auto write = [this, i] {
                if (isUpdatingUi())
                    return;
                writeRow(m_rows.at(i));
            };
            if (auto *combo = qobject_cast<QComboBox *>(widget))
                connect(combo, &QComboBox::currentIndexChanged, this, write);
            else if (auto *check = qobject_cast<QCheckBox *>(widget))
                connect(check, &QCheckBox::toggled, this, write);
            else if (auto *spin = qobject_cast<QSpinBox *>(widget))
                connect(spin, &QSpinBox::valueChanged, this, write);
            else if (auto *dspin = qobject_cast<QDoubleSpinBox *>(widget))
                connect(dspin, &QDoubleSpinBox::valueChanged, this, write);
            else if (auto *edit = qobject_cast<QLineEdit *>(widget))
                connect(edit, &QLineEdit::editingFinished, this, write);
        }
    }
}

QWidget *QuirksTab::createParamWidget(const QuirkParamDef &def)
{
    // 控件类型完全由元数据推出来：有 choices 就是枚举，否则看默认值的类型。
    if (!def.choices.isEmpty()) {
        auto *combo = new QComboBox;
        combo->addItems(def.choices);
        return combo;
    }

    switch (def.defaultValue.typeId()) {
    case QMetaType::Bool: {
        auto *check = new QCheckBox;
        return check;
    }
    case QMetaType::Double: {
        auto *spin = new QDoubleSpinBox;
        spin->setDecimals(3);
        spin->setRange(def.minValue.isValid() ? def.minValue.toDouble() : -1.0e9,
                       def.maxValue.isValid() ? def.maxValue.toDouble() : 1.0e9);
        spin->setSingleStep(0.1);
        return spin;
    }
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong: {
        auto *spin = new QSpinBox;
        spin->setRange(def.minValue.isValid() ? def.minValue.toInt() : 0,
                       def.maxValue.isValid() ? def.maxValue.toInt()
                                              : std::numeric_limits<int>::max());
        return spin;
    }
    default:
        break;
    }

    auto *edit = new QLineEdit;
    return edit;
}

bool QuirksTab::editingGlobal() const
{
    return m_target->currentData().toBool();
}

Quirks QuirksTab::targetQuirks() const
{
    if (editingGlobal())
        return simulator() ? simulator()->globalQuirks() : Quirks();
    return camera() ? camera()->quirks() : Quirks();
}

void QuirksTab::setTargetQuirks(const Quirks &quirks)
{
    if (editingGlobal()) {
        if (simulator())
            simulator()->setGlobalQuirks(quirks);
        return;
    }
    if (camera()) {
        // 走 setQuirks 而不是 mutableQuirks：有几条 quirk（比如 C6 的出厂预置位）
        // 需要在开关翻转时做副作用，那些逻辑都在 setQuirks 里。
        const UiUpdateGuard guard(this);
        camera()->setQuirks(quirks);
    }
}

void QuirksTab::writeRow(const Row &row)
{
    Quirks quirks = targetQuirks();
    quirks.setEnabled(row.id, row.check->isChecked());
    for (const auto &param : row.params) {
        const QVariant value = widgetValue(param.second);
        if (value.isValid())
            quirks.setParam(row.id, param.first, value);
    }
    setTargetQuirks(quirks);
}

void QuirksTab::reloadValues()
{
    const bool global = editingGlobal();
    // 全局默认与相机无关，没选相机时也能编。
    setEnabled(global || camera() != nullptr);
    m_applyGlobal->setEnabled(global);

    const Quirks quirks = targetQuirks();

    const UiUpdateGuard guard(this);
    for (const Row &row : m_rows) {
        const bool on = quirks.isEnabled(row.id);
        row.check->setChecked(on);
        if (row.paramBox)
            row.paramBox->setEnabled(on);
        for (const auto &param : row.params)
            setWidgetValue(param.second, quirks.param(row.id, param.first));
    }

    updateSummary();
    applyFilter();
}

void QuirksTab::updateSummary()
{
    const Quirks quirks = targetQuirks();
    m_summary->setText(tr("共 %1 条故障注入，已开启 %2 条。悬停任意一项可以看到它的出处说明。")
                           .arg(m_rows.size())
                           .arg(quirks.enabledIds().size()));
}

void QuirksTab::applyFilter()
{
    const QString needle = m_search->text().trimmed().toLower();
    const bool onlyEnabled = m_onlyEnabled->isChecked();

    QVector<int> visiblePerGroup(m_groups.size(), 0);
    for (const Row &row : m_rows) {
        bool visible = needle.isEmpty() || row.searchText.contains(needle);
        if (visible && onlyEnabled)
            visible = row.check->isChecked();
        row.container->setVisible(visible);
        if (visible)
            ++visiblePerGroup[row.groupIndex];
    }
    // 整组都被过滤掉时连标题一起收走，免得留一排空框子。
    for (int i = 0; i < m_groups.size(); ++i)
        m_groups.at(i)->setVisible(visiblePerGroup.at(i) > 0);
}

void QuirksTab::bindCamera()
{
    if (VirtualCamera *cam = camera()) {
        trackConnection(connect(cam, &VirtualCamera::quirksChanged, this, [this] {
            if (!isUpdatingUi() && !editingGlobal())
                reloadValues();
        }));
    }
    reloadValues();
}

void QuirksTab::refresh()
{
    // 故障注入的状态只在被改动时变，不需要跟着每秒刷新 —— 这里刻意留空，
    // 否则用户正在下拉框里选东西时会被定时刷新打断。
}
} // namespace gui
} // namespace onvifsim
