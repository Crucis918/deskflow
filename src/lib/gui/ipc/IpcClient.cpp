/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Symless Ltd.
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcClient.h"

#include "common/VersionInfo.h"

#include <QDebug>
#include <QLocalSocket>
#include <QTimer>

namespace deskflow::gui::ipc {

const auto kTimeout = 1000;
const auto kRetryLimit = 3;

IpcClient::IpcClient(QObject *parent, const QString &socketName, const QString &typeName)
    : QObject(parent),
      m_socket{new QLocalSocket(this)},
      m_socketName(socketName), // NOSONAR - Qt memory
      m_typeName(typeName)
{
  connect(m_socket, &QLocalSocket::disconnected, this, &IpcClient::handleDisconnected);
  connect(m_socket, &QLocalSocket::errorOccurred, this, &IpcClient::handleErrorOccurred);
  connect(m_socket, &QLocalSocket::readyRead, this, &IpcClient::handleReadyRead);
}

void IpcClient::connectToServer()
{
  if (m_state == State::Connecting) {
    qWarning().noquote() << m_typeName << "ipc client already connecting to server";
    return;
  }

  if (m_state != State::Unconnected) {
    qDebug().noquote() << m_typeName << "ipc client not in unconnected state, disconnecting";
    disconnectFromServer();
  }

  if (m_socket->state() != QLocalSocket::UnconnectedState) {
    qWarning().noquote() << m_typeName << "ipc client socket not in unconnected state, disconnecting";
    disconnectFromServer();
  }

  m_retryCount = 0;
  attemptConnection();
}

void IpcClient::attemptConnection()
{
  if (m_retryCount >= kRetryLimit) {
    qWarning().noquote() << m_typeName << "ipc client failed to connect after" << kRetryLimit << "attempts";
    m_state = State::Unconnected;
    Q_EMIT connectionFailed();
    return;
  }

  if (m_retryCount == 0) {
    qDebug().noquote() << m_typeName << "ipc client connecting to server:" << m_socketName;
  } else {
    qDebug().noquote() << m_typeName << "ipc client retrying connection, attempt:" << m_retryCount + 1;
  }

  m_state = State::Connecting;
  m_retryCount++;

  connect(
      m_socket, &QLocalSocket::connected, this,
      [this] {
        const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
        m_socket->write(QString("hello=%1\n").arg(versionId).toUtf8());
        qDebug().noquote() << m_typeName << "ipc client sent hello with version:" << versionId;
      },
      Qt::SingleShotConnection
  );

  connect(
      m_socket, &QLocalSocket::errorOccurred, this,
      [this] {
        qWarning().noquote() << m_typeName << "ipc client failed to connect:" << m_socket->errorString();
        m_socket->disconnectFromServer();
        m_state = State::Unconnected;
        QTimer::singleShot(0, this, &IpcClient::attemptConnection);
      },
      Qt::SingleShotConnection
  );

  m_socket->connectToServer(m_socketName);
}

void IpcClient::disconnectFromServer()
{
  m_state = State::Disconnecting;
  qDebug().noquote() << m_typeName << "ipc client disconnecting from server";
  m_socket->disconnectFromServer();
  m_state = State::Unconnected;
}

void IpcClient::handleDisconnected()
{
  if (m_state == State::Connecting) {
    return;
  }

  qDebug().noquote() << m_typeName << "ipc client disconnected from server";
  const auto wasConnected = m_state == State::Connected;
  m_state = State::Unconnected;

  if (wasConnected) {
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleErrorOccurred()
{
  if (m_state == State::Connecting) {
    return;
  }

  qWarning().noquote() << m_typeName << "ipc client error:" << m_socket->errorString();

  if (m_state == State::Connected) {
    disconnectFromServer();
    Q_EMIT connectionFailed();
  }
}

void IpcClient::handleReadyRead()
{
  QByteArray data = m_readBuffer + m_socket->readAll();
  m_readBuffer.clear();

  while (data.contains('\n')) {
    const auto index = data.indexOf('\n');
    const auto message = QString::fromUtf8(data.left(index));
    data.remove(0, index + 1);

    qDebug().noquote() << m_typeName << "ipc client message:" << message;
    const auto parts = message.split('=');
    if (parts.isEmpty()) {
      qWarning().noquote() << m_typeName << "ipc client got invalid message:" << message;
      continue;
    }

    if (m_state == State::Connecting) {
      if (parts[0] == "hello") {
        const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
        const auto serverVersion = parts.size() >= 2 ? parts[1] : QString();
        if (serverVersion != versionId) {
          qCritical().noquote() << m_typeName << "ipc version mismatch (client:" << versionId
                                << "server:" << serverVersion << ")";
          disconnectFromServer();
          Q_EMIT connectionFailed();
          continue;
        }

        m_state = State::Connected;
        qDebug().noquote() << m_typeName << "ipc client connected";
        Q_EMIT connected();
        continue;
      }
    }

    processCommand(parts[0], parts);
  }

  if (!data.isEmpty()) {
    m_readBuffer = data;
  }
}

void IpcClient::sendMessage(const QString &message)
{
  if (m_state != State::Connected) {
    qWarning().noquote() << m_typeName << "cannot send command, ipc client not connected";
    return;
  }

  m_socket->write(message.toUtf8() + "\n");
  qDebug().noquote() << m_typeName << "ipc client sent message:" << message;
}

} // namespace deskflow::gui::ipc
