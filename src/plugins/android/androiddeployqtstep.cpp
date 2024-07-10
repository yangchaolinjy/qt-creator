// Copyright (C) 2016 BogDan Vatra <bog_dan_ro@yahoo.com>
// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "androiddeployqtstep.h"

#include "androidavdmanager.h"
#include "androidbuildapkstep.h"
#include "androidconstants.h"
#include "androiddevice.h"
#include "androidmanager.h"
#include "androidqtversion.h"
#include "androidtr.h"
#include "androidtr.h"

#include <coreplugin/fileutils.h>
#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>

#include <projectexplorer/abstractprocessstep.h>
#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/buildsystem.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/runconfiguration.h>
#include <projectexplorer/target.h>
#include <projectexplorer/taskhub.h>
#include <projectexplorer/toolchain.h>

#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitaspect.h>

#include <utils/algorithm.h>
#include <utils/async.h>
#include <utils/commandline.h>
#include <utils/environment.h>
#include <utils/futuresynchronizer.h>
#include <utils/layoutbuilder.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QInputDialog>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>

using namespace ProjectExplorer;
using namespace Tasking;
using namespace Utils;

using namespace std::chrono_literals;

namespace Android::Internal {

static Q_LOGGING_CATEGORY(deployStepLog, "qtc.android.build.androiddeployqtstep", QtWarningMsg)

const char UninstallPreviousPackageKey[] = "UninstallPreviousPackage";

const QLatin1String InstallFailedInconsistentCertificatesString("INSTALL_PARSE_FAILED_INCONSISTENT_CERTIFICATES");
const QLatin1String InstallFailedUpdateIncompatible("INSTALL_FAILED_UPDATE_INCOMPATIBLE");
const QLatin1String InstallFailedPermissionModelDowngrade("INSTALL_FAILED_PERMISSION_MODEL_DOWNGRADE");
const QLatin1String InstallFailedVersionDowngrade("INSTALL_FAILED_VERSION_DOWNGRADE");

enum DeployErrorFlag
{
    NoError = 0,
    InconsistentCertificates = 0x0001,
    UpdateIncompatible = 0x0002,
    PermissionModelDowngrade = 0x0004,
    VersionDowngrade = 0x0008,
    Failure = 0x0010
};

Q_DECLARE_FLAGS(DeployErrorFlags, DeployErrorFlag)

static DeployErrorFlags parseDeployErrors(const QString &deployOutputLine)
{
    DeployErrorFlags errorCode = NoError;

    if (deployOutputLine.contains(InstallFailedInconsistentCertificatesString))
        errorCode |= InconsistentCertificates;
    if (deployOutputLine.contains(InstallFailedUpdateIncompatible))
        errorCode |= UpdateIncompatible;
    if (deployOutputLine.contains(InstallFailedPermissionModelDowngrade))
        errorCode |= PermissionModelDowngrade;
    if (deployOutputLine.contains(InstallFailedVersionDowngrade))
        errorCode |= VersionDowngrade;

    return errorCode;
}

// AndroidDeployQtStep

struct FileToPull
{
    QString from;
    FilePath to;
};

static QList<FileToPull> filesToPull(Target *target)
{
    QList<FileToPull> fileList;
    const FilePath appProcessDir = AndroidManager::androidAppProcessDir(target);

    QString linkerName("linker");
    QString libDirName("lib");
    const QString preferredAbi = AndroidManager::apkDevicePreferredAbi(target);
    if (preferredAbi == ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A
        || preferredAbi == ProjectExplorer::Constants::ANDROID_ABI_X86_64) {
        fileList.append({"/system/bin/app_process64", appProcessDir / "app_process"});
        libDirName = "lib64";
        linkerName = "linker64";
    } else {
        fileList.append({"/system/bin/app_process32", appProcessDir / "app_process"});
        fileList.append({"/system/bin/app_process", appProcessDir / "app_process"});
    }

    fileList.append({"/system/bin/" + linkerName, appProcessDir / linkerName});
    fileList.append({"/system/" + libDirName + "/libc.so", appProcessDir / "libc.so"});

    for (const FileToPull &file : std::as_const(fileList))
        qCDebug(deployStepLog).noquote() << "Pulling file from device:" << file.from
                                         << "to:" << file.to;
    return fileList;
}

class AndroidDeployQtStep : public BuildStep
{
    Q_OBJECT

public:
    AndroidDeployQtStep(BuildStepList *bc, Id id);

signals:
    void askForUninstall(DeployErrorFlags errorCode);

private:
    void runCommand(const CommandLine &command);

