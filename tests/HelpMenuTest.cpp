/* SPDX-FileCopyrightText: 2026 SonicDE contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "Gui/HelpMenu.h"
#include "SpectacleCore.h"

#include <KAboutData>
#include <KLocalizedString>
#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QMessageBox>
#include <QTabWidget>
#include <QUrl>
#include <QtTest>

using namespace Qt::StringLiterals;

// Isolate the real menu implementation from the screenshot engine. These tests
// construct the menu directly, not through its application singleton factory.
SpectacleCore* SpectacleCore::instance()
{
    return nullptr;
}

class HelpMenuTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void menuUsesSonicDEBranding();
    void aboutDialogUsesSonicDEAndCanReopen();
    void applicationDialogPreservesAuthorPage();
    void reportBugOpensScreeniesTracker();

public Q_SLOTS:
    void recordUrl(const QUrl& url)
    {
        m_lastUrl = url;
    }

private:
    QUrl m_lastUrl;
};

void HelpMenuTest::initTestCase()
{
    qApp->setQuitOnLastWindowClosed(false);
    KLocalizedString::setApplicationDomain("spectacle");
    KAboutData about(u"screenies"_s, u"Screenies"_s, u"1.0"_s);
    about.setBugAddress(QByteArrayLiteral("https://github.com/Sonic-DE/sonic-screenies/issues"));
    about.addAuthor(u"Screenies Contributor"_s);
    KAboutData::setApplicationData(about);
}

void HelpMenuTest::cleanup()
{
    QDesktopServices::unsetUrlHandler(u"https"_s);
    m_lastUrl.clear();
}

void HelpMenuTest::menuUsesSonicDEBranding()
{
    HelpMenu menu;
    int sonicDEActions = 0;
    for (auto* action : menu.actions()) {
        QVERIFY(!action->text().contains(u"KDE"_s));
        QVERIFY(!action->text().contains(u"Plasma"_s));
        sonicDEActions += action->text() == u"About SonicDE"_s;
        QVERIFY(action != menu.kHelpMenu->action(KHelpMenu::menuAboutKDE));
        QVERIFY(action != menu.kHelpMenu->action(KHelpMenu::menuAboutApp));
        QVERIFY(action != menu.kHelpMenu->action(KHelpMenu::menuDonate));
    }
    QCOMPARE(sonicDEActions, 1);
}

void HelpMenuTest::aboutDialogUsesSonicDEAndCanReopen()
{
    HelpMenu menu;
    menu.showAboutSonicDE();
    QTRY_VERIFY(menu.m_aboutSonicDEDialog && menu.m_aboutSonicDEDialog->isVisible());
    auto* dialog = qobject_cast<QMessageBox*>(menu.m_aboutSonicDEDialog.data());
    QVERIFY(dialog);
    QCOMPARE(dialog->windowTitle(), u"About SonicDE"_s);
    QVERIFY(dialog->text().contains(u"SonicDE"_s));
    QVERIFY(!dialog->text().contains(u"KDE"_s));
    QVERIFY(!dialog->text().contains(u"Plasma"_s));
    menu.showAboutSonicDE();
    QCOMPARE(menu.m_aboutSonicDEDialog.data(), dialog);
    dialog->close();
    QTRY_VERIFY(menu.m_aboutSonicDEDialog.isNull());
    menu.showAboutSonicDE();
    QTRY_VERIFY(menu.m_aboutSonicDEDialog && menu.m_aboutSonicDEDialog->isVisible());
    menu.m_aboutSonicDEDialog->close();
    QTRY_VERIFY(menu.m_aboutSonicDEDialog.isNull());
}

void HelpMenuTest::applicationDialogPreservesAuthorPage()
{
    HelpMenu menu;
    menu.showAboutApplication();
    QTRY_VERIFY(menu.m_aboutApplicationDialog && menu.m_aboutApplicationDialog->isVisible());
    auto* dialog = menu.m_aboutApplicationDialog.data();
    QVERIFY(dialog->windowTitle().contains(u"Screenies"_s));
    auto* tabs = dialog->findChild<QTabWidget*>();
    QVERIFY(tabs);
    bool hasAuthors = false;
    for (int i = 0; i < tabs->count(); ++i) {
        hasAuthors |= tabs->tabText(i).contains(u"Author"_s);
        QVERIFY(!tabs->tabText(i).contains(u"Components"_s));
    }
    QVERIFY(hasAuthors);
    for (auto* label : dialog->findChildren<QLabel*>()) {
        QVERIFY(!label->text().contains(u"KDE"_s));
        QVERIFY(!label->text().contains(u"Plasma"_s));
    }
    dialog->close();
    QTRY_VERIFY(menu.m_aboutApplicationDialog.isNull());
}

void HelpMenuTest::reportBugOpensScreeniesTracker()
{
    HelpMenu menu;
    QDesktopServices::setUrlHandler(u"https"_s, this, "recordUrl");
    const auto reportText = menu.kHelpMenu->action(KHelpMenu::menuReportBug)->text();
    for (auto* action : menu.actions()) {
        if (action->text() == reportText) {
            action->trigger();
            QTRY_COMPARE(m_lastUrl, QUrl(u"https://github.com/Sonic-DE/sonic-screenies/issues"_s));
            return;
        }
    }
    QFAIL("Report Bug action is missing");
}

QTEST_MAIN(HelpMenuTest)
#include "HelpMenuTest.moc"
