/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideMgr.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

#include "ConditionMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GuideRules.h"
#include "GuideTravel.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include "Trainer.h"

namespace
{
std::unordered_map<std::string, uint8> const RACES = {{"human", RACE_HUMAN},          {"orc", RACE_ORC},
                                                      {"dwarf", RACE_DWARF},          {"nightelf", RACE_NIGHTELF},
                                                      {"undead", RACE_UNDEAD_PLAYER}, {"scourge", RACE_UNDEAD_PLAYER},
                                                      {"tauren", RACE_TAUREN},        {"gnome", RACE_GNOME},
                                                      {"troll", RACE_TROLL},          {"bloodelf", RACE_BLOODELF},
                                                      {"draenei", RACE_DRAENEI}};

std::unordered_map<std::string, uint8> const CLASSES = {{"warrior", CLASS_WARRIOR}, {"paladin", CLASS_PALADIN},
                                                        {"hunter", CLASS_HUNTER},   {"rogue", CLASS_ROGUE},
                                                        {"priest", CLASS_PRIEST},   {"deathknight", CLASS_DEATH_KNIGHT},
                                                        {"shaman", CLASS_SHAMAN},   {"mage", CLASS_MAGE},
                                                        {"warlock", CLASS_WARLOCK}, {"druid", CLASS_DRUID}};

// The players' faction templates (Human, Orc): what each side's creatures are friendly or hostile to
constexpr uint32 ALLIANCE_PLAYERS = 1;
constexpr uint32 HORDE_PLAYERS = 2;

std::vector<uint32> const NONE;
std::vector<GuideSpawn> const NO_SPAWNS;

// "orc rogue/troll !priest" (groups split by "/"; a group's entries joined by spaces in a step's column, by "&"
// after an element's "@" or a next guide's)
GuideCondition ParseCondition(std::string const& text, char entrySeparator)
{
    GuideCondition condition;
    if (text.empty() || text == "-")
        return condition;

    for (std::string_view group : Acore::Tokenize(text, '/', false))
    {
        std::vector<GuideConditionOption> options;
        for (std::string_view word : Acore::Tokenize(group, entrySeparator, false))
        {
            GuideConditionOption option;
            if (!word.empty() && word[0] == '!')
            {
                option.negate = true;
                word.remove_prefix(1);
            }
            std::string name(word);
            if (auto race = RACES.find(name); race != RACES.end())
                option.raceMask = 1 << (race->second - 1);
            else if (auto cls = CLASSES.find(name); cls != CLASSES.end())
                option.classMask = 1 << (cls->second - 1);
            else if (name == "alliance")
                option.team = TEAM_ALLIANCE;
            else if (name == "horde")
                option.team = TEAM_HORDE;
            else
                continue;
            options.push_back(option);
        }
        if (!options.empty())
            condition.groups.push_back(std::move(options));
    }
    return condition;
}

std::vector<uint32> ParseEntries(std::string const& text)
{
    std::vector<uint32> entries;
    for (std::string_view entry : Acore::Tokenize(text, ',', false))
        entries.push_back(static_cast<uint32>(std::stoul(std::string(entry))));
    return entries;
}

// One element column: "<type> <fields...>[ @<condition>]"
bool ParseElement(std::string const& column, GuideElement& element)
{
    std::string text = column;
    if (size_t at = text.find(" @"); at != std::string::npos)
    {
        element.condition = ParseCondition(text.substr(at + 2), '&');
        text.resize(at);
    }

    std::istringstream in(text);
    std::string type;
    in >> type;
    if (type == "goto")
    {
        element.type = GuideElementType::Goto;
        in >> element.mapId >> element.x >> element.y >> element.radius;
    }
    else if (type == "accept" || type == "turnin" || type == "onquest" || type == "notonquest" || type == "turnedin" ||
             type == "questcomplete" || type == "available" || type == "abandon")
    {
        static std::unordered_map<std::string, GuideElementType> const QUEST_TYPES = {
            {"accept", GuideElementType::Accept},       {"turnin", GuideElementType::TurnIn},
            {"onquest", GuideElementType::OnQuest},     {"notonquest", GuideElementType::NotOnQuest},
            {"turnedin", GuideElementType::TurnedIn},   {"questcomplete", GuideElementType::QuestComplete},
            {"available", GuideElementType::Available}, {"abandon", GuideElementType::Abandon}};
        element.type = QUEST_TYPES.at(type);
        in >> element.id;
    }
    else if (type == "complete")
    {
        element.type = GuideElementType::Complete;
        in >> element.id >> element.objective;
    }
    else if (type == "xp")
    {
        element.type = GuideElementType::Xp;
        in >> element.id;
        if (!(in >> element.xp))
        {
            element.xp = 0.0f;
            in.clear();
        }
    }
    else if (type == "train")
    {
        element.type = GuideElementType::Train;
        in >> element.id;
    }
    else if (type == "collect")
    {
        element.type = GuideElementType::Collect;
        in >> element.id >> element.count >> element.quest >> element.objective;
    }
    else if (type == "trainer" || type == "vendor")
        element.type = type == "trainer" ? GuideElementType::Trainer : GuideElementType::Vendor;
    else if (type == "use")
    {
        element.type = GuideElementType::Use;
        in >> element.id;
    }
    else if (type == "hs" || type == "home")
        element.type = type == "hs" ? GuideElementType::Hearth : GuideElementType::Home;
    else if (type == "deathskip")
        element.type = GuideElementType::DeathSkip;
    else if (type == "fp" || type == "fly" || type == "side")
    {
        element.type = type == "fp"    ? GuideElementType::FlightPath
                       : type == "fly" ? GuideElementType::Fly
                                       : GuideElementType::Side;
        in >> element.other;
        std::replace(element.other.begin(), element.other.end(), '_', ' ');
    }
    else if (type == "below" || type == "atleast")
    {
        element.type = type == "below" ? GuideElementType::Below : GuideElementType::AtLeast;
        in >> element.id;
    }
    else if (type == "target" || type == "mob")
    {
        element.type = type == "target" ? GuideElementType::Target : GuideElementType::Mob;
        std::string entries;
        in >> entries;
        element.entries = ParseEntries(entries);
    }
    else if (type == "zoneskip")
    {
        element.type = GuideElementType::ZoneSkip;
        std::string areas;
        in >> areas >> element.flags;
        element.entries = ParseEntries(areas);
    }
    else if (type == "itemcount" || type == "skill")
    {
        element.type = type == "itemcount" ? GuideElementType::ItemCount : GuideElementType::Skill;
        in >> element.id >> element.count >> element.flags;
    }
    else if (type == "money")
    {
        element.type = GuideElementType::Money;
        in >> element.count >> element.flags;
    }
    else if (type == "maxlevel")
    {
        element.type = GuideElementType::MaxLevel;
        in >> element.id >> element.other;
    }
    else if (type == "aura")
    {
        element.type = GuideElementType::Aura;
        in >> element.id >> element.amount >> element.count >> element.flags;
    }
    else if (type == "reputation")
    {
        element.type = GuideElementType::Reputation;
        in >> element.id >> element.count >> element.amount >> element.flags;
    }
    else if (type == "group")
        element.type = GuideElementType::Group;
    else if (type == "bindlocation")
    {
        element.type = GuideElementType::BindLocation;
        in >> element.id >> element.flags;
    }
    else if (type == "equipped")
    {
        element.type = GuideElementType::Equipped;
        in >> element.count >> element.id >> element.flags;
    }
    else if (type == "gossip")
    {
        element.type = GuideElementType::Gossip;
        std::string options;
        in >> element.id >> options;
        element.entries = ParseEntries(options);
    }
    else if (type == "keep")
    {
        element.type = GuideElementType::Keep;
        std::string items;
        in >> items;
        element.entries = ParseEntries(items);
    }
    else if (type == "other")
    {
        element.type = GuideElementType::Other;
        in >> element.other;
    }
    else
        return false;

    return !in.fail();
}
}  // namespace

