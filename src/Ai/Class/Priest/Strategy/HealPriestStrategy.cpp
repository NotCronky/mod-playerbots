/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HealPriestStrategy.h"
#include "PlayerbotEra.h"
#include "GenericPriestStrategyActionNodeFactory.h"
#include "Playerbots.h"

HealPriestStrategy::HealPriestStrategy(PlayerbotAI* botAI) : GenericPriestStrategy(botAI)
{
    actionNodeFactories.Add(new GenericPriestStrategyActionNodeFactory());
}

std::vector<NextAction> HealPriestStrategy::getDefaultActions()
{
    return {
        NextAction("shoot", ACTION_DEFAULT)
    };
}

// 1.12, Discipline: Power Word: Shield only on someone about to die (it cost a disc priest a third of its casts and most
// of its mana when it went on every hurt member), Prayer of Healing on group damage, Greater Heal or Flash Heal for big
// deficits, Heal (Lesser Heal before it) for steady damage, Renew on the lightly hurt. No Penance or Prayer of Mending.
void AddVanillaPriestHealTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("party member to heal out of spell range",
                                       { NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10) }));
    triggers.push_back(new TriggerNode("medium group heal setting",
                                       { NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 4) }));
    triggers.push_back(new TriggerNode("party member critical health",
                                       {
                                           NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 5),
                                           NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 2)
                                       }));
    triggers.push_back(new TriggerNode("party member low health",
                                       {
                                           NextAction("greater heal on party", ACTION_MEDIUM_HEAL + 1),
                                           NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 0)
                                       }));
    triggers.push_back(new TriggerNode("party member medium health",
                                       {
                                           NextAction("heal on party", ACTION_LIGHT_HEAL + 4),
                                           NextAction("lesser heal on party", ACTION_LIGHT_HEAL + 3),
                                           NextAction("renew on party", ACTION_LIGHT_HEAL + 2)
                                       }));
    triggers.push_back(new TriggerNode("party member almost full health",
                                       { NextAction("renew on party", ACTION_LIGHT_HEAL + 1) }));
}

void HealPriestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericPriestStrategy::InitTriggers(triggers);

    if (PlayerbotEra::IsVanilla())
    {
        AddVanillaPriestHealTriggers(triggers);
        return;
    }

    // 1.12 and 2.4.3 have no Penance (nor 1.12 Prayer of Mending): Greater Heal for big deficits, Heal (Lesser Heal
    // before it) for steady damage, Renew on the lightly hurt.
    if (PlayerbotEra::IsClassic())
    {
        triggers.push_back(new TriggerNode("party member low health", { NextAction("greater heal on party", ACTION_MEDIUM_HEAL + 1) }));
        triggers.push_back(new TriggerNode("party member medium health", { NextAction("heal on party", ACTION_LIGHT_HEAL + 4),
                                                                          NextAction("lesser heal on party", ACTION_LIGHT_HEAL + 3),
                                                                          NextAction("renew on party", ACTION_LIGHT_HEAL + 2) }));
    }

    triggers.push_back(
        new TriggerNode(
            "group heal setting",
            {
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 8),
                NextAction("power word: shield on not full", ACTION_MEDIUM_HEAL + 7)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "medium group heal setting",
            {
                NextAction("divine hymn", ACTION_CRITICAL_HEAL + 7),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 6),
                NextAction("power word: shield on not full", ACTION_CRITICAL_HEAL + 5),
                NextAction("prayer of healing on party", ACTION_CRITICAL_HEAL + 4)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member critical health",
            {
                NextAction("power word: shield on party", ACTION_CRITICAL_HEAL + 5),
                NextAction("penance on party", ACTION_CRITICAL_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_CRITICAL_HEAL + 3),
                NextAction("flash heal on party", ACTION_CRITICAL_HEAL + 2)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member low health",
            {
                NextAction("power word: shield on party", ACTION_MEDIUM_HEAL + 4),
                NextAction("prayer of mending on party", ACTION_MEDIUM_HEAL + 3),
                NextAction("penance on party", ACTION_MEDIUM_HEAL + 2),
                NextAction("flash heal on party", ACTION_MEDIUM_HEAL + 0)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member medium health",
            {
                NextAction("power word: shield on party", ACTION_LIGHT_HEAL + 9),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 7),
                NextAction("penance on party", ACTION_LIGHT_HEAL + 6),
                NextAction("flash heal on party", ACTION_LIGHT_HEAL + 5)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member almost full health",
            {
                NextAction("power word: shield on party", ACTION_LIGHT_HEAL + 3),
                NextAction("prayer of mending on party", ACTION_LIGHT_HEAL + 2),
                NextAction("renew on party", ACTION_LIGHT_HEAL + 1)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "party member to heal out of spell range",
            {
                NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10)
            }
        )
    );

    triggers.push_back(
        new TriggerNode(
            "critical health", {
                NextAction("pain suppression", ACTION_EMERGENCY + 1)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "protect party member",
            {
                NextAction("pain suppression on party", ACTION_EMERGENCY)
            }
        )
    );
}
