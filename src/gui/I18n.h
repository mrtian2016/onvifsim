#pragma once

// 把 core 里的「数据表字串」翻译成界面语言。
//
// 背景：界面自己的按钮和标签走 tr()，翻译在 assets/i18n/*.ts 里。但**相机预设名、
// 故障注入表的标题与说明这些是 core 的数据**，不经过 tr()，于是切成英文之后
// 「添加相机」的下拉是一列中文、故障注入页整页中文 —— 界面的壳翻了、内容没翻。
//
// core 不该依赖界面，所以做法是：源串在 core 的表里用 QT_TRANSLATE_NOOP 标出来
// （lupdate 扫得到、运行期是原样的中文串），界面显示前经过这里查一次译文。
// 查不到译文就原样返回，所以中文界面下这层是透明的。

#include <QtCore/QString>

namespace onvifsim {

struct Persona;
struct QuirkDef;
struct QuirkParamDef;
struct TopicDef;

namespace gui {
namespace i18n {

// 相机预设名。context 与 Persona.cpp 里的 QT_TRANSLATE_NOOP 一致。
QString personaName(const Persona &persona);

// 故障注入表的标题 / 说明 / 参数说明。
QString quirkTitle(const QuirkDef &def);
QString quirkDescription(const QuirkDef &def);
QString quirkParamDescription(const QuirkParamDef &param);

// 分组标题（"发现"、"建连与鉴权" …）。
QString quirkGroupTitle(const QString &title);

// 事件 topic 的显示名（"移动侦测" …）。同样是 core 的数据表。
QString topicName(const TopicDef &topic);

} // namespace i18n
} // namespace gui
} // namespace onvifsim
