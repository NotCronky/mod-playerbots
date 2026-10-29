/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideTravel.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <queue>
#include <set>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotsDatabase.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Tokenize.h"
#include "TransportMgr.h"

namespace
{
// Walks of a trip's own (from the start or home, to the goal) reach the stored places this near: the core's paths
// only find a whole way that far (further, a partial path heads straight at the place: over the hills). None that
// near: the few nearest within NEAR_FALLBACK.
constexpr float NEAR_RANGE = 250.0f;
constexpr float NEAR_FALLBACK = 1000.0f;
// From the start, the goal is walked to straight this far (off the road), against the stored places' and roads' ways
constexpr float DIRECT_RANGE = 600.0f;
constexpr uint32 NEAR_FALLBACK_COUNT = 3;
constexpr uint32 START_PATHS = 12;   // from the start, the nearest this many are checked for a whole path
constexpr float PATH_STEP = 40.0f;   // a stored walk is followed by points this far apart
constexpr float PATH_END = 2.0f;     // and by its first and last recorded points when this far from its ends
constexpr float WAIT_NEAR = 15.0f;   // a transport is waited for at the walk's last point when that's this near
constexpr float WALK_DETOUR = 1.3f;  // a straight walk is this much longer on the ground
// A walk along a road takes this share of its time: roads are taken over the land unless much longer
constexpr float ROAD_FACTOR = 0.6f;
constexpr float ROAD_JOIN = 100.0f;      // a stored place joins the road points this near (the nearest, by walk)
constexpr float ROAD_GAP = 150.0f;       // a road's end joins another road's nearest point this near (a gap in it)
constexpr float PORTAL_TIME = 5.0f;      // seconds
constexpr float STATIC_WAIT = 20.0f;     // a tram car or elevator: the average wait for it
constexpr float FLIGHT_SPEED = 32.0f;    // yards a second
constexpr float FLIGHT_EXTRA = 10.0f;    // taking off and landing
constexpr float HEARTH_TIME = 20.0f;     // casting and loading
constexpr float TRANSPORT_STOP = 40.0f;  // a boat or zeppelin's path passes this near the places it links
constexpr float DOOR_CLEARANCE = 3.0f;   // a place this near an instance's entrance trigger is on its door
// A walk past the other faction's creatures (a camp, a town: guards) costs this many times more, plus ENEMY_PER_LEVEL
// for each level they're above the bot
constexpr float ENEMY_FACTOR = 5.0f;
constexpr float ENEMY_PER_LEVEL = 0.3f;
// A walk past creatures hostile to the bot more than DANGER_MARGIN levels above it (they pull it in from far) costs
// DANGER_PER_LEVEL times more for each further level: a level 16 takes the road or the gryphon, not the gnoll camps.
// Hostile: of a faction hostile to the team, attackable, on land, not a critter, trigger or civilian; elites count
// DANGER_ELITE levels more. Near: within DANGER_RANGE of the way, looked at every DANGER_STEP yards.
constexpr int32 DANGER_MARGIN = 2;
constexpr float DANGER_PER_LEVEL = 1.0f;
constexpr uint8 DANGER_ELITE = 3;
constexpr float DANGER_RANGE = 30.0f;
constexpr float DANGER_STEP = 20.0f;
constexpr float DANGER_CELL = 50.0f;
// A transport's stop is where it stands (off the pier, beside the zeppelin tower, over the tram's pit): the bot waits
// and steps off at the nearest recorded walk point by it instead, within EDGE_RANGE and EDGE_HEIGHT
constexpr float EDGE_RANGE = 40.0f;
constexpr float EDGE_HEIGHT = 15.0f;
constexpr float TELEPORT_TIME = 10.0f;        // a class teleport: casting and loading, past its cast time
constexpr uint32 FACTION_TEMPLATE_HUMAN = 1;  // the players' own: hostility to each team
constexpr uint32 FACTION_TEMPLATE_ORC = 2;
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr uint32 ITEM_HEARTHSTONE = 6948;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    return text;
}

bool SameMap(WorldPosition const& a, WorldPosition const& b) { return a.GetMapId() == b.GetMapId(); }

// The transport's path passes the place (a boat or zeppelin links its stops; the store was made on other maps' data).
bool PathPasses(TransportTemplate const* transport, WorldPosition const& pos)
{
    for (KeyFrame const& frame : transport->keyFrames)
        if (frame.Node->mapid == pos.GetMapId() &&
            std::hypot(frame.Node->x - pos.GetPositionX(), frame.Node->y - pos.GetPositionY()) <= TRANSPORT_STOP)
            return true;
    return false;
}
}  // namespace

