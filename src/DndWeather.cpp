/*
 * mod-dnd weather: each open zone has a climate; every 6-14 minutes the weather turns (it builds up - a blizzard only
 * follows snowfall, a thunderstorm a downpour), the zone sees it (Map::SetZoneWeather; the core's own random weather
 * is off, ActivateWeather = 0) and everyone standing outdoors in it lives by its rules - players and monsters alike:
 *
 *   Fog           ranged attacks at anything over 9 m away: disadvantage. Passive Perception -5 (treats).
 *   Flurries      ranged attacks -1. Cold damage +1, fire damage -1 (the cold feeds frost magic).
 *   Snowfall      movement -1.5 m (dwarves don't mind snow). Ranged -2. Cold +2, fire -2.
 *   Blizzard      movement -3 m (not dwarves). Ranged attacks over 9 m: disadvantage. Cold +3, fire -3. Each round
 *                 everyone makes a CON save, DC 10 (dwarves: advantage), or takes 1d4 cold.
 *   Drizzle       Damp: lightning and cold damage +2, fire -2.
 *   Downpour      Drenched (BG3's Wet): vulnerable to lightning and cold, resistant to fire. Movement -1.5 m,
 *                 ranged -2.
 *   Thunderstorm  Drenched, and each round lightning may strike someone in the fight (d20 17+): DEX save DC 13 or 2d8
 *                 lightning (half on a save) - doubled for the drenched.
 *   Gale          ranged attacks: disadvantage. Jumps go 50% further with the wind behind you.
 * Indoors (caves, inns, under a roof) none of it applies. Players outdoors wear the weather as a buff (spells
 * 200301-200308, tooltips from tools/make_patch.py) and get a line in the log when it turns.
 */
#include "Dnd.h"

#include "Chat.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include "Weather.h"
#include <map>

namespace Dnd
{
    namespace
    {
        struct WeatherDef
        {
            char const* name;
            WeatherState state;
            float grade;
            uint32 aura;                     // the buff players outdoors wear (0: none)
            char const* turns;               // the log line when it arrives
        };
        WeatherDef const sDefs[WEATHER_COUNT] =
        {
            { "Clear",        WEATHER_STATE_FINE,             0.0f,  0,      "The sky clears." },
            { "Fog",          WEATHER_STATE_FOG,              0.5f,  200301, "A fog rolls in. Ranged attacks past 9 m are made at disadvantage." },
            { "Flurries",     WEATHER_STATE_LIGHT_SNOW,       0.3f,  200302, "Snow begins to drift down. Cold bites harder, fire burns lower." },
            { "Snowfall",     WEATHER_STATE_MEDIUM_SNOW,      0.6f,  200303, "The snow comes thick and steady - heavy going, and hard to aim through." },
            { "Blizzard",     WEATHER_STATE_HEAVY_SNOW,       0.95f, 200304, "A blizzard howls down the mountain! Everyone outdoors fights the cold as well as each other." },
            { "Drizzle",      WEATHER_STATE_LIGHT_RAIN,       0.3f,  200305, "A thin rain starts to fall. Damp: lightning and cold sting more, fire less." },
            { "Downpour",     WEATHER_STATE_HEAVY_RAIN,       0.75f, 200306, "The heavens open. Drenched: lightning and cold do double damage, fire half." },
            { "Thunderstorm", WEATHER_STATE_THUNDERS,         0.9f,  200307, "Thunder rolls overhead - lightning may strike anyone caught in the open!" },
            { "Gale",         WEATHER_STATE_LIGHT_SANDSTORM,  0.45f, 200308, "A gale tears across the moor. Arrows go astray; jumps carry far." },
        };

