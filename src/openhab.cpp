/******************************************************************************
 *
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
 * Copyright (C) 2020 Andreas Mroß <andreas@mross.pw>
 * Copyright (C) 2020 Michael Löcher <MichaelLoercher@web.de>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#include "openhab.h"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QString>

#include "openhab_channelmappings.h"
#include "yio-interface/entities/blindinterface.h"
#include "yio-interface/entities/entityinterface.h"
#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-interface/entities/switchinterface.h"

OpenHABPlugin::OpenHABPlugin() : Plugin("yio.plugin.openhab", NO_WORKER_THREAD) {}

Integration* OpenHABPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                              NotificationsInterface* notifications, YioAPIInterface* api,
                                              ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating OpenHAB integration plugin" << PLUGIN_VERSION;
    return new OpenHAB(config, entities, notifications, api, configObj, this);
}

OpenHAB::OpenHAB(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                 YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "url") {
            _url = iter.value().toString();
        }
        if (iter.key() == "token") {
            _token = iter.value().toString();
        }
    }
    if (!_url.contains("rest")) {
        if (!_url.endsWith('/')) {
            _url += "/rest/";
        } else {
            _url += "rest/";
        }
    } else if (!_url.endsWith('/')) {
        _url += "/";
    }
    context_openHab = this;
    _sseNetworkManager = new QNetworkAccessManager(context_openHab);
    _sseReconnectTimer = new QTimer(context_openHab);
    _nam = new QNetworkAccessManager(context_openHab);

    for (QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.type() == QNetworkInterface::Wifi) {
            qCDebug(m_logCategory) << iface.humanReadableName() << "(" << iface.name() << ")"
                                   << "is up:" << iface.flags().testFlag(QNetworkInterface::IsUp)
                                   << "is running:" << iface.flags().testFlag(QNetworkInterface::IsRunning);
            _iface = iface;
        }
    }
}

void OpenHAB::streamReceived() {
    if (_sseReply->error() == QNetworkReply::NoError) {
        QJsonParseError parseerror;
        QJsonDocument   doc;
        QJsonDocument   pyload;
        QByteArray      rawData;
        QByteArrayList  splitted;

        rawData.clear();
        rawData = _sseReply->readAll();
        splitted = rawData.split('\n');

        for (QByteArray data : splitted) {
            if ((data.length() == 0) || (data.startsWith("event: message"))) {
                continue;
            }

            if (_flagMoreDataNeeded) {
                _tempJSONData = _tempJSONData + QString(data);
                doc = QJsonDocument::fromJson(_tempJSONData.toUtf8(), &parseerror);
                _flagMoreDataNeeded = false;
            } else {
                doc = QJsonDocument::fromJson(data.mid(6), &parseerror);
            }
            if ((parseerror.error == QJsonParseError::UnterminatedString ||
                 parseerror.error == QJsonParseError::UnterminatedObject ||
                 parseerror.error == QJsonParseError::IllegalValue ||
                 parseerror.error == QJsonParseError::IllegalEscapeSequence)) {
                _tempJSONData = QString(data.mid(6));
                _flagMoreDataNeeded = true;
            } else if (_tempJSONData != "") {
                _flagMoreDataNeeded = false;
                _tempJSONData = "";
            }

            if (parseerror.error == QJsonParseError::GarbageAtEnd) {
                doc = QJsonDocument::fromJson(data.mid(6).left(data.lastIndexOf("}") + 1), &parseerror);
            }

            if (parseerror.error != QJsonParseError::NoError && !_flagMoreDataNeeded) {
                qCDebug(m_logCategory) << QString(doc.toJson(QJsonDocument::Compact)) << "read " << data.size()
                                       << "bytes"
                                       << "read data" << data.mid(6) << "SSE JSON error:" << parseerror.error
                                       << parseerror.errorString();
                continue;
            }
            // only process state changes
            if (!_flagMoreDataNeeded) {
                if ((doc.object().value("type").toString() == "ItemStateEvent") ||
                    (doc.object().value("type").toString() == "GroupItemStateChangedEvent")) {
                    // get item name from the topic string
                    // example: smarthome/items/EG_Esszimmer_Sonos_CurrentPlayingTime/state
                    QString          name = doc.object().value("topic").toString().split('/')[2];
                    EntityInterface* entity = m_entities->getEntityInterface(name);
                    if (entity != nullptr && entity->connected()) {
                        pyload =
                            QJsonDocument::fromJson(doc.object().value("payload").toString().toUtf8(), &parseerror);
                        if (parseerror.error != QJsonParseError::NoError && !_flagMoreDataNeeded) {
                            qCDebug(m_logCategory)
                                << QString(pyload.toJson(QJsonDocument::Compact)) << "read "
                                << doc.object().value("payload").toString().size() << "bytes"
                                << "read " << doc.object().value("payload").toString()
                                << "SSE JSON pyload error:" << parseerror.error << parseerror.errorString();
                            continue;
                        }
                        // because OpenHab doesn't send the item type in the status update, we have to extract it from
                        // our own entity library
                        if (pyload.object().value("value").toString() != "UNDEF") {
                            if (entity->type() == "light" && entity->supported_features().contains("BRIGHTNESS") &&
                                _brightnessValueTemplate.exactMatch(pyload.object().value("value").toString())) {
                                processLight(pyload.object().value("value").toString(), entity, true);
                            } else if (entity->type() == "light" && entity->supported_features().contains("COLOR") &&
                                       _colorValueTemplate.exactMatch(pyload.object().value("value").toString())) {
                                processComplexLight(pyload.object().value("value").toString(), entity);
                            } else if (entity->type() == "light") {
                                processLight(pyload.object().value("value").toString(), entity, false);
                            } else if (entity->type() == "blind") {
                                processBlind(pyload.object().value("value").toString(), entity);
                            } else if (entity->type() == "switch") {
                                processSwitch(pyload.object().value("value").toString(), entity);
                            }
                        }
                    } else if (entity == nullptr) {
                        // qCDebug(m_logCategory) << QString("openHab Item %1 is not configured").arg(name);
                    } else {
                        qCDebug(m_logCategory) << QString("Entity %1 is offline").arg(name);
                    }
                }
            }
            doc = QJsonDocument();
            pyload = QJsonDocument();
            data.clear();
        }
        rawData.clear();
        splitted.clear();
    } else {
        qCDebug(m_logCategory) << "streamerror";
    }
}

void OpenHAB::streamFinished(QNetworkReply* reply) {
    reply->abort();

    if (_flagOpenHabConnected && !_flagStandby) {
        qCDebug(m_logCategory) << "Lost SSE connection to OpenHab";
        _sseReconnectTimer->start();
    }
    reply->deleteLater();
}

void OpenHAB::onSseTimeout() {
    qCDebug(m_logCategory) << "timer";
    if (_tries == 3) {
        disconnect();
        qCDebug(m_logCategory) << "disconnect 1";
        qCCritical(m_logCategory) << "Cannot connect to OpenHab: retried 3 times connecting to" << _url;
        QObject* param = this;

        m_notifications->add(
            true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
            [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            param);

        _tries = 0;
    } else {
        if (_flagSseConnected) {
            if (_sseReply->isRunning()) {
                _sseReply->abort();
                QObject::disconnect(_sseReply, &QNetworkReply::readyRead, context_openHab, &OpenHAB::streamReceived);
                _sseNetworkManager->clearConnectionCache();
                _flagSseConnected = false;
            }
        }
        if (!_flagStandby) {
            startSse();
            qCDebug(m_logCategory) << "Try to reconnect the OpenHab SSE connection";
            _sseReconnectTimer->stop();
            _tries++;
        }
    }
}

void OpenHAB::startSse() {
    QNetworkRequest request(_url + "events");
    request.setRawHeader("Accept", "text/event-stream");
    request.setHeader(QNetworkRequest::UserAgentHeader, "Yio Remote OpenHAB Plugin");
    if (_token != "") {
        request.setRawHeader("accept", "*/*");
        QString token = "Bearer " + _token;
        request.setRawHeader("Authorization", token.toUtf8());
    }
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);  // Events shouldn't be cached
    _sseReply = _sseNetworkManager->get(request);
    QObject::connect(_sseReply, &QNetworkReply::readyRead, context_openHab, &OpenHAB::streamReceived);
    _flagSseConnected = true;
}

