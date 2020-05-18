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