        struct Climate { uint32 map, zone; std::vector<std::pair<WeatherKind, int>> chances; };
        std::vector<Climate> const sClimates =
        {
            { 0, 1,  { { WEATHER_CLEAR, 35 }, { WEATHER_FOG, 10 }, { WEATHER_FLURRIES, 25 }, { WEATHER_SNOWFALL, 18 }, { WEATHER_BLIZZARD, 12 } } },   // Dun Morogh
            { 0, 38, { { WEATHER_CLEAR, 40 }, { WEATHER_FOG, 12 }, { WEATHER_DRIZZLE, 18 }, { WEATHER_DOWNPOUR, 14 }, { WEATHER_THUNDERSTORM, 8 },
                       { WEATHER_GALE, 8 } } },                                                                                                       // Loch Modan
            { 0, 11, { { WEATHER_CLEAR, 22 }, { WEATHER_FOG, 24 }, { WEATHER_DRIZZLE, 24 }, { WEATHER_DOWNPOUR, 18 }, { WEATHER_THUNDERSTORM, 8 },
                       { WEATHER_GALE, 4 } } },                                                                                                       // the Wetlands: fog and rain
            { 0, 3,  { { WEATHER_CLEAR, 58 }, { WEATHER_GALE, 30 }, { WEATHER_THUNDERSTORM, 12 } } },                                                  // the Badlands: sun, dust-storms, dry lightning
            { 0, 51, { { WEATHER_CLEAR, 50 }, { WEATHER_FOG, 25 }, { WEATHER_GALE, 15 }, { WEATHER_THUNDERSTORM, 10 } } },                             // the Searing Gorge: heat, smoke, ash-winds
            { 0, 45, { { WEATHER_CLEAR, 40 }, { WEATHER_FOG, 14 }, { WEATHER_DRIZZLE, 18 }, { WEATHER_GALE, 16 }, { WEATHER_THUNDERSTORM, 12 } } },     // Arathi: open, windy highland
            { 0, 12, { { WEATHER_CLEAR, 50 }, { WEATHER_FOG, 12 }, { WEATHER_DRIZZLE, 22 }, { WEATHER_DOWNPOUR, 10 }, { WEATHER_THUNDERSTORM, 6 } } },  // Elwynn: mild, green, showers
            { 0, 1519, { { WEATHER_CLEAR, 50 }, { WEATHER_FOG, 18 }, { WEATHER_DRIZZLE, 22 }, { WEATHER_DOWNPOUR, 10 } } },                           // Stormwind: harbour fog and rain
            { 1, 1638, { { WEATHER_CLEAR, 46 }, { WEATHER_GALE, 30 }, { WEATHER_DRIZZLE, 12 }, { WEATHER_THUNDERSTORM, 12 } } },                      // Thunder Bluff: high, windy mesas, plains storms
            { 1, 1637, { { WEATHER_CLEAR, 62 }, { WEATHER_GALE, 28 }, { WEATHER_THUNDERSTORM, 10 } } },                                               // Orgrimmar: Durotar sun, dust-winds, dry lightning
        };

        struct ZoneWeather { WeatherKind kind = WEATHER_CLEAR; uint32 untilMs = 0; bool forced = false; };
        std::map<std::pair<uint32, uint32>, ZoneWeather> sNow;
        std::unordered_map<ObjectGuid, uint32> sAuraTimer;

        WeatherKind Roll(Climate const& c, WeatherKind cur)
        {
            int total = 0;
            for (auto const& [k, w] : c.chances)
                total += w;
            int r = int(urand(1, uint32(total)));
            WeatherKind pick = WEATHER_CLEAR;
            for (auto const& [k, w] : c.chances)
                if ((r -= w) <= 0)
                {
                    pick = k;
                    break;
                }
            // weather builds: the worst of it only comes out of the middling kind
            if (pick == WEATHER_BLIZZARD && cur != WEATHER_SNOWFALL && cur != WEATHER_BLIZZARD)
                pick = WEATHER_SNOWFALL;
            if (pick == WEATHER_THUNDERSTORM && cur != WEATHER_DOWNPOUR && cur != WEATHER_THUNDERSTORM)
                pick = WEATHER_DOWNPOUR;
            return pick;
        }

