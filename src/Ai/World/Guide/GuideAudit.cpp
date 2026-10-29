/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideAudit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

#include "Chat.h"
#include "DBCStores.h"
#include "GuideMgr.h"
#include "GuideRules.h"
#include "GuideTravel.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TravelMgr.h"

namespace
{
// An objective or item source spawning this near the step's places is found (the guide searches spawns this near its
// waypoints); a giver or ender anywhere is walked to (or routed to, on another map)
constexpr float NEAR_PLACE = 400.0f;
constexpr float SERVICE_NEAR = 150.0f;  // a serving NPC this near (as the guide looks for one)
// A step this far from the last one (or on another map) is a trip, routed as the bot routes it
// (GuideUpdateAction::STEP_ROUTE_MIN)
constexpr float TRIP_MIN = 1000.0f;
constexpr int32 DANGER_REPORTED = 7;  // a trip's walk past creatures this many levels above (4-6: RestedXP's own roads)
constexpr float FLIGHT_POINT_NEAR = 150.0f;  // a ".fp" step learns the flight point this near its places

// RestedXP tags with nothing for the bot to do: text or map drawing only ("xpto60", "daily", "blastedLands" are
// hints too); "stable" and "tame" swap pets to learn their abilities, which the realm's pets learn by level; "aura"
// goals are buffs a ".use" in the step gives, or optional ones (Mangletooth's)
std::set<std::string> const INFO_TAGS = {"itemStat", "waypoint",        "link",         "line",         "timer",
                                         "cooldown", "disablecheckbox", "bronzetube",   "addquestitem", "xpto60",
                                         "daily",    "dailyturnin",     "blastedLands", "stable",       "tame",
                                         "aura"};

class Auditor
{
public:
    Auditor(Player* bot, GuideAudit::Summary& summary) : _bot(bot), _summary(summary) {}

    void Guide(::Guide const& guide)
    {
        // The guide's levels: its name's numbers ("14-16 Darkshore"), else the character's
        _low = _high = _bot->GetLevel();
        int low = 0;
        int high = 0;
        if (std::sscanf(guide.name.c_str(), "%d-%d", &low, &high) >= 1 && low > 0)
        {
            _low = uint8(low);
            _high = uint8(std::max(low, high));
        }
    }

    void Step(::Guide const& guide, GuideStep const& step)
    {
        _guide = &guide;
        _step = &step;
        _quest.clear();
        // The character's level by now: the guide's range, across its steps
        size_t index = &step - guide.steps.data();
        _route.level = uint8(_low + (_high - _low) * index / std::max<size_t>(1, guide.steps.size()));
        std::vector<WorldPosition> places;
        for (GuideElement const& element : step.elements)
            if (element.type == GuideElementType::Goto && GuideRules::Applies(_bot, element))
                places.emplace_back(element.mapId, element.x, element.y, 0.0f);
        // The step's hearthstone takes the character home first
        bool hearths =
            std::any_of(step.elements.begin(), step.elements.end(), [&](GuideElement const& element)
                        { return element.type == GuideElementType::Hearth && GuideRules::Applies(_bot, element); });
        if (hearths && _route.home != WorldPosition())
            _last = _route.home;
        if (!places.empty() && !step.side)
        {
            if (_last != WorldPosition())
                Trip(_last, places.front());
            _last = places.back();
        }
        if (!places.empty())
            _places = places;  // a step without places is done where the last one left the character

        for (GuideElement const& element : step.elements)
            if (GuideRules::Applies(_bot, element))
                Element(element);
    }

private:
    void Find(std::string const& kind, std::string const& detail)
    {
        std::string line = Acore::StringFormat("{} step {}{}: {}: {}", _guide->name, _step->number,
                                               _quest.empty() ? "" : " (" + _quest + ")", kind, detail);
        if (!_summary.seen.insert(line).second)
            return;  // the same for another character (the console's audit of every race and class)
        LOG_INFO("playerbots.guide", "AUDIT {}", line);
        ++_summary.findings;
        ++_summary.byKind[kind];
    }

    static std::string Where(WorldPosition const& pos)
    {
        return Acore::StringFormat("{:.0f} {:.0f} map {}", pos.GetPositionX(), pos.GetPositionY(), pos.GetMapId());
    }