void GuideTravel::Load()
{
    _nodes.clear();
    _nodesByMap.clear();
    uint32 oldMSTime = getMSTime();

    std::unordered_map<uint32, uint32> index;  // stored id -> _nodes index
    if (PreparedQueryResult result =
            PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE)))
    {
        do
        {
            Field* fields = result->Fetch();
            index[fields[0].Get<uint32>()] = _nodes.size();
            Node node;
            node.name = fields[1].Get<std::string>();
            node.pos = WorldPosition(fields[2].Get<uint32>(), fields[3].Get<float>(), fields[4].Get<float>(),
                                     fields[5].Get<float>());
            _nodes.push_back(std::move(node));
        } while (result->NextRow());
    }
    if (_nodes.empty())
    {
        LOG_WARN("playerbots", "Guide travel: no travel nodes (playerbots_travelnode); trips walk straight");
        return;
    }

    // What the realm has: tram cars and elevators by map, boats and zeppelins on their way
    std::set<std::pair<uint32, uint32>> staticTransports;  // (map, entry)
    for (auto const& [guid, data] : sObjectMgr->GetAllGOData())
        if (GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(data.id))
            if (go->type == GAMEOBJECT_TYPE_TRANSPORT)
                staticTransports.insert({data.mapid, data.id});
    std::set<uint32> motionTransports;
    if (QueryResult result = WorldDatabase.Query("SELECT entry FROM transports"))
        do
            motionTransports.insert(result->Fetch()[0].Get<uint32>());
        while (result->NextRow());

    uint32 links = 0;
    uint32 dropped = 0;
    std::set<uint32> piers;  // the places of the store's boat and zeppelin links, kept or not
    if (PreparedQueryResult result =
            PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE_LINK)))
    {
        do
        {
            Field* fields = result->Fetch();
            auto from = index.find(fields[0].Get<uint32>());
            auto to = index.find(fields[1].Get<uint32>());
            if (from == index.end() || to == index.end())
                continue;

            Link link;
            link.to = to->second;
            link.type = fields[2].Get<uint8>();
            link.object = fields[3].Get<uint32>();
            link.distance = fields[4].Get<float>();
            link.swim = fields[5].Get<float>();
            float extra = fields[6].Get<float>();
            link.allianceLevel = fields[9].Get<uint8>();
            link.hordeLevel = fields[10].Get<uint8>();

            WorldPosition const& a = _nodes[from->second].pos;
            WorldPosition const& b = _nodes[to->second].pos;
            bool keep = false;
            switch (link.type)
            {
                case LINK_WALK:
                    keep = SameMap(a, b);
                    break;
                case LINK_PORTAL:
                {
                    // Into the world, not into an instance
                    AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(link.object);
                    AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(link.object);
                    MapEntry const* map = teleport ? sMapStore.LookupEntry(teleport->target_mapId) : nullptr;
                    keep = trigger && map && !map->Instanceable() && trigger->map == a.GetMapId() &&
                           teleport->target_mapId == b.GetMapId();
                    link.time = PORTAL_TIME;
                    break;
                }
                case LINK_TRANSPORT:
                {
                    GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(link.object);
                    if (go && go->type == GAMEOBJECT_TYPE_TRANSPORT)
                    {
                        keep = SameMap(a, b) && staticTransports.count({a.GetMapId(), link.object});
                        link.ride = std::max(extra, PORTAL_TIME);
                        link.time = link.ride + STATIC_WAIT;
                    }
                    if (go && go->type == GAMEOBJECT_TYPE_MO_TRANSPORT)
                    {
                        piers.insert(from->second);
                        piers.insert(to->second);
                    }
                    if (go && go->type == GAMEOBJECT_TYPE_MO_TRANSPORT && motionTransports.count(link.object))
                    {
                        // Half its round for the ride, half on average for the wait
                        TransportTemplate const* transport = sTransportMgr->GetTransportTemplate(link.object);
                        keep = transport && PathPasses(transport, a) && PathPasses(transport, b);
                        link.time = transport ? transport->pathTime / float(IN_MILLISECONDS) : 0.0f;
                        link.ride = link.time / 2;
                    }
                    break;
                }
                case LINK_FLIGHT:
                {
                    TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(link.object);
                    if (!path || link.object >= sTaxiPathNodesByPath.size() ||
                        sTaxiPathNodesByPath[link.object].empty())
                        break;
                    float length = 0.0f;
                    TaxiPathNodeList const& points = sTaxiPathNodesByPath[link.object];
                    for (size_t i = 1; i < points.size(); ++i)
                        if (points[i]->mapid == points[i - 1]->mapid)
                            length += std::hypot(points[i]->x - points[i - 1]->x, points[i]->y - points[i - 1]->y);
                    link.time = length / FLIGHT_SPEED + FLIGHT_EXTRA;
                    keep = sTaxiNodesStore.LookupEntry(path->from) && sTaxiNodesStore.LookupEntry(path->to);
                    break;
                }
                default:
                    break;
            }
            if (!keep)
            {
                ++dropped;
                continue;
            }
            _nodes[from->second].links.push_back(link);
            ++links;
        } while (result->NextRow());
    }

    // A boat on another route than the store's (the era realms' Bravery sails from Menethil Harbor, not Stormwind's
    // harbor) links the store's piers at its stops: the pier nearest each stop
    uint32 rerouted = 0;
    for (uint32 entry : motionTransports)
    {
        TransportTemplate const* transport = sTransportMgr->GetTransportTemplate(entry);
        if (!transport)
            continue;
        std::vector<uint32> stops;
        for (KeyFrame const& frame : transport->keyFrames)
        {
            if (!frame.IsStopFrame())
                continue;
            uint32 nearest = std::numeric_limits<uint32>::max();
            float best = TRANSPORT_STOP;
            for (uint32 pier : piers)
            {
                WorldPosition const& pos = _nodes[pier].pos;
                float dist = std::hypot(frame.Node->x - pos.GetPositionX(), frame.Node->y - pos.GetPositionY());
                if (pos.GetMapId() == frame.Node->mapid && dist <= best)
                {
                    best = dist;
                    nearest = pier;
                }
            }
            if (nearest < _nodes.size() && std::find(stops.begin(), stops.end(), nearest) == stops.end())
                stops.push_back(nearest);
        }
        for (uint32 a : stops)
            for (uint32 b : stops)
            {
                std::vector<Link>& from = _nodes[a].links;
                auto stored = [&](Link const& link)
                { return link.type == LINK_TRANSPORT && link.object == entry && link.to == b; };
                if (a == b || std::any_of(from.begin(), from.end(), stored))
                    continue;
                Link link;
                link.to = b;
                link.type = LINK_TRANSPORT;
                link.object = entry;
                link.time = transport->pathTime / float(IN_MILLISECONDS);
                link.ride = link.time / 2;
                from.push_back(link);
                ++links;
                ++rerouted;
            }
    }

    // A stop the store has no walk to (Auberdine's pier of the Bravery: a bot could get off there, never on) is
    // joined to the stored places within NEAR_RANGE of it, both ways (boats and zeppelins: not a lift's stop)
    uint32 stopWalks = 0;
    for (uint32 i = 0; i < _nodes.size(); ++i)
    {
        auto isWalk = [&](Link const& link)
        { return link.type == LINK_WALK && SameMap(_nodes[i].pos, _nodes[link.to].pos); };
        auto isTransport = [&](Link const& link)
        { return link.type == LINK_TRANSPORT && motionTransports.count(link.object); };
        std::vector<Link> const& own = _nodes[i].links;
        if (std::none_of(own.begin(), own.end(), isTransport) || std::any_of(own.begin(), own.end(), isWalk))
            continue;
        for (uint32 j = 0; j < _nodes.size(); ++j)
        {
            if (j == i || !SameMap(_nodes[i].pos, _nodes[j].pos) ||
                _nodes[i].pos.GetExactDist2d(&_nodes[j].pos) > NEAR_RANGE)
                continue;
            for (auto [from, to] : {std::pair{i, j}, std::pair{j, i}})
            {
                Link link;
                link.to = to;
                link.type = LINK_WALK;
                link.distance = _nodes[from].pos.GetExactDist(&_nodes[to].pos) * WALK_DETOUR;
                _nodes[from].links.push_back(link);
                ++stopWalks;
            }
        }
    }

    LoadRoads();

    // A place on an instance's door (Stormwind's Stockade: the store's "entrance" on its trigger, the "exit" where a
    // bot comes out a few yards off) takes a bot walking to it into the instance: never walked to, only come out at
    uint32 doors = 0;
    for (auto const& [id, teleport] : sObjectMgr->GetAllAreaTriggerTeleports())
    {
        AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(id);
        MapEntry const* map = sMapStore.LookupEntry(teleport.target_mapId);
        if (!trigger || !map || !map->Instanceable())
            continue;
        float reach = (trigger->radius > 0.0f ? trigger->radius : std::hypot(trigger->length, trigger->width) / 2) +
                      DOOR_CLEARANCE;
        for (Node& node : _nodes)
            if (!node.cut && node.pos.GetMapId() == trigger->map &&
                std::hypot(node.pos.GetPositionX() - trigger->x, node.pos.GetPositionY() - trigger->y) <= reach &&
                std::abs(node.pos.GetPositionZ() - trigger->z) <= trigger->height + DOOR_CLEARANCE)
            {
                node.cut = true;
                ++doors;
            }
    }
    for (uint32 i = 0; i < _nodes.size(); ++i)
        if (!_nodes[i].cut)
            _nodesByMap[_nodes[i].pos.GetMapId()].push_back(i);
    LoadPaths(index);
    LoadDanger();
    LoadEdges();
    LoadTeleports();
    LOG_INFO("server.loading",
             ">> Loaded {} travel places and {} links ({} not on this realm, {} for boats on other routes, {} walks to "
             "stops without one, {} places on instance doors) for guide trips in {} ms",
             _nodes.size(), links, dropped, rerouted, stopWalks, doors, GetMSTimeDiffToNow(oldMSTime));
}

