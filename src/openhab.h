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

#pragma once

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkConfigurationManager>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QString>
#include <QTimer>

#include "yio-interface/entities/lightinterface.h"
#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-interface/notificationsinterface.h"
#include "yio-interface/plugininterface.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

const bool NO_WORKER_THREAD = false;

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

    void sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) override;

 private slots:
    void connect() override;
    void disconnect() override;
    void leaveStandby() override;
    void enterStandby() override;

    void streamFinished(QNetworkReply* reply);
    void networkManagerFinished(QNetworkReply* reply);
    void streamReceived();
    void onSseTimeout();
    void onNetWorkAccessible(QNetworkAccessManager::NetworkAccessibility accessibility);

 private:
    void startSse();
    void getItems();
    void getSystemInfo();
    void jsonError(const QString& error);
    void processItem(const QJsonDocument& result);
    void processItems(const QJsonDocument& result, bool first);
    void processEntity(const QJsonObject& item, EntityInterface* entity);
    void processPlayerItem(const QString& item, const QString& name);
    void processLight(const QString& value, EntityInterface* entity, bool isDimmer);
    void processBlind(const QString& value, EntityInterface* entity);
    void processSwitch(const QString& value, EntityInterface* entity);
    void processComplexLight(const QString& value, EntityInterface* entity);
    void sendOpenHABCommand(const QString& itemId, const QString& state);
    void getItem(const QString name);

    const QString* lookupPlayerItem(const QString& entityId, MediaPlayerDef::Attributes attr);
    const QString* lookupComplexLightItem(const QString& entityId, LightDef::Attributes attr);

 private:
    QNetworkInterface       _iface;
    QNetworkAccessManager*  _sseNetworkManager;
    QNetworkReply*          _sseReply;
    QTimer*                 _sseReconnectTimer;
    QString                 _url;
    QString                 _token;
    int                     _networktries = 0;
    QNetworkAccessManager*  _nam;
    bool                    _flagleaveStandby = false;
    QList<EntityInterface*> _myEntities;  // Entities of this integration
    QRegExp                 regex_colorvalue =
        QRegExp("[0-9]?[0-9]?[0-9][,][0-9]?[0-9]?[0-9][,][0-9]?[0-9][.]?[0-9]?[0-9]?[0-9]?[0-9]?");
    QRegExp regex_brightnessvalue = QRegExp("[1]?[0-9]?[0-9]");
    int     _tries;
    bool    _flagStandby;
    // bool     _flagprocessitems = false;
    bool _flagOpenHabConnected = false;
    // QObject* context = new QObject(this);
    QString        _tempJSONData = "";
    OpenHAB*       context_openHab;
    bool           _flagSseConnected = false;
    bool           _flagMoreDataNeeded = false;
    QJsonDocument  doc;
    QJsonDocument  pyload;
    QByteArray     rawData;
    QByteArrayList splitted;
};
