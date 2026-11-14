/*
 * mod-dnd: hooks into the core (players, movement, damage gate, world tick), the encounter creature AI,
 * the addon channel and the .dnd GM commands.
 */
#include "Dnd.h"

#include "AllCreatureScript.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "CreatureAI.h"
#include "CreatureScript.h"
#include "Map.h"
#include "MapMgr.h"
#include "MovementHandlerScript.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "ServerScript.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "UnitScript.h"
#include "WorldScript.h"
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace Dnd
{
    void HandleClientMessage(Player* p, std::string const& msg)
    {
        if (msg == "hello" || msg == "sheet")
        {
            PushSheet(p);
            PushSkills(p);
            if (!GetSheet(p).created)
                Send(p, "C^open");           // not built yet: the addon opens character creation
            PushEconomy(p);
            if (Encounter* e = EncounterOf(p->GetGUID()))
                PushOrder(*e);
            else
                Send(p, "Z");
            PushProgress(p);
        }
        else if (msg.rfind("build^", 0) == 0)
            HandleBuild(p, msg.substr(6));
        else if (msg.rfind("pick^", 0) == 0)
            HandlePick(p, msg.substr(5));
        else if (msg.rfind("treatgame^", 0) == 0)
            TreatGameResult(p, msg.substr(10));
        else if (msg.rfind("tav^", 0) == 0)
            TavernMessage(p, msg.substr(4));
        else if (msg.rfind("say^", 0) == 0)
            DialogueChoose(p, uint32(std::atoi(msg.c_str() + 4)));
        else if (msg.rfind("hover^", 0) == 0)
            DialogueHover(p, uint32(std::atoi(msg.c_str() + 6)));
        else if (msg == "bye")
            CloseDialogue(p, true, 8000);      // Escape / the panel closed: walk back up (or right-click) to resume
        else if (msg == "endturn")
        {
            if (IsTurnOf(p))
                EndTurn(p);
            else
                Error(p, "It is not your turn.");
        }
    }
}

using namespace Dnd;

static std::unordered_set<ObjectGuid> sClearHearthCooldown;

class DndWorldScript : public WorldScript
{
public:
    DndWorldScript() : WorldScript("DndWorldScript") { }

    void OnStartup() override { LoadWorldData(); }
    void OnUpdate(uint32 diff) override
    {
        UpdateCombat(diff);
        UpdateWeather(diff);
        Test::Update(diff);
    }
};

class DndPlayerScript : public PlayerScript
{
public:
    DndPlayerScript() : PlayerScript("DndPlayerScript") { }

    void OnPlayerLevelChanged(Player* p, uint8 oldLevel) override
    {
        OnLevelUp(p, oldLevel);              // HP, new spells on the bars, the popup and any choice due
    }

    void OnPlayerFirstLogin(Player* p) override
    {
        SetupNewCharacter(p);
    }

    void OnPlayerLogin(Player* p) override
    {
        LoadSheet(p);
        LoadProgress(p);
        SyncXp(p);                           // XP a crash lost since the last save, where the clears were kept
        LoadTreatProgress(p);
        LoadTownProgress(p);
        p->UpdateObjectVisibility();         // treats already claimed: their tells go (DndTreats.cpp)
        EnsureKit(p);
        PushPendingChoices(p);               // a subclass / ASI still to choose comes back after a relog
        p->UpdateMaxHealth();
        PushSheet(p);
        Send(p, "Z");
    }

    void OnPlayerLogout(Player* p) override
    {
        ForgetDowned(p->GetGUID());
        CloseDialogue(p, false);
        OnPlayerLeft(p);
        SaveSheet(p);
        ForgetSheet(p->GetGUID());
        ForgetProgress(p->GetGUID());
        ForgetDialogue(p->GetGUID());
        ForgetClasses(p->GetGUID());
        ForgetTreats(p->GetGUID());
        ForgetTavern(p->GetGUID());
        ForgetWeather(p->GetGUID());
    }

    void OnPlayerJustDied(Player* p) override
    {
        StopSneaking(p, "");
    }

