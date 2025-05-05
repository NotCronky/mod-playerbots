#include "RagefireChasmStrategy.h"
#include "RagefireChasmMultipliers.h"

void RagefireChasmStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // Oggleflint (Tank-and-spank, no special mechanics)
    triggers.push_back(new TriggerNode("oggleflint priority",
        NextAction::array(0, new NextAction("attack oggleflint", ACTION_NORMAL + 1), nullptr)));

    // Taragaman the Hungerer
    triggers.push_back(new TriggerNode("taragaman fire nova",
        NextAction::array(0, new NextAction("move away taragaman", ACTION_MOVE + 5), nullptr)));
    triggers.push_back(new TriggerNode("taragaman uppercut",
        NextAction::array(0, new NextAction("dodge uppercut", ACTION_MOVE + 5), nullptr)));

    // Jergosh the Invoker
    triggers.push_back(new TriggerNode("jergosh healing wave",
        NextAction::array(0, new NextAction("interrupt healing wave", ACTION_INTERRUPT + 5), nullptr)));

    // Bazzalan
    triggers.push_back(new TriggerNode("bazzalan poison",
        NextAction::array(0, new NextAction("cleanse poison", ACTION_NORMAL + 2), nullptr)));
    triggers.push_back(new TriggerNode("bazzalan priority",
        NextAction::array(0, new NextAction("attack bazzalan", ACTION_NORMAL + 1), nullptr)));
}

void RagefireChasmStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new OggleflintMultiplier(botAI));
    multipliers.push_back(new TaragamanMultiplier(botAI));
    multipliers.push_back(new JergoshMultiplier(botAI));
    multipliers.push_back(new BazzalanMultiplier(botAI));
}