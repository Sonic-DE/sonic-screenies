/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest>

using namespace Qt::StringLiterals;

namespace {
const QString canonicalName = u"org.sonicde.screenies"_s;
const QStringList legacyNames = {u"org.kde.spectacle"_s, u"org.kde.Spectacle"_s};

QString entry(const QString& path, const QByteArray& group, const QByteArray& key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    bool inGroup = false;
    const QByteArray heading = '[' + group + ']';
    const QByteArray prefix = key + '=';
    while (!file.atEnd()) {
        const auto line = file.readLine().trimmed();
        if (line.startsWith('[')) {
            inGroup = line == heading;
        } else if (inGroup && line.startsWith(prefix)) {
            return QString::fromUtf8(line.mid(key.size() + 1));
        }
    }
    return {};
}
} // namespace

class DBusActivationTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void activationMetadataMatchesCanonicalName();
    void ownsBothNamesAndForwardsSecondLaunch();
    void onDemandActivation_data();
    void onDemandActivation();

private:
    QTemporaryDir state;
    QProcess app;
    bool activatedByBus = false;
};

void DBusActivationTest::initTestCase()
{
    if (!qEnvironmentVariableIsSet("SCREENIES_TEST_PRIVATE_BUS")) {
        QSKIP("Run through CTest so a private session bus is created.");
    }
    QVERIFY(QDBusConnection::sessionBus().isConnected());
}

void DBusActivationTest::cleanup()
{
    if (app.state() != QProcess::NotRunning) {
        app.terminate();
        if (!app.waitForFinished(5000)) {
            app.kill();
            QVERIFY(app.waitForFinished(5000));
        }
    }
    if (activatedByBus) {
        const auto quit = QDBusMessage::createMethodCall(canonicalName, u"/MainApplication"_s,
            u"org.qtproject.Qt.QCoreApplication"_s, u"quit"_s);
        QVERIFY(QDBusConnection::sessionBus().send(quit));
        activatedByBus = false;
    }
    auto* bus = QDBusConnection::sessionBus().interface();
    for (const auto& name : QStringList{canonicalName} + legacyNames) {
        QTRY_VERIFY_WITH_TIMEOUT(!bus->isServiceRegistered(name).value(), 5000);
    }
}

void DBusActivationTest::activationMetadataMatchesCanonicalName()
{
    const auto dbusDirectory = QString::fromUtf8(TEST_DBUS_DIRECTORY);
    const QString unit = dbusDirectory + u"/app-org.sonicde.screenies.service"_s;
    QCOMPARE(entry(unit, "Service", "BusName"), canonicalName);
    QCOMPARE(entry(unit, "Service", "Type"), u"dbus"_s);
    QCOMPARE(entry(QString::fromUtf8(TEST_DESKTOP_FILE), "Desktop Entry", "X-DBUS-ServiceName"), canonicalName);
    for (const auto& name : QStringList{canonicalName} + legacyNames) {
        const QString path = dbusDirectory + u'/' + name + u".service"_s;
        QCOMPARE(entry(path, "D-BUS Service", "Name"), name);
        QCOMPARE(entry(path, "D-BUS Service", "SystemdService"), u"app-org.sonicde.screenies.service"_s);
        QCOMPARE(entry(path, "D-BUS Service", "Exec"), entry(unit, "Service", "ExecStart"));
    }
}

