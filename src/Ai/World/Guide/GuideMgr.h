/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEMGR_H
#define PLAYERBOTS_GUIDEMGR_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Define.h"
#include "SharedDefines.h"

class Player;
class Quest;

// A RestedXP "<<" condition, read as RestedXP reads it: any of its groups may hold; a group holds when all its entries
// do (a race, a class or a faction, or "!" not).
struct GuideConditionOption
{
    uint32 raceMask{0};
    uint32 classMask{0};
    int8 team{-1};  // TEAM_ALLIANCE / TEAM_HORDE, -1 for none
    bool negate{false};
};

struct GuideCondition
{
    std::vector<std::vector<GuideConditionOption>> groups;

    bool Holds(Player* player) const;
};

enum class GuideElementType : uint8
{
    Goto,
    Accept,
    TurnIn,
    Complete,
    Xp,
    Below,
    AtLeast,
    OnQuest,
    NotOnQuest,
    TurnedIn,
    QuestComplete,
    Available,
    Target,
    Mob,
    Train,       // learn one spell at the step's trainer
    Trainer,     // learn the class spells there
    Vendor,      // sell junk at the step's vendor
    Collect,     // gather items (for a quest, or before taking it)
    Hearth,      // use the hearthstone
    Home,        // bind at the step's innkeeper
    FlightPath,  // learn the step's flight path
    Fly,         // fly to the flight point named in other
    Use,         // use item id for the step's objective, equip it, or open it
    Gossip,      // talk to creature id (0: the step's ".target") and pick the gossip options in entries, a menu each
    Keep,        // items RestedXP banks for later (entries): never junk
    Abandon,     // drop quest id from the log
    Side,        // "#completewith": the step is done along the way until the step in other ("next" or a label)
    // Skip the step while: (see guide_data.py for the flags)
    ZoneSkip,      // in one of the zones or areas in entries
    ItemCount,     // the bags don't hold count of item id
    Money,         // the character has less than count copper
    Skill,         // skill id has count points
    MaxLevel,      // past level id (and on to the step at index count, when other names a label)
    Aura,          // the character has aura id (amount: seconds left, count: stacks)
    Reputation,    // faction id is short of standing count, plus amount
    Group,         // always: a group quest, and the self-bot plays solo
    BindLocation,  // the hearthstone is bound in area id (flags 1: isn't)
    Equipped,      // item id (0: any) is worn in equipment slot count (flags 1: isn't)
    DeathSkip,     // RestedXP's death skip: put at the graveyard nearest the step's last place
    Other
};

struct GuideElement
{
    GuideElementType type{GuideElementType::Other};
    uint32 id{0};         // quest, or level for Xp / Below / AtLeast
    uint32 objective{0};  // Complete: the objective, numbered as the client lists them (from 1)
    uint32 count{0};      // Collect: how many of item id
    uint32 quest{0};      // Collect: the quest they're for (0: none named)
    float xp{0.0f};       // Xp: xp into the level (< 0: that far short of it; under 1: a fraction of it)
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float radius{0.0f};           // Goto: 0 is a waypoint on the way, not a place the step ends at
    float amount{0.0f};           // Aura: seconds; Reputation: points into the standing
    uint32 flags{0};              // ZoneSkip, ItemCount, Money, Skill, Aura, Reputation: guide_data.py's flags
    std::vector<uint32> entries;  // Target / Mob: creature entries; ZoneSkip: areas; Keep: items; Gossip: options
    std::string other;            // Other: RestedXP's tag
    GuideCondition condition;     // the element's own "<<", empty for every character

    bool IsQuestWork() const;
    bool IsService() const;
};

struct GuideStep
{
    uint32 number{0};  // RestedXP's step number in its guide
    GuideCondition condition;
    std::vector<GuideElement> elements;
    bool side{false};     // a "#completewith" step: never the current step; worked on while it is active
    uint32 sideUntil{0};  // side: the index of the last step it stays active through
};

// Where a creature the route uses spawns (the guides give no height: a cave or an upper floor needs this).
struct GuideSpawn
{
    uint32 mapId{0};
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

// How a ".use" item is used, read from its spell once at startup.
struct GuideUse
{
    enum class Mode : uint8
    {
        Equip,         // no spell: put it on
        Focus,         // cast by a spell focus object
        Object,        // cast on an object
        NearbyObject,  // cast by the right object; the spell picks it
        Unit,          // cast on a creature (or its corpse)
        NearbyUnit,    // cast by the right creature; the spell picks it
        Dest,          // cast on a spot
        Self,          // cast where the step's place is
        Open,          // no spell, holds loot: open it (a crate, a footlocker)
        Lure           // places a trap a creature must walk into: placed, the creature pulled onto it
    };

