/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include <unordered_map>

#include "CharacterPackets.h"
#include "Language.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldSession.h"

class PlayerbotsSelfBotAfkServerScript : public ServerScript
{
public:
    PlayerbotsSelfBotAfkServerScript()
        : ServerScript("PlayerbotsSelfBotAfkServerScript", {SERVERHOOK_CAN_PACKET_RECEIVE})
    {
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (packet.GetOpcode() == CMSG_MESSAGECHAT)
        {
            MarkAfkInCombat(session, packet);
            return true;
        }
        if (packet.GetOpcode() != CMSG_LOGOUT_REQUEST)
            return true;

        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !IsSelfBot(player))
            return true;

        // Refused, the client's idle logout asks again many times a second. AFK can go meanwhile (any chat ends it:
        // an addon's level-up announcement), so the asks that keep coming are refused too; one after a pause (the
        // player back at the keyboard) goes through. Logout requests are handled on the world thread only.
        uint32 now = getMSTime();
        auto refused = _logoutRefusedAt.find(player->GetGUID());
        bool retrying = refused != _logoutRefusedAt.end() && getMSTimeDiff(refused->second, now) <= LOGOUT_RETRY_GAP;
        if (!player->isAFK() && !retrying)
        {
            _logoutRefusedAt.erase(player->GetGUID());
            return true;
        }
        _logoutRefusedAt[player->GetGUID()] = now;

        LOG_DEBUG("playerbots", "SelfBot {} stays in world: refused the AFK logout{}", player->GetName(),
                  player->isAFK() ? "" : " (asked again, no longer AFK)");

        WorldPackets::Character::LogoutResponse logoutResponse;
        logoutResponse.LogoutResult = 2;
        logoutResponse.Instant = false;
        session->SendPacket(logoutResponse.Write());

        return false;
    }

private:
    static constexpr uint32 LOGOUT_RETRY_GAP = 5 * IN_MILLISECONDS;  // asked again within this: still the idle logout

    std::unordered_map<ObjectGuid, uint32> _logoutRefusedAt;  // the last refused logout request's time

    // The client's own "/afk" after a few idle minutes is dropped by the core while in combat, and a SelfBot is in
    // combat often: the character would never turn AFK, and the client's idle logout later would be let through.
    // Marked AFK here instead (the core's handler then does nothing with it).
    static void MarkAfkInCombat(WorldSession* session, WorldPacket const& packet)
    {
        Player* player = session ? session->GetPlayer() : nullptr;
        if (!player || !IsSelfBot(player) || !player->IsInCombat() || player->isAFK())
            return;

        WorldPacket data(packet);
        data.rpos(0);
        if (data.size() < 8)
            return;
        uint32 type = 0;
        uint32 lang = 0;
        data >> type >> lang;
        if (type != CHAT_MSG_AFK)
            return;
        std::string message;
        if (data.rpos() < data.size())
            data >> message;

        player->autoReplyMsg = message.empty() ? session->GetAcoreString(LANG_PLAYER_AFK_DEFAULT) : message;
        if (player->isDND())
            player->ToggleDND();
        player->ToggleAFK();
        LOG_DEBUG("playerbots", "SelfBot {} marked AFK in combat (the core skips it)", player->GetName());
    }
};

void AddPlayerbotsSelfBotAfkScripts() { new PlayerbotsSelfBotAfkServerScript(); }
