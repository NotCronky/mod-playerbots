#ifndef _PLAYERBOT_RAGEFIRECHASMACTIONS_H
#define _PLAYERBOT_RAGEFIRECHASMACTIONS_H

#include "Action.h"
#include "AttackAction.h"
#include "MovementActions.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "RagefireChasmTriggers.h"

class AttackOggleflintAction : public AttackAction
{
public:
    AttackOggleflintAction(PlayerbotAI* ai) : AttackAction(ai, "attack oggleflint") {}
    bool Execute(Event event) override;
};

class MoveAwayTaragamanAction : public MovementAction
{
public:
    MoveAwayTaragamanAction(PlayerbotAI* ai) : MovementAction(ai, "move away taragaman") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class DodgeUppercutAction : public MovementAction
{
public:
    DodgeUppercutAction(PlayerbotAI* ai) : MovementAction(ai, "dodge uppercut") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class InterruptHealingWaveAction : public Action
{
public:
    InterruptHealingWaveAction(PlayerbotAI* ai) : Action(ai, "interrupt healing wave") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class CleansePoisonAction : public Action
{
public:
    CleansePoisonAction(PlayerbotAI* ai) : Action(ai, "cleanse poison") {}
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AttackBazzalanAction : public AttackAction
{
public:
    AttackBazzalanAction(PlayerbotAI* ai) : AttackAction(ai, "attack bazzalan") {}
    bool Execute(Event event) override;
};

#endif