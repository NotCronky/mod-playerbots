/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidBearActions.h"
#include "PlayerbotEra.h"
#include "Playerbots.h"

bool CastGrowlAction::isUseful()
{
    // 1.12 and 2.4.3 Growl is melee range (20 yards from 3.0): out of reach, the bear closes in first.
    if (PlayerbotEra::IsClassic())
    {
        Unit* target = GetTarget();
        if (!target || !bot->IsWithinMeleeRange(target))
            return false;
    }

    return CastSpellAction::isUseful();
}

bool CastMaulAction::isUseful()
{
    return CastMeleeSpellAction::isUseful() && AI_VALUE2(uint8, "rage", "self target") >= 45;
}
