/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDETRAVEL_H
#define PLAYERBOTS_GUIDETRAVEL_H

#include <string>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "TravelMgr.h"

class Player;

// One part of a trip: a walk to a place, a portal (an area trigger that teleports), a ride on a transport (boat,
// zeppelin, tram car, elevator), a flight, or the hearthstone.
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
    WorldPosition to;               // where the leg ends
    WorldPosition from;             // Portal: the trigger; Transport: where it stops to board; Fly: the flight master
    uint32 entry{0};                // Portal: the area trigger; Transport: the transport's gameobject entry
    std::vector<uint32> taxiNodes;  // Fly: the flight points, first to last
    // Transport: the ride is played out, not ridden (the transports aren't in the navmesh: bots can't get on). It
    // takes rideTime from the transport stopping at from; then the bot is put at land, the far platform's or pier's
    // edge.
    uint32 rideTime{0};
    WorldPosition land;
};

// The fastest way from a bot to a place: walking, portals, transports, flights and the hearthstone, over playerbots'
// travel node store (playerbots_travelnode and _link: places and the walks, portals, rides and flights between them).
// Loaded once at startup, read-only afterwards.
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

    // Empty: no route found (walk straight, if on the same map).
    std::vector<GuideLeg> FindRoute(Player* bot, WorldPosition const& goal, bool hearth, bool fly) const;
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
        std::vector<WorldPosition> path;  // walk: the recorded way, a point every PATH_STEP yards (ends left out)
    };

    struct Node
    {
        WorldPosition pos;
        std::string name;
        std::vector<Link> links;
    };

    GuideTravel() = default;

    float LinkCost(Player* bot, Link const& link) const;
    // The stored places within range of pos, nearest first; none: the few nearest a little further.
    std::vector<uint32> NearNodes(WorldPosition const& pos) const;
    void LoadPaths(std::unordered_map<uint32, uint32> const& index);

    std::vector<Node> _nodes;
    std::unordered_map<uint32, std::vector<uint32>> _nodesByMap;
};

#endif