bool GuideCondition::Holds(Player* player) const
{
    uint32 raceMask = player->getRaceMask();
    uint32 classMask = player->getClassMask();
    int8 team = static_cast<int8>(player->GetTeamId());
    if (groups.empty())
        return true;
    for (std::vector<GuideConditionOption> const& group : groups)
    {
        bool all = true;
        for (GuideConditionOption const& option : group)
        {
            bool match = (option.raceMask & raceMask) || (option.classMask & classMask) || option.team == team;
            if (match == option.negate)
            {
                all = false;
                break;
            }
        }
        if (all)
            return true;
    }
    return false;
}

bool GuideElement::IsQuestWork() const
{
    return type == GuideElementType::Accept || type == GuideElementType::TurnIn || type == GuideElementType::Complete ||
           type == GuideElementType::Xp || type == GuideElementType::Collect || type == GuideElementType::Abandon;
}

bool GuideElement::IsService() const
{
    return type == GuideElementType::Train || type == GuideElementType::Trainer || type == GuideElementType::Vendor ||
           type == GuideElementType::Home || type == GuideElementType::FlightPath || type == GuideElementType::Fly ||
           type == GuideElementType::Hearth || type == GuideElementType::Use || type == GuideElementType::Gossip;
}

void GuideMgr::Load(std::string const& path)
{
    _guides.clear();
    _raceStart.clear();
    if (path.empty())
        return;

    uint32 oldMSTime = getMSTime();
    std::ifstream file(path);
    if (!file)
    {
        LOG_ERROR("playerbots", "Guide strategy: can't read Playerbots.Guide.DataFile {}", path);
        return;
    }

    uint32 steps = 0;
    uint32 skipped = 0;
    std::unordered_map<std::string, std::unordered_map<std::string, uint32>> labels;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> columns;
        for (std::string_view column : Acore::Tokenize(line, '\t', true))
            columns.emplace_back(column);

        if (columns[0] == "L" && columns.size() >= 4)
        {
            labels[columns[1]][columns[3]] = static_cast<uint32>(std::stoul(columns[2]));
            continue;
        }
        if (columns[0] == "R" && columns.size() >= 3)
        {
            if (auto race = RACES.find(columns[1]); race != RACES.end())
                _raceStart[race->second] = columns[2];
        }
        else if (columns[0] == "G" && columns.size() >= 4)
        {
            Guide& guide = _guides[columns[1]];
            guide.name = columns[1];
            // "name@condition;name..." (a group's entries joined by "&", as on elements)
            for (std::string_view entry : Acore::Tokenize(columns[2], ';', false))
            {
                std::string text(entry);
                GuideNext next;
                size_t at = text.find('@');
                next.name = text.substr(0, at);
                if (at != std::string::npos)
                    next.condition = ParseCondition(text.substr(at + 1), '&');
                guide.next.push_back(std::move(next));
            }
            guide.condition = ParseCondition(columns[3], ' ');
        }
        else if (columns[0] == "S" && columns.size() >= 5)
        {
            auto guide = _guides.find(columns[1]);
            if (guide == _guides.end())
            {
                ++skipped;
                continue;
            }
            GuideStep step;
            step.number = static_cast<uint32>(std::stoul(columns[2]));
            step.condition = ParseCondition(columns[3], ' ');
            for (size_t i = 4; i < columns.size(); ++i)
            {
                GuideElement element;
                if (ParseElement(columns[i], element))
                {
                    step.side |= element.type == GuideElementType::Side;
                    step.elements.push_back(std::move(element));
                }
                else
                    LOG_WARN("playerbots", "Guide strategy: {} step {}: can't read '{}'", columns[1], step.number,
                             columns[i]);
            }
            guide->second.steps.push_back(std::move(step));
            ++steps;
        }
    }

    for (auto& [name, guideLabels] : labels)
        if (auto guide = _guides.find(name); guide != _guides.end())
            guide->second.labels = std::move(guideLabels);
    ResolveLabels();
    LoadQuestRelations();
    LoadGossipCredits();
    LoadUses();
    LoadItemDroppers();
    LoadTrains();
    LoadSpawns();
    LoadCreatureIndexes();
    LOG_INFO("server.loading", ">> Loaded {} guides ({} steps, {} skipped) for the guide strategy in {} ms",
             _guides.size(), steps, skipped, GetMSTimeDiffToNow(oldMSTime));
    GuideTravel::instance().Load();
}

