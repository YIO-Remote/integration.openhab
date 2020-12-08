/******************************************************************************
 *
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

#include "openhab_channelmappings.h"
#include <QMap>
#include "yio-interface/entities/mediaplayerinterface.h"

/********************************************************************
 * Media Player
 ********************************************************************/

// Mapping of OpenHAB item channel to the YIO media player attribute
const QMap<QString, MediaPlayerDef::Attributes> MediaPlayerChannels::channels = {
    // STATE
    {"power", MediaPlayerDef::STATE},
    {"control", MediaPlayerDef::STATE},
    {"state", MediaPlayerDef::STATE},

    // SOURCE
    {"mode", MediaPlayerDef::SOURCE},

    // VOLUME
    {"volume", MediaPlayerDef::VOLUME},
    {"volume-percent", MediaPlayerDef::VOLUME},

    // MUTED
    {"mute", MediaPlayerDef::MUTED},

    // MEDIAARTIST
    {"artist", MediaPlayerDef::MEDIAARTIST},
    {"play-info-name", MediaPlayerDef::MEDIAARTIST},

    // MEDIATITLE
    {"title", MediaPlayerDef::MEDIATITLE},
    {"play-info-text", MediaPlayerDef::MEDIATITLE},

    // MEDIAPROGRESS
    {"currentPlayingTime", MediaPlayerDef::MEDIAPROGRESS},

    // MEDIADURATION
    {"duration", MediaPlayerDef::MEDIADURATION},
};

// mandatory channels for the media player entity auto discover
// a OpenHAB thing must have these item channels for the auto discovery
const QList<MediaPlayerDef::Attributes> MediaPlayerChannels::mandatory = {
    MediaPlayerDef::STATE
};

// number of additional channels a OpenHAB thing which are mapped to YIO attribute must have for auto discovery
const int MediaPlayerChannels::channelcount = 2;


/********************************************************************
 * Complex Lights (with color or color temperature)
 ********************************************************************/

// Mapping of OpenHAB item channel to the YIO media player attribute
const QMap<QString, LightDef::Attributes> LightChannels::channels = {
    // BRIGHTNESS
    {"brightness", LightDef::BRIGHTNESS},

    // COLOR
    {"color", LightDef::COLOR},

    // COLORTEMPERATURE
    {"colorTemperature", LightDef::COLORTEMP}
};

// mandatory channels for the complex light entity auto discover
// a OpenHAB thing must have these item channels for the auto discovery
const QList<LightDef::Attributes> LightChannels::mandatory = {
   //LightDef::STATE
};



// number of additional channels a OpenHAB thing which are mapped to YIO attribute must have for auto discovery
const int LightChannels::channelcount = 1;


