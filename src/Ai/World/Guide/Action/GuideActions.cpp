/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideActions.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <unordered_map>

#include "Bag.h"
#include "Corpse.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameGraveyard.h"
#include "GameObject.h"
#include "GossipDef.h"
#include "GuideAudit.h"
#include "GuideMgr.h"
#include "GuideProgress.h"
#include "GuideRules.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "LootAction.h"
#include "LootMgr.h"
#include "LootObjectStack.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "Playerbots.h"
#include "PullSafety.h"
#include "QuestPackets.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"
#include "Trainer.h"
#include "Transport.h"

namespace
{
constexpr uint32 ITEM_HEARTHSTONE = 6948;
constexpr uint32 SPELL_HEARTHSTONE = 8690;
constexpr float HOME_REACHED = 100.0f;  // yards from the bind point: the hearthstone took it home
// Off a ride that ends in the water: dry ground this near the stop, in rings ASHORE_STEP apart with ASHORE_ANGLES
// spots each, searched from ASHORE_ABOVE over the stop's height down; of the first ASHORE_PATHS dry spots, the first
// with a whole path on, else the nearest
constexpr float ASHORE_RANGE = 40.0f;
constexpr float ASHORE_STEP = 4.0f;
constexpr uint32 ASHORE_ANGLES = 16;
constexpr float ASHORE_ABOVE = 10.0f;
constexpr uint32 ASHORE_PATHS = 12;

bool NearHome(Player* bot)
{
    return bot->GetMapId() == bot->m_homebindMapId &&
           bot->GetExactDist2d(bot->m_homebindX, bot->m_homebindY) <= HOME_REACHED;
}

// The flight point named by a ".fly" or ".fp" place (TaxiNodes names read "Stormwind, Elwynn"), one this character's
// faction flies from.
TaxiNodesEntry const* FindTaxiNode(std::string const& place, TeamId team)
{
    std::string want = place;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);
    for (uint32 i = 1; i < sTaxiNodesStore.GetNumRows(); ++i)
    {
        TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(i);
        if (!node || !node->name[0] || !node->MountCreatureID[team == TEAM_ALLIANCE ? 1 : 0])
            continue;
        std::string name = node->name[0];
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        if (name.find(want) != std::string::npos)
            return node;
    }
    return nullptr;
}

// Uses an item the way the client does: CMSG_USE_ITEM aimed at a creature, an object, a spot, or nothing.
bool CastItem(Player* bot, Item* item, uint32 spellId, Unit* unit, GameObject* object, WorldPosition const* dest)
{
    if (bot->IsNonMeleeSpellCast(false))
        return false;
    if (bot->isMoving())
        bot->StopMoving();

    WorldPacket packet(CMSG_USE_ITEM);
    packet << item->GetBagSlot() << item->GetSlot() << uint8(1) << spellId << item->GetGUID() << uint32(0) << uint8(0);
    if (unit)
        packet << uint32(TARGET_FLAG_UNIT) << unit->GetGUID().WriteAsPacked();
    else if (object)
        packet << uint32(TARGET_FLAG_GAMEOBJECT) << object->GetGUID().WriteAsPacked();
    else if (dest)
        packet << uint32(TARGET_FLAG_DEST_LOCATION) << ObjectGuid::Empty.WriteAsPacked() << dest->GetPositionX()
               << dest->GetPositionY() << dest->GetPositionZ();
    else
        packet << uint32(TARGET_FLAG_NONE);
    bot->GetSession()->HandleUseItemOpcode(packet);
    return true;
}

// The step's vendor (a ".vendor", or a ".target" among them) is one of those selling the item
bool StepSells(Player* bot, GuideStep const& step, std::vector<uint32> const& vendors)
{
    return std::any_of(
        step.elements.begin(), step.elements.end(),
        [&](GuideElement const& other)
        {
            return GuideRules::Applies(bot, other) &&
                   (other.type == GuideElementType::Vendor ||
                    (other.type == GuideElementType::Target &&
                     std::any_of(other.entries.begin(), other.entries.end(), [&](uint32 entry)
                                 { return std::find(vendors.begin(), vendors.end(), entry) != vendors.end(); })));
        });
}

// A ".complete" objective's item, as a ".collect" of what the quest asks (id 0: not an item objective)
GuideElement ObjectiveItem(GuideElement const& element)
{
    GuideElement collect = element;
    collect.type = GuideElementType::Collect;
    collect.id = 0;
    Quest const* quest = element.type == GuideElementType::Complete && element.objective
                             ? sObjectMgr->GetQuestTemplate(element.id)
                             : nullptr;
    if (!quest)
        return collect;
    std::vector<GuideObjective> objectives = GuideRules::Objectives(quest);
    if (element.objective > objectives.size())
        return collect;
    GuideObjective const& objective = objectives[element.objective - 1];
    if (objective.kind != GuideObjective::Kind::Item)
        return collect;
    collect.id = quest->RequiredItemId[objective.slot];
    collect.count = quest->RequiredItemCount[objective.slot];
    collect.quest = element.id;
    return collect;
}

bool Guiding(PlayerbotAI* botAI, Player* bot)
{
    GuideInfo const& info = botAI->guideInfo;
    return info.active && (info.guide || info.trip.going) && IsSelfBot(bot);
}

void GuideOff(PlayerbotAI* botAI)
{
    botAI->ChangeStrategy("-guide", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("-guide death", BOT_STATE_DEAD);
    botAI->ChangeStrategy("-guide combat", BOT_STATE_COMBAT);
    GuideProgress::Save(botAI);
}

// Where to step off a ride that ends at its stop (no edge known there): the transport stands off the pier or beside
// the zeppelin tower, so the bot is in the water or the air. The nearest dry ground near the stop's height. False:
// none near.
bool FindAshore(Player* bot, WorldPosition const& stop, WorldPosition const& next, WorldPosition& ashore)
{
    Map* map = bot->GetMap();
    std::vector<WorldPosition> dry;
    for (float radius = ASHORE_STEP; radius <= ASHORE_RANGE; radius += ASHORE_STEP)
        for (uint32 i = 0; i < ASHORE_ANGLES; ++i)
        {
            float angle = 2.0f * float(M_PI) * i / ASHORE_ANGLES;
            float x = stop.GetPositionX() + radius * std::cos(angle);
            float y = stop.GetPositionY() + radius * std::sin(angle);
            float z =
                map->GetHeight(bot->GetPhaseMask(), x, y, stop.GetPositionZ() + ASHORE_ABOVE, true, 2 * ASHORE_ABOVE);
            if (z <= INVALID_HEIGHT || map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
                continue;
            dry.emplace_back(stop.GetMapId(), x, y, z);
        }
    if (dry.empty())
        return false;
    ashore = dry.front();
    for (size_t i = 0; i < dry.size() && i < ASHORE_PATHS; ++i)
    {
        PathGenerator path(bot);
        path.CalculatePath(dry[i].GetPositionX(), dry[i].GetPositionY(), dry[i].GetPositionZ(), next.GetPositionX(),
                           next.GetPositionY(), next.GetPositionZ(), false);
        if (IsWholePath(path.GetPathType()))
        {
            ashore = dry[i];
            break;
        }
    }
    return true;
}

// The route ahead needs the item (GuideRules::NeededItems) and the bags' other stacks of it don't hold enough
bool RouteKeeps(Player* bot, Item* item, std::unordered_map<uint32, uint32> const& needed)
{
    auto it = needed.find(item->GetEntry());
    return it != needed.end() && bot->GetItemCount(item->GetEntry()) - item->GetCount() < it->second;
}

// Junk to sell or, with the bags full, to destroy: grey, or white that playerbots rates as only worth selling. Never
// quest items, food, drink, ammo, bags, quest starters, what a quest or the route ahead wants, or what RestedXP banks.
bool IsJunk(Player* bot, Item* item, ItemUsage usage, std::set<uint32> const& collect,
            std::unordered_map<uint32, uint32> const& needed)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (proto->Quality > ITEM_QUALITY_NORMAL || proto->Class == ITEM_CLASS_QUEST ||
        proto->Class == ITEM_CLASS_CONSUMABLE || proto->Class == ITEM_CLASS_PROJECTILE ||
        proto->Class == ITEM_CLASS_CONTAINER || proto->StartQuest || collect.count(proto->ItemId) ||
        bot->HasQuestForItem(proto->ItemId) || GuideMgr::instance().IsKept(proto->ItemId) ||
        RouteKeeps(bot, item, needed))
        return false;
    return proto->Quality == ITEM_QUALITY_POOR || usage == ITEM_USAGE_VENDOR || usage == ITEM_USAGE_AH;
}

// The gossip options that give the step's quests their credit (for the character)
std::vector<GuideGossipCredit> StepGossipCredits(Player* bot, GuideStep const& step)
{
    std::vector<GuideGossipCredit> credits;
    for (GuideElement const& element : step.elements)
        if (element.type == GuideElementType::Complete && GuideRules::Applies(bot, element))
        {
            std::vector<GuideGossipCredit> const& mine = GuideMgr::instance().GetGossipCredits(element.id);
            credits.insert(credits.end(), mine.begin(), mine.end());
        }
    return credits;
}

// Something was already taken from the corpse
bool AnyLooted(Creature* corpse)
{
    for (std::vector<LootItem> const* list : {&corpse->loot.items, &corpse->loot.quest_items})
        for (LootItem const& item : *list)
            if (item.is_looted)
                return true;
    return false;
}

// An equipped item down to fraction of its durability or less (broken: 0)
bool WornDown(Player* bot, float fraction)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (uint32 max = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
                if (item->GetUInt32Value(ITEM_FIELD_DURABILITY) <= max * fraction)
                    return true;
    return false;
}

// Every item in the backpack and bags
std::vector<Item*> BagItems(Player* bot)
{
    std::vector<Item*> items;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            items.push_back(item);
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag* container = bot->GetBagByPos(bag))
            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                if (Item* item = container->GetItemByPos(slot))
                    items.push_back(item);
    return items;
}

// What a vendor's item restocks: food (eaten sitting, health back), drink (mana back), ammo
enum class Supply : uint8
{
    None,
    Food,
    Drink,
    Ammo
};

Supply SupplyOf(ItemTemplate const* proto)
{
    if (proto->Class == ITEM_CLASS_PROJECTILE)
        return Supply::Ammo;
    if (proto->Class != ITEM_CLASS_CONSUMABLE)
        return Supply::None;
    for (auto const& spell : proto->Spells)
        if (SpellInfo const* info = spell.SpellId > 0 ? sSpellMgr->GetSpellInfo(spell.SpellId) : nullptr)
        {
            if (info->HasAura(SPELL_AURA_MOD_POWER_REGEN))
                return Supply::Drink;
            if (info->HasAura(SPELL_AURA_MOD_REGEN))
                return Supply::Food;
        }
    return Supply::None;
}

// The best food and drink in the bags the character can use (0: none): kept when selling
std::pair<uint32, uint32> BestSupplies(Player* bot, std::vector<Item*> const& bags)
{
    ItemTemplate const* best[2] = {nullptr, nullptr};
    for (Item* item : bags)
    {
        ItemTemplate const* proto = item->GetTemplate();
        Supply supply = SupplyOf(proto);
        if ((supply != Supply::Food && supply != Supply::Drink) || proto->RequiredLevel > bot->GetLevel())
            continue;
        ItemTemplate const*& kept = best[supply == Supply::Food ? 0 : 1];
        if (!kept || proto->RequiredLevel > kept->RequiredLevel ||
            (proto->RequiredLevel == kept->RequiredLevel && proto->ItemLevel > kept->ItemLevel))
            kept = proto;
    }
    return {best[0] ? best[0]->ItemId : 0, best[1] ? best[1]->ItemId : 0};
}

// A bag in the bags no bigger than the smallest one worn, with every bag slot taken: it can never be worn
bool SpareBag(Player* bot, ItemTemplate const* proto)
{
    if (proto->Class != ITEM_CLASS_CONTAINER)
        return false;
    uint32 smallest = std::numeric_limits<uint32>::max();
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
    {
        Bag* worn = bot->GetBagByPos(slot);
        if (!worn)
            return false;
        smallest = std::min(smallest, worn->GetBagSize());
    }
    return proto->ContainerSlots <= smallest;
}

// What a vendor takes while guiding: everything with a price but the best food and drink, the quests' items (in the
// log, starting one, or as many as the route ahead wants), what RestedXP banks, and what the character needs to work:
// bags it can wear, reagents, a hunter's ammo.
bool Sellable(Player* bot, Item* item, std::set<uint32> const& collect, std::pair<uint32, uint32> const& supplies,
              std::unordered_map<uint32, uint32> const& needed)
{
    ItemTemplate const* proto = item->GetTemplate();
    if (!proto->SellPrice || proto->Class == ITEM_CLASS_QUEST ||
        (proto->Class == ITEM_CLASS_CONTAINER && !SpareBag(bot, proto)) || proto->Class == ITEM_CLASS_QUIVER ||
        proto->Class == ITEM_CLASS_REAGENT || proto->Class == ITEM_CLASS_KEY ||
        (proto->Class == ITEM_CLASS_PROJECTILE && bot->getClass() == CLASS_HUNTER) || proto->StartQuest ||
        collect.count(proto->ItemId) || bot->HasQuestForItem(proto->ItemId) ||
        GuideMgr::instance().IsKept(proto->ItemId) || RouteKeeps(bot, item, needed))
        return false;
    return proto->ItemId != supplies.first && proto->ItemId != supplies.second;
}

// "walk > Deeprun Tram > Subway > walk > fly to Ironforge"
std::string RouteText(std::vector<GuideLeg> const& legs)
{
    std::string text;
    std::string last;
    for (GuideLeg const& leg : legs)
    {
        std::string part = "walk";
        if (leg.kind == GuideLeg::Kind::Portal)
            part = "portal";
        else if (leg.kind == GuideLeg::Kind::Hearth)
        {
            SpellInfo const* spell = leg.entry ? sSpellMgr->GetSpellInfo(leg.entry) : nullptr;
            part = spell ? spell->SpellName[0] : "hearthstone";
        }
        else if (leg.kind == GuideLeg::Kind::Transport)
        {
            GameObjectTemplate const* go = sObjectMgr->GetGameObjectTemplate(leg.entry);
            part = go ? go->name : "transport";
        }
        else if (leg.kind == GuideLeg::Kind::Fly)
        {
            TaxiNodesEntry const* node = sTaxiNodesStore.LookupEntry(leg.taxiNodes.back());
            part = node && node->name[0] ? "fly to " + std::string(node->name[0]) : "fly";
        }
        if (part == last)
            continue;
        text += (text.empty() ? "" : " > ") + part;
        last = part;
    }
    return text;
}

// The places a step names: those it ends at (radius > 0), or every one when travelOnly is false.
std::vector<GuideElement const*> Gotos(Player* bot, GuideStep const& step, bool travelOnly)
{
    std::vector<GuideElement const*> gotos;
    for (GuideElement const& element : step.elements)
        if (element.type == GuideElementType::Goto && GuideRules::Applies(bot, element) &&
            (!travelOnly || element.radius > 0.0f))
            gotos.push_back(&element);
    return gotos;
}
}  // namespace

bool GuideUpdateAction::isUseful() { return Guiding(botAI, bot); }

bool GuideUpdateAction::Execute(Event /*event*/)
{
    GuideInfo& info = botAI->guideInfo;
    if (info.lastUpdate && GetMSTimeDiffToNow(info.lastUpdate) < UPDATE_INTERVAL)
        return false;
    info.lastUpdate = getMSTime();
    if (bot->IsInFlight())
    {
        info.task = GuideTask();
        info.progressSince = 0;
        return false;
    }
    if (bot->IsAlive())
        info.diedAt = 0;  // resurrected before releasing: the next death waits again
    if (info.trip.going)
        return UpdateTrip(info);

    // Move on past the steps that are done or don't apply (bounded per update; the rest next time).
    bool changed = false;
    for (uint32 i = 0; i < 50; ++i)
    {
        GuideStep const& step = info.guide->steps[info.step];
        if (info.catchUp ? GuideRules::CatchUpTurnIn(bot, *info.guide, step) != nullptr
                         : (!step.side || info.finishSide) && GuideRules::StepApplies(bot, step) && !StepDone(step))
            break;
        if (!GuideRules::Advance(bot, info))
        {
            Finish();
            return true;
        }
        info.stepStart = getMSTime();
        changed = true;
    }
    if (GuideRules::FetchReagents(bot, info))
    {
        info.stepStart = getMSTime();
        changed = true;
    }

    info.useHealth = 0.0f;  // MakeTask sets it again for a use at low health
    info.task = MakeTask(info.guide->steps[info.step]);
    if (info.passStep)
    {
        // The step can't be done (MakeTask said why): on to the next
        LOG_INFO("playerbots.guide", "{}: passes by {} step {} (quest log full)", bot->GetName(), info.guide->name,
                 info.guide->steps[info.step].number);
        info.passStep = false;
        if (!GuideRules::Advance(bot, info))
            Finish();
        info.lastUpdate = 0;
        return true;
    }
    if (!bot->IsInCombat())
    {
        CloseStaleLoot(info);
        FreeBagSpace();
        SideWork(info);
        WaitForLoot(info);
        AvoidPatrols(info);
        WorldPosition target = Goal(info.task);
        Route(info);
        ApplyDetour(info);
        if (CheckTargetStuck(info, target) || CheckStuck(info))
            return true;
    }
    else if (info.stuckSince)
        info.stuckSince = getMSTime();  // a fight doesn't count as stuck
    LogTask(info);
    GuideProgress::Save(botAI);
    // Up from resting: a self-bot is its own master, and playerbots keeps a bot sitting while its master sits.
    if (info.task.type != GuideTaskType::Rest && bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_STAND);
    if (changed)
        botAI->TellMasterNoFacing(GuideRules::Describe(bot, info));
    return changed;
}

bool GuideUpdateAction::StepDone(GuideStep const& step)
{
    bool questWork = GuideRules::HasQuestWork(bot, step);
    bool services = GuideRules::HasServices(bot, step);
    // A travel step: done once its places are reached (MakeTravelTask counts them).
    if (!questWork && !services)
        return botAI->guideInfo.reached >= Gotos(bot, step, true).size();

    if (!GuideRules::QuestWorkDone(bot, step, botAI->guideInfo.skippedWork))
        return false;
    // A failed escort abandoned to take again: not done while out of the log
    for (GuideElement const& element : step.elements)
        if (element.type == GuideElementType::Complete && botAI->guideInfo.retake.count(element.id) &&
            bot->GetQuestStatus(element.id) == QUEST_STATUS_NONE)
            return false;
    for (uint32 i = 0; i < step.elements.size(); ++i)
        if (step.elements[i].IsService() && GuideRules::Applies(bot, step.elements[i]) &&
            !ServiceDone(step, step.elements[i], i))
            return false;
    return true;
}

bool GuideUpdateAction::ServiceDone(GuideStep const& step, GuideElement const& element, uint32 index)
{
    if (index < 32 && (botAI->guideInfo.serviced & (1u << index)))
        return true;
    // Some show by themselves that they're done: the spell known, back home, at the flight's end. The rest (training,
    // selling, binding, flight paths) once the NPC was visited.
    switch (element.type)
    {
        case GuideElementType::Train:
            return GuideRules::ElementDone(bot, element);
        case GuideElementType::Hearth:
            return NearHome(bot);
        case GuideElementType::Use:
        {
            // With an objective in the step, the objective says when (UseForObjective); alone, once used. Equip:
            // once worn. Without the item: nothing to do.
            GuideUse const* use = GuideMgr::instance().GetUse(element.id);
            if (use && use->mode == GuideUse::Mode::Equip)
                return bot->HasItemOrGemWithIdEquipped(element.id, 1) || !bot->HasItemCount(element.id);
            if (use && use->mode == GuideUse::Mode::Open)
                return !bot->HasItemCount(element.id);
            if (!use || !bot->HasItemCount(element.id))
                return true;
            for (GuideElement const& other : step.elements)
                if ((other.type == GuideElementType::Complete || other.type == GuideElementType::Collect) &&
                    GuideRules::Applies(bot, other))
                    return true;
            return false;
        }
        case GuideElementType::Gossip:
        {
            // A bare ".skipgossip" for a window the step opens anyway (a quest giver's): nothing of its own to do
            std::vector<uint32> creatures;
            std::vector<uint32> objects;
            GuideMgr::instance().GossipTargets(bot, step, element, creatures, objects);
            return creatures.empty() && objects.empty();
        }
        case GuideElementType::Fly:
            if (TaxiNodesEntry const* node = FindTaxiNode(element.other, bot->GetTeamId()))
                return bot->GetMapId() == node->map_id && bot->GetExactDist2d(node->x, node->y) <= HOME_REACHED;
            return true;  // no such flight point: walk
        default:
            return false;
    }
}

void GuideUpdateAction::Finish()
{
    GuideInfo& info = botAI->guideInfo;
    info.active = false;
    info.task = GuideTask();
    info.task.type = GuideTaskType::Finished;
    GuideOff(botAI);
    botAI->TellMasterNoFacing(
        PlayerbotTextMgr::instance().GetBotTextOrDefault("guide_finished", "Guide: the route is finished.", {}));
}