    // A far step: the route the bot would take from the last step's place, at the guide's level, with the flight
    // points learned on the way so far
    void Trip(WorldPosition const& from, WorldPosition const& to)
    {
        if (from.GetMapId() == to.GetMapId() && from.GetExactDist2d(&to) < TRIP_MIN)
            return;
        _route.pos = from;
        std::vector<GuideLeg> legs = GuideTravel::instance().FindRoute(_bot, _route, to, false);
        if (legs.empty())  // as the bot: with the hearthstone too
            legs = GuideTravel::instance().FindRoute(_bot, _route, to, true);
        if (legs.empty())
        {
            Find(from.GetMapId() == to.GetMapId() ? "trip without a route" : "trip to another map without a route",
                 Acore::StringFormat("{} to {} ({:.0f} yd): walks straight", Where(from), Where(to),
                                     from.GetMapId() == to.GetMapId() ? from.GetExactDist2d(&to) : 0.0f));
            return;
        }
        std::string last = "the start";
        for (GuideLeg const& leg : legs)
        {
            if (leg.kind == GuideLeg::Kind::Transport && leg.landAtStop)
            {
                GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(leg.entry);
                Find("ride ends at its stop",
                     Acore::StringFormat("{} at {} ({}): no stored walk point near it "
                                         "(the bot looks for ground on arrival)",
                                         go ? go->name : std::to_string(leg.entry), Where(leg.to), leg.place));
            }
            if (leg.kind == GuideLeg::Kind::Walk && int32(leg.danger) - _route.level >= DANGER_REPORTED)
                Find("trip past higher creatures",
                     Acore::StringFormat("level {} walk {} -> {}: creatures up to level {} by the way", _route.level,
                                         last, leg.place.empty() ? Where(leg.to) : leg.place, leg.danger));
            if (!leg.place.empty())
                last = leg.place;
        }
    }

    // A ".fp" step: the flight point nearest its places is known from then on
    void LearnFlightPoint()
    {
        TeamId team = _bot->GetTeamId();
        for (TaxiNodesEntry const* node : sTaxiNodesStore)
            if (node && node->MountCreatureID[team == TEAM_ALLIANCE ? 1 : 0])
                for (WorldPosition const& place : _places)
                    if (place.GetMapId() == node->map_id &&
                        std::hypot(place.GetPositionX() - node->x, place.GetPositionY() - node->y) <= FLIGHT_POINT_NEAR)
                        _route.taxiNodes.insert(node->ID);
    }

    std::string QuestName(uint32 questId)
    {
        ::Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        return quest ? quest->GetTitle() : std::to_string(questId);
    }

    static std::string CreatureName(uint32 entry)
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        return creature ? creature->Name : std::to_string(entry);
    }

    static std::string ObjectName(uint32 entry)
    {
        GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(entry);
        return object ? object->name : std::to_string(entry);
    }

    // The nearest of the spawns to the step's places (2D); max with none on their map
    float Nearest(std::vector<GuideSpawn> const& spawns) const
    {
        float nearest = std::numeric_limits<float>::max();
        for (GuideSpawn const& spawn : spawns)
            for (WorldPosition const& place : _places)
                if (place.GetMapId() == spawn.mapId)
                    nearest =
                        std::min(nearest, std::hypot(place.GetPositionX() - spawn.x, place.GetPositionY() - spawn.y));
        return nearest;
    }

    // Who or what the step needs: spawned at all, and near its places
    void CheckSpawns(std::string const& what, std::vector<uint32> const& creatures, std::vector<uint32> const& objects,
                     bool anywhere = false)
    {
        GuideMgr const& mgr = GuideMgr::instance();
        std::vector<GuideSpawn> spawns;
        std::string names;
        for (uint32 entry : creatures)
        {
            std::vector<GuideSpawn> const& found = mgr.GetSpawns(entry);
            spawns.insert(spawns.end(), found.begin(), found.end());
            names += (names.empty() ? "" : ", ") + CreatureName(entry);
        }
        for (uint32 entry : objects)
        {
            std::vector<GuideSpawn> const& found = mgr.GetObjectSpawns(entry);
            spawns.insert(spawns.end(), found.begin(), found.end());
            names += (names.empty() ? "" : ", ") + ObjectName(entry);
        }
        if (spawns.empty())
        {
            // Summoned by an item the step uses (a horn, a demon's summoning item): it shows up when the bot uses it
            if (std::any_of(creatures.begin(), creatures.end(),
                            [&](uint32 entry) { return SummonedByStepItem(entry); }))
                return;
            Find(what + " not spawned", names.empty() ? "-" : names);
            return;
        }
        if (_places.empty() || _step->side || anywhere)
            return;  // a side step is done along the way, wherever that leads
        float nearest = Nearest(spawns);
        if (nearest == std::numeric_limits<float>::max())
            Find(what + " on another map", names);
        else if (nearest > NEAR_PLACE)
            Find(what + " far",
                 Acore::StringFormat("{}: nearest spawn {:.0f} yd from the step's places", names, nearest));
    }

