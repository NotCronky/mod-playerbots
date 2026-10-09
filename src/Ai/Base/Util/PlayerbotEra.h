/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTERA_H
#define PLAYERBOTS_PLAYERBOTERA_H

#include "Define.h"

class SpellInfo;

// The realm's era, from mod-progressive when it is installed (one realm per era, each with its own spells).
// Class strategies use it where 1.12 or 2.4.3 plays differently from 3.3.5; without mod-progressive every realm is
// WotLK.
namespace PlayerbotEra
{
    bool IsVanilla();
    bool IsTbc();
    // Vanilla or TBC: where 1.12 and 2.4.3 play alike and 3.3.5 differs.
    bool IsClassic();
    // Whether the effect heals its target directly: a heal effect, or on vanilla Flash of Light's script effect (its
    // amount is the heal, which mod-progressive's script casts through 19993).
    bool IsDirectHealEffect(SpellInfo const* spellInfo, uint8 effIndex);
}

#endif
