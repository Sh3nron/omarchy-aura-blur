#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QTimer>

class QLocalSocket;
class QQuickItem;
class QQuickWindow;

class PopupGeometryObserver : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString socketPath READ socketPath WRITE setSocketPath NOTIFY socketPathChanged)
    Q_PROPERTY(QString configPath READ configPath WRITE setConfigPath NOTIFY configPathChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)

public:
    explicit PopupGeometryObserver(QObject* parent = nullptr);
    ~PopupGeometryObserver() override;

    QString socketPath() const;
    void setSocketPath(const QString& value);
    QString configPath() const;
    void setConfigPath(const QString& value);
    bool connected() const;

signals:
    void socketPathChanged();
    void configPathChanged();
    void connectedChanged();

private slots:
    void scan();
    void reconnect();
    void socketStateChanged();

private:
    struct Candidate {
        QPointer<QQuickItem> item;
        QRectF rect;
        qreal radius = 0;
        qreal opacity = 1;
        int depth = 0;
    };

    QString namespaceFor(QQuickWindow* window) const;
    QString namespaceBelow(QObject* object, int depth = 0) const;
    bool isExcluded(const QString& name) const;
    bool isRectangleLike(const QMetaObject* meta) const;
    qreal effectiveOpacity(QQuickItem* item) const;
    void collectCandidates(QQuickItem* item, QQuickWindow* window, int depth,
                           QList<Candidate>& out) const;
    QJsonArray cardsFor(QQuickWindow* window, const QString& name) const;
    void loadConfig();
    void publish(const QByteArray& payload);

    QString m_socketPath;
    QString m_configPath;
    QLocalSocket* m_socket = nullptr;
    QTimer m_scanTimer;
    QTimer m_reconnectTimer;
    QByteArray m_lastPayload;
    quint64 m_generation = 0;
    QSet<QString> m_excluded;
    int m_minWidth = 48;
    int m_minHeight = 32;
    int m_maxCards = 32;
};