    void OnPlayerMapChanged(Player* p) override
    {
        StopSneaking(p, "");
        CloseDialogue(p, false);
        OnPlayerLeft(p);
    }

    void OnPlayerAfterUpdateMaxHealth(Player* p, float& value) override { value = float(MaxHp(p)); }

    void OnPlayerCalculateTalentsPoints(Player const* /*p*/, uint32& points) override { points = 0; }

    bool OnPlayerCanUseChat(Player* p, uint32 /*type*/, uint32 lang, std::string& msg, Player* /*receiver*/) override
    {
        if (lang == LANG_ADDON && msg.rfind("DNDC\t", 0) == 0)
        {
            HandleClientMessage(p, msg.substr(5));
            return false;
        }
        return true;
    }

    bool OnPlayerCanEnterMap(Player* /*p*/, MapEntry const* entry, InstanceTemplate const* /*instance*/, MapDifficulty const* /*diff*/,
        bool /*loginCheck*/) override
    {
        // 1-60 world only: Eastern Kingdoms, Kalimdor and classic instances
        return entry->Expansion() == 0;
    }

    void OnPlayerSpellCast(Player* p, Spell* spell, bool /*skipCheck*/) override
    {
        if (spell && spell->GetSpellInfo()->Id == SPELL_HEARTHSTONE)
            sClearHearthCooldown.insert(p->GetGUID());
    }

    void OnPlayerUpdate(Player* p, uint32 diff) override
    {
        UpdateGate(p, diff);
        UpdateWeatherAura(p, diff);
        UpdateFood(p, diff);
        UpdateDialogue(p, diff);
        UpdateTreats(p, diff);
        UpdateDowned(p);
        UpdateFallen(p, diff);
        // a released spirit has no corpse to run back to: Brother Marrow, at the graveyard, raises the dead
        if (!p->IsAlive() && p->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !p->IsBeingTeleported() && p->GetCorpse())
        {
            p->SpawnCorpseBones();
            Dnd::Log(p, "|cffc8b890Your spirit drifts to the graveyard, where Brother Marrow waits for the dead. Go to him - or "
                "a friend can pay him to raise you.|r");
        }
        // no white swings, ever, and never WoW "in combat": the D&D engine owns combat, and WoW's combat flag
        // would lock the addon's secure buttons (click-to-target on the turn bar)
        if (p->GetVictim() && !IsDealing())
            p->AttackStop();
        if (p->IsInCombat() && !IsDealing())
            p->CombatStop();
        if (!sClearHearthCooldown.empty() && sClearHearthCooldown.count(p->GetGUID()) && !p->IsNonMeleeSpellCast(false))
        {
            sClearHearthCooldown.erase(p->GetGUID());
            p->RemoveSpellCooldown(SPELL_HEARTHSTONE, true);
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_HEARTHSTONE))
                if (info->GetCategory())
                    p->RemoveCategoryCooldown(info->GetCategory());
        }
    }
};

class DndMovementScript : public MovementHandlerScript
{
public:
    DndMovementScript() : MovementHandlerScript("DndMovementScript") { }

    void OnPlayerMove(Player* p, MovementInfo info, uint32 /*opcode*/) override
    {
        OnPlayerMoved(p, info.pos);
    }
};

class DndUnitScript : public UnitScript
{
public:
    DndUnitScript() : UnitScript("DndUnitScript") { }

