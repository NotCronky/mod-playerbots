/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// The bot panel's server side (Playerbots.PanelApi.Enable): answers the panel's addon messages and the .pbp command
// (PanelProtocol.h), and pushes the roster and the bots' state to the players who have the panel open. Everything
// here runs on the world thread (chat packets, chat commands and the world update), while the maps are idle; the
// work on a bot itself (talents, gear, maintenance) goes to the bot as a "panel" command, run on its own update.

#include <algorithm>
#include <array>
#include <unordered_map>

#include "AiFactory.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "PanelProtocol.h"
#include "PanelSend.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotEra.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TrainerAction.h"

using namespace Acore::ChatCommands;

namespace
{
constexpr uint32 STATE_INTERVAL_MS = 1000;
constexpr uint8 CLASS_UNUSED = 10;  // the class ids skip 10

// Class -> role of each talent tree (a druid's feral tree is the tank's; cat builds say so in their name).
constexpr PanelProtocol::Role TREE_ROLES[MAX_CLASSES][3] = {
    {},
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Tank},   // warrior
    {PanelProtocol::Role::Heal, PanelProtocol::Role::Tank, PanelProtocol::Role::Dps},  // paladin
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Dps},    // hunter
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Dps},    // rogue
    {PanelProtocol::Role::Heal, PanelProtocol::Role::Heal, PanelProtocol::Role::Dps},  // priest
    {PanelProtocol::Role::Tank, PanelProtocol::Role::Dps, PanelProtocol::Role::Dps},   // death knight
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Heal},   // shaman
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Dps},    // mage
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Dps, PanelProtocol::Role::Dps},    // warlock
    {},
    {PanelProtocol::Role::Dps, PanelProtocol::Role::Tank, PanelProtocol::Role::Heal},  // druid
};

// What the panel last sent a watching player, so only changes go out.
struct Watch
{
    std::string roster;
    std::unordered_map<uint32, std::string> states;
};

std::unordered_map<ObjectGuid, Watch> watches;
uint32 stateTimer = 0;

std::string EraName()
{
    if (PlayerbotEra::IsVanilla())
        return "vanilla";

    return PlayerbotEra::IsTbc() ? "tbc" : "wrath";
}

bool ClassExists(uint8 cls)
{
    if (cls == CLASS_DEATH_KNIGHT)
        return !PlayerbotEra::IsClassic();

    return cls && cls < MAX_CLASSES && cls != CLASS_UNUSED;
}

std::string ClassToken(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR:
            return "warrior";
        case CLASS_PALADIN:
            return "paladin";
        case CLASS_HUNTER:
            return "hunter";
        case CLASS_ROGUE:
            return "rogue";
        case CLASS_PRIEST:
            return "priest";
        case CLASS_DEATH_KNIGHT:
            return "dk";
        case CLASS_SHAMAN:
            return "shaman";
        case CLASS_MAGE:
            return "mage";
        case CLASS_WARLOCK:
            return "warlock";
        case CLASS_DRUID:
            return "druid";
        default:
            return "";
    }
}

PanelProtocol::Role BotRole(Player* bot)
{
    if (PlayerbotAI::IsTank(bot, true))
        return PanelProtocol::Role::Tank;

    return PlayerbotAI::IsHeal(bot, true) ? PanelProtocol::Role::Heal : PanelProtocol::Role::Dps;
}

// The bots in the player's group, in group order.
std::vector<Player*> GroupBots(Player* player)
{
    std::vector<Player*> bots;
    Group* group = player->GetGroup();
    if (!group)
        return bots;

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member != player && GET_PLAYERBOT_AI(member))
            bots.push_back(member);
    }

    return bots;
}

uint32 Subgroup(Player* player, Player* bot)
{
    Group* group = player->GetGroup();
    return group ? group->GetMemberGroup(bot->GetGUID()) + 1 : 1;
}

