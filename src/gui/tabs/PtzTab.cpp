#include "gui/tabs/PtzTab.h"

#include "core/VirtualCamera.h"
#include "gui/widgets/PtzPadWidget.h"
#include "imaging/ImagingState.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

QString moveStatusName(PtzMoveStatus status)
{
    switch (status) {
    case PtzMoveStatus::Idle:
        return PtzTab::tr("空闲");
    case PtzMoveStatus::Moving:
        return PtzTab::tr("移动中");
    case PtzMoveStatus::Unknown:
        return PtzTab::tr("未知");
    }
    return QString();
}
} // namespace

PtzTab::PtzTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *layout = new QHBoxLayout(this);

    // ---- 左：准星 + 摇杆 ----
    auto *left = new QVBoxLayout;

    m_pad = new PtzPadWidget(this);
    left->addWidget(m_pad);

    m_statusLabel = new QLabel(this);
    left->addWidget(m_statusLabel);

    auto *speedRow = new QHBoxLayout;
    speedRow->addWidget(new QLabel(tr("速度"), this));
    m_speed = new QSlider(Qt::Horizontal, this);
    m_speed->setRange(1, 100);
    m_speed->setValue(50);
    speedRow->addWidget(m_speed, 1);
    m_speedLabel = new QLabel(QStringLiteral("0.50"), this);
    speedRow->addWidget(m_speedLabel);
    left->addLayout(speedRow);
    connect(m_speed, &QSlider::valueChanged, this, [this](int value) {
        m_speedLabel->setText(QString::number(value / 100.0, 'f', 2));
    });

    // 八向按钮：按下开始连续移动、松开停止，跟真实客户端的操作方式一致。
    auto *padBox = new QGroupBox(tr("方向"), this);
    auto *grid = new QGridLayout(padBox);
    struct Direction {
        const char *text;
        int row;
        int column;
        double pan;
        double tilt;
    };
    static const Direction directions[] = {
        { "↖", 0, 0, -1.0, 1.0 },  { "↑", 0, 1, 0.0, 1.0 },   { "↗", 0, 2, 1.0, 1.0 },
        { "←", 1, 0, -1.0, 0.0 },  { "→", 1, 2, 1.0, 0.0 },   { "↙", 2, 0, -1.0, -1.0 },
        { "↓", 2, 1, 0.0, -1.0 },  { "↘", 2, 2, 1.0, -1.0 },
    };
    for (const Direction &d : directions) {
        auto *button = new QPushButton(QString::fromUtf8(d.text), padBox);
        button->setFixedWidth(44);
        grid->addWidget(button, d.row, d.column);
        const double pan = d.pan;
        const double tilt = d.tilt;
        connect(button, &QPushButton::pressed, this, [this, pan, tilt] { move(pan, tilt, 0.0); });
        connect(button, &QPushButton::released, this, [this] { move(0.0, 0.0, 0.0); });
    }
    auto *stopButton = new QPushButton(tr("停"), padBox);
    grid->addWidget(stopButton, 1, 1);
    connect(stopButton, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->ptz()->stop();
    });

    auto *zoomIn = new QPushButton(QStringLiteral("Zoom +"), padBox);
    auto *zoomOut = new QPushButton(QStringLiteral("Zoom −"), padBox);
    grid->addWidget(zoomIn, 0, 3);
    grid->addWidget(zoomOut, 2, 3);
    connect(zoomIn, &QPushButton::pressed, this, [this] { move(0.0, 0.0, 1.0); });
    connect(zoomIn, &QPushButton::released, this, [this] { move(0.0, 0.0, 0.0); });
    connect(zoomOut, &QPushButton::pressed, this, [this] { move(0.0, 0.0, -1.0); });
    connect(zoomOut, &QPushButton::released, this, [this] { move(0.0, 0.0, 0.0); });
    left->addWidget(padBox);

    auto *homeRow = new QHBoxLayout;
    auto *setHome = new QPushButton(tr("设为 Home"), this);
    auto *gotoHome = new QPushButton(tr("回 Home"), this);
    homeRow->addWidget(setHome);
    homeRow->addWidget(gotoHome);
    homeRow->addStretch(1);
    left->addLayout(homeRow);
    connect(setHome, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->ptz()->setHomePosition();
    });
    connect(gotoHome, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera()) {
            PtzVector speed;
            speed.pan = speed.tilt = speed.zoom = speedFactor();
            // 返回值要看：没设过 Home 的相机会返回 false，丢掉的话
            // 「回 Home」这个按钮在新建相机上是彻底静默的无反应。
            if (!cam->ptz()->gotoHome(speed)) {
                QMessageBox::warning(this, tr("回 Home 失败"),
                                     tr("这台相机还没有设置过 Home 位置，"
                                        "先点「设为 Home」。"));
            }
        }
    });

    // ---- 图像参数 ----
    auto *imagingBox = new QGroupBox(tr("图像参数"), this);
    auto *imagingForm = new QFormLayout(imagingBox);
    m_brightness = new QSlider(Qt::Horizontal, imagingBox);
    m_brightness->setRange(0, 100);
    imagingForm->addRow(tr("亮度"), m_brightness);
    m_contrast = new QSlider(Qt::Horizontal, imagingBox);
    m_contrast->setRange(0, 100);
    imagingForm->addRow(tr("对比度"), m_contrast);
    m_irCut = new QComboBox(imagingBox);
    m_irCut->addItem(tr("自动"), QStringLiteral("AUTO"));
    m_irCut->addItem(tr("开"), QStringLiteral("ON"));
    m_irCut->addItem(tr("关"), QStringLiteral("OFF"));
    imagingForm->addRow(tr("红外滤片"), m_irCut);
    m_whiteLight = new QCheckBox(tr("白光补光灯"), imagingBox);
    m_whiteLight->setToolTip(tr("ONVIF 没有标准字段，走厂商私有接口，但状态就是这一份。"));
    imagingForm->addRow(QString(), m_whiteLight);
    m_whiteLightBrightness = new QSpinBox(imagingBox);
    m_whiteLightBrightness->setRange(0, 100);
    imagingForm->addRow(tr("补光亮度"), m_whiteLightBrightness);
    left->addWidget(imagingBox);

    connect(m_brightness, &QSlider::valueChanged, this, [this](int value) {
        if (isUpdatingUi())
            return;
        if (VirtualCamera *cam = camera())
            cam->imaging()->setBrightness(value);
    });
    connect(m_contrast, &QSlider::valueChanged, this, [this](int value) {
        if (isUpdatingUi())
            return;
        if (VirtualCamera *cam = camera())
            cam->imaging()->setContrast(value);
    });
    connect(m_irCut, &QComboBox::currentTextChanged, this, [this] {
        if (isUpdatingUi())
            return;
        if (VirtualCamera *cam = camera()) {
            cam->imaging()->setIrCutFilter(
                ImagingState::irCutFilterFromName(m_irCut->currentData().toString()));
        }
    });
    const auto applyWhiteLight = [this] {
        if (isUpdatingUi())
            return;
        if (VirtualCamera *cam = camera()) {
            cam->imaging()->setWhiteLight(m_whiteLight->isChecked(),
                                          m_whiteLightBrightness->value());
        }
    };
    connect(m_whiteLight, &QCheckBox::toggled, this, applyWhiteLight);
    connect(m_whiteLightBrightness, &QSpinBox::valueChanged, this, applyWhiteLight);

    left->addStretch(1);
    layout->addLayout(left);

    // ---- 右：预置位 ----
    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(tr("预置位"), this));

    m_presets = new QTableWidget(0, 5, this);
    m_presets->setHorizontalHeaderLabels(
        { tr("Token"), tr("名称"), QStringLiteral("Pan"), QStringLiteral("Tilt"),
          QStringLiteral("Zoom") });
    m_presets->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_presets->setSelectionMode(QAbstractItemView::SingleSelection);
    m_presets->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_presets->verticalHeader()->setVisible(false);
    m_presets->horizontalHeader()->setStretchLastSection(true);
    right->addWidget(m_presets, 1);

    auto *nameRow = new QHBoxLayout;
    m_presetName = new QLineEdit(this);
    m_presetName->setPlaceholderText(tr("预置位名称"));
    nameRow->addWidget(m_presetName, 1);
    auto *savePreset = new QPushButton(tr("保存当前位置"), this);
    nameRow->addWidget(savePreset);
    right->addLayout(nameRow);

    auto *presetButtons = new QHBoxLayout;
    auto *gotoPreset = new QPushButton(tr("转到"), this);
    auto *removePreset = new QPushButton(tr("删除"), this);
    presetButtons->addWidget(gotoPreset);
    presetButtons->addWidget(removePreset);
    presetButtons->addStretch(1);
    right->addLayout(presetButtons);

    auto *factoryRow = new QHBoxLayout;
    m_factoryCount = new QSpinBox(this);
    m_factoryCount->setRange(1, 1000);
    m_factoryCount->setValue(300);
    factoryRow->addWidget(m_factoryCount);
    auto *factory = new QPushButton(tr("一键生成出厂槽位"), this);
    factory->setToolTip(tr("对应故障注入 C6：真机出厂就把槽位填满，"
                           "客户端把 GetPresets 结果原样铺到界面上会被撑爆。"));
    factoryRow->addWidget(factory);
    auto *clearPresets = new QPushButton(tr("清空"), this);
    factoryRow->addWidget(clearPresets);
    factoryRow->addStretch(1);
    right->addLayout(factoryRow);
    layout->addLayout(right, 1);

    connect(savePreset, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        if (!cam)
            return;
        QString error;
        const QString name = m_presetName->text().isEmpty()
                                 ? tr("预置位 %1").arg(cam->ptz()->presets().size() + 1)
                                 : m_presetName->text();
        if (cam->ptz()->setPreset(name, QString(), &error).isEmpty())
            QMessageBox::warning(this, tr("保存预置位失败"), error);
        reloadPresets();
    });
    connect(gotoPreset, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        const QString token = selectedPresetToken();
        if (!cam || token.isEmpty())
            return;
        PtzVector speed;
        speed.pan = speed.tilt = speed.zoom = speedFactor();
        if (!cam->ptz()->gotoPreset(token, speed)) {
            QMessageBox::warning(this, tr("转到预置位失败"),
                                 tr("预置位 %1 不存在，列表可能已经过期。").arg(token));
            reloadPresets();
        }
    });
    connect(removePreset, &QPushButton::clicked, this, [this] {
        VirtualCamera *cam = camera();
        const QString token = selectedPresetToken();
        if (!cam || token.isEmpty())
            return;
        if (!cam->ptz()->removePreset(token)) {
            QMessageBox::warning(this, tr("删除预置位失败"),
                                 tr("预置位 %1 不存在，列表可能已经过期。").arg(token));
        }
        reloadPresets();
    });
    connect(factory, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->ptz()->generateFactoryPresets(m_factoryCount->value());
        reloadPresets();
    });
    connect(clearPresets, &QPushButton::clicked, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->ptz()->clearPresets();
        reloadPresets();
    });

    connect(m_pad, &PtzPadWidget::velocityRequested, this,
            [this](double pan, double tilt) { move(pan, tilt, 0.0); });
    connect(m_pad, &PtzPadWidget::stopRequested, this, [this] {
        if (VirtualCamera *cam = camera())
            cam->ptz()->stop();
    });
}

