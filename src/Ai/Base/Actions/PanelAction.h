/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PANELACTION_H
#define PLAYERBOTS_PANELACTION_H

#include "Action.h"
#include "PanelProtocol.h"

// The bot panel's actions ("panel ..." from Script/Panel/PlayerbotsPanelScript.cpp), run on the bot's own update so
// the factory works on its map thread: set a role (talents, then gear, then maintenance), gear, maintain, refresh.
// Each step reports to the panel; nothing goes to chat.
class PanelAction : public Action
{
public:
    PanelAction(PlayerbotAI* botAI) : Action(botAI, "panel") {}

    bool Execute(Event event) override;

private:
    bool SetTalents(Player* owner, uint32 request, int32 specNo);
    bool Gear(Player* owner, uint32 request, PanelProtocol::GearProfile const& profile);
    bool Maintain(Player* owner, uint32 request);
    void Report(Player* owner, uint32 request, std::string const& step, bool ok, std::string const& detail = "");
};

#endif
