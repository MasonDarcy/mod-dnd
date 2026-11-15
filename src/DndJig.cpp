/*
 * mod-dnd spell jig (GM: `.dnd jig`): a sparring ground on the Shimmering Flats for testing every spell, class action and
 * consumable by hand.
 *
 * Going in saves the character's build, level and place, makes them level 6 and takes them to the flats. Picking an
 * entry on the jig's panel (DnDJig.lua) quietly makes the character a class (and subclass) that has it - the sheet's
 * class, which is what the rules read; the WoW class stays as it is - learns that kit, puts the entry on action button 1,
 * and starts a fresh fight against two sparring partners: an undead and a humanoid (Turn Undead, Holy Water and the
 * Arrow of Slaying need one of each), thousands of hit points, a 1d2 jab - and two kobolds of 15 and 30 hit points
 * for whatever counts or cares about hit points (Sleep, the wounded, a kill). You always go first; you're proficient with
 * every weapon, and carry a test kit (weapons, bows, a shield) while you're in. Every turn of the player's refills slots and
 * class resources, tops up health and restocks the item under test. Pass / Fail and a note go to logs/dnd_spelljig.tsv,
 * which the panel reads back so it shows what's left to test. Leaving (the panel, or logging out) puts the build, the
 * level, the kit and the action button back and returns the character where they were.
 *
 * Addon messages: JO (the list follows), JE^idx^kind^group^status^id^name (id: the spell, or the item), JD (the list is done), JS^idx^tested as,
 * JV^idx^status, JX (the jig is over). The addon sends jig^ready^idx, jig^again, jig^verdict^idx^p|f|c^note, jig^leave.
 */
#include "Dnd.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include <fstream>
#include <map>
#include <sstream>

namespace Dnd
{
    namespace
    {
        uint32 const JIG_MAP = 1;                               // Kalimdor: the Shimmering Flats, east of the Mirage Raceway
        float const JIG_X = -5980.f, JIG_Y = -3760.f, JIG_Z = -57.6f;
        uint32 const DUMMY_UNDEAD = 48, DUMMY_HUMANOID = 38;   // a Skeletal Warrior and a Defias Thug, by look and type
        uint32 const DUMMY_KOBOLD = 6;                          // Kobold Vermin: two of them, with real hit points (Sleep's
                                                                // pool, anything that cares how hurt the target is, a kill)
        uint32 const DUMMY_HP = 4000, KOBOLD_HP[] = { 15, 30 };   // (as the fight shows them, after scaling for the party)
        uint32 const FRAIL_HP = 15;
        // a test kit, for everything that wants a weapon or a shield in hand: dagger, quarterstaff, handaxe, longsword,
        // greatsword, shortbow, light crossbow, shield (tools/shops.py)
        uint32 const KIT[] = { 2139, 35, 2134, 2488, 49778, 2504, 15807, 2362 };
        uint32 const SHORTBOW = 2504;                           // (the arrows want a bow)
        uint32 const ITEM_STACK = 5;
        uint8 const JIG_LEVEL = 6;
        char const* const VERDICT_FILE = "logs/dnd_spelljig.tsv";

        struct Entry
        {
            std::string key;                 // "s:<spell>" / "i:<item>" - the verdict file's key
            std::string kind;                // "spell", "class", "item"
            std::string group;               // "Cantrips", "Level 2", "Druid", "Scrolls"...
            std::string name;
            uint32 spell = 0, item = 0;
            std::string itemKind;
            uint8 cls = 0;                   // the class it's tested as (0: anyone - an item)
            std::string sub;
        };

        struct Jig
        {
            Sheet saved;
            uint8 level = 1;
            uint32 xp = 0;
            WorldLocation back;
            bool teleported = false;
            std::vector<Entry> entries;
            int current = -1;
            std::vector<ObjectGuid> dummies;
            std::map<uint32, uint32> given;  // item -> how many the jig put in the bags
            uint32 button0 = 0;              // action button 1 as it was (packed: action | type << 24)
        };
        std::unordered_map<ObjectGuid, Jig> sJigs;
        std::set<ObjectGuid> sDummies;
        std::set<ObjectGuid> sFrail;
        std::map<std::string, std::pair<std::string, std::string>> sVerdicts;   // key -> (verdict, note)
        bool sVerdictsLoaded = false;

