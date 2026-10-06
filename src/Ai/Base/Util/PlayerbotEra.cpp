/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PlayerbotEra.h"

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
