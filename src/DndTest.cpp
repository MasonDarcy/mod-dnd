/*
 * mod-dnd self-test jig:  .dnd selftest [filter]   (GM or SOAP; tools/selftest.py runs it and prints the report)
 *
 * Headless bot characters (no client: a WorldSession without a socket) are made in an empty stretch of the Shimmering
 * Flats, given a class, level, subclass and picks, and then cast every action through the real spell path - the spell
 * script's CheckCast / AfterCast / hit, the action economy, turns - against summoned test creatures that run the
 * encounter AI with stat blocks set here. The dice are rigged (a queue of exact rolls, then a fallback value for every
 * other die), so each outcome - hit or miss, damage, saves, conditions - is checked against the 5e/BG3 rule by number.
 *
 * Every test gets fresh bots and creatures and removes them afterwards; nothing is saved (the bots' dnd_character
 * rows are deleted). Results: logs/dnd_selftest.log (one PASS/FAIL line per test, then "DONE"); while a run is going,
 * the dice log goes to logs/dnd_selftest_dice.log instead of dnd.log.
 */
#include "Dnd.h"
#include "PathGenerator.h"
#include "GameEventMgr.h"

#include "AccountMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace Dnd::Test
{
namespace
{
    // an empty stretch of the Shimmering Flats: no spawns within ~150 m, flat salt
    constexpr uint32 MAP_ID = 1;
    constexpr float HOME_X = -5900.f, HOME_Y = -3900.f;
    constexpr uint32 HUMANOID = 87;      // Forest Troll Berserker: a humanoid template with no script of its own
    constexpr uint32 BEAST = 3566;       // Flatland Prowler: a beast, same

    // gnome mage, human warrior, dwarf warrior, dwarf mage, dwarf shaman (barbarian), gnome rogue, dwarf druid (monk),
    // gnome priest (sorcerer), gnome warlock
    enum Kind { WIZ, FIGHTER, DWARF, DWARF_WIZ, BARB, ROGUE, MONK, CLERIC, LOCK, BARD, DRUID };

    std::string& SavedSubName() { static std::string s; return s; }   // (a test's borrowed creature title)

    struct Ctx
    {
        std::vector<ObjectGuid> bots, mobs;
        std::map<std::string, float> v;          // values carried from one step to the next
        std::vector<std::string> fails;
        int checks = 0;
    };
    using Fn = std::function<void(Ctx&)>;
    using Cond = std::function<bool(Ctx&)>;

    struct Step
    {
        Fn act;
        uint32 wait = 0;
        Cond until;
        uint32 timeout = 0;
        std::string what;
    };

    struct Script
    {
        std::vector<Step> steps;
        Script& Do(Fn f) { Step s; s.act = std::move(f); steps.push_back(std::move(s)); return *this; }
        Script& Wait(uint32 ms) { Step s; s.wait = ms; steps.push_back(std::move(s)); return *this; }
        Script& Until(Cond c, std::string what, uint32 timeout = 8000)
        {
            Step s;
            s.until = std::move(c);
            s.what = std::move(what);
            s.timeout = timeout;
            steps.push_back(std::move(s));
            return *this;
        }
    };

    // slow: the world suites (every real encounter fought, every dialogue walked) - only run when asked for
    struct TestDef { std::string name; std::function<void(Script&)> build; bool slow = false; };

    // ------------------------------------------------------------------ run state
    bool sRunning = false;
    std::vector<TestDef> sTests;
    std::size_t sTestIdx = 0;
    Script sScript;
    std::size_t sStep = 0;
    bool sStepStarted = false;
    uint32 sElapsed = 0;
    Ctx sCtx;
    std::ofstream sOut;
    int sPassed = 0, sFailed = 0;
    std::vector<std::string> sFailedNames;
    uint32 sAccount = 0;
    std::set<ObjectGuid> sBotSet;
    std::vector<uint32> sAllBotGuids;
    std::vector<std::string> sMsgs;
    std::map<ObjectGuid, std::vector<std::string>> sMsgsOf;     // the same, per bot (multiplayer tests)
    std::vector<std::string> sAnnounced;                         // [Update] lines about bots (never sent to the world)
    bool WasAnnounced(std::string const& needle)
    {
        for (std::string const& a : sAnnounced)
            if (a.find(needle) != std::string::npos)
                return true;
        return false;
    }
    std::deque<int> sDice;
    int sFallback = 0;
    uint32 sCooldown = 0;                        // pause between tests (despawns settle)

    void Out(std::string const& line)
    {
        if (sOut)
        {
            sOut << line << '\n';
            sOut.flush();
        }
        LOG_INFO("module", "dnd selftest: {}", line);
    }

    // ------------------------------------------------------------------ checks
    void Check(Ctx& c, bool ok, std::string const& what)
    {
        ++c.checks;
        if (!ok)
            c.fails.push_back(what);
    }

    void CheckEq(Ctx& c, double got, double want, std::string const& what, double tol = 0.01)
    {
        Check(c, std::fabs(got - want) <= tol, Acore::StringFormat("{}: got {}, want {}", what, got, want));
    }

    bool Saw(std::string const& needle)
    {
        for (std::string const& m : sMsgs)
            if (m.find(needle) != std::string::npos)
                return true;
        return false;
    }

    std::vector<std::string> Split(std::string const& s, char sep)
    {
        std::vector<std::string> out(1);
        for (char ch : s)
            if (ch == sep)
                out.emplace_back();
            else
                out.back() += ch;
        return out;
    }

    // ------------------------------------------------------------------ the actors
    Player* Bot(Ctx& c, std::size_t i = 0)
    {
        Player* p = i < c.bots.size() ? ObjectAccessor::FindPlayer(c.bots[i]) : nullptr;
        if (!p)
            throw std::runtime_error("bot is gone");
        return p;
    }

    Creature* Mob(Ctx& c, std::size_t i = 0)
    {
        Player* p = Bot(c);
        Creature* m = i < c.mobs.size() ? ObjectAccessor::GetCreature(*p, c.mobs[i]) : nullptr;
        if (!m)
            throw std::runtime_error(Acore::StringFormat("test creature {} is gone", i));
        return m;
    }

    Combatant* BotC(Ctx& c)
    {
        Combatant* x = CombatantOf(Bot(c)->GetGUID());
        if (!x)
            throw std::runtime_error("the bot is not in a fight");
        return x;
    }

    Combatant* MobC(Ctx& c, std::size_t i = 0)
    {
        Combatant* x = CombatantOf(Mob(c, i)->GetGUID());
        if (!x)
            throw std::runtime_error(Acore::StringFormat("test creature {} is not in a fight", i));
        return x;
    }

    Encounter* Enc(Ctx& c)
    {
        Encounter* e = EncounterOf(Bot(c)->GetGUID());
        if (!e)
            throw std::runtime_error("the bot is not in a fight");
        return e;
    }

    float Ground(Map* m, float x, float y)
    {
        float z = m->GetHeight(x, y, 50.f, true, 200.f);
        return z > INVALID_HEIGHT ? z : -58.7f;
    }

    void StripGear(Player* p)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    }

    void Equip(Player* p, uint8 slot, uint32 item)
    {
        if (p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        p->EquipNewItem((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, item, true);
    }

    struct BotOpt
    {
        Kind kind = WIZ;
        int level = 1;
        std::string sub;
        std::vector<std::string> picks;
        // somewhere other than the test flats: a spot in the real world (z = 0: the ground there)
        bool at = false;
        uint32 map = MAP_ID;
        float x = 0.f, y = 0.f, z = 0.f;
    };

    Player* MakeBot(Ctx& c, BotOpt const& o)
    {
        static char const* const names[] = { "Jigwiz", "Jigfighter", "Jigdwarf", "Jigdwarfwiz", "Jigbarb", "Jigrogue", "Jigmonk",
            "Jigcleric", "Jiglock", "Jigbard", "Jigdruid" };
        static uint8 const races[] = { RACE_GNOME, RACE_HUMAN, RACE_DWARF, RACE_DWARF, RACE_DWARF, RACE_GNOME, RACE_DWARF, RACE_GNOME,
            RACE_GNOME, RACE_GNOME, RACE_DWARF };
        static uint8 const classes[] = { CLASS_MAGE, CLASS_WARRIOR, CLASS_WARRIOR, CLASS_MAGE, CLASS_SHAMAN, ::CLASS_ROGUE,
            CLASS_DRUID, CLASS_PRIEST, CLASS_WARLOCK, CLASS_PALADIN, CLASS_HUNTER };
        static std::array<int, ABILITY_COUNT> const abil[] = {
            { { 8, 14, 14, 16, 12, 10 } }, { { 16, 14, 14, 8, 12, 10 } }, { { 16, 14, 14, 8, 12, 10 } }, { { 8, 14, 14, 16, 12, 10 } },
            { { 16, 14, 14, 8, 12, 10 } }, { { 8, 16, 14, 14, 12, 10 } }, { { 10, 16, 14, 8, 14, 10 } }, { { 14, 8, 14, 10, 16, 10 } },
            { { 8, 14, 14, 10, 12, 16 } }, { { 8, 14, 14, 10, 12, 17 } }, { { 8, 14, 14, 10, 16, 10 } } };
        WorldSession* session = new WorldSession(sAccount, std::string("DNDSELFTEST"), 0, nullptr, SEC_PLAYER, 2, 0, LOCALE_enUS,
            0, false, true, 0);
        Player* p = new Player(session);
        CharacterCreateInfo info(names[o.kind], races[o.kind], classes[o.kind], GENDER_MALE);
        if (!p->Create(sObjectMgr->GetGenerator<HighGuid::Player>().Generate(), &info))
        {
            delete p;
            delete session;
            throw std::runtime_error("Player::Create failed for the bot");
        }
        session->SetPlayer(p);
        p->GetMotionMaster()->Initialize();
        p->ResetMap();
        float bx = o.at ? o.x : HOME_X, by = o.at ? o.y : HOME_Y;
        Map* map = sMapMgr->CreateMap(o.at ? o.map : MAP_ID, p);
        map->LoadGrid(bx, by);
        float bz = o.at && o.z != 0.f ? o.z : Ground(map, bx, by);
        p->Relocate(bx, by, bz + 0.05f, 0.f);    // facing +x, where the creatures stand
        p->SetMap(map);
        ObjectAccessor::AddObject(p);
        if (!map->AddPlayerToMap(p))
        {
            ObjectAccessor::RemoveObject(p);
            throw std::runtime_error("AddPlayerToMap failed for the bot");
        }
        p->UpdatePositionData();                 // zone / indoors / floor: Relocate doesn't refresh them (it was still
                                                 // the race's start, e.g. inside Anvilmar)
        sBotSet.insert(p->GetGUID());
        sAllBotGuids.push_back(p->GetGUID().GetCounter());
        c.bots.push_back(p->GetGUID());

        // what login does for a new character, with the build fixed: the jig's standard ability scores
        LoadSheet(p);
        Sheet& s = GetSheet(p);
        s.abil = abil[o.kind];
        s.created = true;
        s.background = IsCaster(p) ? "Sage" : "Soldier";
        s.picks = o.picks;
        s.subclass.clear();
        SaveSheet(p);
        LoadProgress(p);
        LoadTreatProgress(p);
        LoadTownProgress(p);
        SetupNewCharacter(p);
        if (o.level > 1)
            p->GiveLevel(uint8(o.level));            // OnLevelUp: new spells, hit points
        s.subclass = o.sub;
        EnsureKit(p);
        StripGear(p);
        p->UpdateMaxHealth();
        LongRest(p);
        sMsgs.clear();
        return p;
    }

    void RemoveBot(ObjectGuid g)
    {
        if (Player* p = ObjectAccessor::FindPlayer(g))
        {
            AbortEncounter(p);
            CloseDialogue(p, false);
            OnPlayerLeft(p);
            WorldSession* session = p->GetSession();
            p->CleanupsBeforeDelete();
            if (Map* m = p->FindMap())
            {
                m->RemovePlayerFromMap(p, true);     // deletes the Player
                m->AfterPlayerUnlinkFromMap();
            }
            session->SetPlayer(nullptr);
            delete session;
        }
        ForgetSheet(g);
        ForgetProgress(g);
        ForgetDialogue(g);
        ForgetTreats(g);
        ForgetClasses(g);
        ForgetTavern(g);
        for (char const* table : { "dnd_character", "dnd_character_treat", "dnd_character_encounter", "dnd_character_townsfolk" })
            CharacterDatabase.Execute("DELETE FROM {} WHERE guid = {}", table, g.GetCounter());
        sBotSet.erase(g);
    }

    CreatureStats Stats(uint32 entry = HUMANOID, int ac = 10, int hp = 300)
    {
        CreatureStats s;
        s.entry = entry;
        s.ac = ac;
        s.hp = hp;
        s.speed = 9.f;
        s.melee.name = "Test Claw";
        s.melee.bonus = 4;
        s.melee.dice = Dice::Parse("1");             // its turns tickle: 1 damage, so bots never drop mid-test
        s.melee.type = DMG_BLUDGEONING;
        s.xp = 0;
        return s;
    }

    struct MobOpt
    {
        float dx = 2.f, dy = 0.f;                    // from the bot's spot (the bot faces +x)
        CreatureStats st = Stats();
    };

    Creature* Spawn(Ctx& c, MobOpt const& o)
    {
        Player* p = Bot(c);
        SetTestStats(o.st);
        Map* m = p->GetMap();
        float x = p->GetPositionX() + o.dx, y = p->GetPositionY() + o.dy;
        Position pos(x, y, Ground(m, x, y) + 0.05f, std::atan2(-o.dy, -o.dx));
        TempSummon* t = m->SummonCreature(o.st.entry, pos);
        if (!t)
            throw std::runtime_error("SummonCreature failed");
        t->AIM_Initialize(MakeCombatantAI(t));       // the encounter AI: passive, HP from the test stat block
        t->SetReactState(REACT_PASSIVE);
        t->SetFaction(35);                           // friendly until the fight starts, so it doesn't pull on sight
        c.mobs.push_back(t->GetGUID());
        return t;
    }

    void Prep(Player* p, uint32 spell)
    {
        p->RemoveAllSpellCooldown();
        if (SpellInfo const* si = sSpellMgr->GetSpellInfo(spell))
            p->GetGlobalCooldownMgr().CancelGlobalCooldown(si);
        if (p->GetMaxPower(POWER_MANA))
            p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA));
    }

    void Remember(Ctx& c)
    {
        Player* p = Bot(c);
        c.v["bothp"] = float(p->GetHealth());
        for (std::size_t i = 0; i < c.mobs.size(); ++i)
            if (Creature* m = ObjectAccessor::GetCreature(*p, c.mobs[i]))
                c.v["hp" + std::to_string(i)] = float(m->GetHealth());
    }

    int Dropped(Ctx& c, std::size_t i) { return int(c.v["hp" + std::to_string(i)]) - int(Mob(c, i)->GetHealth()); }

    void AckTeleport(Player* p)
    {
        if (!p->IsBeingTeleportedNear())
            return;
        WorldPacket ack(MSG_MOVE_TELEPORT_ACK, 16);
        ack << p->GetPackGUID();
        ack << uint32(0) << uint32(0);
        p->GetSession()->HandleMoveTeleportAck(ack);
    }

    // ------------------------------------------------------------------ script building blocks
    void Rig(Script& s, std::vector<int> q, int fallback = 0)
    {
        s.Do([q, fallback](Ctx&) { sDice.assign(q.begin(), q.end()); sFallback = fallback; });
    }

    void AddBot(Script& s, BotOpt o) { s.Do([o](Ctx& c) { MakeBot(c, o); }); }
    void AddMob(Script& s, MobOpt o) { s.Do([o](Ctx& c) { Spawn(c, o); }); }

    // a fight: every test creature against the bot, the bot first in the order (rigged initiative), its turn up
    void StartFight(Script& s, int botRoll = 20)
    {
        s.Do([botRoll](Ctx& c)
        {
            Player* p = Bot(c);
            sDice.assign({ botRoll, botRoll });
            for (std::size_t i = 0; i < c.mobs.size(); ++i)
            {
                sDice.push_back(1);
                sDice.push_back(1);
                Mob(c, i)->SetFaction(14);
            }
            sFallback = 0;
            StartEncounter(Mob(c, 0), p);
            for (std::size_t i = 1; i < c.mobs.size(); ++i)
                AddCreatureToFight(Mob(c, i), p);
            sDice.clear();
            Check(c, InCombat(p), "the fight started");
        });
        s.Until([](Ctx& c) { return IsTurnOf(Bot(c)); }, "the bot's first turn", 6000);
    }

    // cast at a creature (target >= 0), at the bot itself (-1)
    void Cast(Script& s, uint32 spell, int target = 0)
    {
        s.Do([spell, target](Ctx& c)
        {
            Player* p = Bot(c);
            Unit* t = target < 0 ? static_cast<Unit*>(p) : static_cast<Unit*>(Mob(c, std::size_t(target)));
            if (t != p)
                p->SetOrientation(p->GetAngle(t));
            Remember(c);
            sMsgs.clear();
            Prep(p, spell);
            c.v["cast"] = float(p->CastSpell(t, spell, false));
        });
        s.Until([](Ctx& c) { return !Bot(c)->IsNonMeleeSpellCast(true, false, true, false, false); }, "the cast to finish", 8000);
        s.Wait(700);
    }

    // a ground-targeted cast at an offset from the bot
    void CastAt(Script& s, uint32 spell, float dx, float dy)
    {
        s.Do([spell, dx, dy](Ctx& c)
        {
            Player* p = Bot(c);
            float x = p->GetPositionX() + dx, y = p->GetPositionY() + dy;
            c.v["fromx"] = p->GetPositionX();
            c.v["fromy"] = p->GetPositionY();
            c.v["tox"] = x;
            c.v["toy"] = y;
            p->SetOrientation(std::atan2(dy, dx));
            Remember(c);
            sMsgs.clear();
            Prep(p, spell);
            c.v["cast"] = float(p->CastSpell(x, y, Ground(p->GetMap(), x, y), spell, false));
        });
        s.Until([](Ctx& c) { return !Bot(c)->IsNonMeleeSpellCast(true, false, true, false, false); }, "the cast to finish", 8000);
        s.Wait(700);
    }

    // End Turn, let the creatures act, and wait for the bot's next turn (the dice go back to random unless keepRig)
    void NextTurn(Script& s, bool keepRig = false)
    {
        if (!keepRig)
            Rig(s, {}, 0);
        s.Do([](Ctx& c)
        {
            Player* p = Bot(c);
            Prep(p, SPELL_END_TURN);
            p->CastSpell(p, SPELL_END_TURN, false);
        });
        s.Until([](Ctx& c) { return !IsTurnOf(Bot(c)); }, "the turn to pass", 3000);
        s.Until([](Ctx& c) { return IsTurnOf(Bot(c)); }, "the bot's next turn", 30000);
    }

    void Expect(Script& s, std::string what, Cond f) { s.Do([what, f](Ctx& c) { Check(c, f(c), what); }); }

    void ExpectEq(Script& s, std::string what, std::function<double(Ctx&)> got, double want, double tol = 0.01)
    {
        s.Do([what, got, want, tol](Ctx& c) { CheckEq(c, got(c), want, what, tol); });
    }

    void ExpectDrop(Script& s, int want, std::size_t mob = 0, std::string what = "")
    {
        std::string w = what.empty() ? Acore::StringFormat("damage to creature {}", mob) : what;
        s.Do([want, mob, w](Ctx& c) { CheckEq(c, Dropped(c, mob), want, w); });
    }

    void ExpectSaw(Script& s, std::string needle)
    {
        s.Do([needle](Ctx& c) { Check(c, Saw(needle), "expected to see \"" + needle + "\""); });
    }

    void ExpectNotSaw(Script& s, std::string needle)
    {
        s.Do([needle](Ctx& c) { Check(c, !Saw(needle), "did not expect \"" + needle + "\""); });
    }

    void ExpectRefused(Script& s, std::string needle)
    {
        s.Do([needle](Ctx& c)
        {
            Check(c, int(c.v["cast"]) != SPELL_CAST_OK, "the cast should have been refused");
            Check(c, Saw(needle), "expected the refusal \"" + needle + "\"");
        });
    }

    void ExpectCastOk(Script& s)
    {
        s.Do([](Ctx& c)
        {
            int r = int(c.v["cast"]);
            std::string err;
            for (std::string const& m : sMsgs)
                if (m.rfind("X^", 0) == 0)
                    err = m.substr(2);
            Check(c, r == SPELL_CAST_OK, Acore::StringFormat("the cast was refused (result {}{})", r, err.empty() ? "" : ": " + err));
        });
    }

    // the "RS^target^ability^what^d20^bonus^dc^saved^adv^isPlayer" roll the addon gets for a save
    void ExpectSaveAdv(Script& s, std::string what, int adv)
    {
        s.Do([what, adv](Ctx& c)
        {
            for (std::string const& m : sMsgs)
                if (m.rfind("RS^", 0) == 0)
                {
                    std::vector<std::string> f = Split(m, '^');
                    if (f.size() > 8 && f[3] == what)
                    {
                        CheckEq(c, std::atoi(f[8].c_str()), adv, what + " save advantage flag");
                        return;
                    }
                }
            Check(c, false, "no " + what + " save was rolled");
        });
    }

    void Act(Script& s, Fn f) { s.Do(std::move(f)); }

    // ------------------------------------------------------------------ multiplayer helpers
    // what one bot was sent since the last ClearMsgs
    bool SawBy(Player* p, std::string const& needle)
    {
        for (std::string const& m : sMsgsOf[p->GetGUID()])
            if (m.find(needle) != std::string::npos)
                return true;
        return false;
    }

    std::vector<std::string> MsgsBy(Player* p, std::string const& prefix)
    {
        std::vector<std::string> out;
        for (std::string const& m : sMsgsOf[p->GetGUID()])
            if (m.rfind(prefix, 0) == 0)
                out.push_back(m);
        return out;
    }

    bool GotExact(Player* p, std::string const& msg)
    {
        std::vector<std::string> const& v = sMsgsOf[p->GetGUID()];
        return std::find(v.begin(), v.end(), msg) != v.end();
    }

    void ClearMsgs()
    {
        sMsgs.clear();
        sMsgsOf.clear();
    }

    // one more bot, on the ground at (x, y) on the first bot's map
    Player* AddBotAt(Ctx& c, Kind kind, int level, float x, float y)
    {
        Player* first = Bot(c);
        Map* m = first->GetMap();
        m->LoadGrid(x, y);
        float z = m->GetHeight(x, y, first->GetPositionZ() + 10.f, true, 60.f);
        BotOpt o{ kind, level };
        o.at = true;
        o.map = first->GetMapId();
        o.x = x;
        o.y = y;
        o.z = z > INVALID_HEIGHT ? z : first->GetPositionZ();
        return MakeBot(c, o);
    }

    // walk a bot somewhere (a near teleport, acknowledged), and tell the engine it moved
    void MoveBotTo(Player* p, float x, float y)
    {
        float z = p->GetMap()->GetHeight(x, y, p->GetPositionZ() + 10.f, true, 60.f);
        p->NearTeleportTo(x, y, z > INVALID_HEIGHT ? z : p->GetPositionZ(), p->GetOrientation());
        AckTeleport(p);
        OnPlayerMoved(p, p->GetPosition());
    }

    // bot i casts at a unit; the result goes in c.v["cast"]
    void CastAs(Ctx& c, std::size_t bot, uint32 spell, Unit* t)
    {
        Player* p = Bot(c, bot);
        if (t != p)
            p->SetOrientation(p->GetAngle(t));
        Remember(c);
        ClearMsgs();
        Prep(p, spell);
        c.v["cast"] = float(p->CastSpell(t, spell, false));
    }

    // a spell the character has learned is one its class, level, subclass and picks really give
    bool KitOk(Player* p, uint32 id)
    {
        for (uint32 w : WizardKit())
            if (w == id)
                return true;                 // (which wizard spells a class knows is SpellKnown's business)
        return KitHas(p, id);
    }

    // ------------------------------------------------------------------ world suites: helpers
    std::set<std::string> sChosen;               // a dialogue walk: the options already taken (by their text)

    // a spot r metres from (x, y) for a bot: on real ground near that height, as clear of every other encounter's
    // aggro range as it can be
    bool SpotNear(Map* map, uint32 defId, float x, float y, float z, float r, float& ox, float& oy, float& oz)
    {
        float best = -1e9f;
        bool found = false;
        for (int k = 0; k < 16; ++k)
        {
            float a = float(k) * float(M_PI) / 8.f;
            float px = x + std::cos(a) * r, py = y + std::sin(a) * r;
            float h = map->GetHeight(px, py, z + 8.f, true, 30.f);
            if (h <= INVALID_HEIGHT || std::fabs(h - z) > 8.f)
                continue;
            float clear = 1000.f;
            for (EncounterDef const* o : AllEncounters())
                if (o->id != defId)
                    for (uint32 spawn : o->spawns)
                        if (CreatureData const* d = sObjectMgr->GetCreatureData(spawn))
                            if (d->mapid == map->GetId())
                                clear = std::min(clear, std::hypot(d->posX - px, d->posY - py) - o->aggroRange);
            if (clear > best)
            {
                best = clear;
                ox = px;
                oy = py;
                oz = h;
                found = true;
            }
        }
        return found;
    }

    bool Skipped(Ctx& c) { return c.v.count("skip") != 0; }

    // a tough bot (a level 6 dwarf fighter nothing out there can kill) next to a real encounter; its creatures, alive,
    // in c.mobs. Skips (passes, with a note) when a real player is close enough to be dragged in.
    void WorldSetup(Script& s, uint32 id, float dist, Kind kind = DWARF, int level = 6)
    {
        s.Do([id, dist, kind, level](Ctx& c)
        {
            EncounterDef const* def = GetEncounterDef(id);
            if (!def || def->spawns.empty())
                throw std::runtime_error("the encounter has no spawns");
            CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front());
            if (!d)
                throw std::runtime_error("its first spawn is not in the creature table");
            Map* map = sMapMgr->CreateBaseMap(d->mapid);
            map->LoadGrid(d->posX, d->posY);
            bool busy = false;
            map->DoForAllPlayers([&](Player* pl)
            {
                if (!sBotSet.count(pl->GetGUID()) && pl->GetExactDist2d(d->posX, d->posY) < 200.f)
                    busy = true;
            });
            if (busy)
            {
                c.v["skip"] = 1;
                Out(Acore::StringFormat("      (skipped: a player is near {})", def->name));
                return;
            }
            float x = 0, y = 0, z = 0;
            if (!SpotNear(map, id, d->posX, d->posY, d->posZ, dist > 0 ? dist : def->aggroRange + 8.f, x, y, z))
                throw std::runtime_error("no ground to stand on near it");
            BotOpt o = kind == DWARF ? BotOpt{ DWARF, 6, "Champion", { "style:Defense" } } : BotOpt{ kind, level };
            o.at = true;
            o.map = d->mapid;
            o.x = x;
            o.y = y;
            o.z = z;
            Player* p = MakeBot(c, o);
            GetSheet(p).tempHp = 100000;
            c.v["def"] = float(id);
        });
        s.Wait(2000);
        // the whole group must be standing there: a talk that ended in peace sends them walking off, and they are
        // only re-created a little after being asked for
        s.Until([id](Ctx& c)
        {
            if (Skipped(c))
                return true;
            EncounterDef const* def = GetEncounterDef(id);
            Map* map = Bot(c)->GetMap();
            bool all = true;
            bool ask = int(c.v["asked"]++) % 40 == 0;         // every couple of seconds
            for (uint32 spawn : def->spawns)
            {
                bool alive = false;
                auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
                for (auto itr = range.first; itr != range.second; ++itr)
                    alive = alive || (itr->second->IsAlive() && itr->second->IsInWorld() && !IsLeaving(itr->second->GetGUID()));
                if (!alive)
                {
                    all = false;
                    if (ask)
                        RespawnSpawn(map, spawn);
                }
            }
            return all;
        }, "the encounter's creatures to be in the world", 40000);
        s.Wait(500);
        s.Do([id](Ctx& c)
        {
            if (Skipped(c))
                return;
            EncounterDef const* def = GetEncounterDef(id);
            Map* map = Bot(c)->GetMap();
            int n = 0;
            for (uint32 spawn : def->spawns)
            {
                ++n;
                Creature* found = nullptr;
                auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
                for (auto itr = range.first; itr != range.second; ++itr)
                    if (itr->second->IsAlive() && itr->second->IsInWorld())
                        found = itr->second;
                Check(c, found != nullptr, Acore::StringFormat("spawn {} (creature.guid {}) is not in the world", n, spawn));
                if (!found)
                    continue;
                c.mobs.push_back(found->GetGUID());
                Check(c, GetStats(found->GetEntry()) != nullptr, Acore::StringFormat("{} has no D&D stat block", found->GetName()));
            }
            if (c.mobs.empty())
                throw std::runtime_error("none of its creatures are in the world");
        });
    }

    // the real encounter, fought: every creature must finish its turn (two rounds), and winning must pay out
    void WorldFight(Script& s, uint32 id)
    {
        WorldSetup(s, id, 0.f);
        s.Do([](Ctx& c)
        {
            if (Skipped(c))
                return;
            Player* p = Bot(c);
            if (!InCombat(p))                    // (a spread-out group may have spotted the bot already)
                StartEncounter(Mob(c, 0), p);
            Check(c, InCombat(p), "the fight started");
            if (InCombat(p))
                CheckEq(c, double(Enc(c)->order.size()), double(c.mobs.size() + 1), "the whole group joined the fight");
        });
        for (int round = 1; round <= 2; ++round)
        {
            s.Until([](Ctx& c) { return Skipped(c) || !InCombat(Bot(c)) || IsTurnOf(Bot(c)); },
                Acore::StringFormat("the bot's turn in round {} (a creature's turn never ended)", round), 150000);
            s.Do([round](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* p = Bot(c);
                Check(c, InCombat(p), Acore::StringFormat("still fighting in round {}", round));
                Check(c, p->IsAlive(), "the bot is alive");
                if (IsTurnOf(p))
                    EndTurn(p);
            });
            s.Until([](Ctx& c) { return Skipped(c) || !InCombat(Bot(c)) || !IsTurnOf(Bot(c)); }, "the bot's turn to pass", 5000);
        }
        s.Until([](Ctx& c) { return Skipped(c) || !InCombat(Bot(c)) || IsTurnOf(Bot(c)); },
            "the bot's turn after two rounds (a creature's turn never ended)", 150000);
        s.Do([id](Ctx& c)
        {
            if (Skipped(c))
                return;
            Player* p = Bot(c);
            sMsgs.clear();
            std::vector<ObjectGuid> foes(c.mobs.begin(), c.mobs.end());
            if (Encounter* e = Enc(c))           // and whatever joined since (a lair's call)
                for (Combatant const& x : e->order)
                    if (!x.player && std::find(foes.begin(), foes.end(), x.guid) == foes.end())
                        foes.push_back(x.guid);
            for (std::size_t i = 0; i < foes.size(); ++i)
                if (Creature* m = ObjectAccessor::GetCreature(*p, foes[i]))
                    for (DmgType type : { DMG_FORCE, DMG_BLUDGEONING, DMG_FIRE, DMG_PSYCHIC })
                    {
                        if (!m->IsAlive())
                            break;
                        DamageCtx d;
                        d.attacker = p;
                        d.victim = m;
                        d.amount = 1000000;
                        d.type = type;
                        ApplyDamage(d);
                    }
            (void)id;
        });
        s.Until([](Ctx& c) { return Skipped(c) || !InCombat(Bot(c)); }, "the fight to end once every creature is dead", 15000);
        s.Do([id](Ctx& c)
        {
            if (Skipped(c))
                return;
            EncounterDef const* def = GetEncounterDef(id);
            Player* p = Bot(c);
            Check(c, Saw("V^VICTORY"), "the victory banner");
            Check(c, HasCleared(p, id), "the encounter counts as cleared");
            if (def->xp)
                Check(c, Saw(Acore::StringFormat("+{} XP", def->xp)), Acore::StringFormat("its {} XP was paid", def->xp));
            Check(c, p->IsAlive() && !p->HasUnitState(UNIT_STATE_ROOT), "the bot is free to walk on");
        });
    }


    // ------------------------------------------------------------------ the campaign (selftest.py campaign)
    // Two level-1 adventurers - a fighter and a wizard - play the built game in order: every encounter, zone by zone (by
    // level), fought with a simple turn policy (into reach or range, attack; a potion or Second Wind when low; Help or a
    // thrown potion for a partner who is down), level-up choices taken, a rest between fights when hurt, wipes retried
    // (a wipe is a reload at the graveyard; three and it gives up on that one), a death raised by Brother Marrow. After
    // every encounter the invariants are checked. The report is numbers only (encounter ids, zone ids): no spoilers.
    struct Campaign
    {
        std::vector<uint32> order;               // encounter ids, in playing order
        std::size_t next = 0;
        uint32 cur = 0;                          // the encounter being fought (0: between fights)
        int wipes = 0;
        uint32 fightStart = 0, lastAct[2] = { 0, 0 }, lastTurnKey[2] = { 0, 0 }, placeAsk = 0, startedAt = 0;
        std::string refusal[2];              // why this turn's last action was refused (logged if the bot then passes)
        int turnActs[2] = { 0, 0 }, potionsUsed[2] = { 0, 0 };
        uint32 expectedXp = 0;                   // what each of them should have earned so far
        std::set<uint32> cleared;
        int won = 0, gaveUp = 0, skipped = 0, deaths = 0, downs = 0, totalWipes = 0;
        uint32 lastZone = 0;
        uint32 side = 0;                         // a fight they were drawn into (a group nearby pulled them): its encounter
        int sideFights = 0;
    };
    Campaign sCamp;

    float CampaignHealth(Player* p) { return p->IsAlive() ? p->GetHealthPct() : 0.f; }

    uint32 TotalXp(Player* p)
    {
        uint32 total = p->GetUInt32Value(PLAYER_XP);
        for (uint8 l = 1; l < p->GetLevel(); ++l)
            total += sObjectMgr->GetXPForLevel(l);
        return total;
    }

    // a near teleport, acknowledged as a client would, after loading the ground there
    void CampaignPut(Player* p, float x, float y, float z, float o, float above = 5.f)
    {
        p->GetMap()->LoadGrid(x, y);
        float gz = p->GetMap()->GetHeight(x, y, z + above, true, 50.f);
        p->NearTeleportTo(x, y, gz > INVALID_HEIGHT ? gz + 0.05f : z, o);
        AckTeleport(p);
    }

    // the level-up choices a player would click: Defense and Champion for the fighter, Evocation for the wizard, and
    // +2 to their main ability
    void CampaignPicks(Player* p)
    {
        for (int i = 0; i < 6; ++i)
        {
            sMsgsOf[p->GetGUID()].clear();
            PushPendingChoices(p);
            std::vector<std::string> lp = MsgsBy(p, "LP^");
            std::string kind = lp.empty() ? "" : lp.back().substr(3);
            bool fighter = IsFighter(p);
            if (kind == "style")
                HandlePick(p, "style^Defense");
            else if (kind == "feat1")
                HandlePick(p, "feat1^Tough");
            else if (kind == "subclass")
                HandlePick(p, fighter ? "subclass^Champion" : "subclass^Evocation");
            else if (kind == "asi")
                HandlePick(p, fighter ? "asi^STR" : "asi^INT");
            else
                return;
        }
    }

    bool HasPotion(Player* p) { return p->GetItemCount(118, false) + p->GetItemCount(858, false) > 0; }

    // a potion used as a player would: the item goes, its spell heals (thrown to whoever is selected, else drunk)
    void CampaignPotion(Player* p, int who, Unit* to)
    {
        bool greater = p->GetItemCount(858, false) > 0;
        p->DestroyItemCount(greater ? 858 : 118, 1, true);
        p->SetSelection(to ? to->GetGUID() : p->GetGUID());
        Prep(p, greater ? 440 : 439);
        p->CastSpell(p, greater ? 440 : 439, false);
        ++sCamp.potionsUsed[who];
    }

    // a step toward a point (in this turn's movement), stopping `stop` metres short
    bool CampaignStep(Player* p, Unit* to, float stop)
    {
        Combatant* me = CombatantOf(p->GetGUID());
        if (!me)
            return false;
        float left = me->moveMax - me->moved, d = std::max(0.1f, p->GetExactDist2d(to));
        float want = p->GetExactDist(to) - stop;
        if (left < 0.5f || want < 0.3f)
            return false;
        float step = std::min(want, left - 0.2f);
        // walk it as a player would: along the navmesh path, so a hop never lands on a ledge or rock the creatures can't
        // path to (a straight-line hop with the ground found from 5 m above did - the fight stalled, neither side in reach)
        PathGenerator path(p);
        if (path.CalculatePath(to->GetPositionX(), to->GetPositionY(), to->GetPositionZ()) &&
            !(path.GetPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE)) && path.GetPath().size() >= 2)
        {
            Movement::PointsArray const& pts = path.GetPath();
            float total = 0.f;
            for (std::size_t i = 1; i < pts.size(); ++i)
                total += (pts[i] - pts[i - 1]).length();
            float go = std::min(left - 0.2f, total - stop);
            if (go < 0.3f)
                return false;
            G3D::Vector3 at = pts[0];
            for (std::size_t i = 1; i < pts.size() && go > 0.f; ++i)
            {
                float seg = (pts[i] - pts[i - 1]).length();
                if (seg >= go)
                {
                    at = pts[i - 1] + (pts[i] - pts[i - 1]) * (go / seg);
                    go = 0.f;
                }
                else
                {
                    at = pts[i];
                    go -= seg;
                }
            }
            CampaignPut(p, at.x, at.y, at.z, p->GetAngle(to), 1.f);
            OnPlayerMoved(p, p->GetPosition());
            return true;
        }
        float x = p->GetPositionX() + (to->GetPositionX() - p->GetPositionX()) / d * step;
        float y = p->GetPositionY() + (to->GetPositionY() - p->GetPositionY()) / d * step;
        CampaignPut(p, x, y, p->GetPositionZ(), p->GetAngle(to), 1.f);
        OnPlayerMoved(p, p->GetPosition());
        return true;
    }

    Unit* CampaignNearestEnemy(Encounter* e, Player* p)
    {
        Unit* best = nullptr;
        for (Combatant const& c : e->order)
            if (!c.player && !c.gone && c.alive)
                if (Unit* u = e->GetUnit(c); u && u->IsAlive() && (!best || p->GetExactDist(u) < p->GetExactDist(best)))
                    best = u;
        return best;
    }

    int CampaignEnemiesNear(Encounter* e, Unit* at, float r)
    {
        int n = 0;
        for (Combatant const& c : e->order)
            if (!c.player && !c.gone && c.alive)
                if (Unit* u = e->GetUnit(c); u && u->IsAlive() && u->GetExactDist(at) <= r)
                    ++n;
        return n;
    }

    // one action of a bot's turn (one a tick, each after the last has played out)
    void CampaignTurn(int who, Player* p, Player* partner)
    {
        Encounter* e = EncounterOf(p->GetGUID());
        Combatant* me = e ? e->Find(p->GetGUID()) : nullptr;
        uint32 now = getMSTime();
        if (!me || p->IsNonMeleeSpellCast(true, false, true, false, false) || getMSTimeDiff(sCamp.lastAct[who], now) < 900)
            return;
        sCamp.lastAct[who] = now;
        uint32 key = uint32(e->round) * 256u + uint32(e->cur + 1);
        if (key != sCamp.lastTurnKey[who])
        {
            sCamp.lastTurnKey[who] = key;
            sCamp.turnActs[who] = 0;
        }
        auto cast = [&](uint32 spell, Unit* t)
        {
            Prep(p, spell);
            std::size_t seen = sMsgsOf[p->GetGUID()].size();
            SpellCastResult r = p->CastSpell(t, spell, false);
            if (r != SPELL_CAST_OK)
            {
                std::string why;
                std::vector<std::string> const& msgs = sMsgsOf[p->GetGUID()];
                for (std::size_t i = seen; i < msgs.size(); ++i)
                    if (msgs[i].rfind("X^", 0) == 0)
                        why = msgs[i].substr(2);
                SpellInfo const* si = sSpellMgr->GetSpellInfo(spell);
                sCamp.refusal[who] = Acore::StringFormat("{} refused (result {}{}{})", si ? si->SpellName[0] : "?", int(r),
                    why.empty() ? "" : ": ", why);
            }
            return r == SPELL_CAST_OK;
        };
        if (++sCamp.turnActs[who] > 8 || IsDowned(p))
        {
            EndTurn(p);
            return;
        }
        // a partner who is down: a hand up, or a potion thrown, or get over there
        if (partner && IsDowned(partner))
        {
            float d = p->GetExactDist(partner);
            if (!me->actionUsed && d <= 2.f && cast(SPELL_HELP, partner))
                return;
            if (!me->bonusUsed && HasPotion(p) && d <= 18.f && p->IsWithinLOSInMap(partner))
            {
                CampaignPotion(p, who, partner);
                return;
            }
            if (!me->actionUsed && d > 2.f && CampaignStep(p, partner, 1.5f))
                return;
        }
        // hurt: a potion, or Second Wind
        if (!me->bonusUsed && CampaignHealth(p) < 40.f && HasPotion(p))
        {
            CampaignPotion(p, who, nullptr);
            return;
        }
        if (IsFighter(p) && !me->bonusUsed && CampaignHealth(p) < 50.f && !GetSheet(p).secondWindUsed && cast(SPELL_SECOND_WIND, p))
            return;
        Unit* t = CampaignNearestEnemy(e, p);
        if (t && (!me->actionUsed || me->extraAttacks > 0))
        {
            float d = p->GetExactDist(t);
            if (IsFighter(p))
            {
                float reach = MeleeRange(p, t);
                if (d > reach)
                {
                    if (CampaignStep(p, t, std::max(1.f, reach - 0.6f)))
                        return;
                    if (d <= 30.f && p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED) && cast(SPELL_RANGED_ATTACK, t))
                        return;
                }
                else if (cast(SPELL_ATTACK, t))
                    return;
            }
            else
            {
                if (d > 17.f && CampaignStep(p, t, 15.f))
                    return;
                uint32 spell = SPELL_FIRE_BOLT;
                if (Level(p) >= 5 && SlotsLeft(p, 3) > 0 && CampaignEnemiesNear(e, t, 6.f) >= 2)
                    spell = SPELL_FIREBALL;
                else if (Level(p) >= 3 && SlotsLeft(p, 2) > 0 && t->GetHealth() > 15)
                    spell = SPELL_SCORCHING_RAY;
                if (d <= 17.5f && (cast(spell, t) || (spell != SPELL_FIRE_BOLT && cast(SPELL_FIRE_BOLT, t))))
                    return;
            }
        }
        if (!me->actionUsed)
        {
            // passing with the Action unused: say why in the dice log (a stalled fight explains itself)
            std::string why = sCamp.refusal[who].empty() ? std::string("nothing tried") : sCamp.refusal[who];
            if (t)
                why = Acore::StringFormat("nearest foe {} at {:.1f} m, {}line of sight, {:.1f} m of movement left; {}", t->GetName(),
                    p->GetExactDist(t), p->IsWithinLOSInMap(t) ? "" : "no ", me->moveMax - me->moved, why);
            else
                why = "no foe found in the fight; " + why;
            Log(p, "[campaign] passes: " + why);
        }
        sCamp.refusal[who].clear();
        EndTurn(p);
    }

    // after an encounter is over: what must be true of both of them and of the world
    void CampaignCheck(Ctx& c, uint32 id)
    {
        for (std::size_t i = 0; i < 2; ++i)
        {
            Player* p = Bot(c, i);
            std::string who = Acore::StringFormat("[{}] {}", id, p->GetName());
            Check(c, !InCombat(p), who + ": out of the fight");
            Check(c, WatchedTalker(p).IsEmpty(), who + ": not watching a talk");
            Check(c, !InDialogue(p), who + ": not in a talk");
            if (p->IsAlive() && !IsDowned(p))
            {
                Check(c, !p->HasUnitState(UNIT_STATE_ROOT), who + ": free to move");
                Check(c, !p->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH), who + ": standing");
            }
            uint32 owed = 0;
            for (EncounterDef const* def : AllEncounters())
                if (HasCleared(p, def->id))
                    owed += def->xp;
            if (p->GetLevel() < MAX_DND_LEVEL)
                CheckEq(c, TotalXp(p), owed, who + ": XP earned matches the encounters cleared");
        }
    }

    bool CampaignTick(Ctx& c)
    {
        Player* a = Bot(c, 0);
        Player* b = Bot(c, 1);
        Player* bots[2] = { a, b };
        uint32 now = getMSTime();
        for (Player* p : bots)
            AckTeleport(p);

        if (Encounter* fe = EncounterOf(a->GetGUID()) ? EncounterOf(a->GetGUID()) : EncounterOf(b->GetGUID()))
        {
            if (!sCamp.cur && !sCamp.side)
            {
                sCamp.side = fe->defId ? fe->defId : 1;
                sCamp.fightStart = now;
            }
            if (getMSTimeDiff(sCamp.fightStart, now) > 10 * 60 * 1000)
            {
                Check(c, false, Acore::StringFormat("[{}] the fight took over 10 minutes", sCamp.cur ? sCamp.cur : sCamp.side));
                Out(Acore::StringFormat("      [{:>3}] stalled: over 10 minutes, called off (the fight's state is in the dice log)",
                    sCamp.cur ? sCamp.cur : sCamp.side));
                {
                    std::string dump = FightDump();
                    std::size_t at = 0;
                    while (at < dump.size())
                    {
                        std::size_t nl = dump.find('\n', at);
                        if (nl == std::string::npos)
                            nl = dump.size();
                        Log(a, "[campaign] stalled: " + dump.substr(at, nl - at));
                        at = nl + 1;
                    }
                }
                AbortEncounter(a);
                AbortEncounter(b);
                if (sCamp.cur)
                    ++sCamp.next;
                sCamp.cur = sCamp.side = 0;
                return false;
            }
            for (int i = 0; i < 2; ++i)
                if (IsTurnOf(bots[i]))
                    CampaignTurn(i, bots[i], bots[1 - i]);
            return false;
        }
        if (sCamp.side)
        {
            ++sCamp.sideFights;
            Out(Acore::StringFormat("      [{:>3}] drawn into it on the way (levels {}/{}, HP {:.0f}%/{:.0f}%){}", sCamp.side,
                a->GetLevel(), b->GetLevel(), CampaignHealth(a), CampaignHealth(b), a->IsAlive() && b->IsAlive() ? "" : ", one dead"));
            CampaignCheck(c, sCamp.side);
            sCamp.side = 0;
            return false;
        }
        if (sCamp.cur)
        {
            EncounterDef const* def = GetEncounterDef(sCamp.cur);
            // the fight is over: won (every member dead) or a wipe (they woke at the graveyard)
            bool allDead = true;
            for (uint32 spawn : def->spawns)
            {
                auto range = a->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
                for (auto itr = range.first; itr != range.second; ++itr)
                    allDead = allDead && !itr->second->IsAlive();
            }
            uint32 secs = getMSTimeDiff(sCamp.fightStart, now) / 1000;
            if (allDead)
            {
                if (!sCamp.cleared.count(sCamp.cur))
                {
                    sCamp.cleared.insert(sCamp.cur);
                    sCamp.expectedXp += def->xp;
                }
                ++sCamp.won;
                for (Player* p : bots)
                {
                    sCamp.downs += IsDowned(p) ? 1 : 0;
                    sCamp.deaths += p->IsAlive() ? 0 : 1;
                }
                Out(Acore::StringFormat("      [{:>3}] zone {:>2}: won in {:>3}s after {} wipe(s); levels {}/{}, HP {:.0f}%/{:.0f}%{}{}",
                    sCamp.cur, def->zoneId, secs, sCamp.wipes, a->GetLevel(), b->GetLevel(), CampaignHealth(a), CampaignHealth(b),
                    IsDowned(a) || IsDowned(b) ? ", one down" : "", !a->IsAlive() || !b->IsAlive() ? ", one dead" : ""));
                CampaignCheck(c, sCamp.cur);
                sCamp.cur = 0;
                sCamp.wipes = 0;
                ++sCamp.next;
            }
            else
            {
                ++sCamp.wipes;
                ++sCamp.totalWipes;
                if (sCamp.wipes >= 3)
                {
                    Out(Acore::StringFormat("      [{:>3}] zone {:>2}: GAVE UP after 3 wipes (levels {}/{})", sCamp.cur, def->zoneId,
                        a->GetLevel(), b->GetLevel()));
                    ++sCamp.gaveUp;
                    sCamp.wipes = 0;
                    ++sCamp.next;
                }
                sCamp.cur = 0;               // (the same one again, unless it gave up)
            }
            return false;
        }

        // ---- between fights: anyone down gets a hand up, anyone dead is raised (the other pays Brother Marrow)
        for (int i = 0; i < 2; ++i)
        {
            Player* p = bots[i];
            Player* o = bots[1 - i];
            if (IsDowned(p) && o->IsAlive() && !IsDowned(o))
            {
                CampaignPut(o, p->GetPositionX() + 1.2f, p->GetPositionY(), p->GetPositionZ(), o->GetAngle(p));
                Prep(o, SPELL_HELP);
                o->CastSpell(p, SPELL_HELP, false);
                Check(c, !IsDowned(p), Acore::StringFormat("{} is helped up after the fight", p->GetName()));
                return false;
            }
            if (!p->IsAlive() && o->IsAlive())
            {
                Position at(o->GetPositionX() + 3.f, o->GetPositionY(), o->GetPositionZ(), 0.f);
                if (TempSummon* m = o->GetMap()->SummonCreature(6491, at))
                {
                    if (!o->HasEnoughMoney(RaiseCost(p)))
                        o->ModifyMoney(int32(RaiseCost(p)));    // (no shops yet: they'd have earned it)
                    Check(c, MarrowRaise(m, p, o), Acore::StringFormat("{} is raised by Brother Marrow", p->GetName()));
                    AckTeleport(p);
                    m->DespawnOrUnsummon(1s);
                }
                return false;
            }
        }
        for (Player* p : bots)
            CampaignPicks(p);
        if (sCamp.next >= sCamp.order.size())
        {
            uint32 mins = getMSTimeDiff(sCamp.startedAt, now) / 60000;
            Out(Acore::StringFormat("      campaign: {} won, {} given up, {} skipped, {} side fights; {} wipes, {} downs, {} deaths; "
                "levels {}/{}; potions {}/{}; {} minutes", sCamp.won, sCamp.gaveUp, sCamp.skipped, sCamp.sideFights, sCamp.totalWipes,
                sCamp.downs, sCamp.deaths, a->GetLevel(), b->GetLevel(), sCamp.potionsUsed[0], sCamp.potionsUsed[1], mins));
            return true;
        }

        uint32 id = sCamp.order[sCamp.next];
        EncounterDef const* def = GetEncounterDef(id);
        CreatureData const* d = def && !def->spawns.empty() ? sObjectMgr->GetCreatureData(def->spawns.front()) : nullptr;
        if (!d || d->mapid != a->GetMapId())
        {
            ++sCamp.skipped;
            ++sCamp.next;
            return false;
        }
        // a new zone: a rest at its inn, and potions bought for it (no shops in the run yet)
        if (def->zoneId != sCamp.lastZone)
        {
            sCamp.lastZone = def->zoneId;
            for (Player* p : bots)
            {
                LongRest(p);
                if (p->GetItemCount(118, false) < 3)
                    p->AddItem(118, 3 - p->GetItemCount(118, false));
            }
        }
        // hurt or spent: rest (an inn, a camp)
        for (Player* p : bots)
            if (CampaignHealth(p) < 60.f || SlotsLeft(p, 1) == 0)
                LongRest(p);
        // a real player near it: leave them be
        bool busy = false;
        a->GetMap()->DoForAllPlayers([&](Player* pl)
        {
            if (!sBotSet.count(pl->GetGUID()) && pl->GetExactDist2d(d->posX, d->posY) < 200.f)
                busy = true;
        });
        if (busy)
        {
            ++sCamp.skipped;
            ++sCamp.next;
            Out(Acore::StringFormat("      [{:>3}] skipped: a player is near", id));
            return false;
        }
        // the whole group standing (the fallen of a wipe come back a little later)
        a->GetMap()->LoadGrid(d->posX, d->posY);
        bool all = true;
        Creature* leader = nullptr;
        for (uint32 spawn : def->spawns)
        {
            bool alive = false;
            auto range = a->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawn);
            for (auto itr = range.first; itr != range.second; ++itr)
                if (itr->second->IsAlive() && itr->second->IsInWorld() && !InCombat(itr->second))
                {
                    alive = true;
                    if (!leader)
                        leader = itr->second;
                }
            if (!alive)
            {
                all = false;
                if (getMSTimeDiff(sCamp.placeAsk, now) > 2000)
                    RespawnSpawn(a->GetMap(), spawn);
            }
        }
        if (!all || !leader)
        {
            if (getMSTimeDiff(sCamp.placeAsk, now) > 2000)
                sCamp.placeAsk = now;
            return false;
        }
        float x = 0, y = 0, z = 0;
        if (!SpotNear(a->GetMap(), id, d->posX, d->posY, d->posZ, def->aggroRange + 4.f, x, y, z))
        {
            Check(c, false, Acore::StringFormat("[{}] no ground to stand on near it", id));
            ++sCamp.next;
            return false;
        }
        CampaignPut(a, x, y, z, 0.f);
        CampaignPut(b, x - 2.f, y + 1.f, z, 0.f);
        a->SetOrientation(a->GetAngle(leader));
        sMsgs.clear();
        if (InGrace(a) || InGrace(b))
            return false;                        // (just woke at the graveyard: a few seconds before a fight can start)
        StartEncounter(leader, a);
        if (!InCombat(b))
            OnPlayerMoved(b, b->GetPosition());
        if (!(InCombat(a) && InCombat(b)))
            Check(c, false, Acore::StringFormat("[{}] both of them are in the fight (in fight {}/{}, grace {}/{}, down {}/{}, alive "
                "{}/{}, leader in a fight {} alive {}, {:.0f} m / {:.0f} m from it, join {:.0f}, sneaking {}/{}, combat flag {}/{})", id,
                InCombat(a), InCombat(b), InGrace(a), InGrace(b), IsDowned(a), IsDowned(b), a->IsAlive(), b->IsAlive(),
                InCombat(leader), leader->IsAlive(), a->GetExactDist(leader), b->GetExactDist(leader), def->joinRange,
                IsSneaking(a), IsSneaking(b), a->IsInCombat(), b->IsInCombat()));
        sCamp.cur = id;
        sCamp.fightStart = now;
        return false;
    }

    // the encounter's conversation, walked to an ending. policy 0: every new option once, all checks pass; 1: the
    // same with every check failing; 2: always the last option.
    void WorldTalk(Script& s, uint32 id, int policy)
    {
        WorldSetup(s, id, 5.f);
        s.Do([](Ctx& c)
        {
            if (Skipped(c))
                return;
            Player* p = Bot(c);
            sChosen.clear();
            if (!InDialogue(p))
            {
                sMsgs.clear();
                StartDialogue(p, Mob(c, 0));
            }
            AckTeleport(p);
            Check(c, InDialogue(p), "the conversation opened");
            c.v["picks"] = 0;
            c.v["idle"] = 0;
        });
        s.Until([policy](Ctx& c)
        {
            if (Skipped(c))
                return true;
            Player* p = Bot(c);
            AckTeleport(p);
            if (!InDialogue(p))
                return true;
            std::vector<std::pair<uint32, std::string>> opts;
            bool shown = false;
            for (std::string const& m : sMsgs)
            {
                if (m.rfind("DO^", 0) == 0)
                {
                    std::vector<std::string> f = Split(m, '^');
                    if (f.size() > 2)
                        opts.push_back({ uint32(std::atoi(f[1].c_str())), f.back() });
                }
                else if (m == "DE")
                    shown = true;
            }
            if (!shown)
            {
                // nothing came back from the last pick: a dead option
                if (++c.v["idle"] > 60)
                {
                    Check(c, false, Acore::StringFormat("after {} picks the conversation stopped answering", int(c.v["picks"])));
                    CloseDialogue(p, false);
                    return true;
                }
                return false;
            }
            c.v["idle"] = 0;
            if (opts.empty())
            {
                Check(c, false, Acore::StringFormat("after {} picks: a screen with no options (stuck)", int(c.v["picks"])));
                CloseDialogue(p, false);
                return true;
            }
            if (++c.v["picks"] > 80)
            {
                Check(c, false, "80 picks and the conversation still hasn't ended (a loop with no way out?)");
                CloseDialogue(p, false);
                return true;
            }
            std::pair<uint32, std::string> pick = opts.back();
            if (policy != 2)
                for (auto const& o : opts)
                    if (!sChosen.count(o.second))
                    {
                        pick = o;
                        break;
                    }
            sChosen.insert(pick.second);
            sMsgs.clear();
            sDice.clear();
            sFallback = policy == 0 ? 20 : (policy == 1 ? 1 : 10);
            HandleClientMessage(p, Acore::StringFormat("say^{}", pick.first));
            return false;
        }, "the conversation to reach an ending", 120000);
        s.Do([id](Ctx& c)
        {
            if (Skipped(c))
                return;
            Player* p = Bot(c);
            Check(c, !InDialogue(p), "the conversation ended");
            Check(c, p->IsAlive(), "the bot is alive");
            // it ended one of three ways: a fight, the encounter resolved, or a walk-away that can be picked up again
            if (!InCombat(p) && !HasCleared(p, id))
                Check(c, !p->HasUnitState(UNIT_STATE_ROOT), "left the talk: free to walk away");
        });
    }

    std::string Slug(std::string const& name)
    {
        std::string out;
        for (char ch : name)
            out += std::isalnum(static_cast<unsigned char>(ch)) ? char(std::tolower(static_cast<unsigned char>(ch))) : '_';
        return out;
    }

    // ------------------------------------------------------------------ the tests
    // Expected numbers are worked out from the rules, not read back from the engine. The jig's builds:
    //   wizard (gnome): STR 8 DEX 14 CON 14 INT 16 WIS 12 CHA 10  -> spell attack +5 / DC 13 at level 1, AC 12 unarmoured
    //   fighter (human, dwarf): STR 16 DEX 14 CON 14 INT 8 WIS 12 CHA 10 -> weapon attack +5 at level 1
    //   test creature: AC 10, 300 HP, every ability 10 (saves +0), claw +4 for a flat 1
    std::vector<TestDef> Catalogue()
    {
        std::vector<TestDef> t;
        auto add = [&t](std::string name, std::function<void(Script&)> f) { t.push_back({ std::move(name), std::move(f) }); };
        auto addSlow = [&t](std::string name, std::function<void(Script&)> f) { t.push_back({ std::move(name), std::move(f), true }); };
        BotOpt wiz1{ WIZ, 1 };
        BotOpt fig1{ FIGHTER, 1, "", { "style:Defense" } };

        // ============================================================ spell data (the DBC clones, tools/make_patch.py)
        add("data.action_spells_carry_no_stock_requirements", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                std::vector<uint32> all = CommonActions();
                for (auto const* kit : { &WizardKit(), &FighterKit(), &ClassActions() })
                    all.insert(all.end(), kit->begin(), kit->end());
                for (uint32 id : all)
                {
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(id);
                    std::string n = Acore::StringFormat("{} ({})", si ? si->SpellName[0] : "?", id);
                    if (!si)
                    {
                        Check(c, false, n + ": missing from the server's Spell.dbc");
                        continue;
                    }
                    Check(c, GetAction(id) != nullptr, n + ": no action-economy entry");
                    Check(c, !si->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING) && !si->HasAttribute(SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE),
                        n + ": waits for the next melee swing");
                    Check(c, !si->NeedsComboPoints(), n + ": needs combo points");
                    Check(c, !si->TargetCreatureType, n + ": limited to a creature type");
                    Check(c, !si->CastTimeEntry || si->CastTimeEntry->CastTime <= 0, n + ": has a cast time");
                    Check(c, !si->ManaCost && !si->ManaCostPercentage, n + ": costs mana");
                    Check(c, !si->RecoveryTime && !si->CategoryRecoveryTime, n + ": has a cooldown");
                    Check(c, !si->Stances, n + ": needs a stance");
                    Check(c, !si->CasterAuraState && !si->TargetAuraState, n + ": needs an aura state");
                    Check(c, !si->Reagent[0], n + ": needs a reagent");
                }
            });
        });

        // ============================================================ common actions
        add("common.attack.hit_damage_and_action", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });     // Quarterstaff 1d6
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 3, 0, "quarterstaff 1d6 (4) - 1 STR");
            Expect(s, "the Attack used the Action", [](Ctx& c) { return BotC(c)->actionUsed; });
            Expect(s, "the Attack left the Bonus Action", [](Ctx& c) { return !BotC(c)->bonusUsed; });
            Cast(s, SPELL_ATTACK);
            ExpectRefused(s, "no Action left");
            ExpectDrop(s, 0, 0, "a refused attack does no damage");
        });
        add("common.attack.natural_1_misses", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 1) });
            StartFight(s);
            Rig(s, { 1, 1 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 0);
            ExpectSaw(s, "CRITICAL MISS");
            Expect(s, "a miss still spends the Action", [](Ctx& c) { return BotC(c)->actionUsed; });
        });
        add("common.attack.natural_20_crits", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 30) });
            StartFight(s);
            Rig(s, { 20, 20 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "crit: 2d6 (4+4) - 1");
            ExpectSaw(s, "CRITICAL HIT");
        });
        add("common.attack.meets_ac_exactly", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 15) });
            StartFight(s);
            Rig(s, { 14, 14 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 3, 0, "14 + 1 = 15 vs AC 15 hits");
            NextTurn(s);
            Rig(s, { 13, 13 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 0, 0, "13 + 1 = 14 vs AC 15 misses");
        });
        add("common.dash", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            ExpectEq(s, "gnome movement", [](Ctx& c) { return BotC(c)->moveMax; }, 7.5);
            Cast(s, SPELL_DASH, -1);
            ExpectCastOk(s);
            ExpectEq(s, "movement after Dash", [](Ctx& c) { return BotC(c)->moveMax; }, 15.0);
            Expect(s, "Dash costs the Action", [](Ctx& c) { return BotC(c)->actionUsed && !BotC(c)->bonusUsed; });
        });
        add("common.disengage", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_DISENGAGE, -1);
            ExpectCastOk(s);
            Expect(s, "disengaged", [](Ctx& c) { return BotC(c)->disengaged; });
            Expect(s, "Disengage costs the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });
        add("common.jump", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            CastAt(s, SPELL_JUMP, 0.f, 3.5f);
            ExpectCastOk(s);
            ExpectSaw(s, "jumps");
            ExpectEq(s, "landing spot (combatant position) from the take-off", [](Ctx& c)
            {
                Position from(c.v["fromx"], c.v["fromy"], 0.f);
                return double(BotC(c)->lastPos.GetExactDist2d(&from));
            }, 3.5, 0.6);
            Expect(s, "Jump costs the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            ExpectEq(s, "a jump spends no movement", [](Ctx& c) { return BotC(c)->moved; }, 0.0, 0.3);
        });
        add("common.shove.success", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 20, 20, 1, 1 });
            Cast(s, SPELL_SHOVE);
            ExpectCastOk(s);
            ExpectSaw(s, "SUCCESS");
            s.Wait(1500);
            Expect(s, "the target was driven back (> 4 m away now)", [](Ctx& c) { return Mob(c)->GetExactDist2d(Bot(c)) > 4.f; });
            Expect(s, "Shove costs the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });
        add("common.shove.failure", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 1, 1, 20, 20 });
            Cast(s, SPELL_SHOVE);
            ExpectSaw(s, "FAILED");
            s.Wait(1000);
            Expect(s, "the target stays put", [](Ctx& c) { return Mob(c)->GetExactDist2d(Bot(c)) < 3.f; });
        });
        add("common.end_turn_and_creature_turn", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx&) { sMsgs.clear(); });
            NextTurn(s);
            ExpectSaw(s, "Test Claw");
            ExpectEq(s, "round", [](Ctx& c) { return Enc(c)->round; }, 2);
            Expect(s, "a fresh turn has a fresh Action and Bonus Action", [](Ctx& c) { return !BotC(c)->actionUsed && !BotC(c)->bonusUsed; });
        });
        add("common.not_your_turn", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Prep(p, SPELL_END_TURN);
                p->CastSpell(p, SPELL_END_TURN, false);
                Remember(c);
                sMsgs.clear();
                Prep(p, SPELL_FIRE_BOLT);
                c.v["cast"] = float(p->CastSpell(Mob(c), SPELL_FIRE_BOLT, false));
            });
            ExpectRefused(s, "It is not your turn");
            s.Wait(500);
            ExpectDrop(s, 0);
        });
        add("common.short_rest", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectCastOk(s);
            ExpectEq(s, "HP after a short rest (1 + 8/2)", [](Ctx& c) { return Bot(c)->GetHealth(); }, 5);
            ExpectEq(s, "short rests used", [](Ctx& c) { return GetSheet(Bot(c)).shortRestsUsed; }, 1);
            Cast(s, SPELL_SHORT_REST, -1);
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectSaw(s, "No short rests left");
            ExpectEq(s, "still two short rests used", [](Ctx& c) { return GetSheet(Bot(c)).shortRestsUsed; }, 2);
        });
        add("common.short_rest_not_in_combat", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectRefused(s, "can't do that in combat");
        });
        add("common.potion_bonus_action", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Cast(s, 439, -1);
            ExpectCastOk(s);
            Expect(s, "the potion healed", [](Ctx& c) { return Bot(c)->GetHealth() > 1; });
            Expect(s, "a potion costs the Bonus Action, not the Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            Cast(s, 439, -1);
            ExpectRefused(s, "no Bonus Action left");
        });
        add("common.potion_out_of_combat", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Cast(s, 439, -1);
            ExpectCastOk(s);
            Expect(s, "the potion healed", [](Ctx& c) { return Bot(c)->GetHealth() > 1; });
        });
        add("common.opportunity_attack", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                sMsgs.clear();
                Position to(p->GetPositionX() - 5.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectSaw(s, "Opportunity Attack");
            Expect(s, "the creature spent its reaction", [](Ctx& c) { return MobC(c)->reactionUsed; });
            ExpectEq(s, "movement spent", [](Ctx& c) { return BotC(c)->moved; }, 5.0, 0.2);
        });
        add("common.disengage_prevents_opportunity_attack", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_DISENGAGE, -1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                sMsgs.clear();
                Position to(p->GetPositionX() - 5.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectNotSaw(s, "Opportunity Attack");
            Expect(s, "the creature kept its reaction", [](Ctx& c) { return !MobC(c)->reactionUsed; });
        });
        add("common.movement_budget", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 0.f, 12.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                sMsgs.clear();
                Position to(p->GetPositionX() - 8.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectSaw(s, "Out of movement");
            ExpectEq(s, "movement capped at the budget", [](Ctx& c) { return BotC(c)->moved; }, 7.5);
            Expect(s, "rooted once the movement is spent", [](Ctx& c) { return Bot(c)->HasUnitState(UNIT_STATE_ROOT); });
        });
        add("common.long_rest", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Sheet& sh = GetSheet(p);
                sh.slotsUsed[1] = 3;
                sh.slotsUsed[2] = 2;
                sh.shortRestsUsed = 2;
                p->SetHealth(1);
                LongRest(p);
            });
            Expect(s, "full HP", [](Ctx& c) { return Bot(c)->GetHealth() == Bot(c)->GetMaxHealth(); });
            Expect(s, "slots back", [](Ctx& c) { return SlotsLeft(Bot(c), 1) == 4 && SlotsLeft(Bot(c), 2) == 2; });
            ExpectEq(s, "short rests back", [](Ctx& c) { return GetSheet(Bot(c)).shortRestsUsed; }, 0);
        });
        add("common.wow_auto_attack_blocked", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                WorldPacket swing(CMSG_ATTACKSWING, 8);
                swing << ObjectGuid::Empty;
                Check(c, !sScriptMgr->CanPacketReceive(p->GetSession(), swing), "CMSG_ATTACKSWING is dropped");
                WorldPacket stop(CMSG_ATTACKSTOP, 0);
                Check(c, sScriptMgr->CanPacketReceive(p->GetSession(), stop), "other packets pass");
            });
        });
        add("common.casting_on_a_creature_starts_a_fight", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            Act(s, [](Ctx& c) { Mob(c)->SetFaction(14); });
            Rig(s, { 15, 15 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectCastOk(s);
            ExpectDrop(s, 5, 0, "Fire Bolt 1d10 (5)");
            s.Wait(500);
            Expect(s, "the bot is in combat", [](Ctx& c) { return InCombat(Bot(c)); });
        });
        add("common.victory_ends_the_fight", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 10, 1) });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to end", 5000);
            Expect(s, "the creature died", [](Ctx& c) { return !Mob(c)->IsAlive(); });
            Expect(s, "the bot can move again", [](Ctx& c) { return !Bot(c)->HasUnitState(UNIT_STATE_ROOT); });
        });

        // ============================================================ conditions and advantage
        add("combat.prone_target_melee_advantage", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->prone = true; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "target prone: advantage");
        });
        add("combat.prone_target_ranged_disadvantage", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->prone = true; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectSaw(s, "target prone at range: disadvantage");
        });
        add("combat.ranged_attack_in_melee_disadvantage", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectSaw(s, "ranged attack in melee: disadvantage");
        });
        add("combat.held_creature_loses_its_turn", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->held = true; sMsgs.clear(); });
            Rig(s, {}, 1);                            // its WIS save: 1, still held
            NextTurn(s, true);
            ExpectSaw(s, "can't act");
            ExpectNotSaw(s, "Test Claw");
        });
        add("combat.frightened_creature_attacks_at_disadvantage", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->frightened = true; sMsgs.clear(); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectSaw(s, "frightened: disadvantage");
        });

        // ============================================================ wizard
        add("wizard.kit_by_level", [=](Script& s)
        {
            AddBot(s, wiz1);
            Expect(s, "L1 knows Fire Bolt, Magic Missile, Ice Knife", [](Ctx& c)
            {
                Player* p = Bot(c);
                return p->HasSpell(SPELL_FIRE_BOLT) && p->HasSpell(SPELL_MAGIC_MISSILE) && p->HasSpell(SPELL_ICE_KNIFE);
            });
            Expect(s, "L1 has no 2nd-level spells", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_SCORCHING_RAY); });
            Expect(s, "a wizard has no fighter actions", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_SECOND_WIND); });
            Expect(s, "every common action", [](Ctx& c)
            {
                for (uint32 id : CommonActions())
                    if (!Bot(c)->HasSpell(id))
                        return false;
                return true;
            });
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(3); });
            Expect(s, "L3 learns the 2nd-level spells", [](Ctx& c)
            {
                Player* p = Bot(c);
                return p->HasSpell(SPELL_SCORCHING_RAY) && p->HasSpell(SPELL_MISTY_STEP) && p->HasSpell(SPELL_SHATTER) &&
                    p->HasSpell(SPELL_HOLD_PERSON) && !p->HasSpell(SPELL_FIREBALL);
            });
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(5); });
            Expect(s, "L5 learns the 3rd-level spells", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_FIREBALL) && Bot(c)->HasSpell(SPELL_LIGHTNING_BOLT); });
        });
        add("wizard.slot_table", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "L1: 1st-level slots", [](Ctx& c) { return SlotsMax(Bot(c), 1); }, 2);
            ExpectEq(s, "L1: 2nd-level slots", [](Ctx& c) { return SlotsMax(Bot(c), 2); }, 0);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(3); });
            ExpectEq(s, "L3: 1st", [](Ctx& c) { return SlotsMax(Bot(c), 1); }, 4);
            ExpectEq(s, "L3: 2nd", [](Ctx& c) { return SlotsMax(Bot(c), 2); }, 2);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(5); });
            ExpectEq(s, "L5: 2nd", [](Ctx& c) { return SlotsMax(Bot(c), 2); }, 3);
            ExpectEq(s, "L5: 3rd", [](Ctx& c) { return SlotsMax(Bot(c), 3); }, 2);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(6); });
            ExpectEq(s, "L6: 3rd", [](Ctx& c) { return SlotsMax(Bot(c), 3); }, 3);
        });
        add("wizard.numbers_by_level", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "L1 spell save DC (8 + 2 + 3)", [](Ctx& c) { return SpellSaveDC(Bot(c)); }, 13);
            ExpectEq(s, "L1 spell attack (+2 +3)", [](Ctx& c) { return SpellAttackBonus(Bot(c)); }, 5);
            ExpectEq(s, "L1 HP (6 + 2)", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 8);
            ExpectEq(s, "AC unarmoured (10 + 2)", [](Ctx& c) { return ArmorClass(Bot(c)); }, 12);
            ExpectEq(s, "INT save (+3 +2 proficient)", [](Ctx& c) { return SaveBonus(Bot(c), Dnd::INT); }, 5);
            ExpectEq(s, "WIS save (+1 +2 proficient)", [](Ctx& c) { return SaveBonus(Bot(c), WIS); }, 3);
            ExpectEq(s, "STR save (-1, not proficient)", [](Ctx& c) { return SaveBonus(Bot(c), STR); }, -1);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(5); });
            ExpectEq(s, "L5 DC (prof 3)", [](Ctx& c) { return SpellSaveDC(Bot(c)); }, 14);
            ExpectEq(s, "L5 HP (8 + 4 x (4 + 2))", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 32);
        });
        add("wizard.fire_bolt", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectCastOk(s);
            ExpectDrop(s, 5, 0, "1d10 (5)");
            Expect(s, "a cantrip uses no slot", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1] == 0; });
            Expect(s, "it costs the Action", [](Ctx& c) { return BotC(c)->actionUsed; });
        });
        add("wizard.fire_bolt_crit", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 25) });
            StartFight(s);
            Rig(s, { 20, 20 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectDrop(s, 10, 0, "crit: 2d10 (5+5)");
        });
        add("wizard.fire_bolt_level5_two_dice", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectDrop(s, 10, 0, "L5: 2d10 (5+5)");
        });
        add("wizard.ray_of_frost", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_RAY_OF_FROST);
            ExpectCastOk(s);
            ExpectDrop(s, 3, 0, "1d8 (3)");
            Expect(s, "the target is slowed", [](Ctx& c) { return MobC(c)->slowed; });
        });
        add("wizard.shocking_grasp", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_SHOCKING_GRASP);
            ExpectCastOk(s);
            ExpectDrop(s, 3, 0, "1d8 (3)");
            Expect(s, "the target loses its reactions", [](Ctx& c) { return MobC(c)->noReaction; });
        });
        add("wizard.magic_missile", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 30) });
            StartFight(s);
            Rig(s, {}, 2);
            Cast(s, SPELL_MAGIC_MISSILE);
            ExpectCastOk(s);
            ExpectDrop(s, 9, 0, "3 darts x (1d4 (2) + 1), no attack roll");
            ExpectEq(s, "1st-level slots used", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 1);
        });
        add("wizard.burning_hands_and_running_out_of_slots", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 3.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 3);
            Cast(s, SPELL_BURNING_HANDS);
            ExpectCastOk(s);
            ExpectDrop(s, 9, 0, "failed DEX save: 3d6 (3+3+3)");
            NextTurn(s);
            Rig(s, { 20, 20 }, 3);
            Cast(s, SPELL_BURNING_HANDS);
            ExpectDrop(s, 4, 0, "made the save: 9 halved");
            ExpectEq(s, "both 1st-level slots used", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 2);
            NextTurn(s);
            Rig(s, { 1, 1 }, 3);
            Cast(s, SPELL_BURNING_HANDS);
            ExpectRefused(s, "No level 1 spell slots left");
            ExpectDrop(s, 0);
        });
        add("wizard.chromatic_orb_resist_immune_vulnerable", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            CreatureStats st = Stats(HUMANOID, 10);
            st.resist = 1u << DMG_FIRE;
            st.immune = 1u << DMG_COLD;
            st.vuln = 1u << DMG_LIGHTNING;
            AddMob(s, { 8.f, 0.f, st });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_CHROMATIC_FIRE);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "fire 3d8 (12), resisted: 6");
            NextTurn(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_CHROMATIC_COLD);
            ExpectDrop(s, 0, 0, "cold, immune: 0");
            NextTurn(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_CHROMATIC_LIGHT);
            ExpectDrop(s, 24, 0, "lightning 12, vulnerable: 24");
        });
        add("wizard.ice_knife_explosion", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 6.f, 0.f });
            AddMob(s, { 7.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15, 2, 1, 1, 2, 2, 1, 1, 2, 2 });
            Cast(s, SPELL_ICE_KNIFE);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "target: 1d10 (2) + failed save 2d6 (4)");
            ExpectDrop(s, 4, 1, "neighbour: failed save 2d6 (4)");
        });
        add("wizard.mage_armor", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "AC before", [](Ctx& c) { return ArmorClass(Bot(c)); }, 12);
            Cast(s, SPELL_MAGE_ARMOR, -1);
            ExpectCastOk(s);
            ExpectEq(s, "AC with Mage Armor (13 + 2)", [](Ctx& c) { return ArmorClass(Bot(c)); }, 15);
            ExpectEq(s, "slot used", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 1);
        });
        add("wizard.false_life_soaks_damage", [=](Script& s)
        {
            AddBot(s, wiz1);
            CreatureStats st = Stats();
            st.melee.dice = Dice::Parse("5");
            AddMob(s, { 2.f, 0.f, st });
            Rig(s, {}, 3);
            Cast(s, SPELL_FALSE_LIFE, -1);
            ExpectCastOk(s);
            ExpectEq(s, "temp HP (1d4 (3) + 4)", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 7);
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { Remember(c); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectEq(s, "temp HP after a 5-damage hit", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 2);
            Expect(s, "real HP untouched", [](Ctx& c) { return Bot(c)->GetHealth() == Bot(c)->GetMaxHealth(); });
        });
        add("wizard.grease", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 });
            CastAt(s, SPELL_GREASE, 6.f, 0.f);
            ExpectCastOk(s);
            Expect(s, "a grease area is down", [](Ctx& c) { return !Enc(c)->areas.empty(); });
            Expect(s, "the creature in it fell prone (failed DEX save)", [](Ctx& c) { return MobC(c)->prone; });
        });
        add("wizard.enhanced_leap", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "jump distance (STR 8: the 3 m minimum... max(3, 4))", [](Ctx& c) { return JumpDistance(Bot(c)); }, 4.0);
            Cast(s, SPELL_ENHANCED_LEAP, -1);
            ExpectCastOk(s);
            ExpectEq(s, "tripled", [](Ctx& c) { return JumpDistance(Bot(c)); }, 12.0);
        });
        add("wizard.scorching_ray", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15, 2, 2, 15, 15, 2, 2, 15, 15, 2, 2 });
            Cast(s, SPELL_SCORCHING_RAY);
            ExpectCastOk(s);
            ExpectDrop(s, 12, 0, "three rays x 2d6 (4)");
            ExpectEq(s, "2nd-level slot used", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[2]; }, 1);
        });
        add("wizard.misty_step", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            CastAt(s, SPELL_MISTY_STEP, 0.f, 10.f);
            ExpectCastOk(s);
            Act(s, [](Ctx& c) { AckTeleport(Bot(c)); });
            s.Wait(300);
            ExpectEq(s, "teleported ~10 m", [](Ctx& c)
            {
                Position from(c.v["fromx"], c.v["fromy"], 0.f);
                return double(Bot(c)->GetExactDist2d(&from));
            }, 10.0, 1.0);
            Expect(s, "Misty Step costs the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });
        add("wizard.shatter", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 3);
            Cast(s, SPELL_SHATTER);
            ExpectCastOk(s);
            ExpectDrop(s, 9, 0, "failed CON save: 3d8 (3+3+3)");
        });
        add("wizard.hold_person_and_auto_crit", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 1.8f, 0.f, Stats(HUMANOID) });
            StartFight(s);
            Rig(s, { 1, 1 });
            Cast(s, SPELL_HOLD_PERSON);
            ExpectCastOk(s);
            Expect(s, "held", [](Ctx& c) { return MobC(c)->held; });
            Rig(s, {}, 1);
            NextTurn(s, true);
            Expect(s, "still held after its turn (save 1)", [](Ctx& c) { return MobC(c)->held; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "target held: advantage");
            ExpectDrop(s, 7, 0, "melee vs held from close by is a crit: 2d6 (4+4) - 1");
        });
        add("wizard.hold_person_not_on_beasts", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 6.f, 0.f, Stats(BEAST) });
            StartFight(s);
            Rig(s, { 1, 1 });
            Cast(s, SPELL_HOLD_PERSON);
            ExpectSaw(s, "not a person");
            Expect(s, "not held", [](Ctx& c) { return !MobC(c)->held; });
        });
        add("wizard.fireball_failed_saves", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 10.f, -1.f });
            AddMob(s, { 10.f, 1.f });
            StartFight(s);
            Rig(s, {}, 1);
            Cast(s, SPELL_FIREBALL);
            ExpectCastOk(s);
            ExpectDrop(s, 8, 0, "8d6 all 1s");
            ExpectDrop(s, 8, 1, "the second creature in the blast too");
            ExpectEq(s, "3rd-level slot used", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[3]; }, 1);
        });
        add("wizard.fireball_saved_half", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Rig(s, {}, 20);
            Cast(s, SPELL_FIREBALL);
            ExpectDrop(s, 24, 0, "8d6 all 6s = 48, saved: 24");
        });
        add("wizard.lightning_bolt_line", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 8.f, 0.f });
            AddMob(s, { 8.f, 6.f });
            StartFight(s);
            Rig(s, {}, 1);
            Cast(s, SPELL_LIGHTNING_BOLT);
            ExpectCastOk(s);
            ExpectDrop(s, 8, 0, "on the line: 8d6 all 1s");
            ExpectDrop(s, 0, 1, "6 m off the line: untouched");
        });
        add("wizard.arcane_recovery", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).slotsUsed[1] = 2; });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "Arcane Recovery: (3 + 1) / 2 = 2 slot levels back", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 0);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).slotsUsed[1] = 2; });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "only once per long rest", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 2);
        });
        add("wizard.abjuration_arcane_ward", [=](Script& s)
        {
            AddBot(s, { WIZ, 2, "Abjuration" });
            CreatureStats st = Stats();
            st.melee.dice = Dice::Parse("5");
            AddMob(s, { 2.f, 0.f, st });
            Cast(s, SPELL_MAGE_ARMOR, -1);
            ExpectEq(s, "ward raised: 2 x 2 + 3", [](Ctx& c) { return GetSheet(Bot(c)).wardHp; }, 7);
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectEq(s, "ward after a 5-damage hit", [](Ctx& c) { return GetSheet(Bot(c)).wardHp; }, 2);
            Expect(s, "HP untouched", [](Ctx& c) { return Bot(c)->GetHealth() == Bot(c)->GetMaxHealth(); });
            Cast(s, SPELL_MAGE_ARMOR, -1);
            ExpectEq(s, "another abjuration recharges it by 2 x slot level", [](Ctx& c) { return GetSheet(Bot(c)).wardHp; }, 4);
        });
        add("wizard.abjuration_warded_rest", [=](Script& s)
        {
            AddBot(s, { WIZ, 6, "Abjuration" });
            Cast(s, SPELL_MAGE_ARMOR, -1);
            ExpectEq(s, "ward at L6: 2 x 6 + 3", [](Ctx& c) { return GetSheet(Bot(c)).wardHp; }, 15);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).wardHp = 3; });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "a short rest restores it", [](Ctx& c) { return GetSheet(Bot(c)).wardHp; }, 15);
        });
        add("wizard.evocation_empowered", [=](Script& s)
        {
            AddBot(s, { WIZ, 2, "Evocation" });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectDrop(s, 8, 0, "Fire Bolt 5 + INT 3");
            NextTurn(s);
            Rig(s, {}, 2);
            Cast(s, SPELL_MAGIC_MISSILE);
            ExpectDrop(s, 12, 0, "Magic Missile 3 x 3 + INT 3 once");
        });
        add("wizard.evocation_potent_cantrip", [=](Script& s)
        {
            AddBot(s, { WIZ, 6, "Evocation" });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 2, 2 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectSaw(s, "MISS");
            ExpectDrop(s, 5, 0, "a miss still deals half: (2d10 (8) + 3) / 2");
        });
        add("wizard.ability_score_improvement", [=](Script& s)
        {
            AddBot(s, { WIZ, 4 });
            Act(s, [](Ctx& c) { HandlePick(Bot(c), "asi^INT"); });
            ExpectEq(s, "INT 16 + 2", [](Ctx& c) { return GetSheet(Bot(c)).abil[Dnd::INT]; }, 18);
            Act(s, [](Ctx& c) { HandlePick(Bot(c), "asi^INT"); });
            ExpectEq(s, "only one improvement at L4", [](Ctx& c) { return GetSheet(Bot(c)).abil[Dnd::INT]; }, 18);
            ExpectEq(s, "DC follows INT (8 + 2 + 4)", [](Ctx& c) { return SpellSaveDC(Bot(c)); }, 14);
        });
        add("wizard.gnome_cunning", [=](Script& s)
        {
            AddBot(s, wiz1);
            Expect(s, "advantage on a magic WIS save", [](Ctx& c) { return SaveAdvantage(Bot(c), WIS, true); });
            Expect(s, "advantage on a magic INT save", [](Ctx& c) { return SaveAdvantage(Bot(c), Dnd::INT, true); });
            Expect(s, "none on a non-magic WIS save", [](Ctx& c) { return !SaveAdvantage(Bot(c), WIS, false); });
            Expect(s, "none on a magic DEX save", [](Ctx& c) { return !SaveAdvantage(Bot(c), DEX, true); });
        });

        // ============================================================ feats
        add("feat.tough", [=](Script& s)
        {
            AddBot(s, { WIZ, 4 });
            ExpectEq(s, "L4 HP (8 + 3 x 6)", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 26);
            Act(s, [](Ctx& c) { HandlePick(Bot(c), "feat^Tough"); });
            ExpectEq(s, "Tough: + 2 x 4", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 34);
        });
        add("feat.alert_initiative", [=](Script& s)
        {
            AddBot(s, { WIZ, 4 });
            Act(s, [](Ctx& c) { HandlePick(Bot(c), "feat^Alert"); });
            Expect(s, "the feat is taken", [](Ctx& c) { return HasFeat(Bot(c), "Alert"); });
            AddMob(s, {});
            StartFight(s, 10);
            ExpectEq(s, "initiative 10 + DEX 2 + Alert 5", [](Ctx& c) { return BotC(c)->initiative; }, 17);
        });

        // every character picks a feat at level 1, any race, on top of the racial traits; it isn't the level-4 ASI
        add("feat.a_starting_feat_at_level_one", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                auto pending = [&]() { sMsgs.clear(); PushPendingChoices(p); return sMsgs.empty() ? std::string() : sMsgs.back(); };
                Check(c, pending() == "LP^feat1", "a fresh level-1 wizard: a feat to choose");
                int con = GetSheet(p).abil[CON];
                HandlePick(p, "feat1^Resilient^CON");
                Check(c, HasFeat(p, "Resilient") && FeatOption(p, "Resilient") == "CON", "Resilient (Constitution) taken");
                CheckEq(c, GetSheet(p).abil[CON] - con, 1, "its +1 Constitution");
                Check(c, pending() == "LP^", "nothing more to choose at level 1");
                HandlePick(p, "feat1^Alert");
                Check(c, !HasFeat(p, "Alert"), "only one starting feat");
                HandlePick(p, "feat^Alert");
                Check(c, !HasFeat(p, "Alert"), "and no ASI feat before level 4");
                p->GiveLevel(4);
                HandlePick(p, "subclass^Evocation");     // (a wizard's tradition, due at 2, comes first)
                Check(c, pending() == "LP^asi", "level 4: the ASI is still due");
                HandlePick(p, "feat^Resilient^CON");
                Check(c, pending() == "LP^asi", "the same feat twice: refused, the ASI still due");
                HandlePick(p, "feat^Tough");
                Check(c, HasFeat(p, "Tough") && pending() == "LP^", "Tough at 4: done");
                ClearLevelChoices(p);
                Check(c, HasFeat(p, "Resilient") && !HasFeat(p, "Tough"), "a restart keeps the starting feat, undoes the level-4 one");
                CheckEq(c, GetSheet(p).abil[CON] - con, 1, "and its +1 stays");
            });
        });

        // ============================================================ feats (BG3's list, DndLevels.cpp sFeatDefs)
        auto pick = [](std::string what) { return [what](Ctx& c) { HandlePick(Bot(c), what); }; };
        BotOpt wiz4{ WIZ, 4 };
        BotOpt fig4{ FIGHTER, 4, "Champion", { "style:Defense" } };
        add("feat.ability_split", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("asi2^STR^CON"));
            ExpectEq(s, "STR 8 + 1", [](Ctx& c) { return GetSheet(Bot(c)).abil[STR]; }, 9);
            ExpectEq(s, "CON 14 + 1", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 15);
            Act(s, pick("asi2^INT^WIS"));
            ExpectEq(s, "only one improvement at L4", [](Ctx& c) { return GetSheet(Bot(c)).abil[Dnd::INT]; }, 16);
            Act(s, [](Ctx& c) { ClearLevelChoices(Bot(c)); });
            ExpectEq(s, ".dnd restart undoes it: STR", [](Ctx& c) { return GetSheet(Bot(c)).abil[STR]; }, 8);
            ExpectEq(s, ".dnd restart undoes it: CON", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 14);
        });
        add("feat.class_limits", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Heavy Armour Master"));
            Expect(s, "a wizard can't take Heavy Armour Master", [](Ctx& c) { return !HasFeat(Bot(c), "Heavy Armour Master"); });
            Act(s, pick("feat^Resilient^LUCK"));
            Expect(s, "a variant must be one of the feat's", [](Ctx& c) { return !HasFeat(Bot(c), "Resilient"); });
            Act(s, pick("feat^Resilient"));
            Expect(s, "a variant feat needs its variant", [](Ctx& c) { return !HasFeat(Bot(c), "Resilient"); });
            Act(s, pick("feat^Spell Sniper"));
            Expect(s, "a wizard can take Spell Sniper", [](Ctx& c) { return HasFeat(Bot(c), "Spell Sniper"); });
        });
        add("feat.resilient", [=](Script& s)
        {
            AddBot(s, wiz4);
            ExpectEq(s, "CON save before (+2)", [](Ctx& c) { return SaveBonus(Bot(c), CON); }, 2);
            Act(s, pick("feat^Resilient^CON"));
            ExpectEq(s, "CON 15", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 15);
            ExpectEq(s, "CON save: +2 + proficiency 2", [](Ctx& c) { return SaveBonus(Bot(c), CON); }, 4);
            Act(s, [](Ctx& c) { ClearLevelChoices(Bot(c)); });
            ExpectEq(s, "undone with the feat", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 14);
        });
        add("feat.athlete", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Athlete^STR"));
            ExpectEq(s, "STR 9: jump max(3, 4.5) x 1.5", [](Ctx& c) { return JumpDistance(Bot(c)); }, 6.75);
        });
        add("feat.durable", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Durable"));
            ExpectEq(s, "CON 15", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 15);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Cast(s, SPELL_SHORT_REST, -1);
            Expect(s, "a short rest heals to full", [](Ctx& c) { return Bot(c)->GetHealth() == Bot(c)->GetMaxHealth(); });
        });
        add("feat.elemental_adept", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Elemental Adept^Fire"));
            CreatureStats st = Stats();
            st.resist = 1u << DMG_FIRE;
            AddMob(s, { 8.f, 0.f, st });
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectDrop(s, 2, 0, "the 1 counts as 2, fire resistance ignored");
            NextTurn(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_CHROMATIC_COLD);
            ExpectDrop(s, 3, 0, "cold isn't the element: 3d8 all 1s");
        });
        add("feat.heavy_armour_master", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Heavy Armour Master"));
            ExpectEq(s, "STR 17", [](Ctx& c) { return GetSheet(Bot(c)).abil[STR]; }, 17);
            CreatureStats st = Stats();
            st.melee.dice = Dice::Parse("5");
            AddMob(s, { 2.f, 0.f, st });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 9966); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { Remember(c); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectEq(s, "plate: a 5-damage claw does 2", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 2);
            Act(s, [](Ctx& c) { StripGear(Bot(c)); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { Remember(c); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectEq(s, "no heavy armour: the full 5", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 5);
        });
        add("feat.medium_armour_master", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).abil[DEX] = 16; Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 285); });
            ExpectEq(s, "scale mail, DEX +3 capped at 2, Defense", [](Ctx& c) { return ArmorClass(Bot(c)); }, 17);
            Act(s, pick("feat^Medium Armour Master"));
            ExpectEq(s, "cap 3", [](Ctx& c) { return ArmorClass(Bot(c)); }, 18);
        });
        add("feat.mobile_hit_and_run", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Mobile"));
            ExpectEq(s, "gnome 7.5 + 3", [](Ctx& c) { return Speed(Bot(c)); }, 10.5);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 1, 1 });
            Cast(s, SPELL_ATTACK);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                sMsgs.clear();
                Position to(p->GetPositionX() - 5.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectNotSaw(s, "Opportunity Attack");
        });
        add("feat.mobile_dash_through_difficult_terrain", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Mobile"));
            AddMob(s, { 0.f, 15.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Enc(c)->areas.push_back({ SPELL_GREASE, p->GetPosition(), 10.f, uint32(GameTime::GetGameTimeMS().count() + 60000) });
                Position to(p->GetPositionX() - 2.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectEq(s, "2 m of grease costs 4", [](Ctx& c) { return BotC(c)->moved; }, 4.0, 0.1);
            Cast(s, SPELL_DASH, -1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Position to(p->GetPositionX() - 2.f, p->GetPositionY(), p->GetPositionZ(), 0.f);
                p->Relocate(to);
                OnPlayerMoved(p, to);
            });
            ExpectEq(s, "after a Dash, 2 m costs 2", [](Ctx& c) { return BotC(c)->moved; }, 6.0, 0.1);
        });
        add("feat.savage_attacker", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Savage Attacker"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15, 2, 7 });
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 10, 0, "longsword rolled 2 and 7, keeps 7, + 3");
            ExpectSaw(s, "Savage Attacker");
        });
        add("feat.shield_master", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Shield Master"));
            ExpectEq(s, "no shield: DEX save +2", [](Ctx& c) { return SaveBonus(Bot(c), DEX); }, 2);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2362); });
            ExpectEq(s, "with a shield: +4", [](Ctx& c) { return SaveBonus(Bot(c), DEX); }, 4);
        });
        add("feat.spell_sniper", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Spell Sniper"));
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 30) });
            StartFight(s);
            Rig(s, { 19, 19 }, 5);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectDrop(s, 10, 0, "19 crits: 2d10 (5+5)");
        });
        add("feat.unarmed_strike_is_one_plus_strength", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 });
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 4, 0, "unarmed: 1 + STR 3");
        });
        add("feat.tavern_brawler", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Tavern Brawler^STR"));
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 });
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "STR 17: 1 + 3 + 3 again");
        });
        // ... and thrown weapons: Strength again to hit and to damage (a 4 on the die: 4 + 2 + 3 = 9 misses AC 12, + 3 hits)
        add("feat.tavern_brawler_thrown", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Tavern Brawler^STR"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 25861); });      // a throwing axe (2d2)
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Rig(s, { 4, 4 }, 2);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectDrop(s, 10, 0, "hits AC 12 with 4 + 2 + 3 + 3; 2d2 (4) + STR 3 + 3 again");
        });
        // Throw: the main-hand weapon at range. A handaxe from 8 m: 9 + STR 3 + prof 2 = 14 hits AC 12; 1d6 (4) + 3 = 7
        add("action.throw_a_handaxe", [=](Script& s)
        {
            AddBot(s, fig4);
            Expect(s, "everyone has Throw", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_THROW); });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2134); });
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Rig(s, { 9, 9 }, 4);
            Cast(s, SPELL_THROW);
            ExpectCastOk(s);
            ExpectSaw(s, "Handaxe (thrown)");
            ExpectNotSaw(s, "ranged attack in melee: disadvantage");
            ExpectDrop(s, 7, 0, "hits AC 12 with 9 + 3 + 2; 1d6 (4) + STR 3");
        });
        // ... with Tavern Brawler, Strength again: STR 17 (+3), so 4 + 3 + 2 + 3 = 12 hits AC 12; 1d6 (4) + 3 + 3 = 10
        add("action.throw_tavern_brawler", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Tavern Brawler^STR"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2134); });
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Rig(s, { 4, 4 }, 4);
            Cast(s, SPELL_THROW);
            ExpectDrop(s, 10, 0, "4 + 3 + 2 + 3 = 12 hits AC 12; 4 + 3 + 3");
        });
        // a battleaxe is a one-handed axe in WoW, like the handaxe - but it isn't made to be thrown
        add("action.throw_refuses_a_battleaxe", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 20850); });
            AddMob(s, { 8.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Cast(s, SPELL_THROW);
            Expect(s, "refused, saying what can be thrown", [](Ctx& c)
            {
                return int(c.v["cast"]) != SPELL_CAST_OK && SawBy(Bot(c), "weapon made for throwing");
            });
        });
        add("feat.weapon_master", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Weapon Master^DEX"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2134); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 9, 9 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 2, 0, "proficient with a handaxe now: 9 - 1 + 2 = 10 hits; 3 - 1");
        });
        add("feat.crossbow_expert", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Crossbow Expert"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 15807); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectCastOk(s);
            ExpectSaw(s, "Crossbow Expert");
            ExpectNotSaw(s, "ranged attack in melee: disadvantage");
        });
        add("feat.martial_adept", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Martial Adept"));
            ExpectEq(s, "a wizard gets one superiority die", [](Ctx& c) { return SuperiorityMax(Bot(c)); }, 1);
            Expect(s, "and the maneuvers", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_TRIP_ATTACK) && Bot(c)->HasSpell(SPELL_MENACING_ATTACK); });
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).supUsed = 1; });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "a short rest brings it back", [](Ctx& c) { return GetSheet(Bot(c)).supUsed; }, 0);
        });
        add("feat.martial_adept_battle_master", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 4, "Battle Master", { "style:Defense" } });
            Act(s, pick("feat^Martial Adept"));
            ExpectEq(s, "4 + 1 dice", [](Ctx& c) { return SuperiorityMax(Bot(c)); }, 5);
        });
        add("feat.great_weapon_master", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Great Weapon Master"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 10, 500) });
            StartFight(s);
            Rig(s, { 20, 20 }, 4);
            Cast(s, SPELL_ATTACK);
            Expect(s, "a crit readies the bonus attack", [](Ctx& c) { return BotC(c)->gwmReady; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 11, 0, "the bonus attack lands: 2d6 (8) + 3");
            Expect(s, "it cost the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->gwmReady; });
            Cast(s, SPELL_ATTACK);
            ExpectRefused(s, "no Action left");
        });
        add("feat.great_weapon_master_needs_a_crit_or_kill", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Great Weapon Master"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            Cast(s, SPELL_ATTACK);
            ExpectRefused(s, "no Action left");
        });
        add("feat.great_weapon_master_on_a_kill", [=](Script& s)
        {
            AddBot(s, fig4);
            Act(s, pick("feat^Great Weapon Master"));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 10, 1) });
            AddMob(s, { 2.f, 2.f, Stats(HUMANOID, 10, 300) });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            Expect(s, "a kill (no crit) readies the bonus attack", [](Ctx& c) { return !Mob(c)->IsAlive() && BotC(c)->gwmReady; });
        });
        add("feat.alert_cannot_be_surprised", [=](Script& s)
        {
            AddBot(s, wiz4);
            Act(s, pick("feat^Alert"));
            AddMob(s, {});
            Act(s, [](Ctx& c)
            {
                sDice.assign({ 20, 20, 1, 1 });
                Mob(c)->SetFaction(14);
                StartEncounter(Mob(c), Bot(c));
                sDice.clear();
                BotC(c)->surprised = true;
            });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c)); }, "the bot's turn", 15000);
            ExpectEq(s, "the bot still acts in round 1", [](Ctx& c) { return Enc(c)->round; }, 1);
        });

        // ============================================================ treat minigames (DndTreats.cpp; the games: DnDGames.lua)
        // the last TG^id^game^level^nonce^item the bot was sent ("" if none)
        auto lastGame = []
        {
            std::string g;
            for (std::string const& m : sMsgs)
                if (m.rfind("TG^", 0) == 0)
                    g = m;
            return g;
        };
        auto answer = [lastGame](int won)
        {
            return [lastGame, won](Ctx& c)
            {
                std::vector<std::string> f = Split(lastGame(), '^');
                if (f.size() < 5)
                    throw std::runtime_error("no minigame was opened");
                TreatGameResult(Bot(c), f[1] + "^" + f[4] + "^" + std::to_string(won));
            };
        };
        add("treat.payoff_game_after_a_passed_check", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(991, Bot(c), "geode", 12, 5000); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 991); });
            Expect(s, "the check passed: the geode game opens", [lastGame](Ctx&) { return lastGame().rfind("TG^991^geode^0^", 0) == 0; });
            Expect(s, "nothing paid out yet", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Act(s, answer(1));
            ExpectEq(s, "cracked open: the reward", [](Ctx& c) { return float(Bot(c)->GetMoney()) - c.v["money"]; }, 5000);
            ExpectSaw(s, "+3 XP");                   // a DC 12 find: 3 XP
            ExpectSaw(s, "LF^");                     // what it taught you goes to the log's Finds tab
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 991); });
            ExpectSaw(s, "already searched");
            Act(s, [](Ctx&) { RemoveTestTreat(991); });
        });
        // one find for everyone: whoever claims a treat takes it from the world (for every other character, for good), and
        // takes a share of the goods for each player within 30 m to hand out
        add("treat.taken_is_gone_for_everyone_else", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, FIGHTER, 1, a->GetPositionX() + 40.f, a->GetPositionY());   // 40 m off: not in on it
                AddTestTreat(985, a, "", 5, 1000);
                c.v["amoney"] = float(a->GetMoney());
                c.v["bmoney"] = float(b->GetMoney());
                sDice.assign({ 15, 15 });
                UseTestTreat(a, 985);
                sDice.clear();
                CheckEq(c, double(a->GetMoney()) - c.v["amoney"], 1000, "the finder alone: one share");
                MoveBotTo(b, a->GetPositionX() + 2.f, a->GetPositionY());
                ClearMsgs();
                sDice.assign({ 15, 15 });
                UseTestTreat(b, 985);
                sDice.clear();
                Check(c, SawBy(b, "Someone got here first - Jigwiz"), "the next one finds it gone, and who took it");
                CheckEq(c, double(b->GetMoney()) - c.v["bmoney"], 0, "and gets nothing");
                Check(c, MsgsBy(b, "TD^").empty(), "without even a roll");
                ResetTreats(a);                                   // (.dnd restart: the character gives back what it took)
                sDice.assign({ 15, 15 });
                UseTestTreat(b, 985);
                sDice.clear();
                CheckEq(c, double(b->GetMoney()) - c.v["bmoney"], 2000, "given back: the fighter finds it (with the wizard by: two shares)");
                RemoveTestTreat(985);
            });
        });
        add("treat.the_finder_gets_a_share_for_each_player_nearby", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                AddBotAt(c, FIGHTER, 1, a->GetPositionX() + 3.f, a->GetPositionY());
                AddBotAt(c, ROGUE, 1, a->GetPositionX(), a->GetPositionY() + 3.f);
                AddTestTreat(986, a, "", 5, 500);
                SetTestTreat(986, "", "", "", 118);               // and a potion
                c.v["money"] = float(a->GetMoney());
                uint32 before = a->GetItemCount(118);
                sDice.assign({ 15, 15 });
                ClearMsgs();
                UseTestTreat(a, 986);
                sDice.clear();
                CheckEq(c, double(a->GetMoney()) - c.v["money"], 1500, "three players: 500 x 3");
                CheckEq(c, double(a->GetItemCount(118) - before), 3, "and three potions");
                Check(c, SawBy(a, "A share for each of the 3 of you"), "to hand out");
                RemoveTestTreat(986);
            });
        });
        // [Update] lines in everyone's chat (about bots, the jig keeps them instead of sending them)
        add("announce.a_find_says_what_came_out_of_it", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                sAnnounced.clear();
                AddTestTreat(987, Bot(c), "", 5, 1000);
                SetTestTreat(987, "", "", "", 118);
                sDice.assign({ 15, 15 });
                UseTestTreat(Bot(c), 987);
                sDice.clear();
                std::string want = "Jigwiz searched something hidden in ";
                Check(c, WasAnnounced(want), "announced: " + want + "...");
                ItemTemplate const* it = sObjectMgr->GetItemTemplate(118);
                Check(c, it && WasAnnounced("came away with " + it->Name1 + " and 10s."), "with what it held");
                RemoveTestTreat(987);
            });
        });
        add("announce.deaths_victories_and_levels", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                sAnnounced.clear();
                DamageCtx d;
                d.attacker = Bot(c);
                d.victim = Mob(c);
                d.amount = 1000000;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to be won", 6000);
            Expect(s, "the victory is announced", [](Ctx&) { return WasAnnounced("Jigfighter defeated "); });
            Act(s, [](Ctx& c) { sAnnounced.clear(); Bot(c)->GiveLevel(2); });
            Expect(s, "a new level is announced", [](Ctx&) { return WasAnnounced("Jigfighter has reached level 2."); });
            AddMob(s, { 2.f, 2.f });
            Act(s, [](Ctx& c)
            {
                if (TempSummon* dead = Mob(c, 0)->ToTempSummon())
                    dead->UnSummon();
                c.mobs.erase(c.mobs.begin());
            });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                sAnnounced.clear();
                GetSheet(Bot(c)).tempHp = 0;
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = Bot(c);
                d.amount = 1000000;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
            Expect(s, "a death in a fight is announced", [](Ctx&) { return WasAnnounced("Jigfighter has fallen fighting "); });
            Act(s, [](Ctx& c) { Bot(c)->ResurrectPlayer(1.f); });
        });
        add("treat.failed_check_opens_no_payoff_game", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(992, Bot(c), "ice", 12, 5000); });
            Rig(s, { 1, 1 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 992); });
            Expect(s, "no game", [lastGame](Ctx&) { return lastGame().empty(); });
            ExpectSaw(s, "TE^fail");
            Act(s, [](Ctx&) { RemoveTestTreat(992); });
        });
        add("treat.puzzle_needs_a_passed_check", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(993, Bot(c), "runes", 12, 5000); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 1, 1 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 993); });
            Expect(s, "a failed roll: no puzzle at all", [lastGame](Ctx&) { return lastGame().empty(); });
            ExpectSaw(s, "TE^fail");
            Expect(s, "and nothing paid", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 993); });
            ExpectSaw(s, "already searched");
            Expect(s, "no second chance", [lastGame](Ctx&) { return lastGame().empty(); });
            Act(s, [](Ctx&) { RemoveTestTreat(993); });
        });
        add("treat.puzzle_out_of_tries_pays_nothing_and_keeps_its_difficulty", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(997, Bot(c), "lock", 12, 5000); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 997); });
            Expect(s, "a passed roll: the lock opens to pick", [lastGame](Ctx&) { return lastGame().rfind("TG^997^lock^0^", 0) == 0; });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 997); });
            Expect(s, "closed and reopened: no new roll", [lastGame](Ctx&) { return lastGame().rfind("TG^997^lock^0^", 0) == 0; });
            ExpectNotSaw(s, "TD^");
            Act(s, answer(0));
            ExpectSaw(s, "TE^fail");
            Expect(s, "out of picks pays nothing", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Act(s, [](Ctx&) { RemoveTestTreat(997); });
        });
        // the second set of games (DnDGames2.lua): the server only knows payoffs (can't be lost) from puzzles
        add("treat.new_games_a_puzzle_can_be_lost_a_payoff_cannot", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(975, Bot(c), "wards", 15, 500); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 975); });
            Expect(s, "the seal opens, hard (DC 15)", [lastGame](Ctx&) { return lastGame().rfind("TG^975^wards^1^", 0) == 0; });
            Act(s, answer(0));
            ExpectSaw(s, "TE^fail");
            Expect(s, "out of touches: nothing paid", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Act(s, [](Ctx&) { RemoveTestTreat(975); });
            Act(s, [](Ctx& c) { AddTestTreat(974, Bot(c), "pry", 12, 500); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 974); });
            Expect(s, "the crate opens to pry, easy", [lastGame](Ctx&) { return lastGame().rfind("TG^974^pry^0^", 0) == 0; });
            Act(s, answer(0));                                // (the addon never sends it for a payoff)
            ExpectNotSaw(s, "TE^fail");
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 974); });
            ExpectNotSaw(s, "already searched");
            Act(s, answer(1));
            Expect(s, "levered open: paid", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"] + 500; });
            Act(s, [](Ctx&) { RemoveTestTreat(974); });
            Act(s, [](Ctx& c) { AddTestTreat(973, Bot(c), "mosaic", 15, 500); });      // (the third set: a payoff too)
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 973); });
            Expect(s, "a payoff is never hard, whatever its DC", [lastGame](Ctx&) { return lastGame().rfind("TG^973^mosaic^0^", 0) == 0; });
            Act(s, answer(0));
            ExpectNotSaw(s, "TE^fail");
            Act(s, [](Ctx& c) { AddTestTreat(972, Bot(c), "crates", 15, 500); });      // and a puzzle from it
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 972); });
            Expect(s, "the crates, hard", [lastGame](Ctx&) { return lastGame().rfind("TG^972^crates^1^", 0) == 0; });
            Act(s, answer(0));
            ExpectSaw(s, "TE^fail");
            Act(s, [](Ctx&) { RemoveTestTreat(973); RemoveTestTreat(972); });
        });
        add("treat.passive_miss_is_shown_quietly", [=](Script& s)
        {
            AddBot(s, wiz1);
            Rig(s, { 1, 1 });
            Act(s, [](Ctx& c) { sMsgs.clear(); AddTestTreat(998, Bot(c), "", 12, 300, true); });
            s.Until([](Ctx&) { return Saw("TR^"); }, "the passive roll", 3000);
            ExpectSaw(s, "nothing catches your eye");
            Expect(s, "the TR says it failed", [](Ctx&)
            {
                for (std::string const& m : sMsgs)
                    if (m.rfind("TR^", 0) == 0)
                        return m.size() > 2 && m.substr(m.size() - 2) == "^0";
                return false;
            });
            Act(s, [](Ctx&) { RemoveTestTreat(998); });
        });
        add("treat.passive_find", [=](Script& s)
        {
            AddBot(s, wiz1);
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); AddTestTreat(999, Bot(c), "", 12, 300, true); });
            s.Until([](Ctx&) { return Saw("TR^"); }, "the passive roll", 3000);
            ExpectSaw(s, "you notice something");
            Expect(s, "the TR says it passed", [](Ctx&)
            {
                for (std::string const& m : sMsgs)
                    if (m.rfind("TR^", 0) == 0)
                        return m.substr(m.size() - 2) == "^1";
                return false;
            });
            Act(s, [](Ctx&) { RemoveTestTreat(999); });
        });
        add("treat.a_find_can_teach_a_skill", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "Survival before: WIS 1", [](Ctx& c) { return SkillBonusByName(Bot(c), "Survival"); }, 1);
            Act(s, [](Ctx& c) { AddTestTreat(980, Bot(c), "", 12, 0); SetTestTreat(980, "", "", "Survival", 0); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 980); });
            ExpectSaw(s, "Proficiency: Survival");
            ExpectEq(s, "Survival after: WIS 1 + proficiency 2", [](Ctx& c) { return SkillBonusByName(Bot(c), "Survival"); }, 3);
            Act(s, [](Ctx& c) { HandleClientMessage(Bot(c), "build^8,14,13,15,12,10^3^2^Sage^Insight,Medicine"); });
            ExpectEq(s, "and it survives a rebuild", [](Ctx& c) { return SkillBonusByName(Bot(c), "Survival"); }, 3);
            Act(s, [](Ctx&) { RemoveTestTreat(980); });
        });
        add("treat.song_is_hard_until_you_learn_the_air", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                AddTestTreat(981, Bot(c), "", 12, 0);
                SetTestTreat(981, "test_air", "", "", 0);
                AddTestTreat(982, Bot(c), "song", 12, 500);
                SetTestTreat(982, "", "test_air", "", 0);
                AddTestTreat(983, Bot(c), "song", 12, 500);
                SetTestTreat(983, "", "test_air", "", 0);
            });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 982); });
            Expect(s, "never heard the air: the hard puzzle", [lastGame](Ctx&) { return lastGame().rfind("TG^982^song^1^", 0) == 0; });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { UseTestTreat(Bot(c), 981); });          // the treat that teaches it
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 983); });
            Expect(s, "having learned it: the stones play it first", [lastGame](Ctx&) { return lastGame().rfind("TG^983^song^0^", 0) == 0; });
            Act(s, [](Ctx&) { RemoveTestTreat(981); RemoveTestTreat(982); RemoveTestTreat(983); });
        });
        add("treat.harder_puzzles_for_higher_dcs", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(984, Bot(c), "lock", 15, 500); });
            Rig(s, { 20, 20 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 984); });
            Expect(s, "a DC 15 lock is the hard lock", [lastGame](Ctx&) { return lastGame().rfind("TG^984^lock^1^", 0) == 0; });
            Act(s, [](Ctx&) { RemoveTestTreat(984); });
        });
        add("magic.plus_one_weapon", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 3103); });      // Coldridge Hammer +1: 2d6
            ExpectEq(s, "its +1", [](Ctx& c) { return MagicBonus(Bot(c), EQUIPMENT_SLOT_MAINHAND); }, 1);
            MobOpt tough;
            tough.st.ac = 12;
            AddMob(s, tough);
            StartFight(s);
            Rig(s, { 6, 6 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 12, 0, "6 + 5 + 1 = 12 hits AC 12 only with the +1; 2d6 (4 + 4) + STR 3 + 1");
        });
        add("magic.plus_one_bow", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 3039); });
            MobOpt target;
            target.dx = 8.f;
            AddMob(s, target);
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectDrop(s, 8, 0, "1d6 (4) + DEX 3 + 1");
        });
        add("magic.ring_of_protection", [=](Script& s)
        {
            AddBot(s, wiz1);
            ExpectEq(s, "AC without", [](Ctx& c) { return ArmorClass(Bot(c)); }, 12);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_FINGER1, 1116); });
            ExpectEq(s, "AC with the ring", [](Ctx& c) { return ArmorClass(Bot(c)); }, 13);
        });
        add("progress.an_under_level_zone_warns_but_lets_you_in", [=](Script& s)
        {
            // a level-1 character in the Wetlands (a level-4 zone): not moved, not phased behind a gate, warned once
            BotOpt o{ FIGHTER, 1, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -3824.0f;
            o.y = -830.0f;
            AddBot(s, o);
            Act(s, [](Ctx&) { sMsgs.clear(); });
            s.Wait(2000);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, p->GetExactDist2d(-3824.0f, -830.0f) < 3.f, "still standing where they arrived");
                Check(c, Saw("Wetlands is a level 4 zone"), "the under-level warning");
                Check(c, Saw("V^Wetlands: level 4"), "the warning banner");
                Check(c, !Saw("requires level"), "not turned back");
                CheckEq(c, int(p->GetPhaseMask() & (1u << 5)), 0, "no Wetlands gate phase");
                sMsgs.clear();
            });
            s.Wait(1000);
            Act(s, [](Ctx& c) { Check(c, !Saw("is a level 4 zone"), "warned once, not every tick"); });
        });
        add("progress.a_zone_not_built_yet_still_turns_you_back", [=](Script& s)
        {
            // the Burning Steppes (level 99, nothing built): a character inside is turned back
            BotOpt o{ FIGHTER, 1, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -8100.0f;
            o.y = -1700.0f;
            AddBot(s, o);
            Act(s, [](Ctx&) { sMsgs.clear(); });
            s.Wait(2000);
            Act(s, [](Ctx& c) { AckTeleport(Bot(c)); });     // a new bot has no trail: sent home, a near teleport
            s.Wait(300);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, Saw("Burning Steppes requires level 99"), "the zone line turns them back");
                Check(c, p->GetMapId() != 0 || p->GetExactDist2d(-8100.0f, -1700.0f) > 5.f, "moved out of the zone");
                Check(c, (p->GetPhaseMask() & (1u << 11)) != 0, "a closed zone's gate phase stays on");
            });
        });
        // a bot beside a town's innkeeper: the town's own inn lines (zone-scoped roles, tools/townsfolk.py)
        auto innTalk = [](Script& s, float x, float y, float z, uint32 keeper, std::vector<std::string> any, std::string what)
        {
            BotOpt o{ FIGHTER, 4, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = x;
            o.y = y;
            o.z = z;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [keeper, any, what](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> inn;
                p->GetCreatureListWithEntryInGrid(inn, keeper, 30.f);
                Check(c, !inn.empty(), "the innkeeper is at the inn");
                if (inn.empty())
                    return;
                Creature* k = inn.front();
                Check(c, IsTownsfolk(k), "the town's folk talk");
                sMsgs.clear();
                Check(c, StartTownTalk(p, k), "the talk opens");
                std::string all;
                for (std::string const& m : sMsgs)
                    all += m + "\n";
                bool found = false;
                for (std::string const& a : any)
                    found = found || all.find(a) != std::string::npos;
                Check(c, found, what);
                CloseDialogue(p, false);
            });
        };
        add("town.stormwind_under_the_lord_protector", [=](Script& s)
        {
            // Innkeeper Allison at the Gilded Rose: Stormwind's own inn lines (either greeting)
            innTalk(s, -8865.8f, 673.7f, 98.0f, 6740, { "Lady", "Brotherhood" }, "a Stormwind greeting, the Lady and all");
            Act(s, [](Ctx& c)
            {
                std::list<Creature*> gone;
                Bot(c)->GetCreatureListWithEntryInGrid(gone, 29611, 400.f);     // King Varian: sailed north
                Check(c, gone.empty(), "the king isn't on his throne");
            });
        });
        add("town.goldshire_speaks_like_goldshire", [=](Script& s)
        {
            // Innkeeper Farley at the Lion's Pride: Elwynn's own inn lines (either greeting)
            innTalk(s, -9460.7f, 16.2f, 57.0f, 295, { "Lion's Pride", "Brotherhood" }, "a Goldshire greeting");
        });
        add("town.menethil_villagers_speak_like_menethil", [=](Script& s)
        {
            // a bot in Menethil Harbor next to the innkeeper: the harbour's own lines, not Dun Morogh's
            BotOpt o{ FIGHTER, 1, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -3824.0f;
            o.y = -830.0f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> inn;
                p->GetCreatureListWithEntryInGrid(inn, 1464, 40.f);      // Innkeeper Helbrek
                Check(c, !inn.empty(), "the innkeeper is standing in the inn");
                if (inn.empty())
                    return;
                Creature* keeper = inn.front();
                Check(c, IsTownsfolk(keeper), "Menethil's villagers talk");
                sMsgs.clear();
                Check(c, StartTownTalk(p, keeper), "the talk opens");
                std::string all;
                for (std::string const& m : sMsgs)
                    all += m + "\n";
                Check(c, all.find("out of the wet") != std::string::npos || all.find("Just Menethil") != std::string::npos,
                    "an innkeeper's Menethil greeting");
                Check(c, all.find("letting the winter in") == std::string::npos && all.find("just been swept") == std::string::npos,
                    "not the Dun Morogh innkeeper's");
                CloseDialogue(p, false);
            });
        });
        add("town.refuge_pointe_villagers_speak_like_refuge_pointe", [=](Script& s)
        {
            // a bot beside Refuge Pointe's general goods seller: the camp's own merchant lines
            BotOpt o{ FIGHTER, 5, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -1273.0f;
            o.y = -2535.0f;
            o.z = 21.7f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> seller;
                p->GetCreatureListWithEntryInGrid(seller, 2808, 40.f);   // Vikki Lonsav, General Goods
                Check(c, !seller.empty(), "the general goods seller is at her stall");
                if (seller.empty())
                    return;
                Creature* v = seller.front();
                Check(c, IsTownsfolk(v), "Refuge Pointe's villagers talk");
                sMsgs.clear();
                Check(c, StartTownTalk(p, v), "the talk opens");
                std::string all;
                for (std::string const& m : sMsgs)
                    all += m + "\n";
                Check(c, all.find("up the hill") != std::string::npos || all.find("over the bridge") != std::string::npos,
                    "a Refuge Pointe merchant's greeting");
                CloseDialogue(p, false);
            });
        });
        add("town.thorium_point_smiths_speak_like_the_brotherhood", [=](Script& s)
        {
            // a bot at the Thorium Brotherhood's forge: the camp's own smith lines, not Dun Morogh's
            BotOpt o{ FIGHTER, 5, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -6519.0f;
            o.y = -1186.0f;
            o.z = 309.4f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> smith;
                p->GetCreatureListWithEntryInGrid(smith, 14624, 40.f);   // Master Smith Burninate
                Check(c, !smith.empty(), "the master smith is at the forge");
                if (smith.empty())
                    return;
                Creature* b = smith.front();
                Check(c, IsTownsfolk(b), "Thorium Point's folk talk");
                sMsgs.clear();
                Check(c, StartTownTalk(p, b), "the talk opens");
                std::string all;
                for (std::string const& m : sMsgs)
                    all += m + "\n";
                Check(c, all.find("Mind the forge") != std::string::npos || all.find("Thorium steel") != std::string::npos,
                    "a Brotherhood smith's greeting");
                CloseDialogue(p, false);
            });
        });
        add("town.ironforge_under_the_regency", [=](Script& s)
        {
            // a bot at the Stonefire Tavern: Ironforge's own inn lines; and the city is cleared out around them
            BotOpt o{ FIGHTER, 3, "", { "style:Defense" } };
            o.at = true;
            o.map = 0;
            o.x = -4976.0f;                // beside the bartender: > 12 m from the brawl, so its scene doesn't open
            o.y = -1288.0f;
            o.z = 502.1f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> bar, gone;
                p->GetCreatureListWithEntryInGrid(bar, 5570, 40.f);      // Bruuk Barleybeard, bartender (kept)
                p->GetCreatureListWithEntryInGrid(gone, 15457, 60.f);    // Huntress Swiftriver (cleared out)
                Check(c, gone.empty(), "the city's extra NPCs are gone");
                Check(c, !bar.empty(), "the Stonefire's bartender is still behind his bar");
                if (bar.empty())
                    return;
                Creature* b = bar.front();
                Check(c, IsTownsfolk(b), "Ironforge's folk talk");
                sMsgs.clear();
                Check(c, StartTownTalk(p, b), "the talk opens");
                std::string all;
                for (std::string const& m : sMsgs)
                    all += m + "\n";
                Check(c, all.find("regent") != std::string::npos || all.find("Dark Iron") != std::string::npos ||
                    all.find("Anvilrage") != std::string::npos,                       // either of the two greetings
                    "an Ironforge greeting, regency and all");
                CloseDialogue(p, false);
            });
        });
        add("log.long_lines_split_between_words_and_keep_their_colour", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                std::string text = "|cffe8d8a8";
                for (int i = 0; i < 60; ++i)
                    text += "frozen claws ";
                text += "end.|r";
                std::vector<std::string> parts = LogChunks(text);
                Check(c, parts.size() >= 3, "a long line comes in several pieces");
                for (std::size_t i = 0; i < parts.size(); ++i)
                {
                    Check(c, parts[i].size() + 5 <= 254, Acore::StringFormat("piece {} fits a packet", i));
                    Check(c, parts[i].rfind("|cffe8d8a8", 0) == 0, Acore::StringFormat("piece {} starts in the colour", i));
                    Check(c, parts[i].size() >= 2 && parts[i].substr(parts[i].size() - 2) == "|r", Acore::StringFormat("piece {} closes it", i));
                    std::string body = parts[i].substr(10, parts[i].size() - 12);
                    Check(c, body.rfind("frozen", 0) == 0 || body.rfind("claws", 0) == 0, Acore::StringFormat("piece {} starts on a whole word", i));
                }
                sMsgs.clear();
                Log(Bot(c), text);
                Check(c, Saw("L^") && Saw("L+^"), "sent as a line and its continuations");
            });
        });

        // ============================================================ two-weapon fighting
        add("common.offhand_attack", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2139); });
            Expect(s, "a dagger in each hand", [](Ctx& c) { return Bot(c)->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND) != nullptr; });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_OFFHAND_ATTACK);
            ExpectRefused(s, "Attack action");
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "main hand: 4 + DEX 3");
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_OFFHAND_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 4, 0, "off hand: 4, no DEX on the damage");
            Expect(s, "a Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed; });
        });
        add("common.offhand_attack_needs_two_weapons", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2362); });   // sword + shield
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            Cast(s, SPELL_OFFHAND_ATTACK);
            ExpectRefused(s, "in each hand");
        });
        add("common.offhand_attack_carries_sneak_attack", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 2, 2 }, 4);
            Cast(s, SPELL_ATTACK);                   // a miss: no Sneak Attack spent
            Act(s, [](Ctx& c) { BotC(c)->hidden = true; });
            Rig(s, { 3, 15, 4, 5 }, 1);
            Cast(s, SPELL_OFFHAND_ATTACK);
            ExpectSaw(s, "Sneak Attack");
            ExpectDrop(s, 9, 0, "off hand 4 + Sneak Attack 5");
        });
        add("fighter.style_two_weapon_fighting", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1, "", { "style:Two-Weapon Fighting" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_OFFHAND_ATTACK);
            ExpectDrop(s, 7, 0, "with the style: 4 + STR 3 on the off-hand too");
        });
        add("caster.no_offhand_attack", [=](Script& s)
        {
            AddBot(s, wiz1);
            Expect(s, "a wizard doesn't get it", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_OFFHAND_ATTACK); });
            AddBot(s, { ROGUE, 1 });
            Expect(s, "a rogue does", [](Ctx& c) { return Bot(c, 1)->HasSpell(SPELL_OFFHAND_ATTACK); });
        });
        add("treat.puzzle_easy_on_a_pass", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(994, Bot(c), "runes", 12, 700); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 994); });
            Expect(s, "the easy runes", [lastGame](Ctx&) { return lastGame().rfind("TG^994^runes^0^", 0) == 0; });
            Act(s, answer(1));
            ExpectEq(s, "solved: the reward", [](Ctx& c) { return float(Bot(c)->GetMoney()) - c.v["money"]; }, 700);
            Act(s, [](Ctx&) { RemoveTestTreat(994); });
        });
        add("treat.game_answer_is_checked", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { AddTestTreat(995, Bot(c), "geode", 5, 900); c.v["money"] = float(Bot(c)->GetMoney()); });
            Rig(s, { 15, 15 });
            Act(s, [](Ctx& c) { sMsgs.clear(); UseTestTreat(Bot(c), 995); });
            Act(s, [lastGame](Ctx& c) { TreatGameResult(Bot(c), "995^12345^1"); });
            Expect(s, "a wrong nonce pays nothing", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                c.v["x"] = p->GetPositionX();
                p->Relocate(p->GetPositionX() + 40.f, p->GetPositionY(), p->GetPositionZ());
            });
            Act(s, answer(1));
            Expect(s, "40 m away pays nothing", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
            Act(s, [](Ctx& c) { Player* p = Bot(c); p->Relocate(c.v["x"], p->GetPositionY(), p->GetPositionZ()); sMsgs.clear(); UseTestTreat(p, 995); });
            Expect(s, "back again: the game reopens without a new roll", [lastGame](Ctx&) { return lastGame().rfind("TG^995^geode^0^", 0) == 0; });
            Act(s, answer(1));
            ExpectEq(s, "now it pays", [](Ctx& c) { return float(Bot(c)->GetMoney()) - c.v["money"]; }, 900);
            Act(s, [](Ctx&) { RemoveTestTreat(995); });
        });

        // ============================================================ tavern games (DndTavern.cpp)
        // a table with a summoned villager, the bot flush with coin
        auto table = [](Script& s, std::string games)
        {
            AddMob(s, { 2.f, 0.f });
            s.Do([games](Ctx& c)
            {
                Player* p = Bot(c);
                p->SetMoney(100000);
                c.v["money"] = 100000.f;
                sMsgs.clear();
                OpenTavern(p, Mob(c), games);
            });
        };
        auto tav = [](std::string msg) { return [msg](Ctx& c) { sMsgs.clear(); TavernMessage(Bot(c), msg); }; };
        auto money = [](Script& s, std::string what, double delta)
        {
            ExpectEq(s, what, [](Ctx& c) { return double(Bot(c)->GetMoney()) - c.v["money"]; }, delta);
        };
        // the dice queue for dealing these cards (Draw picks index D(deck size) - 1 from a deck in order)
        auto deal = [](std::vector<int> cards)
        {
            std::vector<int> deck(52), rolls;
            for (int i = 0; i < 52; ++i)
                deck[i] = i;
            for (int c : cards)
            {
                auto it = std::find(deck.begin(), deck.end(), c);
                rolls.push_back(int(it - deck.begin()) + 1);
                deck.erase(it);
            }
            return rolls;
        };
        add("tavern.opens_with_the_games_on_offer", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice,cards");
            Expect(s, "GO with the games and a full purse", [](Ctx&) { return Saw("GO^") && Saw("^dice,cards^3000^"); });
        });
        add("tavern.highroll_pair_beats_a_total", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            Rig(s, { 2, 3, 4, 5, 5, 1 });
            Act(s, tav("play^dice^500"));
            money(s, "the stake is on the table", -500);
            Act(s, tav("act^stand"));
            money(s, "a pair of 5s beats 9: stake back + 5 gp", 500);
            ExpectSaw(s, "GS^dice^500^done^");
        });
        add("tavern.highroll_reroll_and_push", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            Rig(s, { 6, 6, 6, 1, 2, 3, 6, 6, 6 });
            Act(s, tav("play^dice^100"));
            Act(s, tav("act^reroll^7"));
            money(s, "triple 6s each: a push", 0);
            Act(s, tav("act^reroll^7"));
            ExpectNotSaw(s, "GS^");
        });
        add("tavern.twenty_one_natural_pays_3_to_2", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "cards");
            Rig(s, deal({ 48, 32, 0, 24 }));                 // you: King, Ace; the dealer: 9, 7
            Act(s, tav("play^cards^100"));
            money(s, "natural 21: +1.5 gp", 150);
        });
        add("tavern.twenty_one_dealer_stands_on_17", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "cards");
            Rig(s, deal({ 36, 37, 20, 24 }));                // you: 10, 6; the dealer: 10, 7
            Act(s, tav("play^cards^100"));
            Act(s, tav("act^stand"));
            money(s, "16 against 17: lost", -100);
        });
        add("tavern.twenty_one_dealer_draws_and_busts", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "cards");
            Rig(s, deal({ 36, 37, 32, 20, 38 }));            // you: 10, 9; the dealer: 10, 6, then draws a 10
            Act(s, tav("play^cards^100"));
            Act(s, tav("act^stand"));
            money(s, "the dealer busts on 26", 100);
        });
        add("tavern.twenty_one_bust", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "cards");
            Rig(s, deal({ 36, 37, 20, 24, 38 }));            // you: 10, 6, hit a 10
            Act(s, tav("play^cards^100"));
            Act(s, tav("act^hit"));
            money(s, "26: bust", -100);
        });
        add("tavern.drinking_contest_under_the_table", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "drink");
            Act(s, tav("play^drink^100"));
            Rig(s, { 1, 1, 20, 20, 1, 1, 20, 20, 1, 1, 20, 20 });
            Act(s, tav("act^drink"));
            Expect(s, "a failed save: drunk", [](Ctx& c) { return Bot(c)->GetDrunkValue() > 0; });
            Act(s, tav("act^drink"));
            Act(s, tav("act^drink"));
            money(s, "three failed saves: lost", -100);
            ExpectSaw(s, "GS^drink^100^done^3^3^0^");
        });
        add("tavern.drinking_contest_win", [=](Script& s)
        {
            AddBot(s, { DWARF, 1, "", { "style:Defense" } });
            table(s, "drink");
            Act(s, tav("play^drink^1000"));
            Rig(s, { 20, 20, 1, 1, 20, 20, 1, 1, 20, 20, 1, 1 });
            Act(s, tav("act^drink"));
            Act(s, tav("act^drink"));
            Act(s, tav("act^drink"));
            money(s, "the villager folds first: +10 gp", 1000);
        });
        add("tavern.caught_cheating", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            Rig(s, { 2, 3, 4, 1, 2, 4 });
            Act(s, tav("play^dice^100"));
            Rig(s, { 1, 1 });
            Act(s, tav("act^cheat^0"));
            ExpectSaw(s, "catches you");
            money(s, "the wager is forfeit", -100);
            Act(s, tav("play^dice^100"));
            ExpectNotSaw(s, "GS^");
            money(s, "and they won't play again", -100);
        });
        add("tavern.palmed_six", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            Rig(s, { 2, 3, 4, 1, 2, 6 });
            Act(s, tav("play^dice^100"));
            Rig(s, { 20, 20 });
            Act(s, tav("act^cheat^0"));
            Act(s, tav("act^stand"));
            money(s, "a palmed six makes a pair of 6s: won", 100);
        });
        add("tavern.purse_runs_dry", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            for (int i = 0; i < 3; ++i)
            {
                Rig(s, { 1, 2, 4, 6, 6, 6 });
                Act(s, tav("play^dice^1000"));
                Act(s, tav("act^stand"));
            }
            money(s, "three wins of 10 gp", 3000);
            Act(s, tav("play^dice^1000"));
            ExpectSaw(s, "cleaned me out");
            money(s, "no fourth game", 3000);
        });
        add("tavern.walking_away_forfeits_the_hand", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "cards");
            Rig(s, deal({ 36, 37, 20, 24 }));
            Act(s, tav("play^cards^500"));
            Act(s, tav("leave"));
            money(s, "the stake stays on the table", -500);
            ExpectSaw(s, "GX");
        });
        add("tavern.too_far_from_the_table", [=](Script& s)
        {
            AddBot(s, wiz1);
            table(s, "dice");
            Act(s, [](Ctx& c) { Player* p = Bot(c); p->Relocate(p->GetPositionX() - 20.f, p->GetPositionY(), p->GetPositionZ()); });
            Act(s, tav("play^dice^100"));
            ExpectSaw(s, "GX");
            money(s, "nothing wagered", 0);
        });

        // ============================================================ weather (DndWeather.cpp) - the bots' own zone
        auto weather = [](Script& s, WeatherKind k)
        {
            s.Do([k](Ctx& c)
            {
                Player* p = Bot(c);
                c.v["wzone"] = float(p->GetZoneId());
                SetWeather(p->GetMapId(), p->GetZoneId(), k, true);
            });
        };
        add("weather.outdoors_here", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_SNOWFALL);
            Expect(s, "the Shimmering Flats are outdoors, and it's snowing on the bot", [](Ctx& c) { return WeatherAt(Bot(c)) == WEATHER_SNOWFALL; });
        });
        add("weather.drenched_lightning_double_fire_half", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            weather(s, WEATHER_DOWNPOUR);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            ExpectEq(s, "movement in the mud: 7.5 - 1.5", [](Ctx& c) { return BotC(c)->moveMax; }, 6.0);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_CHROMATIC_LIGHT);
            ExpectSaw(s, "Downpour: -2");
            ExpectDrop(s, 24, 0, "lightning 3d8 (12), drenched: 24");
            NextTurn(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_CHROMATIC_FIRE);
            ExpectDrop(s, 6, 0, "fire 12, drenched: 6");
        });
        add("weather.snowfall_slows_a_gnome", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_SNOWFALL);
            AddMob(s, {});
            StartFight(s);
            ExpectEq(s, "gnome in snow: 7.5 - 1.5", [](Ctx& c) { return BotC(c)->moveMax; }, 6.0);
        });
        add("weather.snowfall_doesnt_slow_a_dwarf", [=](Script& s)
        {
            AddBot(s, { DWARF, 1, "", { "style:Defense" } });
            weather(s, WEATHER_SNOWFALL);
            AddMob(s, {});
            StartFight(s);
            ExpectEq(s, "a dwarf in snow: 7.5", [](Ctx& c) { return BotC(c)->moveMax; }, 7.5);
        });
        add("weather.flurries_feed_frost", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_FLURRIES);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_RAY_OF_FROST);
            ExpectDrop(s, 4, 0, "1d8 (3) + 1 in the flurries");
        });
        add("weather.fog_far_ranged_disadvantage", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_FOG);
            AddMob(s, { 12.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectSaw(s, "fog: disadvantage");
        });
        add("weather.fog_near_is_fine", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_FOG);
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectNotSaw(s, "fog: disadvantage");
        });
        add("weather.gale", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_GALE);
            ExpectEq(s, "jump 4 x 1.5 with the wind", [](Ctx& c) { return JumpDistance(Bot(c)); }, 6.0);
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FIRE_BOLT);
            ExpectSaw(s, "gale: disadvantage");
        });
        add("weather.blizzard_bites_each_round", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            weather(s, WEATHER_BLIZZARD);
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { Remember(c); sMsgs.clear(); });
            Rig(s, {}, 1);                                   // every save fails, every die a 1 (the claw misses on a 1)
            NextTurn(s, true);
            ExpectSaw(s, "The blizzard bites");
            ExpectEq(s, "the bot: 1d4 (1) + 3 blizzard cold", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 4);
            ExpectDrop(s, 4, 0, "the creature too");
        });
        add("weather.thunderstorm_lightning", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            weather(s, WEATHER_THUNDERSTORM);
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { Remember(c); sMsgs.clear(); });
            Rig(s, {}, 20);                                  // the strike comes, it hits the last in line, who saves
            NextTurn(s, true);
            ExpectSaw(s, "Lightning strikes");
            ExpectDrop(s, 16, 0, "2d8 (16) halved, then doubled for the drenched");
        });
        add("weather.buff_follows_the_weather", [=](Script& s)
        {
            AddBot(s, wiz1);
            weather(s, WEATHER_BLIZZARD);
            s.Wait(2600);
            Expect(s, "the Blizzard buff", [](Ctx& c) { return Bot(c)->HasAura(200304); });
            weather(s, WEATHER_CLEAR);
            s.Wait(2600);
            Expect(s, "gone when it clears", [](Ctx& c) { return !Bot(c)->HasAura(200304); });
        });

        // ============================================================ fighter
        add("fighter.kit_by_level", [=](Script& s)
        {
            AddBot(s, fig1);
            Expect(s, "L1: Second Wind and Ranged Attack", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_SECOND_WIND) && Bot(c)->HasSpell(SPELL_RANGED_ATTACK); });
            Expect(s, "L1: no Action Surge yet", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_ACTION_SURGE); });
            Expect(s, "a fighter has no wizard spells", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_FIRE_BOLT) && !Bot(c)->HasSpell(SPELL_MAGIC_MISSILE); });
            Expect(s, "no spell slots", [](Ctx& c) { return SlotsMax(Bot(c), 1) == 0; });
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(2); });
            Expect(s, "L2: Action Surge", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_ACTION_SURGE); });
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(3); HandlePick(Bot(c), "subclass^Battle Master"); });
            Expect(s, "Battle Master: the three maneuvers", [](Ctx& c)
            {
                Player* p = Bot(c);
                return p->HasSpell(SPELL_TRIP_ATTACK) && p->HasSpell(SPELL_PUSHING_ATTACK) && p->HasSpell(SPELL_MENACING_ATTACK);
            });
        });
        add("fighter.champion_has_no_maneuvers", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Champion", { "style:Defense" } });
            Expect(s, "no maneuvers", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_TRIP_ATTACK); });
            ExpectEq(s, "no superiority dice", [](Ctx& c) { return SuperiorityMax(Bot(c)); }, 0);
        });
        add("fighter.numbers_by_level", [=](Script& s)
        {
            AddBot(s, fig1);
            ExpectEq(s, "L1 HP (10 + 2)", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 12);
            ExpectEq(s, "STR save (+3 +2)", [](Ctx& c) { return SaveBonus(Bot(c), STR); }, 5);
            ExpectEq(s, "CON save (+2 +2)", [](Ctx& c) { return SaveBonus(Bot(c), CON); }, 4);
            ExpectEq(s, "DEX save (+2)", [](Ctx& c) { return SaveBonus(Bot(c), DEX); }, 2);
            ExpectEq(s, "maneuver DC (8 + 2 + 3)", [](Ctx& c) { return ManeuverDC(Bot(c)); }, 13);
            ExpectEq(s, "human speed", [](Ctx& c) { return Speed(Bot(c)); }, 9.0);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(5); });
            ExpectEq(s, "L5 HP (12 + 4 x (6 + 2))", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 44);
        });
        add("fighter.second_wind", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Rig(s, {}, 5);
            Cast(s, SPELL_SECOND_WIND, -1);
            ExpectCastOk(s);
            ExpectEq(s, "HP: 1 + 1d10 (5) + level 1", [](Ctx& c) { return Bot(c)->GetHealth(); }, 7);
            Expect(s, "Second Wind costs the Bonus Action and leaves the Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            NextTurn(s);
            Cast(s, SPELL_SECOND_WIND, -1);
            ExpectRefused(s, "Second Wind is spent");
        });
        add("fighter.second_wind_back_after_short_rest", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(2); });
            Cast(s, SPELL_SECOND_WIND, -1);
            ExpectEq(s, "used", [](Ctx& c) { return GetSheet(Bot(c)).secondWindUsed; }, 1);
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "restored", [](Ctx& c) { return GetSheet(Bot(c)).secondWindUsed; }, 0);
        });
        add("fighter.action_surge", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 2, "", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_ACTION_SURGE, -1);
            ExpectRefused(s, "still have your Action");
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "longsword 1d8 (4) + 3");
            Cast(s, SPELL_ACTION_SURGE, -1);
            ExpectCastOk(s);
            Expect(s, "the Action is back", [](Ctx& c) { return !BotC(c)->actionUsed; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "the surge's attack lands");
            Cast(s, SPELL_ACTION_SURGE, -1);
            ExpectRefused(s, "Action Surge is spent");
        });
        add("fighter.action_surge_only_in_combat", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 2, "", { "style:Defense" } });
            Cast(s, SPELL_ACTION_SURGE, -1);
            ExpectRefused(s, "only do that in combat");
        });
        add("fighter.style_defense", [=](Script& s)
        {
            AddBot(s, fig1);
            ExpectEq(s, "unarmoured: 10 + 2, no Defense bonus", [](Ctx& c) { return ArmorClass(Bot(c)); }, 12);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 285); });
            ExpectEq(s, "scale mail 14 + 2 + Defense 1", [](Ctx& c) { return ArmorClass(Bot(c)); }, 17);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_OFFHAND, 2362); });
            ExpectEq(s, "and a shield + 2", [](Ctx& c) { return ArmorClass(Bot(c)); }, 19);
        });
        add("fighter.style_dueling", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1, "", { "style:Dueling" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 9, 0, "longsword 4 + 3 + Dueling 2");
        });
        add("fighter.style_dueling_not_two_handed", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1, "", { "style:Dueling" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 11, 0, "greatsword 2d6 (8) + 3, no Dueling");
        });
        add("fighter.style_great_weapon_fighting", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1, "", { "style:Great Weapon Fighting" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15, 1, 6, 2, 5 });
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 14, 0, "1 and 2 rerolled into 6 and 5, + 3");
        });
        add("fighter.no_reroll_without_the_style", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 49778); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15, 1, 2 });
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 6, 0, "1 + 2 + 3");
        });
        add("fighter.style_archery", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1, "", { "style:Archery" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 15807); });
            AddMob(s, { 10.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Rig(s, { 6, 6 }, 4);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "6 + DEX 2 + prof 2 + Archery 2 = 12 hits AC 12; 1d8 (4) + 2");
        });
        add("fighter.ranged_without_archery", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 15807); });
            AddMob(s, { 10.f, 0.f, Stats(HUMANOID, 12) });
            StartFight(s);
            Rig(s, { 6, 6 }, 4);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectDrop(s, 0, 0, "6 + 4 = 10 misses AC 12");
        });
        add("fighter.ranged_needs_a_ranged_weapon", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectRefused(s, "no ranged weapon equipped");
        });
        add("fighter.champion_improved_critical", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Champion", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, { 2.f, 0.f, Stats(HUMANOID, 30) });
            StartFight(s);
            Rig(s, { 19, 19 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "CRITICAL HIT");
            ExpectDrop(s, 11, 0, "19 crits: 2d8 (4+4) + 3");
        });
        add("fighter.nineteen_is_no_crit_otherwise", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 19, 19 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "4 + 3");
        });
        add("fighter.trip_attack", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_TRIP_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 11, 0, "longsword 7 + superiority d8 (4)");
            Expect(s, "prone (failed STR save vs 13)", [](Ctx& c) { return MobC(c)->prone; });
            ExpectEq(s, "one superiority die spent", [](Ctx& c) { return GetSheet(Bot(c)).supUsed; }, 1);
            Expect(s, "a maneuver is an Attack action", [](Ctx& c) { return BotC(c)->actionUsed; });
        });
        add("fighter.maneuver_die_only_spent_on_a_hit", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 1, 1 }, 4);
            Cast(s, SPELL_TRIP_ATTACK);
            ExpectEq(s, "no die spent on a miss", [](Ctx& c) { return GetSheet(Bot(c)).supUsed; }, 0);
            Expect(s, "not prone", [](Ctx& c) { return !MobC(c)->prone; });
        });
        add("fighter.pushing_attack", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_PUSHING_ATTACK);
            ExpectCastOk(s);
            s.Wait(1500);
            Expect(s, "driven back (> 4 m away now)", [](Ctx& c) { return Mob(c)->GetExactDist2d(Bot(c)) > 4.f; });
        });
        add("fighter.menacing_attack", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_MENACING_ATTACK);
            ExpectCastOk(s);
            Expect(s, "frightened (failed WIS save)", [](Ctx& c) { return MobC(c)->frightened; });
        });
        add("fighter.out_of_superiority_dice", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3, "Battle Master", { "style:Defense" } });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).supUsed = 4; });
            Cast(s, SPELL_TRIP_ATTACK);
            ExpectRefused(s, "No superiority dice left");
            Act(s, [](Ctx& c) { AbortEncounter(Bot(c)); });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "a short rest restores them", [](Ctx& c) { return GetSheet(Bot(c)).supUsed; }, 0);
        });
        add("fighter.extra_attack", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 5, "Champion", { "style:Defense" } });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "first swing 4 + 3");
            ExpectEq(s, "one more swing in this Attack action", [](Ctx& c) { return BotC(c)->extraAttacks; }, 1);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 7, 0, "second swing");
            Cast(s, SPELL_ATTACK);
            ExpectRefused(s, "no Action left");
        });
        add("fighter.ability_score_improvements", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 4, "Champion", { "style:Defense" } });
            Act(s, [](Ctx& c) { HandlePick(Bot(c), "asi^STR"); HandlePick(Bot(c), "asi^CON"); });
            ExpectEq(s, "L4: one improvement (STR 18)", [](Ctx& c) { return GetSheet(Bot(c)).abil[STR]; }, 18);
            ExpectEq(s, "L4: CON untouched", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 14);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(6); HandlePick(Bot(c), "asi^CON"); });
            ExpectEq(s, "L6: a second one (CON 16)", [](Ctx& c) { return GetSheet(Bot(c)).abil[CON]; }, 16);
        });
        add("fighter.heavy_armour_strength", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).abil[STR] = 13; Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 9966); });
            ExpectEq(s, "plate with STR 13 (needs 15): 9 - 3", [](Ctx& c) { return Speed(Bot(c)); }, 6.0);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).abil[STR] = 15; });
            ExpectEq(s, "plate with STR 15", [](Ctx& c) { return Speed(Bot(c)); }, 9.0);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).abil[STR] = 13; Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 9286); });
            ExpectEq(s, "chain mail with STR 13 (needs 13)", [](Ctx& c) { return Speed(Bot(c)); }, 9.0);
            ExpectEq(s, "plate AC: 18 flat + Defense", [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 9966); return ArmorClass(Bot(c)); }, 19);
        });

        // ============================================================ dwarves
        add("dwarf.toughness_and_speed", [=](Script& s)
        {
            AddBot(s, { DWARF, 1, "", { "style:Defense" } });
            ExpectEq(s, "L1 HP (10 + 2 + 1)", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 13);
            ExpectEq(s, "dwarf speed", [](Ctx& c) { return Speed(Bot(c)); }, 7.5);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).abil[STR] = 13; Equip(Bot(c), EQUIPMENT_SLOT_CHEST, 9966); });
            ExpectEq(s, "plate with STR 13: no penalty for a dwarf", [](Ctx& c) { return Speed(Bot(c)); }, 7.5);
            Act(s, [](Ctx& c) { Bot(c)->GiveLevel(5); });
            ExpectEq(s, "L5 HP (44 + 5)", [](Ctx& c) { return Bot(c)->GetMaxHealth(); }, 49);
        });
        add("dwarf.poison_damage_halved", [=](Script& s)
        {
            AddBot(s, { DWARF, 5, "Champion", { "style:Defense" } });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Remember(c);
                DamageCtx d;
                d.victim = p;
                d.amount = 10;
                d.type = DMG_POISON;
                ApplyDamage(d);
            });
            ExpectEq(s, "10 poison -> 5", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 5);
        });
        auto venomMob = []
        {
            CreatureStats st = Stats(BEAST);
            st.venomDc = 12;
            st.venom = Dice::Parse("2d4");
            return MobOpt{ 2.f, 0.f, st };
        };
        add("dwarf.venom_save_with_advantage", [=](Script& s)
        {
            AddBot(s, { DWARF, 1, "", { "style:Defense" } });
            AddMob(s, venomMob());
            StartFight(s);
            Rig(s, { 15, 15, 1, 1 }, 2);
            Act(s, [](Ctx& c) { Remember(c); sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectSaveAdv(s, "Venom", 1);
            ExpectEq(s, "claw 1 + venom 2d4 (4) halved to 2", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 3);
            ExpectEq(s, "poisoned (failed the save)", [](Ctx& c) { return BotC(c)->poisoned; }, 2);
        });
        add("dwarf.combat_training", [=](Script& s)
        {
            AddBot(s, { DWARF_WIZ, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2134); });     // Handaxe
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 9, 9 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 2, 0, "a dwarf wizard is proficient with a handaxe: 9 - 1 + 2 = 10 hits; 3 - 1");
        });
        add("dwarf.no_combat_training_for_gnomes", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2134); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 9, 9 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 0, 0, "a gnome wizard isn't: 9 - 1 = 8 misses");
        });

        // ============================================================ venom and Poisoned
        add("venom.human_full_damage_no_advantage", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, venomMob());
            StartFight(s);
            Rig(s, { 15, 15, 1, 1 }, 2);
            Act(s, [](Ctx& c) { Remember(c); sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectSaveAdv(s, "Venom", 0);
            ExpectEq(s, "claw 1 + venom 4", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 5);
        });
        add("venom.saved_half_and_not_poisoned", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, venomMob());
            StartFight(s);
            Rig(s, { 15, 15, 20, 20 }, 2);
            Act(s, [](Ctx& c) { Remember(c); CreatureAttack(Mob(c), Bot(c), false, false); });
            ExpectEq(s, "claw 1 + venom 4 halved", [](Ctx& c) { return c.v["bothp"] - Bot(c)->GetHealth(); }, 3);
            ExpectEq(s, "not poisoned", [](Ctx& c) { return BotC(c)->poisoned; }, 0);
        });
        add("venom.poisoned_attacks_at_disadvantage_then_wears_off", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2488); });
            AddMob(s, venomMob());
            StartFight(s);
            Rig(s, { 15, 15, 1, 1 }, 2);
            Act(s, [](Ctx& c) { CreatureAttack(Mob(c), Bot(c), false, false); SetTestStats(Stats(BEAST)); });
            Rig(s, { 18, 3 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "poisoned: disadvantage");
            ExpectDrop(s, 0, 0, "disadvantage keeps the 3: 3 + 5 = 8 misses AC 10");
            NextTurn(s);
            ExpectEq(s, "still poisoned through the next turn", [](Ctx& c) { return BotC(c)->poisoned; }, 1);
            Act(s, [](Ctx&) { sMsgs.clear(); });
            NextTurn(s);
            ExpectEq(s, "worn off", [](Ctx& c) { return BotC(c)->poisoned; }, 0);
            ExpectSaw(s, "no longer poisoned");
        });

        // ============================================================ barbarian, rogue, monk, sorcerer
        // The builds: barbarian (dwarf) STR 16 DEX 14 CON 14 INT 8 WIS 12 CHA 10; rogue (gnome) STR 8 DEX 16 CON 14 INT 14
        // WIS 12 CHA 10; monk (dwarf) STR 10 DEX 16 CON 14 INT 8 WIS 14 CHA 10; sorcerer (gnome) STR 8 DEX 14 CON 14
        // INT 10 WIS 12 CHA 16.
        auto hitter = [](char const* dice, DmgType type = DMG_BLUDGEONING, bool ranged = false)
        {
            MobOpt m;
            if (ranged)
            {
                m.dx = 8.f;
                m.st.ranged.name = "Test Bolt";
                m.st.ranged.bonus = 4;
                m.st.ranged.dice = Dice::Parse(dice);
                m.st.ranged.type = type;
                m.st.ranged.range = 30.f;
            }
            else
            {
                m.st.melee.dice = Dice::Parse(dice);
                m.st.melee.type = type;
            }
            return m;
        };
        auto hurt = [](Ctx& c) { return c.v["bothp"] - double(Bot(c)->GetHealth()); };
        auto creatureHits = [](Script& s, bool ranged = false)
        {
            Act(s, [ranged](Ctx& c) { Remember(c); sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), ranged, false); });
        };

        add("rules.a_creature_throws_what_its_attack_does", [=](Script& s)
        {
            // nothing in the ranged slot: piercing flies as a spear, slashing as a hand axe, bludgeoning as a rock, cold
            // as a frost bolt - each a spell whose visual carries its own missile (stock throws draw the caster's thrown
            // weapon, and creatures have none: nothing flew). (The test creatures share one stat block: one creature,
            // its attack swapped between checks.)
            AddBot(s, wiz1);
            AddMob(s, hitter("1d6", DMG_PIERCING, true));
            Act(s, [hitter](Ctx& c)
            {
                std::pair<DmgType, std::pair<uint32, char const*>> const want[] = {
                    { DMG_PIERCING, { SPELL_CREATURE_SPEAR, "piercing: a spear" } },
                    { DMG_SLASHING, { SPELL_CREATURE_AXE, "slashing: a hand axe" } },
                    { DMG_BLUDGEONING, { SPELL_CREATURE_THROW, "bludgeoning: a rock" } },
                    { DMG_COLD, { SPELL_CREATURE_FROST, "cold: a frost bolt" } } };
                for (auto const& [type, w] : want)
                {
                    SetTestStats(hitter("1d6", type, true).st);
                    uint32 id = CreatureMissile(Mob(c));
                    CheckEq(c, double(id), double(w.first), w.second);
                    SpellInfo const* si = sSpellMgr->GetSpellInfo(id);
                    Check(c, si && si->SpellVisual[0] != 0, std::string(w.second) + " - the spell exists, with a visual");
                    Check(c, IsCreatureMissile(id), std::string(w.second) + " - lands as a creature's attack");
                }
            });
        });
        add("classes.create_pairs_exist", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                for (auto [race, cls] : { std::pair{ RACE_DWARF, CLASS_SHAMAN }, { RACE_DWARF, CLASS_ROGUE }, { RACE_DWARF, CLASS_DRUID },
                         { RACE_DWARF, CLASS_WARLOCK }, { RACE_DWARF, CLASS_MAGE }, { RACE_GNOME, CLASS_SHAMAN }, { RACE_GNOME, CLASS_DRUID },
                         { RACE_GNOME, CLASS_ROGUE }, { RACE_GNOME, CLASS_WARLOCK }, { RACE_GNOME, CLASS_PRIEST },
                         { RACE_DWARF, CLASS_PRIEST }, { RACE_DWARF, CLASS_PALADIN }, { RACE_DWARF, CLASS_HUNTER },
                         { RACE_GNOME, CLASS_PALADIN }, { RACE_GNOME, CLASS_HUNTER } })
                    Check(c, sObjectMgr->GetPlayerInfo(race, cls) != nullptr,
                        Acore::StringFormat("no playercreateinfo for race {} class {}", race, cls));
            });
        });
        add("town.an_innkeeper_makes_their_inn_your_home", [=](Script& s)
        {
            // Innkeeper Belm in Kharanos: the talk offers "(Make this inn your home.)"; picking it binds the hearthstone
            // there (WoW's bind, cast by the innkeeper), and the talk stays open. Already talked: his table offers it
            BotOpt o{ FIGHTER, 1 };
            o.at = true;
            o.map = 0;
            o.x = -5599.5f;
            o.y = -531.2f;
            o.z = 399.7f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> found;
                p->GetCreatureListWithEntryInGrid(found, 1247, 30.f);
                Check(c, !found.empty(), "Belm is at his inn");
                if (found.empty())
                    return;
                Creature* k = found.front();
                Check(c, k->IsInnkeeper(), "an innkeeper");
                ClearMsgs();
                Check(c, StartTownTalk(p, k), "the talk opens");
                Check(c, SawBy(p, "^(Make this inn your home.)"), "it offers to make the inn home");
                float before = p->m_homebindX;
                Check(c, std::fabs(before - k->GetPositionX()) > 50.f, "(the bot's home starts elsewhere)");
                HandleClientMessage(p, "say^96");
                Check(c, std::fabs(p->m_homebindX - k->GetPositionX()) < 30.f &&
                    std::fabs(p->m_homebindY - k->GetPositionY()) < 30.f, "the hearthstone is bound to the inn");
                Check(c, InDialogue(p), "and the talk goes on");
                CloseDialogue(p, false);
                ClearMsgs();
                OpenTavern(p, k, "dice");
                bool inn = false;
                for (std::string const& m : MsgsBy(p, "GO^"))
                    inn = inn || (m.size() > 2 && m.substr(m.size() - 2) == "^1");
                Check(c, inn, "his table says it's an inn (the home button)");
                HandleClientMessage(p, "tav^leave");
            });
        });
        add("town.a_lost_talk_stays_lost_when_closed", [=](Script& s)
        {
            // Grundel Harkin, Anvilmar's smith: a check failed, the panel shut on the losing line (Escape) instead of
            // "Fair enough." - it's still lost: walking back up gets the parting words, not the talk again
            BotOpt o{ WIZ, 1 };
            o.at = true;
            o.map = 0;
            o.x = -6102.5f;
            o.y = 384.f;
            o.z = 395.6f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> found;
                p->GetCreatureListWithEntryInGrid(found, 1104, 30.f);
                Check(c, !found.empty(), "the smith is at his forge");
                if (found.empty())
                    return;
                Creature* k = found.front();
                ClearMsgs();
                Check(c, StartTownTalk(p, k), "the talk opens");
                int idx = -1;
                for (std::string const& m : MsgsBy(p, "DO^"))
                {
                    std::vector<std::string> f = Split(m, '^');
                    if (idx < 0 && f.size() > 3 && !f[3].empty())
                        idx = std::atoi(f[1].c_str());
                }
                Check(c, idx >= 0, "an option with a check");
                if (idx < 0)
                    return;
                ClearMsgs();
                sDice.assign({ 1, 1 });
                HandleClientMessage(p, Acore::StringFormat("say^{}", idx));
                sDice.clear();
                Check(c, InDialogue(p) && !MsgsBy(p, "DR^").empty(), "the check fails, on to the losing line");
                HandleClientMessage(p, "bye");                     // Escape
                Check(c, !InDialogue(p), "the panel shut");
                ClearMsgs();
                StartTownTalk(p, k);
                Check(c, !InDialogue(p) && !GotExact(p, "DC"), "walking back up: no second try");
                CloseDialogue(p, false);
            });
        });
        add("town.a_gryphon_master_flies_you", [=](Script& s)
        {
            // Thorgrum Borrelson in Thelsamar: a talk like any villager's, with a way to the flight map - and the point learned
            BotOpt o{ DWARF, 1 };
            o.at = true;
            o.map = 0;
            o.x = -5422.f;
            o.y = -2926.f;
            o.z = 0.f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> masters;
                p->GetCreatureListWithEntryInGrid(masters, 1572, 30.f);
                Creature* m = masters.empty() ? nullptr : masters.front();
                Check(c, m != nullptr, "the gryphon master is there");
                if (!m)
                    return;
                uint32 node = sObjectMgr->GetNearestTaxiNode(*m, p->GetTeamId(true));
                Check(c, node && !p->m_taxi.IsTaximaskNodeKnown(node), "a new dwarf doesn't know Thelsamar's flight point");
                ClearMsgs();
                Check(c, StartTownTalk(p, m), "a talk opens");
                Check(c, SawBy(p, "(Fly somewhere.)"), "with a way to fly");
                DialogueChoose(p, 97);
                Check(c, !InDialogue(p), "the talk gives way to the flight map");
                Check(c, p->m_taxi.IsTaximaskNodeKnown(node), "and the flight point is learned");
            });
        });
        add("announce.reaches_every_session", [=](Script& s)
        {
            // the real send (bots' announcements are kept by the jig instead): a system line to every session online,
            // the bots' included - it must not need a session of its own
            AddBot(s, wiz1);
            AddBot(s, fig1);
            Act(s, [](Ctx& c)
            {
                AnnounceToAll("|cffffcc00[Update]|r (a self-test line, please ignore)");
                Check(c, true, "the line went out without a session of its own");
            });
        });
        add("data.no_holidays_or_events", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                // the real calendar has no say: no Winter Veil, no Darkmoon Faire, no fishing contest, ever
                GameEventMgr::ActiveEvents const& active = sGameEventMgr->GetActiveEventList();
                std::string names;
                GameEventMgr::GameEventDataMap const& events = sGameEventMgr->GetEventMap();
                // (the core itself runs two kinds for content outside this world: the arena seasons and the Isle of
                // Quel'Danas phases - not the calendar's, and nothing of theirs stands anywhere a player can go)
                for (uint16 id : active)
                {
                    std::string d = id < events.size() ? events[id].Description : std::to_string(id);
                    if (d.rfind("Arena Season", 0) == 0 || d.rfind("Sun's Reach", 0) == 0 || d.rfind("Sun Reach", 0) == 0 ||
                        d.rfind("SWP", 0) == 0)          // (the Sunwell's gates: Quel'Danas too)
                        continue;
                    names += (names.empty() ? "" : ", ") + d;
                }
                Check(c, names.empty(), "no calendar event is running" + (names.empty() ? std::string() : " (running: " + names + ")"));
            });
        });
        add("classes.everyone_starts_in_coldridge", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                // every race and class (death knights aside) begins at the Coldridge Valley spot
                int seen = 0;
                for (uint8 race = 1; race <= RACE_DRAENEI; ++race)
                    for (uint8 cls = 1; cls < MAX_CLASSES; ++cls)
                    {
                        PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, cls);
                        if (!info || cls == CLASS_DEATH_KNIGHT)
                            continue;
                        ++seen;
                        bool ok = info->mapId == 0 && info->areaId == 1 && std::fabs(info->positionX + 6240.32f) < 1.f &&
                            std::fabs(info->positionY - 331.033f) < 1.f;
                        Check(c, ok, Acore::StringFormat("race {} class {} starts on map {} area {} at ({:.0f}, {:.0f})", race, cls,
                            info->mapId, info->areaId, info->positionX, info->positionY));
                    }
                Check(c, seen >= 40, Acore::StringFormat("only {} race/class starts found", seen));
            });
        });
        add("classes.names_and_mapping", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            AddBot(s, { ROGUE, 1 });
            AddBot(s, { MONK, 1 });
            AddBot(s, { CLERIC, 1 });
            AddBot(s, { LOCK, 1 });
            Act(s, [](Ctx& c)
            {
                char const* want[] = { "Barbarian", "Rogue", "Monk", "Cleric", "Warlock" };
                for (int i = 0; i < 5; ++i)
                {
                    Player* p = ObjectAccessor::FindPlayer(c.bots[i]);
                    Check(c, p && std::string(ClassName(p)) == want[i], std::string("class name ") + want[i]);
                }
            });
        });

        // ---------------------------------------------------------------- barbarian
        add("barbarian.hp_ac_saves", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            ExpectEq(s, "HP: d12 + CON 2 + dwarf 1", [](Ctx& c) { return MaxHp(Bot(c)); }, 15);
            ExpectEq(s, "Unarmored Defense 10 + DEX 2 + CON 2", [](Ctx& c) { return ArmorClass(Bot(c)); }, 14);
            ExpectEq(s, "STR save proficient (+3 +2)", [](Ctx& c) { return SaveBonus(Bot(c), STR); }, 5);
            ExpectEq(s, "DEX save not proficient", [](Ctx& c) { return SaveBonus(Bot(c), DEX); }, 2);
            ExpectEq(s, "two rages at level 1", [](Ctx& c) { return RageMax(Bot(c)); }, 2);
            ExpectEq(s, "no fast movement yet", [](Ctx& c) { return ClassSpeedBonus(Bot(c)); }, 0);
        });
        add("barbarian.level5", [=](Script& s)
        {
            AddBot(s, { BARB, 5, "Berserker" });
            ExpectEq(s, "HP: 14 + 4 x (7 + 2) + dwarf 5", [](Ctx& c) { return MaxHp(Bot(c)); }, 55);
            ExpectEq(s, "Fast Movement +3 m", [](Ctx& c) { return ClassSpeedBonus(Bot(c)); }, 3);
            ExpectEq(s, "three rages", [](Ctx& c) { return RageMax(Bot(c)); }, 3);
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 7, 0, "Extra Attack: the second swing 1d6 (4) + STR 3");
        });
        add("barbarian.kit_by_level", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            Expect(s, "L1: Rage, no Reckless Attack", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_RAGE) && !Bot(c)->HasSpell(SPELL_RECKLESS_ATTACK); });
            Expect(s, "no fighter or wizard actions", [](Ctx& c) { return !Bot(c)->HasSpell(SPELL_SECOND_WIND) && !Bot(c)->HasSpell(SPELL_FIRE_BOLT); });
            AddBot(s, { BARB, 3, "Berserker" });
            Expect(s, "L3 Berserker: Reckless, Frenzied Strike", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[1]);
                return p->HasSpell(SPELL_RECKLESS_ATTACK) && p->HasSpell(SPELL_FRENZIED_STRIKE);
            });
            AddBot(s, { BARB, 3, "Bear Totem" });
            Expect(s, "Bear Totem: no Frenzied Strike", [](Ctx& c) { return !ObjectAccessor::FindPlayer(c.bots[2])->HasSpell(SPELL_FRENZIED_STRIKE); });
        });
        add("barbarian.rage_damage_and_resistance", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, hitter("10"));
            Cast(s, SPELL_RAGE, -1);
            ExpectRefused(s, "combat");
            StartFight(s);
            Cast(s, SPELL_RAGE, -1);
            ExpectCastOk(s);
            Expect(s, "raging", [](Ctx& c) { return BotC(c)->raging; });
            ExpectEq(s, "one rage used", [](Ctx& c) { return GetSheet(Bot(c)).rageUsed; }, 1);
            Expect(s, "Rage took the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed; });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 9, 0, "quarterstaff 4 + STR 3 + Rage 2");
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "10 bludgeoning halved by Rage", hurt, 5);
            NextTurn(s);
            Cast(s, SPELL_RAGE, -1);
            ExpectRefused(s, "already raging");
        });
        add("barbarian.rage_does_not_resist_fire", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            AddMob(s, hitter("10", DMG_FIRE));
            StartFight(s);
            Cast(s, SPELL_RAGE, -1);
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "fire is not a weapon: full 10", hurt, 10);
        });
        add("barbarian.bear_totem_resists_everything", [=](Script& s)
        {
            AddBot(s, { BARB, 3, "Bear Totem" });
            AddMob(s, hitter("10", DMG_FIRE));
            StartFight(s);
            Cast(s, SPELL_RAGE, -1);
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "Bear Totem halves fire too", hurt, 5);
        });
        add("barbarian.reckless_attack", [=](Script& s)
        {
            AddBot(s, { BARB, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_RECKLESS_ATTACK, -1);
            ExpectCastOk(s);
            Expect(s, "Reckless is free", [](Ctx& c) { return !BotC(c)->actionUsed && !BotC(c)->bonusUsed; });
            Rig(s, { 3, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "advantage keeps the 15: 4 + STR 3");
            Rig(s, { 3, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "the creature has advantage too: 15 + 4 hits AC 14", hurt, 1);
            NextTurn(s);
            ExpectEq(s, "reckless ends at the barbarian's next turn", [](Ctx& c) { return BotC(c)->reckless; }, 0);
        });
        add("barbarian.frenzy_needs_rage", [=](Script& s)
        {
            AddBot(s, { BARB, 3, "Berserker" });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_FRENZIED_STRIKE);
            ExpectRefused(s, "only while you rage");
            Cast(s, SPELL_RAGE, -1);
            NextTurn(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_FRENZIED_STRIKE);
            ExpectCastOk(s);
            ExpectDrop(s, 9, 0, "Frenzied Strike 4 + STR 3 + Rage 2");
            Expect(s, "it took the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });

        // ---------------------------------------------------------------- rogue
        add("rogue.hp_ac_saves_expertise", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            ExpectEq(s, "HP: d8 + CON 2", [](Ctx& c) { return MaxHp(Bot(c)); }, 10);
            ExpectEq(s, "AC 10 + DEX 3", [](Ctx& c) { return ArmorClass(Bot(c)); }, 13);
            ExpectEq(s, "DEX save +5", [](Ctx& c) { return SaveBonus(Bot(c), DEX); }, 5);
            ExpectEq(s, "INT save +4", [](Ctx& c) { return SaveBonus(Bot(c), INT); }, 4);
            Expect(s, "expertise in Stealth", [](Ctx& c) { return HasExpertise(Bot(c), "Stealth"); });
            Expect(s, "no Perception expertise before 6", [](Ctx& c) { return !HasExpertise(Bot(c), "Perception"); });
            ExpectEq(s, "Sneak Attack 1d6", [](Ctx& c) { return SneakDice(Bot(c)); }, 1);
            AddBot(s, { ROGUE, 5, "Thief" });
            ExpectEq(s, "Sneak Attack 3d6 at 5", [](Ctx& c) { return SneakDice(ObjectAccessor::FindPlayer(c.bots[1])); }, 3);
        });
        add("rogue.kit_by_level", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Expect(s, "L1: Hide, no Cunning Action", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_HIDE) && !Bot(c)->HasSpell(SPELL_CUNNING_DASH); });
            AddBot(s, { ROGUE, 2 });
            Expect(s, "L2: Cunning Action", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[1]);
                return p->HasSpell(SPELL_CUNNING_DASH) && p->HasSpell(SPELL_CUNNING_DISENGAGE);
            });
        });
        add("rogue.no_sneak_attack_without_advantage", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectSaw(s, "Dagger");
            ExpectNotSaw(s, "Sneak Attack");
        });
        add("rogue.hide_then_sneak_attack", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            MobOpt distant;
            distant.dx = 8.f;
            AddMob(s, distant);
            StartFight(s);
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            ExpectCastOk(s);
            Expect(s, "hidden", [](Ctx& c) { return BotC(c)->hidden; });
            Expect(s, "Cunning Action: Hide took the Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            Rig(s, { 3, 15 }, 4);
            Cast(s, SPELL_RANGED_ATTACK);
            ExpectCastOk(s);
            ExpectSaw(s, "Sneak Attack");
            Expect(s, "attacking gave the hiding place away", [](Ctx& c) { return !BotC(c)->hidden; });
        });
        add("rogue.hide_refused_next_to_an_enemy", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_HIDE, -1);
            ExpectRefused(s, "right next to you");
        });
        add("rogue.cunning_dash", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_CUNNING_DASH, -1);
            ExpectCastOk(s);
            Expect(s, "a Bonus Action, the Action still free", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            Cast(s, SPELL_DASH, -1);
            ExpectCastOk(s);
        });
        add("rogue.thief_fast_hands", [=](Script& s)
        {
            AddBot(s, { ROGUE, 3, "Thief" });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_CUNNING_DASH, -1);
            ExpectCastOk(s);
            Cast(s, SPELL_CUNNING_DISENGAGE, -1);
            ExpectCastOk(s);
            Cast(s, SPELL_CUNNING_DASH, -1);
            ExpectRefused(s, "Bonus Action");
        });
        add("rogue.uncanny_dodge", [=](Script& s)
        {
            AddBot(s, { ROGUE, 5, "Thief" });
            AddMob(s, hitter("10"));
            StartFight(s);
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "10 halved", hurt, 5);
            Expect(s, "it used the reaction", [](Ctx& c) { return BotC(c)->reactionUsed; });
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "no reaction left: full 10", hurt, 10);
        });

        add("rogue.sneak_out_of_combat_then_ambush", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            AddMob(s, {});
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            ExpectCastOk(s);
            Expect(s, "sneaking, out of combat, with WoW's stealth look", [](Ctx& c) { return IsSneaking(Bot(c)) && !InCombat(Bot(c)) &&
                Bot(c)->HasAura(SPELL_SNEAK_AURA); });
            Rig(s, { 5, 5 }, 0);
            Expect(s, "Perception 5 + 0 misses Stealth 15 + 7 (Expertise)", [](Ctx& c) { return !SpotsSneaker(Mob(c), Bot(c)); });
            ExpectSaw(s, "+7 = 22");
            Act(s, [](Ctx& c) { Mob(c)->SetFaction(14); });
            // the strike resolves first (advantage: unseen), then the fight starts around it
            Rig(s, { 3, 15, 4, 4 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectSaw(s, ": advantage");
            ExpectSaw(s, "Sneak Attack");
            Expect(s, "the strike started the fight", [](Ctx& c) { return InCombat(Bot(c)) && !IsSneaking(Bot(c)); });
            Expect(s, "the creature that never noticed is Surprised", [](Ctx& c) { return MobC(c)->surprised; });
            Expect(s, "but the attack gave the rogue away", [](Ctx& c) { return !BotC(c)->hidden; });
            Expect(s, "the stealth look is gone in the fight", [](Ctx& c) { return !Bot(c)->HasAura(SPELL_SNEAK_AURA); });
        });
        add("rogue.sneak_attack_on_a_killing_blow", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            MobOpt frail;
            frail.st.hp = 3;
            AddMob(s, frail);
            MobOpt other;
            other.dx = 2.f;
            other.dy = 3.f;
            AddMob(s, other);
            StartFight(s);
            Act(s, [](Ctx& c) { BotC(c)->hidden = true; });
            Rig(s, { 3, 15, 4, 5 }, 1);
            Cast(s, SPELL_ATTACK);
            Expect(s, "dead", [](Ctx& c) { return !Mob(c, 0)->IsAlive(); });
            ExpectSaw(s, "Sneak Attack");
            Expect(s, "used for the turn", [](Ctx& c) { return BotC(c)->sneakUsed; });
        });
        add("rogue.silent_kill_keeps_you_sneaking", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            MobOpt frail;
            frail.st.hp = 3;
            AddMob(s, frail);
            MobOpt away;                             // a second creature 6 m off, facing away from the kill
            away.dx = 2.f;
            away.dy = 6.f;
            AddMob(s, away);
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            Rig(s, { 5, 5 }, 0);
            Act(s, [](Ctx& c)
            {
                Mob(c, 1)->SetOrientation(float(M_PI) / 2);  // looking north, away from both
                SpotsSneaker(Mob(c), Bot(c));
                Mob(c)->SetFaction(14);
                Mob(c, 1)->SetFaction(14);
            });
            Rig(s, { 3, 15, 4, 5 }, 1);
            Cast(s, SPELL_ATTACK);
            Expect(s, "killed outright: no fight", [](Ctx& c) { return !Mob(c)->IsAlive() && !InCombat(Bot(c)); });
            Expect(s, "the other faced away: no second look", [](Ctx& c) { return !Saw("saw that"); });
            Expect(s, "still sneaking, and ready to strike from hiding again", [](Ctx& c) { return IsSneaking(Bot(c)) && !SneakStruck(Bot(c)); });
        });
        add("rogue.silent_kill_witnessed", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            MobOpt frail;
            frail.st.hp = 3;
            AddMob(s, frail);
            MobOpt watcher;                          // 6 m off, facing the bot
            watcher.dx = 2.f;
            watcher.dy = 6.f;
            AddMob(s, watcher);
            Rig(s, { 5, 5 }, 0);                     // Stealth 5 + 7 = 12
            Cast(s, SPELL_HIDE, -1);
            Rig(s, { 2, 2, 2, 2 }, 0);
            Act(s, [](Ctx& c)
            {
                Mob(c, 1)->SetOrientation(Mob(c, 1)->GetAngle(Bot(c)));
                SpotsSneaker(Mob(c), Bot(c));
                SpotsSneaker(Mob(c, 1), Bot(c));
                Mob(c)->SetFaction(14);
                Mob(c, 1)->SetFaction(14);
            });
            Rig(s, { 3, 15, 4, 5, 20, 20 }, 10);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "saw that");
            Expect(s, "the watcher looked again, spotted the rogue: the fight is on", [](Ctx& c) { return InCombat(Bot(c)) && !IsSneaking(Bot(c)); });
        });
        add("rogue.sight_cone", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            MobOpt farMob;
            farMob.dx = 8.f;
            AddMob(s, farMob);
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            Act(s, [](Ctx& c) { Mob(c)->SetOrientation(0.f); sMsgs.clear(); });   // facing +x, away from the bot
            Expect(s, "behind it: no roll, not noticed", [](Ctx& c) { return !NoticesSneaker(Mob(c), Bot(c)) && !Saw("RS^"); });
            Act(s, [](Ctx& c) { Mob(c)->SetOrientation(Mob(c)->GetAngle(Bot(c))); sDice.assign({ 1, 1 }); });
            Expect(s, "in its cone at 8 m: a roll (a 1 - unnoticed)", [](Ctx& c) { return !NoticesSneaker(Mob(c), Bot(c)) && Saw("RS^"); });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Creature* m = Mob(c);
                p->Relocate(m->GetPositionX() - 2.f, m->GetPositionY(), m->GetPositionZ(), 0.f);
                p->UpdatePositionData();
            });
            Expect(s, "walking up within 3 m in front of it: seen outright", [](Ctx& c) { return NoticesSneaker(Mob(c), Bot(c)); });
            ExpectSaw(s, "walked right into");
        });
        add("rogue.sneak_spotted_starts_the_fight", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            AddMob(s, {});
            Rig(s, { 2, 2 }, 0);
            Cast(s, SPELL_HIDE, -1);
            Rig(s, { 20, 20 }, 0);
            Expect(s, "Perception 20 beats Stealth 9", [](Ctx& c) { return SpotsSneaker(Mob(c), Bot(c)); });
            Expect(s, "asked again, the same answer (one roll each)", [](Ctx& c) { sDice.assign({ 1, 1 }); return SpotsSneaker(Mob(c), Bot(c)); });
            ExpectSaw(s, "RS^");
            ExpectSaw(s, "V^SPOTTED!");
            Cast(s, SPELL_HIDE, -1);
            Expect(s, "Hide again steps out of the shadows", [](Ctx& c) { return !IsSneaking(Bot(c)); });
        });

        // ---------------------------------------------------------------- monk
        add("monk.hp_ac_ki", [=](Script& s)
        {
            AddBot(s, { MONK, 1 });
            ExpectEq(s, "HP: d8 + CON 2 + dwarf 1", [](Ctx& c) { return MaxHp(Bot(c)); }, 11);
            ExpectEq(s, "Unarmored Defense 10 + DEX 3 + WIS 2", [](Ctx& c) { return ArmorClass(Bot(c)); }, 15);
            ExpectEq(s, "no ki at 1", [](Ctx& c) { return KiMax(Bot(c)); }, 0);
            ExpectEq(s, "Ki DC 8 + 2 + WIS 2", [](Ctx& c) { return SpellSaveDC(Bot(c)); }, 12);
            AddBot(s, { MONK, 6, "Open Hand" });
            ExpectEq(s, "6 ki at 6", [](Ctx& c) { return KiMax(ObjectAccessor::FindPlayer(c.bots[1])); }, 6);
            ExpectEq(s, "Unarmored Movement +4.5 m at 6", [](Ctx& c) { return ClassSpeedBonus(ObjectAccessor::FindPlayer(c.bots[1])); }, 4.5);
        });
        add("monk.kit_by_level", [=](Script& s)
        {
            AddBot(s, { MONK, 1 });
            Expect(s, "L1: Martial Arts, no Flurry", [](Ctx& c) { return Bot(c)->HasSpell(SPELL_UNARMED_STRIKE) && !Bot(c)->HasSpell(SPELL_FLURRY); });
            AddBot(s, { MONK, 5, "Shadow" });
            Expect(s, "L5 Shadow: ki actions, Stunning Strike, Cloak, no Wholeness", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[1]);
                return p->HasSpell(SPELL_FLURRY) && p->HasSpell(SPELL_PATIENT_DEFENSE) && p->HasSpell(SPELL_STEP_OF_THE_WIND) &&
                    p->HasSpell(SPELL_STUNNING_STRIKE) && p->HasSpell(SPELL_CLOAK_OF_SHADOWS) && !p->HasSpell(SPELL_WHOLENESS);
            });
        });
        add("monk.martial_arts", [=](Script& s)
        {
            AddBot(s, { MONK, 1 });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_UNARMED_STRIKE);
            ExpectRefused(s, "Attack action");
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 6, 0, "fists: Martial Arts d4 (3) + DEX 3");
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_UNARMED_STRIKE);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "the bonus strike: d4 (3) + DEX 3");
            Expect(s, "a Bonus Action", [](Ctx& c) { return BotC(c)->bonusUsed; });
        });
        add("monk.flurry_of_blows", [=](Script& s)
        {
            AddBot(s, { MONK, 2 });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            Rig(s, { 15, 15, 3, 15, 15, 3 }, 3);
            Cast(s, SPELL_FLURRY);
            ExpectCastOk(s);
            ExpectDrop(s, 12, 0, "two strikes of 3 + 3");
            ExpectEq(s, "one ki", [](Ctx& c) { return GetSheet(Bot(c)).kiUsed; }, 1);
        });
        add("monk.open_hand_topples", [=](Script& s)
        {
            AddBot(s, { MONK, 3, "Open Hand" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            Rig(s, { 15, 15, 3, 1, 1 }, 1);
            Cast(s, SPELL_FLURRY);
            ExpectSaw(s, "Open Hand Technique");
            Expect(s, "knocked prone", [](Ctx& c) { return MobC(c)->prone; });
        });
        add("monk.stunning_strike", [=](Script& s)
        {
            AddBot(s, { MONK, 5, "Open Hand" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15, 3, 1, 1 }, 1);
            Cast(s, SPELL_STUNNING_STRIKE);
            ExpectCastOk(s);
            ExpectEq(s, "ki spent on the hit", [](Ctx& c) { return GetSheet(Bot(c)).kiUsed; }, 1);
            ExpectEq(s, "stunned", [](Ctx& c) { return MobC(c)->stunned; }, 1);
            NextTurn(s);
            ExpectEq(s, "the stun is spent with its turn", [](Ctx& c) { return MobC(c)->stunned; }, 0);
        });
        add("monk.stunning_strike_miss_costs_nothing", [=](Script& s)
        {
            AddBot(s, { MONK, 5, "Open Hand" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 2, 2 }, 2);
            Cast(s, SPELL_STUNNING_STRIKE);
            ExpectEq(s, "no ki on a miss", [](Ctx& c) { return GetSheet(Bot(c)).kiUsed; }, 0);
        });
        add("monk.patient_defense", [=](Script& s)
        {
            AddBot(s, { MONK, 2 });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_PATIENT_DEFENSE, -1);
            ExpectCastOk(s);
            Rig(s, { 15, 3 }, 0);
            creatureHits(s);
            ExpectEq(s, "disadvantage keeps the 3: 3 + 4 misses AC 15", hurt, 0);
        });
        add("monk.deflect_missiles", [=](Script& s)
        {
            AddBot(s, { MONK, 3, "Shadow" });
            AddMob(s, hitter("10", DMG_PIERCING, true));
            StartFight(s);
            Rig(s, { 15, 15, 1 }, 0);
            creatureHits(s, true);
            s.Wait(2500);                            // the bolt flies; the roll is when it lands
            ExpectEq(s, "10 - (1d10 (1) + DEX 3 + level 3)", hurt, 3);
            Expect(s, "it used the reaction", [](Ctx& c) { return BotC(c)->reactionUsed; });
        });
        add("monk.wholeness_of_body", [=](Script& s)
        {
            AddBot(s, { MONK, 6, "Open Hand" });
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(10); });
            Cast(s, SPELL_WHOLENESS, -1);
            ExpectCastOk(s);
            ExpectEq(s, "healed 3 x 6", [](Ctx& c) { return Bot(c)->GetHealth(); }, 28);
            Cast(s, SPELL_WHOLENESS, -1);
            ExpectRefused(s, "");
        });

        // ---------------------------------------------------------------- sorcerer

        // ---------------------------------------------------------------- warlock (gnome: STR 8 DEX 14 CON 14 INT 10 WIS 12
        // CHA 16 -> spell attack +5, DC 13 at level 1, AC 12 unarmoured)
        auto lockAt = [](int lvl, char const* patron, std::vector<std::string> picks = {}) { return BotOpt{ LOCK, lvl, patron, picks }; };
        auto weak = [](float dx = 2.f, float dy = 0.f)
        {
            MobOpt m;
            m.dx = dx;
            m.dy = dy;
            m.st.hp = 3;
            return m;
        };
        MobOpt distant8;
        distant8.dx = 8.f;
        add("warlock.hp_ac_saves_pact_magic", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            ExpectEq(s, "HP: d8 + CON 2", [](Ctx& c) { return MaxHp(Bot(c)); }, 10);
            ExpectEq(s, "AC 10 + DEX 2", [](Ctx& c) { return ArmorClass(Bot(c)); }, 12);
            ExpectEq(s, "WIS save +3", [](Ctx& c) { return SaveBonus(Bot(c), WIS); }, 3);
            ExpectEq(s, "CHA save +5", [](Ctx& c) { return SaveBonus(Bot(c), CHA); }, 5);
            ExpectEq(s, "spell DC 8 + 2 + CHA 3", [](Ctx& c) { return SpellSaveDC(Bot(c)); }, 13);
            ExpectEq(s, "one 1st-level pact slot", [](Ctx& c) { return SlotsMax(Bot(c), 1) * 10 + SlotsMax(Bot(c), 2); }, 10);
            AddBot(s, lockAt(3, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast", "pact:Tome" }));
            ExpectEq(s, "level 3: two 2nd-level slots, no 1st", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[1]);
                return SlotsMax(p, 1) * 10 + SlotsMax(p, 2);
            }, 2);
            AddBot(s, lockAt(5, "Fiend"));
            ExpectEq(s, "level 5: two 3rd-level slots", [](Ctx& c) { return SlotsMax(ObjectAccessor::FindPlayer(c.bots[2]), 3); }, 2);
        });
        add("warlock.kit_by_level", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            Expect(s, "L1 Fiend: the warlock spells and Burning Hands", [](Ctx& c)
            {
                Player* p = Bot(c);
                return p->HasSpell(SPELL_ELDRITCH_BLAST) && p->HasSpell(SPELL_HEX) && p->HasSpell(SPELL_ARMOR_OF_AGATHYS) &&
                    p->HasSpell(SPELL_HELLISH_REBUKE) && p->HasSpell(SPELL_ARMS_OF_HADAR) && p->HasSpell(SPELL_BURNING_HANDS);
            });
            Expect(s, "no wizard cantrips, no 2nd-level spells, no Vampiric Touch", [](Ctx& c)
            {
                Player* p = Bot(c);
                return !p->HasSpell(SPELL_FIRE_BOLT) && !p->HasSpell(SPELL_HOLD_PERSON) && !p->HasSpell(SPELL_VAMPIRIC_TOUCH) &&
                    !p->HasSpell(SPELL_MAGIC_MISSILE) && !p->HasSpell(SPELL_DISSONANT_WHISPERS);
            });
            AddBot(s, lockAt(3, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast", "pact:Tome" }));
            Expect(s, "L3: Hold Person, Misty Step, Shatter, Scorching Ray; the Tome's cantrips", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[1]);
                return p->HasSpell(SPELL_HOLD_PERSON) && p->HasSpell(SPELL_MISTY_STEP) && p->HasSpell(SPELL_SHATTER) &&
                    p->HasSpell(SPELL_SCORCHING_RAY) && p->HasSpell(SPELL_FIRE_BOLT) && !p->HasSpell(SPELL_FIREBALL);
            });
            AddBot(s, lockAt(6, "Fiend"));
            Expect(s, "L6 Fiend: Vampiric Touch, Fireball, Dark One's Own Luck", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[2]);
                return p->HasSpell(SPELL_VAMPIRIC_TOUCH) && p->HasSpell(SPELL_FIREBALL) && p->HasSpell(SPELL_DARK_ONES_LUCK);
            });
            AddBot(s, lockAt(1, "Great Old One"));
            Expect(s, "Great Old One: Dissonant Whispers, no Burning Hands", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[3]);
                return p->HasSpell(SPELL_DISSONANT_WHISPERS) && !p->HasSpell(SPELL_BURNING_HANDS) && !p->HasSpell(SPELL_FAERIE_FIRE);
            });
            AddBot(s, lockAt(1, "Archfey"));
            Expect(s, "Archfey: Faerie Fire, Fey Presence", [](Ctx& c)
            {
                Player* p = ObjectAccessor::FindPlayer(c.bots[4]);
                return p->HasSpell(SPELL_FAERIE_FIRE) && p->HasSpell(SPELL_FEY_PRESENCE);
            });
        });
        add("warlock.eldritch_blast", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ELDRITCH_BLAST);
            ExpectCastOk(s);
            ExpectDrop(s, 4, 0, "1d10 (4) Force");
            Expect(s, "a cantrip spends no slot", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1] == 0; });
        });
        add("warlock.eldritch_blast_two_beams_agonizing", [=](Script& s)
        {
            AddBot(s, lockAt(5, "Fiend", { "inv:Agonizing Blast", "inv:Armor of Shadows", "inv:Fiendish Vigour", "pact:Tome" }));
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 15, 15, 4, 15, 15, 4 }, 4);
            Cast(s, SPELL_ELDRITCH_BLAST);
            ExpectDrop(s, 14, 0, "two beams of 4 + CHA 3");
        });
        add("warlock.pact_slots_come_back_on_a_short_rest", [=](Script& s)
        {
            AddBot(s, lockAt(3, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast", "pact:Tome" }));
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            ExpectCastOk(s);
            ExpectEq(s, "a 2nd-level pact slot spent", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[2]; }, 1);
            ExpectEq(s, "Agathys at 2nd level: 10 temporary HP", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 10);
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            ExpectRefused(s, "No Pact Magic slots");
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "a short rest restores them", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[2]; }, 0);
        });
        add("warlock.hex", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, distant8);
            StartFight(s);
            Cast(s, SPELL_HEX);
            ExpectCastOk(s);
            Expect(s, "a Bonus Action and the slot", [](Ctx& c) { return BotC(c)->bonusUsed && GetSheet(Bot(c)).slotsUsed[1] == 1; });
            Expect(s, "hexed", [](Ctx& c) { return MobC(c)->hexedBy == Bot(c)->GetGUID(); });
            Rig(s, { 15, 15, 4, 3 }, 1);
            Cast(s, SPELL_ELDRITCH_BLAST);
            ExpectDrop(s, 7, 0, "Eldritch Blast 4 + Hex 3");
        });
        add("warlock.hex_moves_free_when_its_target_dies", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Great Old One"));
            AddMob(s, weak(8.f));
            AddMob(s, weak(8.f, 3.f));
            StartFight(s);
            Cast(s, SPELL_HEX);
            Rig(s, { 15, 15, 4 }, 1);
            Cast(s, SPELL_ELDRITCH_BLAST);
            Expect(s, "the hexed creature is dead", [](Ctx& c) { return !Mob(c, 0)->IsAlive(); });
            Expect(s, "Hex may move", [](Ctx& c) { return BotC(c)->hexFree; });
            Rig(s, {}, 20);                          // the creature's turn: a hit on the warlock mustn't break the hex
            NextTurn(s, true);                       // (its concentration save rolls 20 too)
            Cast(s, SPELL_HEX, 1);
            ExpectCastOk(s);
            Expect(s, "moved without a slot (the only one is spent)", [](Ctx& c) { return MobC(c, 1)->hexedBy == Bot(c)->GetGUID(); });
            Expect(s, "free only once", [](Ctx& c) { return !BotC(c)->hexFree; });
        });
        add("warlock.armor_of_agathys_bites_back", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            ExpectEq(s, "5 temporary HP", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 5);
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            ExpectEq(s, "the claw's 1 went into the temporary HP", hurt, 0);
            ExpectDrop(s, 5, 0, "5 Cold back at the attacker");
        });
        add("warlock.hellish_rebuke", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_HELLISH_REBUKE, -1);
            ExpectCastOk(s);
            Expect(s, "readied, free, no slot yet", [](Ctx& c) { return BotC(c)->rebukeReady && !BotC(c)->actionUsed &&
                GetSheet(Bot(c)).slotsUsed[1] == 0; });
            Rig(s, { 15, 15, 1, 1, 5, 5 }, 0);
            creatureHits(s);
            ExpectDrop(s, 10, 0, "2d10 (5 + 5) Fire, save failed");
            Expect(s, "the reaction and the slot are spent", [](Ctx& c) { return BotC(c)->reactionUsed && GetSheet(Bot(c)).slotsUsed[1] == 1; });
        });
        add("warlock.arms_of_hadar", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, {});
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 1, 1, 3, 3 }, 1);
            Cast(s, SPELL_ARMS_OF_HADAR, -1);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "2d6 (3 + 3) Necrotic next to you");
            ExpectDrop(s, 0, 1, "the one 8 m away is out of reach");
            Expect(s, "no reactions after a failed save", [](Ctx& c) { return MobC(c)->noReaction; });
        });
        add("warlock.dissonant_whispers", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Great Old One"));
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 1, 1, 2, 2, 2 }, 1);
            Cast(s, SPELL_DISSONANT_WHISPERS);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "3d6 (2 + 2 + 2) Psychic");
            Expect(s, "frightened", [](Ctx& c) { return MobC(c)->frightened; });
        });
        add("warlock.faerie_fire", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Archfey"));
            MobOpt rooted = distant8;
            rooted.st.speed = 0.f;                   // stays out of melee through its turn
            AddMob(s, rooted);
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_FAERIE_FIRE);
            ExpectCastOk(s);
            Expect(s, "outlined", [](Ctx& c) { return MobC(c)->outlined; });
            NextTurn(s);
            Rig(s, { 2, 15 }, 4);
            Cast(s, SPELL_ELDRITCH_BLAST);
            ExpectDrop(s, 4, 0, "advantage keeps the 15");
        });

        // ============================================================ the bard (WoW Paladin)
        add("bard.inspiration_turns_a_miss_into_a_hit", [=](Script& s)
        {
            // the fighter (STR 16: +3, proficiency +2 = +5) holds a d6; a 4 (9) misses AC 10, the die's 3 makes it 12
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c)
            {
                Player* f = Bot(c);
                Equip(f, EQUIPMENT_SLOT_MAINHAND, 35);
                AddBotAt(c, BARD, 1, f->GetPositionX() - 3.f, f->GetPositionY());
                CastAs(c, 1, SPELL_BARDIC_INSPIRATION, f);
            });
            s.Wait(600);
            Act(s, [](Ctx& c)
            {
                CheckEq(c, HeldInspiration(Bot(c)), 6, "the fighter holds a d6");
                CheckEq(c, GetSheet(Bot(c, 1)).inspirationUsed, 1, "one of the bard's uses (CHA 17: three)");
                MoveBotTo(Bot(c, 1), Bot(c)->GetPositionX() - 60.f, Bot(c)->GetPositionY());   // out of the fight to come
            });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Rig(s, { 4, 4, 3 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "Bardic Inspiration: +3 on the attack");
            ExpectDrop(s, 4, 0, "a hit: 1d6 (1) + 3");
            Expect(s, "and the die is spent", [](Ctx& c) { return HeldInspiration(Bot(c)) == 0; });
        });
        add("bard.cutting_words_spoils_a_hit", [=](Script& s)
        {
            // a Lore bard, AC 12 (no armour, DEX 14); the creature's 9 + 4 = 13 would hit - the bard's d6 (3) makes it 10
            AddBot(s, { BARD, 3, "Lore" });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { c.v["hp"] = float(Bot(c)->GetHealth()); sMsgs.clear(); });
            Rig(s, { 9, 9, 3 }, 1);
            Act(s, [](Ctx& c) { CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Wait(500);
            ExpectSaw(s, "Cutting Words");
            Expect(s, "the blow misses", [](Ctx& c) { return float(Bot(c)->GetHealth()) == c.v["hp"]; });
            Expect(s, "a die and the reaction spent", [](Ctx& c) { return GetSheet(Bot(c)).inspirationUsed == 1 && BotC(c)->reactionUsed; });
        });
        add("bard.jack_of_all_trades", [=](Script& s)
        {
            AddBot(s, { BARD, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                int one = SkillBonusByName(p, "Athletics");
                CheckEq(c, one, -1, "level 1, not proficient: STR 8 (-1)");
                p->GiveLevel(2);
                CheckEq(c, SkillBonusByName(p, "Athletics"), 0, "level 2: + half the proficiency bonus (1)");
            });
        });
        add("bard.vicious_mockery_and_its_disadvantage", [=](Script& s)
        {
            AddBot(s, { BARD, 1 });
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);                                  // the creature's WIS save fails; the d4 rolls 1
            Cast(s, SPELL_VICIOUS_MOCKERY);
            ExpectCastOk(s);
            ExpectDrop(s, 1, 0, "1d4 Psychic (1)");
            Expect(s, "mocked", [](Ctx& c) { return MobC(c)->mocked; });
            Act(s, [](Ctx& c) { sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Wait(500);
            ExpectSaw(s, "mocked: disadvantage");
            Expect(s, "and only for one attack", [](Ctx& c) { return !MobC(c)->mocked; });
        });
        add("bard.thunderwave_throws_them_back", [=](Script& s)
        {
            AddBot(s, { BARD, 1 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { c.v["d"] = Bot(c)->GetExactDist2d(Mob(c)); });
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_THUNDERWAVE, -1);
            ExpectDrop(s, 2, 0, "2d8 Thunder (1 + 1)");
            Expect(s, "and 3 m further away", [](Ctx& c) { return Bot(c)->GetExactDist2d(Mob(c)) >= c.v["d"] + 2.f; });
        });
        add("bard.hypnotic_pattern_holds_until_hurt", [=](Script& s)
        {
            AddBot(s, { BARD, 5 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            CastAt(s, SPELL_HYPNOTIC_PATTERN, 8.f, 0.f);
            Expect(s, "entranced", [](Ctx& c) { return MobC(c)->entranced && BotC(c)->concentration == SPELL_HYPNOTIC_PATTERN; });
            NextTurn(s);
            ExpectSaw(s, "entranced");
            Act(s, [](Ctx& c)
            {
                DamageCtx d;
                d.attacker = Bot(c);
                d.victim = Mob(c);
                d.amount = 1;
                d.type = DMG_FORCE;
                ApplyDamage(d);
                Check(c, !MobC(c)->entranced, "a hurt snaps it out of it");
            });
        });
        add("bard.healing_word_gets_the_fallen_up", [=](Script& s)
        {
            // the fighter goes down; the bard (initiative 15, after the fighter) heals them from 8 m: 1d4 (1) + CHA 3 = 4
            AddBot(s, { FIGHTER, 3 });
            MobOpt stuck{ 2.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* f = Bot(c);
                sDice.assign({ 15, 15 });
                Player* b = AddBotAt(c, BARD, 1, f->GetPositionX() - 8.f, f->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                sDice.clear();
                Check(c, InCombat(b), "the bard is in the fight");
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = f;
                d.amount = int(f->GetHealth());
                d.type = DMG_FORCE;
                ApplyDamage(d);
                Check(c, IsDowned(f), "the fighter is down");
            });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c, 1)); }, "the bard's turn", 8000);
            Rig(s, {}, 1);
            Act(s, [](Ctx& c) { CastAs(c, 1, SPELL_HEALING_WORD, Bot(c)); });
            s.Wait(700);
            Expect(s, "up, with 4 hit points", [](Ctx& c) { return !IsDowned(Bot(c)) && Bot(c)->GetHealth() == 4; });
        });

        // ============================================================ the druid (WoW Hunter)
        add("druid.produce_flame", [=](Script& s)
        {
            AddBot(s, { DRUID, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_PRODUCE_FLAME);
            ExpectCastOk(s);
            ExpectDrop(s, 1, 0, "1d8 Fire (1)");
        });
        add("druid.shillelagh_strikes_with_wisdom", [=](Script& s)
        {
            AddBot(s, { DRUID, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_SHILLELAGH, -1);
            ExpectCastOk(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 4, 0, "a d8 (1) + WIS 3, not STR -1");
        });
        add("druid.entangle_restrains", [=](Script& s)
        {
            AddBot(s, { DRUID, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            CastAt(s, SPELL_ENTANGLE, 8.f, 0.f);
            Expect(s, "restrained, under the druid's concentration", [](Ctx& c)
            {
                return MobC(c)->restrained && BotC(c)->concentration == SPELL_ENTANGLE;
            });
            Act(s, [](Ctx& c) { BotC(c)->actionUsed = false; });
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_PRODUCE_FLAME);
            ExpectSaw(s, "target restrained: advantage");
        });
        add("druid.moonbeam_burns_at_turn_start", [=](Script& s)
        {
            AddBot(s, { DRUID, 3 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Rig(s, { 1, 1 }, 1);                                  // every save fails, every die a 1
            CastAt(s, SPELL_MOONBEAM, 8.f, 0.f);
            ExpectDrop(s, 2, 0, "on the cast: 2d10 Radiant (1 + 1)");
            Act(s, [](Ctx& c) { Remember(c); });
            NextTurn(s, true);
            ExpectDrop(s, 2, 0, "and again as its turn starts in the light");
        });
        add("druid.call_lightning_again_without_a_slot", [=](Script& s)
        {
            AddBot(s, { DRUID, 5 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            CastAt(s, SPELL_CALL_LIGHTNING, 8.f, 0.f);
            ExpectCastOk(s);
            ExpectEq(s, "a 3rd-level slot", [](Ctx& c) { return double(GetSheet(Bot(c)).slotsUsed[3]); }, 1);
            NextTurn(s);
            CastAt(s, SPELL_CALL_LIGHTNING, 8.f, 0.f);
            ExpectCastOk(s);
            ExpectEq(s, "the second bolt: no slot", [](Ctx& c) { return double(GetSheet(Bot(c)).slotsUsed[3]); }, 1);
        });
        add("druid.wild_shape_wolf_and_the_overflow", [=](Script& s)
        {
            // a wolf: AC 13, 11 hit points in front of the druid's own; 15 damage takes the 11 and 4 of the druid's
            AddBot(s, { DRUID, 2 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_SHAPE_WOLF, -1);
            ExpectCastOk(s);
            Expect(s, "a wolf", [](Ctx& c)
            {
                Player* p = Bot(c);
                return WildShapeOf(p) && ArmorClass(p) == 13 && GetSheet(p).tempHp == 11 && Speed(p) == 12.f;
            });
            Act(s, [](Ctx& c) { BotC(c)->actionUsed = false; });
            Cast(s, SPELL_PRODUCE_FLAME);
            ExpectRefused(s, "can't cast spells in beast form");
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                uint32 hp = p->GetHealth();
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = p;
                d.amount = 15;
                d.type = DMG_FORCE;
                ApplyDamage(d);
                CheckEq(c, double(hp) - double(p->GetHealth()), 4, "15 damage: the wolf's 11, then 4 of the druid's");
                Check(c, !WildShapeOf(p), "and the druid is back in their own shape");
            });
        });
        add("druid.moon_bear_attacks_twice", [=](Script& s)
        {
            AddBot(s, { DRUID, 2, "Moon" });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_SHAPE_BEAR, -1);
            ExpectCastOk(s);
            Expect(s, "a bonus action for the Circle of the Moon", [](Ctx& c) { return BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "Claws");
            Expect(s, "a second attack to come", [](Ctx& c) { return BotC(c)->extraAttacks == 1; });
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "Bite");
        });
        add("druid.barkskin_sets_the_floor", [=](Script& s)
        {
            AddBot(s, { DRUID, 3, "Land" });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Expect(s, "AC under 16 to begin with", [](Ctx& c) { return ArmorClass(Bot(c)) < 16; });
            Cast(s, SPELL_BARKSKIN, -1);
            ExpectCastOk(s);
            ExpectEq(s, "Barkskin: 16", [](Ctx& c) { return double(ArmorClass(Bot(c))); }, 16);
        });
        add("classes.bard_and_druid_learn_their_spells", [=](Script& s)
        {
            AddBot(s, { BARD, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, p->HasSpell(SPELL_VICIOUS_MOCKERY) && p->HasSpell(SPELL_BARDIC_INSPIRATION) && p->HasSpell(SPELL_HEALING_WORD),
                    "a level 1 bard: Vicious Mockery, Bardic Inspiration, Healing Word");
                Check(c, !p->HasSpell(SPELL_FIRE_BOLT) && !p->HasSpell(SPELL_HOLD_PERSON), "and not the wizard's spells");
                p->GiveLevel(3);
                Check(c, p->HasSpell(SPELL_HOLD_PERSON) && p->HasSpell(SPELL_SHATTER), "level 3: Hold Person, Shatter");
                p->GiveLevel(5);
                Check(c, p->HasSpell(SPELL_HYPNOTIC_PATTERN), "level 5: Hypnotic Pattern");
                CheckEq(c, InspirationDie(p), 8, "and a d8 to inspire with");
            });
        });
        add("classes.druid_learns_wild_shape_and_spells", [=](Script& s)
        {
            AddBot(s, { DRUID, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, p->HasSpell(SPELL_PRODUCE_FLAME) && p->HasSpell(SPELL_ENTANGLE) && !p->HasSpell(SPELL_SHAPE_WOLF),
                    "a level 1 druid: Produce Flame, Entangle, no Wild Shape yet");
                p->GiveLevel(2);
                Check(c, p->HasSpell(SPELL_SHAPE_WOLF) && !p->HasSpell(SPELL_SHAPE_BEAR), "level 2: the wolf, not the bear (no circle)");
                CheckEq(c, WildShapeMax(p), 2, "twice a short rest");
                p->GiveLevel(3);
                Check(c, p->HasSpell(SPELL_MOONBEAM), "level 3: Moonbeam");
                p->GiveLevel(5);
                Check(c, p->HasSpell(SPELL_CALL_LIGHTNING), "level 5: Call Lightning");
            });
        });


        // ============================================================ more spells (every die a 1 unless rigged)
        add("spells.chill_touch", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_CHILL_TOUCH);
            ExpectCastOk(s);
            ExpectDrop(s, 1, 0, "1d8 Necrotic (1)");
        });
        add("spells.acid_splash_hits_two", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            AddMob(s, { 8.f, 0.f });
            AddMob(s, { 8.f, 1.2f });                             // right next to the first
            StartFight(s);
            Rig(s, { 1, 1, 1, 1, 1 }, 1);                         // both fail their DEX saves; each d6 a 1
            Cast(s, SPELL_ACID_SPLASH);
            ExpectDrop(s, 1, 0, "the target: 1d6 (1)");
            ExpectDrop(s, 1, 1, "and its neighbour: 1d6 (1)");
        });
        add("spells.poison_spray", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_POISON_SPRAY);
            ExpectDrop(s, 1, 0, "1d12 Poison (1)");
        });
        add("spells.ray_of_sickness_poisons", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15, 1, 1, 1, 1 }, 1);                   // a hit; 2d8 (1 + 1); the CON save fails
            Cast(s, SPELL_RAY_OF_SICKNESS);
            ExpectDrop(s, 2, 0, "2d8 Poison (1 + 1)");
            Expect(s, "and Poisoned", [](Ctx& c) { return MobC(c)->poisoned > 0; });
        });
        add("spells.witch_bolt_again_with_no_slot", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_WITCH_BOLT);
            ExpectDrop(s, 1, 0, "a hit: 1d12 (1)");
            ExpectEq(s, "a 1st-level slot", [](Ctx& c) { return double(GetSheet(Bot(c)).slotsUsed[1]); }, 1);
            Rig(s, {}, 1);
            NextTurn(s, true);
            Cast(s, SPELL_WITCH_BOLT);
            ExpectSaw(s, "the arc holds");
            ExpectDrop(s, 1, 0, "the arc: 1d12 (1), no attack roll");
            ExpectEq(s, "and no second slot", [](Ctx& c) { return double(GetSheet(Bot(c)).slotsUsed[1]); }, 1);
        });
        add("spells.sleep_takes_the_weakest", [=](Script& s)
        {
            // 5d8, every die a 1: 5 hit points' worth - the 5 HP creature sleeps, the 300 HP one doesn't
            AddBot(s, { WIZ, 1 });
            MobOpt frail{ 8.f, 0.f };
            frail.st = Stats(HUMANOID, 10, 5);
            AddMob(s, frail);
            AddMob(s, { 9.f, 2.f });
            StartFight(s);
            Rig(s, {}, 1);
            CastAt(s, SPELL_SLEEP, 8.f, 1.f);
            Expect(s, "the frail one sleeps", [](Ctx& c) { return MobC(c, 0)->asleep; });
            Expect(s, "the tough one doesn't", [](Ctx& c) { return !MobC(c, 1)->asleep; });
            Act(s, [](Ctx& c)
            {
                DamageCtx d;
                d.attacker = Bot(c);
                d.victim = Mob(c, 0);
                d.amount = 1;
                d.type = DMG_FORCE;
                ApplyDamage(d);
                Check(c, !MobC(c, 0)->asleep, "a hurt wakes it");
            });
        });
        add("spells.heroism_temp_hp_each_turn", [=](Script& s)
        {
            AddBot(s, { BARD, 1 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).tempHp = 0; });
            Cast(s, SPELL_HEROISM, -1);
            ExpectCastOk(s);
            NextTurn(s);
            ExpectEq(s, "at the start of the bard's turn: CHA 17, +3 temporary HP", [](Ctx& c) { return double(GetSheet(Bot(c)).tempHp); }, 3);
        });
        add("spells.cloud_of_daggers_at_turn_start", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Rig(s, {}, 1);
            CastAt(s, SPELL_CLOUD_OF_DAGGERS, 8.f, 0.f);
            ExpectDrop(s, 4, 0, "on the cast: 4d4 (four 1s)");
            Act(s, [](Ctx& c) { Remember(c); });
            NextTurn(s, true);
            ExpectDrop(s, 4, 0, "and again as its turn starts among them");
        });
        add("spells.web_restrains", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            CastAt(s, SPELL_WEB, 8.f, 0.f);
            Expect(s, "restrained", [](Ctx& c) { return MobC(c)->restrained && BotC(c)->concentration == SPELL_WEB; });
        });
        add("spells.blur", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_BLUR, -1);
            ExpectCastOk(s);
            Act(s, [](Ctx& c) { sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Wait(500);
            ExpectSaw(s, "blurred: disadvantage");
        });
        add("spells.fear_cone", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 4.f, 0.f });                              // in front: in the cone
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_FEAR, -1);
            Expect(s, "terrified", [](Ctx& c) { return MobC(c)->afraid; });
            Act(s, [](Ctx& c) { sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Wait(500);
            ExpectSaw(s, "afraid: disadvantage");
        });
        add("spells.stinking_cloud_costs_the_turn", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            CastAt(s, SPELL_STINKING_CLOUD, 8.f, 0.f);
            ExpectCastOk(s);
            Rig(s, {}, 1);
            NextTurn(s, true);
            ExpectSaw(s, "retches in the stinking cloud");
        });
        add("spells.hunger_of_hadar", [=](Script& s)
        {
            AddBot(s, { LOCK, 5 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            CastAt(s, SPELL_HUNGER_OF_HADAR, 8.f, 0.f);
            ExpectCastOk(s);
            Act(s, [](Ctx& c) { Remember(c); });
            Rig(s, {}, 1);
            NextTurn(s, true);
            ExpectDrop(s, 2, 0, "its turn starts in the dark: 2d6 Cold (1 + 1)");
        });
        add("spells.sleet_storm_trips", [=](Script& s)
        {
            AddBot(s, { WIZ, 5 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            CastAt(s, SPELL_SLEET_STORM, 8.f, 0.f);
            ExpectCastOk(s);
            Rig(s, {}, 1);
            NextTurn(s, true);
            ExpectSaw(s, "knocked |cffff8000Prone");
        });
        add("spells.who_knows_what", [=](Script& s)
        {
            AddBot(s, { WIZ, 1 });
            Act(s, [](Ctx& c)
            {
                Player* w = Bot(c);
                Check(c, w->HasSpell(SPELL_CHILL_TOUCH) && w->HasSpell(SPELL_ACID_SPLASH) && w->HasSpell(SPELL_POISON_SPRAY) &&
                    w->HasSpell(SPELL_THUNDERWAVE) && w->HasSpell(SPELL_SLEEP) && w->HasSpell(SPELL_WITCH_BOLT), "a level 1 wizard's new spells");
                Check(c, !w->HasSpell(SPELL_HEROISM) && !w->HasSpell(SPELL_HUNGER_OF_HADAR), "not the bard's or the warlock's");
                w->GiveLevel(5);
                Check(c, w->HasSpell(SPELL_HYPNOTIC_PATTERN) && w->HasSpell(SPELL_FEAR) && w->HasSpell(SPELL_SLEET_STORM) &&
                    w->HasSpell(SPELL_WEB), "level 5: Hypnotic Pattern, Fear, Sleet Storm, Web");
                Player* d = AddBotAt(c, DRUID, 1, w->GetPositionX() + 3.f, w->GetPositionY());
                Check(c, d->HasSpell(SPELL_ICE_KNIFE) && d->HasSpell(SPELL_POISON_SPRAY), "a druid: Ice Knife, Poison Spray");
                Player* l = AddBotAt(c, LOCK, 1, w->GetPositionX() - 3.f, w->GetPositionY());
                Check(c, l->HasSpell(SPELL_CHILL_TOUCH) && l->HasSpell(SPELL_WITCH_BOLT) && !l->HasSpell(SPELL_SLEEP),
                    "a warlock: Chill Touch, Witch Bolt, not Sleep");
            });
        });



        // ============================================================ special arrows and grenades
        // (a level 1 rogue: DEX 16, +3; proficient with the shortbow, +2 - a 15 hits AC 10. Grenades: DC 13 DEX, the test
        // creature has DEX 10)
        add("arrows.fire_catches", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ARROW_FIRE);
            ExpectCastOk(s);
            ExpectDrop(s, 11, 0, "1d6 (4) + DEX 3, and the arrow's 1d6 Fire (4)");
            Expect(s, "it's on fire", [](Ctx& c) { return MobC(c)->burningBy == Bot(c)->GetGUID(); });
            Rig(s, {}, 1);
            NextTurn(s, true);
            ExpectDrop(s, 12, 0, "its turn starts burning: 1d4 (1) more");
            Expect(s, "a 1 doesn't beat the flames (DC 10)", [](Ctx& c) { return !MobC(c)->burningBy.IsEmpty(); });
        });
        add("arrows.ice_slows", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ARROW_ICE);
            ExpectDrop(s, 11, 0, "1d6 (4) + 3, and 1d6 Cold (4)");
            Expect(s, "slowed", [](Ctx& c) { return MobC(c)->slowed; });
        });
        add("arrows.acid_eats_armour", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ARROW_ACID);
            ExpectDrop(s, 11, 0, "1d6 (4) + 3, and 1d6 Acid (4)");
            ExpectEq(s, "AC 10 - 2", [](Ctx& c) { return double(ArmorClass(Mob(c))); }, 8);
        });
        add("arrows.lightning_leaps", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            AddMob(s, { 8.f, 0.f });
            AddMob(s, { 8.f, 2.f });                              // 2 m from the first
            AddMob(s, { 8.f, -9.f });                             // too far for the arc
            StartFight(s);
            Rig(s, { 15, 15 }, 4);                               // (the arc's save: a 4, short of 13)
            Cast(s, SPELL_ARROW_LIGHTNING);
            ExpectDrop(s, 11, 0, "1d6 (4) + 3, and 1d6 Lightning (4)");
            ExpectDrop(s, 4, 1, "the arc: 1d6 (4), the save failed");
            ExpectDrop(s, 0, 2, "nothing for the one 9 m off");
        });
        add("arrows.slaying_only_monsters", [=](Script& s)
        {
            AddBot(s, { ROGUE, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            AddMob(s, { 8.f, 0.f, Stats(BEAST) });
            AddMob(s, { 8.f, 4.f });                              // a humanoid
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ARROW_SLAYING, 0);
            ExpectDrop(s, 15, 0, "a beast: 1d6 (4) + 3, and 2d8 (4 + 4)");
            Act(s, [](Ctx& c) { BotC(c)->actionUsed = false; });  // (another shot this turn)
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ARROW_SLAYING, 1);
            ExpectDrop(s, 7, 1, "a humanoid: just the shot");
        });
        add("arrows.need_a_bow", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_ARROW_FIRE);
            ExpectRefused(s, "need a bow or crossbow");
        });
        add("arrows.extra_attack_follows", [=](Script& s)
        {
            // an arrow is a shot of the Attack action: a level 5 fighter gets the second one
            AddBot(s, { FIGHTER, 5 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_RANGED, 2504); });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_ARROW_ICE);
            ExpectCastOk(s);
            Expect(s, "Extra Attack: one more", [](Ctx& c) { return BotC(c)->extraAttacks == 1; });
        });
        add("grenades.alchemists_fire", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 12, 12 }, 3);                               // a 12 misses DC 13
            Cast(s, SPELL_ALCHEMISTS_FIRE);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "2d4 Fire (3 + 3)");
            Expect(s, "it's on fire", [](Ctx& c) { return !MobC(c)->burningBy.IsEmpty(); });
        });
        add("grenades.acid_vial", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 3);
            Cast(s, SPELL_ACID_VIAL);
            ExpectDrop(s, 6, 0, "2d6 Acid (3 + 3)");
            ExpectEq(s, "AC 10 - 2", [](Ctx& c) { return double(ArmorClass(Mob(c))); }, 8);
        });
        add("grenades.smokepowder_half_on_a_save", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 13, 13 }, 2);                               // a 13 makes DC 13
            Cast(s, SPELL_SMOKEPOWDER);
            ExpectDrop(s, 3, 0, "3d6 (2 + 2 + 2) halved");
        });
        add("grenades.holy_water_only_the_unholy", [=](Script& s)
        {
            AddBot(s, wiz1);
            MobOpt zombie{ 6.f, 0.f };
            zombie.st = Stats(1501);
            AddMob(s, zombie);
            AddMob(s, { 7.f, 2.f });                              // a living one in the splash
            StartFight(s);
            Rig(s, {}, 3);
            Cast(s, SPELL_HOLY_WATER, 0);
            ExpectDrop(s, 6, 0, "the zombie: 2d6 Radiant (3 + 3)");
            ExpectDrop(s, 0, 1, "the living one is only splashed");
        });
        add("grenades.frost_bomb_knocks_down", [=](Script& s)
        {
            AddBot(s, wiz1);
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 12, 12 }, 2);
            Cast(s, SPELL_FROST_BOMB);
            ExpectDrop(s, 4, 0, "2d4 Cold (2 + 2)");
            Expect(s, "prone", [](Ctx& c) { return MobC(c)->prone; });
        });
        add("grenades.sold_as_items", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                // the shop items carry the actions, are used up, and the arrows are consumables (not ammo to equip)
                struct { uint32 item, spell; } const want[] = { { 2514, SPELL_ARROW_FIRE }, { 2517, SPELL_ARROW_ICE },
                    { 3029, SPELL_ARROW_ACID }, { 3031, SPELL_ARROW_LIGHTNING }, { 2103, SPELL_ARROW_SLAYING },
                    { 16005, SPELL_ALCHEMISTS_FIRE }, { 18512, SPELL_ACID_VIAL }, { 10562, SPELL_SMOKEPOWDER },
                    { 13180, SPELL_HOLY_WATER }, { 32413, SPELL_FROST_BOMB } };
                for (auto const& w : want)
                {
                    ItemTemplate const* t = sObjectMgr->GetItemTemplate(w.item);
                    Check(c, t && t->Spells[0].SpellId == w.spell && t->Spells[0].SpellCharges == -1 && t->RequiredSkill == 0,
                        Acore::StringFormat("item {} uses up its action {}", w.item, w.spell));
                    if (t && w.item != 16005 && w.item != 18512 && w.item != 10562 && w.item != 32413)
                        Check(c, t->Class == ITEM_CLASS_CONSUMABLE && t->InventoryType == INVTYPE_NON_EQUIP,
                            Acore::StringFormat("item {} is a consumable", w.item));
                }
            });
        });

        // ============================================================ the cleric (WoW Priest)
        add("cleric.sacred_flame", [=](Script& s)
        {
            AddBot(s, { CLERIC, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_SACRED_FLAME);
            ExpectCastOk(s);
            ExpectDrop(s, 1, 0, "1d8 Radiant (1)");
        });
        add("cleric.toll_the_dead_a_d12_on_the_wounded", [=](Script& s)
        {
            // every die rigged to 10: a d8 tops out at 8, a d12 shows the 10
            AddBot(s, { CLERIC, 1 });
            MobOpt stuck{ 8.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Rig(s, { 1, 1 }, 10);
            Cast(s, SPELL_TOLL_THE_DEAD);
            ExpectDrop(s, 8, 0, "unhurt: 1d8 (8)");
            NextTurn(s);
            Rig(s, { 1, 1 }, 10);
            Cast(s, SPELL_TOLL_THE_DEAD);
            ExpectDrop(s, 10, 0, "hurt now: 1d12 (10)");
        });
        add("cleric.bless_turns_a_miss", [=](Script& s)
        {
            // mace: STR 14 (+2) + proficiency 2 = +4; a 5 (9) misses AC 10, Bless's d4 (3) makes it 12
            AddBot(s, { CLERIC, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2130); });
            MobOpt stuck{ 2.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Cast(s, SPELL_BLESS, -1);
            ExpectCastOk(s);
            Expect(s, "blessed, under the cleric's concentration", [](Ctx& c)
            {
                return BotC(c)->blessedBy == Bot(c)->GetGUID() && BotC(c)->concentration == SPELL_BLESS;
            });
            Rig(s, {}, 20);                                      // (the creature's blow: the concentration save holds)
            NextTurn(s, true);
            Rig(s, { 5, 5, 3 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "(Bless)");
            ExpectDrop(s, 3, 0, "a hit: 1d6 (1) + 2");
        });
        add("cleric.guiding_bolt_lights_the_target", [=](Script& s)
        {
            AddBot(s, { CLERIC, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_GUIDING_BOLT);
            ExpectDrop(s, 4, 0, "4d6 Radiant (four 1s)");
            Expect(s, "lit up", [](Ctx& c) { return MobC(c)->guided; });
            NextTurn(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_SACRED_FLAME);                          // (a save, not an attack: the light stays)
            Expect(s, "a save doesn't spend it", [](Ctx& c) { return MobC(c)->guided; });
        });
        add("cleric.shield_of_faith", [=](Script& s)
        {
            AddBot(s, { CLERIC, 1 });
            AddMob(s, { 8.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { c.v["ac"] = float(ArmorClass(Bot(c))); });
            Cast(s, SPELL_SHIELD_OF_FAITH, -1);
            ExpectCastOk(s);
            ExpectEq(s, "+2 Armour Class", [](Ctx& c) { return double(ArmorClass(Bot(c))) - c.v["ac"]; }, 2);
        });
        add("cleric.life_heals_more", [=](Script& s)
        {
            // Cure Wounds: 1d8 (1) + WIS 3, and Disciple of Life's 2 + 1 = 7
            AddBot(s, { CLERIC, 1, "Life" });
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); c.v["hp"] = 1; });
            Rig(s, {}, 1);
            Cast(s, SPELL_CURE_WOUNDS, -1);
            ExpectCastOk(s);
            ExpectEq(s, "7 hit points back", [](Ctx& c) { return double(Bot(c)->GetHealth()) - c.v["hp"]; }, 7);
        });
        add("cleric.preserve_life_up_to_half", [=](Script& s)
        {
            AddBot(s, { CLERIC, 2, "Life" });
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(1); });
            Cast(s, SPELL_PRESERVE_LIFE, -1);
            ExpectCastOk(s);
            Expect(s, "healed to half their hit points (the pool of 10 covers it)", [](Ctx& c)
            {
                return Bot(c)->GetHealth() == std::min<uint32>(Bot(c)->GetMaxHealth() / 2, 11);
            });
            ExpectEq(s, "a Channel Divinity spent", [](Ctx& c) { return double(GetSheet(Bot(c)).channelUsed); }, 1);
        });
        add("cleric.turn_undead", [=](Script& s)
        {
            AddBot(s, { CLERIC, 2 });
            MobOpt zombie{ 6.f, 0.f };
            zombie.st = Stats(1501);
            AddMob(s, zombie);
            AddMob(s, { 7.f, 2.f });                              // and a living one
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_TURN_UNDEAD, -1);
            ExpectCastOk(s);
            Expect(s, "the zombie is turned", [](Ctx& c) { return MobC(c, 0)->turned; });
            Expect(s, "the living one isn't", [](Ctx& c) { return !MobC(c, 1)->turned; });
        });
        add("cleric.spirit_guardians_at_turn_start", [=](Script& s)
        {
            AddBot(s, { CLERIC, 5 });
            MobOpt stuck{ 2.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Cast(s, SPELL_SPIRIT_GUARDIANS, -1);
            ExpectCastOk(s);
            Act(s, [](Ctx& c) { Remember(c); });
            Rig(s, {}, 1);
            NextTurn(s, true);
            ExpectDrop(s, 3, 0, "its turn starts beside the cleric: 3d8 Radiant (three 1s)");
        });
        add("cleric.tempest_wrath_of_the_storm", [=](Script& s)
        {
            AddBot(s, { CLERIC, 1, "Tempest" });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { Remember(c); sMsgs.clear(); });
            Rig(s, { 19, 19 }, 1);                               // the creature's blow lands; then everything rolls 1
            Act(s, [](Ctx& c) { CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Wait(500);
            ExpectSaw(s, "Wrath of the Storm");
            ExpectDrop(s, 2, 0, "the storm answers: 2d8 Lightning (1 + 1)");
            Expect(s, "the reaction and a use spent", [](Ctx& c) { return BotC(c)->reactionUsed && GetSheet(Bot(c)).wrathUsed == 1; });
        });
        add("cleric.destructive_wrath_maximises", [=](Script& s)
        {
            AddBot(s, { CLERIC, 2, "Tempest" });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Cast(s, SPELL_DESTRUCTIVE_WRATH, -1);
            ExpectCastOk(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_THUNDERWAVE, -1);
            ExpectDrop(s, 16, 0, "Thunderwave at its fullest: 2d8 = 16");
            Expect(s, "and the wrath is spent", [](Ctx& c) { return !BotC(c)->wrathArmed; });
        });
        add("cleric.learns_their_spells", [=](Script& s)
        {
            AddBot(s, { CLERIC, 1, "Life" });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, p->HasSpell(SPELL_SACRED_FLAME) && p->HasSpell(SPELL_BLESS) && p->HasSpell(SPELL_CURE_WOUNDS) &&
                    p->HasSpell(SPELL_GUIDING_BOLT), "a level 1 cleric: Sacred Flame, Bless, Cure Wounds, Guiding Bolt");
                Check(c, !p->HasSpell(SPELL_THUNDERWAVE) && !p->HasSpell(SPELL_FIRE_BOLT), "a Life cleric: no Thunderwave, no wizardry");
                p->GiveLevel(3);
                Check(c, p->HasSpell(SPELL_AID) && p->HasSpell(SPELL_HOLD_PERSON) && p->HasSpell(SPELL_PRESERVE_LIFE),
                    "level 3: Aid, Hold Person, Preserve Life");
                p->GiveLevel(5);
                Check(c, p->HasSpell(SPELL_SPIRIT_GUARDIANS) && p->HasSpell(SPELL_MASS_HEALING_WORD), "level 5: Spirit Guardians, Mass Healing Word");
                CheckEq(c, ChannelMax(p), 1, "one Channel Divinity");
                p->GiveLevel(6);
                CheckEq(c, ChannelMax(p), 2, "two at 6");
            });
        });


        // ============================================================ spell scrolls (tools/shops.py)
        add("scroll.a_fighter_reads_hold_person", [=](Script& s)
        {
            // a fighter knows no spells: the scroll's DC 13 and no slot; the scroll is used up
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c) { Bot(c)->AddItem(3013, 1); });
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Item* scroll = p->GetItemByEntry(3013);
                Check(c, scroll != nullptr, "the scroll is in the pack");
                if (!scroll)
                    return;
                ClearMsgs();
                Prep(p, SPELL_HOLD_PERSON);
                c.v["cast"] = float(p->CastSpell(Mob(c), SPELL_HOLD_PERSON, TRIGGERED_NONE, scroll));
            });
            s.Wait(1000);
            ExpectCastOk(s);
            Expect(s, "held", [](Ctx& c) { return MobC(c)->held; });
            ExpectSaw(s, "DC 13");
            ExpectEq(s, "no slot", [](Ctx& c) { return double(GetSheet(Bot(c)).slotsUsed[2]); }, 0);
            ExpectEq(s, "the scroll is gone", [](Ctx& c) { return double(Bot(c)->GetItemCount(3013, false)); }, 0);
        });
        add("scroll.a_fighter_reads_healing_word", [=](Script& s)
        {
            // 1d4 (1) + the scroll's +3
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c) { Bot(c)->AddItem(954, 1); Bot(c)->SetHealth(1); });
            Rig(s, {}, 1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Prep(p, SPELL_HEALING_WORD);
                c.v["cast"] = float(p->CastSpell(p, SPELL_HEALING_WORD, TRIGGERED_NONE, p->GetItemByEntry(954)));
            });
            s.Wait(1000);
            ExpectCastOk(s);
            ExpectEq(s, "healed to 5", [](Ctx& c) { return double(Bot(c)->GetHealth()); }, 5);
        });

        // ============================================================ concentration
        // Hold Person, Hex and Faerie Fire: one at a time; damage calls for a CON save, DC 10 or half the damage (temporary
        // HP soaking it still counts), War Caster gives advantage; falling ends it
        auto hit = [](int amount)
        {
            return [amount](Ctx& c)
            {
                ClearMsgs();
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = Bot(c);
                d.amount = amount;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            };
        };
        auto heldSetup = [=](Script& s, BotOpt o)
        {
            AddBot(s, o);
            AddMob(s, { 6.f, 0.f, Stats(HUMANOID) });
            StartFight(s);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).tempHp = 1000; });   // the hits below never drop the wizard
            Rig(s, { 1, 1 });
            Cast(s, SPELL_HOLD_PERSON);
            ExpectCastOk(s);
            Expect(s, "held, and the wizard concentrating", [](Ctx& c)
            {
                return MobC(c)->held && MobC(c)->heldBy == Bot(c)->GetGUID() && BotC(c)->concentration == SPELL_HOLD_PERSON;
            });
        };
        add("conc.a_second_concentration_spell_ends_the_first", [=](Script& s)
        {
            AddBot(s, lockAt(3, "Archfey"));
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_FAERIE_FIRE);
            Expect(s, "outlined, under the warlock's concentration", [](Ctx& c)
            {
                return MobC(c)->outlined && BotC(c)->concentration == SPELL_FAERIE_FIRE;
            });
            Cast(s, SPELL_HEX);                                   // (a Bonus Action: same turn)
            ExpectCastOk(s);
            Expect(s, "Hex is the concentration now", [](Ctx& c) { return BotC(c)->concentration == SPELL_HEX && MobC(c)->hexedBy == Bot(c)->GetGUID(); });
            Expect(s, "and the faerie light is gone", [](Ctx& c) { return !MobC(c)->outlined; });
            ExpectSaw(s, "loses concentration: Faerie Fire ends - Jiglock casts Hex");
        });
        add("conc.damage_calls_for_a_con_save", [=](Script& s)
        {
            heldSetup(s, { WIZ, 3 });
            // the wizard's CON save: +2 (CON 14, not proficient)
            Rig(s, { 9, 9 });
            Act(s, hit(8));                                       // DC 10 (half of 8 is 4): 9 + 2 = 11 holds
            ExpectSaw(s, "Concentration (Hold Person)");
            Expect(s, "8 damage, DC 10, an 11: still held", [](Ctx& c) { return MobC(c)->held && BotC(c)->concentration; });
            Rig(s, { 9, 9 });
            Act(s, hit(30));                                      // DC 15: 11 fails (all of it soaked by temporary HP)
            Expect(s, "30 damage, DC 15, an 11: the hold breaks", [](Ctx& c)
            {
                return !MobC(c)->held && !BotC(c)->concentration && Mob(c)->GetUInt32Value(UNIT_NPC_EMOTESTATE) == 0;
            });
            ExpectSaw(s, "loses concentration: Hold Person ends");
        });
        add("conc.war_caster_gives_advantage", [=](Script& s)
        {
            heldSetup(s, BotOpt{ WIZ, 4, "", { "feat:War Caster" } });
            Rig(s, { 5, 18 });
            Act(s, hit(30));                                      // DC 15: advantage keeps the 18 (+2 = 20)
            ExpectSaveAdv(s, "Concentration (Hold Person)", 1);
            Expect(s, "War Caster: the hold stays", [](Ctx& c) { return MobC(c)->held && BotC(c)->concentration; });
        });
        add("conc.falling_ends_it", [=](Script& s)
        {
            heldSetup(s, { WIZ, 3 });
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).tempHp = 0; });
            Act(s, hit(1000));
            ExpectSaw(s, "Hold Person ends - they fell");
            Expect(s, "the creature is free", [](Ctx& c) { return Mob(c)->GetUInt32Value(UNIT_NPC_EMOTESTATE) == 0; });
        });
        add("conc.the_turn_order_shows_who_concentrates", [=](Script& s)
        {
            heldSetup(s, { WIZ, 3 });
            Act(s, [](Ctx& c)
            {
                ClearMsgs();
                PushOrder(*Enc(c));
                int mine = -1, theirs = -1;
                for (std::string const& m : sMsgs)
                    if (m.rfind("O^", 0) == 0)
                    {
                        std::vector<std::string> f = Split(m, '^');
                        if (f.size() > 13)
                            (f[5] == "1" ? mine : theirs) = std::atoi(f[13].c_str());
                    }
                CheckEq(c, mine, 1, "the wizard's portrait: CONC");
                CheckEq(c, theirs, 0, "the creature's: no");
            });
        });
        add("conc.a_spell_that_took_no_hold_needs_no_save", [=](Script& s)
        {
            AddBot(s, { WIZ, 3 });
            AddMob(s, { 6.f, 0.f, Stats(HUMANOID) });
            StartFight(s);
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).tempHp = 1000; });
            Rig(s, { 20, 20 });
            Cast(s, SPELL_HOLD_PERSON);                            // the creature saves
            Expect(s, "not held", [](Ctx& c) { return !MobC(c)->held; });
            Act(s, hit(8));
            ExpectNotSaw(s, "Concentration (Hold Person)");
            Expect(s, "and nothing to concentrate on", [](Ctx& c) { return !BotC(c)->concentration; });
        });
        add("conc.war_caster_is_for_casters", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 4 });
            Act(s, pick("feat^War Caster"));
            Expect(s, "a fighter can't take War Caster", [](Ctx& c) { return !HasFeat(Bot(c), "War Caster"); });
        });
        add("warlock.fey_presence", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Archfey"));
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 1, 1 }, 1);
            Cast(s, SPELL_FEY_PRESENCE, -1);
            ExpectCastOk(s);
            Expect(s, "frightened", [](Ctx& c) { return MobC(c)->frightened; });
            ExpectEq(s, "spent", [](Ctx& c) { return GetSheet(Bot(c)).patronUsed; }, 1);
            NextTurn(s);
            Cast(s, SPELL_FEY_PRESENCE, -1);
            ExpectRefused(s, "spent until a short rest");
        });
        add("warlock.vampiric_touch", [=](Script& s)
        {
            AddBot(s, lockAt(5, "Fiend"));
            AddMob(s, {});
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(10); });
            StartFight(s);
            Rig(s, { 15, 15, 4, 4, 4 }, 1);
            Cast(s, SPELL_VAMPIRIC_TOUCH);
            ExpectCastOk(s);
            ExpectDrop(s, 12, 0, "3d6 (4 + 4 + 4) Necrotic");
            ExpectEq(s, "healed half", [](Ctx& c) { return Bot(c)->GetHealth(); }, 16);
        });
        add("warlock.pact_of_the_blade", [=](Script& s)
        {
            AddBot(s, lockAt(3, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast", "pact:Blade" }));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectDrop(s, 7, 0, "quarterstaff 4 + CHA 3 (not STR -1)");
        });
        add("warlock.thirsting_blade", [=](Script& s)
        {
            AddBot(s, lockAt(5, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast", "pact:Blade", "inv:Thirsting Blade" }));
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 7, 0, "the second swing");
        });
        add("warlock.invocations_at_will", [=](Script& s)
        {
            AddBot(s, lockAt(2, "Fiend", { "inv:Armor of Shadows", "inv:Fiendish Vigour" }));
            Cast(s, SPELL_MAGE_ARMOR, -1);
            ExpectCastOk(s);
            ExpectEq(s, "Armor of Shadows: AC 13 + DEX", [](Ctx& c) { return ArmorClass(Bot(c)); }, 15);
            Cast(s, SPELL_FALSE_LIFE, -1);
            ExpectCastOk(s);
            ExpectEq(s, "no pact slot spent", [](Ctx& c) { return GetSheet(Bot(c)).slotsUsed[1]; }, 0);
        });
        add("warlock.dark_ones_blessing", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, weak(8.f));
            AddMob(s, distant8);
            StartFight(s);
            Rig(s, { 15, 15, 4 }, 1);
            Cast(s, SPELL_ELDRITCH_BLAST);
            ExpectEq(s, "a kill: CHA 3 + level 1 temporary HP", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 4);
        });
        add("warlock.dark_ones_own_luck", [=](Script& s)
        {
            AddBot(s, lockAt(6, "Fiend"));
            AddMob(s, venomMob());
            StartFight(s);
            Cast(s, SPELL_DARK_ONES_LUCK, -1);
            ExpectCastOk(s);
            Expect(s, "armed", [](Ctx& c) { return BotC(c)->luckArmed; });
            Rig(s, { 15, 15, 1, 1 }, 5);
            creatureHits(s);
            ExpectSaw(s, "Dark One's Own Luck");
            Expect(s, "spent on that save", [](Ctx& c) { return !BotC(c)->luckArmed; });
        });
        add("warlock.entropic_ward", [=](Script& s)
        {
            AddBot(s, lockAt(6, "Great Old One"));
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 3 }, 0);
            creatureHits(s);
            ExpectEq(s, "disadvantage keeps the 3: a miss", hurt, 0);
            Expect(s, "the ward marks the attacker", [](Ctx& c) { return BotC(c)->entropic == Mob(c)->GetGUID(); });
            ExpectEq(s, "once a short rest", [](Ctx& c) { return GetSheet(Bot(c)).patron6Used; }, 1);
        });
        add("warlock.misty_escape", [=](Script& s)
        {
            AddBot(s, lockAt(6, "Archfey"));
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 0);
            creatureHits(s);
            Expect(s, "hurt, the warlock vanishes", [](Ctx& c) { return BotC(c)->hidden; });
            ExpectEq(s, "once a short rest", [](Ctx& c) { return GetSheet(Bot(c)).patron6Used; }, 1);
        });

        // ============================================================ fights that could hang (softlocks)
        add("flow.creature_dies_on_its_own_turn", [=](Script& s)
        {
            // Armor of Agathys kills the attacker mid-swing: its turn must still end and the next one start
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, weak());
            MobOpt second;
            second.dy = 3.f;
            AddMob(s, second);
            StartFight(s);
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            Rig(s, {}, 15);
            NextTurn(s, true);
            Expect(s, "the attacker died on the frost", [](Ctx& c) { return !Mob(c, 0)->IsAlive(); });
            Expect(s, "the fight goes on with the other one", [](Ctx& c) { return InCombat(Bot(c)) && IsTurnOf(Bot(c)); });
        });
        add("flow.last_creature_dies_on_its_own_turn", [=](Script& s)
        {
            AddBot(s, lockAt(1, "Fiend"));
            AddMob(s, weak());
            StartFight(s);
            Cast(s, SPELL_ARMOR_OF_AGATHYS, -1);
            Rig(s, {}, 15);
            Act(s, [](Ctx& c) { sMsgs.clear(); EndTurn(Bot(c)); });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to end when the last attacker kills itself", 20000);
            ExpectSaw(s, "V^VICTORY");
        });
        add("flow.unreachable_enemy_passes_its_turn", [=](Script& s)
        {
            AddBot(s, fig1);
            MobOpt stuck;
            stuck.dx = 12.f;
            stuck.st.speed = 0.f;                    // can't move, can't reach, has no ranged attack
            AddMob(s, stuck);
            StartFight(s);
            NextTurn(s);
            NextTurn(s);
            Expect(s, "two rounds went by", [](Ctx& c) { return InCombat(Bot(c)) && Enc(c)->round >= 3; });
        });
        add("flow.ranged_enemy_finishes_its_turn", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, hitter("2", DMG_PIERCING, true));
            StartFight(s);
            NextTurn(s);
            NextTurn(s);
            Expect(s, "two rounds went by", [](Ctx& c) { return InCombat(Bot(c)) && Enc(c)->round >= 3; });
        });
        add("flow.one_spell_kills_everyone", [=](Script& s)
        {
            AddBot(s, { WIZ, 5, "Evocation" });
            AddMob(s, weak(8.f));
            AddMob(s, weak(8.f, 2.f));
            StartFight(s);
            Rig(s, {}, 2);
            CastAt(s, SPELL_FIREBALL, 8.f, 1.f);
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to end with both dead", 10000);
            ExpectSaw(s, "V^VICTORY");
            Expect(s, "free to move", [](Ctx& c) { return !Bot(c)->HasUnitState(UNIT_STATE_ROOT); });
        });
        add("flow.death_is_a_defeat_and_the_fight_can_start_again", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, hitter("500"));
            StartFight(s);
            Act(s, [](Ctx& c) { DamageCtx d; d.attacker = Bot(c); d.victim = Mob(c); d.amount = 20; d.type = DMG_FORCE; ApplyDamage(d); });
            Rig(s, { 15, 15 }, 0);
            Act(s, [](Ctx& c) { sMsgs.clear(); CreatureAttack(Mob(c), Bot(c), false, false); });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to end when the only player dies", 8000);
            ExpectSaw(s, "V^DEFEAT");
            // (a wipe is a reload: the bot wakes alive at the graveyard - wipe.nobody_left_standing_is_a_reload)
            Act(s, [](Ctx& c) { AckTeleport(Bot(c)); });
            Expect(s, "the bot wakes alive", [](Ctx& c) { return Bot(c)->IsAlive() && !IsDowned(Bot(c)); });
            Expect(s, "the creature is out of the fight, healed", [](Ctx& c)
            {
                return !InCombat(Mob(c)) && Mob(c)->GetHealth() == Mob(c)->GetMaxHealth();
            });
            Act(s, [](Ctx& c)
            {
                // back to the creature, the grace after the wipe over
                Player* p = Bot(c);
                Creature* m = Mob(c);
                GrantGrace(p, 0);
                p->TeleportTo(m->GetMapId(), m->GetPositionX() - 2.f, m->GetPositionY(), m->GetPositionZ(), 0.f);
                AckTeleport(p);
            });
            s.Wait(800);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                AckTeleport(p);
                if (!InCombat(p))
                    StartEncounter(Mob(c), p);
                Check(c, InCombat(p), "a new fight starts after the defeat");
            });
        });
        add("flow.running_far_away_resets_the_fight", [=](Script& s)
        {
            AddBot(s, fig1);
            MobOpt stuck;
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                p->Relocate(p->GetPositionX() - 80.f, p->GetPositionY(), Ground(p->GetMap(), p->GetPositionX() - 80.f, p->GetPositionY()));
                p->UpdatePositionData();
                sMsgs.clear();
                EndTurn(p);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to reset once nobody is within reach", 8000);
            ExpectSaw(s, "V^ESCAPED");
            Expect(s, "the creature is out of the fight", [](Ctx& c)
            {
                Map* m = sMapMgr->FindMap(MAP_ID, 0);
                Creature* cr = m ? m->GetCreature(c.mobs[0]) : nullptr;
                return cr && !InCombat(cr);
            });
        });
        add("flow.logout_on_your_turn_frees_the_enemy", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { RemoveBot(c.bots[0]); });
            s.Until([](Ctx& c)
            {
                Map* m = sMapMgr->FindMap(MAP_ID, 0);
                Creature* cr = m ? m->GetCreature(c.mobs[0]) : nullptr;
                return cr && !InCombat(cr);
            }, "the creature to leave the fight after the player logged out", 6000);
        });
        add("flow.logout_during_the_enemy_turn", [=](Script& s)
        {
            AddBot(s, fig1);
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { EndTurn(Bot(c)); });
            s.Wait(1300);                            // the creature is mid-turn
            Act(s, [](Ctx& c) { RemoveBot(c.bots[0]); });
            s.Until([](Ctx& c)
            {
                Map* m = sMapMgr->FindMap(MAP_ID, 0);
                Creature* cr = m ? m->GetCreature(c.mobs[0]) : nullptr;
                return cr && !InCombat(cr);
            }, "the creature to leave the fight after the player logged out mid-turn", 10000);
        });
        add("flow.two_players_take_turns_and_the_dead_stay_dead", [=](Script& s)
        {
            AddBot(s, fig1);
            BotOpt friendOpt{ DWARF, 1, "", { "style:Defense" } };
            friendOpt.at = true;
            friendOpt.x = HOME_X;
            friendOpt.y = HOME_Y + 2.5f;
            AddBot(s, friendOpt);
            MobOpt mob;
            mob.st.speed = 0.f;
            mob.dx = 6.f;
            AddMob(s, mob);
            Act(s, [](Ctx& c)
            {
                sDice.assign({ 15, 15, 15, 15, 1, 1 });
                Mob(c)->SetFaction(14);
                StartEncounter(Mob(c), Bot(c, 0));
                sDice.clear();
                Check(c, InCombat(Bot(c, 0)) && InCombat(Bot(c, 1)), "both players are in the fight");
                c.v["t0"] = c.v["t1"] = 0;
            });
            s.Until([](Ctx& c)
            {
                for (std::size_t i = 0; i < 2; ++i)
                    if (IsTurnOf(Bot(c, i)))
                    {
                        ++c.v[i ? "t1" : "t0"];
                        EndTurn(Bot(c, i));
                    }
                return c.v["t0"] >= 2 && c.v["t1"] >= 2;
            }, "both players to get two turns each", 60000);
            Act(s, [](Ctx& c)
            {
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = Bot(c, 1);
                d.amount = 5000;
                ApplyDamage(d);
                Check(c, !Bot(c, 1)->IsAlive(), "the second player fell");
                Check(c, InCombat(Bot(c, 0)), "the fight goes on for the first");
                DamageCtx k;
                k.attacker = Bot(c, 0);
                k.victim = Mob(c);
                k.amount = 5000;
                k.type = DMG_FORCE;
                ApplyDamage(k);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c, 0)); }, "the fight to end", 8000);
            // 5000 damage is past their whole hit points: dead outright, not down - and a won fight doesn't raise the dead
            Expect(s, "the fallen player stays dead (Brother Marrow raises the dead)", [](Ctx& c) { return !Bot(c, 1)->IsAlive(); });
        });
        add("flow.junk_messages_from_the_addon_are_harmless", [=](Script& s)
        {
            static std::vector<std::string> const junk = { "", "pick^", "pick^subclass^", "pick^subclass^Nope", "pick^asi^ZZZ",
                "pick^asi2^STR", "pick^asi2^STR^STR", "pick^feat^", "pick^feat^Nope^X", "pick^meta^X", "pick^inv^X", "pick^pact^X",
                "pick^style^X", "pick^unknown^x", "build^", "build^a^b^c^d^e", "build^99,99,99,99,99,99^0^1^Sage^Arcana,History",
                "build^15,13,14,8,12,10^7^7^Sage^Arcana,History", "build^15,13,14,8,12,10^0^2^Nobody^Arcana,History", "say^", "say^999",
                "say^-1", "bye", "tav^", "tav^play^dice^-5", "tav^play^nothing^100", "tav^act^", "tav^act^hit^9", "tav^leave",
                "treatgame^", "treatgame^x^y^z", "treatgame^999999^1^1", "endturn", "hello", "sheet", std::string(300, '^'),
                std::string(250, 'x') };
            AddBot(s, fig1);
            AddMob(s, {});
            Act(s, [](Ctx& c) { for (std::string const& m : junk) HandleClientMessage(Bot(c), m); });
            StartFight(s);
            Act(s, [](Ctx& c) { for (std::string const& m : junk) if (m != "endturn") HandleClientMessage(Bot(c), m); });
            Expect(s, "still standing, still the bot's turn", [](Ctx& c) { return Bot(c)->IsAlive() && IsTurnOf(Bot(c)); });
            Expect(s, "the junk changed nothing on the sheet", [](Ctx& c)
            {
                Sheet& sh = GetSheet(Bot(c));
                return sh.abil[STR] == 16 && sh.subclass.empty() && sh.picks.size() == 1;
            });
        });

        // ============================================================ levelling: every class and subclass, 1 to 6, every choice
        struct Flow { Kind kind; char const* cls; int subclasses; };
        for (Flow f : { Flow{ WIZ, "wizard", 2 }, Flow{ DWARF, "fighter", 2 }, Flow{ BARB, "barbarian", 2 }, Flow{ ROGUE, "rogue", 2 },
                 Flow{ MONK, "monk", 2 }, Flow{ CLERIC, "cleric", 2 }, Flow{ LOCK, "warlock", 3 },
                 Flow{ BARD, "bard", 2 }, Flow{ DRUID, "druid", 2 } })
            for (int sub = 0; sub < f.subclasses; ++sub)
                add(Acore::StringFormat("levels.{}_{}_one_to_six", f.cls, sub + 1), [=](Script& s)
                {
                    AddBot(s, { f.kind, 1 });
                    Act(s, [=](Ctx& c)
                    {
                        Player* p = Bot(c);
                        Sheet& sh = GetSheet(p);
                        sh.picks.clear();
                        sh.subclass.clear();
                        auto candidates = [&](std::string const& kind)
                        {
                            std::vector<std::string> out;
                            if (kind == "style")
                                out = { "Defense", "Dueling", "Archery", "Great Weapon Fighting" };
                            else if (kind == "subclass")
                            {
                                std::vector<char const*> const& all = ClassInfo(p)->subclasses;
                                for (std::size_t i = 0; i < all.size(); ++i)
                                    out.push_back(all[(i + sub) % all.size()]);
                            }
                            else if (kind == "meta")
                                out = { "Quickened", "Twinned", "Heightened", "Empowered" };
                            else if (kind == "inv")
                                out = { "Thirsting Blade", "Agonizing Blast", "Repelling Blast", "Armor of Shadows", "Fiendish Vigour",
                                    "Beguiling Influence" };
                            else if (kind == "pact")
                                out = sub % 2 ? std::vector<std::string>{ "Tome", "Blade" } : std::vector<std::string>{ "Blade", "Tome" };
                            else if (kind == "asi")
                                out = { "CON" };
                            else if (kind == "feat1")
                                out = { "Tough", "Alert" };
                            return out;
                        };
                        auto resolve = [&](int lvl)
                        {
                            for (int guard = 0; guard < 12; ++guard)
                            {
                                sMsgs.clear();
                                PushPendingChoices(p);
                                std::string kind;
                                for (std::string const& m : sMsgs)
                                    if (m.rfind("LP^", 0) == 0)
                                        kind = m.substr(3);
                                if (kind.empty())
                                    return;
                                std::size_t before = sh.picks.size();
                                std::string subBefore = sh.subclass;
                                for (std::string const& cand : candidates(kind))
                                {
                                    HandleClientMessage(p, "pick^" + kind + "^" + cand);
                                    if (sh.picks.size() != before || sh.subclass != subBefore)
                                        break;
                                }
                                if (sh.picks.size() == before && sh.subclass == subBefore)
                                {
                                    Check(c, false, Acore::StringFormat("level {}: nothing answers the pending choice \"{}\" - stuck", lvl, kind));
                                    return;
                                }
                            }
                            Check(c, false, Acore::StringFormat("level {}: the pending choices never ran out", lvl));
                        };
                        resolve(1);
                        for (int lvl = 2; lvl <= MAX_DND_LEVEL; ++lvl)
                        {
                            p->GiveLevel(uint8(lvl));
                            resolve(lvl);
                            Check(c, int(p->GetMaxHealth()) == MaxHp(p), Acore::StringFormat("level {}: max health follows the sheet", lvl));
                        }
                        Check(c, !sh.subclass.empty(), "a subclass was chosen");
                        int asi = 0, style = 0, meta = 0, inv = 0, pact = 0, start = 0;
                        for (std::string const& k : sh.picks)
                        {
                            start += k.rfind("feat1:", 0) == 0;
                            asi += k.rfind("asi", 0) == 0 || k.rfind("feat:", 0) == 0;
                            style += k.rfind("style:", 0) == 0;
                            meta += k.rfind("meta:", 0) == 0;
                            inv += k.rfind("inv:", 0) == 0;
                            pact += k.rfind("pact:", 0) == 0;
                        }
                        CheckEq(c, start, 1, "a starting feat");
                        CheckEq(c, asi, f.kind == DWARF ? 2 : 1, "ability score improvements taken");
                        CheckEq(c, style, f.kind == DWARF ? 1 : 0, "fighting styles");
                        CheckEq(c, meta, 0, "metamagic options");
                        CheckEq(c, inv, f.kind == LOCK ? 3 : 0, "invocations");
                        CheckEq(c, pact, f.kind == LOCK ? 1 : 0, "pact boons");
                        sMsgs.clear();
                        PushPendingChoices(p);
                        Check(c, Saw("LP^") && sMsgs.size() == 1 && sMsgs[0] == "LP^", "nothing left to choose at level 6");
                        // every spell the kit taught is a known action; the sheet still goes out
                        for (auto const* kit : { &WizardKit(), &ClassActions() })
                            for (uint32 id : *kit)
                                if (p->HasSpell(id))
                                    Check(c, GetAction(id) != nullptr && KitOk(p, id), Acore::StringFormat("spell {} belongs in this kit", id));
                        sMsgs.clear();
                        PushSheet(p);
                        Check(c, Saw("S^6^"), "the level 6 sheet is sent");
                    });
                });

        // ============================================================ the character builder
        add("build.every_class_accepts_a_legal_build", [=](Script& s)
        {
            for (Kind k : { WIZ, DWARF, BARB, ROGUE, MONK, CLERIC, LOCK })
                AddBot(s, { k, 1 });
            Act(s, [](Ctx& c)
            {
                for (ObjectGuid g : c.bots)
                {
                    Player* p = ObjectAccessor::FindPlayer(g);
                    Sheet& sh = GetSheet(p);
                    sh.created = false;
                    std::vector<std::string> two;
                    for (char const* k : ClassInfo(p)->skills)
                        if (two.size() < 2 && std::string(k) != "Acrobatics" && std::string(k) != "Performance")
                            two.push_back(k);
                    sMsgs.clear();
                    HandleClientMessage(p, "build^15,13,14,8,12,10^0^2^Entertainer^" + two[0] + "," + two[1]);
                    std::string who = ClassName(p);
                    Check(c, sh.created && Saw("C^done"), who + ": the build was accepted");
                    Check(c, sh.abil[STR] == 17 && sh.abil[CON] == 15 && sh.abil[DEX] == 13, who + ": 15 + 2 STR, 14 + 1 CON");
                    Check(c, sh.skills.size() == 4 && sh.background == "Entertainer", who + ": two background skills and two class skills");
                    Check(c, int(p->GetMaxHealth()) == MaxHp(p) && p->GetHealth() == p->GetMaxHealth(), who + ": hit points follow the build");
                }
            });
        });
        add("build.illegal_builds_are_refused", [=](Script& s)
        {
            AddBot(s, wiz1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Sheet& sh = GetSheet(p);
                std::array<int, ABILITY_COUNT> before = sh.abil;
                for (char const* bad : { "build^15,15,15,15,15,15^0^2^Sage^Insight,Medicine",       // 54 points
                         "build^15,13,14,8,12,10^2^2^Sage^Insight,Medicine",                          // +2 and +1 on one ability
                         "build^15,13,14,8,12,10^0^2^Sage^Insight,Athletics",                         // Athletics isn't a wizard skill
                         "build^15,13,14,8,12,10^0^2^Sage^Arcana,Insight",                            // Arcana comes with Sage already
                         "build^15,13,14,8,12,10^0^2^Sage^Insight",                                   // one skill
                         "build^15,13,14,8,12,10^0^2^Sage^Insight,Insight",                           // the same skill twice
                         "build^15,13,14,8,12^0^2^Sage^Insight,Medicine" })                           // five scores
                {
                    HandleClientMessage(p, bad);
                    Check(c, sh.abil == before, std::string("refused: ") + bad);
                }
                HandleClientMessage(p, "build^8,14,13,15,12,10^3^2^Sage^Insight,Medicine");
                Check(c, sh.abil[INT] == 17 && sh.abil[CON] == 14, "and a legal one still goes through");
            });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Sheet& sh = GetSheet(Bot(c));
                HandleClientMessage(Bot(c), "build^15,13,14,8,12,10^0^2^Sage^Insight,Medicine");
                Check(c, sh.abil[INT] == 17, "no rebuilding in the middle of a fight");
            });
        });

        // ============================================================ zones: clearing them levels you, once
        add("progress.clearing_each_zone_is_exactly_one_level", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                int zones = 0;
                for (uint32 z = 1; z <= 40; ++z)
                {
                    ZoneDef const* zone = GetZone(z);
                    if (!zone || zone->encounters.empty())
                        continue;
                    ++zones;
                    // each zone from its own level (two zones may share one: the Wetlands and the Badlands both
                    // branch off Loch Modan at level 4)
                    if (p->GetLevel() != zone->minLevel)
                        p->GiveLevel(zone->minLevel);
                    p->SetUInt32Value(PLAYER_XP, 0);
                    int before = p->GetLevel();
                    sMsgs.clear();
                    // an endgame zone (past the last level): no XP at all, and every encounter pays rewards instead
                    bool endgame = zone->minLevel >= MAX_DND_LEVEL && std::all_of(zone->encounters.begin(), zone->encounters.end(),
                        [](uint32 id) { EncounterDef const* d = GetEncounterDef(id); return d && d->xp == 0; });
                    for (uint32 id : zone->encounters)
                    {
                        EncounterDef const* def = GetEncounterDef(id);
                        Check(c, def != nullptr, Acore::StringFormat("{}: encounter {} exists", zone->name, id));
                        if (!def)
                            continue;
                        Check(c, !def->spawns.empty(), Acore::StringFormat("{} has creatures", def->name));
                        if (endgame)
                            Check(c, HasRewards(id), def->name + ": an endgame encounter pays rewards");
                        OnEncounterWon(p, *def);
                        Check(c, HasCleared(p, id), def->name + " is cleared");
                    }
                    Check(c, Saw(zone->name + " cleared"), zone->name + ": the zone-cleared banner");
                    if (endgame)
                    {
                        CheckEq(c, p->GetLevel(), before, zone->name + ": endgame - no level");
                        continue;
                    }
                    CheckEq(c, p->GetLevel(), before + 1, zone->name + ": clearing it is exactly one level");
                    uint32 xp = p->GetUInt32Value(PLAYER_XP);
                    sMsgs.clear();
                    OnEncounterWon(p, *GetEncounterDef(zone->encounters.front()));
                    Check(c, Saw("Already cleared") && p->GetUInt32Value(PLAYER_XP) == xp, zone->name + ": no XP for a second clear");
                }
                Check(c, zones >= 3, "three zones have encounters");
            });
        });
        add("progress.every_encounter_and_creature_is_wired_up", [=](Script& s)
        {
            AddBot(s, fig1);
            Act(s, [](Ctx& c)
            {
                std::set<uint32> seen;
                for (EncounterDef const* def : AllEncounters())
                {
                    Check(c, GetZone(def->zoneId) != nullptr, def->name + " belongs to a zone");
                    Check(c, def->aggroRange > 0 && def->joinRange >= def->aggroRange, def->name + ": sane aggro and join ranges");
                    for (uint32 spawn : def->spawns)
                    {
                        Check(c, seen.insert(spawn).second, Acore::StringFormat("{}: spawn {} is in only one encounter", def->name, spawn));
                        CreatureData const* d = sObjectMgr->GetCreatureData(spawn);
                        Check(c, d != nullptr, Acore::StringFormat("{}: spawn {} is in the creature table", def->name, spawn));
                        if (!d)
                            continue;
                        CreatureStats const* st = GetStats(d->id);
                        Check(c, st != nullptr, Acore::StringFormat("{}: creature entry {} has a stat block", def->name, d->id));
                        if (st)
                            Check(c, st->hp > 0 && st->ac >= 5 && st->melee.valid() && st->speed >= 0,
                                Acore::StringFormat("{}: entry {} has hit points, an armour class and a melee attack", def->name, d->id));
                    }
                }
            });
        });

        // ============================================================ class features nothing covered yet
        add("barbarian.danger_sense_and_raging_strength_saves", [=](Script& s)
        {
            AddBot(s, { BARB, 2 });
            Expect(s, "Danger Sense at 2: advantage on DEX saves", [](Ctx& c) { return SaveAdvantage(Bot(c), DEX, false); });
            BotOpt novice{ BARB, 1 };
            novice.at = true;                        // well away from the fight below
            novice.x = HOME_X;
            novice.y = HOME_Y + 120.f;
            AddBot(s, novice);
            Expect(s, "no Danger Sense at 1", [](Ctx& c) { return !SaveAdvantage(Bot(c, 1), DEX, false); });
            AddMob(s, {});
            StartFight(s);
            Expect(s, "no advantage on STR saves before raging", [](Ctx& c) { return !SaveAdvantage(Bot(c), STR, false); });
            Cast(s, SPELL_RAGE, -1);
            Expect(s, "raging: advantage on STR saves", [](Ctx& c) { return SaveAdvantage(Bot(c), STR, false); });
        });
        add("barbarian.rage_gives_advantage_on_shoves", [=](Script& s)
        {
            AddBot(s, { BARB, 1 });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 3, 15, 1, 1 }, 1);
            Cast(s, SPELL_SHOVE);
            ExpectNotSaw(s, "adv");
            NextTurn(s);
            Cast(s, SPELL_RAGE, -1);
            NextTurn(s);
            Rig(s, { 3, 15, 1, 1 }, 1);
            Cast(s, SPELL_SHOVE);
            ExpectSaw(s, "3/15 adv");
            ExpectSaw(s, "SUCCESS");
        });
        add("barbarian.aspect_of_the_bear", [=](Script& s)
        {
            AddBot(s, { BARB, 6, "Bear Totem" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 3, 15, 1, 1 }, 1);
            Cast(s, SPELL_SHOVE);
            ExpectSaw(s, "3/15 adv");
            Expect(s, "without raging", [](Ctx& c) { return !BotC(c)->raging; });
        });
        add("barbarian.berserker_has_no_bear_aspect", [=](Script& s)
        {
            AddBot(s, { BARB, 6, "Berserker" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 3, 15, 1, 1 }, 1);
            Cast(s, SPELL_SHOVE);
            ExpectNotSaw(s, "adv");
        });
        add("rogue.assassinate", [=](Script& s)
        {
            AddBot(s, { ROGUE, 3, "Assassin" });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 3, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "Assassinate: advantage");
            ExpectSaw(s, "Sneak Attack");
            NextTurn(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectNotSaw(s, "Assassinate");
        });
        add("rogue.assassin_crits_the_surprised", [=](Script& s)
        {
            AddBot(s, { ROGUE, 3, "Assassin" });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->surprised = true; });
            Rig(s, { 12, 12 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "CRITICAL HIT");
        });
        add("rogue.thief_does_not_crit_the_surprised", [=](Script& s)
        {
            AddBot(s, { ROGUE, 3, "Thief" });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 2139); });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { MobC(c)->surprised = true; });
            Rig(s, { 12, 12 }, 4);
            Cast(s, SPELL_ATTACK);
            ExpectNotSaw(s, "CRITICAL HIT");
        });
        add("rogue.expertise_at_six", [=](Script& s)
        {
            AddBot(s, { ROGUE, 6, "Thief" });
            Expect(s, "Perception and Investigation", [](Ctx& c) { return HasExpertise(Bot(c), "Perception") && HasExpertise(Bot(c), "Investigation"); });
            ExpectEq(s, "Stealth: DEX 3 + 2 x proficiency 3", [](Ctx& c) { return SkillBonusByName(Bot(c), "Stealth"); }, 9);
        });
        add("rogue.cunning_disengage", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_CUNNING_DISENGAGE, -1);
            ExpectCastOk(s);
            Expect(s, "disengaged, as a Bonus Action", [](Ctx& c) { return BotC(c)->disengaged && BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });
        add("monk.step_of_the_wind", [=](Script& s)
        {
            AddBot(s, { MONK, 2 });
            AddMob(s, {});
            StartFight(s);
            Act(s, [](Ctx& c) { c.v["move"] = BotC(c)->moveMax; });
            Cast(s, SPELL_STEP_OF_THE_WIND, -1);
            ExpectCastOk(s);
            Expect(s, "a Dash's worth of movement", [](Ctx& c) { return std::fabs(BotC(c)->moveMax - (c.v["move"] + Speed(Bot(c)))) < 0.01f; });
            Expect(s, "jumps doubled, 1 ki, a Bonus Action", [](Ctx& c) { return BotC(c)->stepJump && GetSheet(Bot(c)).kiUsed == 1 && BotC(c)->bonusUsed; });
            NextTurn(s);
            Expect(s, "the jump bonus ends with the turn", [](Ctx& c) { return !BotC(c)->stepJump; });
        });
        add("monk.cloak_of_shadows", [=](Script& s)
        {
            AddBot(s, { MONK, 3, "Shadow" });
            AddMob(s, {});
            StartFight(s);
            Cast(s, SPELL_CLOAK_OF_SHADOWS, -1);
            ExpectCastOk(s);
            Expect(s, "hidden for 2 ki", [](Ctx& c) { return BotC(c)->hidden && GetSheet(Bot(c)).kiUsed == 2; });
            Rig(s, { 3, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectSaw(s, "hidden: advantage");
            Cast(s, SPELL_CLOAK_OF_SHADOWS, -1);
            ExpectRefused(s, "");
        });
        add("monk.shadow_step", [=](Script& s)
        {
            AddBot(s, { MONK, 6, "Shadow" });
            MobOpt off;
            off.dx = 12.f;
            off.st.speed = 0.f;
            AddMob(s, off);
            StartFight(s);
            CastAt(s, SPELL_SHADOW_STEP, 9.f, 0.f);
            ExpectCastOk(s);
            Act(s, [](Ctx& c) { AckTeleport(Bot(c)); });
            s.Wait(300);
            Expect(s, "stepped 9 m", [](Ctx& c) { return Bot(c)->GetPositionX() - c.v["fromx"] > 8.f; });
            Expect(s, "hidden, as a Bonus Action", [](Ctx& c) { return BotC(c)->hidden && BotC(c)->bonusUsed && !BotC(c)->actionUsed; });
        });
        add("monk.extra_attack_at_five", [=](Script& s)
        {
            AddBot(s, { MONK, 5, "Open Hand" });
            AddMob(s, {});
            StartFight(s);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            Rig(s, { 15, 15 }, 3);
            Cast(s, SPELL_ATTACK);
            ExpectCastOk(s);
            ExpectDrop(s, 6, 0, "the second strike: Martial Arts d6 (3) + DEX 3");
            Cast(s, SPELL_ATTACK);
            ExpectRefused(s, "no Action left");
        });
        add("monk.ki_returns_on_a_short_rest", [=](Script& s)
        {
            AddBot(s, { MONK, 4, "Open Hand" });
            Act(s, [](Ctx& c) { GetSheet(Bot(c)).kiUsed = 3; });
            Cast(s, SPELL_SHORT_REST, -1);
            ExpectEq(s, "ki spent", [](Ctx& c) { return GetSheet(Bot(c)).kiUsed; }, 0);
        });
        add("warlock.repelling_blast", [=](Script& s)
        {
            AddBot(s, lockAt(2, "Fiend", { "inv:Repelling Blast", "inv:Agonizing Blast" }));
            MobOpt rooted = distant8;
            rooted.st.speed = 0.f;
            AddMob(s, rooted);
            StartFight(s);
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ELDRITCH_BLAST);
            s.Wait(1500);
            ExpectDrop(s, 7, 0, "4 + CHA 3 (Agonizing Blast)");
            Expect(s, "pushed 3 m further off", [](Ctx& c) { return Bot(c)->GetExactDist2d(Mob(c)) > 10.f; });
        });
        add("warlock.beguiling_influence", [=](Script& s)
        {
            AddBot(s, lockAt(2, "Fiend", { "inv:Agonizing Blast", "inv:Repelling Blast" }));
            AddBot(s, lockAt(2, "Fiend", { "inv:Beguiling Influence", "inv:Repelling Blast" }));
            ExpectEq(s, "Persuasion without it: CHA 3", [](Ctx& c) { return SkillBonusByName(Bot(c, 0), "Persuasion"); }, 3);
            ExpectEq(s, "with it: CHA 3 + proficiency 2", [](Ctx& c) { return SkillBonusByName(Bot(c, 1), "Persuasion"); }, 5);
            ExpectEq(s, "Deception too", [](Ctx& c) { return SkillBonusByName(Bot(c, 1), "Deception"); }, 5);
        });

        // ============================================================ edges: abilities meeting the rest of the game
        // (sneaking into conversations, deaths and cancelled looks; blasts that catch a second group; steps into walls;
        // what a fight leaves behind when it ends)
        add("edge.sneak_ends_when_the_stealth_look_is_taken_off", [=](Script& s)
        {
            // cancelling the Sneaking buff (or anything else that strips it) ends the sneak: no invisible-to-the-rules
            // rogue walking about in plain sight, and no plain-looking rogue the creatures can't see
            AddBot(s, { ROGUE, 2 });
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            ExpectCastOk(s);
            Expect(s, "sneaking", [](Ctx& c) { return IsSneaking(Bot(c)); });
            Act(s, [](Ctx& c) { Bot(c)->RemoveAurasDueToSpell(SPELL_SNEAK_AURA); });
            Expect(s, "the look is gone, so is the sneak", [](Ctx& c) { return !IsSneaking(Bot(c)); });
            AddMob(s, {});
            Expect(s, "a creature right there just sees them (no roll)", [](Ctx& c) { return SpotsSneaker(Mob(c), Bot(c)); });
        });
        add("edge.sneak_ends_when_you_die", [=](Script& s)
        {
            AddBot(s, { ROGUE, 2 });
            Rig(s, { 15, 15 }, 0);
            Cast(s, SPELL_HIDE, -1);
            Expect(s, "sneaking", [](Ctx& c) { return IsSneaking(Bot(c)); });
            Act(s, [](Ctx& c) { Unit::Kill(Bot(c), Bot(c)); });
            Expect(s, "dead, and no longer sneaking", [](Ctx& c) { return !Bot(c)->IsAlive() && !IsSneaking(Bot(c)); });
            Act(s, [](Ctx& c)
            {
                Bot(c)->ResurrectPlayer(1.f);
                Check(c, !IsSneaking(Bot(c)) && !Bot(c)->HasAura(SPELL_SNEAK_AURA), "up again in plain sight");
            });
        });
        uint32 talkerId = 0;
        for (EncounterDef const* def : AllEncounters())
            if (def->dialogueNode && !def->spawns.empty())
                if (CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front()))
                    if (d->mapid == 0 && !talkerId)
                        talkerId = def->id;
        add("edge.sneak_ends_when_you_talk", [=](Script& s)
        {
            // a sneaking rogue who walks up and right-clicks a talker has shown themselves: if the talk turns into a
            // fight, nobody is Surprised and the rogue isn't Hidden
            WorldSetup(s, talkerId, 0.f, ROGUE, 3);
            Rig(s, { 20, 20 }, 1);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* p = Bot(c);
                Prep(p, SPELL_HIDE);
                c.v["cast"] = float(p->CastSpell(p, SPELL_HIDE, false));
            });
            s.Wait(700);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* p = Bot(c);
                Check(c, IsSneaking(p), "sneaking, out of the talker's reach");
                Check(c, StartDialogue(p, Mob(c, 0)), "the talk opens (a right-click)");
                Check(c, InDialogue(p), "in the conversation");
                Check(c, !IsSneaking(p) && !p->HasAura(SPELL_SNEAK_AURA), "talking shows yourself");
                CloseDialogue(p, false);
                StartEncounter(Mob(c, 0), p);                 // the talk goes bad
                Check(c, InCombat(p), "the fight started");
                if (!InCombat(p))
                    return;
                bool surprised = false;
                for (Combatant const& x : Enc(c)->order)
                    surprised = surprised || x.surprised;
                Check(c, !surprised, "nobody is Surprised by someone they were just talking to");
                Check(c, !BotC(c)->hidden, "and the rogue isn't Hidden");
            });
        });
        add("edge.a_blast_pulls_a_second_group_into_the_fight", [=](Script& s)
        {
            // a Fireball that catches creatures outside the fight brings them into it (they don't stand there burning)
            AddBot(s, { WIZ, 5 });
            AddMob(s, { 10.f, -1.f });
            StartFight(s);
            AddMob(s, { 11.f, 1.5f });                        // came after: not in the fight
            Act(s, [](Ctx& c)
            {
                Mob(c, 1)->SetFaction(14);                    // a hostile group that hasn't noticed yet
                Check(c, !InCombat(Mob(c, 1)), "the second creature starts outside the fight");
            });
            Rig(s, {}, 1);
            Cast(s, SPELL_FIREBALL);
            ExpectCastOk(s);
            ExpectDrop(s, 8, 0, "8d6 all 1s on the first");
            ExpectDrop(s, 8, 1, "and on the one outside the fight");
            Expect(s, "which has joined the same fight", [](Ctx& c)
            {
                return InCombat(Mob(c, 1)) && EncounterOf(Mob(c, 1)->GetGUID()) == Enc(c);
            });
            ExpectEq(s, "three in the order", [](Ctx& c) { return double(Enc(c)->order.size()); }, 3);
        });
        add("edge.misty_step_stops_at_a_wall", [=](Script& s)
        {
            // in the Stonefire Tavern: steps of 18 m every way; each must land somewhere the wizard could see from
            // where they stood (never through a wall), on the floor - or be refused for line of sight - and at least
            // one must have been stopped by a wall
            BotOpt o{ WIZ, 3 };
            o.at = true;
            o.map = 0;
            o.x = -4976.0f;
            o.y = -1288.0f;
            o.z = 502.1f;
            AddBot(s, o);
            s.Wait(1000);
            for (int dir = 0; dir < 4; ++dir)
            {
                float ang = float(M_PI) / 2.f * dir + 0.3f;
                Act(s, [](Ctx& c)
                {
                    Player* p = Bot(c);
                    LongRest(p);
                    p->NearTeleportTo(-4976.0f, -1288.0f, 502.1f, 0.f);
                    AckTeleport(p);
                });
                s.Wait(300);
                CastAt(s, SPELL_MISTY_STEP, 18.f * std::cos(ang), 18.f * std::sin(ang));
                Act(s, [](Ctx& c) { AckTeleport(Bot(c)); });
                s.Wait(300);
                Act(s, [dir](Ctx& c)
                {
                    Player* p = Bot(c);
                    int r = int(c.v["cast"]);
                    if (r != SPELL_CAST_OK)
                    {
                        // a point behind a wall: WoW refuses the cast (line of sight) - fine, that's blocked too
                        Check(c, r == SPELL_FAILED_LINE_OF_SIGHT, Acore::StringFormat("step {}: refused only for sight (got {})", dir, r));
                        c.v["short"] += 1;
                        return;
                    }
                    float sx = c.v["fromx"], sy = c.v["fromy"];
                    bool sees = p->GetMap()->isInLineOfSight(sx, sy, 502.1f + 1.5f, p->GetPositionX(), p->GetPositionY(),
                        p->GetPositionZ() + 1.5f, p->GetPhaseMask(), LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
                    Check(c, sees, Acore::StringFormat("step {}: landed where the wizard could see", dir));
                    Check(c, std::fabs(p->GetPositionZ() - 502.1f) < 4.f, Acore::StringFormat("step {}: on the floor", dir));
                    double d = p->GetExactDist2d(sx, sy);
                    if (d < 17.f)
                        c.v["short"] += 1;
                });
            }
            Expect(s, "a wall stopped at least one step (refused or cut short)", [](Ctx& c) { return c.v["short"] >= 1; });
        });
        add("edge.held_ends_when_the_fight_resets", [=](Script& s)
        {
            // Hold Person, then the wizard runs off and the fight resets: the creature isn't left frozen
            AddBot(s, { WIZ, 3 });
            MobOpt person{ 1.8f, 0.f, Stats(HUMANOID) };
            person.st.speed = 0.f;
            AddMob(s, person);
            StartFight(s);
            Rig(s, { 1, 1 });
            Cast(s, SPELL_HOLD_PERSON);
            Expect(s, "held", [](Ctx& c) { return MobC(c)->held; });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                p->Relocate(p->GetPositionX() - 80.f, p->GetPositionY(), Ground(p->GetMap(), p->GetPositionX() - 80.f, p->GetPositionY()));
                p->UpdatePositionData();
                EndTurn(p);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to reset", 8000);
            Expect(s, "no longer frozen in place", [](Ctx& c)
            {
                return Mob(c)->GetUInt32Value(UNIT_NPC_EMOTESTATE) != EMOTE_STATE_STUN && !InCombat(Mob(c));
            });
        });
        add("edge.rage_ends_with_the_fight", [=](Script& s)
        {
            // a won fight ends the Rage: the buff goes, and the next fight starts calm (Rage to spend again)
            AddBot(s, { BARB, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35); });
            AddMob(s, weak());
            StartFight(s);
            Cast(s, SPELL_RAGE, -1);
            Expect(s, "raging", [](Ctx& c) { return BotC(c)->raging && Bot(c)->HasAura(SPELL_RAGE_AURA); });
            Rig(s, { 15, 15 }, 4);
            Cast(s, SPELL_ATTACK);
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to be won", 6000);
            Expect(s, "the Rage buff is gone", [](Ctx& c) { return !Bot(c)->HasAura(SPELL_RAGE_AURA); });
            AddMob(s, { 2.f, 3.f });
            Act(s, [](Ctx& c)
            {
                if (TempSummon* dead = Mob(c, 0)->ToTempSummon())   // the dead one out of the way: the new one leads
                    dead->UnSummon();
                c.mobs.erase(c.mobs.begin());
            });
            StartFight(s);
            Expect(s, "the next fight starts calm", [](Ctx& c) { return !BotC(c)->raging; });
            Cast(s, SPELL_RAGE, -1);
            ExpectCastOk(s);
            ExpectEq(s, "and the second Rage is spent", [](Ctx& c) { return GetSheet(Bot(c)).rageUsed; }, 2);
        });

        // ============================================================ multiplayer
        // fights: a second (and third) player joins by walking up, or by aiming at the fight from further off; the
        // creatures turn on whoever is in it; the win is shared; a player who leaves doesn't end it for the rest.
        // conversations: players standing by see the talk on their own panel (lines, options, rolls) but can't pick.
        add("multi.walking_up_joins_the_fight", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 50.f, a->GetPositionY());
                ClearMsgs();
                OnPlayerMoved(b, b->GetPosition());
                Check(c, !InCombat(b), "60 m from the creature: not in the fight");
            });
            s.Wait(600);                                          // (join checks are twice a second at most)
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                sDice.assign({ 10, 10 });
                MoveBotTo(b, a->GetPositionX() - 15.f, a->GetPositionY());   // 25 m off: inside the 30 m join range
                sDice.clear();
                Check(c, InCombat(b), "25 m from the creature: in the fight");
                if (!InCombat(b))
                    return;
                Check(c, EncounterOf(b->GetGUID()) == Enc(c), "the same fight as the first player");
                CheckEq(c, double(Enc(c)->order.size()), 3, "three in the order");
                Check(c, IsTurnOf(a), "whoever's turn it was keeps it");
                Check(c, SawBy(a, "Jigwiz joins the fight"), "the first player is told who joined");
                Check(c, SawBy(b, "V^COMBAT"), "the joiner gets the combat banner");
                Check(c, b->HasUnitState(UNIT_STATE_ROOT), "and is held until their turn");
            });
        });
        add("multi.a_joiner_waits_for_their_turn_then_hits", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);                                        // the fighter 20, the creature 1
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                sDice.assign({ 15, 15 });                         // the wizard rolls 15: after the fighter, before the creature
                AddBotAt(c, WIZ, 3, a->GetPositionX() - 4.f, a->GetPositionY());     // 14 m from it: in Fire Bolt's reach
                OnPlayerMoved(Bot(c, 1), Bot(c, 1)->GetPosition());
                sDice.clear();
                Check(c, InCombat(Bot(c, 1)), "the wizard joined");
                CastAs(c, 1, SPELL_FIRE_BOLT, Mob(c));
            });
            s.Wait(700);
            Expect(s, "the wizard's Fire Bolt waits: not their turn", [](Ctx& c)
            {
                return int(c.v["cast"]) != SPELL_CAST_OK && SawBy(Bot(c, 1), "not your turn") && Dropped(c, 0) == 0;
            });
            Act(s, [](Ctx& c) { Prep(Bot(c), SPELL_END_TURN); Bot(c)->CastSpell(Bot(c), SPELL_END_TURN, false); });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c, 1)); }, "the wizard's turn (initiative 15, next after the fighter)", 6000);
            Rig(s, { 18 }, 1);                                   // 18 + 5 hits AC 10; the d10 rolls 1
            Act(s, [](Ctx& c) { CastAs(c, 1, SPELL_FIRE_BOLT, Mob(c)); });
            s.Until([](Ctx& c) { return !Bot(c, 1)->IsNonMeleeSpellCast(true, false, true, false, false); }, "the cast to finish", 8000);
            s.Wait(700);
            Expect(s, "on their turn the joiner's spell lands on the creature", [](Ctx& c)
            {
                return int(c.v["cast"]) == SPELL_CAST_OK && Dropped(c, 0) >= 1;
            });
        });
        add("multi.a_shot_from_a_bystander_joins_the_fight", [=](Script& s)
        {
            // joining by walking up happens as you move: someone who was already standing there (the fight came to them,
            // or they were busy when it started) is outside it. Their shot brings them in - rolled into the order like
            // anyone joining - instead of landing as a free hit from outside the turns
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Creature* m = Mob(c);
                Player* b = AddBotAt(c, WIZ, 3, m->GetPositionX() - 14.f, m->GetPositionY());   // hasn't moved since
                Check(c, !InCombat(b), "standing 14 m off, never moved: not in the fight");
                CastAs(c, 1, SPELL_FIRE_BOLT, m);
            });
            s.Wait(700);
            Act(s, [](Ctx& c)
            {
                Player* b = Bot(c, 1);
                Check(c, int(c.v["cast"]) != SPELL_CAST_OK && SawBy(b, "You join the fight"), "the shot joins the fight instead");
                Check(c, InCombat(b) && EncounterOf(b->GetGUID()) == Enc(c), "the wizard is in the same fight");
                CheckEq(c, double(Mob(c)->GetMaxHealth()) - double(Mob(c)->GetHealth()), 0, "no free hit from outside the order");
                Check(c, IsTurnOf(Bot(c)), "the fighter's turn goes on");
                Check(c, SawBy(Bot(c), "Jigwiz joins the fight"), "the fighter is told");
            });
        });
        add("multi.a_blast_landing_in_a_fight_joins_it", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Creature* m = Mob(c);
                Player* b = AddBotAt(c, WIZ, 5, m->GetPositionX() - 14.f, m->GetPositionY() + 1.f);   // a bystander, as above
                Check(c, !InCombat(b), "not in the fight to begin with");
                Remember(c);
                ClearMsgs();
                Prep(b, SPELL_FIREBALL);
                b->SetOrientation(b->GetAngle(m));
                c.v["cast"] = float(b->CastSpell(m->GetPositionX(), m->GetPositionY(), m->GetPositionZ(), SPELL_FIREBALL, false));
            });
            s.Wait(700);
            Act(s, [](Ctx& c)
            {
                Player* b = Bot(c, 1);
                Check(c, int(c.v["cast"]) != SPELL_CAST_OK && SawBy(b, "You join the fight"), "the blast joins the fight instead");
                Check(c, InCombat(b) && EncounterOf(b->GetGUID()) == Enc(c), "the wizard is in the same fight");
                CheckEq(c, double(Mob(c)->GetMaxHealth()) - double(Mob(c)->GetHealth()), 0, "nothing burned outside the order");
                CheckEq(c, double(GetSheet(b).slotsUsed[3]), 0, "and the slot isn't spent");
            });
        });
        add("multi.the_creatures_turn_on_whoever_joined", [=](Script& s)
        {
            // the wizard comes up on the creature's far side, closer to it than the fighter: on its turn it goes for them
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Creature* m = Mob(c);
                sDice.assign({ 10, 10 });
                Player* b = AddBotAt(c, WIZ, 3, m->GetPositionX() + 2.5f, m->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                sDice.clear();
                Check(c, InCombat(b), "the wizard joined");
                c.v["ahp"] = float(Bot(c)->GetHealth());
                c.v["bhp"] = float(b->GetHealth());
            });
            Rig(s, {}, 15);                                       // every roll 15: the creature's 15 + 4 hits
            Act(s, [](Ctx& c) { Prep(Bot(c), SPELL_END_TURN); Bot(c)->CastSpell(Bot(c), SPELL_END_TURN, false); });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c, 1)); }, "the wizard's turn", 6000);
            Act(s, [](Ctx& c) { Prep(Bot(c, 1), SPELL_END_TURN); Bot(c, 1)->CastSpell(Bot(c, 1), SPELL_END_TURN, false); });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c)); }, "round two: the fighter again", 30000);
            Expect(s, "the creature hit the wizard, who stood closest", [](Ctx& c)
            {
                return float(Bot(c, 1)->GetHealth()) < c.v["bhp"];
            });
            Expect(s, "and left the fighter alone", [](Ctx& c) { return float(Bot(c)->GetHealth()) == c.v["ahp"]; });
        });
        add("multi.the_win_is_shared_and_a_bystander_gets_nothing", [=](Script& s)
        {
            // an ad-hoc fight splits the creature's XP among the players in it: 100 between two is 50 each; a third
            // player watching from 60 m off was never in it
            AddBot(s, { FIGHTER, 3 });
            MobOpt mo{ 10.f, 0.f };
            mo.st.xp = 100;
            AddMob(s, mo);
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 15.f, a->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                Player* away = AddBotAt(c, ROGUE, 3, a->GetPositionX() - 50.f, a->GetPositionY());
                OnPlayerMoved(away, away->GetPosition());
                Check(c, InCombat(b) && !InCombat(away), "the wizard is in, the rogue 60 m off is not");
                for (std::size_t i = 0; i < 3; ++i)
                    c.v["xp" + std::to_string(i)] = float(Bot(c, i)->GetUInt32Value(PLAYER_XP));
                DamageCtx d;
                d.attacker = a;
                d.victim = Mob(c);
                d.amount = 1000000;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)) && !InCombat(Bot(c, 1)); }, "the fight to be won", 6000);
            Act(s, [](Ctx& c)
            {
                for (std::size_t i = 0; i < 3; ++i)
                {
                    double got = double(Bot(c, i)->GetUInt32Value(PLAYER_XP)) - c.v["xp" + std::to_string(i)];
                    CheckEq(c, got, i < 2 ? 50 : 0, Acore::StringFormat("XP for player {}", i + 1));
                }
            });
        });
        add("multi.an_encounter_won_counts_for_everyone_online", [=](Script& s)
        {
            // a win anywhere pays every player online the encounter's whole XP (not split) and clears it for them -
            // even one 200 m off who never saw it; a second win of it pays nobody again
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 1, a->GetPositionX() - 200.f, a->GetPositionY());
                EncounterDef const* def = nullptr;
                for (EncounterDef const* d : AllEncounters())
                    if (d->xp > 0 && d->xp < sObjectMgr->GetXPForLevel(1))
                    {
                        def = d;
                        break;
                    }
                Check(c, def != nullptr, "an encounter worth less than a level");
                if (!def)
                    return;
                ClearMsgs();
                ShareEncounterWon({ a }, *def);
                for (Player* p : { a, b })
                {
                    Check(c, HasCleared(p, def->id), std::string(p->GetName()) + ": cleared");
                    CheckEq(c, double(TotalXp(p)), double(def->xp), std::string(p->GetName()) + ": the whole XP");
                }
                Check(c, SawBy(b, "Jigfighter won " + def->name + " - it counts for you too."), "the far player is told who won");
                Check(c, !SawBy(a, "counts for you too"), "the winner isn't told it counts for them too");
                ShareEncounterWon({ b }, *def);
                for (Player* p : { a, b })
                    CheckEq(c, double(TotalXp(p)), double(def->xp), std::string(p->GetName()) + ": nothing for a second win");
            });
        });
        add("multi.the_fight_goes_on_when_one_player_leaves", [=](Script& s)
        {
            // the fighter logs out on their own turn: the fight carries on for the wizard, whose turn comes up
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                sDice.assign({ 15, 15 });
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 15.f, a->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                sDice.clear();
                Check(c, InCombat(b), "the wizard joined");
                OnPlayerLeft(a);                                  // what logging out does to the fight
                Check(c, !InCombat(a), "the fighter is out of it");
                Check(c, InCombat(b), "the wizard is still fighting");
            });
            s.Until([](Ctx& c) { return IsTurnOf(Bot(c, 1)); }, "the turn passes to the wizard", 6000);
            Expect(s, "the creature is still in the fight", [](Ctx& c) { return InCombat(Mob(c)); });
        });

        // party tuning, worked from the rule: every fight is tuned for Dnd.PartySize players whoever turns up, and no
        // creature is ever brought in for it - a group's hit points x(1 + 0.5 per player beyond the first), a lone
        // creature's x(1 + 0.75 per) and a legendary action a round per (the tests run tuned for 1 unless they say)
        add("multi.tuned_for_the_party_size_not_for_who_turns_up", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });                             // a 300 HP leader
            MobOpt follower{ 11.f, 2.f };
            follower.st = Stats(BEAST, 10, 100);
            AddMob(s, follower);
            Act(s, [](Ctx&) { SetTunedPartySize(2); });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                auto extras = [&]() { int n = 0; for (Combatant const& x : Enc(c)->order) n += x.extra; return n; };
                CheckEq(c, Mob(c)->GetMaxHealth(), 450, "tuned for two, one player there: the leader 300 x 1.5");
                CheckEq(c, Mob(c, 1)->GetMaxHealth(), 150, "the follower 100 x 1.5");
                CheckEq(c, extras(), 0, "nobody brought in");
                CheckEq(c, MobC(c)->legendaryMax, 0, "a group: no legendary actions");
                Mob(c)->SetHealth(430);                           // 20 damage taken
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 4.f, a->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                Player* c3 = AddBotAt(c, ROGUE, 3, a->GetPositionX() - 4.f, a->GetPositionY() + 2.f);
                OnPlayerMoved(c3, c3->GetPosition());
                Check(c, InCombat(b) && InCombat(c3), "two more players join");
                CheckEq(c, Mob(c)->GetMaxHealth(), 450, "and nothing changes: still 450");
                CheckEq(c, Mob(c)->GetHealth(), 430, "still 20 down");
                CheckEq(c, extras(), 0, "still nobody brought in");
                OnPlayerLeft(c3);
                OnPlayerLeft(b);
                CheckEq(c, Mob(c)->GetMaxHealth(), 450, "they leave: still 450");
            });
        });
        add("multi.party_scaling_leaves_creature_damage_alone", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                auto hit = [&](int amount) -> int
                {
                    a->SetHealth(a->GetMaxHealth());
                    GetSheet(a).tempHp = 0;
                    uint32 before = a->GetHealth();
                    DamageCtx d;
                    d.attacker = Mob(c);
                    d.victim = a;
                    d.amount = amount;
                    d.type = DMG_FORCE;
                    ApplyDamage(d);
                    return int(before) - int(a->GetHealth());
                };
                CheckEq(c, hit(8), 8, "alone: 8 damage is 8");
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 4.f, a->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                CheckEq(c, hit(8), 8, "a party of two: still 8 (the party's threat is bodies and actions)");
                Player* c3 = AddBotAt(c, ROGUE, 3, a->GetPositionX() - 4.f, a->GetPositionY() + 2.f);
                OnPlayerMoved(c3, c3->GetPosition());
                Player* d4 = AddBotAt(c, MONK, 3, a->GetPositionX() - 4.f, a->GetPositionY() - 2.f);
                OnPlayerMoved(d4, d4->GetPosition());
                CheckEq(c, hit(8), 8, "four: still 8");
                DamageCtx back;                                   // and players' damage to the creature is never scaled
                back.attacker = a;
                back.victim = Mob(c);
                back.amount = 8;
                back.type = DMG_FORCE;
                uint32 mh = Mob(c)->GetHealth();
                ApplyDamage(back);
                CheckEq(c, double(mh) - double(Mob(c)->GetHealth()), 8, "a player's 8 on the creature stays 8");
            });
        });
        add("multi.a_lone_creature_takes_legendary_actions", [=](Script& s)
        {
            // one creature, no underlings: a legendary action a round per player beyond the first, taken at the end of
            // someone else's turn against whoever is in reach
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            Act(s, [](Ctx&) { SetTunedPartySize(2); });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                CheckEq(c, MobC(c)->legendaryMax, 1, "tuned for two: one a round, even with one player there");
                CheckEq(c, Mob(c)->GetMaxHealth(), 525, "and 300 x 1.75");
                Creature* m = Mob(c);
                sDice.assign({ 10, 10 });
                Player* b = AddBotAt(c, WIZ, 3, m->GetPositionX() + 2.f, m->GetPositionY());   // right beside it
                OnPlayerMoved(b, b->GetPosition());
                sDice.clear();
                CheckEq(c, MobC(c)->legendaryMax, 1, "a second player joins: still one a round");
                CheckEq(c, double(Enc(c)->order.size()), 3, "and no extra creature");
                ClearMsgs();
            });
            Rig(s, {}, 15);
            Act(s, [](Ctx& c) { Prep(Bot(c), SPELL_END_TURN); Bot(c)->CastSpell(Bot(c), SPELL_END_TURN, false); });
            s.Until([](Ctx& c) { return SawBy(Bot(c), "Legendary Action"); }, "a legendary action at the end of the fighter's turn", 6000);
            Expect(s, "at the wizard, the one in reach, and spent", [](Ctx& c)
            {
                return SawBy(Bot(c, 1), "Legendary Action (Test Claw)") && MobC(c)->legendary == 0;
            });
            Act(s, [](Ctx& c)
            {
                SetTunedPartySize(4);                             // (the setting changed: worked out again at the next join)
                Player* a = Bot(c);
                Player* c3 = AddBotAt(c, ROGUE, 3, a->GetPositionX() - 4.f, a->GetPositionY() + 2.f);
                OnPlayerMoved(c3, c3->GetPosition());
                CheckEq(c, MobC(c)->legendaryMax, 3, "tuned for four: three a round");
            });
        });
        add("multi.party_scaling_ends_with_the_fight", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 10.f, 0.f });
            Act(s, [](Ctx&) { SetTunedPartySize(2); });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                CheckEq(c, Mob(c)->GetMaxHealth(), 525, "tuned for two");
                AbortEncounter(a);
                CheckEq(c, Mob(c)->GetMaxHealth(), 300, "the fight over: the stat block's 300 again");
                Check(c, Mob(c)->GetHealth() <= 300, "and no more than that");
            });
        });

        add("endgame.three_magic_items_at_once", [=](Script& s)
        {
            // attunement: a fourth item with effects won't go on, and one worn anyway does nothing
            AddBot(s, { FIGHTER, 6 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                int stealth = SkillBonusByName(p, "Stealth");
                Equip(p, EQUIPMENT_SLOT_NECK, 5002);              // +1 to all saves
                Equip(p, EQUIPMENT_SLOT_FINGER1, 20906);          // +2 CON saves
                Equip(p, EQUIPMENT_SLOT_HANDS, 3314);             // +2 Persuasion, +2 Deception
                CheckEq(c, AttunedCount(p, nullptr, NULL_SLOT), 3, "three attuned");
                ItemPosCountVec dest;
                p->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, 10638, 1);
                Item* cloak = p->StoreNewItem(dest, 10638, true);
                Check(c, cloak != nullptr, "a fourth magic item in the bag");
                if (!cloak)
                    return;
                uint16 into = 0;
                CheckEq(c, int(p->CanEquipItem(EQUIPMENT_SLOT_BACK, into, cloak, false)), int(EQUIP_ERR_CANT_DO_RIGHT_NOW),
                    "it won't go on");
                Check(c, Saw("attuned to 3 magic items"), "and says why");
                uint16 neck = 0;
                Item* amulet = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_NECK);
                (void)amulet;
                ItemPosCountVec d2;
                p->CanStoreNewItem(NULL_BAG, NULL_SLOT, d2, 21934, 1);
                Item* stone = p->StoreNewItem(d2, 21934, true);
                Check(c, stone && p->CanEquipItem(EQUIPMENT_SLOT_NECK, neck, stone, true) == EQUIP_ERR_OK,
                    "but one can take another's place");
                Equip(p, EQUIPMENT_SLOT_BACK, 10638);             // (worn anyway, past the rule)
                CheckEq(c, SkillBonusByName(p, "Stealth"), stealth, "a fourth worn anyway: its Stealth bonus does nothing");
            });
        });
        add("endgame.an_elixir_lasts_one_fight", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 6 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, !ItemResists(p, DMG_NECROTIC), "no necrotic resistance to start with");
                p->CastSpell(p, 11371, true);                    // the Elixir of the Grave Ward's spell
                CheckEq(c, double(ActiveElixir(p)), 9088, "the elixir is active");
                Check(c, ItemResists(p, DMG_NECROTIC) && ItemResists(p, DMG_POISON), "resisting Necrotic and Poison");
                Check(c, !p->HasAura(11371), "and the stock aura never lands");
            });
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                CheckEq(c, double(ActiveElixir(Bot(c))), 9088, "it lasts into the fight");
                AbortEncounter(Bot(c));
            });
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                CheckEq(c, double(ActiveElixir(Bot(c))), 0, "and wears off with it");
                Check(c, !ItemResists(Bot(c), DMG_NECROTIC), "the resistance gone");
            });
        });
        add("endgame.an_elixir_drunk_mid_fight_gives_its_temporary_hit_points", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 6 });
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                GetSheet(Bot(c)).tempHp = 0;
                DrinkElixir(Bot(c), 3825);                       // Elixir of Vigour: 10 temporary hit points
                CheckEq(c, GetSheet(Bot(c)).tempHp, 10, "10 temporary hit points at once");
                AbortEncounter(Bot(c));
            });
        });
        add("endgame.the_rewards_items_coin_and_a_title", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 6 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                EncounterDef const* def = GetEncounterDef(222);
                Check(c, def && HasRewards(222), "the queen pays rewards");
                if (!def)
                    return;
                CheckEq(c, double(def->xp), 0, "and no XP: the levels stop at 6");
                uint32 money = p->GetMoney();
                uint32 xp = p->GetUInt32Value(PLAYER_XP);
                OnEncounterWon(p, *def);
                CheckEq(c, double(p->GetMoney() - money), 30000, "300 gold");
                Check(c, p->HasItemCount(18713, 1), "the Windrunner Longbow");
                CharTitlesEntry const* t = sCharTitlesStore.LookupEntry(178);
                Check(c, t && p->HasTitle(t), "and the title");
                CheckEq(c, double(p->GetUInt32Value(PLAYER_XP)), double(xp), "no XP");
                Check(c, std::any_of(sAnnounced.begin(), sAnnounced.end(), [](std::string const& a)
                    { return a.find("Queenslayer") != std::string::npos; }), "the title is announced");
                money = p->GetMoney();
                OnEncounterWon(p, *def);
                CheckEq(c, double(p->GetMoney() - money), 0, "a second clear pays nothing");
            });
        });
        add("endgame.the_kalimdor_capitals_send_you_home", [=](Script& s)
        {
            // Thunder Bluff and Orgrimmar (map 1): a keeper and Brother Marrow by each bank, and the keeper's way home.
            // (Not walked: a headless bot's cross-continent teleport crashes the server - the same code's near-teleport
            // round trip is a_portal_keeper_sends_a_party_and_back. Here only a destination that isn't on the list is tried.)
            struct Cap { uint32 keeper; float x, y, z; char const* name; };
            static Cap const caps[] = { { 9102003, -1255.5f, 28.0f, 128.3f, "Thunder Bluff" },
                                        { 9102004, 1624.8f, -4371.5f, 12.2f, "Orgrimmar" } };
            for (int i = 0; i < 2; ++i)
            {
                BotOpt o{ FIGHTER, 5 };
                o.at = true;
                o.map = 1;
                o.x = caps[i].x;
                o.y = caps[i].y;
                o.z = caps[i].z;
                AddBot(s, o);
            }
            s.Wait(2000);
            Act(s, [](Ctx& c)
            {
                for (int i = 0; i < 2; ++i)
                {
                    Player* p = Bot(c, i);
                    std::list<Creature*> keepers, marrow;
                    p->GetCreatureListWithEntryInGrid(keepers, caps[i].keeper, 40.f);
                    p->GetCreatureListWithEntryInGrid(marrow, 6491, 40.f);
                    Check(c, !keepers.empty(), std::string(caps[i].name) + ": a portal keeper by the bank");
                    Check(c, !marrow.empty(), std::string(caps[i].name) + ": and Brother Marrow");
                    uint32 zone = p->GetZoneId();
                    Check(c, zone == (i == 0 ? 1638u : 1637u), Acore::StringFormat("{}: standing in its zone ({})", caps[i].name, zone));
                    if (keepers.empty())
                        continue;
                    Check(c, !IsTownsfolk(keepers.front()), std::string(caps[i].name) + ": the keeper has her own menu");
                    std::string why;
                    Check(c, !PortalSend(p, keepers.front(), 5, why) && p->GetMapId() == 1,
                        std::string(caps[i].name) + ": no such destination - nothing happens");
                }
            });
        });
        add("endgame.a_portal_keeper_sends_a_party_and_back", [=](Script& s)
        {
            BotOpt o{ FIGHTER, 5 };
            o.at = true;
            o.map = 0;
            o.x = -9003.f;
            o.y = 846.f;
            o.z = 106.f;
            AddBot(s, o);
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                std::list<Creature*> keepers;
                p->GetCreatureListWithEntryInGrid(keepers, 9102001, 40.f);
                Creature* k = keepers.empty() ? nullptr : keepers.front();
                Check(c, k != nullptr, "the portal keeper stands in the Wizard's Sanctum");
                if (!k)
                    return;
                Check(c, !IsTownsfolk(k), "with her own menu, not a villager's talk");
                std::string why;
                Check(c, !PortalSend(p, k, 0, why) && why.find("level 6") != std::string::npos, "level 5: not sent (" + why + ")");
                why.clear();
                Check(c, !PortalSend(p, k, 1, why) && why.find("level 6") != std::string::npos, "nor to Thunder Bluff (" + why + ")");
                p->GiveLevel(6);
                why.clear();
                Check(c, PortalSend(p, k, 0, why), "level 6: sent to the Undercity");
                AckTeleport(p);
            });
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                AckTeleport(p);
                Check(c, p->GetExactDist2d(1791.f, 127.5f) < 15.f, Acore::StringFormat("in the Undercity's portal room ({:.0f}, {:.0f})",
                    p->GetPositionX(), p->GetPositionY()));
                Check(c, p->GetZoneId() == 1497, "the zone is the Undercity");
                std::list<Creature*> keepers;
                p->GetCreatureListWithEntryInGrid(keepers, 9102002, 40.f);
                Check(c, !keepers.empty(), "with a keeper to send them home");
                std::list<Creature*> marrow;
                p->GetCreatureListWithEntryInGrid(marrow, 6491, 40.f);
                Check(c, !marrow.empty(), "and Brother Marrow");
                if (keepers.empty())
                    return;
                std::string why;
                Check(c, PortalSend(p, keepers.front(), 0, why), "home to Stormwind");
                AckTeleport(p);
            });
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                AckTeleport(Bot(c));
                Check(c, Bot(c)->GetExactDist2d(-9003.f, 846.f) < 15.f, "back in the Sanctum");
            });
        });

        // ============================================================ magic gear (MAGIC in tools/make_treats_sql.py)
        add("items.saves_skills_scores_and_speed", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                int wis = SaveBonus(p, WIS), con = SaveBonus(p, CON);
                int pers = SkillBonusByName(p, "Persuasion"), stealth = SkillBonusByName(p, "Stealth");
                int intel = AbilityScore(p, INT);
                float speed = Speed(p);
                Equip(p, EQUIPMENT_SLOT_NECK, 5002);              // +1 to all saves
                Equip(p, EQUIPMENT_SLOT_FINGER1, 20906);          // +2 CON saves
                CheckEq(c, SaveBonus(p, WIS) - wis, 1, "amulet: +1 to a WIS save");
                CheckEq(c, SaveBonus(p, CON) - con, 3, "amulet and ring: +3 to a CON save");
                StripGear(p);                                     // (three effect items at a time: attunement)
                Equip(p, EQUIPMENT_SLOT_HANDS, 3314);             // +2 Persuasion, +2 Deception
                Equip(p, EQUIPMENT_SLOT_BACK, 10638);             // +2 Stealth (advantage), -1 Persuasion
                CheckEq(c, SkillBonusByName(p, "Persuasion") - pers, 1, "gloves +2, cloak -1: Persuasion +1");
                CheckEq(c, SkillBonusByName(p, "Stealth") - stealth, 2, "cloak: Stealth +2");
                Check(c, ItemSkillAdvantage(p, "Stealth") && !ItemSkillAdvantage(p, "Persuasion"), "and advantage on Stealth only");
                StripGear(p);
                Equip(p, EQUIPMENT_SLOT_HEAD, 3556);              // INT 17
                CheckEq(c, intel, 8, "the fighter's INT is 8");
                CheckEq(c, AbilityScore(p, INT), 17, "the circlet: 17");
                Equip(p, EQUIPMENT_SLOT_FEET, 2569);              // +1.5 m
                CheckEq(c, Speed(p) - speed, 1.5, "boots: +1.5 m");
                Equip(p, EQUIPMENT_SLOT_NECK, 5002);              // the warding amulet again (the third)
                CheckEq(c, SaveBonus(p, WIS) - wis, 1, "the amulet back on: +1 to a WIS save");
                Equip(p, EQUIPMENT_SLOT_NECK, 21934);             // (the amulet swapped for +2 initiative)
                CheckEq(c, ItemInitiative(p), 2, "the lucky stone: +2 initiative");
                CheckEq(c, SaveBonus(p, WIS) - wis, 0, "and the warding amulet's +1 is gone");
            });
        });
        add("items.resistance_reduction_and_thorns", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                auto take = [&](int amount, DmgType type) -> int
                {
                    p->SetHealth(p->GetMaxHealth());
                    GetSheet(p).tempHp = 0;
                    uint32 before = p->GetHealth();
                    DamageCtx d;
                    d.attacker = Mob(c);
                    d.victim = p;
                    d.amount = amount;
                    d.type = type;
                    ApplyDamage(d);
                    return int(before) - int(p->GetHealth());
                };
                Equip(p, EQUIPMENT_SLOT_BACK, 2580);              // resist Cold
                CheckEq(c, take(10, DMG_COLD), 5, "the cape: 10 Cold halved");
                CheckEq(c, take(10, DMG_FIRE), 10, "but not Fire");
                Equip(p, EQUIPMENT_SLOT_WRISTS, 6543);            // -1 damage from enemies
                CheckEq(c, take(10, DMG_FIRE), 9, "the bracers: 10 Fire, less 1");
                CheckEq(c, take(10, DMG_COLD), 4, "10 Cold: halved, then less 1");
                Equip(p, EQUIPMENT_SLOT_WRISTS, 6550);            // thorns 1d4 Piercing
                uint32 mob = Mob(c)->GetHealth();
                sFallback = 3;
                take(6, DMG_SLASHING);
                sFallback = 0;
                CheckEq(c, double(mob) - double(Mob(c)->GetHealth()), 3, "the spiked wristguards: the creature beside them takes 1d4 (3)");
            });
        });
        add("items.temp_hp_when_a_fight_starts", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_WAIST, 2899); GetSheet(Bot(c)).tempHp = 0; });
            AddMob(s, { 6.f, 0.f });
            StartFight(s);
            ExpectEq(s, "the belt: 5 temporary hit points", [](Ctx& c) { return GetSheet(Bot(c)).tempHp; }, 5);
        });
        add("items.immunities", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, !ItemImmune(p, "prone") && !ItemImmune(p, "poisoned"), "nothing worn: no immunities");
                Equip(p, EQUIPMENT_SLOT_OFFHAND, 7108);
                Equip(p, EQUIPMENT_SLOT_FEET, 22992);
                Check(c, ItemImmune(p, "prone"), "the shield: can't be knocked prone");
                Check(c, ItemImmune(p, "poisoned"), "the boots: can't be poisoned");
                CheckEq(c, ArmorClass(p) >= 12, 1, "and the shield still gives its +2 (10 + DEX 2 + 2 = 14)");
            });
        });
        // on a weapon hit: the quarterstaff (1d6, every die a 1) + STR 3 = 4, then what the gear adds
        auto staffHit = [=](Script& s, uint32 gear, uint8 slot, MobOpt mob)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [gear, slot](Ctx& c)
            {
                Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35);
                if (gear)
                    Equip(Bot(c), slot, gear);
            });
            AddMob(s, mob);
            StartFight(s);
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_ATTACK);
        };
        add("items.gauntlets_add_fire_to_a_hit", [=](Script& s)
        {
            staffHit(s, 3472, EQUIPMENT_SLOT_HANDS, MobOpt{ 2.f, 0.f });
            ExpectDrop(s, 5, 0, "4 from the staff + 1d4 Fire (1)");
            ExpectSaw(s, "Forgeheart Gauntlets");
        });
        add("items.a_bane_only_bites_its_kind", [=](Script& s)
        {
            MobOpt beast{ 2.f, 0.f };
            beast.st = Stats(BEAST);
            staffHit(s, 11965, EQUIPMENT_SLOT_FINGER1, beast);
            ExpectDrop(s, 5, 0, "a beast: 4 + 1d6 (1)");
        });
        add("items.a_bane_leaves_others_alone", [=](Script& s)
        {
            staffHit(s, 11965, EQUIPMENT_SLOT_FINGER1, MobOpt{ 2.f, 0.f });   // a humanoid
            ExpectDrop(s, 4, 0, "a humanoid: just the 4");
        });
        add("items.a_glaive_hit_can_trip", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c) { Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 1406); });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Rig(s, { 15, 15 }, 1);                               // the hit, then the creature's DEX save rolls 1
            Cast(s, SPELL_ATTACK);
            Expect(s, "it failed its DEX save: Prone", [](Ctx& c) { return MobC(c)->prone; });
        });
        add("items.a_kill_heals", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            Act(s, [](Ctx& c)
            {
                Equip(Bot(c), EQUIPMENT_SLOT_MAINHAND, 35);
                Equip(Bot(c), EQUIPMENT_SLOT_FINGER1, 22979);
            });
            MobOpt frail{ 2.f, 0.f };
            frail.st = Stats(HUMANOID, 10, 2);
            AddMob(s, frail);
            StartFight(s);
            Act(s, [](Ctx& c) { Bot(c)->SetHealth(Bot(c)->GetMaxHealth() - 5); c.v["hp"] = float(Bot(c)->GetHealth()); });
            Rig(s, { 15, 15 }, 1);
            Cast(s, SPELL_ATTACK);
            ExpectEq(s, "the kill: +3 hit points", [](Ctx& c) { return double(Bot(c)->GetHealth()) - c.v["hp"]; }, 3);
        });

        // ============================================================ downed, death saves, Help, Brother Marrow
        // the fighter (bot 0) beside a creature, the wizard (bot 1) 8 m off in the same fight; the fighter is knocked down
        auto downSetup = [=](Script& s, bool second = true)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            if (second)
                Act(s, [](Ctx& c)
                {
                    Player* a = Bot(c);
                    Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 8.f, a->GetPositionY());
                    OnPlayerMoved(b, b->GetPosition());
                    Check(c, InCombat(b), "the wizard is in the fight");
                });
            Act(s, [](Ctx& c)
            {
                ClearMsgs();
                sAnnounced.clear();
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = Bot(c);
                d.amount = int(Bot(c)->GetHealth());
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
        };
        auto win = [](Script& s)
        {
            Act(s, [](Ctx& c)
            {
                DamageCtx d;
                d.attacker = Bot(c, 1);
                d.victim = Mob(c);
                d.amount = 1000000;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c, 1)); }, "the fight to be won", 6000);
        };
        add("down.zero_hp_knocks_you_down_not_dead", [=](Script& s)
        {
            downSetup(s);
            Expect(s, "down, not dead: alive at 1 HP", [](Ctx& c) { return IsDowned(Bot(c)) && Bot(c)->IsAlive() && Bot(c)->GetHealth() == 1; });
            ExpectSaw(s, "is knocked down");
            Expect(s, "lying there: Feign Death's pose, rooted", [](Ctx& c)
            {
                return Bot(c)->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH) && Bot(c)->HasUnitState(UNIT_STATE_ROOT);
            });
            Expect(s, "the fight goes on for the wizard", [](Ctx& c) { return InCombat(Bot(c, 1)) && InCombat(Bot(c)); });
            Act(s, [](Ctx& c)
            {
                ClearMsgs();
                PushOrder(*Enc(c));
                bool shown = false;
                for (std::string const& m : MsgsBy(Bot(c), "O^"))
                {
                    std::vector<std::string> f = Split(m, '^');
                    if (f.size() > 14 && f[2] == "Jigfighter")
                        shown = f[3] == "0" && f[14] == "D00";
                }
                Check(c, shown, "the turn order shows them down: 0 HP, no saves yet");
            });
        });
        add("down.three_successes_and_you_are_stable", [=](Script& s)
        {
            downSetup(s);
            Act(s, [](Ctx& c)
            {
                for (int i = 0; i < 3; ++i)
                {
                    sDice.assign({ 12, 12 });
                    RollDeathSave(Bot(c));
                }
                sDice.clear();
                Check(c, BotC(c)->stable && IsDowned(Bot(c)), "three 12s: stable, still down");
                DamageCtx d;                                  // the creature beside them hits them anyway
                d.attacker = Mob(c);
                d.victim = Bot(c);
                d.amount = 3;
                d.type = DMG_FORCE;
                ApplyDamage(d);
                Check(c, !BotC(c)->stable, "hit while down: dying again");
                CheckEq(c, BotC(c)->saveFail, 2, "two failures (from close by)");
                CheckEq(c, Bot(c)->GetHealth(), 1, "and no hit points lost");
            });
        });
        add("down.three_failures_and_you_die", [=](Script& s)
        {
            downSetup(s);
            Act(s, [](Ctx& c)
            {
                for (int i = 0; i < 3; ++i)
                {
                    sDice.assign({ 5, 5 });
                    RollDeathSave(Bot(c));
                }
                sDice.clear();
                Check(c, !Bot(c)->IsAlive(), "three 5s: dead");
                Check(c, WasAnnounced("Jigfighter has fallen fighting"), "and everyone is told");
                Check(c, InCombat(Bot(c, 1)), "the wizard fights on");
            });
        });
        add("down.natural_1_counts_twice_and_natural_20_gets_you_up", [=](Script& s)
        {
            downSetup(s);
            Act(s, [](Ctx& c)
            {
                sDice.assign({ 1, 1 });
                RollDeathSave(Bot(c));
                CheckEq(c, BotC(c)->saveFail, 2, "a natural 1: two failures");
                sDice.assign({ 20, 20 });
                RollDeathSave(Bot(c));
                sDice.clear();
                Check(c, !IsDowned(Bot(c)) && Bot(c)->GetHealth() == 1, "a natural 20: up with 1 HP");
                CheckEq(c, BotC(c)->saveFail, 0, "the saves wiped");
            });
        });
        add("down.the_downed_cannot_act", [=](Script& s)
        {
            downSetup(s);
            Act(s, [](Ctx& c) { CastAs(c, 0, SPELL_ATTACK, Mob(c)); });
            Expect(s, "refused", [](Ctx& c) { return int(c.v["cast"]) != SPELL_CAST_OK && SawBy(Bot(c), "You're down"); });
        });
        add("down.a_client_that_walks_while_held_is_put_back", [=](Script& s)
        {
            // down (or waiting for a turn), the root holds you; a client whose root came off and walks anyway is sent
            // back and rooted again. A hand's breadth of drift is let be
            downSetup(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                Check(c, IsDowned(p) && !IsTurnOf(p), "down, and not their turn");
                Position at = p->GetPosition();
                ClearMsgs();
                Position drift(at.GetPositionX() + 0.5f, at.GetPositionY(), at.GetPositionZ());
                OnPlayerMoved(p, drift);
                Check(c, !SawBy(p, "X^"), "half a metre of drift: let be");
                p->SetControlled(false, UNIT_STATE_ROOT);         // the root came off
                Position walked(at.GetPositionX() + 3.f, at.GetPositionY(), at.GetPositionZ());
                OnPlayerMoved(p, walked);
                Check(c, SawBy(p, "X^You're down - you can't move."), "3 m: told they can't");
                Check(c, p->HasUnitState(UNIT_STATE_ROOT), "and rooted again");
            });
        });
        // fleeing: on your turn, nobody within 20 m
        add("flee.too_close_is_refused", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c) { CastAs(c, 0, SPELL_FLEE, Bot(c)); });
            s.Wait(500);
            Expect(s, "refused, with how close they are", [](Ctx& c)
            {
                return int(c.v["cast"]) != SPELL_CAST_OK && SawBy(Bot(c), "Too close to flee") && InCombat(Bot(c));
            });
        });
        add("flee.alone_and_clear_the_fight_ends", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            MobOpt stuck{ 2.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                p->Relocate(p->GetPositionX() - 24.f, p->GetPositionY(), Ground(p->GetMap(), p->GetPositionX() - 24.f, p->GetPositionY()));
                p->UpdatePositionData();
                CastAs(c, 0, SPELL_FLEE, p);
            });
            s.Wait(500);
            Expect(s, "26 m off: fled, and the fight is over", [](Ctx& c) { return !InCombat(Bot(c)) && !InCombat(Mob(c)); });
            Expect(s, "an escape, not a wipe", [](Ctx& c) { return SawBy(Bot(c), "V^ESCAPED") && Bot(c)->IsAlive(); });
        });
        add("flee.a_party_fights_on", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 1 });
            MobOpt stuck{ 2.f, 0.f };
            stuck.st.speed = 0.f;
            AddMob(s, stuck);
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 1, a->GetPositionX() - 6.f, a->GetPositionY());
                OnPlayerMoved(b, b->GetPosition());
                Check(c, InCombat(b), "the wizard is in it");
                a->Relocate(a->GetPositionX() - 24.f, a->GetPositionY(), Ground(a->GetMap(), a->GetPositionX() - 24.f, a->GetPositionY()));
                a->UpdatePositionData();
                CastAs(c, 0, SPELL_FLEE, a);
            });
            s.Wait(800);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Check(c, !InCombat(a), "the fighter fled");
                Check(c, InCombat(Bot(c, 1)) && InCombat(Mob(c)), "the wizard and the creature fight on");
                MoveBotTo(a, Mob(c)->GetPositionX() - 10.f, Mob(c)->GetPositionY());   // back within the join range
                Check(c, !InCombat(a), "walking back near doesn't drag the fighter in again");
            });
        });
        add("wipe.nobody_left_standing_is_a_reload", [=](Script& s)
        {
            // alone, with two potions; one drunk and Second Wind spent in the fight, then knocked down: a wipe. The fighter
            // wakes at the graveyard - alive, standing, full HP - with both potions and Second Wind back
            AddBot(s, { FIGHTER, 3 });
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                p->DestroyItemCount(118, 99, true);
                p->AddItem(118, 2);
                c.v["x"] = p->GetPositionX();
                c.v["y"] = p->GetPositionY();
            });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* p = Bot(c);
                p->DestroyItemCount(118, 1, true);            // drunk
                GetSheet(p).secondWindUsed = 1;               // spent
                sAnnounced.clear();
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = p;
                d.amount = int(p->GetHealth());
                d.type = DMG_FORCE;
                ApplyDamage(d);
                AckTeleport(p);
            });
            Expect(s, "the fight is over", [](Ctx& c) { return !InCombat(Bot(c)); });
            Expect(s, "alive, standing, at full HP", [](Ctx& c)
            {
                Player* p = Bot(c);
                return p->IsAlive() && !IsDowned(p) && p->GetHealth() == p->GetMaxHealth() && !p->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH);
            });
            ExpectEq(s, "both potions back", [](Ctx& c) { return double(Bot(c)->GetItemCount(118, false)); }, 2);
            ExpectEq(s, "Second Wind back", [](Ctx& c) { return double(GetSheet(Bot(c)).secondWindUsed); }, 0);
            Expect(s, "somewhere else: the graveyard", [](Ctx& c)
            {
                return Bot(c)->GetExactDist2d(c.v["x"], c.v["y"]) > 10.f;
            });
            Expect(s, "and everyone is told", [](Ctx&) { return WasAnnounced("Jigfighter was overwhelmed by "); });
        });
        add("wipe.a_won_fight_keeps_what_was_spent", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            Act(s, [](Ctx& c) { Bot(c)->DestroyItemCount(118, 99, true); Bot(c)->AddItem(118, 2); });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Bot(c)->DestroyItemCount(118, 1, true);
                DamageCtx d;
                d.attacker = Bot(c);
                d.victim = Mob(c);
                d.amount = 1000000;
                d.type = DMG_FORCE;
                ApplyDamage(d);
            });
            s.Until([](Ctx& c) { return !InCombat(Bot(c)); }, "the fight to be won", 6000);
            ExpectEq(s, "a win: the potion stays drunk", [](Ctx& c) { return double(Bot(c)->GetItemCount(118, false)); }, 1);
        });
        // alone in a fight, but a groupmate on their feet nearby (out of it): no wipe. The fallen lie where they fell,
        // rolling death saves, for the friend to come; once nobody's dying, the creatures leave them for dead
        auto friendAway = [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            AddMob(s, { 2.f, 0.f });
            StartFight(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() - 60.f, a->GetPositionY());   // (looting, over there)
                Check(c, !InCombat(b) && !a->GetGroup(), "the wizard isn't in the fight (nor in a WoW group: it goes by who's near)");
                ClearMsgs();
                sAnnounced.clear();
                c.v["x"] = a->GetPositionX();
                DamageCtx d;
                d.attacker = Mob(c);
                d.victim = a;
                d.amount = int(a->GetHealth());
                d.type = DMG_FORCE;
                ApplyDamage(d);
                Check(c, IsDowned(a) && InCombat(a), "the fighter is down, and the fight goes on");
                Check(c, !WasAnnounced("overwhelmed") && std::abs(a->GetPositionX() - c.v["x"]) < 1.f, "no wipe: still lying there");
            });
        };
        add("down.alone_with_a_friend_nearby_is_no_wipe_and_dying_means_dead", [=](Script& s)
        {
            friendAway(s);
            Act(s, [](Ctx& c)
            {
                for (int i = 0; i < 3; ++i)
                {
                    sDice.assign({ 5, 5 });
                    RollDeathSave(Bot(c));
                }
                sDice.clear();
                Check(c, !Bot(c)->IsAlive(), "three failed saves: dead");
                Check(c, Bot(c)->HasAura(SPELL_FALLEN) && Bot(c)->HasPreventResurectionAura(),
                    "no releasing the spirit: the friend raises them at Brother Marrow");
                Check(c, !InCombat(Bot(c)) && Saw("leave you for dead"), "and the creatures leave");
                Check(c, !WasAnnounced("overwhelmed"), "not a wipe: no graveyard - Brother Marrow, for the friend to see to");
            });
        });
        // nobody left standing in the fight: the creatures turn on the downed (each hit a failed death save)
        add("down.nobody_standing_the_creatures_go_for_the_downed", [=](Script& s)
        {
            friendAway(s);
            Rig(s, {}, 15);                              // (death saves pass; the creature's swing hits)
            s.Until([](Ctx&) { return Saw("is hit while down"); }, "the creature to strike the downed fighter", 20000);
            Act(s, [](Ctx&) { sFallback = 0; });
        });
        add("down.alone_with_a_friend_nearby_stable_waits_for_help", [=](Script& s)
        {
            friendAway(s);
            Act(s, [](Ctx& c)
            {
                for (int i = 0; i < 3; ++i)
                {
                    sDice.assign({ 15, 15 });
                    RollDeathSave(Bot(c));
                }
                sDice.clear();
                Check(c, Bot(c)->IsAlive() && IsDowned(Bot(c)) && !InCombat(Bot(c)), "stable: the fight over, still down for the friend to help");
                Check(c, Saw("leave you for dead") && !WasAnnounced("overwhelmed"), "left for dead, not a wipe");
            });
        });
        add("down.a_won_fight_leaves_them_down_until_helped", [=](Script& s)
        {
            downSetup(s);
            win(s);
            Expect(s, "the fight is won and the fighter is still down", [](Ctx& c) { return IsDowned(Bot(c)) && !InCombat(Bot(c)); });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                CastAs(c, 1, SPELL_HELP, a);                  // 8 m off: too far
            });
            s.Wait(500);
            Expect(s, "Help from 8 m off doesn't reach", [](Ctx& c) { return IsDowned(Bot(c)); });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                MoveBotTo(Bot(c, 1), a->GetPositionX() - 1.5f, a->GetPositionY());
                CastAs(c, 1, SPELL_HELP, a);
            });
            s.Wait(700);
            Expect(s, "a hand up: on their feet with 1 HP", [](Ctx& c)
            {
                return int(c.v["cast"]) == SPELL_CAST_OK && !IsDowned(Bot(c)) && Bot(c)->GetHealth() == 1;
            });
            Expect(s, "and free to move", [](Ctx& c) { return !Bot(c)->HasUnitState(UNIT_STATE_ROOT); });
            Expect(s, "standing again: the pose gone", [](Ctx& c) { return !Bot(c)->HasUnitFlag2(UNIT_FLAG2_FEIGN_DEATH); });
        });
        add("down.help_is_only_for_the_downed", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                AddBotAt(c, WIZ, 3, a->GetPositionX() - 1.5f, a->GetPositionY());
                CastAs(c, 1, SPELL_HELP, a);
            });
            s.Wait(500);
            Expect(s, "refused: they're standing", [](Ctx& c) { return int(c.v["cast"]) != SPELL_CAST_OK && SawBy(Bot(c, 1), "Help: an ally who is down"); });
        });
        add("down.a_thrown_potion_gets_them_up", [=](Script& s)
        {
            downSetup(s);
            win(s);
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                b->SetSelection(a->GetGUID());               // the downed fighter targeted, 8 m off
                c.v["bhp"] = float(b->GetHealth());
                Prep(b, 439);
                ClearMsgs();
                c.v["cast"] = float(b->CastSpell(b, 439, false));    // a Potion of Healing
            });
            s.Wait(1000);
            Expect(s, "thrown, and the fighter is up with 2d4+2 (4-10)", [](Ctx& c)
            {
                return !IsDowned(Bot(c)) && Bot(c)->GetHealth() >= 4 && Bot(c)->GetHealth() <= 10;
            });
            ExpectSaw(s, "You throw a healing potion to Jigfighter");
        });
        add("marrow.a_friend_pays_to_raise_the_dead", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() + 20.f, a->GetPositionY());
                KillOutright(a);
                Check(c, !a->IsAlive(), "the fighter is dead");
                Position pos(b->GetPositionX() + 3.f, b->GetPositionY(), b->GetPositionZ(), 0.f);
                TempSummon* m = b->GetMap()->SummonCreature(6491, pos);
                Check(c, m && m->GetName() == "Brother Marrow", "every graveyard keeper is Brother Marrow");
                if (!m)
                    return;
                c.mobs.push_back(m->GetGUID());
                b->ModifyMoney(10000);
                uint32 before = b->GetMoney();
                Check(c, MarrowRaise(m, a, b), "the wizard pays");
                AckTeleport(a);
                CheckEq(c, double(before) - double(b->GetMoney()), 600, "2 silver a level: 6 silver for a level 3");
                Check(c, a->IsAlive(), "the fighter is alive");
                Check(c, a->GetExactDist(m) < 6.f, "beside Brother Marrow");
                Check(c, WasAnnounced("Jigwiz paid Brother Marrow to raise Jigfighter"), "and everyone is told");
            });
        });
        add("marrow.a_ghost_is_raised_for_nothing", [=](Script& s)
        {
            AddBot(s, { FIGHTER, 3 });
            Act(s, [](Ctx& c)
            {
                Player* a = Bot(c);
                Position pos(a->GetPositionX() + 30.f, a->GetPositionY(), a->GetPositionZ(), 0.f);
                TempSummon* m = a->GetMap()->SummonCreature(6491, pos);
                if (!m)
                    throw std::runtime_error("no Brother Marrow");
                c.mobs.push_back(m->GetGUID());
                c.v["money"] = float(a->GetMoney());
                KillOutright(a);
                a->BuildPlayerRepop();                        // released: a ghost
                Check(c, a->HasPlayerFlag(PLAYER_FLAGS_GHOST), "a ghost");
                MoveBotTo(a, m->GetPositionX() - 4.f, m->GetPositionY());
            });
            s.Until([](Ctx& c) { return Bot(c)->IsAlive(); }, "Brother Marrow to raise the ghost", 5000);
            Expect(s, "for nothing", [](Ctx& c) { return float(Bot(c)->GetMoney()) == c.v["money"]; });
        });

        // a real talker (the first conversation on the Eastern Kingdoms), the talking bot at its edge, the onlooker beside it
        // ...and for the roll: a talker on the Eastern Kingdoms whose opening line has a check anyone is offered (not a
        // specialist skill: those only show to the trained)
        uint32 rollTalkerId = 0;
        for (EncounterDef const* def : AllEncounters())
            if (def->dialogueNode && !def->spawns.empty() && !rollTalkerId)
                if (CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front()); d && d->mapid == 0)
                    if (WorldDatabase.Query("SELECT 1 FROM dnd_dialogue_option WHERE node_id = {} AND skill <> '' AND req = '' "
                            "AND need = '' AND skill NOT IN ('Sleight of Hand', 'Arcana', 'History', 'Nature', 'Religion', 'Animal Handling', "
                            "'Medicine', 'Survival', 'Performance') LIMIT 1", def->dialogueNode))
                        rollTalkerId = def->id;
        // ...and one that would stand friendly (a townsman, a custom NPC): between fights it must still be attackable
        uint32 friendlyTalkerId = 0;
        for (EncounterDef const* def : AllEncounters())
            if (def->dialogueNode && !def->spawns.empty() && !friendlyTalkerId)
                if (CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front()); d && d->mapid == 0)
                    if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(d->id))
                        if (FactionTemplateEntry const* ft = sFactionTemplateStore.LookupEntry(ct->faction); ft && !ft->IsHostileToPlayers())
                            friendlyTalkerId = def->id;
        // the group back as it was after a test that fought it (the fight called off, faction and health restored)
        auto restoreGroup = [](Script& s)
        {
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                for (std::size_t i = 0; i < c.bots.size(); ++i)
                    if (Player* p = ObjectAccessor::FindPlayer(c.bots[i]))
                        AbortEncounter(p);
                for (std::size_t i = 0; i < c.mobs.size(); ++i)
                    if (Creature* m = ObjectAccessor::GetCreature(*Bot(c), c.mobs[i]); m && m->IsAlive())
                    {
                        m->RestoreFaction();
                        m->SetFullHealth();
                        TalkerAtRest(m);
                    }
                for (std::size_t i = 0; i < c.bots.size(); ++i)
                    if (Player* p = ObjectAccessor::FindPlayer(c.bots[i]))
                        ClearGrudges(p);
            });
        };
        auto talkWithOnlooker = [=](Script& s, float onlookerOff, uint32 id = 0)
        {
            WorldSetup(s, id ? id : talkerId, 0.f, DWARF, 6);
            Act(s, [onlookerOff](Ctx& c)
            {
                if (Skipped(c))
                    return;
                // the dwarf walks up to 8 m from the leader; the onlooker stands further out along the same line
                Player* a = Bot(c);
                Creature* m = Mob(c, 0);
                float dx = a->GetPositionX() - m->GetPositionX(), dy = a->GetPositionY() - m->GetPositionY();
                float len = std::max(0.1f, std::sqrt(dx * dx + dy * dy));
                c.v["dx"] = dx / len;
                c.v["dy"] = dy / len;
                MoveBotTo(a, m->GetPositionX() + c.v["dx"] * 8.f, m->GetPositionY() + c.v["dy"] * 8.f);
                Player* b = AddBotAt(c, WIZ, 3, m->GetPositionX() + c.v["dx"] * (8.f + onlookerOff),
                    m->GetPositionY() + c.v["dy"] * (8.f + onlookerOff));
                GetSheet(b).tempHp = 100000;
                ClearMsgs();
                Check(c, StartDialogue(a, Mob(c, 0), true), "the talk opens");
                Check(c, InDialogue(a), "the dwarf is talking");
            });
            s.Wait(1200);                                         // a couple of the talker's scans
            Act(s, [](Ctx& c)
            {
                if (Skipped(c) || InDialogue(Bot(c)))
                    return;
                Player* a = Bot(c);
                Creature* m = Mob(c, 0);
                Out(Acore::StringFormat("      (the talk closed by itself: in combat {}, alive {}, {:.1f} m from the leader, z {:.1f} vs {:.1f}, "
                    "leader alive {} in world {})", InCombat(a), a->IsAlive(), a->GetExactDist(m), a->GetPositionZ(), m->GetPositionZ(),
                    m->IsAlive(), m->IsInWorld()));
            });
        };
        auto firstOption = [](Player* p, int field, bool wantSet) -> int
        {
            // the first option on the panel whose field (3 skill, 6 attack) is set or not: its index, or -1
            for (std::string const& m : MsgsBy(p, "DO^"))
            {
                std::vector<std::string> f = Split(m, '^');
                if (f.size() > 6 && ((field == 6 ? f[6] == "1" : !f[field].empty()) == wantSet) && (field == 6 || f[6] != "1"))
                    return std::atoi(f[1].c_str());
            }
            return -1;
        };
        add("multi.onlookers_see_the_conversation", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                Check(c, !InDialogue(b), "the onlooker isn't in a talk of their own");
                Check(c, WatchedTalker(b) == a->GetGUID(), "the onlooker is watching the dwarf's talk");
                Check(c, SawBy(b, "DW^Jigdwarf"), "their panel opens read-only, naming who is talking");
                Check(c, !GotExact(b, "DC"), "no letterbox for them (they aren't held)");
                Check(c, !b->HasUnitState(UNIT_STATE_ROOT), "and they can still move");
                Check(c, !MsgsBy(b, "DT^").empty() && !MsgsBy(b, "DE").empty(), "they get the line");
                CheckEq(c, double(MsgsBy(b, "DO^").size()), double(MsgsBy(a, "DO^").size()), "and the same options");
            });
        });
        add("multi.an_onlooker_cannot_choose", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [firstOption](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                int idx = firstOption(a, 3, false);
                if (idx < 0)
                    idx = firstOption(a, 3, true);
                ClearMsgs();
                HandleClientMessage(b, Acore::StringFormat("say^{}", idx));
                Check(c, MsgsBy(a, "DS^").empty() && MsgsBy(b, "DS^").empty(), "the onlooker's pick does nothing");
                Check(c, InDialogue(a) && WatchedTalker(b) == a->GetGUID(), "the talk goes on as it was");
            });
        });
        add("multi.onlookers_see_the_hover_and_the_pick", [=](Script& s)
        {
            // the talker's mouse over an option lights it on the onlooker's panel (DH^idx; 0 when it leaves), and a pick
            // reaches them as DH^idx^1 ahead of everything it leads to; an onlooker's own mouse tells nobody anything
            talkWithOnlooker(s, 3.f);
            Act(s, [firstOption](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                int idx = firstOption(a, 3, false);
                if (idx < 0)
                    idx = firstOption(a, 3, true);
                Check(c, idx >= 0, "the talker has an option");
                if (idx < 0)
                    return;
                ClearMsgs();
                HandleClientMessage(a, Acore::StringFormat("hover^{}", idx));
                Check(c, GotExact(b, Acore::StringFormat("DH^{}", idx)), "the onlooker sees the hover");
                Check(c, MsgsBy(a, "DH^").empty(), "the talker isn't sent their own hover");
                HandleClientMessage(a, "hover^0");
                Check(c, GotExact(b, "DH^0"), "and sees it leave");
                ClearMsgs();
                HandleClientMessage(b, Acore::StringFormat("hover^{}", idx));
                Check(c, MsgsBy(a, "DH^").empty() && MsgsBy(b, "DH^").empty(), "the onlooker's hover goes nowhere");
                ClearMsgs();
                HandleClientMessage(a, Acore::StringFormat("say^{}", idx));
                std::vector<std::string> const& got = sMsgsOf[b->GetGUID()];
                std::string pick = Acore::StringFormat("DH^{}^1", idx);
                auto at = std::find(got.begin(), got.end(), pick);
                Check(c, at != got.end(), "the onlooker sees the pick");
                bool first = true;
                for (auto m = got.begin(); m != at && m != got.end(); ++m)
                    if (m->rfind("DS^", 0) == 0 || m->rfind("DR^", 0) == 0 || *m == "DX")
                        first = false;
                Check(c, first, "before the answer, the roll or the end it leads to");
            });
        });
        add("multi.onlookers_see_the_roll", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f, rollTalkerId);
            Act(s, [firstOption](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                // a check on the opening line, or a few lines in (taking the first plain answer each time)
                int idx = firstOption(a, 3, true);
                for (int step = 0; idx < 0 && step < 4 && InDialogue(a); ++step)
                {
                    int plain = firstOption(a, 3, false);
                    if (plain < 0)
                        break;
                    ClearMsgs();
                    DialogueChoose(a, uint32(plain));
                    idx = firstOption(a, 3, true);
                }
                if (idx < 0 || !InDialogue(a) || WatchedTalker(b) != a->GetGUID())
                {
                    std::string seen;
                    for (std::string const& m : MsgsBy(a, "DO^"))
                        seen += " | " + m.substr(0, 60);
                    Out(Acore::StringFormat("      (no skill check within a few lines of the opening: the roll mirror isn't exercised; "
                        "talker {}, idx {}, talking {}, watched {}{})", GetEncounterDef(uint32(c.v["def"]))->name, idx, InDialogue(a),
                        WatchedTalker(b) == a->GetGUID(), seen));
                    return;
                }
                ClearMsgs();
                sDice.assign({ 20, 20 });
                DialogueChoose(a, uint32(idx));
                sDice.clear();
                std::vector<std::string> ra = MsgsBy(a, "DR^"), rb = MsgsBy(b, "DR^");
                Check(c, ra.size() == 1, "the dwarf rolled");
                Check(c, rb == ra, "the onlooker saw the same roll");
                Check(c, SawBy(b, "Jigdwarf:") && SawBy(b, "Jigdwarf's"), "and what was said, and whose check it was");
            });
        });
        add("multi.clicking_a_busy_talker_watches_it", [=](Script& s)
        {
            // 25 m off: too far to be drawn in, close enough to keep watching once they ask to
            talkWithOnlooker(s, 25.f);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                Check(c, WatchedTalker(b).IsEmpty(), "25 m off: not watching");
                Check(c, StartDialogue(b, Mob(c, 0), true), "a right-click on the busy talker");
                Check(c, !InDialogue(b), "doesn't start a second conversation with them");
                Check(c, WatchedTalker(b) == a->GetGUID(), "it watches the one going on");
            });
            s.Wait(1200);
            Expect(s, "and keeps watching from there", [](Ctx& c) { return Skipped(c) || WatchedTalker(Bot(c, 1)) == Bot(c)->GetGUID(); });
        });
        add("multi.escape_stops_watching_until_asked_again", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                ClearMsgs();
                HandleClientMessage(Bot(c, 1), "bye");            // Escape on the onlooker's panel
                Check(c, WatchedTalker(Bot(c, 1)).IsEmpty() && GotExact(Bot(c, 1), "DX"), "the panel closes");
                Check(c, InDialogue(Bot(c)), "the talk itself goes on");
            });
            s.Wait(1500);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Check(c, WatchedTalker(Bot(c, 1)).IsEmpty(), "standing right there, it doesn't pop back up");
                Check(c, StartDialogue(Bot(c, 1), Mob(c, 0), true), "a right-click");
                Check(c, WatchedTalker(Bot(c, 1)) == Bot(c)->GetGUID(), "opens it again");
            });
        });
        add("multi.walking_away_stops_watching", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Creature* m = Mob(c, 0);
                ClearMsgs();
                MoveBotTo(Bot(c, 1), m->GetPositionX() + c.v["dx"] * 53.f, m->GetPositionY() + c.v["dy"] * 53.f);
            });
            s.Wait(1500);
            Expect(s, "45 m off, the onlooker's panel has closed", [](Ctx& c)
            {
                return Skipped(c) || (WatchedTalker(Bot(c, 1)).IsEmpty() && GotExact(Bot(c, 1), "DX"));
            });
        });
        add("multi.the_talk_ending_closes_the_onlookers_panel", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                ClearMsgs();
                CloseDialogue(Bot(c), true);
                Check(c, WatchedTalker(Bot(c, 1)).IsEmpty() && GotExact(Bot(c, 1), "DX"), "the onlooker's panel closes with it");
            });
        });
        add("multi.a_talk_gone_bad_pulls_the_onlooker_in", [=](Script& s)
        {
            talkWithOnlooker(s, 3.f);
            Act(s, [firstOption](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                int idx = firstOption(a, 6, true);
                if (idx >= 0)
                    DialogueChoose(a, uint32(idx));               // the [ATTACK] option
                else
                {
                    Out("      (no [ATTACK] on the opening line: the talk is broken off and the fight started by hand)");
                    CloseDialogue(a, false);
                    StartEncounter(Mob(c, 0), a);
                }
                Check(c, InCombat(a), "the dwarf is fighting");
                Check(c, InCombat(b) && EncounterOf(b->GetGUID()) == EncounterOf(a->GetGUID()), "the onlooker is in the same fight");
                Check(c, WatchedTalker(b).IsEmpty(), "and no longer watching a talk");
            });
            restoreGroup(s);
        });
        add("multi.a_named_loner_brings_no_underlings", [=](Script& s)
        {
            uint32 id = 0;
            for (EncounterDef const* def : AllEncounters())
                if (def->minion && def->spawns.size() == 1 && !id)
                    if (CreatureData const* d = sObjectMgr->GetCreatureData(def->spawns.front()); d && d->mapid == 0)
                        id = def->id;
            WorldSetup(s, id, 0.f, DWARF, 6);
            Act(s, [id](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = AddBotAt(c, WIZ, 3, a->GetPositionX() + 2.f, a->GetPositionY());
                GetSheet(b).tempHp = 100000;
                Player* c3 = AddBotAt(c, ROGUE, 3, a->GetPositionX(), a->GetPositionY() + 2.f);
                GetSheet(c3).tempHp = 100000;
                Mob(c, 0)->SetFaction(14);
                SetTunedPartySize(3);
                StartEncounter(Mob(c, 0), a);
                Check(c, InCombat(a) && InCombat(b) && InCombat(c3), "all three players are in the fight");
                if (!InCombat(a))
                    return;
                int extras = 0;
                for (Combatant const& x : Enc(c)->order)
                    extras += x.extra;
                CheckEq(c, extras, 0, "tuned for three: none of its underlings brought in");
                CheckEq(c, MobC(c)->legendaryMax, 2, "two legendary actions instead");
                CheckEq(c, Mob(c)->GetMaxHealth(), std::lround(StatsOrDefault(Mob(c)).hp * 2.5f), "its hit points x2.5");
            });
            restoreGroup(s);
        });
        // ============================================================ the endgame (DndEndgame.cpp; tools/endgame.py, undercity.py)
        // the Undercity's queen: the encounter the lair and legendary tests stand by (its lair swapped for a test one,
        // the real one loaded back after)
        uint32 const QUEEN = 222;
        auto lairFight = [](Script& s, std::vector<LairAction> lair)
        {
            WorldSetup(s, 222, 0.f, DWARF, 6);
            Act(s, [lair](Ctx& c)
            {
                if (Skipped(c))
                    return;
                SetTestLair(222, lair);
                ClearMsgs();
                c.v["th"] = float(GetSheet(Bot(c)).tempHp);
                Mob(c, 0)->SetFaction(14);
                sFallback = 3;                       // every die a 3: saves fail against DC 30, 2d6 is 6
                StartEncounter(Mob(c, 0), Bot(c));
                Check(c, InCombat(Bot(c)), "the fight with the queen starts");
            });
            s.Until([](Ctx& c)
            {
                if (Skipped(c))
                    return true;
                if (!SawBy(Bot(c), "[Lair]"))
                    return false;
                c.v["after"] = float(GetSheet(Bot(c)).tempHp);
                return true;
            }, "the lair to act at the top of the first round", 8000);
        };
        auto lairDone = [restoreGroup](Script& s)
        {
            Act(s, [](Ctx& c) { sFallback = 0; LoadEndgame(); });
            restoreGroup(s);
        };
        add("endgame.a_boss_takes_legendary_actions_even_alone", [=](Script& s)
        {
            WorldSetup(s, QUEEN, 0.f, DWARF, 6);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Mob(c, 0)->SetFaction(14);
                StartEncounter(Mob(c, 0), Bot(c));
                Check(c, InCombat(Bot(c)), "the fight starts");
                if (!InCombat(Bot(c)))
                    return;
                CheckEq(c, MobC(c)->legendaryMax, 1, "one adventurer, and still a legendary action a round");
                Player* b = AddBotAt(c, WIZ, 6, Bot(c)->GetPositionX() + 2.f, Bot(c)->GetPositionY());
                GetSheet(b).tempHp = 100000;
                OnPlayerMoved(b, b->GetPosition());
                CheckEq(c, MobC(c)->legendaryMax, 1, "a second player joins: the same (tuned for the party size, not who comes)");
                int extras = 0;
                for (Combatant const& x : Enc(c)->order)
                    extras += x.extra;
                CheckEq(c, extras, 0, "and nobody is brought in for them");
            });
            restoreGroup(s);
        });
        add("endgame.a_lair_burst_hurts_whoever_stands_there", [=](Script& s)
        {
            LairAction a;
            a.kind = "burst";
            a.name = "Test Burst";
            a.dice = Dice{ 2, 6, 0 };
            a.type = DMG_PSYCHIC;
            a.save = WIS;
            a.dc = 30;
            a.radius = 4.f;
            lairFight(s, { a });
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                CheckEq(c, c.v["th"] - c.v["after"], 6, "2d6 (two 3s), the save failed: 6 off the bot");
                Check(c, SawBy(Bot(c), "Test Burst"), "the lair action is named in the log");
            });
            lairDone(s);
        });
        add("endgame.a_lair_grip_holds_until_the_next_round", [=](Script& s)
        {
            LairAction a;
            a.kind = "grip";
            a.name = "Test Grip";
            a.save = STR;
            a.dc = 30;
            a.radius = 4.f;
            lairFight(s, { a });
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Combatant* bc = BotC(c);
                Check(c, bc && bc->restrained && bc->lairHeld, "held fast: restrained until the next round");
            });
            lairDone(s);
        });
        add("endgame.a_lair_call_brings_one_more", [=](Script& s)
        {
            LairAction a;
            a.kind = "call";
            a.name = "Test Call";
            a.entry = 13839;
            a.most = 1;
            lairFight(s, { a });
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                int called = 0;
                for (Combatant const& x : Enc(c)->order)
                    if (x.extra)
                        if (Unit* u = Enc(c)->GetUnit(x); u && u->GetEntry() == 13839)
                            ++called;
                CheckEq(c, called, 1, "a Royal Dreadguard steps in");
                CheckEq(c, double(Enc(c)->lairCalled.size()), 1, "and is counted as the lair's");
            });
            lairDone(s);
        });
        add("endgame.a_lair_action_the_party_took_away_stays_off", [=](Script& s)
        {
            // the only lair action is switched off by clearing another encounter first: nothing happens
            WorldSetup(s, QUEEN, 0.f, DWARF, 6);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                LairAction a;
                a.kind = "burst";
                a.name = "Test Burst";
                a.dice = Dice{ 2, 6, 0 };
                a.dc = 30;
                a.stop = "cleared:206";
                std::vector<Player*> party{ Bot(c) };
                Check(c, !LairStopped(a, party), "not cleared yet: the action is on");
                OnEncounterWon(Bot(c), *GetEncounterDef(206));
                Check(c, LairStopped(a, party), "cleared: it's off");
                a.stop = "found:no_such_flag";
                Check(c, !LairStopped(a, party), "a find nobody made leaves it on");
                a.stop = "cleared:206";
                SetTestLair(222, { a });
                ClearMsgs();
                Mob(c, 0)->SetFaction(14);
                StartEncounter(Mob(c, 0), Bot(c));
            });
            s.Wait(3000);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Check(c, InCombat(Bot(c)) && Enc(c)->round >= 1, "the first round has begun");
                Check(c, !SawBy(Bot(c), "[Lair]"), "and the lair stays quiet");
            });
            lairDone(s);
        });

        add("multi.a_friendly_talker_can_be_attacked_between_fights", [=](Script& s)
        {
            WorldSetup(s, friendlyTalkerId, 0.f, DWARF, 6);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* p = Bot(c);
                for (std::size_t i = 0; i < c.mobs.size(); ++i)
                {
                    Creature* m = Mob(c, i);
                    Check(c, !m->IsHostileTo(p), m->GetName() + " doesn't stand hostile");
                    Check(c, p->IsValidAttackTarget(m), m->GetName() + " can be attacked");
                    Check(c, m->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP), m->GetName() + " still talks on a right-click");
                }
            });
        });
        add("multi.attacking_a_talker_mid_conversation_starts_the_fight", [=](Script& s)
        {
            // the dwarf is talking to a group that would stand friendly; the wizard beside them throws a Fire Bolt at it:
            // the talk ends, and the dwarf, the wizard and the group are in one fight
            talkWithOnlooker(s, 3.f, friendlyTalkerId);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                CastAs(c, 1, SPELL_FIRE_BOLT, Mob(c, 0));
            });
            s.Until([](Ctx& c) { return Skipped(c) || InCombat(Bot(c, 1)); }, "the wizard's attack to start a fight", 5000);
            Act(s, [](Ctx& c)
            {
                if (Skipped(c))
                    return;
                Player* a = Bot(c);
                Player* b = Bot(c, 1);
                Check(c, int(c.v["cast"]) == SPELL_CAST_OK, Acore::StringFormat("the Fire Bolt was cast (result {})", int(c.v["cast"])));
                Check(c, !InDialogue(a), "the dwarf's conversation is over");
                Check(c, SawBy(a, "cut short"), "and they are told why");
                Encounter* e = EncounterOf(b->GetGUID());
                Check(c, e && EncounterOf(a->GetGUID()) == e, "the dwarf and the wizard are in the same fight");
                Check(c, e && EncounterOf(Mob(c, 0)->GetGUID()) == e, "against the group");
                Check(c, WatchedTalker(b).IsEmpty(), "and the wizard isn't watching a talk any more");
            });
            restoreGroup(s);
        });

        // ============================================================ the campaign (very slow: only `selftest.py campaign`)
        addSlow("campaign.two_adventurers", [=](Script& s)
        {
            BotOpt f{ FIGHTER, 1 };
            f.at = true;
            f.map = 0;
            f.x = -6240.f;
            f.y = 331.f;
            f.z = 383.f;
            BotOpt w = f;
            w.kind = WIZ;
            w.x = -6238.f;
            AddBot(s, f);
            AddBot(s, w);
            Act(s, [](Ctx& c)
            {
                sCamp = Campaign();
                sCamp.startedAt = getMSTime();
                SetTunedPartySize(0);                // the campaign plays the real thing: the server's Dnd.PartySize
                std::vector<EncounterDef const*> defs = AllEncounters();
                std::stable_sort(defs.begin(), defs.end(), [](EncounterDef const* x, EncounterDef const* y)
                {
                    ZoneDef const* zx = GetZone(x->zoneId);
                    ZoneDef const* zy = GetZone(y->zoneId);
                    int lx = zx ? zx->minLevel : 99, ly = zy ? zy->minLevel : 99;
                    return lx != ly ? lx < ly : (x->zoneId != y->zoneId ? x->zoneId < y->zoneId : x->id < y->id);
                });
                for (EncounterDef const* def : defs)
                    sCamp.order.push_back(def->id);
                for (std::size_t i = 0; i < 2; ++i)
                {
                    Player* p = Bot(c, i);
                    EnsureKit(p);                      // their class kit, as a new character has it
                    if (IsFighter(p))
                    {
                        Equip(p, EQUIPMENT_SLOT_MAINHAND, 2488);    // longsword
                        Equip(p, EQUIPMENT_SLOT_OFFHAND, 2362);     // shield
                        Equip(p, EQUIPMENT_SLOT_CHEST, 285);        // scale mail
                    }
                    else
                        Equip(p, EQUIPMENT_SLOT_MAINHAND, 35);      // quarterstaff
                }
                Out(Acore::StringFormat("      campaign: {} encounters to play", sCamp.order.size()));
            });
            s.Until([](Ctx& c) { return CampaignTick(c); }, "the campaign to finish", 12u * 3600u * 1000u);
        });

        // ============================================================ the world itself (slow: `selftest.py world.` or `all`)
        // every real encounter fought for two rounds (no creature's turn may hang) and won; every conversation walked
        // to an ending three ways. The bot is put back out and the creatures restored afterwards.
        for (EncounterDef const* def : AllEncounters())
        {
            uint32 id = def->id;
            std::string tag = Acore::StringFormat("{:02}_{}", id, Slug(def->name));
            addSlow("world.fight." + tag, [=](Script& s) { WorldFight(s, id); });
            if (def->dialogueNode)
                for (int policy = 0; policy < 3; ++policy)
                    addSlow(Acore::StringFormat("world.talk.{}.{}", tag, policy == 0 ? "checks_pass" : (policy == 1 ? "checks_fail" : "last_option")),
                        [=](Script& s) { WorldTalk(s, id, policy); });
        }
        return t;
    }

    // ------------------------------------------------------------------ runner
    void Teardown()
    {
        if (sCtx.v.count("wzone"))               // a weather test: the bots' zone goes back to clear
            SetWeather(MAP_ID, uint32(sCtx.v["wzone"]), WEATHER_CLEAR, false);
        Player* any = nullptr;
        for (ObjectGuid g : sCtx.bots)
            if ((any = ObjectAccessor::FindPlayer(g)))
                break;
        EncounterDef const* worldDef = sCtx.v.count("def") ? GetEncounterDef(uint32(sCtx.v["def"])) : nullptr;
        Map* worldMap = any ? any->GetMap() : nullptr;
        if (any)
        {
            AbortEncounter(any);
            if (!worldDef)
                for (ObjectGuid g : sCtx.mobs)
                    if (Creature* m = ObjectAccessor::GetCreature(*any, g))
                        m->DespawnOrUnsummon();
        }
        else if (Map* flats = sMapMgr->FindMap(MAP_ID, 0))
            for (ObjectGuid g : sCtx.mobs)
                if (Creature* m = flats->GetCreature(g))
                {
                    AbortEncounter(m);
                    m->DespawnOrUnsummon();
                }
        for (ObjectGuid g : sCtx.bots)
            RemoveBot(g);
        // a world test fought (or talked to) a real encounter: its creatures come back, healed, home and friendly again
        if (worldDef && worldMap)
            for (uint32 spawn : worldDef->spawns)
                RespawnSpawn(worldMap, spawn);
        sCtx = Ctx();
        ClearTestStats();
        sDice.clear();
        sFallback = 0;
        sMsgs.clear();
    }

    void FinishTest(std::string const& abortReason = "")
    {
        SetTunedPartySize(1);                    // (a test that tuned for more is put back)
        TestDef const& t = sTests[sTestIdx];
        if (!abortReason.empty())
            sCtx.fails.push_back(abortReason);
        if (sCtx.fails.empty())
        {
            ++sPassed;
            Out(Acore::StringFormat("PASS  {} ({} checks)", t.name, sCtx.checks));
        }
        else
        {
            ++sFailed;
            sFailedNames.push_back(t.name);
            Out(Acore::StringFormat("FAIL  {}", t.name));
            for (std::string const& f : sCtx.fails)
                Out("        - " + f);
        }
        Teardown();
        ++sTestIdx;
        sStep = 0;
        sStepStarted = false;
        sElapsed = 0;
        sCooldown = 400;
        if (sTestIdx < sTests.size())
        {
            sScript = Script();
            sTests[sTestIdx].build(sScript);
        }
    }

    void Finish()
    {
        SetTunedPartySize(0);                    // the tests done: back to the server's Dnd.PartySize
        Out(Acore::StringFormat("DONE  passed {} failed {}{}", sPassed, sFailed, sFailedNames.empty() ? "" : "  (" + [] {
            std::string s;
            for (std::string const& n : sFailedNames)
                s += (s.empty() ? "" : ", ") + n;
            return s;
        }() + ")"));
        sOut.close();
        sRunning = false;
        // the bots' rows once more: a sheet save queued just before a delete can land after it
        for (uint32 g : sAllBotGuids)
            for (char const* table : { "dnd_character", "dnd_character_treat", "dnd_character_encounter", "dnd_character_townsfolk" })
                CharacterDatabase.Execute("DELETE FROM {} WHERE guid = {}", table, g);
        sAllBotGuids.clear();
    }
}

    bool Running() { return sRunning; }

    int ForcedRoll(int sides)
    {
        int top = std::max(1, sides);
        if (!sDice.empty())
        {
            int v = sDice.front();
            sDice.pop_front();
            return std::clamp(v, 1, top);
        }
        return sFallback ? std::clamp(sFallback, 1, top) : 0;
    }

    bool IsBot(ObjectGuid g) { return sBotSet.count(g) != 0; }
    void Announced(std::string const& text) { sAnnounced.push_back(text); }

    bool Capture(Player* p, std::string const& msg)
    {
        if (!p || !sBotSet.count(p->GetGUID()))
            return false;
        sMsgs.push_back(msg);
        sMsgsOf[p->GetGUID()].push_back(msg);
        return true;
    }

    void Start(ChatHandler* handler, std::string const& filter)
    {
        if (sRunning)
        {
            handler->SendSysMessage("mod-dnd selftest: a run is already going.");
            return;
        }
        sAccount = AccountMgr::GetId("DND");
        sTests.clear();
        // no filter: the quick suites; "all": everything; anything else: the tests whose name contains it
        for (TestDef& t : Catalogue())
            if ((filter == "all" && t.name.rfind("campaign.", 0) != 0) ||
                (filter != "all" && (filter.empty() ? !t.slow : t.name.find(filter) != std::string::npos)))
                sTests.push_back(std::move(t));
        if (sTests.empty())
        {
            handler->PSendSysMessage("mod-dnd selftest: no test matches \"{}\".", filter);
            return;
        }
        std::string path = sConfigMgr->GetOption<std::string>("Dnd.LogFile", "logs/dnd.log");
        path = path.substr(0, path.find_last_of("/\\") + 1) + "dnd_selftest.log";
        sOut.open(path, std::ios::trunc);
        sPassed = sFailed = 0;
        sFailedNames.clear();
        sTestIdx = 0;
        sStep = 0;
        sStepStarted = false;
        sElapsed = 0;
        sCooldown = 0;
        sCtx = Ctx();
        sScript = Script();
        sTests[0].build(sScript);
        SetTunedPartySize(1);                    // the tests count on a creature's own stat block unless they say
        sRunning = true;
        Out(Acore::StringFormat("START {} tests{}", sTests.size(), filter.empty() ? "" : " matching \"" + filter + "\""));
        handler->PSendSysMessage("mod-dnd selftest: {} tests started; results in {}", sTests.size(), path);
    }

    void Update(uint32 diff)
    {
        if (!sRunning)
            return;
        if (sCooldown)
        {
            sCooldown = diff >= sCooldown ? 0 : sCooldown - diff;
            return;
        }
        if (sTestIdx >= sTests.size())
        {
            Finish();
            return;
        }
        // a few steps per tick: plain actions run back to back, waits end the tick
        for (int guard = 0; guard < 16; ++guard)
        {
            if (sStep >= sScript.steps.size())
            {
                FinishTest();
                return;
            }
            Step& st = sScript.steps[sStep];
            try
            {
                if (st.act)
                {
                    st.act(sCtx);
                    ++sStep;
                    if (!sCtx.fails.empty() && sCtx.fails.back().rfind("ABORT", 0) == 0)
                    {
                        FinishTest();
                        return;
                    }
                    continue;
                }
                if (st.wait)
                {
                    if (!sStepStarted)
                    {
                        sStepStarted = true;
                        sElapsed = 0;
                        return;
                    }
                    sElapsed += diff;
                    if (sElapsed < st.wait)
                        return;
                    sStepStarted = false;
                    ++sStep;
                    continue;
                }
                if (st.until)
                {
                    if (!sStepStarted)
                    {
                        sStepStarted = true;
                        sElapsed = 0;
                    }
                    else
                        sElapsed += diff;
                    if (st.until(sCtx))
                    {
                        sStepStarted = false;
                        ++sStep;
                        continue;
                    }
                    if (sElapsed >= st.timeout)
                    {
                        FinishTest("timed out waiting for " + st.what);
                        return;
                    }
                    return;
                }
                ++sStep;
            }
            catch (std::exception const& ex)
            {
                FinishTest(std::string("stopped: ") + ex.what());
                return;
            }
        }
    }
}
