/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideRules.h"

#include <algorithm>
#include <limits>

#include "DBCStores.h"
#include "GuideInfo.h"
#include "GuideMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ReputationMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Trainer.h"

namespace
{
constexpr uint32 SPELL_KODO_RIDING_CLASSIC = 132245;  // 3.4.3's id, not in the realm's spells

bool InLog(Player* bot, uint32 questId)
{
    QuestStatus status = bot->GetQuestStatus(questId);
    return status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE || status == QUEST_STATUS_FAILED;
}

// RestedXP's ".itemcount": the bags hold the right number (exactly, fewer, more, or else at least that many).
bool ItemCountHolds(Player* bot, GuideElement const& element)
{
    uint32 count = bot->GetItemCount(element.id, element.flags & 1);
    if ((element.flags & 2) && count == element.count)
        return true;
    if (element.flags & 4)
        return count < element.count;
    if (element.flags & 8)
        return count > element.count;
    return !(element.flags & 2) && count >= element.count;
}

// RestedXP's ".reputation": the faction's standing reached, plus points into it (or short of it, or a fraction of it).
bool ReputationReached(Player* bot, GuideElement const& element)
{
    FactionEntry const* faction = sFactionStore.LookupEntry(element.id);
    if (!faction)
        return false;

    ReputationMgr const& reputation = bot->GetReputationMgr();
    int32 earned = reputation.GetReputation(faction);
    uint32 rank = reputation.GetRank(faction);
    int32 bottom = ReputationMgr::Reputation_Bottom;
    for (uint32 i = 0; i < rank; ++i)
        bottom += ReputationMgr::PointsInRank[i];
    int32 top = bottom + ReputationMgr::PointsInRank[rank];
    uint32 standing = rank + 1;  // RestedXP's standing ids: hated 1 ... exalted 8
    uint32 want = element.count;
    float value = element.amount;
    if (value < 0.0f)
        return standing >= want || (standing + 1 == want && earned >= top + value);
    if (standing != want)
        return standing > want;
    if (value >= 1.0f)
        return earned >= bottom + value;
    return earned - bottom >= (top - bottom) * value;
}

// RestedXP's ".aura" (with no text): the aura is on the character, with more than the seconds left or the stacks.
bool HasGuideAura(Player* bot, GuideElement const& element)
{
    Aura const* aura = bot->GetAura(element.id);
    if (!aura || aura->GetStackAmount() < element.count)
        return false;
    return aura->IsPermanent() || aura->GetDuration() > element.amount * IN_MILLISECONDS;
}

// A ".maxlevel" naming a label: past its level, the route skips on to that step (its index; 0: no skip).
// The hearthstone is bound in the area, as RestedXP tells: by the area's name (a subzone's, or the zone's)
bool BoundIn(Player* bot, uint32 areaId)
{
    if (bot->m_homebindAreaId == areaId)
        return true;
    AreaTableEntry const* wanted = sAreaTableStore.LookupEntry(areaId);
    AreaTableEntry const* bound = sAreaTableStore.LookupEntry(bot->m_homebindAreaId);
    return wanted && bound && std::string(wanted->area_name[0]) == bound->area_name[0];
}

uint32 SkipTo(Player* bot, GuideStep const& step)
{
    if (!step.condition.Holds(bot))
        return 0;
    for (GuideElement const& element : step.elements)
        if (element.type == GuideElementType::MaxLevel && element.count && GuideRules::Applies(bot, element) &&
            bot->GetLevel() > element.id)
            return element.count;
    return 0;
}

// Guide levels from the name: "6-12 Mulgore" starts at 6 and ends at 12 (0: none named)
uint32 GuideEndLevel(Guide const& guide)
{
    size_t dash = guide.name.find('-');
    uint32 level = 0;
    for (size_t i = dash == std::string::npos ? guide.name.size() : dash + 1; i < guide.name.size(); ++i)
    {
        char c = guide.name[i];
        if (c < '0' || c > '9')
            break;
        level = level * 10 + uint32(c - '0');
    }
    return level;
}

// Quest work the character can do now: a quest it can take, one in its log to finish or hand in, a level to reach.
// Work it can't (a quest it skipped and can no longer take, one not in the log to finish) doesn't hold it there.
bool DoableWork(Player* bot, GuideElement const& element, std::string& why)
{
    Quest const* quest =
        sObjectMgr->GetQuestTemplate(element.type == GuideElementType::Collect ? element.quest : element.id);
    std::string name = quest ? quest->GetTitle() : std::to_string(element.id);
    switch (element.type)
    {
        case GuideElementType::Accept:
            why = "take " + name;
            return quest && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false);
        case GuideElementType::TurnIn:
            why = "hand in " + name;
            return InLog(bot, element.id);
        case GuideElementType::Complete:
            why = Acore::StringFormat("{} (objective {}, in the log)", name, element.objective);
            return InLog(bot, element.id);
        case GuideElementType::Collect:
            why = Acore::StringFormat("collect for {}", name);
            return element.quest && (InLog(bot, element.quest) ||
                                     (quest && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false)));
        case GuideElementType::Xp:
            why = Acore::StringFormat("reach level {}", element.id);
            return true;
        default:
            return false;
    }
}

