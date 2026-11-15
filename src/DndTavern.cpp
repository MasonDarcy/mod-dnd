/*
 * mod-dnd tavern games: a villager whose townsfolk role has `games` (tools/townsfolk.py GAMES) offers a game for a
 * wager - innkeepers and tavern hands: Highroll (dice) and Thane's Twenty-One (cards); brewers: a drinking contest.
 * The server deals, rolls and pays (Dnd::D, so the self-test jig can rig it); the addon (DnDTavern.lua) only draws
 * the table and sends the player's choices.
 *
 *   Highroll    both roll 3d6; you see the dwarf's dice, then may reroll any of yours once. Triples beat pairs beat
 *               sums (pairs by the pair, then the odd die). A tie is a push.
 *   Twenty-One  blackjack against the dwarf: hit / stand, the dwarf draws to 17, a natural 21 pays 3:2.
 *   Drinking    each round both drink and make a CON save, DC 10 + 2 per round (a dwarf has advantage: in 5e, drink
 *               is poison). Three failed saves and you're under the table. Your character really does get drunk.
 * Cheating (Sleight of Hand): palm a six, palm a card, tip a drink into the spittoon. Caught: the wager is forfeit and
 * that villager won't play with you again (until the server restarts). Each villager's purse is finite (per
 * character, per server run), so a winning streak ends with "you've cleaned me out".
 *
 * Wagers are any whole number of silver from 1 to MAX_BET (the addon's slider). Money is told the way the bag shows it
 * - gold, silver, copper - since 1 gp here is a WoW silver.
 *
 * Some villagers are hot-headed (fixed per villager): catch one cheating and they swing at you; clean one out (their
 * purse under a third) and they accuse you - talk them down (Persuasion) or it comes to blows. A brawl is a real fight
 * where nobody dies (DndCombat.cpp: 0 HP is out cold); win it and a cheat's stake is thrown back at you, lose it and
 * you wake up lighter.
 *
 * Addon messages: GO^name^games^purse^money^inn^maxbet (the table opens: what's on offer), GS^... the game's state
 * (below), GX (the table closes). The addon sends tav^play^game^bet, tav^act^action[^arg], tav^menu, tav^leave.
 */
#include "Dnd.h"

#include "Creature.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include "WorldSession.h"
#include <algorithm>
#include <map>
#include <set>

namespace Dnd
{
    namespace
    {
        uint32 const PURSE = 6000;                       // copper: 60 silver per villager (1 gp = 1 silver here)
        uint32 const MIN_BET = 100, MAX_BET = 2500, BET_STEP = 100;    // a wager: 1 to 25 silver, in whole silvers
        int sTestTemper = -1;                            // (the self-test's: every host calm / hot-headed)
        char const* const DRINKS[] = { "Thunder Ale", "Dark Stout", "Dwarven Whiskey", "Moonshine", "Moonshine" };

        struct Table
        {
            ObjectGuid npc;
            uint32 spawn = 0;
            std::string name, games, game;
            bool inn = false;                            // the host is an innkeeper: the table offers to make it home
            std::set<ObjectGuid> watchers;               // players watching this table (read-only: "Watch the game")
            std::set<ObjectGuid> offered;                // nearby players offered the watch (DLG / DLGX)
            uint32 scanTimer = 0;
            uint32 bet = 0;
            std::string phase;                           // "" no game yet, "yours" your move, "done"
            bool cheated = false;
            bool hot = false;                            // a hot-headed host: a caught cheat, or a cleaning-out, means a fight
            bool wager = false;                          // an encounter's wager (OpenWager): one hand, then the talk goes on
            bool caught = false;                         // (a wager) caught cheating: it comes to a fight, them first
            uint32 wagerIn = 0;
            int brawl = 0;                               // a brawl coming (BRAWL_*), once the hand's words have landed
            uint32 brawlStake = 0, brawlIn = 0;
            std::string note;                            // the last line of narration
            int result = 2;                              // 1 won, 0 push, -1 lost, 2 still playing
            // dice
            std::vector<int> npcDice, myDice;
            bool rerolled = false;
            // cards: rank 1..13 * 4 + suit
            std::vector<int> deck, mine, dealer;
            // drinking
            int round = 0, myDrunk = 0, npcDrunk = 0;
            std::string myLast, npcLast;
        };
        std::unordered_map<ObjectGuid, Table> sTables;
        std::map<std::pair<ObjectGuid, uint32>, uint32> sSpent;       // (player, spawn) -> copper won off that villager
        std::set<std::pair<ObjectGuid, uint32>> sBanned;              // caught cheating
        std::set<std::pair<ObjectGuid, uint32>> sAccused;             // a sore loser has had their say (once)
        enum { BRAWL_NONE, BRAWL_CHEAT, BRAWL_SORE };
        struct Brawl { int why = 0; uint32 stake = 0, spawn = 0; std::string host; };
        std::unordered_map<ObjectGuid, Brawl> sBrawls;                // a player in a tavern brawl: what rides on it

