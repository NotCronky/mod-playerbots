/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUECOMBOACTIONS_H
#define PLAYERBOTS_ROGUECOMBOACTIONS_H

#include "GenericSpellActions.h"

class PlayerbotAI;

class CastComboAction : public CastMeleeSpellAction
{
public:
    CastComboAction(PlayerbotAI* botAI, std::string const name) : CastMeleeSpellAction(botAI, name) {}

    bool isUseful() override;
};

class CastSinisterStrikeAction : public CastSpellAction
{
public:
    CastSinisterStrikeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "sinister strike") {}
};

class CastMutilateAction : public CastSpellAction
{
public:
    CastMutilateAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "mutilate") {}
};

class CastRiposteAction : public CastSpellAction
{
public:
    CastRiposteAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "riposte") {}
};

class CastGougeAction : public CastSpellAction
{
public:
    CastGougeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "gouge") {}
};

class CastBackstabAction : public CastSpellAction
{
public:
    CastBackstabAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "backstab") {}
};

// Subtlety builders (1.12 talents in place of Mutilate).
class CastHemorrhageAction : public CastSpellAction
{
public:
    CastHemorrhageAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "hemorrhage") {}
};

class CastGhostlyStrikeAction : public CastSpellAction
{
public:
    CastGhostlyStrikeAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "ghostly strike") {}
};

// Two combo points before the opener (Subtlety talent); the server wants a target within 10 yards.
class CastPremeditationAction : public CastSpellAction
{
public:
    CastPremeditationAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "premeditation") {}
};

// Resets Evasion, Vanish and the other rogue cooldowns (Subtlety talent).
class CastPreparationAction : public CastSpellAction
{
public:
    CastPreparationAction(PlayerbotAI* botAI) : CastSpellAction(botAI, "preparation") {}
    std::string const GetTargetName() override { return "self target"; }
};

#endif
