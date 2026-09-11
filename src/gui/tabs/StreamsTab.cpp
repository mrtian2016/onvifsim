#include "gui/tabs/StreamsTab.h"

#include "core/VirtualCamera.h"
#include "media/H264Source.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QSplitter>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

StreamsTab::StreamsTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Vertical, this);
    layout->addWidget(splitter);

    // ---- profiles 表 ----
    auto *top = new QWidget(splitter);
    auto *topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(0, 9, top);
    m_table->setHorizontalHeaderLabels({ tr("Token"), tr("名称"), tr("分辨率"), tr("编码"),
                                         tr("帧率"), tr("码率"), tr("音频"),
                                         tr("PTZConfiguration"), tr("样片") });
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    topLayout->addWidget(m_table);

    auto *buttons = new QHBoxLayout;
    m_addProfile = new QPushButton(tr("添加 profile"), top);
    m_removeProfile = new QPushButton(tr("删除 profile"), top);
    buttons->addWidget(m_addProfile);
    buttons->addWidget(m_removeProfile);
    buttons->addStretch(1);
    topLayout->addLayout(buttons);
    splitter->addWidget(top);

    // ---- 编辑面板 ----
    m_editor = new QGroupBox(tr("编辑选中的 profile"), splitter);
    auto *form = new QFormLayout(m_editor);

    m_token = new QLineEdit(m_editor);
    form->addRow(tr("Token"), m_token);
    m_name = new QLineEdit(m_editor);
    m_name->setToolTip(tr("参照客户端判主 / 子码流纯看这个名字，"
                          "改成没有主子语义的串就能复现「认不出主码流」。"));
    form->addRow(tr("名称"), m_name);

    auto *resolution = new QWidget(m_editor);
    auto *resolutionLayout = new QHBoxLayout(resolution);
    resolutionLayout->setContentsMargins(0, 0, 0, 0);
    m_width = new QSpinBox(resolution);
    m_width->setRange(160, 7680);
    m_width->setSingleStep(16);
    m_height = new QSpinBox(resolution);
    m_height->setRange(120, 4320);
    m_height->setSingleStep(16);
    resolutionLayout->addWidget(m_width);
    resolutionLayout->addWidget(new QLabel(QStringLiteral("×"), resolution));
    resolutionLayout->addWidget(m_height);
    resolutionLayout->addStretch(1);
    form->addRow(tr("声明分辨率"), resolution);

    m_encoding = new QComboBox(m_editor);
    m_encoding->addItems({ QStringLiteral("H264"), QStringLiteral("H265"),
                           QStringLiteral("JPEG") });
    form->addRow(tr("视频编码"), m_encoding);

    m_frameRate = new QDoubleSpinBox(m_editor);
    m_frameRate->setRange(1.0, 60.0);
    m_frameRate->setDecimals(1);
    form->addRow(tr("帧率"), m_frameRate);

    m_bitrate = new QSpinBox(m_editor);
    m_bitrate->setRange(64, 65536);
    m_bitrate->setSuffix(tr(" kbps"));
    form->addRow(tr("码率"), m_bitrate);

    m_govLength = new QSpinBox(m_editor);
    m_govLength->setRange(1, 300);
    form->addRow(tr("GOP 长度"), m_govLength);

    m_hasAudio = new QCheckBox(tr("带音频"), m_editor);
    form->addRow(QString(), m_hasAudio);
    m_audioEncoding = new QComboBox(m_editor);
    m_audioEncoding->addItems({ QStringLiteral("G711"), QStringLiteral("G726"),
                                QStringLiteral("AAC") });
    form->addRow(tr("音频编码"), m_audioEncoding);
    m_audioBitrate = new QSpinBox(m_editor);
    m_audioBitrate->setRange(8, 320);
    m_audioBitrate->setSuffix(tr(" kbps"));
    form->addRow(tr("音频码率"), m_audioBitrate);

    m_hasPtz = new QCheckBox(tr("挂 PTZConfiguration"), m_editor);
    m_hasPtz->setToolTip(tr("很多真机只在子码流上挂，主码流不挂 —— 见故障注入 C1 / C2。"));
    form->addRow(QString(), m_hasPtz);
    m_hasAudioOutput = new QCheckBox(tr("挂 AudioOutput / AudioDecoder（对讲）"), m_editor);
    form->addRow(QString(), m_hasAudioOutput);
    m_hasMetadata = new QCheckBox(tr("挂 Metadata 配置"), m_editor);
    form->addRow(QString(), m_hasMetadata);

    m_streamPath = new QLineEdit(m_editor);
    m_streamPath->setToolTip(tr("留空表示按品牌预设的风格生成。"));
    form->addRow(tr("RTSP 路径"), m_streamPath);

    auto *assetRow = new QWidget(m_editor);
    auto *assetLayout = new QHBoxLayout(assetRow);
    assetLayout->setContentsMargins(0, 0, 0, 0);
    m_mediaAsset = new QComboBox(assetRow);
    m_mediaAsset->setEditable(true);
    m_mediaAsset->addItems(H264Source::builtinAssets());
    m_mediaAsset->setToolTip(tr("内嵌样片档位，或外部 .h264 文件的绝对路径。"
                                "这里发的才是真实码流，与上面的声明值可以故意不一致。"));
    assetLayout->addWidget(m_mediaAsset, 1);
    m_browseAsset = new QPushButton(tr("外部文件…"), assetRow);
    assetLayout->addWidget(m_browseAsset);
    form->addRow(tr("实际样片"), assetRow);

    auto *apply = new QPushButton(tr("应用"), m_editor);
    form->addRow(QString(), apply);
    splitter->addWidget(m_editor);
    splitter->setStretchFactor(0, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged, this,
            [this] { loadEditor(currentProfileIndex()); });
    connect(apply, &QPushButton::clicked, this, &StreamsTab::applyEditor);
    connect(m_hasAudio, &QCheckBox::toggled, this, [this](bool on) {
        m_audioEncoding->setEnabled(on);
        m_audioBitrate->setEnabled(on);
    });
    connect(m_browseAsset, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("选择 H.264 裸流文件"), QString(),
            tr("H.264 裸流 (*.h264 *.264);;所有文件 (*)"));
        if (!path.isEmpty())
            m_mediaAsset->setCurrentText(path);
    });

    connect(m_addProfile, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        if (!cam)
            return;
        CameraModel &model = cam->mutableModel();
        MediaProfile profile;
        const int index = model.profiles.size() + 1;
        profile.token = QStringLiteral("Profile_%1").arg(index);
        profile.name = QStringLiteral("Stream%1").arg(index);
        profile.fixed = false;
        model.profiles.append(profile);
        cam->applyModelChanges();
        reloadTable();
    });
    connect(m_removeProfile, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        const int index = currentProfileIndex();
        if (!cam || index < 0)
            return;
        cam->mutableModel().profiles.removeAt(index);
        cam->applyModelChanges();
        reloadTable();
    });
}