std::vector<Player*> TargetBots(Player* player, PanelProtocol::Target const& target)
{
    std::vector<Player*> bots;
    for (Player* bot : GroupBots(player))
    {
        bool matches = true;
        switch (target.kind)
        {
            case PanelProtocol::TargetKind::Bot:
                matches = bot->GetGUID().GetCounter() == target.value;
                break;
            case PanelProtocol::TargetKind::Group:
                matches = Subgroup(player, bot) == target.value;
                break;
            case PanelProtocol::TargetKind::Role:
                matches = BotRole(bot) == target.role;
                break;
            case PanelProtocol::TargetKind::All:
                break;
        }

        if (matches)
            bots.push_back(bot);
    }

    return bots;
}

// A build's points per tree at the highest level the config has it for.
std::array<uint32, 3> BuildSplit(uint8 cls, uint32 specNo)
{
    std::array<uint32, 3> split = {0, 0, 0};
    for (int32 level = MAX_LEVEL - 1; level > 0; --level)
    {
        auto const& order = sPlayerbotAIConfig.ParsedSpecLinkOrder[cls][specNo][level];
        if (order.empty())
            continue;

        for (std::vector<uint32> const& talent : order)
            if (talent.size() > 3 && talent[0] < split.size())
                split[talent[0]] += talent[3];

        break;
    }

    return split;
}

PanelProtocol::Role BuildRole(uint8 cls, uint32 specNo, std::array<uint32, 3> const& split)
{
    uint32 const tree = static_cast<uint32>(std::max_element(split.begin(), split.end()) - split.begin());
    std::string const& name = sPlayerbotAIConfig.PremadeSpecName[cls][specNo];
    if (cls == CLASS_DRUID && tree == 1 && name.find("cat") != std::string::npos)
        return PanelProtocol::Role::Dps;

    return TREE_ROLES[cls][tree];
}

int32 FindBuild(uint8 cls, std::string const& name)
{
    for (uint32 specNo = 0; specNo < MAX_SPECNO; ++specNo)
    {
        std::string const& spec = sPlayerbotAIConfig.PremadeSpecName[cls][specNo];
        if (spec.empty())
            break;

        if (spec == name)
            return static_cast<int32>(specNo);
    }

    return -1;
}

// The combat switches the panel shows: a(oe), c(rowd control), b(oost, the cooldowns).
std::string CombatSwitches(Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    std::string switches;
    if (!botAI)
        return switches;

    if (botAI->HasStrategy("aoe", BOT_STATE_COMBAT))
        switches += 'a';

    if (botAI->HasStrategy("cc", BOT_STATE_COMBAT))
        switches += 'c';

    if (botAI->HasStrategy("boost", BOT_STATE_COMBAT))
        switches += 'b';

    return switches;
}

std::vector<std::string> BotLine(Player* player, Player* bot)
{
    return {std::to_string(bot->GetGUID().GetCounter()),
            bot->GetName(),
            std::to_string(bot->getClass()),
            std::to_string(bot->getRace()),
            std::to_string(bot->GetLevel()),
            std::string(PanelProtocol::RoleName(BotRole(bot))),
            AiFactory::GetPlayerSpecName(bot),
            std::to_string(GetWornItemLevelAverage(bot)),
            std::to_string(Subgroup(player, bot)),
            CombatSwitches(bot)};
}

std::vector<std::string> StateLine(Player* player, Player* bot)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    std::string flags;
    if (!bot->IsAlive())
        flags += 'd';

    if (bot->IsInCombat())
        flags += 'c';

    if (bot->GetMapId() != player->GetMapId())
        flags += 'f';

    std::string mode = "follow";
    if (botAI && botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        mode = "stay";
    else if (botAI && botAI->HasStrategy("guard", BOT_STATE_NON_COMBAT))
        mode = "guard";

    Powers const power = bot->getPowerType();
    uint32 const maxPower = bot->GetMaxPower(power);
    return {std::to_string(bot->GetGUID().GetCounter()),
            std::to_string(static_cast<uint32>(bot->GetHealthPct())),
            std::to_string(power),
            std::to_string(maxPower ? bot->GetPower(power) * 100 / maxPower : 0),
            flags,
            mode};
}

