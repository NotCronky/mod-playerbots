/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PanelProtocol.h"
#include "gtest/gtest.h"

using namespace PanelProtocol;

TEST(PanelProtocol, StripPrefixTakesOnlyThePanelsMessages)
{
    EXPECT_EQ(StripPrefix("PBP\tHELLO"), "HELLO");
    EXPECT_FALSE(StripPrefix("DC\tCMD\tsep\tus").has_value());
    EXPECT_FALSE(StripPrefix("PBPX\tHELLO").has_value());
    EXPECT_FALSE(StripPrefix("PBP").has_value());
}

TEST(PanelProtocol, SplitFieldsAcceptsEverySeparatorAndKeepsEmptyFields)
{
    std::vector<std::string> const expected = {"DO", "7", "all", "cmd", "follow"};
    EXPECT_EQ(SplitFields("DO\t7\tall\tcmd\tfollow"), expected);
    EXPECT_EQ(SplitFields("DO\x1F"
                          "7\x1F"
                          "all\x1F"
                          "cmd\x1F"
                          "follow"),
              expected);
    EXPECT_EQ(SplitFields("DO~7~all~cmd~follow"), expected);
    EXPECT_EQ(SplitFields("ERR\t\tx"), (std::vector<std::string>{"ERR", "", "x"}));
}

TEST(PanelProtocol, EncodeKeepsThePrefixTabAndSeparatesFieldsForHermesProxy)
{
    EXPECT_EQ(Encode("HELLO", {"1", "vanilla"}),
              "PBP\tHELLO\x1F"
              "1\x1Fvanilla");
    EXPECT_EQ(Encode("ROSTER"), "PBP\tROSTER");
    // A tab or separator inside a field would split it on the client.
    EXPECT_EQ(Encode("BOT", {"a\tb\x1F"
                             "c"}),
              "PBP\tBOT\x1F"
              "a b c");
}

TEST(PanelProtocol, ParseTargetReadsBotsGroupsRolesAndAll)
{
    EXPECT_EQ(ParseTarget("all")->kind, TargetKind::All);
    EXPECT_EQ(ParseTarget("1234")->value, 1234u);
    EXPECT_EQ(ParseTarget("group:3")->value, 3u);
    EXPECT_EQ(ParseTarget("role:heal")->role, Role::Heal);
    EXPECT_FALSE(ParseTarget("group:9").has_value());
    EXPECT_FALSE(ParseTarget("group:0").has_value());
    EXPECT_FALSE(ParseTarget("role:healer").has_value());
    EXPECT_FALSE(ParseTarget("0").has_value());
    EXPECT_FALSE(ParseTarget("12ab").has_value());
}

TEST(PanelProtocol, GearProfileDefaultsToUncommonAtTheCapUpgradingOnly)
{
    GearProfile const profile = *ParseGearProfile("");
    EXPECT_EQ(profile.quality, 2u);
    EXPECT_EQ(profile.mode, IlvlMode::Cap);
    EXPECT_FALSE(profile.fresh);
}

TEST(PanelProtocol, GearProfileRoundTrips)
{
    GearProfile profile;
    profile.quality = 3;
    profile.mode = IlvlMode::Number;
    profile.ilvl = 60;
    profile.fresh = true;
    std::string const text = FormatGearProfile(profile);
    EXPECT_EQ(text, "q=3,ilvl=60,fresh=1");

    GearProfile const parsed = *ParseGearProfile(text);
    EXPECT_EQ(parsed.quality, 3u);
    EXPECT_EQ(parsed.mode, IlvlMode::Number);
    EXPECT_EQ(parsed.ilvl, 60u);
    EXPECT_TRUE(parsed.fresh);
    EXPECT_EQ(ParseGearProfile("ilvl=match")->mode, IlvlMode::Match);
}

TEST(PanelProtocol, GearProfileRejectsWhatTheServerWouldMisread)
{
    EXPECT_FALSE(ParseGearProfile("q=1").has_value());
    EXPECT_FALSE(ParseGearProfile("q=5").has_value());
    EXPECT_FALSE(ParseGearProfile("ilvl=0").has_value());
    EXPECT_FALSE(ParseGearProfile("fresh=yes").has_value());
    EXPECT_FALSE(ParseGearProfile("bis=1").has_value());
    EXPECT_FALSE(ParseGearProfile("q3").has_value());
}

TEST(PanelProtocol, ClampGearHoldsTheProfileToTheRealmsCaps)
{
    GearProfile epic;
    epic.quality = 4;
    EXPECT_EQ(ClampGear(epic, 3, 78, 0)->quality, 3u);
    EXPECT_EQ(ClampGear(epic, 3, 78, 0)->ilvl, 78u);

    GearProfile number;
    number.mode = IlvlMode::Number;
    number.ilvl = 200;
    EXPECT_EQ(ClampGear(number, 4, 125, 0)->ilvl, 125u);
    EXPECT_EQ(ClampGear(number, 4, 0, 0)->ilvl, 200u);

    GearProfile match;
    match.mode = IlvlMode::Match;
    EXPECT_EQ(ClampGear(match, 4, 78, 65)->ilvl, 65u);
    EXPECT_EQ(ClampGear(match, 4, 78, 90)->ilvl, 78u);
    EXPECT_FALSE(ClampGear(match, 4, 78, 0).has_value());

    GearProfile cap;
    EXPECT_EQ(ClampGear(cap, 4, 0, 0)->ilvl, 0u);
}

TEST(PanelProtocol, BotCommandsRoundTrip)
{
    BotCommand setRole;
    setRole.request = 12;
    setRole.op = BotOp::SetRole;
    setRole.specNo = 2;
    std::string const text = FormatBotCommand(setRole);
    EXPECT_EQ(text, "panel 12 setrole 2 q=2,ilvl=cap,fresh=0");

    BotCommand const parsed = *ParseBotCommand(text.substr(6));
    EXPECT_EQ(parsed.request, 12u);
    EXPECT_EQ(parsed.op, BotOp::SetRole);
    EXPECT_EQ(parsed.specNo, 2);

    EXPECT_EQ(ParseBotCommand("3 maintain")->op, BotOp::Maintain);
    EXPECT_EQ(ParseBotCommand("3 refresh q=4,ilvl=match,fresh=0")->gear.mode, IlvlMode::Match);
    EXPECT_FALSE(ParseBotCommand("3 setrole q=2").has_value());
    EXPECT_FALSE(ParseBotCommand("x gear q=2").has_value());
    EXPECT_FALSE(ParseBotCommand("3 dance").has_value());
}