    bool init() override;
    GroupItem runRecipe() final;
    DeployErrorFlags runDeploy(QPromise<void> &promise);
    void slotAskForUninstall(DeployErrorFlags errorFlags);

    void runImpl(QPromise<void> &promise);

    QWidget *createConfigWidget() override;

    void processReadyReadStdOutput(DeployErrorFlags &errorCode);
    void stdOutput(const QString &line);
    void processReadyReadStdError(DeployErrorFlags &errorCode);
    void stdError(const QString &line);

    void reportWarningOrError(const QString &message, Task::TaskType type);

    QString m_serialNumber;
    QString m_avdName;
    FilePath m_apkPath;
    QList<FileToPull> m_filesToPull;

    BoolAspect m_uninstallPreviousPackage{this};
    bool m_uninstallPreviousPackageRun = false;
    bool m_useAndroiddeployqt = false;
    bool m_askForUninstall = false;
    CommandLine m_androiddeployqtArgs;
    FilePath m_adbPath;
    FilePath m_command;
    FilePath m_workingDirectory;
    Environment m_environment;
    AndroidDeviceInfo m_deviceInfo;
    FutureSynchronizer m_synchronizer;
};

AndroidDeployQtStep::AndroidDeployQtStep(BuildStepList *parent, Id id)
    : BuildStep(parent, id)
{
    setImmutable(true);
    setUserExpanded(true);

    m_uninstallPreviousPackage.setSettingsKey(UninstallPreviousPackageKey);
    m_uninstallPreviousPackage.setLabel(Tr::tr("Uninstall the existing app before deployment"),
                                         BoolAspect::LabelPlacement::AtCheckBox);
    m_uninstallPreviousPackage.setValue(false);

    const QtSupport::QtVersion * const qt = QtSupport::QtKitAspect::qtVersion(kit());
    const bool forced = qt && qt->qtVersion() < AndroidManager::firstQtWithAndroidDeployQt;
    if (forced) {
        m_uninstallPreviousPackage.setValue(true);
        m_uninstallPreviousPackage.setEnabled(false);
    }

    connect(this, &AndroidDeployQtStep::askForUninstall,
            this, &AndroidDeployQtStep::slotAskForUninstall,
            Qt::BlockingQueuedConnection);
}

bool AndroidDeployQtStep::init()
{
    QtSupport::QtVersion *version = QtSupport::QtKitAspect::qtVersion(kit());
    if (!version) {
        reportWarningOrError(Tr::tr("The Qt version for kit %1 is invalid.").arg(kit()->displayName()),
                             Task::Error);
        return false;
    }

    m_androiddeployqtArgs = {};

    const QStringList androidABIs = AndroidManager::applicationAbis(target());
    if (androidABIs.isEmpty()) {
        reportWarningOrError(Tr::tr("No Android architecture (ABI) is set by the project."),
                             Task::Error);
        return false;
    }

    emit addOutput(Tr::tr("Initializing deployment to Android device/simulator"),
                   OutputFormat::NormalMessage);

    RunConfiguration *rc = target()->activeRunConfiguration();
    QTC_ASSERT(rc, reportWarningOrError(Tr::tr("The kit's run configuration is invalid."), Task::Error);
            return false);
    BuildConfiguration *bc = target()->activeBuildConfiguration();
    QTC_ASSERT(bc, reportWarningOrError(Tr::tr("The kit's build configuration is invalid."),
                                        Task::Error);
            return false);

    auto androidBuildApkStep = bc->buildSteps()->firstOfType<AndroidBuildApkStep>();
    const int minTargetApi = AndroidManager::minimumSDK(target());
    qCDebug(deployStepLog) << "Target architecture:" << androidABIs
                           << "Min target API" << minTargetApi;

    // Try to re-use user-provided information from an earlier step of the same type.
    BuildStepList *bsl = stepList();
    QTC_ASSERT(bsl, reportWarningOrError(Tr::tr("The kit's build steps list is invalid."), Task::Error);
            return false);
    auto androidDeployQtStep = bsl->firstOfType<AndroidDeployQtStep>();
    QTC_ASSERT(androidDeployQtStep,
               reportWarningOrError(Tr::tr("The kit's deploy configuration is invalid."), Task::Error);
            return false);
    AndroidDeviceInfo info;
    if (androidDeployQtStep != this)
        info = androidDeployQtStep->m_deviceInfo;

    const BuildSystem *bs = buildSystem();
    auto selectedAbis = bs->property(Constants::AndroidAbis).toStringList();

    const QString buildKey = target()->activeBuildKey();
    if (selectedAbis.isEmpty())
        selectedAbis = bs->extraData(buildKey, Constants::AndroidAbis).toStringList();

    if (selectedAbis.isEmpty())
        selectedAbis.append(bs->extraData(buildKey, Constants::AndroidAbi).toString());

    if (!info.isValid()) {
        const auto dev =
                static_cast<const AndroidDevice *>(DeviceKitAspect::device(kit()).get());
        if (!dev) {
            reportWarningOrError(Tr::tr("No valid deployment device is set."), Task::Error);
            return false;
        }

        // TODO: use AndroidDevice directly instead of AndroidDeviceInfo.
        info = AndroidDevice::androidDeviceInfoFromIDevice(dev);
        m_deviceInfo = info; // Keep around for later steps

        if (!info.isValid()) {
            reportWarningOrError(Tr::tr("The deployment device \"%1\" is invalid.")
                                 .arg(dev->displayName()), Task::Error);
            return false;
        }

        if (!dev->canSupportAbis(selectedAbis)) {
            const QString error = Tr::tr("The deployment device \"%1\" does not support the "
                                         "architectures used by the kit.\n"
                                         "The kit supports \"%2\", but the device uses \"%3\".")
                    .arg(dev->displayName()).arg(selectedAbis.join(", "))
                    .arg(dev->supportedAbis().join(", "));
            reportWarningOrError(error, Task::Error);
            return false;
        }

        if (!dev->canHandleDeployments()) {
            reportWarningOrError(Tr::tr("The deployment device \"%1\" is disconnected.")
                                 .arg(dev->displayName()), Task::Error);
            return false;
        }
    }

    const QtSupport::QtVersion * const qt = QtSupport::QtKitAspect::qtVersion(kit());
    if (qt && qt->supportsMultipleQtAbis() && !info.cpuAbi.isEmpty() &&
            !selectedAbis.contains(info.cpuAbi.first())) {
        TaskHub::addTask(DeploymentTask(Task::Warning,
            Tr::tr("Android: The main ABI of the deployment device (%1) is not selected. The app "
                   "execution or debugging might not work properly. Add it from Projects > Build > "
                   "Build Steps > qmake > ABIs.")
            .arg(info.cpuAbi.first())));
    }

    m_avdName = info.avdName;
    m_serialNumber = info.serialNumber;
    qCDebug(deployStepLog) << "Selected device info:" << info;

    AndroidManager::setDeviceSerialNumber(target(), m_serialNumber);
    AndroidManager::setDeviceApiLevel(target(), info.sdk);
    AndroidManager::setDeviceAbis(target(), info.cpuAbi);

    if (m_deviceInfo.isValid())
        m_filesToPull = filesToPull(target());

    emit addOutput(Tr::tr("Deploying to %1").arg(m_serialNumber), OutputFormat::NormalMessage);

    m_uninstallPreviousPackageRun = m_uninstallPreviousPackage();
    m_useAndroiddeployqt = version->qtVersion() >= AndroidManager::firstQtWithAndroidDeployQt;
    if (m_useAndroiddeployqt) {
        const QString buildKey = target()->activeBuildKey();
        const ProjectNode *node = target()->project()->findNodeForBuildKey(buildKey);
        if (!node) {
            reportWarningOrError(Tr::tr("The deployment step's project node is invalid."), Task::Error);
            return false;
        }
        m_apkPath = FilePath::fromString(node->data(Constants::AndroidApk).toString());
        if (!m_apkPath.isEmpty()) {
            m_command = AndroidConfig::adbToolPath();
            AndroidManager::setManifestPath(target(),
                FilePath::fromString(node->data(Constants::AndroidManifest).toString()));
        } else {
            QString jsonFile = AndroidQtVersion::androidDeploymentSettings(target()).toString();
            if (jsonFile.isEmpty()) {
                reportWarningOrError(Tr::tr("Cannot find the androiddeployqt input JSON file."),
                                     Task::Error);
                return false;
            }
            m_command = version->hostBinPath();
            if (m_command.isEmpty()) {
                reportWarningOrError(Tr::tr("Cannot find the androiddeployqt tool."), Task::Error);
                return false;
            }
            m_command = m_command.pathAppended("androiddeployqt").withExecutableSuffix();

            m_workingDirectory = AndroidManager::androidBuildDirectory(target());

            m_androiddeployqtArgs.addArgs({"--verbose",
                                           "--output", m_workingDirectory.toString(),
                                           "--no-build",
                                           "--input", jsonFile});

            m_androiddeployqtArgs.addArg("--gradle");

            if (buildType() == BuildConfiguration::Release)
                m_androiddeployqtArgs.addArgs({"--release"});

            if (androidBuildApkStep && androidBuildApkStep->signPackage()) {
                // The androiddeployqt tool is not really written to do stand-alone installations.
                // This hack forces it to use the correct filename for the apk file when installing
                // as a temporary fix until androiddeployqt gets the support. Since the --sign is
                // only used to get the correct file name of the apk, its parameters are ignored.
                m_androiddeployqtArgs.addArgs({"--sign", "foo", "bar"});
            }
        }
    } else {
        m_uninstallPreviousPackageRun = true;
        m_command = AndroidConfig::adbToolPath();
        m_apkPath = AndroidManager::packagePath(target());
        m_workingDirectory = bc ? AndroidManager::buildDirectory(target()): FilePath();
    }
    m_environment = bc ? bc->environment() : Environment();

    m_adbPath = AndroidConfig::adbToolPath();

    // Start the AVD if not running.
    if (!m_avdName.isEmpty() && AndroidAvdManager::findAvd(m_avdName).isEmpty())
        AndroidAvdManager::startAvdAsync(m_avdName);
    return true;
}

DeployErrorFlags AndroidDeployQtStep::runDeploy(QPromise<void> &promise)
{
    CommandLine cmd(m_command);
    if (m_useAndroiddeployqt && m_apkPath.isEmpty()) {
        cmd.addArgs(m_androiddeployqtArgs.arguments(), CommandLine::Raw);
        if (m_uninstallPreviousPackageRun)
            cmd.addArg("--install");
        else
            cmd.addArg("--reinstall");

        if (!m_serialNumber.isEmpty() && !m_serialNumber.startsWith("????"))
            cmd.addArgs({"--device", m_serialNumber});

    } else {
        RunConfiguration *rc = target()->activeRunConfiguration();
        QTC_ASSERT(rc, return Failure);
        QString packageName;

        if (m_uninstallPreviousPackageRun) {
            packageName = AndroidManager::packageName(target());
            if (packageName.isEmpty()) {
                reportWarningOrError(
                    Tr::tr("Cannot find the package name from AndroidManifest.xml nor "
                           "build.gradle files at \"%1\".")
                        .arg(AndroidManager::androidBuildDirectory(target()).toUserOutput()),
                    Task::Error);
                return Failure;
            }
            const QString msg = Tr::tr("Uninstalling the previous package \"%1\".").arg(packageName);
            qCDebug(deployStepLog) << msg;
            emit addOutput(msg, OutputFormat::NormalMessage);
            runCommand({m_adbPath,
                        {AndroidDeviceInfo::adbSelector(m_serialNumber), "uninstall", packageName}});
        }

        cmd.addArgs(AndroidDeviceInfo::adbSelector(m_serialNumber));
        cmd.addArgs({"install", "-r", m_apkPath.toString()});
    }

    Process process;
    process.setCommand(cmd);
    process.setWorkingDirectory(m_workingDirectory);
    process.setEnvironment(m_environment);
    process.setUseCtrlCStub(true);

    DeployErrorFlags deployError = NoError;

    process.setStdOutLineCallback([this, &deployError](const QString &line) {
        deployError |= parseDeployErrors(line);
        stdOutput(line);
    });
    process.setStdErrLineCallback([this, &deployError](const QString &line) {
        deployError |= parseDeployErrors(line);
        stdError(line);
    });

    process.start();

    emit addOutput(Tr::tr("Starting: \"%1\"").arg(cmd.toUserOutput()), OutputFormat::NormalMessage);

    while (!process.waitForFinished(200ms)) {
        if (process.state() == QProcess::NotRunning)
            break;

        if (promise.isCanceled()) {
            process.kill();
            process.waitForFinished();
        }
    }

    const QProcess::ExitStatus exitStatus = process.exitStatus();
    const int exitCode = process.exitCode();

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        emit addOutput(Tr::tr("The process \"%1\" exited normally.").arg(m_command.toUserOutput()),
                       OutputFormat::NormalMessage);
    } else if (exitStatus == QProcess::NormalExit) {
        const QString error = Tr::tr("The process \"%1\" exited with code %2.")
                .arg(m_command.toUserOutput(), QString::number(exitCode));
        reportWarningOrError(error, Task::Error);
    } else {
        const QString error = Tr::tr("The process \"%1\" crashed.").arg(m_command.toUserOutput());
        reportWarningOrError(error, Task::Error);
    }

