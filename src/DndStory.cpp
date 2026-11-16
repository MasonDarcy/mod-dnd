/*
 * mod-dnd story: what carries over from one encounter to another - one shared world for every character on the server.
 * The content is tools/story.py (world: dnd_story_*); the state is the server's (characters: dnd_story_state, one row a
 * flag: story flags, "rep:<faction>" standing, "lean", and the engine's own "@resolved:<encounter>", "@node:<id>").
 *
 *   Effects  (a conversation line's `story`, an encounter's outcome): "flag", "flag=3", "!flag", "rep:<faction>+2",
 *            "lean-1" - comma-separated
 *   Conditions (a dialogue option's need, the story tables): "story:flag", "!story:flag", "story:flag>=2",
 *            "rep:<faction>>=3", "lean<=-2", "resolved:<encounter>" - comma-separated, all must hold
 *
 * An encounter's outcome (peace / sour / fight) and a line's effects apply once, the first time anyone gets there. An
 * encounter ABSENT by the story is gone (its creatures phased away) and counts as resolved for every character (its XP
 * paid). WORLD spawns show or hide with their conditions (phase 1 << 30 hides them: no player is ever in it). A change
 * that brings a new Journal entry into view tells everyone online.
 *
 * Addon messages (the Journal, DnDJournal.lua): YO (it follows), YF^id^name^standing^about, YL^lean^ruthless^merciful,
 * YT^thread^title, YE^thread^text / Y+^more text, YD (done), YN^thread title (a new entry: a toast).
 * The Chronicle (one shared record): YK (it follows), YC^time^who^kind^text (an entry; live, as it happens).
 * The addon sends "journal" (the lot again).
 */
#include "Dnd.h"

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include <deque>
#include <map>

namespace Dnd
{
    namespace
    {
        uint32 const PHASE_STORY_HIDDEN = 1u << 30;

        struct Faction { std::string id, name, about; };
        struct Entry { std::string cond, text; };
        struct Thread { std::string id, title; std::vector<Entry> entries; };
        struct Rumor { uint32 zone; std::string cond, text; };

        std::vector<Faction> sFactions;
        std::vector<Thread> sThreads;
        std::map<std::pair<uint32, std::string>, std::string> sOutcomes;
        std::map<uint32, std::vector<std::pair<std::string, uint32>>> sStarts;
        std::map<uint32, std::string> sAbsent;
        std::unordered_map<uint32, std::string> sStorySpawns;      // creature guid -> the condition to be there
        std::vector<Rumor> sRumors;
        std::map<std::string, int> sState;
        std::string sLeanNeg = "Ruthless", sLeanPos = "Merciful";
        struct LogEntry { uint32 at; std::string who, kind, text; };
        std::deque<LogEntry> sChronicle;                       // the Chronicle's last entries (oldest first)
        std::size_t const LOG_KEEP = 150;

        std::string LogLine(LogEntry const& e)
        {
            std::string text = e.text.substr(0, 180);
            for (char& ch : text)
                if (ch == '^')
                    ch = ' ';
            return Acore::StringFormat("YC^{}^{}^{}^{}", e.at, e.who, e.kind, text);
        }

        std::vector<std::string> Split(std::string const& s, char sep)
        {
            std::vector<std::string> out;
            for (std::string_view v : Acore::Tokenize(s, sep, false))
            {
                std::string t(v);
                t.erase(0, t.find_first_not_of(' '));
                t.erase(t.find_last_not_of(' ') + 1);
                if (!t.empty())
                    out.push_back(t);
            }
            return out;
        }

        void Store(std::string const& key, int value)
        {
            if (value)
                sState[key] = value;
            else
                sState.erase(key);
            std::string k = key;
            CharacterDatabase.EscapeString(k);
            if (value)
                CharacterDatabase.Execute("REPLACE INTO dnd_story_state (flag, value, at) VALUES ('{}', {}, UNIX_TIMESTAMP())", k, value);
            else
                CharacterDatabase.Execute("DELETE FROM dnd_story_state WHERE flag = '{}'", k);
        }