void GuideMgr::LoadTrains()
{
    // Each ".train" spell as the realm's trainers teach it: the trainer's spell (GuideRules::TrainerSpellFor) and what
    // it gives (a trainer spell that casts learns the spells it wraps). None teaches it: passed by, and listed here.
    std::vector<Trainer::Trainer const*> trainers;
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
        if (Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(entry))
            trainers.push_back(trainer);
    std::set<uint32> wanted;
    for (auto const& [name, guide] : _guides)
        for (GuideStep const& step : guide.steps)
            for (GuideElement const& element : step.elements)
                if (element.type == GuideElementType::Train)
                    wanted.insert(element.id);
    std::string untrainable;
    for (uint32 spell : wanted)
    {
        uint32 taught = 0;
        for (Trainer::Trainer const* trainer : trainers)
            if ((taught = GuideRules::TrainerSpellFor(trainer, spell)))
                break;
        if (!taught)
        {
            _untrainable.insert(spell);
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            untrainable += Acore::StringFormat("{}{} ({})", untrainable.empty() ? "" : ", ",
                                               info && info->SpellName[0] ? info->SpellName[0] : "?", spell);
            continue;
        }
        std::vector<uint32>& as = _trainedAs[spell];
        as.push_back(taught);
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(taught))
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    as.push_back(effect.TriggerSpell);
    }
    LOG_INFO("server.loading", ">> Guide strategy: {} trained spells, {} no trainer teaches (passed by): {}",
             wanted.size(), _untrainable.size(), untrainable);
}

std::vector<uint32> const& GuideMgr::GetTrainedAs(uint32 spell) const
{
    static std::vector<uint32> const none;
    auto it = _trainedAs.find(spell);
    return it != _trainedAs.end() ? it->second : none;
}

void GuideMgr::LoadQuestRelations()
{
    auto reverse = [](QuestRelations const* relations, std::unordered_map<uint32, std::vector<uint32>>& out)
    {
        out.clear();
        for (auto const& [entry, questId] : *relations)
            out[questId].push_back(entry);
    };
    reverse(sObjectMgr->GetCreatureQuestRelationMap(), _questStarters);
    reverse(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), _questEnders);
    reverse(sObjectMgr->GetGOQuestRelationMap(), _questStarterObjects);
    reverse(sObjectMgr->GetGOQuestInvolvedRelationMap(), _questEnderObjects);
    _questStarterItems.clear();
    for (auto const& [entry, item] : *sObjectMgr->GetItemTemplateStore())
        if (item.StartQuest)
            _questStarterItems[item.StartQuest].push_back(entry);

    // ObjectMgr only looks these up by trigger
    _questTriggers.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT id, quest FROM areatrigger_involvedrelation"))
        do
        {
            Field* fields = result->Fetch();
            _questTriggers[fields[1].Get<uint32>()].push_back(fields[0].Get<uint32>());
        } while (result->NextRow());

    // Quest items held by objects (Scavenged Goods in the Equipment Boxes), and where those objects spawn
    _itemObjects.clear();
    _objectSpawns.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT GameObjectEntry, ItemId FROM gameobject_questitem"))
        do
        {
            Field* fields = result->Fetch();
            _itemObjects[fields[1].Get<uint32>()].push_back(fields[0].Get<uint32>());
            _objectSpawns[fields[0].Get<uint32>()];
        } while (result->NextRow());
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (auto spawns = _objectSpawns.find(data.id); spawns != _objectSpawns.end())
            spawns->second.push_back({data.mapid, data.posX, data.posY, data.posZ});
}

void GuideMgr::ResolveLabels()
{
    // A side step stays active through the step it names: "next" (the following step), or the step with that label
    // (or the first kept after it, when the labelled step itself was left out). A ".maxlevel" naming a label skips on
    // to that step the same way.
    for (auto& [name, guide] : _guides)
    {
        auto firstFrom = [&guide](uint32 number, uint32 from)
        {
            uint32 index = from;
            while (index + 1 < guide.steps.size() && guide.steps[index].number < number)
                ++index;
            return index;
        };
        for (uint32 i = 0; i < guide.steps.size(); ++i)
        {
            GuideStep& step = guide.steps[i];
            for (GuideElement& element : step.elements)
            {
                auto label = guide.labels.find(element.other);
                if (element.type == GuideElementType::MaxLevel && label != guide.labels.end() &&
                    label->second > step.number)
                    element.count = firstFrom(label->second, i + 1);
            }
            if (!step.side)
                continue;
            std::string until;
            for (GuideElement const& element : step.elements)
                if (element.type == GuideElementType::Side)
                    until = element.other;
            uint32 number = step.number + 1;
            if (auto label = guide.labels.find(until); until != "next" && label != guide.labels.end())
                number = label->second;
            step.sideUntil = firstFrom(number, i + 1);
        }
    }
}

