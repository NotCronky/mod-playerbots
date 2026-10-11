/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidTriggers.h"
#include "DynamicObject.h"
#include "Player.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "SpellMgr.h"

bool MarkOfTheWildTrigger::IsActive()
{
    return BuffTrigger::IsActive() && !botAI->HasAura("gift of the wild", GetTarget());
}

bool ThornsOnPartyTrigger::IsActive()
{
    return BuffOnPartyTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget());
}

bool EntanglingRootsKiteTrigger::IsActive()
{
    return DebuffTrigger::IsActive() && AI_VALUE(uint8, "attacker count") < 3 && !GetTarget()->GetPower(POWER_MANA);
}

bool ThornsTrigger::IsActive() { return BuffTrigger::IsActive() && !botAI->HasAura("thorns", GetTarget()); }

bool BearFormTrigger::IsActive() { return !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr); }

bool TreeFormTrigger::IsActive()
{
    return !bot->HasAura(SPELL_TREE_OF_LIFE);
}

bool CatFormTrigger::IsActive() { return !botAI->HasAura("cat form", bot); }

bool HealerShouldAttackAndNotBlanketingTrigger::IsActive()
{
    if (bot->GetGroup() && bot->HasSpell(SPELL_TREE_OF_LIFE) && botAI->HasStrategy("blanketing", BOT_STATE_COMBAT))
        return false;

    return HealerShouldAttackTrigger::IsActive();
}

Unit* BlanketHotTrigger::GetTarget() { return botAI->GetUnit(AI_VALUE2(ObjectGuid, "blanket hot target", _spell)); }

bool BlanketHotTrigger::IsActive()
{
    if (!bot->GetGroup() || !botAI->HasStrategy("resto", BOT_STATE_COMBAT))
        return false;

    return GetTarget();
}

bool FerociousBiteDyingTargetTrigger::IsActive()
{
    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || AI_VALUE2(uint8, "combo", "current target") < 5)
        return false;

    return target->GetHealth() / AI_VALUE(float, "estimated group dps") <= 10.0f;
}

bool PowershiftTrigger::IsActive()
{
    constexpr uint32 FUROR_ICON = 238;
    constexpr int32 POWERSHIFT_MIN_MANA = 500;

    if (!bot->IsInCombat() || !botAI->HasAura("cat form", bot) || botAI->HasAura("clearcasting", bot))
        return false;

    if (AI_VALUE2(uint8, "energy", "self target") > 20 ||
        !bot->GetDummyAuraEffect(SPELLFAMILY_DRUID, FUROR_ICON, EFFECT_0))
        return false;

    SpellInfo const* catForm = sSpellMgr->GetSpellInfo(AI_VALUE2(uint32, "spell id", "cat form"));
    if (!catForm || bot->GetGlobalCooldownMgr().HasGlobalCooldown(catForm))
        return false;

    int32 const mana = bot->GetPower(POWER_MANA);
    return mana > POWERSHIFT_MIN_MANA && mana >= catForm->CalcPowerCost(bot, catForm->GetSchoolMask());
}

bool SwipeBearFillerTrigger::IsActive()
{
    if (!bot->IsInCombat() || !botAI->HasAnyAuraOf(bot, "bear form", "dire bear form", nullptr))
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    if (!target || !target->IsAlive() || AI_VALUE2(uint8, "rage", "self target") < 15)
        return false;

    uint32 const mangle = AI_VALUE2(uint32, "spell id", "mangle (bear)");
    return !mangle || bot->HasSpellCooldown(mangle);
}

bool LifebloomOnMainTankTrigger::IsActive()
{
    Unit* tank = AI_VALUE(Unit*, "main tank");
    return tank && tank->IsAlive() && tank->IsInCombat();
}

bool AquaticFormTrigger::IsActive()
{
    return !bot->IsInCombat() && !botAI->HasAura("aquatic form", bot) &&
           bot->GetLiquidData().Status == LIQUID_MAP_UNDER_WATER;
}

bool ProwlTrigger::IsActive()
{
    if (botAI->HasAura("prowl", bot) || bot->IsInCombat())
        return false;

    if (!botAI->HasSpell("prowl"))
        return false;

    uint32 const prowlId = AI_VALUE2(uint32, "spell id", "prowl");
    if (bot->HasSpellCooldown(prowlId))
        return false;

    float distance = 30.f;

    Unit* target = AI_VALUE(Unit*, "enemy player target");
    if (target && !target->IsInWorld())
        return false;
    if (!target)
        target = AI_VALUE(Unit*, "grind target");
    if (!target)
        target = AI_VALUE(Unit*, "dps target");
    if (!target)
        return false;

    if (target && target->GetVictim())
        distance -= 10;
    if (target->isMoving() && target->GetVictim())
        distance -= 10;
    if (bot->InBattleground())
        distance += 15;
    if (bot->InArena())
        distance += 15;

    return target && ServerFacade::instance().GetDistance2d(bot, target) < distance;
}

const std::set<uint32> HurricaneChannelCheckTrigger::HURRICANE_SPELL_IDS = {
    16914,  // Hurricane Rank 1
    17401,  // Hurricane Rank 2
    17402,  // Hurricane Rank 3
    27012,  // Hurricane Rank 4
    48467   // Hurricane Rank 5
};

bool HurricaneChannelCheckTrigger::IsActive()
{
    if (Spell* spell = bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
    {
        if (!HURRICANE_SPELL_IDS.count(spell->m_spellInfo->Id))
            return false;

        // Find this bot's own Hurricane DynamicObject
        DynamicObject* dynObj = nullptr;
        for (uint32 spellId : HURRICANE_SPELL_IDS)
        {
            dynObj = bot->GetDynObject(spellId);
            if (dynObj)
                break;
        }

        if (!dynObj)
            return false;

        // Count attackers actually inside the Hurricane AoE
        float radius = dynObj->GetRadius();
        GuidVector attackers = AI_VALUE(GuidVector, "attackers");
        uint32 count = 0;
        for (ObjectGuid const& guid : attackers)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive())
                continue;
            if (unit->GetDistance(dynObj->GetPosition()) <= radius)
                count++;
        }

        return count < minEnemies;
    }

    return false;
}
