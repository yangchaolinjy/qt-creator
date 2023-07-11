// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#pragma once

#include "buildstep.h"

#include <QProcess>

namespace Utils {
class CommandLine;
enum class ProcessResult;
class Process;
}

namespace Tasking { class Group; }

namespace ProjectExplorer {
class ProcessParameters;

// Documentation inside.
class PROJECTEXPLORER_EXPORT AbstractProcessStep : public BuildStep
{
    Q_OBJECT

public:
    ProcessParameters *processParameters();

protected:
    AbstractProcessStep(BuildStepList *bsl, Utils::Id id);
    ~AbstractProcessStep() override;

    bool setupProcessParameters(ProcessParameters *params) const;
    bool ignoreReturnValue() const;
    void setIgnoreReturnValue(bool b);
    void setDoneHook(const std::function<void(bool)> &doneHook);
    void setCommandLineProvider(const std::function<Utils::CommandLine()> &provider);
    void setWorkingDirectoryProvider(const std::function<Utils::FilePath()> &provider);
    void setEnvironmentModifier(const std::function<void(Utils::Environment &)> &modifier);
    void setUseEnglishOutput();

    void emitFaultyConfigurationMessage();

    bool init() override;
    void setupOutputFormatter(Utils::OutputFormatter *formatter) override;
    void doRun() override;
    void doCancel() override;
    void setLowPriority();
    void setDisplayedParameters(ProcessParameters *params);

    bool checkWorkingDirectory();
    void setupProcess(Utils::Process *process);
    void handleProcessDone(const Utils::Process &process);
    void runTaskTree(const Tasking::Group &recipe);

private:
    void setupStreams();
    void finish(Utils::ProcessResult result);

    class Private;
    Private *d;
};

} // namespace ProjectExplorer
