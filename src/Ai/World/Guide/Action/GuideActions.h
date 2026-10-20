/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEACTIONS_H
#define PLAYERBOTS_GUIDEACTIONS_H

#include <unordered_map>

#include "AttackAction.h"
#include "GuideInfo.h"
#include "NewRpgBaseAction.h"
#include "UseItemAction.h"

struct GuideElement;
struct GuideStep;

// Works out the current step's task (GuideInfo::task), moving on past steps that are done or don't apply.
// Takes the tick only when the step changes.
class GuideUpdateAction : public NewRpgBaseAction
{
public:
    GuideUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide update") {}

    bool Execute(Event event) override;
    bool isUseful() override;

    // Once resting (below the ready mark below), until this much health and mana (percent).
    static constexpr float RESTED_HEALTH = 90.0f;
    static constexpr float RESTED_MANA = 80.0f;
    static constexpr float OPEN_RANGE = INTERACTION_DISTANCE - 2.0f;  // as near as playerbots' "open loot" needs

private:
    static constexpr uint32 UPDATE_INTERVAL = 500;
    static constexpr float GIVER_RANGE = 40.0f;  // yards from the bot to look for the quest giver
    static constexpr float KILL_RANGE = 60.0f;   // yards from the bot to look for kill targets
    static constexpr float AREA_REACHED = 20.0f;
    static constexpr float AREA_SPAWN_RADIUS = 80.0f;  // spawns this near a step's waypoint belong to it
    static constexpr float AREA_SPAWN_SPACING = 15.0f;
    // A place with no whole path to it (in a cave, from the hill above it) is reached by way of the step's places
    // this near it first (the cave's entrance)
    static constexpr float ENTRANCE_RANGE = 100.0f;
    // A place's floors: up to MAX_FLOORS, the next looked for FLOOR_GAP under the last and up to FLOOR_SEARCH further;
    // the route's creatures within FLOOR_HINT_RANGE say which (the one nearest their middle height, if within
    // FLOOR_HINT_MATCH)
    static constexpr uint32 MAX_FLOORS = 6;
    static constexpr float FLOOR_GAP = 5.0f;
    static constexpr float FLOOR_SEARCH = 150.0f;
    static constexpr float FLOOR_HINT_RANGE = 40.0f;
    static constexpr float FLOOR_HINT_MATCH = 20.0f;
    // Patrols: watched within this range; their path passing within aggro range + margin is too close; the bot
    // steps out to aggro range + clearance from it.
    static constexpr float PATROL_WATCH = 50.0f;
    static constexpr float PATROL_MARGIN = 3.0f;
    static constexpr float PATROL_CLEARANCE = 8.0f;
    static constexpr uint32 PATROL_WAIT = 30 * IN_MILLISECONDS;  // longest wait for it to pass
    static constexpr float SIDE_RANGE = 30.0f;                   // a side step's kills: only this close to the bot
    static constexpr float LOOT_RANGE = 25.0f;                   // its own kills this close are looted before moving on
    static constexpr uint32 LOOT_WAIT = 8 * IN_MILLISECONDS;
    static constexpr uint32 NEEDED_LOOT_WAIT = 30 * IN_MILLISECONDS;  // a quest's item: walked to and taken
    // Stuck: no progress on a step for this long; walking this much closer to the goal counts as progress.
    static constexpr uint32 STUCK_TIME = 8 * MINUTE * IN_MILLISECONDS;
    static constexpr float STUCK_GAIN = 15.0f;
    // Stuck on one target: this long without getting TARGET_GAIN closer tries the next fallback (route around, the
    // step's place by the target, give up)
    static constexpr uint32 TARGET_STUCK_TIME = MINUTE * IN_MILLISECONDS;
    static constexpr float TARGET_GAIN = 5.0f;
    static constexpr uint32 REACH_RECHECK = 10 * IN_MILLISECONDS;     // a target's whole path is checked again after
    static constexpr uint32 UNREACHABLE_TIME = 30 * IN_MILLISECONDS;  // a creature with no whole path is left that long
    static constexpr uint32 KILL_PATH_CHECKS = 3;                     // creatures checked for a whole path per update
    // A walk passing the other faction's creatures this near (a camp, a town) is routed round them, unless its target
    // is among them
    static constexpr float ENEMY_RANGE = 35.0f;
    static constexpr uint32 MIN_FREE_SLOTS = 2;                 // fewer free bag slots: the cheapest junk goes
    static constexpr uint32 STALE_LOOT = 10 * IN_MILLISECONDS;  // a loot window open this long is closed
    static constexpr float SERVICE_RANGE = 150.0f;  // an NPC to train, sell, bind or fly with: its spawn this near
    // A fight starts only with this much health and mana (percent); below, the bot eats and drinks first.
    static constexpr float READY_HEALTH = 60.0f;
    static constexpr float READY_MANA = 40.0f;
    // Trips: a step's place is routed (flights, portals, transports) when on another map or this far; "guide go" from
    // this far. Arrived within ARRIVED. A failed route is tried again after REROUTE_WAIT.
    static constexpr float STEP_ROUTE_MIN = 1000.0f;
    static constexpr float TRIP_ROUTE_MIN = 200.0f;
    static constexpr float ARRIVED = 25.0f;
    static constexpr float GOAL_MOVED = 30.0f;  // a route still serves a place this near its goal
    static constexpr float WAYPOINT_REACHED = 10.0f;
    static constexpr float WAIT_REACHED = 2.0f;  // the platform's or pier's edge a transport is waited for at
    static constexpr float OFF_ROUTE = 1500.0f;  // this far from a walk's end: taken off the route (died, teleported)
    static constexpr uint32 REROUTE_WAIT = 30 * IN_MILLISECONDS;

