/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PanelProtocol.h"

#include <algorithm>
#include <charconv>

namespace
{
constexpr uint32_t QUALITY_UNCOMMON = 2;
constexpr uint32_t QUALITY_EPIC = 4;
constexpr uint32_t MAX_SUBGROUP = 8;

std::optional<uint32_t> ParseNumber(std::string_view text)
{
    uint32_t value = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size() || text.empty())
        return std::nullopt;

    return value;
}

std::vector<std::string_view> SplitOn(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (start <= text.size())
    {
        size_t const end = std::min(text.find(separator, start), text.size());
        if (end > start)
            parts.push_back(text.substr(start, end - start));

        start = end + 1;
    }

    return parts;
}
}  // namespace

namespace PanelProtocol
{
std::optional<std::string> StripPrefix(std::string_view message)
{
    if (message.size() <= PREFIX.size() || message.substr(0, PREFIX.size()) != PREFIX || message[PREFIX.size()] != '\t')
        return std::nullopt;

    return std::string(message.substr(PREFIX.size() + 1));
}

std::vector<std::string> SplitFields(std::string_view payload)
{
    std::vector<std::string> fields;
    std::string field;
    for (char c : payload)
    {
        if (c == '\t' || c == FIELD_SEPARATOR || c == '~')
        {
            fields.push_back(field);
            field.clear();
        }
        else
            field += c;
    }

    fields.push_back(field);
    return fields;
}

std::string Encode(std::string_view verb, std::vector<std::string> const& fields)
{
    std::string out(PREFIX);
    out += '\t';
    out += verb;
    for (std::string const& field : fields)
    {
        out += FIELD_SEPARATOR;
        // A field can't carry the separators or a tab (HermesProxy would join it with spaces).
        for (char c : field)
            out += (c == FIELD_SEPARATOR || c == '\t') ? ' ' : c;
    }

    return out;
}

std::string_view RoleName(Role role)
{
    switch (role)
    {
        case Role::Tank:
            return "tank";
        case Role::Heal:
            return "heal";
        default:
            return "dps";
    }
}

std::optional<Role> ParseRole(std::string_view text)
{
    if (text == "tank")
        return Role::Tank;

    if (text == "heal")
        return Role::Heal;

    if (text == "dps")
        return Role::Dps;

    return std::nullopt;
}

std::optional<Target> ParseTarget(std::string_view text)
{
    Target target;
    if (text == "all")
        return target;

    if (text.substr(0, 5) == "role:")
    {
        std::optional<Role> const role = ParseRole(text.substr(5));
        if (!role)
            return std::nullopt;

        target.kind = TargetKind::Role;
        target.role = *role;
        return target;
    }

    if (text.substr(0, 6) == "group:")
    {
        std::optional<uint32_t> const group = ParseNumber(text.substr(6));
        if (!group || *group < 1 || *group > MAX_SUBGROUP)
            return std::nullopt;

        target.kind = TargetKind::Group;
        target.value = *group;
        return target;
    }

    std::optional<uint32_t> const guid = ParseNumber(text);
    if (!guid || !*guid)
        return std::nullopt;

    target.kind = TargetKind::Bot;
    target.value = *guid;
    return target;
}

std::optional<GearProfile> ParseGearProfile(std::string_view text)
{
    GearProfile profile;
    for (std::string_view part : SplitOn(text, ','))
    {
        size_t const equals = part.find('=');
        if (equals == std::string_view::npos)
            return std::nullopt;

        std::string_view const key = part.substr(0, equals);
        std::string_view const value = part.substr(equals + 1);
        if (key == "q")
        {
            std::optional<uint32_t> const quality = ParseNumber(value);
            if (!quality || *quality < QUALITY_UNCOMMON || *quality > QUALITY_EPIC)
                return std::nullopt;

            profile.quality = *quality;
        }
        else if (key == "ilvl")
        {
            if (value == "cap")
                profile.mode = IlvlMode::Cap;
            else if (value == "match")
                profile.mode = IlvlMode::Match;
            else
            {
                std::optional<uint32_t> const ilvl = ParseNumber(value);
                if (!ilvl || !*ilvl)
                    return std::nullopt;

                profile.mode = IlvlMode::Number;
                profile.ilvl = *ilvl;
            }
        }
        else if (key == "fresh")
        {
            if (value != "0" && value != "1")
                return std::nullopt;

            profile.fresh = value == "1";
        }
        else
            return std::nullopt;
    }

    return profile;
}

std::string FormatGearProfile(GearProfile const& profile)
{
    std::string ilvl = "cap";
    if (profile.mode == IlvlMode::Match)
        ilvl = "match";
    else if (profile.mode == IlvlMode::Number)
        ilvl = std::to_string(profile.ilvl);

    return "q=" + std::to_string(profile.quality) + ",ilvl=" + ilvl + ",fresh=" + (profile.fresh ? "1" : "0");
}

std::optional<GearRequest> ClampGear(GearProfile const& profile, uint32_t qualityCap, uint32_t ilvlCap,
                                     uint32_t matchIlvl)
{
    GearRequest request;
    request.quality = std::min(profile.quality, qualityCap);

    uint32_t wanted = 0;
    switch (profile.mode)
    {
        case IlvlMode::Cap:
            wanted = ilvlCap;
            break;
        case IlvlMode::Match:
            if (!matchIlvl)
                return std::nullopt;

            wanted = matchIlvl;
            break;
        case IlvlMode::Number:
            wanted = profile.ilvl;
            break;
    }

    request.ilvl = ilvlCap ? std::min(wanted, ilvlCap) : wanted;
    return request;
}

std::string FormatBotCommand(BotCommand const& command)
{
    std::string out = "panel " + std::to_string(command.request) + " ";
    switch (command.op)
    {
        case BotOp::SetRole:
            return out + "setrole " + std::to_string(command.specNo) + " " + FormatGearProfile(command.gear);
        case BotOp::Gear:
            return out + "gear " + FormatGearProfile(command.gear);
        case BotOp::Maintain:
            return out + "maintain";
        case BotOp::Refresh:
            return out + "refresh " + FormatGearProfile(command.gear);
    }

    return out;
}

std::optional<BotCommand> ParseBotCommand(std::string_view parameters)
{
    std::vector<std::string_view> const words = SplitOn(parameters, ' ');
    if (words.size() < 2)
        return std::nullopt;

    BotCommand command;
    std::optional<uint32_t> const request = ParseNumber(words[0]);
    if (!request)
        return std::nullopt;

    command.request = *request;
    std::string_view const op = words[1];
    size_t gearWord = 0;
    if (op == "setrole" && words.size() == 4)
    {
        std::optional<uint32_t> const specNo = ParseNumber(words[2]);
        if (!specNo)
            return std::nullopt;

        command.op = BotOp::SetRole;
        command.specNo = static_cast<int32_t>(*specNo);
        gearWord = 3;
    }
    else if (op == "gear" && words.size() == 3)
    {
        command.op = BotOp::Gear;
        gearWord = 2;
    }
    else if (op == "refresh" && words.size() == 3)
    {
        command.op = BotOp::Refresh;
        gearWord = 2;
    }
    else if (op == "maintain" && words.size() == 2)
    {
        command.op = BotOp::Maintain;
        return command;
    }
    else
        return std::nullopt;

    std::optional<GearProfile> const gear = ParseGearProfile(words[gearWord]);
    if (!gear)
        return std::nullopt;

    command.gear = *gear;
    return command;
}
}  // namespace PanelProtocol