        std::string Clean(std::string s)
        {
            for (char& ch : s)
                if (ch == '\t' || ch == '\n' || ch == '\r' || ch == '^')
                    ch = ' ';
            return s.substr(0, 200);
        }

        void LoadVerdicts()
        {
            if (sVerdictsLoaded)
                return;
            sVerdictsLoaded = true;
            std::ifstream in(VERDICT_FILE);
            std::string line;
            while (std::getline(in, line))
            {
                std::vector<std::string> f;
                std::stringstream ss(line);
                std::string part;
                while (std::getline(ss, part, '\t'))
                    f.push_back(part);
                if (f.size() >= 3 && f[0] != "key")
                    sVerdicts[f[0]] = { f[2], f.size() > 3 ? f[3] : "" };
            }
        }

        void SaveVerdicts(std::vector<Entry> const& names)
        {
            std::map<std::string, std::string> nameOf;
            for (Entry const& e : names)
                nameOf[e.key] = e.group + ": " + e.name;
            std::ofstream out(VERDICT_FILE, std::ios::trunc);
            out << "key\tname\tverdict\tnote\n";
            for (auto const& [key, v] : sVerdicts)
                out << key << '\t' << (nameOf.count(key) ? nameOf[key] : std::string("?")) << '\t' << v.first << '\t' << v.second << '\n';
        }

        std::string SpellName(uint32 id)
        {
            SpellInfo const* si = sSpellMgr->GetSpellInfo(id);
            return si && si->SpellName[0] && *si->SpellName[0] ? std::string(si->SpellName[0]) : Acore::StringFormat("Spell {}", id);
        }

        std::vector<uint8> Classes()
        {
            std::vector<uint8> out;
            for (uint8 c = 1; c < 32; ++c)
                if (ClassDef const* d = ClassInfo(c); d && d->id == c)
                    out.push_back(c);
            return out;
        }

        // every entry, and for each spell or class action a class (and subclass) at level 6 whose kit has it
        std::vector<Entry> BuildEntries(Player* p)
        {
            Sheet& s = GetSheet(p);
            Sheet keep = s;
            std::vector<uint32> common = CommonActions();
            std::vector<Entry> out;
            for (uint32 id : AllActionIds())
            {
                if (std::find(common.begin(), common.end(), id) != common.end())
                    continue;
                ActionInfo const* a = GetAction(id);
                Entry e;
                e.spell = id;
                e.key = Acore::StringFormat("s:{}", id);
                e.name = SpellName(id);
                for (int pass = 0; pass < 2 && !e.cls; ++pass)     // a class's own kit first, a subclass's after
                    for (uint8 c : Classes())
                    {
                        s.cls = c;
                        std::vector<std::string> subs;
                        if (pass == 0)
                            subs.push_back("");
                        else
                            for (char const* sc : ClassInfo(c)->subclasses)
                                subs.push_back(sc);
                        for (std::string const& sc : subs)
                        {
                            s.subclass = sc;
                            if (KitHas(p, id))
                            {
                                e.cls = c;
                                e.sub = sc;
                                break;
                            }
                        }
                        if (e.cls)
                            break;
                    }
                if (!e.cls)
                    continue;                    // nobody's kit at level 6 (a scroll's spell: the scrolls cover it)
                if (a && a->slotLevel >= 0)
                {
                    e.kind = "spell";
                    e.group = a->slotLevel == 0 ? "Cantrips" : Acore::StringFormat("Level {}", int(a->slotLevel));
                }
                else
                {
                    e.kind = "class";
                    e.group = ClassInfo(e.cls)->name;
                }
                out.push_back(e);
            }
            s = keep;
            if (QueryResult r = WorldDatabase.Query("SELECT entry, name, kind FROM dnd_jig_item ORDER BY kind, entry"))
            {
                do
                {
                    Field* f = r->Fetch();
                    Entry e;
                    e.kind = "item";
                    e.item = f[0].Get<uint32>();
                    e.key = Acore::StringFormat("i:{}", e.item);
                    e.name = f[1].Get<std::string>();
                    e.itemKind = f[2].Get<std::string>();
                    static std::map<std::string, char const*> const groups = { { "potion", "Potions" }, { "scroll", "Scrolls" },
                        { "arrow", "Arrows" }, { "grenade", "Grenades" }, { "elixir", "Elixirs" } };
                    auto g = groups.find(e.itemKind);
                    e.group = g != groups.end() ? g->second : "Items";
                    out.push_back(e);
                } while (r->NextRow());
            }
            std::stable_sort(out.begin(), out.end(), [](Entry const& a, Entry const& b)
            {
                auto rank = [](Entry const& e) { return e.kind == "spell" ? 0 : (e.kind == "class" ? 1 : 2); };
                return rank(a) != rank(b) ? rank(a) < rank(b) : a.group < b.group;
            });
            return out;
        }