void OpenHAB::networkManagerFinished(QNetworkReply* reply) {
    QString answer = reply->readAll();
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        if (_networktries == 3) {
            m_notifications->add(
                true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                [](QObject* param) {
                    Integration* i = qobject_cast<Integration*>(param);
                    i->connect();
                },
                this);
            _flagOpenHabConnected = false;
            disconnect();
            qCDebug(m_logCategory) << "disconnect 2"
                                   << QString::number(
                                          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt())
                                   << "openhab not reachable";
            _networktries = 0;
        } else {
            getSystemInfo();
            _networktries++;
        }
    } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
        _networktries = 0;
        if (state() != CONNECTED && answer.contains("systemInfo")) {
            qCDebug(m_logCategory) << reply->header(QNetworkRequest::ContentTypeHeader).toString() << " : " << answer;

            _sseReconnectTimer->setSingleShot(true);
            _sseReconnectTimer->setInterval(2000);
            QObject::connect(_sseReconnectTimer, &QTimer::timeout, context_openHab, &OpenHAB::onSseTimeout);
            _sseReconnectTimer->stop();

            QObject::connect(_sseNetworkManager, &QNetworkAccessManager::finished, context_openHab,
                             &OpenHAB::streamFinished);

            _flagOpenHabConnected = true;
            startSse();
            getItems();

        } else if (state() != CONNECTED && answer.contains("rest/items/")) {
            QJsonParseError parseerror;
            if (answer != "") {
                QJsonDocument doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                if (parseerror.error != QJsonParseError::NoError) {
                    jsonError(parseerror.errorString());
                    return;
                }
                processItems(doc, true);
                _flagOpenHabConnected = true;
                setState(CONNECTED);
            }
        } else if (state() == CONNECTED && answer.contains("rest/items/")) {
            QJsonParseError parseerror;
            if (answer != "") {
                QJsonDocument doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                if (parseerror.error != QJsonParseError::NoError) {
                    jsonError(parseerror.errorString());
                    return;
                }
                processItems(doc, false);
                _flagOpenHabConnected = true;
            }
        } else if (state() == CONNECTED && _flagleaveStandby) {
            _flagOpenHabConnected = true;
            _flagleaveStandby = false;
            getItems();
            startSse();
        } else if (state() == CONNECTED) {
            _flagOpenHabConnected = true;
        }
    } else {
        if (_networktries == 3) {
            m_notifications->add(
                true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                [](QObject* param) {
                    Integration* i = qobject_cast<Integration*>(param);
                    i->connect();
                },
                this);
            _flagOpenHabConnected = false;
            disconnect();
            qCDebug(m_logCategory) << "disconnect 3"
                                   << "openhab not reachable";
            _networktries = 0;
        } else {
            getSystemInfo();
            _networktries++;
        }
    }
}
void OpenHAB::connect() {
    setState(CONNECTING);

    if (!_iface.flags().testFlag(QNetworkInterface::IsUp) && !_iface.flags().testFlag(QNetworkInterface::IsRunning)) {
        m_notifications->add(
            true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
            [](QObject* param) {
                Integration* i = qobject_cast<Integration*>(param);
                i->connect();
            },
            this);
    } else {
        qCDebug(m_logCategory) << "setup";

        _myEntities = m_entities->getByIntegration(integrationId());

        _flagStandby = false;
        QObject::connect(_nam, &QNetworkAccessManager::finished, context_openHab, &OpenHAB::networkManagerFinished);
        getSystemInfo();
    }
}

