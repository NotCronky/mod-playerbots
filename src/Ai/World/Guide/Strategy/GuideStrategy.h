/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDESTRATEGY_H
#define PLAYERBOTS_GUIDESTRATEGY_H

#include "Multiplier.h"
#include "Strategy.h"

// A self-bot plays RestedXP's leveling guides (Playerbots.Guide.DataFile). In no default strategy set: the "guide
// start" chat command adds it, and its actions do nothing for anything but a self-bot.
class GuideStrategy : public Strategy
{
public:
    GuideStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "guide"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// The guide's deaths, in the dead engine: release (no free repair) and walk back to the body.
class GuideDeathStrategy : public Strategy
{
public:
    GuideDeathStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "guide death"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// In combat, while a "use at low health" item is the step's way: use it at the mark, and hold back spells on the
// creature close above it so auto-attack brings it down without killing it.
class GuideCombatStrategy : public Strategy
{
public:
    GuideCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}

    std::string const getName() override { return "guide combat"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class GuideHoldMultiplier : public Multiplier
{
public:
    GuideHoldMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "guide hold") {}

    float GetValue(Action* action) override;

private:
    static constexpr float HOLD_MARGIN = 15.0f;  // percent above the mark where spells on it stop
};

#endif
