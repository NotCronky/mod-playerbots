#ifndef _PLAYERBOT_RAGEFIRECHASMTRIGGERCONTEXT_H
#define _PLAYERBOT_RAGEFIRECHASMTRIGGERCONTEXT_H

#include "NamedObjectContext.h"
#include "AiObjectContext.h"
#include "RagefireChasmTriggers.h"

class RagefireChasmTriggerContext : public NamedObjectContext<Trigger>
{
public:
    RagefireChasmTriggerContext()
    {
        creators["oggleflint priority"] = &RagefireChasmTriggerContext::oggleflint_priority;
        creators["taragaman fire nova"] = &RagefireChasmTriggerContext::taragaman_fire_nova;
        creators["taragaman uppercut"] = &RagefireChasmTriggerContext::taragaman_uppercut;
        creators["jergosh healing wave"] = &RagefireChasmTriggerContext::jergosh_healing_wave;
        creators["bazzalan poison"] = &RagefireChasmTriggerContext::bazzalan_poison;
        creators["bazzalan priority"] = &RagefireChasmTriggerContext::bazzalan_priority;
    }
private:
    static Trigger* oggleflint_priority(PlayerbotAI* ai) { return new OggleflintPriorityTrigger(ai); }
    static Trigger* taragaman_fire_nova(PlayerbotAI* ai) { return new TaragamanFireNovaTrigger(ai); }
    static Trigger* taragaman_uppercut(PlayerbotAI* ai) { return new TaragamanUppercutTrigger(ai); }
    static Trigger* jergosh_healing_wave(PlayerbotAI* ai) { return new JergoshHealingWaveTrigger(ai); }
    static Trigger* bazzalan_poison(PlayerbotAI* ai) { return new BazzalanPoisonTrigger(ai); }
    static Trigger* bazzalan_priority(PlayerbotAI* ai) { return new BazzalanPriorityTrigger(ai); }
};

#endif