void OpenHAB::disconnect() {
    qCDebug(m_logCategory) << state();
    if (_flagSseConnected) {
        if (_sseReply->isRunning()) {
            _sseReply->abort();
            QObject::disconnect(_sseReply, &QNetworkReply::readyRead, context_openHab, &OpenHAB::streamReceived);
            _sseNetworkManager->clearConnectionCache();
            _flagSseConnected = false;
        }
    }
    _sseReconnectTimer->stop();
    QObject::disconnect(_sseNetworkManager, &QNetworkAccessManager::finished, context_openHab,
                        &OpenHAB::streamFinished);
    QObject::disconnect(_sseReconnectTimer, &QTimer::timeout, context_openHab, &OpenHAB::onSseTimeout);
    QObject::disconnect(_nam, &QNetworkAccessManager::finished, context_openHab, &OpenHAB::networkManagerFinished);

    setState(DISCONNECTED);
}

void OpenHAB::enterStandby() {
    _flagStandby = true;
    if (_flagSseConnected) {
        if (_sseReply->isRunning()) {
            _sseReply->abort();
            QObject::disconnect(_sseReply, &QNetworkReply::readyRead, context_openHab, &OpenHAB::streamReceived);
            _sseNetworkManager->clearConnectionCache();
            _flagSseConnected = false;
        }
    }
}

void OpenHAB::leaveStandby() {
    _flagStandby = false;
    _flagleaveStandby = true;
    getSystemInfo();
}

void OpenHAB::jsonError(const QString& error) {
    qCWarning(m_logCategory) << "JSON error " << error;
}

void OpenHAB::onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility) {
    qCInfo(m_logCategory) << "network accessibility" << accessibility;
}

