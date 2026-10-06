/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROTATIONBOTSUPPORT_H
#define PLAYERBOTS_ROTATIONBOTSUPPORT_H

// mod-rotation-bot is optional: with it installed, bots can fight with its profiles
// (AiPlayerbot.UseRotationBot) in place of the class strategies.
#if __has_include("RotationBotBridge.h")
#include "RotationBotBridge.h"
#define PLAYERBOTS_ROTATION_BOT 1
#endif

class Player;
class SpellInfo;

namespace PlayerbotRotation
{
    // AiPlayerbot.UseRotationBot is on, mod-rotation-bot is installed and a profile fits the bot.
    bool IsUsed(Player* bot);

    // Casting this spell would undo a stance, form, buff or pet the bot's profile picks.
    bool Conflicts(Player* bot, SpellInfo const* spellInfo);

    // The bot's profile casts a stance, form or Shadowform.
    bool UsesShapeshift(Player* bot);

    // The bot tanks: by spec, and for druids by the Thick Hide talent rather than the current form,
    // so a druid in Bear Form for want of Cat Form still moves to Cat Form once it has it.
    bool IsTank(Player* bot);
}

#endif
