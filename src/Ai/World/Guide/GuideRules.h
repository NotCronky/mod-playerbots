/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDERULES_H
#define PLAYERBOTS_GUIDERULES_H

#include <string>
#include <vector>

#include "Define.h"

class Player;
class Quest;
namespace Trainer
{
class Trainer;
}
struct Guide;
struct GuideElement;
struct GuideInfo;
struct GuideStep;

// One quest objective as the client lists it (RestedXP's ".complete <quest>,<n>" counts in this order):
// reputation, money, player kills, then creatures and objects, then items; an area to explore (or a scripted event)
// last.
struct GuideObjective
{
    enum class Kind : uint8
    {
        Reputation,
        Money,
        PlayerKills,
        CreatureOrObject,
        Item,
        Exploration
    };

    Kind kind{Kind::Item};
    uint32 slot{0};  // CreatureOrObject: RequiredNpcOrGo index; Item: RequiredItemId index
};

// RestedXP's step rules for the guide strategy, simplified (see the guide-strategy plan).
namespace GuideRules
{
bool Applies(Player* bot, GuideElement const& element);
bool StepApplies(Player* bot, GuideStep const& step);
bool ElementDone(Player* bot, GuideElement const& element);
// Quest work done (Travel steps also need their places reached, which GuideInfo tracks).
// skipped: the step's elements given up (by index: an item the character can't get), counted as done
bool QuestWorkDone(Player* bot, GuideStep const& step, uint32 skipped = 0);
bool HasQuestWork(Player* bot, GuideStep const& step);
// Training or selling (done is tracked per step, in GuideInfo::serviced).
bool HasServices(Player* bot, GuideStep const& step);
// The step was certainly reached: one of its quests is in the log or handed in.
bool Reached(Player* bot, GuideStep const& step);
std::vector<GuideObjective> Objectives(Quest const* quest);
bool ObjectiveDone(Player* bot, Quest const* quest, GuideObjective const& objective);
// The creature (by entry) does the service element's job for the character: sells, binds, flies, trains (that spell).
bool Serves(Player* bot, GuideElement const& element, uint32 creatureEntry);
// The trainer's spell that gives `wanted`: the spell itself, or one teaching it (a profession's "Blacksmithing" is
// taught by a spell of its own on 3.3.5 trainers). 0: none.
uint32 TrainerSpellFor(Trainer::Trainer const* trainer, uint32 wanted);
// The step's ".target" is an auctioneer: its ".collect"s are bought at the auction house (passed by: no auctions)
bool AtAuctionHouse(Player* bot, GuideStep const& step);
// A later step on the route (this guide and the next few) works on the quest or hands it in.
bool StillNeeded(Player* bot, GuideInfo const& info, uint32 questId);
constexpr uint32 STILL_NEEDED_GUIDES = 4;
// Moves info to the first step that isn't done, from the character's first guide on. False: nothing left.
bool Resume(Player* bot, GuideInfo& info);
// The next step that applies after info's current one, through the next guides. False: the route is done.
bool Advance(Player* bot, GuideInfo& info);
std::string Describe(Player* bot, GuideInfo const& info);
}  // namespace GuideRules

#endif
