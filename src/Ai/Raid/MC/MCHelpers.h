/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MCHELPERS_H
#define PLAYERBOTS_MCHELPERS_H

#include <cmath>
#include <list>

#include "Creature.h"
#include "Object.h"

namespace MoltenCoreHelpers
{
enum MoltenCoreNPCs
{
    // Garr
    NPC_FIRESWORN = 12099,

    // Golemagg
    NPC_CORE_RAGER = 11672,

    // Majordomo Executus
    NPC_MAJORDOMO_EXECUTUS = 12018,

    // Core Hound (trash)
    NPC_CORE_HOUND = 11671,
};
enum MoltenCoreSpells
{
    // Baron Geddon
    SPELL_INFERNO = 19695,
    SPELL_LIVING_BOMB = 20475,

    // Golemagg
    SPELL_GOLEMAGGS_TRUST = 20553,
    SPELL_MAGMA_SPLASH = 13880,

    // Majordomo Executus: every 30s one of these lands on all his adds for 10s.
    SPELL_MAGIC_REFLECTION = 20619,     // reflects half of the spells cast at them
    SPELL_DAMAGE_REFLECTION = 21075,    // Damage Shield: fire damage back at each melee hit
};

// Garr: each warlock keeps one Firesworn banished, refreshed under this much time left.
constexpr int32 GARR_BANISH_REFRESH_MS = 5000;

constexpr uint32 MAGMA_SPLASH_BACK_OFF_STACKS = 20;
constexpr float MAGMA_SPLASH_BACK_OFF_DISTANCE = 12.0f;

// Shazzrah's Arcane Explosion radius
constexpr float ARCANE_EXPLOSION_DISTANCE = 26.0f;

// Majordomo Executus teleports his victim and a random raider into the lava pit of hot coals beside him (the
// Teleport spells' destination). Every death in it fell within 9yd of this spot (tr-20261007-154231-1).
constexpr float MAJORDOMO_COALS_X = 736.5f;
constexpr float MAJORDOMO_COALS_Y = -1176.35f;
constexpr float MAJORDOMO_COALS_Z = -119.0f;
constexpr float MAJORDOMO_COALS_RADIUS = 12.0f;
// Dry floor on either side of the pit: Majordomo's own spot, and the raid's side where it comes in.
constexpr float MAJORDOMO_DRY_EAST_X = 759.5f, MAJORDOMO_DRY_EAST_Y = -1173.4f, MAJORDOMO_DRY_EAST_Z = -119.3f;
constexpr float MAJORDOMO_DRY_WEST_X = 717.0f, MAJORDOMO_DRY_WEST_Y = -1165.0f, MAJORDOMO_DRY_WEST_Z = -119.5f;

// Garr: a Firesworn erupts as it dies (Eruption, 19497: 15yd, ~2k fire and a knockback; four times that under
// Separation Anxiety). Ten raiders died to Eruptions meleeing the leftovers in one pile after Garr fell
// (tr-20261007-213207-1, 7-8.5k each). Below this health everyone but the tanks stands clear and ranged finish it.
constexpr float FIRESWORN_ERUPTION_HEALTH_PCT = 20.0f;
constexpr float FIRESWORN_ERUPTION_SAFE_DISTANCE = 18.0f;

// The nearest living Firesworn about to erupt within `range` of `obj`, if any.
inline Creature* FindDyingFiresworn(WorldObject const* obj, float range)
{
    std::list<Creature*> found;
    obj->GetCreatureListWithEntryInGrid(found, NPC_FIRESWORN, range);
    Creature* nearest = nullptr;
    for (Creature* add : found)
    {
        if (!add->IsAlive() || add->GetHealthPct() > FIRESWORN_ERUPTION_HEALTH_PCT)
            continue;
        if (!nearest || obj->GetDistance(add) < obj->GetDistance(nearest))
            nearest = add;
    }
    return nearest;
}

inline bool InMajordomoCoals(WorldObject const* obj)
{
    return obj->GetMapId() == 409 &&
           obj->GetExactDist2d(MAJORDOMO_COALS_X, MAJORDOMO_COALS_Y) < MAJORDOMO_COALS_RADIUS &&
           std::fabs(obj->GetPositionZ() - MAJORDOMO_COALS_Z) < 6.0f;
}
}

#endif
