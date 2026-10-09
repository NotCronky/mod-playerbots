/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidShapeshiftActions.h"
#include "PlayerbotEra.h"
#include "Playerbots.h"

bool CastBearFormAction::isUseful()
{
    return CastBuffSpellAction::isUseful() && !botAI->HasAura("dire bear form", GetTarget());
}

bool CastBearFormAction::isPossible()
{
    return CastBuffSpellAction::isPossible() && !botAI->HasAura("dire bear form", GetTarget());
}

std::vector<NextAction> CastDireBearFormAction::getAlternatives()
{
    return NextAction::merge({NextAction("bear form")}, CastSpellAction::getAlternatives());
}

bool CastTravelFormAction::isUseful()
{
    bool firstmount = bot->GetLevel() >= 20;

    // useful if no mount or with wsg flag
    return !bot->IsMounted() && (!firstmount || (bot->HasAura(23333) || bot->HasAura(23335) || bot->HasAura(34976))) &&
           !botAI->HasAura("dash", bot);
}

bool CastCasterFormAction::Execute(Event /*event*/)
{
    botAI->RemoveShapeshift();
    // RemoveShapeshift keeps Tree of Life; isUseful only asks for this out of combat on the classic realms.
    if (PlayerbotEra::IsClassic() && !bot->IsInCombat())
        botAI->RemoveAura("tree of life");
    return true;
}

bool CastCasterFormAction::isUseful()
{
    // 2.4.3's Tree of Life blocks Mark of the Wild, Thorns and Omen of Clarity: out of combat the druid leaves it to
    // buff (TBC census: 450+ refusals each). Not in combat, where the heals this prerequisite also guards are
    // castable in the tree and leaving it would cost it.
    bool const leaveTree = PlayerbotEra::IsClassic() && !bot->IsInCombat() && botAI->HasAura("tree of life", bot);
    if (!leaveTree &&
        !botAI->HasAnyAuraOf(GetTarget(), "dire bear form", "bear form", "cat form", "travel form", "aquatic form",
                             "flight form", "swift flight form", "moonkin form", nullptr))
        return false;

    // 1.12 forms block Innervate, Rebirth and the cures outright, so the druid leaves the form whatever its mana:
    // that is when Innervate matters most.
    if (PlayerbotEra::IsVanilla())
        return true;

    return AI_VALUE2(uint8, "mana", "self target") > sPlayerbotAIConfig.mediumHealth;
}

bool CastCancelDruidAction::Execute(Event /*event*/)
{
    botAI->RemoveAura(auraName);
    return true;
}

bool CastCancelDruidAction::isUseful() { return bot->HasAura(auraId); }

bool CastTreeFormAction::isUseful()
{
    constexpr uint32 SPELL_TREE_OF_LIFE = 33891;
    return GetTarget() && CastSpellAction::isUseful() && !bot->HasAura(SPELL_TREE_OF_LIFE);
}
