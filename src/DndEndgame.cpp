/*
 * mod-dnd endgame: what a level-6 party plays for once the levels run out.
 *
 *  - Lair actions (world: dnd_lair): at the top of each round a boss's home acts on its own - a burst of harm at
 *    someone's feet, a grip that holds them, or more of the boss's people arriving. One a round, never the same twice
 *    running, while the encounter's leader stands. Each can be switched off by something the party did beforehand
 *    (`stop`: cleared another encounter, or made a find). The round itself runs in DndCombat.cpp (LairRound).
 *  - Rewards (world: dnd_encounter_reward): endgame encounters pay no XP - the levels stop at 6 - but items, coin and,
 *    for the bosses, a title and a line to the whole server. Paid on a character's first clear, like XP.
 *  - Attunement: only ATTUNEMENT_SLOTS magic items with effects work at once (5e). Equipping a fourth is refused; the
 *    effects of anything past the third are ignored anyway (EachItemEffect in DndRules.cpp).
 *  - Elixirs (world: dnd_elixir): stock elixirs drink as D&D ones - the item's dnd_item_effect rows, worn like gear
 *    until the end of the next fight (or an hour). One at a time; a Bonus Action in a fight, like a potion.
 *  - Portal keepers (world: dnd_portal, script npc_dnd_portal): mages who send a party across the world - the way to
 *    the Horde capitals from Stormwind, and back.
 */
#include "Dnd.h"

#include "Creature.h"
#include "CreatureScript.h"
#include "Item.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "StringFormat.h"
#include <map>

namespace Dnd
{
    static std::map<uint32, std::vector<LairAction>> sLairs;

    struct Reward
    {
        uint32 item = 0, count = 1, money = 0, title = 0;
    };
    static std::map<uint32, std::vector<Reward>> sRewards;

    struct PortalDest
    {
        std::string label, say;
        uint32 map = 0;
        float x = 0.f, y = 0.f, z = 0.f, o = 0.f;
        uint32 zone = 0;                     // the zone it leads into (closed: the way isn't open yet)
        uint8 level = 1;                     // the level a character needs to be sent
    };
    static std::map<uint32, std::vector<PortalDest>> sPortals;

    static std::unordered_map<uint32, uint32> sElixirSpells;    // spell -> item

    struct ElixirState
    {
        uint32 item = 0;
        uint32 since = 0;
        bool fought = false;
    };
    static std::unordered_map<ObjectGuid, ElixirState> sElixir;

    static Ability ParseAbility(std::string const& s)
    {
        for (int i = 0; i < ABILITY_COUNT; ++i)
            if (StringEqualI(s, AbilityNames[i]))
                return Ability(i);
        return DEX;
    }