        uint32 Now() { return getMSTime(); }

        void Announce(Map* map, uint32 zone, WeatherKind kind)
        {
            map->DoForAllPlayers([&](Player* p)
            {
                if (p->GetZoneId() != zone)
                    return;
                if (p->IsOutdoors())
                    Log(p, Acore::StringFormat("|cff9ad0ff{}|r", sDefs[kind].turns));
                Send(p, Acore::StringFormat("W^{}", sDefs[kind].name));
                if (kind == WEATHER_BLIZZARD || kind == WEATHER_THUNDERSTORM)
                    Send(p, Acore::StringFormat("V^{}", sDefs[kind].name == std::string("Blizzard") ? "BLIZZARD" : "THUNDERSTORM"));
                sAuraTimer[p->GetGUID()] = 0;    // the buff changes on the next check
            });
        }

        void Apply(uint32 mapId, uint32 zone, WeatherKind kind)
        {
            ZoneWeather& z = sNow[{ mapId, zone }];
            if (z.kind == kind)
                return;
            z.kind = kind;
            if (Map* map = sMapMgr->FindBaseNonInstanceMap(mapId))
            {
                map->SetZoneWeather(zone, sDefs[kind].state, sDefs[kind].grade);
                Announce(map, zone, kind);
            }
        }
    }

    char const* WeatherName(WeatherKind k) { return k < WEATHER_COUNT ? sDefs[k].name : "?"; }

    WeatherKind WeatherAt(Unit* u)
    {
        if (!u || !u->IsInWorld())
            return WEATHER_CLEAR;
        auto itr = sNow.find({ u->GetMapId(), u->GetZoneId() });
        if (itr == sNow.end() || itr->second.kind == WEATHER_CLEAR || !u->IsOutdoors())
            return WEATHER_CLEAR;
        return itr->second.kind;
    }

    void SetWeather(uint32 mapId, uint32 zone, WeatherKind kind, bool forced)
    {
        ZoneWeather& z = sNow[{ mapId, zone }];
        z.forced = forced;
        z.untilMs = Now() + (forced ? 30 * MINUTE * IN_MILLISECONDS : urand(6, 14) * MINUTE * IN_MILLISECONDS);
        if (z.kind == kind)
            return;
        Apply(mapId, zone, kind);
    }

    void UpdateWeather(uint32 /*diff*/)
    {
        uint32 now = Now();
        for (Climate const& c : sClimates)
        {
            ZoneWeather& z = sNow[{ c.map, c.zone }];
            if (!z.untilMs)
            {
                z.untilMs = now + urand(1, 3) * MINUTE * IN_MILLISECONDS;     // the first change soon after start-up
                continue;
            }
            if (int32(now - z.untilMs) < 0)
                continue;
            z.forced = false;
            z.untilMs = now + urand(6, 14) * MINUTE * IN_MILLISECONDS;
            Apply(c.map, c.zone, Roll(c, z.kind));
        }
    }

    // a player's buff follows the weather where they stand (gone indoors); and the zone's look on arrival
    void UpdateWeatherAura(Player* p, uint32 diff)
    {
        uint32& t = sAuraTimer[p->GetGUID()];
        if (t > diff)
        {
            t -= diff;
            return;
        }
        t = 2000;
        WeatherKind k = WeatherAt(p);
        uint32 want = sDefs[k].aura;
        for (WeatherDef const& d : sDefs)
            if (d.aura && d.aura != want && p->HasAura(d.aura))
                p->RemoveAurasDueToSpell(d.aura);
        if (want && !p->HasAura(want))
            p->AddAura(want, p);
    }

    void ForgetWeather(ObjectGuid guid) { sAuraTimer.erase(guid); }