    // The damage gate: only the D&D engine hurts things.
    uint32 DealDamage(Unit* /*attacker*/, Unit* /*victim*/, uint32 damage, DamageEffectType /*type*/) override
    {
        return IsDealing() ? damage : 0;
    }

    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (Player* dead = unit->ToPlayer())
        {
            if (Encounter* e = EncounterOf(dead->GetGUID()))
                Announce(dead, Acore::StringFormat("{} has fallen fighting {} in {}.", dead->GetName(), e->name, ZoneName(dead)));
            else
                Announce(dead, Acore::StringFormat("{} has died in {}.", dead->GetName(), ZoneName(dead)));
        }
        OnUnitDied(unit);
        // testing aid: dying hands back your spell slots
        static bool restore = sConfigMgr->GetOption<bool>("Dnd.RestoreSlotsOnDeath", true);
        if (Player* p = unit->ToPlayer())
            if (restore)
            {
                GetSheet(p).slotsUsed.fill(0);
                SaveSheet(p);
                PushSheet(p);
                Dnd::Log(p, "|cff8080ff(testing) Spell slots restored on death.|r");
            }
    }

    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (Player* p = unit->ToPlayer())
            if (aura && GetAction(aura->GetId()))
                PushSheet(p);
    }

    void OnAuraRemove(Unit* unit, AuraApplication* app, AuraRemoveMode /*mode*/) override
    {
        if (Player* p = unit->ToPlayer())
            if (app && GetAction(app->GetBase()->GetId()))
                PushSheet(p);
    }
};

// Every encounter creature runs this AI. It never acts on its own: the engine moves it and rolls for it.
struct npc_dnd_combatant : public CreatureAI
{
    explicit npc_dnd_combatant(Creature* c) : CreatureAI(c) { }

    void ApplyStats()
    {
        CreatureStats const& s = StatsOrDefault(me);
        me->SetCreateHealth(s.hp);
        me->SetMaxHealth(s.hp);
        me->SetHealth(s.hp);
        me->SetRegeneratingHealth(false);
        me->SetReactState(REACT_PASSIVE);
        if (!InCombat(me))
            me->RestoreFaction();            // talkers that were turned hostile come back friendly
    }

    // encounter spawns are placed by hand; stand them on the ground so a guessed height never leaves one floating
    // or buried (and their walk home after a wipe ends on the ground too)
    void SnapToGround()
    {
        if (snapped || me->IsInCombat() || InCombat(me) || !me->FindMap())
            return;
        float x, y, z, o;
        me->GetRespawnPosition(x, y, z, &o);
        float ground = me->GetMapHeight(x, y, z + 4.f, true, 12.f);
        if (ground <= INVALID_HEIGHT || std::fabs(ground - z) > 8.f)
            return;                          // terrain not loaded yet: try again later
        snapped = true;
        if (std::fabs(ground - z) < 0.05f)
            return;
        me->SetHomePosition(x, y, ground, o);
        if (me->GetExactDist2d(x, y) < 1.f)
        {
            if (me->IsInWorld())
                me->NearTeleportTo(x, y, ground, o);
            else
                me->Relocate(x, y, ground, o);
        }
    }

    bool snapped = false;

    void InitializeAI() override { ApplyStats(); SnapToGround(); TalkerAtRest(me); }
    void Reset() override { ApplyStats(); snapped = false; SnapToGround(); TalkerAtRest(me); }
    void JustRespawned() override { ClearLeaving(me->GetGUID()); ApplyStats(); snapped = false; SnapToGround(); TalkerAtRest(me); }