void DBusActivationTest::ownsBothNamesAndForwardsSecondLaunch()
{
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        QSKIP("Requires an X11 display; install Xvfb for a fully isolated run.");
    }
    auto* bus = QDBusConnection::sessionBus().interface();
    QVERIFY(bus);
    QVERIFY(!bus->isServiceRegistered(canonicalName).value());
    for (const auto& name : legacyNames) {
        QVERIFY(!bus->isServiceRegistered(name).value());
    }

    QVERIFY(state.isValid());
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto& name : {u"CONFIG"_s, u"CACHE"_s, u"DATA"_s}) {
        const QString directory = state.path() + u'/' + name;
        QVERIFY(QDir().mkpath(directory));
        environment.insert(u"XDG_"_s + name + u"_HOME"_s, directory);
    }
    environment.insert(u"QT_QPA_PLATFORM"_s, u"xcb"_s);
    environment.insert(u"XDG_SESSION_TYPE"_s, u"x11"_s);
    environment.insert(u"QT_QUICK_BACKEND"_s, u"software"_s);
    app.setProcessEnvironment(environment);
    app.setProgram(QString::fromUtf8(TEST_APP_BINARY));
    app.setArguments({u"--dbus"_s});
    app.setProcessChannelMode(QProcess::MergedChannels);
    app.start();
    QVERIFY(app.waitForStarted());
    QTRY_VERIFY_WITH_TIMEOUT(bus->isServiceRegistered(canonicalName).value(), 10000);
    for (const auto& name : legacyNames) {
        QTRY_VERIFY_WITH_TIMEOUT(bus->isServiceRegistered(name).value(), 10000);
    }
    const auto pid = quint32(app.processId());
    for (const auto& name : QStringList{canonicalName} + legacyNames) {
        QCOMPARE(bus->servicePid(name).value(), pid);
        for (const auto& path : {u"/org/sonicde/Screenies"_s, u"/org/kde/Spectacle"_s}) {
            const auto call = QDBusMessage::createMethodCall(name, path,
                u"org.freedesktop.DBus.Introspectable"_s, u"Introspect"_s);
            const QDBusReply<QString> reply = QDBusConnection::sessionBus().call(call);
            QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
            QVERIFY(reply.value().contains(u"org.sonicde.Screenies"_s));
            QVERIFY(reply.value().contains(u"org.kde.Spectacle"_s));
            QCOMPARE(reply.value().count(u"method name=\"CurrentScreen\""_s), 2);
            QCOMPARE(reply.value().count(u"method name=\"RecordWindow\""_s), 2);
        }
    }
    // The freedesktop activation path follows the new name; old desktop IDs
    // still reach the same KDBusService adaptor through the compatibility path.
    for (const auto& path : {u"/org/sonicde/screenies"_s, u"/org/kde/spectacle"_s}) {
        const auto call = QDBusMessage::createMethodCall(canonicalName, path,
            u"org.freedesktop.DBus.Introspectable"_s, u"Introspect"_s);
        const QDBusReply<QString> reply = QDBusConnection::sessionBus().call(call);
        QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
        QVERIFY(reply.value().contains(u"org.freedesktop.Application"_s));
        QVERIFY(reply.value().contains(u"ActivateAction"_s));
    }

    QProcess second;
    second.setProcessEnvironment(environment);
    second.start(QString::fromUtf8(TEST_APP_BINARY), {u"--dbus"_s});
    QVERIFY(second.waitForStarted());
    QVERIFY(second.waitForFinished(10000));
    QCOMPARE(second.exitStatus(), QProcess::NormalExit);
    QCOMPARE(second.exitCode(), 0);
    QCOMPARE(app.state(), QProcess::Running);
    QCOMPARE(bus->servicePid(canonicalName).value(), pid);
    for (const auto& name : legacyNames) {
        QCOMPARE(bus->servicePid(name).value(), pid);
    }
}

void DBusActivationTest::onDemandActivation_data()
{
    QTest::addColumn<QString>("name");
    for (const auto& name : QStringList{canonicalName} + legacyNames) {
        QTest::newRow(qPrintable(name)) << name;
    }
}

void DBusActivationTest::onDemandActivation()
{
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        QSKIP("Requires an X11 display; install Xvfb for a fully isolated run.");
    }
    QFETCH(QString, name);
    auto* bus = QDBusConnection::sessionBus().interface();
    QVERIFY(bus);
    QVERIFY(!bus->isServiceRegistered(canonicalName).value());
    const auto reply = bus->startService(name);
    QVERIFY2(reply.isValid(), qPrintable(reply.error().message()));
    activatedByBus = true;
    QTRY_VERIFY_WITH_TIMEOUT(bus->isServiceRegistered(canonicalName).value(), 10000);
    const auto pid = bus->servicePid(canonicalName);
    QVERIFY(pid.isValid());
    for (const auto& alias : QStringList{canonicalName} + legacyNames) {
        QTRY_VERIFY_WITH_TIMEOUT(bus->isServiceRegistered(alias).value(), 10000);
        QCOMPARE(bus->servicePid(alias).value(), pid.value());
    }
}

QTEST_GUILESS_MAIN(DBusActivationTest)
#include "DBusActivationTest.moc"
