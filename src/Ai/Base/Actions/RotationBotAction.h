/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROTATIONBOTACTION_H
#define PLAYERBOTS_ROTATIONBOTACTION_H

#include "Action.h"

class PlayerbotAI;

// Casts what the bot's mod-rotation-bot profile picks next: its precombat list out of combat, the
// rest in combat. Fails when the profile has nothing ready, so lower actions (melee, shoot) run.
class RotationBotAction : public Action
{
public:
    RotationBotAction(PlayerbotAI* botAI, bool inCombat)
        : Action(botAI, inCombat ? "rotation" : "rotation precombat"), inCombat(inCombat)
    {
    }

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool inCombat;
    uint32 nextPrecombatMs = 0;
};

#endif
