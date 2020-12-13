/******************************************************************************
 *
 * Copyright (C) 2019 Christian Riedl <ric@rts.co.at>
 * Copyright (C) 2020 Andreas Mroß <andreas@mross.pw>
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
#include <QSet>
#include <QString>
#include <QtDebug>

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
            // qCDebug(m_logCategory) << QString(doc.toJson(QJsonDocument::Compact));
            // qCDebug(m_logCategory) << "read " << rawData.size() << "bytes";
            qCCritical(m_logCategory) << "SSE JSON error:" << parseerror.errorString();
            continue;
        }

        QVariantMap map = doc.toVariant().toMap();
        // only process state changes
        if (!(map.value("type").toString().endsWith("ItemStateEvent"))) continue;
        // if (!(map.value("topic").toString().endsWith("state"))) continue;
        // get item name from the topic string
        // example: smarthome/items/EG_Esszimmer_Sonos_CurrentPlayingTime/state
        QString          name = map.value("topic").toString().split('/')[2];
        EntityInterface* entity = m_entities->getEntityInterface(name);
        if (entity != nullptr && entity->connected()) {

            QString value =
                    (reinterpret_cast<const QVariantMap*>(map.value("payload").data()))->value("value").toString();
            // because OpenHab doesn't send the item type in the status update, we have to extract it from our own
            // entity library
            if (entity->type() == "light" && entity->supported_features().contains("BRIGHTNESS") &&
                    regex_brightnessvalue.exactMatch(value)) {
                processLight(value, entity, true);
            } else if (entity->type() == "light" && entity->supported_features().contains("COLOR") &&
                       regex_colorvalue.exactMatch(value)) {
                processComplexLight(value, entity);
            } else if (entity->type() == "light") {
                processLight(value, entity, false);
            } else if (entity->type() == "blind") {
                processBlind(value, entity);
            } else if (entity->type() == "switch") {
                processSwitch(value, entity);
            }
            /*} else if (_ohPlayerItems.contains(name) && _ohPlayers[_ohPlayerItems[name].playerId].connected) {
            QString value =
                    (reinterpret_cast<const QVariantMap*>(map.value("payload").data()))->value("value").toString();
            processPlayerItem(value, name);
        } else if (_ohLightItems.contains(name) && _ohLights[_ohLightItems[name].lightId].connected) {
            QString value =
                    (reinterpret_cast<const QVariantMap*>(map.value("payload").data()))->value("value").toString();
            //processComplexLight(value, name);
        }*/
        } else if (entity == nullptr) {
            // qCDebug(m_logCategory) << QString("openHab Item %1 is not configured").arg(name);
        } else {
            qCDebug(m_logCategory) << QString("Entity %1 is offline").arg(name);
        }
    }

    if (_wasDisconnected) {
        _wasDisconnected = false;
        getItems(true);
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

        m_notifications->add(
                    true, tr("Cannot connect to ").append(friendlyName()).append("."), tr("Reconnect"),
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

    // int i = m_entities->getByIntegration(integrationId()).count();
    /*for ( int i = 0; i < m_entities->getByIntegration(integrationId()).count(); ++i ) {
        _myEntities.insert(m_entities->getByIntegration(integrationId())[i], false);
    }
*/

    // Look for mediaplayers
    /*for (QList<EntityInterface*>::iterator i = _myEntities.begin(); i != _myEntities.end(); ++i) {
        if ((*i)->type() == "media_player") {
            _ohPlayers.insert((*i)->entity_id(), OHPlayer());
        }
    }*/
    // if (_ohPlayers.count() > 0) {
    // getThings();
    // } else {
    //    getItems(true);
    // }
    getItems(true);

    _tries = 0;
    _userDisconnect = false;
    _wasDisconnected = false;
    _standby = false;
}

void OpenHAB::disconnect() {
    _userDisconnect = true;

    if (_sseReply->isRunning()) {
        _sseReply->abort();
    }

    _pollingTimer.stop();
    _sseReconnectTimer->stop();

    setState(DISCONNECTED);
}

void OpenHAB::enterStandby() {
    _standby = true;

    if (_sseReply->isRunning()) {
        _sseReply->abort();
    }
    _pollingTimer.start();
}

void OpenHAB::leaveStandby() {
    _standby = false;

    _pollingTimer.stop();
    startSse();
    getItems(false);
}

void OpenHAB::jsonError(const QString& error) {
    qCWarning(m_logCategory) << "JSON error " << error;
}

void OpenHAB::onPollingTimer() {
    getItems(false);
}

void OpenHAB::onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility) {
    qCInfo(m_logCategory) << "network accessibility" << accessibility;
}