double PtzTab::speedFactor() const
{
    return m_speed->value() / 100.0;
}

void PtzTab::move(double pan, double tilt, double zoom)
{
    VirtualCamera *cam = camera();
    if (!cam)
        return;
    PtzVector velocity;
    const double factor = speedFactor();
    velocity.pan = pan * factor;
    velocity.tilt = tilt * factor;
    velocity.zoom = zoom * factor;
    cam->ptz()->continuousMove(velocity);
}

QString PtzTab::selectedPresetToken() const
{
    const int row = m_presets->currentRow();
    if (row < 0)
        return QString();
    QTableWidgetItem *item = m_presets->item(row, 0);
    return item ? item->text() : QString();
}

void PtzTab::bindCamera()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_presets->setRowCount(0);
        return;
    }

    PtzState *ptz = cam->ptz();
    // 位置是「相机在动」时高频变的，直接接信号比每秒轮询顺滑得多。
    trackConnection(connect(ptz, &PtzState::positionChanged, this,
                            [this](const PtzVector &position) { m_pad->setPosition(position); }));
    trackConnection(connect(ptz, &PtzState::presetsChanged, this, &PtzTab::reloadPresets));
    trackConnection(connect(ptz, &PtzState::moveStatusChanged, this, &CameraTab::refreshNow));
    trackConnection(
        connect(cam->imaging(), &ImagingState::settingsChanged, this, &PtzTab::refreshImaging));

    m_factoryCount->setValue(qMax(1, cam->model().ptzNode.maxPresets));
    reloadPresets();
    refreshImaging();
}

