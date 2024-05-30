// Copyright (C) 2016 BogDan Vatra <bog_dan_ro@yahoo.com>
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "androidmanager.h"

#include "androidavdmanager.h"
#include "androidbuildapkstep.h"
#include "androidconstants.h"
#include "androiddevice.h"
#include "androidqtversion.h"
#include "androidtr.h"

#include <cmakeprojectmanager/cmakeprojectconstants.h>

#include <coreplugin/icontext.h>
#include <coreplugin/messagemanager.h>

#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/buildsystem.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/target.h>

#include <qtsupport/qtkitaspect.h>

#include <utils/algorithm.h>
#include <utils/qtcprocess.h>
#include <utils/qtcassert.h>

#include <QDomDocument>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QVersionNumber>

using namespace Android::Internal;
using namespace Core;
using namespace ProjectExplorer;
using namespace Utils;

using namespace std::chrono_literals;

namespace Android::AndroidManager {

const char AndroidManifestName[] = "AndroidManifest.xml";
const char AndroidDeviceSn[] = "AndroidDeviceSerialNumber";
const char AndroidDeviceAbis[] = "AndroidDeviceAbis";
const char ApiLevelKey[] = "AndroidVersion.ApiLevel";
const char qtcSignature[] = "This file is generated by QtCreator to be read by "
                            "androiddeployqt and should not be modified by hand.";

static Q_LOGGING_CATEGORY(androidManagerLog, "qtc.android.androidManager", QtWarningMsg)

static std::optional<QDomElement> documentElement(const FilePath &fileName)
{
    QFile file(fileName.toString());
    if (!file.open(QIODevice::ReadOnly)) {
        MessageManager::writeDisrupting(Tr::tr("Cannot open: %1").arg(fileName.toUserOutput()));
        return {};
    }
    QDomDocument doc;
    if (!doc.setContent(file.readAll())) {
        MessageManager::writeDisrupting(Tr::tr("Cannot parse: %1").arg(fileName.toUserOutput()));
        return {};
    }
    return doc.documentElement();
}

static int parseMinSdk(const QDomElement &manifestElem)
{
    const QDomElement usesSdk = manifestElem.firstChildElement("uses-sdk");
    if (!usesSdk.isNull() && usesSdk.hasAttribute("android:minSdkVersion")) {
        bool ok;
        int tmp = usesSdk.attribute("android:minSdkVersion").toInt(&ok);
        if (ok)
            return tmp;
    }
    return 0;
}

static const ProjectNode *currentProjectNode(const Target *target)
{
    return target->project()->findNodeForBuildKey(target->activeBuildKey());
}

QString packageName(const Target *target)
{
    QString packageName;

    // Check build.gradle
    auto isComment = [](const QByteArray &trimmed) {
        return trimmed.startsWith("//") || trimmed.startsWith('*') || trimmed.startsWith("/*");
    };

    const FilePath androidBuildDir = androidBuildDirectory(target);
    const expected_str<QByteArray> gradleContents = androidBuildDir.pathAppended("build.gradle")
                                                        .fileContents();
    if (gradleContents) {
        const auto lines = gradleContents->split('\n');
        for (const auto &line : lines) {
            const QByteArray trimmed = line.trimmed();
            if (isComment(trimmed) || !trimmed.contains("namespace"))
                continue;

            int idx = trimmed.indexOf('=');
            if (idx == -1)
                idx = trimmed.indexOf(' ');
            if (idx > -1) {
                packageName = QString::fromUtf8(trimmed.mid(idx + 1).trimmed());
                if (packageName == "androidPackageName") {
                    // Check gradle.properties
                    const QSettings gradleProperties = QSettings(
                        androidBuildDir.pathAppended("gradle.properties").toFSPathString(),
                        QSettings::IniFormat);
                    packageName = gradleProperties.value("androidPackageName").toString();
                } else {
                    // Remove quotes
                    if (packageName.size() > 2)
                        packageName = packageName.remove(0, 1).chopped(1);
                }

                break;
            }
        }
    }

    if (packageName.isEmpty()) {
        // Check AndroidManifest.xml
        const auto element = documentElement(AndroidManager::manifestPath(target));
        if (element)
            packageName = element->attribute("package");
    }

    return packageName;
}

QString activityName(const Target *target)
{
    const auto element = documentElement(AndroidManager::manifestPath(target));
    if (!element)
        return {};
    return element->firstChildElement("application").firstChildElement("activity")
                                                    .attribute("android:name");
}

static FilePath manifestSourcePath(const Target *target)
{
    if (const ProjectNode *node = currentProjectNode(target)) {
        const QString packageSource
            = node->data(Android::Constants::AndroidPackageSourceDir).toString();
        if (!packageSource.isEmpty()) {
            const FilePath manifest = FilePath::fromUserInput(packageSource + "/AndroidManifest.xml");
            if (manifest.exists())
                return manifest;
        }
    }
    return manifestPath(target);
}

/*!
    Returns the minimum Android API level set for the APK. Minimum API level
    of the kit is returned if the manifest file of the APK cannot be found
    or parsed.
*/
int minimumSDK(const Target *target)
{
    const auto element = documentElement(manifestSourcePath(target));
    if (!element)
        return minimumSDK(target->kit());

    const int minSdkVersion = parseMinSdk(*element);
    if (minSdkVersion == 0)
        return AndroidManager::defaultMinimumSDK(QtSupport::QtKitAspect::qtVersion(target->kit()));
    return minSdkVersion;
}

/*!
    Returns the minimum Android API level required by the kit to compile. -1 is
    returned if the kit does not support Android.
*/
int minimumSDK(const Kit *kit)
{
    int minSdkVersion = -1;
    QtSupport::QtVersion *version = QtSupport::QtKitAspect::qtVersion(kit);
    if (version && version->targetDeviceTypes().contains(Constants::ANDROID_DEVICE_TYPE)) {
        const FilePath stockManifestFilePath = FilePath::fromUserInput(
            version->prefix().toString() + "/src/android/templates/AndroidManifest.xml");

        const auto element = documentElement(stockManifestFilePath);
        if (element)
            minSdkVersion = parseMinSdk(*element);
    }
    if (minSdkVersion == 0)
        return AndroidManager::defaultMinimumSDK(version);
    return minSdkVersion;
}

QString buildTargetSDK(const Target *target)
{
    if (auto bc = target->activeBuildConfiguration()) {
        if (auto androidBuildApkStep = bc->buildSteps()->firstOfType<AndroidBuildApkStep>())
            return androidBuildApkStep->buildTargetSdk();
    }

    QString fallback = AndroidConfig::apiLevelNameFor(
                AndroidConfigurations::sdkManager()->latestAndroidSdkPlatform());
    return fallback;
}

QStringList applicationAbis(const Target *target)
{
    auto qt = dynamic_cast<AndroidQtVersion *>(QtSupport::QtKitAspect::qtVersion(target->kit()));
    return qt ? qt->androidAbis() : QStringList();
}

QString archTriplet(const QString &abi)
{
    if (abi == ProjectExplorer::Constants::ANDROID_ABI_X86) {
        return {"i686-linux-android"};
    } else if (abi == ProjectExplorer::Constants::ANDROID_ABI_X86_64) {
        return {"x86_64-linux-android"};
    } else if (abi == ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A) {
        return {"aarch64-linux-android"};
    }
    return {"arm-linux-androideabi"};
}

QJsonObject deploymentSettings(const Target *target)
{
    QtSupport::QtVersion *qt = QtSupport::QtKitAspect::qtVersion(target->kit());
    if (!qt)
        return {};

    auto tc = ToolchainKitAspect::cxxToolchain(target->kit());
    if (!tc || tc->typeId() != Constants::ANDROID_TOOLCHAIN_TYPEID)
        return {};
    QJsonObject settings;
    settings["_description"] = qtcSignature;
    settings["qt"] = qt->prefix().toString();
    settings["ndk"] = AndroidConfig::ndkLocation(qt).toString();
    settings["sdk"] = AndroidConfig::sdkLocation().toString();
    if (!qt->supportsMultipleQtAbis()) {
        const QStringList abis = applicationAbis(target);
        QTC_ASSERT(abis.size() == 1, return {});
        settings["stdcpp-path"] = (AndroidConfig::toolchainPath(qt)
                                      / "sysroot/usr/lib"
                                      / archTriplet(abis.first())
                                      / "libc++_shared.so").toString();
    } else {
        settings["stdcpp-path"]
            = AndroidConfig::toolchainPath(qt).pathAppended("sysroot/usr/lib").toString();
    }
    settings["toolchain-prefix"] =  "llvm";
    settings["tool-prefix"] = "llvm";
    settings["useLLVM"] = true;
    settings["ndk-host"] = AndroidConfig::toolchainHost(qt);
    return settings;
}

bool isQtCreatorGenerated(const FilePath &deploymentFile)
{
    QFile f{deploymentFile.toString()};
    if (!f.open(QIODevice::ReadOnly))
        return false;
    return QJsonDocument::fromJson(f.readAll()).object()["_description"].toString() == qtcSignature;
}

FilePath androidBuildDirectory(const Target *target)
{
    QString suffix;
    const Project *project = target->project();
    if (project->extraData(Android::Constants::AndroidBuildTargetDirSupport).toBool()
        && project->extraData(Android::Constants::UseAndroidBuildTargetDir).toBool())
        suffix = QString("-%1").arg(target->activeBuildKey());

    return buildDirectory(target) / (Constants::ANDROID_BUILD_DIRECTORY + suffix);
}

FilePath androidAppProcessDir(const Target *target)
{
    return buildDirectory(target) / Constants::ANDROID_APP_PROCESS_DIRECTORY;
}

bool isQt5CmakeProject(const ProjectExplorer::Target *target)
{
    const QtSupport::QtVersion *qt = QtSupport::QtKitAspect::qtVersion(target->kit());
    const bool isQt5 = qt && qt->qtVersion() < QVersionNumber(6, 0, 0);
    const Context cmakeCtx(CMakeProjectManager::Constants::CMAKE_PROJECT_ID);
    const bool isCmakeProject = (target->project()->projectContext() == cmakeCtx);
    return isQt5 && isCmakeProject;
}

FilePath buildDirectory(const Target *target)
{
    if (const BuildSystem *bs = target->buildSystem()) {
        const QString buildKey = target->activeBuildKey();

        // Get the target build dir based on the settings file path
        FilePath buildDir;
        const ProjectNode *node = target->project()->findNodeForBuildKey(buildKey);
        if (node) {
            const QString settingsFile = node->data(Constants::AndroidDeploySettingsFile).toString();
            buildDir = FilePath::fromUserInput(settingsFile).parentDir();
        }

        if (!buildDir.isEmpty())
            return buildDir;

        // Otherwise fallback to target working dir
        buildDir = bs->buildTarget(target->activeBuildKey()).workingDirectory;
        if (isQt5CmakeProject(target)) {
            // Return the main build dir and not the android libs dir
            const QString libsDir = QString(Constants::ANDROID_BUILD_DIRECTORY) + "/libs";
            FilePath parentDuildDir = buildDir.parentDir();
            if (parentDuildDir.endsWith(libsDir) || libsDir.endsWith(libsDir + "/"))
                return parentDuildDir.parentDir().parentDir();
        } else {
            // Qt6 + CMake: Very cautios hack to work around QTCREATORBUG-26479 for simple projects
            const QString jsonFileName =
                AndroidQtVersion::androidDeploymentSettingsFileName(target);
            const FilePath jsonFile = buildDir / jsonFileName;
            if (!jsonFile.exists()) {
                const FilePath projectBuildDir = bs->buildConfiguration()->buildDirectory();
                if (buildDir != projectBuildDir) {
                    const FilePath projectJsonFile = projectBuildDir / jsonFileName;
                    if (projectJsonFile.exists())
                        buildDir = projectBuildDir;
                }
            }
        }
        return buildDir;
    }
    return {};
}

enum PackageFormat { Apk, Aab };

static QString packageSubPath(PackageFormat format, BuildConfiguration::BuildType buildType,
                              bool sig)
{
    const bool deb = (buildType == BuildConfiguration::Debug);

    if (format == Apk) {
        if (deb) {
            return sig ? packageSubPath(Apk, BuildConfiguration::Release, true) // Intentional
                       : QLatin1String("apk/debug/android-build-debug.apk");
        }
        return QLatin1String(sig ? "apk/release/android-build-release-signed.apk"
                                 : "apk/release/android-build-release-unsigned.apk");
    }
    return QLatin1String(deb ? "bundle/debug/android-build-debug.aab"
                             : "bundle/release/android-build-release.aab");
}

FilePath packagePath(const Target *target)
{
    QTC_ASSERT(target, return {});

    auto bc = target->activeBuildConfiguration();
    if (!bc)
        return {};
    auto buildApkStep = bc->buildSteps()->firstOfType<AndroidBuildApkStep>();
    if (!buildApkStep)
        return {};

    const QString subPath = packageSubPath(buildApkStep->buildAAB() ? Aab : Apk,
                                           bc->buildType(), buildApkStep->signPackage());

    return androidBuildDirectory(target) / "build/outputs" / subPath;
}

Abi androidAbi2Abi(const QString &androidAbi)
{
    if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_ARM64_V8A) {
        return Abi{Abi::Architecture::ArmArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   64, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_ARMEABI_V7A) {
        return Abi{Abi::Architecture::ArmArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   32, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_X86_64) {
        return Abi{Abi::Architecture::X86Architecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   64, androidAbi};
    } else if (androidAbi == ProjectExplorer::Constants::ANDROID_ABI_X86) {
        return Abi{Abi::Architecture::X86Architecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   32, androidAbi};
    } else {
        return Abi{Abi::Architecture::UnknownArchitecture,
                   Abi::OS::LinuxOS,
                   Abi::OSFlavor::AndroidLinuxFlavor,
                   Abi::BinaryFormat::ElfFormat,
                   0, androidAbi};
    }
}

bool skipInstallationAndPackageSteps(const Target *target)
{
    // For projects using Qt 5.15 and Qt 6, the deployment settings file
    // is generated by CMake/qmake and not Qt Creator, so if such file doesn't exist
    // or it's been generated by Qt Creator, we can assume the project is not
    // an android app.
    const FilePath inputFile = AndroidQtVersion::androidDeploymentSettings(target);
    if (!inputFile.exists() || AndroidManager::isQtCreatorGenerated(inputFile))
        return true;

    const Project *p = target->project();

    const Context cmakeCtx(CMakeProjectManager::Constants::CMAKE_PROJECT_ID);
    const bool isCmakeProject = p->projectContext() == cmakeCtx;
    if (isCmakeProject)
        return false; // CMake reports ProductType::Other for Android Apps

    const ProjectNode *n = p->rootProjectNode()->findProjectNode([] (const ProjectNode *n) {
        return n->productType() == ProductType::App;
    });
    return n == nullptr; // If no Application target found, then skip steps
}

FilePath manifestPath(const Target *target)
{
    QVariant manifest = target->namedSettings(AndroidManifestName);
    if (manifest.isValid())
        return manifest.value<FilePath>();
    return androidBuildDirectory(target).pathAppended(AndroidManifestName);
}

void setManifestPath(Target *target, const FilePath &path)
{
     target->setNamedSettings(AndroidManifestName, QVariant::fromValue(path));
}

QString deviceSerialNumber(const Target *target)
{
    return target->namedSettings(AndroidDeviceSn).toString();
}

void setDeviceSerialNumber(Target *target, const QString &deviceSerialNumber)
{
    qCDebug(androidManagerLog) << "Target device serial changed:"
                               << target->displayName() << deviceSerialNumber;
    target->setNamedSettings(AndroidDeviceSn, deviceSerialNumber);
}

static QString preferredAbi(const QStringList &appAbis, const Target *target)
{
    const auto deviceAbis = target->namedSettings(AndroidDeviceAbis).toStringList();
    for (const auto &abi : deviceAbis) {
        if (appAbis.contains(abi))
            return abi;
    }
    return {};
}

QString apkDevicePreferredAbi(const Target *target)
{
    const FilePath libsPath = androidBuildDirectory(target).pathAppended("libs");
    if (!libsPath.exists()) {
        if (const ProjectNode *node = currentProjectNode(target)) {
            const QString abi = preferredAbi(
                        node->data(Android::Constants::AndroidAbis).toStringList(), target);
            if (abi.isEmpty())
                return node->data(Android::Constants::AndroidAbi).toString();
        }
    }
    QStringList apkAbis;
    const FilePaths libsPaths = libsPath.dirEntries(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const FilePath &abiDir : libsPaths) {
        if (!abiDir.dirEntries({{"*.so"}, QDir::Files | QDir::NoDotAndDotDot}).isEmpty())
            apkAbis << abiDir.fileName();
    }
    return preferredAbi(apkAbis, target);
}

void setDeviceAbis(Target *target, const QStringList &deviceAbis)
{
    target->setNamedSettings(AndroidDeviceAbis, deviceAbis);
}

int deviceApiLevel(const Target *target)
{
    return target->namedSettings(ApiLevelKey).toInt();
}

void setDeviceApiLevel(Target *target, int level)
{
    qCDebug(androidManagerLog) << "Target device API level changed:"
                               << target->displayName() << level;
    target->setNamedSettings(ApiLevelKey, level);
}

int defaultMinimumSDK(const QtSupport::QtVersion *qtVersion)
{
    if (qtVersion && qtVersion->qtVersion() >= QVersionNumber(6, 0))
        return 23;
    else if (qtVersion && qtVersion->qtVersion() >= QVersionNumber(5, 13))
        return 21;
    else
        return 16;
}

QString androidNameForApiLevel(int x)
{
    switch (x) {
    case 2:
        return QLatin1String("Android 1.1");
    case 3:
        return QLatin1String("Android 1.5 (\"Cupcake\")");
    case 4:
        return QLatin1String("Android 1.6 (\"Donut\")");
    case 5:
        return QLatin1String("Android 2.0 (\"Eclair\")");
    case 6:
        return QLatin1String("Android 2.0.1 (\"Eclair\")");
    case 7:
        return QLatin1String("Android 2.1 (\"Eclair\")");
    case 8:
        return QLatin1String("Android 2.2 (\"Froyo\")");
    case 9:
        return QLatin1String("Android 2.3 (\"Gingerbread\")");
    case 10:
        return QLatin1String("Android 2.3.3 (\"Gingerbread\")");
    case 11:
        return QLatin1String("Android 3.0 (\"Honeycomb\")");
    case 12:
        return QLatin1String("Android 3.1 (\"Honeycomb\")");
    case 13:
        return QLatin1String("Android 3.2 (\"Honeycomb\")");
    case 14:
        return QLatin1String("Android 4.0 (\"IceCreamSandwich\")");
    case 15:
        return QLatin1String("Android 4.0.3 (\"IceCreamSandwich\")");
    case 16:
        return QLatin1String("Android 4.1 (\"Jelly Bean\")");
    case 17:
        return QLatin1String("Android 4.2 (\"Jelly Bean\")");
    case 18:
        return QLatin1String("Android 4.3 (\"Jelly Bean\")");
    case 19:
        return QLatin1String("Android 4.4 (\"KitKat\")");
    case 20:
        return QLatin1String("Android 4.4W (\"KitKat Wear\")");
    case 21:
        return QLatin1String("Android 5.0 (\"Lollipop\")");
    case 22:
        return QLatin1String("Android 5.1 (\"Lollipop\")");
    case 23:
        return QLatin1String("Android 6.0 (\"Marshmallow\")");
    case 24:
        return QLatin1String("Android 7.0 (\"Nougat\")");
    case 25:
        return QLatin1String("Android 7.1.1 (\"Nougat\")");
    case 26:
        return QLatin1String("Android 8.0 (\"Oreo\")");
    case 27:
        return QLatin1String("Android 8.1 (\"Oreo\")");
    case 28:
        return QLatin1String("Android 9.0 (\"Pie\")");
    case 29:
        return QLatin1String("Android 10.0 (\"Q\")");
    case 30:
        return QLatin1String("Android 11.0 (\"R\")");
    case 31:
        return QLatin1String("Android 12.0 (\"S\")");
    case 32:
        return QLatin1String("Android 12L (\"Sv2\")");
    case 33:
        return QLatin1String("Android 13.0 (\"Tiramisu\")");
    case 34:
        return QLatin1String("Android 14.0 (\"UpsideDownCake\")");
    default:
        return Tr::tr("Unknown Android version. API Level: %1").arg(x);
    }
}

void installQASIPackage(Target *target, const FilePath &packagePath)
{
    const QStringList appAbis = AndroidManager::applicationAbis(target);
    if (appAbis.isEmpty())
        return;
    const IDevice::ConstPtr device = DeviceKitAspect::device(target->kit());
    AndroidDeviceInfo info = AndroidDevice::androidDeviceInfoFromIDevice(device.get());
    if (!info.isValid()) // aborted
        return;

    QString deviceSerialNumber = info.serialNumber;
    if (info.type == IDevice::Emulator) {
        deviceSerialNumber = AndroidAvdManager::startAvd(info.avdName);
        if (deviceSerialNumber.isEmpty())
            MessageManager::writeDisrupting(Tr::tr("Starting Android virtual device failed."));
    }

    QStringList arguments = AndroidDeviceInfo::adbSelector(deviceSerialNumber);
    arguments << "install" << "-r " << packagePath.path();
    QString error;
    Process *process = startAdbProcess(arguments, &error);
    if (process) {
        // TODO: Potential leak when the process is still running on Creator shutdown.
        QObject::connect(process, &Process::done, process, &QObject::deleteLater);
    } else {
        MessageManager::writeDisrupting(
            Tr::tr("Android package installation failed.\n%1").arg(error));
    }
}

bool checkKeystorePassword(const FilePath &keystorePath, const QString &keystorePasswd)
{
    if (keystorePasswd.isEmpty())
        return false;
    const CommandLine cmd(AndroidConfig::keytoolPath(),
                          {"-list", "-keystore", keystorePath.toUserOutput(),
                           "--storepass", keystorePasswd});
    Process proc;
    proc.setCommand(cmd);
    proc.runBlocking(10s);
    return proc.result() == ProcessResult::FinishedWithSuccess;
}

bool checkCertificatePassword(const FilePath &keystorePath, const QString &keystorePasswd,
                              const QString &alias, const QString &certificatePasswd)
{
    // assumes that the keystore password is correct
    QStringList arguments = {"-certreq", "-keystore", keystorePath.toUserOutput(),
                             "--storepass", keystorePasswd, "-alias", alias, "-keypass"};
    if (certificatePasswd.isEmpty())
        arguments << keystorePasswd;
    else
        arguments << certificatePasswd;

    Process proc;
    proc.setCommand({AndroidConfig::keytoolPath(), arguments});
    proc.runBlocking(10s);
    return proc.result() == ProcessResult::FinishedWithSuccess;
}

bool checkCertificateExists(const FilePath &keystorePath, const QString &keystorePasswd,
                            const QString &alias)
{
    // assumes that the keystore password is correct
    const QStringList arguments = {"-list", "-keystore", keystorePath.toUserOutput(),
                                   "--storepass", keystorePasswd, "-alias", alias};

    Process proc;
    proc.setCommand({AndroidConfig::keytoolPath(), arguments});
    proc.runBlocking(10s);
    return proc.result() == ProcessResult::FinishedWithSuccess;
}

Process *startAdbProcess(const QStringList &args, QString *err)
{
    std::unique_ptr<Process> process(new Process);
    const FilePath adb = AndroidConfig::adbToolPath();
    const CommandLine command{adb, args};
    qCDebug(androidManagerLog).noquote() << "Running command (async):" << command.toUserOutput();
    process->setCommand(command);
    process->start();
    if (process->waitForStarted(500ms) && process->state() == QProcess::Running)
        return process.release();

    const QString errorStr = process->readAllStandardError();
    qCDebug(androidManagerLog).noquote() << "Running command (async) failed:"
                                         << command.toUserOutput() << "Output:" << errorStr;
    if (err)
        *err = errorStr;
    return nullptr;
}

static SdkToolResult runCommand(const CommandLine &command, const QByteArray &writeData,
                                int timeoutS)
{
    Android::SdkToolResult cmdResult;
    Process cmdProc;
    cmdProc.setWriteData(writeData);
    qCDebug(androidManagerLog) << "Running command (sync):" << command.toUserOutput();
    cmdProc.setCommand(command);
    cmdProc.runBlocking(std::chrono::seconds(timeoutS), EventLoopMode::On);
    cmdResult.m_stdOut = cmdProc.cleanedStdOut().trimmed();
    cmdResult.m_stdErr = cmdProc.cleanedStdErr().trimmed();
    cmdResult.m_success = cmdProc.result() == ProcessResult::FinishedWithSuccess;
    qCDebug(androidManagerLog) << "Command finshed (sync):" << command.toUserOutput()
                               << "Success:" << cmdResult.m_success
                               << "Output:" << cmdProc.allRawOutput();
    if (!cmdResult.success())
        cmdResult.m_exitMessage = cmdProc.exitMessage();
    return cmdResult;
}

SdkToolResult runAdbCommand(const QStringList &args, const QByteArray &writeData, int timeoutS)
{
    return runCommand({AndroidConfig::adbToolPath(), args}, writeData, timeoutS);
}

} // namespace Android::AndroidManager
