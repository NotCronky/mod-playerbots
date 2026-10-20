/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDETRIGGERS_H
#define PLAYERBOTS_GUIDETRIGGERS_H

#include "Trigger.h"

// Reads the task "guide update" worked out (GuideInfo::task); O(1).
class GuideTaskTrigger : public Trigger
{
public:
    enum class Kind : uint8
    {
        Move,
        Interact,
        Service,
        Rest,
        Avoid,
        Hearth,
        Ride,
        Open,
        Use,
        Attack
    };

    GuideTaskTrigger(PlayerbotAI* botAI, std::string const name, Kind kind) : Trigger(botAI, name), _kind(kind) {}

    bool IsActive() override;

private:
    Kind _kind;
};

// The guide's deaths: a dead body to release, or a ghost with a body to walk back to.
class GuideDeathTrigger : public Trigger
{
public:
    GuideDeathTrigger(PlayerbotAI* botAI, std::string const name, bool ghost) : Trigger(botAI, name), _ghost(ghost) {}

    bool IsActive() override;

private:
    bool _ghost;
};

// In combat: the victim is one a "use at low health" item is for, at or below the mark, and the item is ready.
class GuideUseNowTrigger : public Trigger
{
public:
    GuideUseNowTrigger(PlayerbotAI* botAI) : Trigger(botAI, "guide use now") {}

    bool IsActive() override;
};

#endif