std::string Join(std::vector<std::string> const& fields)
{
    std::string out;
    for (std::string const& field : fields)
        out += field + '\x1F';

    return out;
}

// The roster when it changed since the last one sent (always with force).
void SendRoster(Player* player, bool force)
{
    std::vector<std::vector<std::string>> lines;
    std::string signature;
    for (Player* bot : GroupBots(player))
    {
        lines.push_back(BotLine(player, bot));
        signature += Join(lines.back()) + '\n';
    }

    Watch* watch = nullptr;
    auto itr = watches.find(player->GetGUID());
    if (itr != watches.end())
        watch = &itr->second;

    if (!force && watch && watch->roster == signature)
        return;

    if (watch)
        watch->roster = signature;

    PanelSend::Send(player, "ROSTER", {"begin", std::to_string(lines.size())});
    for (std::vector<std::string> const& line : lines)
        PanelSend::Send(player, "BOT", line);

    PanelSend::Send(player, "ROSTER", {"end"});
}

// The bots' state lines that changed (all of them with force).
void SendStates(Player* player, Watch& watch, bool force)
{
    std::unordered_map<uint32, std::string> current;
    for (Player* bot : GroupBots(player))
    {
        std::vector<std::string> const line = StateLine(player, bot);
        std::string const joined = Join(line);
        uint32 const guid = bot->GetGUID().GetCounter();
        current[guid] = joined;
        auto const itr = watch.states.find(guid);
        if (force || itr == watch.states.end() || itr->second != joined)
            PanelSend::Send(player, "ST", line);
    }

    watch.states = std::move(current);
}

void SendHello(Player* player)
{
    std::string classes;
    for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
        if (ClassExists(cls))
            classes += (classes.empty() ? "" : ",") + std::to_string(cls);

    PanelSend::Send(
        player, "HELLO",
        {std::to_string(PanelProtocol::VERSION), EraName(), classes,
         std::to_string(sPlayerbotAIConfig.AutoGearQualityLimit), std::to_string(sPlayerbotAIConfig.AutoGearScoreLimit),
         sPlayerbotAIConfig.AutoGearCommand ? "1" : "0", sPlayerbotAIConfig.MaintenanceCommand ? "1" : "0",
         std::to_string(sPlayerbotAIConfig.MaxAddedBots)});
}

// Every class's builds: "SPEC <class> <number> <name> <a-b-c> <role>", then "SPECS end".
void SendSpecs(Player* player)
{
    for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
    {
        if (!ClassExists(cls))
            continue;

        for (uint32 specNo = 0; specNo < MAX_SPECNO; ++specNo)
        {
            std::string const& name = sPlayerbotAIConfig.PremadeSpecName[cls][specNo];
            if (name.empty())
                break;

            std::array<uint32, 3> const split = BuildSplit(cls, specNo);
            PanelSend::Send(player, "SPEC",
                            {std::to_string(cls), std::to_string(specNo), name,
                             Acore::StringFormat("{}-{}-{}", split[0], split[1], split[2]),
                             std::string(PanelProtocol::RoleName(BuildRole(cls, specNo, split)))});
        }
    }

    PanelSend::Send(player, "SPECS", {"end"});
}

void SendResult(Player* player, std::string const& request, bool ok, std::string const& detail)
{
    PanelSend::Send(player, ok ? "OK" : "ERR", {request, detail});
}

