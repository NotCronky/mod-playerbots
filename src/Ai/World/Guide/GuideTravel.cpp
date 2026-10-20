/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideTravel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>

#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotsDatabase.h"
#include "Timer.h"
#include "TransportMgr.h"

namespace
{
// Walks of a trip's own (from the start or home, to the goal) reach the stored places this near: the core's paths
// only find a whole way that far (further, a partial path heads straight at the place: over the hills). None that
// near: the few nearest within NEAR_FALLBACK.
constexpr float NEAR_RANGE = 250.0f;
constexpr float NEAR_FALLBACK = 1000.0f;
constexpr uint32 NEAR_FALLBACK_COUNT = 3;
constexpr uint32 START_PATHS = 12;       // from the start, the nearest this many are checked for a whole path
constexpr float PATH_STEP = 40.0f;       // a stored walk is followed by points this far apart
constexpr float PATH_END = 2.0f;         // and by its first and last recorded points when this far from its ends
constexpr float WAIT_NEAR = 15.0f;       // a transport is waited for at the walk's last point when that's this near
constexpr float WALK_DETOUR = 1.3f;      // a straight walk is this much longer on the ground
constexpr float PORTAL_TIME = 5.0f;      // seconds
constexpr float STATIC_WAIT = 20.0f;     // a tram car or elevator: the average wait for it
constexpr float FLIGHT_SPEED = 32.0f;    // yards a second
constexpr float FLIGHT_EXTRA = 10.0f;    // taking off and landing
constexpr float HEARTH_TIME = 20.0f;     // casting and loading
constexpr float TRANSPORT_STOP = 40.0f;  // a boat or zeppelin's path passes this near the places it links
// A walk past the other faction's creatures (a camp, a town: guards) costs this many times more, plus ENEMY_PER_LEVEL
// for each level they're above the bot
constexpr float ENEMY_FACTOR = 5.0f;
constexpr float ENEMY_PER_LEVEL = 0.3f;
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
                    else if (go && go->type == GAMEOBJECT_TYPE_MO_TRANSPORT && motionTransports.count(link.object))
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