    if (deployError != NoError) {
        if (m_uninstallPreviousPackageRun) {
            deployError = Failure; // Even re-install failed. Set to Failure.
            reportWarningOrError(
                        Tr::tr("Installing the app failed even after uninstalling the previous one."),
                        Task::Error);
        }
    } else if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
        // Set the deployError to Failure when no deployError code was detected
        // but the adb tool failed otherwise relay the detected deployError.
        reportWarningOrError(Tr::tr("Installing the app failed with an unknown error."), Task::Error);
        deployError = Failure;
    }

    return deployError;
}

void AndroidDeployQtStep::slotAskForUninstall(DeployErrorFlags errorFlags)
{
    Q_ASSERT(errorFlags > 0);

    QString uninstallMsg = Tr::tr("Deployment failed with the following errors:") + "\n\n";
    if (errorFlags & InconsistentCertificates)
        uninstallMsg += InstallFailedInconsistentCertificatesString + '\n';
    if (errorFlags & UpdateIncompatible)
        uninstallMsg += InstallFailedUpdateIncompatible + '\n';
    if (errorFlags & PermissionModelDowngrade)
        uninstallMsg += InstallFailedPermissionModelDowngrade + '\n';
    if (errorFlags & VersionDowngrade)
        uninstallMsg += InstallFailedVersionDowngrade + '\n';
    uninstallMsg += '\n';
    uninstallMsg.append(Tr::tr("Uninstalling the installed package may solve the issue.") + '\n');
    uninstallMsg.append(Tr::tr("Do you want to uninstall the existing package?"));

    m_askForUninstall = QMessageBox::critical(nullptr, Tr::tr("Install failed"), uninstallMsg,
                        QMessageBox::Yes, QMessageBox::No) == QMessageBox::Yes;
}