void GuideTravel::LoadRoads()
{
    // The road points and the roads between them, as walks; each stored place joined to the nearest road point within
    // ROAD_JOIN (on its map)
    std::string const& path = sPlayerbotAIConfig.GuideRoadFile;
    if (path.empty())
        return;
    std::ifstream file(path);
    if (!file)
    {
        LOG_ERROR("playerbots", "Guide travel: can't read Playerbots.Guide.RoadFile {}", path);
        return;
    }

    uint32 const stored = _nodes.size();
    std::unordered_map<uint32, uint32> index;  // the file's point id -> _nodes index
    uint32 roads = 0;
    auto walk = [&](uint32 from, uint32 to, bool road)
    {
        Link link;
        link.to = to;
        link.type = LINK_WALK;
        link.distance = _nodes[from].pos.GetExactDist(&_nodes[to].pos) * (road ? 1.0f : WALK_DETOUR);
        link.road = road;
        _nodes[from].links.push_back(link);
    };
    std::string line;
    while (std::getline(file, line))
    {
        std::vector<std::string_view> columns = Acore::Tokenize(line, '\t', true);
        if (columns.size() >= 6 && columns[0] == "P")
        {
            Node node;
            node.road = true;
            node.pos = WorldPosition(uint32(std::stoul(std::string(columns[2]))), std::stof(std::string(columns[3])),
                                     std::stof(std::string(columns[4])), std::stof(std::string(columns[5])));
            index[uint32(std::stoul(std::string(columns[1])))] = _nodes.size();
            _nodes.push_back(std::move(node));
        }
        else if (columns.size() >= 3 && columns[0] == "E")
        {
            auto a = index.find(uint32(std::stoul(std::string(columns[1]))));
            auto b = index.find(uint32(std::stoul(std::string(columns[2]))));
            if (a == index.end() || b == index.end())
                continue;
            walk(a->second, b->second, true);
            walk(b->second, a->second, true);
            ++roads;
        }
    }

    // Road points by ROAD_JOIN cell, for the stored places to find theirs
    auto key = [](uint32 mapId, float x, float y)
    {
        return (uint64(mapId) << 40) | (uint64(uint32(int32(std::floor(x / ROAD_JOIN))) & 0xFFFFF) << 20) |
               (uint32(int32(std::floor(y / ROAD_JOIN))) & 0xFFFFF);
    };
    std::unordered_map<uint64, std::vector<uint32>> cells;
    for (uint32 i = stored; i < _nodes.size(); ++i)
        cells[key(_nodes[i].pos.GetMapId(), _nodes[i].pos.GetPositionX(), _nodes[i].pos.GetPositionY())].push_back(i);
    uint32 joined = 0;
    for (uint32 i = 0; i < stored; ++i)
    {
        WorldPosition const& pos = _nodes[i].pos;
        uint32 best = 0;
        float bestDistance = ROAD_JOIN;
        for (float dx : {-ROAD_JOIN, 0.0f, ROAD_JOIN})
            for (float dy : {-ROAD_JOIN, 0.0f, ROAD_JOIN})
                if (auto found = cells.find(key(pos.GetMapId(), pos.GetPositionX() + dx, pos.GetPositionY() + dy));
                    found != cells.end())
                    for (uint32 r : found->second)
                        if (float distance = pos.GetExactDist(&_nodes[r].pos); distance < bestDistance)
                        {
                            best = r;
                            bestDistance = distance;
                        }
        if (!best)
            continue;
        walk(i, best, false);
        walk(best, i, false);
        ++joined;
    }

    // The road data's roads come in pieces (the texture fades out for a stretch). A piece's end joins the nearest point
    // of another piece within ROAD_GAP; a piece still joined to no stored place leads nowhere (a bot by it found no way
    // on and hearthed across the world), so it's left out.
    std::vector<uint32> piece(_nodes.size() - stored);
    for (uint32 i = 0; i < piece.size(); ++i)
        piece[i] = i;
    auto pieceOf = [&](uint32 i)
    {
        uint32 at = i - stored;
        while (piece[at] != at)
            at = piece[at] = piece[piece[at]];
        return at;
    };
    auto unite = [&](uint32 a, uint32 b) { piece[pieceOf(a)] = pieceOf(b); };
    for (uint32 i = stored; i < _nodes.size(); ++i)
        for (Link const& link : _nodes[i].links)
            if (link.road)
                unite(i, link.to);
    uint32 gaps = 0;
    for (uint32 i = stored; i < _nodes.size(); ++i)
    {
        WorldPosition const& pos = _nodes[i].pos;
        if (std::count_if(_nodes[i].links.begin(), _nodes[i].links.end(), [](Link const& link) { return link.road; }) >
            1)
            continue;
        uint32 best = 0;
        float bestDistance = ROAD_GAP;
        for (float dx : {-2 * ROAD_JOIN, -ROAD_JOIN, 0.0f, ROAD_JOIN, 2 * ROAD_JOIN})
            for (float dy : {-2 * ROAD_JOIN, -ROAD_JOIN, 0.0f, ROAD_JOIN, 2 * ROAD_JOIN})
                if (auto found = cells.find(key(pos.GetMapId(), pos.GetPositionX() + dx, pos.GetPositionY() + dy));
                    found != cells.end())
                    for (uint32 r : found->second)
                        if (float distance = pos.GetExactDist(&_nodes[r].pos);
                            distance < bestDistance && pieceOf(r) != pieceOf(i))
                        {
                            best = r;
                            bestDistance = distance;
                        }
        if (!best)
            continue;
        walk(i, best, false);
        walk(best, i, false);
        unite(i, best);
        ++gaps;
    }
    std::set<uint32> reached;
    for (uint32 i = 0; i < stored; ++i)
        for (Link const& link : _nodes[i].links)
            if (link.to >= stored)
                reached.insert(pieceOf(link.to));
    uint32 cut = 0;
    for (uint32 i = stored; i < _nodes.size(); ++i)
        if (!reached.count(pieceOf(i)))
        {
            _nodes[i].cut = true;
            ++cut;
        }
    LOG_INFO("server.loading",
             ">> Guide trips: {} road points, {} roads, {} gaps joined, {} travel places joined to them; {} points on "
             "roads to nowhere left out",
             _nodes.size() - stored, roads, gaps, joined, cut);
}

