/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTERA_H
#define PLAYERBOTS_PLAYERBOTERA_H

// The realm's era, from mod-progressive when it is installed (one realm per era, each with its own spells).
// Class strategies use it where 1.12 plays differently from 3.3.5; without mod-progressive every realm is WotLK.
namespace PlayerbotEra
{
    bool IsVanilla();
}

#endif
