#include "PopupGeometryObserver.h"

#include <QQmlExtensionPlugin>
#include <qqml.h>

class GradualBlurObserverPlugin final : public QQmlExtensionPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)

public:
    void registerTypes(const char* uri) override {
        qmlRegisterType<PopupGeometryObserver>(uri, 1, 0, "PopupGeometryObserver");
    }
};

#include "Plugin.moc"