    void LoadEndgame()
    {
        sLairs.clear();
        sRewards.clear();
        sPortals.clear();
        sElixirSpells.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT encounter_id, kind, name, text, dice, dmg_type, save_abil, dc, radius, "
            "entry, most, stop FROM dnd_lair ORDER BY encounter_id, idx"))
            do
            {
                Field* f = r->Fetch();
                LairAction a;
                a.kind = f[1].Get<std::string>();
                a.name = f[2].Get<std::string>();
                a.text = f[3].Get<std::string>();
                a.dice = Dice::Parse(f[4].Get<std::string>());
                a.type = ParseDmgType(f[5].Get<std::string>());
                a.save = ParseAbility(f[6].Get<std::string>());
                a.dc = f[7].Get<int32>();
                a.radius = f[8].Get<float>();
                a.entry = f[9].Get<uint32>();
                a.most = f[10].Get<uint8>();
                a.stop = f[11].Get<std::string>();
                sLairs[f[0].Get<uint32>()].push_back(a);
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT encounter_id, item, count, money, title FROM dnd_encounter_reward"))
            do
            {
                Field* f = r->Fetch();
                sRewards[f[0].Get<uint32>()].push_back({ f[1].Get<uint32>(), f[2].Get<uint32>(), f[3].Get<uint32>(), f[4].Get<uint32>() });
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT entry, label, say, map, x, y, z, o, zone, level FROM dnd_portal ORDER BY entry, idx"))
            do
            {
                Field* f = r->Fetch();
                PortalDest d;
                d.label = f[1].Get<std::string>();
                d.say = f[2].Get<std::string>();
                d.map = f[3].Get<uint32>();
                d.x = f[4].Get<float>();
                d.y = f[5].Get<float>();
                d.z = f[6].Get<float>();
                d.o = f[7].Get<float>();
                d.zone = f[8].Get<uint32>();
                d.level = f[9].Get<uint8>();
                sPortals[f[0].Get<uint32>()].push_back(d);
            } while (r->NextRow());
        if (QueryResult r = WorldDatabase.Query("SELECT spell, item FROM dnd_elixir"))
            do
                sElixirSpells[r->Fetch()[0].Get<uint32>()] = r->Fetch()[1].Get<uint32>();
            while (r->NextRow());
        LOG_INFO("module", "mod-dnd: {} lairs, {} rewarded encounters, {} portal keepers, {} elixirs", sLairs.size(), sRewards.size(),
            sPortals.size(), sElixirSpells.size());
    }

    std::vector<LairAction> const* GetLair(uint32 encounterId)
    {
        auto itr = sLairs.find(encounterId);
        return itr == sLairs.end() ? nullptr : &itr->second;
    }

    void SetTestLair(uint32 encounterId, std::vector<LairAction> const& actions)
    {
        if (actions.empty())
            sLairs.erase(encounterId);
        else
            sLairs[encounterId] = actions;
    }

    // "cleared:<encounter id>" or "found:<treat flag>": the lair action is off if anyone in the fight has it
    bool LairStopped(LairAction const& a, std::vector<Player*> const& party)
    {
        if (a.stop.empty())
            return false;
        std::size_t colon = a.stop.find(':');
        if (colon == std::string::npos)
            return false;
        std::string kind = a.stop.substr(0, colon), what = a.stop.substr(colon + 1);
        for (Player* p : party)
        {
            if (kind == "cleared" && HasCleared(p, uint32(std::atoi(what.c_str()))))
                return true;
            if (kind == "found")
                for (std::string const& f : TreatFlags(p))
                    if (f == what)
                        return true;
        }
        return false;
    }

    // ------------------------------------------------------------------ rewards
    bool HasRewards(uint32 encounterId) { return sRewards.count(encounterId) != 0; }

    void SetTestReward(uint32 encounterId, uint32 item, uint32 count, uint32 money, uint32 title)
    {
        if (!item && !money && !title)
            sRewards.erase(encounterId);
        else
            sRewards[encounterId] = { { item, count, money, title } };
    }

    void GiveRewards(Player* p, EncounterDef const& def)
    {
        auto itr = sRewards.find(def.id);
        if (itr == sRewards.end())
            return;
        for (Reward const& r : itr->second)
        {
            if (r.money)
            {
                p->ModifyMoney(int32(r.money));
                Log(p, Acore::StringFormat("|cffffd100You take {} gold from {}.|r", r.money / 100, def.name));
            }
            if (r.item)
            {
                ItemTemplate const* t = sObjectMgr->GetItemTemplate(r.item);
                if (p->AddItem(r.item, r.count))
                    Log(p, Acore::StringFormat("|cffffd100You take {}{} from {}.|r", r.count > 1 ? std::to_string(r.count) + " x " : "",
                        t ? t->Name1 : "something", def.name));
                else
                    Log(p, Acore::StringFormat("|cffff8040Your bags are full: {} stays behind.|r", t ? t->Name1 : "a reward"));
            }
            if (r.title)
                if (CharTitlesEntry const* title = sCharTitlesStore.LookupEntry(r.title))
                {
                    if (!p->HasTitle(title))
                    {
                        p->SetTitle(title);
                        std::string name = title->nameMale[0] ? title->nameMale[0] : "";
                        if (std::size_t s = name.find("%s"); s != std::string::npos)
                            name.replace(s, 2, p->GetName());
                        Log(p, Acore::StringFormat("|cffff9040A new title: {}.|r", name));
                        Announce(p, Acore::StringFormat("{} is now known as {}.", p->GetName(), name));
                    }
                }
        }
    }

    // ------------------------------------------------------------------ attunement
    // attuned items worn, leaving out `except` (the item being moved) and whatever is in `exceptSlot` (it'd come off)
    int AttunedCount(Player* p, Item const* except, uint8 exceptSlot)
    {
        int n = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == exceptSlot)
                continue;
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (item && item != except && NeedsAttunement(item->GetEntry()))
                ++n;
        }
        return n;
    }

    // ------------------------------------------------------------------ elixirs
    uint32 ActiveElixir(Player* p)
    {
        auto itr = p ? sElixir.find(p->GetGUID()) : sElixir.end();
        return itr == sElixir.end() ? 0 : itr->second.item;
    }

    uint32 ElixirItemForSpell(uint32 spell)
    {
        auto itr = sElixirSpells.find(spell);
        return itr == sElixirSpells.end() ? 0 : itr->second;
    }

    void DrinkElixir(Player* p, uint32 item)
    {
        ItemTemplate const* t = sObjectMgr->GetItemTemplate(item);
        std::string name = t ? t->Name1 : "the elixir";
        if (uint32 old = ActiveElixir(p); old && old != item)
            if (ItemTemplate const* ot = sObjectMgr->GetItemTemplate(old))
                Log(p, Acore::StringFormat("|cff909090({} fades as the new one takes hold.)|r", ot->Name1));
        ElixirState& s = sElixir[p->GetGUID()];
        s.item = item;
        s.since = getMSTime();
        s.fought = InCombat(p);
        Log(p, Acore::StringFormat("|cff80c0ffYou drink {}. It lasts until the end of your next fight (or an hour).|r", name));
        if (InCombat(p))                          // drunk mid-fight: what it does at a fight's start, it does now
            if (int th = ItemTempHp(p); th > GetSheet(p).tempHp)
            {
                GetSheet(p).tempHp = th;
                Log(p, Acore::StringFormat("|cff80c0ff{} temporary hit points.|r", th));
            }
        PushSheet(p);
    }

    void EndElixir(Player* p, std::string const& why)
    {
        auto itr = sElixir.find(p->GetGUID());
        if (itr == sElixir.end())
            return;
        ItemTemplate const* t = sObjectMgr->GetItemTemplate(itr->second.item);
        sElixir.erase(itr);
        if (!why.empty())
            Log(p, Acore::StringFormat("|cff909090{} {}.|r", t ? t->Name1 : "Your elixir", why));
        PushSheet(p);
    }

    void UpdateElixir(Player* p, uint32 /*diff*/)
    {
        auto itr = sElixir.find(p->GetGUID());
        if (itr == sElixir.end())
            return;
        if (InCombat(p))
            itr->second.fought = true;
        else if (itr->second.fought)
            EndElixir(p, "wears off with the fight");
        else if (getMSTimeDiff(itr->second.since, getMSTime()) > HOUR * IN_MILLISECONDS)
            EndElixir(p, "wears off");
    }

    void ForgetElixir(ObjectGuid g) { sElixir.erase(g); }

    // ------------------------------------------------------------------ portals
    bool PortalSend(Player* p, Creature* keeper, uint32 idx, std::string& why)
    {
        auto itr = sPortals.find(keeper->GetEntry());
        if (itr == sPortals.end() || idx >= itr->second.size())
            return false;
        PortalDest const& d = itr->second[idx];
        if (InCombat(p) || IsDowned(p) || !p->IsAlive())
        {
            why = "Not in the middle of a fight.";
            return false;
        }
        if (ZoneDef const* z = d.zone ? GetZone(d.zone) : nullptr; z && z->minLevel >= CLOSED_ZONE_LEVEL)
        {
            why = "That way isn't open yet.";
            return false;
        }
        if (p->GetLevel() < d.level && !p->IsGameMaster())
        {
            why = Acore::StringFormat("Only an adventurer of level {} or more is sent that way.", d.level);
            return false;
        }
        if (!d.say.empty())
            Log(p, Acore::StringFormat("|cffc8b890{}: \"{}\"|r", keeper->GetName(), d.say));
        keeper->HandleEmoteCommand(EMOTE_ONESHOT_SPELL_CAST_OMNI);
        GrantGrace(p, 8000);                     // a moment to get your bearings on the far side
        p->TeleportTo(d.map, d.x, d.y, d.z, d.o);
        return true;
    }
}