GuideTask GuideUpdateAction::MakeTask(GuideStep const& step)
{
    GuideTask task;
    if (DeathSkip(step))
        return task;
    if (MakeRepairTask(task) || MakeVendorTask(task))
        return task;
    // Catching up before leaving the guide: only the step's turn-in of a finished quest
    if (botAI->guideInfo.catchUp)
    {
        if (GuideElement const* turnIn = GuideRules::CatchUpTurnIn(bot, *botAI->guideInfo.guide, step))
            MakeGiverTask(step, *turnIn, task);
        return task;
    }
    for (uint32 i = 0; i < step.elements.size(); ++i)
    {
        GuideElement const& element = step.elements[i];
        if (element.IsService() && GuideRules::Applies(bot, element) && !ServiceDone(step, element, i))
        {
            if (element.type == GuideElementType::Hearth)
            {
                task.type = GuideTaskType::Hearth;
                task.element = i;
            }
            else if (element.type == GuideElementType::Use)
            {
                if (!MakeUseTask(step, element, i, task) && i < 32)
                    botAI->guideInfo.serviced |= 1u << i;  // can't be used (no plan, cooldown): pass it by
            }
            else
                MakeServiceTask(step, element, i, task);
            return task;
        }
        // A failed escort abandoned: taken again from its NPC
        if (element.type == GuideElementType::Complete && botAI->guideInfo.retake.count(element.id) &&
            GuideRules::Applies(bot, element))
        {
            if (bot->GetQuestStatus(element.id) == QUEST_STATUS_NONE)
            {
                GuideElement accept;
                accept.type = GuideElementType::Accept;
                accept.id = element.id;
                MakeGiverTask(step, accept, task);
                return task;
            }
        }
        if (!element.IsQuestWork() || !GuideRules::Applies(bot, element) || GuideRules::ElementDone(bot, element) ||
            (i < 32 && (botAI->guideInfo.skippedWork & (1u << i))))
            continue;

        // The step's gossip first: talking is how a "speak to" objective is done (or an escort started)
        if (element.type == GuideElementType::Complete)
            for (uint32 j = 0; j < step.elements.size(); ++j)
                if (step.elements[j].type == GuideElementType::Gossip && GuideRules::Applies(bot, step.elements[j]) &&
                    !ServiceDone(step, step.elements[j], j))
                {
                    MakeServiceTask(step, step.elements[j], j, task);
                    return task;
                }

        switch (element.type)
        {
            case GuideElementType::TurnIn:
                // Not finished yet (a step missed or failed): go back to the step that gathers what's missing.
                if (bot->GetQuestStatus(element.id) == QUEST_STATUS_INCOMPLETE && FinishQuestTask(element.id, task))
                    break;
                MakeGiverTask(step, element, task);
                break;
            case GuideElementType::Accept:
                // A full log: drop a quest the route has no use for left, else pass the step by
                if (bot->FindQuestSlot(0) >= MAX_QUEST_LOG_SIZE && !MakeRoom(element.id))
                {
                    botAI->guideInfo.passStep = true;
                    break;
                }
                MakeGiverTask(step, element, task);
                break;
            case GuideElementType::Abandon:
                AbandonQuest(element.id);
                break;
            case GuideElementType::Collect:
                if (!UseForObjective(step, task))
                    MakeCollectTask(step, element, i, task);
                break;
            case GuideElementType::Complete:
            {
                // An item whose use completes the quest (the Winterhoof Cleansing Totem at the well): used
                GuideElement use;
                use.type = GuideElementType::Use;
                use.id = GuideMgr::instance().GetQuestUseItem(element.id);
                if (use.id && MakeUseTask(step, use, 32, task))
                    break;
                // An event nothing on the realm completes (an escort with no script): passed by, said
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(element.id);
                    quest && quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) &&
                    !GuideMgr::instance().CanCompleteEvent(element.id) &&
                    std::none_of(step.elements.begin(), step.elements.end(), [&](GuideElement const& other)
                                 { return other.type == GuideElementType::Use && GuideRules::Applies(bot, other); }))
                {
                    PassBy(i, quest->GetTitle(), "nothing on this realm completes its event");
                    return GuideTask();
                }
                if (!UseForObjective(step, task))
                    MakeKillTask(step, element, i, task);
                break;
            }
            case GuideElementType::Xp:
                task.type = GuideTaskType::Level;
                task.questId = element.id;
                if (!ReadyToFight())
                {
                    task.type = GuideTaskType::Rest;
                    break;
                }
                if (Unit* target = AI_VALUE(Unit*, "grind target"))
                    task.target = target->GetGUID();
                else
                    MakeAreaTask(step, task);
                break;
            default:
                break;
        }
        return task;
    }

    MakeTravelTask(step, task);
    return task;
}

void GuideUpdateAction::AbandonQuest(uint32 questId)
{
    // As the client drops it (quest items and timers go with it)
    uint16 slot = bot->FindQuestSlot(questId);
    if (slot >= MAX_QUEST_LOG_SIZE)
        return;
    LOG_INFO("playerbots.guide", "{}: abandons {}", bot->GetName(), questId);
    WorldPacket packet(CMSG_QUESTLOG_REMOVE_QUEST);
    packet << uint8(slot);
    WorldPackets::Quest::QuestLogRemoveQuest remove(std::move(packet));
    remove.Read();
    bot->GetSession()->HandleQuestLogRemoveQuest(remove);
}

bool GuideUpdateAction::MakeRoom(uint32 questId)
{
    // The quest log is full: drop the lowest quest the route has no later step for. None: say so (once per quest).
    uint32 drop = 0;
    uint32 dropLevel = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        uint32 inLog = bot->GetQuestSlotQuestId(slot);
        Quest const* quest = inLog ? sObjectMgr->GetQuestTemplate(inLog) : nullptr;
        if (!quest || inLog == questId || GuideRules::StillNeeded(bot, botAI->guideInfo, inLog))
            continue;
        uint32 level = quest->GetQuestLevel() > 0 ? quest->GetQuestLevel() : bot->GetLevel();
        if (!drop || level < dropLevel)
        {
            drop = inLog;
            dropLevel = level;
        }
    }
    PlayerbotTextMgr& text = PlayerbotTextMgr::instance();
    Quest const* wanted = sObjectMgr->GetQuestTemplate(questId);
    std::string wantedName = wanted ? wanted->GetTitle() : std::to_string(questId);
    if (!drop)
    {
        LOG_INFO("playerbots.guide", "{}: quest log full, nothing to drop for {}", bot->GetName(), questId);
        botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
            "guide_log_full",
            "Guide: my quest log is full and the route still needs every quest in it; leaving %quest.",
            {{"%quest", wantedName}}));
        return false;
    }
    Quest const* dropped = sObjectMgr->GetQuestTemplate(drop);
    botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
        "guide_log_made_room", "Guide: my quest log is full; dropping %dropped (no step left for it) to take %quest.",
        {{"%dropped", dropped ? dropped->GetTitle() : std::to_string(drop)}, {"%quest", wantedName}}));
    AbandonQuest(drop);
    return true;
}

void GuideUpdateAction::FreeBagSpace(bool full)
{
    // Nearly full bags (no room for a quest drop or a reward), or a needed item that didn't fit (full): the cheapest
    // junk goes, grey first
    if (!full && bot->GetFreeInventorySpace() >= MIN_FREE_SLOTS)
        return;
    std::set<uint32> const& collect = AI_VALUE(std::set<uint32>&, "always loot list");
    std::unordered_map<uint32, uint32> const& needed = GuideRules::NeededItems(bot, botAI->guideInfo);
    std::vector<Item*> bags = BagItems(bot);
    Item* cheapest = nullptr;
    auto rank = [](Item* item)
    { return std::make_pair(item->GetTemplate()->Quality, item->GetTemplate()->SellPrice * item->GetCount()); };
    for (Item* item : bags)
        if (IsJunk(bot, item, AI_VALUE2(ItemUsage, "item usage", item->GetEntry()), collect, needed) &&
            (!cheapest || rank(item) < rank(cheapest)))
            cheapest = item;
    // No junk and a needed item that didn't fit: the cheapest of what a vendor would take (the bags full of cloth
    // and meat, the quest drop left on the corpse)
    if (!cheapest && full)
    {
        std::pair<uint32, uint32> const supplies = BestSupplies(bot, bags);
        for (Item* item : bags)
            if (Sellable(bot, item, collect, supplies, needed) && (!cheapest || rank(item) < rank(cheapest)))
                cheapest = item;
    }
    if (!cheapest)
        return;

    LOG_INFO("playerbots.guide", "{}: bags full, destroys {} x{}", bot->GetName(), cheapest->GetTemplate()->Name1,
             cheapest->GetCount());
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_bags_full", "Guide: my bags are full; destroying %item to make room.",
        {{"%item", cheapest->GetTemplate()->Name1}}));
    bot->DestroyItem(cheapest->GetBagSlot(), cheapest->GetSlot(), true);
}

void GuideUpdateAction::CloseStaleLoot(GuideInfo& info)
{
    // A loot window left open (nothing took what's left, or nothing closed it) stops all looting: closed after a while
    ObjectGuid open = bot->GetLootGUID();
    if (!open)
    {
        info.lootOpenSince = 0;
        return;
    }
    if (!info.lootOpenSince)
    {
        info.lootOpenSince = getMSTime();
        return;
    }
    if (GetMSTimeDiffToNow(info.lootOpenSince) < STALE_LOOT)
        return;
    info.lootOpenSince = 0;
    LOG_DEBUG("playerbots.guide", "{}: closes a loot window left open", bot->GetName());
    WorldPacket release(CMSG_LOOT_RELEASE, 8);
    release << open;
    bot->GetSession()->HandleLootReleaseOpcode(release);
}

void GuideUpdateAction::MakeGiverTask(GuideStep const& step, GuideElement const& element, GuideTask& task)
{
    bool accept = element.type == GuideElementType::Accept;
    task.type = accept ? GuideTaskType::Accept : GuideTaskType::TurnIn;
    task.questId = element.id;

    GuideMgr const& mgr = GuideMgr::instance();
    // A quest an item starts (a deed, a letter): taken from the item in the bags, as the client does
    if (accept)
        for (uint32 entry : mgr.GetQuestStarterItems(element.id))
            if (Item* item = bot->GetItemByEntry(entry))
            {
                task.giver = item->GetGUID();
                task.interact = true;
                return;
            }

    WorldObject* giver = nullptr;
    for (uint32 entry : accept ? mgr.GetQuestStarters(element.id) : mgr.GetQuestEnders(element.id))
        if (Creature* creature = bot->FindNearestCreature(entry, GIVER_RANGE))
            if (!giver || bot->GetExactDist(creature) < bot->GetExactDist(giver))
                giver = creature;
    if (!giver)
        for (uint32 entry : accept ? mgr.GetQuestStarterObjects(element.id) : mgr.GetQuestEnderObjects(element.id))
            if (GameObject* object = bot->FindNearestGameObject(entry, GIVER_RANGE))
                if (!giver || bot->GetExactDist(object) < bot->GetExactDist(giver))
                    giver = object;

    if (giver)
    {
        task.giver = giver->GetGUID();
        task.interact = bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE);
        return;
    }

    // Not in sight: walk to where it spawns by the step's place (the place has no height: indoors that is the roof),
    // else to the place, or wait there for it.
    std::vector<GuideElement const*> places = Gotos(bot, step, true);
    if (places.empty())
        places = Gotos(bot, step, false);
    std::vector<WorldPosition> spawns =
        SpawnPlaces(accept ? mgr.GetQuestStarters(element.id) : mgr.GetQuestEnders(element.id),
                    places.empty() ? places : std::vector<GuideElement const*>{places.front()});
    WorldPosition place = !spawns.empty() ? spawns.front() : !places.empty() ? Place(*places.front()) : WorldPosition();
    if (place != WorldPosition() && bot->GetExactDist(place) > 5.0f && !spawns.empty())
    {
        task.moveTo = place;
        return;
    }
    // Not by the step's places (elsewhere in town, on the tram's map): where it spawns nearest, routed there
    WorldPosition nearest = NearestSpawn(accept ? mgr.GetQuestStarters(element.id) : mgr.GetQuestEnders(element.id));
    if (nearest != WorldPosition() && (nearest.GetMapId() != bot->GetMapId() || bot->GetExactDist(nearest) > 5.0f))
    {
        task.moveTo = nearest;
        return;
    }
    if (place != WorldPosition() && bot->GetExactDist(place) > 5.0f)
    {
        task.moveTo = place;
        return;
    }
    task.wander = true;
}

WorldPosition GuideUpdateAction::NearestSpawn(std::vector<uint32> const& entries)
{
    // On the bot's map first (nearest), else anywhere
    WorldPosition best;
    for (uint32 entry : entries)
        for (GuideSpawn const& spawn : GuideMgr::instance().GetSpawns(entry))
        {
            WorldPosition pos(spawn.mapId, spawn.x, spawn.y, spawn.z);
            bool here = spawn.mapId == bot->GetMapId();
            bool bestHere = best != WorldPosition() && best.GetMapId() == bot->GetMapId();
            if (best == WorldPosition() || (here && !bestHere) ||
                (here && bestHere && bot->GetExactDist(pos) < bot->GetExactDist(best)))
                best = pos;
        }
    return best;
}

void GuideUpdateAction::MakeServiceTask(GuideStep const& step, GuideElement const& element, uint32 index,
                                        GuideTask& task)
{
    task.type = GuideTaskType::Service;
    task.element = index;

    GuideElementType type = element.type;
    std::vector<uint32> targets;
    for (GuideElement const& other : step.elements)
        if (other.type == GuideElementType::Target && GuideRules::Applies(bot, other))
            targets.insert(targets.end(), other.entries.begin(), other.entries.end());
    // Gossip: the NPCs, or objects, to talk to
    std::vector<uint32> talkTo;
    std::vector<uint32> talkToObjects;
    if (type == GuideElementType::Gossip)
        GuideMgr::instance().GossipTargets(bot, step, element, talkTo, talkToObjects);
    auto suits = [&](Creature* creature)
    {
        if (!creature || !creature->IsAlive())
            return false;
        if (type == GuideElementType::Gossip)
            return std::find(talkTo.begin(), talkTo.end(), creature->GetEntry()) != talkTo.end();
        if (type == GuideElementType::Vendor)
            return creature->IsVendor();
        if (type == GuideElementType::Home)
            return creature->IsInnkeeper();
        if (type == GuideElementType::FlightPath || type == GuideElementType::Fly)
            return creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER);
        if (!creature->IsTrainer())
            return false;
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(creature->GetEntry());
        if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
            return false;
        return element.type != GuideElementType::Train || GuideRules::TrainerSpellFor(trainer, element.id);
    };

    // The step's own NPC (".target"), else the nearest that does the job.
    Creature* npc = nullptr;
    for (GuideElement const& other : step.elements)
        if (other.type == GuideElementType::Target && GuideRules::Applies(bot, other))
            for (uint32 entry : other.entries)
                if (Creature* creature = bot->FindNearestCreature(entry, GIVER_RANGE))
                    if (suits(creature) && (!npc || bot->GetExactDist(creature) < bot->GetExactDist(npc)))
                        npc = creature;
    if (!npc)
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
            if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
                if (suits(creature) && (!npc || bot->GetExactDist(creature) < bot->GetExactDist(npc)))
                    npc = creature;

    if (npc)
    {
        task.giver = npc->GetGUID();
        task.interact = bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE);
        return;
    }
    GameObject* object = nullptr;
    for (uint32 entry : talkToObjects)
        if (GameObject* found = bot->FindNearestGameObject(entry, GIVER_RANGE))
            if (!object || bot->GetExactDist(found) < bot->GetExactDist(object))
                object = found;
    if (object)
    {
        task.giver = object->GetGUID();
        task.interact = bot->IsWithinDistInMap(object, INTERACTION_DISTANCE);
        return;
    }

    // Not in sight: the step's place first, then where one spawns within SERVICE_RANGE of it (the step's own NPC
    // first, else the nearest that does the job: upstairs, inside, round the corner). There and still nobody: pass
    // it by, and say so.
    GuideInfo& info = botAI->guideInfo;
    std::vector<GuideElement const*> places = Gotos(bot, step, true);
    if (places.empty())
        places = Gotos(bot, step, false);
    WorldPosition around = places.empty() ? WorldPosition(bot) : Place(*places.front());
    if (!places.empty() && bot->GetExactDist(around) > 5.0f &&
        std::none_of(info.serviceSearched.begin(), info.serviceSearched.end(),
                     [&](WorldPosition const& seen) { return seen.GetExactDist(&around) <= 5.0f; }))
    {
        task.moveTo = around;
        return;
    }
    if (!places.empty() && bot->GetExactDist(around) <= 5.0f)
        info.serviceSearched.push_back(around);

    std::vector<std::pair<uint32, GuideSpawn>> candidates;
    if (type == GuideElementType::Gossip)
    {
        for (bool objects : {false, true})
            for (uint32 entry : objects ? talkToObjects : talkTo)
                for (GuideSpawn const& spawn :
                     objects ? GuideMgr::instance().GetObjectSpawns(entry) : GuideMgr::instance().GetSpawns(entry))
                    if (spawn.mapId == around.GetMapId() &&
                        std::hypot(spawn.x - around.GetPositionX(), spawn.y - around.GetPositionY()) <= SERVICE_RANGE)
                        candidates.push_back({entry, spawn});
    }
    else
        candidates = GuideMgr::instance().ServicesNear(around.GetMapId(), around.GetPositionX(), around.GetPositionY(),
                                                       SERVICE_RANGE);
    WorldPosition best;
    bool bestTarget = false;
    for (auto const& [entry, spawn] : candidates)
    {
        WorldPosition pos(spawn.mapId, spawn.x, spawn.y, spawn.z);
        if (pos.GetMapId() != bot->GetMapId() ||
            (type != GuideElementType::Gossip && !GuideRules::Serves(bot, element, entry)))
            continue;
        if (bot->GetExactDist(pos) <= 5.0f)
        {
            info.serviceSearched.push_back(pos);  // here, and nobody: not again
            continue;
        }
        if (std::any_of(info.serviceSearched.begin(), info.serviceSearched.end(),
                        [&](WorldPosition const& seen) { return seen.GetExactDist(&pos) <= 5.0f; }))
            continue;
        bool isTarget = std::find(targets.begin(), targets.end(), entry) != targets.end();
        if (best == WorldPosition() || (isTarget && !bestTarget) ||
            (isTarget == bestTarget && bot->GetExactDist(pos) < bot->GetExactDist(best)))
        {
            best = pos;
            bestTarget = isTarget;
        }
    }
    if (best != WorldPosition())
    {
        task.moveTo = best;
        return;
    }

    static char const* const WHAT[] = {"trainer", "vendor", "innkeeper", "flight master", "NPC to talk to"};
    char const* what = type == GuideElementType::Vendor                                          ? WHAT[1]
                       : type == GuideElementType::Home                                          ? WHAT[2]
                       : (type == GuideElementType::FlightPath || type == GuideElementType::Fly) ? WHAT[3]
                       : type == GuideElementType::Gossip                                        ? WHAT[4]
                                                                                                 : WHAT[0];
    LOG_INFO("playerbots.guide", "{}: no {} found for {} step {}; passed by", bot->GetName(), what, info.guide->name,
             step.number);
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_service_missing", "Guide: I can't find the %what for step %step; passing it by.",
        {{"%what", what}, {"%step", std::to_string(step.number)}}));
    if (index < 32)
        info.serviced |= 1u << index;
    task.type = GuideTaskType::None;
}

bool GuideUpdateAction::MakeRepairTask(GuideTask& task)
{
    // Worn down: the nearest friendly repairer in sight, else whose spawn is within REPAIR_RANGE, before the step's
    // work. One found gone, or too dear for the purse, isn't walked to again this step.
    if (!WornDown(bot, REPAIR_DURABILITY) || !bot->GetMoney())
        return false;
    GuideInfo& info = botAI->guideInfo;
    auto searched = [&](WorldPosition const& pos)
    {
        return std::any_of(info.repairSearched.begin(), info.repairSearched.end(),
                           [&](WorldPosition const& seen) { return seen.GetExactDist(&pos) <= 5.0f; });
    };
    FactionTemplateEntry const* ours = bot->GetFactionTemplateEntry();

    Creature* npc = nullptr;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
        if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
            if (creature->IsAlive() && creature->IsArmorer() && !creature->IsHostileTo(bot) &&
                !searched(WorldPosition(creature)) && (!npc || bot->GetExactDist(creature) < bot->GetExactDist(npc)))
                npc = creature;
    if (npc)
    {
        task.type = GuideTaskType::Service;
        task.repair = true;
        task.giver = npc->GetGUID();
        task.interact = bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE);
        return true;
    }

    WorldPosition best;
    for (auto const& [entry, spawn] :
         GuideMgr::instance().RepairersNear(bot->GetMapId(), bot->GetPositionX(), bot->GetPositionY(), REPAIR_RANGE))
    {
        CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
        FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creature->faction);
        if (!ours || !faction || faction->IsHostileTo(*ours))
            continue;
        WorldPosition pos(spawn.mapId, spawn.x, spawn.y, spawn.z);
        if (bot->GetExactDist(pos) <= 5.0f)
        {
            info.repairSearched.push_back(pos);  // here, and nobody
            continue;
        }
        if (!searched(pos) && (best == WorldPosition() || bot->GetExactDist(pos) < bot->GetExactDist(best)))
            best = pos;
    }
    if (best == WorldPosition())
        return false;
    task.type = GuideTaskType::Service;
    task.repair = true;
    task.moveTo = best;
    return true;
}

bool GuideUpdateAction::DeathSkip(GuideStep const& step)
{
    // RestedXP's death skip (jump off a ledge, respawn at the spirit healer by the step's place): put at that
    // graveyard instead, as the respawn would. The step's last place says which; without one, or none near it, walked.
    GuideElement const* place = nullptr;
    bool skip = false;
    for (GuideElement const& element : step.elements)
        if (GuideRules::Applies(bot, element))
        {
            skip |= element.type == GuideElementType::DeathSkip;
            if (element.type == GuideElementType::Goto)
                place = &element;
        }
    if (!skip || !place || !bot->IsAlive() || bot->IsInFlight() || bot->GetTransport() || bot->IsBeingTeleported() ||
        (bot->GetMapId() == place->mapId && bot->GetExactDist2d(place->x, place->y) <= DEATHSKIP_DONE))
        return false;

    GraveyardStruct const* best = nullptr;
    for (auto const& [id, graveyard] : sGraveyard->GetGraveyardData())
    {
        float distance = std::hypot(graveyard.x - place->x, graveyard.y - place->y);
        if (graveyard.Map == place->mapId && distance <= DEATHSKIP_GRAVEYARD &&
            (!best || distance < std::hypot(best->x - place->x, best->y - place->y)))
            best = &graveyard;
    }
    if (!best)
    {
        LOG_INFO("playerbots.guide", "{}: death skip on {} step {}: no graveyard near {:.1f} {:.1f}; walking",
                 bot->GetName(), botAI->guideInfo.guide->name, step.number, place->x, place->y);
        return false;
    }
    LOG_INFO("playerbots.guide", "{}: death skip on {} step {}: put at {} ({:.1f} {:.1f} {:.1f})", bot->GetName(),
             botAI->guideInfo.guide->name, step.number, best->name, best->x, best->y, best->z);
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_death_skip", "Guide: death skip to %place.", {{"%place", best->name}}));
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    bot->TeleportTo(best->Map, best->x, best->y, best->z, bot->GetOrientation());
    botAI->guideInfo.lastUpdate = 0;
    return true;
}