    void MoveInLineOfSight(Unit* who) override
    {
        SnapToGround();
        if (InCombat(me) || !who || !who->IsPlayer() || !who->IsAlive() || !me->IsAlive())
            return;
        Player* p = who->ToPlayer();
        if (p->IsGameMaster())
            return;
        EncounterDef const* def = GetEncounterDefBySpawn(me->GetSpawnId());
        if (def && def->dialogueNode && HasGrudge(p, def->id))
        {
            // fought them before: no more talking
            if (!HasCleared(p, def->id) && p->IsAlive() && !InCombat(p) && me->GetExactDist(p) <= def->aggroRange
                && me->IsWithinLOSInMap(p))
            {
                if (InGrace(p))
                    return;
                Dnd::Log(p, Acore::StringFormat("|cffff8060{} remembers you - no more talking.|r", me->GetName()));
                for (uint32 spawn : def->spawns)
                {
                    auto range = me->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
                    for (auto itr = range.first; itr != range.second; ++itr)
                        if (itr->second->IsAlive())
                            itr->second->SetFaction(14);
                }
                StartEncounter(me, p);
            }
            return;
        }
        if (def && def->dialogueNode)
        {
            // talkers open a conversation instead of rolling initiative - unless a sneaking rogue slips past them
            if (!InDialogue(p) && !HasCleared(p, def->id) && !DialogueDeclined(p, def->id) && p->IsAlive() && !InCombat(p)
                && me->GetExactDist(p) <= def->aggroRange && me->IsWithinLOSInMap(p))
            {
                if (IsSneaking(p))
                {
                    if (!NoticesSneaker(me, p))
                        return;
                    StopSneaking(p, "You've been spotted.");
                }
                StartDialogue(p, me);
            }
            return;
        }
        if (!me->IsHostileTo(p))
            return;
        float range = def ? def->aggroRange : 10.f;
        if (me->GetExactDist(p) <= range && me->IsWithinLOSInMap(p))
        {
            // a sneaking rogue: each creature gets one Perception roll when they come close
            if (IsSneaking(p))
            {
                if (SneakStruck(p))
                    StopSneaking(p, "Your strike gave you away.");
                else if (!NoticesSneaker(me, p))
                    return;
            }
            StartEncounter(me, p);
        }
    }

    void AttackStart(Unit* /*who*/) override { }
    void UpdateAI(uint32 /*diff*/) override { }
    void EnterEvadeMode(EvadeReason /*why*/) override { }
    void JustEngagedWith(Unit* who) override
    {
        // only a real pull; WoW's "call for help" must not drag a neighbouring encounter in
        if (who && who->IsPlayer() && !InCombat(me) && me->GetExactDist(who) <= 20.f)
            StartEncounter(me, who);
    }

    void SpellHit(Unit* caster, SpellInfo const* /*spell*/) override
    {
        // (a blast that catches a second group mid-fight pulls that group into the same fight)
        if (caster && caster->IsPlayer() && !InCombat(me) && me->IsAlive())
        {
            if (InCombat(caster))
                PullGroupIntoFight(me, caster->ToPlayer());
            else
                StartEncounter(me, caster);
        }
    }

    void MovementInform(uint32 type, uint32 /*id*/) override
    {
        if (type == POINT_MOTION_TYPE)
            OnCreatureMoveDone(me);
    }
};

CreatureAI* Dnd::MakeCombatantAI(Creature* c) { return new npc_dnd_combatant(c); }

// The AI plus a right-click: talkers open their conversation instead of a gossip window.
class npc_dnd_combatant_script : public CreatureScript
{
public:
    npc_dnd_combatant_script() : CreatureScript("npc_dnd_combatant") { }

    CreatureAI* GetAI(Creature* creature) const override { return new npc_dnd_combatant(creature); }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        CloseGossipMenuFor(player);
        if (!InDialogue(player))
            StartDialogue(player, creature, true);
        return true;
    }
};

