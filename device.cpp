#include <QRandomGenerator>
#include "controller.h"
#include "device.h"
#include "logger.h"

DeviceList::DeviceList(QSettings *config, QObject *parent) : QObject(parent), m_timer(new QTimer(this)), m_sync(false)
{
    m_file.setFileName(config->value("device/database", "/opt/homed-camera/database.json").toString());

    connect(m_timer, &QTimer::timeout, this, &DeviceList::writeDatabase);
    m_timer->setSingleShot(true);
}

DeviceList::~DeviceList(void)
{
    m_sync = true;
    writeDatabase();
}

void DeviceList::init(void)
{
    QJsonObject json;

    if (!m_file.open(QFile::ReadOnly))
        return;

    json = QJsonDocument::fromJson(m_file.readAll()).object();
    unserialize(json.value("devices").toArray());
    m_file.close();
}

void DeviceList::store(bool sync)
{
    if (sync)
        m_sync = true;

    m_timer->start(STORE_DATABASE_DELAY);
}

Device DeviceList::byName(const QString &name, int *index)
{
    for (int i = 0; i < count(); i++)
    {
        if (at(i)->id() != name && at(i)->name() != name)
            continue;

        if (index)
            *index = i;

        return at(i);
    }

    return Device();
}

Device DeviceList::parse(const QJsonObject &json)
{
    QString id = mqttSafe(json.value("id").toString()), name = mqttSafe(json.value("name").toString()), mainStream = json.value("mainStream").toString().trimmed(), subStream = json.value("subStream").toString().trimmed();
    Device device;

    if (!name.isEmpty() && !mainStream.isEmpty())
        device = Device(new DeviceObject(id.isEmpty() ? randomData(5).toHex() : id, name, mainStream, subStream));

    return device;
}

QByteArray DeviceList::randomData(int length)
{
    QByteArray data;

    for (int i = 0; i < length; i++)
        data.append(static_cast <char> (QRandomGenerator::global()->generate()));

    return data;
}

void DeviceList::unserialize(const QJsonArray &devices)
{
    for (auto it = devices.begin(); it != devices.end(); it++)
    {
        QJsonObject json = it->toObject();
        Device device;

        if (!byName(json.value("id").toString()).isNull() || !byName(json.value("name").toString()).isNull())
            continue;

        device = parse(json);

        if (device.isNull())
            continue;

        append(device);
    }

    if (!count())
        return;

    logInfo << count() << "devices loaded";
}

QJsonArray DeviceList::serialize(void)
{
    QJsonArray array;

    for (int i = 0; i < count(); i++)
    {
        const Device &device = at(i);
        QJsonObject json = {{"id", device->id()}, {"name", device->name()}, {"mainStream", device->mainStream()}};

        if (!device->subStream().isEmpty())
            json.insert("subStream", device->subStream());

        array.append(json);
    }

    return array;
}

void DeviceList::writeDatabase(void)
{
    HOMEd *homed = reinterpret_cast <HOMEd*> (parent());
    QJsonObject json = {{"devices", serialize()}, {"timestamp", QDateTime::currentSecsSinceEpoch()}, {"version", SERVICE_VERSION}};

    homed->mqttPublishStatus(json);

    if (!m_sync)
        return;

    m_sync = false;

    if (homed->writeFile(m_file, QJsonDocument(json).toJson(QJsonDocument::Compact)))
        return;

    logWarning << "Database not stored";
}