std::string QuestName(uint32 questId)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    return quest ? quest->GetTitle() : std::to_string(questId);
}
}  // namespace

bool GuideRules::Applies(Player* bot, GuideElement const& element) { return element.condition.Holds(bot); }

bool GuideRules::StepApplies(Player* bot, GuideStep const& step)
{
    if (!step.condition.Holds(bot))
        return false;

    uint8 level = bot->GetLevel();
    for (GuideElement const& element : step.elements)
    {
        if (!Applies(bot, element))
            continue;

        uint32 id = element.id;
        switch (element.type)
        {
            case GuideElementType::Below:
                if (level >= id)
                    return false;
                break;
            case GuideElementType::AtLeast:
                if (level < id)
                    return false;
                break;
            case GuideElementType::OnQuest:
                if (!InLog(bot, id))
                    return false;
                break;
            case GuideElementType::NotOnQuest:
                if (InLog(bot, id))
                    return false;
                break;
            case GuideElementType::TurnedIn:
                if (!bot->GetQuestRewardStatus(id))
                    return false;
                break;
            case GuideElementType::QuestComplete:
                if (bot->GetQuestStatus(id) != QUEST_STATUS_COMPLETE)
                    return false;
                break;
            case GuideElementType::Available:
                if (InLog(bot, id) || bot->GetQuestRewardStatus(id))
                    return false;
                break;
            // RestedXP's skip conditions (guide_data.py's flags)
            case GuideElementType::ZoneSkip:
            {
                bool in = std::find(element.entries.begin(), element.entries.end(), bot->GetZoneId()) !=
                              element.entries.end() ||
                          std::find(element.entries.begin(), element.entries.end(), bot->GetAreaId()) !=
                              element.entries.end();
                if ((element.flags & 2) && !bot->GetMap()->Instanceable())
                    in = false;
                if (in != bool(element.flags & 1))
                    return false;
                break;
            }
            case GuideElementType::ItemCount:
                if (!ItemCountHolds(bot, element))
                    return false;
                break;
            case GuideElementType::Money:
                if ((bot->GetMoney() >= element.count) == bool(element.flags & 1))
                    return false;
                break;
            case GuideElementType::Skill:
            {
                int32 points = -1;
                if (bot->HasSkill(id))
                    points = (element.flags & 2) ? bot->GetMaxSkillValue(id) : bot->GetSkillValue(id);
                if ((points >= static_cast<int32>(element.count)) != bool(element.flags & 1))
                    return false;
                break;
            }
            case GuideElementType::MaxLevel:
                if (level > id)
                    return false;
                break;
            case GuideElementType::Aura:
                if (HasGuideAura(bot, element) != bool(element.flags & 1))
                    return false;
                break;
            case GuideElementType::Reputation:
                if (ReputationReached(bot, element) == bool(element.flags & 1))
                    return false;
                break;
            case GuideElementType::Group:
                return false;
            case GuideElementType::BindLocation:
                if (BoundIn(bot, id) != bool(element.flags & 1))
                    return false;
                break;
            case GuideElementType::Equipped:
            {
                Item* worn = element.count < EQUIPMENT_SLOT_END
                                 ? bot->GetItemByPos(INVENTORY_SLOT_BAG_0, static_cast<uint8>(element.count))
                                 : nullptr;
                if ((worn && (!id || worn->GetEntry() == id)) != bool(element.flags & 1))
                    return false;
                break;
            }
            default:
                break;
        }
    }
    return true;
}

