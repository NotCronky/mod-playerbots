/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PULLSAFETY_H
#define PLAYERBOTS_PULLSAFETY_H

#include "ObjectGuid.h"

class Player;
class PlayerbotAI;
class Unit;

// What a pull of a target brings along (Playerbots.SafePull): its pack, the creatures that answer its calls for help
// (on the pull, again while the fight drifts, from its scripts, when it flees for help) and its formation; and
// strays, aggressive creatures whose aggro range covers the fight or the walk to it, or whose patrol comes by.
struct PullAdds
{
    uint32 pack{0};
    uint32 strays{0};

    uint32 Total() const { return pack + strays; }
};

namespace PullSafety
{
// The core's call for help reaches CreatureFamilyAssistanceRadius (10) from where the target stands; the fight moves
// a few yards, and the periodic calls go out from wherever it is.
constexpr float PACK_RADIUS = 10.0f;
constexpr float FIGHT_DRIFT = 5.0f;
// A patrol comes this much closer than its aggro range.
constexpr float PATROL_MARGIN = 20.0f;
// A tank pulls one pack at a time: its own pack up to this size, never strays.
constexpr uint32 TANK_PACK_ADDS = 4;

// Reads which creatures call for help or flee for it in their SmartAI scripts. Once, at startup.
void Load();
// nearby: the hostile units around the bot ("possible targets no los").
PullAdds Count(Player* bot, Unit* target, GuidVector const& nearby, PlayerbotAI* botAI);
// The pull is one this bot takes on: nothing along for a damage dealer or healer, a single pack for a tank.
bool Acceptable(Player* bot, PullAdds const& adds);
}  // namespace PullSafety

#endif