class dnd_commandscript : public CommandScript
{
public:
    dnd_commandscript() : CommandScript("dnd_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable dndTable =
        {
            { "reload",  HandleReload,  rbac::RBAC_PERM_COMMAND_RELOAD, Console::Yes },
            { "rest",    HandleRest,    rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No },
            { "sheet",   HandleSheet,   rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No },
            { "fight",   HandleFight,   rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No },
            { "endturn", HandleEndTurn, rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No },
            { "setup",   HandleSetup,   rbac::RBAC_PERM_COMMAND_DEBUG,  Console::No },
            { "resetfights", HandleResetFights, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "restart", HandleRestart, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "progress", HandleProgress, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "spawncheck", HandleSpawnCheck, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "gateprobe", HandleGateProbe, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "gatefit", HandleGateFit, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "level", HandleLevel, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "tour", HandleTour, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "selftest", HandleSelfTest, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "weather", HandleWeather, rbac::RBAC_PERM_COMMAND_DEBUG, Console::No },
            { "whereis", HandleWhereIs, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "fightdump", HandleFightDump, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
            { "treatsback", HandleTreatsBack, rbac::RBAC_PERM_COMMAND_DEBUG, Console::Yes },
        };
        static ChatCommandTable root = { { "dnd", dndTable } };
        return root;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadWorldData();
        handler->SendSysMessage("mod-dnd: stat blocks and encounters reloaded.");
        return true;
    }

    static bool HandleRest(ChatHandler* handler)
    {
        if (Player* p = handler->GetPlayer())
            LongRest(p);
        return true;
    }

    static bool HandleSetup(ChatHandler* handler)
    {
        if (Player* p = handler->GetPlayer())
            SetupNewCharacter(p);
        return true;
    }

    static bool HandleSheet(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        Sheet& s = GetSheet(p);
        handler->PSendSysMessage("Lvl {} STR {} DEX {} CON {} INT {} WIS {} CHA {} | AC {} HP {}/{} (+{} temp) | DC {} atk +{} | slots L1 {}/{}",
            Level(p), s.abil[STR], s.abil[DEX], s.abil[CON], s.abil[Dnd::INT], s.abil[WIS], s.abil[CHA], ArmorClass(p), p->GetHealth(),
            p->GetMaxHealth(), s.tempHp, SpellSaveDC(p), SpellAttackBonus(p), SlotsLeft(p, 1), SlotsMax(p, 1));
        PushSheet(p);
        return true;
    }

    static bool HandleFight(ChatHandler* handler)
    {
        Creature* c = handler->getSelectedCreature();
        if (!c)
        {
            handler->SendSysMessage("Select a creature.");
            return true;
        }
        StartEncounter(c, handler->GetPlayer());
        return true;
    }

    // testing: every encounter on this map comes back to life (won fights included)
    static bool HandleResetFights(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        // by spawn id, not by the creatures present: a fallen creature whose corpse is gone isn't on the map at all
        uint32 n = 0;
        for (EncounterDef const* def : AllEncounters())
            for (uint32 spawnId : def->spawns)
                if (RespawnSpawn(p->GetMap(), spawnId))
                    ++n;
        CharacterDatabase.Execute("DELETE FROM creature_respawn WHERE guid BETWEEN 9000000 AND 9000999");
        handler->PSendSysMessage("mod-dnd: {} encounter creatures respawned or sent home.", n);
        return true;
    }

    // every treat back in the world for everyone (they're gone for all once anyone takes one)
    static bool HandleTreatsBack(ChatHandler* handler)
    {
        handler->PSendSysMessage("mod-dnd: {} taken treats returned to the world (characters who took them keep what they got).",
            ReturnAllTreats());
        return true;
    }

    // testing: back to a fresh level 1 character with every encounter alive and uncleared
    static bool HandleRestart(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p || InCombat(p))
            return false;
        ResetProgress(p);
        ResetTreats(p);
        ResetTownsfolk(p);
        ResetTavern(p);
        ClearLevelChoices(p);
        ClearGrudges(p);
        if (p->GetLevel() != 1)
            p->GiveLevel(1);
        p->SetUInt32Value(PLAYER_XP, 0);
        p->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(1));
        LongRest(p);
        HandleResetFights(handler);
        PushProgress(p);
        handler->SendSysMessage("mod-dnd: progress reset - level 1, no encounters cleared.");
        return true;
    }

    static bool HandleProgress(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        for (EncounterDef const* e : AllEncounters())
        {
            ZoneDef const* z = GetZone(e->zoneId);
            handler->PSendSysMessage("[{}] {} - {} ({} XP){}", e->id, z ? z->name : "no zone", e->name, e->xp,
                HasCleared(p, e->id) ? " - cleared" : "");
        }
        PushProgress(p);
        return true;
    }

    // every encounter spawn against the collision data; "fix" moves the bad ones (see DndPlacement.cpp)
    // testing: jump to a D&D level (the level-up popup, new spells and choices fire as they would in play)
    static bool HandleLevel(ChatHandler* handler, uint8 level)
    {
        Player* p = handler->GetPlayer();
        if (!p || InCombat(p))
            return false;
        level = std::clamp<uint8>(level, 1, MAX_DND_LEVEL);
        p->GiveLevel(level);
        p->SetUInt32Value(PLAYER_XP, 0);
        p->SetUInt32Value(PLAYER_NEXT_LEVEL_XP, sObjectMgr->GetXPForLevel(level));
        handler->PSendSysMessage("mod-dnd: level {}.", level);
        return true;
    }

    static bool HandleGateProbe(ChatHandler* handler, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        ProbeGate(handler, 0, x1, y1, z1, x2, y2, z2);
        return true;
    }

    // along a straight stretch of tunnel: wall-to-wall width and roof height every metre, and the best fit for a gate
    static bool HandleGateFit(ChatHandler* handler, float x1, float y1, float z1, float x2, float y2, float z2)
    {
        FitGate(handler, 0, x1, y1, z1, x2, y2, z2);
        return true;
    }

    static bool HandleSpawnCheck(ChatHandler* handler, Optional<std::string> arg)
    {
        CheckSpawns(handler, arg && *arg == "fix");
        return true;
    }

    // teleport to the next encounter (or encounter N) to look at how it's placed
    static bool HandleTour(ChatHandler* handler, Optional<uint32> which)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        static std::unordered_map<ObjectGuid, uint32> next;
        std::vector<EncounterDef const*> all = AllEncounters();
        if (all.empty())
            return true;
        uint32 i = which ? *which : next[p->GetGUID()];
        EncounterDef const* def = nullptr;
        for (EncounterDef const* e : all)
            if (which ? e->id == i : true)
            {
                def = e;
                if (which)
                    break;
            }
        if (!which)
            def = all[i % all.size()];
        if (!def || def->spawns.empty())
            return true;
        next[p->GetGUID()] = (which ? 0 : i) + 1;
        CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front());
        if (!d)
            return true;
        float x = d->posX + 6.f, y = d->posY, z = d->posZ + 1.f;
        p->TeleportTo(d->mapid, x, y, z, std::atan2(d->posY - y, d->posX - x));
        handler->PSendSysMessage("Encounter {}: {} ({} spawns). .dnd tour for the next one.", def->id, def->name, def->spawns.size());
        return true;
    }

    // the automatic test jig (DndTest.cpp): bots in the Shimmering Flats try every action; results in logs/dnd_selftest.log
    static bool HandleSelfTest(ChatHandler* handler, Optional<std::string> filter)
    {
        Test::Start(handler, filter ? *filter : "");
        return true;
    }

    // .dnd weather [clear|fog|flurries|snowfall|blizzard|drizzle|downpour|thunderstorm|gale]: what's falling here, or
    // set it for this zone (30 minutes, then the climate takes over again)
    static bool HandleWeather(ChatHandler* handler, Optional<std::string> what)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return false;
        if (!what)
        {
            handler->PSendSysMessage("Weather here: {}{}", WeatherName(WeatherAt(p)), p->IsOutdoors() ? "" : " (you're indoors)");
            return true;
        }
        for (uint8 k = 0; k < WEATHER_COUNT; ++k)
            if (StringEqualI(*what, WeatherName(WeatherKind(k))))
            {
                SetWeather(p->GetMapId(), p->GetZoneId(), WeatherKind(k), true);
                handler->PSendSysMessage("Weather in this zone: {}.", WeatherName(WeatherKind(k)));
                return true;
            }
        handler->SendSysMessage("clear, fog, flurries, snowfall, blizzard, drizzle, downpour, thunderstorm or gale");
        return true;
    }

    // .dnd whereis <spawn guid> [...]: the area each creature spawn stands in and whether it's indoors (for writing
    // townsfolk / guard lines that fit where they are; tools/check_places.py)
    static bool HandleWhereIs(ChatHandler* handler, std::vector<uint32> guids)
    {
        for (uint32 guid : guids)
        {
            CreatureData const* d = sObjectMgr->GetCreatureData(guid);
            Map* map = d ? sMapMgr->CreateBaseMap(d->mapid) : nullptr;
            if (!d || !map)
            {
                handler->PSendSysMessage("{} ?", guid);
                continue;
            }
            map->LoadGrid(d->posX, d->posY);
            Creature* c = nullptr;
            auto range = map->GetCreatureBySpawnIdStore().equal_range(guid);
            if (range.first != range.second)
                c = range.first->second;
            uint32 areaId = map->GetAreaId(PHASEMASK_NORMAL, d->posX, d->posY, d->posZ);
            AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId);
            handler->PSendSysMessage("{} {} | {} | {}", guid, c ? c->GetName() : "-", area ? area->area_name[0] : "?",
                c ? (c->IsOutdoors() ? "outdoors" : "indoors") : "not loaded");
        }
        return true;
    }

    static bool HandleFightDump(ChatHandler* handler)
    {
        std::string all = FightDump();
        std::size_t at = 0;
        while (at < all.size())
        {
            std::size_t nl = all.find('\n', at);
            if (nl == std::string::npos)
                nl = all.size();
            handler->SendSysMessage(all.substr(at, nl - at));
            at = nl + 1;
        }
        return true;
    }

    static bool HandleEndTurn(ChatHandler* handler)
    {
        if (Player* p = handler->GetPlayer())
            EndTurn(p);
        return true;
    }
};

