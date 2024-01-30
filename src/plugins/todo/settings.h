// Copyright (C) 2016 Dmitry Savchenko
// Copyright (C) 2016 Vasiliy Sorokin
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "keyword.h"

namespace Utils { class QtcSettings; }

namespace Todo::Internal {

enum ScanningScope {
    ScanningScopeCurrentFile,
    ScanningScopeProject,
    ScanningScopeSubProject,
    ScanningScopeMax
};

class Settings
{
public:
    KeywordList keywords;
    ScanningScope scanningScope = ScanningScopeCurrentFile;
    bool keywordsEdited = false;

    void save(Utils::QtcSettings *settings) const;
    void load(Utils::QtcSettings *settings);
    void setDefault();
};

Settings &todoSettings();

void setupTodoSettingsPage(const std::function<void()> &onApply);

} // Todo::Internal

Q_DECLARE_METATYPE(Todo::Internal::ScanningScope)
