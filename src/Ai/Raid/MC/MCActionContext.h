/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_MCACTIONCONTEXT_H
#define PLAYERBOTS_MCACTIONCONTEXT_H

#include "Action.h"
#include "BossAuraActions.h"
#include "MCActions.h"
#include "NamedObjectContext.h"

class RaidMcActionContext : public NamedObjectContext<Action>
{
public:
    RaidMcActionContext()
    {
        creators["mc lucifron shadow resistance"] = &RaidMcActionContext::lucifron_shadow_resistance;
        creators["mc magmadar fire resistance"] = &RaidMcActionContext::magmadar_fire_resistance;
        creators["mc gehennas shadow resistance"] = &RaidMcActionContext::gehennas_shadow_resistance;
        creators["mc garr fire resistance"] = &RaidMcActionContext::garr_fire_resistance;
        creators["mc baron geddon fire resistance"] = &RaidMcActionContext::baron_geddon_fire_resistance;
        creators["mc move from group"] = &RaidMcActionContext::check_should_move_from_group;
        creators["mc move from baron geddon"] = &RaidMcActionContext::move_from_baron_geddon;
        creators["mc shazzrah move away"] = &RaidMcActionContext::shazzrah_move_away;
        creators["mc sulfuron harbinger fire resistance"] = &RaidMcActionContext::sulfuron_harbinger_fire_resistance;
        creators["mc golemagg fire resistance"] = &RaidMcActionContext::golemagg_fire_resistance;
        creators["mc garr main tank attack garr"] = &RaidMcActionContext::garr_main_tank_attack_garr;
        creators["mc garr assist tank attack firesworn"] = &RaidMcActionContext::garr_assist_tank_attack_firesworn;
        creators["mc garr banish firesworn"] = &RaidMcActionContext::garr_banish_firesworn;
        creators["mc golemagg mark boss"] = &RaidMcActionContext::golemagg_mark_boss;
        creators["mc golemagg main tank attack golemagg"] = &RaidMcActionContext::golemagg_main_tank_attack_golemagg;
        creators["mc golemagg assist tank attack core rager"] = &RaidMcActionContext::golemagg_assist_tank_attack_core_rager;
        creators["mc majordomo shadow resistance"] = &RaidMcActionContext::majordomo_shadow_resistance;
        creators["mc ragnaros fire resistance"] = &RaidMcActionContext::ragnaros_fire_resistance;
        creators["mc core hound mark"] = &RaidMcActionContext::core_hound_mark;
        creators["mc move from lava"] = &RaidMcActionContext::move_from_lava;
        creators["mc majordomo leave coals"] = &RaidMcActionContext::majordomo_leave_coals;
        creators["mc move from dying firesworn"] = &RaidMcActionContext::move_from_dying_firesworn;
        creators["mc golemagg back off"] = &RaidMcActionContext::golemagg_back_off;
        creators["mc golemagg healer position"] = &RaidMcActionContext::golemagg_healer_position;
    }

private:
    static Action* lucifron_shadow_resistance(PlayerbotAI* botAI) { return new BossShadowResistanceAction(botAI, "lucifron"); }
    static Action* magmadar_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "magmadar"); }
    static Action* gehennas_shadow_resistance(PlayerbotAI* botAI) { return new BossShadowResistanceAction(botAI, "gehennas"); }
    static Action* garr_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "garr"); }
    static Action* baron_geddon_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "baron geddon"); }
    static Action* check_should_move_from_group(PlayerbotAI* botAI) { return new McMoveFromGroupAction(botAI); }
    static Action* move_from_baron_geddon(PlayerbotAI* botAI) { return new McMoveFromBaronGeddonAction(botAI); }
    static Action* shazzrah_move_away(PlayerbotAI* botAI) { return new McShazzrahMoveAwayAction(botAI); }
    static Action* sulfuron_harbinger_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "sulfuron harbinger"); }
    static Action* golemagg_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "golemagg the incinerator"); }
    static Action* garr_main_tank_attack_garr(PlayerbotAI* botAI) { return new McGarrMainTankAction(botAI); }
    static Action* garr_assist_tank_attack_firesworn(PlayerbotAI* botAI) { return new McGarrAssistTankAction(botAI); }
    static Action* garr_banish_firesworn(PlayerbotAI* botAI) { return new McGarrBanishFireswornAction(botAI); }
    static Action* golemagg_mark_boss(PlayerbotAI* botAI) { return new McGolemaggMarkBossAction(botAI); }
    static Action* golemagg_main_tank_attack_golemagg(PlayerbotAI* botAI) { return new McGolemaggMainTankAttackGolemaggAction(botAI); }
    static Action* golemagg_assist_tank_attack_core_rager(PlayerbotAI* botAI) { return new McGolemaggAssistTankAttackCoreRagerAction(botAI); }
    static Action* majordomo_shadow_resistance(PlayerbotAI* botAI) { return new BossShadowResistanceAction(botAI, "majordomo executus"); }
    static Action* ragnaros_fire_resistance(PlayerbotAI* botAI) { return new BossFireResistanceAction(botAI, "ragnaros"); }
    static Action* core_hound_mark(PlayerbotAI* botAI) { return new McCoreHoundMarkAction(botAI); }
    static Action* move_from_lava(PlayerbotAI* botAI) { return new McMoveFromLavaAction(botAI); }
    static Action* majordomo_leave_coals(PlayerbotAI* botAI) { return new McMajordomoLeaveCoalsAction(botAI); }
    static Action* move_from_dying_firesworn(PlayerbotAI* botAI) { return new McMoveFromDyingFireswornAction(botAI); }
    static Action* golemagg_back_off(PlayerbotAI* botAI) { return new McGolemaggBackOffAction(botAI); }
    static Action* golemagg_healer_position(PlayerbotAI* botAI) { return new McGolemaggHealerPositionAction(botAI); }
};

#endif