void GuideMgr::LoadUses()
{
    // Each ".use" item's spell: how it is aimed (spell focus, object, creature, spot, self), what at (the spell's
    // target conditions, and creatures whose SmartAI answers being hit by it), at what health, alive or dead. And the
    // items whose use completes a quest the route has (the Winterhoof Cleansing Totem at the well): the step's
    // ".complete" uses them, though RestedXP only says so in its text.
    _uses.clear();
    _keep.clear();
    _questUseItems.clear();
    std::unordered_map<uint32, uint32> itemBySpell;
    std::vector<uint32> wanted;
    std::unordered_set<uint32> completes;
    for (auto const& [name, guide] : _guides)
        for (GuideStep const& step : guide.steps)
            for (GuideElement const& element : step.elements)
            {
                if (element.type == GuideElementType::Keep)
                    _keep.insert(element.entries.begin(), element.entries.end());
                else if (element.type == GuideElementType::Use)
                    wanted.push_back(element.id);
                else if (element.type == GuideElementType::Complete)
                    completes.insert(element.id);
            }
    for (auto const& [entry, proto] : *sObjectMgr->GetItemTemplateStore())
        for (auto const& spell : proto.Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(static_cast<uint32>(spell.SpellId)))
                    for (SpellEffectInfo const& effect : spellInfo->Effects)
                        if (effect.Effect == SPELL_EFFECT_QUEST_COMPLETE && completes.count(effect.MiscValue) &&
                            !_questUseItems.count(effect.MiscValue))
                        {
                            _questUseItems[effect.MiscValue] = entry;
                            wanted.push_back(entry);
                        }

    for (uint32 item : wanted)
    {
        if (_uses.count(item))
            continue;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item);
        if (!proto)
            continue;

        GuideUse use;
        for (auto const& spell : proto->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                use.spell = static_cast<uint32>(spell.SpellId);
                break;
            }
        SpellInfo const* spellInfo = use.spell ? sSpellMgr->GetSpellInfo(use.spell) : nullptr;
        if (!spellInfo)
        {
            // Worn (a weapon, goggles, a detector), or opened (a crate, a footlocker)
            if (proto->InventoryType != INVTYPE_NON_EQUIP)
                use.mode = GuideUse::Mode::Equip;
            else if (proto->HasFlag(ITEM_FLAG_HAS_LOOT))
                use.mode = GuideUse::Mode::Open;
            else
                continue;
            _uses[item] = use;
            continue;
        }

        bool nearbyUnit = false;
        bool nearbyObject = false;
        for (SpellEffectInfo const& effect : spellInfo->Effects)
        {
            for (Targets target : {effect.TargetA.GetTarget(), effect.TargetB.GetTarget()})
            {
                nearbyUnit |= target == TARGET_UNIT_NEARBY_ENTRY;
                nearbyObject |= target == TARGET_GAMEOBJECT_NEARBY_ENTRY;
            }
            if (!effect.ImplicitTargetConditions)
                continue;
            for (Condition const* condition : *effect.ImplicitTargetConditions)
            {
                if (condition->NegativeCondition)
                    continue;
                if (condition->ConditionType == CONDITION_OBJECT_ENTRY_GUID && condition->ConditionValue2)
                {
                    if (condition->ConditionValue1 == TYPEID_UNIT)
                        use.units.push_back(condition->ConditionValue2);
                    else if (condition->ConditionValue1 == TYPEID_GAMEOBJECT)
                        use.objects.push_back(condition->ConditionValue2);
                }
                else if (condition->ConditionType == CONDITION_HP_PCT &&
                         (condition->ConditionValue2 == COMP_TYPE_LOW ||
                          condition->ConditionValue2 == COMP_TYPE_LOW_EQ))
                    use.health = static_cast<float>(condition->ConditionValue1);
            }
        }
        if (spellInfo->TargetAuraState == AURA_STATE_HEALTHLESS_20_PERCENT)
            use.health = 20.0f;
        else if (spellInfo->TargetAuraState == AURA_STATE_HEALTHLESS_35_PERCENT)
            use.health = 35.0f;
        use.corpse = spellInfo->IsAllowingDeadTarget();
        use.focus = spellInfo->RequiresSpellFocus;
        use.range = std::max({spellInfo->GetMaxRange(false), spellInfo->GetMaxRange(true), 5.0f});

        // A trap it places (itself, or linked to the spell focus it places): Tharnariun's Hope's bear trap
        for (SpellEffectInfo const& effect : spellInfo->Effects)
            if (effect.Effect == SPELL_EFFECT_TRANS_DOOR || effect.Effect == SPELL_EFFECT_SUMMON_OBJECT_WILD)
                if (GameObjectTemplate const* placed = sObjectMgr->GetGameObjectTemplate(effect.MiscValue))
                {
                    if (placed->type == GAMEOBJECT_TYPE_TRAP)
                        use.trap = placed->entry;
                    else if (placed->type == GAMEOBJECT_TYPE_SPELL_FOCUS && placed->spellFocus.linkedTrapId)
                        use.trap = placed->spellFocus.linkedTrapId;
                }
        bool trap = use.trap != 0;

        uint32 mask = spellInfo->GetExplicitTargetMask();
        if (trap)
            use.mode = GuideUse::Mode::Lure;
        else if (use.focus)
            use.mode = GuideUse::Mode::Focus;
        else if (mask & TARGET_FLAG_GAMEOBJECT_MASK)
            use.mode = GuideUse::Mode::Object;
        else if (mask & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_CORPSE_MASK))
            use.mode = GuideUse::Mode::Unit;
        else if (nearbyObject)
            use.mode = GuideUse::Mode::NearbyObject;
        else if (nearbyUnit)
            use.mode = GuideUse::Mode::NearbyUnit;
        else if (mask & TARGET_FLAG_DEST_LOCATION)
            use.mode = GuideUse::Mode::Dest;
        _uses[item] = use;
        itemBySpell[use.spell] = item;
    }

    // Creatures a C++ script makes an item's spell work on (what the data can't tell): Triage's injured soldiers
    if (auto triage = _uses.find(16991); triage != _uses.end())  // Triage Bandage
        triage->second.units.insert(triage->second.units.end(), {12923, 12924, 12925, 12936, 12937, 12938});

    // Creatures scripted to answer the spell hitting them (SMART_EVENT_SPELLHIT): what it is meant for.
    for (auto const& [entry, creatureTemplate] : *sObjectMgr->GetCreatureTemplates())
    {
        if (creatureTemplate.AIName != "SmartAI")
            continue;
        for (SmartScriptHolder const& holder : sSmartScriptMgr->GetScript(entry, SMART_SCRIPT_TYPE_CREATURE))
            if (holder.event.type == SMART_EVENT_SPELLHIT)
                if (auto item = itemBySpell.find(holder.event.spellHit.spell); item != itemBySpell.end())
                    _uses[item->second].units.push_back(entry);
    }

    uint32 counts[10] = {};
    uint32 lowHealth = 0;
    uint32 corpses = 0;
    for (auto const& [item, use] : _uses)
    {
        ++counts[static_cast<uint8>(use.mode)];
        lowHealth += use.health > 0.0f;
        corpses += use.corpse;
    }
    LOG_INFO("server.loading",
             ">> Guide strategy: {} items to use (equip {}, spell focus {}, object {}, nearby object {}, creature {}, "
             "nearby creature {}, spot {}, self {}, open {}, trap {}; {} at low health, {} on corpses)",
             _uses.size(), counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7],
             counts[8], counts[9], lowHealth, corpses);
}