bool GuideRules::ElementDone(Player* bot, GuideElement const& element)
{
    uint32 questId = element.id;
    switch (element.type)
    {
        case GuideElementType::Accept:
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || InLog(bot, questId) || bot->GetQuestRewardStatus(questId))
                return true;
            // A quest this character can't take (class, race, an earlier quest skipped) is passed by; one it is
            // only too low for waits.
            return !bot->CanTakeQuest(quest, false) && bot->SatisfyQuestLevel(quest, false);
        }
        case GuideElementType::TurnIn:
            // Never taken (joined the chain late, or skipped): passed by, as RestedXP's ported guides do.
            return bot->GetQuestRewardStatus(questId) || !InLog(bot, questId);
        case GuideElementType::Complete:
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest || !InLog(bot, questId) || bot->GetQuestStatus(questId) == QUEST_STATUS_COMPLETE)
                return true;
            std::vector<GuideObjective> objectives = Objectives(quest);
            if (!element.objective || element.objective > objectives.size())
                return false;
            return ObjectiveDone(bot, quest, objectives[element.objective - 1]);
        }
        case GuideElementType::Xp:
        {
            // RestedXP's rule: level N, plus xp into it (or short of it, or a fraction of it)
            uint32 level = bot->GetLevel();
            uint32 xp = bot->GetUInt32Value(PLAYER_XP);
            uint32 next = bot->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
            float want = element.xp;
            if (want < 0.0f)
                return level >= element.id || (level + 1 == element.id && xp >= next + want);
            if (want >= 1.0f)
                return level > element.id || (level == element.id && xp >= want);
            return level > element.id || (level == element.id && xp >= next * want);
        }
        case GuideElementType::Train:
        {
            // As the realm teaches it (riding: Apprentice Riding); no trainer here teaches it: nothing to do
            if (bot->HasSpell(element.id) || GuideMgr::instance().IsUntrainable(element.id))
                return true;
            std::vector<uint32> const& as = GuideMgr::instance().GetTrainedAs(element.id);
            return std::any_of(as.begin(), as.end(), [&](uint32 spell) { return bot->HasSpell(spell); });
        }
        case GuideElementType::Abandon:
            return !InLog(bot, questId);
        case GuideElementType::Collect:
            if (element.quest && (bot->GetQuestRewardStatus(element.quest) ||
                                  bot->GetQuestStatus(element.quest) == QUEST_STATUS_COMPLETE))
                return true;
            return bot->GetItemCount(element.id, true) >= element.count;
        default:
            return true;
    }
}

bool GuideRules::QuestWorkDone(Player* bot, GuideStep const& step, uint32 skipped)
{
    for (uint32 i = 0; i < step.elements.size(); ++i)
    {
        GuideElement const& element = step.elements[i];
        if (i < 32 && (skipped & (1u << i)))
            continue;
        if (element.IsQuestWork() && Applies(bot, element) && !ElementDone(bot, element))
            return false;
    }
    return true;
}

std::set<uint32> GuideRules::FinishedKills(Player* bot, GuideStep const& step)
{
    std::set<uint32> finished;
    std::set<uint32> open;
    bool lastDone = false;
    bool afterObjective = false;
    for (GuideElement const& element : step.elements)
    {
        if (!Applies(bot, element))
            continue;
        if (element.type == GuideElementType::Complete || element.type == GuideElementType::Collect)
        {
            // Only a kill objective claims the ".mob" entries after it (an item's list may come before it); an
            // unfinished item keeps its droppers
            bool done = ElementDone(bot, element);
            afterObjective = false;
            uint32 item = element.type == GuideElementType::Collect ? element.id : 0;
            Quest const* quest =
                element.type == GuideElementType::Complete ? sObjectMgr->GetQuestTemplate(element.id) : nullptr;
            std::vector<GuideObjective> objectives = quest ? Objectives(quest) : std::vector<GuideObjective>();
            if (quest && element.objective && element.objective <= objectives.size())
            {
                GuideObjective const& objective = objectives[element.objective - 1];
                if (objective.kind == GuideObjective::Kind::CreatureOrObject &&
                    quest->RequiredNpcOrGo[objective.slot] > 0)
                {
                    (done ? finished : open).insert(static_cast<uint32>(quest->RequiredNpcOrGo[objective.slot]));
                    lastDone = done;
                    afterObjective = true;
                }
                else if (objective.kind == GuideObjective::Kind::Item)
                    item = quest->RequiredItemId[objective.slot];
            }
            if (item && !done)
            {
                std::vector<uint32> const& droppers = GuideMgr::instance().GetItemDroppers(item);
                open.insert(droppers.begin(), droppers.end());
            }
        }
        else if (element.type == GuideElementType::Mob && afterObjective)
            (lastDone ? finished : open).insert(element.entries.begin(), element.entries.end());
    }
    for (uint32 entry : open)
        finished.erase(entry);
    return finished;
}

bool GuideRules::HasQuestWork(Player* bot, GuideStep const& step)
{
    for (GuideElement const& element : step.elements)
        if (element.IsQuestWork() && Applies(bot, element))
            return true;
    return false;
}

