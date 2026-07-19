/* SPDX-FileCopyrightText: 2019 Boudhayan Gupta <bgupta@kde.org>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "PlatformLoader.h"

#include "ImagePlatformXcb.h"
#include "PlatformNull.h"
#include "VideoPlatformX11.h"

#include <QCoreApplication>
#include <QString>
#include <QStringLiteral>

using namespace Qt::StringLiterals;

namespace {

QString forcedImage()
{
    return qEnvironmentVariable("SPECTACLE_IMAGE_PLATFORM");
}

QString forcedVideo()
{
    return qEnvironmentVariable("SPECTACLE_VIDEO_PLATFORM");
}

} // namespace

ImagePlatformPtr loadImagePlatform()
{
    const auto forced = forcedImage();
    if (!forced.isEmpty()) {
        if (forced == QString::fromLatin1(ImagePlatformXcb::staticMetaObject.className())) {
            return std::make_unique<ImagePlatformXcb>();
        }
        if (forced == u"ImagePlatformNull"_s) {
            return std::make_unique<ImagePlatformNull>();
        }
    }
    return std::make_unique<ImagePlatformXcb>();
}

VideoPlatformPtr loadVideoPlatform()
{
    const auto forced = forcedVideo();
    if (!forced.isEmpty()) {
        if (forced == QString::fromLatin1(Spectacle::VideoPlatformX11::staticMetaObject.className())) {
            return std::make_unique<Spectacle::VideoPlatformX11>();
        }
        if (forced == u"VideoPlatformNull"_s) {
            return std::make_unique<VideoPlatformNull>();
        }
    }
    return std::make_unique<Spectacle::VideoPlatformX11>();
}