    for (uint32 i = 0; i < _nodes.size(); ++i)
        _nodesByMap[_nodes[i].pos.GetMapId()].push_back(i);
    LoadPaths(index);
    LOG_INFO("server.loading",
             ">> Loaded {} travel places and {} links ({} not on this realm) for guide trips in {} ms", _nodes.size(),
             links, dropped, GetMSTimeDiffToNow(oldMSTime));
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

float GuideTravel::LinkCost(Player* bot, Link const& link) const
{
    switch (link.type)
    {
        case LINK_WALK:
        {
            // Past the other faction's creatures (Rebel Camp for the Horde, Grom'gol for the Alliance), much longer.
            // The stored hostile mob levels aren't used: they count neutral NPCs (the road out of Northshire reads 65).
            float time = (link.distance - link.swim) / bot->GetSpeed(MOVE_RUN) + link.swim / bot->GetSpeed(MOVE_SWIM);
            int32 enemy = bot->GetTeamId() == TEAM_ALLIANCE ? link.hordeLevel : link.allianceLevel;
            if (enemy > 0)
                time *= 1.0f + ENEMY_FACTOR + ENEMY_PER_LEVEL * std::max<int32>(0, enemy - int32(bot->GetLevel()));
            return time;
        }
        case LINK_FLIGHT:
        {
            TaxiPathEntry const* path = sTaxiPathStore.LookupEntry(link.object);
            TaxiNodesEntry const* from = sTaxiNodesStore.LookupEntry(path->from);
            TaxiNodesEntry const* to = sTaxiNodesStore.LookupEntry(path->to);
            uint8 team = bot->GetTeamId() == TEAM_ALLIANCE ? 1 : 0;
            if (!bot->m_taxi.IsTaximaskNodeKnown(path->from) || !bot->m_taxi.IsTaximaskNodeKnown(path->to) ||
                !from->MountCreatureID[team] || !to->MountCreatureID[team] || bot->GetMoney() < path->price)
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

std::vector<GuideLeg> GuideTravel::FindRoute(Player* bot, WorldPosition const& goal, bool hearth, bool fly) const
{
    std::vector<GuideLeg> legs;
    if (_nodes.empty())
        return legs;

    // Dijkstra over the stored places plus three of the trip's own: the start, home (by hearthstone) and the goal.
    uint32 const count = _nodes.size();
    uint32 const START = count;
    uint32 const HOME = count + 1;
    uint32 const GOAL = count + 2;
    WorldPosition const start(bot);
    WorldPosition const home(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ);
    hearth = hearth && bot->HasItemCount(ITEM_HEARTHSTONE) && !bot->HasSpellCooldown(SPELL_HEARTHSTONE);
    float run = bot->GetSpeed(MOVE_RUN);
    auto position = [&](uint32 i) -> WorldPosition const&
    {
        return i < count ? _nodes[i].pos : i == START ? start : i == HOME ? home : goal;
    };
    auto walk = [&](WorldPosition const& a, WorldPosition const& b) { return a.GetExactDist(&b) * WALK_DETOUR / run; };

    std::vector<bool> nearGoal(count, false);
    for (uint32 i : NearNodes(goal))
        nearGoal[i] = true;

    std::vector<float> cost(count + 3, std::numeric_limits<float>::max());
    std::vector<uint32> previous(count + 3, 0);
    std::vector<Link const*> via(count + 3, nullptr);  // nullptr: a walk of the trip's own, or the hearthstone
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
            path.GetPathType() != PATHFIND_NORMAL)
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
        if (from == START)
            for (uint32 n = 0; n < near.size() && n < START_PATHS; ++n)
                if (float length = pathLength(_nodes[near[n]].pos); length >= 0.0f)
                {
                    reach(from, near[n], length / run, nullptr);
                    pathed = true;
                }
        if (!pathed)
            for (uint32 i : near)
                reach(from, i, walk(pos, _nodes[i].pos), nullptr);
        if (SameMap(pos, goal) && pos.GetExactDist(&goal) <= NEAR_RANGE)
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
            continue;
        }
        if (at == HOME)
        {
            walkOut(HOME);
            continue;
        }
        for (Link const& link : _nodes[at].links)
            if (fly || link.type != LINK_FLIGHT)
                reach(at, link.to, LinkCost(bot, link), &link);
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
    for (uint32 at : path)
    {
        Link const* link = via[at];
        GuideLeg leg;
        leg.to = position(at);
        if (link && link->type == LINK_WALK)
            for (WorldPosition const& point : link->path)
            {
                GuideLeg step;
                step.to = point;
                legs.push_back(step);
            }
        if (at == HOME)
            leg.kind = GuideLeg::Kind::Hearth;
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
            // last recorded point instead, the platform's or pier's edge, where the bot waits.
            if (legs.size() > 1 && legs.back().kind == GuideLeg::Kind::Walk &&
                legs.back().to.GetExactDist(&leg.from) < 1.0f && legs[legs.size() - 2].kind == GuideLeg::Kind::Walk &&
                SameMap(legs[legs.size() - 2].to, leg.from) &&
                legs[legs.size() - 2].to.GetExactDist2d(&leg.from) <= WAIT_NEAR)
                legs.pop_back();
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
    // that's by the far stop, else at the stop itself.
    for (size_t i = 0; i < legs.size(); ++i)
    {
        if (legs[i].kind != GuideLeg::Kind::Transport)
            continue;
        legs[i].land = legs[i].to;
        if (i + 1 < legs.size() && legs[i + 1].kind == GuideLeg::Kind::Walk && SameMap(legs[i + 1].to, legs[i].to) &&
            legs[i + 1].to.GetExactDist2d(&legs[i].to) <= WAIT_NEAR)
            legs[i].land = legs[i + 1].to;
    }
    return legs;
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
