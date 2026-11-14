/*
 * mod-dnd dialogue: BG3-style conversations that can replace (or start) an encounter's fight.
 *
 * An encounter with a dialogue (world: dnd_encounter.dialogue_node) spawns friendly. Walking up to it, or
 * right-clicking its speaker, opens the conversation in the addon. Options can carry a skill check
 * (d20 + ability modifier + proficiency against a DC, natural 20 always succeeds, natural 1 always fails) and
 * lead to another node, to the end of the talk, to a fight (-1: the group turns hostile and initiative is rolled)
 * or to a peaceful resolution (-2: the encounter counts as cleared and pays its full XP, as in BG3).
 */
#include "Dnd.h"

#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "SharedDefines.h"
#include "WorldSession.h"
#include "StringFormat.h"
#include "Timer.h"
#include "Tokenize.h"
#include <algorithm>
#include <set>
#include <cmath>

namespace Dnd
{
    static int32 const NEXT_END = 0;
    static int32 const NEXT_FIGHT = -1;
    static int32 const NEXT_PEACE = -2;
    static int32 const NEXT_AMBUSH = -3;     // a fight in which the enemies are Surprised (lose their first turn)
    static int32 const NEXT_SOUR = -4;       // resolved on a sour note: cleared for half XP
    static uint32 const TRADE_OPTION = 99;   // a villager who sells things: "(Browse their wares.)" opens the shop
    static uint32 const GAME_OPTION = 98;    // a villager with tavern games: the table opens (DndTavern.cpp)
    static uint32 const TAXI_OPTION = 97;    // a gryphon master: "(Fly somewhere.)" learns the flight point, opens the map
    static std::string TownGames(int role);  // (townsfolk section, below)
    static uint32 const HOSTILE_FACTION = 14;

    struct DialogueOption
    {
        uint8 idx = 0;
        std::string text;
        std::string skill;                   // "" = no check
        int dc = 0;
        int32 nextOk = 0, nextFail = 0;
        std::string req;                     // "" or a race / class ("Dwarf", "Wizard") or an ability ("INT>=14")
        std::vector<std::string> need;       // clues the player must have learned ("!clue": must not have)
        std::string adv;                     // knowing this clue gives advantage on the check
        bool once = false;                   // gone after it has been picked
    };

    struct DialogueNode
    {
        uint32 id = 0;
        std::string text;
        std::string shot;                    // "" over your shoulder at the speaker, "close[:N]", "ots:N", "wide"
        uint8 actor = 1;                     // encounter member (1 = leader) who speaks the line / plays the emote
        uint32 emote = 0;
        std::string sets;                    // clue learned on reaching this line
        std::vector<uint32> leaves;          // members who walk off when this line is reached (0 = everyone)
        int patience = 0;                    // change to the talker's patience on reaching this line
        std::vector<DialogueOption> options;
    };

    struct Talk
    {
        uint32 encounterId = 0;
        uint32 node = 0;
        ObjectGuid speaker;
        uint32 checkTimer = 0;
        std::set<std::string> clues;         // what the player has learned (and which one-time options are used)
        int patience = 0, patienceMax = 0;
        bool secured = false;                // offered a no-roll way out to PEACE: later failures don't start fights
        uint32 townSpawn = 0;                // a villager's chat (no encounter): PEACE pays their gift, SOUR closes it
        int townRole = -1;
    };

    static std::unordered_map<uint32, DialogueNode> sNodes;
    static std::set<ObjectGuid> sLeaving;

    static std::set<std::pair<ObjectGuid, uint32>> sGrudges;   // (character, encounter); lasts until the server restarts

    void AddGrudge(Player* p, uint32 encounterId) { sGrudges.insert({ p->GetGUID(), encounterId }); }
    bool HasGrudge(Player* p, uint32 encounterId) { return sGrudges.count({ p->GetGUID(), encounterId }) != 0; }
    void ForgetGrudge(Player* p, uint32 encounterId) { sGrudges.erase({ p->GetGUID(), encounterId }); }

    void ClearGrudges(Player* p)
    {
        for (auto itr = sGrudges.begin(); itr != sGrudges.end();)
            itr = itr->first == p->GetGUID() ? sGrudges.erase(itr) : std::next(itr);
    }

    bool IsLeaving(ObjectGuid creature) { return sLeaving.count(creature) != 0; }
    void ClearLeaving(ObjectGuid creature) { sLeaving.erase(creature); }
    static std::unordered_map<ObjectGuid, Talk> sTalks;
    // onlookers: players near a conversation see it (lines, options, rolls) on the same panel, read-only
    static std::unordered_map<ObjectGuid, ObjectGuid> sWatch;             // watcher -> the player talking
    static std::set<std::pair<ObjectGuid, ObjectGuid>> sWatchDeclined;    // (watcher, talker): closed it, until it ends
    static float const WATCH_RANGE = 20.f, WATCH_LEAVE = 30.f;

    static std::vector<Player*> Watchers(Player* talker)
    {
        std::vector<Player*> out;
        for (auto const& [w, t] : sWatch)
            if (t == talker->GetGUID())
                if (Player* pl = ObjectAccessor::FindPlayer(w))
                    out.push_back(pl);
        return out;
    }

    static std::vector<Player*> Audience(Player* talker)
    {
        std::vector<Player*> out = Watchers(talker);
        out.insert(out.begin(), talker);
        return out;
    }

    static void LogWatchers(Player* talker, std::string const& text)
    {
        for (Player* w : Watchers(talker))
            Log(w, text);
    }

    static void Unwatch(Player* w, bool declined)
    {
        auto itr = sWatch.find(w->GetGUID());
        if (itr == sWatch.end())
            return;
        if (declined)
            sWatchDeclined.insert({ w->GetGUID(), itr->second });
        sWatch.erase(itr);
        Send(w, "DX");
    }

    ObjectGuid WatchedTalker(Player* p)
    {
        auto itr = sWatch.find(p->GetGUID());
        return itr == sWatch.end() ? ObjectGuid::Empty : itr->second;
    }
    static std::map<std::pair<ObjectGuid, uint32>, uint32> sDeclined;   // (player, encounter) -> until (ms)

    // ------------------------------------------------------------------ skills
    struct SkillDef { char const* name; Ability ability; bool proficient; bool trainedOnly; };
    // "proficient" is the default for characters who haven't made their build yet (a scholar wizard: Arcana and
    // History from the class, Insight and Persuasion from the background); a built character uses its own list.
    // "trainedOnly": specialist skills - a conversation only offers their checks to characters proficient in them
    // (anyone may still try the open ones: Perception, Persuasion, Stealth...)
    static SkillDef const sSkills[] =
    {
        { "Athletics", STR, false, false },
        { "Acrobatics", DEX, false, false }, { "Sleight of Hand", DEX, false, true }, { "Stealth", DEX, false, false },
        { "Arcana", INT, true, true }, { "History", INT, true, true }, { "Investigation", INT, false, false },
        { "Nature", INT, false, true }, { "Religion", INT, false, true },
        { "Animal Handling", WIS, false, true }, { "Insight", WIS, true, false }, { "Medicine", WIS, false, true },
        { "Perception", WIS, false, false }, { "Survival", WIS, false, true },
        { "Deception", CHA, false, false }, { "Intimidation", CHA, false, false }, { "Performance", CHA, false, true },
        { "Persuasion", CHA, true, false },
    };