    void SendZoneWeatherTo(Player* p)
    {
        auto itr = sNow.find({ p->GetMapId(), p->GetZoneId() });
        Send(p, Acore::StringFormat("W^{}", sDefs[itr == sNow.end() ? WEATHER_CLEAR : itr->second.kind].name));
    }

    // ---------------------------------------------------------------- the rules
    int WeatherSlow(Unit* u)
    {
        WeatherKind k = WeatherAt(u);
        bool dwarf = u->IsPlayer() && u->ToPlayer()->getRace() == RACE_DWARF;
        switch (k)
        {
            case WEATHER_SNOWFALL: return dwarf ? 0 : 15;          // tenths of a metre
            case WEATHER_BLIZZARD: return dwarf ? 0 : 30;
            case WEATHER_DOWNPOUR:
            case WEATHER_THUNDERSTORM: return 15;
            default: return 0;
        }
    }

    int WeatherRangedPenalty(Unit* attacker)
    {
        switch (WeatherAt(attacker))
        {
            case WEATHER_FLURRIES: return 1;
            case WEATHER_SNOWFALL:
            case WEATHER_DOWNPOUR:
            case WEATHER_THUNDERSTORM: return 2;
            default: return 0;
        }
    }

    // disadvantage on a ranged attack from the weather (why: appended to the roll's reasons)
    bool WeatherRangedDisadvantage(Unit* attacker, Unit* target, std::string& why)
    {
        WeatherKind k = WeatherAt(attacker);
        float d = attacker->GetExactDist(target);
        if (k == WEATHER_GALE)
            why += " gale: disadvantage;";
        else if ((k == WEATHER_FOG || k == WEATHER_BLIZZARD) && d > 9.f)
            why += k == WEATHER_FOG ? " fog: disadvantage;" : " blizzard: disadvantage;";
        else
            return false;
        return true;
    }

    // damage taken by a unit standing in the weather: Drenched doubles lightning and cold and halves fire; snow and
    // drizzle add or take a little. Returns the new amount; note gets the reason.
    int WeatherDamage(Unit* victim, int amount, DmgType type, bool alreadyVulnerable, bool alreadyResisted, std::string& note)
    {
        if (amount <= 0)
            return amount;
        WeatherKind k = WeatherAt(victim);
        bool cold = type == DMG_COLD, fire = type == DMG_FIRE, shock = type == DMG_LIGHTNING;
        switch (k)
        {
            case WEATHER_DOWNPOUR:
            case WEATHER_THUNDERSTORM:
                if ((cold || shock) && !alreadyVulnerable)
                {
                    amount *= 2;                     // (a resisted hit comes back to whole, as 5e has it)
                    note += " (Drenched: vulnerable)";
                }
                else if (fire && !alreadyResisted)
                {
                    amount /= 2;
                    note += " (Drenched: fire resisted)";
                }
                return amount;
            case WEATHER_DRIZZLE:
                if (cold || shock)
                    amount += 2, note += " (Damp: +2)";
                else if (fire)
                    amount = std::max(0, amount - 2), note += " (Damp: -2)";
                return amount;
            case WEATHER_FLURRIES:
            case WEATHER_SNOWFALL:
            case WEATHER_BLIZZARD:
            {
                int n = k == WEATHER_FLURRIES ? 1 : (k == WEATHER_SNOWFALL ? 2 : 3);
                if (cold)
                    amount += n, note += Acore::StringFormat(" ({}: +{})", WeatherName(k), n);
                else if (fire)
                    amount = std::max(0, amount - n), note += Acore::StringFormat(" ({}: -{})", WeatherName(k), n);
                return amount;
            }
            default:
                return amount;
        }
    }

    float WeatherJump(Unit* u) { return WeatherAt(u) == WEATHER_GALE ? 1.5f : 1.f; }
    int WeatherPerception(Unit* u) { return WeatherAt(u) == WEATHER_FOG ? -5 : 0; }
}
