#ifndef _PLAYERBOT_RAGEFIRECHASMMULTIPLIERS_H
#define _PLAYERBOT_RAGEFIRECHASMMULTIPLIERS_H

#include "Multiplier.h"

class OggleflintMultiplier : public Multiplier
{
public:
    OggleflintMultiplier(PlayerbotAI* ai) : Multiplier(ai, "oggleflint") {}
    virtual float GetValue(Action* action);
};

class TaragamanMultiplier : public Multiplier
{
public:
    TaragamanMultiplier(PlayerbotAI* ai) : Multiplier(ai, "taragaman the hungerer") {}
    virtual float GetValue(Action* action);
};

class JergoshMultiplier : public Multiplier
{
public:
    JergoshMultiplier(PlayerbotAI* ai) : Multiplier(ai, "jergosh the invoker") {}
    virtual float GetValue(Action* action);
};

class BazzalanMultiplier : public Multiplier
{
public:
    BazzalanMultiplier(PlayerbotAI* ai) : Multiplier(ai, "bazzalan") {}
    virtual float GetValue(Action* action);
};

#endif