        void SendList(Player* p, Jig& j)
        {
            LoadVerdicts();
            Send(p, "JO");
            for (std::size_t i = 0; i < j.entries.size(); ++i)
            {
                Entry const& e = j.entries[i];
                auto v = sVerdicts.find(e.key);
                Send(p, Acore::StringFormat("JE^{}^{}^{}^{}^{}^{}", i, e.kind, e.group, v == sVerdicts.end() ? "" : v->second.first,
                    e.kind == "item" ? e.item : e.spell, e.name));
            }
            Send(p, "JD");
        }

        void Despawn(Jig& j, Player* p)
        {
            for (ObjectGuid g : j.dummies)
            {
                sDummies.erase(g);
                sFrail.erase(g);
                if (Creature* c = ObjectAccessor::GetCreature(*p, g))
                    c->DespawnOrUnsummon();
            }
            j.dummies.clear();
        }

        void TakeItems(Player* p, Jig& j)
        {
            for (auto const& [item, n] : j.given)
                p->DestroyItemCount(item, std::min(n, p->GetItemCount(item)), true);
            j.given.clear();
        }

        void Give(Player* p, Jig& j, uint32 item, uint32 want)
        {
            uint32 have = p->GetItemCount(item);
            if (have >= want)
                return;
            if (p->AddItem(item, want - have))
                j.given[item] += want - have;
        }

        // two sparring partners a few metres ahead, and a fight with them
        void Spar(Player* p, Jig& j)
        {
            if (InCombat(p))
                AbandonFight(p);
            Despawn(j, p);
            float o = p->GetOrientation();
            int i = 0;
            for (uint32 entry : { DUMMY_UNDEAD, DUMMY_KOBOLD, DUMMY_KOBOLD })
            {
                float side = i == 0 ? 0.f : (i == 1 ? -0.5f : 0.5f);
                float dist = i == 0 ? 10.f : 7.f;
                uint32 want = i == 0 ? DUMMY_HP : KOBOLD_HP[i - 1];
                ++i;
                float x = p->GetPositionX() + std::cos(o + side) * dist, y = p->GetPositionY() + std::sin(o + side) * dist;
                float z = p->GetMap()->GetHeight(x, y, p->GetPositionZ() + 5.f);
                if (z <= INVALID_HEIGHT)
                    z = p->GetPositionZ();
                Creature* c = p->SummonCreature(entry, x, y, z, o + float(M_PI), TEMPSUMMON_MANUAL_DESPAWN);
                if (!c)
                    continue;
                sDummies.insert(c->GetGUID());
                if (entry == DUMMY_KOBOLD)
                    sFrail.insert(c->GetGUID());
                j.dummies.push_back(c->GetGUID());
                c->SetFaction(14);
                // the fight scales a group's hit points for the party: aim so that it shows `want`
                c->SetMaxHealth(entry == DUMMY_KOBOLD ? std::max<uint32>(1, uint32(std::lround(want / PartyHpScale(TunedPartySize(), false))))
                                                      : DUMMY_HP);
                c->SetFullHealth();
            }
            if (j.dummies.empty())
                return;
            Creature* first = ObjectAccessor::GetCreature(*p, j.dummies[0]);
            if (!first)
                return;
            StartEncounter(first, p);
            for (std::size_t k = 1; k < j.dummies.size(); ++k)
                if (Creature* c = ObjectAccessor::GetCreature(*p, j.dummies[k]))
                    AddCreatureToFight(c, p);
            GoFirst(p);                          // (the jig's fights always open with your turn)
        }