using namespace Dnd;

// a portal keeper: lists where they can send you
class npc_dnd_portal : public CreatureScript
{
public:
    npc_dnd_portal() : CreatureScript("npc_dnd_portal") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);
        auto itr = sPortals.find(creature->GetEntry());
        if (itr == sPortals.end() || InCombat(player))
        {
            CloseGossipMenuFor(player);
            return true;
        }
        for (std::size_t i = 0; i < itr->second.size(); ++i)
        {
            PortalDest const& d = itr->second[i];
            std::string label = d.label;
            if (ZoneDef const* z = d.zone ? GetZone(d.zone) : nullptr; z && z->minLevel >= CLOSED_ZONE_LEVEL)
                label += " (not open yet)";
            else if (player->GetLevel() < d.level)
                label += Acore::StringFormat(" (level {})", d.level);
            AddGossipItemFor(player, GOSSIP_ICON_TAXI, label, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_INFO_DEF + uint32(i));
        }
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        std::string why;
        if (!PortalSend(player, creature, action - GOSSIP_ACTION_INFO_DEF, why) && !why.empty())
            Error(player, why);
        return true;
    }
};

// an elixir: its effects worn until the end of the next fight; a Bonus Action in one, like a potion. The stock aura
// never lands (the D&D effects stand in for it)
class spell_dnd_elixir : public SpellScript
{
    PrepareSpellScript(spell_dnd_elixir);