// TODO: This implementation is not thread safe.
void AndroidDeployQtStep::runImpl(QPromise<void> &promise)
{
    if (!m_avdName.isEmpty()) {
        const QString serialNumber = AndroidAvdManager::waitForAvd(m_avdName, promise.future());
        qCDebug(deployStepLog) << "Deploying to AVD:" << m_avdName << serialNumber;
        if (serialNumber.isEmpty()) {
            reportWarningOrError(Tr::tr("The deployment AVD \"%1\" cannot be started.")
                                 .arg(m_avdName), Task::Error);
            promise.future().cancel();
            return;
        }
        m_serialNumber = serialNumber;
        qCDebug(deployStepLog) << "Deployment device serial number changed:" << serialNumber;
        AndroidManager::setDeviceSerialNumber(target(), serialNumber);
    }

    DeployErrorFlags returnValue = runDeploy(promise);
    if (returnValue > NoError && returnValue < Failure) {
        emit askForUninstall(returnValue);
        if (m_askForUninstall) {
            m_uninstallPreviousPackageRun = true;
            returnValue = runDeploy(promise);
        }
    }

    if (!m_filesToPull.isEmpty())
        emit addOutput(Tr::tr("Pulling files necessary for debugging."), OutputFormat::NormalMessage);

    // Note that values are not necessarily unique, e.g. app_process is looked up in several
    // directories
    for (const FileToPull &file : std::as_const(m_filesToPull))
        file.to.removeFile();

    for (const FileToPull &file : std::as_const(m_filesToPull)) {
        const FilePath parentDir = file.to.parentDir();
        if (!parentDir.ensureWritableDir()) {
            const QString error = QString("Package deploy: Unable to create directory %1.")
                                      .arg(parentDir.nativePath());
            reportWarningOrError(error, Task::Error);
        }

        runCommand({m_adbPath, {AndroidDeviceInfo::adbSelector(m_serialNumber), "pull", file.from,
                                file.to.nativePath()}});
        if (!file.to.exists()) {
            const QString error = Tr::tr("Package deploy: Failed to pull \"%1\" to \"%2\".")
                                      .arg(file.from, file.to.nativePath());
            reportWarningOrError(error, Task::Error);
        }
    }
    if (returnValue != NoError)
        promise.future().cancel();
}