        // a comparison: "name>=3" / "name<=-2" / "name" (>= 1)
        bool Compare(std::string const& expr, std::string const& prefix)
        {
            std::string body = expr.substr(prefix.size());
            std::size_t ge = body.find(">="), le = body.find("<=");
            if (ge != std::string::npos)
                return StoryValue(prefix + body.substr(0, ge)) >= std::atoi(body.c_str() + ge + 2);
            if (le != std::string::npos)
                return StoryValue(prefix + body.substr(0, le)) <= std::atoi(body.c_str() + le + 2);
            return StoryValue(prefix + body) >= 1;
        }

        std::set<std::string> VisibleEntries()
        {
            std::set<std::string> out;
            for (Thread const& t : sThreads)
                for (std::size_t i = 0; i < t.entries.size(); ++i)
                    if (StoryConds(t.entries[i].cond))
                        out.insert(t.id + "#" + std::to_string(i));
            return out;
        }

        void SendLongTo(Player* p, std::string const& first, std::string const& text)
        {
            std::size_t pos = 0;
            bool start = true;
            while (pos < text.size() || start)
            {
                std::size_t len = std::min<std::size_t>(200, text.size() - pos);
                if (pos + len < text.size())
                    if (std::size_t sp = text.rfind(' ', pos + len); sp != std::string::npos && sp > pos)
                        len = sp - pos + 1;
                Send(p, (start ? first : std::string("Y+^")) + text.substr(pos, len));
                pos += len;
                start = false;
            }
        }

        // the story's spawns and the absent encounters' creatures, shown or hidden to match the story now
        void PhaseCreature(Creature* c)
        {
            if (!c || !c->GetSpawnId())
                return;
            bool known = false, there = true;
            if (auto w = sStorySpawns.find(c->GetSpawnId()); w != sStorySpawns.end())
            {
                known = true;
                there = StoryConds(w->second);
            }
            if (EncounterDef const* def = GetEncounterDefBySpawn(c->GetSpawnId()))
                if (StoryAbsent(def->id))
                {
                    known = true;
                    there = false;
                }
            if (!known)
                return;
            uint32 want = there ? PHASEMASK_NORMAL : PHASE_STORY_HIDDEN;
            if (c->GetPhaseMask() != want)
                c->SetPhaseMask(want, true);
        }

        void PhaseSpawn(uint32 guid)
        {
            CreatureData const* d = sObjectMgr->GetCreatureData(guid);
            if (!d)
                return;
            Map* m = sMapMgr->FindBaseMap(d->mapid);
            if (!m)
                return;
            auto range = m->GetCreatureBySpawnIdStore().equal_range(guid);
            for (auto itr = range.first; itr != range.second; ++itr)
                PhaseCreature(itr->second);
        }

        void ApplyWorld()
        {
            for (auto const& [guid, cond] : sStorySpawns)
                PhaseSpawn(guid);
            for (auto const& [enc, cond] : sAbsent)
                if (EncounterDef const* def = GetEncounterDef(enc))
                    for (uint32 spawn : def->spawns)
                        PhaseSpawn(spawn);
        }
    }

    int StoryValue(std::string const& key)
    {
        auto itr = sState.find(key);
        return itr == sState.end() ? 0 : itr->second;
    }

    bool StoryCond(std::string const& raw)
    {
        std::string c = raw;
        bool neg = !c.empty() && c[0] == '!';
        if (neg)
            c.erase(0, 1);
        bool v;
        if (c.rfind("story:", 0) == 0)
            v = Compare(c, "story:") || false;
        else if (c.rfind("rep:", 0) == 0)
            v = Compare(c, "rep:");
        else if (c.rfind("lean", 0) == 0)
        {
            std::string body = c.substr(4);
            int lean = StoryValue("lean");
            v = body.rfind(">=", 0) == 0 ? lean >= std::atoi(body.c_str() + 2) : (body.rfind("<=", 0) == 0 ? lean <= std::atoi(body.c_str() + 2) : lean != 0);
        }
        else if (c.rfind("resolved:", 0) == 0)
            v = StoryValue("@resolved:" + c.substr(9)) > 0;
        else if (c.rfind("found:", 0) == 0)
            v = TreatFlagTaken(c.substr(6));     // (the Journal's, a rumour's, a spawn's: anyone took it. A conversation
                                                 // asks the clues instead - IsStoryCond says no - which counts a bot's own)
        else
            return true;                         // (not the story's: a clue or a find - the conversation decides those)
        return v != neg;
    }