void GuideMgr::LoadSpawns()
{
    // The creatures the route names: quest givers and enders, objective creatures, ".target", ".mob" and gossip NPCs;
    // and the objective objects (a grave to use), next to the objects holding quest items.
    std::unordered_set<uint32> entries;
    std::unordered_set<uint32> objects;
    auto add = [&](std::vector<uint32> const& list) { entries.insert(list.begin(), list.end()); };
    for (auto const& [name, guide] : _guides)
        for (GuideStep const& step : guide.steps)
            for (GuideElement const& element : step.elements)
            {
                if (element.type == GuideElementType::Accept)
                    add(GetQuestStarters(element.id));
                else if (element.type == GuideElementType::TurnIn)
                    add(GetQuestEnders(element.id));
                else if (element.type == GuideElementType::Target || element.type == GuideElementType::Mob)
                    add(element.entries);
                else if (element.type == GuideElementType::Gossip)
                {
                    std::vector<uint32> creatures;
                    std::vector<uint32> gossipObjects;
                    GossipTargets(nullptr, step, element, creatures, gossipObjects);
                    add(creatures);
                    for (uint32 object : gossipObjects)
                        if (!_objectSpawns.count(object))
                            objects.insert(object);
                }
                else if (element.type == GuideElementType::Use)
                {
                    if (GuideUse const* use = GetUse(element.id))
                        add(use->units);
                }
                else if (element.type == GuideElementType::Complete)
                {
                    for (GuideGossipCredit const& credit : GetGossipCredits(element.id))
                        if (!credit.object)
                            entries.insert(credit.entry);
                        else if (!_objectSpawns.count(credit.entry))
                            objects.insert(credit.entry);
                    if (GetQuestEmote(element.id) || HasScriptedGiver(element.id))
                        add(GetQuestStarters(element.id));
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(element.id))
                    {
                        for (int32 objective : quest->RequiredNpcOrGo)
                            if (objective > 0)
                                entries.insert(static_cast<uint32>(objective));
                            else if (objective < 0 && !_objectSpawns.count(static_cast<uint32>(-objective)))
                                objects.insert(static_cast<uint32>(-objective));
                        for (uint32 item : quest->RequiredItemId)
                            add(GetItemDroppers(item));
                    }
                }
                else if (element.type == GuideElementType::Collect)
                {
                    add(GetItemDroppers(element.id));
                    add(GetItemVendors(element.id));
                }
                if (element.type == GuideElementType::Accept || element.type == GuideElementType::TurnIn)
                    for (uint32 object : element.type == GuideElementType::Accept ? GetQuestStarterObjects(element.id)
                                                                                  : GetQuestEnderObjects(element.id))
                        if (!_objectSpawns.count(object))
                            objects.insert(object);
            }

    _spawns.clear();
    _spawnCells.clear();
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        GuideSpawn spawn{data.mapid, data.posX, data.posY, data.posZ};
        bool named = false;
        for (uint32 entry : {data.id, data.id2, data.id3})
            if (entry && entries.count(entry))
            {
                _spawns[entry].push_back(spawn);
                named = true;
            }
        if (named)
            _spawnCells[SpawnCell(spawn.mapId, spawn.x, spawn.y)].push_back(spawn);
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (objects.count(data.id))
            _objectSpawns[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});
}

uint64 GuideMgr::SpawnCell(uint32 mapId, float x, float y)
{
    auto cell = [](float value) { return uint64(int64(std::floor(value / SPAWN_CELL)) & 0xFFFFF); };
    return (uint64(mapId) << 40) | (cell(x) << 20) | cell(y);
}

std::vector<float> GuideMgr::SpawnHeightsNear(uint32 mapId, float x, float y, float radius) const
{
    std::vector<float> heights;
    for (float dx : {-SPAWN_CELL, 0.0f, SPAWN_CELL})
        for (float dy : {-SPAWN_CELL, 0.0f, SPAWN_CELL})
        {
            auto cell = _spawnCells.find(SpawnCell(mapId, x + dx, y + dy));
            if (cell == _spawnCells.end())
                continue;
            for (GuideSpawn const& spawn : cell->second)
                if (std::hypot(spawn.x - x, spawn.y - y) <= radius)
                    heights.push_back(spawn.z);
        }
    return heights;
}