    // The items whose spell summons each creature (directly, through a spell it triggers or an event script it sends)
    static std::unordered_map<uint32, std::vector<uint32>> const& SummonItems()
    {
        static std::unordered_map<uint32, std::vector<uint32>> const items = []
        {
            auto summons = [](uint32 spellId, auto& self, uint32 depth) -> std::vector<uint32>
            {
                std::vector<uint32> out;
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
                if (!info || depth > 2)
                    return out;
                for (SpellEffectInfo const& effect : info->Effects)
                    if (effect.Effect == SPELL_EFFECT_SUMMON && effect.MiscValue > 0)
                        out.push_back(uint32(effect.MiscValue));
                    else if (effect.Effect == SPELL_EFFECT_SEND_EVENT && effect.MiscValue > 0)
                    {
                        // An event script that summons it (the Horn of Echeyakee)
                        auto event = sEventScripts.find(uint32(effect.MiscValue));
                        if (event != sEventScripts.end())
                            for (auto const& [delay, script] : event->second)
                                if (script.command == SCRIPT_COMMAND_TEMP_SUMMON_CREATURE)
                                    out.push_back(script.TempSummonCreature.CreatureEntry);
                    }
                    else if ((effect.Effect == SPELL_EFFECT_TRIGGER_SPELL ||
                              effect.Effect == SPELL_EFFECT_TRIGGER_MISSILE) &&
                             effect.TriggerSpell)
                        for (uint32 entry : self(effect.TriggerSpell, self, depth + 1))
                            out.push_back(entry);
                return out;
            };
            std::unordered_map<uint32, std::vector<uint32>> byCreature;
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                for (_Spell const& spell : proto.Spells)
                    if (spell.SpellId)
                        for (uint32 creature : summons(spell.SpellId, summons, 0))
                            byCreature[creature].push_back(entry);
            return byCreature;
        }();
        return items;
    }

    bool SummonedByStepItem(uint32 creature) const
    {
        auto found = SummonItems().find(creature);
        if (found == SummonItems().end())
            return false;
        for (GuideElement const& element : _step->elements)
            if (element.type == GuideElementType::Use && GuideRules::Applies(_bot, element) &&
                std::find(found->second.begin(), found->second.end(), element.id) != found->second.end())
                return true;
        return false;
    }

    void CheckQuest(uint32 questId)
    {
        _quest = QuestName(questId);
        ::Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return;
        if (quest->GetAllowableRaces() && !(quest->GetAllowableRaces() & _bot->getRaceMask()))
            Find("quest not for this race", _quest);
        if (quest->GetRequiredClasses() && !(quest->GetRequiredClasses() & _bot->getClassMask()))
            Find("quest not for this class", _quest);
    }

    // Items quests give (a reward, a quest's starting item): they come from an earlier step's quest
    static bool QuestGives(uint32 item)
    {
        static std::set<uint32> const given = []
        {
            std::set<uint32> items;
            for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
            {
                items.insert(quest->GetSrcItemId());
                items.insert(std::begin(quest->RewardItemId), std::end(quest->RewardItemId));
                items.insert(std::begin(quest->RewardChoiceItemId), std::end(quest->RewardChoiceItemId));
            }
            items.erase(0);
            return items;
        }();
        return given.count(item);
    }

