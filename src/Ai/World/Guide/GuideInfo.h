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
#include <unordered_map>
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
    int32 sideStep{-1};           // Hearth / Service (a bind): the side step it belongs to (-1: the current step)
    std::vector<uint32> entries;  // Kill: creatures that count
    std::vector<uint32> objects;  // Kill: objects that hold the item (Equipment Boxes); object is the one to open
    bool open{false};             // Kill: object is in reach: open it
    WorldPosition moveTo;         // where to walk now (empty: nowhere)
    bool wander{false};           // at the place with nothing to do there yet: look around
    ObjectGuid giver;             // Accept / TurnIn: the quest giver in reach of the place
    bool repair{false};           // Service: repair at giver (worn down, a repairer near), not the step's element
    bool vendor{false};           // Service: sell junk at giver (a vendor near), repair and restock, likewise
    uint32 gossipQuest{0};        // Service: talk to giver for this quest's gossip credit, not the step's element
    uint32 emote{0};              // Service: this text emote at giver (its script credits the quest), likewise
    bool wake{false};             // Service: giver's first gossip option (starts its script), likewise
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
    uint32 enteredAt{0};  // Portal: when the bot reached the trigger

    void Clear()
    {
        legs.clear();
        leg = legSince = routedAt = enteredAt = 0;
        goal = WorldPosition();
    }
};

// A self-bot's position in the guides. The guide, step and whether it's on are saved (GuideProgress).
struct GuideInfo
{
    bool active{false};
    Guide const* guide{nullptr};
    uint32 step{0};            // index into guide->steps
    uint32 area{0};            // Kill / Level: the step's waypoint being searched
    WorldPosition areaAnchor;  // a step without waypoints: where it began (its creatures are looked for around it)
    uint32 reached{0};         // Travel: the step's places already reached
    uint32 serviced{0};        // the step's service elements done, by index (bit)
    uint32 stepStart{0};
    uint32 lastUpdate{0};
    bool resting{false};         // below the ready mark: rests until well recovered
    bool toldUnfinished{false};  // said once per step that a quest to hand in isn't finished
    std::set<uint32> sideDone;   // side steps of this guide that are done (indexes)
    uint32 hearthAt{0};          // when the hearthstone was used
    uint32 openedAt{0};          // when an object was last opened or used ("guide open")
    ObjectGuid stepIn;           // a chest stopped short of (as near as the paths get), stepped up to straight
    uint8 stepIns{0};            // and how often
    uint32 lootOpenSince{0};     // a loot window open since (closed if left open)
    bool passStep{false};        // the step can't be done (a full quest log with nothing to drop): move on
    bool catchUp{false};         // before leaving the guide: its steps visited only to hand in finished quests
    // A side step whose run ended with its quests unfinished, done as a step of its own; then on from sideReturn
    bool finishSide{false};
    uint32 sideReturn{0};
    bool sideBack{false};  // finishSide for what sideReturn's ".use" needs (FetchReagents): back to it, not past it
    std::set<uint32> sidesFinished;              // side steps already finished that way, since the route last moved on
    std::vector<WorldPosition> serviceSearched;  // this step's NPC spawns already walked to and found empty
    std::vector<WorldPosition> repairSearched;   // repairers walked to this step, found gone or too dear
    std::set<ObjectGuid> vendorSearched;         // vendors sold to on the way this step, or given up on
    ObjectGuid vendorChase;                      // the vendor walked to now, and since when
    uint32 vendorChaseSince{0};
    uint32 skippedWork{0};  // this step's quest work given up, by element index (an item the character can't get)
    std::map<uint32, uint32> retake;  // failed escorts abandoned to take again, and how often (this step)
    ObjectGuid lootWait;              // the corpse being waited on, since when, and those given up on
    uint32 lootWaitSince{0};
    uint32 lootWaitLeft{0};  // what the corpse held for the loot strategy when the wait began (LootLeft)
    std::set<ObjectGuid> lootGivenUp;
    // A use at low health, for the combat side: the item, its spell, the health mark and the creatures it's for.
    uint32 useItem{0};
    uint32 useSpell{0};
    float useHealth{0.0f};
    std::vector<uint32> useUnits;
    std::set<ObjectGuid> usedOn;  // corpses already used on
    WorldPosition lureTrap;       // a Lure item's trap: where it was placed, when, and the creature to pull onto it
    uint32 lureSince{0};
    ObjectGuid lurePrey;
    uint64 progressMark{0};  // stuck check: what the step's progress looked like, since when, how far to go
    uint32 progressSince{0};
    float progressDistance{0.0f};
    ObjectGuid avoidPatrol;  // the patrol it stepped away from, and the spot it left
    WorldPosition avoidFrom;
    uint32 avoidSince{0};
    std::map<ObjectGuid, uint32> avoided;  // patrols already waited for, and when
    uint32 diedAt{0};                      // when the body was first seen dead (release waits a moment)
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
    uint64 loggedTask{0};    // the last task written to the guide log
    std::map<ObjectGuid, uint32> unreachable;  // kill targets with no whole path to them, until when they're left
    // GuideRules::NeededItems, worked out at neededGuide's neededStep
    std::unordered_map<uint32, uint32> neededItems;
    Guide const* neededGuide{nullptr};
    uint32 neededStep{0};
    // What playerbots_guide_progress holds, written when these change
    Guide const* savedGuide{nullptr};
    uint32 savedStep{0};
    bool savedActive{false};
    GuideTrip trip;
    GuideTask task;

    void Reset()
    {
        guide = nullptr;
        step = area = reached = serviced = stepStart = lastUpdate = 0;
        areaAnchor = WorldPosition();
        toldUnfinished = catchUp = finishSide = sideBack = false;
        sidesFinished.clear();
        sideDone.clear();
        hearthAt = progressSince = stuckSince = 0;
        detour = WorldPosition();
        task = GuideTask();
    }
};

#endif
