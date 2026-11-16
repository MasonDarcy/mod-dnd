/*
 * mod-dnd classes: the table of the six classes (the WoW class each rides on, hit die, saves, casting ability, skills,
 * subclasses) and the class rules that aren't a spell of their own - Unarmored Defense, fast movement, the resource
 * pools (rage, ki, sorcery points), Hide, metamagic and wild magic surges.
 *
 *   Wizard    (Mage)     d6   INT/WIS  INT caster    subclass at 2: Abjuration / Evocation
 *   Fighter   (Warrior)  d10  STR/CON               subclass at 3: Champion / Battle Master
 *   Barbarian (Shaman)   d12  STR/CON               subclass at 3: Berserker / Bear Totem
 *   Rogue     (Rogue)    d8   DEX/INT               subclass at 3: Thief / Assassin
 *   Monk      (Druid)    d8   STR/DEX  ki: WIS      subclass at 3: Open Hand / Shadow
 *   Warlock   (Warlock)  d8   WIS/CHA  CHA pact magic subclass at 1: Fiend / Great Old One / Archfey
 *   Bard      (Paladin)  d8   DEX/CHA  CHA caster    subclass at 3: Lore / Valor
 *   Druid     (Hunter)   d8   INT/WIS  WIS caster    subclass at 2: Land / Moon
 *   Cleric    (Priest)   d8   WIS/CHA  WIS caster    subclass at 1: Life / Tempest
 * (The sorcerer's rules - metamagic, wild magic, the draconic bloodline - are still in the code, unused: the Priest slot it
 * rode on is the cleric's now.)
 * Race limits are lifted in the client patch (CharBaseInfo) and playercreateinfo, so dwarves and gnomes can be any of them.
 */
#include "Dnd.h"

#include "Creature.h"
#include "GameTime.h"
#include "Item.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuras.h"
#include "StringFormat.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include <algorithm>

namespace Dnd
{
    namespace
    {
        ClassDef const sClasses[] =
        {
            //  id               name         wow class      die  saves      cast   subclass lvl  subclasses
            { CLASS_WIZARD,    "Wizard",    CLASS_MAGE,    6,  INT, WIS,  INT,   2, { "Abjuration", "Evocation" },
              { 8, 14, 14, 17, 12, 10 },
              { "Arcana", "History", "Insight", "Investigation", "Medicine", "Religion" } },
            { CLASS_FIGHTER,   "Fighter",   CLASS_WARRIOR, 10, STR, CON,  ABILITY_COUNT, 3, { "Champion", "Battle Master" },
              { 17, 13, 15, 8, 12, 10 },
              { "Acrobatics", "Animal Handling", "Athletics", "History", "Insight", "Intimidation", "Perception", "Survival" } },
            { CLASS_BARBARIAN, "Barbarian", CLASS_SHAMAN,  12, STR, CON,  ABILITY_COUNT, 3, { "Berserker", "Bear Totem" },
              { 17, 13, 15, 8, 12, 10 },
              { "Animal Handling", "Athletics", "Intimidation", "Nature", "Perception", "Survival" } },
            { CLASS_DND_ROGUE, "Rogue",     ::CLASS_ROGUE, 8,  DEX, INT,  ABILITY_COUNT, 3, { "Thief", "Assassin" },
              { 8, 17, 14, 13, 12, 10 },
              { "Acrobatics", "Athletics", "Deception", "Insight", "Intimidation", "Investigation", "Perception", "Performance",
                "Persuasion", "Sleight of Hand", "Stealth" } },
            { CLASS_MONK,      "Monk",      CLASS_DRUID,   8,  STR, DEX,  ABILITY_COUNT, 3, { "Open Hand", "Shadow" },
              { 10, 17, 13, 8, 15, 10 },
              { "Acrobatics", "Athletics", "History", "Insight", "Religion", "Stealth" } },
            { CLASS_DND_WARLOCK, "Warlock", CLASS_WARLOCK, 8,  WIS, CHA,  CHA,   1, { "Fiend", "Great Old One", "Archfey" },
              { 8, 14, 15, 10, 12, 17 },
              { "Arcana", "Deception", "History", "Intimidation", "Investigation", "Nature", "Religion" } },
            { CLASS_BARD,      "Bard",      CLASS_PALADIN, 8,  DEX, CHA,  CHA,   3, { "Lore", "Valor" },
              { 8, 14, 14, 10, 12, 17 },
              { "Acrobatics", "Animal Handling", "Arcana", "Athletics", "Deception", "History", "Insight", "Intimidation",
                "Investigation", "Medicine", "Nature", "Perception", "Performance", "Persuasion", "Religion", "Sleight of Hand",
                "Stealth", "Survival" } },
            { CLASS_DND_DRUID, "Druid",     CLASS_HUNTER,  8,  INT, WIS,  WIS,   2, { "Land", "Moon" },
              { 8, 14, 14, 10, 17, 10 },
              { "Arcana", "Animal Handling", "Insight", "Medicine", "Nature", "Perception", "Religion", "Survival" } },
            { CLASS_CLERIC,    "Cleric",    CLASS_PRIEST,  8,  WIS, CHA,  WIS,   1, { "Life", "Tempest" },
              { 14, 8, 14, 10, 17, 10 },
              { "History", "Insight", "Medicine", "Persuasion", "Religion" } },
        };

