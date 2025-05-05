#include "RagefireChasmMultipliers.h"
#include "RagefireChasmActions.h"
#include "GenericSpellActions.h"

float OggleflintMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "oggleflint");
    if (!boss) { return 1.0f; }

    // No special mechanics, allow all actions
    return 1.0f;
}

float TaragamanMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss) { return 1.0f; }

    bool isTank = botAI->IsTank(bot);
    if (isTank && boss->HasUnitState(UNIT_STATE_CASTING))
    {
        if (boss->FindCurrentSpellBySpellId(SPELL_UPPERCUT) || boss->FindCurrentSpellBySpellId(SPELL_FIRE_NOVA))
        {
            if (dynamic_cast<MovementAction*>(action) &&
                !dynamic_cast<DodgeUppercutAction*>(action) &&
                !dynamic_cast<MoveAwayTaragamanAction*>(action))
            {
                return 0.0f; // Prevent other movement actions during Uppercut or Fire Nova
            }
        }
    }
    return 1.0f;
}

float JergoshMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "jergosh the invoker");
    if (!boss) { return 1.0f; }

    if (boss->FindCurrentSpellBySpellId(SPELL_HEALING_WAVE))
    {
        if (!dynamic_cast<InterruptHealingWaveAction*>(action))
        {
            uint32 spellId = AI_VALUE2(uint32, "spell id", action->getName());
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (spellInfo && spellInfo->CalcCastTime(bot) != 0)
            {
                return 0.0f; // Prevent non-instant casts during Healing Wave
            }
        }
    }
    return 1.0f;
}

float BazzalanMultiplier::GetValue(Action* action)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bazzalan");
    if (!boss) { return 1.0f; }

    // Prioritize cleansing poison for healers
    if (dynamic_cast<CleansePoisonAction*>(action) && botAI->IsHeal(bot))
    {
        return 2.0f; // Boost priority for cleansing
    }
    return 1.0f;
}