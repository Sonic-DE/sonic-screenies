/* SPDX-FileCopyrightText: 2026 Spectacle contributors
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#pragma once

#include <QString>
#include <QUrl>

namespace PlatformOutput {

// Compute the URL for an implicit recording output when the caller did not supply one.
// Honors Settings::preferredVideoFormat via the caller-provided effective format so fallback does not
// produce an extension mismatch. Returns a local file URL or empty on failure.
QUrl makeImplicitOutputUrl(const QString& filenameTemplate, const QString& caption,
    const QString& saveLocation, int effectiveFormat);

// Remove a file if present. Safe to call from worker threads; logging only.
void removeLocalFile(const QString& path);

// Verify that a finalized output URL is a local file that exists and is nonempty.
bool isAcceptableFinalOutput(const QUrl& url, qint64 minimumSize = 1);

} // namespace PlatformOutput
