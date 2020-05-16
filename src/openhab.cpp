/******************************************************************************
 *
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
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

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QString>
#include <QtDebug>

#include "yio-interface/entities/blindinterface.h"
#include "yio-interface/entities/entityinterface.h"
#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/switchinterface.h"

OpenHABPlugin::OpenHABPlugin() : Plugin("openhab", NO_WORKER_THREAD) {}

Integration* OpenHABPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                              NotificationsInterface* notifications, YioAPIInterface* api,
                                              ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating OpenHAB integration plugin" << PLUGIN_VERSION;

    return new OpenHAB(config, entities, notifications, api, configObj, this);
}

OpenHAB::OpenHAB(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                 YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin), _sseNetworkManager(), _nam(this) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "url") {
            _url = iter.value().toString();
        }
    }
    if (!_url.endsWith('/')) {
        _url += '/';
    }

    _sseReconnectTimer = new QTimer(this);
    _sseReconnectTimer->setSingleShot(true);
    _sseReconnectTimer->setInterval(2000);
    _sseReconnectTimer->stop();

    _pollingTimer.setSingleShot(false);
    _pollingTimer.setInterval(1000 * 60);  // 1 minute

    QObject::connect(&_sseNetworkManager, &QNetworkAccessManager::finished, this, &OpenHAB::streamFinished);
    QObject::connect(_sseReconnectTimer, &QTimer::timeout, this, &OpenHAB::onSseTimeout);

    QObject::connect(&_pollingTimer, &QTimer::timeout, this, &OpenHAB::onPollingTimer);
    QObject::connect(&_nam, &QNetworkAccessManager::networkAccessibleChanged, this, &OpenHAB::onNetWorkAccessible);

    qCDebug(m_logCategory) << "setup";
}

void OpenHAB::streamReceived() {
    QByteArray     rawData = _sseReply->readAll();
    QByteArrayList splitted = rawData.split('\n');

    for (QByteArray data : splitted) {
        if ((data.length() == 0) || (data.startsWith("event: message"))) {
            continue;
        }

        data = data.replace("\"{", "{").replace("}\"", "}").replace("\\\"", "\"");

        QJsonParseError parseerror;
        // remove the message start passage, because it is no valid JSON
        QJsonDocument doc = QJsonDocument::fromJson(data.mid(6), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            qCCritical(m_logCategory) << "SSE JSON error:" << parseerror.errorString();
            continue;
        }

        QVariantMap map = doc.toVariant().toMap();
        // only process state changes
        if (!(map.value("topic").toString().endsWith("statechanged"))) continue;

        // get item name from the topic string
        // example: smarthome/items/EG_Esszimmer_Sonos_CurrentPlayingTime/state
        QString name = map.value("topic").toString().split('/')[2];

        EntityInterface* entity = m_entities->getEntityInterface(name);
        if (entity != nullptr) {
            QString value =
                (reinterpret_cast<const QVariantMap*>(map.value("payload").data()))->value("value").toString();
            // because OpenHab doesn't send the item type in the status update, we have to extract it from our own
            // entity library
            if (entity->type() == "light") {
                processLight(value, entity, false, false);
            } else if (entity->type() == "blind") {
                processBlind(value, entity);
            } else if (entity->type() == "switch") {
                processSwitch(value, entity);
            }
        } else if (_ohPlayerItems.contains(name)) {
            QString value =
                (reinterpret_cast<const QVariantMap*>(map.value("payload").data()))->value("value").toString();
            processPlayerItem(value, name);
        }
    }

    if (_wasDisconnected) {
        _wasDisconnected = false;
        getItems();
    }
}

void OpenHAB::streamFinished(QNetworkReply* reply) {
    reply->abort();
    _wasDisconnected = true;

    if (_userDisconnect == false && _standby == false) {
        qCDebug(m_logCategory) << "Lost SSE connection to OpenHab";
        _sseReconnectTimer->start();
    }
}

void OpenHAB::onSseTimeout() {
    if (_tries == 3) {
        disconnect();

        qCCritical(m_logCategory) << "Cannot connect to OpenHab: retried 3 times connecting to" << _url;
        QObject* param = this;

        m_notifications->add(true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
                             [](QObject* param) {
                                 Integration* i = qobject_cast<Integration*>(param);
                                 i->connect();
                             },
                             param);

        _tries = 0;
    } else {
        startSse();
        qCDebug(m_logCategory) << "Try to reconnect the OpenHab SSE connection";

        _tries++;
    }
}

void OpenHAB::startSse() {
    QNetworkRequest request(_url + "events");
    request.setRawHeader(QByteArray("Accept"), QByteArray("text/event-stream"));
    request.setHeader(QNetworkRequest::UserAgentHeader, "Yio Remote OpenHAB Plugin");
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);  // Events shouldn't be cached

    _sseReply = _sseNetworkManager.get(request);
    QObject::connect(_sseReply, &QNetworkReply::readyRead, this, &OpenHAB::streamReceived);
}

void OpenHAB::connect() {
    setState(CONNECTING);

    _myEntities = m_entities->getByIntegration(integrationId());

    // Look for mediaplayers
    for (QList<EntityInterface*>::iterator i = _myEntities.begin(); i != _myEntities.end(); ++i) {
        if ((*i)->type() == "media_player") {
            _ohPlayers.insert((*i)->entity_id(), OHPlayer());
        }
    }
    if (_ohPlayers.count() > 0) {
        getThings();
    } else {
        getItems(true);
    }

    _tries = 0;
    _userDisconnect = false;
    _wasDisconnected = false;
    _standby = false;

    // connect to the SSE source
    startSse();
}

void OpenHAB::disconnect() {
    _userDisconnect = true;

    if (_sseReply->isRunning()) {
        _sseReply->close();
    }

    _pollingTimer.stop();
    _sseReconnectTimer->stop();

    setState(DISCONNECTED);
}

void OpenHAB::enterStandby() {
    _standby = true;

    if (_sseReply->isRunning()) {
        _sseReply->close();
    }
    _pollingTimer.start();
}

void OpenHAB::leaveStandby() {
    _standby = false;

    _pollingTimer.stop();
    startSse();
}

void OpenHAB::jsonError(const QString& error) { qCWarning(m_logCategory) << "JSON error " << error; }

void OpenHAB::onPollingTimer() { getItems(); }

void OpenHAB::onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility) {
    qCInfo(m_logCategory) << "network accessibility" << accessibility;
}

void OpenHAB::getThings() {
    QNetworkRequest request(_url + "things");
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = _nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }
        processThings(doc);
    });
}

void OpenHAB::processThings(const QJsonDocument& result) {
    int countFound = 0, countAll = 0;

    // get all things
    QJsonArray array = result.array();
    for (QJsonArray::iterator i = array.begin(); i != array.end(); ++i) {
        QJsonObject item = i->toObject();
        QString     uid = item.value("UID").toString();
        countAll++;
        if (_ohPlayers.contains(uid)) {
            countFound++;
            for (QMap<QString, OHPlayer>::iterator i = _ohPlayers.begin(); i != _ohPlayers.end(); ++i) {
                if (i.key() == uid) {
                    initializePlayer(uid, i.value(), item);
                }
            }
        }
    }
    int missing = _ohPlayers.count() - countFound;
    if (missing > 0) {
        m_notifications->add(true, "openHAB - players missing : " + QString::number(missing));
        for (QMap<QString, OHPlayer>::iterator i = _ohPlayers.begin(); i != _ohPlayers.end(); ++i) {
            if (!i.value().found) {
                qCInfo(m_logCategory) << "missing player: " << i.key();
                EntityInterface* entity = m_entities->getEntityInterface(i.key());
                entity->setConnected(false);
            }
        }
    }
    qCInfo(m_logCategory)
        << QString("Got %1 things, %2 players for YIO, %3 missing").arg(countAll).arg(countFound).arg(missing);

    // Now getItems must be called
    getItems(true);
}

void OpenHAB::initializePlayer(const QString& entityId, OHPlayer& player, const QJsonObject& json) {
    player.found = true;

    QJsonArray channels = json.value("channels").toArray();
    for (QJsonArray::iterator i = channels.begin(); i != channels.end(); ++i) {
        QJsonObject channel = i->toObject();
        QJsonArray  linkedItems = channel.value("linkedItems").toArray();
        if (linkedItems.count() == 0) {
            continue;
        }

        QString linkItem = linkedItems[0].toString();
        QString id = channel.value("id").toString();
        if (id == "power") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "control") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "state") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "mode") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::SOURCE));
        } else if ((id == "volume") || (id == "volume-percent")) {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::VOLUME));
        } else if (id == "mute") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MUTED));
        } else if ((id == "artist") || (id == "play-info-name")) {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIAARTIST));
        } else if ((id == "title") || (id == "play-info-text")) {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIATITLE));
        } else if (id == "currentPlayingTime") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIAPROGRESS));
        } else if (id == "duration") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIADURATION));
        }
    }
}

void OpenHAB::getItems(bool first) {
    QNetworkRequest request(_url + "items");
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = _nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }
        if (first) {
            // called during connect
            setState(CONNECTED);
            _pollingTimer.start();
        }
        processItems(doc, first);
    });
}

void OpenHAB::processItems(const QJsonDocument& result, bool first) {
    int              countFound = 0, countAll = 0;
    QSet<QString>*   allEntities = nullptr;
    EntityInterface* entity = nullptr;

    if (first) {
        // Build a set of integrations entities (exclude media players)
        allEntities = new QSet<QString>();
        for (QList<EntityInterface*>::iterator i = _myEntities.begin(); i != _myEntities.end(); ++i) {
            if ((*i)->type() != "media_player") {
                allEntities->insert((*i)->entity_id());
            }
        }
    }

    // get all items
    QJsonArray array = result.array();
    for (QJsonArray::iterator i = array.begin(); i != array.end(); ++i) {
        QJsonObject item = i->toObject();
        QString     name = item.value("name").toString();
        countAll++;
        entity = m_entities->getEntityInterface(name);
        if (entity != nullptr) {
            countFound++;
            if (allEntities != nullptr) {
                // found, remove from allEntites
                allEntities->remove(name);
            }
            // process entity
            processItem(item, entity);
        } else {
            // try player
            if (_ohPlayerItems.contains(name)) {
                processPlayerItem(item.value("state").toString(), name);
            }
        }
    }

    if (allEntities != nullptr) {
        // Check if we have got all entities, disconnect this entities
        for (const QString& entityId : *allEntities) {
            qCInfo(m_logCategory) << "missing: " << entityId;
            entity = m_entities->getEntityInterface(entityId);
            entity->setConnected(false);
        }
        int missing = allEntities->count();
        if (missing > 0) {
            m_notifications->add(true, "openHAB - entities missing : " + QString::number(missing));
        }
        delete allEntities;
        qCInfo(m_logCategory)
            << QString("Got %1 items, %2 for YIO, %3 missing").arg(countAll).arg(countFound).arg(missing);
    }
}

void OpenHAB::processItem(const QJsonObject& item, EntityInterface* entity) {
    Q_ASSERT(entity != nullptr);
    QString ohtype = item.value("type").toString();
    if (ohtype == "Dimmer") {
        processLight(item.value("state").toString(), entity, true);
    } else if (ohtype == "Switch" && entity->type() == "light") {
        processLight(item.value("state").toString(), entity, false);
    } else if (ohtype == "Switch" && entity->type() == "switch") {
        processSwitch(item.value("state").toString(), entity);
    } else if (ohtype == "Rollershutter") {
        processBlind(item.value("state").toString(), entity);
    } else {
        qCDebug(m_logCategory)
            << QString("Unsupported openHab type %1 for entity %s").arg(ohtype).arg(entity->entity_id());
    }
}

void OpenHAB::processLight(const QString& value, EntityInterface* entity, bool isDimmer, bool hasValidDimmerInfo) {
    int  brightness;
    bool isInt;

    brightness = value.toInt(&isInt);
    if (!hasValidDimmerInfo) isDimmer = isInt;
    if (isDimmer) {
        // int  brightness = value.toInt();
        bool on = brightness == 100;
        entity->setState(on ? LightDef::ON : LightDef::OFF);
        if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, brightness);
        } else {
            qCDebug(m_logCategory) << QString("OpenHab Dimmer %1 not supporting BRIGHTNESS").arg(entity->entity_id());
        }
    } else {
        QString state = value.toUpper();
        if (state == "ON") {
            entity->setState(LightDef::ON);
        } else if (state == "OFF") {
            entity->setState(LightDef::OFF);
        } else {
            qCDebug(m_logCategory)
                << QString("OpenHab Switch %1 undefined state %2").arg(entity->entity_id()).arg(state);
        }
        if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            qCDebug(m_logCategory) << QString("OpenHab Switch %1 does not support BRIGHTNESS").arg(entity->entity_id());
        }
    }
}

void OpenHAB::processBlind(const QString& value, EntityInterface* entity) {
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
    QString state = value.toUpper();
    if (state == "ON") {
        entity->setState(SwitchDef::ON);
    } else {
        entity->setState(SwitchDef::OFF);
    }
}

void OpenHAB::processPlayerItem(const QString& value, const QString& name) {
    OHPlayerItem     playerItem = _ohPlayerItems[name];
    QString          state = value.toUpper();
    EntityInterface* entity = m_entities->getEntityInterface(playerItem.playerId);
    switch (playerItem.attribute) {
        case MediaPlayerDef::STATE:
            if (state == "ON") {
                entity->setState(MediaPlayerDef::ON);
            } else if (state == "OFF") {
                entity->setState(MediaPlayerDef::OFF);
            } else if (state == "PLAY") {
                entity->setState(MediaPlayerDef::PLAYING);
            } else if (state == "PAUSE") {
                entity->setState(MediaPlayerDef::IDLE);
            }
            break;
        case MediaPlayerDef::VOLUME:
            entity->updateAttrByIndex(MediaPlayerDef::VOLUME, state.toInt());
            break;
        case MediaPlayerDef::MUTED:
            entity->updateAttrByIndex(MediaPlayerDef::MUTED, state == "ON");
            break;
        case MediaPlayerDef::SOURCE:
            entity->updateAttrByIndex(MediaPlayerDef::SOURCE, state);
            break;
        case MediaPlayerDef::MEDIAARTIST:
            entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, state);
            break;
        case MediaPlayerDef::MEDIATITLE:
            entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, state);
            break;
        case MediaPlayerDef::MEDIADURATION:
            entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, state);
            break;
        case MediaPlayerDef::MEDIAPROGRESS:
            entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, state);
            break;
        default:
            break;
    }
}

const QString* OpenHAB::lookupPlayerItem(const QString& entityId, MediaPlayerDef::Attributes attr) {
    for (QMap<QString, OHPlayerItem>::iterator i = _ohPlayerItems.begin(); i != _ohPlayerItems.end(); ++i) {
        if (i.value().attribute == attr && i.value().playerId == entityId) {
            return &i.key();
        }
    }
    return nullptr;
}

void OpenHAB::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    QString state;
    if (type == "light") {
        ////////////////////////////////////////////////////////////////
        // Light
        ////////////////////////////////////////////////////////////////
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
            default:
                qCInfo(m_logCategory) << "Light command" << command << " not supported for " << entityId;
                return;
        }
        qCDebug(m_logCategory) << "Light command" << command << " - " << state << " for " << entityId;
        openHABCommand(entityId, state);
    } else if (type == "switch") {
        ////////////////////////////////////////////////////////////////
        // Switch
        ////////////////////////////////////////////////////////////////
        if (command == SwitchDef::C_OFF) {
            state = "OFF";
        } else if (command == SwitchDef::C_ON) {
            state = "ON";
        }
        qCDebug(m_logCategory) << "Switch command" << command << " - " << state << " for " << entityId;
        openHABCommand(entityId, state);
    } else if (type == "blind") {
        ////////////////////////////////////////////////////////////////
        // Blind
        ////////////////////////////////////////////////////////////////
        switch (static_cast<BlindDef::Commands>(command)) {
            case BlindDef::C_OPEN:
                state = "UP";
                break;
            case BlindDef::C_CLOSE:
                state = "DOWN";
                break;
            case BlindDef::C_STOP:
                state = "STOP";
                break;
            case BlindDef::C_POSITION:
                state = QString::number(param.toInt());
                break;
        }
        qCDebug(m_logCategory) << "Blind command" << command << " - " << state << " for " << entityId;
        openHABCommand(entityId, state);
    } else if (type == "media_player") {
        ////////////////////////////////////////////////////////////////
        // Media Player
        ////////////////////////////////////////////////////////////////
        const QString* ohitemId = nullptr;
        switch (static_cast<MediaPlayerDef::Commands>(command)) {
            case MediaPlayerDef::C_TURNON:
                state = "ON";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_TURNOFF:
                state = "OFF";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PLAY:
                state = "PLAY";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PAUSE:
                state = "PAUSE";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_STOP:
                state = "STOP";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PREVIOUS:
                state = "PREVIOUS";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_NEXT:
                state = "NEXT";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_VOLUME_SET:
                state = QString::number(param.toInt());
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::VOLUME);
                break;
            case MediaPlayerDef::C_VOLUME_UP:
                state = "UP";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::VOLUME);
                break;
            case MediaPlayerDef::C_VOLUME_DOWN:
                state = "UP";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::VOLUME);
                break;
            default:
                qCInfo(m_logCategory) << "Media player command" << command << " not supported for " << entityId;
                return;
        }
        if (ohitemId == nullptr) {
            qCInfo(m_logCategory) << "Media player command" << command << " not supported for " << entityId;
            return;
        }
        qCDebug(m_logCategory) << "Media player command" << command << " - " << state << " for " << entityId;
        openHABCommand(*ohitemId, state);
    }
}

void OpenHAB::openHABCommand(const QString& itemId, const QString& state) {
    QNetworkRequest request(_url + "items/" + itemId);
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "text/plain");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = _nam.post(request, state.toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString answer = reply->readAll();
        return;
    });
}
