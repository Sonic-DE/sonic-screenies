/* SPDX-FileCopyrightText: 2022 Noah Davis <noahadvs@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "HelpMenu.h"
#include "SpectacleCore.h"
#include "WidgetWindowUtils.h"

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KLocalizedString>

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QMessageBox>
#include <QUrl>
#include <QWindow>

#include <cstring>

using namespace Qt::StringLiterals;

static HelpMenu *s_instance = nullptr;

static QObject *findWidgetOfType(const char *className)
{
    if (strlen(className) == 0) {
        return nullptr;
    }
    const auto widgets = qApp->allWidgets();
    for (const auto w : widgets) {
        if (w->inherits(className)) {
            return w;
        }
    }
    return nullptr;
}

HelpMenu::HelpMenu(QWidget *parent)
    : SpectacleMenu(parent)
    , kHelpMenu(new KHelpMenu(parent, KAboutData::applicationData()))
{
    setTitle(i18nc("@title:menu", "Help"));
    setIcon(QIcon::fromTheme(u"help-contents"_s));
    for (auto action : kHelpMenu->menu()->actions()) {
        if (action == kHelpMenu->action(KHelpMenu::menuAboutKDE)) {
            auto aboutAction = addAction(i18nc("@action:inmenu", "About SonicDE"));
            connect(aboutAction, &QAction::triggered, this, &HelpMenu::showAboutSonicDE);
        } else if (action == kHelpMenu->action(KHelpMenu::menuAboutApp)) {
            auto aboutAction = addAction(action->icon(), action->text());
            connect(aboutAction, &QAction::triggered, this, &HelpMenu::showAboutApplication);
        } else if (action == kHelpMenu->action(KHelpMenu::menuReportBug)) {
            auto reportAction = addAction(action->icon(), action->text());
            connect(reportAction, &QAction::triggered, this, [] {
                QDesktopServices::openUrl(QUrl(KAboutData::applicationData().bugAddress()));
            });
        } else if (action != kHelpMenu->action(KHelpMenu::menuDonate)) {
            addAction(action);
        }
    }
    connect(this, &QMenu::triggered, this, &HelpMenu::onTriggered);
}

HelpMenu::~HelpMenu()
{
    s_instance = nullptr;
}

HelpMenu *HelpMenu::instance()
{
    if (!s_instance && SpectacleCore::instance()) {
        s_instance = new HelpMenu;
        // We have to destroy this after SpectacleCore to ensure that destructors
        // are called. We don't just rely on smart pointers because they won't delete
        // the menus at the right time and cause a crash while quitting.
        connect(SpectacleCore::instance(), &QObject::destroyed, qApp, [] {
            if (s_instance) {
                delete s_instance;
            }
        });
    }
    return s_instance;
}

void HelpMenu::showAppHelp()
{
    kHelpMenu->appHelpActivated();
}

void HelpMenu::showAboutApplication()
{
    if (!m_aboutApplicationDialog) {
        // The framework's automatic Components tab inserts KDE project
        // branding. Keep the application, author, license and translator pages
        // without relabeling third-party libraries or their legal attribution.
        auto dialog = new KAboutApplicationDialog(KAboutData::applicationData(), KAboutApplicationDialog::HideLibraries, this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        m_aboutApplicationDialog = dialog;
    }
    setWidgetTransientParent(m_aboutApplicationDialog, getWidgetTransientParent(this));
    m_aboutApplicationDialog->show();
    m_aboutApplicationDialog->windowHandle()->requestActivate();
}

void HelpMenu::showAboutSonicDE()
{
    if (!m_aboutSonicDEDialog) {
        auto dialog = new QMessageBox(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(i18nc("@title:window", "About SonicDE"));
        dialog->setTextFormat(Qt::RichText);
        dialog->setTextInteractionFlags(Qt::TextBrowserInteraction);
        dialog->setText(i18n("<h2>SonicDE</h2>"
                             "<p>A free and open-source desktop environment.</p>"
                             "<p><a href=\"https://github.com/Sonic-DE\">SonicDE on GitHub</a></p>"));
        dialog->setStandardButtons(QMessageBox::Close);
        m_aboutSonicDEDialog = dialog;
    }
    setWidgetTransientParent(m_aboutSonicDEDialog, getWidgetTransientParent(this));
    m_aboutSonicDEDialog->show();
    m_aboutSonicDEDialog->windowHandle()->requestActivate();
}

void HelpMenu::onTriggered(QAction *action)
{
    auto transientParent = getWidgetTransientParent(this);
    if (!transientParent || !transientParent->isVisible() || action == kHelpMenu->action(KHelpMenu::menuWhatsThis)) {
        return;
    }

    QDialog *dialog = nullptr;
    // KHelpMenu creates these dialogs and sets the parent of KHelpMenu as the parent of the dialogs.
    // KHelpMenu doesn't expose pointers to the dialogs,
    // so we have to search for them in the parent.
    // The language dialog is a private framework type.
    if (action == kHelpMenu->action(KHelpMenu::menuSwitchLanguage)) {
        dialog = qobject_cast<QDialog *>(findWidgetOfType("KDEPrivate::KSwitchLanguageDialog"));
    }

    if (dialog) {
        setWidgetTransientParent(dialog, transientParent);
        dialog->windowHandle()->requestActivate();
    }
}

#include "moc_HelpMenu.cpp"
