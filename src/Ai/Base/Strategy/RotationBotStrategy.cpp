/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationBotStrategy.h"

#include "GenericSpellActions.h"
#include "Playerbots.h"
#include "RotationBotSupport.h"

uint32 RotationBotStrategy::GetType() const
{
    Player* bot = botAI->GetBot();
    uint32 type = STRATEGY_TYPE_COMBAT;

    if (PlayerbotRotation::IsTank(bot))
        type |= STRATEGY_TYPE_TANK;
    else if (PlayerbotAI::IsHeal(bot, true))
        type |= STRATEGY_TYPE_HEAL;
    else
        type |= STRATEGY_TYPE_DPS;

    type |= PlayerbotAI::IsRanged(bot, true) ? STRATEGY_TYPE_RANGED : STRATEGY_TYPE_MELEE;
    return type;
}

std::vector<NextAction> RotationBotStrategy::getDefaultActions()
{
    // The profile first; auto-attack or a wand when it has nothing ready.
    return {
        NextAction("rotation", ACTION_NORMAL + 5),
        NextAction(PlayerbotAI::IsRanged(botAI->GetBot(), true) ? "shoot" : "melee", ACTION_DEFAULT)
    };
}

void RotationBotStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    CombatStrategy::InitTriggers(triggers);

    Player* bot = botAI->GetBot();
    if (PlayerbotAI::IsRanged(bot, true))
        triggers.push_back(new TriggerNode("enemy too close for spell", { NextAction("flee", ACTION_MOVE + 4) }));
    else
        triggers.push_back(new TriggerNode("enemy out of melee", { NextAction("reach melee", ACTION_HIGH + 1) }));

    if (PlayerbotAI::IsHeal(bot, true))
        triggers.push_back(new TriggerNode("party member to heal out of spell range",
            { NextAction("reach party member to heal", ACTION_HIGH + 1) }));
}

void RotationBotStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new RotationBotMultiplier(botAI));
}

std::vector<NextAction> RotationBotNonCombatStrategy::getDefaultActions()
{
    return { NextAction("rotation precombat", ACTION_NORMAL + 2) };
}

void RotationBotNonCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new RotationBotMultiplier(botAI));
}

float RotationBotMultiplier::GetValue(Action* action)
{
    if (!action)
        return 1.0f;

    std::string const name = action->getName();
    if (name == "caster form" || name == "remove shadowform")
        return PlayerbotRotation::UsesShapeshift(bot) ? 0.0f : 1.0f;

    CastSpellAction* spellAction = dynamic_cast<CastSpellAction*>(action);
    if (!spellAction)
        return 1.0f;

    uint32 spellId = AI_VALUE2(uint32, "spell id", spellAction->getSpell());
    if (!spellId)
        return 1.0f;

    return PlayerbotRotation::Conflicts(bot, sSpellMgr->GetSpellInfo(spellId)) ? 0.0f : 1.0f;
}
