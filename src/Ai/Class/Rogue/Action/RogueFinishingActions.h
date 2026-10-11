/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_ROGUEFINISHINGACTIONS_H
#define PLAYERBOTS_ROGUEFINISHINGACTIONS_H

#include "GenericSpellActions.h"
#include "PlayerbotEra.h"

class PlayerbotAI;

class CastEviscerateAction : public CastMeleeSpellAction
{
public:
    CastEviscerateAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "eviscerate") {}
};

class CastSliceAndDiceAction : public CastMeleeSpellAction
{
public:
    CastSliceAndDiceAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "slice and dice") {}
};

class CastExposeArmorAction : public CastDebuffSpellAction
{
public:
    CastExposeArmorAction(PlayerbotAI* botAI) : CastDebuffSpellAction(botAI, "expose armor", false, 25.0f) {}
};

class CastRuptureAction : public CastDebuffSpellAction
{
public:
    // 2.4.3 (the TBC APL): only on targets that live 8 seconds.
    CastRuptureAction(PlayerbotAI* botAI)
        : CastDebuffSpellAction(botAI, "rupture", true, PlayerbotEra::IsTbc() ? 8.0f : 6.0f) {}
};

class CastKidneyShotAction : public CastMeleeSpellAction
{
public:
    CastKidneyShotAction(PlayerbotAI* botAI) : CastMeleeSpellAction(botAI, "kidney shot") {}
};

#endif
