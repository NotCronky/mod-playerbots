/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MCTRIGGERS_H
#define PLAYERBOTS_MCTRIGGERS_H

#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "Trigger.h"

class McLivingBombDebuffTrigger : public Trigger
{
public:
    McLivingBombDebuffTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc living bomb debuff") {}
    bool IsActive() override;
};

class McBaronGeddonInfernoTrigger : public Trigger
{
public:
    McBaronGeddonInfernoTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc baron geddon inferno") {}
    bool IsActive() override;
};

class McShazzrahRangedTrigger : public Trigger
{
public:
    McShazzrahRangedTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc shazzrah ranged") {}
    bool IsActive() override;
};

class McGarrIsMainTankTrigger : public Trigger
{
public:
    McGarrIsMainTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc garr is main tank") {}
    bool IsActive() override;
};

class McGarrIsAssistTankTrigger : public Trigger
{
public:
    McGarrIsAssistTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc garr is assist tank") {}
    bool IsActive() override;
};

class McGarrIsWarlockTrigger : public Trigger
{
public:
    McGarrIsWarlockTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc garr is warlock") {}
    bool IsActive() override;
};

class McGolemaggMarkBossTrigger : public Trigger
{
public:
    McGolemaggMarkBossTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg mark boss") {}
    bool IsActive() override;
};

class McGolemaggIsMainTankTrigger : public Trigger
{
public:
    McGolemaggIsMainTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is main tank") {}
    bool IsActive() override;
};

class McGolemaggIsAssistTankTrigger : public Trigger
{
public:
    McGolemaggIsAssistTankTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is assist tank") {}
    bool IsActive() override;
};

class McGolemaggIsHealerTrigger : public Trigger
{
public:
    McGolemaggIsHealerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg is healer") {}
    bool IsActive() override;
};

class McCoreHoundMarkTrigger : public Trigger
{
public:
    McCoreHoundMarkTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc core hound mark") {}
    bool IsActive() override;
};

// Standing/swimming in magma (knocked into a lava pool by Ragnaros' Wrath or an Eruption)
class McInLavaTrigger : public Trigger
{
public:
    McInLavaTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc in lava") {}
    bool IsActive() override;
};

// Garr: a Firesworn about to erupt is within its blast radius (not for tanks, who hold it).
class McFireswornEruptionTrigger : public Trigger
{
public:
    McFireswornEruptionTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc firesworn eruption") {}
    bool IsActive() override;
};

class McMajordomoInCoalsTrigger : public Trigger
{
public:
    McMajordomoInCoalsTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc majordomo in coals") {}
    bool IsActive() override;
};

class McGolemaggMagmaSplashTrigger : public Trigger
{
public:
    McGolemaggMagmaSplashTrigger(PlayerbotAI* botAI) : Trigger(botAI, "mc golemagg magma splash") {}
    bool IsActive() override;
};

#endif