        // a villager's temper, fixed for that villager: about a third of them have a short fuse
        bool HotHeaded(uint32 spawn)
        {
            if (sTestTemper >= 0)
                return sTestTemper == 1;
            return (spawn * 2654435761u >> 13) % 3 == 0;
        }

        uint32 PurseLeft(Player* p, Table const& t)
        {
            if (t.wager)
                return 1000000;                          // (what they stake is the encounter itself)
            auto itr = sSpent.find({ p->GetGUID(), t.spawn });
            uint32 spent = itr == sSpent.end() ? 0 : itr->second;
            return spent >= PURSE ? 0 : PURSE - spent;
        }

        // money as the bag shows it: "1 gold 20 silver", "5 silver", "50 copper"
        std::string Money(uint32 c)
        {
            std::string out;
            auto add = [&out](uint32 n, char const* what)
            {
                if (n)
                    out += (out.empty() ? "" : " ") + std::to_string(n) + " " + what;
            };
            add(c / 10000, "gold");
            add(c / 100 % 100, "silver");
            add(c % 100, "copper");
            return out.empty() ? "nothing" : out;
        }

        Creature* NpcOf(Player* p, Table const& t)
        {
            Creature* c = ObjectAccessor::GetCreature(*p, t.npc);
            return c && c->IsAlive() && c->GetExactDist(p) <= (t.wager ? 30.f : 12.f) ? c : nullptr;
        }

        // ---------------------------------------------------------------- dice
        int Score(std::vector<int> d)
        {
            std::sort(d.begin(), d.end());
            if (d[0] == d[2])
                return 3000 + d[0];
            if (d[0] == d[1])
                return 2000 + d[0] * 10 + d[2];
            if (d[1] == d[2])
                return 2000 + d[1] * 10 + d[0];
            return d[0] + d[1] + d[2];
        }

        std::string Describe(std::vector<int> const& d)
        {
            int s = Score(d);
            if (s >= 3000)
                return Acore::StringFormat("triple {}s", s - 3000);
            if (s >= 2000)
                return Acore::StringFormat("a pair of {}s", (s - 2000) / 10);
            return Acore::StringFormat("{}", s);
        }

        std::string Csv(std::vector<int> const& v, bool hideSecond = false)
        {
            std::string out;
            for (std::size_t i = 0; i < v.size(); ++i)
                out += (i ? "," : "") + ((hideSecond && i == 1) ? std::string("?") : std::to_string(v[i]));
            return out;
        }

        // ---------------------------------------------------------------- cards
        int Rank(int card) { return card / 4 + 1; }
        int Total(std::vector<int> const& h)
        {
            int sum = 0, aces = 0;
            for (int c : h)
            {
                int r = Rank(c);
                sum += r == 1 ? 11 : std::min(10, r);
                aces += r == 1;
            }
            while (sum > 21 && aces--)
                sum -= 10;
            return sum;
        }

        int Draw(Table& t)
        {
            if (t.deck.empty())
                for (int c = 0; c < 52; ++c)
                    t.deck.push_back(c);
            int i = D(int(t.deck.size())) - 1;       // Dnd::D: the jig can rig the deal
            int c = t.deck[i];
            t.deck.erase(t.deck.begin() + i);
            return c;
        }

        // ---------------------------------------------------------------- messages
        // to the player at the table and whoever is watching them play
        void ToTable(Player* p, Table const& t, std::string const& msg)
        {
            Send(p, msg);
            for (ObjectGuid g : t.watchers)
                if (Player* w = ObjectAccessor::FindPlayer(g))
                    Send(w, msg);
        }

