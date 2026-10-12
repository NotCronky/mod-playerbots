/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PANELPROTOCOL_H
#define PLAYERBOTS_PANELPROTOCOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The bot panel's addon protocol (prefix "PBP"). The client sends "PBP\t<verb><sep><field>..." as a party, raid or
// whisper addon message, or the same payload through the .pbp command when it has no group. Replies go out as
// "PBP\t<verb>\x1F<field>...": HermesProxy finds the addon prefix at the first tab and joins any later tabs with
// spaces, so fields after it are separated by \x1F. Std-only, so the tests build without the core.
namespace PanelProtocol
{
constexpr std::string_view PREFIX = "PBP";
constexpr char FIELD_SEPARATOR = '\x1F';
constexpr uint32_t VERSION = 1;

// The payload after "PBP\t", or nothing when the message isn't the panel's.
std::optional<std::string> StripPrefix(std::string_view message);

// Fields of an incoming payload. Tabs, \x1F and '~' all separate: tabs survive the client's addon messages, '~'
// the .pbp chat command (which can't carry control characters).
std::vector<std::string> SplitFields(std::string_view payload);

std::string Encode(std::string_view verb, std::vector<std::string> const& fields = {});

enum class Role
{
    Tank,
    Heal,
    Dps
};

std::string_view RoleName(Role role);
std::optional<Role> ParseRole(std::string_view text);

enum class TargetKind
{
    Bot,    // one bot, by its guid counter
    Group,  // a raid subgroup, 1-8
    Role,
    All
};

struct Target
{
    TargetKind kind = TargetKind::All;
    uint32_t value = 0;  // the bot's guid counter, or the subgroup (1-8)
    Role role = Role::Dps;
};

// "all", "role:tank|heal|dps", "group:<1-8>" or a guid counter.
std::optional<Target> ParseTarget(std::string_view text);

enum class IlvlMode
{
    Cap,    // the realm's item level cap (none when the realm has none)
    Match,  // the commanding player's average equipped item level
    Number
};

// How a bot is geared: the panel's default is uncommon items up to the realm's cap, upgrading what it wears.
struct GearProfile
{
    uint32_t quality = 2;  // ItemQualities: 2 uncommon, 3 rare, 4 epic
    IlvlMode mode = IlvlMode::Cap;
    uint32_t ilvl = 0;   // IlvlMode::Number only
    bool fresh = false;  // strip the equipped gear first and gear every slot from scratch
};

// "q=<n>,ilvl=<cap|match|n>,fresh=<0|1>", any of the parts, in any order; a missing part keeps the default.
std::optional<GearProfile> ParseGearProfile(std::string_view text);
std::string FormatGearProfile(GearProfile const& profile);

struct GearRequest
{
    uint32_t quality = 0;
    uint32_t ilvl = 0;  // 0 = no item level limit
};

// The profile held to the realm's caps (a cap of 0 means none). matchIlvl is the player's average for
// IlvlMode::Match; 0 there means nothing to match, and nothing is returned.
std::optional<GearRequest> ClampGear(GearProfile const& profile, uint32_t qualityCap, uint32_t ilvlCap,
                                     uint32_t matchIlvl);

// The bot-side command that runs a panel action on the bot's own update:
// "panel <request> setrole <spec number> <gear>", "panel <request> gear <gear>",
// "panel <request> maintain" and "panel <request> refresh <gear>".
enum class BotOp
{
    SetRole,
    Gear,
    Maintain,
    Refresh
};

struct BotCommand
{
    uint32_t request = 0;
    BotOp op = BotOp::Gear;
    int32_t specNo = -1;
    GearProfile gear;
};

std::string FormatBotCommand(BotCommand const& command);
std::optional<BotCommand> ParseBotCommand(std::string_view parameters);  // the text after "panel "
}  // namespace PanelProtocol

#endif
