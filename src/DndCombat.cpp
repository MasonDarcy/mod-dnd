/*
 * mod-dnd combat: encounters, initiative, turns, movement budget, opportunity attacks and the creature AI turn.
 *
 * All of this runs on the world thread (WorldScript::OnUpdate) or inside packet handlers / spell hits. With
 * MapUpdate.Threads = 1 the world thread waits for map updates, so nothing here runs concurrently.
 */
#include "Dnd.h"
#include "SpellMgr.h"
#include "DynamicObject.h"

#include "Bag.h"
#include "Config.h"
#include "GameGraveyard.h"
#include "Creature.h"
#include "GameTime.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "MotionMaster.h"
#include "PathGenerator.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "GameObject.h"
#include "DBCStores.h"
#include <algorithm>
#include <cmath>

namespace Dnd
{
    static std::map<uint32, Encounter> sEncounters;
    static std::unordered_map<ObjectGuid, uint32> sUnitEncounter;
    static std::unordered_map<ObjectGuid, uint32> sLastJoinCheck;
    static uint32 sNextEncounterId = 1;
    static std::unordered_map<ObjectGuid, uint32> sBrawlers;       // a villager in a brawl -> their own max health
    uint32 const BRAWLER_HP = 16;

    bool IsBrawler(ObjectGuid g) { return sBrawlers.count(g) != 0; }

    // a villager squaring up: a commoner who's been in a few of these
    CreatureStats const& BrawlerStats()
    {
        static CreatureStats st = []
        {
            CreatureStats b;
            b.ac = 11;
            b.hp = int(BRAWLER_HP);
            b.speed = 9.f;
            b.abil = { { 14, 12, 14, 10, 10, 10 } };
            b.melee.name = "Fists";
            b.melee.bonus = 4;
            b.melee.dice = Dice::Parse("1d4+2");
            b.melee.type = DMG_BLUDGEONING;
            b.xp = 0;
            return b;
        }();
        return st;
    }
    static constexpr uint32 MOVE_POINT_ID = 0xD0D;

    static uint32 Now() { return GameTime::GetGameTimeMS().count(); }

    // a player who just came back (graveyard, revive) can't be dragged into a fight for a few seconds
    static std::unordered_map<ObjectGuid, uint32> sGrace;
    static std::unordered_set<ObjectGuid> sDownedOut;    // down when their fight was won: waiting for a hand up
    static std::set<std::pair<ObjectGuid, uint32>> sFled;   // (player, encounter) they fled: walking near doesn't rejoin
    static float const FLEE_RANGE = 20.f;

    void GrantGrace(Player* p, uint32 ms) { if (p) sGrace[p->GetGUID()] = GameTime::GetGameTimeMS().count() + ms; }
    bool InGrace(Unit* u)
    {
        auto itr = u ? sGrace.find(u->GetGUID()) : sGrace.end();
        if (itr == sGrace.end())
            return false;
        if (GameTime::GetGameTimeMS().count() < itr->second)
            return true;
        sGrace.erase(itr);
        return false;
    }

    struct PendingRespawn { uint32 mapId, instanceId; ObjectGuid::LowType spawnId; uint32 dueMs; };
    static std::vector<PendingRespawn> sRespawns;