    SpellCastResult CheckCast()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        std::string why;
        if (p && IsDowned(p))
            why = "You're down - you can't act.";
        if (why.empty() && (!p || CanPay(p, COST_BONUS, why)))
            return SPELL_CAST_OK;
        Error(p, why);
        return SPELL_FAILED_DONT_REPORT;
    }

    void AfterHitHandler()
    {
        if (Unit* u = GetHitUnit())
            u->RemoveAurasDueToSpell(GetSpellInfo()->Id);
    }

    void AfterCastHandler()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!p)
            return;
        p->RemoveAurasDueToSpell(GetSpellInfo()->Id);
        if (uint32 item = ElixirItemForSpell(GetSpellInfo()->Id))
            DrinkElixir(p, item);
        if (InCombat(p))
        {
            Pay(p, COST_BONUS);
            PushEconomy(p);
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_dnd_elixir::CheckCast);
        AfterHit += SpellHitFn(spell_dnd_elixir::AfterHitHandler);
        AfterCast += SpellCastFn(spell_dnd_elixir::AfterCastHandler);
    }
};

class DndEndgamePlayerScript : public PlayerScript
{
public:
    DndEndgamePlayerScript() : PlayerScript("DndEndgamePlayerScript") { }

    // attunement: a fourth magic item with effects won't go on
    bool OnPlayerCanEquipItem(Player* p, uint8 slot, uint16& /*dest*/, Item* item, bool swap, bool notLoading) override
    {
        if (!notLoading || !item || !NeedsAttunement(item->GetEntry()))
            return true;
        uint8 into = slot != NULL_SLOT ? slot : p->FindEquipSlot(item->GetTemplate(), NULL_SLOT, swap);
        if (AttunedCount(p, item, into) < ATTUNEMENT_SLOTS)
            return true;
        Error(p, Acore::StringFormat("You can only be attuned to {} magic items at once - take one off first.", ATTUNEMENT_SLOTS));
        return false;
    }

    void OnPlayerUpdate(Player* p, uint32 diff) override { UpdateElixir(p, diff); }
    void OnPlayerLogout(Player* p) override { ForgetElixir(p->GetGUID()); }
};

void AddSC_dnd_endgame()
{
    new npc_dnd_portal();
    RegisterSpellScript(spell_dnd_elixir);
    new DndEndgamePlayerScript();
}
