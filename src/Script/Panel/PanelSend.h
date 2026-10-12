/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PANELSEND_H
#define PLAYERBOTS_PANELSEND_H

#include <string>
#include <string_view>
#include <vector>

class Player;

namespace PanelSend
{
// A panel reply to this player only, as a party addon message (also outside a group).
void Send(Player* player, std::string_view verb, std::vector<std::string> const& fields = {});
}  // namespace PanelSend

#endif
