/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEAUDIT_H
#define PLAYERBOTS_GUIDEAUDIT_H

#include <map>
#include <string>
#include <vector>

#include "Define.h"

class Player;
struct Guide;

// "guide audit": checks the route against the realm's data for one character, before it is played. Each step that
// applies to the character (race, class, faction; the level and quest-state checks are left out): its quests,
// givers, enders, objectives and their creatures, objects and item sources, item uses and services, and RestedXP
// tags the bot doesn't do. Findings go to the guide log (playerbots.guide), one line each; the counts come back.
// Reads only data loaded at startup.
namespace GuideAudit
{
struct Summary
{
    uint32 guides{0};
    uint32 steps{0};
    uint32 findings{0};
    std::map<std::string, uint32> byKind;
};

Summary Run(Player* bot, std::vector<Guide const*> const& guides);
}  // namespace GuideAudit

#endif
