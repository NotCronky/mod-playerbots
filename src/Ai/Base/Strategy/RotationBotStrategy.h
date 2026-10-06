/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROTATIONBOTSTRATEGY_H
#define PLAYERBOTS_ROTATIONBOTSTRATEGY_H

#include "CombatStrategy.h"
#include "Multiplier.h"

class PlayerbotAI;

// Fights with the bot's mod-rotation-bot profile (AiPlayerbot.UseRotationBot) in place of the class
// strategies. Movement stays with the bot AI: melee bots close in, ranged bots keep their distance
// and healers reach the member to heal (they don't run from melee). Its type carries the bot's role, which the class strategies
// it replaces would otherwise give.
class RotationBotStrategy : public CombatStrategy
{
public:
    RotationBotStrategy(PlayerbotAI* botAI) : CombatStrategy(botAI) {}

    std::string const getName() override { return "rotation"; }
    uint32 GetType() const override;
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Out of combat: the profile's precombat list (buffs, stances, forms, pets).
class RotationBotNonCombatStrategy : public Strategy
{
public:
    RotationBotNonCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "rotation nc"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    std::vector<NextAction> getDefaultActions() override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

// Blocks the bot AI's own picks that would undo the profile's: another stance or form, aspect,
// paladin aura, blessing, seal, armor, shield, weapon imbue or pet, and dropping a form.
class RotationBotMultiplier : public Multiplier
{
public:
    RotationBotMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "rotation") {}

    float GetValue(Action* action) override;
};

#endif