GroupItem AndroidDeployQtStep::runRecipe()
{
    const auto onSetup = [this](Async<void> &async) {
        async.setConcurrentCallData(&AndroidDeployQtStep::runImpl, this);
        async.setFutureSynchronizer(&m_synchronizer);
    };
    return AsyncTask<void>(onSetup);
}

void AndroidDeployQtStep::runCommand(const CommandLine &command)
{
    Process buildProc;
    emit addOutput(Tr::tr("Package deploy: Running command \"%1\".").arg(command.toUserOutput()),
                   OutputFormat::NormalMessage);

    buildProc.setCommand(command);
    buildProc.runBlocking(2min, EventLoopMode::On);
    if (buildProc.result() != ProcessResult::FinishedWithSuccess)
        reportWarningOrError(buildProc.exitMessage(), Task::Error);
}

QWidget *AndroidDeployQtStep::createConfigWidget()
{
    auto widget = new QWidget;
    auto installCustomApkButton = new QPushButton(widget);
    installCustomApkButton->setText(Tr::tr("Install an APK File"));

    connect(installCustomApkButton, &QAbstractButton::clicked, this, [this, widget] {
        const FilePath packagePath
                = FileUtils::getOpenFilePath(widget,
                                             Tr::tr("Qt Android Installer"),
                                             FileUtils::homePath(),
                                             Tr::tr("Android package (*.apk)"));
        if (!packagePath.isEmpty())
            AndroidManager::installQASIPackage(target(), packagePath);
    });

    using namespace Layouting;

    Form {
        m_uninstallPreviousPackage, br,
        installCustomApkButton,
        noMargin
    }.attachTo(widget);

    return widget;
}