    static SkillDef const* FindSkill(std::string const& name)
    {
        for (SkillDef const& s : sSkills)
            if (StringEqualI(name, s.name))
                return &s;
        return nullptr;
    }

    static bool Proficient(Player* p, SkillDef const& s)
    {
        Sheet& sh = GetSheet(p);
        for (std::string const& k : sh.picks)
            if (k.rfind("skill:", 0) == 0 && StringEqualI(k.substr(6), s.name))
                return true;                 // learned out in the world (a treat's reward)
        if (!sh.created)
        {
            // an unbuilt character: the class's natural four (a wizard: the scholar's flags in sSkills)
            static std::map<uint8, std::vector<char const*>> const defaults = {
                { CLASS_FIGHTER, { "Athletics", "Intimidation", "Perception", "Survival" } },
                { CLASS_BARBARIAN, { "Athletics", "Intimidation", "Perception", "Survival" } },
                { CLASS_DND_ROGUE, { "Stealth", "Sleight of Hand", "Deception", "Perception" } },
                { CLASS_MONK, { "Acrobatics", "Stealth", "Insight", "Religion" } },
                { CLASS_SORCERER, { "Arcana", "Persuasion", "Deception", "Insight" } },
                { CLASS_DND_WARLOCK, { "Arcana", "Deception", "Intimidation", "Religion" } },
                { CLASS_BARD, { "Performance", "Persuasion", "Deception", "Insight" } },
                { CLASS_DND_DRUID, { "Nature", "Animal Handling", "Medicine", "Survival" } },
                { CLASS_CLERIC, { "Religion", "Medicine", "Insight", "Persuasion" } } };
            auto itr = defaults.find(sh.cls);
            if (itr == defaults.end())
                return s.proficient;
            return std::any_of(itr->second.begin(), itr->second.end(), [&](char const* k) { return StringEqualI(s.name, k); });
        }
        if (HasInvocation(p, "Beguiling Influence") && (StringEqualI(s.name, "Deception") || StringEqualI(s.name, "Persuasion")))
            return true;                     // the invocation: Deception and Persuasion
        if (IsClass(p, CLASS_BARD) && Level(p) >= 3 && IsSubclass(p, "Lore") &&
            (StringEqualI(s.name, "Perception") || StringEqualI(s.name, "Insight") || StringEqualI(s.name, "Investigation")))
            return true;                     // College of Lore: three more skills
        if (IsClass(p, CLASS_BARD) && Level(p) >= 3 && (StringEqualI(s.name, "Persuasion") || StringEqualI(s.name, "Deception")))
            return true;                     // (the bard's Expertise skills)
        for (std::string const& k : sh.picks)
            if (k.rfind("skill:", 0) == 0 && StringEqualI(k.substr(6), s.name))
                return true;                 // learned out in the world (a treat's reward), whatever the build says
        for (std::string const& k : sh.skills)
            if (StringEqualI(k, s.name))
                return true;
        return false;
    }

    static int SkillBonus(Player* p, SkillDef const& s)
    {
        bool expert = HasExpertise(p, s.name);           // a rogue's Expertise (it comes with proficiency)
        int prof = Proficient(p, s) || expert ? Prof(Level(p)) : 0;
        if (!prof && IsClass(p, CLASS_BARD) && Level(p) >= 2)
            prof = Prof(Level(p)) / 2;                    // Jack of All Trades
        if (expert)
            prof *= 2;
        return Mod(AbilityScore(p, s.ability)) + prof + ItemSkillBonus(p, s.name);   // (gear: a headband's score, a skill bonus)
    }

    // ------------------------------------------------------------------ character creation (5e / BG3 rules)
    // 27-point buy (8-15 before bonuses), +2 and +1 to two different abilities, a background (two skills) and two
    // wizard skills that the background doesn't already give.
    struct Background { char const* name; char const* a; char const* b; };
    static Background const sBackgrounds[] =
    {
        { "Acolyte", "Insight", "Religion" }, { "Charlatan", "Deception", "Sleight of Hand" },
        { "Criminal", "Deception", "Stealth" }, { "Entertainer", "Acrobatics", "Performance" },
        { "Folk Hero", "Animal Handling", "Survival" }, { "Guild Artisan", "Insight", "Persuasion" },
        { "Noble", "History", "Persuasion" }, { "Outlander", "Athletics", "Survival" },
        { "Sage", "Arcana", "History" }, { "Soldier", "Athletics", "Intimidation" },
        { "Urchin", "Sleight of Hand", "Stealth" },
    };

    static int PointCost(int score)
    {
        static int const cost[] = { 0, 1, 2, 3, 4, 5, 7, 9 };      // 8 .. 15
        return score < 8 || score > 15 ? 1000 : cost[score - 8];
    }

    void PushSkills(Player* p)
    {
        // "K^background^created^skill,skill,..." (the addon works out the bonuses from the sheet)
        Sheet& sh = GetSheet(p);
        std::string list;
        for (SkillDef const& s : sSkills)
            if (Proficient(p, s))
                list += (list.empty() ? "" : ",") + std::string(s.name);
        // "K^background^created^skill,skill,...^the builder's raw choices"
        Send(p, Acore::StringFormat("K^{}^{}^{}^{}", sh.background.empty() ? "Sage" : sh.background, sh.created ? 1 : 0, list,
            sh.build));
    }

    // "str,dex,con,int,wis,cha^plus2^plus1^background^skill,skill"
    void HandleBuild(Player* p, std::string const& args)
    {
        if (InCombat(p))
        {
            Error(p, "Not in the middle of a fight.");
            return;
        }
        Sheet& sh = GetSheet(p);                 // (rebuilding is allowed any time outside a fight, for now)
        std::vector<std::string_view> f = Acore::Tokenize(args, '^', true);
        if (f.size() < 5)
            return;
        std::vector<std::string_view> sc = Acore::Tokenize(f[0], ',', false);
        if (sc.size() != ABILITY_COUNT)
            return;
        std::array<int, ABILITY_COUNT> base{};
        int points = 0;
        for (int i = 0; i < ABILITY_COUNT; ++i)
        {
            base[i] = std::atoi(std::string(sc[i]).c_str());
            points += PointCost(base[i]);
        }
        int plus2 = std::atoi(std::string(f[1]).c_str()), plus1 = std::atoi(std::string(f[2]).c_str());
        if (points > 27 || plus2 < 0 || plus2 >= ABILITY_COUNT || plus1 < 0 || plus1 >= ABILITY_COUNT || plus1 == plus2)
        {
            Error(p, "Those ability scores don't add up (27 points, 8-15 each, +2 and +1 to different abilities).");
            return;
        }
        Background const* bg = nullptr;
        for (Background const& b : sBackgrounds)
            if (StringEqualI(std::string(f[3]), b.name))
                bg = &b;
        if (!bg)
            return;
        std::vector<std::string> skills = { bg->a, bg->b };
        std::vector<std::string_view> picks = Acore::Tokenize(f[4], ',', false);
        std::vector<char const*> const& classSkills = ClassInfo(sh.cls)->skills;
        if (picks.size() != 2)
        {
            Error(p, "Pick two class skills.");
            return;
        }
        for (std::string_view v : picks)
        {
            std::string k(v);
            bool ok = std::any_of(classSkills.begin(), classSkills.end(), [&](char const* w) { return StringEqualI(k, w); });
            bool dup = std::any_of(skills.begin(), skills.end(), [&](std::string const& s) { return StringEqualI(k, s); });
            if (!ok || dup)
            {
                Error(p, "Pick two different class skills your background doesn't already give.");
                return;
            }
            skills.push_back(k);
        }
        for (int i = 0; i < ABILITY_COUNT; ++i)
            sh.abil[i] = base[i] + (i == plus2 ? 2 : 0) + (i == plus1 ? 1 : 0);
        sh.background = bg->name;
        sh.skills = skills;
        sh.created = true;
        sh.build = args.substr(0, 95);
        SaveSheet(p);
        p->UpdateMaxHealth();
        p->SetHealth(p->GetMaxHealth());
        PushSheet(p);
        PushSkills(p);
        Send(p, "C^done");
        Log(p, Acore::StringFormat("|cffffd100{} the {} - STR {} DEX {} CON {} INT {} WIS {} CHA {}. Skills: {}, {}, {}, {}.|r",
            p->GetName(), bg->name, sh.abil[STR], sh.abil[DEX], sh.abil[CON], sh.abil[INT], sh.abil[WIS], sh.abil[CHA],
            skills[0], skills[1], skills[2], skills[3]));
    }

