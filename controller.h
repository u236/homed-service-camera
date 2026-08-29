#ifndef CONTROLLER_H
#define CONTROLLER_H

#define SERVICE_VERSION             "1.0.0"
#define REQUEST_TIMEOUT             15
#define RETRY_INTERVAL              10000

#include <QMetaEnum>
#include <QProcess>
#include "device.h"
#include "homed.h"

class Controller : public HOMEd
{
    Q_OBJECT

public:

    enum class Command
    {
        restartService,
        updateDevice,
        removeDevice,
        getStream
    };

    enum class Event
    {
        nameDuplicate,
        incompleteData,
        added,
        updated,
        removed
    };

    Controller(const QString &configFile);

    Q_ENUM(Command)
    Q_ENUM(Event)

private:

    QTimer *m_timer;
    DeviceList *m_devices;

    QMetaEnum m_commands, m_events;
    QString m_url, m_prefix;
    bool m_debug;

    QString streamName(const Device &device, bool mainStream);

    void sendRequest(const QString &method, const QString &path, const QString &id = QString(), const QByteArray &data = QByteArray());
    void updateStream(const QString &name, const QString &source);
    void syncStreams(const QJsonObject &json);

    void publishEvent(const QString &name, Event event);

public slots:

    void quit(void) override;

private slots:

    void mqttConnected(void) override;
    void mqttReceived(const QByteArray &message, const QMqttTopicName &topic) override;

    void requestStreams(void);
    void finished(int exitCode, QProcess::ExitStatus exitStatus);

};

#endif