void PtzTab::reloadPresets()
{
    VirtualCamera *cam = camera();
    m_presets->setRowCount(0);
    if (!cam)
        return;

    const QList<PtzPreset> presets = cam->ptz()->presets();
    m_presets->setRowCount(presets.size());
    for (int row = 0; row < presets.size(); ++row) {
        const PtzPreset &p = presets.at(row);
        m_presets->setItem(row, 0, new QTableWidgetItem(p.token));
        m_presets->setItem(row, 1, new QTableWidgetItem(p.name));
        m_presets->setItem(row, 2,
                           new QTableWidgetItem(QString::number(p.position.pan, 'f', 3)));
        m_presets->setItem(row, 3,
                           new QTableWidgetItem(QString::number(p.position.tilt, 'f', 3)));
        m_presets->setItem(row, 4,
                           new QTableWidgetItem(QString::number(p.position.zoom, 'f', 3)));
    }
}

void PtzTab::refreshImaging()
{
    VirtualCamera *cam = camera();
    if (!cam)
        return;

    const UiUpdateGuard guard(this);
    const ImagingSettings &s = cam->imaging()->settings();
    m_brightness->setValue(static_cast<int>(s.brightness));
    m_contrast->setValue(static_cast<int>(s.contrast));
    const int index = m_irCut->findData(ImagingState::irCutFilterName(s.irCutFilter).toUpper());
    if (index >= 0)
        m_irCut->setCurrentIndex(index);
    m_whiteLight->setChecked(s.whiteLightOn);
    m_whiteLightBrightness->setValue(s.whiteLightBrightness);
}

void PtzTab::refresh()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        m_statusLabel->clear();
        return;
    }

    PtzState *ptz = cam->ptz();
    m_pad->setPosition(ptz->position());
    const bool moving = ptz->panTiltStatus() == PtzMoveStatus::Moving
                        || ptz->zoomStatus() == PtzMoveStatus::Moving;
    m_pad->setMoving(moving);
    m_statusLabel->setText(tr("PanTilt %1 · Zoom %2 · 预置位 %3 / %4")
                               .arg(moveStatusName(ptz->panTiltStatus()),
                                    moveStatusName(ptz->zoomStatus()))
                               .arg(ptz->presets().size())
                               .arg(ptz->maxPresets()));
}
} // namespace gui
} // namespace onvifsim