int StreamsTab::currentProfileIndex() const
{
    return m_table->currentRow();
}

void StreamsTab::bindCamera()
{
    if (VirtualCamera *cam = camera()) {
        trackConnection(connect(cam, &VirtualCamera::modelChanged, this, [this] {
            m_dirty = true;
            refreshNow();
        }));
    }
    m_editingRow = -1;
    m_dirty = true;
}

void StreamsTab::refresh()
{
    // 定时刷新对这一页是有害的：重建表格会顺手把编辑面板里没提交的内容冲掉。
    if (m_dirty)
        reloadTable();
}

void StreamsTab::reloadTable()
{
    VirtualCamera *cam = camera();
    m_dirty = false;
    const UiUpdateGuard guard(this);

    const int previous = m_table->currentRow();
    m_table->setRowCount(0);
    if (cam) {
        const QList<MediaProfile> &profiles = cam->model().profiles;
        m_table->setRowCount(profiles.size());
        for (int row = 0; row < profiles.size(); ++row) {
            const MediaProfile &p = profiles.at(row);
            const VideoEncoderConfig &v = p.videoEncoder;
            const QStringList cells = {
                p.token,
                p.name,
                QStringLiteral("%1×%2").arg(v.width).arg(v.height),
                v.encoding,
                QString::number(v.frameRate, 'f', 1),
                tr("%1 kbps").arg(v.bitrateKbps),
                p.hasAudio ? p.audioEncoder.encoding : tr("无"),
                p.hasPtz ? tr("有") : tr("无"),
                p.mediaAsset,
            };
            for (int col = 0; col < cells.size(); ++col) {
                auto *item = new QTableWidgetItem(cells.at(col));
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                m_table->setItem(row, col, item);
            }
        }
        if (previous >= 0 && previous < profiles.size())
            m_table->selectRow(previous);
        else if (!profiles.isEmpty())
            m_table->selectRow(0);
    }
    m_table->resizeColumnsToContents();

    loadEditor(currentProfileIndex());
}

