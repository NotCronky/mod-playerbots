/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEra.h"
#include "SharedDefines.h"
#include "SpellInfo.h"

#if __has_include("ProgressiveEra.h")
#include "ProgressiveEra.h"
#define PLAYERBOTS_PROGRESSIVE_ERA 1
#endif

bool PlayerbotEra::IsVanilla()
{
#ifdef PLAYERBOTS_PROGRESSIVE_ERA
    return Progressive::GetRealmEra() == Progressive::Era::Vanilla;
#else
    return false;
#endif
}

bool PlayerbotEra::IsTbc()
{
#ifdef PLAYERBOTS_PROGRESSIVE_ERA
    return Progressive::GetRealmEra() == Progressive::Era::Tbc;
#else
    return false;
#endif
}

bool PlayerbotEra::IsClassic()
{
    return IsVanilla() || IsTbc();
}

bool PlayerbotEra::IsDirectHealEffect(SpellInfo const* spellInfo, uint8 effIndex)
{
    SpellEffectInfo const& effect = spellInfo->Effects[effIndex];
    if (effect.Effect == SPELL_EFFECT_HEAL)
        return true;

    return IsVanilla() && effect.Effect == SPELL_EFFECT_SCRIPT_EFFECT
        && spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN
        && effect.TargetA.GetTarget() == TARGET_UNIT_TARGET_ALLY;
}
