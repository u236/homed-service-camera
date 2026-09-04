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
    QString stream = mainStream ? device->mainStream() : device->subStream();
    QList <QString> list = {m_prefix, device->id()};

    if (!stream.isEmpty() && !stream.contains("://"))
        return stream;

    if (!mainStream)
        list.append("sub");

    return list.join('_');
}

void Controller::updateStream(const QString &name, const QString &stream)
{
    if (!stream.isEmpty())
    {
        logInfo << "Stream" << name << "updated";
        sendRequest("PUT", QString("streams?name=%1&src=%2").arg(name, QString(QUrl::toPercentEncoding(stream))));
    }
    else
    {
        logInfo << "Stream" << name << "removed";
        sendRequest("DELETE", QString("streams?src=%1").arg(name));
    }
}

void Controller::updatePreload(const QString &name, bool enabled)
{
    logInfo << "Stream" << name << "preload" << (enabled ? "enabled" : "disabled");
    sendRequest(enabled ? "PUT" : "DELETE", QString("preload?src=%1").arg(name));
}

void Controller::syncStreams(const QJsonObject &json)
{
    QRegExp expression(QString("^%1_[0-9a-f]{10}(_sub)?$").arg(QRegExp::escape(m_prefix)));
    QMap <QString, QString> map;
    QList <QString> list;

    for (int i = 0; i < m_devices->count(); i++)
    {
        const Device &device = m_devices->at(i);

        for (int j = 1; j >= 0; j--)
        {
            QString stream = j ? device->mainStream() : device->subStream();

            if (stream.isEmpty() || stream.contains("://"))
            {
                map.insert(streamName(device, j), stream);
                continue;
            }

            if (!json.contains(stream))
            {
                logWarning << device << "stream" << stream << "not found";
                publishEvent(device->name(), Event::missingStream);
            }

            if (list.contains(stream))
                continue;

            list.append(stream);
        }
    }

    for (auto it = map.begin(); it != map.end(); it++)
    {
        if (it.value().isEmpty() && !json.contains(it.key()))
            continue;

        updateStream(it.key(), it.value());
    }

    for (auto it = json.begin(); it != json.end(); it++)
    {
        if (!expression.exactMatch(it.key()) || map.contains(it.key()) || list.contains(it.key()))
            continue;

        updateStream(it.key(), QString());
    }

    sendRequest("GET", "preload");
}

void Controller::syncPreload(const QJsonObject &json)
{
    QRegExp expression(QString("^%1_[0-9a-f]{10}$").arg(QRegExp::escape(m_prefix)));
    QList <QString> list;

    for (int i = 0; i < m_devices->count(); i++)
    {
        const Device &device = m_devices->at(i);

        if (!device->mainStream().contains("://") || !device->preload())
            continue;

        list.append(streamName(device, true));
        updatePreload(list.last(), true);
    }

    for (auto it = json.begin(); it != json.end(); it++)
    {
        if (!expression.exactMatch(it.key()) || list.contains(it.key()))
            continue;

        updatePreload(it.key(), false);
    }
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

    list.append(QString("'%1'").arg(QString(path.contains("://") ? path : QString("%1/api/%2").arg(m_url, path)).replace("'", "'\\''")));
    command = list.join(0x20);

    connect(process, static_cast <void (QProcess::*)(int, QProcess::ExitStatus)> (&QProcess::finished), this, &Controller::finished);
    process->setProperty("method", method);
    process->setProperty("path", path);
    process->setProperty("id", id);

    logDebug(m_debug) << "API request:" << command.toUtf8().constData();
    process->start("sh", {"-c", command});

    if (data.isEmpty())
        return;

    process->write(data);
    process->closeWriteChannel();
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

        case Command::getFrame:
        {
            Device device = m_devices->byName(json.value("device").toString());

            if (!device.isNull())
                sendRequest("GET", device->frame().isEmpty() ? QString("frame.jpeg?src=%1").arg(QString(QUrl::toPercentEncoding(streamName(device, !json.value("subStream").toBool() || device->subStream().isEmpty())))) : device->frame(), json.value("id").toString());

            break;
        }

        case Command::getStream:
        {
            Device device = m_devices->byName(json.value("device").toString());

            if (!device.isNull())
                sendRequest("POST", QString("webrtc?src=%1").arg(QString(QUrl::toPercentEncoding(streamName(device, !json.value("subStream").toBool() || device->subStream().isEmpty())))), json.value("id").toString(), QJsonDocument({{"sdp", json.value("sdp")}, {"type", "offer"}}).toJson(QJsonDocument::Compact));

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
    QByteArray response = process->readAllStandardOutput();
    QJsonObject json = QJsonDocument::fromJson(response).object();
    QString method = process->property("method").toString(), id = process->property("id").toString();
    bool frame = method == "GET" && !id.isEmpty();

    logDebug(m_debug) << "API response:" << (frame ? QString("frame data (%1 bytes)").arg(response.length()).toUtf8() : response).constData();
    process->deleteLater();

    if (exitCode)
        logWarning << "API request failed, curl exit code:" << exitCode;

    if (frame)
    {
        mqttPublish(mqttTopic(serviceTopic()), !exitCode ? QJsonObject {{"id", id}, {"data", QString(response.toBase64())}} : QJsonObject {{"id", id}, {"error", "request failed"}});
        return;
    }

    if (!id.isEmpty())
    {
        mqttPublish(mqttTopic(serviceTopic()), json.value("type").toString() == "answer" ? QJsonObject {{"id", id}, {"sdp", json.value("sdp")}} : QJsonObject {{"id", id}, {"error", "request failed"}});
        return;
    }

    if (method != "GET")
        return;

    if (exitCode)
    {
        m_timer->start(RETRY_INTERVAL);
        return;
    }

    if (process->property("path").toString() == "preload")
    {
        syncPreload(json);
        return;
    }

    syncStreams(json);
    m_timer->stop();
}