void GuideMgr::LoadItemDroppers()
{
    // The creatures that drop the route's items (an item objective, a ".collect", a quest's item to take somewhere):
    // where to look for them when none is in sight
    std::unordered_set<uint32> items;
    for (auto const& [name, guide] : _guides)
        for (GuideStep const& step : guide.steps)
            for (GuideElement const& element : step.elements)
            {
                if (element.type == GuideElementType::Collect)
                    items.insert(element.id);
                else if (element.type == GuideElementType::Complete || element.type == GuideElementType::Accept)
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(element.id))
                    {
                        for (uint32 item : quest->RequiredItemId)
                            if (item)
                                items.insert(item);
                        for (uint32 item : quest->ItemDrop)
                            if (item)
                                items.insert(item);
                    }
            }

    // A drop counts when it's worth killing for: a quest drop, 5% or more, or a share of a loot group
    _itemDroppers.clear();
    std::unordered_map<uint32, std::vector<uint32>> lootDrops;  // loot id -> the route's items in it
    if (QueryResult result =
            WorldDatabase.Query("SELECT Entry, Item FROM creature_loot_template WHERE Reference = 0 AND "
                                "(QuestRequired = 1 OR Chance >= 5 OR (Chance = 0 AND GroupId > 0))"))
        do
        {
            Field* fields = result->Fetch();
            if (items.count(fields[1].Get<uint32>()))
                lootDrops[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
        } while (result->NextRow());
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
        if (auto drops = lootDrops.find(creature.lootid); creature.lootid && drops != lootDrops.end())
            for (uint32 item : drops->second)
                _itemDroppers[item].push_back(entry);

    // And the vendors that sell them (RestedXP's ".collect" often means buying: a weapon, food, Simple Wood)
    _itemVendors.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT entry, item FROM npc_vendor WHERE item > 0"))
        do
        {
            Field* fields = result->Fetch();
            if (items.count(fields[1].Get<uint32>()))
                _itemVendors[fields[1].Get<uint32>()].push_back(fields[0].Get<uint32>());
        } while (result->NextRow());

    // And the chests that hold them (Power Crystals, a Hippogryph Egg), next to gameobject_questitem's objects. Not the
    // world's treasure chests (Battered Chest, Solid Chest: hundreds of items, a Minor Mana Potion at 25%, rarely
    // spawned where they can): a quest drop, half the time or more, or one of a small loot table's.
    std::unordered_map<uint32, std::vector<uint32>> chestLoot;  // loot id -> the route's items in it
    if (QueryResult result = WorldDatabase.Query(
            "SELECT l.Entry, l.Item FROM gameobject_loot_template l JOIN (SELECT Entry, COUNT(*) AS n FROM "
            "gameobject_loot_template GROUP BY Entry) t ON t.Entry = l.Entry WHERE l.Reference = 0 AND "
            "(l.QuestRequired = 1 OR l.Chance >= 50 OR (t.n <= 20 AND (l.Chance >= 5 OR (l.Chance = 0 AND "
            "l.GroupId > 0))))"))
        do
        {
            Field* fields = result->Fetch();
            if (items.count(fields[1].Get<uint32>()))
                chestLoot[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
        } while (result->NextRow());
    std::unordered_set<uint32> chests;
    for (auto const& [entry, object] : *sObjectMgr->GetGameObjectTemplates())
        if (auto loot = chestLoot.find(object.GetLootId());
            object.type == GAMEOBJECT_TYPE_CHEST && loot != chestLoot.end())
            for (uint32 item : loot->second)
            {
                std::vector<uint32>& objects = _itemObjects[item];
                if (std::find(objects.begin(), objects.end(), entry) != objects.end())
                    continue;
                objects.push_back(entry);
                if (!_objectSpawns.count(entry))
                    chests.insert(entry);
            }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (chests.count(data.id))
            _objectSpawns[data.id].push_back({data.mapid, data.posX, data.posY, data.posZ});
}

std::vector<uint32> const& GuideMgr::GetItemVendors(uint32 item) const
{
    auto it = _itemVendors.find(item);
    return it != _itemVendors.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetItemDroppers(uint32 item) const
{
    auto it = _itemDroppers.find(item);
    return it != _itemDroppers.end() ? it->second : NONE;
}

std::vector<Guide const*> GuideMgr::GetGuides() const
{
    std::vector<Guide const*> guides;
    for (auto const& [name, guide] : _guides)
        guides.push_back(&guide);
    return guides;
}

void GuideMgr::LoadCreatureIndexes()
{
    // Where each faction's creatures stand (a camp, a town: guards), and the NPCs that serve
    _enemyCells[TEAM_ALLIANCE].clear();
    _enemyCells[TEAM_HORDE].clear();
    _serviceCells.clear();
    FactionTemplateEntry const* alliance = sFactionTemplateStore.LookupEntry(ALLIANCE_PLAYERS);
    FactionTemplateEntry const* horde = sFactionTemplateStore.LookupEntry(HORDE_PLAYERS);
    uint32 const services = UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR | UNIT_NPC_FLAG_INNKEEPER |
                            UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_TRAINER;
    uint32 enemies = 0;
    uint32 servers = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(data.id);
        if (!creature)
            continue;
        GuideSpawn spawn{data.mapid, data.posX, data.posY, data.posZ};
        uint64 cell = SpawnCell(spawn.mapId, spawn.x, spawn.y);
        if (creature->npcflag & services)
        {
            _serviceCells[cell].push_back({data.id, spawn});
            ++servers;
        }
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
        if (!faction || !alliance || !horde || creature->HasFlagsExtra(CREATURE_FLAG_EXTRA_CIVILIAN))
            continue;
        if (faction->IsFriendlyTo(*alliance) && faction->IsHostileTo(*horde))
            _enemyCells[TEAM_HORDE][cell].push_back(spawn);
        else if (faction->IsFriendlyTo(*horde) && faction->IsHostileTo(*alliance))
            _enemyCells[TEAM_ALLIANCE][cell].push_back(spawn);
        else
            continue;
        ++enemies;
    }
    LOG_INFO("server.loading", ">> Guide strategy: {} faction creatures, {} serving NPCs indexed", enemies, servers);
}

bool GuideMgr::EnemyNear(TeamId team, uint32 mapId, float x, float y, float radius) const
{
    if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
        return false;
    for (float dx : {-SPAWN_CELL, 0.0f, SPAWN_CELL})
        for (float dy : {-SPAWN_CELL, 0.0f, SPAWN_CELL})
        {
            auto cell = _enemyCells[team].find(SpawnCell(mapId, x + dx, y + dy));
            if (cell == _enemyCells[team].end())
                continue;
            for (GuideSpawn const& spawn : cell->second)
                if (std::hypot(spawn.x - x, spawn.y - y) <= radius)
                    return true;
        }
    return false;
}

std::vector<std::pair<uint32, GuideSpawn>> GuideMgr::ServicesNear(uint32 mapId, float x, float y, float radius) const
{
    std::vector<std::pair<uint32, GuideSpawn>> found;
    int32 cells = int32(std::ceil(radius / SPAWN_CELL));
    for (int32 cx = -cells; cx <= cells; ++cx)
        for (int32 cy = -cells; cy <= cells; ++cy)
        {
            auto cell = _serviceCells.find(SpawnCell(mapId, x + cx * SPAWN_CELL, y + cy * SPAWN_CELL));
            if (cell == _serviceCells.end())
                continue;
            for (auto const& [entry, spawn] : cell->second)
                if (std::hypot(spawn.x - x, spawn.y - y) <= radius)
                    found.push_back({entry, spawn});
        }
    return found;
}

void GuideMgr::LoadGossipCredits()
{
    // What gives quests their events on the realm. SmartAI rows that credit a quest (or a kill, or give an item), a
    // linked row followed back to the row that starts its chain: when that is "on gossip option selected", the option
    // (its NPC or object, menu and option) does the quest's work. Any other start (a spell hitting, a waypoint, an
    // action list) still counts as the quest being credited somehow. Also event / spell / waypoint scripts that mark
    // a quest explored, and spells that complete one.
    _gossipQuests.clear();
    _gossipKills.clear();
    _gossipItems.clear();
    _eventCredited.clear();
    _scriptedGivers.clear();
    LoadScriptedGivers();

    struct Row
    {
        int32 entryOrGuid;
        uint8 source;
        uint32 id;
        uint32 link;
        uint32 event;
        uint32 eventParam1;
        uint32 eventParam2;
        uint32 action;
        uint32 actionParam1;
    };
    std::vector<Row> rows;
    std::map<std::tuple<int32, uint8, uint32>, size_t> linkedFrom;  // (entryorguid, source, linked id) -> the row
    if (QueryResult result = WorldDatabase.Query(
            "SELECT entryorguid, source_type, id, link, event_type, event_param1, event_param2, action_type, "
            "action_param1 FROM smart_scripts"))
        do
        {
            Field* fields = result->Fetch();
            Row row{fields[0].Get<int32>(),  fields[1].Get<uint8>(),  fields[2].Get<uint32>(),
                    fields[3].Get<uint32>(), fields[4].Get<uint32>(), fields[5].Get<uint32>(),
                    fields[6].Get<uint32>(), fields[7].Get<uint32>(), fields[8].Get<uint32>()};
            if (row.link)
                linkedFrom[{row.entryOrGuid, row.source, row.link}] = rows.size();
            rows.push_back(row);
        } while (result->NextRow());

    for (Row const& row : rows)
    {
        bool questCredit = row.action == SMART_ACTION_CALL_AREAEXPLOREDOREVENTHAPPENS ||
                           row.action == SMART_ACTION_CALL_GROUPEVENTHAPPENS;
        if (!row.actionParam1 ||
            (!questCredit && row.action != SMART_ACTION_CALL_KILLEDMONSTER && row.action != SMART_ACTION_ADD_ITEM))
            continue;
        if (questCredit)
            _eventCredited.insert(row.actionParam1);
        Row const* start = &row;
        for (uint32 hops = 0; start->event == SMART_EVENT_LINK && hops < 10; ++hops)
        {
            auto from = linkedFrom.find({start->entryOrGuid, start->source, start->id});
            if (from == linkedFrom.end())
                break;
            start = &rows[from->second];
        }
        if (start->event != SMART_EVENT_GOSSIP_SELECT ||
            (row.source != SMART_SCRIPT_TYPE_CREATURE && row.source != SMART_SCRIPT_TYPE_GAMEOBJECT))
            continue;
        bool object = row.source == SMART_SCRIPT_TYPE_GAMEOBJECT;
        uint32 entry = 0;
        if (row.entryOrGuid > 0)
            entry = static_cast<uint32>(row.entryOrGuid);
        else if (object)
        {
            if (GameObjectData const* data = sObjectMgr->GetGameObjectData(static_cast<uint32>(-row.entryOrGuid)))
                entry = data->id;
        }
        else if (CreatureData const* data = sObjectMgr->GetCreatureData(static_cast<uint32>(-row.entryOrGuid)))
            entry = data->id;
        if (!entry)
            continue;
        auto& credits = row.action == SMART_ACTION_CALL_KILLEDMONSTER ? _gossipKills
                        : row.action == SMART_ACTION_ADD_ITEM         ? _gossipItems
                                                                      : _gossipQuests;
        credits[row.actionParam1].push_back({entry, object, start->eventParam1, start->eventParam2});
    }

    for (char const* table : {"event_scripts", "spell_scripts", "waypoint_scripts"})
        if (QueryResult result =
                WorldDatabase.Query("SELECT datalong FROM {} WHERE command = {}", table, SCRIPT_COMMAND_QUEST_EXPLORED))
            do
                _eventCredited.insert(result->Fetch()[0].Get<uint32>());
            while (result->NextRow());
    for (uint32 spellId = 0; spellId < sSpellMgr->GetSpellInfoStoreSize(); ++spellId)
        if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
            for (SpellEffectInfo const& effect : spellInfo->Effects)
                if (effect.Effect == SPELL_EFFECT_QUEST_COMPLETE && effect.MiscValue > 0)
                    _eventCredited.insert(static_cast<uint32>(effect.MiscValue));
}

void GuideMgr::LoadScriptedGivers()
{
    // Quest givers with a script for the quest being taken: SmartAI "on quest accepted" (an escort's path, a fight,
    // attackers summoned), or a C++ script (escorts written in C++)
    std::unordered_set<uint32> smartAccept;
    std::unordered_set<uint64> smartAcceptQuest;
    if (QueryResult result = WorldDatabase.Query(
            "SELECT entryorguid, event_param1 FROM smart_scripts WHERE source_type = {} AND event_type = {}",
            SMART_SCRIPT_TYPE_CREATURE, SMART_EVENT_ACCEPTED_QUEST))
        do
        {
            Field* fields = result->Fetch();
            int32 entryOrGuid = fields[0].Get<int32>();
            uint32 entry = entryOrGuid > 0 ? static_cast<uint32>(entryOrGuid) : 0;
            if (!entry)
                if (CreatureData const* data = sObjectMgr->GetCreatureData(static_cast<uint32>(-entryOrGuid)))
                    entry = data->id;
            if (!fields[1].Get<uint32>())
                smartAccept.insert(entry);
            else
                smartAcceptQuest.insert((uint64(entry) << 32) | fields[1].Get<uint32>());
        } while (result->NextRow());
    for (auto const& [questId, givers] : _questStarters)
        for (uint32 entry : givers)
        {
            CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
            if (creature && (creature->ScriptID || smartAccept.count(entry) ||
                             smartAcceptQuest.count((uint64(entry) << 32) | questId)))
                _scriptedGivers.insert(questId);
        }
}

std::vector<GuideGossipCredit> const& GuideMgr::GetGossipCredits(uint32 questId) const
{
    static std::vector<GuideGossipCredit> const NO_CREDITS;
    auto it = _gossipQuests.find(questId);
    return it != _gossipQuests.end() ? it->second : NO_CREDITS;
}

uint32 GuideMgr::GetQuestEmote(uint32 questId) const
{
    // C++ scripts answering a text emote: what they need can't be read from the data
    static std::unordered_map<uint32, uint32> const EMOTES = {{2460, TEXT_EMOTE_SALUTE}};  // The Shattered Salute
    auto it = EMOTES.find(questId);
    return it != EMOTES.end() ? it->second : 0;
}

bool GuideMgr::CanCompleteEvent(uint32 questId) const
{
    return !GetQuestTriggers(questId).empty() || GetQuestUseItem(questId) || !GetGossipCredits(questId).empty() ||
           GetQuestEmote(questId) || HasScriptedGiver(questId) || _eventCredited.count(questId);
}

void GuideMgr::GossipTargets(Player* bot, GuideStep const& step, GuideElement const& element,
                             std::vector<uint32>& creatures, std::vector<uint32>& objects) const
{
    if (element.id)
    {
        creatures.push_back(element.id);
        return;
    }
    for (GuideElement const& other : step.elements)
        if (other.type == GuideElementType::Target && (!bot || GuideRules::Applies(bot, other)))
            creatures.insert(creatures.end(), other.entries.begin(), other.entries.end());
    if (!creatures.empty())
        return;

    auto add = [&](std::unordered_map<uint32, std::vector<GuideGossipCredit>> const& credits, uint32 key)
    {
        if (auto found = credits.find(key); key && found != credits.end())
            for (GuideGossipCredit const& credit : found->second)
                (credit.object ? objects : creatures).push_back(credit.entry);
    };
    for (GuideElement const& other : step.elements)
    {
        if (bot && !GuideRules::Applies(bot, other))
            continue;
        if (other.type == GuideElementType::Collect)
            add(_gossipItems, other.id);
        if (other.type != GuideElementType::Complete)
            continue;
        add(_gossipQuests, other.id);
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(other.id))
        {
            for (int32 objective : quest->RequiredNpcOrGo)
                if (objective > 0)
                    add(_gossipKills, static_cast<uint32>(objective));
            for (uint32 item : quest->RequiredItemId)
                add(_gossipItems, item);
        }
    }
}

