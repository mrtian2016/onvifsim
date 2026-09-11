#pragma once

// 流 Tab：profiles 表格 + 选中那条的编辑面板。
//
// 参照客户端判主 / 子码流纯看 profile 的 Name 字串，而分辨率、编码、帧率
// 是靠 ffprobe 探流探出来的、不信 VideoEncoderConfiguration —— 所以这里
// 「声明值」和「实际发的样片」是两列独立的东西，故意分开摆，
// 方便复现「声明 1080p 实发 360p」这类不一致。
//
// 编辑面板里有输入框，所以这一页不跟着每秒刷新走 —— 每秒 setText 一次
// 会让人没法打字。只有模型真的从别处变过才回填，靠 m_dirty 标记。

#include "gui/tabs/CameraTab.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace onvifsim {
namespace gui {

class StreamsTab : public CameraTab
{
    Q_OBJECT
public:
    explicit StreamsTab(Simulator *simulator, QWidget *parent = nullptr);

protected:
    void bindCamera() override;
    void refresh() override;

private:
    void reloadTable();
    void loadEditor(int row);
    void applyEditor();
    int currentProfileIndex() const;

    QTableWidget *m_table = nullptr;
    QPushButton *m_addProfile = nullptr;
    QPushButton *m_removeProfile = nullptr;

    QGroupBox *m_editor = nullptr;
    QLineEdit *m_token = nullptr;
    QLineEdit *m_name = nullptr;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QComboBox *m_encoding = nullptr;
    QDoubleSpinBox *m_frameRate = nullptr;
    QSpinBox *m_bitrate = nullptr;
    QSpinBox *m_govLength = nullptr;
    QCheckBox *m_hasAudio = nullptr;
    QComboBox *m_audioEncoding = nullptr;
    QSpinBox *m_audioBitrate = nullptr;
    QCheckBox *m_hasPtz = nullptr;
    QCheckBox *m_hasAudioOutput = nullptr;
    QCheckBox *m_hasMetadata = nullptr;
    QLineEdit *m_streamPath = nullptr;
    QComboBox *m_mediaAsset = nullptr;
    QPushButton *m_browseAsset = nullptr;

    int m_editingRow = -1;
    bool m_dirty = true;
};

} // namespace gui
} // namespace onvifsim
