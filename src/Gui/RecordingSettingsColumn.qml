/*
 * SPDX-FileCopyrightText: 2023 Aleix Pol i Gonzalez <aleixpol@kde.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC
import org.kde.kirigami as Kirigami
import org.kde.spectacle.private

ColumnLayout {
    property string captureMode: ""

    spacing: Kirigami.Units.smallSpacing

    QQC.CheckBox {
        Layout.fillWidth: true
        text: i18n("Capture mouse pointer")
        checked: Settings.videoIncludePointer
        onToggled: Settings.videoIncludePointer = checked
    }
    QQC.CheckBox {
        Layout.fillWidth: true
        text: i18n("Record microphone")
        checked: Settings.videoRecordMicrophone
        enabled: VideoPlatform.formatSupportsAudio(SpectacleCore.videoPlatform.effectivePreferredFormat)
        onToggled: Settings.videoRecordMicrophone = checked
    }
    QQC.CheckBox {
        Layout.fillWidth: true
        text: i18n("Record system audio")
        checked: Settings.videoRecordSystemAudio
        enabled: VideoPlatform.formatSupportsAudio(SpectacleCore.videoPlatform.effectivePreferredFormat)
        onToggled: Settings.videoRecordSystemAudio = checked
    }
}
