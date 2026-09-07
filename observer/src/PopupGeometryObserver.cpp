#include "PopupGeometryObserver.h"

#include <QColor>
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QMetaObject>
#include <QMetaProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QWindow>

#include <algorithm>

namespace {
QString defaultSocketPath() {
    auto runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.isEmpty()) runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return runtime + "/gradual-blur.sock";
}

QString defaultConfigPath() {
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
        + "/.config/hypr/gradual-blur/config.jsonc";
}

bool containsRect(const QRectF& outer, const QRectF& inner) {
    constexpr qreal epsilon = 1.5;
    return outer.adjusted(-epsilon, -epsilon, epsilon, epsilon).contains(inner);
}
}

PopupGeometryObserver::PopupGeometryObserver(QObject* parent)
    : QObject(parent), m_socketPath(defaultSocketPath()), m_configPath(defaultConfigPath()),
      m_socket(new QLocalSocket(this)) {
    m_excluded = {
        "omarchy-bar", "omarchy-bar-drag-ghost", "omarchy-bar-move-ghost",
        "omarchy-background", "omarchy-dock", "omarchy-dock-edge",
        "omarchy-keyboard-panel-dismiss", "omarchy-lock-preview"
    };

    m_scanTimer.setInterval(400);
    connect(&m_scanTimer, &QTimer::timeout, this, &PopupGeometryObserver::scan);

    m_reconnectTimer.setInterval(1000);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &PopupGeometryObserver::reconnect);
    connect(m_socket, &QLocalSocket::connected, this, &PopupGeometryObserver::socketStateChanged);
    connect(m_socket, &QLocalSocket::disconnected, this, &PopupGeometryObserver::socketStateChanged);
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this] { socketStateChanged(); });

    loadConfig();
    m_scanTimer.start();
    m_reconnectTimer.start();
    QTimer::singleShot(0, this, &PopupGeometryObserver::reconnect);
    QTimer::singleShot(0, this, &PopupGeometryObserver::scan);
}

PopupGeometryObserver::~PopupGeometryObserver() = default;

QString PopupGeometryObserver::socketPath() const { return m_socketPath; }
QString PopupGeometryObserver::configPath() const { return m_configPath; }
bool PopupGeometryObserver::connected() const { return m_socket->state() == QLocalSocket::ConnectedState; }

void PopupGeometryObserver::setSocketPath(const QString& value) {
    if (value == m_socketPath || value.isEmpty()) return;
    m_socketPath = value;
    m_socket->abort();
    emit socketPathChanged();
    reconnect();
}

void PopupGeometryObserver::setConfigPath(const QString& value) {
    if (value == m_configPath || value.isEmpty()) return;
    m_configPath = value;
    emit configPathChanged();
    loadConfig();
    scan();
}

void PopupGeometryObserver::socketStateChanged() {
    emit connectedChanged();
    if (connected()) {
        m_lastPayload.clear();
        scan();
    }
}

void PopupGeometryObserver::reconnect() {
    if (m_socketPath.isEmpty() || m_socket->state() != QLocalSocket::UnconnectedState) return;
    m_socket->connectToServer(m_socketPath, QIODevice::WriteOnly);
}

void PopupGeometryObserver::loadConfig() {
    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) return;
    auto raw = QString::fromUtf8(file.readAll());
    raw.remove(QRegularExpression(R"((?m)^\s*//.*$)"));
    const auto doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isObject()) return;
    const auto root = doc.object();
    const auto detector = root.value("detector").toObject();
    m_minWidth = std::max(1, detector.value("min_width").toInt(m_minWidth));
    m_minHeight = std::max(1, detector.value("min_height").toInt(m_minHeight));
    m_maxCards = std::clamp(detector.value("max_cards").toInt(m_maxCards), 1, 128);
    const auto excluded = root.value("exclude_namespaces").toArray();
    for (const auto& value : excluded) m_excluded.insert(value.toString());
}

QString PopupGeometryObserver::namespaceBelow(QObject* object, int depth) const {
    if (!object || depth > 8) return {};
    const auto meta = object->metaObject();
    const QString className = meta ? QString::fromLatin1(meta->className()) : QString();
    const int index = meta ? meta->indexOfProperty("namespace") : -1;
    if (index >= 0 && (className.contains("Wlr", Qt::CaseInsensitive)
                      || className.contains("Layer", Qt::CaseInsensitive))) {
        const auto value = meta->property(index).read(object).toString();
        if (!value.isEmpty()) return value;
    }
    for (auto* child : object->children()) {
        const auto found = namespaceBelow(child, depth + 1);
        if (!found.isEmpty()) return found;
    }
    return {};
}