bool GuideUpdateAction::MakeVendorTask(GuideTask& task)
{
    // Junk in the bags and a friendly vendor in sight near by: sold there on the way (and repaired, restocked), once
    // per vendor this step. One vendor at a time, for VENDOR_CHASE at most (one it can't get to, upstairs or behind a
    // wall, is given up on; the nearest of several changing as it walks kept it from any).
    GuideInfo& info = botAI->guideInfo;
    Creature* npc = nullptr;
    if (!info.vendorChase.IsEmpty())
    {
        npc = ObjectAccessor::GetCreature(*bot, info.vendorChase);
        if (!npc || !npc->IsAlive() || GetMSTimeDiffToNow(info.vendorChaseSince) > VENDOR_CHASE)
        {
            info.vendorSearched.insert(info.vendorChase);
            info.vendorChase.Clear();
            npc = nullptr;
        }
    }
    if (!npc)
        for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
            if (Creature* creature = ObjectAccessor::GetCreature(*bot, guid))
                if (creature->IsAlive() && creature->IsVendor() && !creature->IsHostileTo(bot) &&
                    bot->GetExactDist(creature) <= VENDOR_RANGE && !info.vendorSearched.count(creature->GetGUID()) &&
                    (!npc || bot->GetExactDist(creature) < bot->GetExactDist(npc)))
                    npc = creature;
    if (!npc)
        return false;

    std::set<uint32> const& collect = AI_VALUE(std::set<uint32>&, "always loot list");
    std::unordered_map<uint32, uint32> const& needed = GuideRules::NeededItems(bot, info);
    std::vector<Item*> bags = BagItems(bot);
    std::pair<uint32, uint32> const supplies = BestSupplies(bot, bags);
    if (std::none_of(bags.begin(), bags.end(),
                     [&](Item* item) { return Sellable(bot, item, collect, supplies, needed); }))
        return false;

    if (info.vendorChase != npc->GetGUID())
    {
        info.vendorChase = npc->GetGUID();
        info.vendorChaseSince = getMSTime();
    }
    task.type = GuideTaskType::Service;
    task.vendor = true;
    task.giver = npc->GetGUID();
    task.interact = bot->IsWithinDistInMap(npc, INTERACTION_DISTANCE);
    return true;
}

bool GuideUpdateAction::MakeGossipCreditTask(uint32 questId, GuideTask& task)
{
    // A quest whose event a gossip option does (SmartAI): its NPC or object in sight, else where one spawns
    std::vector<GuideGossipCredit> const& credits = GuideMgr::instance().GetGossipCredits(questId);
    if (credits.empty())
        return false;
    WorldObject* giver = nullptr;
    for (GuideGossipCredit const& credit : credits)
    {
        WorldObject* found = credit.object
                                 ? static_cast<WorldObject*>(bot->FindNearestGameObject(credit.entry, GIVER_RANGE))
                                 : bot->FindNearestCreature(credit.entry, GIVER_RANGE);
        if (found && (!giver || bot->GetExactDist(found) < bot->GetExactDist(giver)))
            giver = found;
    }
    task.type = GuideTaskType::Service;
    task.gossipQuest = questId;
    if (giver)
    {
        task.giver = giver->GetGUID();
        task.interact = bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE);
        return true;
    }
    WorldPosition best;
    for (GuideGossipCredit const& credit : credits)
        for (GuideSpawn const& spawn : credit.object ? GuideMgr::instance().GetObjectSpawns(credit.entry)
                                                     : GuideMgr::instance().GetSpawns(credit.entry))
        {
            WorldPosition pos(spawn.mapId, spawn.x, spawn.y, spawn.z);
            if (spawn.mapId == bot->GetMapId() &&
                (best == WorldPosition() || bot->GetExactDist(pos) < bot->GetExactDist(best)))
                best = pos;
        }
    if (best == WorldPosition())
    {
        task = GuideTask();
        return false;
    }
    task.moveTo = best;
    return true;
}

bool GuideUpdateAction::MakeEmoteTask(uint32 questId, GuideTask& task)
{
    // A quest a C++ script credits for an emote at its giver: near it, the emote (again, until it counts)
    uint32 emote = GuideMgr::instance().GetQuestEmote(questId);
    if (!emote)
        return false;
    Creature* giver = nullptr;
    for (uint32 entry : GuideMgr::instance().GetQuestStarters(questId))
        if (Creature* found = bot->FindNearestCreature(entry, GIVER_RANGE))
            if (!giver || bot->GetExactDist(found) < bot->GetExactDist(giver))
                giver = found;
    task.type = GuideTaskType::Service;
    task.emote = emote;
    if (giver)
    {
        task.giver = giver->GetGUID();
        task.interact = bot->IsWithinDistInMap(giver, INTERACTION_DISTANCE);
        return true;
    }
    task.moveTo = NearestSpawn(GuideMgr::instance().GetQuestStarters(questId));
    if (task.moveTo == WorldPosition())
    {
        task = GuideTask();
        return false;
    }
    return true;
}

void GuideUpdateAction::MakeKillTask(GuideStep const& step, GuideElement const& element, uint32 index, GuideTask& task)
{
    // An item the step's vendor sells, or one nothing drops or holds: bought (the Flagon of Dwarven Honeymead from
    // Menethil's innkeeper, not from a far dropper on the other side of the sea)
    if (GuideElement const item = ObjectiveItem(element); item.id)
    {
        GuideMgr const& mgr = GuideMgr::instance();
        std::vector<uint32> const& vendors = mgr.GetItemVendors(item.id);
        bool dropped = !mgr.GetItemDroppers(item.id).empty() || !mgr.GetItemObjects(item.id).empty();
        bool given = index < 32 && (botAI->guideInfo.skippedWork & (1u << index));  // couldn't buy it
        if (!vendors.empty() && !given && (StepSells(bot, step, vendors) || !dropped) &&
            bot->GetItemCount(item.id, true) < item.count && MakeBuyTask(step, index, vendors, task))
            return;
    }

    task.type = GuideTaskType::Kill;
    task.questId = element.id;
    task.objective = element.objective;

    Quest const* quest = sObjectMgr->GetQuestTemplate(element.id);
    std::vector<GuideObjective> objectives = GuideRules::Objectives(quest);
    bool questLoot = false;
    if (element.objective && element.objective <= objectives.size())
    {
        GuideObjective const& objective = objectives[element.objective - 1];
        // An event: a gossip option, an emote, an area to explore, else the giver's script (an escort, a fight)
        if (objective.kind == GuideObjective::Kind::Exploration &&
            (MakeGossipCreditTask(element.id, task) || MakeEmoteTask(element.id, task) ||
             MakeExploreTask(element.id, task) || MakeEscortTask(element.id, task)))
            return;
        if (objective.kind == GuideObjective::Kind::CreatureOrObject && quest->RequiredNpcOrGo[objective.slot] > 0)
            task.entries.push_back(static_cast<uint32>(quest->RequiredNpcOrGo[objective.slot]));
        else if (objective.kind == GuideObjective::Kind::CreatureOrObject)
            task.objects.push_back(static_cast<uint32>(-quest->RequiredNpcOrGo[objective.slot]));  // to use
        questLoot = objective.kind == GuideObjective::Kind::Item;
        if (questLoot)
        {
            // Where it comes from: objects holding it, creatures dropping it (their spawns are searched)
            uint32 item = quest->RequiredItemId[objective.slot];
            task.objects = GuideMgr::instance().GetItemObjects(item);
            std::vector<uint32> const& droppers = GuideMgr::instance().GetItemDroppers(item);
            task.entries.insert(task.entries.end(), droppers.begin(), droppers.end());
        }
    }
    for (GuideElement const& other : step.elements)
        if (other.type == GuideElementType::Mob && GuideRules::Applies(bot, other))
            task.entries.insert(task.entries.end(), other.entries.begin(), other.entries.end());
    std::set<uint32> const finished = GuideRules::FinishedKills(bot, step);
    task.entries.erase(
        std::remove_if(task.entries.begin(), task.entries.end(), [&](uint32 entry) { return finished.count(entry); }),
        task.entries.end());

    if (MakeObjectTask(task.objects, task))
        return;
    if (!ReadyToFight())
    {
        task.type = GuideTaskType::Rest;
        return;
    }
    task.target = FindKillTarget(task.entries, questLoot);
    if (task.target.IsEmpty() && !MakeLeadTask(step, task))
        MakeAreaTask(step, task);
}

bool GuideUpdateAction::MakeLeadTask(GuideStep const& step, GuideTask& task)
{
    // A creature to kill that isn't hostile yet: one a gossip option sets off (the Threshwackonator 4100's "Turn the
    // key") is talked to; following the bot then, it's led to the step's last place, where it turns (by Gelkak).
    for (uint32 entry : task.entries)
    {
        Creature* creature = bot->FindNearestCreature(entry, ESCORT_RANGE);
        if (!creature || creature->IsInCombat() || bot->IsValidAttackTarget(creature))
            continue;
        if (creature->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
            bot->GetExactDist(creature) <= LEAD_RANGE)
        {
            std::vector<GuideElement const*> gotos = Gotos(bot, step, false);
            if (gotos.empty())
                continue;
            WorldPosition place = Place(*gotos.back());
            if (bot->GetExactDist(place) > WAYPOINT_REACHED)
                task.moveTo = place;  // there: waits for it to turn
            return true;
        }
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) && creature->GetAIName() == "SmartAI")
        {
            task.type = GuideTaskType::Service;
            task.wake = true;
            task.giver = creature->GetGUID();
            task.interact = bot->IsWithinDistInMap(creature, INTERACTION_DISTANCE);
            return true;
        }
    }
    return false;
}

bool GuideUpdateAction::MakeObjectTask(std::vector<uint32> const& objects, GuideTask& task, float range)
{
    // The nearest object in sight that holds the quest item, or is the quest's object to use, and can be opened or used
    // now (spawned, ready, the quest's), with a whole path to it: walk right up to it (the loot strategy only adds
    // objects within Playerbots.LootDistance, on its own ticks), then open or use it.
    if (objects.empty())
        return false;
    std::map<ObjectGuid, uint32> const& unreachable = botAI->guideInfo.unreachable;
    std::list<GameObject*> found;
    bot->GetGameObjectListWithEntryInGrid(found, objects, range);
    found.sort([&](GameObject* a, GameObject* b) { return bot->GetExactDist(a) < bot->GetExactDist(b); });
    uint32 checks = 0;
    for (GameObject* object : found)
    {
        if (!object->isSpawned() || object->GetGoState() != GO_STATE_READY ||
            object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) ||
            (object->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !object->ActivateToQuest(bot)) ||
            unreachable.count(object->GetGUID()))
            continue;
        float distance = bot->GetExactDist(object);
        if (distance > WAYPOINT_REACHED && !HasWholePath(WorldPosition(object)))
        {
            botAI->guideInfo.unreachable[object->GetGUID()] = getMSTime() + UNREACHABLE_TIME;
            if (++checks >= KILL_PATH_CHECKS)
                break;
            continue;
        }
        // In reach as playerbots' "open loot" counts it (to its edge), or as near as the paths get (stopped within
        // interaction distance: used; a chest, stepped up to: GuideOpenAction)
        task.object = object->GetGUID();
        float edge = bot->GetDistance(object);
        if (edge <= OPEN_RANGE ||
            (edge <= INTERACTION_DISTANCE && (!bot->isMoving() || botAI->guideInfo.stepIn == object->GetGUID())))
            task.open = true;
        else
            task.moveTo = WorldPosition(object);
        return true;
    }
    return false;
}

bool GuideUpdateAction::HasWholePath(WorldPosition const& to, bool* enemy)
{
    // The core's paths search a limited part of the navmesh: further, they end at the nearest point they found (from
    // a hill, the ground over the cave the place is in). With enemy: whether the way passes the other faction's
    // creatures.
    PathGenerator path(bot);
    if (!path.CalculatePath(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ()) ||
        !IsWholePath(path.GetPathType()))
        return false;
    G3D::Vector3 const& end = path.GetActualEndPosition();
    if (enemy)
    {
        *enemy = false;
        Movement::PointsArray const& points = path.GetPath();
        for (size_t i = 0; i < points.size() && !*enemy; i += 3)
            *enemy = GuideMgr::instance().EnemyNear(bot->GetTeamId(), bot->GetMapId(), points[i].x, points[i].y,
                                                    ENEMY_RANGE);
    }
    return to.GetExactDist(end.x, end.y, end.z) <= 5.0f;
}

WorldPosition GuideUpdateAction::Goal(GuideTask const& task)
{
    // Where the task walks to: its place, or the quest giver it walks up to
    if (task.moveTo != WorldPosition())
        return task.moveTo;
    if (task.giver && !task.interact)
        if (WorldObject* giver = ObjectAccessor::GetWorldObject(*bot, task.giver))
            return WorldPosition(giver);
    return WorldPosition();
}

void GuideUpdateAction::ApplyDetour(GuideInfo& info)
{
    // A fallback's place on the way to a target it couldn't reach: walked to first
    if (info.detour == WorldPosition())
        return;
    if (bot->GetMapId() != info.detour.GetMapId() || bot->GetExactDist2d(info.detour) <= WAYPOINT_REACHED)
    {
        // There: from here, the target again, with a fresh minute
        info.detour = WorldPosition();
        info.stuckSince = getMSTime();
        info.stuckBest = std::numeric_limits<float>::max();
        return;
    }
    info.task.moveTo = info.detour;
    info.task.giver = ObjectGuid::Empty;
    info.task.interact = info.task.wander = false;
}

bool GuideUpdateAction::CheckTargetStuck(GuideInfo& info, WorldPosition const& target)
{
    // No closer to the target for a minute: route around it, then by the step's place nearest it (a cave's entrance),
    // then give up on the step (or the trip) and say where. A route under way is judged by its current leg.
    GuideTrip& trip = info.trip;
    GuideTask const& task = info.task;
    bool routed = trip.leg < trip.legs.size() && target.GetMapId() == trip.goal.GetMapId() &&
                  trip.goal.GetExactDist(&target) <= GOAL_MOVED;
    WorldPosition goal = routed || info.detour != WorldPosition() ? task.moveTo : target;
    bool waiting = task.type == GuideTaskType::Rest || task.type == GuideTaskType::Loot ||
                   task.type == GuideTaskType::Avoid || task.type == GuideTaskType::Hearth || task.ride;
    if (goal == WorldPosition() || waiting || goal.GetMapId() != bot->GetMapId())
    {
        if (info.stuckSince)
            info.stuckSince = getMSTime();  // waiting (resting, for a transport, ...) doesn't count
        return false;
    }

    float distance = bot->GetExactDist(goal);
    if (!info.stuckSince || info.stuckGoal.GetMapId() != goal.GetMapId() ||
        info.stuckGoal.GetExactDist(&goal) > GOAL_MOVED)
    {
        // Something new to head for (a route's next leg counts): a fresh minute. The fallbacks only start over for a
        // new target.
        if (info.stuckTarget.GetMapId() != target.GetMapId() || info.stuckTarget.GetExactDist(&target) > GOAL_MOVED)
        {
            info.stuckTries = 0;
            info.stuckTarget = target;
        }
        info.stuckGoal = goal;
        info.stuckSince = getMSTime();
        info.stuckBest = distance;
        return false;
    }
    if (distance + TARGET_GAIN < info.stuckBest)
    {
        info.stuckBest = distance;
        info.stuckSince = getMSTime();
        return false;
    }
    if (GetMSTimeDiffToNow(info.stuckSince) < TARGET_STUCK_TIME)
        return false;

    std::string where =
        Acore::StringFormat("at {:.1f} {:.1f} {:.1f} map {}, target {:.1f} {:.1f} {:.1f} ({:.0f} yd)",
                            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), bot->GetMapId(),
                            goal.GetPositionX(), goal.GetPositionY(), goal.GetPositionZ(), distance);
    info.stuckSince = getMSTime();
    info.stuckBest = distance;
    GuideStep const* step = !trip.going && info.guide ? &info.guide->steps[info.step] : nullptr;

    // 1. Route around: work the route out again, or route over the stored walks
    if (info.stuckTries == 0)
    {
        ++info.stuckTries;
        if (routed || trip.going)
        {
            trip.legs.clear();
            trip.routedAt = 0;
            LOG_INFO("playerbots.guide", "{}: stuck {}; working the route out again", bot->GetName(), where);
            return false;
        }
        std::vector<GuideLeg> legs = GuideTravel::instance().FindRoute(bot, target, false, false);
        if (!legs.empty())
        {
            trip.Clear();
            trip.goal = target;
            trip.legs = std::move(legs);
            trip.routedAt = trip.legSince = getMSTime();
            LOG_INFO("playerbots.guide", "{}: stuck {}; routing round over {} stored walks", bot->GetName(), where,
                     trip.legs.size());
            return false;
        }
    }
    // 2. By the step's place nearest the target (the guides lead to a cave's entrance)
    if (info.stuckTries == 1)
    {
        ++info.stuckTries;
        WorldPosition entrance = step ? StepEntrance(info, target) : WorldPosition();
        if (entrance != WorldPosition() && bot->GetExactDist2d(entrance) > 2 * WAYPOINT_REACHED)
        {
            info.detour = entrance;
            LOG_INFO("playerbots.guide", "{}: stuck {}; by the step's place {:.1f} {:.1f} {:.1f} first", bot->GetName(),
                     where, entrance.GetPositionX(), entrance.GetPositionY(), entrance.GetPositionZ());
            return false;
        }
    }

    // 3. Give up: skip the step, or end the trip
    info.stuckTries = 0;
    info.stuckSince = 0;
    info.detour = WorldPosition();
    PlayerbotTextMgr& text = PlayerbotTextMgr::instance();
    if (trip.going)
    {
        LOG_INFO("playerbots.guide", "{}: stuck {}; giving up the trip to {}", bot->GetName(), where, trip.name);
        botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
            "guide_trip_stuck", "Guide: I can't find a way on to %place from here; stopped.", {{"%place", trip.name}}));
        bool resume = trip.resume && info.guide;
        trip.going = false;
        trip.Clear();
        if (!resume)
        {
            info.active = false;
            GuideOff(botAI);
        }
        info.lastUpdate = 0;
        return true;
    }
    if (!step)
        return false;
    LOG_INFO("playerbots.guide", "{}: stuck {}; skipping {} step {}", bot->GetName(), where, info.guide->name,
             step->number);
    botAI->TellMasterNoFacing(text.GetBotTextOrDefault("guide_target_stuck",
                                                       "Guide: I can't reach where step %step needs me; skipping it.",
                                                       {{"%step", std::to_string(step->number)}}));
    trip.Clear();
    if (!GuideRules::Advance(bot, info))
        Finish();
    info.lastUpdate = 0;
    return true;
}

std::string GuideUpdateAction::PathKind(WorldPosition const& to)
{
    if (to.GetMapId() != bot->GetMapId())
        return "other map";
    PathGenerator path(bot);
    path.CalculatePath(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ());
    G3D::Vector3 const& end = path.GetActualEndPosition();
    if (IsWholePath(path.GetPathType()) && to.GetExactDist(end.x, end.y, end.z) <= 5.0f)
        return "whole";
    if (path.GetPathType() & PATHFIND_NOPATH)
        return "none";
    return Acore::StringFormat("partial (type {:#x}), ends {:.0f} yd short at {:.1f} {:.1f} {:.1f}",
                               uint32(path.GetPathType()), to.GetExactDist(end.x, end.y, end.z), end.x, end.y, end.z);
}

void GuideUpdateAction::LogTask(GuideInfo& info)
{
    // One line per new task (not per update): what, where it is, where it heads and whether a whole path leads there
    GuideTask const& task = info.task;
    WorldPosition goal = Goal(task);
    auto round5 = [](float value) { return uint64(int64(std::floor(value / 5.0f)) & 0xFFFF); };
    uint64 key = uint64(task.type) * 1000003ull + task.questId * 31ull + task.objective + task.target.GetCounter() +
                 task.giver.GetCounter() * 7ull + (task.interact ? 11 : 0) + (task.ride ? 13 : 0) + info.step * 17ull +
                 info.trip.leg * 19ull + round5(goal.GetPositionX()) * 23ull + (round5(goal.GetPositionY()) << 20) +
                 (round5(goal.GetPositionZ()) << 40);
    if (key == info.loggedTask)
        return;
    info.loggedTask = key;
    if (!sLog->ShouldLog("playerbots.guide", LOG_LEVEL_DEBUG))
        return;

    std::string to = "-";
    if (goal != WorldPosition())
        to = Acore::StringFormat("{:.1f} {:.1f} {:.1f} map {}, path {}", goal.GetPositionX(), goal.GetPositionY(),
                                 goal.GetPositionZ(), goal.GetMapId(), PathKind(goal));
    else if (Unit* target = ObjectAccessor::GetUnit(*bot, task.target))
        to = Acore::StringFormat("{} ({:.0f} yd)", target->GetName(), bot->GetExactDist(target));
    LOG_DEBUG("playerbots.guide", "{}: {} | at {:.1f} {:.1f} {:.1f} map {} | to {}{}", bot->GetName(),
              GuideRules::Describe(bot, info), bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
              bot->GetMapId(), to, info.detour != WorldPosition() ? " (detour)" : "");
}

bool GuideUpdateAction::Reachable(GuideInfo& info, WorldPosition const& target)
{
    // Whole path to the target, checked again when it moves or after a while
    if (bot->GetExactDist(target) <= WAYPOINT_REACHED)
    {
        info.reachEnemy = false;
        return true;
    }
    if (info.reachChecked && GetMSTimeDiffToNow(info.reachChecked) < REACH_RECHECK &&
        info.reachTarget.GetMapId() == target.GetMapId() && info.reachTarget.GetExactDist(&target) <= TARGET_GAIN)
        return info.reachWhole;
    info.reachTarget = target;
    info.reachChecked = getMSTime();
    info.reachWhole = HasWholePath(target, &info.reachEnemy);
    return info.reachWhole;
}

WorldPosition GuideUpdateAction::StepEntrance(GuideInfo& info, WorldPosition const& target)
{
    // The current step's place nearest the target, within ENTRANCE_RANGE of it (the guides lead to a cave's
    // entrance, then in)
    WorldPosition entrance;
    if (info.trip.going || !info.guide)
        return entrance;
    for (GuideElement const* place : Gotos(bot, info.guide->steps[info.step], false))
    {
        WorldPosition pos = Place(*place);
        if (pos.GetMapId() == target.GetMapId() && pos.GetExactDist2d(&target) <= ENTRANCE_RANGE &&
            pos.GetExactDist2d(&target) > WAYPOINT_REACHED &&
            (entrance == WorldPosition() || pos.GetExactDist2d(&target) < entrance.GetExactDist2d(&target)))
            entrance = pos;
    }
    return entrance;
}