        // metamagic armed for the next spell (per character)
        struct Meta { bool quicken = false, twin = false, heighten = false, empower = false, bend = false, tides = false; };
        std::unordered_map<ObjectGuid, Meta> sMeta;
        // the armed metamagic that went with a cast, for its hits (missiles land after the cast is over)
        struct CastMeta { Meta m; uint32 spell = 0; uint32 untilMs = 0; };
        std::unordered_map<ObjectGuid, CastMeta> sCastMeta;

        uint32 NowMs() { return GameTime::GetGameTimeMS().count(); }
    }

    ClassDef const* ClassInfo(uint8 cls)
    {
        for (ClassDef const& c : sClasses)
            if (c.id == cls)
                return &c;
        return &sClasses[0];
    }

    ClassDef const* ClassInfo(Player* p) { return ClassInfo(GetSheet(p).cls); }

    // ---------------------------------------------------------------- the bard: Bardic Inspiration
    namespace { std::unordered_map<ObjectGuid, int> sInspired; }   // an ally -> the die they hold

    int InspirationMax(Player* p) { return IsClass(p, CLASS_BARD) ? std::max(1, Mod(GetSheet(p).abil[CHA])) : 0; }
    int InspirationDie(Player* p) { return Level(p) >= 5 ? 8 : 6; }

    void Inspire(Player* bard, Player* ally)
    {
        ++GetSheet(bard).inspirationUsed;
        SaveSheet(bard);
        PushSheet(bard);
        int die = InspirationDie(bard);
        sInspired[ally->GetGUID()] = std::max(die, HeldInspiration(ally));
        Log(ally, Acore::StringFormat("|cffffd100{} inspires you: a d{} for your next attack, save or check that would fall "
            "short.|r", bard->GetName(), die));
        Log(bard, Acore::StringFormat("|cffffd100You inspire {} (d{}). {} left.|r", ally->GetName(), die,
            InspirationMax(bard) - GetSheet(bard).inspirationUsed));
    }

    int HeldInspiration(Player* p)
    {
        auto itr = p ? sInspired.find(p->GetGUID()) : sInspired.end();
        return itr == sInspired.end() ? 0 : itr->second;
    }

    int SpendInspiration(Player* p, std::string const& on)
    {
        auto itr = sInspired.find(p->GetGUID());
        if (itr == sInspired.end())
            return 0;
        int roll = D(itr->second);
        sInspired.erase(itr);
        Log(p, Acore::StringFormat("|cffffd100Bardic Inspiration: +{} on {}.|r", roll, on));
        return roll;
    }

    // ---------------------------------------------------------------- the druid: Wild Shape
    // A beast's AC, attacks and speed replace your own; its hit points are a pool in front of yours (shown as temporary
    // hit points): when they run out you change back, and what's left of the blow carries over. No spells while shaped.
    namespace
    {
        BeastForm const sForms[] =
        {
            //  spell                    name          display ac  hp speed  attack        bon  dice     type          prone 2nd attack  dice2    type2
            { SPELL_SHAPE_WOLF,        "Wolf",        247,   13, 11, 12.f, "Bite",          4, "2d4+2", DMG_PIERCING, 11, nullptr, nullptr, DMG_PIERCING },
            { SPELL_SHAPE_BEAR,        "Brown Bear",  1006,  11, 34, 12.f, "Claws",         6, "2d6+4", DMG_SLASHING, 0,  "Bite",  "1d8+4", DMG_PIERCING },
            { SPELL_SHAPE_DIRE_WOLF,   "Dire Wolf",   11421, 14, 37, 15.f, "Bite",          5, "2d6+3", DMG_PIERCING, 13, nullptr, nullptr, DMG_PIERCING },
            { SPELL_SHAPE_SABRETOOTH,  "Sabre-tooth", 320,   12, 52, 12.f, "Claws",         6, "2d6+5", DMG_SLASHING, 14, "Bite",  "1d10+5", DMG_PIERCING },
        };
        struct Shape { uint32 form = 0; int savedTempHp = 0; };
        std::unordered_map<ObjectGuid, Shape> sShapes;
    }

