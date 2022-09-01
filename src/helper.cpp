#include "helper.h"

#include <QCoreApplication>
#include <QtDBus>
#include <QFile>
#include <PolkitQt1/Authority>
#include <PolkitQt1/Subject>

#include <signal.h>

HelperAdaptor::HelperAdaptor(Helper *parent) :
    QDBusAbstractAdaptor(parent)
{
    m_parentHelper = parent;
}

QVariantMap HelperAdaptor::listStorages()
{
    return m_parentHelper->listStorages();
}

void HelperAdaptor::prepareFile(const QString &benchmarkFile, int fileSize, bool fillZeros)
{
    return m_parentHelper->prepareFile(benchmarkFile, fileSize, fillZeros);
}

void HelperAdaptor::startTest(const QString &benchmarkFile, int measuringTime, int fileSize, int randomReadPercentage, bool fillZeros,
                              int blockSize, int queueDepth, int threads, const QString &rw)
{
    m_parentHelper->startTest(benchmarkFile, measuringTime, fileSize, randomReadPercentage, fillZeros, blockSize, queueDepth, threads, rw);
}

QVariantMap HelperAdaptor::flushPageCache()
{
    return m_parentHelper->flushPageCache();
}

bool HelperAdaptor::removeFile(const QString &benchmarkFile)
{
    return m_parentHelper->removeFile(benchmarkFile);
}

void HelperAdaptor::stopCurrentTask()
{
    m_parentHelper->stopCurrentTask();
}

Helper::Helper() : m_helperAdaptor(new HelperAdaptor(this))
{
    if (!QDBusConnection::systemBus().isConnected() || !QDBusConnection::systemBus().registerService(QStringLiteral("dev.jonmagon.kdiskmark.helperinterface")) ||
        !QDBusConnection::systemBus().registerObject(QStringLiteral("/Helper"), this)) {
        qWarning() << QDBusConnection::systemBus().lastError().message();
        qApp->quit();
    }

    m_serviceWatcher = new QDBusServiceWatcher(this);
    m_serviceWatcher->setConnection(QDBusConnection ::systemBus());
    m_serviceWatcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);

    connect(m_serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, qApp, [this](const QString &service) {
        m_serviceWatcher->removeWatchedService(service);
        if (m_serviceWatcher->watchedServices().isEmpty()) {
            qApp->quit();
        }
    });

    QObject::connect(this, &Helper::taskFinished, m_helperAdaptor, &HelperAdaptor::taskFinished);
}

void Helper::testFilePath(const QString &benchmarkFile)
{
    if (!benchmarkFile.endsWith("/.kdiskmark.tmp"))
        qFatal("The path must end with /.kdiskmark.tmp");
}

QVariantMap Helper::listStorages()
{
    if (!isCallerAuthorized()) {
        return QVariantMap();
    }

    QVariantMap reply;
    foreach (const QStorageInfo &storage, QStorageInfo::mountedVolumes()) {
        if (storage.isValid() && storage.isReady() && !storage.isReadOnly()) {
            if (storage.device().indexOf("/dev") != -1) {
                reply[storage.rootPath()] =
                        QVariant::fromValue(QDBusVariant(QVariant::fromValue(QVector<qlonglong> { storage.bytesTotal(), storage.bytesAvailable() })));
            }
        }
    }

    return reply;
}

void Helper::prepareFile(const QString &benchmarkFile, int fileSize, bool fillZeros)
{
    if (!isCallerAuthorized()) {
        return;
    }

    testFilePath(benchmarkFile);

    m_process = new QProcess();
    m_process->start("fio", QStringList()
                     << "--output-format=json"
                     << "--create_only=1"
                     << QStringLiteral("--filename=%1").arg(benchmarkFile)
                     << QStringLiteral("--size=%1m").arg(fileSize)
                     << QStringLiteral("--zero_buffers=%1").arg(fillZeros)
                     << QStringLiteral("--name=prepare"));

    connect(m_process, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            [=] (int exitCode, QProcess::ExitStatus exitStatus) {
        emit taskFinished(exitStatus == QProcess::NormalExit, QString(m_process->readAllStandardOutput()), QString(m_process->readAllStandardError()));
    });
}

