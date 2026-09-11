#pragma once

// 右栏各 Tab 的共同基类。
//
// 每个 Tab 都要做同样三件事：跟着左栏换相机、在相机信号来时刷新、
// 在自己写回模型时避免「信号 → 刷新 → 又发信号」的回环。基类把这三件事收进来，
// 子类只实现 bindCamera() 与 refresh()。
//
// 刷新策略：MainWindow 每秒只调用**当前可见**那个 Tab 的 refresh()，
// 隐藏的 Tab 一行代码都不跑 —— 8 个 Tab 全都定时轮询相机状态是纯浪费。

#include <QtCore/QVector>
#include <QtWidgets/QWidget>

namespace onvifsim {

class Simulator;
class VirtualCamera;

namespace gui {

class CameraTab : public QWidget
{
    Q_OBJECT
public:
    explicit CameraTab(Simulator *simulator, QWidget *parent = nullptr);
    ~CameraTab() override;

    // 切换当前相机：先断开上一台的全部连接，再让子类接上新的。
    void setCamera(VirtualCamera *camera);
    VirtualCamera *camera() const;
    Simulator *simulator() const;

public slots:
    // MainWindow 的定时器与「相机状态变了」都会走这里。
    void refreshNow();

protected:
    // 子类实现：把界面接到 m_camera 上（此时 m_camera 已经是新值，可能为空）。
    virtual void bindCamera() = 0;
    // 子类实现：只读地刷新显示，不要在这里写回模型。
    virtual void refresh() = 0;

    // 记下与当前相机相关的连接，换相机时基类统一断开。
    void trackConnection(const QMetaObject::Connection &connection);

    // 写回模型 / quirks 时置位，避免自己触发的信号又把界面重刷一遍。
    // 「界面正在被代码回填」——各页的槽靠它区分「用户改的」和「我自己填的」。
    bool isUpdatingUi() const;

    // 作用域标记。**必须是计数器而不是 bool**：回填过程里常常会同步触发别的
    // 回填（StreamsTab::reloadTable() 里 selectRow() 会触发 itemSelectionChanged
    // → loadEditor()，而 loadEditor() 自己也要标一次），bool 的话内层那次「关」
    // 会把外层的保护一并清掉，外层从那一行往后就裸奔了。
    // 做成 RAII 还顺带解决了「中间 return 一下就漏了收尾」这类问题。
    class UiUpdateGuard
    {
    public:
        explicit UiUpdateGuard(CameraTab *tab) : m_tab(tab)
        {
            if (m_tab)
                ++m_tab->m_updating;
        }
        ~UiUpdateGuard()
        {
            if (m_tab)
                --m_tab->m_updating;
        }

    private:
        Q_DISABLE_COPY(UiUpdateGuard)
        CameraTab *m_tab = nullptr;
    };

private:
    friend class UiUpdateGuard;

    Simulator *m_simulator = nullptr;
    VirtualCamera *m_camera = nullptr;
    QVector<QMetaObject::Connection> m_connections;
    int m_updating = 0;
};

} // namespace gui
} // namespace onvifsim
