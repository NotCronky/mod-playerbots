/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PanelSend.h"

#include "Chat.h"
#include "PanelProtocol.h"
#include "Player.h"
#include "ServerFacade.h"
#include "WorldPacket.h"

void PanelSend::Send(Player* player, std::string_view verb, std::vector<std::string> const& fields)
{
    if (!player)
        return;

    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, LANG_ADDON, player->GetGUID(), ObjectGuid::Empty,
                                 PanelProtocol::Encode(verb, fields), CHAT_TAG_NONE, player->GetName());
    ServerFacade::instance().SendPacket(player, &data);
}