// "DO <request> <target> <action> [args]": setrole <build> <gear>, gear <gear>, refresh <gear>, maintain, or
// cmd <chat command>.
void HandleDo(Player* player, std::vector<std::string> const& fields)
{
    std::string const request = fields.size() > 1 ? fields[1] : "0";
    if (fields.size() < 4)
        return SendResult(player, request, false, "incomplete order");

    std::optional<PanelProtocol::Target> const target = PanelProtocol::ParseTarget(fields[2]);
    if (!target)
        return SendResult(player, request, false, "unknown target " + fields[2]);

    std::vector<Player*> const bots = TargetBots(player, *target);
    if (bots.empty())
        return SendResult(player, request, false, "no bots in your group match");

    std::string const& action = fields[3];
    uint32 const requestNumber = static_cast<uint32>(std::strtoul(request.c_str(), nullptr, 10));
    if (action == "cmd")
    {
        if (fields.size() < 5 || fields[4].empty())
            return SendResult(player, request, false, "no command");

        // "#a " answers on the addon channel, so the bots' replies stay out of chat.
        for (Player* bot : bots)
            GET_PLAYERBOT_AI(bot)->HandleCommand(CHAT_MSG_WHISPER, "#a " + fields[4], player);

        return SendResult(player, request, true, std::to_string(bots.size()));
    }

    PanelProtocol::BotCommand command;
    command.request = requestNumber;
    std::string const* gearText = nullptr;
    if (action == "setrole" && fields.size() >= 6)
    {
        command.op = PanelProtocol::BotOp::SetRole;
        gearText = &fields[5];
    }
    else if ((action == "gear" || action == "refresh") && fields.size() >= 5)
    {
        command.op = action == "gear" ? PanelProtocol::BotOp::Gear : PanelProtocol::BotOp::Refresh;
        gearText = &fields[4];
    }
    else if (action == "maintain")
        command.op = PanelProtocol::BotOp::Maintain;
    else
        return SendResult(player, request, false, "unknown order " + action);

    if (gearText)
    {
        std::optional<PanelProtocol::GearProfile> const gear = PanelProtocol::ParseGearProfile(*gearText);
        if (!gear)
            return SendResult(player, request, false, "unreadable gear profile");

        command.gear = *gear;
    }

    uint32 sent = 0;
    for (Player* bot : bots)
    {
        if (command.op == PanelProtocol::BotOp::SetRole)
        {
            command.specNo = FindBuild(bot->getClass(), fields[4]);
            if (command.specNo < 0)
            {
                PanelSend::Send(player, "PROGRESS",
                                {request, std::to_string(bot->GetGUID().GetCounter()), "talents", "err",
                                 "no build " + fields[4] + " for this class"});
                continue;
            }
        }

        GET_PLAYERBOT_AI(bot)->HandleCommand(CHAT_MSG_WHISPER, PanelProtocol::FormatBotCommand(command), player);
        ++sent;
    }

    SendResult(player, request, sent > 0, std::to_string(sent));
}

// "ADD <request> <class>" and "REMOVE <request> <bot guid>", through the .playerbots bot commands.
void HandleBotCommand(Player* player, std::vector<std::string> const& fields, bool add)
{
    std::string const request = fields.size() > 1 ? fields[1] : "0";
    PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
    if (!mgr || fields.size() < 3)
        return SendResult(player, request, false, mgr ? "incomplete order" : "you can't control bots yet");

    std::string args;
    if (add)
    {
        uint8 const cls = static_cast<uint8>(std::strtoul(fields[2].c_str(), nullptr, 10));
        if (!ClassExists(cls))
            return SendResult(player, request, false, "that class doesn't exist on this realm");

        args = "addclass " + ClassToken(cls);
    }
    else
    {
        std::optional<PanelProtocol::Target> const target = PanelProtocol::ParseTarget(fields[2]);
        std::vector<Player*> const bots = target && target->kind == PanelProtocol::TargetKind::Bot
                                              ? TargetBots(player, *target)
                                              : std::vector<Player*>();
        if (bots.empty())
            return SendResult(player, request, false, "no such bot in your group");

        args = "remove " + bots.front()->GetName();
    }

    std::string detail;
    for (std::string const& message : mgr->HandlePlayerbotCommand(args.c_str(), player))
        detail += (detail.empty() ? "" : "; ") + message;

    SendResult(player, request, true, detail);
}