    bool StoryConds(std::string const& list)
    {
        for (std::string const& c : Split(list, ','))
            if (!StoryCond(c))
                return false;
        return true;
    }

    bool IsStoryCond(std::string const& c)
    {
        std::string b = !c.empty() && c[0] == '!' ? c.substr(1) : c;
        return b.rfind("story:", 0) == 0 || b.rfind("rep:", 0) == 0 || b.rfind("lean", 0) == 0 || b.rfind("resolved:", 0) == 0;
    }

    // (the story key behind a flag: "story:" flags are stored by their bare name)
    void StoryApply(std::string const& effects, Player* by)
    {
        if (effects.empty())
            return;
        std::set<std::string> before = VisibleEntries();
        std::set<uint32> wasAbsent;
        for (auto const& [enc, cond] : sAbsent)
            if (StoryConds(cond))
                wasAbsent.insert(enc);
        for (std::string const& e : Split(effects, ','))
        {
            if (e.empty() || e[0] == '@')
                continue;                        // ("@find": nothing to set - only see what follows from it)
            if (e.rfind("rep:", 0) == 0)
            {
                std::size_t at = e.find_first_of("+-", 4);
                if (at == std::string::npos)
                    continue;
                std::string fac = e.substr(4, at - 4);
                int delta = std::atoi(e.c_str() + at);
                int now = std::clamp(StoryValue("rep:" + fac) + delta, -10, 10);
                Store("rep:" + fac, now);
                for (Faction const& f : sFactions)
                    if (f.id == fac)
                    {
                        AnnounceToAll(Acore::StringFormat("|cffffd100Your standing with {} {}.|r", f.name, delta > 0 ? "rises" : "falls"));
                        StoryLog(by ? by->GetName() : "", "standing", Acore::StringFormat("Standing with {} {} ({:+d}).", f.name,
                            delta > 0 ? "rose" : "fell", now));
                    }
            }
            else if (e.rfind("lean", 0) == 0 && (e.size() > 4 && (e[4] == '+' || e[4] == '-')))
                Store("lean", std::clamp(StoryValue("lean") + std::atoi(e.c_str() + 4), -10, 10));
            else if (e[0] == '!')
                Store("story:" + e.substr(1), 0);
            else if (std::size_t eq = e.find('='); eq != std::string::npos)
                Store("story:" + e.substr(0, eq), std::atoi(e.c_str() + eq + 1));
            else
                Store("story:" + e, 1);
        }
        if (effects[0] != '@')
            FileLog(Acore::StringFormat("story: {} ({})", effects, by ? by->GetName() : std::string("the world")));
        ApplyWorld();
        // an encounter the story just took away: resolved for everyone online (the rest when they next log in)
        for (auto const& [enc, cond] : sAbsent)
            if (!wasAbsent.count(enc) && StoryConds(cond))
                for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
                    if (p && p->IsInWorld())
                        CreditAbsent(p);
        std::set<std::string> after = VisibleEntries();
        std::set<std::string> told;
        for (std::string const& key : after)
            if (!before.count(key))
                for (Thread const& t : sThreads)
                    if (key.rfind(t.id + "#", 0) == 0)
                    {
                        std::size_t idx = std::size_t(std::atoi(key.c_str() + t.id.size() + 1));
                        if (idx < t.entries.size())
                            StoryLog(by ? by->GetName() : "", "journal", t.title + ": " + t.entries[idx].text);
                        if (told.insert(t.title).second)
                            for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
                                if (p && p->IsInWorld())
                                    Send(p, "YN^" + t.title);
                    }
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            if (p && p->IsInWorld())
                PushJournal(p);
    }

    void StoryOutcome(uint32 encounterId, std::string const& how, Player* by)
    {
        std::string key = "@resolved:" + std::to_string(encounterId);
        if (StoryValue(key))
            return;                              // (the story remembers the first ending only)
        Store(key, 1);
        if (EncounterDef const* def = GetEncounterDef(encounterId))
            StoryLog(by ? by->GetName() : "", "settled", Acore::StringFormat("{} - {}.", def->name,
                how == "fight" ? "settled by force" : (how == "sour" ? "settled, on a sour note" : "settled without bloodshed")));
        auto itr = sOutcomes.find({ encounterId, how });
        if (itr != sOutcomes.end())
            StoryApply(itr->second, by);
        else
            ApplyWorld();
    }

