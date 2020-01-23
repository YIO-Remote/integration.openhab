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

#pragma once

#include <QColor>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>

#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-interface/notificationsinterface.h"
#include "yio-interface/plugininterface.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

const bool USE_WORKER_THREAD = false;

class OpenHABPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "openhab.json")

 public:
    OpenHABPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

class OpenHAB : public Integration {
    Q_OBJECT

 public:
    explicit OpenHAB(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                     YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    Q_INVOKABLE void connect() override;
    Q_INVOKABLE void disconnect() override;
    Q_INVOKABLE void leaveStandby() override;
    Q_INVOKABLE void enterStandby() override;
    Q_INVOKABLE void sendCommand(const QString& type, const QString& entity_id, int command,
                                 const QVariant& param) override;

 private slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void onPollingTimer();
    void onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility);

 private:
    enum OHType { Switch, Dimmer, Player };
    struct OHConfiguration {
        OHType ohType;
    };
    struct OHPlayer {
        OHPlayer() : found(false) {}
        bool found;
    };
    struct OHPlayerItem {
        OHPlayerItem() {}
        OHPlayerItem(const QString& playerId, MediaPlayerDef::Attributes attr) : playerId(playerId), attribute(attr) {}
        QString                    playerId;  // Entityid of player
        MediaPlayerDef::Attributes attribute;
    };

    void getThings();
    void getItems(bool first = false);
    void jsonError(const QString& error);
    void processThings(const QJsonDocument& result);
    void processItems(const QJsonDocument& result, bool first);
    void processItem(const QJsonObject& item, EntityInterface* entity);
    void processPlayerItem(const QJsonObject& item, const QString& name);
    void processLight(const QJsonObject& item, EntityInterface* entity, bool isDimmer);
    void processBlind(const QJsonObject& item, EntityInterface* entity);
    void initializePlayer(const QString& entityId, OHPlayer& player,  // NOLINT we need a non-const reference
                          const QJsonObject& json);
    void openHABCommand(const QString& itemId, const QString& state);

    const QString* lookupPlayerItem(const QString& entityId, MediaPlayerDef::Attributes attr);

 private:
    QTimer                         _pollingTimer;
    int                            _pollingInterval;
    QString                        _url;
    QNetworkAccessManager          _nam;
    QList<EntityInterface*>        _myEntities;       // Entities of this integration
    QMap<QString, OHConfiguration> _ohConfiguration;  // OpenHAB items configuration
    QMap<QString, OHPlayer>        _ohPlayers;        // YIO player entities
    QMap<QString, OHPlayerItem>    _ohPlayerItems;    // OpenHAB items associated with player
};