void OpenHAB::getItems() {
    QNetworkRequest request(_url + "items");
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    if (_token != "") {
        request.setRawHeader("accept", "*/*");
        QString token = "Bearer " + _token;
        request.setRawHeader("Authorization", token.toUtf8());
    }
    request.setRawHeader("Accept", "application/json");
    _nam->get(request);
}

void OpenHAB::getItem(const QString name) {
    QNetworkRequest request(_url + "items/" + name);
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    if (_token != "") {
        request.setRawHeader("accept", "*/*");
        QString token = "Bearer " + _token;
        request.setRawHeader("Authorization", token.toUtf8());
    }
    request.setRawHeader("Accept", "application/json");
    _nam->get(request);
}
void OpenHAB::processItem(const QJsonDocument& result) {
    QJsonObject json = result.object();

    for (int i = 0; i < _myEntities.size(); ++i) {
        if (json.value("name") == _myEntities[i]->entity_id()) {
            processEntity(json, _myEntities[i]);
        }
    }
}
void OpenHAB::processItems(const QJsonDocument& result, bool first) {
    int        countFound = 0, countAll = 0;
    QJsonArray array = result.array();

    qCDebug(m_logCategory) << array.size();

    if (first) {
        for (int i = 0; i < _myEntities.size(); ++i) {
            _myEntities[i]->setConnected(false);
        }
        for (int i = 0; i < _myEntities.size(); ++i) {
            for (QJsonArray::iterator j = array.begin(); j != array.end(); ++j) {
                QJsonObject item = j->toObject();
                QString     name = item.value("name").toString();
                countAll++;
                if (name == _myEntities[i]->entity_id()) {
                    countFound++;
                    _myEntities[i]->setConnected(true);
                    qCDebug(m_logCategory) << _myEntities[i]->entity_id() + "connected:" + _myEntities[i]->connected();
                    processEntity(item, _myEntities[i]);
                }
            }
        }
        if ((_myEntities.count() - countFound) > 0) {
            m_notifications->add(
                true, "Could not load : " + QString::number((_myEntities.count() - countFound)) + "openHAB items");
        }
    } else {
        for (int i = 0; i < _myEntities.size(); ++i) {
            for (QJsonArray::iterator j = array.begin(); j != array.end(); ++j) {
                QJsonObject item = j->toObject();
                QString     name = item.value("name").toString();
                countAll++;
                if (name == _myEntities[i]->entity_id()) {
                    countFound++;
                    processEntity(item, _myEntities[i]);
                } else {
                    // _myEntities.removeAt(i);
                }
            }
        }
    }
}

void OpenHAB::processEntity(const QJsonObject& item, EntityInterface* entity) {
    Q_ASSERT(entity != nullptr);

    if (entity->connected()) {
        if (entity->type() == "light" && entity->supported_features().contains("BRIGHTNESS") &&
            _brightnessValueTemplate.exactMatch(item.value("state").toString())) {
            processLight(item.value("state").toString(), entity, true);
        }
        if (entity->type() == "light" && entity->supported_features().contains("COLOR") &&
            _colorValueTemplate.exactMatch(item.value("state").toString())) {
            processComplexLight(item.value("state").toString(), entity);
        }
        if (entity->type() == "light") {
            processLight(item.value("state").toString(), entity, false);
        }

        if (entity->type() == "switch") {
            processSwitch(item.value("state").toString(), entity);
        }
        if (entity->type() == "blind") {
            processBlind(item.value("state").toString(), entity);
        }
    } else {
        qCDebug(m_logCategory) << QString("Entity %s is offline").arg(entity->entity_id());
    }
}

void OpenHAB::processLight(const QString& value, EntityInterface* entity, bool isDimmer) {
    if (entity == nullptr) return;
    if (!(value.contains("ON")) && !(value.contains("OFF")) && isDimmer) {
        entity->setState(value.toInt() > 0 ? LightDef::ON : LightDef::OFF);
        if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, value);
        } else {
            qCDebug(m_logCategory) << QString("OpenHab Dimmer %1 not supporting BRIGHTNESS").arg(entity->entity_id());
        }
    } else {
        if (value.toUpper() == "ON") {
            entity->setState(LightDef::ON);
        } else if (value.toUpper() == "OFF") {
            entity->setState(LightDef::OFF);
        } else {
            qCDebug(m_logCategory)
                << QString("OpenHab Switch %1 undefined state %2").arg(entity->entity_id()).arg(value.toUpper());
        }
    }
}