void GuideTravel::LoadPaths(std::unordered_map<uint32, uint32> const& index)
{
    // The recorded way of each walk (playerbots_travelnode_path, a point every few yards), thinned out.
    std::unordered_map<uint64, Link*> walks;
    for (uint32 i = 0; i < _nodes.size(); ++i)
        for (Link& link : _nodes[i].links)
            if (link.type == LINK_WALK)
                walks[(uint64(i) << 32) | link.to] = &link;

    uint32 points = 0;
    std::unordered_map<Link*, WorldPosition> lastPoints;
    PreparedQueryResult result =
        PlayerbotsDatabase.Query(PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_TRAVELNODE_PATH));
    if (!result)
        return;
    do
    {
        Field* fields = result->Fetch();
        auto from = index.find(fields[0].Get<uint32>());
        auto to = index.find(fields[1].Get<uint32>());
        if (from == index.end() || to == index.end())
            continue;
        auto walk = walks.find((uint64(from->second) << 32) | to->second);
        if (walk == walks.end())
            continue;
        WorldPosition point(fields[3].Get<uint32>(), fields[4].Get<float>(), fields[5].Get<float>(),
                            fields[6].Get<float>());
        // Every PATH_STEP yards, plus the first and last recorded points (a platform's or pier's edge by a transport)
        std::vector<WorldPosition>& path = walk->second->path;
        WorldPosition const& start = _nodes[from->second].pos;
        WorldPosition const& last = path.empty() ? start : path.back();
        if (point.GetMapId() != last.GetMapId())
            continue;
        bool first = path.empty() && point.GetExactDist(&start) >= PATH_END;
        if (first ||
            (point.GetExactDist(&last) >= PATH_STEP && point.GetExactDist(&_nodes[to->second].pos) >= PATH_STEP / 2))
        {
            path.push_back(point);
            ++points;
        }
        lastPoints[walk->second] = point;
    } while (result->NextRow());

    for (auto const& [link, point] : lastPoints)
    {
        WorldPosition const& end = _nodes[link->to].pos;
        if (point.GetMapId() == end.GetMapId() && point.GetExactDist(&end) >= PATH_END &&
            (link->path.empty() || point.GetExactDist(&link->path.back()) >= PATH_END))
        {
            link->path.push_back(point);
            ++points;
        }
    }
    LOG_INFO("server.loading", ">> Loaded {} travel path points for guide trips", points);
}

