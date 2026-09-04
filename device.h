#ifndef DEVICE_H
#define DEVICE_H

#define STORE_DATABASE_DELAY        20

#include <QFile>
#include <QJsonArray>
#include <QSettings>
#include <QTimer>

class DeviceObject;
typedef QSharedPointer <DeviceObject> Device;

class DeviceObject
{

public:

    DeviceObject(const QString &id, const QString &name, const QString &mainStream, const QString &subStream, const QString &frame, bool preload) :
        m_id(id), m_name(name), m_mainStream(mainStream), m_subStream(subStream), m_frame(frame), m_preload(preload) {}

    inline QString id(void) { return m_id; }
    inline QString name(void) { return m_name; }
    inline QString mainStream(void) { return m_mainStream; }
    inline QString subStream(void) { return m_subStream; }
    inline QString frame(void) { return m_frame; }
    inline bool preload(void) { return m_preload; }

private:

    QString m_id, m_name, m_mainStream, m_subStream, m_frame;
    bool m_preload;

};

class DeviceList : public QObject, public QList <Device>
{
    Q_OBJECT

public:

    DeviceList(QSettings *config, QObject *parent);
    ~DeviceList(void);

    void init(void);
    void store(bool sync = false);

    Device byName(const QString &name, int *index = nullptr);
    Device parse(const QJsonObject &json);

private:

    QTimer *m_timer;
    QFile m_file;
    bool m_sync;

    QByteArray randomData(int length);

    void unserialize(const QJsonArray &devices);
    QJsonArray serialize(void);

private slots:

    void writeDatabase(void);

};

inline QDebug operator << (QDebug debug, const Device &device) { return debug << "device" << device->name(); }

#endif
