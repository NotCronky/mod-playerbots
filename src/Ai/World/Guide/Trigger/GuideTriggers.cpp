/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideTriggers.h"

#include <algorithm>

#include "PlayerbotAI.h"

bool GuideTaskTrigger::IsActive()
{
    GuideInfo const& info = botAI->guideInfo;
    if (!info.active || !IsSelfBot(bot))
        return false;

    GuideTask const& task = info.task;
    switch (_kind)
    {
        case Kind::Move:
            return (task.giver && !task.interact) || task.moveTo != WorldPosition() || task.wander;
        case Kind::Interact:
            return task.giver && task.interact && task.type != GuideTaskType::Service;
        case Kind::Service:
            return task.giver && task.interact && task.type == GuideTaskType::Service;
        case Kind::Use:
            return task.type == GuideTaskType::Use && task.castNow;
        case Kind::Hearth:
            return task.type == GuideTaskType::Hearth;
        case Kind::Ride:
            return task.ride;
        case Kind::Open:
            return task.open;
        case Kind::Avoid:
            return task.type == GuideTaskType::Avoid && task.moveTo != WorldPosition();
        case Kind::Rest:
            return task.type == GuideTaskType::Rest;
        case Kind::Attack:
            return !task.target.IsEmpty();
    }
    return false;
}

bool GuideDeathTrigger::IsActive()
{
    if (!botAI->guideInfo.active || !IsSelfBot(bot) || bot->IsAlive() || bot->InBattleground())
        return false;

    bool ghost = bot->HasPlayerFlag(PLAYER_FLAGS_GHOST);
    return _ghost ? ghost && bot->GetCorpse() : !ghost;
}

bool GuideUseNowTrigger::IsActive()
{
    GuideInfo const& info = botAI->guideInfo;
    if (!info.active || !info.useHealth || !IsSelfBot(bot))
        return false;
    Unit* victim = bot->GetVictim();
    if (!victim || !victim->IsAlive() || victim->GetHealthPct() > info.useHealth ||
        std::find(info.useUnits.begin(), info.useUnits.end(), victim->GetEntry()) == info.useUnits.end())
        return false;
    return bot->HasItemCount(info.useItem) && !bot->HasSpellCooldown(info.useSpell) && !bot->IsNonMeleeSpellCast(false);
}
