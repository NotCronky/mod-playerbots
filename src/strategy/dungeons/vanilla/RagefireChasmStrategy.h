#ifndef _PLAYERBOT_RAGEFIRECHASMSTRATEGY_H
#define _PLAYERBOT_RAGEfireCHASMSTRATEGY_H

#include "Multiplier.h"
#include "AiObjectContext.h"
#include "Strategy.h"

class RagefireChasmStrategy : public Strategy
{
public:
    RagefireChasmStrategy(PlayerbotAI* ai) : Strategy(ai) {}
    virtual std::string const getName() override { return "ragefire chasm"; }
    virtual void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    virtual void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

#endif