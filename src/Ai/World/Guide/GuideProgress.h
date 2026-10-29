/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GUIDEPROGRESS_H
#define PLAYERBOTS_GUIDEPROGRESS_H

class PlayerbotAI;

// A self-bot's place on the route (guide, RestedXP step number, on or off), kept in playerbots_guide_progress.
namespace GuideProgress
{
// Writes the place when it changed since the last write (async, any thread)
void Save(PlayerbotAI* botAI);
// Reads the saved place as self-bot comes on (world thread) and turns the guide back on if it was on
void Restore(PlayerbotAI* botAI);
}  // namespace GuideProgress

#endif
