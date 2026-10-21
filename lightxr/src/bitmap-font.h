// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QApplication>

inline QFont lightxrBitmapFont() {
    static const QString family = [] {
        QFile file(":/lightxr/fonts/unifont-17.0.04.pcf");
        if (!file.open(QIODevice::ReadOnly)) qFatal("Bundled LightXR bitmap font is missing");
        const int id = QFontDatabase::addApplicationFontFromData(file.readAll());
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (id < 0 || families.isEmpty()) qFatal("Cannot load the bundled LightXR bitmap font");
        return families.first();
    }();
    QFont font(family); font.setStyleName("Sans-Serif"); font.setPixelSize(16);
    font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferBitmap | QFont::NoAntialias));
    if (QFontDatabase::isSmoothlyScalable(family, "Sans-Serif") || QFontInfo(font).pixelSize() != 16)
        qFatal("LightXR requires the native 16 px bitmap font strike");
    return font;
}
