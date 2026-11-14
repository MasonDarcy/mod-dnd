/*
 * mod-dnd progression: zones, encounter clears and the level gates between zones.
 *
 * Each zone (world: dnd_zone) lists the areas it covers and the level a character needs to enter it. Encounters
 * belong to a zone and pay their XP once per character (characters: dnd_character_encounter), so clearing a zone
 * is what levels you up and opens the next one. A character below a zone's level is put back where it last
 * stood on allowed ground.
 */
#include "Dnd.h"
#include <algorithm>
#include <deque>

#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include <set>

namespace Dnd
{
    static std::map<uint32, ZoneDef> sZones;
    static std::unordered_map<ObjectGuid, std::set<uint32>> sCleared;

    struct GateState
    {
        uint32 timer = 0;
        bool hasSafe = false;
        uint32 safeMap = 0;
        Position safe;
        uint32 zoneId = 0;                   // the zone the player is standing in (0: none)
        uint32 lastWarnMs = 0;
        uint32 warnedZone = 0;               // the under-level zone last warned about
        std::deque<Position> trail;          // recent allowed positions (newest last), for turning people back
    };
    static std::unordered_map<ObjectGuid, GateState> sGate;

    static uint32 NowMs() { return getMSTime(); }

    void LoadZones()
    {
        sZones.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT id, name, min_level, areas, gate_text, clear_text FROM dnd_zone"))
        {
            do
            {
                Field* f = r->Fetch();
                ZoneDef& z = sZones[f[0].Get<uint32>()];
                z.id = f[0].Get<uint32>();
                z.name = f[1].Get<std::string>();
                z.minLevel = f[2].Get<uint8>();
                std::string areas = f[3].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(areas, ',', false))
                    if (!v.empty())
                        z.areas.push_back(uint32(std::atoi(std::string(v).c_str())));
                z.gateText = f[4].Get<std::string>();
                z.clearText = f[5].Get<std::string>();
            } while (r->NextRow());
        }
        // encounters, in id order (the order they are listed in the tracker)
        for (EncounterDef const* e : AllEncounters())
        {
            auto itr = sZones.find(e->zoneId);
            if (itr != sZones.end())
                itr->second.encounters.push_back(e->id);
        }
        LOG_INFO("module", "mod-dnd: {} zones", sZones.size());
    }

    ZoneDef const* GetZone(uint32 id)
    {
        auto itr = sZones.find(id);
        return itr == sZones.end() ? nullptr : &itr->second;
    }

    // a specific area wins over a whole zone
    static ZoneDef const* ZoneFor(uint32 zone, uint32 area)
    {
        ZoneDef const* byZone = nullptr;
        for (auto const& [id, z] : sZones)
            for (uint32 a : z.areas)
            {
                if (a == area)
                    return &z;
                if (a == zone && !byZone)
                    byZone = &z;
            }
        return byZone;
    }

    // ------------------------------------------------------------------ clears
    void LoadProgress(Player* p)
    {
        std::set<uint32>& s = sCleared[p->GetGUID()];
        s.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT encounter_id FROM dnd_character_encounter WHERE guid = {}",
            p->GetGUID().GetCounter()))
        {
            do
                s.insert(r->Fetch()[0].Get<uint32>());
            while (r->NextRow());
        }
        sGate.erase(p->GetGUID());
    }

    // A clear is written at once, the character's XP only when it's saved: a crash (or a killed server) in between keeps
    // the clear and loses the XP. At login: what the clears paid (the xp column; full for old rows) against the XP the
    // character has earned in all; any shortfall comes back.
    void SyncXp(Player* p)
    {
        QueryResult r = CharacterDatabase.Query("SELECT encounter_id, xp FROM dnd_character_encounter WHERE guid = {}",
            p->GetGUID().GetCounter());
        if (!r)
            return;
        uint32 owed = 0;
        do
        {
            Field* f = r->Fetch();
            EncounterDef const* def = GetEncounterDef(f[0].Get<uint32>());
            owed += f[1].IsNull() ? (def ? def->xp : 0) : f[1].Get<uint32>();
        } while (r->NextRow());
        uint32 earned = p->GetUInt32Value(PLAYER_XP);
        for (uint8 l = 1; l < p->GetLevel(); ++l)
            earned += sObjectMgr->GetXPForLevel(l);
        if (earned >= owed || p->GetLevel() >= MAX_DND_LEVEL)
            return;
        uint32 missing = owed - earned;
        Log(p, Acore::StringFormat("|cff40ff40+{} XP|r |cff909090(earned before the server went down, not saved in time - "
            "restored)|r", missing));
        p->GiveXP(missing, nullptr);
    }

    void ForgetProgress(ObjectGuid guid)
    {
        sCleared.erase(guid);
        sGate.erase(guid);
    }

    void ResetProgress(Player* p)
    {
        sCleared[p->GetGUID()].clear();
        CharacterDatabase.Execute("DELETE FROM dnd_character_encounter WHERE guid = {}", p->GetGUID().GetCounter());
        sGate[p->GetGUID()].zoneId = 0;      // re-push the tracker
    }

    bool HasCleared(Player* p, uint32 encounterId)
    {
        auto itr = sCleared.find(p->GetGUID());
        return itr != sCleared.end() && itr->second.count(encounterId);
    }

    static std::pair<uint32, uint32> Count(Player* p, ZoneDef const& z)
    {
        uint32 done = 0;
        for (uint32 id : z.encounters)
            if (HasCleared(p, id))
                ++done;
        return { done, uint32(z.encounters.size()) };
    }

    void OnEncounterWon(Player* p, EncounterDef const& def, bool peaceful, bool halfXp)
    {
        if (HasCleared(p, def.id))
        {
            Log(p, "|cff909090(Already cleared - no XP.)|r");
            return;
        }
        sCleared[p->GetGUID()].insert(def.id);
        uint32 xp = halfXp ? def.xp / 2 : def.xp;
        CharacterDatabase.Execute("REPLACE INTO dnd_character_encounter (guid, encounter_id, cleared_at, xp) VALUES ({}, {}, "
            "UNIX_TIMESTAMP(), {})", p->GetGUID().GetCounter(), def.id, xp);

        if (peaceful && halfXp)
            Log(p, Acore::StringFormat("|cffd0c060{} - resolved, on a sour note (half XP).|r", def.name));
        else if (peaceful)
            Log(p, Acore::StringFormat("|cff40ff40{} - resolved without bloodshed.|r", def.name));
        if (xp)
        {
            Log(p, Acore::StringFormat("|cff40ff40+{} XP|r", xp));
            p->GiveXP(xp, nullptr);
        }
        GiveRewards(p, def);                 // (the endgame's pay: items, coin, titles)

        ZoneDef const* z = GetZone(def.zoneId);
        if (!z)
            return;
        auto [done, total] = Count(p, *z);
        Log(p, Acore::StringFormat("|cffffd100{}: {}/{} encounters cleared.|r", z->name, done, total));
        if (done == total)
        {
            Announce(p, Acore::StringFormat("{} has cleared {}!", p->GetName(), z->name));
            Send(p, "V^" + z->name + " cleared");
            if (!z->clearText.empty())
                Log(p, "|cffffd100" + z->clearText + "|r");
        }
        PushProgress(p);
    }

    void ShareEncounterWon(std::vector<Player*> const& winners, EncounterDef const& def, bool peaceful, bool halfXp)
    {
        for (Player* p : winners)
            OnEncounterWon(p, def, peaceful, halfXp);
        if (winners.empty())
            return;
        // the self-test's bots and the real players never share: a test's wins would level whoever is playing
        bool bots = Test::IsBot(winners.front()->GetGUID());
        std::string names;
        for (std::size_t i = 0; i < winners.size(); ++i)
            names += (i == 0 ? "" : (i + 1 == winners.size() ? " and " : ", ")) + std::string(winners[i]->GetName());
        std::vector<Player*> others;         // gathered first: paying can level someone up mid-loop
        for (auto const& [guid, o] : ObjectAccessor::GetPlayers())
            if (o && o->IsInWorld() && Test::IsBot(guid) == bots && !HasCleared(o, def.id) &&
                std::find(winners.begin(), winners.end(), o) == winners.end())
                others.push_back(o);
        for (Player* o : others)
        {
            Log(o, Acore::StringFormat("|cff40ff40{} won {} - it counts for you too.|r", names, def.name));
            OnEncounterWon(o, def, peaceful, halfXp);
        }
    }

    // "Q^zone^done^total^name|cleared^..." for the zone the player stands in; "Q" alone hides the tracker
    void PushProgress(Player* p)
    {
        auto g = sGate.find(p->GetGUID());
        ZoneDef const* z = g == sGate.end() ? nullptr : GetZone(g->second.zoneId);
        Send(p, "MC");
        if (!z || z->encounters.empty())
        {
            Send(p, "Q");
            Send(p, "ME");
            return;
        }
        // map pins: "M^id^x^y^cleared^kind^map^name", kind 0 fight, 1 talk, 2 boss (x/y = world coordinates)
        for (uint32 id : z->encounters)
        {
            EncounterDef const* e = GetEncounterDef(id);
            if (!e || e->spawns.empty())
                continue;
            // the pin sits on the group's leader (its first spawn)
            CreatureData const* data = sObjectMgr->GetCreatureData(e->spawns.front());
            if (!data)
                continue;
            CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data->id);
            bool talker = e->dialogueNode && tmpl && tmpl->faction == 35;
            int kind = (e->xp >= 60 || e->legendary) ? 2 : (talker ? 1 : 0);   // (an endgame boss pays no XP)
            Send(p, Acore::StringFormat("M^{}^{:.1f}^{:.1f}^{}^{}^{}^{}", id, data->posX, data->posY, HasCleared(p, id) ? 1 : 0, kind,
                data->mapid, e->name.substr(0, 60)));
        }
        Send(p, "ME");
        auto [done, total] = Count(p, *z);
        std::string msg = Acore::StringFormat("Q^{}^{}^{}", z->name, done, total);
        for (uint32 id : z->encounters)
            if (EncounterDef const* e = GetEncounterDef(id))
            {
                std::string part = Acore::StringFormat("^{}|{}", e->name, HasCleared(p, id) ? 1 : 0);
                if (msg.size() + part.size() > 240)
                    break;
                msg += part;
            }
        Send(p, msg);
    }

    // ------------------------------------------------------------------ the gate
    void UpdateGate(Player* p, uint32 diff)
    {
        GateState& g = sGate[p->GetGUID()];
        g.timer += diff;
        if (g.timer < 250)
            return;
        g.timer = 0;
        if (!p->IsInWorld() || p->IsBeingTeleported() || p->IsInFlight() || !p->GetMap())
            return;

        // Borders are open: a built zone lets anyone in and only warns a character below its level. Zones not built
        // yet (level CLOSED_ZONE_LEVEL) are shut: their portcullises (tools/make_world_sql.py GATES) stand in phase
        // 1 << zone id, everyone is put in that phase so they see - and bump into - the gate, and the zone line turns
        // them back.
        uint32 mask = PHASEMASK_NORMAL;
        if (!p->IsGameMaster())
            for (auto const& [id, zd] : sZones)
                if (zd.minLevel >= CLOSED_ZONE_LEVEL && p->GetLevel() < zd.minLevel && id < 31)
                    mask |= 1u << id;
        if (p->GetPhaseMask() != mask)
            p->SetPhaseMask(mask, true);

        uint32 zone = 0, area = 0;
        p->GetMap()->GetZoneAndAreaId(PHASEMASK_NORMAL, zone, area, p->GetPositionX(), p->GetPositionY(), p->GetPositionZ());
        ZoneDef const* z = ZoneFor(zone, area);

        if (!z || p->GetLevel() >= z->minLevel || p->IsGameMaster() || z->minLevel < CLOSED_ZONE_LEVEL)
        {
            g.hasSafe = true;
            g.safeMap = p->GetMapId();
            g.safe = p->GetPosition();
            if (g.trail.empty() || g.trail.back().GetExactDist(&g.safe) > 1.f)
            {
                g.trail.push_back(g.safe);
                if (g.trail.size() > 40)
                    g.trail.pop_front();
            }
            uint32 zid = z ? z->id : 0;
            if (zid != g.zoneId)
            {
                g.zoneId = zid;
                // into a zone above the character's level: a warning, once (again only after a while, so walking
                // along a border doesn't repeat it)
                uint32 now = NowMs();
                if (z && p->GetLevel() < z->minLevel && !p->IsGameMaster()
                    && (zid != g.warnedZone || !g.lastWarnMs || now - g.lastWarnMs > 120000))
                {
                    g.warnedZone = zid;
                    g.lastWarnMs = now;
                    Log(p, Acore::StringFormat("|cffff8040{} is a level {} zone - you are level {}. The things here will be "
                        "more than a match for you.|r", z->name, z->minLevel, p->GetLevel()));
                    if (!z->gateText.empty())
                        Log(p, "|cffffd100" + z->gateText + "|r");
                    Send(p, "V^" + z->name + ": level " + std::to_string(z->minLevel));
                }
                PushProgress(p);
            }
            return;
        }

        // turned back
        uint32 now = NowMs();
        if (now - g.lastWarnMs > 4000 || !g.lastWarnMs)
        {
            g.lastWarnMs = now;
            Error(p, Acore::StringFormat("{} requires level {}.", z->name, z->minLevel));
            if (!z->gateText.empty())
                Log(p, "|cffffd100" + z->gateText + "|r");
            if (ZoneDef const* from = GetZone(g.zoneId))
                if (!from->encounters.empty())
                {
                    auto [done, total] = Count(p, *from);
                    Log(p, Acore::StringFormat("|cff909090{}: {}/{} encounters cleared.|r", from->name, done, total));
                }
            Send(p, "V^" + z->name + ": level " + std::to_string(z->minLevel));
        }
        // back to where they were a few steps ago - at least 8 m behind, so it's one clean step back, not a stutter
        Position const* back = nullptr;
        for (auto it = g.trail.rbegin(); it != g.trail.rend(); ++it)
            if (it->GetExactDist(p) >= 8.f)
            {
                back = &*it;
                break;
            }
        if (!back && g.hasSafe)
            back = &g.safe;
        if (back && g.safeMap == p->GetMapId())
        {
            float face = std::atan2(back->GetPositionY() - p->GetPositionY(), back->GetPositionX() - p->GetPositionX());
            p->NearTeleportTo(back->GetPositionX(), back->GetPositionY(), back->GetPositionZ(), face);
        }
        else
            p->TeleportTo(p->m_homebindMapId, p->m_homebindX, p->m_homebindY, p->m_homebindZ, p->GetOrientation());
    }
}