    // "spell:Grease": a spell you know can be cast in a conversation (BG3's spell dialogue options). Leveled spells
    // need - and spend - a slot of their level; cantrips are free.
    struct TalkSpell { char const* name; uint32 id; int level; };
    static TalkSpell const TalkSpells[] = {
        { "Fire Bolt", SPELL_FIRE_BOLT, 0 }, { "Ray of Frost", SPELL_RAY_OF_FROST, 0 }, { "Shocking Grasp", SPELL_SHOCKING_GRASP, 0 },
        { "Magic Missile", SPELL_MAGIC_MISSILE, 1 }, { "Burning Hands", SPELL_BURNING_HANDS, 1 }, { "Ice Knife", SPELL_ICE_KNIFE, 1 },
        { "Mage Armor", SPELL_MAGE_ARMOR, 1 }, { "False Life", SPELL_FALSE_LIFE, 1 }, { "Grease", SPELL_GREASE, 1 },
        { "Enhanced Leap", SPELL_ENHANCED_LEAP, 1 },
    };
    static TalkSpell const* TalkSpellOf(std::string const& req)
    {
        if (req.rfind("spell:", 0) != 0)
            return nullptr;
        for (TalkSpell const& t : TalkSpells)
            if (StringEqualI(req.substr(6), t.name))
                return &t;
        return nullptr;
    }

    static bool MeetsReq(Player* p, std::string const& req)
    {
        if (req.empty())
            return true;
        if (req.rfind("spell:", 0) == 0)
        {
            TalkSpell const* t = TalkSpellOf(req);
            return t && p->HasSpell(t->id) && (t->level == 0 || SlotsLeft(p, t->level) > 0);
        }
        // "INT>=14": an ability score at least this high
        std::size_t ge = req.find(">=");
        if (ge != std::string::npos)
        {
            std::string ab = req.substr(0, ge);
            int need = std::atoi(req.c_str() + ge + 2);
            for (int i = 0; i < ABILITY_COUNT; ++i)
                if (StringEqualI(ab, AbilityNames[i]))
                    return GetSheet(p).abil[i] >= need;
            return false;
        }
        for (uint8 c = CLASS_WIZARD; c <= CLASS_DND_WARLOCK; ++c)
            if (StringEqualI(req, ClassInfo(c)->name))
                return GetSheet(p).cls == c;
        static std::pair<char const*, uint8> const races[] = { { "Human", RACE_HUMAN }, { "Dwarf", RACE_DWARF },
            { "NightElf", RACE_NIGHTELF }, { "Gnome", RACE_GNOME }, { "Draenei", RACE_DRAENEI }, { "Orc", RACE_ORC },
            { "Undead", RACE_UNDEAD_PLAYER }, { "Tauren", RACE_TAUREN }, { "Troll", RACE_TROLL }, { "BloodElf", RACE_BLOODELF } };
        for (auto const& [name, race] : races)
            if (StringEqualI(req, name))
                return p->getRace() == race;
        return false;
    }

    // for treats (DndTreats.cpp): the same skill bonuses and requirements conversations use
    int SkillBonusByName(Player* p, std::string const& skill)
    {
        SkillDef const* s = FindSkill(skill);
        return s ? SkillBonus(p, *s) : 0;
    }
    bool MeetsRequirement(Player* p, std::string const& req) { return MeetsReq(p, req); }

