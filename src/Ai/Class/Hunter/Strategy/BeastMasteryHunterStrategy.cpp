/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BeastMasteryHunterStrategy.h"
#include "PlayerbotEra.h"
#include "Playerbots.h"

BeastMasteryHunterStrategy::BeastMasteryHunterStrategy(PlayerbotAI* botAI) : GenericHunterStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Default Actions =====
std::vector<NextAction> BeastMasteryHunterStrategy::getDefaultActions()
{
    // 1.12 shot loop (no Kill Command, Kill Shot or Steady Shot): Aimed Shot, Multi-Shot, then Arcane Shot.
    if (PlayerbotEra::IsVanilla())
        return {
            NextAction("bestial wrath", 19.0f),
            NextAction("serpent sting", 5.5f),
            NextAction("aimed shot", 5.4f),
            NextAction("multi-shot", 5.35f),
            NextAction("arcane shot", 5.3f),
            NextAction("auto shot", 5.1f)
        };

    return {
        NextAction("bestial wrath", 19.0f),
        NextAction("kill command", 5.7f),
        NextAction("kill shot", 5.6f),
        NextAction("serpent sting", 5.5f),
        NextAction("aimed shot", 5.4f),
        NextAction("arcane shot", 5.3f),
        NextAction("steady shot", 5.2f),
        NextAction("auto shot", 5.1f)
    };
}

// ===== Trigger Initialization ===
void BeastMasteryHunterStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    bool const classic = PlayerbotEra::IsClassic();
    GenericHunterStrategy::InitTriggers(triggers);
    triggers.push_back(
        new TriggerNode(
            "intimidation",
            {
                NextAction("intimidation", 40.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "kill command",
            {
                NextAction("kill command", 18.5f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "target critical health",
            {
                NextAction("kill shot", 18.0f)
            }
        )
    );
    // 1.12 and 2.4.3 Viper Sting drains the target's mana without returning any.
    if (!classic)
        triggers.push_back(
            new TriggerNode(
                "low mana",
                {
                    NextAction("viper sting", 17.5f)
                }
            )
        );
    triggers.push_back(
        new TriggerNode(
            "no stings",
            {
                NextAction("serpent sting", 17.0f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "serpent sting on attacker",
            {
                NextAction("serpent sting on attacker", 16.5f)
            }
        )
    );
}