bool GuideUpdateAction::MakeExploreTask(uint32 questId, GuideTask& task)
{
    // An area to explore: its trigger (the guide's places give no height: in a cave, the ground they'd lead to is the
    // hill above it). Inside it, explored as the core does when a client reports the trigger. False: no trigger (a
    // scripted event): the step's places.
    AreaTrigger const* nearest = nullptr;
    for (uint32 id : GuideMgr::instance().GetQuestTriggers(questId))
        if (AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(id))
            if (trigger->map == bot->GetMapId() &&
                (!nearest || bot->GetExactDist(trigger->x, trigger->y, trigger->z) <
                                 bot->GetExactDist(nearest->x, nearest->y, nearest->z)))
                nearest = trigger;
    if (!nearest)
        return false;

    bool inside = nearest->radius > 0.0f
                      ? bot->IsWithinDist3d(nearest->x, nearest->y, nearest->z, nearest->radius)
                      : bot->GetExactDist2d(nearest->x, nearest->y) <= std::max(nearest->length, nearest->width) / 2 &&
                            std::fabs(bot->GetPositionZ() - nearest->z) <= std::max(nearest->height / 2, 5.0f);
    if (inside)
    {
        if (bot->IsAlive() && bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
            bot->AreaExploredOrEventHappens(questId);
        return true;
    }
    task.moveTo = WorldPosition(nearest->map, nearest->x, nearest->y, nearest->z);
    return true;
}

bool GuideUpdateAction::MakeEscortTask(uint32 questId, GuideTask& task)
{
    // A scripted event with no trigger, as an escort: the quest's giver walks off once it's taken. Its attackers
    // first; else kept near; not in sight: where it spawns (it walks back there after a failed escort). A failed
    // escort is abandoned and taken again (ESCORT_TRIES), then passed by. False: no giver to escort.
    GuideInfo& info = botAI->guideInfo;
    std::vector<uint32> const& givers = GuideMgr::instance().GetQuestStarters(questId);
    if (givers.empty() || !GuideMgr::instance().HasScriptedGiver(questId))
        return false;
    if (bot->GetQuestStatus(questId) == QUEST_STATUS_FAILED)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        uint32& tries = info.retake[questId];
        if (++tries > ESCORT_TRIES)
        {
            LOG_INFO("playerbots.guide", "{}: escort {} failed {} times; leaving it", bot->GetName(), questId,
                     tries - 1);
            botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "guide_escort_failed", "Guide: the escort for %quest keeps failing; leaving it.",
                {{"%quest", quest ? quest->GetTitle() : std::to_string(questId)}}));
            info.retake.erase(questId);
            AbandonQuest(questId);
            return true;
        }
        LOG_INFO("playerbots.guide", "{}: escort {} failed; taking it again (try {})", bot->GetName(), questId, tries);
        AbandonQuest(questId);
        return true;
    }

    Creature* escorted = nullptr;
    for (uint32 entry : givers)
        if (Creature* creature = bot->FindNearestCreature(entry, ESCORT_RANGE))
            if (!escorted || bot->GetExactDist(creature) < bot->GetExactDist(escorted))
                escorted = creature;
    if (!escorted)
    {
        WorldPosition spawn = NearestSpawn(givers);
        if (spawn != WorldPosition() && (spawn.GetMapId() != bot->GetMapId() || bot->GetExactDist(spawn) > 10.0f))
            task.moveTo = spawn;
        else
            task.wander = true;
        return true;
    }
    for (Unit* attacker : escorted->getAttackers())
        if (attacker->IsAlive() && bot->IsValidAttackTarget(attacker))
        {
            task.target = attacker->GetGUID();
            return true;
        }
    if (bot->GetExactDist(escorted) > ESCORT_FOLLOW)
        task.moveTo = WorldPosition(escorted);
    return true;
}

bool GuideUpdateAction::MakeLureTask(GuideStep const& step, GuideElement const& element, uint32 index,
                                     GuideUse const& use, GuideTask& task)
{
    // A trap the creature must walk into (Tharnariun's Hope: a Rabid Thistle Bear on the bear trap): placed near one,
    // then the creature pulled; the combat side ("guide lure") brings the bot back onto the trap, so the creature
    // follows it in. A trap counts for LURE_TIME; a creature caught (no longer hostile) or gone: the next.
    GuideInfo& info = botAI->guideInfo;
    std::vector<uint32> units = use.units;
    for (GuideElement const& other : step.elements)
        if ((other.type == GuideElementType::Mob || other.type == GuideElementType::Target) &&
            GuideRules::Applies(bot, other))
            units.insert(units.end(), other.entries.begin(), other.entries.end());
    auto prey = [&](Creature* creature)
    {
        return creature && creature->IsAlive() && creature->IsHostileTo(bot) &&
               std::find(units.begin(), units.end(), creature->GetEntry()) != units.end();
    };

    task = GuideTask();
    task.type = GuideTaskType::Use;
    task.item = element.id;
    task.element = index;
    // (still there: a caught creature uses it up)
    GameObject* trap = info.lureTrap != WorldPosition() && info.lureTrap.GetMapId() == bot->GetMapId()
                           ? bot->FindNearestGameObject(use.trap, bot->GetExactDist(info.lureTrap) + 5.0f)
                           : nullptr;
    bool placed = info.lureTrap != WorldPosition() && GetMSTimeDiffToNow(info.lureSince) < LURE_TIME &&
                  ((trap && trap->GetExactDist(info.lureTrap) <= 3.0f) ||
                   GetMSTimeDiffToNow(info.lureSince) < LURE_PLACING);  // (the cast may not have finished)
    Creature* target = placed ? ObjectAccessor::GetCreature(*bot, info.lurePrey) : nullptr;
    if (placed && !prey(target))
    {
        // Caught, or gone: the trap is spent or waits for another
        target = nullptr;
        for (uint32 entry : units)
            if (Creature* creature = bot->FindNearestCreature(entry, LURE_RANGE * 2))
                if (prey(creature) && creature->GetExactDist(info.lureTrap) <= LURE_RANGE * 2 &&
                    (!target || bot->GetExactDist(creature) < bot->GetExactDist(target)))
                    target = creature;
    }
    if (placed && target)
    {
        info.lurePrey = target->GetGUID();
        task.target = target->GetGUID();  // pulled: the bot attacks it, then backs onto the trap
        return true;
    }

    info.lureTrap = WorldPosition();
    if (bot->HasSpellCooldown(use.spell) || bot->IsInCombat())
        return false;
    Creature* nearest = nullptr;
    for (uint32 entry : units)
        if (Creature* creature = bot->FindNearestCreature(entry, KILL_RANGE))
            if (prey(creature) && !creature->IsInCombat() &&
                (!nearest || bot->GetExactDist(creature) < bot->GetExactDist(nearest)))
                nearest = creature;
    if (!nearest)
        return false;
    if (bot->GetExactDist(nearest) > LURE_RANGE)
    {
        task.moveTo = WorldPosition(nearest);
        return true;
    }
    // Here: placed at the bot's feet (the use casts on the bot), then the creature pulled
    task.castNow = true;
    info.lureTrap = WorldPosition(bot);
    info.lureSince = getMSTime();
    info.lurePrey = nearest->GetGUID();
    LOG_INFO("playerbots.guide", "{}: trap placed at {:.1f} {:.1f} for {}", bot->GetName(), bot->GetPositionX(),
             bot->GetPositionY(), nearest->GetName());
    return true;
}

bool GuideUpdateAction::UseForObjective(GuideStep const& step, GuideTask& task)
{
    // The step's ".use" item is how its objective is done: use it rather than (only) kill.
    for (uint32 i = 0; i < step.elements.size(); ++i)
        if (step.elements[i].type == GuideElementType::Use && GuideRules::Applies(bot, step.elements[i]) &&
            MakeUseTask(step, step.elements[i], i, task))
            return true;
    return false;
}

bool GuideUpdateAction::MakeUseTask(GuideStep const& step, GuideElement const& element, uint32 index, GuideTask& task)
{
    GuideUse const* use = GuideMgr::instance().GetUse(element.id);
    if (!use || !bot->HasItemCount(element.id))
        return false;
    if (use->mode == GuideUse::Mode::Lure)
        return MakeLureTask(step, element, index, *use, task);
    bool noSpell = use->mode == GuideUse::Mode::Equip || use->mode == GuideUse::Mode::Open;
    if ((!noSpell && bot->HasSpellCooldown(use->spell)) ||
        (use->mode == GuideUse::Mode::Equip && bot->HasItemOrGemWithIdEquipped(element.id, 1)))
        return false;

    GuideInfo& info = botAI->guideInfo;
    task = GuideTask();
    task.type = GuideTaskType::Use;
    task.item = element.id;
    task.element = index;
    if (noSpell)
    {
        task.castNow = true;
        return true;
    }

    // What it is for: its spell's targets, the objective's creature or object, the step's creatures.
    std::vector<uint32> units = use->units;
    std::vector<uint32> objects = use->objects;
    for (GuideElement const& other : step.elements)
        if ((other.type == GuideElementType::Mob || other.type == GuideElementType::Target) &&
            GuideRules::Applies(bot, other))
            units.insert(units.end(), other.entries.begin(), other.entries.end());
    // The objective's creature only when nothing else names one: it can be what the use makes of its target (a rat
    // the flute enthralls becomes the Enthralled Deeprun Rat, which follows the bot), not what to use it on
    bool named = !units.empty();
    for (GuideElement const& other : step.elements)
    {
        if (!GuideRules::Applies(bot, other) || other.type != GuideElementType::Complete)
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(other.id);
        std::vector<GuideObjective> objectives = quest ? GuideRules::Objectives(quest) : std::vector<GuideObjective>();
        if (other.objective && other.objective <= objectives.size() &&
            objectives[other.objective - 1].kind == GuideObjective::Kind::CreatureOrObject)
        {
            int32 entry = quest->RequiredNpcOrGo[objectives[other.objective - 1].slot];
            if (entry > 0 && !named)
                units.push_back(static_cast<uint32>(entry));
            else if (entry < 0)
                objects.push_back(static_cast<uint32>(-entry));
        }
    }
    task.entries = units;
    float reach = std::max(std::min(use->range, 30.0f) - 2.0f, 3.0f);
    auto nearestCreature = [&](bool alive) -> Creature*
    {
        Creature* best = nullptr;
        for (uint32 entry : units)
            if (Creature* creature = bot->FindNearestCreature(entry, KILL_RANGE, alive))
                if ((alive || !info.usedOn.count(creature->GetGUID())) &&
                    // (not one the use already took: under its spell, or charmed by the bot)
                    !(alive &&
                      (creature->HasAura(use->spell) || creature->GetCharmerOrOwnerGUID() == bot->GetGUID())) &&
                    (!best || bot->GetExactDist(creature) < bot->GetExactDist(best)))
                    best = creature;
        return best;
    };
    auto goTo = [&](WorldObject* object, float distance)
    {
        if (bot->IsWithinDistInMap(object, distance))
            task.castNow = true;
        else
            task.moveTo = WorldPosition(object);
    };

    switch (use->mode)
    {
        case GuideUse::Mode::Focus:
        {
            GameObject* focus = nullptr;
            for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest game objects no los"))
                if (GameObject* object = botAI->GetGameObject(guid))
                    if (object->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS &&
                        object->GetGOInfo()->spellFocus.focusId == use->focus &&
                        (!focus || bot->GetExactDist(object) < bot->GetExactDist(focus)))
                        focus = object;
            if (focus)
            {
                goTo(focus, 4.0f);
                return true;
            }
            break;
        }
        case GuideUse::Mode::Object:
        case GuideUse::Mode::NearbyObject:
        {
            GameObject* best = nullptr;
            for (uint32 entry : objects)
                if (GameObject* object = bot->FindNearestGameObject(entry, KILL_RANGE, true))
                    if (!best || bot->GetExactDist(object) < bot->GetExactDist(best))
                        best = object;
            if (best)
            {
                goTo(best, INTERACTION_DISTANCE);
                if (use->mode == GuideUse::Mode::Object)
                    task.object = best->GetGUID();
                return true;
            }
            break;
        }
        case GuideUse::Mode::Unit:
        case GuideUse::Mode::NearbyUnit:
        {
            if (use->corpse)
            {
                // On a body: one lying here, else make one.
                if (Creature* corpse = nearestCreature(false))
                {
                    goTo(corpse, reach);
                    if (use->mode == GuideUse::Mode::Unit)
                        task.useOn = corpse->GetGUID();
                    return true;
                }
                if (!ReadyToFight())
                {
                    task.type = GuideTaskType::Rest;
                    return true;
                }
                task.target = FindKillTarget(units, false);
                if (!task.target.IsEmpty())
                    return true;
                break;
            }

            Creature* creature = nullptr;
            if (ObjectGuid hostile = FindKillTarget(units, false); !hostile.IsEmpty())
                creature = ObjectAccessor::GetCreature(*bot, hostile);
            if (!creature)
                creature = nearestCreature(true);
            if (!creature)
                break;
            if (use->health > 0.0f)
            {
                // Wear it down first; the combat side ("guide combat") uses the item at the mark.
                info.useItem = element.id;
                info.useSpell = use->spell;
                info.useHealth = use->health;
                info.useUnits = units;
                if (!ReadyToFight())
                {
                    task.type = GuideTaskType::Rest;
                    return true;
                }
                task.target = creature->GetGUID();
                return true;
            }
            goTo(creature, reach);
            if (use->mode == GuideUse::Mode::Unit)
                task.useOn = creature->GetGUID();
            return true;
        }
        case GuideUse::Mode::Dest:
        {
            WorldPosition spot;
            if (Creature* creature = nearestCreature(true))
                spot = WorldPosition(creature);
            else if (std::vector<GuideElement const*> places = Gotos(bot, step, false); !places.empty())
                spot = Place(*places.front());
            if (spot == WorldPosition())
                break;
            if (bot->GetExactDist(spot) <= reach)
            {
                task.castNow = true;
                task.dest = spot;
            }
            else
                task.moveTo = spot;
            return true;
        }
        default:
        {
            // Where the step's place is, or by a creature it is for.
            if (Creature* creature = nearestCreature(true))
            {
                goTo(creature, 15.0f);
                return true;
            }
            std::vector<GuideElement const*> places = Gotos(bot, step, true);
            if (places.empty())
                places = Gotos(bot, step, false);
            if (places.empty())
            {
                task.castNow = true;
                return true;
            }
            // Within the place's own radius (on its ground or not: the Phial of Scrying's bowl by the Master's Glaive)
            WorldPosition place = Place(*places.front());
            if (bot->GetExactDist2d(place) <= std::max(8.0f, places.front()->radius))
                task.castNow = true;
            else
                task.moveTo = place;
            return true;
        }
    }

    // Nothing to use it on in sight: search the step's places.
    MakeAreaTask(step, task);
    return true;
}

void GuideUpdateAction::MakeCollectTask(GuideStep const& step, GuideElement const& element, uint32 index,
                                        GuideTask& task)
{
    // Until the bags hold enough: bought (from the step's own vendor, or any when nothing drops it), else from what
    // drops it or holds it and the step's ".mob"s (a quest drop only drops while on the quest). Nothing the bot can get
    // it from (the auction house, crafting, fishing): passed by. The item goes on the always-loot list: the "normal"
    // loot strategy can leave a cooking reagent before the quest is taken.
    AI_VALUE(std::set<uint32>&, "always loot list").insert(element.id);
    if (GuideRules::AtAuctionHouse(bot, step))
    {
        // Bought at the auction house: there are no auctions for the bot, so its auctioneer stands in (the items
        // are given at the auctioneer, "guide service"). No auctioneer to go to: passed by.
        std::vector<uint32> auctioneers;
        for (GuideElement const& other : step.elements)
            if (other.type == GuideElementType::Target && GuideRules::Applies(bot, other))
                for (uint32 entry : other.entries)
                    if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
                        if (creature->npcflag & UNIT_NPC_FLAG_AUCTIONEER)
                            auctioneers.push_back(entry);
        if (MakeBuyTask(step, index, auctioneers, task))
            return;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
        PassBy(index, proto ? proto->Name1 : std::to_string(element.id), "no auctioneer to get it from");
        task = GuideTask();
        return;
    }
    GuideMgr const& mgr = GuideMgr::instance();
    task.type = GuideTaskType::Kill;
    task.questId = element.quest;
    task.objective = element.objective;
    task.objects = mgr.GetItemObjects(element.id);
    task.entries = mgr.GetItemDroppers(element.id);
    for (GuideElement const& other : step.elements)
        if (other.type == GuideElementType::Mob && GuideRules::Applies(bot, other))
            task.entries.insert(task.entries.end(), other.entries.begin(), other.entries.end());

    std::vector<uint32> const& vendors = mgr.GetItemVendors(element.id);
    bool dropped = !task.entries.empty() || !task.objects.empty();
    if (!vendors.empty() && (StepSells(bot, step, vendors) || !dropped) && MakeBuyTask(step, index, vendors, task))
        return;
    if (!dropped)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
        PassBy(index, proto ? proto->Name1 : std::to_string(element.id),
               vendors.empty() ? "nothing drops, holds or sells it here (the auction house, crafting, fishing?)"
                               : "no vendor selling it near");
        task = GuideTask();
        return;
    }

    if (MakeObjectTask(task.objects, task))
        return;
    if (!ReadyToFight())
    {
        task.type = GuideTaskType::Rest;
        return;
    }
    bool questLoot = element.quest && bot->GetQuestStatus(element.quest) == QUEST_STATUS_INCOMPLETE;
    task.target = FindKillTarget(task.entries, questLoot);
    if (!task.target.IsEmpty())
        return;
    // Nothing that has it in sight, near the step, or within reach (the Elixir of Minor Fortitude's droppers are in
    // Ahn'Qiraj): passed by
    std::vector<GuideElement const*> gotos = Gotos(bot, step, false);
    if (SpawnPlaces(task.entries, gotos, task.objects, AREA_SPAWN_FALLBACK).empty() &&
        FarSpawn(task.entries, task.objects) == WorldPosition())
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
        PassBy(index, proto ? proto->Name1 : std::to_string(element.id), "nothing that drops it is within reach");
        task = GuideTask();
        return;
    }
    MakeAreaTask(step, task);
}

WorldPosition GuideUpdateAction::FarSpawn(std::vector<uint32> const& entries, std::vector<uint32> const& objects)
{
    // The nearest spawn on the bot's map within FAR_SPAWN, else the first one further off, there or on another map of
    // the open world (not an instance). Further off counts alike on either map: else the bot sails to a spawn across
    // the sea, finds it too far once there and sails back (Malgin Barleybrew in Dustwallow, from Menethil).
    WorldPosition best;
    auto consider = [&](GuideSpawn const& spawn)
    {
        WorldPosition pos(spawn.mapId, spawn.x, spawn.y, spawn.z);
        bool here = spawn.mapId == bot->GetMapId() && bot->GetExactDist(pos) <= FAR_SPAWN;
        if (spawn.mapId != bot->GetMapId())
        {
            MapEntry const* map = sMapStore.LookupEntry(spawn.mapId);
            if (!map || map->Instanceable())
                return;
        }
        bool bestHere =
            best != WorldPosition() && best.GetMapId() == bot->GetMapId() && bot->GetExactDist(best) <= FAR_SPAWN;
        if (best == WorldPosition() || (here && !bestHere) ||
            (here && bestHere && bot->GetExactDist(pos) < bot->GetExactDist(best)))
            best = pos;
    };
    for (uint32 entry : entries)
        for (GuideSpawn const& spawn : GuideMgr::instance().GetSpawns(entry))
            consider(spawn);
    for (uint32 entry : objects)
        for (GuideSpawn const& spawn : GuideMgr::instance().GetObjectSpawns(entry))
            consider(spawn);
    return best;
}

bool GuideUpdateAction::MakeBuyTask(GuideStep const& step, uint32 index, std::vector<uint32> const& vendors,
                                    GuideTask& task)
{
    // A vendor that sells it: in sight (bought by "guide service"), else where one spawns by the step's places, else
    // the nearest on this map within BUY_RANGE. False: none.
    if (index >= 32)
        return false;  // an earlier step's collect: its own step buys it
    GuideTask buy;
    buy.type = GuideTaskType::Service;
    buy.element = index;
    Creature* vendor = nullptr;
    for (uint32 entry : vendors)
        if (Creature* creature = bot->FindNearestCreature(entry, GIVER_RANGE))
            if (!vendor || bot->GetExactDist(creature) < bot->GetExactDist(vendor))
                vendor = creature;
    if (vendor)
    {
        buy.giver = vendor->GetGUID();
        buy.interact = bot->IsWithinDistInMap(vendor, INTERACTION_DISTANCE);
        task = buy;
        return true;
    }

    GuideInfo& info = botAI->guideInfo;
    auto searched = [&](WorldPosition const& pos)
    {
        return std::any_of(info.serviceSearched.begin(), info.serviceSearched.end(),
                           [&](WorldPosition const& seen) { return seen.GetExactDist(&pos) <= 5.0f; });
    };
    std::vector<WorldPosition> spawns = SpawnPlaces(vendors, Gotos(bot, step, false));
    if (spawns.empty())
        for (uint32 entry : vendors)
            for (GuideSpawn const& spawn : GuideMgr::instance().GetSpawns(entry))
                if (spawn.mapId == bot->GetMapId() && bot->GetExactDist2d(spawn.x, spawn.y) <= BUY_RANGE)
                    spawns.emplace_back(spawn.mapId, spawn.x, spawn.y, spawn.z);
    std::sort(spawns.begin(), spawns.end(), [&](WorldPosition const& a, WorldPosition const& b)
              { return bot->GetExactDist(a) < bot->GetExactDist(b); });
    for (WorldPosition const& spawn : spawns)
    {
        if (searched(spawn))
            continue;
        if (bot->GetExactDist(spawn) <= 5.0f)
        {
            info.serviceSearched.push_back(spawn);  // there, and nobody: not again
            continue;
        }
        buy.moveTo = spawn;
        task = buy;
        return true;
    }
    return false;
}

void GuideUpdateAction::PassBy(uint32 index, std::string const& what, std::string const& why)
{
    // The step's element given up (once): the rest of the step goes on
    GuideInfo& info = botAI->guideInfo;
    if (index >= 32 || (info.skippedWork & (1u << index)))
        return;
    info.skippedWork |= 1u << index;
    LOG_INFO("playerbots.guide", "{}: passes by {} on {} step {}: {}", bot->GetName(), what, info.guide->name,
             info.guide->steps[info.step].number, why);
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_passed_by", "Guide: I can't get %what (%why); passing it by.", {{"%what", what}, {"%why", why}}));
}