    // Items a spell makes that the character has without a profession: an item's own spell (the empty phial used at
    // the moonwell), or a class spell's (Drain Soul's Soul Shard)
    static bool SpellMakes(uint32 item)
    {
        static std::set<uint32> const made = []
        {
            std::set<uint32> items;
            auto add = [&](SpellInfo const* info)
            {
                for (SpellEffectInfo const& effect : info->Effects)
                    if ((effect.Effect == SPELL_EFFECT_CREATE_ITEM ||
                         effect.ApplyAuraName == SPELL_AURA_CHANNEL_DEATH_ITEM) &&
                        effect.ItemType)
                        items.insert(effect.ItemType);
            };
            for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
                for (_Spell const& spell : proto.Spells)
                    if (SpellInfo const* info = spell.SpellId ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr)
                        add(info);
            for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                    if (info->SpellFamilyName)  // a class's (profession recipes have none)
                        add(info);
            return items;
        }();
        return made.count(item);
    }

    void CheckItem(uint32 item, bool used)
    {
        // Where it comes from: creatures that drop it (worth killing for), vendors that sell it, objects that hold it,
        // the step's ".mob"s; made by a ".use", given by a quest
        GuideMgr const& mgr = GuideMgr::instance();
        std::vector<uint32> creatures = mgr.GetItemDroppers(item);
        std::vector<uint32> const& vendors = mgr.GetItemVendors(item);
        creatures.insert(creatures.end(), vendors.begin(), vendors.end());
        for (GuideElement const& other : _step->elements)
            if (other.type == GuideElementType::Mob && GuideRules::Applies(_bot, other))
                creatures.insert(creatures.end(), other.entries.begin(), other.entries.end());
        std::vector<uint32> const& objects = mgr.GetItemObjects(item);
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        std::string name = proto ? proto->Name1 : std::to_string(item);
        if (creatures.empty() && objects.empty())
        {
            if (!used && !QuestGives(item) && !SpellMakes(item))
                Find("item without a source", name + ": nothing drops, sells or holds it (auction house, crafting?)");
            return;
        }
        CheckSpawns("item source", creatures, objects);
    }

    void Element(GuideElement const& element)
    {
        GuideMgr const& mgr = GuideMgr::instance();
        bool used = std::any_of(_step->elements.begin(), _step->elements.end(), [&](GuideElement const& other)
                                { return other.type == GuideElementType::Use && GuideRules::Applies(_bot, other); });
        switch (element.type)
        {
            case GuideElementType::Accept:
                CheckQuest(element.id);
                if (!mgr.GetQuestStarterItems(element.id).empty())
                    break;  // started from an item in the bags
                if (mgr.GetQuestStarters(element.id).empty() && mgr.GetQuestStarterObjects(element.id).empty())
                    Find("no giver", _quest);
                else
                    CheckSpawns("giver", mgr.GetQuestStarters(element.id), mgr.GetQuestStarterObjects(element.id),
                                true);
                break;
            case GuideElementType::TurnIn:
                CheckQuest(element.id);
                if (mgr.GetQuestEnders(element.id).empty() && mgr.GetQuestEnderObjects(element.id).empty())
                    Find("no ender", _quest);
                else
                    CheckSpawns("ender", mgr.GetQuestEnders(element.id), mgr.GetQuestEnderObjects(element.id), true);
                break;
            case GuideElementType::Complete:
            {
                CheckQuest(element.id);
                ::Quest const* quest = sObjectMgr->GetQuestTemplate(element.id);
                if (!quest)
                    break;
                std::vector<GuideObjective> objectives = GuideRules::Objectives(quest);
                if (!element.objective || element.objective > objectives.size())
                {
                    Find("no such objective",
                         Acore::StringFormat("objective {} of {}", element.objective, objectives.size()));
                    break;
                }
                GuideObjective const& objective = objectives[element.objective - 1];
                switch (objective.kind)
                {
                    case GuideObjective::Kind::CreatureOrObject:
                    {
                        int32 entry = quest->RequiredNpcOrGo[objective.slot];
                        if (entry > 0)
                            CheckSpawns("objective creature", {uint32(entry)}, {});
                        else
                            CheckSpawns("objective object", {}, {uint32(-entry)});
                        break;
                    }
                    case GuideObjective::Kind::Item:
                        CheckItem(quest->RequiredItemId[objective.slot], used);
                        break;
                    case GuideObjective::Kind::Exploration:
                        // An event: an area trigger, an item, a gossip option, an emote, the giver's script
                        if (!mgr.CanCompleteEvent(element.id))
                            Find("event nothing completes",
                                 "no area trigger, item, gossip option, emote or scripted giver on the realm");
                        break;
                    default:
                        Find("objective not handled", objective.kind == GuideObjective::Kind::Reputation ? "reputation"
                                                      : objective.kind == GuideObjective::Kind::Money ? "money"
                                                                                                      : "player kills");
                        break;
                }
                if (_places.empty())
                    Find("no place", "an objective with no place to look");
                break;
            }
            case GuideElementType::Collect:
                if (element.quest)
                    CheckQuest(element.quest);
                if (!GuideRules::AtAuctionHouse(_bot, *_step))  // auction house shopping: passed by, said
                    CheckItem(element.id, used);
                break;
            case GuideElementType::Use:
            {
                GuideUse const* use = mgr.GetUse(element.id);
                ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
                std::string name = proto ? proto->Name1 : std::to_string(element.id);
                if (!use)
                {
                    if (!proto || !proto->StartQuest)  // an item starting a quest is taken from, not used
                        Find("item use without a plan", name);
                }
                else if (((use->mode == GuideUse::Mode::Object || use->mode == GuideUse::Mode::NearbyObject) &&
                          use->objects.empty()) ||
                         ((use->mode == GuideUse::Mode::Unit || use->mode == GuideUse::Mode::NearbyUnit) &&
                          use->units.empty() && !StepNamesCreatures()))
                    Find("item use without a target", name + ": only the step's objective can tell");
                break;
            }
            case GuideElementType::Train:
                if (!GuideMgr::instance().IsUntrainable(element.id))  // else passed by: nothing to learn here
                    Service(element);
                break;
            case GuideElementType::Trainer:
            case GuideElementType::Vendor:
            case GuideElementType::Fly:
                Service(element);
                break;
            case GuideElementType::FlightPath:
                LearnFlightPoint();
                Service(element);
                break;
            case GuideElementType::Home:
                Service(element);
                if (!_places.empty())
                    _route.home = _places.front();  // bound at this inn from now on
                break;
            case GuideElementType::Gossip:
            {
                // What to talk to (none: a bare ".skipgossip" for a window the step opens anyway)
                std::vector<uint32> npcs;
                std::vector<uint32> objects;
                mgr.GossipTargets(_bot, *_step, element, npcs, objects);
                if (!npcs.empty() || !objects.empty())
                    CheckSpawns("gossip target", npcs, objects);
                break;
            }
            case GuideElementType::Other:
                if (element.other == "cast" && used)
                    break;  // marks the step's ".use" (its spell) as done
                if (!INFO_TAGS.count(element.other))
                    Find("not done: ." + element.other, "the bot doesn't do this RestedXP instruction");
                break;
            default:
                break;
        }
    }