std::vector<std::pair<uint32, GuideSpawn>> GuideMgr::RepairersNear(uint32 mapId, float x, float y, float radius) const
{
    std::vector<std::pair<uint32, GuideSpawn>> found;
    for (auto const& [entry, spawn] : ServicesNear(mapId, x, y, radius))
        if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
            if (creature->npcflag & UNIT_NPC_FLAG_REPAIR)
                found.push_back({entry, spawn});
    return found;
}

Guide const* GuideMgr::GetGuide(std::string const& name) const
{
    auto guide = _guides.find(name);
    return guide != _guides.end() ? &guide->second : nullptr;
}

Guide const* GuideMgr::GetStartGuide(Player* player) const
{
    auto start = _raceStart.find(player->getRace());
    return start != _raceStart.end() ? GetGuide(start->second) : nullptr;
}

Guide const* GuideMgr::GetNextGuide(Player* player, Guide const& guide) const
{
    // The first listed that is for the character; none: the last listed anyway, as RestedXP does (its Alliance
    // Hunters go on from 21-23 Ashenvale/Stonetalon to the non-Hunter 23-24 Wetlands that way)
    Guide const* last = nullptr;
    for (GuideNext const& candidate : guide.next)
        if (candidate.condition.Holds(player))
            if (Guide const* next = GetGuide(candidate.name))
            {
                if (next->condition.Holds(player))
                    return next;
                last = next;
            }
    return last;
}