QString PopupGeometryObserver::namespaceFor(QQuickWindow* window) const {
    auto found = namespaceBelow(window);
    if (found.isEmpty() && window->contentItem()) found = namespaceBelow(window->contentItem());
    return found;
}

bool PopupGeometryObserver::isExcluded(const QString& name) const {
    return name.isEmpty() || m_excluded.contains(name);
}

bool PopupGeometryObserver::isRectangleLike(const QMetaObject* meta) const {
    for (auto* current = meta; current; current = current->superClass()) {
        const QString name = QString::fromLatin1(current->className());
        if (name.contains("BorderSurface", Qt::CaseInsensitive)
            || name.contains("PopupCard", Qt::CaseInsensitive)
            || name.contains("NotificationCard", Qt::CaseInsensitive)
            || name.contains("QQuickRectangle", Qt::CaseInsensitive)) return true;
    }
    return false;
}

qreal PopupGeometryObserver::effectiveOpacity(QQuickItem* item) const {
    qreal opacity = 1.0;
    for (auto* current = item; current; current = current->parentItem()) {
        if (!current->isVisible()) return 0.0;
        opacity *= current->opacity();
    }
    return opacity;
}

void PopupGeometryObserver::collectCandidates(QQuickItem* item, QQuickWindow* window,
                                               int depth, QList<Candidate>& out) const {
    if (!item || !window || !item->isVisible()) return;
    const qreal opacity = effectiveOpacity(item);
    if (opacity > 0.015 && isRectangleLike(item->metaObject())) {
        const auto colorValue = item->property("color");
        const auto color = colorValue.value<QColor>();
        const auto rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height())).normalized();
        const auto available = window->screen() ? window->screen()->geometry().size() : QSize();
        const bool screenSizedSurface = available.isValid()
            && window->width() >= available.width() * 0.90
            && window->height() >= available.height() * 0.90;
        const bool fillsSurface = screenSizedSurface
            && rect.width() >= window->width() * 0.96
            && rect.height() >= window->height() * 0.96;
        if (color.isValid() && color.alphaF() * opacity > 0.025 && !fillsSurface
            && rect.width() >= m_minWidth && rect.height() >= m_minHeight) {
            Candidate candidate;
            candidate.item = item;
            candidate.rect = rect;
            candidate.radius = std::max<qreal>(0, item->property("radius").toReal());
            candidate.opacity = opacity;
            candidate.depth = depth;
            out.push_back(candidate);
        }
    }
    for (auto* child : item->childItems()) collectCandidates(child, window, depth + 1, out);
}

// Push-based live tracking: geometry or visibility changes on a detected
// card, any of its ancestors, or its window are published the same event
// loop pass. The periodic scan stays only as a structural net for new
// windows and items that no signal could announce.
void PopupGeometryObserver::schedulePublish() {
    if (m_publishScheduled) return;
    m_publishScheduled = true;
    QMetaObject::invokeMethod(this, [this] {
        m_publishScheduled = false;
        scan();
    }, Qt::QueuedConnection);
}

void PopupGeometryObserver::watchWindow(QQuickWindow* window) {
    if (!window) return;
    for (const auto& watched : m_watchedWindows)
        if (watched.data() == window) return;
    m_watchedWindows.push_back(window);
    const auto notify = [this] { schedulePublish(); };
    connect(window, &QWindow::xChanged, this, notify);
    connect(window, &QWindow::yChanged, this, notify);
    connect(window, &QWindow::widthChanged, this, notify);
    connect(window, &QWindow::heightChanged, this, notify);
    connect(window, &QWindow::visibleChanged, this, notify);
}