void Helper::startTest(const QString &benchmarkFile, int measuringTime, int fileSize, int randomReadPercentage, bool fillZeros,
                       int blockSize, int queueDepth, int threads, const QString &rw)
{
    if (!isCallerAuthorized()) {
        return;
    }

    testFilePath(benchmarkFile);

    m_process = new QProcess();
    m_process->start("fio", QStringList()
                     << "--output-format=json"
                     << "--ioengine=libaio"
                     << "--direct=1"
                     << "--randrepeat=0"
                     << "--refill_buffers"
                     << "--end_fsync=1"
                     << QStringLiteral("--rwmixread=%1").arg(randomReadPercentage)
                     << QStringLiteral("--filename=%1").arg(benchmarkFile)
                     << QStringLiteral("--name=%1").arg(rw)
                     << QStringLiteral("--size=%1m").arg(fileSize)
                     << QStringLiteral("--zero_buffers=%1").arg(fillZeros)
                     << QStringLiteral("--bs=%1k").arg(blockSize)
                     << QStringLiteral("--runtime=%1").arg(measuringTime)
                     << QStringLiteral("--rw=%1").arg(rw)
                     << QStringLiteral("--iodepth=%1").arg(queueDepth)
                     << QStringLiteral("--numjobs=%1").arg(threads));

    connect(m_process, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            [=] (int exitCode, QProcess::ExitStatus exitStatus) {
        emit taskFinished(exitStatus == QProcess::NormalExit, QString(m_process->readAllStandardOutput()), QString(m_process->readAllStandardError()));
    });
}

QVariantMap Helper::flushPageCache()
{
    QVariantMap reply;
    reply[QStringLiteral("success")] = true;

    if (!isCallerAuthorized()) {
        reply[QStringLiteral("success")] = false;
        return reply;
    }

    QFile file("/proc/sys/vm/drop_caches");

    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write("1");
        file.close();
    }
    else {
        reply[QStringLiteral("success")] = false;
        reply[QStringLiteral("error")] = file.errorString();
    }

    return reply;
}

bool Helper::removeFile(const QString &benchmarkFile)
{
    if (!isCallerAuthorized()) {
        return false;
    }

    return QFile(benchmarkFile).remove();
}

void Helper::stopCurrentTask()
{
    if (!isCallerAuthorized()) {
        return;
    }

    if (!m_process) return;

    if (m_process->state() == QProcess::Running || m_process->state() == QProcess::Starting) {
        m_process->terminate();
        m_process->waitForFinished(-1);
    }

    delete m_process;
}

bool Helper::isCallerAuthorized()
{
    if (!calledFromDBus()) {
        return false;
    }

    if (m_serviceWatcher->watchedServices().contains(message().service())) {
        return true;
    }

    if (!m_serviceWatcher->watchedServices().isEmpty()) {
        qDebug() << "There are already registered DBus connections.";
        return false;
    }

    PolkitQt1::SystemBusNameSubject subject(message().service());
    PolkitQt1::Authority *authority = PolkitQt1::Authority::instance();

    PolkitQt1::Authority::Result result;
    QEventLoop e;
    connect(authority, &PolkitQt1::Authority::checkAuthorizationFinished, &e, [&e, &result](PolkitQt1::Authority::Result _result) {
        result = _result;
        e.quit();
    });

    authority->checkAuthorization(QStringLiteral("dev.jonmagon.kdiskmark.helper.init"), subject, PolkitQt1::Authority::AllowUserInteraction);
    e.exec();

    if (authority->hasError()) {
        qDebug() << "Encountered error while checking authorization, error code: " << authority->lastError() << authority->errorDetails();
        authority->clearError();
    }

    switch (result) {
    case PolkitQt1::Authority::Yes:
        // track who called into us so we can close when all callers have gone away
        m_serviceWatcher->addWatchedService(message().service());
        return true;
    default:
        sendErrorReply(QDBusError::AccessDenied);
        if (m_serviceWatcher->watchedServices().isEmpty())
            qApp->quit();
        return false;
    }
}

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);
    Helper helper;
    a.exec();
}