void StreamsTab::loadEditor(int row)
{
    VirtualCamera *cam = camera();
    m_editingRow = row;
    const bool valid = cam && row >= 0 && row < cam->model().profiles.size();
    m_editor->setEnabled(valid);
    m_removeProfile->setEnabled(valid && cam->model().profiles.size() > 1);
    if (!valid)
        return;

    const UiUpdateGuard guard(this);
    const MediaProfile &p = cam->model().profiles.at(row);
    m_token->setText(p.token);
    m_name->setText(p.name);
    m_width->setValue(p.videoEncoder.width);
    m_height->setValue(p.videoEncoder.height);
    m_encoding->setCurrentText(p.videoEncoder.encoding);
    m_frameRate->setValue(p.videoEncoder.frameRate);
    m_bitrate->setValue(p.videoEncoder.bitrateKbps);
    m_govLength->setValue(p.videoEncoder.govLength);
    m_hasAudio->setChecked(p.hasAudio);
    m_audioEncoding->setCurrentText(p.audioEncoder.encoding);
    m_audioBitrate->setValue(p.audioEncoder.bitrateKbps);
    m_audioEncoding->setEnabled(p.hasAudio);
    m_audioBitrate->setEnabled(p.hasAudio);
    m_hasPtz->setChecked(p.hasPtz);
    m_hasAudioOutput->setChecked(p.hasAudioOutput);
    m_hasMetadata->setChecked(p.hasMetadata);
    m_streamPath->setText(p.streamPath);
    m_mediaAsset->setCurrentText(p.mediaAsset);
}

void StreamsTab::applyEditor()
{
    VirtualCamera *cam = camera();
    if (!cam || m_editingRow < 0 || m_editingRow >= cam->model().profiles.size())
        return;

    MediaProfile &p = cam->mutableModel().profiles[m_editingRow];
    p.token = m_token->text();
    p.name = m_name->text();
    p.videoEncoder.width = m_width->value();
    p.videoEncoder.height = m_height->value();
    p.videoEncoder.encoding = m_encoding->currentText();
    p.videoEncoder.frameRate = m_frameRate->value();
    p.videoEncoder.bitrateKbps = m_bitrate->value();
    p.videoEncoder.govLength = m_govLength->value();
    p.hasAudio = m_hasAudio->isChecked();
    p.audioEncoder.encoding = m_audioEncoding->currentText();
    p.audioEncoder.bitrateKbps = m_audioBitrate->value();
    // 对讲要靠 AudioOutput / AudioDecoder 两个配置一起才成立，界面上就当一个开关。
    p.hasAudioOutput = m_hasAudioOutput->isChecked();
    p.hasAudioDecoder = m_hasAudioOutput->isChecked();
    p.hasPtz = m_hasPtz->isChecked();
    p.hasMetadata = m_hasMetadata->isChecked();
    p.streamPath = m_streamPath->text();
    p.mediaAsset = m_mediaAsset->currentText();

    cam->applyModelChanges();
    reloadTable();
}
} // namespace gui
} // namespace onvifsim