/*void OpenHAB::getThings() {
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
        searchThings(doc);
        getItems(true);
    });
    getItems(true);
}
*/
/*void OpenHAB::searchThings(const QJsonDocument& result) {
    // get all things
    QJsonArray array = result.array();
    for (QJsonArray::iterator i = array.begin(); i != array.end(); ++i) {
        QJsonObject item = i->toObject();
        QString     uid = item.value("UID").toString();
        // search new things
        QJsonArray channels = item.value("channels").toArray();
        int        mp_mandatory = 0, li_mandatory = 0;
        int        mp_optional = 0, li_optional = 0;
        auto       mp_manchan = MediaPlayerChannels::mandatory;
        auto       li_manchan = LightChannels::mandatory;

        QStringList mp_features = {"PLAY", "PAUSE", "NEXT", "PREVIOUS", "STOP"};
        QStringList li_features = {"BRIGHTNESS"};
        for (QJsonArray::iterator i = channels.begin(); i != channels.end(); ++i) {
            QJsonObject channel = i->toObject();
            QJsonArray  linkedItems = channel.value("linkedItems").toArray();
            if (linkedItems.count() == 0) {
                continue;
            }
            QString id = channel.value("id").toString();

            // search for media players
            if (mp_manchan.contains(MediaPlayerChannels::channels[id])) {
                mp_manchan.removeAll(MediaPlayerChannels::channels[id]);
                mp_mandatory++;
            } else if (MediaPlayerChannels::channels.contains(id)) {
                mp_optional++;
                if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::VOLUME) {
                    mp_features << "VOLUME"
                                << "VOLUME_SET";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::SOURCE) {
                    mp_features << "SOURCE";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::MUTED) {
                    mp_features << "MUTED";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::MEDIAARTIST) {
                    mp_features << "MEDIA_ARTIST";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::MEDIATITLE) {
                    mp_features << "MEDIA_TITLE";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::MEDIAPROGRESS) {
                    mp_features << "MEDIA_POSITION";
                } else if (MediaPlayerChannels::channels.value(id) == MediaPlayerDef::MEDIADURATION) {
                    mp_features << "MEDIA_DURATION";
                }
            }

            // search for lights
            if (li_manchan.contains(LightChannels::channels[id])) {
                li_manchan.removeAll(LightChannels::channels[id]);
                li_mandatory++;
            } else if (LightChannels::channels.contains(id)) {
                li_optional++;
                if (LightChannels::channels.value(id) == LightDef::COLOR) {
                    li_features << "COLOR";
                } else if (LightChannels::channels.value(id) == LightDef::COLORTEMP) {
                    mp_features << "COLORTEMP";
                }
            }
        }

        // check if it is a valid media player
        if ((mp_mandatory == MediaPlayerChannels::mandatory.length()) &&
                (mp_optional >= MediaPlayerChannels::channelcount)) {
            // add the media player to the list
            addAvailableEntity(uid, "media_player", integrationId(), item.value("label").toString(), mp_features);

            // add the channels from the recognized media player to the channel list
            for (QJsonArray::iterator i = channels.begin(); i != channels.end(); ++i) {
                QJsonObject channel = i->toObject();
                QJsonArray  linkedItems = channel.value("linkedItems").toArray();
                if (linkedItems.count() == 0) {
                    continue;
                }

                QString linkItem = linkedItems[0].toString();
                QString id = channel.value("id").toString();
                if (MediaPlayerChannels::channels.contains(id)) {
                    _ohPlayerItems.insert(linkItem, OHPlayerItem(uid, MediaPlayerChannels::channels[id]));
                }
            }

            // add player to own player list and set it as connected if we have it in our entity list
            _ohPlayers.insert(uid, OHPlayer());
            for (QList<EntityInterface*>::iterator i = _myEntities.begin(); i != _myEntities.end(); ++i) {
                if (((*i)->type() == "media_player") && ((*i)->entity_id() == uid)) {
                    _ohPlayers[uid].connected = true;
                }
            }
        }

        // check if it is a valid complex light
        if ((li_mandatory == LightChannels::mandatory.length()) && (li_optional >= LightChannels::channelcount)) {
            // add the light to the list
            addAvailableEntity(uid, "light", integrationId(), item.value("label").toString(), li_features);

            // add the channels from the recognized media player to the channel list
            for (QJsonArray::iterator i = channels.begin(); i != channels.end(); ++i) {
                QJsonObject channel = i->toObject();
                QJsonArray  linkedItems = channel.value("linkedItems").toArray();
                if (linkedItems.count() == 0) {
                    continue;
                }

                QString linkItem = linkedItems[0].toString();
                QString id = channel.value("id").toString();
                if (LightChannels::channels.contains(id)) {
                    _ohLightItems.insert(linkItem, OHLightItem(uid, LightChannels::channels[id]));
                }
            }

            // add light to own light list and set it as connected if we have it in our entity list
            _ohLights.insert(uid, OHLight());
            for (QList<EntityInterface*>::iterator i = _myEntities.begin(); i != _myEntities.end(); ++i) {
                if (((*i)->type() == "light") && ((*i)->entity_id() == uid)) {
                    _ohLights[uid].connected = true;
                }
            }
        }
    }
}*/

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
            qCDebug(m_logCategory) << answer << _url + "items";
            jsonError(parseerror.errorString());
            return;
        }
        if (first) {
            // called during connect
            setState(CONNECTED);
            // connect to the SSE source
            startSse();
        }
        processItems(doc, first);
    });
}

