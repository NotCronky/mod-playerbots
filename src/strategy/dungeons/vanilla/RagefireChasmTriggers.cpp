#include "Playerbots.h"
#include "RagefireChasmTriggers.h"
#include "AiObject.h"
#include "AiObjectContext.h"

bool OggleflintPriorityTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "oggleflint");
    return boss && boss->isTargetableForAttack();
}

bool TaragamanFireNovaTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss) { return false; }

    return boss->FindCurrentSpellBySpellId(SPELL_FIRE_NOVA) != nullptr;
}

bool TaragamanUppercutTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss || !botAI->IsTank(bot)) { return false; }

    return boss->FindCurrentSpellBySpellId(SPELL_UPPERCUT) != nullptr;
}

bool JergoshHealingWaveTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "jergosh the invoker");
    if (!boss) { return false; }

    return boss->FindCurrentSpellBySpellId(SPELL_HEALING_WAVE) != nullptr;
}

bool BazzalanPoisonTrigger::IsActive()
{
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (unit && unit->HasAura(SPELL_POISON))
        {
            return true;
        }
    }
    return false;
}

bool BazzalanPriorityTrigger::IsActive()
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bazzalan");
    return boss && boss->isTargetableForAttack();
}