void OpenHAB::processBlind(const QString& value, EntityInterface* entity) {
    if (entity == nullptr) return;
    QString state = value.toUpper();
    bool    ok = false;
    int     pos = state.toInt(&ok, 10);
    if (ok && entity->isSupported(BlindDef::F_POSITION)) {
        entity->updateAttrByIndex(BlindDef::POSITION, pos);
        entity->setState(pos == 100 ? BlindDef::OPEN : BlindDef::CLOSED);
    } else if (state == "ON") {
        entity->setState(BlindDef::OPEN);
    } else if (state == "OFF") {
        entity->setState(BlindDef::CLOSED);
    }
}

void OpenHAB::processSwitch(const QString& value, EntityInterface* entity) {
    if (entity == nullptr) return;
    QString state = value.toUpper();
    if (state == "ON") {
        entity->setState(SwitchDef::ON);
    } else {
        entity->setState(SwitchDef::OFF);
    }
}

void OpenHAB::processComplexLight(const QString& value, EntityInterface* entity) {
    if (entity == nullptr) return;
    if (entity->supported_features().contains("COLOR")) {
        if (_colorValueTemplate.exactMatch(value)) {
            QStringList cs = value.split(',');
            QColor      color =
                QColor((cs[0].toInt()), ((cs[1].toInt() * 255) / 100), ((cs[2].toInt() * 255) / 100), QColor::Hsl);
            char buffer[10];
            snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.red(), color.green(), color.blue());
            entity->updateAttrByIndex(LightDef::COLOR, buffer);
        } else if (_brightnessValueTemplate.exactMatch(value) && entity->supported_features().contains("BRIGHTNESS")) {
            int  brightness = value.toInt();
            bool on = brightness == 100;
            entity->setState(on ? LightDef::ON : LightDef::OFF);
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, brightness);
        } else if (value.contains("ON") || value.contains("OFF")) {
            if (value.toUpper() == "ON") {
                entity->setState(LightDef::ON);
            } else if (value.toUpper() == "OFF") {
                entity->setState(LightDef::OFF);
            } else {
                qCInfo(m_logCategory) << "Wrong or not supported Color/Brightness command for " << entity->entity_id();
            }
        } else {
            qCInfo(m_logCategory) << "Wrong or not supported Color/Brightness command for " << entity->entity_id();
        }
    } else if (entity->supported_features().contains("COLORTEMP")) {
        int colortemp = value.toInt();
        entity->updateAttrByIndex(LightDef::COLORTEMP, colortemp);
    } else {
        qCInfo(m_logCategory) << "Not supported Color/Brightness/Colortemp command for " << entity->entity_id();
    }
}

void OpenHAB::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    QString      state;
    QColor       color;
    QVariantMap  data;
    QVariantList list;

    if (type == "light") {
        switch (static_cast<LightDef::Commands>(command)) {
            case LightDef::C_OFF:
                state = "OFF";
                break;
            case LightDef::C_ON:
                state = "ON";
                break;
            case LightDef::C_BRIGHTNESS:
                state = QString::number(param.toInt());
                break;
            case LightDef::C_COLOR:
                color = QColor(param.toString());
                state = QString::number(color.hue()) + ',' + QString::number(color.saturationF() * 100) + ',' +
                        QString::number(color.lightnessF() * 100);
                break;
            default:
                qCInfo(m_logCategory) << "Light command" << command << " not supported for " << entityId;
                return;
        }
    } else if (type == "switch") {
        switch (static_cast<SwitchDef::Commands>(command)) {
            case SwitchDef::C_OFF:
                state = "OFF";
                break;
            case SwitchDef::C_ON:
                state = "ON";
                break;
            default:
                qCInfo(m_logCategory) << "Light command" << command << " not supported for " << entityId;
                return;
        }
    } else {
        qCInfo(m_logCategory) << "Command" << command << " not supported for " << entityId;
    }
    qCDebug(m_logCategory) << "Command" << command << " - " << state << " for " << entityId;
    sendOpenHABCommand(entityId, state);
}

void OpenHAB::sendOpenHABCommand(const QString& itemId, const QString& state) {
    QNetworkRequest request(_url + "items/" + itemId);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    if (_token != "") {
        request.setRawHeader("accept", "*/*");
        QString token = "Bearer " + _token;
        request.setRawHeader("Authorization", token.toUtf8());
    }

    _nam->post(request, state.toUtf8());
}

void OpenHAB::getSystemInfo() {
    QNetworkRequest request(_url + "/systeminfo");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    if (_token != "") {
        request.setRawHeader("accept", "*/*");
        QString token = "Bearer " + _token;
        request.setRawHeader("Authorization", token.toUtf8());
    }
    _nam->get(request);
}