    Mode mode{Mode::Self};
    uint32 spell{0};
    uint32 focus{0};              // Focus: the spell focus id
    std::vector<uint32> units;    // creature entries its spell is for (conditions, SmartAI "hit by spell")
    std::vector<uint32> objects;  // object entries it is for
    float health{0.0f};           // use only at or below this health percent (0: any)
    bool corpse{false};           // on a dead creature
    float range{5.0f};
    uint32 trap{0};  // Lure: the trap object it places (or links to the spell focus it places)
};

// A gossip option that does a quest's work (SmartAI "on gossip option selected"): who offers it, in which menu
struct GuideGossipCredit
{
    uint32 entry{0};
    bool object{false};
    uint32 menu{0};
    uint32 option{0};  // the option's id in its menu
};

// A guide that can come next, for the characters its condition holds for (RestedXP's "#next name << condition")
struct GuideNext
{
    std::string name;
    GuideCondition condition;
};

struct Guide
{
    std::string name;
    std::vector<GuideNext> next;  // the next guides, the first that applies
    GuideCondition condition;
    std::vector<GuideStep> steps;
    std::unordered_map<std::string, uint32> labels;  // "#label" -> RestedXP step number
};

// The leveling route the guide strategy follows: RestedXP's guides, written for the realm by mod-progressive's
// guide_data.py (Playerbots.Guide.DataFile). Loaded once at startup, read-only afterwards.
class GuideMgr
{
public:
    static GuideMgr& instance()
    {
        static GuideMgr instance;
        return instance;
    }

    void Load(std::string const& path);
    bool IsEnabled() const { return !_guides.empty(); }

    Guide const* GetGuide(std::string const& name) const;
    std::vector<Guide const*> GetGuides() const;
    Guide const* GetStartGuide(Player* player) const;
    Guide const* GetNextGuide(Player* player, Guide const& guide) const;

    std::vector<uint32> const& GetQuestStarters(uint32 questId) const;  // creature entries
    std::vector<uint32> const& GetQuestEnders(uint32 questId) const;
    std::vector<uint32> const& GetQuestStarterObjects(uint32 questId) const;  // gameobject entries
    std::vector<uint32> const& GetQuestEnderObjects(uint32 questId) const;
    std::vector<uint32> const& GetQuestStarterItems(uint32 questId) const;  // items that start it (a deed, a letter)
    std::vector<GuideSpawn> const& GetSpawns(uint32 entry) const;
    // The heights of the route's creature spawns within radius (up to SPAWN_CELL) of x, y: which floor a place is on
    std::vector<float> SpawnHeightsNear(uint32 mapId, float x, float y, float radius) const;
    // The other faction's creatures (not civilians) within radius of x, y for a character of team: a camp or a town
    bool EnemyNear(TeamId team, uint32 mapId, float x, float y, float radius) const;
    // The NPCs that serve (vendors, innkeepers, flight masters, trainers) within radius of x, y: entry and spawn
    std::vector<std::pair<uint32, GuideSpawn>> ServicesNear(uint32 mapId, float x, float y, float radius) const;
    std::vector<uint32> const& GetQuestTriggers(uint32 questId) const;  // area triggers that explore it
    std::vector<uint32> const& GetItemObjects(uint32 item) const;   // objects that hold it (quest items, chest loot)
    std::vector<uint32> const& GetItemDroppers(uint32 item) const;  // creatures that drop it (the route's items)
    std::vector<uint32> const& GetItemVendors(uint32 item) const;   // creatures that sell it (the route's items)
    std::vector<GuideSpawn> const& GetObjectSpawns(uint32 entry) const;
    GuideUse const* GetUse(uint32 item) const;
    // The item whose use completes the quest (its spell's quest-complete effect; 0: none)
    uint32 GetQuestUseItem(uint32 questId) const
    {
        auto it = _questUseItems.find(questId);
        return it != _questUseItems.end() ? it->second : 0;
    }
    bool IsKept(uint32 item) const { return _keep.count(item); }  // banked by RestedXP: never junk
    // A gossip element's NPCs and objects: the creature named, else the step's ".target", else those whose gossip
    // (SmartAI) gives the step's objectives: quest credit, the kill credit, the item (a pylon, a tablet). bot: the
    // step's elements for that character (null: all)
    void GossipTargets(Player* bot, GuideStep const& step, GuideElement const& element, std::vector<uint32>& creatures,
                       std::vector<uint32>& objects) const;
    // The gossip options that give the quest its credit (empty: none)
    std::vector<GuideGossipCredit> const& GetGossipCredits(uint32 questId) const;
    // A quest's event done by a text emote at its giver (a C++ script: Shenthul's Shattered Salute; 0: none)
    uint32 GetQuestEmote(uint32 questId) const;
    // Whether the quest's giver has a script that runs once it's taken (an escort, a fight, summoned attackers)
    bool HasScriptedGiver(uint32 questId) const { return _scriptedGivers.count(questId); }
    // Whether anything on the realm completes the quest's event: an area trigger, an item, a gossip option, an emote,
    // a scripted giver, or any script or spell that credits it. False: no one can do it here (an escort with no
    // script)
    bool CanCompleteEvent(uint32 questId) const;
    // The NPCs that repair within radius of x, y: entry and spawn
    std::vector<std::pair<uint32, GuideSpawn>> RepairersNear(uint32 mapId, float x, float y, float radius) const;
    // A ".train" spell no trainer on the realm teaches (1.12's pet abilities learned by taming and the pet trainers'
    // ranks: the realm's pets learn by level): passed by
    bool IsUntrainable(uint32 spell) const { return _untrainable.count(spell); }
    // The spells whose knowing shows a ".train" done: what a trainer teaches for it (riding: Apprentice Riding)
    std::vector<uint32> const& GetTrainedAs(uint32 spell) const;

private:
    GuideMgr() = default;