    BeastForm const* GetBeastForm(uint32 spell)
    {
        for (BeastForm const& f : sForms)
            if (f.spell == spell)
                return &f;
        return nullptr;
    }

    BeastForm const* WildShapeOf(Player* p)
    {
        auto itr = p ? sShapes.find(p->GetGUID()) : sShapes.end();
        return itr == sShapes.end() ? nullptr : GetBeastForm(itr->second.form);
    }

    int WildShapeMax(Player* p) { return IsClass(p, CLASS_DND_DRUID) && Level(p) >= 2 ? 2 : 0; }

    // ---------------------------------------------------------------- the cleric
    int ChannelMax(Player* p) { return IsClass(p, CLASS_CLERIC) && Level(p) >= 2 ? (Level(p) >= 6 ? 2 : 1) : 0; }
    int WrathMax(Player* p) { return IsClass(p, CLASS_CLERIC) && IsSubclass(p, "Tempest") ? std::max(1, Mod(GetSheet(p).abil[WIS])) : 0; }
    int HealBonus(Player* p, int spellLevel)
    {
        return (IsClass(p, CLASS_CLERIC) && IsSubclass(p, "Life") && spellLevel >= 1 ? 2 + spellLevel : 0) +
            (IsSubrace(p, "Anchorite") && spellLevel >= 1 ? 2 : 0);     // Healing Hands (draenei anchorites)
    }

    void StartWildShape(Player* p, uint32 spell)
    {
        BeastForm const* f = GetBeastForm(spell);
        if (!f)
            return;
        if (WildShapeOf(p))
            EndWildShape(p, "");
        Sheet& s = GetSheet(p);
        ++s.wildShapeUsed;
        sShapes[p->GetGUID()] = { spell, s.tempHp };
        s.tempHp = f->hp;
        p->SetDisplayId(f->display);
        SaveSheet(p);
        PushSheet(p);
        Log(p, Acore::StringFormat("|cff80ff80You take the shape of a {}: AC {}, {} hit points of its own, {} {}d... ({} Wild Shape "
            "left).|r", f->name, f->ac, f->hp, f->attack, f->dice, WildShapeMax(p) - s.wildShapeUsed));
        if (Encounter* e = EncounterOf(p->GetGUID()))
            PushOrder(*e);
    }

    void EndWildShape(Player* p, std::string const& why)
    {
        auto itr = p ? sShapes.find(p->GetGUID()) : sShapes.end();
        if (itr == sShapes.end())
            return;
        BeastForm const* f = GetBeastForm(itr->second.form);
        Sheet& s = GetSheet(p);
        s.tempHp = itr->second.savedTempHp;  // the beast's pool goes; your own temporary hit points come back
        sShapes.erase(itr);
        p->DeMorph();
        SaveSheet(p);
        PushSheet(p);
        Log(p, Acore::StringFormat("|cff80ff80You return to your own shape{}.|r", why.empty() && f ? "" : why));
        if (Encounter* e = EncounterOf(p->GetGUID()))
            PushOrder(*e);
    }

    uint8 DndClassForWow(uint8 wowClass)
    {
        for (ClassDef const& c : sClasses)
            if (c.wowClass == wowClass)
                return c.id;
        return CLASS_WIZARD;
    }

    bool IsClass(Player* p, uint8 cls) { return p && GetSheet(p).cls == cls; }
    bool IsCaster(Player* p) { return p && ClassInfo(p)->cast != ABILITY_COUNT; }

