#pragma once

// 故障注入 Tab —— 整个界面里唯一一处「不许硬编码」的地方。
//
// 这一页从 QuirkRegistry::all() 现场生成：分组、标题、出处编号、说明、参数
// 全部来自那张表，控件类型按参数的默认值类型推（枚举→下拉、整数→数字框、
// 小数→小数框、布尔→复选框、文本→输入框）。将来往 Quirks.cpp 里加一条 quirk，
// 这一页自动就多一项，不用回来改任何代码 —— 表是唯一真源。

#include "core/Quirks.h"
#include "gui/tabs/CameraTab.h"

#include <QtCore/QPair>
#include <QtCore/QVector>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace onvifsim {
namespace gui {

class QuirksTab : public CameraTab
{
    Q_OBJECT
public:
    explicit QuirksTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    // 一条 quirk 在界面上的全部零件。
    struct Row {
        QuirkId id = QuirkId::Count;
        QWidget *container = nullptr;      // 整行（复选框 + 参数），过滤时整块隐藏
        QCheckBox *check = nullptr;
        QWidget *paramBox = nullptr;       // 没有参数时为 nullptr
        QVector<QPair<QString, QWidget *>> params;
        QString searchText;                // 预拼好的小写搜索串
        int groupIndex = 0;
    };

    void buildUi();
    QWidget *createParamWidget(const QuirkParamDef &def);
    // 把界面上的值写回目标（当前相机或全局默认）。
    void writeRow(const Row &row);
    void reloadValues();
    void applyFilter();
    void updateSummary();

    Quirks targetQuirks() const;
    void setTargetQuirks(const Quirks &quirks);
    bool editingGlobal() const;

    QComboBox *m_target = nullptr;
    QLineEdit *m_search = nullptr;
    QCheckBox *m_onlyEnabled = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_clearAll = nullptr;
    QPushButton *m_applyGlobal = nullptr;

    QVector<Row> m_rows;
    QVector<QGroupBox *> m_groups;
};

} // namespace gui
} // namespace onvifsim
