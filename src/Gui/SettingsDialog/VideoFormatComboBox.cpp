/* SPDX-FileCopyrightText: 2024 Noah Davis <noahadvs@gmail.com>
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "VideoFormatComboBox.h"

using namespace Qt::StringLiterals;

VideoFormatComboBox::VideoFormatComboBox(VideoFormatModel *model, QWidget *parent)
    : QComboBox(parent)
{
    setModel(model);
    setObjectName(u"kcfg_preferredVideoFormat"_s);
    setProperty("kcfg_property", u"currentFormat"_s);
    connect(this, &QComboBox::currentIndexChanged, this, &VideoFormatComboBox::currentFormatChanged);
}

VideoPlatform::Format VideoFormatComboBox::currentFormat() const
{
    if (currentIndex() < 0) {
        return VideoPlatform::NoFormat;
    }
    return currentData(VideoFormatModel::FormatRole).value<VideoPlatform::Format>();
}

void VideoFormatComboBox::setCurrentFormat(VideoPlatform::Format format)
{
    auto model = static_cast<VideoFormatModel *>(this->model());
    const int preferredIndex = model->indexOfFormat(format);
    if (preferredIndex >= 0) {
        setCurrentIndex(preferredIndex);
        return;
    }
    // Unsupported stored preference: show the first supported row for UI but do not change the
    // configuration binding target. KConfig is driven by the user's selection, so we avoid emitting
    // NoFormat through this widget.
    if (model->rowCount() > 0) {
        setCurrentIndex(0);
    } else {
        setCurrentIndex(-1);
    }
}

#include "moc_VideoFormatComboBox.cpp"
