/* Copyright (C) 2005-2007, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "ServerHandler.h"
#include "MainWindow.h"
#include "AudioInput.h"
#include "AudioOutput.h"
#include "Message.h"
#include "Player.h"
#include "Connection.h"
#include "Global.h"
#include "Database.h"

ServerHandlerMessageEvent::ServerHandlerMessageEvent(QByteArray &msg, bool udp) : QEvent(static_cast<QEvent::Type>(SERVERSEND_EVENT)) {
	qbaMsg = msg;
	bUdp = udp;
}

ServerHandler::ServerHandler()
{
	cConnection = NULL;
	qusUdp = NULL;

	// For some strange reason, on Win32, we have to call supportsSsl before the cipher list is ready.
	qWarning("OpenSSL Support: %d", QSslSocket::supportsSsl());

  QList<QSslCipher> pref;
  foreach(QSslCipher c, QSslSocket::defaultCiphers()) {
    if (c.usedBits() < 128)
      continue;
    pref << c;
  }
  if (pref.isEmpty())
    qFatal("No ciphers of at least 128 bit found");
  QSslSocket::setDefaultCiphers(pref);
}

ServerHandler::~ServerHandler()
{
	wait();
}

void ServerHandler::customEvent(QEvent *evt) {
	if (evt->type() != SERVERSEND_EVENT)
		return;

	ServerHandlerMessageEvent *shme=static_cast<ServerHandlerMessageEvent *>(evt);

	if (cConnection) {
		if (shme->qbaMsg.size() > 0) {
			if (shme->bUdp && ! g.s.bTCPCompat) {
				if (! qusUdp) {
					if (cConnection->peerAddress().isNull())
						return;
					qusUdp = new QUdpSocket(this);
					qusUdp->bind();
					connect(qusUdp, SIGNAL(readyRead()), this, SLOT(udpReady()));
#ifdef Q_OS_WIN
					int tos = 0xb8;
					if (setsockopt(qusUdp->socketDescriptor(), IPPROTO_IP, 3, reinterpret_cast<char *>(&tos), sizeof(tos)) != 0) {
						tos = 0x98;
						setsockopt(qusUdp->socketDescriptor(), IPPROTO_IP, 3, reinterpret_cast<char *>(&tos), sizeof(tos));
					}
#endif
					qhaRemote = cConnection->peerAddress();
				}
				qusUdp->writeDatagram(shme->qbaMsg, qhaRemote, iPort);
			} else {
				cConnection->sendMessage(shme->qbaMsg);
				if (shme->bUdp)
					cConnection->forceFlush();
			}
		} else
			cConnection->disconnect();
	}
}

void ServerHandler::udpReady() {
	while (qusUdp->hasPendingDatagrams()) {
		QByteArray qba;
		qba.resize(qusUdp->pendingDatagramSize());
		QHostAddress senderAddr;
		quint16 senderPort;
		qusUdp->readDatagram(qba.data(), qba.size(), &senderAddr, &senderPort);

		if (!(senderAddr == qhaRemote) || (senderPort != iPort))
			continue;

		Message *msg = Message::networkToMessage(qba);

		if (! msg)
			continue;
		if ((msg->messageType() != Message::Speex)) {
			delete msg;
			continue;
		}
        message(qba);
		delete msg;
	}
}

void ServerHandler::sendMessage(Message *mMsg, bool forceTCP)
{
	QByteArray qbaBuffer;
	mMsg->sPlayerId = g.sId;
	mMsg->messageToNetwork(qbaBuffer);
	bool mayUdp = !forceTCP && g.sId && ((mMsg->messageType() == Message::Speex) || (mMsg->messageType() == Message::Ping));

	ServerHandlerMessageEvent *shme=new ServerHandlerMessageEvent(qbaBuffer, mayUdp);
	QApplication::postEvent(this, shme);
}

void ServerHandler::run()
{
	QSslSocket *qtsSock = new QSslSocket(this);
	cConnection = new Connection(this, qtsSock);
	qusUdp = NULL;

	qlErrors.clear();
	qscCert = QSslCertificate();

	connect(qtsSock, SIGNAL(encrypted()), this, SLOT(serverConnectionConnected()));
	connect(cConnection, SIGNAL(connectionClosed(QString)), this, SLOT(serverConnectionClosed(QString)));
	connect(cConnection, SIGNAL(message(QByteArray &)), this, SLOT(message(QByteArray &)));
	connect(cConnection, SIGNAL(handleSslErrors(const QList<QSslError> &)), this, SLOT(setSslErrors(const QList<QSslError> &)));
	qtsSock->connectToHostEncrypted(qsHostName, iPort);

	QTimer *ticker = new QTimer(this);
	connect(ticker, SIGNAL(timeout()), this, SLOT(sendPing()));
	ticker->start(10000);

	g.mw->rtLast = MessageServerReject::None;

	exec();

	ticker->stop();
	cConnection->disconnect();
	delete cConnection;
	cConnection = NULL;
	if (qusUdp) {
		delete qusUdp;
		qusUdp = NULL;
	}
}

void ServerHandler::setSslErrors(const QList<QSslError> &errors) {
    	qscCert = cConnection->peerCertificate();
    	if (QString::fromLatin1(qscCert.digest(QCryptographicHash::Sha1).toHex()) == Database::getDigest(qsHostName, iPort))
    		cConnection->proceedAnyway();
    	else
	    	qlErrors = errors;
}

void ServerHandler::sendPing() {
	MessagePing mp;
	sendMessage(&mp, true);
	sendMessage(&mp, false);
}

void ServerHandler::message(QByteArray &qbaMsg) {
	Message *mMsg = Message::networkToMessage(qbaMsg);
	if (! mMsg)
		return;

	Player *p = Player::get(mMsg->sPlayerId);

	AudioOutputPtr ao = g.ao;

	if (mMsg->messageType() == Message::Speex) {
		if (ao) {
			if (p) {
				MessageSpeex *msMsg=static_cast<MessageSpeex *>(mMsg);
				if (! p->bLocalMute)
					ao->addFrameToBuffer(p, msMsg->qbaSpeexPacket, msMsg->iSeq);
			} else {
				// Eek, we just got a late packet for a player already removed. Remove
				// the buffer and pretend this never happened.
				// If ~AudioOutputPlayer or decendants uses the Player object now,
				// Bad Things happen.
				ao->removeBuffer(p);
			}
		}
	} else {
		if(mMsg->messageType() == Message::ServerLeave) {
			if (ao)
				ao->removeBuffer(p);
		}
		ServerHandlerMessageEvent *shme=new ServerHandlerMessageEvent(qbaMsg, false);
		QApplication::postEvent(g.mw, shme);
	}

	delete mMsg;
}

void ServerHandler::disconnect() {
	// Actual TCP object is in a different thread, so signal it
	QByteArray qbaBuffer;
	ServerHandlerMessageEvent *shme=new ServerHandlerMessageEvent(qbaBuffer, false);
	QApplication::postEvent(this, shme);
}

void ServerHandler::serverConnectionClosed(QString reason) {
	AudioOutputPtr ao = g.ao;
	if (ao)
			ao->wipe();
	emit disconnected(reason);
	exit(0);
}

void ServerHandler::serverConnectionConnected() {
	AudioInputPtr ai = g.ai;
	MessageServerAuthenticate msaMsg;
	msaMsg.qsUsername = qsUserName;
	msaMsg.qsPassword = qsPassword;
	if (ai)
		msaMsg.iMaxBandwidth = ai->getMaxBandwidth();
	else
		msaMsg.iMaxBandwidth = 0;

	cConnection->sendMessage(&msaMsg);
	emit connected();
}

void ServerHandler::setConnectionInfo(const QString &host, int port, const QString &username, const QString &pw) {
	qsHostName = host;
	iPort = port;
	qsUserName = username;
	qsPassword = pw;
}

void ServerHandler::getConnectionInfo(QString &host, int &port, QString &username, QString &pw) {
	host = qsHostName;
	port = iPort;
	username = qsUserName;
	pw = qsPassword;
}