bool GuideRules::HasServices(Player* bot, GuideStep const& step)
{
    for (GuideElement const& element : step.elements)
        if (element.IsService() && Applies(bot, element))
            return true;
    return false;
}

bool GuideRules::Reached(Player* bot, GuideStep const& step)
{
    for (GuideElement const& element : step.elements)
    {
        if (!Applies(bot, element))
            continue;
        // Proof the character got this far: a quest taken, an objective done, a quest handed in. A quest only in
        // the log says nothing about its objective or turn-in steps.
        bool rewarded = bot->GetQuestRewardStatus(element.id);
        switch (element.type)
        {
            case GuideElementType::Accept:
                if (rewarded || InLog(bot, element.id))
                    return true;
                break;
            case GuideElementType::TurnIn:
                if (rewarded)
                    return true;
                break;
            case GuideElementType::Complete:
                if (rewarded || (InLog(bot, element.id) && ElementDone(bot, element)))
                    return true;
                break;
            case GuideElementType::Collect:
                if ((element.quest && bot->GetQuestRewardStatus(element.quest)) ||
                    bot->GetItemCount(element.id, true) >= element.count)
                    return true;
                break;
            default:
                break;
        }
    }
    return false;
}

std::vector<GuideObjective> GuideRules::Objectives(Quest const* quest)
{
    using Kind = GuideObjective::Kind;
    std::vector<GuideObjective> objectives;
    if (quest->GetRepObjectiveFaction() && quest->GetRepObjectiveValue())
        objectives.push_back({Kind::Reputation, 0});
    if (quest->GetRepObjectiveFaction2() && quest->GetRepObjectiveValue2())
        objectives.push_back({Kind::Reputation, 1});
    if (quest->GetRewOrReqMoney() < 0)
        objectives.push_back({Kind::Money, 0});
    if (quest->GetPlayersSlain())
        objectives.push_back({Kind::PlayerKills, 0});
    for (uint32 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredNpcOrGo[i])
            objectives.push_back({Kind::CreatureOrObject, i});
    for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
        if (quest->RequiredItemId[i])
            objectives.push_back({Kind::Item, i});
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
        objectives.push_back({Kind::Exploration, 0});
    return objectives;
}

bool GuideRules::ObjectiveDone(Player* bot, Quest const* quest, GuideObjective const& objective)
{
    using Kind = GuideObjective::Kind;
    auto status = bot->getQuestStatusMap().find(quest->GetQuestId());
    if (status == bot->getQuestStatusMap().end())
        return true;

    QuestStatusData const& data = status->second;
    switch (objective.kind)
    {
        case Kind::Reputation:
        {
            uint32 faction = objective.slot ? quest->GetRepObjectiveFaction2() : quest->GetRepObjectiveFaction();
            int32 value = objective.slot ? quest->GetRepObjectiveValue2() : quest->GetRepObjectiveValue();
            return bot->GetReputationMgr().GetReputation(faction) >= value;
        }
        case Kind::Money:
            return bot->GetMoney() >= static_cast<uint32>(-quest->GetRewOrReqMoney());
        case Kind::PlayerKills:
            return data.PlayerCount >= quest->GetPlayersSlain();
        case Kind::CreatureOrObject:
            return data.CreatureOrGOCount[objective.slot] >= quest->RequiredNpcOrGoCount[objective.slot];
        case Kind::Item:
            return bot->GetItemCount(quest->RequiredItemId[objective.slot]) >= quest->RequiredItemCount[objective.slot];
        case Kind::Exploration:
            return data.Explored;
    }
    return true;
}

bool GuideRules::Serves(Player* bot, GuideElement const& element, uint32 creatureEntry)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(creatureEntry);
    if (!creature)
        return false;
    switch (element.type)
    {
        case GuideElementType::Vendor:
            return creature->npcflag & UNIT_NPC_FLAG_VENDOR;
        case GuideElementType::Home:
            return creature->npcflag & UNIT_NPC_FLAG_INNKEEPER;
        case GuideElementType::FlightPath:
        case GuideElementType::Fly:
            return creature->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER;
        case GuideElementType::Train:
        case GuideElementType::Trainer:
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creatureEntry);
            return (creature->npcflag & UNIT_NPC_FLAG_TRAINER) && trainer && trainer->IsTrainerValidForPlayer(bot) &&
                   (element.type != GuideElementType::Train || TrainerSpellFor(trainer, element.id));
        }
        default:
            return false;
    }
}

bool GuideRules::AtAuctionHouse(Player* bot, GuideStep const& step)
{
    for (GuideElement const& element : step.elements)
        if (element.type == GuideElementType::Target && Applies(bot, element))
            for (uint32 entry : element.entries)
                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
                    if (creature->npcflag & UNIT_NPC_FLAG_AUCTIONEER)
                        return true;
    return false;
}

