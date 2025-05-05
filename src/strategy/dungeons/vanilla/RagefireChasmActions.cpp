#include "Playerbots.h"
#include "RagefireChasmActions.h"
#include "RagefireChasmStrategy.h"

bool AttackOggleflintAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "oggleflint");
    if (!boss) { return false; }

    if (AI_VALUE(Unit*, "current target") == boss) { return false; }
    return Attack(boss);
}

bool MoveAwayTaragamanAction::isUseful() { return botAI->IsTank(bot); }
bool MoveAwayTaragamanAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float safeDistance = 10.0f; // Move out of Fire Nova range
    if (distance < safeDistance)
    {
        return Move(bot->GetAngle(boss) + M_PI, safeDistance);
    }
    return false;
}

bool DodgeUppercutAction::isUseful() { return botAI->IsTank(bot); }
bool DodgeUppercutAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "taragaman the hungerer");
    if (!boss) { return false; }

    float distance = bot->GetExactDist2d(boss->GetPosition());
    float extraDistance = 5.0f; // Move slightly behind to avoid knockback
    return Move(bot->GetAngle(boss), distance + extraDistance);
}

bool InterruptHealingWaveAction::isUseful()
{
    // Check if the bot can cast an interrupt spell based on class
    switch (bot->getClass())
    {
    case CLASS_MAGE:
        return botAI->CanCastSpell("counterspell", bot);
    case CLASS_ROGUE:
        return botAI->CanCastSpell("kick", bot);
    case CLASS_WARRIOR:
        return botAI->CanCastSpell("pummel", bot);
    case CLASS_SHAMAN:
        return botAI->CanCastSpell("wind shear", bot);
    case CLASS_PRIEST:
        return botAI->CanCastSpell("silence", bot);
    case CLASS_HUNTER:
        return botAI->CanCastSpell("silencing shot", bot);
    default:
        return false; // No interrupt for this class
    }
}

bool InterruptHealingWaveAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "jergosh the invoker");
    if (!boss) { return false; }

    // Cast the appropriate interrupt spell based on class
    switch (bot->getClass())
    {
    case CLASS_MAGE:
        return botAI->CastSpell("counterspell", boss);
    case CLASS_ROGUE:
        return botAI->CastSpell("kick", boss);
    case CLASS_WARRIOR:
        return botAI->CastSpell("pummel", boss);
    case CLASS_SHAMAN:
        return botAI->CastSpell("wind shear", boss);
    case CLASS_PRIEST:
        return botAI->CastSpell("silence", boss);
    case CLASS_HUNTER:
        return botAI->CastSpell("silencing shot", boss);
    default:
        return false;
    }
}

bool CleansePoisonAction::isUseful()
{
    // Only healers with cleansing spells can use this action
    if (!botAI->IsHeal(bot)) { return false; }
    switch (bot->getClass())
    {
    case CLASS_PRIEST:
        return botAI->CanCastSpell("dispel magic", bot) || botAI->CanCastSpell("cure disease", bot);
    case CLASS_DRUID:
        return botAI->CanCastSpell("cure poison", bot) || botAI->CanCastSpell("abolish poison", bot);
    case CLASS_SHAMAN:
        return botAI->CanCastSpell("cure poison", bot);
    case CLASS_PALADIN:
        return botAI->CanCastSpell("purify", bot);
    default:
        return false;
    }
}

bool CleansePoisonAction::Execute(Event event)
{
    Unit* target = nullptr;
    GuidVector members = AI_VALUE(GuidVector, "group members");
    for (auto& member : members)
    {
        Unit* unit = botAI->GetUnit(member);
        if (unit && unit->HasAura(SPELL_POISON))
        {
            target = unit;
            break;
        }
    }
    if (!target) { return false; }

    // Cast the appropriate cleansing spell based on class
    switch (bot->getClass())
    {
    case CLASS_PRIEST:
        return botAI->CastSpell("dispel magic", target) || botAI->CastSpell("cure disease", target);
    case CLASS_DRUID:
        return botAI->CastSpell("cure poison", target) || botAI->CastSpell("abolish poison", target);
    case CLASS_SHAMAN:
        return botAI->CastSpell("cure poison", target);
    case CLASS_PALADIN:
        return botAI->CastSpell("purify", target);
    default:
        return false;
    }
}

bool AttackBazzalanAction::Execute(Event event)
{
    Unit* boss = AI_VALUE2(Unit*, "find target", "bazzalan");
    if (!boss) { return false; }

    if (AI_VALUE(Unit*, "current target") == boss) { return false; }
    return Attack(boss);
}