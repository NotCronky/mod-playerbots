/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PanelAction.h"

#include "Event.h"
#include "PanelSend.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "TrainerAction.h"

bool PanelAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::optional<PanelProtocol::BotCommand> const command = PanelProtocol::ParseBotCommand(event.getParam());
    if (!command)
    {
        Report(owner, 0, "command", false, "unreadable panel command");
        return false;
    }

    bool ok = true;
    switch (command->op)
    {
        case PanelProtocol::BotOp::SetRole:
            // Talents first: the gear and the maintenance follow the spec the bot ends up with.
            ok = SetTalents(owner, command->request, command->specNo) && Gear(owner, command->request, command->gear) &&
                 Maintain(owner, command->request);
            break;
        case PanelProtocol::BotOp::Gear:
            ok = Gear(owner, command->request, command->gear);
            break;
        case PanelProtocol::BotOp::Maintain:
            ok = Maintain(owner, command->request);
            break;
        case PanelProtocol::BotOp::Refresh:
            ok = Gear(owner, command->request, command->gear) && Maintain(owner, command->request);
            break;
    }

    PanelSend::Send(owner, "DONE",
                    {std::to_string(command->request), std::to_string(bot->GetGUID().GetCounter()), ok ? "ok" : "err"});
    return ok;
}

bool PanelAction::SetTalents(Player* owner, uint32 request, int32 specNo)
{
    uint8 const cls = bot->getClass();
    if (specNo < 0 || specNo >= MAX_SPECNO || sPlayerbotAIConfig.PremadeSpecName[cls][specNo].empty())
    {
        Report(owner, request, "talents", false, "no such build for this class");
        return false;
    }

    PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);
    PlayerbotFactory factory(bot, bot->GetLevel());
    factory.InitGlyphs(false);
    // The combat strategies, role and class buffs follow the new talents.
    botAI->ResetStrategies();
    Report(owner, request, "talents", true, sPlayerbotAIConfig.PremadeSpecName[cls][specNo]);
    return true;
}

bool PanelAction::Gear(Player* owner, uint32 request, PanelProtocol::GearProfile const& profile)
{
    if (!sPlayerbotAIConfig.AutoGearCommand)
    {
        Report(owner, request, "gear", false, "autogear is turned off on this realm");
        return false;
    }

    if (!sPlayerbotAIConfig.AutoGearCommandAltBots &&
        !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()))
    {
        Report(owner, request, "gear", false, "autogear is turned off for your own characters");
        return false;
    }

    uint32 const matchIlvl = profile.mode == PanelProtocol::IlvlMode::Match ? GetWornItemLevelAverage(owner) : 0;
    std::optional<PanelProtocol::GearRequest> const gear =
        PanelProtocol::ClampGear(profile, static_cast<uint32>(sPlayerbotAIConfig.AutoGearQualityLimit),
                                 static_cast<uint32>(sPlayerbotAIConfig.AutoGearScoreLimit), matchIlvl);
    if (!gear)
    {
        Report(owner, request, "gear", false, "you have no gear on to match");
        return false;
    }

    if (profile.fresh)
    {
        PlayerbotFactory::DestroyEquippedGear(bot);
        PlayerbotFactory::AutoGear(bot, gear->quality, gear->ilvl, false, sPlayerbotAIConfig.TwoRoundsGearInit);
    }
    else
        PlayerbotFactory::AutoGear(bot, gear->quality, gear->ilvl, true);

    Report(owner, request, "gear", true, std::to_string(gear->quality) + "," + std::to_string(gear->ilvl));
    return true;
}

bool PanelAction::Maintain(Player* owner, uint32 request)
{
    if (!sPlayerbotAIConfig.MaintenanceCommand)
    {
        Report(owner, request, "maintenance", false, "maintenance is turned off on this realm");
        return false;
    }

    MaintenanceAction::Maintain(botAI, bot);
    Report(owner, request, "maintenance", true);
    return true;
}

void PanelAction::Report(Player* owner, uint32 request, std::string const& step, bool ok, std::string const& detail)
{
    PanelSend::Send(
        owner, "PROGRESS",
        {std::to_string(request), std::to_string(bot->GetGUID().GetCounter()), step, ok ? "ok" : "err", detail});
}
