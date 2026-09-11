// 翻译覆盖测试。
//
// 界面自己的按钮标签走 tr()，lupdate 扫得到；但**故障注入表与相机预设名是 core
// 的数据**，不经过 tr()。它们靠 tools/make-i18n.py 从表里抽出来灌进 .ts ——
// 加了一条 quirk 却忘了跑那个脚本，症状是「英文界面下这一条是中文」，
// 而编译、单测、e2e 全都不会有任何反应。所以在这里钉住：
// 表里每一条可显示字串，英文译文都必须存在且与源串不同。
//
// 例外是本来就不用翻的专有名词（Reolink / Axis / PTZ 这些），列在 kSameInEnglish。

#include "core/Persona.h"
#include "core/QuirkStrings.h"
#include "core/Quirks.h"
#include "events/EventTypes.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSet>
#include <QtCore/QTranslator>
#include <QtTest/QtTest>

using namespace onvifsim;

namespace {

// 英文与中文写法相同的专有名词，不算漏译。
const QSet<QString> kSameInEnglish = {
    QStringLiteral("Reolink"), QStringLiteral("Axis"),
    QStringLiteral("TP-Link VIGI"), QStringLiteral("TP-Link TL-IPC"),
    QStringLiteral("PTZ"),
};

} // namespace

class TestI18n : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void generatedStubCoversTheTable();
    void quirkTableIsFullyTranslated();
    void personaNamesAreTranslated();
    void eventTopicNamesAreTranslated();
    void guiNeverShowsRawCoreStrings();

private:
    QString translate(const QString &context, const QString &source) const;
    QTranslator m_translator;
    bool m_loaded = false;
};

QString TestI18n::translate(const QString &context, const QString &source) const
{
    return QCoreApplication::translate(context.toUtf8().constData(),
                                       source.toUtf8().constData());
}

void TestI18n::initTestCase()
{
    // .qm 由构建生成，放在可执行文件旁的 i18n/ 下。
    const QString dir = QCoreApplication::applicationDirPath() + QStringLiteral("/i18n");
    m_loaded = m_translator.load(QStringLiteral("onvifsim_en"), dir);
    if (!m_loaded)
        QSKIP("没有编出 onvifsim_en.qm（大概没装 Qt LinguistTools），跳过");
    QVERIFY(QCoreApplication::installTranslator(&m_translator));
}

void TestI18n::generatedStubCoversTheTable()
{
    // src/core/QuirkStrings.cpp 是 tools/make-i18n.py 生成的，存在的意义是
    // 让 lupdate 扫得到表里的字串。表改了却忘了重新生成，这里先挂给你看，
    // 免得只在「英文界面下少了一条译文」时才发现。
    // 先落成具名变量：两次调用返回的是两个不同的临时 QStringList，
    // 从其中一个取 begin、另一个取 end 是未定义行为（这里直接段错误）。
    const QStringList generated = quirkTranslatableStrings();
    const QSet<QString> stub(generated.cbegin(), generated.cend());
    QStringList missing;
    auto want = [&](const QString &s) {
        if (!s.isEmpty() && !stub.contains(s))
            missing.append(s);
    };
    for (const QuirkDef &def : QuirkRegistry::all()) {
        want(QuirkRegistry::groupTitle(def.group));
        want(def.title);
        want(def.description);
        for (const QuirkParamDef &param : def.params)
            want(param.description);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("QuirkStrings.cpp 与 quirk 表不同步，少了 %1 条，"
                                       "跑一遍 tools/make-i18n.py：\n  %2")
                            .arg(missing.size())
                            .arg(missing.mid(0, 5).join(QStringLiteral("\n  ")))));
}

