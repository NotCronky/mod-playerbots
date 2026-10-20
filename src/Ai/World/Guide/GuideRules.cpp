/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideRules.h"

#include <algorithm>

#include "DBCStores.h"
#include "GuideInfo.h"
#include "GuideMgr.h"
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
            return bot->HasSpell(element.id);
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
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (spell.SpellId == wanted)
            return spell.SpellId;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
            for (SpellEffectInfo const& effect : info->Effects)
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell == wanted)
                    return spell.SpellId;
    }
    return 0;
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

bool GuideRules::Resume(Player* bot, GuideInfo& info)
{
    // Walk the character's route: the first step with quest work left after the last step it certainly reached.
    // Steps without quest work (travel, training, selling) are passed by: nothing shows whether they were done, and
    // every start would walk back to them.
    Guide const* foundGuide = nullptr;
    uint32 foundStep = 0;
    uint32 guides = 0;
    for (Guide const* guide = GuideMgr::instance().GetStartGuide(bot); guide && guides < 500;
         guide = GuideMgr::instance().GetNextGuide(bot, *guide), ++guides)
    {
        for (uint32 i = 0; i < guide->steps.size(); ++i)
        {
            GuideStep const& step = guide->steps[i];
            if (uint32 to = SkipTo(bot, step))
            {
                i = to - 1;
                continue;
            }
            if (!StepApplies(bot, step))
                continue;
            if (Reached(bot, step))
                foundGuide = nullptr;
            // Side steps are never the current step (they run alongside later ones).
            bool done = step.side || !HasQuestWork(bot, step) || QuestWorkDone(bot, step);
            if (!done && !foundGuide)
            {
                foundGuide = guide;
                foundStep = i;
            }
        }
    }

    info.Reset();
    if (!foundGuide)
        return false;
    info.guide = foundGuide;
    info.step = foundStep;
    return true;
}

bool GuideRules::Advance(Player* bot, GuideInfo& info)
{
    Guide const* guide = info.guide;
    uint32 step = info.step + 1;
    if (uint32 to = guide ? SkipTo(bot, guide->steps[info.step]) : 0)
        step = to;
    uint32 guides = 0;
    while (guide && guides < 500)
    {
        for (; step < guide->steps.size(); ++step)
        {
            if (uint32 to = SkipTo(bot, guide->steps[step]))
            {
                step = to - 1;
                continue;
            }
            if (!guide->steps[step].side && StepApplies(bot, guide->steps[step]))
            {
                if (info.guide != guide)
                    info.sideDone.clear();
                info.guide = guide;
                info.step = step;
                info.area = info.reached = info.serviced = info.stuckSince = 0;
                info.detour = WorldPosition();
                info.serviceSearched.clear();
                info.skippedWork = 0;
                info.retake.clear();
                info.toldUnfinished = false;
                return true;
            }
        }
        guide = GuideMgr::instance().GetNextGuide(bot, *guide);
        step = 0;
        ++guides;
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
                what = "using the hearthstone";
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
            what = "training / selling";
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
