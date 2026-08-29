#include "controller.h"
#include "logger.h"

Controller::Controller(const QString &configFile) : HOMEd(SERVICE_VERSION, configFile), m_timer(new QTimer(this)), m_devices(new DeviceList(getConfig(), this)), m_commands(QMetaEnum::fromType <Command> ()), m_events(QMetaEnum::fromType <Event> ())
{
    m_url = getConfig()->value("rtc/url", "http://localhost:1984").toString();
    m_prefix = getConfig()->value("rtc/prefix", "homed").toString();
    m_debug = getConfig()->value("rtc/debug", false).toBool();

    connect(m_timer, &QTimer::timeout, this, &Controller::requestStreams);

    m_timer->setSingleShot(true);
    m_devices->init();
}

QString Controller::streamName(const Device &device, bool mainStream)
{
    QList <QString> list = {m_prefix, device->id()};

    if (!mainStream)
        list.append("sub");

    return list.join('_');
}

void Controller::sendRequest(const QString &method, const QString &path, const QString &id, const QByteArray &data)
{
    QList <QString> list = {"curl", "-fsS"};
    QProcess *process(new QProcess(this));
    QString command;

    list.append(QString("-m %1").arg(REQUEST_TIMEOUT));
    list.append(QString("-X %1").arg(method));

    if (!data.isEmpty())
    {
        list.append("-H 'Content-Type: application/json'");
        list.append("-d @-");
    }

    list.append(QString("'%1/api/%2'").arg(m_url, path));
    command = list.join(0x20);

    connect(process, static_cast <void (QProcess::*)(int, QProcess::ExitStatus)> (&QProcess::finished), this, &Controller::finished);
    process->setProperty("method", method);
    process->setProperty("id", id);

    logDebug(m_debug) << "API request:" << command.toUtf8().constData();
    process->start("sh", {"-c", command});

    if (data.isEmpty())
        return;

    process->write(data);
    process->closeWriteChannel();
}

void Controller::updateStream(const QString &name, const QString &source)
{
    if (!source.isEmpty())
    {
        logInfo << "Stream" << name << "updated";
        sendRequest("PUT", QString("streams?name=%1&src=%2").arg(name, QString(QUrl::toPercentEncoding(source))));
    }
    else
    {
        logInfo << "Stream" << name << "removed";
        sendRequest("DELETE", QString("streams?src=%1").arg(name));
    }
}

void Controller::syncStreams(const QJsonObject &json)
{
    QMap <QString, QString> items;

    for (int i = 0; i < m_devices->count(); i++)
    {
        const Device &device = m_devices->at(i);
        items.insert(streamName(device, true), device->mainStream());
        items.insert(streamName(device, false), device->subStream());
    }

    for (auto it = items.begin(); it != items.end(); it++)
    {
        if (json.value(it.key()).toObject().value("producers").toArray().first().toObject().value("url").toString() == it.value())
            continue;

        updateStream(it.key(), it.value());
    }

    for (auto it = json.begin(); it != json.end(); it++)
    {
        if (!it.key().startsWith(m_prefix) || items.contains(it.key()))
            continue;

        updateStream(it.key(), QString());
    }
}

void Controller::publishEvent(const QString &name, Event event)
{
    mqttPublish(mqttTopic("event/%1").arg(serviceTopic()), {{"device", name}, {"event", m_events.valueToKey(static_cast <int> (event))}});
}

void Controller::quit(void)
{
    delete m_devices;
    HOMEd::quit();
}

void Controller::mqttConnected(void)
{
    mqttSubscribe(mqttTopic("command/%1").arg(serviceTopic()));

    requestStreams();
    m_devices->store();

    mqttPublishService();
}

void Controller::mqttReceived(const QByteArray &message, const QMqttTopicName &topic)
{
    QString subTopic = topic.name().replace(0, mqttTopic().length(), QString());
    QJsonObject json = QJsonDocument::fromJson(message).object();

    if (subTopic != QString("command/%1").arg(serviceTopic()))
        return;

    switch (static_cast <Command> (m_commands.keyToValue(json.value("action").toString().toUtf8().constData())))
    {
        case Command::restartService:
        {
            logWarning << "Restart request received...";
            mqttPublish(topic.name(), QJsonObject(), true);
            QCoreApplication::exit(EXIT_RESTART);
            break;
        }

        case Command::updateDevice:
        {
            int index = -1;
            QJsonObject data = json.value("data").toObject();
            QString name = mqttSafe(data.value("name").toString());
            Device device = m_devices->byName(json.value("device").toString(), &index), other = m_devices->byName(name);

            if (device != other && !other.isNull())
            {
                logWarning << "Device" << name << "update failed, name already in use";
                publishEvent(name, Event::nameDuplicate);
                break;
            }

            if (!device.isNull())
                data.insert("id", device->id());

            device = m_devices->parse(data);

            if (device.isNull())
            {
                logWarning << "Device" << name << "update failed, data is incomplete";
                publishEvent(name, Event::incompleteData);
                break;
            }

            if (index >= 0)
            {
                m_devices->replace(index, device);
                logInfo << device << "successfully updated";
                publishEvent(device->name(), Event::updated);
            }
            else
            {
                m_devices->append(device);
                logInfo << device << "successfully added";
                publishEvent(device->name(), Event::added);
            }

            requestStreams();
            m_devices->store(true);
            break;
        }

        case Command::removeDevice:
        {
            int index = -1;
            const Device &device = m_devices->byName(json.value("device").toString(), &index);

            if (index >= 0)
            {
                m_devices->removeAt(index);
                logInfo << device << "removed";
                publishEvent(device->name(), Event::removed);
                requestStreams();
                m_devices->store(true);
            }

            break;
        }

        case Command::getStream:
        {
            Device device = m_devices->byName(json.value("device").toString());

            if (!device.isNull())
                sendRequest("POST", QString("webrtc?src=%1").arg(streamName(device, !json.value("subStream").toBool() || device->subStream().isEmpty())), json.value("id").toString(), QJsonDocument({{"sdp", json.value("sdp")}, {"type", "offer"}}).toJson(QJsonDocument::Compact));

            break;
        }
    }
}

void Controller::requestStreams(void)
{
    sendRequest("GET", "streams");
}

void Controller::finished(int exitCode, QProcess::ExitStatus)
{
    QProcess *process = reinterpret_cast <QProcess*> (sender());
    QString id = process->property("id").toString();

    process->deleteLater();

    if (exitCode)
        logWarning << "API request failed, curl exit code:" << exitCode;

    if (!id.isEmpty())
    {
        QJsonObject json = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
        mqttPublish(mqttTopic(serviceTopic()), json.value("type").toString() == "answer" ? QJsonObject {{"id", id}, {"sdp", json.value("sdp")}} : QJsonObject {{"id", id}, {"error", "request failed"}});
        return;
    }

    if (process->property("method").toString() != "GET")
        return;

    if (exitCode)
    {
        m_timer->start(RETRY_INTERVAL);
        return;
    }

    syncStreams(QJsonDocument::fromJson(process->readAllStandardOutput()).object());
    m_timer->stop();
}