void GuideTravel::LoadDanger()
{
    // The realm's creatures hostile to either team, by DANGER_CELL cells: where, how high, to whom
    struct Hostile
    {
        float x;
        float y;
        uint8 level;
        uint8 teams;  // 1 << TeamId
    };
    auto cellKey = [](uint32 mapId, int32 cx, int32 cy)
    { return (uint64(mapId) << 40) | (uint64(uint32(cx) & 0xFFFFF) << 20) | (uint32(cy) & 0xFFFFF); };
    auto cell = [](float coordinate) { return int32(std::floor(coordinate / DANGER_CELL)); };

    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(FACTION_TEMPLATE_HUMAN);
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(FACTION_TEMPLATE_ORC);
    if (!alliance || !horde)
        return;
    std::unordered_map<uint64, std::vector<Hostile>> cells;
    uint32 hostiles = 0;
    for (auto const& [guid, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!_nodesByMap.count(data.mapid))
            continue;
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
        // (not those that can't leave the water: the harbour's sharks don't reach the pier)
        if (!proto || proto->type == CREATURE_TYPE_CRITTER || !proto->Movement.IsGroundAllowed() ||
            (proto->flags_extra & (CREATURE_FLAG_EXTRA_TRIGGER | CREATURE_FLAG_EXTRA_CIVILIAN)) ||
            ((proto->unit_flags | data.unit_flags) & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE)))
            continue;
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(proto->faction);
        if (!faction)
            continue;
        uint8 teams = (faction->IsHostileTo(*alliance) ? 1 << TEAM_ALLIANCE : 0) |
                      (faction->IsHostileTo(*horde) ? 1 << TEAM_HORDE : 0);
        if (!teams)
            continue;
        bool elite = proto->rank == CREATURE_ELITE_ELITE || proto->rank == CREATURE_ELITE_RAREELITE ||
                     proto->rank == CREATURE_ELITE_WORLDBOSS;
        uint8 level = uint8(std::min<uint32>(UINT8_MAX, proto->maxlevel + (elite ? DANGER_ELITE : 0)));
        cells[cellKey(data.mapid, cell(data.posX), cell(data.posY))].push_back({data.posX, data.posY, level, teams});
        ++hostiles;
    }

    // Each walk's way (its ends and recorded points), looked at every DANGER_STEP yards
    auto lookAt = [&](uint32 mapId, float x, float y, uint8(&danger)[2])
    {
        for (int32 cx = cell(x) - 1; cx <= cell(x) + 1; ++cx)
            for (int32 cy = cell(y) - 1; cy <= cell(y) + 1; ++cy)
                if (auto found = cells.find(cellKey(mapId, cx, cy)); found != cells.end())
                    for (Hostile const& hostile : found->second)
                        if (std::hypot(hostile.x - x, hostile.y - y) <= DANGER_RANGE)
                            for (uint8 team : {TEAM_ALLIANCE, TEAM_HORDE})
                                if (hostile.teams & (1 << team))
                                    danger[team] = std::max(danger[team], hostile.level);
    };
    uint32 dangerous = 0;
    for (Node& node : _nodes)
        for (Link& link : node.links)
        {
            if (link.type != LINK_WALK)
                continue;
            std::vector<WorldPosition> way = {node.pos};
            way.insert(way.end(), link.path.begin(), link.path.end());
            way.push_back(_nodes[link.to].pos);
            for (size_t i = 1; i < way.size(); ++i)
            {
                WorldPosition const& a = way[i - 1];
                WorldPosition const& b = way[i];
                if (a.GetMapId() != b.GetMapId())
                    continue;
                uint32 steps = std::max<uint32>(1, uint32(std::ceil(a.GetExactDist2d(&b) / DANGER_STEP)));
                for (uint32 n = 0; n <= steps; ++n)
                {
                    float f = float(n) / steps;
                    lookAt(a.GetMapId(), a.GetPositionX() + (b.GetPositionX() - a.GetPositionX()) * f,
                           a.GetPositionY() + (b.GetPositionY() - a.GetPositionY()) * f, link.danger);
                }
            }
            if (link.danger[TEAM_ALLIANCE] || link.danger[TEAM_HORDE])
                ++dangerous;
        }
    LOG_INFO("server.loading", ">> Guide trips: {} hostile creatures, near {} walks", hostiles, dangerous);
}

float GuideTravel::LinkCost(Player* bot, Link const& link, GuideRouteStart const* start) const
{
    int32 level = start ? start->level : bot->GetLevel();
    switch (link.type)
    {
        case LINK_WALK:
        {
            // Past the other faction's creatures (Rebel Camp for the Horde, Grom'gol for the Alliance), much longer.
            // The stored hostile mob levels aren't used: they count neutral NPCs (the road out of Northshire reads 65);
            // the realm's own hostile spawns (danger) are.
            float time = (link.distance - link.swim) / bot->GetSpeed(MOVE_RUN) + link.swim / bot->GetSpeed(MOVE_SWIM);
            int32 enemy = bot->GetTeamId() == TEAM_ALLIANCE ? link.hordeLevel : link.allianceLevel;
            if (enemy > 0)
                time *= 1.0f + ENEMY_FACTOR + ENEMY_PER_LEVEL * std::max<int32>(0, enemy - level);
            int32 over = int32(link.danger[bot->GetTeamId()]) - level - DANGER_MARGIN;
            if (over > 0)
                time *= 1.0f + DANGER_PER_LEVEL * over;
            return link.road ? time * ROAD_FACTOR : time;
        }
        case LINK_FLIGHT:
        {
            TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(link.object);
            TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(path->from);
            TaxiNodesEntry const* to = sTaxiNodesStore.LookupEntry(path->to);
            uint8 team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;
            bool known = start ? start->taxiNodes.count(path->from) && start->taxiNodes.count(path->to)
                               : bot->m_taxi.IsTaximaskNodeKnown(path->from) &&
                                     bot->m_taxi.IsTaximaskNodeKnown(path->to) && bot->GetMoney() >= path->price;
            if (!known || !from->MountCreatureID[team] || !to->MountCreatureID[team])
                return -1.0f;
            return link.time;
        }
        default:
            return link.time;
    }
}