void AndroidDeployQtStep::stdOutput(const QString &line)
{
    emit addOutput(line, BuildStep::OutputFormat::Stdout, BuildStep::DontAppendNewline);
}

void AndroidDeployQtStep::stdError(const QString &line)
{
    emit addOutput(line, BuildStep::OutputFormat::Stderr, BuildStep::DontAppendNewline);

    QString newOutput = line;
    static const QRegularExpression re("^(\\n)+");
    newOutput.remove(re);

    if (newOutput.isEmpty())
        return;

    if (newOutput.startsWith("warning", Qt::CaseInsensitive)
        || newOutput.startsWith("note", Qt::CaseInsensitive)
        || newOutput.startsWith(QLatin1String("All files should be loaded."))) {
        TaskHub::addTask(DeploymentTask(Task::Warning, newOutput));
    } else {
        TaskHub::addTask(DeploymentTask(Task::Error, newOutput));
    }
}


void AndroidDeployQtStep::reportWarningOrError(const QString &message, Task::TaskType type)
{
    qCDebug(deployStepLog).noquote() << message;
    emit addOutput(message, OutputFormat::ErrorMessage);
    TaskHub::addTask(DeploymentTask(type, message));
}

// AndroidDeployQtStepFactory

class AndroidDeployQtStepFactory final : public BuildStepFactory
{
public:
    AndroidDeployQtStepFactory()
    {
        registerStep<AndroidDeployQtStep>(Constants::ANDROID_DEPLOY_QT_ID);
        setSupportedStepList(ProjectExplorer::Constants::BUILDSTEPS_DEPLOY);
        setSupportedDeviceType(Constants::ANDROID_DEVICE_TYPE);
        setRepeatable(false);
        setDisplayName(Tr::tr("Deploy to Android device"));
    }
};

void setupAndroidDeployQtStep()
{
    static AndroidDeployQtStepFactory theAndroidDeployQtStepFactory;
}

} // Android::Internal

#include "androiddeployqtstep.moc"
