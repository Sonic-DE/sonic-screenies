/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "PlatformOutput.h"

#include "ExportManager.h"
#include "Platforms/VideoPlatform.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace PlatformOutput {

QUrl makeImplicitOutputUrl(const QString& filenameTemplate, const QString& caption,
    const QString& saveLocation, int effectiveFormat)
{
    auto exportManager = ExportManager::instance();
    exportManager->updateTimestamp();

    const auto format = static_cast<VideoPlatform::Format>(effectiveFormat);
    const auto filename = ExportManager::formattedFilename(filenameTemplate,
        exportManager->timestamp(),
        caption,
        QUrl(saveLocation));
    auto tempUrl = exportManager->tempVideoUrl(filename);
    if (!tempUrl.isLocalFile()) {
        return {};
    }

    const auto expectedExt = VideoPlatform::extensionForFormat(format);
    if (expectedExt.isEmpty()) {
        return {};
    }

    auto localPath = tempUrl.toLocalFile();
    if (!localPath.endsWith(u"."_s + expectedExt, Qt::CaseInsensitive)) {
        QFileInfo info(localPath);
        localPath = info.absolutePath() + u"/"_s + info.completeBaseName() + u"."_s + expectedExt;
    }

    QDir().mkpath(QFileInfo(localPath).absolutePath());
    return QUrl::fromLocalFile(localPath);
}

void removeLocalFile(const QString& path)
{
    if (path.isEmpty()) {
        return;
    }
    QFile::remove(path);
}

bool isAcceptableFinalOutput(const QUrl& url, qint64 minimumSize)
{
    if (!url.isValid() || !url.isLocalFile()) {
        return false;
    }
    QFileInfo info(url.toLocalFile());
    return info.exists() && info.isFile() && info.size() >= minimumSize;
}

} // namespace PlatformOutput