    // ------------------------------------------------------------------ data
    void LoadDialogue()
    {
        sNodes.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT id, text, shot, actor, emote, sets, patience, leaves FROM dnd_dialogue_node"))
        {
            do
            {
                Field* f = r->Fetch();
                DialogueNode& n = sNodes[f[0].Get<uint32>()];
                n.id = f[0].Get<uint32>();
                n.text = f[1].Get<std::string>();
                n.shot = f[2].Get<std::string>();
                n.actor = f[3].Get<uint8>();     // 0 = the narrator
                n.emote = f[4].Get<uint32>();
                n.sets = f[5].Get<std::string>();
                n.patience = f[6].Get<int32>();
                std::string leaves = f[7].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(leaves, ',', false))
                    if (!v.empty())
                        n.leaves.push_back(uint32(std::atoi(std::string(v).c_str())));
            } while (r->NextRow());
        }
        uint32 options = 0;
        if (QueryResult r = WorldDatabase.Query("SELECT node_id, idx, text, skill, dc, next_ok, next_fail, req, need, adv, once "
            "FROM dnd_dialogue_option ORDER BY node_id, idx"))
        {
            do
            {
                Field* f = r->Fetch();
                auto itr = sNodes.find(f[0].Get<uint32>());
                if (itr == sNodes.end())
                    continue;
                DialogueOption o;
                o.idx = f[1].Get<uint8>();
                o.text = f[2].Get<std::string>();
                o.skill = f[3].Get<std::string>();
                o.dc = f[4].Get<int32>();
                o.nextOk = f[5].Get<int32>();
                o.nextFail = f[6].Get<int32>();
                o.req = f[7].Get<std::string>();
                std::string need = f[8].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(need, ',', false))
                    if (!v.empty())
                        o.need.emplace_back(v);
                o.adv = f[9].Get<std::string>();
                o.once = f[10].Get<bool>();
                if (!o.skill.empty() && !FindSkill(o.skill))
                    LOG_ERROR("module", "mod-dnd: dialogue node {} option {} uses unknown skill '{}'", itr->first, o.idx, o.skill);
                itr->second.options.push_back(o);
                ++options;
            } while (r->NextRow());
        }
        sTalks.clear();
        LOG_INFO("module", "mod-dnd: {} dialogue nodes, {} options", sNodes.size(), options);
    }

    // ------------------------------------------------------------------ helpers
    static Creature* SpeakerOf(Player* p, Talk const& t)
    {
        Creature* c = ObjectAccessor::GetCreature(*p, t.speaker);
        return c && c->IsAlive() && c->IsInWorld() ? c : nullptr;
    }

    static std::vector<Creature*> GroupOf(Map* map, EncounterDef const& def)
    {
        std::vector<Creature*> out;
        for (uint32 spawn : def.spawns)
        {
            auto range = map->GetCreatureBySpawnIdStore().equal_range(spawn);
            for (auto itr = range.first; itr != range.second; ++itr)
                if (itr->second->IsAlive() && itr->second->IsInWorld())
                    out.push_back(itr->second);
        }
        return out;
    }

    static void SendLong(Player* p, std::string const& prefix, std::string const& text)
    {
        // addon messages carry ~250 bytes: split at spaces
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t len = std::min<std::size_t>(200, text.size() - pos);
            if (pos + len < text.size())
            {
                std::size_t sp = text.rfind(' ', pos + len);
                if (sp != std::string::npos && sp > pos)
                    len = sp - pos + 1;
            }
            Send(p, prefix + text.substr(pos, len));
            pos += len;
        }
    }

    // ------------------------------------------------------------------ staging
    // (The 3.3.5 client gives addons no reliable camera control, so conversations keep the player's own camera;
    // the scene is the group turning to you, their gestures and the letterbox bars.)
    static Creature* Member(Player* p, EncounterDef const& def, uint32 n, Creature* fallback)
    {
        if (n < 1 || n > def.spawns.size())
            return fallback;
        auto range = p->GetMap()->GetCreatureBySpawnIdStore().equal_range(def.spawns[n - 1]);
        for (auto itr = range.first; itr != range.second; ++itr)
            if (itr->second->IsAlive() && itr->second->IsInWorld())
                return itr->second;
        return fallback;
    }

    // one-time options are remembered by their text, so a question asked from one copy of a hub is gone from all of them
    static void WalkOff(Player* p, EncounterDef const& def, DialogueNode const& n)
    {
        if (n.leaves.empty())
            return;
        bool all = std::find(n.leaves.begin(), n.leaves.end(), 0u) != n.leaves.end();
        for (uint32 i = 1; i <= def.spawns.size(); ++i)
        {
            if (!all && std::find(n.leaves.begin(), n.leaves.end(), i) == n.leaves.end())
                continue;
            Creature* c = Member(p, def, i, nullptr);
            if (!c || IsLeaving(c->GetGUID()))
                continue;
            sLeaving.insert(c->GetGUID());
            float ang = p->GetAngle(c);
            float x = c->GetPositionX() + std::cos(ang) * 22.f, y = c->GetPositionY() + std::sin(ang) * 22.f, z = c->GetPositionZ();
            c->UpdateGroundPositionZ(x, y, z);
            c->GetMotionMaster()->Clear();
            c->GetMotionMaster()->MovePoint(0, x, y, z);
            // they vanish when the conversation ends (CloseDialogue), not mid-sentence
        }
    }

    // "{{wizard words|fighter words}}" in a line: the part for the player's class
    // {{wizard text|fighter text}} by the player's class; [[outdoors text|indoors text]] by where the speaker stands
    static std::string ClassText(Player* p, std::string s, Creature* speaker = nullptr)
    {
        bool fighter = !IsCaster(p);         // {{caster text|martial text}}: wizards and sorcerers / everyone else
        bool outdoors = speaker ? speaker->IsOutdoors() : p->IsOutdoors();
        size_t open;
        while ((open = s.find("[[")) != std::string::npos)
        {
            size_t close = s.find("]]", open);
            if (close == std::string::npos)
                break;
            std::string inner = s.substr(open + 2, close - open - 2);
            size_t bar = inner.find('|');
            std::string pick = bar == std::string::npos ? (outdoors ? inner : "") : (outdoors ? inner.substr(0, bar) : inner.substr(bar + 1));
            s.replace(open, close - open + 2, pick);
        }
        while ((open = s.find("{{")) != std::string::npos)
        {
            size_t close = s.find("}}", open);
            if (close == std::string::npos)
                break;
            std::string inner = s.substr(open + 2, close - open - 2);
            size_t bar = inner.find('|');
            std::string pick = bar == std::string::npos ? inner : (fighter ? inner.substr(bar + 1) : inner.substr(0, bar));
            s.replace(open, close - open + 2, pick);
        }
        return s;
    }

    static std::string OnceKey(DialogueOption const& o) { return "#" + o.text; }

    static bool Visible(Player* p, Talk const& t, DialogueOption const& o)
    {
        if (!MeetsReq(p, o.req))
            return false;
        if (!o.skill.empty())
            if (SkillDef const* sk = FindSkill(o.skill))
                if (sk->trainedOnly && !Proficient(p, *sk))
                    return false;            // a specialist's option: only the trained think of it
        if (o.once && t.clues.count(OnceKey(o)))
            return false;
        for (std::string const& c : o.need)
        {
            bool neg = !c.empty() && c[0] == '!';
            bool has = t.clues.count(neg ? c.substr(1) : c) != 0;
            if (has == neg)
                return false;
        }
        return true;
    }

    static void ShowNode(Player* p, Talk& t);
    static void SendNode(Player* to, Player* p, Talk& t, DialogueNode const& n, Creature* speaker, std::string const& voiceName,
        std::string const& text);
    static void Peace(Player* p, Creature* speaker, EncounterDef const& def, bool halfXp = false);

    // a fight, or a line whose every answer is a fight (a failed check's escalation, a snap)
    static bool LeadsToFight(int32 next)
    {
        if (next == NEXT_FIGHT || next == NEXT_AMBUSH)
            return true;
        auto itr = next > 0 ? sNodes.find(uint32(next)) : sNodes.end();
        if (itr == sNodes.end() || itr->second.options.empty())
            return false;
        for (DialogueOption const& o : itr->second.options)
            if ((o.nextOk != NEXT_FIGHT && o.nextOk != NEXT_AMBUSH) || (!o.skill.empty() && o.nextFail != NEXT_FIGHT && o.nextFail != NEXT_AMBUSH))
                return false;
        return true;
    }

    // a secured talk went wrong: the encounter's sour line, or (none written) a short exit - half XP, no fight
    static void SourEnding(Player* p, Talk& t, EncounterDef const& def)
    {
        if (def.sourNode && sNodes.count(def.sourNode))
        {
            t.node = def.sourNode;
            ShowNode(p, t);
            return;
        }
        Creature* speaker = SpeakerOf(p, t);
        CloseDialogue(p, false);
        if (!speaker)
            return;
        Log(p, Acore::StringFormat("|cffc8b890{} has had enough of your questions, and turns away.|r", speaker->GetName()));
        Peace(p, speaker, def, true);
    }

    static void ShowNode(Player* p, Talk& t)
    {
        auto itr = sNodes.find(t.node);
        Creature* speaker = SpeakerOf(p, t);
        if (itr == sNodes.end() || !speaker)
        {
            CloseDialogue(p, false);
            return;
        }
        DialogueNode const& n = itr->second;
        EncounterDef const* def = GetEncounterDef(t.encounterId);

        // what this line teaches, and what it does to the talker's patience
        bool learned = false;
        for (std::string_view v : Acore::Tokenize(n.sets, ',', false))
            if (!v.empty() && t.clues.emplace(v).second)
                learned = true;
        if (learned)
            for (Player* to : Audience(p))
                Send(to, "DK");              // the addon plays the "secret found" jingle with this line
        if (t.patienceMax > 0 && n.patience)
        {
            t.patience = std::clamp(t.patience + n.patience, 0, t.patienceMax);
            if (n.patience < 0)
                Log(p, Acore::StringFormat("|cffff8060{} drops.|r", def && !def->meterLabel.empty() ? def->meterLabel : "Patience"));
            else
                Log(p, Acore::StringFormat("|cff80ff80{} rises.|r", def && !def->meterLabel.empty() ? def->meterLabel : "Patience"));
            if (t.patience == 0 && def && t.secured && t.node != def->sourNode && t.node != def->snapNode)
            {
                for (Player* to : Audience(p))
                    Send(to, Acore::StringFormat("DP^0^{}", t.patienceMax));
                SourEnding(p, t, *def);      // out of patience, but the problem is already solved: no fight
                return;
            }
            if (t.patience == 0 && def && def->snapNode && t.node != def->snapNode)
            {
                for (Player* to : Audience(p))
                    Send(to, Acore::StringFormat("DP^0^{}", t.patienceMax));
                t.node = def->snapNode;      // out of patience
                ShowNode(p, t);
                return;
            }
        }

        // everyone in the group turns to the player; whoever has the line speaks it
        Creature* voice = speaker;
        std::string voiceName = speaker->GetName();
        if (def)
        {
            for (Creature* c : GroupOf(p->GetMap(), *def))
                if (!IsLeaving(c->GetGUID()))
                    c->SetFacingToObject(p);
            if (n.actor == 0)
                voiceName = def->name;       // narration: the scene speaks, nobody moves their lips
            else
            {
                voice = Member(p, *def, n.actor, speaker);
                voiceName = voice->GetName();
                voice->HandleEmoteCommand(n.emote ? n.emote : EMOTE_ONESHOT_TALK);
            }
            if (n.actor == 0 && n.emote)
                speaker->HandleEmoteCommand(n.emote);
            WalkOff(p, *def, n);
        }
        else
        {
            speaker->SetFacingToObject(p);
            speaker->StopMoving();
            speaker->PauseMovement(60000);   // a walking villager stops for the chat
            speaker->HandleEmoteCommand(n.emote ? n.emote : EMOTE_ONESHOT_TALK);
        }

        std::string text = ClassText(p, n.text, speaker);
        for (Player* to : Audience(p))
            SendNode(to, p, t, n, speaker, voiceName, text);
        std::string logLine = n.actor == 0 ? "|cffc8b890" + text + "|r" : Acore::StringFormat("|cffffd100{}:|r {}", voiceName, text);
        Log(p, logLine);
        LogWatchers(p, logLine);
    }

    // one line of a conversation on someone's panel: the talker's own, or an onlooker's copy (the same options, as the
    // talker sees them - the onlooker's panel doesn't let them pick)
    static void SendNode(Player* to, Player* p, Talk& t, DialogueNode const& n, Creature* speaker, std::string const& voiceName,
        std::string const& text)
    {
        EncounterDef const* def = GetEncounterDef(t.encounterId);
        Send(to, "DS^" + voiceName);
        if (t.patienceMax > 0)
            Send(to, Acore::StringFormat("DP^{}^{}^{}", t.patience, t.patienceMax,
                def && !def->meterLabel.empty() ? def->meterLabel : std::string("Patience")));
        SendLong(to, "DT^", text);
        for (DialogueOption const& o : n.options)
        {
            if (!Visible(p, t, o))
                continue;
            // "DO^idx^requirement^skill^dc^bonus^attack^advantage^text"
            std::string skill;
            int dc = 0, bonus = 0;
            if (!o.skill.empty())
                if (SkillDef const* sk = FindSkill(o.skill))
                {
                    skill = sk->name;
                    dc = o.dc;
                    bonus = SkillBonus(p, *sk);
                }
            bool attack = o.skill.empty() && (o.nextOk == NEXT_FIGHT || o.nextOk == NEXT_AMBUSH);
            if (o.skill.empty() && o.nextOk == NEXT_PEACE)
                t.secured = true;            // they could walk away with the win right now
            bool adv = !o.adv.empty() && t.clues.count(o.adv);
            Send(to, Acore::StringFormat("DO^{}^{}^{}^{}^{}^{}^{}^{}", o.idx, o.req, skill, dc, bonus, attack ? 1 : 0, adv ? 1 : 0,
                ClassText(p, o.text, speaker).substr(0, 180)));
        }
        if (t.townSpawn && speaker->IsVendor())
            Send(to, Acore::StringFormat("DO^{}^^^0^0^0^0^(Browse their wares.)", TRADE_OPTION));
        if (t.townSpawn && speaker->IsTaxi())
            Send(to, Acore::StringFormat("DO^{}^^^0^0^0^0^(Fly somewhere.)", TAXI_OPTION));
        if (t.townSpawn && !TownGames(t.townRole).empty())
            Send(to, Acore::StringFormat("DO^{}^^^0^0^0^0^{}", GAME_OPTION, TownGames(t.townRole) == "drink" ?
                "(Challenge them to a drinking contest.)" : "(Sit down for a game of chance.)"));
        Send(to, "DE");
    }

    // an onlooker arrives: their panel opens on the line the talker is on now
    static void Watch(Player* w, Player* talker, Talk& t)
    {
        if (WatchedTalker(w) == talker->GetGUID())
            return;
        Unwatch(w, false);
        auto itr = sNodes.find(t.node);
        Creature* speaker = SpeakerOf(talker, t);
        EncounterDef const* def = GetEncounterDef(t.encounterId);
        if (itr == sNodes.end() || !speaker || !def)
            return;
        sWatch[w->GetGUID()] = talker->GetGUID();
        DialogueNode const& n = itr->second;
        std::string voiceName = n.actor == 0 ? def->name : Member(talker, *def, n.actor, speaker)->GetName();
        Send(w, "DW^" + std::string(talker->GetName()));
        Log(w, Acore::StringFormat("|cffc0c0ff--- {} talks: {} ---|r", talker->GetName(), def->name));
        SendNode(w, talker, t, n, speaker, voiceName, ClassText(talker, n.text, speaker));
    }

    // who is close enough to watch (an encounter's conversation; a villager's chat is private), and who has wandered off
    static void WatchScan(Player* talker, Talk& t)
    {
        for (Player* w : Watchers(talker))
            if (!w->IsAlive() || InCombat(w) || InDialogue(w) || w->GetMap() != talker->GetMap() || w->GetExactDist(talker) > WATCH_LEAVE)
                Unwatch(w, false);
        if (t.townSpawn || !t.encounterId)
            return;
        std::vector<Player*> fresh;
        talker->GetMap()->DoForAllPlayers([&](Player* pl)
        {
            if (pl != talker && pl->IsAlive() && !pl->IsGameMaster() && !InCombat(pl) && !InDialogue(pl) && sWatch.count(pl->GetGUID()) == 0
                && !sWatchDeclined.count({ pl->GetGUID(), talker->GetGUID() }) && pl->GetExactDist(talker) <= WATCH_RANGE)
                fresh.push_back(pl);
        });
        for (Player* pl : fresh)
            Watch(pl, talker, t);
    }

    static void StartFight(Player* p, Creature* speaker, EncounterDef const& def, bool ambush = false)
    {
        Creature* source = nullptr;
        for (Creature* c : GroupOf(p->GetMap(), def))
            if (!IsLeaving(c->GetGUID()))
            {
                c->SetFaction(HOSTILE_FACTION);
                if (!source || c == speaker)
                    source = c;
            }
        if (!source)
            return;                          // everyone walked off
        StartEncounter(source, p);
        if (!ambush)
            return;
        if (Encounter* e = EncounterOf(source->GetGUID()))
            for (Combatant& c : e->order)
                if (!c.player)
                    c.surprised = true;
        Log(p, "|cffffd100You strike from hiding - your enemies are Surprised!|r");
    }

    static void Peace(Player* p, Creature* speaker, EncounterDef const& def, bool halfXp)
    {
        speaker->HandleEmoteCommand(EMOTE_ONESHOT_BOW);
        std::vector<Player*> party;
        p->GetMap()->DoForAllPlayers([&](Player* other)
        {
            if (other == p || (!other->IsGameMaster() && other->IsAlive() && !InCombat(other) && other->GetExactDist(speaker) <= 30.f))
                party.push_back(other);
        });
        for (Player* pl : party)
            Send(pl, "V^RESOLVED");          // the banner for those there; the win counts for everyone online
        ShareEncounterWon(party, def, true, halfXp);
    }

    // ------------------------------------------------------------------ API
    bool InDialogue(Player* p) { return sTalks.count(p->GetGUID()) != 0; }

    bool DialogueDeclined(Player* p, uint32 encounterId)
    {
        auto itr = sDeclined.find({ p->GetGUID(), encounterId });
        return itr != sDeclined.end() && getMSTimeDiff(getMSTime(), itr->second) < 0x80000000u;
    }

    bool StartDialogue(Player* p, Creature* speaker, bool clicked)
    {
        if (!p || !speaker || !speaker->IsAlive() || InCombat(p) || InCombat(speaker) || !p->IsAlive() || p->IsInFlight())
            return false;
        EncounterDef const* def = GetEncounterDefBySpawn(speaker->GetSpawnId());
        if (!def || !def->dialogueNode || HasGrudge(p, def->id))
            return false;
        // someone is already talking to them: you watch that conversation instead of starting a second one
        for (auto& [guid, other] : sTalks)
            if (guid != p->GetGUID() && other.encounterId == def->id)
                if (Player* talker = ObjectAccessor::FindPlayer(guid); talker && talker->GetMap() == p->GetMap())
                {
                    if (clicked)
                        sWatchDeclined.erase({ p->GetGUID(), guid });
                    else if (sWatchDeclined.count({ p->GetGUID(), guid }))
                        return false;
                    Watch(p, talker, other);
                    return true;
                }
        Unwatch(p, false);                   // (watching someone else's talk: your own takes over the panel)
        StopSneaking(p, "You step out of the shadows to talk.");     // they've seen you now: no ambush out of a chat
        if (HasCleared(p, def->id))
        {
            if (!def->afterText.empty())
                Log(p, "|cffc0c0c0" + def->afterText + "|r");
            speaker->SetFacingToObject(p);
            speaker->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
            return true;
        }
        // the conversation is with the group's leader (its first spawn) whoever was clicked
        Creature* leader = speaker;
        std::vector<Creature*> group = GroupOf(p->GetMap(), *def);
        for (Creature* c : group)
            if (!def->spawns.empty() && c->GetSpawnId() == def->spawns.front())
                leader = c;
        Talk& t = sTalks[p->GetGUID()];
        t = Talk();
        t.encounterId = def->id;
        t.node = def->dialogueNode;
        t.speaker = leader->GetGUID();
        t.patience = t.patienceMax = std::max(0, def->patience);
        for (std::string const& f : TreatFlags(p))
            t.clues.insert("found:" + f);    // things found in the world count as clues ("found:compass")
        // turn to whoever is talking and hold still for the conversation; the addon draws the letterbox bars
        p->StopMoving();
        SetRooted(p, true);              // rooted first, then turned to face them (the turn is a tiny teleport)
        p->NearTeleportTo(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), p->GetAngle(leader));
        Send(p, "DC");
        Log(p, Acore::StringFormat("|cffc0c0ff--- {} ---|r", def->name));
        ShowNode(p, t);
        if (auto again = sTalks.find(p->GetGUID()); again != sTalks.end())
            WatchScan(p, again->second);     // whoever is standing with you sees it from the first line
        return true;
    }

    static void TownResult(Player* p, Creature* speaker, Talk const& t, bool won);

    // a villager's talk shut on its last line (Escape, walking off, logging out) - every way on from it the same win or
    // the same loss - ends as that line does: a failed check can't be shrugged off and tried again
    static void SettleTownTalk(Player* p, Talk const& t)
    {
        auto node = sNodes.find(t.node);
        if (!t.townSpawn || node == sNodes.end())
            return;
        int32 ending = 0;
        for (DialogueOption const& o : node->second.options)
        {
            if (!Visible(p, t, o))
                continue;
            if (!o.skill.empty() || (o.nextOk != NEXT_PEACE && o.nextOk != NEXT_SOUR) || (ending && o.nextOk != ending))
                return;                      // a choice still open: nothing settled
            ending = o.nextOk;
        }
        Creature* speaker = SpeakerOf(p, t);
        if (ending && (speaker || ending == NEXT_SOUR))
            TownResult(p, speaker, t, ending == NEXT_PEACE);
    }

    void CloseDialogue(Player* p, bool leave, uint32 cooldownMs)
    {
        auto itr = sTalks.find(p->GetGUID());
        if (itr == sTalks.end())
        {
            Unwatch(p, leave);               // an onlooker closed the panel (Escape): it stays shut until this talk ends
            return;
        }
        SettleTownTalk(p, itr->second);      // (however it closes: Escape, walking off, logging out; TownResult pays once)
        for (Player* w : Watchers(p))
            Unwatch(w, false);
        for (auto d = sWatchDeclined.begin(); d != sWatchDeclined.end();)
            d = d->second == p->GetGUID() ? sWatchDeclined.erase(d) : std::next(d);
        if (leave)
            sDeclined[{ p->GetGUID(), itr->second.encounterId }] = getMSTime() + cooldownMs;
        // whoever walked off during the talk is gone for good once it's over (like a won fight)
        if (EncounterDef const* def = GetEncounterDef(itr->second.encounterId))
            for (uint32 i = 1; i <= def->spawns.size(); ++i)
                if (Creature* c = Member(p, *def, i, nullptr))
                    if (IsLeaving(c->GetGUID()))
                        c->DespawnOrUnsummon(2500ms);
        Send(p, "DCX");
        sTalks.erase(itr);
        if (!InCombat(p))
            SetRooted(p, false);             // a fight that follows roots and frees by turns itself
        Send(p, "DX");
    }

    // the talker's mouse is over option idx (0: over none): onlookers see which
    void DialogueHover(Player* p, uint32 idx)
    {
        if (!InDialogue(p))
            return;
        for (Player* w : Watchers(p))
            Send(w, Acore::StringFormat("DH^{}", idx));
    }

    void DialogueChoose(Player* p, uint32 idx)
    {
        auto itr = sTalks.find(p->GetGUID());
        if (itr == sTalks.end())
            return;
        Talk t = itr->second;
        Creature* speaker = SpeakerOf(p, t);
        EncounterDef const* def = GetEncounterDef(t.encounterId);
        auto node = sNodes.find(t.node);
        bool town = t.townSpawn != 0;
        if (!speaker || (!def && !town) || node == sNodes.end())
        {
            CloseDialogue(p, false);
            return;
        }
        if (town && idx == GAME_OPTION && !TownGames(t.townRole).empty())
        {
            CloseDialogue(p, true, 0);       // the talk waits; the table opens
            OpenTavern(p, speaker, TownGames(t.townRole));
            return;
        }
        if (town && idx == TAXI_OPTION && speaker->IsTaxi())
        {
            CloseDialogue(p, true, 0);       // the talk waits; the flight map opens (and a new flight point is learned)
            p->GetSession()->SendLearnNewTaxiNode(speaker);
            p->GetSession()->SendTaxiMenu(speaker);
            return;
        }
        if (town && idx == TRADE_OPTION && speaker->IsVendor())
        {
            CloseDialogue(p, true, 0);       // the talk waits; the shop opens
            p->GetSession()->SendListInventory(speaker->GetGUID());
            return;
        }
        DialogueOption const* opt = nullptr;
        for (DialogueOption const& o : node->second.options)
            if (o.idx == idx && Visible(p, t, o))
                opt = &o;
        if (!opt)
            return;
        // onlookers see the choice lit up (their panel holds it a moment before the answer plays)
        for (Player* w : Watchers(p))
            Send(w, Acore::StringFormat("DH^{}^1", idx));
        if (opt->once)
            itr->second.clues.insert(OnceKey(*opt));

        Log(p, "|cff80c0ffYou:|r " + ClassText(p, opt->text, speaker));
        LogWatchers(p, Acore::StringFormat("|cff80c0ff{}:|r {}", p->GetName(), ClassText(p, opt->text, speaker)));
        if (TalkSpell const* ts = TalkSpellOf(opt->req))
        {
            // the spell is cast: the caster's flourish, and a slot spent if it has a level
            p->HandleEmoteCommand(EMOTE_ONESHOT_SPELL_CAST_OMNI);
            if (ts->level > 0)
            {
                ++GetSheet(p).slotsUsed[ts->level];
                SaveSheet(p);
                PushSheet(p);
                Log(p, Acore::StringFormat("   |cffb48cffYou cast {} (a level {} slot).|r", ts->name, ts->level));
            }
            else
                Log(p, Acore::StringFormat("   |cffb48cffYou cast {}.|r", ts->name));
        }
        int32 next = opt->nextOk;
        if (!opt->skill.empty())
        {
            SkillDef const* s = FindSkill(opt->skill);
            int bonus = s ? SkillBonus(p, *s) : 0;
            int adv = !opt->adv.empty() && t.clues.count(opt->adv) ? 1 : 0;
            if (s && s->ability == STR && StrengthCheckAdvantage(p))
                adv = 1;                         // Aspect of the Bear
            if (s && ItemSkillAdvantage(p, s->name))
                adv = 1;                         // gear that gives advantage on this skill
            D20Result r = RollD20(bonus, adv);
            bool ok = r.natural == 20 || (r.natural != 1 && r.total >= opt->dc);
            if (!ok && r.natural != 1 && HeldInspiration(p) && r.total + HeldInspiration(p) >= opt->dc)
            {
                int bi = SpendInspiration(p, "the check");
                r.total += bi;
                r.text += Acore::StringFormat(" + {} (Bardic Inspiration) = {}", bi, r.total);
                ok = r.total >= opt->dc;
            }
            std::string rollLine = Acore::StringFormat("   {} check (DC {}): {} - {}", opt->skill, opt->dc, r.text,
                ok ? "|cff40ff40SUCCESS|r" : "|cffff4040FAILURE|r");
            Log(p, rollLine);
            LogWatchers(p, Acore::StringFormat("   {}'s{}", p->GetName(), rollLine.substr(2)));
            for (Player* to : Audience(p))
                Send(to, Acore::StringFormat("DR^{}^{}^{}^{}^{}", opt->skill, r.natural, bonus, opt->dc, ok ? 1 : 0));
            next = ok ? opt->nextOk : opt->nextFail;
            if (!ok && t.secured && def && LeadsToFight(next))
            {
                // the problem was already solved before this went wrong: it sours instead of coming to blows
                if (def->sourNode)
                    next = int32(def->sourNode);
                else
                {
                    CloseDialogue(p, false);
                    Log(p, Acore::StringFormat("|cffc8b890{} has had enough of your questions, and turns away.|r", speaker->GetName()));
                    Peace(p, speaker, *def, true);
                    return;
                }
            }
        }

        if (next > 0)
        {
            itr->second.node = uint32(next);
            ShowNode(p, itr->second);
        }
        else if (next == NEXT_END)
            CloseDialogue(p, true);
        else if (town)
        {
            CloseDialogue(p, false);
            if (next == NEXT_PEACE || next == NEXT_SOUR)
                TownResult(p, speaker, t, next == NEXT_PEACE);
        }
        else if (next == NEXT_FIGHT || next == NEXT_AMBUSH)
        {
            CloseDialogue(p, false);
            StartFight(p, speaker, *def, next == NEXT_AMBUSH);
        }
        else if (next == NEXT_PEACE || next == NEXT_SOUR)
        {
            CloseDialogue(p, false);
            Peace(p, speaker, *def, next == NEXT_SOUR);
        }
    }

    void UpdateDialogue(Player* p, uint32 diff)
    {
        auto itr = sTalks.find(p->GetGUID());
        if (itr == sTalks.end())
            return;
        itr->second.checkTimer += diff;
        if (itr->second.checkTimer < 500)
            return;
        itr->second.checkTimer = 0;
        Creature* speaker = SpeakerOf(p, itr->second);
        if (!speaker || !p->IsAlive() || InCombat(p))
        {
            CloseDialogue(p, true, 5000);
            return;
        }
        if (!p->HasUnitState(UNIT_STATE_ROOT))
            SetRooted(p, true);              // the grip holds for the whole conversation, whatever dropped it
        WatchScan(p, itr->second);
        // walked away: measured to the nearest member of the group, not just the leader - a group can be spread
        // wider than the talk range, and the scene may open next to a member far from the leader
        float nearest = p->GetExactDist(speaker);
        if (EncounterDef const* def = GetEncounterDef(itr->second.encounterId))
            for (Creature* c : GroupOf(p->GetMap(), *def))
                if (c->IsAlive())
                    nearest = std::min(nearest, p->GetExactDist(c));
        if (nearest > 25.f && !IsLeaving(speaker->GetGUID()))
        {
            Log(p, "|cffc8b890You step away and the moment passes. Walk back to pick it up again.|r");
            CloseDialogue(p, true, 5000);
        }
    }

    void ForgetDialogue(ObjectGuid guid)
    {
        sTalks.erase(guid);
        sWatch.erase(guid);
        for (auto w = sWatch.begin(); w != sWatch.end();)
            w = w->second == guid ? sWatch.erase(w) : std::next(w);
        for (auto d = sWatchDeclined.begin(); d != sWatchDeclined.end();)
            d = d->first == guid || d->second == guid ? sWatchDeclined.erase(d) : std::next(d);
        for (auto itr = sDeclined.begin(); itr != sDeclined.end();)
            itr = itr->first.first == guid ? sDeclined.erase(itr) : std::next(itr);
    }
    // ------------------------------------------------------------------ townsfolk
    // Roles (world: dnd_townsfolk_role, from tools/townsfolk.py) are matched by keywords against a villager's name
    // and title, first match wins; "f<id>" matches a faction, "*" anyone. Each role has a couple of opening lines
    // (picked by spawn) leading to one check; a win pays the role's gift once per character and villager, a loss
    // closes that villager's offer for good. Either way they just chat afterwards.
    struct TownRole
    {
        std::string role;
        std::vector<std::string> keys;
        std::vector<uint32> nodes;
        uint32 money = 0, item = 0;
        std::string gift, after;
        std::string games;                   // tavern games on offer: "dice,cards", "drink" (DndTavern.cpp)
        uint32 zone = 0;                     // only villagers in this zone (0: anywhere) - a town's own voice
    };
    static std::vector<TownRole> sRoles;
    static std::string TownGames(int role);
    static std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint8>> sTownDone;   // spawn -> 1 won, 2 lost
    // the open lands with villagers to talk to: Dun Morogh (Coldridge included; Ironforge is a zone of its own), Loch Modan
    // Dun Morogh, Loch Modan, Wetlands, Arathi, Searing Gorge, Ironforge, Stormwind, Elwynn Forest
    static bool IsTownZone(uint32 zone)
    {
        return zone == 1 || zone == 38 || zone == 11 || zone == 45 || zone == 51 || zone == 1537 || zone == 1519 || zone == 12;
    }

    static bool IsFactionKey(std::string const& k)
    {
        return k.size() > 1 && k[0] == 'f' && std::all_of(k.begin() + 1, k.end(), [](char ch) { return std::isdigit((unsigned char)ch); });
    }

    void LoadTownsfolk()
    {
        sRoles.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT role, keywords, nodes, money, item, gift, after_text, games, zone FROM dnd_townsfolk_role ORDER BY ord"))
        {
            do
            {
                Field* f = r->Fetch();
                TownRole role;
                role.role = f[0].Get<std::string>();
                std::string keys = f[1].Get<std::string>(), nodes = f[2].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(keys, ',', false))
                    if (!v.empty())
                    {
                        std::string k(v);
                        std::transform(k.begin(), k.end(), k.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                        role.keys.push_back(k);
                    }
                for (std::string_view v : Acore::Tokenize(nodes, ',', false))
                {
                    uint32 n = uint32(std::atoi(std::string(v).c_str()));
                    if (n && sNodes.count(n))
                        role.nodes.push_back(n);
                }
                role.money = f[3].Get<uint32>();
                role.item = f[4].Get<uint32>();
                role.gift = f[5].Get<std::string>();
                role.after = f[6].Get<std::string>();
                role.games = f[7].Get<std::string>();
                role.zone = f[8].Get<uint32>();
                if (role.nodes.empty())
                    LOG_ERROR("module", "mod-dnd: townsfolk role '{}' has no dialogue", role.role);
                else
                    sRoles.push_back(std::move(role));
            } while (r->NextRow());
        }
        LOG_INFO("module", "mod-dnd: {} townsfolk roles", sRoles.size());
    }

    static int TownRoleOf(Creature* c)
    {
        if (!c || !c->IsInWorld() || c->GetMapId() != 0 || !c->GetSpawnId() || c->IsPet() || c->IsSummon())
            return -1;
        CreatureTemplate const* ct = c->GetCreatureTemplate();
        if (!ct || ct->type != CREATURE_TYPE_HUMANOID || c->GetScriptName() == "npc_dnd_combatant" ||
            c->GetScriptName() == "npc_dnd_portal")
            return -1;                       // (a portal keeper has their own menu)
        if (c->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE)) || c->IsHostileToPlayers())
            return -1;
        if (GetEncounterDefBySpawn(c->GetSpawnId()))
            return -1;
        uint32 zone = c->GetMap()->GetZoneId(c->GetPhaseMask(), c->GetPositionX(), c->GetPositionY(), c->GetPositionZ());
        if (!IsTownZone(zone))
            return -1;
        std::string who = c->GetName() + "|" + ct->SubName;
        std::transform(who.begin(), who.end(), who.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        std::string faction = "f" + std::to_string(c->GetFaction());
        for (std::size_t i = 0; i < sRoles.size(); ++i)
            for (std::string const& k : sRoles[i].keys)
            {
                if (sRoles[i].zone && sRoles[i].zone != zone)
                    break;                   // another town's own role
                if (k == "*" || k == faction)
                    return int(i);
                if (!IsFactionKey(k) && who.find(k) != std::string::npos)
                    return int(i);
            }
        return -1;
    }

    bool IsTownsfolk(Creature* c) { return TownRoleOf(c) >= 0; }
    static std::string TownGames(int role) { return role >= 0 && role < int(sRoles.size()) ? sRoles[role].games : ""; }

    void LoadTownProgress(Player* p)
    {
        auto& m = sTownDone[p->GetGUID()];
        m.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT spawn, result FROM dnd_character_townsfolk WHERE guid = {}",
            p->GetGUID().GetCounter()))
        {
            do
                m[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint8>();
            while (r->NextRow());
        }
    }

    void ResetTownsfolk(Player* p)
    {
        sTownDone[p->GetGUID()].clear();
        CharacterDatabase.Execute("DELETE FROM dnd_character_townsfolk WHERE guid = {}", p->GetGUID().GetCounter());
    }

    bool StartTownTalk(Player* p, Creature* c)
    {
        int role = TownRoleOf(c);
        if (role < 0 || !p || c->IsHostileTo(p) || !c->IsAlive() || InCombat(c))
            return false;
        if (InDialogue(p) || InCombat(p) || !p->IsAlive())
            return true;                     // swallow the click; nothing to open right now
        StopSneaking(p, "You step out of the shadows to talk.");
        TownRole const& r = sRoles[role];
        c->SetFacingToObject(p);
        auto& done = sTownDone[p->GetGUID()];
        if (done.count(c->GetSpawnId()))
        {
            c->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
            Log(p, Acore::StringFormat("|cffffd100{}:|r {}", c->GetName(), r.after));
            if (!r.games.empty())
                OpenTavern(p, c, r.games);   // already talked: straight to the table
            else if (c->IsVendor())
                p->GetSession()->SendListInventory(c->GetGUID());
            return true;
        }
        Talk& t = sTalks[p->GetGUID()];
        t = Talk();
        t.townSpawn = c->GetSpawnId();
        t.townRole = role;
        t.node = r.nodes[c->GetSpawnId() % r.nodes.size()];
        t.speaker = c->GetGUID();
        for (std::string const& f : TreatFlags(p))
            t.clues.insert("found:" + f);
        p->StopMoving();
        SetRooted(p, true);
        p->NearTeleportTo(p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), p->GetAngle(c));
        Send(p, "DC");
        Log(p, Acore::StringFormat("|cffc0c0ff--- {} ---|r", c->GetName()));
        ShowNode(p, t);
        return true;
    }

    static std::string Coins(uint32 m)
    {
        if (m >= 10000)
            return Acore::StringFormat("{}g {}s", m / 10000, m / 100 % 100);
        return m >= 100 ? Acore::StringFormat("{}s", m / 100) : Acore::StringFormat("{}c", m);
    }

    static void TownResult(Player* p, Creature* speaker, Talk const& t, bool won)
    {
        if (t.townRole < 0 || std::size_t(t.townRole) >= sRoles.size())
            return;
        TownRole const& r = sRoles[t.townRole];
        uint8 result = won ? 1 : 2;
        if (sTownDone[p->GetGUID()].count(t.townSpawn))
            return;                          // settled already (closed on its last line): never paid twice
        sTownDone[p->GetGUID()][t.townSpawn] = result;
        CharacterDatabase.Execute("REPLACE INTO dnd_character_townsfolk (guid, spawn, result) VALUES ({}, {}, {})",
            p->GetGUID().GetCounter(), t.townSpawn, result);
        if (!won)
            return;
        speaker->HandleEmoteCommand(EMOTE_ONESHOT_BOW);
        std::string reward;
        if (r.money)
        {
            p->ModifyMoney(int32(r.money));
            Log(p, "|cffffd100+" + Coins(r.money) + "|r");
            reward = Coins(r.money);
        }
        if (r.item)
        {
            p->AddItem(r.item, 1);
            if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(r.item))
                reward += (reward.empty() ? "" : "   |   ") + it->Name1;
        }
        SendCard(p, "town", speaker->GetName(), r.gift, reward);
    }
}
