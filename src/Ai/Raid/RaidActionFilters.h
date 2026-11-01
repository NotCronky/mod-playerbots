/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef _PLAYERBOT_RAIDACTIONFILTERS_H
#define _PLAYERBOT_RAIDACTIONFILTERS_H

#include "DKActions.h"
#include "DruidBearActions.h"
#include "DruidCatActions.h"
#include "GenericSpellActions.h"
#include "PaladinActions.h"
#include "RogueActions.h"
#include "ShamanActions.h"
#include "WarriorActions.h"

// Damage that reaches units other than its target. The Aoe threat type alone does not answer this: every heal
// carries it, and melee splash such as Cleave, Sweeping Strikes and Blade Flurry does not.
inline bool IsRaidSplashAction(Action* action)
{
    if (action->getThreatType() == Action::ActionThreatType::Aoe && !dynamic_cast<CastHealingSpellAction*>(action))
        return true;

    return dynamic_cast<CastCleaveAction*>(action) || dynamic_cast<CastThunderClapAction*>(action) ||
           dynamic_cast<CastSweepingStrikesAction*>(action) || dynamic_cast<CastBladestormAction*>(action) ||
           dynamic_cast<CastShockwaveAction*>(action) || dynamic_cast<CastWhirlwindAction*>(action) ||
           dynamic_cast<CastBladeFlurryAction*>(action) || dynamic_cast<CastKillingSpreeAction*>(action) ||
           dynamic_cast<CastDivineStormAction*>(action) || dynamic_cast<CastHammerOfTheRighteousAction*>(action) ||
           dynamic_cast<CastBloodBoilAction*>(action) || dynamic_cast<CastHowlingBlastAction*>(action) ||
           dynamic_cast<CastFireNovaAction*>(action) || dynamic_cast<CastSwipeBearAction*>(action) ||
           dynamic_cast<CastSwipeCatAction*>(action);
}

#endif
