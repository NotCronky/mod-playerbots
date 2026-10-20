/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEINFO_H
#define PLAYERBOTS_GUIDEINFO_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Define.h"
#include "GuideTravel.h"
#include "ObjectGuid.h"
#include "TravelMgr.h"

struct Guide;

enum class GuideTaskType : uint8
{
    None,
    Travel,   // walk to the step's place
    Accept,   // take a quest from its giver
    TurnIn,   // hand a quest in
    Kill,     // kill or loot for a quest objective
    Service,  // train or sell at the step's NPC
    Rest,     // eat, drink or sit until ready for the next fight
    Avoid,    // step out of a patrol's way
    Hearth,   // use the hearthstone
    Loot,     // wait while the bot's kills nearby are looted
    Use,      // use a quest item (or equip it)
    Level,    // grind until a level
    Finished  // the route is done
};

// What the bot does for the current step, worked out by "guide update" and read by the guide triggers.
struct GuideTask
{
    GuideTaskType type{GuideTaskType::None};
    uint32 questId{0};
    uint32 objective{0};
    uint32 element{0};            // Service / Hearth: the element's index in its step
    int32 sideStep{-1};           // Hearth: the side step it belongs to (-1: the current step)
    std::vector<uint32> entries;  // Kill: creatures that count
    std::vector<uint32> objects;  // Kill: objects that hold the item (Equipment Boxes); object is the one to open
    bool open{false};             // Kill: object is in reach: open it
    WorldPosition moveTo;         // where to walk now (empty: nowhere)
    bool wander{false};           // at the place with nothing to do there yet: look around
    ObjectGuid giver;             // Accept / TurnIn: the quest giver in reach of the place
    bool interact{false};         // the giver is within interaction distance
    ObjectGuid target;            // Kill / Level: the creature to attack
    uint32 item{0};               // Use: the item
    bool castNow{false};          // Use: in place, use it now (on useOn, object or dest, else plain)
    ObjectGuid useOn;
    ObjectGuid object;
    WorldPosition dest;
    bool ride{
        false};  // the trip's leg needs "guide ride" now: through the portal, on or off the transport, fly, hearth
};

// A trip to a far place ("guide go", or a step's place on another map or far off): the legs of its route, in turn.
struct GuideTrip
{
    bool going{false};   // "guide go": the trip comes before the route
    bool resume{false};  // "guide go" while the route was on: back to it on arrival
    std::string name;    // "guide go": the place, by name
    WorldPosition place;
    WorldPosition goal;  // where the route below leads
    std::vector<GuideLeg> legs;
    uint32 leg{0};
    uint32 legSince{0};
    uint32 routedAt{0};    // when the route was worked out (or failed to be)
    bool noHearth{false};  // a hearth or flight leg failed: routes leave them out from then on
    bool noFly{false};
    uint32 enteredAt{0};          // Portal: when the bot reached the trigger
    WorldPosition transportSeen;  // Transport: where it was at the last update (standing still: it has stopped)
    bool transportStopped{false};

    void Clear()
    {
        legs.clear();
        leg = legSince = routedAt = enteredAt = 0;
        goal = WorldPosition();
        transportSeen = WorldPosition();
        transportStopped = false;
    }
};

// A self-bot's position in the guides. Not saved: "guide start" finds it again from the character's quests.
struct GuideInfo
{
    bool active{false};
    Guide const* guide{nullptr};
    uint32 step{0};      // index into guide->steps
    uint32 area{0};      // Kill / Level: the step's waypoint being searched
    uint32 reached{0};   // Travel: the step's places already reached
    uint32 serviced{0};  // the step's service elements done, by index (bit)
    uint32 stepStart{0};
    uint32 lastUpdate{0};
    bool resting{false};         // below the ready mark: rests until well recovered
    bool toldUnfinished{false};  // said once per step that a quest to hand in isn't finished
    std::set<uint32> sideDone;   // side steps of this guide that are done (indexes)
    uint32 hearthAt{0};          // when the hearthstone was used
    uint32 openedAt{0};          // when an object was last opened or used ("guide open")
    uint32 lootOpenSince{0};     // a loot window open since (closed if left open)
    bool passStep{false};        // the step can't be done (a full quest log with nothing to drop): move on
    std::vector<WorldPosition> serviceSearched;  // this step's NPC spawns already walked to and found empty
    ObjectGuid lootWait;                         // the corpse being waited on, since when, and those given up on
    uint32 lootWaitSince{0};
    std::set<ObjectGuid> lootGivenUp;
    // A use at low health, for the combat side: the item, its spell, the health mark and the creatures it's for.
    uint32 useItem{0};
    uint32 useSpell{0};
    float useHealth{0.0f};
    std::vector<uint32> useUnits;
    std::set<ObjectGuid> usedOn;  // corpses already used on
    uint64 progressMark{0};       // stuck check: what the step's progress looked like, since when, how far to go
    uint32 progressSince{0};
    float progressDistance{0.0f};
    ObjectGuid avoidPatrol;  // the patrol it stepped away from, and the spot it left
    WorldPosition avoidFrom;
    uint32 avoidSince{0};
    uint32 diedAt{0};  // when the body was first seen dead (release waits a moment)
    // Stuck on one target: the target, since when it got no closer, the closest it got, the fallbacks tried, and a
    // fallback's place on the way (the step's place by the target: a cave's entrance)
    WorldPosition stuckGoal;    // what it's judged on (a route's leg, a detour, or the target itself)
    WorldPosition stuckTarget;  // the target the fallbacks are for
    uint32 stuckSince{0};
    float stuckBest{0.0f};
    uint8 stuckTries{0};
    WorldPosition detour;
    WorldPosition reachTarget;  // the last target checked for a whole path, when, and whether it had one
    uint32 reachChecked{0};
    bool reachWhole{false};
    bool reachEnemy{false};  // and whether that path passes the other faction's creatures (a camp, a town)
    uint64 loggedTask{0};
    std::map<ObjectGuid, uint32> unreachable;  // kill targets with no whole path to them, until when they're left  //
                                               // the last task written to the guide log
    GuideTrip trip;
    GuideTask task;

    void Reset()
    {
        guide = nullptr;
        step = area = reached = serviced = stepStart = lastUpdate = 0;
        toldUnfinished = false;
        sideDone.clear();
        hearthAt = progressSince = stuckSince = 0;
        detour = WorldPosition();
        task = GuideTask();
    }
};

#endif