// Friendly villagers: a right-click opens their D&D talk instead of WoW's gossip window
class DndTownsfolkScript : public AllCreatureScript
{
public:
    DndTownsfolkScript() : AllCreatureScript("DndTownsfolkScript") { }

    void OnCreatureAddWorld(Creature* c) override
    {
        if (IsTownsfolk(c))
            c->SetNpcFlag(UNIT_NPC_FLAG_GOSSIP);   // the speech-bubble cursor: they can be talked to
    }

    bool CanCreatureGossipHello(Player* p, Creature* c) override { return StartTownTalk(p, c); }
};

// No WoW auto-attack: a right-click on an enemy would start white swings whose damage the engine throws away
// (the client still shows the number). Attacks are the D&D Attack action only.
class DndPacketScript : public ServerScript
{
public:
    DndPacketScript() : ServerScript("DndPacketScript") { }

    bool CanPacketReceive(WorldSession* /*session*/, WorldPacket const& packet) override
    {
        return packet.GetOpcode() != CMSG_ATTACKSWING;
    }
};

// ------------------------------------------------------------------ Brother Marrow
uint32 Dnd::RaiseCost(Player* dead) { return dead ? 200u * std::max<uint32>(1, dead->GetLevel()) : 0; }

bool Dnd::MarrowRaise(Creature* marrow, Player* dead, Player* payer)
{
    if (!marrow || !dead || dead->IsAlive())
        return false;
    uint32 cost = payer ? RaiseCost(dead) : 0;
    if (payer && !payer->HasEnoughMoney(cost))
    {
        Dnd::Log(payer, Acore::StringFormat("|cffc8b890Brother Marrow: \"Bones don't raise themselves, friend. {} silver - and "
            "you're short.\"|r", cost / 100));
        return false;
    }
    if (payer)
        payer->ModifyMoney(-int32(cost));
    if (EncounterOf(dead->GetGUID()))
        OnPlayerLeft(dead);                  // (fell in a fight that's still going: out of it now)
    static char const* const freeLines[] = {
        "Up you get. No charge - the dead have troubles enough.",
        "Back again? Never mind. Bones knit, and so will you.",
        "There we are. Mind the step - you're a bit wobbly for the first while.",
    };
    static char const* const paidLines[] = {
        "Your friend paid. Lucky you - most people's friends just take their boots.",
        "Coin received, soul returned. Try to keep it in you this time.",
    };
    std::string line = payer ? paidLines[urand(0, 1)] : freeLines[urand(0, 2)];
    Position at = marrow->GetFirstCollisionPosition(2.f, 0.f);
    dead->RemoveAurasDueToSpell(SPELL_FALLEN);
    dead->ResurrectPlayer(1.0f);
    dead->SpawnCorpseBones();
    GrantGrace(dead, 10000);
    if (dead->GetMap() == marrow->GetMap())
        dead->NearTeleportTo(at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), marrow->GetAngle(&at) + float(M_PI));
    else
        dead->TeleportTo(marrow->GetMapId(), at.GetPositionX(), at.GetPositionY(), at.GetPositionZ(), marrow->GetOrientation());
    marrow->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
    Dnd::Log(dead, Acore::StringFormat("|cffc8b890Brother Marrow: \"{}\"|r", line));
    if (payer)
    {
        Dnd::Log(payer, Acore::StringFormat("|cffc8b890You pay Brother Marrow {} silver, and {} is raised beside him.|r", cost / 100,
            dead->GetName()));
        Announce(dead, Acore::StringFormat("{} paid Brother Marrow to raise {}.", payer->GetName(), dead->GetName()));
    }
    else
        Announce(dead, Acore::StringFormat("{} was raised by Brother Marrow in {}.", dead->GetName(), ZoneName(dead)));
    PushSheet(dead);
    return true;
}

