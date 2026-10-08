/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DpsPaladinStrategy.h"
#include "PlayerbotEra.h"
#include "Playerbots.h"
#include "Strategy.h"

class DpsPaladinStrategyActionNodeFactory : public NamedObjectFactory<ActionNode>
{
public:
    DpsPaladinStrategyActionNodeFactory()
    {
        creators["sanctity aura"] = &sanctity_aura;
        creators["retribution aura"] = &retribution_aura;
        creators["blessing of might"] = &blessing_of_might;
        creators["repentance"] = &repentance;
        creators["repentance on enemy healer"] = &repentance_on_enemy_healer;
        creators["repentance on snare target"] = &repentance_on_snare_target;
        creators["repentance of shield"] = &repentance_or_shield;
    }

private:
    static ActionNode* blessing_of_might([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "blessing of might",
            /*P*/ {},
            /*A*/ { NextAction("blessing of kings") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance_on_enemy_healer([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance on enemy healer",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice on enemy healer") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance_on_snare_target([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance on snare target",
            /*P*/ {},
            /*A*/ { NextAction("hammer of justice on snare target") },
            /*C*/ {}
        );
    }

    static ActionNode* sanctity_aura([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "sanctity aura",
            /*P*/ {},
            /*A*/ { NextAction("retribution aura") },
            /*C*/ {}
        );
    }

    static ActionNode* retribution_aura([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "retribution aura",
            /*P*/ {},
            /*A*/ { NextAction("devotion aura") },
            /*C*/ {}
        );
    }

    static ActionNode* repentance_or_shield([[maybe_unused]] PlayerbotAI* botAI)
    {
        return new ActionNode(
            "repentance",
            /*P*/ {},
            /*A*/ { NextAction("divine shield") },
            /*C*/ {}
        );
    }
};

DpsPaladinStrategy::DpsPaladinStrategy(PlayerbotAI* botAI) : GenericPaladinStrategy(botAI)
{
    actionNodeFactories.Add(new DpsPaladinStrategyActionNodeFactory());
}

std::vector<NextAction> DpsPaladinStrategy::getDefaultActions()
{
    // 1.12 and 2.4.3 have no Divine Storm or separate judgement spells (Crusader Strike is 2.4.3's); Exorcism only hits
    // undead and demons.
    // 2.4.3 (the TBC APL): Consecration only above 40% mana (a trigger).
    if (PlayerbotEra::IsTbc())
        return {
            NextAction("crusader strike", ACTION_DEFAULT + 0.6f),
            NextAction("judgement", ACTION_DEFAULT + 0.5f),
            NextAction("hammer of wrath", ACTION_DEFAULT + 0.4f),
            NextAction("exorcism", ACTION_DEFAULT + 0.3f),
            NextAction("melee", ACTION_DEFAULT)
        };
    if (PlayerbotEra::IsClassic())
        return {
            NextAction("hammer of wrath", ACTION_DEFAULT + 0.6f),
            NextAction("judgement", ACTION_DEFAULT + 0.5f),
            NextAction("crusader strike", ACTION_DEFAULT + 0.4f),
            NextAction("exorcism", ACTION_DEFAULT + 0.3f),
            NextAction("consecration", ACTION_DEFAULT + 0.1f),
            NextAction("melee", ACTION_DEFAULT)
        };

    return {
        NextAction("hammer of wrath", ACTION_DEFAULT + 0.6f),
        NextAction("judgement of wisdom", ACTION_DEFAULT + 0.5f),
        NextAction("crusader strike", ACTION_DEFAULT + 0.4f),
        NextAction("divine storm", ACTION_DEFAULT + 0.3f),
        NextAction("consecration", ACTION_DEFAULT + 0.1f),
        NextAction("melee", ACTION_DEFAULT)
    };
}

void DpsPaladinStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPaladinStrategy::InitTriggers(triggers);

    // 1.12 and 2.4.3: Seal of Command (Righteousness before the talent), judged on cooldown and resealed.
    // 2.4.3 (the TBC APL): Crusader Strike > Judgement > reseal > Consecration above 40% mana. Seal twisting needs the
    // swing timer, which the bot doesn't follow: one seal, Seal of Blood for the Horde, Seal of Command otherwise.
    bool const classic = PlayerbotEra::IsClassic();
    bool const tbc = PlayerbotEra::IsTbc();
    if (classic)
        triggers.push_back(new TriggerNode("judgement", { NextAction("judgement", ACTION_HIGH + 2) }));
    if (tbc)
    {
        triggers.push_back(new TriggerNode("crusader strike", { NextAction("crusader strike", ACTION_HIGH + 3) }));
        triggers.push_back(new TriggerNode("consecration with mana", { NextAction("consecration", ACTION_HIGH + 1) }));
    }

    triggers.push_back(
        new TriggerNode(
            "art of war",
            {
                NextAction("exorcism", ACTION_DEFAULT + 0.2f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "seal",
            {
                NextAction(tbc ? "seal of blood" : classic ? "seal of command" : "seal of corruption", ACTION_HIGH)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low mana",
            {
                NextAction("seal of wisdom", ACTION_HIGH + 5)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "avenging wrath",
            {
                NextAction("avenging wrath", ACTION_HIGH + 2)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "medium aoe",
            {
                NextAction("divine storm", ACTION_HIGH + 4),
                NextAction("consecration", ACTION_HIGH + 3)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "enemy out of melee",
            {
                NextAction("reach melee", ACTION_HIGH + 1)
            }
        )
    );
}