    void LoadQuestRelations();
    void LoadTrains();
    void LoadSpawns();
    void ResolveLabels();
    void LoadUses();
    void LoadCreatureIndexes();
    void LoadItemDroppers();
    void LoadGossipCredits();
    void LoadScriptedGivers();
    static uint64 SpawnCell(uint32 mapId, float x, float y);

    static constexpr float SPAWN_CELL = 50.0f;

    std::unordered_map<std::string, Guide> _guides;
    std::unordered_map<uint8, std::string> _raceStart;
    std::unordered_map<uint32, std::vector<uint32>> _questStarters;
    std::unordered_map<uint32, std::vector<uint32>> _questEnders;
    std::unordered_map<uint32, std::vector<uint32>> _questStarterObjects;
    std::unordered_map<uint32, std::vector<uint32>> _questEnderObjects;
    std::unordered_map<uint32, std::vector<uint32>> _questStarterItems;
    std::unordered_map<uint32, std::vector<uint32>> _questTriggers;
    std::unordered_map<uint32, std::vector<uint32>> _itemObjects;
    std::unordered_map<uint32, std::vector<uint32>> _itemDroppers;
    std::unordered_map<uint32, std::vector<uint32>> _itemVendors;
    std::unordered_map<uint32, std::vector<GuideSpawn>> _objectSpawns;
    std::unordered_map<uint32, std::vector<GuideSpawn>> _spawns;
    std::unordered_map<uint64, std::vector<GuideSpawn>> _spawnCells;
    std::unordered_map<uint64, std::vector<GuideSpawn>> _enemyCells[2];  // by TeamId: the creatures hostile to it
    std::unordered_map<uint64, std::vector<std::pair<uint32, GuideSpawn>>> _serviceCells;
    std::unordered_map<uint32, GuideUse> _uses;
    std::unordered_set<uint32> _keep;
    std::unordered_set<uint32> _untrainable;
    std::unordered_map<uint32, std::vector<uint32>> _trainedAs;
    std::unordered_map<uint32, uint32> _questUseItems;  // quest -> the item whose use completes it
    // What a gossip option gives (SmartAI): by quest, by kill credit creature, by item; the entry and whether an object
    std::unordered_map<uint32, std::vector<GuideGossipCredit>> _gossipQuests;
    std::unordered_map<uint32, std::vector<GuideGossipCredit>> _gossipKills;
    std::unordered_map<uint32, std::vector<GuideGossipCredit>> _gossipItems;
    std::unordered_set<uint32> _scriptedGivers;  // quests whose giver has a script for being taken
    std::unordered_set<uint32> _eventCredited;   // quests something on the realm credits (SmartAI, scripts, spells)
};

#endif
