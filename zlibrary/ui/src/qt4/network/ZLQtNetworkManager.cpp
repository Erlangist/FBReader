/*
 * Copyright (C) 2004-2012 Geometer Plus <contact@geometerplus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <QtCore/QVector>
#include <QtCore/QStringList>
#include <QtCore/QFile>
#include <QtCore/QEventLoop>
#include <QtCore/QDir>
#include <QtCore/QList>
#include <QtCore/QTimer>

#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkProxy>
#include <QtNetwork/QSslConfiguration>
#include <QtNetwork/QSslCertificate>
#include <QtNetwork/QAuthenticator>

#include <QtCore/QDebug>

#include <ZLStringUtil.h>
#include <ZLLogger.h>
#include <ZLResource.h>

#include "ZLQtNetworkManager.h"

static QString fixPath(const QString &path) {
	if (path.startsWith('~')) {
		return QDir::homePath() + path.mid(1);
	}
	return path;
}

ZLQtNetworkManager::ZLQtNetworkManager() {
	myCache = new QNetworkDiskCache(&myManager);
	myManager.setCache(myCache);
	myCookieJar = new ZLQtNetworkCookieJar(&myManager);
	myManager.setCookieJar(myCookieJar);
	QObject::connect(&myManager, SIGNAL(authenticationRequired(QNetworkReply*,QAuthenticator*)),
					 this, SLOT(onAuthenticationRequired(QNetworkReply*,QAuthenticator*)));
	QObject::connect(&myManager, SIGNAL(finished(QNetworkReply*)),
					 this, SLOT(onFinished(QNetworkReply*)));
}

ZLQtNetworkManager::~ZLQtNetworkManager() {
}

void ZLQtNetworkManager::createInstance() {
	ourInstance = new ZLQtNetworkManager();
}

void ZLQtNetworkManager::initPaths() {
	myCookieJar->setFilePath(fixPath(QString::fromStdString(CookiesPath())));
	QDir cacheDirectory = fixPath(QString::fromStdString(CacheDirectory()));
	if (!cacheDirectory.exists()) {
		cacheDirectory.mkpath(cacheDirectory.absolutePath());
	}
	myCache->setCacheDirectory(cacheDirectory.absolutePath());
}


std::string ZLQtNetworkManager::perform(const ZLExecutionData::Vector &dataList) const {
	if (useProxy()) {
		QString host = QString::fromStdString(proxyHost());
		QNetworkProxy proxy(QNetworkProxy::HttpProxy, host, atoi(proxyPort().c_str()));
		const_cast<QNetworkAccessManager&>(myManager).setProxy(proxy);
	}
	QList<QNetworkReply*> replies;
	QStringList errors;
	QEventLoop eventLoop;

	foreach (const shared_ptr<ZLExecutionData> &data, dataList) {
		if (data.isNull() || !data->isInstanceOf(ZLNetworkRequest::TYPE_ID)) {
			continue;
		}
		ZLNetworkRequest &request = static_cast<ZLNetworkRequest&>(*data);
		QNetworkRequest networkRequest;
		ZLLogger::Instance().println("network", "requesting " + request.url());
		networkRequest.setUrl(QUrl::fromUserInput(QString::fromStdString(request.url())));

		if (!request.doBefore()) {
			if (!request.hasListener()) { //TODO maybe remove this; or add listener notification about error
				std::string error = request.errorMessage();
				if (error.empty()) {
					const ZLResource &errorResource = ZLResource::resource("dialog")["networkError"];
					error = ZLStringUtil::printf(errorResource["somethingWrongMessage"].value(), networkRequest.url().host().toStdString());
				}
				errors << QString::fromStdString(error);
			}
			continue;
		}

		networkRequest.setRawHeader("User-Agent", userAgent().c_str());
		QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
		if (!request.sslCertificate().DoVerify) {
			configuration.setPeerVerifyMode(QSslSocket::VerifyNone);
		}
		networkRequest.setSslConfiguration(configuration);

		QTimer* timeoutTimer = new QTimer;
		ZLQtNetworkReplyScope scope = {&request, timeoutTimer, false, &replies, &errors, &eventLoop};
		if (request.hasListener()) {
			qDebug() << "add request with listener";
			scope.replies = 0;
			scope.errors = 0;
			scope.eventLoop = 0;
		}
		prepareReply(scope, networkRequest);
	}
	if (!replies.isEmpty()) {
		qDebug() << "running eventloop";
		eventLoop.exec(QEventLoop::AllEvents);
	}

	return errors.join(QLatin1String("\n")).toStdString();
}

void ZLQtNetworkManager::prepareReply(ZLQtNetworkReplyScope &scope, QNetworkRequest networkRequest) const {
	QNetworkReply *reply = NULL;
	if (scope.request->isInstanceOf(ZLNetworkPostRequest::TYPE_ID)) {
		QByteArray data;
		ZLNetworkPostRequest &postRequest = static_cast<ZLNetworkPostRequest&>(*scope.request);
		QUrl tmp;
		typedef std::pair<std::string, std::string> string_pair;
		foreach (const string_pair &pair, postRequest.postData()) {
			tmp.addQueryItem(QString::fromStdString(pair.first), QString::fromStdString(pair.second));
		}
		data = tmp.encodedQuery();
		reply = const_cast<QNetworkAccessManager&>(myManager).post(networkRequest, data);
	} else {
		reply = const_cast<QNetworkAccessManager&>(myManager).get(networkRequest);
	}

	if (!scope.request->hasListener()) {
		qDebug() << "add request to replies list" << scope.request;
		scope.replies->push_back(reply);
	}

	QObject::connect(reply, SIGNAL(sslErrors(QList<QSslError>)),this, SLOT(onSslErrors(QList<QSslError>)));
	QObject::connect(reply, SIGNAL(readyRead()), this, SLOT(onReplyReadyRead()));
	QObject::disconnect(scope.timeoutTimer, 0, this, 0);
	QObject::connect(scope.timeoutTimer, SIGNAL(timeout()), this, SLOT(onTimeOut()));
	reply->setProperty("scope", qVariantFromValue(scope));
	scope.timeoutTimer->setProperty("reply", qVariantFromValue(reply));
	scope.timeoutTimer->start(timeoutValue());
}

void ZLQtNetworkManager::onFinished(QNetworkReply *reply) {
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	reply->deleteLater();

	if (!scope.request->hasListener()) {
		qDebug() << "removing request from replies list" << scope.request;
		scope.replies->removeOne(reply);
	}
	scope.timeoutTimer->stop();

	if (!scope.timeoutTimer->property("expired").isValid()) {
		if (handleRedirect(reply)) {
			return;
		}
		handleHeaders(reply);
		handleContent(reply);
	}
	scope.timeoutTimer->deleteLater();

	QString error = handleErrors(reply);

	if (scope.request->hasListener()) {
		scope.request->doAfter(error.toStdString());
		return;
	}

	if (!error.isEmpty()) {
		scope.errors->append(error);
	}
	if (!scope.request->doAfter(error.toStdString())) {
		//TODO maybe fix it: adding second error message
		scope.errors->append(QString::fromStdString(scope.request->errorMessage()));
	}
	if (scope.replies->isEmpty()) {
		qDebug() << "quitting eventloop";
		qDebug() << "";
		scope.eventLoop->quit();
	}
}

bool ZLQtNetworkManager::handleRedirect(QNetworkReply *reply) {
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	if (!scope.request->isRedirectionSupported()) {
		return false;
	}
	QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!redirect.isValid()) {
		return false;
	}
	QObject::disconnect(reply, 0, this, 0);

	QNetworkRequest request = reply->request();
	request.setUrl(reply->url().resolved(redirect));
	scope.authAskedAlready = false;
	prepareReply(scope, request);
	return true;
}

void ZLQtNetworkManager::onReplyReadyRead() {
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	scope.timeoutTimer->start(timeoutValue());
}

void ZLQtNetworkManager::onTimeOut() {
	QTimer *timer = qobject_cast<QTimer*>(sender());
	QNetworkReply* reply = timer->property("reply").value<QNetworkReply*>();
	timer->stop();
	timer->setProperty("expired", true);
	reply->abort();
}

void ZLQtNetworkManager::onAuthenticationRequired(QNetworkReply *reply, QAuthenticator *authenticator) {
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	if (scope.authAskedAlready) {
		return;
	}
	authenticator->setUser(QString::fromStdString(scope.request->userName()));
	authenticator->setPassword(QString::fromStdString(scope.request->password()));
	scope.authAskedAlready = true;
	reply->setProperty("scope", qVariantFromValue(scope));
}

void ZLQtNetworkManager::onSslErrors(const QList<QSslError> &errors) {
	QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
	reply->ignoreSslErrors(errors);
}

void ZLQtNetworkManager::handleHeaders(QNetworkReply *reply) const {
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	QByteArray data = "HTTP/1.1 ";
	data += reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toByteArray();
	data += " ";
	data += reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toByteArray();
	scope.request->handleHeader(data.data(), data.size());
	foreach (const QByteArray &headerName, reply->rawHeaderList()) {
		data  = headerName;
		QByteArray value = reply->rawHeader(headerName);
		data += ": ";
		data += value;
		if (headerName.toLower() == QString("content-encoding") && value.toLower().contains("gzip")) {
			continue;
		}
		scope.request->handleHeader(data.data(), data.size());
	}
}

void ZLQtNetworkManager::handleContent(QNetworkReply *reply) const {
	ZLQtNetworkReplyScope scope = reply->property("scope").value<ZLQtNetworkReplyScope>();
	if (reply->error() != QNetworkReply::NoError) {
		return;
	}
	QByteArray data = reply->readAll();
	if (!data.isEmpty()) {
		scope.request->handleContent(data.data(), data.size());
	}
}

QString ZLQtNetworkManager::handleErrors(QNetworkReply *reply) const {
	if (reply->error() == QNetworkReply::NoError) {
		return QString();
	}
	std::string error;
	const ZLResource &errorResource = ZLResource::resource("dialog")["networkError"];
	switch (reply->error()) {
		case QNetworkReply::AuthenticationRequiredError:
			error = errorResource["authenticationFailed"].value();
			break;
		case QNetworkReply::OperationCanceledError:
		case QNetworkReply::TimeoutError:
		case QNetworkReply::ProxyTimeoutError:
			error = errorResource["operationTimedOutMessage"].value();
			break;
		case QNetworkReply::SslHandshakeFailedError:
			error = ZLStringUtil::printf(errorResource["sslConnectErrorMessage"].value(), reply->url().toString().toStdString());
			break;
		case QNetworkReply::HostNotFoundError:
			error = ZLStringUtil::printf(errorResource["couldntConnectMessage"].value(), reply->url().toString().toStdString());
			break;
		case QNetworkReply::ContentNotFoundError: //404 error, may be it should be other error message?
			error = ZLStringUtil::printf(errorResource["somethingWrongMessage"].value(), reply->url().toString().toStdString());
			break;
		case QNetworkReply::UnknownNetworkError:
		case QNetworkReply::UnknownProxyError:
		case QNetworkReply::UnknownContentError:
		case QNetworkReply::ProtocolUnknownError:
			error = errorResource["unknownErrorMessage"].value();
			break;
		case QNetworkReply::ProxyNotFoundError:
			error = ZLStringUtil::printf(errorResource["couldntResolveProxyMessage"].value(), reply->url().toString().toStdString());
			break;

		//TODO add support of other errors code in our resources
		case QNetworkReply::ConnectionRefusedError:
		case QNetworkReply::RemoteHostClosedError:
		// proxy errors (101-199):
		case QNetworkReply::ProxyConnectionRefusedError:
		case QNetworkReply::ProxyConnectionClosedError:
		case QNetworkReply::ProxyAuthenticationRequiredError:
		// content errors (201-299):
		case QNetworkReply::ContentAccessDenied:
		case QNetworkReply::ContentOperationNotPermittedError:
		case QNetworkReply::ContentReSendError:
		// protocol errors
		case QNetworkReply::ProtocolInvalidOperationError:
		case QNetworkReply::ProtocolFailure:
		default:
			error = reply->errorString().toStdString();
			break;
	}
	return QString::fromStdString(error);
}

int ZLQtNetworkManager::timeoutValue() const {
	return TimeoutOption().value() * 1000;
}

ZLQtNetworkCookieJar::ZLQtNetworkCookieJar(QObject *parent) :
	QNetworkCookieJar(parent) {
}

ZLQtNetworkCookieJar::~ZLQtNetworkCookieJar() {
	save();
}

void ZLQtNetworkCookieJar::setFilePath(const QString &filePath) {
	myFilePath = filePath;
	QFile file(myFilePath);
	QList<QNetworkCookie> cookies;
	if (file.open(QFile::ReadOnly)) {
		cookies = QNetworkCookie::parseCookies(file.readAll());
	}
	setAllCookies(cookies);
}

bool ZLQtNetworkCookieJar::setCookiesFromUrl(const QList<QNetworkCookie> &cookieList, const QUrl &url) {
	bool result = QNetworkCookieJar::setCookiesFromUrl(cookieList, url);
	save();
	return result;
}

void ZLQtNetworkCookieJar::save() {
	if (myFilePath.isEmpty())
		return;
	QFile file(myFilePath);
	QDir dir = QFileInfo(myFilePath).absoluteDir();
	if (!dir.exists()) {
		dir.mkpath(dir.absolutePath());
	}
	if (!file.open(QFile::WriteOnly)) {
		ZLLogger::Instance().println("ZLQtNetworkCookieJar", myFilePath.toStdString() + " can't be open for writing");
		return;
	}
	bool first = true;
	foreach (const QNetworkCookie &cookie, allCookies()) {
		if (first) {
			first = false;
		} else {
			file.write("\n", 1);
		}
		file.write(cookie.toRawForm(QNetworkCookie::Full));
	}
}