        // the character becomes what the entry needs: its class and subclass, at level 6, with that kit
        void Become(Player* p, Jig& j, Entry const& e)
        {
            Sheet& s = GetSheet(p);
            if (e.cls)
            {
                s.cls = e.cls;
                s.subclass = e.sub;
                s.abil = ClassInfo(e.cls)->abil;
                s.picks.clear();
            }
            EnsureKit(p);
            LongRest(p);
            if (e.kind == "item")
            {
                Give(p, j, e.item, ITEM_STACK);
                if (e.itemKind == "arrow")
                    Give(p, j, SHORTBOW, 1);
                p->addActionButton(0, e.item, ACTION_BUTTON_ITEM);
            }
            else
            {
                if (!p->HasSpell(e.spell))
                    p->learnSpell(e.spell);
                p->addActionButton(0, e.spell, ACTION_BUTTON_SPELL);
            }
            p->SendActionButtons(1);
            PushSheet(p);
        }
    }

    bool InJig(Player* p) { return p && sJigs.count(p->GetGUID()) != 0; }
    bool IsJigDummy(ObjectGuid g) { return sDummies.count(g) != 0; }

    CreatureStats const& JigDummyStats(ObjectGuid g)
    {
        static CreatureStats frail = []
        {
            CreatureStats d;
            d.ac = 10;
            d.hp = int(FRAIL_HP);
            d.speed = 9.f;
            d.abil = { { 8, 12, 10, 8, 8, 8 } };
            d.melee.name = "Nip";
            d.melee.bonus = 2;
            d.melee.dice = Dice::Parse("1");
            d.melee.type = DMG_PIERCING;
            d.xp = 0;
            return d;
        }();
        if (sFrail.count(g))
            return frail;
        static CreatureStats st = []
        {
            CreatureStats d;
            d.ac = 12;
            d.hp = int(DUMMY_HP);
            d.speed = 9.f;
            d.abil = { { 12, 12, 12, 10, 10, 10 } };
            d.melee.name = "Jab";
            d.melee.bonus = 3;
            d.melee.dice = Dice::Parse("1d2");
            d.melee.type = DMG_BLUDGEONING;
            d.xp = 0;
            return d;
        }();
        return st;
    }

    // the start of the player's turn: every slot and class resource back, health topped up, the item restocked
    void JigRefill(Player* p)
    {
        auto itr = sJigs.find(p->GetGUID());
        if (itr == sJigs.end())
            return;
        Sheet& s = GetSheet(p);
        s.slotsUsed.fill(0);
        s.shortRestsUsed = 0;
        s.arcaneRecoveryUsed = false;
        s.secondWindUsed = s.actionSurgeUsed = s.supUsed = 0;
        s.rageUsed = s.kiUsed = s.spUsed = s.tidesUsed = s.wholenessUsed = 0;
        s.patronUsed = s.patron6Used = 0;
        s.inspirationUsed = s.wildShapeUsed = s.naturalRecoveryUsed = 0;
        s.channelUsed = s.wrathUsed = 0;
        if (p->IsAlive() && !IsDowned(p))
            p->SetHealth(p->GetMaxHealth());
        Jig& j = itr->second;
        if (j.current >= 0 && j.entries[j.current].kind == "item")
            Give(p, j, j.entries[j.current].item, ITEM_STACK);
        PushSheet(p);
    }

    void OpenJig(Player* p, bool teleport)
    {
        if (!p)
            return;
        if (InJig(p))
        {
            SendList(p, sJigs[p->GetGUID()]);
            return;
        }
        if (InCombat(p) || InDialogue(p))
        {
            Error(p, "Finish what you're doing first.");
            return;
        }
        Jig& j = sJigs[p->GetGUID()];
        j.saved = GetSheet(p);
        j.level = p->GetLevel();
        j.xp = p->GetUInt32Value(PLAYER_XP);
        j.back = p->GetWorldLocation();
        if (ActionButton const* ab = p->GetActionButton(0))
            j.button0 = ab->GetAction() | (uint32(ab->GetType()) << 24);
        if (p->GetLevel() != JIG_LEVEL)
            p->GiveLevel(JIG_LEVEL);
        j.entries = BuildEntries(p);
        for (uint32 item : KIT)
            Give(p, j, item, 1);
        Log(p, Acore::StringFormat("|cffffd100The spell jig: {} entries to test. Pick one on the panel - you become a class that "
            "has it, and two sparring partners come at you. Everything goes back when you leave.|r", j.entries.size()));
        if (teleport)
        {
            j.teleported = true;
            float z = JIG_Z;                     // the flats' real ground, not a guess
            if (Map* m = sMapMgr->CreateBaseMap(JIG_MAP))
            {
                m->LoadGrid(JIG_X, JIG_Y);
                float g = m->GetHeight(JIG_X, JIG_Y, MAX_HEIGHT);
                if (g > INVALID_HEIGHT)
                    z = g + 0.5f;
            }
            p->TeleportTo(JIG_MAP, JIG_X, JIG_Y, z, 0.f);
        }
        SendList(p, j);
    }