    bool StepNamesCreatures() const
    {
        for (GuideElement const& other : _step->elements)
            if ((other.type == GuideElementType::Mob || other.type == GuideElementType::Target) &&
                !other.entries.empty())
                return true;
        return false;
    }

    void Service(GuideElement const& element)
    {
        static char const* const WHAT[] = {"trainer", "vendor", "innkeeper", "flight master"};
        char const* what = element.type == GuideElementType::Vendor ? WHAT[1]
                           : element.type == GuideElementType::Home ? WHAT[2]
                           : (element.type == GuideElementType::FlightPath || element.type == GuideElementType::Fly)
                               ? WHAT[3]
                               : WHAT[0];
        if (_places.empty() || _step->side)
            return;  // a side step's services aren't done (an open point of the plan)
        for (WorldPosition const& place : _places)
            for (auto const& [entry, spawn] : GuideMgr::instance().ServicesNear(place.GetMapId(), place.GetPositionX(),
                                                                                place.GetPositionY(), SERVICE_NEAR))
                if (GuideRules::Serves(_bot, element, entry))
                    return;
        Find(std::string("no ") + what + " near", "none spawns near the step's places");
    }

    Player* _bot;
    GuideAudit::Summary& _summary;
    ::Guide const* _guide{nullptr};
    GuideStep const* _step{nullptr};
    std::vector<WorldPosition> _places;
    std::string _quest;
    WorldPosition _last;     // where the last step with places ended (trips start there)
    GuideRouteStart _route;  // the level, the flight points known so far, the inn bound at
    uint8 _low{1};           // the guide's levels
    uint8 _high{1};
};
}  // namespace

GuideAudit::Summary GuideAudit::Run(Player* bot, std::vector<Guide const*> const& guides)
{
    Summary summary;
    Run(bot, guides, summary);
    return summary;
}