uint32 GuideRules::TrainerSpellFor(Trainer::Trainer const* trainer, uint32 wanted)
{
    // RestedXP's spell, or the one it teaches (1.12's teaching spell for Desperate Prayer: the trainer lists the spell)
    std::vector<uint32> wantedAs = {wanted};
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(wanted))
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                wantedAs.push_back(effect.TriggerSpell);
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (std::find(wantedAs.begin(), wantedAs.end(), spell.SpellId) != wantedAs.end())
            return spell.SpellId;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell == wanted)
                    return spell.SpellId;
    }

    // RestedXP names 1.12's racial riding ("Tiger Riding", "Undead Horsemanship"; 3.4.3's Kodo Riding has an id of its
    // own): the realm teaches the core's riding skill instead (Apprentice Riding), the trainer's first riding spell.
    // Else the trainer's spell of the same name (one the realm lists by another id: Desperate Prayer).
    SpellInfo const* want = sSpellMgr->GetSpellInfo(wanted);
    std::string name = want && want->SpellName[0] ? want->SpellName[0] : "";
    auto endsWith = [&](std::string const& end)
    { return name.size() > end.size() && name.compare(name.size() - end.size(), end.size(), end) == 0; };
    bool riding = wanted == SPELL_KODO_RIDING_CLASSIC || endsWith(" Riding") || endsWith(" Piloting") ||
                  endsWith(" Horsemanship");
    uint32 ridingSpell = 0;
    uint32 ridingLevel = std::numeric_limits<uint32>::max();
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (riding && spell.ReqSkillLine == SKILL_RIDING && spell.ReqLevel < ridingLevel)
        {
            ridingSpell = spell.SpellId;
            ridingLevel = spell.ReqLevel;
        }
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId);
        if (riding || name.empty() || !info)
            continue;
        // The same spell, not a namesake: its name, rank and class (a pet's Claw isn't a druid's)
        auto same = [&](SpellInfo const* other)
        {
            return other->SpellName[0] && name == other->SpellName[0] &&
                   other->SpellFamilyName == want->SpellFamilyName &&
                   std::string(other->Rank[0] ? other->Rank[0] : "") == (want->Rank[0] ? want->Rank[0] : "");
        };
        if (same(info))
            return spell.SpellId;
        for (SpellEffectInfo const& effect : info->Effects)
            if (effect.Effect == SPELL_EFFECT_LEARN_SPELL)
                if (SpellInfo const* taught = sSpellMgr->GetSpellInfo(effect.TriggerSpell))
                    if (same(taught))
                        return spell.SpellId;
    }
    return ridingSpell;
}

bool GuideRules::StillNeeded(Player* bot, GuideInfo const& info, uint32 questId)
{
    // A later step of the route (this guide from the current step, and the next few) works on the quest or hands it in
    Guide const* guide = info.guide;
    uint32 step = info.step;
    for (uint32 guides = 0; guide && guides < STILL_NEEDED_GUIDES; ++guides)
    {
        for (; step < guide->steps.size(); ++step)
        {
            GuideStep const& next = guide->steps[step];
            if (!next.condition.Holds(bot))
                continue;
            for (GuideElement const& element : next.elements)
            {
                bool works =
                    ((element.type == GuideElementType::Complete || element.type == GuideElementType::TurnIn) &&
                     element.id == questId) ||
                    (element.type == GuideElementType::Collect && element.quest == questId);
                if (works && Applies(bot, element))
                    return true;
            }
        }
        guide = GuideMgr::instance().GetNextGuide(bot, *guide);
        step = 0;
    }
    return false;
}

