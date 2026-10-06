/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RotationBotSupport.h"

#include "AiFactory.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"

bool PlayerbotRotation::IsUsed([[maybe_unused]] Player* bot)
{
#ifdef PLAYERBOTS_ROTATION_BOT
    return sPlayerbotAIConfig.useRotationBot && RotationBotBridge::HasProfile(bot);
#else
    return false;
#endif
}

bool PlayerbotRotation::Conflicts([[maybe_unused]] Player* bot, [[maybe_unused]] SpellInfo const* spellInfo)
{
#ifdef PLAYERBOTS_ROTATION_BOT
    return RotationBotBridge::Conflicts(bot, spellInfo);
#else
    return false;
#endif
}

bool PlayerbotRotation::UsesShapeshift([[maybe_unused]] Player* bot)
{
#ifdef PLAYERBOTS_ROTATION_BOT
    return RotationBotBridge::UsesShapeshift(bot);
#else
    return false;
#endif
}

bool PlayerbotRotation::IsTank(Player* bot)
{
    if (bot->getClass() != CLASS_DRUID)
        return PlayerbotAI::IsTank(bot, true);

    if (AiFactory::GetPlayerSpecTab(bot) != DRUID_TAB_FERAL)
        return false;

    // Thick Hide, ranks 1-5 (1.12; 3.3.5 keeps the first three ids).
    for (uint32 spellId : { 16929, 16930, 16931, 16932, 16933 })
        if (bot->HasSpell(spellId))
            return true;

    return false;
}