    bool StepDone(GuideStep const& step);
    bool ServiceDone(GuideStep const& step, GuideElement const& element, uint32 index);
    void Finish();
    GuideTask MakeTask(GuideStep const& step);
    void MakeGiverTask(GuideStep const& step, GuideElement const& element, GuideTask& task);
    void MakeServiceTask(GuideStep const& step, GuideElement const& element, uint32 index, GuideTask& task);
    void MakeKillTask(GuideStep const& step, GuideElement const& element, GuideTask& task);
    bool MakeExploreTask(uint32 questId, GuideTask& task);
    bool HasWholePath(WorldPosition const& to, bool* enemy = nullptr);
    void AbandonQuest(uint32 questId);
    bool MakeRoom(uint32 questId);
    void FreeBagSpace();
    void CloseStaleLoot(GuideInfo& info);
    bool Reachable(GuideInfo& info, WorldPosition const& target);
    WorldPosition StepEntrance(GuideInfo& info, WorldPosition const& target);
    std::string PathKind(WorldPosition const& to);
    WorldPosition Goal(GuideTask const& task);
    bool CheckTargetStuck(GuideInfo& info, WorldPosition const& target);
    void ApplyDetour(GuideInfo& info);
    void LogTask(GuideInfo& info);
    void MakeCollectTask(GuideStep const& step, GuideElement const& element, GuideTask& task);
    bool UseForObjective(GuideStep const& step, GuideTask& task);
    bool MakeUseTask(GuideStep const& step, GuideElement const& element, uint32 index, GuideTask& task);
    bool FinishQuestTask(uint32 questId, GuideTask& task);
    void MakeAreaTask(GuideStep const& step, GuideTask& task);
    void MakeTravelTask(GuideStep const& step, GuideTask& task);
    WorldPosition Place(GuideElement const& element);
    std::vector<WorldPosition> SpawnPlaces(std::vector<uint32> const& entries,
                                           std::vector<GuideElement const*> const& gotos,
                                           std::vector<uint32> const& objects = {});
    bool MakeObjectTask(std::vector<uint32> const& objects, GuideTask& task);
    ObjectGuid FindKillTarget(std::vector<uint32> const& entries, bool questLoot, float range = KILL_RANGE);
    Unit* FindKillCandidate(std::vector<uint32> const& entries, bool questLoot, float range);
    void SideWork(GuideInfo& info);
    void WaitForLoot(GuideInfo& info);
    bool HasLootFor(Creature* corpse);
    std::vector<uint8> NeededLootSlots(Creature* corpse);
    bool CheckStuck(GuideInfo& info);
    bool ReadyToFight();
    void AvoidPatrols(GuideInfo& info);
    float PatrolPassDistance(Creature* patrol, float x, float y);
    WorldPosition PatrolEscape(Creature* patrol);

    std::unordered_map<GuideElement const*, WorldPosition> _places;  // Place(): the guides' elements don't change
    bool UpdateTrip(GuideInfo& info);
    void Route(GuideInfo& info);
    bool LegDone(GuideTrip const& trip, uint32 index);
    uint32 LegTimeout(GuideLeg const& leg);
    void LegTask(GuideTrip& trip, GuideLeg const& leg, GuideTask& task);
};

