#include "gui/tabs/IdentityTab.h"

#include "gui/I18n.h"
#include "core/Persona.h"
#include "core/VirtualCamera.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

namespace onvifsim {
namespace gui {

namespace {

QStringList userLevelNames()
{
    return { QStringLiteral("Administrator"), QStringLiteral("Operator"), QStringLiteral("User"),
             QStringLiteral("Anonymous") };
}
} // namespace

IdentityTab::IdentityTab(Simulator *simulator, QWidget *parent)
    : CameraTab(simulator, parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    outer->addWidget(scroll);

    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    scroll->setWidget(content);

    // ---- 品牌预设 ----
    auto *personaBox = new QGroupBox(tr("品牌预设"), content);
    auto *personaLayout = new QHBoxLayout(personaBox);
    m_persona = new QComboBox(personaBox);
    for (const Persona &p : PersonaRegistry::all())
        m_persona->addItem(i18n::personaName(p), p.key);
    m_persona->setToolTip(tr("预设决定设备信息三件套、RTSP 路径风格、topic 命名、"
                             "私有 API 桩，以及默认打开哪些故障注入。"));
    personaLayout->addWidget(m_persona, 1);
    m_applyPersona = new QPushButton(tr("套用预设"), personaBox);
    m_applyPersona->setToolTip(tr("用预设的默认值覆盖下面的设备信息与码流路径。"));
    personaLayout->addWidget(m_applyPersona);
    layout->addWidget(personaBox);
    connect(m_applyPersona, &QPushButton::clicked, this, &IdentityTab::applyPersona);

    // ---- 设备信息 ----
    auto *infoBox = new QGroupBox(tr("设备信息"), content);
    auto *form = new QFormLayout(infoBox);
    m_displayName = new QLineEdit(infoBox);
    form->addRow(tr("显示名"), m_displayName);
    m_manufacturer = new QLineEdit(infoBox);
    m_manufacturer->setToolTip(tr("客户端按这个字串挑厂商适配器，必须逐字可改。"));
    form->addRow(tr("厂商 Manufacturer"), m_manufacturer);
    m_model = new QLineEdit(infoBox);
    form->addRow(tr("型号 Model"), m_model);
    m_firmware = new QLineEdit(infoBox);
    form->addRow(tr("固件 FirmwareVersion"), m_firmware);
    m_hardwareId = new QLineEdit(infoBox);
    form->addRow(tr("硬件 id HardwareId"), m_hardwareId);
    m_serial = new QLineEdit(infoBox);
    form->addRow(tr("序列号 SerialNumber"), m_serial);
    m_hostname = new QLineEdit(infoBox);
    form->addRow(tr("主机名"), m_hostname);
    m_location = new QLineEdit(infoBox);
    form->addRow(tr("位置"), m_location);
    m_endpoint = new QLineEdit(infoBox);
    m_endpoint->setReadOnly(true);
    m_endpoint->setToolTip(tr("WS-Discovery 的去重键，单次运行内固定。"));
    form->addRow(tr("EndpointReference"), m_endpoint);
    layout->addWidget(infoBox);

    const QList<QLineEdit *> edits = { m_displayName, m_manufacturer, m_model,    m_firmware,
                                       m_hardwareId,  m_serial,       m_hostname, m_location };
    for (QLineEdit *edit : edits)
        connect(edit, &QLineEdit::editingFinished, this, &IdentityTab::applyToModel);

    // ---- scopes ----
    auto *scopeBox = new QGroupBox(tr("Scopes"), content);
    auto *scopeLayout = new QHBoxLayout(scopeBox);
    m_scopes = new QListWidget(scopeBox);
    m_scopes->setToolTip(tr("完整的 scope URI 列表，ProbeMatch 与 GetScopes 都吐这些。"));
    m_scopes->setMinimumHeight(120);
    scopeLayout->addWidget(m_scopes, 1);

    auto *scopeButtons = new QVBoxLayout;
    auto *addScope = new QPushButton(tr("添加"), scopeBox);
    auto *editScope = new QPushButton(tr("编辑"), scopeBox);
    auto *removeScope = new QPushButton(tr("删除"), scopeBox);
    scopeButtons->addWidget(addScope);
    scopeButtons->addWidget(editScope);
    scopeButtons->addWidget(removeScope);
    scopeButtons->addStretch(1);
    scopeLayout->addLayout(scopeButtons);
    layout->addWidget(scopeBox);

    connect(addScope, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString text = QInputDialog::getText(
            this, tr("添加 scope"), tr("scope URI"), QLineEdit::Normal,
            QStringLiteral("onvif://www.onvif.org/"), &ok);
        if (ok && !text.isEmpty()) {
            m_scopes->addItem(text);
            applyToModel();
        }
    });
    connect(editScope, &QPushButton::clicked, this, [this] {
        QListWidgetItem *item = m_scopes->currentItem();
        if (!item)
            return;
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("编辑 scope"), tr("scope URI"),
                                                   QLineEdit::Normal, item->text(), &ok);
        if (ok && !text.isEmpty()) {
            item->setText(text);
            applyToModel();
        }
    });
    connect(removeScope, &QPushButton::clicked, this, [this] {
        delete m_scopes->takeItem(m_scopes->currentRow());
        applyToModel();
    });

    // ---- 用户表 ----
    auto *userBox = new QGroupBox(tr("用户"), content);
    auto *userLayout = new QHBoxLayout(userBox);
    m_users = new QTableWidget(0, 3, userBox);
    m_users->setHorizontalHeaderLabels({ tr("用户名"), tr("密码"), tr("级别") });
    m_users->horizontalHeader()->setStretchLastSection(true);
    m_users->verticalHeader()->setVisible(false);
    m_users->setMinimumHeight(140);
    userLayout->addWidget(m_users, 1);