bool GuideUpdateAction::FinishQuestTask(uint32 questId, GuideTask& task)
{
    // The nearest earlier step of this guide that works on the quest and isn't done.
    GuideInfo& info = botAI->guideInfo;
    for (int32 i = static_cast<int32>(info.step); i >= 0; --i)
    {
        GuideStep const& step = info.guide->steps[i];
        if (!step.condition.Holds(bot))
            continue;
        for (GuideElement const& element : step.elements)
        {
            if (!GuideRules::Applies(bot, element) || GuideRules::ElementDone(bot, element))
                continue;
            if (element.type == GuideElementType::Complete && element.id == questId)
            {
                if (!UseForObjective(step, task))
                    MakeKillTask(step, element, 32, task);
                return true;
            }
            if (element.type == GuideElementType::Collect && element.quest == questId)
            {
                if (!UseForObjective(step, task))
                    MakeCollectTask(step, element, 32, task);
                return true;
            }
        }
    }
    if (!info.toldUnfinished)
    {
        info.toldUnfinished = true;
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "guide_quest_unfinished",
            "Guide: %quest isn't finished and the guide has no step for what's missing. Finish it, or 'guide skip'.",
            {{"%quest", quest ? quest->GetTitle() : std::to_string(questId)}}));
    }
    return false;
}

void GuideUpdateAction::MakeAreaTask(GuideStep const& step, GuideTask& task)
{
    // Search the places in turn (where the step's creatures spawn near its waypoints, else the waypoints); at the
    // only one, look around it.
    std::vector<GuideElement const*> gotos = Gotos(bot, step, false);
    std::vector<WorldPosition> places = SpawnPlaces(task.entries, gotos, task.objects);
    if (places.empty() && !gotos.empty())
        places = SpawnPlaces(task.entries, gotos, task.objects, AREA_SPAWN_FALLBACK);
    if (places.empty())
        if (WorldPosition far = FarSpawn(task.entries, task.objects); far != WorldPosition())
        {
            // Nowhere near the step's waypoints (the tram's rats, on the tram's own map): the nearest, routed. There:
            // look around it (not off to the step's waypoints, which brought it back from the first few yards).
            if (far.GetMapId() != bot->GetMapId() || bot->GetExactDist(far) > AREA_REACHED)
                task.moveTo = far;
            else
                task.wander = true;
            return;
        }
    if (places.empty())
        for (GuideElement const* element : gotos)
            places.push_back(Place(*element));
    if (places.empty())
    {
        task.wander = true;
        return;
    }

    GuideInfo& info = botAI->guideInfo;
    WorldPosition place = places[info.area % places.size()];
    if (bot->GetExactDist(place) > AREA_REACHED)
    {
        task.moveTo = place;
        return;
    }
    if (places.size() > 1)
    {
        info.area = (info.area + 1) % places.size();
        task.moveTo = places[info.area];
    }
    else
        task.wander = true;
}

std::vector<WorldPosition> GuideUpdateAction::SpawnPlaces(std::vector<uint32> const& entries,
                                                          std::vector<GuideElement const*> const& gotos,
                                                          std::vector<uint32> const& objects, float radius)
{
    // The spawns near the step's waypoints (or, without any, near where the step began), nearest the first waypoint
    // first, one per few yards. Real positions, heights included: the waypoints have none, and the ground there can
    // be the hill over a cave.
    std::vector<WorldPosition> places;
    WorldPosition& anchor = botAI->guideInfo.areaAnchor;
    if (gotos.empty() && (anchor == WorldPosition() || anchor.GetMapId() != bot->GetMapId()))
        anchor = WorldPosition(bot);
    if (gotos.empty())
        radius = std::max(radius, AREA_ANCHOR_RADIUS);
    auto near = [&](GuideSpawn const& spawn)
    {
        if (spawn.mapId != bot->GetMapId())
            return false;
        if (gotos.empty())
            return std::hypot(anchor.GetPositionX() - spawn.x, anchor.GetPositionY() - spawn.y) <= radius;
        for (GuideElement const* element : gotos)
            if (element->mapId == spawn.mapId && std::hypot(element->x - spawn.x, element->y - spawn.y) <= radius)
                return true;
        return false;
    };
    auto add = [&](std::vector<GuideSpawn> const& spawns)
    {
        for (GuideSpawn const& spawn : spawns)
        {
            if (!near(spawn))
                continue;
            WorldPosition place(spawn.mapId, spawn.x, spawn.y, spawn.z);
            bool close = std::any_of(places.begin(), places.end(), [&](WorldPosition const& other)
                                     { return other.GetExactDist(&place) < AREA_SPAWN_SPACING; });
            if (!close)
                places.push_back(place);
        }
    };
    for (uint32 entry : entries)
        add(GuideMgr::instance().GetSpawns(entry));
    for (uint32 entry : objects)
        add(GuideMgr::instance().GetObjectSpawns(entry));

    float fromX = gotos.empty() ? anchor.GetPositionX() : gotos.front()->x;
    float fromY = gotos.empty() ? anchor.GetPositionY() : gotos.front()->y;
    std::sort(places.begin(), places.end(),
              [&](WorldPosition const& a, WorldPosition const& b)
              {
                  return std::hypot(a.GetPositionX() - fromX, a.GetPositionY() - fromY) <
                         std::hypot(b.GetPositionX() - fromX, b.GetPositionY() - fromY);
              });
    // Where the step began holds hardly any (a side step finished on its own, far from its creatures: one Young
    // Pridewing by Sun Rock, the Pridewing Wyverns 800 yd off): the nearest AREA_MIN_PLACES within FAR_SPAWN
    if (gotos.empty() && places.size() < AREA_MIN_PLACES && radius < FAR_SPAWN)
    {
        places = SpawnPlaces(entries, gotos, objects, FAR_SPAWN);
        if (places.size() > AREA_MIN_PLACES)
            places.resize(AREA_MIN_PLACES);
    }
    return places;
}

void GuideUpdateAction::MakeTravelTask(GuideStep const& step, GuideTask& task)
{
    GuideInfo& info = botAI->guideInfo;
    std::vector<GuideElement const*> places = Gotos(bot, step, true);
    while (info.reached < places.size() && bot->GetMapId() == places[info.reached]->mapId &&
           bot->GetExactDist2d(places[info.reached]->x, places[info.reached]->y) <= places[info.reached]->radius)
        ++info.reached;
    if (info.reached >= places.size())
        return;

    task.type = GuideTaskType::Travel;
    task.moveTo = Place(*places[info.reached]);
}

bool GuideUpdateAction::UpdateTrip(GuideInfo& info)
{
    // "guide go": the route waits until the bot is there.
    GuideTrip& trip = info.trip;
    info.task = GuideTask();
    if (bot->GetMapId() == trip.place.GetMapId() && bot->GetExactDist2d(trip.place) <= ARRIVED && !bot->GetTransport())
    {
        botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "guide_arrived", "Guide: arrived at %place.", {{"%place", trip.name}}));
        bool resume = trip.resume && info.guide;
        trip.going = false;
        trip.Clear();
        if (resume)
        {
            info.stepStart = getMSTime();
            info.lastUpdate = 0;
            return true;
        }
        info.active = false;
        GuideOff(botAI);
        return true;
    }

    info.task.type = GuideTaskType::Travel;
    info.task.moveTo = trip.place;
    if (!bot->IsInCombat())
    {
        Route(info);
        if (CheckTargetStuck(info, trip.place))
            return true;
    }
    else if (info.stuckSince)
        info.stuckSince = getMSTime();
    LogTask(info);
    return false;
}

void GuideUpdateAction::Route(GuideInfo& info)
{
    // A far place (another map, or far off on this one) is reached by the trip's route: walks, portals, transports,
    // flights and, for "guide go", the hearthstone. So is a near one with no whole path to it (the core's partial
    // path would head straight at it: up the hill over a cave), over the stored walks; with no route, by the step's
    // place nearest it (a cave's entrance). Else it's walked to.
    GuideTrip& trip = info.trip;
    GuideTask& task = info.task;
    WorldPosition const target = task.moveTo;
    if (target == WorldPosition() || task.type == GuideTaskType::Avoid)
        return;

    bool sameGoal = trip.goal.GetMapId() == target.GetMapId() && trip.goal.GetExactDist(&target) <= GOAL_MOVED;
    if (!sameGoal || trip.leg >= trip.legs.size())
    {
        bool far = target.GetMapId() != bot->GetMapId() ||
                   bot->GetExactDist2d(target) > (trip.going ? TRIP_ROUTE_MIN : STEP_ROUTE_MIN);
        // Cut off: no whole path, or one past the other faction's creatures with the target itself not among them
        bool cutOff = !far && (!Reachable(info, target) ||
                               (info.reachEnemy && !GuideMgr::instance().EnemyNear(
                                                       bot->GetTeamId(), target.GetMapId(), target.GetPositionX(),
                                                       target.GetPositionY(), ENEMY_RANGE)));
        // Worked out (or failed) lately for this place: walk
        if ((!far && !cutOff) || (sameGoal && trip.routedAt && GetMSTimeDiffToNow(trip.routedAt) < REROUTE_WAIT))
            return;

        trip.Clear();
        trip.goal = target;
        trip.routedAt = getMSTime();
        trip.legSince = trip.routedAt;
        trip.legs = GuideTravel::instance().FindRoute(bot, target, far && trip.going && !trip.noHearth,
                                                      far && !trip.noFly, !trip.noHearth);
        // A far step with no way there (Moonglade's valley has no stored walk out): with the hearthstone too
        if (trip.legs.empty() && far && !trip.going && !trip.noHearth)
            trip.legs = GuideTravel::instance().FindRoute(bot, target, true, !trip.noFly, true);
        LOG_DEBUG("playerbots.guide", "{}: {} {:.1f} {:.1f} {:.1f}: {} legs", bot->GetName(),
                  far ? "far place" : "no whole path to", target.GetPositionX(), target.GetPositionY(),
                  target.GetPositionZ(), trip.legs.size());
        if (!trip.legs.empty())
        {
            std::string legs;
            for (GuideLeg const& leg : trip.legs)
                legs += Acore::StringFormat(" > {}{:.0f} {:.0f}{}", leg.kind == GuideLeg::Kind::Walk ? "" : "ride ",
                                            leg.to.GetPositionX(), leg.to.GetPositionY(),
                                            leg.place.empty() ? "" : " (" + leg.place + ")");
            LOG_INFO("playerbots.guide", "{}: route to {:.0f} {:.0f}:{}", bot->GetName(), target.GetPositionX(),
                     target.GetPositionY(), legs);
        }
        // Only the walk straight there (no stored place or road on the way): no better than none, so a cave's entrance
        // comes first all the same
        if (trip.legs.size() == 1 && trip.legs[0].kind == GuideLeg::Kind::Walk && trip.legs[0].place.empty() &&
            trip.legs[0].to.GetExactDist(&target) < 1.0f)
            trip.legs.clear();
        if (trip.legs.empty() && cutOff && !trip.going)
        {
            WorldPosition entrance = StepEntrance(info, target);
            if (entrance != WorldPosition() && bot->GetExactDist2d(entrance) > 2 * WAYPOINT_REACHED)
            {
                info.detour = entrance;
                LOG_INFO("playerbots.guide",
                         "{}: no way to {:.1f} {:.1f} {:.1f}; by the step's place {:.1f} {:.1f} first", bot->GetName(),
                         target.GetPositionX(), target.GetPositionY(), target.GetPositionZ(), entrance.GetPositionX(),
                         entrance.GetPositionY());
            }
        }
        if (trip.going)
        {
            PlayerbotTextMgr& text = PlayerbotTextMgr::instance();
            if (trip.legs.empty())
                botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
                    "guide_no_route", "Guide: I know no way to %place; walking straight there.",
                    {{"%place", trip.name}}));
            else
                botAI->TellMasterNoFacing(
                    text.GetBotTextOrDefault("guide_route", "Guide: to %place: %route.",
                                             {{"%place", trip.name}, {"%route", RouteText(trip.legs)}}));
        }
        if (trip.legs.empty())
            return;
    }

    // A portal's trigger can take the bot through before the walk to it is done (the Rut'theran portal's is wider
    // than WAYPOINT_REACHED): through it, both are
    auto through = [&](uint32 index)
    {
        return index + 1 < trip.legs.size() && trip.legs[index].kind == GuideLeg::Kind::Walk &&
               trip.legs[index + 1].kind == GuideLeg::Kind::Portal && LegDone(trip, index + 1);
    };
    while (trip.leg < trip.legs.size() && (LegDone(trip, trip.leg) || through(trip.leg)))
    {
        ++trip.leg;
        trip.legSince = getMSTime();
        trip.enteredAt = 0;
    }
    if (trip.leg >= trip.legs.size())
        return;  // the rest is a plain walk

    // Taken off the route (died, teleported), or stuck on a leg: worked out again on the next update.
    GuideLeg const& leg = trip.legs[trip.leg];
    bool onRoute = leg.kind == GuideLeg::Kind::Hearth || bot->GetMapId() == leg.to.GetMapId() ||
                   bot->GetMapId() == leg.from.GetMapId();
    if (onRoute && leg.kind == GuideLeg::Kind::Walk && !bot->GetTransport())
        onRoute = bot->GetExactDist2d(leg.to) <= OFF_ROUTE;
    if (!onRoute || GetMSTimeDiffToNow(trip.legSince) > LegTimeout(leg))
    {
        if (leg.kind == GuideLeg::Kind::Fly || leg.kind == GuideLeg::Kind::Hearth)
            LOG_INFO("playerbots.guide", "{}: {} leg {}; on without it", bot->GetName(),
                     leg.kind == GuideLeg::Kind::Fly ? "flight" : "hearthstone", onRoute ? "timed out" : "left");
        trip.noHearth |= leg.kind == GuideLeg::Kind::Hearth;
        trip.noFly |= leg.kind == GuideLeg::Kind::Fly;
        trip.legs.clear();
        trip.routedAt = 0;
        return;
    }
    LegTask(trip, leg, task);
}

bool GuideUpdateAction::LegDone(GuideTrip const& trip, uint32 index)
{
    GuideLeg const& leg = trip.legs[index];
    bool onMap = bot->GetMapId() == leg.to.GetMapId();
    bool level = std::fabs(bot->GetPositionZ() - leg.to.GetPositionZ()) <= 15.0f;
    switch (leg.kind)
    {
        case GuideLeg::Kind::Walk:
        {
            // The spot a transport is waited at, exactly (a step further is a fall in the tram's pit)
            // (or near it, stopped: as near as the paths get)
            bool waits = index + 1 < trip.legs.size() && trip.legs[index + 1].kind == GuideLeg::Kind::Transport;
            float distance = bot->GetExactDist2d(leg.to);
            if (!waits)
                return onMap && level && distance <= WAYPOINT_REACHED;
            return onMap && level && (distance <= WAIT_REACHED || (distance <= 2 * WAIT_REACHED && !bot->isMoving()));
        }
        case GuideLeg::Kind::Portal:
            return onMap && bot->GetExactDist(leg.to) <= 50.0f;
        case GuideLeg::Kind::Transport:
        {
            // Put at the far edge; with none known by the far stop, at the stop itself (in the water by a pier, in the
            // air by a zeppelin tower): on the nearest ground there first
            if (bot->GetMapId() != leg.land.GetMapId() || bot->GetExactDist(leg.land) > 5.0f)
                return false;
            // Put there, not carried: off any ship it was taken aboard of by the stop
            if (Transport* transport = bot->GetTransport())
                transport->RemovePassenger(bot, true);
            WorldPosition ashore;
            WorldPosition const& next = index + 1 < trip.legs.size() ? trip.legs[index + 1].to : trip.goal;
            if (leg.landAtStop && !bot->IsBeingTeleported() && FindAshore(bot, leg.land, next, ashore))
            {
                LOG_INFO("playerbots.guide",
                         "{}: off the ride at its stop {:.1f} {:.1f}; on ground at {:.1f} {:.1f} {:.1f}",
                         bot->GetName(), leg.land.GetPositionX(), leg.land.GetPositionY(), ashore.GetPositionX(),
                         ashore.GetPositionY(), ashore.GetPositionZ());
                bot->NearTeleportTo(ashore.GetPositionX(), ashore.GetPositionY(), ashore.GetPositionZ(),
                                    bot->GetOrientation());
            }
            return true;
        }
        case GuideLeg::Kind::Fly:
            return onMap && !bot->IsInFlight() && bot->GetExactDist2d(leg.to) <= 80.0f;
        case GuideLeg::Kind::Hearth:
            // A teleport spell: where it leads; the hearthstone: home
            return leg.entry ? onMap && bot->GetExactDist2d(leg.to) <= HOME_REACHED : NearHome(bot);
    }
    return true;
}

uint32 GuideUpdateAction::LegTimeout(GuideLeg const& leg)
{
    switch (leg.kind)
    {
        case GuideLeg::Kind::Walk:
            return 5 * MINUTE * IN_MILLISECONDS;
        case GuideLeg::Kind::Transport:
            return 15 * MINUTE * IN_MILLISECONDS;  // a boat can be a while
        case GuideLeg::Kind::Fly:
            return 3 * MINUTE * IN_MILLISECONDS;  // to start it; the flight itself doesn't count
        default:
            return MINUTE * IN_MILLISECONDS;
    }
}

void GuideUpdateAction::LegTask(GuideTrip& trip, GuideLeg const& leg, GuideTask& task)
{
    task.moveTo = WorldPosition();
    task.giver = ObjectGuid::Empty;
    task.object = ObjectGuid::Empty;
    task.interact = task.wander = task.ride = false;
    switch (leg.kind)
    {
        case GuideLeg::Kind::Walk:
            task.moveTo = leg.to;
            break;
        case GuideLeg::Kind::Portal:
        {
            AreaTrigger const* trigger = sObjectMgr->GetAreaTrigger(leg.entry);
            float radius = trigger ? std::max({trigger->radius, trigger->length / 2, trigger->width / 2, 2.0f}) : 3.0f;
            if (bot->GetMapId() == leg.from.GetMapId() && bot->GetExactDist(leg.from) <= radius)
                task.ride = true;
            else
                task.moveTo = leg.from;
            break;
        }
        case GuideLeg::Kind::Transport:
            // At the stop: put at the far side at once (bots can't get on: transports aren't in the navmesh), as for
            // a death skip; waiting for the boat or zeppelin to be seen docking and riding it out went wrong too often
            task.ride = true;
            break;
        case GuideLeg::Kind::Fly:
        {
            Creature* master = nullptr;
            for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest npcs"))
                if (Creature* npc = botAI->GetCreature(guid))
                    if (npc->IsTaxi() && npc->GetExactDist(&leg.from) <= 40.0f &&
                        (!master || bot->GetExactDist(npc) < bot->GetExactDist(master)))
                        master = npc;
            if (master && bot->IsWithinDistInMap(master, INTERACTION_DISTANCE))
            {
                task.ride = true;
                task.object = master->GetGUID();
            }
            else if (master)
                task.giver = master->GetGUID();
            else if (bot->GetExactDist(leg.from) <= WAYPOINT_REACHED)
            {
                // Nobody to fly with here: without flights
                LOG_INFO("playerbots.guide",
                         "{}: no flight master in sight at {:.1f} {:.1f} {:.1f}; on without flights", bot->GetName(),
                         leg.from.GetPositionX(), leg.from.GetPositionY(), leg.from.GetPositionZ());
                trip.noFly = true;
                trip.legs.clear();
                trip.routedAt = 0;
            }
            else
                task.moveTo = leg.from;
            break;
        }
        case GuideLeg::Kind::Hearth:
            task.ride = true;
            break;
    }
}

WorldPosition GuideUpdateAction::Place(GuideElement const& element)
{
    // The guides give no height. Of the floors there (buildings' floors over the land, the ground or water, then
    // caves, cellars and lower levels under it), the one the route's creatures nearby stand on; with none, the one
    // nearest the bot's height when it is near (a city's street), else the top one (a cave's place is the hill over
    // it).
    if (auto cached = _places.find(&element); cached != _places.end())
        return cached->second;
    if (bot->GetMapId() != element.mapId)
        return WorldPosition(element.mapId, element.x, element.y, bot->GetPositionZ());

    Map* map = bot->GetMap();
    std::vector<float> floors;
    // (the buildings' floors are only found within the height search of the start: from over the land, not the sky)
    float land = map->GetHeight(element.x, element.y, MAX_HEIGHT, false);
    float z = map->GetHeight(element.x, element.y, land + FLOOR_SKY, true, FLOOR_SKY + FLOOR_GAP);
    z = std::max(std::max(z, land), map->GetWaterLevel(element.x, element.y));
    while (z > INVALID_HEIGHT && floors.size() < MAX_FLOORS)
    {
        floors.push_back(z);
        float below = map->GetHeight(element.x, element.y, z - FLOOR_GAP, true, FLOOR_SEARCH);
        if (below <= INVALID_HEIGHT || below >= z - 1.0f)
            break;
        z = below;
    }
    if (floors.empty())
        return WorldPosition(element.mapId, element.x, element.y, bot->GetPositionZ());  // not loaded yet: not kept

    float floor = floors.front();
    std::vector<float> heights =
        GuideMgr::instance().SpawnHeightsNear(element.mapId, element.x, element.y, FLOOR_HINT_RANGE);
    if (floors.size() > 1 && !heights.empty())
    {
        std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
        float hint = heights[heights.size() / 2];
        for (float level : floors)
            if (std::fabs(level - hint) < std::fabs(floor - hint))
                floor = level;
        if (std::fabs(floor - hint) > FLOOR_HINT_MATCH)
            floor = floors.front();
    }
    else if (floors.size() > 1)
    {
        // By where the bot is now: worked out again each time (not kept)
        if (bot->GetExactDist2d(element.x, element.y) <= FLOOR_BOT_RANGE)
            for (float level : floors)
                if (std::fabs(level - bot->GetPositionZ()) < std::fabs(floor - bot->GetPositionZ()))
                    floor = level;
        return WorldPosition(element.mapId, element.x, element.y, floor);
    }
    WorldPosition place(element.mapId, element.x, element.y, floor);
    _places[&element] = place;
    return place;
}

