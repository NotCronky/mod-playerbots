#ifndef _PLAYERBOT_RAGEFIRECHASMTRIGGERS_H
#define _PLAYERBOT_RAGEFIRECHASMTRIGGERS_H

#include "Trigger.h"
#include "PlayerbotAIConfig.h"
#include "GenericTriggers.h"
#include "DungeonStrategyUtils.h"

enum RagefireChasmIDs
{
    // Taragaman the Hungerer
    SPELL_FIRE_NOVA = 11970,
    SPELL_UPPERCUT = 10966,

    // Jergosh the Invoker
    SPELL_HEALING_WAVE = 11986,

    // Bazzalan
    SPELL_SINISTER_STRIKE = 14873,
    SPELL_POISON = 11959,
};

class OggleflintPriorityTrigger : public Trigger
{
public:
    OggleflintPriorityTrigger(PlayerbotAI* ai) : Trigger(ai, "oggleflint priority") {}
    bool IsActive() override;
};

class TaragamanFireNovaTrigger : public Trigger
{
public:
    TaragamanFireNovaTrigger(PlayerbotAI* ai) : Trigger(ai, "taragaman fire nova") {}
    bool IsActive() override;
};

class TaragamanUppercutTrigger : public Trigger
{
public:
    TaragamanUppercutTrigger(PlayerbotAI* ai) : Trigger(ai, "taragaman uppercut") {}
    bool IsActive() override;
};

class JergoshHealingWaveTrigger : public Trigger
{
public:
    JergoshHealingWaveTrigger(PlayerbotAI* ai) : Trigger(ai, "jergosh healing wave") {}
    bool IsActive() override;
};

class BazzalanPoisonTrigger : public Trigger
{
public:
    BazzalanPoisonTrigger(PlayerbotAI* ai) : Trigger(ai, "bazzalan poison") {}
    bool IsActive() override;
};

class BazzalanPriorityTrigger : public Trigger
{
public:
    BazzalanPriorityTrigger(PlayerbotAI* ai) : Trigger(ai, "bazzalan priority") {}
    bool IsActive() override;
};

#endif