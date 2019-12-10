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

#ifndef OPENHAB_H
#define OPENHAB_H

//#include <QtPlugin>
#include <QObject>
#include <QString>
#include <QColor>
#include <QVariant>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QLoggingCategory>
#include <QJsonObject>

#include "../remote-software/sources/integrations/integration.h"
#include "../remote-software/sources/integrations/plugininterface.h"
#include "../remote-software/sources/notificationsinterface.h"
#include "../remote-software/sources/entities/mediaplayerinterface.h"

class OpenHABPlugin : public PluginInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "openhab.json")
    Q_INTERFACES(PluginInterface)

public:
    explicit OpenHABPlugin (QObject* parent = nullptr) :
        _log("openhab")
    {
        Q_UNUSED(parent)
    }

    virtual ~OpenHABPlugin () override
    {}

    void        create         (const QVariantMap& config, QObject *entities, QObject *notifications, QObject* api, QObject *configObj) override;
    void        setLogEnabled  (QtMsgType msgType, bool enable) override
    {
        _log.setEnabled(msgType, enable);
    }
private:
    QLoggingCategory    _log;
};


class OpenHAB : public Integration
{
    Q_OBJECT

public:
    explicit	OpenHAB             (QLoggingCategory& log, QObject* parent = nullptr);
    virtual     ~OpenHAB            () override;

    Q_INVOKABLE void setup  	    (const QVariantMap& config, QObject *entities, QObject *notifications, QObject* api, QObject *configObj);
    void        connect             () override;
    void        disconnect          () override;
    void        leaveStandby        () override;
    void        enterStandby        () override;
    void        sendCommand         (const QString& type, const QString& entity_id, int command, const QVariant& param) override;

private slots:
    void        onPollingTimer      ();
    void        onNetWorkAccessible (QNetworkAccessManager::NetworkAccessibility accessibility);

private:
    enum OHType { Switch, Dimmer, Player };
    struct OHConfiguration {
        OHType  ohType;
    };
    struct OHPlayer {
        OHPlayer() :
            found(false)
        {}
        bool    found;
    };
    struct OHPlayerItem {
        OHPlayerItem()
        {}
        OHPlayerItem(const QString& playerId, MediaPlayerDef::Attributes attr) :
            playerId(playerId),
            attribute(attr)
        {}
        QString                     playerId;       // Entityid of player
        MediaPlayerDef::Attributes  attribute;
    };

    void        getThings           ();
    void        getItems            (bool first = false);
    void        jsonError           (const QString& error);
    void        processThings       (const QJsonDocument& result);
    void        processItems        (const QJsonDocument& result, bool first);
    void        processItem         (const QJsonObject& item, EntityInterface* entity);
    void        processPlayerItem   (const QJsonObject& item, const QString& name);
    void        processLight        (const QJsonObject& item, EntityInterface* entity, bool isDimmer);
    void        processBlind        (const QJsonObject& item, EntityInterface* entity);
    void        initializePlayer    (const QString& entityId, OHPlayer& player, const QJsonObject& json);
    void        openHABCommand      (const QString& itemId, const QString& state);

    const QString* lookupPlayerItem (const QString& entityId, MediaPlayerDef::Attributes attr);

    QLoggingCategory&               _log;
    QTimer                          _pollingTimer;
    int                             _pollingInterval;
    NotificationsInterface*         _notifications;
    QString                         _url;
    QNetworkAccessManager           _nam;
    QList<EntityInterface*>         _myEntities;        // Entities of this integration
    QMap<QString, OHConfiguration>  _ohConfiguration;   // OpenHAB items configuration
    QMap<QString, OHPlayer>         _ohPlayers;         // YIO player entities
    QMap<QString, OHPlayerItem>     _ohPlayerItems;     // OpenHAB items associated with player
};

#endif // OPENHAB_H
