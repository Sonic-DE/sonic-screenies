/*
 *  SPDX-FileCopyrightText: 2019 David Redondo <kde@david-redondo.de>
 *  SPDX-FileCopyrightText: 2015 Boudhayan Gupta <bgupta@kde.org>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "CommandLineOptions.h"
#include "Config.h"
#include "PlatformStartup.h"
#include "ShortcutActions.h"
#include "SpectacleCore.h"
#include "SpectacleDBusAdapter.h"
#include "settings.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDir>
#include <QIcon>
#include <QSessionManager>
#include <QtGlobal>

#include <KAboutData>
#include <KCrash>
#include <KDBusService>
#include <KLocalizedString>
#include <KMessageBox>

using namespace Qt::StringLiterals;

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QIcon::setFallbackThemeName(u"breeze"_s);
    QApplication app(argc, argv);

    KLocalizedString::setApplicationDomain(QByteArrayLiteral("spectacle"));
    QCoreApplication::setOrganizationDomain(u"org.kde"_s);

    KAboutData aboutData(u"spectacle"_s,
                         i18n("Spectacle"),
                         QStringLiteral(SPECTACLE_VERSION),
                         i18n("KDE Screenshot Utility"),
                         KAboutLicense::GPL_V2,
                         i18n("© 2015 Boudhayan Gupta"));
    aboutData.addAuthor(u"Boudhayan Gupta"_s, {}, u"bgupta@kde.org"_s);
    aboutData.addAuthor(u"David Redondo"_s, {}, u"kde@david-redondo.de"_s);
    aboutData.addAuthor(u"Noah Davis"_s, {}, u"noahadvs@gmail.com"_s);
    aboutData.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));
    KAboutData::setApplicationData(aboutData);
    app.setWindowIcon(QIcon::fromTheme(u"spectacle"_s));

    KCrash::initialize();

    QCommandLineParser commandLineParser;
    aboutData.setupCommandLine(&commandLineParser);
    commandLineParser.addOptions(CommandLineOptions::self()->allOptions);

    commandLineParser.process(app.arguments());
    aboutData.processCommandLine(&commandLineParser);

    if (!PlatformStartup::isSupportedX11Session(app.platformName(), qEnvironmentVariable("XDG_SESSION_TYPE"))) {
        const auto message = i18n("Spectacle now requires an X11 session. Use Spectacle under an X11 desktop, or run with QT_QPA_PLATFORM=xcb in an X11 session.");
        qWarning().noquote() << message;
        if (commandLineParser.isSet(CommandLineOptions::self()->background)
            || commandLineParser.isSet(CommandLineOptions::self()->dbus)) {
            return 1;
        }
        KMessageBox::error(nullptr, message);
        return 1;
    }

    auto disableSessionManagement = [](QSessionManager &sm) {
        sm.setRestartHint(QSessionManager::RestartNever);
    };
    QObject::connect(&app, &QGuiApplication::commitDataRequest, disableSessionManagement);
    QObject::connect(&app, &QGuiApplication::saveStateRequest, disableSessionManagement);

    if (commandLineParser.isSet(CommandLineOptions::self()->newInstance)) {
        auto spectacleCore = SpectacleCore::instance();

        QObject::connect(qApp, &QApplication::aboutToQuit, Settings::self(), &Settings::save);
        QObject::connect(spectacleCore, &SpectacleCore::allDone, &app, &QCoreApplication::quit, Qt::QueuedConnection);

        spectacleCore->activate(app.arguments(), QDir::currentPath());

        return app.exec();
    }

    KDBusService service(KDBusService::Unique);

    auto spectacleCore = SpectacleCore::instance();

    QObject::connect(&service, &KDBusService::activateRequested, spectacleCore, &SpectacleCore::activate);
    QObject::connect(qApp, &QApplication::aboutToQuit, Settings::self(), &Settings::save);

    if (auto dbusAdapter = new SpectacleDBusAdapter(spectacleCore); !QDBusConnection::sessionBus().registerObject(u"/org/kde/Spectacle"_s, dbusAdapter)) {
        qWarning("Failed to register the DBus interface");
    }

    return app.exec();
}