void TestI18n::quirkTableIsFullyTranslated()
{
    QStringList missing;
    auto check = [&](const QString &source) {
        if (source.isEmpty() || kSameInEnglish.contains(source))
            return;
        const QString english = translate(QStringLiteral("onvifsim::quirks"), source);
        if (english == source)
            missing.append(source);
    };

    for (const QuirkDef &def : QuirkRegistry::all()) {
        check(QuirkRegistry::groupTitle(def.group));
        check(def.title);
        check(def.description);
        for (const QuirkParamDef &param : def.params)
            check(param.description);
    }

    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("%1 条 quirk 字串没有英文译文，"
                                       "跑一遍 tools/make-i18n.py 再补 .ts：\n  %2")
                            .arg(missing.size())
                            .arg(missing.mid(0, 8).join(QStringLiteral("\n  ")))));
}

void TestI18n::personaNamesAreTranslated()
{
    QStringList missing;
    for (const Persona &persona : PersonaRegistry::all()) {
        if (kSameInEnglish.contains(persona.displayName))
            continue;
        const QString english = translate(QStringLiteral("onvifsim::persona"),
                                          persona.displayName);
        if (english == persona.displayName)
            missing.append(persona.displayName);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("预设名没有英文译文：%1")
                            .arg(missing.join(QStringLiteral("、")))));
}

void TestI18n::eventTopicNamesAreTranslated()
{
    // 事件页那张表的第一列。和预设名同样是 core 的数据 —— 第一次修 i18n 时
    // 就漏了这一整类，所以单独钉一条。
    QStringList missing;
    for (int i = 0; i < static_cast<int>(EventKind::Count); ++i) {
        const QString source = eventKindDisplayName(static_cast<EventKind>(i));
        if (source.isEmpty() || kSameInEnglish.contains(source))
            continue;
        if (translate(QStringLiteral("onvifsim::events"), source) == source)
            missing.append(source);
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("事件 topic 名没有英文译文：%1")
                            .arg(missing.join(QStringLiteral("、")))));
}

void TestI18n::guiNeverShowsRawCoreStrings()
{
    // 上面那些用例只能证明「译文存在」，证明不了「界面真的去查了译文」——
    // 而实际出问题的两次都是后者：译文明明有，OverviewTab 和事件页却直接
    // 把 core 的原串 setText 上去了，英文界面里就那么一行中文。
    //
    // 所以这里扫源码：GUI 里不准直接碰这些字段，必须经 gui/I18n.h。
    // 是的，这是把 lint 写成了测试 —— 但它拦住的是一类反复出现、
    // 且只有人眼盯着英文界面才看得见的错。
    const QDir gui(QStringLiteral(ONVIFSIM_SOURCE_DIR) + QStringLiteral("/src/gui"));
    QVERIFY2(gui.exists(), qPrintable(gui.absolutePath()));

    // 字段名 → 该走的入口
    const QList<QPair<QString, QString>> forbidden = {
        { QStringLiteral("persona().displayName"), QStringLiteral("i18n::personaName()") },
        { QStringLiteral("persona.displayName"),   QStringLiteral("i18n::personaName()") },
        { QStringLiteral("def.title"),             QStringLiteral("i18n::quirkTitle()") },
        { QStringLiteral("def.description"),       QStringLiteral("i18n::quirkDescription()") },
        { QStringLiteral("param.description"),     QStringLiteral("i18n::quirkParamDescription()") },
        { QStringLiteral("eventKindDisplayName("), QStringLiteral("i18n::topicName()") },
    };

    QStringList problems;
    QDirIterator it(gui.absolutePath(), QStringList() << QStringLiteral("*.cpp"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (QFileInfo(path).fileName() == QLatin1String("I18n.cpp"))
            continue;                      // 桥本身当然要碰这些字段
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        for (const auto &rule : forbidden) {
            if (text.contains(rule.first)) {
                problems.append(QStringLiteral("%1 里直接用了 %2，应该走 %3")
                                    .arg(QFileInfo(path).fileName(), rule.first, rule.second));
            }
        }
    }
    QVERIFY2(problems.isEmpty(), qPrintable(problems.join(QStringLiteral("\n"))));
}

QTEST_MAIN(TestI18n)
#include "tst_i18n.moc"
