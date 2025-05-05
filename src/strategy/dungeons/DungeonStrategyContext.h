#ifndef _PLAYERBOT_DUNGEONSTRATEGYCONTEXT_H
#define _PLAYERBOT_DUNGEONSTRATEGYCONTEXT_H

#include "Strategy.h"
#include "vanilla/RagefireChasmStrategy.h"

/*
* SM = Scarlet Monastery
*/

class DungeonStrategyContext : public NamedObjectContext<Strategy>
{
    public:
        DungeonStrategyContext() : NamedObjectContext<Strategy>(false, true)
        {
            // Vanilla
            creators["RagefireChasm"] = &DungeonStrategyContext::ragefire_chasm;
            /* Not Yet Implemented
            creators["WailingCaverns"] =        &DungeonStrategyContext::wailing_caverns;
            creators["TheDeadmines"] =          &DungeonStrategyContext::the_deadmines;
            creators["ShadowfangKeep"] =        &DungeonStrategyContext::shadowfang_keep;
            creators["TheStockade"] =           &DungeonStrategyContext::the_stockade;
            creators["BlackfathomDeeps"] =      &DungeonStrategyContext::blackfathom_deeps;
            creators["Gnomeregan"] =            &DungeonStrategyContext::gnomeregan;
            creators["RazorfenKraul"] =         &DungeonStrategyContext::razorfen_kraul;
            creators["SM-Graveyard"] =          &DungeonStrategyContext::sm_graveyard;
            creators["SM-Library"] =            &DungeonStrategyContext::sm_library;
            creators["SM-Armory"] =             &DungeonStrategyContext::sm_armory;
            creators["SM-Cathedral"] =          &DungeonStrategyContext::sm_cathedral;
            creators["RazorfenDowns"] =         &DungeonStrategyContext::razorfen_downs;
            creators["Uldaman"] =               &DungeonStrategyContext::uldaman;
            creators["ZulFarrak"] =             &DungeonStrategyContext::zul_farrak;
            creators["Maraudon"] =              &DungeonStrategyContext::maraudon;
            creators["SunkenTemple"] =          &DungeonStrategyContext::sunken_temple;
            creators["BlackrockDepths"] =       &DungeonStrategyContext::blackrock_depths;
            creators["DireMaul"] =              &DungeonStrategyContext::razorfen_downs;
            creators["LowerBlackrockSpire"] =   &DungeonStrategyContext::lower_blackrock_spire;
            creators["Scholomance"] =           &DungeonStrategyContext::scholomance;
            creators["Strathholme"] =           &DungeonStrategyContext::strathholme;
            creators["UpperBlackrockSpire"] =   &DungeonStrategyContext::upper_blackrock_spire;
            creators["TempleOfAtalHakkar"] =    &DungeonStrategyContext::temple_of_atal_hakkar;
            */ 

            // Burning Crusade

            /* Not Yet Implemented
            creators["HellfireRamparts"] =      &DungeonStrategyContext::hellfire_ramparts;
            creators["TheBloodFurnace"] =       &DungeonStrategyContext::the_blood_furnace;
            creators["TheSlavePens"] =          &DungeonStrategyContext::the_slave_pens;
            creators["TheUnderbog"] =           &DungeonStrategyContext::the_underbog;
            creators["ManaTombs"] =             &DungeonStrategyContext::mana_tombs;
            creators["AuchenaiCrypts"] =        &DungeonStrategyContext::anuchenai_crypts;
            creators["OldHillsbradFoothills"] = &DungeonStrategyContext::old_hillsbrad_foothills;
            creators["SethekkHalls"] =          &DungeonStrategyContext::sethekk_halls;
            creators["TheSteamvault"] =         &DungeonStrategyContext::the_steamvault;
            creators["ShadowLabyrinth"] =       &DungeonStrategyContext::shadow_labyrinth;
            creators["TheShatteredHalls"] =     &DungeonStrategyContext::the_shattered_halls;
            creators["TheBlackMorass"] =        &DungeonStrategyContext::the_black_morass;
            creators["TheBotanica"] =           &DungeonStrategyContext::the_botanica;
            creators["TheMechanar"] =           &DungeonStrategyContext::the_mechanar;
            creators["TheArcatraz"] =           &DungeonStrategyContext::the_arcatraz;
            creators["MagistersTerrace"] =      &DungeonStrategyContext::magisters_terrace;
            */
            
            // Wrath of the Lich King

            /* Not Yet Implemented
            creators["TheOldKingdom"] =         &DungeonStrategyContext::the_old_kingdom;
            creators["AzjolNerub"] =            &DungeonStrategyContext::azjol_nerub;
            creators["DraktharonKeep"] =        &DungeonStrategyContext::drak_tharon_keep;
            creators["Gundrak"] =               &DungeonStrategyContext::gundrak;
            creators["HallsOfReflection"] =     &DungeonStrategyContext::halls_of_reflection;
            creators["HallsOfStone"] =          &DungeonStrategyContext::halls_of_stone;
            creators["CullingOfStrathholme"] =  &DungeonStrategyContext::culling_of_stratholme;
            creators["ForgeOfSouls"] =          &DungeonStrategyContext::forge_of_souls;
            creators["TheNexus"] =              &DungeonStrategyContext::the_nexus;
            creators["TheOculus"] =             &DungeonStrategyContext::the_oculus;
            creators["VioletHold"] =            &DungeonStrategyContext::violet_hold;
            creators["PitOfSaron"] =            &DungeonStrategyContext::pit_of_saron;
            creators["TrialOfChampion"] =       &DungeonStrategyContext::trial_of_champion;
            creators["UtgardeKeep"] =           &DungeonStrategyContext::utgarde_keep;
            creators["UtgardePinnacle"] =       &DungeonStrategyContext::utgarde_pinnacle;
            creators["HallsOfLightning"] =       &DungeonStrategyContext::halls_of_lighning;
            */
        }
    private:
        static Strategy* ragefire_chasm(PlayerbotAI* botAI) { return new RagefireChasmStrategy(botAI); }
};

#endif
