/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEAUDIT_H
#define PLAYERBOTS_GUIDEAUDIT_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include "Define.h"

class ChatHandler;
class Player;
struct Guide;

// "guide audit": checks the route against the realm's data for one character, before it is played. Each step that
// applies to the character (race, class, faction; the level and quest-state checks are left out): its quests,
// givers, enders, objectives and their creatures, objects and item sources, item uses and services, and RestedXP
// tags the bot doesn't do; and each far step's trip as the bot would route it (no route, a ride ending in the water,
// a walk past creatures well above the guide's level). Findings go to the guide log (playerbots.guide), one line
// each; the counts come back.
// Reads only data loaded at startup.
namespace GuideAudit
{
struct Summary
{
    uint32 guides{0};
    uint32 steps{0};
    uint32 findings{0};
    std::map<std::string, uint32> byKind;
    std::set<std::string> seen;  // findings already logged: each is counted once
};

Summary Run(Player* bot, std::vector<Guide const*> const& guides);
// Adds to summary (one summary over several characters: what they share is logged once)
void Run(Player* bot, std::vector<Guide const*> const& guides, Summary& summary);
// ".guide audit <character | all> [guide]" (console or GM): the character's route (or one guide), checked as
// "guide audit" does; "all": one online character for each race and class (random bots do), findings shared by
// several logged once
bool HandleCommand(ChatHandler* handler, std::string const& args);
}  // namespace GuideAudit

#endif
