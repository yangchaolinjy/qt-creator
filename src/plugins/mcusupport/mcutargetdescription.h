// Copyright (C) 2022 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0+ OR GPL-3.0 WITH Qt-GPL-exception-1.0

#pragma once

#include <utils/filepath.h>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVersionNumber>

namespace McuSupport::Internal {

struct VersionDetection
{
    QString regex;
    QString filePattern;
    QString executableArgs;
    QString xmlElement;
    QString xmlAttribute;
    bool isFile;
}; // struct VersionDetection

struct PackageDescription
{
    QString label;
    QString envVar;
    QString cmakeVar;
    QString description;
    QString setting;
    Utils::FilePath defaultPath;
    Utils::FilePath validationPath;
    QStringList versions;
    VersionDetection versionDetection;
    bool shouldAddToSystemPath;
}; //struct PackageDescription

struct McuTargetDescription
{
    enum class TargetType { MCU, Desktop };

    Utils::FilePath sourceFile;
    QString qulVersion;
    QString compatVersion;
    struct Platform
    {
        QString id;
        QString name;
        QString vendor;
        QVector<int> colorDepths;
        TargetType type;
        QList<PackageDescription> entries;
    } platform;
    struct Toolchain
    {
        QString id;
        QStringList versions;
        PackageDescription compiler;
        PackageDescription file;
    } toolchain;
    PackageDescription boardSdk;
    struct FreeRTOS
    {
        QString envVar;
        PackageDescription package;
    } freeRTOS;
};

} // namespace McuSupport::Internal

Q_DECLARE_METATYPE(McuSupport::Internal::McuTargetDescription)