        void Push(Player* p, Table& t)
        {
            if (t.game == "dice")
                ToTable(p, t, Acore::StringFormat("GS^dice^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, Csv(t.npcDice), Csv(t.myDice),
                    t.rerolled ? 1 : 0, t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
            else if (t.game == "cards")
                ToTable(p, t, Acore::StringFormat("GS^cards^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, Csv(t.dealer, t.phase == "yours"),
                    Csv(t.mine), Total(t.mine), t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
            else if (t.game == "drink")
                ToTable(p, t, Acore::StringFormat("GS^drink^{}^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, t.round, t.myDrunk, t.npcDrunk,
                    DRINKS[std::min(t.round, 4)], t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
        }

        void Menu(Player* p, Table& t)
        {
            bool banned = sBanned.count({ p->GetGUID(), t.spawn }) != 0;
            ToTable(p, t, Acore::StringFormat("GO^{}^{}^{}^{}^{}^{}", t.name, banned ? "" : t.games, PurseLeft(p, t),
                p->GetMoney(), t.inn ? 1 : 0, std::min(MAX_BET, PurseLeft(p, t))));
        }

        // the wager settles: +1 win, 0 push, -1 loss; mult = what a win pays (1, or 1.5 for a natural 21)
        void Settle(Player* p, Table& t, int outcome, float mult = 1.f)
        {
            t.phase = "done";
            t.result = outcome;
            uint32 won = uint32(t.bet * mult);
            if (outcome > 0)
            {
                p->ModifyMoney(int32(t.bet + won));      // the stake back, and the winnings
                sSpent[{ p->GetGUID(), t.spawn }] += won;
                Log(p, Acore::StringFormat("|cff80ff80You win {}.|r", Money(won)));
                // a hot head cleaned out (their purse under a third) has something to say about your luck, once
                if (t.hot && !t.cheated && PurseLeft(p, t) < PURSE / 3 && sAccused.insert({ p->GetGUID(), t.spawn }).second)
                {
                    int bonus = SkillBonusByName(p, "Persuasion");
                    D20Result r = RollD20(bonus, 0);
                    bool ok = r.natural == 20 || (r.natural != 1 && r.total >= 13);
                    ToTable(p, t, Acore::StringFormat("TD^Persuasion^{}^{}^{}^{}", r.natural, bonus, 13, ok ? 1 : 0));
                    Log(p, Acore::StringFormat("|cffffd100{}:|r \"Nobody's that lucky.\" Persuasion DC 13: {} - {}", t.name, r.text,
                        ok ? "|cff40ff40they back down|r" : "|cffff4040it comes to blows|r"));
                    if (ok)
                        t.note += Acore::StringFormat(" {} glares, mutters, and pays up.", t.name);
                    else
                    {
                        t.note += Acore::StringFormat(" \"Nobody's that lucky.\" {} kicks back their chair.", t.name);
                        t.brawl = BRAWL_SORE;
                        t.brawlStake = won;
                    }
                }
            }
            else if (outcome == 0)
            {
                p->ModifyMoney(int32(t.bet));
                Log(p, "|cffc8b890A push: your stake comes back.|r");
            }
            else
            {
                // what they win off you goes back in their purse
                auto& spent = sSpent[{ p->GetGUID(), t.spawn }];
                spent -= std::min(spent, t.bet);
                Log(p, Acore::StringFormat("|cffff8080You lose {}.|r", Money(t.bet)));
            }
            Push(p, t);
            if (Creature* c = NpcOf(p, t))
                c->HandleEmoteCommand(outcome < 0 ? EMOTE_ONESHOT_LAUGH : (outcome > 0 ? EMOTE_ONESHOT_CRY : EMOTE_ONESHOT_QUESTION));
        }

        // Sleight of Hand vs DC: the big d20 shows; false = caught (wager forfeit, banned from that table)
        bool Cheat(Player* p, Table& t, int dc, char const* what)
        {
            t.cheated = true;
            int bonus = SkillBonusByName(p, "Sleight of Hand");
            D20Result r = RollD20(bonus, 0);
            bool ok = r.natural == 20 || (r.natural != 1 && r.total >= dc);
            ToTable(p, t, Acore::StringFormat("TD^Sleight of Hand^{}^{}^{}^{}", r.natural, bonus, dc, ok ? 1 : 0));
            Log(p, Acore::StringFormat("   Sleight of Hand ({}), DC {}: {} - {}", what, dc, r.text, ok ? "|cff40ff40unseen|r" : "|cffff4040CAUGHT|r"));
            if (ok)
                return true;
            if (t.wager)
            {
                t.caught = true;
                t.note = Acore::StringFormat("{} catches you at it. A hand goes to a hilt.", t.name);
                Settle(p, t, -1);
                return false;
            }
            sBanned.insert({ p->GetGUID(), t.spawn });
            t.note = t.hot ? Acore::StringFormat("{} catches you at it - and comes round the table swinging!", t.name)
                           : Acore::StringFormat("{} catches you at it! \"Get out of my sight, cheat.\"", t.name);
            if (t.hot)
            {
                t.brawl = BRAWL_CHEAT;
                t.brawlStake = t.bet;
            }
            Settle(p, t, -1);
            return false;
        }

        // ---------------------------------------------------------------- the games
        void DiceResolve(Player* p, Table& t)
        {
            int a = Score(t.myDice), b = Score(t.npcDice);
            t.note = Acore::StringFormat("Your {} against {}'s {}.", Describe(t.myDice), t.name, Describe(t.npcDice));
            Settle(p, t, a > b ? 1 : (a == b ? 0 : -1));
        }

        void CardsDealer(Player* p, Table& t)
        {
            while (Total(t.dealer) < 17)
                t.dealer.push_back(Draw(t));
            int me = Total(t.mine), them = Total(t.dealer);
            t.note = Acore::StringFormat("Your {} against {}'s {}{}.", me, t.name, them, them > 21 ? " - bust" : "");
            Settle(p, t, them > 21 || me > them ? 1 : (me == them ? 0 : -1));
        }

        void DrinkRound(Player* p, Table& t, bool skipMine)
        {
            int dc = 10 + 2 * t.round;
            std::string drink = DRINKS[std::min(t.round, 4)];
            if (!skipMine)
            {
                // a dwarf's Dwarven Resilience: advantage on saves against poison, and this is poison
                int bonus = SaveBonus(p, CON);
                bool adv = SaveAdvantage(p, CON, false, true);
                D20Result r = RollD20(bonus, adv ? 1 : 0);
                bool ok = r.total >= dc;
                ToTable(p, t, Acore::StringFormat("TD^CON save^{}^{}^{}^{}", r.natural, bonus, dc, ok ? 1 : 0));
                Log(p, Acore::StringFormat("   You drain the {}. CON save DC {}: {} - {}", drink, dc, r.text, ok ? "|cff40ff40steady|r" : "|cffff8040it hits you|r"));
                if (!ok)
                    ++t.myDrunk;
                t.myLast = ok ? "steady" : "reeling";
                p->SetDrunkValue(uint8(std::min(100, t.myDrunk * 34)));     // WoW's own drunk screen and slur
            }
            else
                t.myLast = "spittoon";
            // the villager: +3 CON, and a dwarf's advantage
            D20Result n = RollD20(3, 1);
            bool nok = n.total >= dc;
            if (!nok)
                ++t.npcDrunk;
            t.npcLast = nok ? "steady" : "reeling";
            Log(p, Acore::StringFormat("   {} drains theirs: {} - {}", t.name, n.text, nok ? "steady" : "it hits them"));
            ++t.round;
            if (t.myDrunk >= 3 || t.npcDrunk >= 3)
            {
                if (t.myDrunk >= 3 && t.npcDrunk >= 3)
                    t.note = "You both slide under the table at once. The room calls it a draw.";
                else if (t.myDrunk >= 3)
                    t.note = Acore::StringFormat("The floor comes up to meet you. {} raises a mug over you.", t.name);
                else
                    t.note = Acore::StringFormat("{} sways, smiles, and folds gently onto the bar.", t.name);
                Settle(p, t, t.myDrunk >= 3 && t.npcDrunk >= 3 ? 0 : (t.myDrunk >= 3 ? -1 : 1));
                return;
            }
            t.note = Acore::StringFormat("Next round: {}.", DRINKS[std::min(t.round, 4)]);
            Push(p, t);
        }
    }

    static void CloseTable(Player* p, Table& t);
    static void StopWatching(Player* w, bool tell);
    std::string MoneyText(uint32 copper) { return Money(copper); }

    // a fresh hand: the stake on the table, the first deal (the same table, its host, and whoever is watching)
    static void Deal(Player* p, Table& t, std::string const& game, uint32 bet)
    {
        p->ModifyMoney(-int32(bet));
        std::string games = t.games;
        ObjectGuid npc = t.npc;
        uint32 spawn = t.spawn;
        std::string name = t.name;
        bool inn = t.inn, hot = t.hot, wager = t.wager;
        std::set<ObjectGuid> watchers = t.watchers, offered = t.offered;
        t = Table();
        t.npc = npc, t.spawn = spawn, t.name = name, t.games = games, t.inn = inn, t.hot = hot, t.wager = wager;
        t.watchers = watchers, t.offered = offered;
        t.game = game;
        t.bet = bet;
        t.phase = "yours";
        Log(p, Acore::StringFormat("|cffffd100You put {} on the table.|r", Money(bet)));
        if (game == "dice")
        {
            for (int i = 0; i < 3; ++i)
                t.npcDice.push_back(D(6));
            for (int i = 0; i < 3; ++i)
                t.myDice.push_back(D(6));
            t.note = Acore::StringFormat("{} rolls {}. You roll {}.", name, Describe(t.npcDice), Describe(t.myDice));
        }
        else if (game == "cards")
        {
            t.mine = { Draw(t) };
            t.dealer = { Draw(t) };
            t.mine.push_back(Draw(t));
            t.dealer.push_back(Draw(t));
            if (Total(t.mine) == 21)
            {
                t.note = Total(t.dealer) == 21 ? "Twenty-one each!" : "Twenty-one on the deal!";
                Settle(p, t, Total(t.dealer) == 21 ? 0 : 1, 1.5f);
                return;
            }
            t.note = "Hit or stand?";
        }
        else if (game == "drink")
        {
            t.note = Acore::StringFormat("{} slams two mugs of {} down.", name, DRINKS[0]);
            if (Creature* c = NpcOf(p, t))
                c->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
        }
        Push(p, t);
    }

    // an encounter's wager: straight to the hand, at the stake the talk named (GO's 8th field: a wager - no menu, no
    // replays; the addon offers "Continue" when it's done)
    bool OpenWager(Player* p, Creature* npc, std::string const& game, uint32 stake)
    {
        if (!p || !npc || (game != "dice" && game != "cards" && game != "drink") || p->GetMoney() < stake)
            return false;
        StopWatching(p, true);
        Table& t = sTables[p->GetGUID()];
        t = Table();
        t.npc = npc->GetGUID();
        t.spawn = 0;
        t.name = npc->GetName();
        t.games = game;
        t.wager = true;
        npc->SetFacingToObject(p);
        ToTable(p, t, Acore::StringFormat("GO^{}^{}^{}^{}^0^{}^1", t.name, game, stake, p->GetMoney(), stake));
        Deal(p, t, game, stake);
        return true;
    }

    // the wager's hand is over (or abandoned): the table shuts and the talk takes the result
    static void EndWager(Player* p, Table& t)
    {
        int outcome = t.caught ? -2 : (t.phase == "done" ? t.result : -1);
        CloseTable(p, t);
        sTables.erase(p->GetGUID());             // (t is gone)
        WagerResult(p, outcome);
    }

    // the table opens (from a villager's talk, or a click on one already talked to)
    static void StopWatching(Player* w, bool tell);

    // an innkeeper makes their inn your home: WoW's own bind (the hearthstone comes back here)
    void MakeHome(Player* p, Creature* npc)
    {
        if (!p || !npc || p->GetMap()->Instanceable())
            return;
        npc->SetFacingToObject(p);
        npc->HandleEmoteCommand(EMOTE_ONESHOT_TALK);
        p->GetSession()->SendBindPoint(npc);
        Log(p, Acore::StringFormat("|cffffd100{}:|r \"There's a bed here whenever you need one. Your hearthstone will bring you "
            "home.\"", npc->GetName()));
    }

    void OpenTavern(Player* p, Creature* npc, std::string const& games)
    {
        if (!p || !npc || games.empty())
            return;
        StopWatching(p, true);                   // (watching someone else's game: your own table takes over)
        Table& t = sTables[p->GetGUID()];
        t = Table();
        t.npc = npc->GetGUID();
        t.spawn = npc->GetSpawnId() ? npc->GetSpawnId() : npc->GetGUID().GetCounter();
        t.name = npc->GetName();
        t.games = games;
        t.inn = npc->IsInnkeeper();
        t.hot = HotHeaded(t.spawn);
        npc->SetFacingToObject(p);
        Menu(p, t);
    }

    void SetTestTemper(int temper) { sTestTemper = temper; }

    // the brawl is over: what rode on it changes hands
    void TavernBrawlOver(Player* p, Creature* host, bool won)
    {
        auto itr = sBrawls.find(p->GetGUID());
        if (itr == sBrawls.end())
            return;
        Brawl b = itr->second;
        sBrawls.erase(itr);
        if (host)
            host->HandleEmoteCommand(won ? EMOTE_ONESHOT_BEG : EMOTE_ONESHOT_CHEER);
        if (won)
        {
            if (b.why == BRAWL_CHEAT && b.stake)
            {
                p->ModifyMoney(int32(b.stake));
                Log(p, Acore::StringFormat("|cff80ff80{} picks themself up and throws your {} back at you. \"Keep it. Just go.\"|r",
                    b.host, Money(b.stake)));
            }
            else
                Log(p, Acore::StringFormat("|cff80ff80{} picks themself up, rubs their jaw, and says no more about luck.|r", b.host));
            return;
        }
        uint32 take = std::min<uint32>(b.stake, p->GetMoney());
        if (take)
        {
            p->ModifyMoney(-int32(take));
            if (b.why == BRAWL_SORE)                 // what they lost to you goes back in their purse
            {
                auto& spent = sSpent[{ p->GetGUID(), b.spawn }];
                spent -= std::min(spent, take);
            }
        }
        Log(p, take ? Acore::StringFormat("|cffff8080You come to with a sore head - and {} lighter. {} is drinking with it.|r", Money(take), b.host)
                    : Acore::StringFormat("|cffff8080You come to with a sore head. {} is still laughing.|r", b.host));
    }

    // ---------------------------------------------------------------- watching a game
    // the table shuts: its panel, and its watchers' panels; nobody's offered it any more
    static void CloseTable(Player* p, Table& t)
    {
        ToTable(p, t, "GX");
        t.watchers.clear();
        for (ObjectGuid g : t.offered)
            if (Player* o = ObjectAccessor::FindPlayer(g))
                Send(o, "DLGX^" + std::string(p->GetName()));
        t.offered.clear();
    }

    static void StopWatching(Player* w, bool tell)
    {
        for (auto& [g, t] : sTables)
            if (t.watchers.erase(w->GetGUID()) && tell)
                Send(w, "GX");
    }

    static bool CanWatch(Player* w, Player* p)
    {
        return w != p && w->IsAlive() && !InCombat(w) && !InDialogue(w) && !sTables.count(w->GetGUID()) &&
            w->GetMap() == p->GetMap() && w->GetExactDist(p) <= 20.f;
    }

    // a player at a table: offer the watch to whoever is near (DLG^player^host), withdraw it from whoever has wandered
    // off or got busy (DLGX^player); watchers who've walked away stop watching
    void UpdateTavern(Player* p, uint32 diff)
    {
        auto itr = sTables.find(p->GetGUID());
        if (itr == sTables.end())
            return;
        Table& t = itr->second;
        if (t.wager && t.phase == "done")
        {
            t.wagerIn += diff;
            if (t.wagerIn >= 3500)
                EndWager(p, t);
            return;
        }
        if (t.brawl)
        {
            t.brawlIn += diff;
            if (t.brawlIn < 1500)
                return;
            Creature* npc = NpcOf(p, t);
            Brawl b{ t.brawl, t.brawlStake, t.spawn, t.name };
            CloseTable(p, t);
            sTables.erase(itr);                  // (t is gone)
            if (npc)
            {
                sBrawls[p->GetGUID()] = b;
                npc->HandleEmoteCommand(EMOTE_ONESHOT_ROAR);
                if (!StartBrawl(p, npc))
                    sBrawls.erase(p->GetGUID());
            }
            return;
        }
        t.scanTimer += diff;
        if (t.scanTimer < 500)
            return;
        t.scanTimer = 0;
        std::string who = p->GetName();
        for (auto w = t.watchers.begin(); w != t.watchers.end();)
        {
            Player* pl = ObjectAccessor::FindPlayer(*w);
            bool keep = pl && pl->IsAlive() && !InCombat(pl) && pl->GetMap() == p->GetMap() && pl->GetExactDist(p) <= 30.f;
            if (!keep && pl)
                Send(pl, "GX");
            w = keep ? std::next(w) : t.watchers.erase(w);
        }
        for (auto o = t.offered.begin(); o != t.offered.end();)
        {
            Player* pl = ObjectAccessor::FindPlayer(*o);
            bool keep = pl && CanWatch(pl, p) && !t.watchers.count(pl->GetGUID());
            if (!keep && pl)
                Send(pl, "DLGX^" + who);
            o = keep ? std::next(o) : t.offered.erase(o);
        }
        std::string offer = "DLG^" + who + "^" + t.name;
        p->GetMap()->DoForAllPlayers([&](Player* pl)
        {
            if (!t.offered.count(pl->GetGUID()) && !t.watchers.count(pl->GetGUID()) && CanWatch(pl, p))
            {
                t.offered.insert(pl->GetGUID());
                Send(pl, offer);
            }
        });
    }

    // "Watch the game" pressed: the table as the player sees it, read-only (GV^player, then the usual GO / GS)
    void TavernWatch(Player* w, std::string const& name)
    {
        Player* p = ObjectAccessor::FindPlayerByName(name, false);
        auto itr = p ? sTables.find(p->GetGUID()) : sTables.end();
        if (itr == sTables.end() || !CanWatch(w, p))
        {
            Send(w, "DLGX^" + name);
            if (itr != sTables.end())
                Error(w, "You're too far away, or too busy, to watch.");
            return;
        }
        StopWatching(w, false);
        Table& t = itr->second;
        t.offered.erase(w->GetGUID());
        t.watchers.insert(w->GetGUID());
        Send(w, "DLGX^" + name);
        Send(w, "GV^" + name);
        bool banned = sBanned.count({ p->GetGUID(), t.spawn }) != 0;
        Send(w, Acore::StringFormat("GO^{}^{}^{}^{}^{}^{}", t.name, banned ? "" : t.games, PurseLeft(p, t), p->GetMoney(), 0,
            std::min(MAX_BET, PurseLeft(p, t))));
        if (!t.game.empty() && !t.phase.empty())
        {
            std::set<ObjectGuid> only = { w->GetGUID() };
            std::swap(only, t.watchers);         // the hand in progress, to the new watcher alone
            Push(p, t);
            std::swap(only, t.watchers);
        }
        Log(p, Acore::StringFormat("|cff909090{} is watching you play.|r", w->GetName()));
    }

    void TavernMessage(Player* p, std::string const& msg)
    {
        if (msg == "unwatch")
        {
            StopWatching(p, true);
            return;
        }
        auto itr = sTables.find(p->GetGUID());
        if (itr == sTables.end())
            return;
        Table& t = itr->second;
        std::vector<std::string> f;
        for (std::size_t a = 0, b; a <= msg.size(); a = b + 1)
        {
            b = msg.find('^', a);
            if (b == std::string::npos)
                b = msg.size();
            f.push_back(msg.substr(a, b - a));
        }
        if (f.empty())
            return;
        if (f[0] == "leave" && t.wager)
        {
            if (t.phase == "yours")              // walking away mid-hand: it's lost
            {
                t.note = "You push your cards away.";
                Settle(p, t, -1);
            }
            EndWager(p, t);
            return;
        }
        if (f[0] == "leave")
        {
            if (t.brawl)                         // (too late to walk away: they're coming round the table)
                return;
            if (t.phase == "yours")              // walking away mid-hand forfeits it
                Settle(p, t, -1);
            CloseTable(p, t);
            sTables.erase(itr);
            return;
        }
        if (!NpcOf(p, t) || InCombat(p))
        {
            Error(p, "You're not at the table any more.");
            if (t.wager)
            {
                if (t.phase == "yours")
                    Settle(p, t, -1);
                EndWager(p, t);
                return;
            }
            CloseTable(p, t);
            sTables.erase(itr);
            return;
        }
        if (t.wager && f[0] != "act")
            return;                              // (a wager is one hand: no menu, no replays, no inn)
        if (t.brawl)                             // (it's coming to blows: the table is done)
            return;
        if (f[0] == "menu" && t.phase != "yours")
        {
            Menu(p, t);
            return;
        }
        if (f[0] == "home" && t.inn)
        {
            if (Creature* npc = NpcOf(p, t))
                MakeHome(p, npc);
            return;
        }
        if (f[0] == "play" && f.size() >= 3 && t.phase != "yours")
        {
            std::string game = f[1];
            uint32 bet = uint32(std::atoi(f[2].c_str()));
            if (sBanned.count({ p->GetGUID(), t.spawn }) || t.games.find(game) == std::string::npos || bet < MIN_BET ||
                bet > MAX_BET || bet % BET_STEP)
                return;
            float pays = game == "cards" ? 1.5f : 1.f;
            if (p->GetMoney() < bet)
            {
                Error(p, "You can't cover that wager.");
                return;
            }
            if (PurseLeft(p, t) < uint32(bet * pays))
            {
                Log(p, Acore::StringFormat("|cffffd100{}:|r \"You've cleaned me out, friend. Come back another day.\"", t.name));
                Menu(p, t);
                return;
            }
            Deal(p, t, game, bet);
            return;
        }
        if (f[0] != "act" || f.size() < 2 || t.phase != "yours")
            return;
        std::string const& a = f[1];
        if (t.game == "dice")
        {
            if (a == "reroll" && f.size() >= 3 && !t.rerolled)
            {
                int mask = std::atoi(f[2].c_str());
                for (int i = 0; i < 3; ++i)
                    if (mask & (1 << i))
                        t.myDice[i] = D(6);
                t.rerolled = true;
                DiceResolve(p, t);
            }
            else if (a == "stand")
                DiceResolve(p, t);
            else if (a == "cheat" && f.size() >= 3 && !t.cheated)
            {
                int i = std::clamp(std::atoi(f[2].c_str()), 0, 2);
                if (Cheat(p, t, 14, "palming a six"))
                {
                    t.myDice[i] = 6;
                    t.note = "A six, as if it had always been there.";
                    Push(p, t);
                }
            }
        }
        else if (t.game == "cards")
        {
            if (a == "hit")
            {
                t.mine.push_back(Draw(t));
                if (Total(t.mine) > 21)
                {
                    t.note = Acore::StringFormat("{} - bust.", Total(t.mine));
                    Settle(p, t, -1);
                }
                else if (Total(t.mine) == 21)
                    CardsDealer(p, t);
                else
                {
                    t.note = "Hit or stand?";
                    Push(p, t);
                }
            }
            else if (a == "stand")
                CardsDealer(p, t);
            else if (a == "cheat" && !t.cheated && t.mine.size() >= 2)
            {
                if (Cheat(p, t, 15, "palming a card"))
                {
                    t.mine.back() = Draw(t);         // the last card swapped for the next off the deck
                    t.note = "A card goes up your sleeve and another comes down.";
                    if (Total(t.mine) > 21)
                    {
                        t.note += " Bust anyway.";
                        Settle(p, t, -1);
                    }
                    else
                        Push(p, t);
                }
            }
        }
        else if (t.game == "drink")
        {
            if (a == "drink")
                DrinkRound(p, t, false);
            else if (a == "concede")
            {
                t.note = Acore::StringFormat("You push the mug away. {} roars with laughter.", t.name);
                Settle(p, t, -1);
            }
            else if (a == "cheat" && !t.cheated)
            {
                if (Cheat(p, t, 13, "tipping it into the spittoon"))
                    DrinkRound(p, t, true);
            }
        }
    }

    void ForgetTavern(ObjectGuid guid)
    {
        sBrawls.erase(guid);
        if (auto itr = sTables.find(guid); itr != sTables.end())
            if (Player* p = ObjectAccessor::FindPlayer(guid))
                CloseTable(p, itr->second);
        sTables.erase(guid);
        for (auto& [g, t] : sTables)
        {
            t.watchers.erase(guid);
            t.offered.erase(guid);
        }
    }
    void ResetTavern(Player* p)
    {
        for (auto itr = sSpent.begin(); itr != sSpent.end();)
            itr = itr->first.first == p->GetGUID() ? sSpent.erase(itr) : std::next(itr);
        for (auto itr = sBanned.begin(); itr != sBanned.end();)
            itr = itr->first == p->GetGUID() ? sBanned.erase(itr) : std::next(itr);
        for (auto itr = sAccused.begin(); itr != sAccused.end();)
            itr = itr->first == p->GetGUID() ? sAccused.erase(itr) : std::next(itr);
    }
}