void GuideUpdateAction::SideWork(GuideInfo& info)
{
    // Active side steps ("#completewith", from the step after them through the step they name): their hearthstone
    // first; their kills and collecting whenever a safe target is close, on the way to whatever the step needs, and
    // the objects holding their items (bundles, boxes) when one is close.
    GuideTask& task = info.task;
    if (task.type == GuideTaskType::Rest || task.type == GuideTaskType::Hearth || (task.giver && task.interact))
        return;

    std::vector<uint32> entries;
    std::vector<uint32> objects;
    bool questLoot = false;
    for (uint32 i = 0; i < info.step; ++i)
    {
        GuideStep const& step = info.guide->steps[i];
        if (!step.side || step.sideUntil < info.step || info.sideDone.count(i) || !GuideRules::StepApplies(bot, step))
            continue;

        bool open = false;
        std::vector<uint32> stepEntries;
        for (uint32 j = 0; j < step.elements.size(); ++j)
        {
            GuideElement const& element = step.elements[j];
            if (!GuideRules::Applies(bot, element))
                continue;
            if (element.type == GuideElementType::Hearth && !NearHome(bot))
            {
                task = GuideTask();
                task.type = GuideTaskType::Hearth;
                task.sideStep = static_cast<int32>(i);
                task.element = j;
                return;
            }
            // Binding at the town's inn before leaving it (Menethil, Auberdine): done once the hearthstone is bound
            // by the step's place; while in town, the innkeeper is visited first. Else the hearth further on goes to
            // the old inn (Astranaar, from Wetlands).
            if (element.type == GuideElementType::Home)
            {
                std::vector<GuideElement const*> places = Gotos(bot, step, true);
                if (places.empty())
                    continue;
                GuideElement const& inn = *places.front();
                if (bot->m_homebindMapId == inn.mapId &&
                    std::hypot(bot->m_homebindX - inn.x, bot->m_homebindY - inn.y) <= HOME_REACHED)
                    continue;  // bound there
                if (bot->GetMapId() != inn.mapId || bot->GetExactDist2d(inn.x, inn.y) > SERVICE_RANGE)
                {
                    open = true;
                    continue;
                }
                uint32 const serviced = info.serviced;  // the pass-by marks the current step's element
                GuideTask home;
                MakeServiceTask(step, element, j, home);
                info.serviced = serviced;
                if (home.type == GuideTaskType::None)
                    continue;  // no innkeeper there: passed by
                task = home;
                task.sideStep = static_cast<int32>(i);
                return;
            }
            if ((element.type == GuideElementType::Complete || element.type == GuideElementType::Collect) &&
                !GuideRules::ElementDone(bot, element))
            {
                open = true;
                bool complete = element.type == GuideElementType::Complete;
                uint32 questId = complete ? element.id : element.quest;
                questLoot |= questId && bot->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE;
                uint32 item = 0;
                if (!complete)
                {
                    AI_VALUE(std::set<uint32>&, "always loot list").insert(element.id);
                    item = element.id;
                }
                else if (Quest const* quest = sObjectMgr->GetQuestTemplate(element.id))
                {
                    std::vector<GuideObjective> objectives = GuideRules::Objectives(quest);
                    if (element.objective && element.objective <= objectives.size())
                    {
                        GuideObjective const& objective = objectives[element.objective - 1];
                        if (objective.kind == GuideObjective::Kind::CreatureOrObject &&
                            quest->RequiredNpcOrGo[objective.slot] > 0)
                            stepEntries.push_back(static_cast<uint32>(quest->RequiredNpcOrGo[objective.slot]));
                        if (objective.kind == GuideObjective::Kind::Item)
                            item = quest->RequiredItemId[objective.slot];
                    }
                }
                if (item)
                {
                    std::vector<uint32> const& holders = GuideMgr::instance().GetItemObjects(item);
                    objects.insert(objects.end(), holders.begin(), holders.end());
                }
            }
            if (element.type == GuideElementType::Mob)
                stepEntries.insert(stepEntries.end(), element.entries.begin(), element.entries.end());
        }
        if (!open)
            info.sideDone.insert(i);
        else
        {
            std::set<uint32> const finished = GuideRules::FinishedKills(bot, step);
            for (uint32 entry : stepEntries)
                if (!finished.count(entry))
                    entries.push_back(entry);
        }
    }

    if ((!entries.empty() || questLoot) && task.target.IsEmpty() && ReadyToFight())
        task.target = FindKillTarget(entries, questLoot, SIDE_RANGE);

    GuideTask side;
    if (!objects.empty() && task.target.IsEmpty() && task.object.IsEmpty() && !bot->IsInCombat() &&
        MakeObjectTask(objects, side, SIDE_RANGE))
    {
        task.object = side.object;
        task.open = side.open;
        task.moveTo = side.moveTo;
        task.interact = false;
    }
}

void GuideUpdateAction::WaitForLoot(GuideInfo& info)
{
    // The bot's own kills close by still hold something it would take: stay while the loot strategy loots them (it
    // only adds corpses on a random tick, and the next pull would leave them behind). Given up per corpse after a
    // while.
    if (info.lootGivenUp.size() > 64)
        info.lootGivenUp.clear();

    Creature* corpse = nullptr;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "nearest corpses"))
    {
        Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
        if (!creature || creature->IsAlive() || !creature->isTappedBy(bot) || info.lootGivenUp.count(guid) ||
            bot->GetExactDist(creature) > LOOT_RANGE || !HasLootFor(creature))
            continue;
        if (!corpse || bot->GetExactDist(creature) < bot->GetExactDist(corpse))
            corpse = creature;
    }
    if (!corpse)
    {
        info.lootWait.Clear();
        return;
    }

    std::vector<uint8> needed = NeededLootSlots(corpse);
    uint32 left = LootLeft(corpse);
    if (info.lootWait != corpse->GetGUID())
    {
        info.lootWait = corpse->GetGUID();
        info.lootWaitSince = getMSTime();
        info.lootWaitLeft = left;
        botAI->DoSpecificAction("add all loot", Event(), true);
    }
    if (needed.empty() && (left < info.lootWaitLeft || AnyLooted(corpse)))
    {
        // The loot strategy has been at it (it loots a corpse once): what it left there, it doesn't want
        info.lootGivenUp.insert(corpse->GetGUID());
        info.lootWait.Clear();
        return;
    }
    if (GetMSTimeDiffToNow(info.lootWaitSince) > (needed.empty() ? LOOT_WAIT : NEEDED_LOOT_WAIT))
    {
        LOG_DEBUG("playerbots.guide", "{}: gave up looting {} ({} needed items left)", bot->GetName(),
                  corpse->GetName(), needed.size());
        info.lootGivenUp.insert(corpse->GetGUID());
        info.lootWait.Clear();
        return;
    }
    info.task = GuideTask();
    info.task.type = GuideTaskType::Loot;

    // An item a quest needs (a required item, or one to take somewhere: Samuel's Remains) isn't left to the loot
    // strategy: open the corpse and take it the way the client does.
    if (needed.empty())
        return;
    if (bot->GetLootGUID() == corpse->GetGUID())
    {
        TakeNeededLoot(corpse, needed);
        return;
    }
    if (bot->GetLootGUID() || bot->IsNonMeleeSpellCast(false))
        return;  // another loot window, or casting: next update
    if (!bot->IsWithinDistInMap(corpse, INTERACTION_DISTANCE - 1.0f))
    {
        info.task.moveTo = WorldPosition(corpse);
        return;
    }
    if (bot->isMoving())
        bot->StopMoving();
    // Opened and emptied of what's needed at once: left open until the next update, something else (the loot
    // strategy, the player's client) can close it first
    WorldPacket packet(CMSG_LOOT, 8);
    packet << corpse->GetGUID();
    bot->GetSession()->HandleLootOpcode(packet);
    if (bot->GetLootGUID() != corpse->GetGUID())
    {
        LOG_INFO("playerbots.guide", "{}: couldn't open {} ({:.1f} yd)", bot->GetName(), corpse->GetName(),
                 bot->GetExactDist(corpse));
        return;
    }
    TakeNeededLoot(corpse, needed);
}

void GuideUpdateAction::TakeNeededLoot(Creature* corpse, std::vector<uint8> const& slots)
{
    // Each needed item taken the way the client does; one left behind is logged with why (no room: room is made for
    // the next try). The window is closed again (the loot strategy comes back for the rest: an open window stops all
    // looting).
    Loot& loot = corpse->loot;
    bool noRoom = false;
    for (uint8 slot : slots)
    {
        QuestItem* questItem = nullptr;
        LootItem* item = loot.LootItemInSlot(slot, bot, &questItem);
        if (!item)
            continue;
        uint32 itemId = item->itemid;
        uint32 had = bot->GetItemCount(itemId, true);
        ItemPosCountVec dest;
        InventoryResult room = bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, item->count);
        WorldPacket packet(CMSG_AUTOSTORE_LOOT_ITEM, 1);
        packet << slot;
        bot->GetSession()->HandleAutostoreLootItemOpcode(packet);
        if (bot->GetItemCount(itemId, true) > had)
            continue;
        noRoom |= room == EQUIP_ERR_INVENTORY_FULL;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        LOG_INFO("playerbots.guide",
                 "{}: couldn't take {} from {} (slot {}, {}; bag check {}, allowed {}, window {}, {:.1f} yd)",
                 bot->GetName(), proto ? proto->Name1 : std::to_string(itemId), corpse->GetName(), slot,
                 questItem ? "quest item" : "item", uint32(room),
                 item->AllowedForPlayer(bot, corpse->GetGUID()) ? "yes" : "no",
                 bot->GetLootGUID() == corpse->GetGUID() ? "open" : "closed", bot->GetExactDist(corpse));
    }
    if (bot->GetLootGUID() == corpse->GetGUID())
    {
        WorldPacket release(CMSG_LOOT_RELEASE, 8);
        release << corpse->GetGUID();
        bot->GetSession()->HandleLootReleaseOpcode(release);
    }
    if (noRoom)
        FreeBagSpace(true);
}

std::vector<uint8> GuideUpdateAction::NeededLootSlots(Creature* corpse)
{
    // The corpse's loot slots (as the client numbers them: its items, then this character's quest items) holding an
    // item a quest in the log needs (the core's own test) or one the guide collects
    std::vector<uint8> slots;
    Loot& loot = corpse->loot;
    std::set<uint32> const& collect = AI_VALUE(std::set<uint32>&, "always loot list");
    size_t questItems = 0;
    if (auto it = loot.GetPlayerQuestItems().find(bot->GetGUID()); it != loot.GetPlayerQuestItems().end())
        questItems = it->second->size();
    for (size_t slot = 0; slot < loot.items.size() + questItems && slot < 255; ++slot)
    {
        LootItem* item = loot.LootItemInSlot(slot, bot);
        if (item && !item->is_looted && (bot->HasQuestForItem(item->itemid) || collect.count(item->itemid)))
            slots.push_back(uint8(slot));
    }
    return slots;
}

bool GuideUpdateAction::HasLootFor(Creature* corpse) { return LootLeft(corpse) || !NeededLootSlots(corpse).empty(); }

uint32 GuideUpdateAction::LootLeft(Creature* corpse)
{
    // The money and the items the loot strategy may take still on the corpse
    Loot const& loot = corpse->loot;
    uint32 left = loot.gold ? 1 : 0;
    for (std::vector<LootItem> const* list : {&loot.items, &loot.quest_items})
        for (LootItem const& item : *list)
            if (!item.is_looted && StoreLootAction::IsLootAllowed(item.itemid, botAI))
                ++left;
    return left;
}

bool GuideUpdateAction::CheckStuck(GuideInfo& info)
{
    // No progress on the step for a long while (no xp, no quest or item change, no ground gained toward where it
    // walks): say so and skip it. Resting, looting, dodging and hearthing don't count against it.
    GuideTask const& task = info.task;
    if (task.type == GuideTaskType::Rest || task.type == GuideTaskType::Loot || task.type == GuideTaskType::Avoid ||
        task.type == GuideTaskType::Hearth)
    {
        info.progressSince = 0;
        return false;
    }

    GuideStep const& step = info.guide->steps[info.step];
    uint64 mark =
        bot->GetLevel() * 1000003ull + bot->GetUInt32Value(PLAYER_XP) + info.reached * 7ull + info.serviced * 13ull;
    for (GuideElement const& element : step.elements)
    {
        uint32 questId = element.type == GuideElementType::Collect ? element.quest : element.id;
        if (element.type == GuideElementType::Accept || element.type == GuideElementType::TurnIn ||
            element.type == GuideElementType::Complete || (element.type == GuideElementType::Collect && questId))
        {
            mark = mark * 31 + bot->GetQuestStatus(questId);
            auto status = bot->getQuestStatusMap().find(questId);
            if (status != bot->getQuestStatusMap().end())
                for (uint16 count : status->second.CreatureOrGOCount)
                    mark = mark * 31 + count;
        }
        if (element.type == GuideElementType::Collect)
            mark = mark * 31 + bot->GetItemCount(element.id, true);
    }
    // On a route, ground gained toward its goal (each leg's point is only a few dozen yards on: every new leg looked
    // as far as the last, and an 8-minute walk counted as stuck)
    WorldPosition const& goal = info.trip.goal != WorldPosition() && info.trip.goal.GetMapId() == bot->GetMapId()
                                    ? info.trip.goal
                                    : task.moveTo;
    float distance = goal != WorldPosition() ? bot->GetExactDist(goal) : 0.0f;

    if (!info.progressSince || mark != info.progressMark || info.progressDistance - distance > STUCK_GAIN)
    {
        info.progressMark = mark;
        info.progressSince = getMSTime();
        info.progressDistance = distance;
        return false;
    }
    if (GetMSTimeDiffToNow(info.progressSince) < STUCK_TIME)
        return false;

    LOG_INFO("playerbots.guide", "{}: no progress on {} step {} for 8 minutes ({}); skipping it", bot->GetName(),
             info.guide->name, step.number, GuideRules::Describe(bot, info));
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_stuck", "Guide: no progress on step %step for 8 minutes; skipping it.",
        {{"%step", std::to_string(step.number)}}));
    info.progressSince = 0;
    if (!GuideRules::Advance(bot, info))
        Finish();
    info.lastUpdate = 0;
    return true;
}

void GuideUpdateAction::AvoidPatrols(GuideInfo& info)
{
    // Stepped aside already: wait there while the patrol's walk still passes by the spot it left.
    if (!info.avoidPatrol.IsEmpty())
    {
        Creature* patrol = ObjectAccessor::GetCreature(*bot, info.avoidPatrol);
        float x = info.avoidFrom.GetPositionX();
        float y = info.avoidFrom.GetPositionY();
        if (patrol && patrol->IsAlive() && !patrol->IsInCombat() && GetMSTimeDiffToNow(info.avoidSince) < PATROL_WAIT &&
            PatrolPassDistance(patrol, x, y) <= patrol->GetAggroRange(bot) + PATROL_MARGIN)
        {
            info.task = GuideTask();
            info.task.type = GuideTaskType::Avoid;
            return;
        }
        info.avoided[info.avoidPatrol] = getMSTime();
        info.avoidPatrol.Clear();
    }
    for (auto it = info.avoided.begin(); it != info.avoided.end();)
        it = GetMSTimeDiffToNow(it->second) > PATROL_AGAIN ? info.avoided.erase(it) : std::next(it);

    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets no los"))
    {
        if (guid == info.task.target)
            continue;
        Creature* patrol = ObjectAccessor::GetCreature(*bot, guid);
        if (!patrol || !patrol->IsAlive() || patrol->IsInCombat() || !patrol->HasReactState(REACT_AGGRESSIVE) ||
            patrol->GetReactionTo(bot) > REP_HOSTILE || bot->GetExactDist2d(patrol) > PATROL_WATCH ||
            std::fabs(patrol->GetPositionZ() - bot->GetPositionZ()) > PATROL_HEIGHT || info.avoided.count(guid))
            continue;
        float aggro = patrol->GetAggroRange(bot);
        if (PatrolPassDistance(patrol, bot->GetPositionX(), bot->GetPositionY()) > aggro + PATROL_MARGIN)
            continue;
        if (WorldPosition away = PatrolEscape(patrol); away != WorldPosition())
        {
            info.avoidPatrol = patrol->GetGUID();
            info.avoidFrom = WorldPosition(bot);
            info.avoidSince = getMSTime();
            info.task = GuideTask();
            info.task.type = GuideTaskType::Avoid;
            info.task.moveTo = away;
            return;
        }
    }
}

float GuideUpdateAction::PatrolPassDistance(Creature* patrol, float x, float y)
{
    // How close a patrol's walk to its next point comes to (x, y); far for anything not patrolling.
    if (patrol->GetMotionMaster()->GetCurrentMovementGeneratorType() != WAYPOINT_MOTION_TYPE ||
        patrol->movespline->Finalized())
        return FLT_MAX;

    G3D::Vector3 to = patrol->movespline->FinalDestination();
    float ax = patrol->GetPositionX();
    float ay = patrol->GetPositionY();
    float dx = to.x - ax;
    float dy = to.y - ay;
    float lengthSquared = dx * dx + dy * dy;
    float t = lengthSquared > 0.0f ? std::clamp(((x - ax) * dx + (y - ay) * dy) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
}

WorldPosition GuideUpdateAction::PatrolEscape(Creature* patrol)
{
    // The spot just outside its aggro range, straight away from its path (the other side if that one can't be walked
    // to); on the path, to its side.
    G3D::Vector3 to = patrol->movespline->FinalDestination();
    float ax = patrol->GetPositionX();
    float ay = patrol->GetPositionY();
    float dx = to.x - ax;
    float dy = to.y - ay;
    float lengthSquared = dx * dx + dy * dy;
    float t = lengthSquared > 0.0f
                  ? std::clamp(((bot->GetPositionX() - ax) * dx + (bot->GetPositionY() - ay) * dy) / lengthSquared,
                               0.0f, 1.0f)
                  : 0.0f;
    float cx = ax + t * dx;
    float cy = ay + t * dy;
    float ux = bot->GetPositionX() - cx;
    float uy = bot->GetPositionY() - cy;
    float away = std::hypot(ux, uy);
    if (away < 1.0f)
    {
        float length = std::max(std::sqrt(lengthSquared), 0.01f);
        ux = -dy / length;
        uy = dx / length;
    }
    else
    {
        ux /= away;
        uy /= away;
    }

    float distance = patrol->GetAggroRange(bot) + PATROL_CLEARANCE;
    for (float side : {1.0f, -1.0f})
    {
        float x = cx + side * ux * distance;
        float y = cy + side * uy * distance;
        float z = bot->GetMap()->GetHeight(x, y, bot->GetPositionZ() + 5.0f);
        if (z <= INVALID_HEIGHT)
            continue;
        PathGenerator path(bot);
        path.CalculatePath(x, y, z);
        if (!(path.GetPathType() & ~(PATHFIND_NORMAL | PATHFIND_INCOMPLETE)))
            return WorldPosition(bot->GetMapId(), x, y, z);
    }
    return WorldPosition();
}

bool GuideUpdateAction::ReadyToFight()
{
    // Rest once below the ready mark, then until well recovered, not just back over it.
    bool mana = bot->getPowerType() == POWER_MANA;
    float health = bot->GetHealthPct();
    float power = mana ? bot->GetPowerPct(POWER_MANA) : 100.0f;
    bool& resting = botAI->guideInfo.resting;
    if (resting)
        resting = health < RESTED_HEALTH || power < RESTED_MANA;
    else
        resting = health < READY_HEALTH || power < READY_MANA;
    return !resting;
}

ObjectGuid GuideUpdateAction::FindKillTarget(std::vector<uint32> const& entries, bool questLoot, float range)
{
    // Always a safe pull (PullSafety): one that brings nothing along first, then the fewest adds, then the nearest.
    // One with no whole path to it (in the cave under the bot, over a gorge) is left for a while.
    std::map<ObjectGuid, uint32>& unreachable = botAI->guideInfo.unreachable;
    for (auto it = unreachable.begin(); it != unreachable.end();)
        it = getMSTime() >= it->second ? unreachable.erase(it) : std::next(it);

    for (uint32 check = 0; check < KILL_PATH_CHECKS; ++check)
    {
        Unit* best = FindKillCandidate(entries, questLoot, range);
        if (!best || bot->GetExactDist(best) <= WAYPOINT_REACHED || HasWholePath(WorldPosition(best)))
            return best ? best->GetGUID() : ObjectGuid();
        unreachable[best->GetGUID()] = getMSTime() + UNREACHABLE_TIME;
        LOG_DEBUG("playerbots.guide", "{}: no whole path to {} at {:.1f} {:.1f} {:.1f}; left for now", bot->GetName(),
                  best->GetName(), best->GetPositionX(), best->GetPositionY(), best->GetPositionZ());
    }
    return ObjectGuid();
}

Unit* GuideUpdateAction::FindKillCandidate(std::vector<uint32> const& entries, bool questLoot, float range)
{
    std::map<ObjectGuid, uint32> const& unreachable = botAI->guideInfo.unreachable;
    GuidVector nearby = AI_VALUE(GuidVector, "possible targets no los");
    Unit* best = nullptr;
    uint32 bestRank = 0;
    for (ObjectGuid const& guid : AI_VALUE(GuidVector, "possible targets"))
    {
        Creature* creature = ObjectAccessor::GetCreature(*bot, guid);
        if (!creature || !creature->IsAlive() || creature->IsInEvadeMode() || unreachable.count(guid))
            continue;
        if (creature->hasLootRecipient() && !creature->isTappedBy(bot))
            continue;
        if (bot->GetExactDist(creature) > range)
            continue;

        bool wanted = std::find(entries.begin(), entries.end(), creature->GetEntry()) != entries.end();
        if (!wanted && questLoot)
            wanted = LootTemplates_Creature.HaveQuestLootForPlayer(creature->GetCreatureTemplate()->lootid, bot);
        if (!wanted)
            continue;

        PullAdds adds = PullSafety::Count(bot, creature, nearby, botAI);
        uint32 rank = PullSafety::Acceptable(bot, adds) ? 0 : 1 + adds.Total();
        if (!best || rank < bestRank || (rank == bestRank && bot->GetExactDist(creature) < bot->GetExactDist(best)))
        {
            best = creature;
            bestRank = rank;
        }
    }
    return best;
}

bool GuideMoveAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideTask const& task = botAI->guideInfo.task;
    // Out of a patrol's way now: MoveFarTo would let a walk under way (maybe into its path) finish first.
    if (task.type == GuideTaskType::Avoid)
        return MoveTo(task.moveTo.GetMapId(), task.moveTo.GetPositionX(), task.moveTo.GetPositionY(),
                      task.moveTo.GetPositionZ(), false, false, false, true, MovementPriority::MOVEMENT_FORCED);
    // MoveFarTo's own way out (a teleport to the place after 90 s without progress) is left to the guide's stuck
    // checks, which try other ways and say so.
    botAI->rpgInfo.stuckAttempts = 0;
    if (task.giver && !task.interact)
        return MoveWorldObjectTo(task.giver);
    // Not a random step when the walk can't get any nearer (it would walk back and step off again, over and over)
    if (task.moveTo != WorldPosition())
        return MoveFarTo(task.moveTo);
    if (task.wander)
        return MoveRandomNear(15.0f);
    return false;
}