std::vector<uint32> const& GuideMgr::GetQuestStarters(uint32 questId) const
{
    auto it = _questStarters.find(questId);
    return it != _questStarters.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetQuestEnders(uint32 questId) const
{
    auto it = _questEnders.find(questId);
    return it != _questEnders.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetQuestStarterObjects(uint32 questId) const
{
    auto it = _questStarterObjects.find(questId);
    return it != _questStarterObjects.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetQuestEnderObjects(uint32 questId) const
{
    auto it = _questEnderObjects.find(questId);
    return it != _questEnderObjects.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetQuestStarterItems(uint32 questId) const
{
    auto it = _questStarterItems.find(questId);
    return it != _questStarterItems.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetQuestTriggers(uint32 questId) const
{
    auto it = _questTriggers.find(questId);
    return it != _questTriggers.end() ? it->second : NONE;
}

std::vector<uint32> const& GuideMgr::GetItemObjects(uint32 item) const
{
    auto it = _itemObjects.find(item);
    return it != _itemObjects.end() ? it->second : NONE;
}

std::vector<GuideSpawn> const& GuideMgr::GetObjectSpawns(uint32 entry) const
{
    auto it = _objectSpawns.find(entry);
    return it != _objectSpawns.end() ? it->second : NO_SPAWNS;
}

std::vector<GuideSpawn> const& GuideMgr::GetSpawns(uint32 entry) const
{
    auto it = _spawns.find(entry);
    return it != _spawns.end() ? it->second : NO_SPAWNS;
}

GuideUse const* GuideMgr::GetUse(uint32 item) const
{
    auto it = _uses.find(item);
    return it != _uses.end() ? &it->second : nullptr;
}