/*QJsonObject findValueFromJsonArray(QJsonArray arr, QString key, QString val, bool caseSensitive) {
    val = caseSensitive ? val : val.toLower();
    for (QJsonArray::iterator i = arr.begin(); i != arr.end(); ++i) {
        QJsonObject obj = i->toObject();
        QString     str = caseSensitive ? obj.value(key).toString() : obj.value(key).toString().toLower();
        if (str == val) {
            return obj;
        }
    }
    return QJsonObject();
}*/

void OpenHAB::getItem(const QString name) {
    QNetworkRequest request(_url + "items/" + name);
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");
    QNetworkReply* reply = _nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QString         answer = reply->readAll();
        QJsonParseError parseerror;

        QJsonDocument doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            jsonError(parseerror.errorString());
            return;
        }
        processItem(doc);
    });
}
void OpenHAB::processItem(const QJsonDocument& result) {
    QJsonObject json = result.object();

    for (int i = 0; i < _myEntities.size(); ++i) {
        if (json.value("name") == _myEntities[i]->entity_id()) {
            // CDebug(m_logCategory) << "Key = " << "name" << ", Value = " << json.value("name").toString();
            processEntity(json, _myEntities[i]);
        }
    }
}
void OpenHAB::processItems(const QJsonDocument& result, bool first) {
    int countFound = 0, countAll = 0;
    // QSet<QString>*   allEntities = nullptr;
    // EntityInterface* entity = nullptr;
    /*if (first) {
         Build a set of integrations entities (exclude media players)
        allEntities = new QSet<QString>();
        for (QList<EntityInterface*>::iterator i = tempEntities.begin(); i != tempEntities.end(); ++i) {
            allEntities->insert((*i)->entity_id());
        }

    }*/
    // get all items
    QJsonArray array = result.array();
    if (first) {
        // QMap<EntityInterface*, bool> tempenteties = _myEntities;
        // _myEntities.clear();
        // for ( auto key : tempenteties.keys() ) {
        // QString name_entity = key->entity_id();
        // set every entity to connected false
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
                    qCDebug(m_logCategory)<< _myEntities[i]->connected();
                    processEntity(item, _myEntities[i]);
                }
            }
        }
        if ((_myEntities.count() - countFound) > 0) {
            m_notifications->add(true,
                                 "openHAB - entities missing : " + QString::number((_myEntities.count() - countFound)));
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

    // QJsonObject item = i->toObject();
    // QString     name = item.value("name").toString();
    // countAll++;

    // if (first && !_ohPlayerItems.contains(name) && !_ohLightItems.contains(name)) {
    // QString label = (item.value("label").toString().length() > 0) ? item.value("label").toString() : name;
    // add entity to the discovered entity list
    // QString type = item.value("type").toString();

    /*if (type == "Dimmer") {
                QStringList features("BRIGHTNESS");
                addAvailableEntity(name, "light", integrationId(), label, features);
            } else if (type == "Switch") {
                QStringList features("POWER");
                addAvailableEntity(name, "switch", integrationId(), label, features);
            } else if (type == "Rollershutter") {
                QStringList features({"OPEN", "CLOSE", "STOP", "POSITION"});
                addAvailableEntity(name, "blind", integrationId(), label, features);
            }*/

    /*
        entity = m_entities->getEntityInterface(name);
        if (entity != nullptr) {
            countFound++;
            if (allEntities != nullptr) {
                // found, remove from allEntites
                allEntities->remove(name);
            }
            // process entity


            // try player
        } else if (_ohPlayerItems.contains(name) && _ohPlayers[_ohPlayerItems[name].playerId].connected) {
            if (allEntities != nullptr) {
                if (allEntities->remove(_ohPlayerItems[name].playerId)) {
                    countFound++;
                }
            }
            processPlayerItem(item.value("state").toString(), name);
            // try light
        } else if (_ohLightItems.contains(name) && _ohLights[_ohLightItems[name].lightId].connected) {
            if (allEntities != nullptr) {
                if (allEntities->remove(_ohLightItems[name].lightId)) {
                    countFound++;
                }
            }
            // processComplexLight(item.value("state").toString(), name);
        }*/
    /*

if (allEntities != nullptr) {
    // Check if we have got all entities, disconnect this entities
    for (const QString& entityId : *allEntities) {
        qCInfo(m_logCategory) << "missing: " << entityId;
        entity = m_entities->getEntityInterface(entityId);
        entity->setConnected(false);
    }
    int missing = allEntities->count();
    if (missing > 0) {

    }
    delete allEntities;
    qCInfo(m_logCategory)
            << QString("Got %1 items, %2 for YIO, %3 missing").arg(countAll).arg(countFound).arg(missing);
}*/
}

