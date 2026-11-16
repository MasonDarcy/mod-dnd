/*
 * mod-dnd treats: small discoveries off the beaten path (content: tools/treats_<zone>.py -> dnd_treat).
 *
 * Every treat has a "tell" gameobject that is always there (a broken cart, a pile of bones...). Two kinds:
 *   passive  walking within its radius rolls the treat's skill (Perception) once per character, like BG3's passive
 *            checks. A pass summons a glinting find beside the tell (plus a sparkle); clicking the find claims it.
 *            A failed roll shows in the log, quietly (TR with ok = 0), and the spot is spent for that character.
 *   active   clicking the tell rolls the skill once per character, with the big d20 (DR message).
 * Rewards: money, an item, and a flag that conversations see as the clue "found:<flag>".
 * Minigames (`game`, the addon's DnDGames.lua): instead of paying out on the spot, the treat opens a little hands-on
 * moment. Payoff games (geode, ice, dig, pry, pan, sift, shards, knots, mosaic) come after the find: the check still decides whether you find it, and the game
 * is the cracking open / digging out - they can't be lost. Puzzles (every other game) only open on a passed check (a
 * failed one: you can't make sense of it, and that's that); running out of tries in one is a failure too. A success
 * pays a little XP on top (TreatXp). A failed passive roll is shown, quietly (TR^...^0). The server sends
 * TG^id^game^level^nonce^item; the addon answers treatgame^id^nonce^1|0, checked against the nonce and the distance.
 * Per character: dnd_character_treat (1 missed, 2 found but not picked up, 3 claimed).
 */