bool GuideQuestAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideTask const& task = botAI->guideInfo.task;
    Quest const* quest = sObjectMgr->GetQuestTemplate(task.questId);
    if (!quest || !task.giver)
        return false;

    bool done = false;
    if (task.type == GuideTaskType::Accept && bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false))
        done = AcceptQuest(quest, task.giver);
    else if (task.type == GuideTaskType::TurnIn && bot->CanRewardQuest(quest, false))
        done = TurnInQuest(quest, task.giver);

    if (done)
    {
        botAI->guideInfo.lastUpdate = 0;  // the step may be done now
        ForceToWait(1000);
    }
    return done;
}

bool GuideServiceAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideInfo& info = botAI->guideInfo;
    GuideTask const& task = info.task;
    // A side step's bind (SideWork) is that step's element
    GuideStep const& step = info.guide->steps[task.sideStep >= 0 ? uint32(task.sideStep) : info.step];
    if (task.element >= step.elements.size())
        return false;
    // An event's gossip option or emote (its credit), not the step's element
    if (task.gossipQuest || task.emote || task.wake)
    {
        WorldObject* giver = task.giver.IsGameObject() ? static_cast<WorldObject*>(botAI->GetGameObject(task.giver))
                                                       : ObjectAccessor::GetCreature(*bot, task.giver);
        if (!giver)
            return false;
        if (task.gossipQuest)
            GossipByCredits(giver, GuideMgr::instance().GetGossipCredits(task.gossipQuest));
        else if (task.wake)
        {
            OpenGossip(giver);
            GossipMenuItemContainer const& items = bot->PlayerTalkClass->GetGossipMenu().GetMenuItems();
            if (!items.empty())
                SelectGossip(giver, items.begin()->first);
            bot->PlayerTalkClass->SendCloseGossip();
            LOG_INFO("playerbots.guide", "{}: set off {} by its gossip", bot->GetName(), giver->GetName());
        }
        else
        {
            bot->SetFacingToObject(giver);
            bot->SetTarget(giver->GetGUID());
            WorldPacket emote(CMSG_TEXT_EMOTE, 4 + 4 + 8);
            emote << uint32(task.emote) << uint32(0) << giver->GetGUID();
            bot->GetSession()->HandleTextEmoteOpcode(emote);
        }
        info.lastUpdate = 0;
        ForceToWait(3000);
        return true;
    }
    // Gossip on an object (a pylon, a tablet)
    if (task.giver.IsGameObject())
    {
        GameObject* object = botAI->GetGameObject(task.giver);
        if (!object || task.repair || step.elements[task.element].type != GuideElementType::Gossip)
            return false;
        Gossip(object, step.elements[task.element], StepGossipCredits(bot, step));
        if (task.element < 32)
            info.serviced |= 1u << task.element;
        info.lastUpdate = 0;
        ForceToWait(1000);
        return true;
    }
    Creature* npc = ObjectAccessor::GetCreature(*bot, task.giver);
    if (!npc)
        return false;

    if (task.vendor)
    {
        SellJunk(npc);
        if (npc->IsArmorer())
            Repair(npc);
        BuySupplies(npc);
        info.vendorSearched.insert(npc->GetGUID());
        info.vendorChase.Clear();
        LOG_INFO("playerbots.guide", "{}: sold junk to {} on the way", bot->GetName(), npc->GetName());
        info.lastUpdate = 0;
        ForceToWait(1000);
        return true;
    }
    if (task.repair)
    {
        Repair(npc);
        if (WornDown(bot, GuideUpdateAction::REPAIR_DURABILITY))
            info.repairSearched.push_back(WorldPosition(npc));  // too dear: not again this step
        info.lastUpdate = 0;
        ForceToWait(1000);
        return true;
    }

    GuideElement const& element = step.elements[task.element];
    if (element.type == GuideElementType::Vendor)
    {
        SellJunk(npc);
        Repair(npc);
        BuySupplies(npc);
        BuyMount(npc);
    }
    else if (element.type == GuideElementType::Gossip)
        Gossip(npc, element, StepGossipCredits(bot, step));
    else if (element.type == GuideElementType::Collect && npc->IsAuctioner())
        AuctionItem(element, task.element);
    else if (element.type == GuideElementType::Collect)
        BuyItem(npc, element, task.element);
    else if (element.type == GuideElementType::Complete && npc->GetVendorItems())
    {
        if (GuideElement const item = ObjectiveItem(element); item.id)
            BuyItem(npc, item, task.element);
    }
    else if (element.type == GuideElementType::Home)
        bot->GetSession()->SendBindPoint(npc);
    else if (element.type == GuideElementType::FlightPath)
        bot->GetSession()->SendLearnNewTaxiNode(npc);
    else if (element.type == GuideElementType::Fly)
    {
        // Talking to the flight master learns its flight point, as for a player (RestedXP's guides have no step of
        // their own for it: Sentinel Hill). Both ends known: fly; else walk (the step's places still lead there).
        bot->GetSession()->SendLearnNewTaxiNode(npc);
        TaxiNodesEntry const* to = FindTaxiNode(element.other, bot->GetTeamId());
        uint32 from = sObjectMgr->GetNearestTaxiNode(npc->GetPositionX(), npc->GetPositionY(), npc->GetPositionZ(),
                                                     npc->GetMapId(), bot->GetTeamId());
        bool known = to && from && from != to->ID && bot->m_taxi.IsTaximaskNodeKnown(from) &&
                     bot->m_taxi.IsTaximaskNodeKnown(to->ID);
        if (!known || !bot->ActivateTaxiPathTo({from, to->ID}, npc))
            LOG_INFO("playerbots.guide", "{}: can't fly from {} to {} ({}); walking", bot->GetName(), from,
                     to ? to->ID : 0, known ? "no flight between them, or no money" : "flight point unknown");
    }
    else if (Trainer::Trainer* trainer = sObjectMgr->GetTrainer(npc->GetEntry()))
    {
        // Twice: a rank learned now opens the next one. ".train": the trainer's spell that gives it.
        uint32 wanted = element.type == GuideElementType::Train ? GuideRules::TrainerSpellFor(trainer, element.id) : 0;
        for (uint32 pass = 0; pass < 2; ++pass)
            for (Trainer::Spell const& spell : trainer->GetSpells())
                if ((element.type != GuideElementType::Train || spell.SpellId == wanted) &&
                    trainer->CanTeachSpell(bot, &spell))
                    trainer->TeachSpell(npc, bot, spell.SpellId);
    }

    if (task.sideStep >= 0)
        info.sideDone.insert(static_cast<uint32>(task.sideStep));
    else if (task.element < 32)
        info.serviced |= 1u << task.element;
    info.lastUpdate = 0;
    ForceToWait(1000);
    return true;
}

void GuideServiceAction::Repair(Creature* npc)
{
    // Everything worn and in the bags, as the client's "repair all" (paid, as far as the money goes)
    if (!npc->IsArmorer())
        return;
    uint64 money = bot->GetMoney();
    bot->DurabilityRepairAll(true, bot->GetReputationPriceDiscount(npc), false);
    if (bot->GetMoney() < money)
        LOG_INFO("playerbots.guide", "{}: repaired at {} for {} copper", bot->GetName(), npc->GetName(),
                 money - bot->GetMoney());
}

void GuideServiceAction::OpenGossip(WorldObject* npc)
{
    // Talked to (an object: used) as the client does
    if (npc->IsGameObject())
    {
        if (bot->isMoving())
            bot->StopMoving();
        WorldPacket use(CMSG_GAMEOBJ_USE, 8);
        use << npc->GetGUID();
        bot->GetSession()->HandleGameObjectUseOpcode(use);
        return;
    }
    WorldPacket hello(CMSG_GOSSIP_HELLO, 8);
    hello << npc->GetGUID();
    bot->GetSession()->HandleGossipHelloOpcode(hello);
}

void GuideServiceAction::SelectGossip(WorldObject* npc, uint32 listId)
{
    WorldPacket select(CMSG_GOSSIP_SELECT_OPTION, 8 + 4 + 4 + 1);
    select << npc->GetGUID() << bot->PlayerTalkClass->GetGossipMenu().GetMenuId() << listId;
    if (bot->PlayerTalkClass->IsGossipOptionCoded(listId))
        select << std::string();
    bot->GetSession()->HandleGossipSelectOptionOpcode(select);
}

bool GuideServiceAction::GossipByCredits(WorldObject* npc, std::vector<GuideGossipCredit> const& credits)
{
    // The option a script gives the credit for, reached through the menus before it (an option leading on to its
    // menu, up to GOSSIP_DEPTH deep). False: this NPC offers none of them.
    std::vector<GuideGossipCredit> mine;
    for (GuideGossipCredit const& credit : credits)
        if (credit.entry == npc->GetEntry())
            mine.push_back(credit);
    if (mine.empty())
        return false;
    std::function<bool(uint32, uint32)> leadsTo = [&](uint32 menuId, uint32 depth)
    {
        if (std::any_of(mine.begin(), mine.end(),
                        [&](GuideGossipCredit const& credit) { return credit.menu == menuId; }))
            return true;
        if (!depth)
            return false;
        auto bounds = sObjectMgr->GetGossipMenuItemsMapBounds(menuId);
        for (auto it = bounds.first; it != bounds.second; ++it)
            if (it->second.ActionMenuID && it->second.ActionMenuID != menuId &&
                leadsTo(it->second.ActionMenuID, depth - 1))
                return true;
        return false;
    };

    OpenGossip(npc);
    static constexpr uint32 GOSSIP_DEPTH = 3;
    for (uint32 depth = 0; depth <= GOSSIP_DEPTH; ++depth)
    {
        GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
        auto credit = std::find_if(mine.begin(), mine.end(), [&](GuideGossipCredit const& at)
                                   { return at.menu == menu.GetMenuId() && menu.GetItem(at.option); });
        if (credit != mine.end())
        {
            SelectGossip(npc, credit->option);
            LOG_INFO("playerbots.guide", "{}: picked {}'s gossip option {} (menu {}) for its quest", bot->GetName(),
                     npc->GetName(), credit->option, credit->menu);
            break;
        }
        uint32 next = 0;
        bool found = false;
        for (auto const& [listId, item] : menu.GetMenuItems())
            if (GossipMenuItemData const* data = menu.GetItemData(listId);
                data && data->GossipActionMenuId && leadsTo(data->GossipActionMenuId, GOSSIP_DEPTH - depth))
            {
                next = listId;
                found = true;
                break;
            }
        if (!found)
        {
            LOG_INFO("playerbots.guide", "{}: {} shows no way to its quest's gossip option (menu {})", bot->GetName(),
                     npc->GetName(), menu.GetMenuId());
            break;
        }
        SelectGossip(npc, next);
    }
    bot->PlayerTalkClass->SendCloseGossip();
    return true;
}

void GuideServiceAction::Gossip(WorldObject* npc, GuideElement const& element,
                                std::vector<GuideGossipCredit> const& credits)
{
    // RestedXP's ".skipgossip": the option a script gives the step's quest credit for when this NPC has one (the
    // data knows the exact option), else the options RestedXP names, a menu at a time (one can open the next)
    if (GossipByCredits(npc, credits))
        return;
    OpenGossip(npc);
    for (uint32 option : element.entries)
    {
        GossipMenu const& menu = bot->PlayerTalkClass->GetGossipMenu();
        GossipMenuItemContainer const& items = menu.GetMenuItems();
        if (!option || option > items.size())
        {
            LOG_INFO("playerbots.guide", "{}: {} has no gossip option {} ({} shown)", bot->GetName(), npc->GetName(),
                     option, items.size());
            break;
        }
        SelectGossip(npc, std::next(items.begin(), option - 1)->first);
        LOG_DEBUG("playerbots.guide", "{}: picked gossip option {} at {}", bot->GetName(), option, npc->GetName());
    }
    bot->PlayerTalkClass->SendCloseGossip();
}

void GuideServiceAction::AuctionItem(GuideElement const& element, uint32 index)
{
    // An auction-house ".collect" (cooking ingredients, a sharpening stone): what's missing, given at the auctioneer
    GuideInfo& info = botAI->guideInfo;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
    uint32 have = bot->GetItemCount(element.id, true);
    if (!proto || have >= element.count)
        return;
    bot->AddItem(element.id, element.count - have);
    LOG_INFO("playerbots.guide", "{}: gets {} x{} at the auction house", bot->GetName(), proto->Name1,
             element.count - have);
    if (bot->GetItemCount(element.id, true) >= element.count || index >= 32)
        return;
    info.skippedWork |= 1u << index;  // no room in the bags
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_cant_buy", "Guide: I couldn't buy %item (not enough money, or no room); passing it by.",
        {{"%item", proto->Name1}}));
}

void GuideServiceAction::BuyItem(Creature* vendor, GuideElement const& element, uint32 index)
{
    // A ".collect" item the vendor sells: what's missing, as far as the money goes. Still short: given up, and said.
    GuideInfo& info = botAI->guideInfo;
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(element.id);
    VendorItemData const* items = vendor->GetVendorItems();
    if (!proto || !items)
        return;
    uint32 have = bot->GetItemCount(element.id, true);
    for (uint32 slot = 0; slot < items->GetItemCount() && have < element.count; ++slot)
    {
        VendorItem const* sold = items->GetItem(slot);
        if (!sold || sold->item != element.id || sold->ExtendedCost)
            continue;
        uint32 perBuy = std::max<uint32>(proto->BuyCount, 1);
        uint32 buys = (element.count - have + perBuy - 1) / perBuy;
        uint32 price = uint32(std::floor(proto->BuyPrice * bot->GetReputationPriceDiscount(vendor)));
        if (price)
            buys = std::min<uint32>(buys, bot->GetMoney() / price);
        buys = std::min<uint32>(buys, 255);
        if (buys)
            bot->BuyItemFromVendorSlot(vendor->GetGUID(), slot, element.id, uint8(buys), NULL_BAG, NULL_SLOT);
        LOG_DEBUG("playerbots.guide", "{}: bought {} x{}", bot->GetName(), proto->Name1, buys * perBuy);
        have = bot->GetItemCount(element.id, true);
    }
    if (have >= element.count || index >= 32 || (info.skippedWork & (1u << index)))
        return;
    info.skippedWork |= 1u << index;
    LOG_INFO("playerbots.guide", "{}: couldn't buy {} ({} of {})", bot->GetName(), proto->Name1, have, element.count);
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_cant_buy", "Guide: I couldn't buy %item (not enough money, or no room); passing it by.",
        {{"%item", proto->Name1}}));
}

void GuideServiceAction::SellJunk(Creature* vendor)
{
    // Everything Sellable (what's worn stays)
    std::set<uint32> const& collect = AI_VALUE(std::set<uint32>&, "always loot list");
    std::vector<Item*> bags = BagItems(bot);
    std::pair<uint32, uint32> const supplies = BestSupplies(bot, bags);
    std::unordered_map<uint32, uint32> const& needed = GuideRules::NeededItems(bot, botAI->guideInfo);
    uint32 sold = 0;
    std::string names;
    for (Item* item : bags)
    {
        if (!Sellable(bot, item, collect, supplies, needed))
            continue;
        WorldPacket packet(CMSG_SELL_ITEM);
        packet << vendor->GetGUID() << item->GetGUID() << uint32(item->GetCount());
        WorldPackets::Item::SellItem sell(std::move(packet));
        sell.Read();
        std::string name = item->GetTemplate()->Name1;
        uint32 count = item->GetCount();
        bot->GetSession()->HandleSellItemOpcode(sell);
        names += Acore::StringFormat("{}{} x{}", names.empty() ? "" : ", ", name, count);
        ++sold;
    }
    if (sold)
        LOG_INFO("playerbots.guide", "{}: sold {}", bot->GetName(), names);
}

void GuideServiceAction::BuySupplies(Creature* vendor)
{
    // The best food the character can eat (and drink for mana, ammo for a hunter's weapon) the vendor has, up to a
    // stack's worth, while the money is at least twice the price
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items)
        return;

    std::vector<Item*> bags = BagItems(bot);
    for (Supply want : {Supply::Food, Supply::Drink, Supply::Ammo})
    {
        if (want == Supply::Drink && bot->getPowerType() != POWER_MANA)
            continue;
        uint32 stock = want == Supply::Ammo ? AMMO_STOCK : FOOD_STOCK;
        uint32 have = 0;
        for (Item* item : bags)
            if (SupplyOf(item->GetTemplate()) == want)
                have += item->GetCount();
        if (have >= stock)
            continue;

        int32 best = -1;
        ItemTemplate const* bestProto = nullptr;
        for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
        {
            VendorItem const* sold = items->GetItem(slot);
            ItemTemplate const* proto = sold ? sObjectMgr->GetItemTemplate(sold->item) : nullptr;
            if (!proto || SupplyOf(proto) != want || proto->RequiredLevel > bot->GetLevel() || sold->ExtendedCost ||
                (want == Supply::Ammo && AI_VALUE2(ItemUsage, "item usage", proto->ItemId) != ITEM_USAGE_AMMO))
                continue;
            if (!bestProto || proto->RequiredLevel > bestProto->RequiredLevel ||
                (proto->RequiredLevel == bestProto->RequiredLevel && proto->ItemLevel > bestProto->ItemLevel))
            {
                best = int32(slot);
                bestProto = proto;
            }
        }
        if (!bestProto)
            continue;

        uint32 perBuy = std::max<uint32>(bestProto->BuyCount, 1);
        uint32 buys = (stock - have + perBuy - 1) / perBuy;
        uint32 price = uint32(std::floor(bestProto->BuyPrice * bot->GetReputationPriceDiscount(vendor)));
        if (price)
            buys = std::min<uint32>(buys, bot->GetMoney() / (2 * price));
        buys = std::min<uint32>(buys, 255);
        if (!buys)
            continue;
        bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(best), bestProto->ItemId, uint8(buys), NULL_BAG,
                                   NULL_SLOT);
        LOG_DEBUG("playerbots.guide", "{}: bought {} x{}", bot->GetName(), bestProto->Name1, buys * perBuy);
    }
}

void GuideServiceAction::BuyMount(Creature* vendor)
{
    // Riding learned and no mount in the bags: the cheapest one the vendor sells that the character can ride (its
    // race's, at its riding skill), as far as the money goes. RestedXP's riding steps train and then buy one here.
    auto isMount = [](ItemTemplate const* proto)
    {
        for (_Spell const& spell : proto->Spells)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell.SpellId))
                if (info->HasAura(SPELL_AURA_MOUNTED))
                    return true;
        return false;
    };
    VendorItemData const* items = vendor->GetVendorItems();
    if (!items || !bot->HasSkill(SKILL_RIDING))
        return;
    for (Item* item : BagItems(bot))
        if (isMount(item->GetTemplate()))
            return;

    int32 best = -1;
    ItemTemplate const* bestProto = nullptr;
    uint32 bestPrice = 0;
    for (uint32 slot = 0; slot < items->GetItemCount(); ++slot)
    {
        VendorItem const* sold = items->GetItem(slot);
        ItemTemplate const* proto = sold ? sObjectMgr->GetItemTemplate(sold->item) : nullptr;
        if (!proto || sold->ExtendedCost || !isMount(proto) || bot->CanUseItem(proto) != EQUIP_ERR_OK)
            continue;
        uint32 price = uint32(std::floor(proto->BuyPrice * bot->GetReputationPriceDiscount(vendor)));
        if (price <= bot->GetMoney() && (!bestProto || price < bestPrice))
        {
            best = int32(slot);
            bestProto = proto;
            bestPrice = price;
        }
    }
    if (!bestProto)
        return;
    bot->BuyItemFromVendorSlot(vendor->GetGUID(), uint32(best), bestProto->ItemId, 1, NULL_BAG, NULL_SLOT);
    LOG_INFO("playerbots.guide", "{}: bought the mount {} for {} copper", bot->GetName(), bestProto->Name1, bestPrice);
}

bool GuideOpenAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideInfo& info = botAI->guideInfo;
    GuideTask const& task = info.task;
    LootObjectStack* loot = AI_VALUE(LootObjectStack*, "available loot");
    if (!task.open || task.object.IsEmpty() || loot->IsLootPending() || bot->GetLootGUID() ||
        bot->IsNonMeleeSpellCast(false) || (info.openedAt && GetMSTimeDiffToNow(info.openedAt) < OPEN_AGAIN))
        return false;
    info.openedAt = getMSTime();

    LootObject object(bot, task.object);
    GameObject* target = botAI->GetGameObject(task.object);
    if (!object.IsEmpty() && target && target->GetGoType() == GAMEOBJECT_TYPE_CHEST &&
        bot->GetDistance(target) > GuideUpdateAction::OPEN_RANGE)
    {
        // A chest opens only by its lock's spell ("open loot", from OPEN_RANGE), never by being used (the Plant
        // Bundles of Bathran's Hair): stepped up to straight, the last yards the paths don't get; still short after
        // STEP_INS tries, left for another
        if (info.stepIn != task.object)
        {
            info.stepIn = task.object;
            info.stepIns = 0;
        }
        if (++info.stepIns > STEP_INS)
        {
            info.unreachable[task.object] = getMSTime() + GuideUpdateAction::UNREACHABLE_TIME;
            info.stepIn = ObjectGuid::Empty;
            info.lastUpdate = 0;
            return false;
        }
        bot->GetMotionMaster()->MovePoint(0, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ(),
                                          FORCED_MOVEMENT_NONE, 0.0f, 0.0f, false);
        return true;
    }
    if (object.IsEmpty() || (target && bot->GetDistance(target) > GuideUpdateAction::OPEN_RANGE))
    {
        // Nothing to loot: an object to use (a grave, a shrine, a lever). Used as the client does; for a quest's
        // object the core gives the credit.
        GameObject* go = botAI->GetGameObject(task.object);
        if (!go)
            return false;
        if (bot->isMoving())
            bot->StopMoving();
        WorldPacket packet(CMSG_GAMEOBJ_USE, 8);
        packet << task.object;
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
        botAI->guideInfo.lastUpdate = 0;
        return true;
    }
    loot->Add(task.object);
    context->GetValue<LootObject>("loot target")->Set(object);
    botAI->guideInfo.lastUpdate = 0;
    return botAI->DoSpecificAction("open loot", Event("guide"), true);
}