// the dead who come to him (ghosts are released at his graveyard) are raised for nothing
struct npc_dnd_marrow_ai : public CreatureAI
{
    explicit npc_dnd_marrow_ai(Creature* c) : CreatureAI(c) { }
    uint32 timer = 0;

    void UpdateAI(uint32 diff) override
    {
        timer += diff;
        if (timer < 1000)
            return;
        timer = 0;
        std::vector<Player*> ghosts;
        me->GetMap()->DoForAllPlayers([&](Player* p)
        {
            if (!p->IsAlive() && p->HasPlayerFlag(PLAYER_FLAGS_GHOST) && !p->IsBeingTeleported() && p->GetExactDist(me) <= 15.f)
                ghosts.push_back(p);
        });
        for (Player* p : ghosts)
            MarrowRaise(me, p, nullptr);
    }
    void AttackStart(Unit* /*who*/) override { }
    void MoveInLineOfSight(Unit* /*who*/) override { }
};

class npc_dnd_marrow : public CreatureScript
{
public:
    npc_dnd_marrow() : CreatureScript("npc_dnd_marrow") { }

    CreatureAI* GetAI(Creature* c) const override { return new npc_dnd_marrow_ai(c); }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);
        if (!player->IsAlive())
        {
            CloseGossipMenuFor(player);
            MarrowRaise(creature, player, nullptr);
            return true;
        }
        int n = 0;
        for (auto const& [guid, other] : ObjectAccessor::GetPlayers())
            if (other && other != player && !other->IsAlive() && other->IsInWorld() && !Test::IsBot(guid) && n < 20)
            {
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, Acore::StringFormat("Raise {} ({} silver)", other->GetName(),
                    RaiseCost(other) / 100), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + other->GetGUID().GetCounter());
                ++n;
            }
        if (!n)
        {
            CloseGossipMenuFor(player);
            creature->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
            Dnd::Log(player, "|cffc8b890Brother Marrow: \"Nobody needs me today. Come back when one of your friends has had a "
                "worse day than you.\"|r");
            return true;
        }
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        Player* dead = ObjectAccessor::FindPlayerByLowGUID(action - GOSSIP_ACTION_INFO_DEF);
        if (!dead || dead->IsAlive())
        {
            Dnd::Log(player, "|cffc8b890Brother Marrow: \"That one's already up and about. Good for them.\"|r");
            return true;
        }
        MarrowRaise(creature, dead, player);
        return true;
    }
};

void AddSC_dnd_spells();
void AddSC_dnd_treats();
void AddSC_dnd_endgame();

void Addmod_dndScripts()
{
    new DndWorldScript();
    new DndPlayerScript();
    new DndMovementScript();
    new DndUnitScript();
    new npc_dnd_combatant_script();
    new DndTownsfolkScript();
    new DndPacketScript();
    new dnd_commandscript();
    new npc_dnd_marrow();
    AddSC_dnd_spells();
    AddSC_dnd_treats();
    AddSC_dnd_endgame();
}