    // Brings a spawn back by its creature.guid. This core uses dynamic respawns: once a corpse is gone the Creature
    // object is destroyed and later re-created under a NEW guid, so a dead creature can't be found by its old guid.
    // A dead object still on the map is respawned on the spot; otherwise the map is told to re-create it now.
    bool RespawnSpawn(Map* map, ObjectGuid::LowType spawnId)
    {
        if (!map)
            return false;
        CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
        if (!data || data->mapid != map->GetId())
            return false;
        Creature* dead = nullptr;
        auto range = map->GetCreatureBySpawnIdStore().equal_range(spawnId);
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            Creature* cr = itr->second;
            if (!cr->IsAlive())
            {
                dead = cr;
                continue;
            }
            // already up: if a fight left it elsewhere (survivors of a defeat walk home and can get stuck),
            // put it back on its spot, healed and friendly again
            if (InCombat(cr))
                return false;
            Position home = cr->GetHomePosition();
            bool moved = cr->GetExactDist(&home) > 2.f;
            if (moved)
            {
                cr->GetMotionMaster()->Clear();
                cr->NearTeleportTo(home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), home.GetOrientation());
            }
            cr->SetStandState(UNIT_STAND_STATE_STAND);
            cr->SetFullHealth();
            cr->RestoreFaction();
            if (IsLeaving(cr->GetGUID()))
            {
                // it was walking off after a talk, with its despawn already scheduled: call that off, or the creature
                // just put back would vanish a moment later
                cr->m_Events.KillAllEvents(false);
                ClearLeaving(cr->GetGUID());
                moved = true;
            }
            return moved;
        }
        if (dead)
            dead->Respawn(true);
        else
            map->ForceCreatureRespawn(spawnId);
        return true;
    }

    Combatant* Encounter::Find(ObjectGuid g)
    {
        for (Combatant& c : order)
            if (c.guid == g)
                return &c;
        return nullptr;
    }

    Unit* Encounter::GetUnit(Combatant const& c) const
    {
        if (c.player)
        {
            Player* p = ObjectAccessor::FindPlayer(c.guid);
            return (p && p->GetMapId() == mapId && p->GetInstanceId() == instanceId) ? p : nullptr;
        }
        Map* m = sMapMgr->FindMap(mapId, instanceId);
        return m ? m->GetCreature(c.guid) : nullptr;
    }

    Encounter* EncounterOf(ObjectGuid g)
    {
        auto itr = sUnitEncounter.find(g);
        if (itr == sUnitEncounter.end())
            return nullptr;
        auto e = sEncounters.find(itr->second);
        return (e == sEncounters.end() || e->second.phase == Encounter::PHASE_DONE) ? nullptr : &e->second;
    }

    Combatant* CombatantOf(ObjectGuid g)
    {
        Encounter* e = EncounterOf(g);
        return e ? e->Find(g) : nullptr;
    }

    bool InCombat(Unit* u) { return u && EncounterOf(u->GetGUID()) != nullptr; }

    bool IsTurnOf(Unit* u)
    {
        Encounter* e = u ? EncounterOf(u->GetGUID()) : nullptr;
        if (!e)
            return false;
        Combatant* c = e->Current();
        return c && c->guid == u->GetGUID() && (e->phase == Encounter::PHASE_WAIT_PLAYER || !c->player);
    }

    static void Broadcast(Encounter& e, std::string const& text)
    {
        bool any = false;
        for (Combatant const& c : e.order)
            if (c.player && !c.gone)
                if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                {
                    SendLog(p, "L", text);
                    any = true;
                }
        FileLog(Acore::StringFormat("[E{} R{}] {}", e.id, e.round, text));
        (void)any;
    }

    static void Banner(Encounter& e, std::string const& text)
    {
        for (Combatant const& c : e.order)
            if (c.player && !c.gone)
                if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                    Send(p, "V^" + text);
    }

    void SetRooted(Player* p, bool rooted)
    {
        if (!p)
            return;
        // A plain server root: SMSG_FORCE_MOVE_ROOT, no stun animation, the player can still turn on the spot.
        p->SetControlled(rooted, UNIT_STATE_ROOT);
    }

    void PushEconomy(Player* p)
    {
        if (!p)
            return;
        Encounter* e = EncounterOf(p->GetGUID());
        Combatant* c = e ? e->Find(p->GetGUID()) : nullptr;
        if (!c)
        {
            Send(p, "E^0");
            return;
        }
        Send(p, Acore::StringFormat("E^1^{}^{}^{}^{}^{:.1f}^{:.1f}", IsTurnOf(p) ? 1 : 0,
            (!c->actionUsed || c->extraAttacks > 0 || (c->gwmReady && !c->bonusUsed)) ? 1 : 0, (!c->bonusUsed || c->bonusExtra) ? 1 : 0,
            c->reactionUsed ? 0 : 1, c->moved, c->moveMax));
    }

    // ------------------------------------------------------------------ what you can see
    // a lasting area's patch on the ground: (encounter, area spell, owner, where) -> the dynamic object showing it
    static std::map<std::tuple<uint32, uint32, uint64, int, int>, ObjectGuid> sGround;

    static uint32 GroundVisual(uint32 spell)
    {
        switch (spell)
        {
            case SPELL_CLOUD_OF_DAGGERS:  return GROUND_CLOUD_OF_DAGGERS;
            case SPELL_MOONBEAM:          return GROUND_MOONBEAM;
            case SPELL_STINKING_CLOUD:    return GROUND_STINKING_CLOUD;
            case SPELL_HUNGER_OF_HADAR:   return GROUND_HUNGER_OF_HADAR;
            case SPELL_SLEET_STORM:       return GROUND_SLEET_STORM;
            case SPELL_CREATE_BONFIRE:    return GROUND_BONFIRE;
            case SPELL_FLAMING_SPHERE:    return GROUND_FLAMING_SPHERE;
            case SPELL_BLACK_TENTACLES:   return GROUND_BLACK_TENTACLES;
            default:                      return 0;
        }
    }

    // a concentration spell with a look of its own on the caster (the rest show "Concentrating")
    static uint32 ConcentrationVisual(uint32 spell)
    {
        switch (spell)
        {
            case SPELL_BARKSKIN:          return STATUS_BARKSKIN;
            case SPELL_BLUR:              return STATUS_BLUR;
            case SPELL_SPIRIT_GUARDIANS:  return STATUS_SPIRIT_GUARDIANS;
            default:                      return STATUS_CONCENTRATING;
        }
    }

    // every combatant wears the auras of what's on them, and every lasting area has its patch on the ground
    void SyncVisuals(Encounter& e)
    {
        e.lastVisualSync = Now();
        Unit* anyone = nullptr;
        for (Combatant& c : e.order)
        {
            Unit* u = c.gone ? nullptr : e.GetUnit(c);
            if (!u || !u->IsInWorld())
                continue;
            if (!anyone)
                anyone = u;
            std::set<uint32> want;
            if (c.alive && u->IsAlive())
            {
                auto on = [&want](bool yes, uint32 id) { if (yes) want.insert(id); };
                on(c.asleep, STATUS_ASLEEP);
                on(c.held || c.lairHeld, STATUS_HELD);
                on(c.restrained, STATUS_RESTRAINED);
                on(c.entranced, STATUS_ENTRANCED);
                on(c.afraid || c.frightened, STATUS_FRIGHTENED);
                on(c.turned, STATUS_TURNED);
                on(c.poisoned > 0, STATUS_POISONED);
                on(!c.burningBy.IsEmpty(), STATUS_BURNING);
                on(c.outlined, STATUS_OUTLINED);
                on(!c.blessedBy.IsEmpty(), STATUS_BLESSED);
                on(!c.faithBy.IsEmpty(), STATUS_SHIELD_OF_FAITH);
                on(!c.heroismBy.IsEmpty(), STATUS_HEROISM);
                on(c.stunned > 0, STATUS_STUNNED);
                on(c.blinded, STATUS_BLINDED);
                on(!c.banedBy.IsEmpty(), STATUS_BANED);
                on(!c.hastedBy.IsEmpty(), STATUS_HASTED);
                on(!c.slowBy.IsEmpty(), STATUS_SLOWED);
                on(!c.sanctuaryBy.IsEmpty(), STATUS_SANCTUARY);
                on(c.images > 0, STATUS_MIRROR_IMAGE);
                on(!c.enfeebledBy.IsEmpty(), STATUS_ENFEEBLED);
                on(!c.laughingBy.IsEmpty(), STATUS_LAUGHING);
                on(c.bladeWard, STATUS_BLADE_WARD);
                on(!c.banishedBy.IsEmpty(), STATUS_BANISHED);
                on(!c.invisibleBy.IsEmpty(), STATUS_INVISIBLE);
                on(!c.stoneskinBy.IsEmpty(), STATUS_STONESKIN);
                on(c.deathWard, STATUS_DEATH_WARD);
                on(c.fireShield, STATUS_FIRE_SHIELD);
                if (c.concentration && ConcentrationLive(e, c))
                    want.insert(ConcentrationVisual(c.concentration));
            }
            for (uint32 id = STATUS_ASLEEP; id <= STATUS_LAST; ++id)
            {
                bool has = u->HasAura(id);
                if (want.count(id) && !has)
                    u->AddAura(id, u);
                else if (!want.count(id) && has)
                    u->RemoveAurasDueToSpell(id);
            }
        }
        std::set<std::tuple<uint32, uint32, uint64, int, int>> live;
        for (Area const& a : e.areas)
        {
            uint32 vis = GroundVisual(a.spellId);
            if (!vis)
                continue;
            auto key = std::make_tuple(e.id, a.spellId, a.owner.GetRawValue(), int(a.pos.GetPositionX() * 10.f), int(a.pos.GetPositionY() * 10.f));
            live.insert(key);
            if (sGround.count(key))
                continue;
            Combatant* oc = e.Find(a.owner);
            Unit* caster = oc ? e.GetUnit(*oc) : nullptr;
            if (!caster || !caster->IsInWorld())
                continue;
            DynamicObject* d = new DynamicObject();
            if (!d->CreateDynamicObject(caster->GetMap()->GenerateLowGuid<HighGuid::DynamicObject>(), caster, vis, a.pos, a.radius,
                DYNAMIC_OBJECT_AREA_SPELL))
            {
                delete d;
                continue;
            }
            d->SetDuration(600000);              // (taken away when the area goes)
            sGround[key] = d->GetGUID();
        }
        for (auto itr = sGround.begin(); itr != sGround.end();)
        {
            if (std::get<0>(itr->first) != e.id || live.count(itr->first))
            {
                ++itr;
                continue;
            }
            if (anyone)
                if (DynamicObject* d = ObjectAccessor::GetDynamicObject(*anyone, itr->second))
                    d->Remove();
            itr = sGround.erase(itr);
        }
    }

    // the fight is over: nobody wears its auras, nothing of it stays on the ground
    static void ClearVisuals(Encounter& e)
    {
        Unit* anyone = nullptr;
        for (Combatant& c : e.order)
        {
            Unit* u = c.gone ? nullptr : e.GetUnit(c);
            if (!u)
                continue;
            if (!anyone)
                anyone = u;
            for (uint32 id = STATUS_ASLEEP; id <= STATUS_LAST; ++id)
                u->RemoveAurasDueToSpell(id);
            u->RemoveAurasDueToSpell(STATUS_MARKER);
        }
        for (auto itr = sGround.begin(); itr != sGround.end();)
        {
            if (std::get<0>(itr->first) != e.id)
            {
                ++itr;
                continue;
            }
            if (anyone)
                if (DynamicObject* d = ObjectAccessor::GetDynamicObject(*anyone, itr->second))
                    d->Remove();
            itr = sGround.erase(itr);
        }
    }

    // hovering an area spell on the action bar: an arrow over whoever it would catch, from where you stand now
    static std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> sMarked;
    void ForgetAoePreview(ObjectGuid g) { sMarked.erase(g); }

    // the range ring: a glowing rune on the ground under a player hovering a spell in a fight, scaled to the spell's reach
    // (or, for one all round them, its radius) - a game object (data/sql 2026_11_16_03_dnd_range_ring.sql), moved with them
    static uint32 const RANGE_RING_ENTRY = 9100195, RANGE_RING_DISPLAY = 7783;  // Nexus_Sigil_Blue_01: one flat sigil
    struct RangeRing { ObjectGuid go; float r = 0.f; Position at; };
    static std::unordered_map<ObjectGuid, RangeRing> sRangeRings;

    static void ClearRangeRing(Player* p)
    {
        auto itr = sRangeRings.find(p->GetGUID());
        if (itr == sRangeRings.end())
            return;
        if (GameObject* go = p->GetMap()->GetGameObject(itr->second.go))
        {
            go->SetRespawnTime(0);
            go->Delete();
        }
        sRangeRings.erase(itr);
    }

    static void ShowRangeRing(Player* p, float r)
    {
        auto itr = sRangeRings.find(p->GetGUID());
        if (itr != sRangeRings.end() && std::fabs(itr->second.r - r) < 0.05f && itr->second.at.GetExactDist2d(p) < 0.4f &&
            p->GetMap()->GetGameObject(itr->second.go))
            return;                              // already there, the right size
        ClearRangeRing(p);
        if (r <= 0.f)
            return;
        // the sigil's main circle sits well inside the model's box (margin, and the knots outside it): ~2/3 of it, by eye
        static float const RING_VISIBLE = 0.66f;
        float base = 9.4f;
        if (GameObjectDisplayInfoEntry const* di = sGameObjectDisplayInfoStore.LookupEntry(RANGE_RING_DISPLAY))
            base = std::max(0.1f, std::max(di->maxX - di->minX, di->maxY - di->minY) / 2.f);
        base *= RING_VISIBLE;
        // built by hand, not summoned: its scale has to be set before the client first sees it (set after, it showed at
        // the model's own size)
        Map* map = p->GetMap();
        GameObject* go = new GameObject();
        if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(), RANGE_RING_ENTRY, map, p->GetPhaseMask(), p->GetPositionX(),
            p->GetPositionY(), p->GetPositionZ() + 0.05f, 0.f, G3D::Quat(), 100, GO_STATE_READY))
        {
            delete go;
            return;
        }
        go->SetObjectScale(r / base);
        go->SetRespawnTime(120);
        go->SetSpawnedByDefault(false);
        map->AddToMap(go, true);
        sRangeRings[p->GetGUID()] = RangeRing{ go->GetGUID(), r, p->GetPosition() };
    }

    void AoePreview(Player* p, std::string const& spellName)
    {
        auto& marks = sMarked[p->GetGUID()];
        std::vector<ObjectGuid> want;            // who the arrows should be over now
        // only the changes: an arrow taken off and put back restarts its bob (the refresh comes every 0.4 s)
        auto settle = [&]()
        {
            for (ObjectGuid g : marks)
                if (std::find(want.begin(), want.end(), g) == want.end())
                    if (Unit* u = ObjectAccessor::GetUnit(*p, g))
                        u->RemoveAurasDueToSpell(STATUS_MARKER);
            for (ObjectGuid g : want)
                if (std::find(marks.begin(), marks.end(), g) == marks.end())
                    if (Unit* u = ObjectAccessor::GetUnit(*p, g))
                        u->AddAura(STATUS_MARKER, u);
            marks = want;
        };
        Encounter* e = EncounterOf(p->GetGUID());
        if (!e || spellName.empty())
        {
            ClearRangeRing(p);
            settle();
            return;
        }
        SpellInfo const* si = nullptr;
        for (uint32 id : AllActionIds())
            if (SpellInfo const* s = sSpellMgr->GetSpellInfo(id); s && s->SpellName[0] && spellName == s->SpellName[0] && p->HasSpell(id))
                si = s;
        if (!si)
        {
            ClearRangeRing(p);
            settle();
            return;
        }
        float radius = 0.f, cone = 0.f;
        if (si->Id == SPELL_THUNDERWAVE || si->Id == SPELL_SPIRIT_GUARDIANS)
            radius = 4.5f;                        // (all round you: the rules' own reach, DndSpells.cpp)
        else if (si->Id == SPELL_EARTH_TREMOR)
            radius = 3.f;
        else if (si->Id == SPELL_WORD_OF_RADIANCE || si->Id == SPELL_THUNDERCLAP)
            radius = NOMINAL_MELEE_RANGE;         // (right beside you: melee reach)
        else if (si->Id == SPELL_ARMS_OF_HADAR || si->Id == SPELL_FEY_PRESENCE)
            radius = 3.f;
        else if (si->Id == SPELL_COLOR_SPRAY)
        {
            radius = 4.5f;
            cone = float(M_PI) / 3.f;
        }
        else
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (si->Effects[i].TargetA.GetSelectionCategory() == TARGET_SELECT_CATEGORY_CONE)
                {
                    radius = si->Effects[i].CalcRadius(p);
                    cone = float(M_PI) / 3.f;    // WoW's cones: 60 degrees ahead of you (Spell::SelectImplicitConeTargets)
                }
        // the ring: the edge of a spell that goes off all round you (an area, or a cone's length); none for a spell
        // aimed at someone or somewhere
        ShowRangeRing(p, radius);
        if (radius > 0.f)
            for (Combatant const& o : e->order)
            {
                if (o.player || o.gone || !o.alive)
                    continue;
                Unit* u = e->GetUnit(o);
                if (!u || !u->IsAlive() || p->GetExactDist(u) > radius + AreaEdge(u) || (cone > 0.f && !p->HasInArc(cone, u)))
                    continue;
                want.push_back(u->GetGUID());
            }
        settle();
    }

    // a combatant's conditions, for the turn order's portraits ("Held,Asleep,...")
    static std::string Conditions(Combatant const& c)
    {
        std::string out;
        auto add = [&out](bool on, char const* tag)
        {
            if (on)
                out += (out.empty() ? "" : ",") + std::string(tag);
        };
        add(c.held || c.lairHeld, "Held");
        add(c.asleep, "Asleep");
        add(c.stunned > 0, "Stunned");
        add(c.restrained, "Restrained");
        add(c.entranced, "Entranced");
        add(c.turned, "Turned");
        add(c.afraid || c.frightened, "Frightened");
        add(c.poisoned > 0, "Poisoned");
        add(!c.burningBy.IsEmpty(), "Burning");
        add(c.corroded > 0, "Corroded");
        add(c.slowed, "Slowed");
        add(c.outlined, "Outlined");
        add(c.guided, "Guided");
        add(c.mocked, "Mocked");
        add(!c.hexedBy.IsEmpty(), "Hexed");
        add(c.surprised, "Surprised");
        add(!c.blessedBy.IsEmpty(), "Blessed");
        add(!c.faithBy.IsEmpty(), "Shielded");
        add(c.raging, "Raging");
        add(c.dodging, "Dodging");
        add(c.hidden, "Hidden");
        add(c.blinded, "Blinded");
        add(!c.banedBy.IsEmpty(), "Baned");
        add(!c.hastedBy.IsEmpty(), "Hasted");
        add(!c.slowBy.IsEmpty(), "Slowed");
        add(!c.sanctuaryBy.IsEmpty(), "Sanctuary");
        add(c.images > 0, "Images");
        add(!c.enfeebledBy.IsEmpty(), "Enfeebled");
        add(!c.laughingBy.IsEmpty(), "Laughing");
        add(c.commanded, "Commanded");
        add(c.lethargic, "Lethargic");
        add(c.bladeWard, "Blade Ward");
        add(c.acidDice > 0, "Acid");
        add(!c.banishedBy.IsEmpty(), "Banished");
        add(!c.invisibleBy.IsEmpty(), "Invisible");
        add(!c.stoneskinBy.IsEmpty(), "Stoneskin");
        add(c.deathWard, "Death Ward");
        add(c.fireShield, "Fire Shield");
        return out;
    }

    // ------------------------------------------------------------------ the creatures' unit ids ("boss1".."boss4")
    // The turn bar's click-to-target is a secure macro, and WoW can only target a creature by name - two kobolds of one
    // name, and a click takes the nearer. So the creatures in a fight are engaged as the client's "boss" units (the
    // raid boss frames' SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT; the addon hides those frames): a portrait whose creature
    // has one targets exactly that creature. Four at most - the ones sharing a name first (a unique name targets by
    // name well enough), then the rest in turn order.
    static std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> sBossUnits;   // player -> the creatures engaged for them
    static uint32 const BOSS_UNITS = 4;

    static void SendEncounterUnit(Player* p, uint32 type, ObjectGuid unit)
    {
        WorldPacket data(SMSG_UPDATE_INSTANCE_ENCOUNTER_UNIT, 15);
        data << uint32(type);                // 0 engage, 1 disengage
        data << unit.WriteAsPacked();
        data << uint8(0);                    // priority
        p->SendDirectMessage(&data);
    }

    static void ReleaseBossUnits(Player* p)
    {
        auto itr = sBossUnits.find(p->GetGUID());
        if (itr == sBossUnits.end())
            return;
        for (ObjectGuid g : itr->second)
            SendEncounterUnit(p, 1, g);
        sBossUnits.erase(itr);
    }

    std::vector<ObjectGuid> BossUnitsOf(ObjectGuid player)
    {
        auto itr = sBossUnits.find(player);
        return itr == sBossUnits.end() ? std::vector<ObjectGuid>{} : itr->second;
    }

    static void SyncBossUnits(Encounter& e)
    {
        // who should have one: alive creatures, the ones sharing a name with another first
        std::map<std::string, int> names;
        for (Combatant const& c : e.order)
            if (!c.player && !c.gone && c.alive)
                ++names[c.name];
        std::vector<ObjectGuid> want;
        for (int pass = 0; pass < 2; ++pass)
            for (Combatant const& c : e.order)
                if (!c.player && !c.gone && c.alive && (names[c.name] > 1) == (pass == 0) && want.size() < BOSS_UNITS)
                    want.push_back(c.guid);
        for (Combatant const& viewer : e.order)
        {
            if (!viewer.player || viewer.gone)
                continue;
            Player* p = ObjectAccessor::FindPlayer(viewer.guid);
            if (!p)
                continue;
            std::vector<ObjectGuid>& have = sBossUnits[p->GetGUID()];
            if (have == want)
                continue;
            for (ObjectGuid g : have)                    // let go of whoever is no longer wanted (or dead)...
                if (std::find(want.begin(), want.end(), g) == want.end())
                    SendEncounterUnit(p, 1, g);
            for (ObjectGuid g : want)                    // ...and engage the new ones (each takes the first free bossN)
                if (std::find(have.begin(), have.end(), g) == have.end())
                    SendEncounterUnit(p, 0, g);
            have = want;
        }
    }

    void PushOrder(Encounter& e)
    {
        SyncBossUnits(e);
        for (Combatant const& viewer : e.order)
        {
            if (!viewer.player || viewer.gone)
                continue;
            Player* p = ObjectAccessor::FindPlayer(viewer.guid);
            if (!p)
                continue;
            Send(p, "OC");
            for (std::size_t i = 0; i < e.order.size(); ++i)
            {
                Combatant const& c = e.order[i];
                if (c.gone)
                    continue;
                Unit* u = e.GetUnit(c);
                uint32 hp = u && !c.downed ? u->GetHealth() : 0;
                uint32 maxHp = u ? u->GetMaxHealth() : 0;
                bool alive = c.alive && u && u->IsAlive();
                uint32 kind = (u && u->ToCreature()) ? u->ToCreature()->GetCreatureTemplate()->type : 0;
                std::string down = !c.downed ? "0" : (c.stable ? "S" : Acore::StringFormat("D{}{}", c.saveOk, c.saveFail));
                Send(p, Acore::StringFormat("O^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^0x{:016X}^{}^{}^{}^{}", i, c.name.substr(0, 40), hp, maxHp,
                    c.player ? 1 : 0, alive ? 1 : 0, c.initiative, int(i) == e.cur ? 1 : 0, c.guid == viewer.guid ? 1 : 0,
                    c.prone ? 1 : 0, c.guid.GetRawValue(), kind, ConcentrationLive(e, c) ? 1 : 0, down, Conditions(c)));
            }
            // what you're concentrating on, by name (the turn panel shows it; empty: nothing)
            std::string conc;
            if (viewer.concentration && ConcentrationLive(e, viewer))
                if (SpellInfo const* si = sSpellMgr->GetSpellInfo(viewer.concentration))
                    conc = si->SpellName[0];
            Send(p, "CN^" + conc);
            Send(p, Acore::StringFormat("OE^{}^{}^{}", e.round, e.cur, e.name.substr(0, 60)));
        }
        e.lastOrderPush = Now();
        SyncVisuals(e);                          // (whatever changed the order changed what's on whom)
    }

    // P^x,y,reach^x,y,reach^...   in turn-order index order; the addon draws them relative to the player
    // who could make an Opportunity Attack on this player right now: enemies whose reach they stand in, with a
    // reaction left, unless the player has Disengaged. "T^i,j" (turn-order indices), only sent when it changes.
    static std::unordered_map<ObjectGuid, std::string> sThreatSent;

    static void PushThreats(Encounter& e, Combatant const& viewer, Player* p)
    {
        std::string list;
        if (!viewer.disengaged && p->IsAlive())
            for (std::size_t i = 0; i < e.order.size(); ++i)
            {
                Combatant const& o = e.order[i];
                if (o.player || !o.alive || o.gone || o.reactionUsed || o.noReaction || o.prone)
                    continue;
                Unit* ou = e.GetUnit(o);
                Creature* cr = ou ? ou->ToCreature() : nullptr;
                if (!cr || !cr->IsAlive() || !StatsOrDefault(cr).melee.valid())
                    continue;
                if (cr->GetExactDist(p) <= MeleeRange(cr, p))
                    list += (list.empty() ? "" : ",") + std::to_string(i);
            }
        std::string& sent = sThreatSent[p->GetGUID()];
        if (sent != list)
        {
            sent = list;
            Send(p, "T^" + list);
        }
    }

    void PushPositions(Encounter& e)
    {
        std::string msg = "P";
        for (Combatant const& c : e.order)
        {
            Unit* u = c.gone ? nullptr : e.GetUnit(c);
            if (!u)
                msg += "^-";
            else
                msg += Acore::StringFormat("^{:.1f},{:.1f},{:.1f}", u->GetPositionX(), u->GetPositionY(), u->GetCombatReach());
        }
        for (Combatant const& viewer : e.order)
            if (viewer.player && !viewer.gone)
                if (Player* p = ObjectAccessor::FindPlayer(viewer.guid))
                {
                    Send(p, msg);
                    PushThreats(e, viewer, p);
                }
        e.lastPositionPush = Now();
    }

    // ------------------------------------------------------------------ economy
    bool CanPay(Unit* u, Cost cost, std::string& why)
    {
        Encounter* e = EncounterOf(u->GetGUID());
        if (!e)
            return true;                     // exploration: no action economy
        Combatant* c = e->Find(u->GetGUID());
        if (!c || !IsTurnOf(u))
        {
            why = "It is not your turn.";
            return false;
        }
        if (cost == COST_ACTION && c->actionUsed)
        {
            why = "You have no Action left this turn.";
            return false;
        }
        if (cost == COST_BONUS && c->bonusUsed && !c->bonusExtra)
        {
            why = "You have no Bonus Action left this turn.";
            return false;
        }
        return true;
    }

    void Pay(Unit* u, Cost cost)
    {
        Combatant* c = CombatantOf(u->GetGUID());
        if (!c || cost == COST_NONE)
            return;
        if (cost == COST_ACTION)
            c->actionUsed = true;
        else if (cost == COST_BONUS && c->bonusUsed && c->bonusExtra)
            --c->bonusExtra;                     // Fast Hands: the second Bonus Action
        else if (cost == COST_BONUS)
            c->bonusUsed = true;
        if (Player* p = u->ToPlayer())
            PushEconomy(p);
    }

    // ------------------------------------------------------------------ start / join
    // a player's initiative bonus (the fight's roll and the sheet both use it): DEX, Alert +5, gear, a bard's Jack of
    // All Trades (half proficiency, from 2)
    int InitiativeBonus(Player* p)
    {
        return AbilityMod(p, DEX) + (HasFeat(p, "Alert") ? 5 : 0) + ItemInitiative(p) +
            (IsClass(p, CLASS_BARD) && Level(p) >= 2 ? Prof(Level(p)) / 2 : 0);
    }

    static Combatant MakeCombatant(Unit* u, std::string& rollText)
    {
        Combatant c;
        c.guid = u->GetGUID();
        c.player = u->IsPlayer();
        c.name = u->GetName();
        c.dexMod = AbilityMod(u, DEX);
        // Alert: +5 to initiative
        D20Result r = RollD20(u->IsPlayer() ? InitiativeBonus(u->ToPlayer()) : c.dexMod, 0);
        c.initiative = r.total;
        if (c.player)
            ApplyPreBuffs(c);                // Bless, Haste... cast before the fight (DndSpells.cpp)
        c.lastPos = u->GetPosition();
        if (Creature* cr = u->ToCreature())
            RollCarried(c, cr);                  // the potions and scrolls it has on it this time
        rollText = Acore::StringFormat("|cffaaaaaaInitiative|r  {}: {}", c.name, r.text);
        return c;
    }

    static void SortOrder(Encounter& e)
    {
        std::stable_sort(e.order.begin(), e.order.end(), [](Combatant const& a, Combatant const& b)
        {
            if (a.initiative != b.initiative)
                return a.initiative > b.initiative;
            if (a.dexMod != b.dexMod)
                return a.dexMod > b.dexMod;
            return a.player && !b.player;    // players win full ties, like BG3
        });
    }

    static void ScaleForParty(Encounter& e);

    // a wipe is a reload: what each player had as they entered the fight (their sheet - slots, rage, ki, every
    // per-rest resource - and how many of each consumable they carried) comes back when nobody is left standing
    struct FightSnapshot { Sheet sheet; std::map<uint32, uint32> consumables; };
    static std::unordered_map<ObjectGuid, FightSnapshot> sSnapshots;
    static bool sWiping = false;

    static void TakeSnapshot(Player* p)
    {
        FightSnapshot snap;
        snap.sheet = GetSheet(p);
        auto note = [&](Item* item)
        {
            if (item && item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                snap.consumables[item->GetEntry()] = p->GetItemCount(item->GetEntry(), false);
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            note(p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* b = p->GetBagByPos(bag))
                for (uint32 slot = 0; slot < b->GetBagSize(); ++slot)
                    note(b->GetItemByPos(uint8(slot)));
        sSnapshots[p->GetGUID()] = std::move(snap);
    }

    static void DownedLook(Player* p, bool down);

    // back at the nearest graveyard, as if the fight never happened
    static void Reload(Player* p, uint32 defId)
    {
        if (auto itr = sSnapshots.find(p->GetGUID()); itr != sSnapshots.end())
        {
            GetSheet(p) = itr->second.sheet;
            for (auto const& [entry, count] : itr->second.consumables)
            {
                uint32 now = p->GetItemCount(entry, false);
                if (now < count)
                    p->AddItem(entry, count - now);
            }
            sSnapshots.erase(itr);
            SaveSheet(p);
        }
        sDownedOut.erase(p->GetGUID());
        DownedLook(p, false);
        p->RemoveAurasDueToSpell(SPELL_FALLEN);
        if (!p->IsAlive())
        {
            p->ResurrectPlayer(1.0f);
            p->SpawnCorpseBones();
        }
        p->SetFullHealth();
        GrantGrace(p, 10000);
        if (defId)
            ForgetGrudge(p, defId);
        if (GraveyardStruct const* g = sGraveyard->GetClosestGraveyard(p, p->GetTeamId()))
        {
            if (g->Map == p->GetMapId())
                p->NearTeleportTo(g->x, g->y, g->z, p->GetOrientation());
            else
                p->TeleportTo(g->Map, g->x, g->y, g->z, p->GetOrientation());
        }
        Log(p, "|cffc8b890Everything goes dark... and you wake at the graveyard, as if the fight never happened. What you "
            "spent in it is back.|r");
        PushSheet(p);
    }

    // gear that does something as a fight starts: temporary hit points
    static void FightStartGear(Player* p)
    {
        TakeSnapshot(p);
        if (int t = ItemTempHp(p))
        {
            Sheet& s = GetSheet(p);
            if (s.tempHp < t)
            {
                s.tempHp = t;
                Log(p, Acore::StringFormat("|cff80c0ffYour gear braces you for the fight: {} temporary hit points.|r", t));
                PushSheet(p);
            }
        }
    }

    void StartEncounter(Creature* source, Unit* trigger)
    {
        if (!source || !source->IsAlive() || InCombat(source))
            return;
        Player* triggerPlayer = trigger ? trigger->ToPlayer() : nullptr;
        if (triggerPlayer && (triggerPlayer->IsGameMaster() || !triggerPlayer->IsAlive() || InGrace(triggerPlayer) || IsDowned(triggerPlayer) ||
            triggerPlayer->IsInFlight()))
            return;
        // an attack from hiding is still resolving (WoW engages as the blow lands): ResolveAmbush decides afterwards
        if (triggerPlayer && IsSneaking(triggerPlayer) && SneakStruck(triggerPlayer))
            return;

        EncounterDef const* def = GetEncounterDefBySpawn(source->GetSpawnId());
        Map* map = source->GetMap();

        Encounter e;
        e.id = sNextEncounterId++;
        e.defId = def ? def->id : 0;
        e.name = def ? def->name : source->GetName();
        e.mapId = map->GetId();
        e.instanceId = map->GetInstanceId();

        std::vector<Creature*> creatures;
        if (def)
        {
            for (uint32 spawn : def->spawns)
            {
                auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
                for (auto itr = range.first; itr != range.second; ++itr)
                    if (itr->second->IsAlive() && itr->second->IsInWorld() && !InCombat(itr->second) && !IsLeaving(itr->second->GetGUID()))
                        creatures.push_back(itr->second);
            }
        }
        if (std::find(creatures.begin(), creatures.end(), source) == creatures.end())
            creatures.push_back(source);
        // whoever is in the fight is an enemy for its duration (talkers spawn friendly; a defeat restores them)
        for (Creature* c : creatures)
            if (triggerPlayer ? !c->IsHostileTo(triggerPlayer) : (c->GetFaction() == 35 || c->GetFaction() == TALKER_FACTION))
                c->SetFaction(14);

        float joinRange = def ? def->joinRange : 30.f;
        std::vector<Player*> players;
        map->DoForAllPlayers([&](Player* p)
        {
            if (!p->IsAlive() || p->IsGameMaster() || InCombat(p) || IsDowned(p) || p->IsInFlight())
                return;
            for (Creature* c : creatures)
                if (p->GetExactDist(c) <= joinRange)
                {
                    players.push_back(p);
                    return;
                }
        });
        if (triggerPlayer && std::find(players.begin(), players.end(), triggerPlayer) == players.end() && !InCombat(triggerPlayer))
            players.push_back(triggerPlayer);
        if (players.empty())
            return;
        // anyone talking (to this group or another) when the fight takes them in: the conversation is over
        for (Player* p : players)
            if (InDialogue(p))
            {
                CloseDialogue(p, false);
                Log(p, "|cffff8060The talk is cut short - a fight breaks out!|r");
            }

        // a talker's group remembers who it fought
        if (def && def->dialogueNode)
            for (Player* p : players)
                AddGrudge(p, def->id);

        auto [itr, ok] = sEncounters.emplace(e.id, std::move(e));
        Encounter& enc = itr->second;
        std::vector<std::string> rolls;
        for (Player* p : players)
        {
            std::string t;
            enc.order.push_back(MakeCombatant(p, t));
            rolls.push_back(t);
            sUnitEncounter[p->GetGUID()] = enc.id;
        }
        for (Creature* c : creatures)
        {
            std::string t;
            enc.order.push_back(MakeCombatant(c, t));
            rolls.push_back(t);
            sUnitEncounter[c->GetGUID()] = enc.id;
            c->GetMotionMaster()->Clear();
            c->StopMoving();
            c->SetFacingToObject(players.front());
        }
        Banner(enc, "COMBAT: " + enc.name);
        Broadcast(enc, Acore::StringFormat("|cffff8040=== Combat: {} ===|r", enc.name));
        for (std::string const& t : rolls)
            Broadcast(enc, t);
        // a sneaking rogue: whoever never noticed them is Surprised (those who hadn't come close roll now)
        // ...and if nobody noticed them at all, they start the fight Hidden: their first attack has advantage (WoW puts
        // them in combat as their strike lands, so this is what carries the ambush into the fight)
        for (Player* p : players)
            if (IsSneaking(p))
            {
                bool unseen = true;
                for (Combatant& c : enc.order)
                    if (!c.player)
                        if (Creature* cr = ObjectAccessor::GetCreature(*p, c.guid))
                        {
                            if (!SpotsSneaker(cr, p))
                            {
                                c.surprised = true;
                                Broadcast(enc, Acore::StringFormat("{} never saw {} coming: |cffffd100Surprised|r!", c.name, p->GetName()));
                            }
                            else
                                unseen = false;
                        }
                unseen = unseen && !SneakRevealed(p);   // (an attack from hiding has already given you away)
                for (Combatant& c : enc.order)
                    if (c.guid == p->GetGUID())
                        c.hidden = unseen;
                if (unseen)
                {
                    p->AddAura(SPELL_HIDDEN_AURA, p);
                    Broadcast(enc, Acore::StringFormat("{} is still |cff9ad0ffHidden|r - nobody has seen them.", p->GetName()));
                }
                StopSneaking(p);
            }
        SortOrder(enc);
        ScaleForParty(enc);
        for (Player* p : players)
        {
            SetRooted(p, true);
            p->AttackStop();
        }
        enc.cur = -1;
        enc.round = 0;
        enc.phase = Encounter::PHASE_NEXT;
        enc.timer = 1500;
        PushOrder(enc);
        for (Player* p : players)
        {
            FightStartGear(p);
            PushEconomy(p);
        }
    }

    // Every fight is tuned for a fixed party (Dnd.PartySize, 2 by default) whoever turns up: no creatures are ever
    // brought in for it. A group's hit points x(1 + 0.5 per player beyond the first); a lone creature's x(1 + 0.75)
    // and a legendary action a round per player beyond the first. (The self-test sets its own size: 1, unless a test
    // asks.)
    static int sTunedOverride = 0;
    void SetTunedPartySize(int players) { sTunedOverride = players; }
    int TunedPartySize()
    {
        static int const configured = std::clamp(sConfigMgr->GetOption<int32>("Dnd.PartySize", 2), 1, 4);
        return sTunedOverride > 0 ? std::clamp(sTunedOverride, 1, 4) : configured;
    }

    int PartyHelp(int players) { return std::clamp(players, 1, 4) - 1; }
    float PartyHpScale(int players, bool legendary)
    {
        return 1.f + (legendary ? 0.75f : 0.5f) * float(std::clamp(players, 1, 8) - 1);
    }

    static void InsertCreature(Encounter* e, Creature* c);

    // a lone creature's legendary actions for the party it's tuned for (an endgame boss has its own floor, alone or not).
    // Topped up, never refilled, when it's worked out again mid-fight
    static void Reinforce(Encounter& e, int n)
    {
        std::vector<Combatant*> originals;
        for (Combatant& c : e.order)
            if (!c.player && !c.extra)
                originals.push_back(&c);
        if (originals.empty())
            return;
        EncounterDef const* def = GetEncounterDef(e.defId);
        uint8 minLegendary = def ? def->legendary : 0;
        bool lone = originals.size() == 1;
        e.legendaryHelp = lone;
        for (std::size_t i = 1; i < originals.size(); ++i)
            originals[i]->legendary = originals[i]->legendaryMax = 0;
        Combatant& boss = *originals.front();
        uint8 now = uint8(lone ? std::max<int>(PartyHelp(n), minLegendary) : minLegendary);
        if (now > boss.legendaryMax)
            boss.legendary = uint8(boss.legendary + (now - boss.legendaryMax));
        boss.legendaryMax = now;
        boss.legendary = std::min(boss.legendary, boss.legendaryMax);
    }

    // the creatures' hit points and legendary actions, for the tuned party (the damage they've taken carries over)
    static void ScaleForParty(Encounter& e)
    {
        int n = TunedPartySize();
        Reinforce(e, n);
        bool lonerHp = e.legendaryHelp;
        bool changed = false;
        for (Combatant& c : e.order)
        {
            if (c.player || c.gone || !c.alive || c.extra)
                continue;
            Creature* cr = e.GetUnit(c) ? e.GetUnit(c)->ToCreature() : nullptr;
            if (!cr || !cr->IsAlive())
                continue;
            if (!c.baseHp)
                c.baseHp = std::max<uint32>(1, cr->GetMaxHealth());
            uint32 want = std::max<uint32>(1, uint32(std::lround(float(c.baseHp) * PartyHpScale(n, lonerHp))));
            uint32 oldMax = cr->GetMaxHealth();
            if (want == oldMax)
                continue;
            uint32 taken = oldMax > cr->GetHealth() ? oldMax - cr->GetHealth() : 0;
            cr->SetMaxHealth(want);
            cr->SetHealth(want > taken ? want - taken : 1);
            changed = true;
        }
        e.party = n;
        if (changed)
            PushOrder(e);
    }

    // a player steps into a running fight: initiative rolled, slotted into the order (whoever's turn it is keeps it)
    static void JoinEncounter(Player* p, Encounter& e)
    {
        std::string rollText;
        Combatant c = MakeCombatant(p, rollText);
        ObjectGuid curGuid = e.Current() ? e.Current()->guid : ObjectGuid::Empty;
        e.order.push_back(c);
        SortOrder(e);
        for (std::size_t i = 0; i < e.order.size(); ++i)
            if (e.order[i].guid == curGuid)
                e.cur = int(i);
        sUnitEncounter[p->GetGUID()] = e.id;
        StopSneaking(p, "");
        SetRooted(p, true);
        p->AttackStop();
        if (EncounterDef const* def = GetEncounterDef(e.defId); def && def->dialogueNode)
            AddGrudge(p, def->id);           // a talker's group remembers everyone who fought it
        Broadcast(e, Acore::StringFormat("|cffffff00{} joins the fight!|r", p->GetName()));
        Broadcast(e, rollText);
        Send(p, "V^COMBAT: " + e.name);
        FightStartGear(p);
        ScaleForParty(e);
        PushOrder(e);
        PushEconomy(p);
    }

    static bool CanJoin(Player* p)
    {
        return p && p->IsAlive() && !p->IsGameMaster() && !InCombat(p) && !InGrace(p) && !IsDowned(p) && !p->IsInFlight();
    }

    void TryJoin(Player* p)
    {
        if (!CanJoin(p))
            return;
        uint32 now = Now();
        uint32& last = sLastJoinCheck[p->GetGUID()];
        if (now - last < 500)
            return;
        last = now;
        for (auto& [id, e] : sEncounters)
        {
            if (e.phase == Encounter::PHASE_DONE || e.mapId != p->GetMapId() || e.instanceId != p->GetInstanceId() ||
                sFled.count({ p->GetGUID(), e.id }))
                continue;
            EncounterDef const* def = GetEncounterDef(e.defId);
            float range = def ? def->joinRange : 30.f;
            bool nearby = false;
            for (Combatant const& c : e.order)
                if (!c.gone && c.alive)
                    if (Unit* u = e.GetUnit(c))
                        if (u->GetExactDist(p) <= range)
                            nearby = true;
            if (!nearby)
                continue;
            JoinEncounter(p, e);
            return;
        }
    }

    void TalkerAtRest(Creature* c)
    {
        if (!c || InCombat(c))
            return;
        EncounterDef const* def = GetEncounterDefBySpawn(c->GetSpawnId());
        if (!def || !def->dialogueNode)
            return;
        FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(c->GetFaction());
        if (ft && ft->IsHostileToPlayers())
            return;                          // a hostile group can be attacked already (it waits to talk all the same)
        c->SetFaction(TALKER_FACTION);
        c->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP); // (a right-click on a neutral without it would swing at them)
    }

    bool JoinFightOf(Player* p, Unit* target)
    {
        if (!CanJoin(p) || !target || target == p)
            return false;
        Encounter* e = EncounterOf(target->GetGUID());
        if (!e || e->phase == Encounter::PHASE_DONE || e->mapId != p->GetMapId() || e->instanceId != p->GetInstanceId())
            return false;
        sFled.erase({ p->GetGUID(), e->id });    // (back in on purpose)
        JoinEncounter(p, *e);
        return true;
    }

    bool JoinFightAt(Player* p, Position const& pos, float radius)
    {
        if (!CanJoin(p))
            return false;
        for (auto& [id, e] : sEncounters)
        {
            if (e.phase == Encounter::PHASE_DONE || e.mapId != p->GetMapId() || e.instanceId != p->GetInstanceId())
                continue;
            for (Combatant const& c : e.order)
                if (!c.gone && c.alive)
                    if (Unit* u = e.GetUnit(c))
                        if (u->GetExactDist2d(&pos) <= radius + u->GetCombatReach())
                        {
                            JoinEncounter(p, e);
                            return true;
                        }
        }
        return false;
    }

    // ------------------------------------------------------------------ end
    static void Cleanup(Encounter& e)
    {
        ClearVisuals(e);
        for (Combatant const& c : e.order)
            if (c.player)
                if (Player* bp = ObjectAccessor::FindPlayer(c.guid))
                {
                    ReleaseBossUnits(bp);        // the fight is over: no more boss units
                    ClearRangeRing(bp);          // ...nor range rings
                }
        for (auto itr = sFled.begin(); itr != sFled.end();)
            itr = itr->second == e.id ? sFled.erase(itr) : std::next(itr);
        for (Combatant& c : e.order)
        {
            sUnitEncounter.erase(c.guid);
            if (!c.player)
                if (Unit* u = e.GetUnit(c))
                {
                    if (c.extra)
                        if (Creature* xc = u->ToCreature())
                            xc->DespawnOrUnsummon(4000ms);   // brought in for the party: gone with the fight
                    if (c.baseHp && u->IsAlive() && u->GetMaxHealth() != c.baseHp)
                    {
                        u->SetMaxHealth(c.baseHp);       // the party's extra hit points go with the fight
                        u->SetHealth(std::min(u->GetHealth(), c.baseHp));
                    }
                    if (c.held)
                        u->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                    u->RemoveAurasDueToSpell(SPELL_HEX_AURA);
                    if (u->IsAlive())
                        u->SetStandState(UNIT_STAND_STATE_STAND);
                }
        }
        for (Combatant& c : e.order)
        {
            if (!c.player)
                continue;
            Player* p = ObjectAccessor::FindPlayer(c.guid);
            if (!p)
                continue;
            if (!sWiping)
                sSnapshots.erase(p->GetGUID());
            if (c.downed && p->IsAlive() && !sWiping)
            {
                sDownedOut.insert(p->GetGUID());
                Log(p, "|cffff8060You're down. Someone has to help you up - Help, a potion thrown your way, or healing.|r");
            }
            else
                SetRooted(p, false);
            p->CombatStop();
            for (uint32 aura : { uint32(SPELL_RAGE_AURA), uint32(SPELL_HIDDEN_AURA), uint32(SPELL_DODGE_AURA), uint32(SPELL_REBUKE_AURA) })
                p->RemoveAurasDueToSpell(aura);      // Rage, hiding and Patient Defense end with the fight
            sThreatSent.erase(p->GetGUID());
            Send(p, "Z");
            PushEconomy(p);
            PushSheet(p);
        }
        e.phase = Encounter::PHASE_DONE;
    }

    static void BrawlOver(Encounter& e, bool won)
    {
        Broadcast(e, won ? "|cff40ff40=== The brawl is over - you're the one still standing ===|r"
                         : "|cffff8060=== The brawl is over - you're out cold on the floorboards ===|r");
        Banner(e, won ? "LAST ONE STANDING" : "OUT COLD");
        Player* starter = ObjectAccessor::FindPlayer(e.brawlPlayer);
        Creature* host = nullptr;
        for (Combatant& c : e.order)
        {
            Unit* u = c.gone ? nullptr : e.GetUnit(c);
            if (!u || !u->IsAlive())
                continue;
            if (Creature* cr = u->ToCreature())
            {
                if (auto itr = sBrawlers.find(cr->GetGUID()); itr != sBrawlers.end())
                {
                    cr->SetMaxHealth(itr->second);
                    sBrawlers.erase(itr);
                }
                c.baseHp = 0;                        // (Cleanup would put the brawl's hit points back)
                cr->SetFullHealth();
                cr->RestoreFaction();
                cr->AttackStop();
                cr->GetMotionMaster()->Clear();
                Position home = cr->GetHomePosition();
                cr->GetMotionMaster()->MovePoint(0, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), FORCED_MOVEMENT_WALK);
                if (!host)
                    host = cr;
            }
            else
                u->SetStandState(UNIT_STAND_STATE_STAND);
        }
        Cleanup(e);
        if (starter)
            TavernBrawlOver(starter, host, won);
    }

    static void Victory(Encounter& e)
    {
        if (e.brawl)
            return BrawlOver(e, true);
        uint32 xp = 0;
        std::vector<Player*> players;
        for (Combatant& c : e.order)
        {
            if (!c.player)
            {
                if (Unit* u = e.GetUnit(c))
                    if (Creature* cr = u->ToCreature(); cr && !c.extra)
                        xp += StatsOrDefault(cr).xp;
                continue;
            }
            if (!c.gone)
                if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                    players.push_back(p);
        }
        Broadcast(e, Acore::StringFormat("|cff40ff40=== Victory: {} ===|r", e.name));
        Banner(e, "VICTORY");
        if (!players.empty())
        {
            std::string names;
            for (std::size_t i = 0; i < players.size(); ++i)
                names += (i == 0 ? "" : (i + 1 == players.size() ? " and " : ", ")) + std::string(players[i]->GetName());
            Announce(players.front(), Acore::StringFormat("{} defeated {} in {}.", names, e.name, ZoneName(players.front())));
        }
        // bespoke encounters pay their own XP to everyone online, once per character; ad-hoc fights split creature XP
        // among the players in them
        EncounterDef const* def = GetEncounterDef(e.defId);
        uint32 share = players.empty() ? 0 : xp / players.size();
        if (def)
        {
            // what the creatures carried and didn't use: to the players clearing it for the first time
            std::vector<Player*> takers;
            for (Player* p : players)
                if (!HasCleared(p, def->id))
                    takers.push_back(p);
            CreatureDrops(e, takers);
            ShareEncounterWon(players, *def);
        }
        else if (share)
            for (Player* p : players)
            {
                Log(p, Acore::StringFormat("|cff40ff40+{} XP|r", share));
                p->GiveXP(share, nullptr);
            }
        Cleanup(e);
    }

    void OnUnseenKill(Player* p, Creature* victim)
    {
        if (!p || !victim || victim->IsAlive())
            return;
        EncounterDef const* def = GetEncounterDefBySpawn(victim->GetSpawnId());
        if (!def)
        {
            if (uint32 xp = StatsOrDefault(victim).xp)
            {
                Log(p, Acore::StringFormat("|cff40ff40+{} XP|r", xp));
                p->GiveXP(xp, nullptr);
            }
            return;
        }
        if (HasCleared(p, def->id))
            return;
        int left = 0, total = 0;
        for (uint32 spawn : def->spawns)
        {
            auto range = victim->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
            for (auto itr = range.first; itr != range.second; ++itr)
            {
                ++total;
                if (itr->second->IsAlive())
                    ++left;
            }
        }
        if (left > 0)
        {
            Log(p, Acore::StringFormat("|cff9ad0ff{}: {} of {} down, unseen.|r", def->name, total - left, total));
            return;
        }
        Log(p, Acore::StringFormat("|cff40ff40=== Victory: {} - not one of them saw you ===|r", def->name));
        Send(p, "V^VICTORY");
        ShareEncounterWon({ p }, *def);
    }

    static void DownedLook(Player* p, bool down);

    // nobody in the fight is left standing. A wipe (escaped false, leftForDead false) sends them all to the graveyard,
    // as they were; escaped / leftForDead only reset the creatures - the downed stay down, the dead stay dead
    static void Defeat(Encounter& e, bool escaped = false, bool leftForDead = false)
    {
        if (e.brawl)
            return BrawlOver(e, false);
        std::vector<ObjectGuid> party;
        if (!escaped && !leftForDead)
            for (Combatant const& c : e.order)
                if (c.player && !c.gone)
                    party.push_back(c.guid);
        uint32 defId = e.defId;
        std::string foe = e.name;
        struct Wipe
        {
            std::vector<ObjectGuid> const& who;
            uint32 defId;
            std::string const& foe;
            ~Wipe()
            {
                sWiping = false;
                std::vector<Player*> back;
                for (ObjectGuid g : who)
                    if (Player* p = ObjectAccessor::FindPlayer(g))
                        back.push_back(p);
                if (back.empty())
                    return;
                std::string names;
                for (std::size_t i = 0; i < back.size(); ++i)
                    names += (i == 0 ? "" : (i + 1 == back.size() ? " and " : ", ")) + std::string(back[i]->GetName());
                Announce(back.front(), Acore::StringFormat("{} {} overwhelmed by {} in {} - and wake at the graveyard.", names,
                    back.size() == 1 ? "was" : "were", foe, ZoneName(back.front())));
                for (Player* p : back)
                    Reload(p, defId);
            }
        } wipe{ party, defId, foe };
        sWiping = !escaped && !leftForDead;
        if (leftForDead)
        {
            Broadcast(e, Acore::StringFormat("|cffff8060=== {}: they leave you for dead - the encounter resets ===|r", e.name));
            Banner(e, "DEFEAT");
        }
        else if (escaped)
        {
            Broadcast(e, Acore::StringFormat("|cffc8b890=== {}: the enemy loses your trail - the encounter resets ===|r", e.name));
            Banner(e, "ESCAPED");
        }
        else
        {
            Broadcast(e, Acore::StringFormat("|cffff4040=== Defeat: {} - the encounter resets ===|r", e.name));
            Banner(e, "DEFEAT");
        }
        for (Combatant& c : e.order)
        {
            if (c.player || c.extra)
                continue;
            Unit* u = e.GetUnit(c);
            Creature* cr = u ? u->ToCreature() : nullptr;
            if (!cr)
                continue;
            if (!cr->IsAlive())
            {
                // the fallen come back a little later instead of popping up in front of the corpse run
                sRespawns.push_back({ e.mapId, e.instanceId, cr->GetSpawnId(), Now() + 12000 });
                continue;
            }
            cr->SetStandState(UNIT_STAND_STATE_STAND);
            cr->SetFullHealth();
            cr->RestoreFaction();            // a talk that turned into a fight can be talked again
            TalkerAtRest(cr);
            cr->GetMotionMaster()->Clear();
            Position home = cr->GetHomePosition();
            cr->GetMotionMaster()->MovePoint(0, home.GetPositionX(), home.GetPositionY(), home.GetPositionZ(), FORCED_MOVEMENT_WALK);
        }
        Cleanup(e);
    }

    std::string FightDump()
    {
        std::string out;
        for (auto& [id, e] : sEncounters)
        {
            if (e.phase == Encounter::PHASE_DONE)
                continue;
            out += Acore::StringFormat("fight {} \"{}\": round {}, turn {}, phase {}\n", id, e.name, e.round, e.cur, int(e.phase));
            for (Combatant const& c : e.order)
            {
                Unit* u = e.GetUnit(c);
                if (!u)
                {
                    out += "  " + c.name + ": (not found)\n";
                    continue;
                }
                Player* nearest = nullptr;
                for (Combatant const& o : e.order)
                    if (o.player && !o.gone)
                        if (Unit* pu = e.GetUnit(o))
                            if (Player* pp = pu->ToPlayer(); pp && pp != u && (!nearest || pp->GetExactDist(u) < nearest->GetExactDist(u)))
                                nearest = pp;
                out += Acore::StringFormat("  {}{}: {}{}{} hp {}/{}, faction {}, flags 0x{:X}, at {:.1f} {:.1f} {:.1f}", c.name,
                    c.player ? " (player)" : "", c.alive ? "alive" : "dead", c.downed ? " downed" : "", c.gone ? " gone" : "",
                    u->GetHealth(), u->GetMaxHealth(), u->GetFaction(), uint32(u->GetUnitFlags()), u->GetPositionX(),
                    u->GetPositionY(), u->GetPositionZ());
                if (nearest)
                    out += Acore::StringFormat(" | {:.1f} m from {}, line of sight {}, hostile {}", nearest->GetExactDist(u),
                        nearest->GetName(), u->IsWithinLOSInMap(nearest) ? "yes" : "no", u->IsHostileTo(nearest) ? "yes" : "no");
                out += "\n";
            }
        }
        return out.empty() ? std::string("no fight running\n") : out;
    }

    void AbortEncounter(Unit* u)
    {
        if (Encounter* e = u ? EncounterOf(u->GetGUID()) : nullptr)
        {
            Broadcast(*e, Acore::StringFormat("|cffc8b890=== {}: fight called off ===|r", e->name));
            Cleanup(*e);
        }
    }

    static void InsertCreature(Encounter* e, Creature* c)
    {
        std::string rollText;
        Combatant cb = MakeCombatant(c, rollText);
        ObjectGuid curGuid = e->Current() ? e->Current()->guid : ObjectGuid::Empty;
        e->order.push_back(cb);
        SortOrder(*e);
        for (std::size_t i = 0; i < e->order.size(); ++i)
            if (e->order[i].guid == curGuid)
                e->cur = int(i);
        sUnitEncounter[c->GetGUID()] = e->id;
        c->GetMotionMaster()->Clear();
        c->StopMoving();
        Broadcast(*e, Acore::StringFormat("|cffffff00{} joins the fight!|r", c->GetName()));
        Broadcast(*e, rollText);
    }

    bool AddCreatureToFight(Creature* c, Unit* inFightWith)
    {
        Encounter* e = inFightWith ? EncounterOf(inFightWith->GetGUID()) : nullptr;
        if (!c || !e || !c->IsAlive() || InCombat(c))
            return false;
        InsertCreature(e, c);
        ScaleForParty(*e);
        PushOrder(*e);
        return true;
    }

    void PullGroupIntoFight(Creature* c, Player* p)
    {
        if (!c || !p || !c->IsAlive() || InCombat(c) || !InCombat(p))
            return;
        std::vector<Creature*> group;
        if (EncounterDef const* def = GetEncounterDefBySpawn(c->GetSpawnId()))
        {
            for (uint32 spawn : def->spawns)
            {
                auto range = c->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
                for (auto itr = range.first; itr != range.second; ++itr)
                    if (itr->second->IsAlive() && itr->second->IsInWorld() && !InCombat(itr->second) && !IsLeaving(itr->second->GetGUID()))
                        group.push_back(itr->second);
            }
            if (def->dialogueNode)
                AddGrudge(p, def->id);
        }
        if (std::find(group.begin(), group.end(), c) == group.end())
            group.push_back(c);
        for (Creature* g : group)
        {
            if (!g->IsHostileTo(p))
                g->SetFaction(14);
            AddCreatureToFight(g, p);
        }
    }

    // someone still on their feet nearby, outside this fight: then it isn't a wipe - the fallen lie where they fell,
    // rolling death saves, until that friend comes to help (or Brother Marrow, after). Like everything else about
    // parties here (who joins a fight, who hears a talk, who shares a find) it goes by who is near, not by the WoW group
    static float const RESCUE_RANGE = 120.f;
    static bool FriendStillStanding(Encounter& e)
    {
        for (Combatant const& c : e.order)
        {
            if (!c.player || c.gone)
                continue;
            Player* p = ObjectAccessor::FindPlayer(c.guid);
            if (!p || !p->IsInWorld())
                continue;
            for (MapReference const& ref : p->GetMap()->GetPlayers())
            {
                Player* m = ref.GetSource();
                if (!m || m == p || !m->IsInWorld() || !m->IsAlive() || IsDowned(m) || m->IsGameMaster() ||
                    m->GetExactDist(p) > RESCUE_RANGE)
                    continue;
                if (Combatant const* mc = e.Find(m->GetGUID()); mc && !mc->gone)
                    continue;                    // (in this fight: already counted, and down)
                return true;
            }
        }
        return false;
    }

    static bool CheckEnd(Encounter& e)
    {
        bool players = false, enemies = false;
        for (Combatant& c : e.order)
        {
            if (c.gone)
                continue;
            Unit* u = e.GetUnit(c);
            bool alive = c.alive && u && u->IsAlive();
            if (!alive || (c.player && c.downed))
                continue;
            (c.player ? players : enemies) = true;
        }
        if (!enemies)
        {
            Victory(e);
            return true;
        }
        if (!players)
        {
            if (FriendStillStanding(e))
            {
                for (Combatant const& c : e.order)
                    if (c.player && !c.gone && c.alive && c.downed && !c.stable)
                        return false;            // still dying: the fight goes on, and the friend can come
                Defeat(e, false, true);          // stable or dead, all of them: left for dead
                return true;
            }
            Defeat(e);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ turns
    // the weather's say at the top of each round: a blizzard's cold, a thunderstorm's lightning (DndWeather.cpp)
    static void WeatherRound(Encounter& e)
    {
        std::vector<Unit*> out;
        WeatherKind kind = WEATHER_CLEAR;
        for (Combatant& c : e.order)
            if (!c.gone && c.alive)
                if (Unit* u = e.GetUnit(c))
                    if (u->IsAlive())
                    {
                        WeatherKind k = WeatherAt(u);
                        if (k == WEATHER_BLIZZARD || k == WEATHER_THUNDERSTORM)
                        {
                            out.push_back(u);
                            kind = k;
                        }
                    }
        if (out.empty())
            return;
        if (kind == WEATHER_THUNDERSTORM)
        {
            D20Result chance = RollD20(0, 0);
            if (chance.natural < 17)
                return;
            Unit* u = out[D(int(out.size())) - 1];
            int bonus = SaveBonus(u, DEX);
            D20Result r = RollD20(bonus, 0);
            bool saved = r.total >= 13;
            std::string detail;
            int dmg = Dice{ 2, 8, 0 }.Roll(false, detail);
            if (saved)
                dmg /= 2;
            Broadcast(e, Acore::StringFormat("|cffd0e8ffLightning strikes {}!|r DEX save {} vs DC 13: {}. {} = |cffffffff{}|r Lightning{}",
                Name(u), r.text, saved ? "half" : "full", detail, dmg, saved ? " (halved)" : ""));
            DamageCtx d;
            d.victim = u;
            d.amount = dmg;
            d.type = DMG_LIGHTNING;
            ApplyDamage(d);
            return;
        }
        for (Unit* u : out)
        {
            bool adv = u->IsPlayer() && u->ToPlayer()->getRace() == RACE_DWARF;
            D20Result r = RollD20(SaveBonus(u, CON), adv ? 1 : 0);
            if (r.total >= 10)
                continue;
            std::string detail;
            int dmg = Dice{ 1, 4, 0 }.Roll(false, detail);
            Broadcast(e, Acore::StringFormat("|cffc0e0ffThe blizzard bites {}|r (CON save {} vs DC 10): {} cold", Name(u), r.text, dmg));
            DamageCtx d;
            d.victim = u;
            d.amount = dmg;
            d.type = DMG_COLD;
            ApplyDamage(d);
        }
    }

    // the boss's home fights back at the top of each round (5e lair actions): one of the encounter's lair actions, never
    // the same twice running, while its leader stands. One is off if anyone in the fight did what its `stop` names first
    static void LairRound(Encounter& e)
    {
        std::vector<LairAction> const* lair = GetLair(e.defId);
        if (!lair || lair->empty() || e.round < 1)
            return;
        for (Combatant& c : e.order)             // last round's grip lets go
            if (c.lairHeld)
            {
                c.lairHeld = false;
                if (c.restrained)
                {
                    c.restrained = false;
                    c.restrainedBy.Clear();
                    Broadcast(e, Acore::StringFormat("{} pulls free.", c.name));
                }
            }
        EncounterDef const* def = GetEncounterDef(e.defId);
        Creature* boss = nullptr;
        std::vector<Player*> party;
        for (Combatant& c : e.order)
        {
            Unit* u = c.gone || !c.alive ? nullptr : e.GetUnit(c);
            if (!u || !u->IsAlive())
                continue;
            if (c.player)
            {
                if (Player* p = u->ToPlayer())
                    party.push_back(p);
            }
            else if (Creature* cr = u->ToCreature(); cr && !c.extra && def && !def->spawns.empty() &&
                cr->GetSpawnId() == def->spawns.front())
                boss = cr;
        }
        if (!boss || party.empty())
            return;                              // the lair answers its master, and only while there's someone to hurt
        std::vector<int> pick;
        for (int i = 0; i < int(lair->size()); ++i)
            if (i != e.lairLast && !LairStopped((*lair)[i], party))
                pick.push_back(i);
        if (pick.empty() && e.lairLast >= 0 && e.lairLast < int(lair->size()) && !LairStopped((*lair)[e.lairLast], party))
            pick.push_back(e.lairLast);          // (only one left: it may come round again)
        if (pick.empty())
            return;
        int idx = pick[D(int(pick.size())) - 1];
        e.lairLast = idx;
        LairAction const& a = (*lair)[idx];
        std::vector<Player*> standing;
        for (Player* p : party)
            if (!IsDowned(p))
                standing.push_back(p);
        Player* at = standing.empty() ? party[D(int(party.size())) - 1] : standing[D(int(standing.size())) - 1];
        Broadcast(e, Acore::StringFormat("|cffd080ff[Lair] {}|r - {}", a.name, a.text));
        if (a.kind == "call")
        {
            int live = 0;
            for (ObjectGuid g : e.lairCalled)
                if (Combatant* c = e.Find(g); c && c->alive && !c->gone)
                    ++live;
            if (live >= a.most || !a.entry)
            {
                Broadcast(e, "|cff909090(Nobody else comes.)|r");
                return;
            }
            Position pos = boss->GetFirstCollisionPosition(4.f, float(M_PI) * (live % 2 ? 0.5f : -0.5f));
            pos.SetOrientation(boss->GetOrientation());
            TempSummon* t = boss->GetMap()->SummonCreature(a.entry, pos);
            if (!t)
                return;
            t->AIM_Initialize(MakeCombatantAI(t));
            t->SetReactState(REACT_PASSIVE);
            t->SetFaction(14);
            InsertCreature(&e, t);
            if (Combatant* added = e.Find(t->GetGUID()))
            {
                added->extra = true;
                e.lairCalled.push_back(t->GetGUID());
            }
            return;
        }
        Position spot = at->GetPosition();
        for (Player* p : party)
        {
            if (p->GetExactDist(&spot) > a.radius)
                continue;
            bool saved = LairSave(boss, p, a.save, a.dc, a.name);
            if (a.kind == "grip")
            {
                if (saved)
                    continue;
                if (Combatant* c = e.Find(p->GetGUID()))
                {
                    c->restrained = true;
                    c->restrainedBy = boss->GetGUID();
                    c->lairHeld = true;
                    Broadcast(e, Acore::StringFormat("{} is |cff60c060restrained|r until the next round.", c->name));
                }
                continue;
            }
            std::string detail;
            int dmg = a.dice.Roll(false, detail);
            if (saved)
                dmg /= 2;
            Broadcast(e, Acore::StringFormat("   {}: {} = |cffffffff{}|r {}{}", p->GetName(), detail, dmg, DmgTypeNames[a.type],
                saved ? " (halved)" : ""));
            DamageCtx d;
            d.attacker = boss;
            d.victim = p;
            d.amount = dmg;
            d.type = a.type;
            ApplyDamage(d);
            if (e.phase == Encounter::PHASE_DONE)
                return;
        }
        CheckEnd(e);
    }

    static void StartTurn(Encounter& e)
    {
        Combatant& c = e.order[e.cur];
        Unit* u = e.GetUnit(c);
        if (!u)
        {
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 0;
            return;
        }
        if (e.weatherRound != e.round)
        {
            e.weatherRound = e.round;
            WeatherRound(e);
            if (e.phase == Encounter::PHASE_DONE)
                return;                          // the weather settled the fight
            LairRound(e);
            if (e.phase == Encounter::PHASE_DONE)
                return;                          // the lair settled it
            if (!u->IsAlive())
            {
                e.phase = Encounter::PHASE_NEXT;
                e.timer = 1000;
                return;
            }
        }
        c.actionUsed = c.bonusUsed = c.reactionUsed = false;
        c.extraAttacks = 0;
        c.disengaged = false;
        c.dashed = false;
        c.gwmReady = false;
        c.meleeAttacked.clear();
        c.sneakUsed = false;
        c.attackedUnarmed = false;
        c.attackedLight = false;
        c.stepJump = false;
        if (c.player)
            if (Player* jp = u->ToPlayer(); jp && InJig(jp))
                JigRefill(jp);                   // the spell jig: every turn starts with everything back
        if (c.reckless || c.dodging)                 // Reckless Attack and Patient Defense last to your next turn
        {
            c.reckless = c.dodging = false;
            u->RemoveAurasDueToSpell(SPELL_DODGE_AURA);
        }
        c.bonusExtra = 0;
        if (Player* tp = u->ToPlayer())
            if (IsSubclass(tp, "Thief") && Level(tp) >= 3)
                c.bonusExtra = 1;                    // Fast Hands: an extra Bonus Action each turn
        if (c.surprised && u->IsPlayer() && HasFeat(u->ToPlayer(), "Alert"))
            c.surprised = false;             // Alert: can't be surprised
        for (Combatant& o : e.order)         // a Menacing Attack's fear lasts until its fighter's next turn
            if (o.frightened && o.frightenedBy == c.guid)
            {
                o.frightened = false;
                o.frightenedBy.Clear();
            }
        c.noReaction = false;
        c.legendary = c.legendaryMax;            // legendary actions come back at the start of its own turn
        if (c.poisoned && --c.poisoned == 0)     // venom lasts through the victim's next turn
            Broadcast(e, Acore::StringFormat("{} is no longer poisoned.", c.name));
        if (c.corroded && --c.corroded == 0)
            Broadcast(e, Acore::StringFormat("{}'s armour is no longer eaten away.", c.name));
        c.moved = 0.f;
        Creature* cr = u->ToCreature();
        c.moveMax = u->IsPlayer() ? Speed(u->ToPlayer()) : StatsOrDefault(cr).speed;
        c.moveMax = std::max(0.f, c.moveMax - WeatherSlow(u) / 10.f);     // snow and mud underfoot
        if (c.slowed)
        {
            c.moveMax = std::max(0.f, c.moveMax - 3.f);
            c.slowed = false;
        }
        SpellTurnReset(e, c);                       // Haste, Slow; the short spells that run out now
        if (c.restrained)
            c.moveMax = 0.f;                         // Entangle: going nowhere
        c.lastPos = u->GetPosition();
        e.turnStartedMs = Now();

        Broadcast(e, Acore::StringFormat("|cffffd100-- Round {}: {}'s turn --|r", e.round, c.name));
        PushOrder(e);

        if (c.player && !c.downed)
            if (Player* tp = u->ToPlayer())
            {
                std::string why;
                if (CanFlee(tp, why))
                    Log(tp, Acore::StringFormat("|cff9ad0ffNobody is within {:.0f} m of you: you can Flee (it takes you out of the fight).|r",
                        FLEE_RANGE));
            }

        if (c.player && c.downed)
        {
            bool rolls = !c.stable;
            if (rolls)
                RollDeathSave(u->ToPlayer());
            if (e.phase == Encounter::PHASE_DONE)
                return;                          // (their death ended it)
            e.phase = Encounter::PHASE_NEXT;
            e.timer = rolls ? 1800 : 400;
            return;
        }

        if (c.surprised)
        {
            c.surprised = false;
            Broadcast(e, Acore::StringFormat("{} is |cffffd100Surprised|r and loses the turn!", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }

        bool firstTurn = !c.hasActed;
        c.hasActed = true;                           // (Assassinate: advantage only before a foe's first turn)
        (void)firstTurn;
        if (c.entranced)
        {
            Broadcast(e, Acore::StringFormat("{} is |cffd080ffentranced|r by the shifting pattern and does nothing.", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }
        if (!c.player)
        {
            std::vector<Area> here;
            for (Area const& a : e.areas)
            {
                Position at = a.pos;
                if (a.spellId == SPELL_SPIRIT_GUARDIANS)
                    if (Player* o = ObjectAccessor::FindPlayer(a.owner))
                        at = o->GetPosition();       // the spirits go where the cleric goes
                if (!a.owner.IsEmpty() && a.expiresMs > Now() && u->GetExactDist2d(&at) <= a.radius + AreaEdge(u))
                    here.push_back(a);
            }
            for (Area const& a : here)
                if (Player* owner = ObjectAccessor::FindPlayer(a.owner))
                {
                    bool lost = AreaTurnStart(owner, u, a.spellId);
                    if (!u->IsAlive() || e.phase == Encounter::PHASE_DONE)
                        return;                  // (its death moved the fight on)
                    if (lost)
                    {
                        e.phase = Encounter::PHASE_NEXT;
                        e.timer = 1500;
                        return;
                    }
                }
        }
        if (!c.burningBy.IsEmpty())
        {
            BurnTurnStart(u);
            if (!u->IsAlive() || e.phase == Encounter::PHASE_DONE)
                return;                          // (its death moved the fight on)
        }
        if (SpellTurnStart(e, c, u))
        {
            if (!u->IsAlive() || e.phase == Encounter::PHASE_DONE)
                return;                          // (its death moved the fight on)
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }
        if (c.turned)
        {
            Broadcast(e, Acore::StringFormat("{} is |cffffe080turned|r: it cowers from the holy power and does nothing.", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1200;
            return;
        }
        if (c.asleep)
        {
            Broadcast(e, Acore::StringFormat("{} is |cff9ad0ffasleep|r.", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1200;
            return;
        }
        if (c.afraid && !c.player && c.killerBy.IsEmpty())   // (Phantasmal Killer has its own save: SpellTurnStart)
        {
            int dc = ConditionDC(c.afraidBy);
            D20Result r = RollD20(AbilityMod(u, WIS), 0);
            if (r.total >= dc)
            {
                c.afraid = false;
                c.afraidBy.Clear();
                Broadcast(e, Acore::StringFormat("{} masters its fear: WIS {} vs DC {}.", c.name, r.text, dc));
            }
        }
        if (c.player && !c.heroismBy.IsEmpty())
            if (Player* hero = ObjectAccessor::FindPlayer(c.heroismBy))
                if (Combatant* hc = e.Find(hero->GetGUID()); hc && hc->concentration == SPELL_HEROISM)
                    if (Player* tp = u->ToPlayer())
                    {
                        int t = std::max(1, CastingMod(hero));
                        Sheet& ts = GetSheet(tp);
                        if (ts.tempHp < t)
                        {
                            ts.tempHp = t;
                            PushSheet(tp);
                            Broadcast(e, Acore::StringFormat("{} is filled with courage: {} temporary hit points.", c.name, t));
                        }
                    }
        if (c.restrained && !c.player)
        {
            bool inReach = false;
            for (Combatant const& o : e.order)
                if (o.player && !o.gone && o.alive && !o.downed)
                    if (Unit* pu = e.GetUnit(o); pu && u->GetExactDist(pu) <= MeleeRange(u, pu))
                        inReach = true;
            if (!inReach)
            {
                // nobody to hit from where it's stuck: it strains against the vines (a STR check against the spell)
                int dc = ConditionDC(c.restrainedBy);
                D20Result r = RollD20(AbilityMod(u, STR), 0);
                bool freed = r.total >= dc;
                Broadcast(e, Acore::StringFormat("{} strains against the vines: STR {} vs DC {} - {}", c.name, r.text, dc,
                    freed ? "|cff40ff40it tears free|r" : "|cffff4040still held fast|r"));
                if (freed)
                {
                    c.restrained = false;
                    c.restrainedBy.Clear();
                }
                e.phase = Encounter::PHASE_NEXT;
                e.timer = 1500;
                return;
            }
        }
        if (c.stunned)
        {
            --c.stunned;                             // Stunning Strike: the turn is lost
            Broadcast(e, Acore::StringFormat("{} is |cffffff40Stunned|r and can't act.", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }

        if (c.held)
        {
            // Hold Person: paralysed - the turn is lost; then a WIS save to break free
            bool freed = false;
            {
                D20Result r = RollD20(AbilityMod(u, WIS), 0);
                int dc = ConditionDC(c.heldBy);      // whoever cast it: a player's DC, or a creature's
                freed = r.total >= dc;
                Broadcast(e, Acore::StringFormat("{} is |cffff8000Held|r and can't act. WIS save {} vs DC {}: {}", c.name, r.text, dc,
                    freed ? "|cff40ff40breaks free|r" : "|cffff4040still held|r"));
            }
            if (freed)
            {
                c.held = false;
                c.heldBy.Clear();
                u->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
            }
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }

        if (c.prone)
        {
            c.prone = false;
            u->SetStandState(UNIT_STAND_STATE_STAND);
            Broadcast(e, Acore::StringFormat("{} is |cffff8000Prone|r and spends the turn getting up.", c.name));
            e.phase = Encounter::PHASE_NEXT;
            e.timer = 1500;
            return;
        }

        if (Player* p = u->ToPlayer())
        {
            SetRooted(p, false);
            e.phase = Encounter::PHASE_WAIT_PLAYER;      // before the push: IsTurnOf() checks the phase
            Send(p, "V^YOUR TURN");
            PushEconomy(p);
            static int32 timeout = sConfigMgr->GetOption<int32>("Dnd.PlayerTurnSeconds", 0);
            e.timer = timeout > 0 ? timeout * 1000 : 0;
            return;
        }
        e.phase = Encounter::PHASE_AI_START;
        e.timer = 700;
    }

    // nobody on either side within this of the other: the fight is over (a player who ran, or one who was revived
    // far away) - it resets like a defeat, without the death
    static float const LEASH_RANGE = 60.f;
    static bool Leashed(Encounter& e)
    {
        bool anyPlayer = false;
        for (Combatant& pc : e.order)
        {
            Unit* pu = pc.player && pc.alive && !pc.gone ? e.GetUnit(pc) : nullptr;
            if (!pu || !pu->IsAlive())
                continue;
            anyPlayer = true;
            for (Combatant& ec : e.order)
            {
                Unit* eu = !ec.player && ec.alive && !ec.gone ? e.GetUnit(ec) : nullptr;
                if (eu && eu->IsAlive() && eu->GetMapId() == pu->GetMapId() && eu->GetExactDist(pu) <= LEASH_RANGE)
                    return false;
            }
        }
        return anyPlayer;
    }

    static Unit* PickTarget(Encounter& e, Creature* cr);

    // at the end of another combatant's turn, a creature with a legendary action left may take it: a swing at someone
    // in reach, or a shot (true if one was taken)
    static bool LegendaryAfterTurn(Encounter& e)
    {
        if (e.cur < 0 || e.cur >= int(e.order.size()))
            return false;
        ObjectGuid ended = e.order[e.cur].guid;
        for (Combatant& c : e.order)
        {
            if (c.player || c.gone || !c.alive || !c.legendary || c.guid == ended || c.held || c.stunned)
                continue;
            Unit* u = e.GetUnit(c);
            Creature* cr = u ? u->ToCreature() : nullptr;
            if (!cr || !cr->IsAlive())
                continue;
            CreatureStats const& s = StatsOrDefault(cr);
            Unit* target = nullptr;
            bool ranged = false;
            for (Combatant const& o : e.order)               // someone in reach first
                if (o.player && !o.gone && o.alive && !o.downed)
                    if (Unit* pu = e.GetUnit(o); pu && pu->IsAlive() && s.melee.valid() && cr->GetExactDist(pu) <= MeleeRange(cr, pu))
                        if (!target || pu->GetHealthPct() < target->GetHealthPct())
                            target = pu;
            if (!target && s.ranged.valid())
                if (Unit* t = PickTarget(e, cr); t && cr->GetExactDist(t) <= s.ranged.range && cr->IsWithinLOSInMap(t))
                {
                    target = t;
                    ranged = true;
                }
            if (!target)
                continue;
            --c.legendary;
            Broadcast(e, Acore::StringFormat("|cffff9040{} takes a Legendary Action|r ({} left this round).", c.name, c.legendary));
            CreatureLegendaryAttack(cr, target, ranged);
            PushOrder(e);
            return true;
        }
        return false;
    }

    static void Advance(Encounter& e)
    {
        if (CheckEnd(e))
            return;
        if (Leashed(e))
        {
            Defeat(e, true);
            return;
        }
        uint32 key = uint32(e.round) * 256u + uint32(e.cur + 1);
        if (e.cur >= 0 && e.legendaryKey != key)
        {
            e.legendaryKey = key;
            if (LegendaryAfterTurn(e))
            {
                if (CheckEnd(e))
                    return;
                e.timer = 1500;                  // the swing plays out before the next turn starts
                return;
            }
        }
        int n = int(e.order.size());
        for (int step = 1; step <= n; ++step)
        {
            int raw = e.cur + step;
            bool wrapped = e.cur < 0 || raw >= n;
            int idx = raw % n;
            Combatant& c = e.order[idx];
            Unit* u = e.GetUnit(c);
            if (c.gone || !c.alive || !u || !u->IsAlive())
                continue;
            if (wrapped)
                ++e.round;
            e.cur = idx;
            StartTurn(e);
            return;
        }
        CheckEnd(e);
    }

    void EndTurn(Unit* u)
    {
        Encounter* e = u ? EncounterOf(u->GetGUID()) : nullptr;
        if (!e)
            return;
        Combatant* c = e->Current();
        if (!c || c->guid != u->GetGUID())
            return;
        if (Player* p = u->ToPlayer())
        {
            SetRooted(p, true);
            Broadcast(*e, Acore::StringFormat("{} ends their turn.", c->name));
        }
        e->phase = Encounter::PHASE_NEXT;
        e->timer = 400;
        if (Player* p = u->ToPlayer())
            PushEconomy(p);
    }

    // a brawler at 1 HP: out cold, out of the fight (turns, targets and the end check all pass over the fallen)
    void BrawlKnockOut(Unit* u)
    {
        Encounter* e = EncounterOf(u->GetGUID());
        if (!e || !e->brawl)
            return;
        Combatant* c = e->Find(u->GetGUID());
        if (!c || !c->alive)
            return;
        if (c->player)
            EndConcentration(u, " - they're out cold");
        c->alive = false;
        c->prone = false;
        u->AttackStop();
        u->SetStandState(UNIT_STAND_STATE_DEAD);
        Broadcast(*e, Acore::StringFormat("|cffffb040{} goes down in a heap - out cold!|r", c->name));
        bool wasCurrent = e->Current() == c;
        if (CheckEnd(*e))
            return;
        PushOrder(*e);
        if (wasCurrent)
        {
            e->phase = Encounter::PHASE_NEXT;
            e->timer = 400;
        }
    }

    // a villager squares up to a player: the villager gets a brawler's hit points and fists for the fight
    bool StartBrawl(Player* p, Creature* npc)
    {
        if (!p || !npc || !p->IsAlive() || !npc->IsAlive() || InCombat(p) || InCombat(npc) || IsDowned(p))
            return false;
        uint32 own = npc->GetMaxHealth();
        sBrawlers[npc->GetGUID()] = own;
        npc->SetMaxHealth(BRAWLER_HP);
        npc->SetHealth(BRAWLER_HP);
        StartEncounter(npc, p);
        Encounter* e = EncounterOf(npc->GetGUID());
        if (!e)
        {
            sBrawlers.erase(npc->GetGUID());
            npc->SetMaxHealth(own);
            npc->SetFullHealth();
            return false;
        }
        e->brawl = true;
        e->brawlPlayer = p->GetGUID();
        e->name = Acore::StringFormat("a brawl with {}", npc->GetName());
        return true;
    }

    // before the first turn: this player first in the order (the spell jig's fights)
    void GoFirst(Player* p)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        Combatant* me = e ? e->Find(p->GetGUID()) : nullptr;
        if (!me || e->round > 0 || e->cur >= 0)
            return;
        int top = 0;
        for (Combatant const& o : e->order)
            top = std::max(top, o.initiative);
        me->initiative = top + 1;
        SortOrder(*e);
        PushOrder(*e);
    }

    // the fight simply ends (the spell jig starting a fresh one): the creatures reset, nobody goes anywhere
    void AbandonFight(Player* p)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        if (!e)
            return;
        sSnapshots.erase(p->GetGUID());
        Defeat(*e, true);
    }

    void OnUnitDied(Unit* u)
    {
        Encounter* e = EncounterOf(u->GetGUID());
        if (!e)
            return;
        Combatant* c = e->Find(u->GetGUID());
        if (!c || !c->alive)
            return;
        EndConcentration(u, " - they fell");    // (a casting creature's hold on someone ends with it)
        c->alive = false;
        c->prone = false;
        if (c->held)
        {
            c->held = false;
            u->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
        }
        Broadcast(*e, Acore::StringFormat("|cffff4040{} falls!|r", c->name));
        bool wasCurrent = e->Current() == c;
        if (CheckEnd(*e))
            return;
        PushOrder(*e);
        if (wasCurrent)
        {
            e->phase = Encounter::PHASE_NEXT;
            e->timer = 1000;
        }
    }

    // ------------------------------------------------------------------ fleeing
    bool CanFlee(Player* p, std::string& why)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        if (!e)
        {
            why = "You aren't in a fight.";
            return false;
        }
        for (Combatant const& c : e->order)
            if (!c.player && !c.gone && c.alive)
                if (Unit* u = e->GetUnit(c); u && u->IsAlive() && u->GetExactDist(p) <= FLEE_RANGE)
                {
                    why = Acore::StringFormat("Too close to flee: {} is {:.0f} m away (you need {:.0f} m from every enemy).", c.name,
                        u->GetExactDist(p), FLEE_RANGE);
                    return false;
                }
        return true;
    }

    void Flee(Player* p)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        if (!e)
            return;
        bool others = false;                     // anyone else still in it (standing or down)
        for (Combatant const& c : e->order)
            if (c.player && !c.gone && c.alive && c.guid != p->GetGUID())
                others = true;
        Broadcast(*e, Acore::StringFormat("|cffc8b890{} flees the fight!|r", p->GetName()));
        sSnapshots.erase(p->GetGUID());          // (an escape, not a wipe: what was spent stays spent)
        GrantGrace(p, 6000);
        if (!others)
        {
            Defeat(*e, true);                    // the last one out: the creatures lose the trail and go home
            return;
        }
        sFled.insert({ p->GetGUID(), e->id });
        OnPlayerLeft(p);
        Send(p, "Z");
        Send(p, "V^ESCAPED");
        PushEconomy(p);
    }

    // ------------------------------------------------------------------ downed
    static void DownedLook(Player* p, bool down)
    {
        if (down)
        {
            p->SetUnitFlag(UNIT_FLAG_PREVENT_EMOTES_FROM_CHAT_TEXT);
            p->SetUnitFlag2(UNIT_FLAG2_FEIGN_DEATH);
            p->SetDynamicFlag(UNIT_DYNFLAG_DEAD);
        }
        else
        {
            p->RemoveUnitFlag(UNIT_FLAG_PREVENT_EMOTES_FROM_CHAT_TEXT);
            p->RemoveUnitFlag2(UNIT_FLAG2_FEIGN_DEATH);
            p->RemoveDynamicFlag(UNIT_DYNFLAG_DEAD);
            p->SetStandState(UNIT_STAND_STATE_STAND);
        }
    }

    bool IsDowned(Unit* u)
    {
        if (!u || !u->IsPlayer() || !u->IsAlive())
            return false;
        if (Combatant* c = CombatantOf(u->GetGUID()))
            return c->downed;
        return sDownedOut.count(u->GetGUID()) != 0;
    }

    void KnockDown(Player* p)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        Combatant* c = e ? e->Find(p->GetGUID()) : nullptr;
        if (!c || c->downed || !p->IsAlive())
            return;
        EndConcentration(p, " - they went down");
        c->downed = true;
        c->stable = false;
        c->saveOk = c->saveFail = 0;
        c->raging = c->hidden = c->dodging = c->reckless = false;
        for (uint32 aura : { uint32(SPELL_RAGE_AURA), uint32(SPELL_HIDDEN_AURA), uint32(SPELL_DODGE_AURA) })
            p->RemoveAurasDueToSpell(aura);
        p->SetHealth(1);
        DownedLook(p, true);
        SetRooted(p, true);
        Broadcast(*e, Acore::StringFormat("|cffff4040{} is knocked down!|r Death saves on their turns - Help them up (an Action, "
            "next to them), heal them, or throw them a potion.", c->name));
        Send(p, "V^KNOCKED DOWN");
        PushSheet(p);
        bool wasCurrent = e->Current() == c;
        if (CheckEnd(*e))
            return;                              // nobody left standing
        PushOrder(*e);
        if (wasCurrent)
        {
            e->phase = Encounter::PHASE_NEXT;
            e->timer = 1000;
        }
    }

    void GetUp(Player* p, int hp, std::string const& how)
    {
        if (!p || !IsDowned(p))
            return;
        Encounter* e = EncounterOf(p->GetGUID());
        if (Combatant* c = CombatantOf(p->GetGUID()))
        {
            c->downed = c->stable = false;
            c->saveOk = c->saveFail = 0;
        }
        sDownedOut.erase(p->GetGUID());
        p->SetHealth(std::clamp<uint32>(uint32(std::max(1, hp)), 1, p->GetMaxHealth()));
        DownedLook(p, false);
        std::string line = Acore::StringFormat("|cff40ff40{} is back on their feet ({}) with {} HP.|r", p->GetName(), how, p->GetHealth());
        if (e)
        {
            Broadcast(*e, line);
            PushOrder(*e);
        }
        else
        {
            SetRooted(p, false);
            Log(p, line);
        }
        PushSheet(p);
    }

    bool SomeoneElseOnline(Player* p)
    {
        for (auto const& [guid, o] : ObjectAccessor::GetPlayers())
            if (o && o != p && o->IsInWorld() && o->IsAlive() && !o->IsGameMaster())
                return true;
        return false;
    }

    static void Perish(Player* p)
    {
        if (Combatant* c = CombatantOf(p->GetGUID()))
            c->downed = c->stable = false;
        sDownedOut.erase(p->GetGUID());
        DownedLook(p, false);
        bool wait = SomeoneElseOnline(p);
        if (wait)
            p->AddAura(SPELL_FALLEN, p);         // (kept through death: no releasing until a friend sees to it)
        KillOutright(p);                         // (OnUnitDeath: the announcement; OnUnitDied: the order, the fight's end)
        if (wait)
            Log(p, "|cffff8060You have fallen. Your spirit stays with your body - a friend can raise you at Brother Marrow.|r");
    }

    // a fallen player whose friends have all gone (logged off): they may release and walk to Brother Marrow after all
    void UpdateFallen(Player* p, uint32 diff)
    {
        static std::unordered_map<ObjectGuid, uint32> since;
        if (p->IsAlive() || !p->HasAura(SPELL_FALLEN))
        {
            since.erase(p->GetGUID());
            return;
        }
        uint32& t = since[p->GetGUID()];
        t += diff;
        if (t < 5000)
            return;
        t = 0;
        if (!SomeoneElseOnline(p))
        {
            p->RemoveAurasDueToSpell(SPELL_FALLEN);
            Log(p, "|cffc8b890There's nobody left to raise you. You can release your spirit and go to Brother Marrow yourself.|r");
        }
    }

    void DownedHit(Player* p, bool close)
    {
        Encounter* e = EncounterOf(p->GetGUID());
        Combatant* c = e ? e->Find(p->GetGUID()) : nullptr;
        if (!c || !c->downed)
            return;
        int fails = close ? 2 : 1;
        c->stable = false;
        c->saveFail = uint8(c->saveFail + fails);
        Broadcast(*e, Acore::StringFormat("|cffff4040{} is hit while down: {} failed death save{}{}.|r", c->name, fails,
            fails == 1 ? "" : "s", close ? " (from close by)" : ""));
        if (c->saveFail >= 3)
            Perish(p);
        else
            PushOrder(*e);
    }

    void RollDeathSave(Player* p)
    {
        Encounter* e = p ? EncounterOf(p->GetGUID()) : nullptr;
        Combatant* c = e ? e->Find(p->GetGUID()) : nullptr;
        if (!c || !c->downed || c->stable)
            return;
        D20Result r = RollD20(0, 0);
        Send(p, Acore::StringFormat("TD^Death Save^{}^0^10^{}", r.natural, r.natural == 20 || (r.natural != 1 && r.total >= 10) ? 1 : 0));
        if (r.natural == 20)
        {
            Broadcast(*e, Acore::StringFormat("{} death save: {} - |cffffd100a natural 20!|r", c->name, r.text));
            GetUp(p, 1, "a natural 20");
            return;
        }
        if (r.natural == 1)
            c->saveFail = uint8(c->saveFail + 2);
        else if (r.total >= 10)
            ++c->saveOk;
        else
            ++c->saveFail;
        Broadcast(*e, Acore::StringFormat("{} death save: {} - {} (successes {}, failures {})", c->name, r.text,
            r.natural == 1 ? "|cffff4040a natural 1: two failures|r" : (r.total >= 10 ? "|cff40ff40success|r" : "|cffff4040failure|r"),
            std::min<int>(c->saveOk, 3), std::min<int>(c->saveFail, 3)));
        if (c->saveFail >= 3)
        {
            Perish(p);
            return;
        }
        if (c->saveOk >= 3)
        {
            c->stable = true;
            Broadcast(*e, Acore::StringFormat("{} is |cffffd100stable|r - still down, but no longer dying.", c->name));
            if (CheckEnd(*e))
                return;                          // (the last one dying, with a friend away: left for dead)
        }
        PushOrder(*e);
    }

    void UpdateDowned(Player* p)
    {
        if (!IsDowned(p))
        {
            if (p->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH) && p->IsAlive())
                DownedLook(p, false);            // (left over from a logout while down)
            return;
        }
        if (!p->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH))
            DownedLook(p, true);
        if (!p->HasUnitState(UNIT_STATE_ROOT))
            SetRooted(p, true);
    }

    void ForgetDowned(ObjectGuid g) { sDownedOut.erase(g); }

    void OnPlayerLeft(Player* p)
    {
        Encounter* e = EncounterOf(p->GetGUID());
        sLastJoinCheck.erase(p->GetGUID());
        if (!e)
            return;
        EndConcentration(p, " - they left the fight");
        ReleaseBossUnits(p);
        Combatant* c = e->Find(p->GetGUID());
        c->gone = true;
        sUnitEncounter.erase(p->GetGUID());
        SetRooted(p, false);
        Broadcast(*e, Acore::StringFormat("{} has left the fight.", c->name));
        bool wasCurrent = e->Current() == c;
        if (CheckEnd(*e))
            return;
        ScaleForParty(*e);
        if (wasCurrent)
        {
            e->phase = Encounter::PHASE_NEXT;
            e->timer = 300;
        }
    }

    // ------------------------------------------------------------------ movement
    static float AreaMultiplier(Encounter& e, Position const& pos)
    {
        uint32 now = Now();
        for (Area const& a : e.areas)
            if ((a.spellId == SPELL_GREASE || a.spellId == SPELL_HUNGER_OF_HADAR || a.spellId == SPELL_SLEET_STORM) &&
                a.expiresMs > now && a.pos.GetExactDist2d(&pos) <= a.radius)
                return 2.f;                  // difficult terrain
        return 1.f;
    }

    void OnPlayerMoved(Player* p, Position const& pos)
    {
        Encounter* e = EncounterOf(p->GetGUID());
        if (!e)
        {
            TryJoin(p);
            return;
        }
        Combatant* c = e->Find(p->GetGUID());
        if (!c || c->gone)
            return;
        uint32 now = Now();
        if (!IsTurnOf(p))
        {
            // held between turns (and while down) by a root the client is asked to keep. One that walks anyway - a root
            // it missed - is put back where it stood and held again; a shove (ignoreMoveUntil, lastPos at the landing
            // spot) is the only off-turn move
            if (now < c->ignoreMoveUntil || !p->IsAlive())
                return;
            if (c->lastPos.GetExactDist2d(&pos) > 1.f)
            {
                SetRooted(p, false);
                SetRooted(p, true);              // (sent again: the client's root had come off)
                p->NearTeleportTo(c->lastPos.GetPositionX(), c->lastPos.GetPositionY(), c->lastPos.GetPositionZ(),
                    p->GetOrientation());
                Error(p, IsDowned(p) ? "You're down - you can't move." : "It isn't your turn.");
            }
            return;
        }
        // mid-jump: the flight is free, but lastPos stays on the landing spot, so anything run after it counts
        if (now < c->ignoreMoveUntil)
            return;
        float dist = c->lastPos.GetExactDist2d(&pos);
        if (dist < 0.05f)
            return;
        // already out of movement (a Jump has to lift the root to fly): hold them again
        if (c->moved >= c->moveMax)
        {
            c->lastPos = pos;
            if (!p->HasUnitState(UNIT_STATE_ROOT))
            {
                SetRooted(p, true);
                Error(p, "Out of movement. Dash for more, or End Turn.");
            }
            return;
        }

        // opportunity attacks against the mover
        if (!c->disengaged)
        {
            for (Combatant& o : e->order)
            {
                if (o.player || !o.alive || o.gone || o.reactionUsed || o.noReaction || o.prone)
                    continue;
                // Mobile: whoever you attacked in melee this turn doesn't get to swing as you leave
                if (std::find(c->meleeAttacked.begin(), c->meleeAttacked.end(), o.guid) != c->meleeAttacked.end() &&
                    HasFeat(p, "Mobile"))
                    continue;
                Unit* ou = e->GetUnit(o);
                Creature* cr = ou ? ou->ToCreature() : nullptr;
                if (!cr || !cr->IsAlive() || !StatsOrDefault(cr).melee.valid())
                    continue;
                float range = MeleeRange(cr, p);
                if (cr->GetExactDist(&c->lastPos) <= range && cr->GetExactDist(&pos) > range)
                {
                    o.reactionUsed = true;
                    Broadcast(*e, Acore::StringFormat("|cffff8000{} makes an Opportunity Attack against {}!|r", o.name, c->name));
                    CreatureAttack(cr, p, false, true);
                    if (!p->IsAlive())
                        return;
                }
            }
        }

        bool hadMovement = c->moved < c->moveMax;
        // Mobile: after a Dash, difficult terrain costs nothing extra
        bool stride = IsSubclass(p, "Land") && Level(p) >= 6;     // Land's Stride
        c->moved += dist * ((c->dashed && HasFeat(p, "Mobile")) || stride ? 1.f : AreaMultiplier(*e, pos));
        c->lastPos = pos;
        if (c->moved >= c->moveMax && hadMovement)
        {
            c->moved = c->moveMax;
            SetRooted(p, true);
            Error(p, "Out of movement. Dash for more, or End Turn.");
            PushEconomy(p);
        }
        else if (now - e->lastEconomyPush > 200)
        {
            e->lastEconomyPush = now;
            PushEconomy(p);
        }
    }

    // ------------------------------------------------------------------ creature turn
    static bool SpotFree(Encounter& e, Unit* self, Position const& p)
    {
        for (Combatant const& c : e.order)
        {
            if (c.gone || !c.alive || c.guid == self->GetGUID())
                continue;
            if (Unit* u = e.GetUnit(c))
                if (u->GetExactDist2d(&p) < 1.6f)
                    return false;
        }
        return true;
    }

    // navmesh path from the creature to a point; false when there is no way there
    static bool PathTo(Creature* cr, Position const& to, Movement::PointsArray& out, float& len)
    {
        PathGenerator path(cr);
        if (!path.CalculatePath(to.GetPositionX(), to.GetPositionY(), to.GetPositionZ(), false))
            return false;
        if (path.GetPathType() & PATHFIND_NOPATH)
            return false;
        out = path.GetPath();
        len = 0.f;
        for (std::size_t i = 1; i < out.size(); ++i)
            len += (out[i] - out[i - 1]).length();
        return out.size() >= 2;
    }

    // the point `budget` metres along a path
    static Position AlongPath(Movement::PointsArray const& pts, float budget)
    {
        for (std::size_t i = 1; i < pts.size(); ++i)
        {
            float seg = (pts[i] - pts[i - 1]).length();
            if (seg >= budget && seg > 0.f)
            {
                G3D::Vector3 p = pts[i - 1] + (pts[i] - pts[i - 1]) * (budget / seg);
                return Position(p.x, p.y, p.z);
            }
            budget -= seg;
        }
        G3D::Vector3 const& last = pts.back();
        return Position(last.x, last.y, last.z);
    }

    static Unit* PickTarget(Encounter& e, Creature* cr)
    {
        std::vector<std::pair<float, Unit*>> ranked;
        // the downed are left alone while someone still stands; with nobody standing, they're fair game (each hit a
        // failed death save, two from close by)
        bool standing = false;
        for (Combatant const& c : e.order)
            if (c.player && !c.gone && c.alive && !c.downed)
                standing = true;
        for (Combatant const& c : e.order)
        {
            if (!c.player || c.gone || !c.alive || (c.downed && standing) || !c.banishedBy.IsEmpty())
                continue;                        // (the banished aren't here to be hit)
            Unit* u = e.GetUnit(c);
            if (!u || !u->IsAlive())
                continue;
            // nearest, with a pull towards wounded and low-AC targets
            ranked.emplace_back(cr->GetExactDist(u) + u->GetHealthPct() * 0.05f + ArmorClass(u) * 0.3f, u);
        }
        std::sort(ranked.begin(), ranked.end(), [](auto const& x, auto const& y) { return x.first < y.first; });
        for (auto const& [score, u] : ranked)
            if (!SanctuaryHolds(cr, u))          // Sanctuary: a WIS save to attack them, or it looks elsewhere
                return u;
        return nullptr;
    }

    static void RunCreatureTurn(Encounter& e, Combatant& c, Creature* cr)
    {
        CreatureStats const& s = StatsOrDefault(cr);
        Unit* target = PickTarget(e, cr);
        if (!target)
        {
            e.phase = Encounter::PHASE_AI_END;
            e.timer = 0;
            return;
        }
        e.aiTarget = target->GetGUID();
        cr->SetFacingToObject(target);
        e.aiCast = -1;
        if (CreaturePlanCast(e, cr, true))
        {
            e.aiRanged = false;                  // a spell that suits, from where it stands
            e.phase = Encounter::PHASE_AI_ACT;
            e.timer = 200;
            return;
        }
        e.aiTarget = target->GetGUID();
        float dist = cr->GetExactDist(target);
        float reach = MeleeRange(cr, target);
        bool inReach = dist <= reach;

        if (s.ranged.valid() && (!s.melee.valid() || !inReach) && dist <= s.ranged.range && cr->IsWithinLOSInMap(target))
        {
            e.aiRanged = true;
            e.phase = Encounter::PHASE_AI_ACT;
            e.timer = 200;
            return;
        }
        e.aiRanged = false;
        if (inReach || !s.melee.valid())
        {
            e.phase = Encounter::PHASE_AI_ACT;
            e.timer = 200;
            return;
        }

        // Find a free spot next to the target that the creature can actually walk to (navmesh path, not a straight
        // line: caves, hills and walls are in the way), then spend at most its movement along that path.
        float stand = std::max(1.0f, reach - 1.0f);
        float base = target->GetAngle(cr);
        Movement::PointsArray bestPath;
        float bestLen = 1e9f;
        static float const offsets[] = { 0.f, 0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f, 2.0f, -2.0f, 2.6f, -2.6f, 3.1f };
        for (float off : offsets)
        {
            float a = base + off;
            float x = target->GetPositionX() + std::cos(a) * stand;
            float y = target->GetPositionY() + std::sin(a) * stand;
            float z = target->GetPositionZ();
            target->UpdateGroundPositionZ(x, y, z);
            Position p(x, y, z);
            if (!SpotFree(e, cr, p))
                continue;
            Movement::PointsArray path;
            float len = 0.f;
            if (!PathTo(cr, p, path, len))
                continue;
            if (len < bestLen)
            {
                bestLen = len;
                bestPath = path;
            }
            if (len <= c.moveMax)
                break;                       // first reachable spot nearest the straight line wins
        }
        if (bestPath.empty())
        {
            float len = 0.f;
            PathTo(cr, target->GetPosition(), bestPath, len);
            bestLen = len;
        }
        if (bestPath.size() < 2)
        {
            Broadcast(e, Acore::StringFormat("{} can find no way to {}.", c.name, target->GetName()));
            e.phase = Encounter::PHASE_AI_END;
            e.timer = 600;
            return;
        }
        // Ranged attackers stop at the first point on the path where they're in range and can see the target.
        // If nothing can be attacked this turn even after moving, Dash (spend the action on movement instead).
        float budget = c.moveMax;
        bool ranged = s.ranged.valid();
        auto shootSpot = [&](float maxWalk, float& walked) -> bool
        {
            for (float d = 0.f; d <= maxWalk; d += 1.0f)
            {
                Position p = AlongPath(bestPath, std::min(d, bestLen));
                if (target->GetExactDist(&p) <= s.ranged.range &&
                    target->IsWithinLOS(p.GetPositionX(), p.GetPositionY(), p.GetPositionZ()))
                {
                    walked = d;
                    return true;
                }
            }
            return false;
        };
        float walked = 0.f;
        bool canShoot = ranged && shootSpot(std::min(budget, bestLen), walked);
        bool canMelee = s.melee.valid() && bestLen <= budget;
        if (!canShoot && !canMelee && !c.actionUsed)
        {
            c.actionUsed = true;
            budget *= 2.f;
            c.moveMax = budget;
            Broadcast(e, Acore::StringFormat("{} |cffffd100Dashes|r (Action: double movement).", c.name));
            canShoot = ranged && shootSpot(std::min(budget, bestLen), walked);
        }
        float go = canShoot ? walked : std::min(budget, bestLen);
        e.aiRanged = canShoot && !canMelee;
        if (go < 0.5f)
        {
            e.phase = Encounter::PHASE_AI_ACT;
            e.timer = 200;
            return;
        }
        Position dest = AlongPath(bestPath, go);
        if (!SpotFree(e, cr, dest))
            dest = AlongPath(bestPath, std::max(0.f, go - 1.6f));

        // players adjacent to the creature now and not after the move get an opportunity attack
        for (Combatant& o : e.order)
        {
            if (!o.player || o.gone || !o.alive || o.reactionUsed || o.noReaction || o.downed)
                continue;                            // (someone down on the ground swings at nothing)
            Unit* pu = e.GetUnit(o);
            if (!pu || !pu->IsAlive())
                continue;
            float r = MeleeRange(pu, cr);
            if (pu->GetExactDist(cr) <= r && pu->GetExactDist(&dest) > r)
            {
                o.reactionUsed = true;
                Broadcast(e, Acore::StringFormat("|cff40c0ff{} makes an Opportunity Attack against {}!|r", o.name, c.name));
                WeaponAttack(pu, cr, nullptr, true);
                if (!cr->IsAlive())
                    return;
            }
        }

        Broadcast(e, Acore::StringFormat("{} moves {:.1f} m{}.", c.name, go,
            bestLen > go + 0.5f ? Acore::StringFormat(" ({:.0f} m path to {})", bestLen, target->GetName()) : ""));
        cr->GetMotionMaster()->Clear();
        cr->GetMotionMaster()->MovePoint(MOVE_POINT_ID, dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ(), FORCED_MOVEMENT_RUN);
        e.phase = Encounter::PHASE_AI_MOVING;
        e.timer = 6000;                      // safety net if MovementInform never fires
    }

    // Attacks are telegraphed: the creature winds up (swing, draw, precast) and the roll resolves a beat later,
    // as the blow lands.
    static bool HasWeapon(Creature* cr, uint8 slot) { return cr->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + slot) != 0; }
    static bool IsElemental(DmgType t) { return t >= DMG_FIRE; }

    static Unit* AiTarget(Encounter& e)
    {
        for (Combatant const& c : e.order)
            if (c.guid == e.aiTarget)
                return e.GetUnit(c);
        return nullptr;
    }

    // An enemy attack plays out in beats: the roll (shown in the roll feed) and a ready stance, then the swing / throw /
    // cast, then the blow lands with the result that was rolled.
    static uint32 const ROLL_BEAT = 1100;    // the feed's die tumbles and lands in this time

    static void CreatureSwing(Encounter& e, Creature* cr)
    {
        Unit* target = AiTarget(e);
        if (!target || !target->IsAlive())
            return;
        cr->SetFacingToObject(target);
        CreatureStats const& s = StatsOrDefault(cr);
        if (e.aiStrike == 1)
            cr->HandleEmoteCommand(HasWeapon(cr, 0) ? EMOTE_ONESHOT_ATTACK1H : EMOTE_ONESHOT_ATTACK_UNARMED);
        else
        {
            if (IsElemental(s.ranged.type))
                cr->HandleEmoteCommand(EMOTE_ONESHOT_SPELL_CAST);
            else if (HasWeapon(cr, 2))
                cr->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK_RIFLE);
            else
                cr->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK_THROWN);
            cr->CastSpell(target, CreatureMissile(cr), true);   // the missile lands the pending result (ResolveHit)
        }
    }

    static void CreatureStrike(Encounter& e, Creature* cr)
    {
        if (e.aiStrike == 1)
            ApplyPendingAttack(cr);          // the swing connects
    }

    static void CreatureAct(Encounter& e, Creature* cr)
    {
        CreatureStats const& s = StatsOrDefault(cr);
        Unit* target = nullptr;
        for (Combatant const& c : e.order)
            if (c.guid == e.aiTarget)
                target = e.GetUnit(c);
        if (!target || !target->IsAlive())
        {
            Broadcast(e, Acore::StringFormat("{} has no target.", cr->GetName()));
            return;
        }
        cr->SetFacingToObject(target);
        if (e.aiCast >= 0 || CreaturePlanCast(e, cr))
        {
            if (Combatant* me = e.Find(cr->GetGUID()))
            {
                if (me->actionUsed)
                {
                    e.aiCast = -1;
                    return;
                }
                me->actionUsed = true;
            }
            cr->HandleEmoteCommand(EMOTE_ONESHOT_SPELL_PRECAST);
            e.aiStrike = 3;                      // a spell: the precast, then the cast, then what it does
            e.aiStrikeStage = 0;
            e.phase = Encounter::PHASE_AI_STRIKE;
            e.timer = ROLL_BEAT;
            return;
        }
        if (Combatant* me = e.Find(cr->GetGUID()))
        {
            if (me->actionUsed)
                return;                      // dashed this turn
            me->actionUsed = true;
        }
        float dist = cr->GetExactDist(target);
        if (!e.aiRanged && s.melee.valid() && dist <= MeleeRange(cr, target) + 0.5f)
        {
            PrepareCreatureAttack(cr, target, false);
            cr->HandleEmoteCommand(HasWeapon(cr, 0) ? EMOTE_ONESHOT_READY1H : EMOTE_ONESHOT_READY_UNARMED);
            e.aiStrike = 1;
            e.aiStrikeStage = 0;
            e.phase = Encounter::PHASE_AI_STRIKE;
            e.timer = ROLL_BEAT;
        }
        else if (s.ranged.valid() && dist <= s.ranged.range && cr->IsWithinLOSInMap(target))
        {
            PrepareCreatureAttack(cr, target, true);
            if (IsElemental(s.ranged.type))
                cr->HandleEmoteCommand(EMOTE_ONESHOT_SPELL_PRECAST);
            else if (HasWeapon(cr, 2))
                cr->HandleEmoteCommand(EMOTE_ONESHOT_READY_RIFLE);
            else
                cr->HandleEmoteCommand(EMOTE_ONESHOT_READY_UNARMED);
            e.aiStrike = 2;
            e.aiStrikeStage = 0;
            e.phase = Encounter::PHASE_AI_STRIKE;
            e.timer = ROLL_BEAT;
        }
        else
            Broadcast(e, Acore::StringFormat("{} cannot reach {} this turn.", cr->GetName(), target->GetName()));
    }

    void OnCreatureMoveDone(Creature* cr)
    {
        Encounter* e = EncounterOf(cr->GetGUID());
        if (!e || e->phase != Encounter::PHASE_AI_MOVING)
            return;
        Combatant* c = e->Current();
        if (!c || c->guid != cr->GetGUID())
            return;
        e->phase = Encounter::PHASE_AI_ACT;
        e->timer = 250;
    }

    // ------------------------------------------------------------------ tick
    void UpdateCombat(uint32 diff)
    {
        uint32 now = Now();
        for (auto r = sRespawns.begin(); r != sRespawns.end();)
        {
            if (r->dueMs > now)
            {
                ++r;
                continue;
            }
            if (r->spawnId)
                RespawnSpawn(sMapMgr->FindMap(r->mapId, r->instanceId), r->spawnId);
            r = sRespawns.erase(r);
        }
        for (auto itr = sEncounters.begin(); itr != sEncounters.end();)
        {
            Encounter& e = itr->second;
            if (e.phase == Encounter::PHASE_DONE)
            {
                itr = sEncounters.erase(itr);
                continue;
            }
            ++itr;

            e.areas.erase(std::remove_if(e.areas.begin(), e.areas.end(), [now](Area const& a) { return a.expiresMs <= now; }), e.areas.end());
            if (Now() - e.lastVisualSync > 500)
                SyncVisuals(e);
            // KeepProneDown: the core stands units up when they're hit or engage; a prone combatant stays on the ground
            for (Combatant const& pc : e.order)
                if (pc.prone && pc.alive && !pc.gone)
                    if (Unit* pu = e.GetUnit(pc))
                        if (pu->IsAlive() && pu->getStandState() != UNIT_STAND_STATE_SLEEP)
                            pu->SetStandState(UNIT_STAND_STATE_SLEEP);
            if (now - e.lastOrderPush > 1500)
                PushOrder(e);
            if (now - e.lastPositionPush > 200)
                PushPositions(e);

            if (e.timer > 0)
                e.timer -= int32(diff);

            Combatant* c = e.Current();
            Unit* u = c ? e.GetUnit(*c) : nullptr;
            switch (e.phase)
            {
                case Encounter::PHASE_WAIT_PLAYER:
                    if (!u || !u->IsAlive() || c->gone)
                    {
                        e.phase = Encounter::PHASE_NEXT;
                        e.timer = 300;
                    }
                    else if (e.timer < 0)
                    {
                        Broadcast(e, Acore::StringFormat("{} ran out of time.", c->name));
                        EndTurn(u);
                    }
                    break;
                case Encounter::PHASE_NEXT:
                    if (e.timer <= 0)
                        Advance(e);
                    break;
                case Encounter::PHASE_AI_START:
                    if (e.timer <= 0)
                    {
                        if (!u || !u->IsAlive() || !u->ToCreature())
                        {
                            e.phase = Encounter::PHASE_NEXT;
                            e.timer = 0;
                        }
                        else
                            RunCreatureTurn(e, *c, u->ToCreature());
                    }
                    break;
                case Encounter::PHASE_AI_MOVING:
                    if (e.timer <= 0)
                    {
                        e.phase = Encounter::PHASE_AI_ACT;
                        e.timer = 0;
                    }
                    break;
                case Encounter::PHASE_AI_ACT:
                    if (e.timer <= 0)
                    {
                        e.aiStrike = 0;
                        if (u && u->IsAlive() && u->ToCreature())
                            CreatureAct(e, u->ToCreature());
                        if (e.phase == Encounter::PHASE_DONE || e.phase == Encounter::PHASE_AI_STRIKE)
                            break;
                        e.phase = Encounter::PHASE_AI_END;
                        e.timer = 1100;
                    }
                    break;
                case Encounter::PHASE_AI_STRIKE:
                    if (e.timer <= 0)
                    {
                        bool ranged = e.aiStrike == 2;
                        bool casting = e.aiStrike == 3;
                        Creature* cr = u && u->IsAlive() ? u->ToCreature() : nullptr;
                        if (e.aiStrikeStage == 0 && cr)
                        {
                            if (casting)
                                CreatureCastVisual(e, cr);
                            else
                                CreatureSwing(e, cr);
                            e.aiStrikeStage = 1;
                            e.timer = casting ? 700 : (ranged ? 0 : 450);   // a swing lands mid-animation; a missile on its own
                            if (!ranged)
                                break;
                        }
                        if (cr)
                        {
                            if (casting)
                                CreatureCastResolve(e, cr);
                            else
                                CreatureStrike(e, cr);
                        }
                        e.aiStrike = 0;
                        e.aiStrikeStage = 0;
                        e.aiCast = -1;
                        if (e.phase == Encounter::PHASE_DONE)
                            break;
                        e.phase = Encounter::PHASE_AI_END;
                        e.timer = (ranged || casting) ? 1500 : 1000;   // let a missile land before the next turn
                    }
                    break;
                case Encounter::PHASE_AI_END:
                    if (e.timer <= 0)
                    {
                        e.phase = Encounter::PHASE_NEXT;
                        e.timer = 0;
                    }
                    break;
                default:
                    break;
            }
        }
    }
}