void Handle(Player* player, std::string const& payload)
{
    std::vector<std::string> const fields = PanelProtocol::SplitFields(payload);
    std::string const& verb = fields.front();
    if (!sPlayerbotAIConfig.PanelApiEnable)
        return PanelSend::Send(player, "ERR", {"0", "the bot panel is turned off on this realm"});

    if (verb == "HELLO")
    {
        SendHello(player);
        SendRoster(player, true);
    }
    else if (verb == "ROSTER")
        SendRoster(player, true);
    else if (verb == "SPECS")
        SendSpecs(player);
    else if (verb == "WATCH")
    {
        if (fields.size() > 1 && fields[1] == "1")
        {
            Watch& watch = watches[player->GetGUID()];
            SendRoster(player, true);
            SendStates(player, watch, true);
        }
        else
            watches.erase(player->GetGUID());
    }
    else if (verb == "DO")
        HandleDo(player, fields);
    else if (verb == "ADD" || verb == "REMOVE")
        HandleBotCommand(player, fields, verb == "ADD");
    else
        PanelSend::Send(player, "ERR", {"0", "unknown request " + verb});
}

bool IsPanelMessage(uint32 type, uint32 lang, std::string const& msg)
{
    if (lang != LANG_ADDON)
        return false;

    if (type != CHAT_MSG_PARTY && type != CHAT_MSG_PARTY_LEADER && type != CHAT_MSG_RAID &&
        type != CHAT_MSG_RAID_LEADER && type != CHAT_MSG_WHISPER)
        return false;

    return PanelProtocol::StripPrefix(msg).has_value();
}
}  // namespace

class PlayerbotsPanelPlayerScript : public PlayerScript
{
public:
    PlayerbotsPanelPlayerScript()
        : PlayerScript("PlayerbotsPanelPlayerScript",
                       {PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
                        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT, PLAYERHOOK_ON_LOGOUT})
    {
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override
    {
        if (!player || !IsPanelMessage(type, lang, msg))
            return;

        Handle(player, *PanelProtocol::StripPrefix(msg));
    }

    // The panel's messages are answered above and never relayed to the group or a whisper target.
    bool OnPlayerCanUseChat(Player* /*player*/, uint32 type, uint32 lang, std::string& msg, Group* /*group*/) override
    {
        return !IsPanelMessage(type, lang, msg);
    }

    bool OnPlayerCanUseChat(Player* /*player*/, uint32 type, uint32 lang, std::string& msg,
                            Player* /*receiver*/) override
    {
        return !IsPanelMessage(type, lang, msg);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (player)
            watches.erase(player->GetGUID());
    }
};

class PlayerbotsPanelWorldScript : public WorldScript
{
public:
    PlayerbotsPanelWorldScript() : WorldScript("PlayerbotsPanelWorldScript", {WORLDHOOK_ON_UPDATE}) {}

    void OnUpdate(uint32 diff) override
    {
        if (watches.empty())
            return;

        stateTimer += diff;
        if (stateTimer < STATE_INTERVAL_MS)
            return;

        stateTimer = 0;
        for (auto itr = watches.begin(); itr != watches.end();)
        {
            Player* player = ObjectAccessor::FindPlayer(itr->first);
            if (!player || !player->IsInWorld())
            {
                itr = watches.erase(itr);
                continue;
            }

            SendRoster(player, false);
            SendStates(player, itr->second, false);
            ++itr;
        }
    }
};

// Without a group the panel has no addon channel (and the client won't send one to its own player), so it sends the
// same payload as ".pbp <payload>", whispered to itself.
class PlayerbotsPanelCommandScript : public CommandScript
{
public:
    PlayerbotsPanelCommandScript() : CommandScript("PlayerbotsPanelCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable = {
            {"pbp", HandlePanelCommand, SEC_PLAYER, Console::No},
        };
        return commandTable;
    }

    static bool HandlePanelCommand(ChatHandler* handler, Tail payload)
    {
        Player* player = handler->GetPlayer();
        if (!player || payload.empty())
            return false;

        Handle(player, std::string(payload));
        return true;
    }
};

void AddPlayerbotsPanelScripts()
{
    new PlayerbotsPanelPlayerScript();
    new PlayerbotsPanelWorldScript();
    new PlayerbotsPanelCommandScript();
}