    auto *userButtons = new QVBoxLayout;
    auto *addUser = new QPushButton(tr("添加"), userBox);
    auto *removeUser = new QPushButton(tr("删除"), userBox);
    userButtons->addWidget(addUser);
    userButtons->addWidget(removeUser);
    userButtons->addStretch(1);
    userLayout->addLayout(userButtons);
    layout->addWidget(userBox);

    connect(addUser, &QPushButton::clicked, this, [this] {
        User u;
        u.username = QStringLiteral("user%1").arg(m_users->rowCount() + 1);
        u.password = QStringLiteral("pass");
        u.level = UserLevel::Operator;
        addUserRow(u);
        applyToModel();
    });
    connect(removeUser, &QPushButton::clicked, this, [this] {
        const int row = m_users->currentRow();
        if (row < 0)
            return;
        m_users->removeRow(row);
        applyToModel();
    });
    // 表格里的任何改动（改用户名 / 改密码）都立刻写回模型，不设「应用」按钮，
    // 因为忘按它会让人以为鉴权坏了。
    connect(m_users, &QTableWidget::cellChanged, this, [this](int, int) { applyToModel(); });

    layout->addStretch(1);
}

void IdentityTab::addUserRow(const User &user)
{
    const int row = m_users->rowCount();
    m_users->insertRow(row);
    m_users->setItem(row, 0, new QTableWidgetItem(user.username));
    m_users->setItem(row, 1, new QTableWidgetItem(user.password));

    auto *level = new QComboBox(m_users);
    level->addItems(userLevelNames());
    level->setCurrentText(CameraModel::userLevelName(user.level));
    m_users->setCellWidget(row, 2, level);
    connect(level, &QComboBox::currentTextChanged, this, [this] { applyToModel(); });
}

void IdentityTab::bindCamera()
{
    if (VirtualCamera *cam = camera()) {
        trackConnection(connect(cam, &VirtualCamera::modelChanged, this, [this] {
            // 自己写回引起的那次不算：applyToModel() 结束时会把标记清掉。
            m_dirty = true;
            refreshNow();
        }));
    }
    m_dirty = true;
}

void IdentityTab::refresh()
{
    // 每秒一次的定时刷新走到这里也直接退掉 —— 不然用户打字时光标每秒被打回行首。
    if (!m_dirty)
        return;
    reload();
}

void IdentityTab::reload()
{
    VirtualCamera *cam = camera();
    if (!cam) {
        // 同 TalkbackTab：没相机就清干净，别把上一台的身份信息留在框里。
        const UiUpdateGuard guard(this);
        for (QLineEdit *field : { m_displayName, m_manufacturer, m_model, m_firmware,
                                  m_hardwareId, m_serial, m_hostname, m_location, m_endpoint }) {
            field->clear();
        }
        m_scopes->clear();
        m_users->setRowCount(0);
        m_dirty = false;
        return;
    }
    m_dirty = false;

    // 刷新期间禁掉写回，否则 setText / insertRow 会把界面里的旧值倒灌进模型。
    const UiUpdateGuard guard(this);

    const CameraModel &model = cam->model();
    const int personaIndex = m_persona->findData(model.personaKey);
    if (personaIndex >= 0)
        m_persona->setCurrentIndex(personaIndex);

    m_displayName->setText(model.displayName);
    m_manufacturer->setText(model.manufacturer);
    m_model->setText(model.model);
    m_firmware->setText(model.firmwareVersion);
    m_hardwareId->setText(model.hardwareId);
    m_serial->setText(model.serialNumber);
    m_hostname->setText(model.hostname);
    m_location->setText(model.location);
    m_endpoint->setText(model.endpointReference);

    m_scopes->clear();
    m_scopes->addItems(model.scopes);

    m_users->setRowCount(0);
    for (const User &u : model.users)
        addUserRow(u);
}

void IdentityTab::applyPersona()
{
    VirtualCamera *cam = camera();
    if (!cam)
        return;
    cam->setPersona(m_persona->currentData().toString());
    reload();
}

void IdentityTab::applyToModel()
{
    VirtualCamera *cam = camera();
    if (!cam || isUpdatingUi())
        return;

    CameraModel &model = cam->mutableModel();
    model.displayName = m_displayName->text();
    model.manufacturer = m_manufacturer->text();
    model.model = m_model->text();
    model.firmwareVersion = m_firmware->text();
    model.hardwareId = m_hardwareId->text();
    model.serialNumber = m_serial->text();
    model.hostname = m_hostname->text();
    model.location = m_location->text();

    QStringList scopes;
    scopes.reserve(m_scopes->count());
    for (int i = 0; i < m_scopes->count(); ++i)
        scopes.append(m_scopes->item(i)->text());
    model.scopes = scopes;

    QList<User> users;
    for (int row = 0; row < m_users->rowCount(); ++row) {
        User u;
        if (QTableWidgetItem *item = m_users->item(row, 0))
            u.username = item->text();
        if (QTableWidgetItem *item = m_users->item(row, 1))
            u.password = item->text();
        if (auto *level = qobject_cast<QComboBox *>(m_users->cellWidget(row, 2)))
            u.level = CameraModel::userLevelFromName(level->currentText());
        if (!u.username.isEmpty())
            users.append(u);
    }
    model.users = users;

    // 写完必须显式落地：applyModelChanges() 会重建受影响的服务并发 modelChanged。
    const UiUpdateGuard guard(this);
    cam->applyModelChanges();
    // 界面里就是刚写进去的那份，不必再回填一次（回填会打断正在编辑的下一格）。
    m_dirty = false;
}
} // namespace gui
} // namespace onvifsim
