#pragma once

// 对讲 Tab：电平表 + 接收统计 + 存 wav。
//
// 对讲是最难排查的一块：客户端以为自己在说话，实际上 RTP 一个包都没进来。
// 这一页把「包进来了没有、丢了多少、talkspurt 首包的 marker 打了没有」
// 摊开摆着，配合故障注入 E13 就能验证客户端的 marker 行为。

#include "gui/tabs/CameraTab.h"
#include "rtsp/RtpReceiver.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace onvifsim {
namespace gui {

class LevelMeterWidget;

class TalkbackTab : public CameraTab
{
    Q_OBJECT
public:
    explicit TalkbackTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    // 当前下拉框选中的那条会话的接收器，没有就返回 nullptr。
    RtpReceiver *currentReceiver() const;
    void reloadSessions();
    // 有对讲轨的会话数。普通取流的会话不进下拉框，兜底判断必须拿这个数比，
    // 不能拿 RtspServer::sessionCount()（那是连接总数）。
    int backchannelSessionCount() const;
    // 把电平表接到当前那条会话的接收器上（换会话时要改接线）。
    void updateLevelConnection();

    QMetaObject::Connection m_levelConnection;

    QComboBox *m_sessions = nullptr;

    // reloadSessions() 末尾会调 refresh()，而 refresh() 的兜底分支又可能调回来。

    // 这个标志把环打断 —— 少了它，一有普通拉流就是无限递归直接爆栈。

    bool m_reloading = false;
    LevelMeterWidget *m_meter = nullptr;
    QTableWidget *m_stats = nullptr;
    QLabel *m_hint = nullptr;
    QPushButton *m_record = nullptr;
    QPushButton *m_reset = nullptr;
};

} // namespace gui
} // namespace onvifsim
