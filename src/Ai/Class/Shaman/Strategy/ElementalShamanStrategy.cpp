/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ElementalShamanStrategy.h"
#include "PlayerbotEra.h"
#include "Playerbots.h"

ElementalShamanStrategy::ElementalShamanStrategy(PlayerbotAI* botAI) : GenericShamanStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> ElementalShamanStrategy::getDefaultActions()
{
    // 2.4.3 (the TBC APL): Chain Lightning above 30% mana (a trigger) > Lightning Bolt.
    if (PlayerbotEra::IsTbc())
        return { NextAction("lightning bolt", 5.0f) };

    return {
        NextAction("lava burst", 5.2f),
        NextAction("lightning bolt", 5.0f)
    };
}

// ===== Trigger Initialization ===
void ElementalShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericShamanStrategy::InitTriggers(triggers);

    // 1.12 has no Water Shield: Lightning Shield for every spec.
    if (PlayerbotEra::IsVanilla())
        triggers.push_back(new TriggerNode("lightning shield", { NextAction("lightning shield", 19.5f) }));

    // Totem Triggers
    triggers.push_back(
        new TriggerNode(
            "call of the elements",
            {
                NextAction("call of the elements", 60.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "low health",
            {
                NextAction("stoneclaw totem", 40.0f)
            }
        )
    );

    // Cooldown Trigger
    triggers.push_back(
        new TriggerNode(
            "elemental mastery",
            {
                NextAction("elemental mastery", 29.0f)
            }
        )
    );

    // Damage Triggers
    // 2.4.3 (the TBC APL): Totem of Wrath (the fire totem strategy) > Elemental Mastery > Flame Shock on targets that
    // live 12 seconds > Chain Lightning above 30% mana > Lightning Bolt. No Earth Shock.
    if (PlayerbotEra::IsTbc())
    {
        triggers.push_back(new TriggerNode("flame shock", { NextAction("flame shock", 5.6f) }));
        triggers.push_back(new TriggerNode("mana above 30", { NextAction("chain lightning", 5.4f) }));
    }
    else
    {
        triggers.push_back(
            new TriggerNode(
                "earth shock execute",
                {
                    NextAction("earth shock", 5.5f)
                }
            )
        );
        triggers.push_back(
            new TriggerNode(
                "flame shock",
                {
                    NextAction("flame shock", 5.3f)
                }
            )
        );
    }

    // Mana Triggers
    triggers.push_back(
        new TriggerNode(
            "water shield",
            {
                NextAction("water shield", 19.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "high mana",
            {
                NextAction("thunderstorm", 19.0f)
            }
        )
    );

    // Range Triggers
    triggers.push_back(
        new TriggerNode(
            "enemy is close",
            {
                NextAction("thunderstorm", 19.0f)
            }
        )
    );
}
