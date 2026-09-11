#include "gui/I18n.h"

#include "core/Persona.h"
#include "core/Quirks.h"
#include "events/EventTypes.h"

#include <QtCore/QCoreApplication>

namespace onvifsim {
namespace gui {
namespace i18n {

namespace {

// 统一的查表入口。源串为空时直接返回，避免 translate 拿到空 key。
QString lookup(const char *context, const QString &source)
{
    if (source.isEmpty())
        return source;
    return QCoreApplication::translate(context, source.toUtf8().constData());
}

} // namespace

QString personaName(const Persona &persona)
{
    return lookup("onvifsim::persona", persona.displayName);
}

QString quirkTitle(const QuirkDef &def)
{
    return lookup("onvifsim::quirks", def.title);
}

QString quirkDescription(const QuirkDef &def)
{
    return lookup("onvifsim::quirks", def.description);
}

QString quirkParamDescription(const QuirkParamDef &param)
{
    return lookup("onvifsim::quirks", param.description);
}

QString quirkGroupTitle(const QString &title)
{
    return lookup("onvifsim::quirks", title);
}

QString topicName(const TopicDef &topic)
{
    return lookup("onvifsim::events", topic.displayName);
}

} // namespace i18n
} // namespace gui
} // namespace onvifsim
