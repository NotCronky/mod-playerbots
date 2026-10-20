/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PullSafety.h"

#include <algorithm>
#include <unordered_map>

#include "Creature.h"
#include "CreatureGroups.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "SmartScriptMgr.h"
#include "World.h"

namespace
{
// How far a creature's own scripts call for help: SMART_ACTION_CALL_FOR_HELP's radius, or the flee for help radius
// (SMART_ACTION_FLEE_FOR_ASSIST). Written once by Load(), read-only afterwards.
std::unordered_map<uint32, float> scriptedHelpRange;

// Distance from a point to the segment a-b, in the plane.
float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
{
    float dx = bx - ax;
    float dy = by - ay;
    float lengthSquared = dx * dx + dy * dy;
    float t = lengthSquared > 0.0f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    float cx = ax + t * dx - px;
    float cy = ay + t * dy - py;
    return std::sqrt(cx * cx + cy * cy);
}

// Would answer a call for help from the target (Creature::CanAssistTo, without the target's own state).
bool Answers(Creature* helper, Creature* target)
{
    return helper->GetFaction() == target->GetFaction() && helper->HasReactState(REACT_AGGRESSIVE) &&
           !helper->IsCivilian() && !helper->GetCharmerOrOwnerGUID() &&
           !helper->GetCreatureTemplate()->HasFlagsExtra(CREATURE_FLAG_EXTRA_IGNORE_ALL_ASSISTANCE_CALLS);
}
}  // namespace

void PullSafety::Load()
{
    scriptedHelpRange.clear();
    float flee = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS);
    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
    {
        if (creatureTemplate.AIName != "SmartAI")
            continue;

        float range = 0.0f;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(entry, SMART_SCRIPT_TYPE_CREATURE))
        {
            if (holder.action.type == SMART_ACTION_CALL_FOR_HELP)
                range = std::max(range, static_cast<float>(holder.action.callHelp.range));
            else if (holder.action.type == SMART_ACTION_FLEE_FOR_ASSIST)
                range = std::max(range, flee);
        }
        if (range > 0.0f)
            scriptedHelpRange[entry] = range;
    }
}

PullAdds PullSafety::Count(Player* bot, Unit* target, GuidVector const& nearby, PlayerbotAI* botAI)
{
    PullAdds adds;
    Creature* targetCreature = target->ToCreature();
    float helpRange = PACK_RADIUS + FIGHT_DRIFT;
    CreatureGroup* formation = targetCreature ? targetCreature->GetFormation() : nullptr;
    if (targetCreature)
        if (auto scripted = scriptedHelpRange.find(targetCreature->GetEntry()); scripted != scriptedHelpRange.end())
            helpRange = std::max(helpRange, scripted->second);

    for (ObjectGuid const& guid : nearby)
    {
        if (guid == target->GetGUID())
            continue;

        Unit* unit = botAI->GetUnit(guid);
        Creature* other = unit ? unit->ToCreature() : nullptr;
        // Only red creatures come along: a yellow (neutral) one never joins on its own.
        if (!other || !other->IsAlive() || other->IsInCombat() || other->IsInEvadeMode() ||
            other->GetReactionTo(bot) > REP_HOSTILE)
            continue;

        float toTarget = other->GetExactDist2d(target);
        // Its pack: its formation wherever they stand, and those its calls for help reach.
        if (targetCreature && ((formation && other->GetFormation() == formation) ||
                               (toTarget <= helpRange && Answers(other, targetCreature))))
        {
            ++adds.pack;
            continue;
        }

        // A stray: aggressive and hostile, with the fight (at the target), the walk there or its patrol within its
        // aggro range.
        if (!other->HasReactState(REACT_AGGRESSIVE) || !other->IsHostileTo(bot))
            continue;
        float aggro = other->GetAggroRange(bot);
        if (other->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
            aggro += PATROL_MARGIN;
        if (toTarget <= aggro ||
            SegmentDistance(other->GetPositionX(), other->GetPositionY(), bot->GetPositionX(), bot->GetPositionY(),
                            target->GetPositionX(), target->GetPositionY()) <= aggro)
            ++adds.strays;
    }
    return adds;
}

bool PullSafety::Acceptable(Player* bot, PullAdds const& adds)
{
    if (PlayerbotAI::IsTank(bot, true))
        return !adds.strays && adds.pack <= TANK_PACK_ADDS;
    return !adds.Total();
}