    void StoryNodeReached(uint32 node, std::string const& effects, Player* by)
    {
        if (effects.empty())
            return;
        std::string key = "@node:" + std::to_string(node);
        if (StoryValue(key))
            return;
        Store(key, 1);
        StoryApply(effects, by);
    }

    uint32 StoryStartNode(uint32 encounterId, uint32 node)
    {
        auto itr = sStarts.find(encounterId);
        if (itr != sStarts.end())
            for (auto const& [cond, alt] : itr->second)
                if (StoryConds(cond))
                    return alt;
        return node;
    }

    bool StoryAbsent(uint32 encounterId)
    {
        auto itr = sAbsent.find(encounterId);
        return itr != sAbsent.end() && StoryConds(itr->second);
    }

    std::string StoryRumor(uint32 zone)
    {
        std::vector<Rumor const*> fit;
        for (Rumor const& r : sRumors)
            if ((r.zone == 0 || r.zone == zone) && StoryConds(r.cond))
                fit.push_back(&r);
        return fit.empty() ? std::string() : fit[urand(0, uint32(fit.size()) - 1)]->text;
    }

    void StoryCreatureAdded(Creature* c) { PhaseCreature(c); }

    // the encounters the story took away, and those anyone has already resolved (one shared world: resolved for one is
    // resolved for all), count as resolved for this character - their XP paid
    void CreditAbsent(Player* p)
    {
        if (!p || Test::IsBot(p->GetGUID()))
            return;
        std::vector<uint32> resolved;
        for (auto const& [key, value] : sState)
            if (value && key.rfind("@resolved:", 0) == 0)
                resolved.push_back(uint32(std::atoi(key.c_str() + 10)));
        for (uint32 enc : resolved)
            if (EncounterDef const* def = GetEncounterDef(enc); def && !HasCleared(p, enc))
            {
                Log(p, Acore::StringFormat("|cffc8b890{} was settled while you were away - it counts for you too.|r", def->name));
                OnEncounterWon(p, *def, true, false);
            }
        for (auto const& [enc, cond] : sAbsent)
            if (StoryConds(cond))
                if (EncounterDef const* def = GetEncounterDef(enc); def && !HasCleared(p, enc))
                {
                    Log(p, Acore::StringFormat("|cffc8b890{} is no longer there to be dealt with - the story moved on without it.|r",
                        def->name));
                    OnEncounterWon(p, *def, true, false);
                }
    }

    void PushJournal(Player* p)
    {
        Send(p, "YO");
        for (Faction const& f : sFactions)
            Send(p, Acore::StringFormat("YF^{}^{}^{}^{}", f.id, f.name, StoryValue("rep:" + f.id), f.about.substr(0, 150)));
        Send(p, Acore::StringFormat("YL^{}^{}^{}", StoryValue("lean"), sLeanNeg, sLeanPos));
        for (Thread const& t : sThreads)
        {
            bool any = false;
            for (Entry const& e : t.entries)
                if (StoryConds(e.cond))
                {
                    if (!any)
                        Send(p, "YT^" + t.id + "^" + t.title);
                    any = true;
                    SendLongTo(p, "YE^" + t.id + "^", e.text);
                }
        }
        Send(p, "YD");
    }

    void StoryLog(std::string const& who, std::string const& kind, std::string const& text)
    {
        LogEntry e{ uint32(time(nullptr)), who.empty() ? std::string("The world") : who, kind, text };
        sChronicle.push_back(e);
        while (sChronicle.size() > LOG_KEEP)
            sChronicle.pop_front();
        std::string w = e.who, k = kind, t = text.substr(0, 480);
        CharacterDatabase.EscapeString(w);
        CharacterDatabase.EscapeString(k);
        CharacterDatabase.EscapeString(t);
        CharacterDatabase.Execute("INSERT INTO dnd_story_log (at, who, kind, text) VALUES ({}, '{}', '{}', '{}')", e.at, w, k, t);
        std::string line = LogLine(e);
        for (auto const& [guid, p] : ObjectAccessor::GetPlayers())
            if (p && p->IsInWorld() && !Test::IsBot(guid))
                Send(p, line);
    }