std::vector<uint32> GuideTravel::NearNodes(WorldPosition const& pos) const
{
    std::vector<std::pair<float, uint32>> found;
    if (auto onMap = _nodesByMap.find(pos.GetMapId()); onMap != _nodesByMap.end())
        for (uint32 i : onMap->second)
            if (float distance = pos.GetExactDist2d(&_nodes[i].pos); distance <= NEAR_FALLBACK)
                found.emplace_back(distance, i);
    std::sort(found.begin(), found.end());

    std::vector<uint32> near;
    bool anyNear = !found.empty() && found.front().first <= NEAR_RANGE;
    for (auto const& [distance, i] : found)
    {
        if (anyNear ? distance > NEAR_RANGE : near.size() >= NEAR_FALLBACK_COUNT)
            break;
        near.push_back(i);
    }
    return near;
}

std::vector<GuideLeg> GuideTravel::FindRoute(Player* bot, WorldPosition const& goal, bool hearth, bool fly,
                                             bool teleport) const
{
    return Route(bot, WorldPosition(bot), goal, hearth, fly, teleport, nullptr);
}

std::vector<GuideLeg> GuideTravel::FindRoute(Player* bot, GuideRouteStart const& start, WorldPosition const& goal,
                                             bool hearth) const
{
    return Route(bot, start.pos, goal, hearth, true, true, &start);
}