bool GuideRestAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot) || bot->IsInCombat() || bot->IsMounted())
        return false;

    // Eat and drink what the bags hold (the food strategy only eats below Playerbots.LowHealth), else sit.
    bool mana = bot->getPowerType() == POWER_MANA;
    if (bot->GetHealthPct() < GuideUpdateAction::RESTED_HEALTH && !bot->HasAuraType(SPELL_AURA_MOD_REGEN))
    {
        std::vector<Item*> food = AI_VALUE2(std::vector<Item*>, "inventory items", "food");
        if (!food.empty() && UseItemAuto(food.front()))
            return true;
    }
    if (mana && bot->GetPowerPct(POWER_MANA) < GuideUpdateAction::RESTED_MANA &&
        !bot->HasAuraType(SPELL_AURA_MOD_POWER_REGEN))
    {
        std::vector<Item*> drink = AI_VALUE2(std::vector<Item*>, "inventory items", "drink");
        if (!drink.empty() && UseItemAuto(drink.front()))
            return true;
    }
    if (bot->isMoving())
        bot->StopMoving();
    if (!bot->IsSitState())
        bot->SetStandState(UNIT_STAND_STATE_SIT);
    return true;
}

bool GuideHearthAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideInfo& info = botAI->guideInfo;
    GuideTask const& task = info.task;
    auto done = [&]()
    {
        if (task.sideStep >= 0)
            info.sideDone.insert(static_cast<uint32>(task.sideStep));
        else if (task.element < 32)
            info.serviced |= 1u << task.element;
        info.hearthAt = 0;
        info.lastUpdate = 0;
    };

    if (NearHome(bot))
    {
        done();
        return true;
    }
    if (bot->IsNonMeleeSpellCast(false))
        return true;
    // Used it: wait for the teleport, or give up and walk.
    if (info.hearthAt)
    {
        if (GetMSTimeDiffToNow(info.hearthAt) > HEARTH_WAIT)
            done();
        return true;
    }

    Item* stone = bot->GetItemByEntry(ITEM_HEARTHSTONE);
    if (!stone || bot->HasSpellCooldown(SPELL_HEARTHSTONE) || bot->IsInCombat())
    {
        done();  // no hearthstone, or on cooldown: walk
        return false;
    }
    if (bot->isMoving())
        bot->StopMoving();
    if (UseItemAuto(stone))
    {
        info.hearthAt = getMSTime();
        return true;
    }
    done();
    return false;
}

bool GuideRideAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideInfo& info = botAI->guideInfo;
    GuideTrip& trip = info.trip;
    if (trip.leg >= trip.legs.size())
        return false;

    GuideLeg const& leg = trip.legs[trip.leg];
    switch (leg.kind)
    {
        case GuideLeg::Kind::Portal:
        {
            // In the trigger: the client's own trigger takes it through; if it doesn't, the bot goes through itself.
            if (bot->IsBeingTeleported())
                return false;
            if (!trip.enteredAt)
            {
                trip.enteredAt = getMSTime();
                if (bot->isMoving())
                    bot->StopMoving();
                return true;
            }
            if (GetMSTimeDiffToNow(trip.enteredAt) < PORTAL_DWELL)
                return false;
            AreaTriggerTeleport const* teleport = sObjectMgr->GetAreaTriggerTeleport(leg.entry);
            if (!teleport)
                return false;
            trip.enteredAt = getMSTime();
            return bot->TeleportTo(teleport->target_mapId, teleport->target_X, teleport->target_Y, teleport->target_Z,
                                   teleport->target_Orientation);
        }
        case GuideLeg::Kind::Transport:
            // The ride's time is up: at the far edge
            if (bot->IsBeingTeleported())
                return false;
            return bot->TeleportTo(leg.land.GetMapId(), leg.land.GetPositionX(), leg.land.GetPositionY(),
                                   leg.land.GetPositionZ(), bot->GetOrientation());
        case GuideLeg::Kind::Fly:
        {
            Creature* master = ObjectAccessor::GetCreature(*bot, info.task.object);
            if (!master)
                return false;
            // What only lasts a while (a fight, a cast, a mount, a form) is waited out or ended; what the core still
            // refuses after that is logged, and the trip goes on without flights
            if (bot->IsInCombat() || bot->IsNonMeleeSpellCast(false) || bot->IsBeingTeleported())
                return false;
            if (bot->IsMounted())
            {
                bot->Dismount();
                bot->RemoveAurasByType(SPELL_AURA_MOUNTED);
                return true;
            }
            if (bot->IsInDisallowedMountForm())
            {
                bot->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);
                return true;
            }
            if (!bot->ActivateTaxiPathTo(leg.taxiNodes, master))
            {
                LOG_INFO("playerbots.guide", "{}: couldn't fly from {} ({}); on without flights", bot->GetName(),
                         master->GetName(), FlightRefusal(leg.taxiNodes));
                Reroute(false, true);
            }
            info.lastUpdate = 0;
            return true;
        }
        case GuideLeg::Kind::Hearth:
        {
            if (bot->IsNonMeleeSpellCast(false))
                return true;
            // Used it (enteredAt): wait for the teleport, or give up on it
            if (trip.enteredAt)
            {
                if (GetMSTimeDiffToNow(trip.enteredAt) > HEARTH_WAIT)
                    Reroute(true, false);
                return false;
            }
            if (leg.entry)
            {
                // A class teleport (Teleport: Moonglade): after a fight; not known or cooling down, on without it
                if (bot->IsInCombat())
                    return false;
                if (!bot->HasSpell(leg.entry) || bot->HasSpellCooldown(leg.entry))
                {
                    Reroute(true, false);
                    return false;
                }
                if (bot->isMoving())
                    bot->StopMoving();
                if (!botAI->CastSpell(leg.entry, bot))
                {
                    LOG_INFO("playerbots.guide", "{}: couldn't cast teleport {}; on without it", bot->GetName(),
                             leg.entry);
                    Reroute(true, false);
                    return false;
                }
                trip.enteredAt = getMSTime();
                return true;
            }
            Item* stone = bot->GetItemByEntry(ITEM_HEARTHSTONE);
            if (!stone || bot->HasSpellCooldown(SPELL_HEARTHSTONE) || bot->IsInCombat())
            {
                Reroute(true, false);
                return false;
            }
            if (bot->isMoving())
                bot->StopMoving();
            if (!UseItemAuto(stone))
            {
                Reroute(true, false);
                return false;
            }
            trip.enteredAt = getMSTime();
            return true;
        }
    }
    return false;
}

// Why the core's ActivateTaxiPathTo said no, as far as it can be told from outside (it doesn't say)
std::string GuideRideAction::FlightRefusal(std::vector<uint32> const& nodes)
{
    if (nodes.size() < 2)
        return "no flight points";
    if (bot->HasUnitState(UNIT_STATE_STUNNED) || bot->HasUnitState(UNIT_STATE_ROOT) ||
        bot->HasUnitFlag(UNIT_FLAG_DISABLE_MOVE))
        return "can't move (stunned, rooted)";
    uint32 cost = 0;
    for (size_t i = 1; i < nodes.size(); ++i)
    {
        uint32 path = 0;
        uint32 price = 0;
        sObjectMgr->GetTaxiPath(nodes[i - 1], nodes[i], path, price);
        if (!path)
            return Acore::StringFormat("no flight path {} -> {}", nodes[i - 1], nodes[i]);
        cost += price;
    }
    for (uint32 node : nodes)
        if (!bot->m_taxi.IsTaximaskNodeKnown(node))
            return Acore::StringFormat("flight point {} not known", node);
    if (bot->GetMoney() < cost)
        return Acore::StringFormat("{} copper short", cost - bot->GetMoney());
    return "the core gave no reason";
}

void GuideRideAction::Reroute(bool noHearth, bool noFly)
{
    GuideTrip& trip = botAI->guideInfo.trip;
    trip.noHearth |= noHearth;
    trip.noFly |= noFly;
    trip.legs.clear();
    trip.routedAt = 0;
    botAI->guideInfo.lastUpdate = 0;
}

bool GuideUseAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    GuideInfo& info = botAI->guideInfo;
    GuideTask const& task = info.task;
    Item* item = bot->GetItemByEntry(task.item);
    GuideUse const* use = GuideMgr::instance().GetUse(task.item);
    if (!item || !use)
        return false;

    bool used = false;
    if (use->mode == GuideUse::Mode::Equip)
    {
        WorldPacket packet(CMSG_AUTOEQUIP_ITEM, 2);
        packet << item->GetBagSlot() << item->GetSlot();
        WorldPackets::Item::AutoEquipItem equip(std::move(packet));
        equip.Read();
        bot->GetSession()->HandleAutoEquipItemOpcode(equip);
        used = true;
    }
    else if (use->mode == GuideUse::Mode::Open)
    {
        // Opened as the client does, everything taken, the window closed (an emptied container is gone)
        if (bot->GetLootGUID() || bot->IsNonMeleeSpellCast(false))
            return false;
        ObjectGuid guid = item->GetGUID();
        WorldPacket open(CMSG_OPEN_ITEM, 2);
        open << item->GetBagSlot() << item->GetSlot();
        bot->GetSession()->HandleOpenItemOpcode(open);
        if (bot->GetLootGUID() != guid)
            return false;
        Loot& loot = item->loot;
        uint32 slots = loot.items.size() + loot.quest_items.size();
        for (uint32 slot = 0; slot < slots && slot < 256; ++slot)
        {
            WorldPacket take(CMSG_AUTOSTORE_LOOT_ITEM, 1);
            take << uint8(slot);
            bot->GetSession()->HandleAutostoreLootItemOpcode(take);
        }
        WorldPacket money(CMSG_LOOT_MONEY, 0);
        bot->GetSession()->HandleLootMoneyOpcode(money);
        WorldPacket release(CMSG_LOOT_RELEASE, 8);
        release << guid;
        bot->GetSession()->HandleLootReleaseOpcode(release);
        LOG_INFO("playerbots.guide", "{}: opened {}", bot->GetName(), sObjectMgr->GetItemTemplate(task.item)->Name1);
        used = true;
    }
    else
    {
        Unit* unit = task.useOn ? ObjectAccessor::GetUnit(*bot, task.useOn) : nullptr;
        GameObject* object = task.object ? botAI->GetGameObject(task.object) : nullptr;
        if (unit)
            bot->SetFacingToObject(unit);
        else if (object)
            bot->SetFacingToObject(object);
        used = CastItem(bot, item, use->spell, unit, object, task.dest != WorldPosition() ? &task.dest : nullptr);
        if (used && unit && !unit->IsAlive())
        {
            if (info.usedOn.size() > 64)
                info.usedOn.clear();
            info.usedOn.insert(unit->GetGUID());
        }
    }
    if (!used)
        return false;

    if (task.element < 32)
        info.serviced |= 1u << task.element;
    info.lastUpdate = 0;
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(use->spell);
    ForceToWait((spellInfo ? spellInfo->CalcCastTime() : 0) + 1000);
    return true;
}

bool GuideLureAction::Execute(Event /*event*/)
{
    WorldPosition const& trap = botAI->guideInfo.lureTrap;
    return MoveTo(trap.GetMapId(), trap.GetPositionX(), trap.GetPositionY(), trap.GetPositionZ(), false, false, false,
                  true, MovementPriority::MOVEMENT_FORCED);
}

bool GuideUseNowAction::Execute(Event /*event*/)
{
    // At the health mark: stop hitting it (the next swing could kill it) and use the item.
    GuideInfo const& info = botAI->guideInfo;
    Unit* victim = bot->GetVictim();
    Item* item = bot->GetItemByEntry(info.useItem);
    if (!victim || !item)
        return false;
    bot->AttackStop();
    bot->SetFacingToObject(victim);
    return CastItem(bot, item, info.useSpell, victim, nullptr, nullptr);
}

bool GuideAttackAction::Execute(Event /*event*/)
{
    if (!Guiding(botAI, bot))
        return false;

    Unit* target = botAI->GetUnit(botAI->guideInfo.task.target);
    return target && target->IsAlive() && Attack(target);
}

bool GuideReleaseAction::Execute(Event /*event*/)
{
    GuideInfo& info = botAI->guideInfo;
    if (!info.diedAt)
        info.diedAt = getMSTime();
    // Waiting takes the tick, so the bots' own auto release (with its free repair) doesn't go first.
    if (GetMSTimeDiffToNow(info.diedAt) < RELEASE_DELAY)
        return true;

    info.diedAt = 0;
    LOG_INFO("playerbots", "Guide: {} releases its spirit", bot->GetName());
    WorldPacket packet(CMSG_REPOP_REQUEST);
    packet << uint8(0);
    bot->GetSession()->HandleRepopRequestOpcode(packet);
    botAI->SetNextCheckDelay(1000);
    return true;
}

bool GuideCorpseRunAction::Execute(Event /*event*/)
{
    Corpse* corpse = bot->GetCorpse();
    if (!corpse)
        return false;

    WorldPosition body(corpse);
    if (bot->GetMapId() != body.GetMapId() || bot->GetExactDist(body) > CORPSE_RECLAIM_RADIUS - 10.0f)
        return MoveFarTo(body);

    time_t ready = corpse->GetGhostTime() + bot->GetCorpseReclaimDelay(corpse->GetType() == CORPSE_RESURRECTABLE_PVP);
    if (ready > time(nullptr))
        return false;

    LOG_INFO("playerbots", "Guide: {} takes its body back", bot->GetName());
    bot->GetMotionMaster()->Clear();
    bot->StopMoving();
    WorldPacket packet(CMSG_RECLAIM_CORPSE);
    packet << bot->GetGUID();
    bot->GetSession()->HandleReclaimCorpseOpcode(packet);
    botAI->guideInfo.lastUpdate = 0;
    return true;
}

bool GuideCommandAction::Execute(Event event)
{
    PlayerbotTextMgr& text = PlayerbotTextMgr::instance();
    if (!IsSelfBot(bot))
    {
        botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
            "guide_selfbot_only", "The guide is for your own character: turn on self-bot first.", {}));
        return false;
    }
    if (!GuideMgr::instance().IsEnabled())
    {
        botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
            "guide_no_data", "There is no guide data on this realm (Playerbots.Guide.DataFile).", {}));
        return false;
    }

    GuideInfo& info = botAI->guideInfo;
    std::string const param = event.getParam();
    auto begin = [&]()
    {
        info.trip.going = false;
        info.trip.Clear();
        info.active = true;
        info.stepStart = getMSTime();
        info.lastUpdate = 0;
        botAI->ChangeStrategy("+guide", BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("+guide death", BOT_STATE_DEAD);
        botAI->ChangeStrategy("+guide combat", BOT_STATE_COMBAT);
        GuideProgress::Save(botAI);
        botAI->TellMasterNoFacing("Guide: " + GuideRules::Describe(bot, info));
        return true;
    };
    auto nothingLeft = [&]()
    {
        botAI->TellMasterNoFacing(
            text.GetBotTextOrDefault("guide_nothing_left", "Guide: nothing left on your route.", {}));
        return false;
    };

    // Says what work a resume or start picked the step for
    auto tellReached = [&](std::string const& reached)
    {
        if (info.guide)
            LOG_INFO("playerbots.guide", "{}: starts at {} step {} from the quest log: {}", bot->GetName(),
                     info.guide->name, info.guide->steps[info.step].number, reached.empty() ? "-" : reached);
        if (!reached.empty())
            botAI->TellMasterNoFacing("Guide: the first work left for you: " + reached + ".");
    };
    // "guide resume": the step it was left on (this session, or saved); with none, where the quest log says
    if (param == "resume")
    {
        if (info.guide)
        {
            LOG_INFO("playerbots.guide", "{}: resumes {} step {}", bot->GetName(), info.guide->name,
                     info.guide->steps[info.step].number);
            return begin();
        }
        std::string reached;
        if (!GuideRules::Resume(bot, info, nullptr, &reached))
            return nothingLeft();
        tellReached(reached);
        return begin();
    }
    if (param.rfind("start", 0) == 0)
    {
        std::string name = param.size() > 6 ? param.substr(6) : "";
        // "guide start <guide> <step>": that step (RestedXP's number), as it is
        uint32 stepNumber = 0;
        if (size_t space = name.find_last_of(' ');
            space != std::string::npos && !GuideMgr::instance().GetGuide(name) &&
            name.find_first_not_of("0123456789", space + 1) == std::string::npos && space + 1 < name.size())
        {
            stepNumber = uint32(std::stoul(name.substr(space + 1)));
            name.resize(space);
        }
        if (!name.empty())
        {
            Guide const* guide = GuideMgr::instance().GetGuide(name);
            if (!guide || guide->steps.empty() || !guide->condition.Holds(bot))
            {
                botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
                    "guide_unknown", "Guide: no guide called %name for your character.", {{"%name", name}}));
                return false;
            }
            if (stepNumber)
            {
                auto step = std::find_if(guide->steps.begin(), guide->steps.end(),
                                         [&](GuideStep const& at) { return at.number >= stepNumber; });
                if (step == guide->steps.end())
                {
                    botAI->TellMasterNoFacing(Acore::StringFormat("Guide: {} has no step {}.", name, stepNumber));
                    return false;
                }
                info.Reset();
                info.guide = guide;
                info.step = uint32(step - guide->steps.begin());
                return begin();
            }
            // From that guide on, where the quest log says (its steps without quest work aren't walked back to)
            std::string reached;
            if (!GuideRules::Resume(bot, info, guide, &reached))
                return nothingLeft();
            tellReached(reached);
            return begin();
        }
        std::string reached;
        if (!GuideRules::Resume(bot, info, nullptr, &reached))
            return nothingLeft();
        tellReached(reached);
        return begin();
    }
    // "guide go <place>": there by the fastest way (walking, flights, portals, the tram, boats, the hearthstone); then
    // back to the route if it was on, else off
    if (param == "go" || param.rfind("go ", 0) == 0)
    {
        std::string name = param.size() > 3 ? param.substr(3) : "";
        GuideTrip& trip = info.trip;
        if (name.empty())
        {
            botAI->TellMasterNoFacing(trip.going ? "Guide: " + GuideRules::Describe(bot, info)
                                                 : std::string("Guide: guide go <place>, e.g. guide go Ironforge."));
            return true;
        }
        WorldPosition place;
        std::string found;
        if (!GuideTravel::instance().FindPlace(bot, name, place, found))
        {
            botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
                "guide_unknown_place", "Guide: I know no place called %place.", {{"%place", name}}));
            return false;
        }
        trip.resume = trip.going ? trip.resume : info.active && info.guide;
        trip.going = true;
        trip.name = found;
        trip.place = place;
        trip.noHearth = trip.noFly = false;
        trip.Clear();
        info.active = true;
        info.lastUpdate = 0;
        botAI->ChangeStrategy("+guide", BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("+guide death", BOT_STATE_DEAD);
        botAI->ChangeStrategy("+guide combat", BOT_STATE_COMBAT);
        botAI->TellMasterNoFacing(
            text.GetBotTextOrDefault("guide_going", "Guide: on my way to %place.", {{"%place", found}}));
        return true;
    }
    if (param == "stop")
    {
        info.active = false;
        info.task = GuideTask();
        info.trip.going = false;
        info.trip.Clear();
        GuideOff(botAI);
        // Stop where it stands (a walk under way would run on), unless flying
        if (!bot->IsInFlight())
        {
            bot->GetMotionMaster()->Clear();
            bot->StopMoving();
        }
        botAI->TellMasterNoFacing(text.GetBotTextOrDefault("guide_stopped", "Guide: stopped.", {}));
        return true;
    }
    // "guide audit [all | <guide>]": the route (or every guide, or one) checked against the realm's data for this
    // character; findings in the guide log, the counts here
    if (param == "audit" || param.rfind("audit ", 0) == 0)
    {
        std::string which = param.size() > 6 ? param.substr(6) : "";
        std::vector<Guide const*> guides;
        if (which == "all")
        {
            for (Guide const* guide : GuideMgr::instance().GetGuides())
                if (guide->condition.Holds(bot))
                    guides.push_back(guide);
        }
        else if (!which.empty())
        {
            if (Guide const* guide = GuideMgr::instance().GetGuide(which))
                guides.push_back(guide);
        }
        else
        {
            std::set<Guide const*> seen;
            for (Guide const* guide = GuideMgr::instance().GetStartGuide(bot); guide && seen.insert(guide).second;
                 guide = GuideMgr::instance().GetNextGuide(bot, *guide))
                guides.push_back(guide);
        }
        if (guides.empty())
        {
            botAI->TellMasterNoFacing(text.GetBotTextOrDefault(
                "guide_unknown", "Guide: no guide called %name for your character.", {{"%name", which}}));
            return false;
        }

        GuideAudit::Summary summary = GuideAudit::Run(bot, guides);
        std::vector<std::pair<uint32, std::string>> kinds;
        for (auto const& [kind, count] : summary.byKind)
            kinds.emplace_back(count, kind);
        std::sort(kinds.rbegin(), kinds.rend());
        std::string top;
        for (size_t i = 0; i < kinds.size() && i < 8; ++i)
            top += Acore::StringFormat("{}{} {}", top.empty() ? "" : ", ", kinds[i].first, kinds[i].second);
        LOG_INFO("playerbots.guide", "AUDIT {}: {} guides, {} steps, {} findings", bot->GetName(), summary.guides,
                 summary.steps, summary.findings);
        for (auto const& [count, kind] : kinds)
            LOG_INFO("playerbots.guide", "AUDIT {}:   {} x {}", bot->GetName(), count, kind);
        botAI->TellMasterNoFacing(
            Acore::StringFormat("Guide audit: {} guides, {} steps, {} findings{}{}. Details in Guide.log.",
                                summary.guides, summary.steps, summary.findings, top.empty() ? "" : ": ", top));
        return true;
    }
    if (param == "skip" && info.guide)
    {
        if (!GuideRules::Advance(bot, info))
            info.Reset();
        info.lastUpdate = 0;
        botAI->TellMasterNoFacing("Guide: " + GuideRules::Describe(bot, info));
        return true;
    }

    botAI->TellMasterNoFacing("Guide: " + (info.active ? GuideRules::Describe(bot, info) : std::string("off")) +
                              ". Commands: guide start [guide [step]], guide resume, guide go <place>, guide stop, "
                              "guide skip, guide audit [all].");
    return true;
}