    // body armour on? (anything with real armour value in the chest slot)
    static bool WearsArmour(Player* p)
    {
        Item* chest = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST);
        return chest && chest->GetTemplate()->Class == ITEM_CLASS_ARMOR && chest->GetTemplate()->Armor >= 10;
    }

    static bool HasShield(Player* p)
    {
        Item* off = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        return off && off->GetTemplate()->Class == ITEM_CLASS_ARMOR && off->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD;
    }

    // Unarmored Defense / Draconic Resilience: the base AC without armour (0: no such feature)
    int UnarmoredBase(Player* p)
    {
        if (WearsArmour(p))
            return 0;
        Sheet& s = GetSheet(p);
        switch (s.cls)
        {
            case CLASS_BARBARIAN: return 10 + Mod(s.abil[DEX]) + Mod(s.abil[CON]);
            case CLASS_MONK:      return HasShield(p) ? 0 : 10 + Mod(s.abil[DEX]) + Mod(s.abil[WIS]);
            case CLASS_SORCERER:  return s.subclass.rfind("Draconic", 0) == 0 ? 13 + Mod(s.abil[DEX]) : 0;
            default:              return 0;
        }
    }

    float ClassSpeedBonus(Player* p)
    {
        Sheet& s = GetSheet(p);
        int lvl = Level(p);
        if (s.cls == CLASS_BARBARIAN && lvl >= 5 && !WearsHeavyArmour(p))
            return 3.f;                              // Fast Movement
        if (s.cls == CLASS_MONK && lvl >= 2 && !WearsArmour(p) && !HasShield(p))
            return lvl >= 6 ? 4.5f : 3.f;            // Unarmored Movement
        return 0.f;
    }

    int ClassHpBonus(Player* p)
    {
        Sheet& s = GetSheet(p);
        return s.cls == CLASS_SORCERER && s.subclass.rfind("Draconic", 0) == 0 ? Level(p) : 0;     // Draconic Resilience
    }

    // the dragon's element for a Draconic sorcerer (DMG_TYPE_COUNT: none)
    DmgType DraconicElement(Player* p)
    {
        std::string const& sub = GetSheet(p).subclass;
        if (sub == "Draconic Red")
            return DMG_FIRE;
        if (sub == "Draconic White")
            return DMG_COLD;
        if (sub == "Draconic Blue")
            return DMG_LIGHTNING;
        return DMG_TYPE_COUNT;
    }

    // ---------------------------------------------------------------- resources
    int RageMax(Player* p) { return IsClass(p, CLASS_BARBARIAN) ? (Level(p) >= 6 ? 4 : (Level(p) >= 3 ? 3 : 2)) : 0; }
    int KiMax(Player* p) { return IsClass(p, CLASS_MONK) && Level(p) >= 2 ? Level(p) : 0; }
    int SorceryMax(Player* p) { return IsClass(p, CLASS_SORCERER) && Level(p) >= 2 ? Level(p) : 0; }
    int SneakDice(Player* p) { return IsClass(p, CLASS_DND_ROGUE) ? (Level(p) + 1) / 2 : 0; }
    int MartialDie(Player* p) { return Level(p) >= 5 ? 6 : 4; }
    int KiDC(Player* p) { return 8 + Prof(Level(p)) + Mod(GetSheet(p).abil[WIS]); }

    bool HasExpertise(Player* p, std::string const& skill)
    {
        if (IsClass(p, CLASS_BARD))
            return Level(p) >= 3 && (skill == "Persuasion" || skill == "Deception");   // the bard's Expertise
        if (!IsClass(p, CLASS_DND_ROGUE))
            return false;
        // Expertise: Stealth and Sleight of Hand at 1, Perception and Investigation at 6
        if (skill == "Stealth" || skill == "Sleight of Hand")
            return true;
        return Level(p) >= 6 && (skill == "Perception" || skill == "Investigation");
    }

    // ---------------------------------------------------------------- Hide
    // A Stealth check against the best passive Perception (10 + WIS) of the enemies in the fight. Hidden: your next
    // attack has advantage, attacks against you have disadvantage; it ends when you attack. Not with an enemy at
    // your elbow.
    bool TryHide(Player* p, std::string& why)
    {
        Encounter* e = EncounterOf(p->GetGUID());
        Combatant* me = e ? e->Find(p->GetGUID()) : nullptr;
        int best = 10;
        if (e)
            for (Combatant const& c : e->order)
            {
                if (c.player || c.gone || !c.alive)
                    continue;
                Unit* u = e->GetUnit(c);
                if (!u || !u->IsAlive())
                    continue;
                if (u->GetExactDist(p) <= MeleeRange(u, p))
                {
                    why = "Not with an enemy right next to you.";
                    return false;
                }
                best = std::max(best, 10 + AbilityMod(u, WIS));
            }
        int bonus = SkillBonusByName(p, "Stealth");
        D20Result r = RollD20(bonus, 0);
        bool ok = r.total >= best;
        Send(p, Acore::StringFormat("TD^Stealth^{}^{}^{}^{}", r.natural, bonus, best, ok ? 1 : 0));
        Log(p, Acore::StringFormat("   Stealth {} vs passive Perception {}: {}", r.text, best, ok ? "|cff40ff40hidden|r" : "|cffff4040seen|r"));
        if (ok)
        {
            if (me)
                me->hidden = true;
            p->AddAura(SPELL_HIDDEN_AURA, p);
        }
        return true;
    }

    void Unhide(Unit* u)
    {
        if (Combatant* c = CombatantOf(u->GetGUID()))
            c->hidden = false;
        u->RemoveAurasDueToSpell(SPELL_HIDDEN_AURA);
    }

    // ---------------------------------------------------------------- metamagic
    bool MetaArmed(Player* p, char const* which)
    {
        auto itr = sMeta.find(p->GetGUID());
        if (itr == sMeta.end())
            return false;
        Meta const& m = itr->second;
        std::string w = which;
        return w == "quicken" ? m.quicken : w == "twin" ? m.twin : w == "heighten" ? m.heighten : w == "empower" ? m.empower :
            w == "bend" ? m.bend : w == "tides" ? m.tides : false;
    }

    void SetMeta(Player* p, char const* which, bool on)
    {
        Meta& m = sMeta[p->GetGUID()];
        std::string w = which;
        (w == "quicken" ? m.quicken : w == "twin" ? m.twin : w == "heighten" ? m.heighten : w == "empower" ? m.empower :
            w == "bend" ? m.bend : m.tides) = on;
    }

    // a spell is cast: its armed metamagic travels with it to the hits; quicken / tides are spent at the cast
    void TakeMeta(Player* p, uint32 spellId)
    {
        auto itr = sMeta.find(p->GetGUID());
        if (itr == sMeta.end())
            return;
        CastMeta& cm = sCastMeta[p->GetGUID()];
        cm.m = itr->second;
        cm.m.tides = false;
        cm.spell = spellId;
        cm.untilMs = NowMs() + 4000;
        bool tides = itr->second.tides;
        itr->second = Meta();
        itr->second.tides = tides;              // Tides of Chaos is for an attack roll, not a spell as such
    }

    bool CastHas(Player* p, uint32 spellId, char const* which)
    {
        auto itr = sCastMeta.find(p->GetGUID());
        if (itr == sCastMeta.end() || itr->second.spell != spellId || int32(NowMs() - itr->second.untilMs) > 0)
            return false;
        Meta const& m = itr->second.m;
        std::string w = which;
        return w == "twin" ? m.twin : w == "heighten" ? m.heighten : w == "empower" ? m.empower : w == "bend" ? m.bend :
            w == "quicken" ? m.quicken : false;
    }

    void UseCastMeta(Player* p, char const* which)
    {
        auto itr = sCastMeta.find(p->GetGUID());
        if (itr == sCastMeta.end())
            return;
        std::string w = which;
        if (w == "twin")
            itr->second.m.twin = false;
        else if (w == "quicken")
            itr->second.m.quicken = false;   // paid for: the next spell takes its Action again
    }

    // ---------------------------------------------------------------- sneaking (out of combat)
    namespace
    {
        struct Sneak { int roll = 0; bool struck = false; bool revealed = false; std::unordered_map<ObjectGuid, bool> seen; };
        std::unordered_map<ObjectGuid, Sneak> sSneak;

        // the sneak, if any - and if the stealth look was taken off some other way (cancelled, dispelled), the sneaking
        // ends with it: what the player sees and what the creatures roll against must agree. Not while a strike from
        // hiding is still resolving (ResolveAmbush puts the look back if no one saw it).
        std::unordered_map<ObjectGuid, Sneak>::iterator FindSneak(Player* p)
        {
            auto itr = sSneak.find(p->GetGUID());
            if (itr != sSneak.end() && !itr->second.struck && p->IsInWorld() && !p->HasAura(SPELL_SNEAK_AURA))
            {
                sSneak.erase(itr);
                return sSneak.end();
            }
            return itr;
        }
    }

    void StartSneaking(Player* p)
    {
        int bonus = SkillBonusByName(p, "Stealth");
        D20Result r = RollD20(bonus, 0);
        Sneak& s = sSneak[p->GetGUID()];
        s = Sneak();
        s.roll = r.total;
        p->AddAura(SPELL_SNEAK_AURA, p);
        Send(p, "V^SNEAKING");               // (no dice popup: nothing to beat yet - each creature's roll shows on its own card)
        Log(p, Acore::StringFormat("|cff9ad0ffYou slip into the shadows:|r Stealth {}. Enemies who come close roll Perception "
            "against {}; strike first and you strike with advantage.", r.text, r.total));
    }

    bool IsSneaking(Player* p) { return p && FindSneak(p) != sSneak.end(); }

    bool SneakStruck(Player* p)
    {
        auto itr = p ? FindSneak(p) : sSneak.end();
        return itr != sSneak.end() && itr->second.struck;
    }

    void MarkSneakStruck(Player* p)
    {
        if (auto itr = FindSneak(p); itr != sSneak.end())
            itr->second.struck = true;
    }

    void ClearSneakStruck(Player* p)
    {
        if (auto itr = FindSneak(p); itr != sSneak.end())
            itr->second.struck = false;
    }

    void StopSneaking(Player* p, std::string const& why)
    {
        if (!p || !sSneak.erase(p->GetGUID()))
            return;
        p->RemoveAurasDueToSpell(SPELL_SNEAK_AURA);
        if (!why.empty())
            Log(p, why);
    }

    bool SpotsSneaker(Creature* c, Player* p, int adv)
    {
        auto itr = p ? FindSneak(p) : sSneak.end();
        if (itr == sSneak.end() || !c)
            return true;
        Sneak& s = itr->second;
        if (auto seen = s.seen.find(c->GetGUID()); seen != s.seen.end())
            return seen->second;
        D20Result r = RollD20(AbilityMod(c, WIS), adv);     // Perception: a creature's Wisdom
        bool spotted = r.total >= s.roll;
        s.seen[c->GetGUID()] = spotted;
        Log(p, Acore::StringFormat("   {} Perception {} vs your Stealth {}: {}", c->GetName(), r.text, s.roll,
            spotted ? "|cffff4040it spots you!|r" : "|cff40ff40it doesn't notice you.|r"));
        // a roll card in the feed ("RS" with Perception as the ability: saved = it spotted you)
        Send(p, Acore::StringFormat("RS^{}^Perception^your Stealth^{}^{}^{}^{}^{}^0", c->GetName(), r.natural, AbilityMod(c, WIS),
            s.roll, spotted ? 1 : 0, adv));
        if (spotted)
            Send(p, "V^SPOTTED!");
        return spotted;
    }

    static float const SIGHT_CONE = float(M_PI) * 2.f / 3.f;     // 120 degrees ahead
    static float const POINT_BLANK = 3.f;                        // right in front of it: no roll, it sees you

    bool InSightCone(Creature* c, Unit* u)
    {
        return c && u && c->HasInArc(SIGHT_CONE, u) && c->IsWithinLOSInMap(u);
    }

    bool NoticesSneaker(Creature* c, Player* p)
    {
        auto itr = p ? FindSneak(p) : sSneak.end();
        if (itr == sSneak.end() || !c)
            return true;
        if (!InSightCone(c, p))
            return false;                    // behind it, or out of its sight: it can't notice you (yet)
        if (c->GetExactDist(p) <= POINT_BLANK)
        {
            itr->second.seen[c->GetGUID()] = true;
            Log(p, Acore::StringFormat("   You walked right into {}'s view - |cffff4040it sees you!|r", c->GetName()));
            Send(p, "V^SPOTTED!");
            return true;
        }
        return SpotsSneaker(c, p);
    }

    void SneakKillWitnessed(Player* p, Unit* victim)
    {
        auto itr = p ? FindSneak(p) : sSneak.end();
        if (itr == sSneak.end() || !victim)
            return;
        std::list<Unit*> nearby;
        Acore::AnyUnitInObjectRangeCheck check(victim, 20.f);
        Acore::UnitListSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(victim, nearby, check);
        Cell::VisitObjects(victim, searcher, 20.f);
        for (Unit* u : nearby)
        {
            Creature* c = u->ToCreature();
            if (!c || c == victim || !c->IsAlive() || InCombat(c) || c->IsPet() ||
                !(c->IsHostileTo(p) || GetEncounterDefBySpawn(c->GetSpawnId())))
                continue;
            if (!InSightCone(c, p) && !InSightCone(c, victim))
                continue;                    // facing away: it didn't see a thing
            itr->second.seen.erase(c->GetGUID());
            Log(p, Acore::StringFormat("   {} saw that - it looks again, alert (advantage).", c->GetName()));
            if (SpotsSneaker(c, p, 1))
            {
                StartEncounter(c, p);        // the fight starts; whoever still hasn't seen you is Surprised
                return;
            }
            itr = FindSneak(p);
            if (itr == sSneak.end())
                return;
        }
    }

    bool SneakRevealed(Player* p)
    {
        auto itr = p ? FindSneak(p) : sSneak.end();
        return itr != sSneak.end() && itr->second.revealed;
    }

    void ResolveAmbush(Player* p, Unit* victim)
    {
        auto itr = p ? sSneak.find(p->GetGUID()) : sSneak.end();
        if (itr == sSneak.end())
            return;
        if (!p->HasAura(SPELL_SNEAK_AURA))
            p->AddAura(SPELL_SNEAK_AURA, p);     // (if the strike took the look off: back on until the fight, if any, starts)
        itr->second.struck = false;
        itr->second.revealed = true;         // you attacked: whoever fights you now knows where you are
        if (victim && victim->IsAlive() && victim->ToCreature())
        {
            StartEncounter(victim->ToCreature(), p);
            return;
        }
        SneakKillWitnessed(p, victim);
        if (auto again = FindSneak(p); again != sSneak.end() && !InCombat(p))
        {
            again->second.revealed = false;
            Log(p, "|cff9ad0ffNo one saw it - you're still hidden.|r");
            if (victim)
                OnUnseenKill(p, victim->ToCreature());    // the last of the group: the encounter is won
        }
    }

    bool StrengthCheckAdvantage(Player* p)
    {
        if (!IsClass(p, CLASS_BARBARIAN))
            return false;
        if (Level(p) >= 6 && IsSubclass(p, "Bear Totem"))
            return true;
        Combatant* c = CombatantOf(p->GetGUID());
        return c && c->raging;
    }

    void ForgetClasses(ObjectGuid g) { sMeta.erase(g); sCastMeta.erase(g); sSneak.erase(g); sShapes.erase(g); sInspired.erase(g); }

    // ---------------------------------------------------------------- the warlock
    // Pact Magic: 1 slot at level 1, 2 from level 2; their level is 1, 2 at 3, 3 at 5 - every spell goes off at it
    int PactLevel(Player* p)
    {
        if (!IsClass(p, CLASS_DND_WARLOCK))
            return 0;
        int lvl = Level(p);
        return lvl >= 5 ? 3 : (lvl >= 3 ? 2 : 1);
    }
    int PactSlots(Player* p) { return IsClass(p, CLASS_DND_WARLOCK) ? (Level(p) >= 2 ? 2 : 1) : 0; }

    static bool HasPick(Player* p, std::string const& want)
    {
        for (std::string const& k : GetSheet(p).picks)
            if (StringEqualI(k, want))
                return true;
        return false;
    }
    bool HasInvocation(Player* p, char const* name) { return IsClass(p, CLASS_DND_WARLOCK) && HasPick(p, std::string("inv:") + name); }
    bool HasPact(Player* p, char const* name) { return IsClass(p, CLASS_DND_WARLOCK) && HasPick(p, std::string("pact:") + name); }

    int CastLevel(Player* p, int spellLevel)
    {
        return IsClass(p, CLASS_DND_WARLOCK) ? std::max(spellLevel, PactLevel(p)) : spellLevel;
    }

    int SlotToSpend(Player* p, uint32 spellId, int spellLevel)
    {
        if (spellLevel < 1)
            return 0;
        if ((spellId == SPELL_MAGE_ARMOR && HasInvocation(p, "Armor of Shadows")) ||
            (spellId == SPELL_FALSE_LIFE && HasInvocation(p, "Fiendish Vigour")))
            return 0;                        // invocations: at will
        if (spellId == SPELL_HEX)
            if (Combatant* c = CombatantOf(p->GetGUID()); c && c->hexFree)
                return 0;                    // the hexed creature died: the curse moves on for free
        if (spellId == SPELL_CALL_LIGHTNING || spellId == SPELL_WITCH_BOLT || spellId == SPELL_AURA_OF_VITALITY)
            if (Combatant* c = CombatantOf(p->GetGUID()); c && c->concentration == spellId)
                return 0;                    // the storm is already overhead / the arc still holds: again, no slot
        if (spellId == SPELL_SPIRITUAL_WEAPON)
            if (Combatant* c = CombatantOf(p->GetGUID()); c && c->spiritWeapon)
                return 0;                    // the weapon is already out: it strikes again for nothing
        return IsClass(p, CLASS_DND_WARLOCK) ? PactLevel(p) : spellLevel;
    }

    // ---------------------------------------------------------------- wild magic
    // After a 1st-level or higher spell: d20, on a 1 the Weave lashes out (a d8 on the table). Tides of Chaos comes back.
    void WildSurge(Player* p)
    {
        if (!IsSubclass(p, "Wild Magic"))
            return;
        if (D(20) != 1)
            return;
        Sheet& s = GetSheet(p);
        s.tidesUsed = 0;
        int roll = D(8);
        std::string what;
        Encounter* e = EncounterOf(p->GetGUID());
        auto everyoneNear = [&](float r)
        {
            std::vector<Unit*> out;
            if (e)
            {
                for (Combatant const& c : e->order)
                    if (!c.gone && c.alive)
                        if (Unit* u = e->GetUnit(c))
                            if (u->IsAlive() && u->GetExactDist(p) <= r)
                                out.push_back(u);
            }
            else
                out.push_back(p);
            return out;
        };
        switch (roll)
        {
            case 1:
            {
                std::string d;
                int heal = Dice{ 2, 8, 0 }.Roll(false, d);
                ApplyHeal(p, p, heal, nullptr);
                what = Acore::StringFormat("warm light pours through you: {} healed", heal);
                break;
            }
            case 2:
            {
                what = "fire erupts from your skin (3d6 fire to everyone within 3 m - you too)";
                for (Unit* u : everyoneNear(3.f))
                {
                    std::string d;
                    DamageCtx dc;
                    dc.attacker = p;
                    dc.victim = u;
                    dc.amount = Dice{ 3, 6, 0 }.Roll(false, d);
                    dc.type = DMG_FIRE;
                    ApplyDamage(dc);
                }
                break;
            }
            case 3:
            {
                std::string d;
                s.tempHp = std::max(s.tempHp, Dice{ 2, 4, 4 }.Roll(false, d));
                what = Acore::StringFormat("a shimmering skin of force: {} temporary hit points", s.tempHp);
                break;
            }
            case 4:
            {
                float a = frand(0.f, 6.283f), dist = frand(6.f, 12.f);
                float x = p->GetPositionX() + std::cos(a) * dist, y = p->GetPositionY() + std::sin(a) * dist, z = p->GetPositionZ();
                p->UpdateGroundPositionZ(x, y, z);
                p->NearTeleportTo(x, y, z, p->GetOrientation());
                if (Combatant* c = CombatantOf(p->GetGUID()))
                    c->lastPos.Relocate(x, y, z);
                what = "space folds - you're somewhere else";
                break;
            }
            case 5:
                SetMeta(p, "empower", true);
                what = "power floods in: your next spell is Empowered, free";
                break;
            case 6:
                what = "the ground around you turns slick (everyone within 3 m: DEX save DC 13 or fall prone)";
                if (e)
                    for (Unit* u : everyoneNear(3.f))
                        if (Combatant* c = CombatantOf(u->GetGUID()))
                        {
                            D20Result r = RollD20(SaveBonus(u, DEX), 0);
                            if (r.total < 13 && !c->prone)
                            {
                                c->prone = true;
                                u->SetStandState(UNIT_STAND_STATE_SLEEP);
                            }
                        }
                break;
            case 7:
                SetMeta(p, "tides", true);
                what = "luck bends toward you: your next attack roll has advantage";
                break;
            default:
            {
                std::string d;
                int dmg = Dice{ 1, 6, 0 }.Roll(false, d);
                DamageCtx dc;
                dc.victim = p;
                dc.amount = dmg;
                dc.type = DMG_FORCE;
                ApplyDamage(dc);
                what = Acore::StringFormat("the magic bites back: {} force damage to you", dmg);
                break;
            }
        }
        SaveSheet(p);
        PushSheet(p);
        Log(p, "|cffd080ffWild Magic Surge!|r " + what + ".");
        Send(p, "V^WILD MAGIC");
    }
}