std::unordered_map<uint32, uint32> const& GuideRules::NeededItems(Player* bot, GuideInfo& info)
{
    if (info.neededGuide == info.guide && info.neededStep == info.step && info.guide)
        return info.neededItems;
    info.neededGuide = info.guide;
    info.neededStep = info.step;
    info.neededItems.clear();
    auto need = [&](uint32 item, uint32 count)
    {
        if (item)
            info.neededItems[item] = std::max(info.neededItems[item], std::max(count, 1u));
    };
    Guide const* guide = info.guide;
    uint32 step = info.step;
    for (uint32 guides = 0; guide && guides < STILL_NEEDED_GUIDES; ++guides)
    {
        for (; step < guide->steps.size(); ++step)
        {
            GuideStep const& next = guide->steps[step];
            if (!next.condition.Holds(bot))
                continue;
            for (GuideElement const& element : next.elements)
            {
                if (!Applies(bot, element))
                    continue;
                switch (element.type)
                {
                    case GuideElementType::Collect:
                    case GuideElementType::ItemCount:
                    case GuideElementType::Use:
                        need(element.id, element.count);
                        break;
                    case GuideElementType::Accept:
                    case GuideElementType::Complete:
                    case GuideElementType::TurnIn:
                        if (Quest const* quest = sObjectMgr->GetQuestTemplate(element.id);
                            quest && !bot->GetQuestRewardStatus(element.id))
                        {
                            for (uint32 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                                need(quest->RequiredItemId[i], quest->RequiredItemCount[i]);
                            for (uint32 i = 0; i < QUEST_SOURCE_ITEM_IDS_COUNT; ++i)
                                need(quest->ItemDrop[i], quest->ItemDropQuantity[i]);
                            need(quest->GetSrcItemId(), quest->GetSrcItemCount());
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        guide = GuideMgr::instance().GetNextGuide(bot, *guide);
        step = 0;
    }
    return info.neededItems;
}

uint32 GuideRules::GuideStartLevel(Guide const& guide)
{
    uint32 level = 0;
    for (char c : guide.name)
    {
        if (c < '0' || c > '9')
            break;
        level = level * 10 + uint32(c - '0');
    }
    return level;
}

bool GuideRules::Resume(Player* bot, GuideInfo& info, Guide const* from, std::string* reached)
{
    // Walk the character's route for the first step with quest work it can do now (DoableWork): a quest to take,
    // finish or hand in. Steps without quest work (travel, training, selling) are passed by: nothing shows whether
    // they were done, and every start would walk back to them. A guide ending below the character's level is behind
    // it: its quests to take are passed by, but those in the log still hold it there (finished ones left unhanded
    // otherwise); one starting more than RESUME_LEVELS_AHEAD above it ends the walk. A guide named (from) is walked
    // whatever its levels.
    uint32 level = bot->GetLevel();
    uint32 guides = 0;
    if (reached)
        reached->clear();
    for (Guide const* guide = from ? from : GuideMgr::instance().GetStartGuide(bot); guide && guides < 500;
         guide = GuideMgr::instance().GetNextGuide(bot, *guide), ++guides)
    {
        if (guide != from && GuideStartLevel(*guide) > level + RESUME_LEVELS_AHEAD)
            break;
        uint32 endLevel = GuideEndLevel(*guide);
        bool behind = guide != from && endLevel && endLevel < level;
        for (uint32 i = 0; i < guide->steps.size(); ++i)
        {
            GuideStep const& step = guide->steps[i];
            if (uint32 to = behind ? 0 : SkipTo(bot, step))
            {
                i = to - 1;
                continue;
            }
            // Side steps are never the current step (they run alongside later ones).
            if (step.side || !(behind ? step.condition.Holds(bot) : StepApplies(bot, step)))
                continue;
            for (GuideElement const& element : step.elements)
            {
                std::string why;
                bool toTake = element.type == GuideElementType::Accept ||
                              (element.type == GuideElementType::Collect && !InLog(bot, element.quest));
                if (!element.IsQuestWork() || (behind && toTake) || !Applies(bot, element) ||
                    ElementDone(bot, element) || !DoableWork(bot, element, why))
                    continue;
                info.Reset();
                info.guide = guide;
                info.step = i;
                if (reached)
                    *reached = why;
                return true;
            }
        }
    }

    info.Reset();
    // A guide named with nothing left to do in it or after: from its first step
    if (from)
    {
        info.guide = from;
        return true;
    }
    return false;
}

GuideElement const* GuideRules::CatchUpTurnIn(Player* bot, Guide const& guide, GuideStep const& step)
{
    if (!step.condition.Holds(bot))
        return nullptr;
    for (GuideElement const& element : step.elements)
    {
        if (element.type != GuideElementType::TurnIn || !Applies(bot, element) ||
            bot->GetQuestStatus(element.id) != QUEST_STATUS_COMPLETE)
            continue;
        GuideInfo later;
        later.guide = GuideMgr::instance().GetNextGuide(bot, guide);
        if (later.guide && StillNeeded(bot, later, element.id))
            continue;
        return &element;
    }
    return nullptr;
}

namespace
{
// Makes step the current one, its per-step state fresh
void EnterStep(GuideInfo& info, uint32 step)
{
    info.step = step;
    info.area = info.reached = info.serviced = info.stuckSince = 0;
    info.areaAnchor = WorldPosition();
    info.detour = WorldPosition();
    info.serviceSearched.clear();
    info.repairSearched.clear();
    info.vendorSearched.clear();
    info.vendorChase.Clear();
    info.skippedWork = 0;
    info.retake.clear();
    info.toldUnfinished = false;
}

// A side step done as a step of its own, then on from (or back to, sideBack) step from
void EnterSide(GuideInfo& info, uint32 side, uint32 from)
{
    info.sidesFinished.insert(side);
    EnterStep(info, side);
    info.finishSide = true;
    info.sideReturn = from;
}
}  // namespace

bool GuideRules::Advance(Player* bot, GuideInfo& info)
{
    Guide const* guide = info.guide;
    // Leaving the step a side step stays active through with its quest work unfinished and still wanted further on
    // (the bandanas weren't all in when Princess died): the side step first, as a step of its own; then on from here
    if (guide && !info.catchUp)
    {
        uint32 const from = info.finishSide ? info.sideReturn : info.step;
        for (uint32 i = 0; i < guide->steps.size(); ++i)
        {
            GuideStep const& side = guide->steps[i];
            if (!side.side || side.sideUntil != from || info.sidesFinished.count(i) || !StepApplies(bot, side) ||
                !HasQuestWork(bot, side) || QuestWorkDone(bot, side))
                continue;
            GuideInfo later;
            later.guide = guide;
            later.step = from + 1;
            bool wanted = false;
            for (GuideElement const& element : side.elements)
                if ((element.type == GuideElementType::Complete || element.type == GuideElementType::Collect) &&
                    Applies(bot, element) && !ElementDone(bot, element))
                {
                    uint32 questId = element.type == GuideElementType::Complete ? element.id : element.quest;
                    wanted |= questId && StillNeeded(bot, later, questId);
                }
            if (!wanted && !info.sideBack)
                continue;
            LOG_INFO("playerbots.guide", "{}: {} side step {} unfinished as step {} is done; finishing it first",
                     bot->GetName(), guide->name, side.number, guide->steps[from].number);
            EnterSide(info, i, from);
            return true;
        }
        if (info.finishSide && info.sideBack)
        {
            // Fetched what the step's use needs: back to that step
            EnterStep(info, info.sideReturn);
            info.finishSide = info.sideBack = false;
            return true;
        }
        if (info.finishSide)
            info.step = info.sideReturn;
        info.finishSide = false;
    }
    uint32 step = info.step + 1;
    if (uint32 to = guide && !info.catchUp ? SkipTo(bot, guide->steps[info.step]) : 0)
        step = to;
    auto enter = [&](uint32 at)
    {
        if (info.guide != guide)
        {
            info.sideDone.clear();
            LOG_INFO("playerbots.guide", "{}: on to guide {}", bot->GetName(), guide->name);
        }
        info.guide = guide;
        EnterStep(info, at);
    };
    uint32 guides = 0;
    while (guide && guides < 500)
    {
        if (info.catchUp)
        {
            for (; step < guide->steps.size(); ++step)
                if (CatchUpTurnIn(bot, *guide, guide->steps[step]))
                {
                    enter(step);
                    return true;
                }
            info.catchUp = false;
            LOG_INFO("playerbots.guide", "{}: caught up on {}", bot->GetName(), guide->name);
        }
        else
        {
            for (; step < guide->steps.size(); ++step)
            {
                if (uint32 to = SkipTo(bot, guide->steps[step]))
                {
                    LOG_INFO("playerbots.guide", "{}: {} step {} skips on to step {} (past its level)", bot->GetName(),
                             guide->name, guide->steps[step].number,
                             to < guide->steps.size() ? guide->steps[to].number : 0);
                    step = to - 1;
                    continue;
                }
                if (!guide->steps[step].side && StepApplies(bot, guide->steps[step]))
                {
                    info.sidesFinished.clear();
                    enter(step);
                    return true;
                }
            }
            // Leaving the guide: its quests finished in the log are handed in first (steps passed by, or done
            // since), unless a step further on hands them in
            for (uint32 i = 0; i < guide->steps.size(); ++i)
                if (GuideElement const* turnIn = CatchUpTurnIn(bot, *guide, guide->steps[i]))
                {
                    LOG_INFO("playerbots.guide", "{}: catching up on {} before leaving it: {} (step {}) first",
                             bot->GetName(), guide->name, QuestName(turnIn->id), guide->steps[i].number);
                    enter(i);
                    info.catchUp = true;
                    return true;
                }
        }
        guide = GuideMgr::instance().GetNextGuide(bot, *guide);
        step = 0;
        ++guides;
    }
    return false;
}

bool GuideRules::FetchReagents(Player* bot, GuideInfo& info)
{
    // The step's ".use" lacks its spell's reagents (the Filled Cleansing Bowl: the Blackwood fruit, grain and nut
    // samples), which side steps active through it collect, but only when a holder comes close: those side steps
    // first, as steps of their own; then back to this one
    Guide const* guide = info.guide;
    if (!guide || info.catchUp || info.finishSide || info.step >= guide->steps.size())
        return false;
    std::set<uint32> missing;
    for (GuideElement const& element : guide->steps[info.step].elements)
    {
        if (element.type != GuideElementType::Use || !Applies(bot, element) || !bot->HasItemCount(element.id))
            continue;
        GuideUse const* use = GuideMgr::instance().GetUse(element.id);
        SpellInfo const* spell = use && use->spell ? sSpellMgr->GetSpellInfo(use->spell) : nullptr;
        if (!spell)
            continue;
        for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (spell->Reagent[i] > 0 && !bot->HasItemCount(uint32(spell->Reagent[i]), spell->ReagentCount[i]))
                missing.insert(uint32(spell->Reagent[i]));
    }
    if (missing.empty())
        return false;
    for (uint32 i = 0; i < info.step; ++i)
    {
        GuideStep const& side = guide->steps[i];
        if (!side.side || side.sideUntil < info.step || info.sidesFinished.count(i) || !StepApplies(bot, side))
            continue;
        for (GuideElement const& element : side.elements)
            if (element.type == GuideElementType::Collect && missing.count(element.id) && Applies(bot, element) &&
                !ElementDone(bot, element))
            {
                LOG_INFO("playerbots.guide", "{}: {} step {} needs what side step {} collects; that first",
                         bot->GetName(), guide->name, guide->steps[info.step].number, side.number);
                EnterSide(info, i, info.step);
                info.sideBack = true;
                return true;
            }
    }
    return false;
}

std::string GuideRules::Describe(Player* bot, GuideInfo const& info)
{
    GuideTrip const& trip = info.trip;
    if (trip.going)
    {
        std::string what = "walking";
        if (trip.leg < trip.legs.size())
        {
            GuideLeg const& leg = trip.legs[trip.leg];
            if (leg.kind == GuideLeg::Kind::Portal)
                what = "going through the portal";
            else if (leg.kind == GuideLeg::Kind::Hearth)
                what = leg.entry ? "teleporting" : "using the hearthstone";
            else if (leg.kind == GuideLeg::Kind::Fly)
                what = bot->IsInFlight() ? "flying" : "going to the flight master";
            else if (leg.kind == GuideLeg::Kind::Transport)
            {
                GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(leg.entry);
                std::string name = go ? go->name : "the transport";
                what = (trip.enteredAt ? "riding " : "waiting for ") + name;
            }
        }
        if (trip.leg >= trip.legs.size())
            return Acore::StringFormat("going to {}: {}", trip.name, what);
        return Acore::StringFormat("going to {}, part {} of {}: {}", trip.name, trip.leg + 1, trip.legs.size(), what);
    }
    if (!info.guide)
        return "no guide";

    GuideTask const& task = info.task;
    std::string what;
    switch (task.type)
    {
        case GuideTaskType::Travel:
            what = "travelling";
            break;
        case GuideTaskType::Accept:
            what = "accepting " + QuestName(task.questId);
            break;
        case GuideTaskType::TurnIn:
            what = "turning in " + QuestName(task.questId);
            break;
        case GuideTaskType::Hearth:
            what = "using the hearthstone";
            break;
        case GuideTaskType::Use:
            if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(task.item))
                what = "using " + proto->Name1;
            else
                what = "using an item";
            break;
        case GuideTaskType::Loot:
            what = "looting";
            break;
        case GuideTaskType::Avoid:
            what = "stepping out of a patrol's way";
            break;
        case GuideTaskType::Rest:
            what = "resting";
            break;
        case GuideTaskType::Service:
            what = task.repair   ? "repairing"
                   : task.vendor ? "selling junk on the way"
                                 : "visiting an NPC (training, selling, talking)";
            break;
        case GuideTaskType::Kill:
            what = task.questId ? Acore::StringFormat("{} (objective {})", QuestName(task.questId), task.objective)
                                : std::string("collecting items");
            break;
        case GuideTaskType::Level:
            what = Acore::StringFormat("grinding to level {}", task.questId);
            break;
        case GuideTaskType::Finished:
            what = "route finished";
            break;
        default:
            what = "waiting";
            break;
    }
    return Acore::StringFormat("{}, step {}: {}{}", info.guide->name, info.guide->steps[info.step].number, what,
                               bot->IsInCombat() ? " (in combat)" : "");
}