#include "Dnd.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Random.h"
#include "StringFormat.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Dnd
{
    static uint32 const GLINT_ENTRY = 9100190;      // tools/make_treats_sql.py
    static uint32 const SPARKLE_ENTRY = 2722;        // stock "Floating Sparkles"
    enum : uint8 { TREAT_NONE = 0, TREAT_MISSED = 1, TREAT_FOUND = 2, TREAT_CLAIMED = 3 };

    struct TreatDef
    {
        uint32 id = 0, map = 0;
        float x = 0, y = 0, z = 0, o = 0;
        bool passive = false;
        std::string skill, req, freeFor, flag;
        int dc = 12;
        uint32 tellGuid = 0;
        float radius = 10.f;
        uint32 money = 0, item = 0, itemCount = 1;
        std::string look, found, success, fail, claim, freeText;
        std::string game;                    // "" none; a payoff game (PAYOFF_GAMES) or a puzzle (any other: DnDGames*.lua)
        std::string hint;                    // a flag (another treat's) that makes this one's puzzle easy: you know the song
        std::string grant;                   // a skill the find teaches: proficiency (the pick "skill:<name>")
        bool Puzzle() const
        {
            static char const* const PAYOFF_GAMES[] = { "geode", "ice", "dig", "pry", "pan", "sift", "shards",
                                                        "knots", "mosaic" };   // can't be lost
            if (game.empty())
                return false;
            for (char const* g : PAYOFF_GAMES)
                if (game == g)
                    return false;
            return true;
        }
    };
    struct PendingGame { uint32 id = 0, nonce = 0; };
    static std::unordered_map<ObjectGuid, PendingGame> sGame;                              // player -> the open minigame
    static std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint8>> sGameLevel;  // player -> treat -> 1 + level

    static std::unordered_map<uint32, TreatDef> sTreats;
    static std::unordered_map<uint32, uint32> sTellToTreat;                               // tell spawn guid -> treat
    static std::unordered_map<ObjectGuid, std::unordered_map<uint32, uint8>> sState;      // player -> treat -> state
    // a treat claimed by anyone is gone for everyone, for good (dnd_treat_taken). The self-test's bots keep their own
    // book: they see the world fresh, and what they take is gone only for other bots
    struct Taken { uint32 guid = 0; std::string name; };
    static std::unordered_map<uint32, Taken> sTaken, sTakenByBots;

    static std::unordered_map<uint32, Taken>& TakenBook(ObjectGuid g) { return Test::IsBot(g) ? sTakenByBots : sTaken; }

    // someone else has taken it
    static Taken const* GoneFor(Player const* p, uint32 id)
    {
        auto& book = TakenBook(p->GetGUID());
        auto itr = book.find(id);
        return itr != book.end() && itr->second.guid != p->GetGUID().GetCounter() ? &itr->second : nullptr;
    }

    static void SayGone(Player* p, Taken const& who)
    {
        Log(p, Acore::StringFormat("|cff909090Someone got here first - {} has already taken what was here.|r", who.name));
    }
    struct Glint { ObjectGuid find, sparkle; };
    static std::unordered_map<ObjectGuid, std::unordered_map<uint32, Glint>> sGlints;     // player -> treat -> objects
    static std::unordered_map<ObjectGuid, uint32> sTimer;

    void LoadTreats()
    {
        sTreats.clear();
        sTellToTreat.clear();
        sTaken.clear();
        if (QueryResult tr = CharacterDatabase.Query("SELECT treat_id, guid, name FROM dnd_treat_taken"))
            do
                sTaken[tr->Fetch()[0].Get<uint32>()] = { tr->Fetch()[1].Get<uint32>(), tr->Fetch()[2].Get<std::string>() };
            while (tr->NextRow());
        QueryResult r = WorldDatabase.Query("SELECT id, map, x, y, z, o, passive, skill, dc, req, free_for, tell_guid, radius, "
            "money, item, item_count, flag, text_look, text_found, text_success, text_fail, text_claim, text_free, game, hint, grant_skill FROM dnd_treat");
        if (!r)
            return;
        do
        {
            Field* f = r->Fetch();
            TreatDef t;
            t.id = f[0].Get<uint32>();
            t.map = f[1].Get<uint32>();
            t.x = f[2].Get<float>(); t.y = f[3].Get<float>(); t.z = f[4].Get<float>(); t.o = f[5].Get<float>();
            t.passive = f[6].Get<int32>() != 0;
            t.skill = f[7].Get<std::string>();
            t.dc = f[8].Get<int32>();
            t.req = f[9].Get<std::string>();
            t.freeFor = f[10].Get<std::string>();
            t.tellGuid = f[11].Get<uint32>();
            t.radius = f[12].Get<float>();
            t.money = f[13].Get<uint32>();
            t.item = f[14].Get<uint32>();
            t.itemCount = f[15].Get<uint32>();
            t.flag = f[16].Get<std::string>();
            t.look = f[17].Get<std::string>();
            t.found = f[18].Get<std::string>();
            t.success = f[19].Get<std::string>();
            t.fail = f[20].Get<std::string>();
            t.claim = f[21].Get<std::string>();
            t.freeText = f[22].Get<std::string>();
            t.game = f[23].Get<std::string>();
            t.hint = f[24].Get<std::string>();
            t.grant = f[25].Get<std::string>();
            if (t.tellGuid)
                sTellToTreat[t.tellGuid] = t.id;
            sTreats[t.id] = std::move(t);
        } while (r->NextRow());
    }

    static uint8 StateOf(Player* p, uint32 id)
    {
        auto itr = sState.find(p->GetGUID());
        if (itr == sState.end())
            return TREAT_NONE;
        auto s = itr->second.find(id);
        return s == itr->second.end() ? TREAT_NONE : s->second;
    }

    // refresh whether this character can see the treat's tell (it goes once they've claimed it)
    static void RefreshTell(Player* p, TreatDef const& t)
    {
        if (!t.tellGuid || !p->IsInWorld() || p->GetMapId() != t.map)
            return;
        auto range = p->GetMap()->GetGameObjectBySpawnIdStore().equal_range(t.tellGuid);
        for (auto itr = range.first; itr != range.second; ++itr)
            p->UpdateVisibilityOf(itr->second);
    }

    static void SetState(Player* p, uint32 id, uint8 state)
    {
        sState[p->GetGUID()][id] = state;
        CharacterDatabase.Execute("REPLACE INTO dnd_character_treat (guid, treat_id, state) VALUES ({}, {}, {})",
            p->GetGUID().GetCounter(), id, state);
        if (state == TREAT_CLAIMED)
            if (auto t = sTreats.find(id); t != sTreats.end())
                RefreshTell(p, t->second);
    }

    bool TreatTellHiddenFor(Player const* p, GameObject const* go)
    {
        auto itr = sTellToTreat.find(go->GetSpawnId());
        if (!p || itr == sTellToTreat.end())
            return false;
        if (GoneFor(p, itr->second))
            return true;                         // someone else took it
        auto ps = sState.find(p->GetGUID());
        if (ps == sState.end())
            return false;
        auto s = ps->second.find(itr->second);
        return s != ps->second.end() && s->second == TREAT_CLAIMED;
    }

    static void DespawnGlint(Player* p, uint32 id)
    {
        auto pit = sGlints.find(p->GetGUID());
        if (pit == sGlints.end())
            return;
        auto g = pit->second.find(id);
        if (g == pit->second.end())
            return;
        for (ObjectGuid guid : { g->second.find, g->second.sparkle })
            if (GameObject* go = ObjectAccessor::GetGameObject(*p, guid))
                go->DespawnOrUnsummon();
        pit->second.erase(g);
    }

    void LoadTreatProgress(Player* p)
    {
        auto& m = sState[p->GetGUID()];
        m.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT treat_id, state FROM dnd_character_treat WHERE guid = {}",
            p->GetGUID().GetCounter()))
        {
            do
                m[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint8>();
            while (r->NextRow());
        }
    }

    void ResetTreats(Player* p)
    {
        for (auto const& [id, t] : sTreats)
            DespawnGlint(p, id);
        sState[p->GetGUID()].clear();
        sGame.erase(p->GetGUID());
        sGameLevel.erase(p->GetGUID());
        CharacterDatabase.Execute("DELETE FROM dnd_character_treat WHERE guid = {}", p->GetGUID().GetCounter());
        auto& book = TakenBook(p->GetGUID());
        for (auto itr = book.begin(); itr != book.end();)
            itr = itr->second.guid == p->GetGUID().GetCounter() ? book.erase(itr) : std::next(itr);
        if (!Test::IsBot(p->GetGUID()))
            CharacterDatabase.Execute("DELETE FROM dnd_treat_taken WHERE guid = {}", p->GetGUID().GetCounter());
        for (auto const& [id, t] : sTreats)
            RefreshTell(p, t);                   // the tells come back
    }

    void ForgetTreats(ObjectGuid guid)
    {
        if (Test::IsBot(guid))
            for (auto itr = sTakenByBots.begin(); itr != sTakenByBots.end();)
                itr = itr->second.guid == guid.GetCounter() ? sTakenByBots.erase(itr) : std::next(itr);
        sState.erase(guid);
        sGlints.erase(guid);
        sTimer.erase(guid);
        sGame.erase(guid);
        sGameLevel.erase(guid);
    }

    // what's been found: in one shared world, a clue anyone found counts for everyone (a bot: only its own)
    std::vector<std::string> TreatFlags(Player* p)
    {
        std::vector<std::string> out;
        auto const& book = TakenBook(p->GetGUID());
        for (auto const& [id, t] : sTreats)
            if (!t.flag.empty() && (StateOf(p, id) == TREAT_CLAIMED || (!Test::IsBot(p->GetGUID()) && book.count(id))))
                out.push_back(t.flag);
        return out;
    }

    // the glinting find: a little beyond the tell, toward where the player stands, with a sparkle over it
    static void SpawnGlint(Player* p, TreatDef const& t)
    {
        auto& g = sGlints[p->GetGUID()][t.id];
        if (ObjectAccessor::GetGameObject(*p, g.find))
            return;
        float ang = std::atan2(p->GetPositionY() - t.y, p->GetPositionX() - t.x);
        float x = t.x + std::cos(ang) * 1.6f, y = t.y + std::sin(ang) * 1.6f, z = t.z;
        p->UpdateGroundPositionZ(x, y, z);
        if (GameObject* go = p->SummonGameObject(GLINT_ENTRY, x, y, z + 0.05f, ang, 0, 0, std::sin(ang / 2), std::cos(ang / 2), 3600))
            g.find = go->GetGUID();
        if (GameObject* sp = p->SummonGameObject(SPARKLE_ENTRY, x, y, z + 0.3f, 0, 0, 0, 0, 1, 3600))
            g.sparkle = sp->GetGUID();
    }

    // the addon's centre card: TB (begin), TT^<text chunk>..., TE^<kind>^<title>^<reward line>
    //   kind: found (a passive find picked up), success / fail (an active roll), known (no roll needed)
    void SendCard(Player* p, std::string const& kind, std::string const& title, std::string const& text,
        std::string const& reward)
    {
        Send(p, "TB");
        for (std::size_t pos = 0; pos < text.size();)
        {
            std::size_t len = std::min<std::size_t>(200, text.size() - pos);
            if (pos + len < text.size())
            {
                std::size_t sp = text.rfind(' ', pos + len);
                if (sp != std::string::npos && sp > pos)
                    len = sp - pos + 1;
            }
            Send(p, "TT^" + text.substr(pos, len));
            pos += len;
        }
        Send(p, "TE^" + kind + "^" + title + "^" + reward);
    }

    static std::string MoneyText(uint32 m)
    {
        if (m >= 10000)
            return Acore::StringFormat("{}g {}s", m / 10000, m / 100 % 100);
        return m >= 100 ? Acore::StringFormat("{}s", m / 100) : Acore::StringFormat("{}c", m);
    }

    // a little XP for a find: harder checks pay more (DC 12: 3, DC 14: 4, DC 16: 5)
    static uint32 TreatXp(TreatDef const& t) { return uint32(std::max(2, 2 + (t.dc - 10) / 2)); }

    static void TakeForEveryone(Player* p, TreatDef const& t);

    static std::string TellName(TreatDef const& t);

    static void Reward(Player* p, TreatDef const& t, std::string const& text, std::string const& kind, std::string const& title)
    {
        if (Taken const* who = GoneFor(p, t.id))
        {
            DespawnGlint(p, t.id);
            SayGone(p, *who);
            return;
        }
        SetState(p, t.id, TREAT_CLAIMED);
        DespawnGlint(p, t.id);
        TakeForEveryone(p, t);
        // one find for the whole party: the finder gets everyone's share of the goods (players within 30 m) to hand out
        uint32 party = 1;
        p->GetMap()->DoForAllPlayers([&](Player* o)
        {
            if (o != p && !o->IsGameMaster() && Test::IsBot(o->GetGUID()) == Test::IsBot(p->GetGUID()) && o->GetExactDist(p) <= 30.f)
                ++party;
        });
        uint32 money = t.money * party, count = t.itemCount * party;
        if (!text.empty())
            LogFind(p, "|cffe8d8a8" + text + "|r");
        uint32 xp = TreatXp(t);
        p->GiveXP(xp, nullptr);
        Log(p, Acore::StringFormat("|cff40ff40+{} XP|r", xp));
        std::vector<std::string> parts;
        if (money)
        {
            p->ModifyMoney(int32(money));
            Log(p, "|cffffd100+" + MoneyText(money) + "|r");
            parts.push_back(MoneyText(money));
        }
        if (t.item)
        {
            p->AddItem(t.item, count);
            if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(t.item))
                parts.push_back(count > 1 ? Acore::StringFormat("{} x{}", it->Name1, count) : it->Name1);
        }
        if (party > 1 && (t.money || t.item))
        {
            Log(p, Acore::StringFormat("|cffffd100A share for each of the {} of you - hand them out.|r", party));
            parts.push_back(Acore::StringFormat("{} shares", party));
        }
        if (!t.flag.empty())
            parts.push_back("something worth remembering");
        if (!t.grant.empty())
        {
            // what you found here taught you something: proficiency in a skill, for good
            Sheet& sh = GetSheet(p);
            std::string pick = "skill:" + t.grant;
            if (std::find(sh.picks.begin(), sh.picks.end(), pick) == sh.picks.end())
            {
                sh.picks.push_back(pick);
                SaveSheet(p);
                PushSkills(p);
                PushSheet(p);
            }
            Log(p, "|cffffd100You are now proficient in " + t.grant + ".|r");
            parts.push_back("Proficiency: " + t.grant);
        }
        {
            std::string what = TellName(t);
            std::vector<std::string> got;
            if (t.item)
                if (ItemTemplate const* it = sObjectMgr->GetItemTemplate(t.item))
                    got.push_back(count > 1 ? Acore::StringFormat("{} x{}", it->Name1, count) : it->Name1);
            if (money)
                got.push_back(MoneyText(money));
            if (!t.grant.empty())
                got.push_back("a knack for " + t.grant);
            if (got.empty())
                got.push_back("something worth remembering");
            std::string list;
            for (std::size_t i = 0; i < got.size(); ++i)
                list += (i == 0 ? "" : (i + 1 == got.size() ? " and " : ", ")) + got[i];
            Announce(p, Acore::StringFormat("{} searched {} in {} and came away with {}.", p->GetName(), what, ZoneName(p), list));
            if (!Test::IsBot(p->GetGUID()))
                StoryLog(p->GetName(), "find", Acore::StringFormat("Searched {} in {} and came away with {}.", what, ZoneName(p), list));
        }
        parts.push_back(Acore::StringFormat("{} XP", xp));
        std::string reward;
        for (std::string const& s : parts)
            reward += (reward.empty() ? "" : "   |   ") + s;
        SendCard(p, kind, title, text, reward);
    }

    // the find is gone for everyone else: the book, and whoever has a glint or a game open for it loses it
    static void TakeForEveryone(Player* p, TreatDef const& t)
    {
        TakenBook(p->GetGUID())[t.id] = { p->GetGUID().GetCounter(), p->GetName() };
        if (!Test::IsBot(p->GetGUID()))
            CharacterDatabase.Execute("REPLACE INTO dnd_treat_taken (treat_id, guid, name) VALUES ({}, {}, '{}')", t.id,
                p->GetGUID().GetCounter(), p->GetName());
        std::vector<ObjectGuid> others;
        for (auto const& [g, m] : sGlints)
            if (g != p->GetGUID() && m.count(t.id))
                others.push_back(g);
        for (ObjectGuid g : others)
            if (Player* o = ObjectAccessor::FindPlayer(g))
                DespawnGlint(o, t.id);
        for (auto itr = sGame.begin(); itr != sGame.end();)
            itr = itr->first != p->GetGUID() && itr->second.id == t.id ? sGame.erase(itr) : std::next(itr);
        p->GetMap()->DoForAllPlayers([&](Player* o) { if (o != p) RefreshTell(o, t); });
    }

    uint32 ReturnAllTreats()
    {
        uint32 n = uint32(sTaken.size());
        sTaken.clear();
        CharacterDatabase.Execute("DELETE FROM dnd_treat_taken");
        for (auto const& [g, m] : sState)
            if (Player* o = ObjectAccessor::FindPlayer(g))
                for (auto const& [id, t] : sTreats)
                    RefreshTell(o, t);           // the tells come back for whoever is about
        return n;
    }

    // open the treat's minigame in the addon (level: 0 easy, 1 hard)
    // a find by its tell: "the weathered bones", "the broken crate" ("something hidden" without one)
    static std::string TellName(TreatDef const& t)
    {
        if (GameObjectData const* gd = t.tellGuid ? sObjectMgr->GetGameObjectData(t.tellGuid) : nullptr)
            if (GameObjectTemplate const* gt = sObjectMgr->GetGameObjectTemplate(gd->id); gt && !gt->name.empty())
            {
                std::string what = gt->name;
                std::transform(what.begin(), what.end(), what.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                return "the " + what;
            }
        return "something hidden";
    }

    // the players near a find's minigame hear about it: who's at it, and how it went (the game itself is the player's,
    // in their addon - nobody else sees it). Real players only: a bot's game tells nobody, and nobody tells a bot
    static void TellNearby(Player* p, TreatDef const& t, std::string const& line)
    {
        p->GetMap()->DoForAllPlayers([&](Player* o)
        {
            if (o != p && o->IsInWorld() && Test::IsBot(o->GetGUID()) == Test::IsBot(p->GetGUID()) &&
                o->GetDistance(t.x, t.y, t.z) <= 30.f)
                Log(o, "|cffc8b890" + line + "|r");
        });
    }

    static void OpenGame(Player* p, TreatDef const& t, uint8 level)
    {
        TellNearby(p, t, Acore::StringFormat("{} sets to work on {}...", p->GetName(), TellName(t)));
        PendingGame& g = sGame[p->GetGUID()];
        g.id = t.id;
        g.nonce = urand(1, 0x7FFFFFFF);
        sGameLevel[p->GetGUID()][t.id] = uint8(1 + level);
        Send(p, Acore::StringFormat("TG^{}^{}^{}^{}^{}", t.id, t.game, level, g.nonce, t.item));
    }

    static uint8 PuzzleLevel(Player* p, TreatDef const& t)
    {
        if (!t.Puzzle())
            return 0;
        if (t.game == "song" || !t.hint.empty())
        {
            for (std::string const& f : TreatFlags(p))
                if (f == t.hint)
                    return 0;
            return 1;
        }
        return t.dc >= 14 ? 1 : 0;
    }

    static void Missed(Player* p, TreatDef const& t)
    {
        SetState(p, t.id, TREAT_MISSED);
        sGameLevel[p->GetGUID()].erase(t.id);
        Log(p, "|cffc8b890" + t.fail + "|r");
        SendCard(p, "fail", t.skill, t.fail, "");
    }

    // the addon's answer: treatgame^id^nonce^1 (done) or ^0 (out of tries)
    void TreatGameResult(Player* p, std::string const& args)
    {
        uint32 id = 0, nonce = 0;
        int won = 0;
        if (std::sscanf(args.c_str(), "%u^%u^%d", &id, &nonce, &won) != 3)
            return;
        auto pg = sGame.find(p->GetGUID());
        auto ti = sTreats.find(id);
        if (pg == sGame.end() || pg->second.id != id || pg->second.nonce != nonce || ti == sTreats.end())
            return;
        sGame.erase(pg);
        TreatDef const& t = ti->second;
        if (Taken const* who = GoneFor(p, id))
        {
            SayGone(p, *who);
            return;
        }
        uint8 state = StateOf(p, id);
        if (state == TREAT_CLAIMED || (state == TREAT_MISSED && !t.passive) || p->GetMapId() != t.map ||
            p->GetDistance(t.x, t.y, t.z) > 25.f || InCombat(p))
            return;
        sGameLevel[p->GetGUID()].erase(id);
        TellNearby(p, t, won ? Acore::StringFormat("...and {} has it - {} gives up what it was hiding.", p->GetName(), TellName(t))
                             : Acore::StringFormat("...but {} gives up on {}.", p->GetName(), TellName(t)));
        if (won)
            Reward(p, t, t.passive ? t.claim : t.success, t.passive ? "found" : "success", t.passive ? "A Discovery" : t.skill);
        else if (t.Puzzle())
            Missed(p, t);
    }

    // an active treat: the tell was clicked
    static void Attempt(Player* p, TreatDef const& t)
    {
        if (Taken const* who = GoneFor(p, t.id))
        {
            SayGone(p, *who);
            return;
        }
        uint8 state = StateOf(p, t.id);
        if (state == TREAT_CLAIMED || state == TREAT_MISSED)
        {
            Log(p, "|cff909090You've already searched here.|r");
            return;
        }
        if (!t.req.empty() && !MeetsRequirement(p, t.req))
        {
            if (!t.look.empty())
                Log(p, "|cffc8b890" + t.look + "|r");
            return;
        }
        bool free = !t.freeFor.empty() && MeetsRequirement(p, t.freeFor);
        if (!t.game.empty())
        {
            // already rolled for this one (the game was closed): straight back in, same difficulty
            auto& levels = sGameLevel[p->GetGUID()];
            if (auto lv = levels.find(t.id); lv != levels.end())
            {
                OpenGame(p, t, uint8(lv->second - 1));
                return;
            }
            if (free)
            {
                if (!t.freeText.empty())
                    Log(p, "|cffe8d8a8" + t.freeText + "|r");
                OpenGame(p, t, PuzzleLevel(p, t));
                return;
            }
        }
        else if (free)
        {
            Reward(p, t, (t.freeText.empty() ? "" : t.freeText + " ") + t.success, "known", "You Know This");
            return;
        }
        int bonus = SkillBonusByName(p, t.skill);
        D20Result r = RollD20(bonus, ItemSkillAdvantage(p, t.skill) ? 1 : 0);
        bool ok = r.natural == 20 || (r.natural != 1 && r.total >= t.dc);
        Log(p, Acore::StringFormat("   {} check (DC {}): {} - {}", t.skill, t.dc, r.text, ok ? "|cff40ff40SUCCESS|r" : "|cffff4040FAILURE|r"));
        // TD, not DR: the same tumbling d20, but nothing for a conversation panel to pick up as its last roll
        Send(p, Acore::StringFormat("TD^{}^{}^{}^{}^{}", t.skill, r.natural, bonus, t.dc, ok ? 1 : 0));
        if (ok && !t.game.empty())
            OpenGame(p, t, PuzzleLevel(p, t));   // found it: now crack it open / dig it out / work the puzzle
        else if (ok)
            Reward(p, t, t.success, "success", t.skill);
        else
            Missed(p, t);
    }

    // the self-test jig: a treat at a bot's feet, and a click on its tell
    void AddTestTreat(uint32 id, Player* at, std::string const& game, int dc, uint32 money, bool passive)
    {
        TreatDef t;
        t.id = id;
        t.passive = passive;
        t.radius = 6.f;
        t.claim = "The test find, picked up.";
        t.map = at->GetMapId();
        t.x = at->GetPositionX(); t.y = at->GetPositionY(); t.z = at->GetPositionZ();
        t.skill = "Perception";
        t.dc = dc;
        t.money = money;
        t.game = game;
        t.success = "The test treat opens.";
        t.fail = "The test treat stays shut.";
        sTreats[id] = t;
    }

    void RemoveTestTreat(uint32 id) { sTreats.erase(id); }

    void SetTestTreat(uint32 id, std::string const& flag, std::string const& hint, std::string const& grant, uint32 item)
    {
        auto itr = sTreats.find(id);
        if (itr == sTreats.end())
            return;
        itr->second.flag = flag;
        itr->second.hint = hint;
        itr->second.grant = grant;
        itr->second.item = item;
    }

    void UseTestTreat(Player* p, uint32 id)
    {
        auto itr = sTreats.find(id);
        if (itr != sTreats.end())
            Attempt(p, itr->second);
    }

    // passive treats: walking close rolls once; a found-but-unclaimed glint comes back after a relog
    void UpdateTreats(Player* p, uint32 diff)
    {
        uint32& timer = sTimer[p->GetGUID()];
        timer += diff;
        if (timer < 500)
            return;
        timer = 0;
        if (sTreats.empty() || !p->IsAlive() || p->IsGameMaster() || InCombat(p) || InDialogue(p) || p->IsInFlight())
            return;                              // (nothing is noticed from a gryphon's back)
        for (auto const& [id, t] : sTreats)
        {
            if (!t.passive || t.map != p->GetMapId())
                continue;
            float d = p->GetDistance(t.x, t.y, t.z);
            if (d > 40.f || GoneFor(p, id))
                continue;
            uint8 state = StateOf(p, id);
            if (state == TREAT_FOUND)
            {
                SpawnGlint(p, t);
                continue;
            }
            if (state != TREAT_NONE || d > t.radius || !p->IsWithinLOS(t.x, t.y, t.z + 1.f))
                continue;
            if (!t.req.empty() && !MeetsRequirement(p, t.req))
                continue;
            bool free = !t.freeFor.empty() && MeetsRequirement(p, t.freeFor);
            int bonus = SkillBonusByName(p, t.skill) + (t.skill == "Perception" ? WeatherPerception(p) : 0);   // fog
            D20Result r = RollD20(bonus, ItemSkillAdvantage(p, t.skill) ? 1 : 0);
            bool ok = free || r.natural == 20 || (r.natural != 1 && r.total >= t.dc);
            if (!ok)
            {
                // you never saw it - but the roll is shown, quietly, so you know you missed something here
                SetState(p, id, TREAT_MISSED);
                Log(p, Acore::StringFormat("   |cff909090{} (DC ?): {} - nothing catches your eye.|r", t.skill, r.text));
                Send(p, Acore::StringFormat("TR^{}^{}^{}^0^0", t.skill, r.natural, bonus));
                continue;
            }
            SetState(p, id, TREAT_FOUND);
            Log(p, Acore::StringFormat("   |cff9ad0ff{} (DC {}): {} - you notice something.|r", t.skill, t.dc, free ? "automatic" : r.text));
            if (!t.found.empty())
                Log(p, "|cffe8d8a8" + t.found + "|r");
            Send(p, Acore::StringFormat("TR^{}^{}^{}^{}^1", t.skill, free ? 0 : r.natural, bonus, t.dc));
            SpawnGlint(p, t);
        }
    }

    static void OnTreatObjectUsed(Player* p, GameObject* go)
    {
        if (go->GetEntry() == GLINT_ENTRY)
        {
            auto pit = sGlints.find(p->GetGUID());
            if (pit != sGlints.end())
                for (auto const& [id, g] : pit->second)
                    if (g.find == go->GetGUID())
                    {
                        auto t = sTreats.find(id);
                        if (t != sTreats.end() && GoneFor(p, id))
                        {
                            SayGone(p, *GoneFor(p, id));
                            DespawnGlint(p, id);
                            return;
                        }
                        if (t != sTreats.end() && !t->second.game.empty())
                            OpenGame(p, t->second, PuzzleLevel(p, t->second));
                        else if (t != sTreats.end())
                            Reward(p, t->second, t->second.claim, "found", "A Discovery");
                        return;
                    }
            return;
        }
        auto itr = sTellToTreat.find(go->GetSpawnId());
        if (itr == sTellToTreat.end())
            return;
        TreatDef const& t = sTreats[itr->second];
        if (t.passive)
        {
            // the tell itself only describes; what's hidden shows up beside it once noticed
            if (!t.look.empty())
                Log(p, "|cffc8b890" + t.look + "|r");
            if (StateOf(p, t.id) == TREAT_FOUND)
                SpawnGlint(p, t);
            else if (StateOf(p, t.id) == TREAT_MISSED)
                Log(p, "|cff909090You looked this over when you came by, and nothing caught your eye.|r");
            return;
        }
        Attempt(p, t);
    }
}

// the tell's AI: only hides it from whoever has already claimed what it marked
struct go_dnd_treat_ai : public GameObjectAI
{
    explicit go_dnd_treat_ai(GameObject* go) : GameObjectAI(go) { }
    bool CanBeSeen(Player const* seer) override { return !Dnd::TreatTellHiddenFor(seer, me); }
};

class go_dnd_treat : public GameObjectScript
{
public:
    go_dnd_treat() : GameObjectScript("go_dnd_treat") { }

    GameObjectAI* GetAI(GameObject* go) const override { return new go_dnd_treat_ai(go); }

    bool OnGossipHello(Player* player, GameObject* go) override
    {
        if (player && go && !Dnd::InCombat(player))
            Dnd::OnTreatObjectUsed(player, go);
        return true;
    }
};

void AddSC_dnd_treats()
{
    new go_dnd_treat();
}
