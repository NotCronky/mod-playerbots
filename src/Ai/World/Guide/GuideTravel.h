/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDETRAVEL_H
#define PLAYERBOTS_GUIDETRAVEL_H

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "PathGenerator.h"
#include "TravelMgr.h"

class Player;

// A core path that leads all the way: normal, not cut short or through obstacles. Starting or ending a little off the
// navmesh (a city's streets: the bot stands on the building's floor) doesn't count against it; callers check the end.
inline bool IsWholePath(uint32 type)
{
    return (type & PATHFIND_NORMAL) &&
           !(type & (PATHFIND_SHORTCUT | PATHFIND_INCOMPLETE | PATHFIND_NOPATH | PATHFIND_SHORT));
}

// One part of a trip: a walk to a place, a portal (an area trigger that teleports), a ride on a transport (boat,
// zeppelin, tram car, elevator), a flight, or the hearthstone or a class teleport (Teleport: Moonglade).
struct GuideLeg
{
    enum class Kind : uint8
    {
        Walk,
        Portal,
        Transport,
        Fly,
        Hearth
    };

    Kind kind{Kind::Walk};
    WorldPosition to;    // where the leg ends
    WorldPosition from;  // Portal: the trigger; Transport: where it stops to board; Fly: the flight master
    uint32 entry{0};     // Portal: the area trigger; Transport: the transport's gameobject entry; Hearth: the teleport
                         // spell (0: the hearthstone)
    std::vector<uint32> taxiNodes;  // Fly: the flight points, first to last
    // Transport: the ride is played out, not ridden (the transports aren't in the navmesh: bots can't get on). It
    // takes rideTime from the transport stopping at from; then the bot is put at land, the far platform's or pier's
    // edge.
    uint32 rideTime{0};
    WorldPosition land;
    bool landAtStop{false};  // Transport: no edge known by the far stop: the bot finds ground near it on arrival
    std::string place;       // the stored place it ends at (empty: the trip's own start, home or goal)
    uint8 danger{0};         // Walk to a stored place: the highest level of the creatures hostile to the bot along it
};

// Where a route starts when it isn't the bot's own position (the audit): the level, the flight points known there and
// the hearthstone's place (none: no hearthstone). The class's teleports up to the level count as known. No money
// limit on flights, no path checks from the start.
struct GuideRouteStart
{
    WorldPosition pos;
    uint8 level{1};
    std::set<uint32> taxiNodes;
    WorldPosition home;
};

// The fastest way from a bot to a place: walking, portals, transports, flights and the hearthstone, over playerbots'
// travel node store (playerbots_travelnode and _link: places and the walks, portals, rides and flights between them)
// and the roads (Playerbots.Guide.RoadFile), which walks favour. Loaded once at startup, read-only afterwards.
class GuideTravel
{
public:
    static GuideTravel& instance()
    {
        static GuideTravel instance;
        return instance;
    }

    void Load();
    bool IsLoaded() const { return !_nodes.empty(); }

    // Empty: no route found (walk straight, if on the same map). teleport: the class's teleport spells it knows.
    std::vector<GuideLeg> FindRoute(Player* bot, WorldPosition const& goal, bool hearth, bool fly,
                                    bool teleport = true) const;
    // The same from elsewhere, for the bot's race, class and faction (the audit)
    std::vector<GuideLeg> FindRoute(Player* bot, GuideRouteStart const& start, WorldPosition const& goal,
                                    bool hearth) const;
    // A place by name: a flight point ("Ironforge"), else a stored place ("Goldshire", "Westfall"). The nearest of
    // the best matches; false when nothing matches.
    bool FindPlace(Player* bot, std::string const& name, WorldPosition& place, std::string& found) const;

private:
    enum LinkType : uint8
    {
        LINK_WALK = 1,
        LINK_PORTAL = 2,
        LINK_TRANSPORT = 3,
        LINK_FLIGHT = 4
    };

    struct Link
    {
        uint32 to{0};  // node index
        uint8 type{LINK_WALK};
        uint32 object{0};  // portal: area trigger; transport: gameobject entry; flight: TaxiPath id
        float distance{0.0f};
        float swim{0.0f};
        float time{0.0f};                 // portal, transport, flight: seconds it takes (transport: with the wait)
        float ride{0.0f};                 // transport: seconds the ride itself takes
        uint8 allianceLevel{0};           // walk: the highest level of the Alliance's creatures along it (0: none)
        uint8 hordeLevel{0};              // and of the Horde's (guards: a camp of the other faction)
        uint8 danger[2]{0, 0};            // walk: the highest level of the creatures hostile to each team near its way
        std::vector<WorldPosition> path;  // walk: the recorded way, a point every PATH_STEP yards (ends left out)
        bool road{false};                 // walk: along a road (road_data.py), from one road point to the next
    };

    struct Node
    {
        WorldPosition pos;
        std::string name;
        std::vector<Link> links;
        WorldPosition edge;  // a transport's stop: where to wait for it and step off it (the pier's, platform's edge)
        bool road{false};    // a road point (no name)
        bool cut{false};     // on a road joined to no stored place: never walked to
    };

    // A class spell that teleports its caster (Teleport: Moonglade): where to, for which classes from which level
    struct Teleport
    {
        uint32 spell{0};
        WorldPosition to;
        uint32 classMask{0};
        uint32 level{0};
        float time{0.0f};  // casting and loading
        bool reagents{false};
    };

    GuideTravel() = default;

    // start: the audit's stand-in for the bot's level and flight points (nullptr: the bot's own)
    float LinkCost(Player* bot, Link const& link, GuideRouteStart const* start) const;
    std::vector<GuideLeg> Route(Player* bot, WorldPosition const& origin, WorldPosition const& goal, bool hearth,
                                bool fly, bool teleport, GuideRouteStart const* start) const;
    // The stored places within range of pos, nearest first; none: the few nearest a little further.
    std::vector<uint32> NearNodes(WorldPosition const& pos) const;
    void LoadPaths(std::unordered_map<uint32, uint32> const& index);
    void LoadDanger();
    void LoadEdges();
    void LoadTeleports();
    void LoadRoads();
    bool CanTeleport(Player* bot, Teleport const& teleport, GuideRouteStart const* start) const;

    std::vector<Node> _nodes;
    std::vector<Teleport> _teleports;
    std::unordered_map<uint32, std::vector<uint32>> _nodesByMap;
};

#endif
