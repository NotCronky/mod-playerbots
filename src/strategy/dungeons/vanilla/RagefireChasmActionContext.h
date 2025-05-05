#ifndef _PLAYERBOT_RAGEFIRECHASMACTIONCONTEXT_H
#define _PLAYERBOT_RAGEFIRECHASMACTIONCONTEXT_H

#include "Action.h"
#include "NamedObjectContext.h"
#include "RagefireChasmActions.h"

class RagefireChasmActionContext : public NamedObjectContext<Action>
{
public:
    RagefireChasmActionContext()
    {
        creators["attack oggleflint"] = &RagefireChasmActionContext::attack_oggleflint;
        creators["move away taragaman"] = &RagefireChasmActionContext::move_away_taragaman;
        creators["dodge uppercut"] = &RagefireChasmActionContext::dodge_uppercut;
        creators["interrupt healing wave"] = &RagefireChasmActionContext::interrupt_healing_wave;
        creators["cleanse poison"] = &RagefireChasmActionContext::cleanse_poison;
        creators["attack bazzalan"] = &RagefireChasmActionContext::attack_bazzalan;
    }
private:
    static Action* attack_oggleflint(PlayerbotAI* ai) { return new AttackOggleflintAction(ai); }
    static Action* move_away_taragaman(PlayerbotAI* ai) { return new MoveAwayTaragamanAction(ai); }
    static Action* dodge_uppercut(PlayerbotAI* ai) { return new DodgeUppercutAction(ai); }
    static Action* interrupt_healing_wave(PlayerbotAI* ai) { return new InterruptHealingWaveAction(ai); }
    static Action* cleanse_poison(PlayerbotAI* ai) { return new CleansePoisonAction(ai); }
    static Action* attack_bazzalan(PlayerbotAI* ai) { return new AttackBazzalanAction(ai); }
};

#endif