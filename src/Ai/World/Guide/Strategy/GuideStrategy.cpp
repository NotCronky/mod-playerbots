/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideStrategy.h"

#include <algorithm>

#include "GenericSpellActions.h"
#include "PlayerbotAI.h"

// Like new rpg: the update runs above the step's actions and only takes the tick when the step changes; the step's
// actions sit below looting, eating and buffing.
std::vector<NextAction> GuideStrategy::getDefaultActions() { return {NextAction("guide update", 11.0f)}; }

void GuideStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Out of a patrol's way before anything else the guide does, eating and looting included.
    triggers.push_back(new TriggerNode("guide avoid", {NextAction("guide move", ACTION_MOVE)}));
    triggers.push_back(new TriggerNode("guide hearth", {NextAction("guide hearth", 4.5f)}));
    triggers.push_back(new TriggerNode("guide ride", {NextAction("guide ride", 4.5f)}));
    triggers.push_back(new TriggerNode("guide rest", {NextAction("guide rest", 4.5f)}));
    triggers.push_back(new TriggerNode("guide attack", {NextAction("guide attack", 4.0f)}));
    triggers.push_back(new TriggerNode("guide use", {NextAction("guide use", 3.5f)}));
    triggers.push_back(new TriggerNode("guide open", {NextAction("guide open", 3.5f)}));
    triggers.push_back(new TriggerNode("guide interact", {NextAction("guide quest", 3.5f)}));
    triggers.push_back(new TriggerNode("guide service", {NextAction("guide service", 3.5f)}));
    triggers.push_back(new TriggerNode("guide move", {NextAction("guide move", 3.0f)}));
}

// Above the dead strategy's own (100): the guide's release keeps the player's repair bill.
void GuideDeathStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("guide release", {NextAction("guide release", 102.0f)}));
    triggers.push_back(new TriggerNode("guide corpse run", {NextAction("guide corpse run", 101.0f)}));
}

void GuideCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("guide use now", {NextAction("guide use now", ACTION_EMERGENCY + 5)}));
}

void GuideCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new GuideHoldMultiplier(botAI));
}

float GuideHoldMultiplier::GetValue(Action* action)
{
    GuideInfo const& info = botAI->guideInfo;
    if (!action || !info.active || !info.useHealth || !bot->HasItemCount(info.useItem))
        return 1.0f;
    Unit* victim = bot->GetVictim();
    if (!victim || victim->GetHealthPct() > info.useHealth + HOLD_MARGIN ||
        std::find(info.useUnits.begin(), info.useUnits.end(), victim->GetEntry()) == info.useUnits.end())
        return 1.0f;
    // Spells on it could kill it before the mark; auto-attack and everything else go on.
    if (dynamic_cast<CastSpellAction*>(action) && action->GetTargetName() == "current target")
        return 0.0f;
    return 1.0f;
}
