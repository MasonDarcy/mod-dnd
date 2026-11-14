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
 * Addon messages: GO^name^games^purse^bets (the table opens: what's on offer), GS^... the game's state (below),
 * GX (the table closes). The addon sends tav^play^game^bet, tav^act^action[^arg], tav^leave.
 */
#include "Dnd.h"

#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringFormat.h"
#include <algorithm>
#include <map>
#include <set>

namespace Dnd
{
    namespace
    {
        uint32 const PURSE = 3000;                       // copper: 30 gp per villager (1 gp = 1 silver here)
        uint32 const BETS[] = { 100, 500, 1000 };        // 1, 5, 10 gp
        char const* const DRINKS[] = { "Thunder Ale", "Dark Stout", "Dwarven Whiskey", "Moonshine", "Moonshine" };

        struct Table
        {
            ObjectGuid npc;
            uint32 spawn = 0;
            std::string name, games, game;
            uint32 bet = 0;
            std::string phase;                           // "" no game yet, "yours" your move, "done"
            bool cheated = false;
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

        uint32 PurseLeft(Player* p, Table const& t)
        {
            auto itr = sSpent.find({ p->GetGUID(), t.spawn });
            uint32 spent = itr == sSpent.end() ? 0 : itr->second;
            return spent >= PURSE ? 0 : PURSE - spent;
        }

        std::string Money(uint32 c) { return c % 100 ? Acore::StringFormat("{}.{:02} gp", c / 100, c % 100) : Acore::StringFormat("{} gp", c / 100); }

        Creature* NpcOf(Player* p, Table const& t)
        {
            Creature* c = ObjectAccessor::GetCreature(*p, t.npc);
            return c && c->IsAlive() && c->GetExactDist(p) <= 12.f ? c : nullptr;
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
        void Push(Player* p, Table& t)
        {
            if (t.game == "dice")
                Send(p, Acore::StringFormat("GS^dice^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, Csv(t.npcDice), Csv(t.myDice),
                    t.rerolled ? 1 : 0, t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
            else if (t.game == "cards")
                Send(p, Acore::StringFormat("GS^cards^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, Csv(t.dealer, t.phase == "yours"),
                    Csv(t.mine), Total(t.mine), t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
            else if (t.game == "drink")
                Send(p, Acore::StringFormat("GS^drink^{}^{}^{}^{}^{}^{}^{}^{}^{}", t.bet, t.phase, t.round, t.myDrunk, t.npcDrunk,
                    DRINKS[std::min(t.round, 4)], t.cheated ? 1 : 0, t.note.substr(0, 150), t.result));
        }

        void Menu(Player* p, Table& t)
        {
            bool banned = sBanned.count({ p->GetGUID(), t.spawn }) != 0;
            Send(p, Acore::StringFormat("GO^{}^{}^{}^{}", t.name, banned ? "" : t.games, PurseLeft(p, t), p->GetMoney()));
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
            Send(p, Acore::StringFormat("TD^Sleight of Hand^{}^{}^{}^{}", r.natural, bonus, dc, ok ? 1 : 0));
            Log(p, Acore::StringFormat("   Sleight of Hand ({}), DC {}: {} - {}", what, dc, r.text, ok ? "|cff40ff40unseen|r" : "|cffff4040CAUGHT|r"));
            if (ok)
                return true;
            sBanned.insert({ p->GetGUID(), t.spawn });
            t.note = Acore::StringFormat("{} catches you at it! \"Get out of my sight, cheat.\"", t.name);
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
                Send(p, Acore::StringFormat("TD^CON save^{}^{}^{}^{}", r.natural, bonus, dc, ok ? 1 : 0));
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

    // the table opens (from a villager's talk, or a click on one already talked to)
    void OpenTavern(Player* p, Creature* npc, std::string const& games)
    {
        if (!p || !npc || games.empty())
            return;
        Table& t = sTables[p->GetGUID()];
        t = Table();
        t.npc = npc->GetGUID();
        t.spawn = npc->GetSpawnId() ? npc->GetSpawnId() : npc->GetGUID().GetCounter();
        t.name = npc->GetName();
        t.games = games;
        npc->SetFacingToObject(p);
        Menu(p, t);
    }

    void TavernMessage(Player* p, std::string const& msg)
    {
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
        if (f[0] == "leave")
        {
            if (t.phase == "yours")              // walking away mid-hand forfeits it
                Settle(p, t, -1);
            Send(p, "GX");
            sTables.erase(itr);
            return;
        }
        if (!NpcOf(p, t) || InCombat(p))
        {
            Error(p, "You're not at the table any more.");
            Send(p, "GX");
            sTables.erase(itr);
            return;
        }
        if (f[0] == "play" && f.size() >= 3 && t.phase != "yours")
        {
            std::string game = f[1];
            uint32 bet = uint32(std::atoi(f[2].c_str()));
            if (sBanned.count({ p->GetGUID(), t.spawn }) || t.games.find(game) == std::string::npos ||
                std::find(std::begin(BETS), std::end(BETS), bet) == std::end(BETS))
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
            p->ModifyMoney(-int32(bet));
            std::string games = t.games;
            ObjectGuid npc = t.npc;
            uint32 spawn = t.spawn;
            std::string name = t.name;
            t = Table();
            t.npc = npc, t.spawn = spawn, t.name = name, t.games = games;
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
                t.note = Acore::StringFormat("{} slams two mugs of {} on the bar.", name, DRINKS[0]);
                if (Creature* c = NpcOf(p, t))
                    c->HandleEmoteCommand(EMOTE_ONESHOT_CHEER);
            }
            Push(p, t);
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

    void ForgetTavern(ObjectGuid guid) { sTables.erase(guid); }
    void ResetTavern(Player* p)
    {
        for (auto itr = sSpent.begin(); itr != sSpent.end();)
            itr = itr->first.first == p->GetGUID() ? sSpent.erase(itr) : std::next(itr);
        for (auto itr = sBanned.begin(); itr != sBanned.end();)
            itr = itr->first == p->GetGUID() ? sBanned.erase(itr) : std::next(itr);
    }
}