    void LeaveJig(Player* p, bool teleport)
    {
        auto itr = sJigs.find(p ? p->GetGUID() : ObjectGuid::Empty);
        if (itr == sJigs.end())
            return;
        Jig& j = itr->second;
        if (InCombat(p))
            AbandonFight(p);
        Despawn(j, p);
        TakeItems(p, j);
        Sheet& s = GetSheet(p);
        s = j.saved;
        if (p->GetLevel() != j.level)
            p->GiveLevel(j.level);
        p->SetUInt32Value(PLAYER_XP, j.xp);
        EnsureKit(p);
        p->UpdateMaxHealth();
        p->SetHealth(p->GetMaxHealth());
        if (j.button0)
            p->addActionButton(0, j.button0 & 0xFFFFFF, uint8(j.button0 >> 24));
        else
            p->removeActionButton(0);
        p->SendActionButtons(1);
        SaveSheet(p);
        PushSheet(p);
        WorldLocation back = j.back;
        bool go = teleport && j.teleported;
        sJigs.erase(itr);
        Send(p, "JX");
        Log(p, "|cffffd100You leave the spell jig. Your own build is back.|r");
        if (go)
            p->TeleportTo(back);
    }

    void JigMessage(Player* p, std::string const& msg)
    {
        auto itr = sJigs.find(p->GetGUID());
        if (itr == sJigs.end())
            return;
        Jig& j = itr->second;
        std::vector<std::string> f;
        for (std::size_t a = 0, b; a <= msg.size(); a = b + 1)
        {
            b = msg.find('^', a);
            if (b == std::string::npos)
                b = msg.size();
            f.push_back(msg.substr(a, b - a));
            if (f.size() == 3 && f[0] == "verdict")
            {
                f.push_back(msg.substr(std::min(msg.size(), b + 1)));    // the note: the rest, carets and all
                break;
            }
        }
        if (f.empty())
            return;
        if (f[0] == "leave")
        {
            LeaveJig(p, true);
            return;
        }
        int idx = f.size() > 1 ? std::atoi(f[1].c_str()) : -1;
        if (f[0] == "ready" && idx >= 0 && idx < int(j.entries.size()))
        {
            if (p->GetMapId() != JIG_MAP && j.teleported)
            {
                Error(p, "Wait until you're on the flats.");
                return;
            }
            if (InCombat(p))
                AbandonFight(p);
            j.current = idx;
            Entry const& e = j.entries[idx];
            Become(p, j, e);
            std::string as = e.cls ? std::string(ClassInfo(e.cls)->name) + (e.sub.empty() ? "" : " (" + e.sub + ")") : "anyone";
            Send(p, Acore::StringFormat("JS^{}^{}", idx, as));
            Log(p, Acore::StringFormat("|cffffd100Testing {} ({}) as {}. It's on action button 1.|r", e.name, e.group, as));
            Spar(p, j);
            return;
        }
        if (f[0] == "again" && j.current >= 0)
        {
            Become(p, j, j.entries[j.current]);
            Spar(p, j);
            return;
        }
        if (f[0] == "verdict" && idx >= 0 && idx < int(j.entries.size()) && f.size() >= 3)
        {
            LoadVerdicts();
            Entry const& e = j.entries[idx];
            std::string v = f[2] == "p" ? "pass" : (f[2] == "f" ? "fail" : "");
            if (v.empty())
                sVerdicts.erase(e.key);
            else
                sVerdicts[e.key] = { v, Clean(f.size() > 3 ? f[3] : "") };
            SaveVerdicts(j.entries);
            Send(p, Acore::StringFormat("JV^{}^{}", idx, v));
        }
    }

    void ForgetJig(Player* p) { LeaveJig(p, false); }
}
