/***************************************************************************
 *   Copyright (C) 2005-08 by the Quassel IRC Team                         *
 *   devel@quassel-irc.org                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3.                                           *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "clientsyncer.h"

#ifndef QT_NO_NETWORKPROXY
#  include <QNetworkProxy>
#endif

#include "client.h"
#include "global.h"
#include "identity.h"
#include "ircuser.h"
#include "ircchannel.h"
#include "network.h"
#include "signalproxy.h"


ClientSyncer::ClientSyncer(QObject *parent)
  : QObject(parent)
{
  socket = 0;
  blockSize = 0;

  connect(Client::signalProxy(), SIGNAL(disconnected()), this, SLOT(coreSocketDisconnected()));
}

ClientSyncer::~ClientSyncer() {
}

void ClientSyncer::coreHasData() {
  QVariant item;
  while(SignalProxy::readDataFromDevice(socket, blockSize, item)) {
    emit recvPartialItem(1,1);
    QVariantMap msg = item.toMap();
    if(!msg.contains("MsgType")) {
      // This core is way too old and does not even speak our init protocol...
      emit connectionError(tr("The Quassel Core you try to connect to is too old! Please consider upgrading."));
      disconnectFromCore();
      return;
    }
    if(msg["MsgType"] == "ClientInitAck") {
      clientInitAck(msg);
    } else if(msg["MsgType"] == "ClientInitReject") {
      emit connectionError(msg["Error"].toString());
      disconnectFromCore();
      return;
    } else if(msg["MsgType"] == "CoreSetupAck") {
      emit coreSetupSuccess();
    } else if(msg["MsgType"] == "CoreSetupReject") {
      emit coreSetupFailed(msg["Error"].toString());
    } else if(msg["MsgType"] == "ClientLoginReject") {
      emit loginFailed(msg["Error"].toString());
    } else if(msg["MsgType"] == "ClientLoginAck") {
      // prevent multiple signal connections
      disconnect(this, SIGNAL(recvPartialItem(quint32, quint32)), this, SIGNAL(sessionProgress(quint32, quint32)));
      connect(this, SIGNAL(recvPartialItem(quint32, quint32)), this, SIGNAL(sessionProgress(quint32, quint32)));
      emit loginSuccess();
    } else if(msg["MsgType"] == "SessionInit") {
      sessionStateReceived(msg["SessionState"].toMap());
      break; // this is definitively the last message we process here!
    } else {
      emit connectionError(tr("<b>Invalid data received from core!</b><br>Disconnecting."));
      disconnectFromCore();
      return;
    }
  }
  if(blockSize > 0) {
    emit recvPartialItem(socket->bytesAvailable(), blockSize);
  }
}

void ClientSyncer::coreSocketError(QAbstractSocket::SocketError) {
  qDebug() << "coreSocketError" << socket << socket->errorString();
  emit connectionError(socket->errorString());
  socket->deleteLater();
}

void ClientSyncer::disconnectFromCore() {
  if(socket) socket->close();
}

void ClientSyncer::connectToCore(const QVariantMap &conn) {
  // TODO implement SSL
  coreConnectionInfo = conn;
  //if(isConnected()) {
  //  emit coreConnectionError(tr("Already connected to Core!"));
  //  return;
  // }
  if(socket != 0) {
    socket->deleteLater();
    socket = 0;
  }
  if(conn["Host"].toString().isEmpty()) {
    emit connectionError(tr("Internal connections not yet supported."));
    return; // FIXME implement internal connections
    //clientMode = LocalCore;
    socket = new QBuffer(this);
    connect(socket, SIGNAL(readyRead()), this, SLOT(coreHasData()));
    socket->open(QIODevice::ReadWrite);
    //QVariant state = connectToLocalCore(coreConnectionInfo["User"].toString(), coreConnectionInfo["Password"].toString());
    //syncToCore(state);
    coreSocketConnected();
  } else {
    //clientMode = RemoteCore;
    //emit coreConnectionMsg(tr("Connecting..."));
    Q_ASSERT(!socket);

#ifndef QT_NO_OPENSSL
    QSslSocket *sock = new QSslSocket(Client::instance());
#else
    if(conn["useSsl"].toBool()) {
	emit connectionError(tr("<b>This client is built without SSL Support!</b><br />Disable the usage of SSL in the account settings."));
	emit encrypted(false);
	return;
    }
    QTcpSocket *sock = new QTcpSocket(Client::instance());
#endif
#ifndef QT_NO_NETWORKPROXY
    if(conn.contains("useProxy") && conn["useProxy"].toBool()) {
      QNetworkProxy proxy((QNetworkProxy::ProxyType)conn["proxyType"].toInt(), conn["proxyHost"].toString(), conn["proxyPort"].toUInt(), conn["proxyUser"].toString(), conn["proxyPassword"].toString());
      sock->setProxy(proxy);
    }
#endif
    socket = sock;
    connect(sock, SIGNAL(readyRead()), this, SLOT(coreHasData()));
    connect(sock, SIGNAL(connected()), this, SLOT(coreSocketConnected()));
    connect(sock, SIGNAL(disconnected()), this, SLOT(coreSocketDisconnected()));
    connect(sock, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(coreSocketError(QAbstractSocket::SocketError)));
    connect(sock, SIGNAL(stateChanged(QAbstractSocket::SocketState)), this, SIGNAL(socketStateChanged(QAbstractSocket::SocketState)));
    sock->connectToHost(conn["Host"].toString(), conn["Port"].toUInt());
  }
}

void ClientSyncer::coreSocketConnected() {
  //connect(this, SIGNAL(recvPartialItem(uint, uint)), this, SIGNAL(coreConnectionProgress(uint, uint)));
  // Phase One: Send client info and wait for core info

  //emit coreConnectionMsg(tr("Synchronizing to core..."));
  QVariantMap clientInit;
  clientInit["MsgType"] = "ClientInit";
  clientInit["ClientVersion"] = Global::quasselVersion;
  clientInit["ClientBuild"] = 860; // FIXME legacy!
  clientInit["ClientDate"] = Global::quasselBuildDate;
  clientInit["ProtocolVersion"] = Global::protocolVersion;
  clientInit["UseSsl"] = coreConnectionInfo["useSsl"];
  
  SignalProxy::writeDataToDevice(socket, clientInit);
}

void ClientSyncer::coreSocketDisconnected() {
  emit socketDisconnected();
  Client::instance()->disconnectFromCore();

  // FIXME handle disconnects gracefully in here as well!

  coreConnectionInfo.clear();
  netsToSync.clear();
  channelsToSync.clear();
  usersToSync.clear();
  blockSize = 0;
  //restartPhaseNull();
}

void ClientSyncer::clientInitAck(const QVariantMap &msg) {
  // Core has accepted our version info and sent its own. Let's see if we accept it as well...
  uint ver = 0;
  if(!msg.contains("ProtocolVersion") && msg["CoreBuild"].toUInt() >= 732) ver = 1; // legacy!
  if(msg.contains("ProtocolVersion")) ver = msg["ProtocolVersion"].toUInt();
  if(ver < Global::clientNeedsProtocol) {
    emit connectionError(tr("<b>The Quassel Core you are trying to connect to is too old!</b><br>"
        "Need at least core/client protocol v%1 to connect.").arg(Global::clientNeedsProtocol));
    disconnectFromCore();
    return;
  }
  emit connectionMsg(msg["CoreInfo"].toString());

#ifndef QT_NO_OPENSSL
  if(coreConnectionInfo["useSsl"].toBool()) {
    if(msg["SupportSsl"].toBool()) {
      QSslSocket *sslSocket = qobject_cast<QSslSocket *>(socket);
      Q_ASSERT(sslSocket);
      connect(sslSocket, SIGNAL(sslErrors(const QList<QSslError> &)), this, SLOT(sslErrors(const QList<QSslError> &)));
      sslSocket->startClientEncryption();
      emit encrypted(true);
      Client::instance()->setSecuredConnection();
    } else {
      emit connectionError(tr("<b>The Quassel Core you are trying to connect to does not support SSL!</b><br />If you want to connect anyways, disable the usage of SSL in the account settings."));
      emit encrypted(false);
      disconnectFromCore();
      return;
    }
  }
#endif

  if(!msg["Configured"].toBool()) {
    // start wizard
    emit startCoreSetup(msg["StorageBackends"].toList());
  } else if(msg["LoginEnabled"].toBool()) {
    emit startLogin();
  }
}

void ClientSyncer::doCoreSetup(const QVariant &setupData) {
  QVariantMap setup;
  setup["MsgType"] = "CoreSetupData";
  setup["SetupData"] = setupData;
  SignalProxy::writeDataToDevice(socket, setup);
}

void ClientSyncer::loginToCore(const QString &user, const QString &passwd) {
  emit connectionMsg(tr("Logging in..."));
  QVariantMap clientLogin;
  clientLogin["MsgType"] = "ClientLogin";
  clientLogin["User"] = user;
  clientLogin["Password"] = passwd;
  SignalProxy::writeDataToDevice(socket, clientLogin);
}

void ClientSyncer::sessionStateReceived(const QVariantMap &state) {
  emit sessionProgress(1, 1);
  disconnect(this, SIGNAL(recvPartialItem(quint32, quint32)), this, SIGNAL(sessionProgress(quint32, quint32)));
  disconnect(socket, 0, this, 0);  // rest of communication happens through SignalProxy
  //Client::signalProxy()->addPeer(socket);
  Client::instance()->setConnectedToCore(socket, coreConnectionInfo["AccountId"].value<AccountId>());
  syncToCore(state);
}

void ClientSyncer::syncToCore(const QVariantMap &sessionState) {

  // create identities
  foreach(QVariant vid, sessionState["Identities"].toList()) {
    Client::instance()->coreIdentityCreated(vid.value<Identity>());
  }

  // create buffers
  // FIXME: get rid of this crap
  QVariantList bufferinfos = sessionState["BufferInfos"].toList();
  foreach(QVariant vinfo, bufferinfos) Client::buffer(vinfo.value<BufferInfo>());  // create Buffers and BufferItems

  QVariantList networkids = sessionState["NetworkIds"].toList();

  // prepare sync progress thingys... FIXME: Care about removal of networks
  numNetsToSync = networkids.count();
  numChannelsToSync = 0; //sessionState["IrcChannelCount"].toUInt();
  numUsersToSync = 0; // sessionState["IrcUserCount"].toUInt(); qDebug() << numUsersToSync;
  emit networksProgress(0, numNetsToSync);
  emit channelsProgress(0, numChannelsToSync);
  emit ircUsersProgress(0, numUsersToSync);

  // create network objects
  foreach(QVariant networkid, networkids) {
    NetworkId netid = networkid.value<NetworkId>();
    Network *net = new Network(netid, Client::instance());
    netsToSync.insert(net);
    connect(net, SIGNAL(initDone()), this, SLOT(networkInitDone()));
    connect(net, SIGNAL(ircUserInitDone(IrcUser *)), this, SLOT(ircUserInitDone(IrcUser *)));
    connect(net, SIGNAL(ircUserAdded(IrcUser *)), this, SLOT(ircUserAdded(IrcUser *)));
    connect(net, SIGNAL(ircUserRemoved(QObject *)), this, SLOT(ircUserRemoved(QObject *)));
    connect(net, SIGNAL(ircChannelInitDone(IrcChannel *)), this, SLOT(ircChannelInitDone(IrcChannel *)));
    connect(net, SIGNAL(ircChannelAdded(IrcChannel *)), this, SLOT(ircChannelAdded(IrcChannel *)));
    connect(net, SIGNAL(ircChannelRemoved(QObject *)), this, SLOT(ircChannelRemoved(QObject *)));
    Client::addNetwork(net);
  }
  checkSyncState();
}

void ClientSyncer::networkInitDone() {
  netsToSync.remove(sender());
  emit networksProgress(numNetsToSync - netsToSync.count(), numNetsToSync);
  checkSyncState();
}

void ClientSyncer::ircChannelInitDone(IrcChannel *chan) {
  channelsToSync.remove(chan);
  emit channelsProgress(numChannelsToSync - channelsToSync.count(), numChannelsToSync);
  checkSyncState();
}

void ClientSyncer::ircChannelAdded(IrcChannel *chan) {
  if(!chan->isInitialized()) {
    channelsToSync.insert(chan);
    numChannelsToSync++;
    emit channelsProgress(numChannelsToSync - channelsToSync.count(), numChannelsToSync);
    checkSyncState();
  }
}

void ClientSyncer::ircChannelRemoved(QObject *chan) {
  if(channelsToSync.contains(chan)) {
    numChannelsToSync--;
    channelsToSync.remove(chan);
    emit channelsProgress(numChannelsToSync - channelsToSync.count(), numChannelsToSync);
    checkSyncState();
  }
}

void ClientSyncer::ircUserInitDone(IrcUser *user) {
  usersToSync.remove(user);
  emit ircUsersProgress(numUsersToSync - usersToSync.count(), numUsersToSync);
  checkSyncState();
}

void ClientSyncer::ircUserAdded(IrcUser *user) {
  if(!user->isInitialized()) {
    usersToSync.insert(user);
    numUsersToSync++;
    emit ircUsersProgress(numUsersToSync - usersToSync.count(), numUsersToSync);
    checkSyncState();
  }
}

void ClientSyncer::ircUserRemoved(QObject *user) {
  if(usersToSync.contains(user)) {
    numUsersToSync--;
    usersToSync.remove(user);
    emit ircUsersProgress(numUsersToSync - usersToSync.count(), numUsersToSync);
    checkSyncState();
  }
}

void ClientSyncer::checkSyncState() {
  if(usersToSync.count() + channelsToSync.count() + netsToSync.count() == 0) {
    // done syncing!
    /*
    qDebug() << "done";
    foreach(Network *net, _networks.values()) {
      //disconnect(net, 0, this, SLOT(networkInitDone()));
      //disconnect(net, 0, this, SLOT(ircUserInitDone(IrcUser *)));
      //disconnect(net, 0, this, SLOT(ircUserAdded(IrcUser *)));
      //disconnect(net, 0, this, SLOT(ircUserRemoved(QObject *)));
      //disconnect(net, 0, this, SLOT(ircChannelInitDone(IrcChannel *)));
      //disconnect(net, 0, this, SLOT(ircChannelAdded(IrcChannel *)));
      //disconnect(net, 0, this, SLOT(ircChannelRemoved(QObject *)));
      qDebug() << "disconnecting";
      disconnect(net, SIGNAL(initDone()), this, SLOT(networkInitDone()));
      disconnect(net, SIGNAL(ircUserInitDone(IrcUser *)), this, SLOT(ircUserInitDone(IrcUser *)));
      disconnect(net, SIGNAL(ircUserAdded(IrcUser *)), this, SLOT(ircUserAdded(IrcUser *)));
      disconnect(net, SIGNAL(ircUserRemoved(QObject *)), this, SLOT(ircUserRemoved(QObject *)));
      disconnect(net, SIGNAL(ircChannelInitDone(IrcChannel *)), this, SLOT(ircChannelInitDone(IrcChannel *)));
      disconnect(net, SIGNAL(ircChannelAdded(IrcChannel *)), this, SLOT(ircChannelAdded(IrcChannel *)));
      disconnect(net, SIGNAL(ircChannelRemoved(QObject *)), this, SLOT(ircChannelRemoved(QObject *)));
    }
    */

    Client::instance()->setSyncedToCore();
    emit syncFinished();
    //emit connected();
    //emit connectionStateChanged(true);

  }
}

#ifndef QT_NO_OPENSSL
void ClientSyncer::sslErrors(const QList<QSslError> &errors) {
  qDebug() << "SSL Errors:";
  foreach(QSslError err, errors)
    qDebug() << "  " << err;

  QSslSocket *socket = qobject_cast<QSslSocket *>(sender());
  if(socket)
    socket->ignoreSslErrors();
}
#endif
