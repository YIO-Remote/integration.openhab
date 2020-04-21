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

OpenHABPlugin::OpenHABPlugin() : Plugin("openhab", NO_WORKER_THREAD) {}

Integration* OpenHABPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                              NotificationsInterface* notifications, YioAPIInterface* api,
                                              ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating OpenHAB integration plugin" << PLUGIN_VERSION;

    return new OpenHAB(config, entities, notifications, api, configObj, this);
}

OpenHAB::OpenHAB(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                 YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin),
      _pollingInterval(1000),  // default
      _nam(this) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == "url") {
            _url = iter.value().toString();
        } else if (iter.key() == "polling_interval") {
            _pollingInterval = iter.value().toInt();
        }
    }
    if (!_url.endsWith('/')) {
        _url += '/';
    }
    _pollingTimer.setSingleShot(false);
    _pollingTimer.setInterval(_pollingInterval);
    _pollingTimer.stop();

    QObject::connect(&_pollingTimer, &QTimer::timeout, this, &OpenHAB::onPollingTimer);
    QObject::connect(&_nam, &QNetworkAccessManager::networkAccessibleChanged, this, &OpenHAB::onNetWorkAccessible);

    qCDebug(m_logCategory) << "setup";
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
}

void OpenHAB::disconnect() {
    _pollingTimer.stop();
    setState(DISCONNECTED);
}

void OpenHAB::enterStandby() {
    _pollingTimer.setInterval(1000 * 60);  // 1 minute
}

void OpenHAB::leaveStandby() { _pollingTimer.setInterval(_pollingInterval); }

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
        QString     uid  = item.value("UID").toString();
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
        QJsonObject channel     = i->toObject();
        QJsonArray  linkedItems = channel.value("linkedItems").toArray();
        if (linkedItems.count() == 0) {
            continue;
        }

        QString linkItem = linkedItems[0].toString();
        QString id       = channel.value("id").toString();
        if (id == "power") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "control") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "state") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::STATE));
        } else if (id == "mode") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::SOURCE));
        } else if (id == "volume-percent") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::VOLUME));
        } else if (id == "mute") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MUTED));
        } else if (id == "play-info-name") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIAARTIST));
        } else if (id == "play-info-text") {
            _ohPlayerItems.insert(linkItem, OHPlayerItem(entityId, MediaPlayerDef::MEDIATITLE));
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
    EntityInterface* entity      = nullptr;

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
                processPlayerItem(item, name);
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
        processLight(item, entity, true);
    } else if (ohtype == "Switch" && entity->type() == "light") {
        processLight(item, entity, false);
    } else if (ohtype == "Rollershutter") {
        processBlind(item, entity);
    } else {
        qCDebug(m_logCategory)
            << QString("Unsupported openHab type %1 for entity %s").arg(ohtype).arg(entity->entity_id());
    }
}

void OpenHAB::processLight(const QJsonObject& item, EntityInterface* entity, bool isDimmer) {
    if (isDimmer) {
        int  brightness = item.value("state").toString().toInt();
        bool on         = brightness == 100;
        entity->setState(on ? LightDef::ON : LightDef::OFF);
        if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, brightness);
        } else {
            qCDebug(m_logCategory) << QString("OpenHab Dimmer %1 not supporting BRIGHTNESS").arg(entity->entity_id());
        }
    } else {
        QString state = item.value("state").toString().toUpper();
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

void OpenHAB::processBlind(const QJsonObject& item, EntityInterface* entity) {
    QString state = item.value("state").toString().toUpper();
    bool    ok    = false;
    int     pos   = state.toInt(&ok, 10);
    if (ok && entity->isSupported(BlindDef::F_POSITION)) {
        entity->updateAttrByIndex(BlindDef::POSITION, pos);
        entity->setState(pos == 100 ? BlindDef::OPEN : BlindDef::CLOSED);
    } else if (state == "ON") {
        entity->setState(BlindDef::OPEN);
    } else if (state == "OFF") {
        entity->setState(BlindDef::CLOSED);
    }
}

void OpenHAB::processPlayerItem(const QJsonObject& item, const QString& name) {
    OHPlayerItem     playerItem = _ohPlayerItems[name];
    QString          state      = item.value("state").toString().toUpper();
    EntityInterface* entity     = m_entities->getEntityInterface(playerItem.playerId);
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
    }

    if (type == "blind") {
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
    }
    if (type == "media_player") {
        ////////////////////////////////////////////////////////////////
        // Media Player
        ////////////////////////////////////////////////////////////////
        const QString* ohitemId = nullptr;
        switch (static_cast<MediaPlayerDef::Commands>(command)) {
            case MediaPlayerDef::C_TURNON:
                state    = "ON";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_TURNOFF:
                state    = "OFF";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PLAY:
                state    = "PLAY";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PAUSE:
                state    = "PAUSE";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_STOP:
                state    = "STOP";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_PREVIOUS:
                state    = "PREVIOUS";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_NEXT:
                state    = "NEXT";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::STATE);
                break;
            case MediaPlayerDef::C_VOLUME_SET:
                state    = QString::number(param.toInt());
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::VOLUME);
                break;
            case MediaPlayerDef::C_VOLUME_UP:
                state    = "UP";
                ohitemId = lookupPlayerItem(entityId, MediaPlayerDef::VOLUME);
                break;
            case MediaPlayerDef::C_VOLUME_DOWN:
                state    = "UP";
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