std::vector<GuideLeg> GuideTravel::Route(Player* bot, WorldPosition const& origin, WorldPosition const& goal,
                                         bool hearth, bool fly, bool teleport, GuideRouteStart const* routeStart) const
{
    std::vector<GuideLeg> legs;
    if (_nodes.empty())
        return legs;

    // Dijkstra over the stored places plus the trip's own: the start, home (by hearthstone), the goal, and where the
    // class's teleports lead.
    uint32 const count = _nodes.size();
    uint32 const START = count;
    uint32 const HOME = count + 1;
    uint32 const GOAL = count + 2;
    uint32 const TELEPORTS = count + 3;
    uint32 const total = TELEPORTS + _teleports.size();
    WorldPosition const start = origin;
    WorldPosition const home =
        routeStart ? routeStart->home
                   : WorldPosition(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    hearth = hearth && (routeStart ? home != WorldPosition()
                                   : bot->HasItemCount(ITEM_HEARTHSTONE) && !bot->HasSpellCooldown(SPELL_HEARTHSTONE));
    float run = bot->GetSpeed(MOVE_RUN);
    auto position = [&](uint32 i) -> WorldPosition const&
    {
        return i < count    ? _nodes[i].pos
               : i == START ? start
               : i == HOME  ? home
               : i == GOAL  ? goal
                            : _teleports[i - TELEPORTS].to;
    };
    auto walk = [&](WorldPosition const& a, WorldPosition const& b) { return a.GetExactDist(&b) * WALK_DETOUR / run; };

    std::vector<bool> nearGoal(count, false);
    for (uint32 i : NearNodes(goal))
        nearGoal[i] = true;

    std::vector<float> cost(total, std::numeric_limits<float>::max());
    std::vector<uint32> previous(total, 0);
    std::vector<Link const*> via(total, nullptr);  // nullptr: a walk of the trip's own, the hearthstone, a teleport
    using Entry = std::pair<float, uint32>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    auto reach = [&](uint32 from, uint32 to, float step, Link const* link)
    {
        if (step < 0.0f || cost[from] + step >= cost[to])
            return;
        cost[to] = cost[from] + step;
        previous[to] = from;
        via[to] = link;
        open.push({cost[to], to});
    };
    // From the start, or home: walk to the stored places near it, or straight to the goal when it's not far.
    // From the start, or home: walk to the stored places near it, or straight to the goal when it's that near. From
    // the start, the places the core finds a whole path to (with its length) come first; the others only without one.
    auto pathLength = [&](WorldPosition const& to) -> float
    {
        PathGenerator path(bot);
        if (!path.CalculatePath(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()) ||
            !IsWholePath(path.GetPathType()))
            return -1.0f;
        Movement::PointsArray const& points = path.GetPath();
        float length = 0.0f;
        for (size_t i = 1; i < points.size(); ++i)
            length += (points[i] - points[i - 1]).length();
        return length;
    };
    auto walkOut = [&](uint32 from)
    {
        WorldPosition const& pos = position(from);
        std::vector<uint32> near = NearNodes(pos);
        bool pathed = false;
        if (from == START && !routeStart)  // the core's paths start at the bot
            for (uint32 n = 0; n < near.size() && n < START_PATHS; ++n)
                if (float length = pathLength(_nodes[near[n]].pos); length >= 0.0f)
                {
                    reach(from, near[n], length / run, nullptr);
                    pathed = true;
                }
        if (!pathed)
            for (uint32 i : near)
                reach(from, i, walk(pos, _nodes[i].pos), nullptr);
        if (!SameMap(pos, goal) || pos.GetExactDist(&goal) > (from == START ? DIRECT_RANGE : NEAR_RANGE))
            return;
        // From the bot, only along a whole path: a route is worked out when there's none (the core's partial path
        // runs straight at the goal, off Darnassus' edge and down the tree), so the straight walk is no way at all
        if (from == START && !routeStart)
        {
            if (float length = pathLength(goal); length >= 0.0f)
                reach(from, GOAL, length / run, nullptr);
        }
        else
            reach(from, GOAL, walk(pos, goal), nullptr);
    };

    cost[START] = 0.0f;
    open.push({0.0f, START});
    while (!open.empty())
    {
        auto [reached, at] = open.top();
        open.pop();
        if (reached > cost[at])
            continue;
        if (at == GOAL)
            break;
        if (at == START)
        {
            walkOut(START);
            if (hearth)
                reach(START, HOME, HEARTH_TIME, nullptr);
            if (teleport)
                for (uint32 i = 0; i < _teleports.size(); ++i)
                    if (CanTeleport(bot, _teleports[i], routeStart))
                        reach(START, TELEPORTS + i, _teleports[i].time, nullptr);
            continue;
        }
        if (at == HOME || at >= TELEPORTS)
        {
            walkOut(at);
            continue;
        }
        for (Link const& link : _nodes[at].links)
            if ((fly || link.type != LINK_FLIGHT) && !(link.type == LINK_WALK && _nodes[link.to].cut))
                reach(at, link.to, LinkCost(bot, link, routeStart), &link);
        if (nearGoal[at])
            reach(at, GOAL, walk(_nodes[at].pos, goal), nullptr);
    }
    if (cost[GOAL] == std::numeric_limits<float>::max())
        return legs;

    std::vector<uint32> path;
    for (uint32 at = GOAL; at != START; at = previous[at])
        path.push_back(at);
    std::reverse(path.begin(), path.end());

    uint32 from = START;
    float roadRun = 0.0f;
    for (size_t k = 0; k < path.size(); ++k)
    {
        uint32 at = path[k];
        Link const* link = via[at];
        // Along a road, a point to walk to every PATH_STEP yards and where the road is left (not each road point)
        if (link && link->road && k + 1 < path.size() && via[path[k + 1]] && via[path[k + 1]]->road)
        {
            roadRun += link->distance;
            if (roadRun < PATH_STEP)
            {
                from = at;
                continue;
            }
        }
        roadRun = 0.0f;
        GuideLeg leg;
        leg.to = position(at);
        if (at < count)
            leg.place = _nodes[at].name;
        if (link && link->type == LINK_WALK)
            leg.danger = link->danger[bot->GetTeamId()];
        if (link && link->type == LINK_WALK)
            for (WorldPosition const& point : link->path)
            {
                GuideLeg step;
                step.to = point;
                legs.push_back(step);
            }
        if (at == HOME)
            leg.kind = GuideLeg::Kind::Hearth;
        else if (at >= TELEPORTS)
        {
            leg.kind = GuideLeg::Kind::Hearth;
            leg.entry = _teleports[at - TELEPORTS].spell;
        }
        else if (link && link->type == LINK_PORTAL)
        {
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(link->object);
            leg.kind = GuideLeg::Kind::Portal;
            leg.from = WorldPosition(trigger->map, trigger->x, trigger->y, trigger->z);
            leg.entry = link->object;
        }
        else if (link && link->type == LINK_TRANSPORT)
        {
            leg.kind = GuideLeg::Kind::Transport;
            leg.from = position(from);
            leg.entry = link->object;
            leg.rideTime = uint32(link->ride * IN_MILLISECONDS);
            // The stop is where the transport stands (over the tram's pit, off the pier): the walk there ends at its
            // last recorded point instead, the platform's or pier's edge, where the bot waits; else at the stop's
            // edge. The ride ends at the far stop's edge (none known: the bot looks for ground there on arrival).
            if (legs.size() > 1 && legs.back().kind == GuideLeg::Kind::Walk &&
                legs.back().to.GetExactDist(&leg.from) < 1.0f && legs[legs.size() - 2].kind == GuideLeg::Kind::Walk &&
                SameMap(legs[legs.size() - 2].to, leg.from) &&
                legs[legs.size() - 2].to.GetExactDist2d(&leg.from) <= WAIT_NEAR)
                legs.pop_back();
            else if (!legs.empty() && legs.back().kind == GuideLeg::Kind::Walk &&
                     legs.back().to.GetExactDist(&leg.from) < 1.0f && from < count &&
                     _nodes[from].edge != WorldPosition())
                legs.back().to = _nodes[from].edge;
            leg.landAtStop = _nodes[at].edge == WorldPosition();
            leg.land = leg.landAtStop ? leg.to : _nodes[at].edge;
        }
        else if (link && link->type == LINK_FLIGHT)
        {
            TaxiPathEntry const* taxiPath = sTaxiPathStore.LookupEntry(link->object);
            if (!legs.empty() && legs.back().kind == GuideLeg::Kind::Fly)
            {
                // One flight with stops
                legs.back().taxiNodes.push_back(taxiPath->to);
                legs.back().to = leg.to;
                from = at;
                continue;
            }
            leg.kind = GuideLeg::Kind::Fly;
            leg.from = position(from);
            leg.taxiNodes = {taxiPath->from, taxiPath->to};
        }
        legs.push_back(leg);
        from = at;
    }
    // A ride ends at the next walk's first point (the far platform's or pier's edge, its recorded way's start) when
    // that's by the far stop, else at the stop's edge.
    for (size_t i = 0; i + 1 < legs.size(); ++i)
        if (legs[i].kind == GuideLeg::Kind::Transport && legs[i + 1].kind == GuideLeg::Kind::Walk &&
            SameMap(legs[i + 1].to, legs[i].to) && legs[i + 1].to.GetExactDist2d(&legs[i].to) <= WAIT_NEAR)
        {
            legs[i].land = legs[i + 1].to;
            legs[i].landAtStop = false;
        }
    return legs;
}

void GuideTravel::LoadEdges()
{
    std::set<uint32> stops;
    for (uint32 i = 0; i < _nodes.size(); ++i)
        for (Link const& link : _nodes[i].links)
            if (link.type == LINK_TRANSPORT)
            {
                stops.insert(i);
                stops.insert(link.to);
            }
    // Where a bot can stand: the recorded walks' points and the places they join (not the stops themselves)
    std::unordered_map<uint32, std::vector<WorldPosition>> ground;
    for (uint32 i = 0; i < _nodes.size(); ++i)
        for (Link const& link : _nodes[i].links)
            if (link.type == LINK_WALK)
            {
                for (WorldPosition const& point : link.path)
                    ground[point.GetMapId()].push_back(point);
                if (!stops.count(i))
                    ground[_nodes[i].pos.GetMapId()].push_back(_nodes[i].pos);
            }
    uint32 found = 0;
    for (uint32 stop : stops)
    {
        Node& node = _nodes[stop];
        float best = EDGE_RANGE;
        for (WorldPosition const& point : ground[node.pos.GetMapId()])
        {
            float distance = node.pos.GetExactDist2d(&point);
            if (distance <= best && distance > 0.5f &&
                std::fabs(point.GetPositionZ() - node.pos.GetPositionZ()) <= EDGE_HEIGHT)
            {
                best = distance;
                node.edge = point;
            }
        }
        if (node.edge != WorldPosition())
            ++found;
    }
    LOG_INFO("server.loading", ">> Guide trips: {} of {} transport stops have an edge to wait and step off at", found,
             stops.size());
}

void GuideTravel::LoadTeleports()
{
    // Class spells that teleport the caster to a fixed place (spell_target_position): Teleport: Moonglade, the mage's
    // teleports (they take a rune)
    static std::unordered_map<uint32, uint32> const FAMILY_CLASS = {{SPELLFAMILY_DRUID, CLASS_DRUID},
                                                                    {SPELLFAMILY_MAGE, CLASS_MAGE}};
    for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        auto family = info ? FAMILY_CLASS.find(info->SpellFamilyName) : FAMILY_CLASS.end();
        if (family == FAMILY_CLASS.end())
            continue;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (info->Effects[i].Effect != SPELL_EFFECT_TELEPORT_UNITS)
                continue;
            SpellTargetPosition const* target = sSpellMgr->GetSpellTargetPosition(id, SpellEffIndex(i));
            if (!target || !_nodesByMap.count(target->target_mapId))
                continue;
            Teleport teleport;
            teleport.spell = id;
            teleport.to = WorldPosition(target->target_mapId, target->target_X, target->target_Y, target->target_Z);
            teleport.classMask = 1 << (family->second - 1);
            teleport.level = info->BaseLevel ? info->BaseLevel : info->SpellLevel;
            teleport.time = info->CalcCastTime() / float(IN_MILLISECONDS) + TELEPORT_TIME;
            teleport.reagents = std::any_of(info->Reagent.begin(), info->Reagent.end(), [](int32 r) { return r > 0; });
            _teleports.push_back(teleport);
            break;
        }
    }
    LOG_INFO("server.loading", ">> Guide trips: {} class teleports", _teleports.size());
}