void PopupGeometryObserver::watchItemChain(QQuickItem* item) {
    for (auto* current = item; current; current = current->parentItem()) {
        bool known = false;
        for (const auto& watched : m_watchedItems)
            if (watched.data() == current) { known = true; break; }
        if (!known) {
            m_watchedItems.push_back(current);
            const auto notify = [this] { schedulePublish(); };
            connect(current, &QQuickItem::xChanged, this, notify);
            connect(current, &QQuickItem::yChanged, this, notify);
            connect(current, &QQuickItem::widthChanged, this, notify);
            connect(current, &QQuickItem::heightChanged, this, notify);
            connect(current, &QQuickItem::visibleChanged, this, notify);
            connect(current, &QQuickItem::visibleChildrenChanged, this, notify);
            connect(current, &QQuickItem::opacityChanged, this, notify);
            connect(current, &QQuickItem::parentChanged, this, notify);
            connect(current, &QQuickItem::windowChanged, this, notify);
        }
    }
}

QJsonArray PopupGeometryObserver::cardsFor(QQuickWindow* window, const QString& name) {
    QList<Candidate> all;
    collectCandidates(window->contentItem(), window, 0, all);

    QList<Candidate> outer;
    for (const auto& candidate : all) {
        bool nested = false;
        for (const auto& other : all) {
            if (candidate.item == other.item || other.rect.size() == candidate.rect.size()) continue;
            if (other.item && candidate.item && other.item->isAncestorOf(candidate.item)
                && other.rect.width() * other.rect.height() > candidate.rect.width() * candidate.rect.height()
                && containsRect(other.rect, candidate.rect)) {
                nested = true;
                break;
            }
        }
        if (!nested) outer.push_back(candidate);
    }

    std::sort(outer.begin(), outer.end(), [](const Candidate& a, const Candidate& b) {
        return a.depth < b.depth;
    });

    for (const auto& candidate : outer)
        watchItemChain(candidate.item.data());

    QJsonArray cards;
    const auto screenName = window->screen() ? window->screen()->name() : QString();
    const qreal dpr = window->screen() ? window->screen()->devicePixelRatio() : 1.0;
    QPoint windowOrigin(window->x(), window->y());
    if (window->screen()) windowOrigin -= window->screen()->geometry().topLeft();
    for (const auto& candidate : outer) {
        if (cards.size() >= m_maxCards) break;
        const auto rect = candidate.rect.translated(windowOrigin);
        QJsonObject card;
        card["id"] = QString::number(reinterpret_cast<quintptr>(candidate.item.data()), 16);
        card["namespace"] = name;
        card["output"] = screenName;
        card["x"] = rect.x();
        card["y"] = rect.y();
        card["w"] = rect.width();
        card["h"] = rect.height();
        card["radius"] = candidate.radius;
        card["opacity"] = candidate.opacity;
        card["dpr"] = dpr;
        cards.push_back(card);
    }
    return cards;
}

void PopupGeometryObserver::publish(const QByteArray& payload) {
    if (!connected()) return;
    if (m_socket->write(payload + '\n') < 0) m_socket->abort();
    else m_socket->flush();
}

void PopupGeometryObserver::scan() {
    m_watchedWindows.removeIf([](const QPointer<QQuickWindow>& watched) { return watched.isNull(); });
    m_watchedItems.removeIf([](const QPointer<QQuickItem>& watched) { return watched.isNull(); });

    QJsonArray cards;
    bool anyEligibleWindow = false;
    for (auto* baseWindow : QGuiApplication::allWindows()) {
        auto* window = qobject_cast<QQuickWindow*>(baseWindow);
        if (!window || !window->isVisible() || !window->contentItem()) continue;
        auto name = namespaceFor(window);
        // Quickshell's layer-shell attached object is intentionally not part of
        // the QObject child tree in some releases. Card geometry is still fully
        // observable; use a neutral namespace and let empty/background surfaces
        // fall out naturally because they contain no non-fullscreen card.
        if (name.isEmpty()) name = "quickshell-popup";
        if (isExcluded(name)) continue;
        anyEligibleWindow = true;
        watchWindow(window);
        const auto found = cardsFor(window, name);
        for (const auto& card : found) cards.push_back(card);
    }

    m_scanTimer.setInterval(anyEligibleWindow ? 33 : 400);
    QJsonObject content;
    content["cards"] = cards;
    const auto signature = QJsonDocument(content).toJson(QJsonDocument::Compact);
    if (signature == m_lastPayload) return;
    m_lastPayload = signature;
    QJsonObject root;
    root["version"] = 1;
    root["generation"] = QString::number(++m_generation);
    root["cards"] = cards;
    const auto payload = QJsonDocument(root).toJson(QJsonDocument::Compact);
    publish(payload);
}
