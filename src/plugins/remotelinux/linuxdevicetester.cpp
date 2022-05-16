/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "linuxdevicetester.h"

#include "filetransfer.h"
#include "remotelinux_constants.h"

#include <projectexplorer/devicesupport/deviceusedportsgatherer.h>
#include <ssh/sshconnection.h>
#include <ssh/sshconnectionmanager.h>
#include <utils/algorithm.h>
#include <utils/port.h>
#include <utils/processinterface.h>
#include <utils/qtcassert.h>
#include <utils/qtcprocess.h>

using namespace ProjectExplorer;
using namespace QSsh;
using namespace Utils;

namespace RemoteLinux {
namespace Internal {
namespace {

enum State { Inactive, Connecting, RunningUname, TestingPorts, TestingSftp, TestingRsync };

} // anonymous namespace

class GenericLinuxDeviceTesterPrivate
{
public:
    IDevice::Ptr device;
    SshConnection *connection = nullptr;
    QtcProcess unameProcess;
    DeviceUsedPortsGatherer portsGatherer;
    FileTransfer fileTransfer;
    State state = Inactive;
    bool sftpWorks = false;
};

} // namespace Internal

using namespace Internal;

GenericLinuxDeviceTester::GenericLinuxDeviceTester(QObject *parent)
    : DeviceTester(parent), d(new GenericLinuxDeviceTesterPrivate)
{
    connect(&d->unameProcess, &QtcProcess::done, this,
            &GenericLinuxDeviceTester::handleUnameDone);
    connect(&d->portsGatherer, &DeviceUsedPortsGatherer::error,
            this, &GenericLinuxDeviceTester::handlePortsGathererError);
    connect(&d->portsGatherer, &DeviceUsedPortsGatherer::portListReady,
            this, &GenericLinuxDeviceTester::handlePortsGathererDone);
    connect(&d->fileTransfer, &FileTransfer::done,
            this, &GenericLinuxDeviceTester::handleFileTransferDone);
}

GenericLinuxDeviceTester::~GenericLinuxDeviceTester()
{
    if (d->connection)
        SshConnectionManager::releaseConnection(d->connection);
}

void GenericLinuxDeviceTester::testDevice(const IDevice::Ptr &deviceConfiguration)
{
    QTC_ASSERT(d->state == Inactive, return);

    d->device = deviceConfiguration;
    d->fileTransfer.setDevice(d->device);
    SshConnectionManager::forceNewConnection(deviceConfiguration->sshParameters());
    d->connection = SshConnectionManager::acquireConnection(deviceConfiguration->sshParameters());
    connect(d->connection, &SshConnection::connected,
            this, &GenericLinuxDeviceTester::handleConnected);
    connect(d->connection, &SshConnection::errorOccurred,
            this, &GenericLinuxDeviceTester::handleConnectionFailure);

    emit progressMessage(tr("Connecting to device..."));
    d->state = Connecting;
    d->connection->connectToHost();
}

void GenericLinuxDeviceTester::stopTest()
{
    QTC_ASSERT(d->state != Inactive, return);

    switch (d->state) {
    case Connecting:
        d->connection->disconnectFromHost();
        break;
    case TestingPorts:
        d->portsGatherer.stop();
        break;
    case RunningUname:
        d->unameProcess.close();
        break;
    case TestingSftp:
    case TestingRsync:
        d->fileTransfer.stop();
        break;
    case Inactive:
        break;
    }

    setFinished(TestFailure);
}

void GenericLinuxDeviceTester::handleConnectionFailure()
{
    QTC_ASSERT(d->state != Inactive, return);

    emit errorMessage(d->connection->errorString() + QLatin1Char('\n'));

    setFinished(TestFailure);
}

void GenericLinuxDeviceTester::handleConnected()
{
    QTC_ASSERT(d->state == Connecting, return);
    emit progressMessage(tr("Connection to device established.") + QLatin1Char('\n'));

    testUname();
}

void GenericLinuxDeviceTester::testUname()
{
    d->state = RunningUname;
    emit progressMessage(tr("Checking kernel version..."));

    d->unameProcess.setCommand({d->device->filePath("uname"), {"-rsm"}});
    d->unameProcess.start();
}

void GenericLinuxDeviceTester::handleUnameDone()
{
    QTC_ASSERT(d->state == RunningUname, return);

    if (!d->unameProcess.errorString().isEmpty() || d->unameProcess.exitCode() != 0) {
        const QByteArray stderrOutput = d->unameProcess.readAllStandardError();
        if (!stderrOutput.isEmpty())
            emit errorMessage(tr("uname failed: %1").arg(QString::fromUtf8(stderrOutput)) + QLatin1Char('\n'));
        else
            emit errorMessage(tr("uname failed.") + QLatin1Char('\n'));
    } else {
        emit progressMessage(QString::fromUtf8(d->unameProcess.readAllStandardOutput()));
    }

    testPortsGatherer();
}

void GenericLinuxDeviceTester::testPortsGatherer()
{
    d->state = TestingPorts;
    emit progressMessage(tr("Checking if specified ports are available..."));

    d->portsGatherer.start(d->device);
}

void GenericLinuxDeviceTester::handlePortsGathererError(const QString &message)
{
    QTC_ASSERT(d->state == TestingPorts, return);

    emit errorMessage(tr("Error gathering ports: %1").arg(message) + QLatin1Char('\n'));
    setFinished(TestFailure);
}

void GenericLinuxDeviceTester::handlePortsGathererDone()
{
    QTC_ASSERT(d->state == TestingPorts, return);

    if (d->portsGatherer.usedPorts().isEmpty()) {
        emit progressMessage(tr("All specified ports are available.") + QLatin1Char('\n'));
    } else {
        const QString portList = transform(d->portsGatherer.usedPorts(), [](const Port &port) {
            return QString::number(port.number());
        }).join(", ");
        emit errorMessage(tr("The following specified ports are currently in use: %1")
            .arg(portList) + QLatin1Char('\n'));
    }

    testFileTransfer(FileTransferMethod::Sftp);
}

void GenericLinuxDeviceTester::testFileTransfer(FileTransferMethod method)
{
    switch (method) {
    case FileTransferMethod::Sftp:  d->state = TestingSftp;  break;
    case FileTransferMethod::Rsync: d->state = TestingRsync; break;
    }
    emit progressMessage(tr("Checking whether \"%1\" works...")
                         .arg(FileTransfer::transferMethodName(method)));

    d->fileTransfer.setTransferMethod(method);
    d->fileTransfer.test();
}

void GenericLinuxDeviceTester::handleFileTransferDone(const ProcessResultData &resultData)
{
    QTC_ASSERT(d->state == TestingSftp || d->state == TestingRsync, return);

    bool succeeded = false;
    QString error;
    const QString methodName = FileTransfer::transferMethodName(d->fileTransfer.transferMethod());
    if (resultData.m_error == QProcess::FailedToStart) {
        error = tr("Failed to start \"%1\": %2\n").arg(methodName, resultData.m_errorString);
    } else if (resultData.m_exitStatus == QProcess::CrashExit) {
        error = tr("\"%1\" crashed.\n").arg(methodName);
    } else if (resultData.m_exitCode != 0) {
        error = tr("\"%1\" failed with exit code %2: %3\n")
                .arg(methodName).arg(resultData.m_exitCode).arg(resultData.m_errorString);
    } else {
        succeeded = true;
    }

    if (succeeded)
        emit progressMessage(tr("\"%1\" is functional.\n").arg(methodName));
    else
        emit errorMessage(error);

    if (d->state == TestingSftp) {
        d->sftpWorks = succeeded;
        testFileTransfer(FileTransferMethod::Rsync);
    } else {
        if (!succeeded) {
            if (d->sftpWorks) {
                emit progressMessage(tr("SFTP will be used for deployment, because rsync "
                                        "is not available.\n"));
            } else {
                emit errorMessage(tr("Deployment to this device will not work out of the box.\n"));
            }
        }
        d->device->setExtraData(Constants::SupportsRSync, succeeded);
        setFinished(d->sftpWorks || succeeded ? TestSuccess : TestFailure);
    }
}

void GenericLinuxDeviceTester::setFinished(TestResult result)
{
    d->state = Inactive;
    if (d->connection) {
        disconnect(d->connection, nullptr, this, nullptr);
        SshConnectionManager::releaseConnection(d->connection);
        d->connection = nullptr;
    }
    emit finished(result);
}

} // namespace RemoteLinux
