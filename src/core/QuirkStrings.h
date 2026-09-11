#pragma once

// 故障注入表里那些要翻译的字串的「占位清单」。
//
// 表本身（Quirks.cpp）是数据，不经过 tr()，lupdate 扫不到。这个头对应的
// QuirkStrings.cpp 是 tools/make-i18n.py 生成的，里面只有 QT_TRANSLATE_NOOP，
// 作用是让 lupdate 把那些字串收进 .ts。界面显示前经 gui/I18n.h 查译文。
//
// 函数本身没有调用者也没关系 —— tst_i18n 会调它做覆盖校验。

#include <QtCore/QStringList>

namespace onvifsim {

QStringList quirkTranslatableStrings();

} // namespace onvifsim
