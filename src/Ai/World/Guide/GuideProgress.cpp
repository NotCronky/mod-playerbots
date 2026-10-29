/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GuideProgress.h"

#include <algorithm>

#include "GuideInfo.h"
#include "GuideMgr.h"
#include "GuideRules.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "PlayerbotsDatabase.h"
#include "Timer.h"

void GuideProgress::Save(PlayerbotAI* botAI)
{
    GuideInfo& info = botAI->guideInfo;
    if (!info.guide || info.step >= info.guide->steps.size())
        return;
    uint32 number = info.guide->steps[info.step].number;
    if (info.guide == info.savedGuide && number == info.savedStep && info.active == info.savedActive)
        return;
    info.savedGuide = info.guide;
    info.savedStep = number;
    info.savedActive = info.active;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_REP_GUIDE_PROGRESS);
    stmt->SetData(0, botAI->GetBot()->GetGUID().GetCounter());
    stmt->SetData(1, info.guide->name);
    stmt->SetData(2, number);
    stmt->SetData(3, uint8(info.active));
    PlayerbotsDatabase.Execute(stmt);
}

void GuideProgress::Restore(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    if (!IsSelfBot(bot) || !GuideMgr::instance().IsEnabled())
        return;

    PlayerbotsDatabasePreparedStatement* stmt = PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_GUIDE_PROGRESS);
    stmt->SetData(0, bot->GetGUID().GetCounter());
    PreparedQueryResult result = PlayerbotsDatabase.Query(stmt);
    if (!result)
        return;
    Field* fields = result->Fetch();
    std::string const name = fields[0].Get<std::string>();
    uint32 const number = fields[1].Get<uint32>();
    bool const active = fields[2].Get<uint8>() != 0;

    // The guide data may have been regenerated since: the first step from that number on
    Guide const* guide = GuideMgr::instance().GetGuide(name);
    if (!guide || guide->steps.empty() || !guide->condition.Holds(bot))
    {
        LOG_INFO("playerbots.guide", "{}: saved guide {} step {} not on this route; not restored", bot->GetName(), name,
                 number);
        return;
    }
    auto step = std::find_if(guide->steps.begin(), guide->steps.end(),
                             [&](GuideStep const& at) { return at.number >= number; });
    if (step == guide->steps.end())
        step = guide->steps.end() - 1;

    GuideInfo& info = botAI->guideInfo;
    info.Reset();
    info.trip.going = false;
    info.trip.Clear();
    info.guide = guide;
    info.step = uint32(step - guide->steps.begin());
    info.savedGuide = guide;
    info.savedStep = number;
    info.savedActive = active;
    LOG_INFO("playerbots.guide", "{}: restored {} step {} ({})", bot->GetName(), guide->name, step->number,
             active ? "on" : "off");
    if (!active)
        return;

    info.active = true;
    info.stepStart = getMSTime();
    botAI->ChangeStrategy("+guide", BOT_STATE_NON_COMBAT);
    botAI->ChangeStrategy("+guide death", BOT_STATE_DEAD);
    botAI->ChangeStrategy("+guide combat", BOT_STATE_COMBAT);
    botAI->TellMasterNoFacing(PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "guide_restored", "Guide: back where you left off. %status", {{"%status", GuideRules::Describe(bot, info)}}));
}