bool GuideTravel::CanTeleport(Player* bot, Teleport const& teleport, GuideRouteStart const* start) const
{
    if (start)  // the audit: the class's teleports from their level on, those without a rune
        return (bot->getClassMask() & teleport.classMask) && teleport.level <= start->level && !teleport.reagents;
    if (!bot->HasSpell(teleport.spell) || bot->HasSpellCooldown(teleport.spell))
        return false;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(teleport.spell);
    for (uint8 i = 0; info && i < MAX_SPELL_REAGENTS; ++i)
        if (info->Reagent[i] > 0 && !bot->HasItemCount(info->Reagent[i], info->ReagentCount[i]))
            return false;
    return true;
}

bool GuideTravel::FindPlace(Player* bot, std::string const& name, WorldPosition& place, std::string& found) const
{
    std::string want = Lower(name);
    if (want.empty())
        return false;

    // Ranks: 0 a flight point's town ("Ironforge" of "Ironforge, Dun Morogh"), 1 a stored place ("Westfall",
    // "Elwynn Forest Goldshire", "Goldshire innkeeper"), 2 and 3 the same containing the name. The nearest of the best.
    uint32 bestRank = 4;
    float bestDistance = 0.0f;
    WorldPosition from(bot);
    auto consider = [&](uint32 rank, WorldPosition const& pos, std::string const& display)
    {
        float distance = SameMap(from, pos) ? from.GetExactDist(&pos) : std::numeric_limits<float>::max();
        if (rank > bestRank || (rank == bestRank && distance >= bestDistance))
            return;
        bestRank = rank;
        bestDistance = distance;
        place = pos;
        found = display;
    };

    for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node || !node->name[0] || !node->name[0][0])
            continue;
        std::string full = node->name[0];
        std::string town = full.substr(0, full.find(','));
        WorldPosition pos(node->map_id, node->x, node->y, node->z);
        if (Lower(town) == want)
            consider(0, pos, town);
        else if (Lower(full).find(want) != std::string::npos)
            consider(2, pos, full);
    }
    for (Node const& node : _nodes)
    {
        std::string lower = Lower(node.name);
        bool exact = lower == want || lower == want + " innkeeper" || lower == want + " flightmaster" ||
                     (lower.size() > want.size() && lower.compare(lower.size() - want.size(), want.size(), want) == 0 &&
                      lower[lower.size() - want.size() - 1] == ' ');
        if (exact)
            consider(1, node.pos, node.name);
        else if (lower.find(want) != std::string::npos)
            consider(3, node.pos, node.name);
    }
    return bestRank < 4;
}