class GuideMoveAction : public NewRpgBaseAction
{
public:
    GuideMoveAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide move") {}

    bool Execute(Event event) override;
};

// Accepts or turns in the task's quest at the giver in reach.
class GuideQuestAction : public NewRpgBaseAction
{
public:
    GuideQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide quest") {}

    bool Execute(Event event) override;
};

// Trains or sells at the task's NPC in reach (the step's ".train", ".trainer", ".vendor").
class GuideServiceAction : public NewRpgBaseAction
{
public:
    GuideServiceAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide service") {}

    bool Execute(Event event) override;

private:
    // Supplies bought at a vendor: up to this many of each, and only while the bags would still hold twice the price
    static constexpr uint32 FOOD_STOCK = 20;
    static constexpr uint32 AMMO_STOCK = 600;

    void SellJunk(Creature* vendor);
    void BuySupplies(Creature* vendor);
};

// Uses the task's item: on its creature, object or spot, or equips it (a step's ".use").
class GuideUseAction : public NewRpgBaseAction
{
public:
    GuideUseAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide use") {}

    bool Execute(Event event) override;
};

// In combat: the creature is at the health mark of a "use it at low health" item; use it now.
class GuideUseNowAction : public Action
{
public:
    GuideUseNowAction(PlayerbotAI* botAI) : Action(botAI, "guide use now") {}

    bool Execute(Event event) override;
};

// Uses the hearthstone (a step's ".hs"); on cooldown or without one, passes it by.
class GuideHearthAction : public UseItemAction
{
public:
    GuideHearthAction(PlayerbotAI* botAI) : UseItemAction(botAI, "guide hearth") {}

    bool Execute(Event event) override;
    bool isPossible() override { return true; }

private:
    static constexpr uint32 HEARTH_WAIT = 20 * IN_MILLISECONDS;
};

// A trip's leg that isn't walking: through a portal, the end of a transport's ride, a flight, the hearthstone.
class GuideRideAction : public UseItemAction
{
public:
    GuideRideAction(PlayerbotAI* botAI) : UseItemAction(botAI, "guide ride") {}

    bool Execute(Event event) override;
    bool isPossible() override { return true; }

private:
    static constexpr uint32 PORTAL_DWELL = 1500;  // in the trigger this long: the client's own trigger didn't go
    static constexpr uint32 HEARTH_WAIT = 20 * IN_MILLISECONDS;

    void Reroute(bool noHearth, bool noFly);
};

// Opens the task's object in reach (a quest item's box), by playerbots' own "open loot" (locks, opening spells); the
// loot strategy takes the items as from a corpse.
class GuideOpenAction : public NewRpgBaseAction
{
public:
    GuideOpenAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide open") {}

    bool Execute(Event event) override;

private:
    static constexpr uint32 OPEN_AGAIN = 3 * IN_MILLISECONDS;  // an object still there is opened or used again after
};

// Eats, drinks or sits until ready for the next fight.
class GuideRestAction : public UseItemAction
{
public:
    GuideRestAction(PlayerbotAI* botAI) : UseItemAction(botAI, "guide rest") {}

    bool Execute(Event event) override;
    bool isPossible() override { return true; }
};

class GuideAttackAction : public AttackAction
{
public:
    GuideAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "guide attack") {}

    bool Execute(Event event) override;
};

// Releases the spirit a moment after dying. No free repair, unlike the bots' auto release.
class GuideReleaseAction : public Action
{
public:
    GuideReleaseAction(PlayerbotAI* botAI) : Action(botAI, "guide release") {}

    bool Execute(Event event) override;

private:
    static constexpr uint32 RELEASE_DELAY = 3 * IN_MILLISECONDS;
};

// Walks the ghost back to the body and takes it back once the reclaim delay is over.
class GuideCorpseRunAction : public NewRpgBaseAction
{
public:
    GuideCorpseRunAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "guide corpse run") {}

    bool Execute(Event event) override;
};

// "guide" chat command: status, start [guide], resume, go <place>, stop, skip. Self-bot only.
class GuideCommandAction : public Action
{
public:
    GuideCommandAction(PlayerbotAI* botAI) : Action(botAI, "guide") {}

    bool Execute(Event event) override;
};

#endif