void OpenHAB::processEntity(const QJsonObject& item, EntityInterface* entity) {
    Q_ASSERT(entity != nullptr);
    // QString ohtype = item.value("type").toString();
    QStringList test = entity->supported_features();
    if (entity->connected()) {
        if (entity->type() == "light" && entity->supported_features().contains("BRIGHTNESS") && regex_brightnessvalue.exactMatch(item.value("state").toString())) {
            processLight(item.value("state").toString(), entity, true);
        }
        if (entity->type() == "light" && entity->supported_features().contains("COLOR") && regex_colorvalue.exactMatch(item.value("state").toString())) {
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
    // int  brightness;
    // bool isInt;
    // brightness = value.toInt(&isInt);
    if (!(value.contains("ON")) && !(value.contains("OFF")) && isDimmer) {
        // int  brightness = value.toInt();
        // bool on = brightness == 100;
        entity->setState(value > 0 ? LightDef::ON : LightDef::OFF);
        if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            entity->updateAttrByIndex(LightDef::BRIGHTNESS, value);
        } else {
            qCDebug(m_logCategory) << QString("OpenHab Dimmer %1 not supporting BRIGHTNESS").arg(entity->entity_id());
        }
    } else {
        // QString state = value.toUpper();
        if (value.toUpper() == "ON") {
            entity->setState(LightDef::ON);
        } else if (value.toUpper() == "OFF") {
            entity->setState(LightDef::OFF);
        } else {
            qCDebug(m_logCategory)
                    << QString("OpenHab Switch %1 undefined state %2").arg(entity->entity_id()).arg(value.toUpper());
        }
        /*if (entity->isSupported(LightDef::F_BRIGHTNESS)) {
            qCDebug(m_logCategory) << QString("OpenHab Switch %1 does not support BRIGHTNESS").arg(entity->entity_id());
        }*/
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

void OpenHAB::processComplexLight(const QString& value, EntityInterface* entity) {
    // OHLightItem      lightItem = _ohLightItems[name];
    // EntityInterface* entity = m_entities->getEntityInterface(lightItem.lightId);
    if (entity == nullptr) return;

    // if (lightItem.attribute == LightDef::BRIGHTNESS) {
    /*if (entity->supported_features().contains("BRIGHTNESS")) {
        int  brightness = value.toInt();
        bool on = brightness == 100;
        entity->setState(on ? LightDef::ON : LightDef::OFF);
        entity->updateAttrByIndex(LightDef::BRIGHTNESS, brightness);
    } else*/
    if (entity->supported_features().contains("COLOR")) {
        if (regex_colorvalue.exactMatch(value)) {
            QStringList cs = value.split(',');
            QColor      color =
                    QColor((cs[0].toInt()), ((cs[1].toInt() * 255) / 100), ((cs[2].toInt() * 255) / 100), QColor::Hsl);
            char buffer[10];
            snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", color.red(), color.green(), color.blue());
            // QColor color = QColor::fromHsv(34,45,45);
            // QString test = QString("#%1%2%3").arg(color.red(),2,16).arg(color.green(),2,16).arg(color.blue(),2,16);
            entity->updateAttrByIndex(LightDef::COLOR, buffer);
        } else if (regex_brightnessvalue.exactMatch(value) && entity->supported_features().contains("BRIGHTNESS")) {
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

/*void OpenHAB::processPlayerItem(const QString& value, const QString& name) {
    OHPlayerItem     playerItem = _ohPlayerItems[name];
    QString          state = value.toUpper();
    EntityInterface* entity = m_entities->getEntityInterface(playerItem.playerId);
    if (entity == nullptr) return;

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

const QString* OpenHAB::lookupComplexLightItem(const QString& entityId, LightDef::Attributes attr) {
    for (QMap<QString, OHLightItem>::iterator i = _ohLightItems.begin(); i != _ohLightItems.end(); ++i) {
        if (i.value().attribute == attr && i.value().lightId == entityId) {
            return &i.key();
        }
    }
    return nullptr;
}
*/
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
    openHABCommand(entityId, state);
    // getItem(entityId);
}

void OpenHAB::openHABCommand(const QString& itemId, const QString& state) {
    QNetworkRequest request(_url + "items/" + itemId);
    request.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader, "text/plain");
    request.setRawHeader("Accept", "application/json");
    QList<QByteArray> reqHeaders = request.rawHeaderList();
    foreach(QByteArray reqName, reqHeaders) {
        QByteArray reqValue = request.rawHeader(reqName);
        qCDebug(m_logCategory) << reqName << ": " << reqValue;
    }
    qCDebug(m_logCategory) << request.rawHeaderList();
    QNetworkReply* reply = _nam.post(request, state.toUtf8());
    QObject::connect(reply, &QNetworkReply::finished, this, [=]() {
        QJsonParseError parseerror;
        QString answer = reply->readAll();
        if (answer != "") {
            QJsonDocument doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                jsonError(parseerror.errorString());
                return;
            }
            qCDebug(m_logCategory) << "Reply: " << doc.toJson(QJsonDocument::Compact).toStdString().c_str();
        }
        return;
    });
}