    void StoryLearnClue(std::string const& scope, std::string const& clue)
    {
        std::string key = "@clue:" + scope + ":" + clue;
        if (!StoryValue(key))
            Store(key, 1);
    }

    std::vector<std::string> StoryClues(std::string const& scope)
    {
        std::vector<std::string> out;
        std::string prefix = "@clue:" + scope + ":";
        for (auto itr = sState.lower_bound(prefix); itr != sState.end() && itr->first.rfind(prefix, 0) == 0; ++itr)
            out.push_back(itr->first.substr(prefix.size()));
        return out;
    }

    void PushChronicle(Player* p)
    {
        Send(p, "YK");
        for (LogEntry const& e : sChronicle)
            Send(p, LogLine(e));
    }

    void LoadStory()
    {
        sFactions.clear();
        sThreads.clear();
        sOutcomes.clear();
        sStarts.clear();
        sAbsent.clear();
        sStorySpawns.clear();
        sRumors.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT id, name, description FROM dnd_story_faction ORDER BY sort"))
            do
                sFactions.push_back({ r->Fetch()[0].Get<std::string>(), r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>() });
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT id, title FROM dnd_story_thread ORDER BY sort"))
            do
                sThreads.push_back({ r->Fetch()[0].Get<std::string>(), r->Fetch()[1].Get<std::string>(), {} });
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT thread, cond, text FROM dnd_story_entry ORDER BY thread, idx"))
            do
            {
                std::string tid = r->Fetch()[0].Get<std::string>();
                for (Thread& t : sThreads)
                    if (t.id == tid)
                        t.entries.push_back({ r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT encounter_id, how, effects FROM dnd_story_outcome"))
            do
                sOutcomes[{ r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<std::string>() }] = r->Fetch()[2].Get<std::string>();
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT encounter_id, cond, node FROM dnd_story_start ORDER BY encounter_id, idx"))
            do
                sStarts[r->Fetch()[0].Get<uint32>()].push_back({ r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<uint32>() });
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT encounter_id, cond FROM dnd_story_absent"))
            do
                sAbsent[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<std::string>();
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT guid, cond FROM dnd_story_world"))
            do
                sStorySpawns[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<std::string>();
            while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT zone, cond, text FROM dnd_story_rumor ORDER BY id"))
            do
                sRumors.push_back({ r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>() });
            while (r->NextRow());
        // the story so far (one shared world: the characters database keeps it)
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS dnd_story_state (flag VARCHAR(80) NOT NULL, value INT NOT NULL, "
            "at INT UNSIGNED NOT NULL DEFAULT 0, PRIMARY KEY (flag)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        CharacterDatabase.DirectExecute("CREATE TABLE IF NOT EXISTS dnd_story_log (id INT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "at INT UNSIGNED NOT NULL, who VARCHAR(48) NOT NULL, kind VARCHAR(12) NOT NULL, text VARCHAR(500) NOT NULL, "
            "PRIMARY KEY (id)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
        sChronicle.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT at, who, kind, text FROM (SELECT * FROM dnd_story_log ORDER BY id DESC "
            "LIMIT {}) recent ORDER BY id", LOG_KEEP))
            do
                sChronicle.push_back({ r->Fetch()[0].Get<uint32>(), r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>(),
                    r->Fetch()[3].Get<std::string>() });
            while (r->NextRow());
        sState.clear();
        if (QueryResult r = CharacterDatabase.Query("SELECT flag, value FROM dnd_story_state"))
            do
                sState[r->Fetch()[0].Get<std::string>()] = r->Fetch()[1].Get<int32>();
            while (r->NextRow());
        LOG_INFO("module", "mod-dnd: the story - {} factions, {} threads, {} outcomes, {} story flags set", sFactions.size(),
            sThreads.size(), sOutcomes.size(), sState.size());
    }

    // the self-test's: a story flag set or cleared directly (no outcome, no journal), and forgetting what it set
    void StoryTestSet(std::string const& key, int value) { Store(key, value); }
}
