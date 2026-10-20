/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideAudit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "GuideMgr.h"
#include "GuideRules.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "TravelMgr.h"

namespace
{
// An objective or item source spawning this near the step's places is found (the guide searches spawns this near its
// waypoints); a giver or ender anywhere is walked to (or routed to, on another map)
constexpr float NEAR_PLACE = 400.0f;
constexpr float SERVICE_NEAR = 150.0f;  // a serving NPC this near (as the guide looks for one)

// RestedXP tags that only show text or draw on the map: nothing for the bot to do
std::set<std::string> const INFO_TAGS = {"itemStat", "waypoint",        "link",       "line",        "timer",
                                         "cooldown", "disablecheckbox", "bronzetube", "addquestitem"};

class Auditor
{
public:
    Auditor(Player* bot, GuideAudit::Summary& summary) : _bot(bot), _summary(summary) {}

    void Step(Guide const& guide, GuideStep const& step)
    {
        _guide = &guide;
        _step = &step;
        _quest.clear();
        std::vector<WorldPosition> places;
        for (GuideElement const& element : step.elements)
            if (element.type == GuideElementType::Goto && GuideRules::Applies(_bot, element))
                places.emplace_back(element.mapId, element.x, element.y, 0.0f);
        if (!places.empty())
            _places = places;  // a step without places is done where the last one left the character

        for (GuideElement const& element : step.elements)
            if (GuideRules::Applies(_bot, element))
                Element(element);
    }

private:
    void Find(std::string const& kind, std::string const& detail)
    {
        LOG_INFO("playerbots.guide", "AUDIT {} step {}{}: {}: {}", _guide->name, _step->number,
                 _quest.empty() ? "" : " (" + _quest + ")", kind, detail);
        ++_summary.findings;
        ++_summary.byKind[kind];
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
            if (!used && !QuestGives(item))
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
                        // A trigger is walked into; else an escort of the quest's giver
                        if (mgr.GetQuestTriggers(element.id).empty() && mgr.GetQuestStarters(element.id).empty())
                            Find("scripted objective", "no area trigger, no giver to escort");
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
            case GuideElementType::Trainer:
            case GuideElementType::Vendor:
            case GuideElementType::Home:
            case GuideElementType::FlightPath:
            case GuideElementType::Fly:
                Service(element);
                break;
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
    Guide const* _guide{nullptr};
    GuideStep const* _step{nullptr};
    std::vector<WorldPosition> _places;
    std::string _quest;
};
}  // namespace

GuideAudit::Summary GuideAudit::Run(Player* bot, std::vector<Guide const*> const& guides)
{
    Summary summary;
    Auditor auditor(bot, summary);
    for (Guide const* guide : guides)
    {
        ++summary.guides;
        for (GuideStep const& step : guide->steps)
        {
            // The steps for this character (race, class, faction); group quests are passed by solo
            if (!step.condition.Holds(bot) ||
                std::any_of(step.elements.begin(), step.elements.end(), [&](GuideElement const& element)
                            { return element.type == GuideElementType::Group && GuideRules::Applies(bot, element); }))
                continue;
            ++summary.steps;
            auditor.Step(*guide, step);
        }
    }
    return summary;
}
