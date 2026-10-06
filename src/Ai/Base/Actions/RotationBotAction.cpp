/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationBotAction.h"

#include "Playerbots.h"
#include "RotationBotSupport.h"

namespace
{
    // Out of combat the profile is only asked this often: buffs, forms and pets don't need more.
    constexpr uint32 PRECOMBAT_INTERVAL_MS = 1000;
}

bool RotationBotAction::isUseful()
{
    if (!inCombat && getMSTime() < nextPrecombatMs)
        return false;

    return PlayerbotRotation::IsUsed(bot);
}

bool RotationBotAction::Execute(Event /*event*/)
{
#ifdef PLAYERBOTS_ROTATION_BOT
    if (!inCombat)
        nextPrecombatMs = getMSTime() + PRECOMBAT_INTERVAL_MS;

    RotationBotBridge::Orders orders;
    orders.Target = inCombat ? AI_VALUE(Unit*, "current target") : nullptr;
    orders.InCombat = inCombat;
    orders.Tank = PlayerbotRotation::IsTank(bot);

    Optional<RotationBotBridge::Decision> decision = RotationBotBridge::Decide(bot, orders);
    if (!decision)
        return false;

    // The bot AI casts spells itself: it stands up, faces the target and records the cast.
    bool cast = decision->CastItem ? RotationBotBridge::UseItem(bot, *decision) :
        botAI->CastSpell(decision->SpellId, decision->Target);

    LOG_DEBUG("playerbots", "{} rotation: {} {} on {}", bot->GetName(), cast ? "cast" : "failed", decision->Text,
        decision->Target ? decision->Target->GetName() : "nobody");
    return cast;
#else
    return false;
#endif
}