void GuideAudit::Run(Player* bot, std::vector<Guide const*> const& guides, Summary& summary)
{
    Auditor auditor(bot, summary);
    for (Guide const* guide : guides)
    {
        ++summary.guides;
        auditor.Guide(*guide);
        for (GuideStep const& step : guide->steps)
        {
            // The steps for this character (race, class, faction); group quests are passed by solo, and so are steps
            // only for while a quest is available that this race or class can never take
            auto neverAvailable = [&](GuideElement const& element)
            {
                ::Quest const* quest = element.type == GuideElementType::Available && GuideRules::Applies(bot, element)
                                           ? sObjectMgr->GetQuestTemplate(element.id)
                                           : nullptr;
                return quest && ((quest->GetAllowableRaces() && !(quest->GetAllowableRaces() & bot->getRaceMask())) ||
                                 (quest->GetRequiredClasses() && !(quest->GetRequiredClasses() & bot->getClassMask())));
            };
            if (!step.condition.Holds(bot) ||
                std::any_of(step.elements.begin(), step.elements.end(), [&](GuideElement const& element)
                            { return element.type == GuideElementType::Group && GuideRules::Applies(bot, element); }) ||
                std::any_of(step.elements.begin(), step.elements.end(), neverAvailable))
                continue;
            ++summary.steps;
            auditor.Step(*guide, step);
        }
    }
}

namespace
{
// The character's route: its race's first guide and each next one
std::vector<Guide const*> RouteOf(Player* player)
{
    std::vector<Guide const*> guides;
    std::set<Guide const*> seen;
    for (Guide const* guide = GuideMgr::instance().GetStartGuide(player); guide && seen.insert(guide).second;
         guide = GuideMgr::instance().GetNextGuide(player, *guide))
        guides.push_back(guide);
    return guides;
}
}  // namespace

bool GuideAudit::HandleCommand(ChatHandler* handler, std::string const& args)
{
    std::istringstream in(args);
    std::string who;
    std::string which;
    in >> who;
    std::getline(in >> std::ws, which);
    GuideMgr const& mgr = GuideMgr::instance();
    if (who.empty() || !mgr.IsEnabled())
    {
        handler->SendSysMessage(who.empty() ? "Usage: .guide audit <character | all> [guide]"
                                            : "The guide strategy has no guides loaded.");
        return false;
    }

    std::vector<Player*> players;
    if (who == "all")
    {
        std::set<std::pair<uint8, uint8>> raceClasses;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (player && player->IsInWorld() && mgr.GetStartGuide(player) &&
                raceClasses.insert({player->getRace(), player->getClass()}).second)
                players.push_back(player);
    }
    else if (Player* player = ObjectAccessor::FindPlayerByName(who, true))
        players.push_back(player);
    if (players.empty())
    {
        handler->SendSysMessage(Acore::StringFormat("No online character {} to audit the guides as.", who));
        return false;
    }

    Summary summary;
    for (Player* player : players)
    {
        std::vector<Guide const*> guides;
        if (which.empty())
            guides = RouteOf(player);
        else if (Guide const* guide = mgr.GetGuide(which))
            guides.push_back(guide);
        uint32 before = summary.findings;
        uint32 steps = summary.steps;
        Run(player, guides, summary);
        LOG_INFO("playerbots.guide", "AUDIT {} (race {}, class {}): {} guides, {} steps, {} new findings",
                 player->GetName(), player->getRace(), player->getClass(), guides.size(), summary.steps - steps,
                 summary.findings - before);
    }
    std::vector<std::pair<uint32, std::string>> kinds;
    for (auto const& [kind, count] : summary.byKind)
        kinds.emplace_back(count, kind);
    std::sort(kinds.rbegin(), kinds.rend());
    LOG_INFO("playerbots.guide", "AUDIT: {} characters, {} steps, {} findings", players.size(), summary.steps,
             summary.findings);
    for (auto const& [count, kind] : kinds)
        LOG_INFO("playerbots.guide", "AUDIT:   {} x {}", count, kind);
    handler->SendSysMessage(
        Acore::StringFormat("Guide audit: {} characters, {} steps, {} findings. Details in Guide.log.", players.size(),
                            summary.steps, summary.findings));
    for (auto const& [count, kind] : kinds)
        handler->SendSysMessage(Acore::StringFormat("  {} x {}", count, kind));
    return true;
}
