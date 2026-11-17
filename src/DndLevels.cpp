/*
 * mod-dnd levelling 1-6 (MAX_DND_LEVEL): what each level brings, the choices and the level-up popup.
 *
 * Wizard
 *   2  Arcane Tradition  Abjuration: Arcane Ward (Mage Armor raises a ward of 2 x level + INT that soaks damage first;
 *                        later abjuration casts recharge it by 2 x the slot level)
 *                        Evocation: Empowered Evocation (+ INT to the damage of your evocation spells - 5e has it at
 *                        level 10; its level-2 feature only protects allies, and there are none yet)
 *   3  2nd-level spells (Scorching Ray, Misty Step, Shatter, Hold Person)
 *   4  Ability Score Improvement (+2 to one ability or +1 to two, max 20) or a feat (sFeatDefs below)
 *   5  3rd-level spells (Fireball, Lightning Bolt); proficiency +3; cantrips to two dice
 *   6  Abjuration: a short rest restores the Arcane Ward in full (5e's Projected Ward guards allies)
 *      Evocation: Potent Cantrip - damage cantrips that miss still deal half (BG3's version)
 * Fighter
 *   1  Fighting Style (Archery: +2 to ranged weapon attack rolls; Defense: +1 AC in armour; Dueling: +2 damage with a one-handed weapon and no second weapon;
 *      Great Weapon Fighting: reroll 1s and 2s on two-handed weapon damage) and Second Wind (bonus action: 1d10 +
 *      level HP, once per short rest)
 *   2  Action Surge (one more action this turn, once per short rest)
 *   3  Martial Archetype  Champion: Improved Critical (weapon attacks crit on 19-20)
 *                         Battle Master: 4 superiority dice (d8, back on a short rest) and three maneuvers - Trip,
 *                         Pushing and Menacing Attack (on a hit: + the die's damage, and a save against 8 + prof + STR/DEX)
 *   4  Ability Score Improvement or a feat
 *   5  Extra Attack (the Attack action swings twice); proficiency +3
 *   6  Ability Score Improvement or a feat (fighters get more of them)
 * Addon messages: LU^level^hp gained^max hp^slots^new spells^features (the popup), LP^choice (what's still to pick:
 * "style", "feat1", "subclass", "asi" or nothing). The addon answers "pick^style^Defense", "pick^subclass^Evocation",
 * "pick^asi^INT", "pick^feat^Tough", "pick^feat1^Alert" (every character's free feat at level 1, any race).
 */
#include "Dnd.h"
#include "ItemTemplate.h"
#include "Player.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include <algorithm>

namespace Dnd
{
    static std::vector<char const*> const sStyles = { "Archery", "Defense", "Dueling", "Great Weapon Fighting", "Two-Weapon Fighting" };

    // The feats (BG3's list, the ones whose mechanics the engine has). A pick is stored as "feat:Name" or
    // "feat:Name:Variant"; a half-feat's +1 goes to `fixed`, or to the variant when the variants are abilities.
    //   who: 0 anyone, 1 fighters only (armour feats: wizards have no armour training), 2 wizards only
    struct FeatDef { char const* name; char const* variants; char const* fixed; bool variantIsAbility; uint8 who; };
    static FeatDef const sFeatDefs[] =
    {
        { "Alert",                "",                                   nullptr, false, 0 },   // +5 initiative, can't be surprised
        { "Athlete",              "STR,DEX",                            nullptr, true,  0 },   // jump +50%
        { "Crossbow Expert",      "",                                   nullptr, false, 0 },   // no disadvantage in melee
        { "Durable",              "",                                   "CON",   false, 0 },   // short rests heal fully
        { "Elemental Adept",      "Fire,Cold,Lightning,Thunder,Acid",   nullptr, false, 0 },   // ignore resistance, 1s count as 2s
        { "Great Weapon Master",  "",                                   nullptr, false, 0 },   // melee crit / kill: bonus attack
        { "Heavy Armour Master",  "",                                   "STR",   false, 1 },   // -3 weapon damage in heavy armour
        { "Martial Adept",        "",                                   nullptr, false, 0 },   // +1 superiority die, the maneuvers
        { "Medium Armour Master", "",                                   nullptr, false, 3 },   // DEX cap 3 in medium armour
        { "Mobile",               "",                                   nullptr, false, 0 },   // +3 m, Dash past terrain, hit and run
        { "Resilient",            "STR,DEX,CON,INT,WIS,CHA",            nullptr, true,  0 },   // +1 and the save proficiency
        { "Savage Attacker",      "",                                   nullptr, false, 0 },   // melee damage rolled twice
        { "Shield Master",        "",                                   nullptr, false, 0 },   // +2 DEX saves with a shield
        { "Spell Sniper",         "",                                   nullptr, false, 2 },   // spell attacks crit on 19
        { "Tavern Brawler",       "STR,CON",                            nullptr, true,  0 },   // STR twice: unarmed, thrown
        { "Tough",                "",                                   nullptr, false, 0 },   // +2 HP per level
        { "War Caster",           "",                                   nullptr, false, 2 },   // advantage on concentration saves
        { "Weapon Master",        "STR,DEX",                            nullptr, true,  0 },   // every weapon proficient
    };

    static FeatDef const* FindFeat(std::string const& name)
    {
        for (FeatDef const& f : sFeatDefs)
            if (StringEqualI(name, f.name))
                return &f;
        return nullptr;
    }

    static int AbilityIndex(std::string const& name)
    {
        for (int i = 0; i < ABILITY_COUNT; ++i)
            if (StringEqualI(name, AbilityNames[i]))
                return i;
        return -1;
    }

    // "feat:Name[:Variant]" (or the level-1 "feat1:...") -> the ability the feat raised by 1 (-1: none)
    static int FeatAbility(std::string const& pick)
    {
        std::string rest = pick.substr(pick.find(':') + 1);
        std::size_t colon = rest.find(':');
        FeatDef const* f = FindFeat(rest.substr(0, colon));
        if (!f)
            return -1;
        if (f->fixed)
            return AbilityIndex(f->fixed);
        if (f->variantIsAbility && colon != std::string::npos)
            return AbilityIndex(rest.substr(colon + 1));
        return -1;
    }

    bool IsSubclass(Player* p, char const* name) { return StringEqualI(GetSheet(p).subclass, name); }

    // a feat taken at an ASI ("feat:Name[:Variant]") or the one every character starts with ("feat1:...")
    static std::string const* FeatPick(Player* p, char const* feat)
    {
        for (char const* prefix : { "feat:", "feat1:" })
        {
            std::string want = std::string(prefix) + feat;
            for (std::string const& k : GetSheet(p).picks)
                if (k.size() >= want.size() && StringEqualI(k.substr(0, want.size()), want) && (k.size() == want.size() || k[want.size()] == ':'))
                    return &k;
        }
        return nullptr;
    }

    // the level-1 feat (every character, whatever the race: a free pick on top of the racial traits)
    static bool HasOriginFeat(Player* p)
    {
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind("feat1:", 0) == 0)
                return true;
        return false;
    }

    bool HasFeat(Player* p, char const* feat) { return FeatPick(p, feat) != nullptr; }

    // ---------------------------------------------------------------- subraces
    // (the WoW race, a subrace name). The race's own traits (every subrace has them) and each subrace's are in the
    // functions below; the addon's DnDRaces.lua describes them.
    //   Human (Civil Militia: spears and polearms)  Stormwind: Persuasion; advantage against being frightened
    //                                               Lordaeron: necrotic resistance; advantage on saves against poison
    //                                               Kul Tiran: Athletics; advantage on Strength saves
    //   Dwarf (Resilience, Combat Training, armour   Hill Dwarf: Dwarven Toughness, +1 HP a level
    //          never slows; 7.5 m)                  Mountain Dwarf: +1 AC in medium or heavy armour
    //                                               Dark Iron Dwarf: fire resistance; advantage against holds and charms
    //   Night Elf (Fey Ancestry: advantage against  Sentinel: +1.5 m; Stealth
    //          charms, no magical sleep; Perception) Highborne: Arcana; advantage on concentration saves
    //   Gnome (Gnome Cunning; 7.5 m)                Rock Gnome: Investigation; lightning resistance
    //                                               Forest Gnome: Stealth and Nature; 9 m
    //   Draenei (radiant and necrotic resistance)   Vindicator: Athletics; advantage against being frightened
    //                                               Anchorite: Medicine; the healing you give, +2
    struct SubraceDef { uint8 race; char const* name; };
    static SubraceDef const sSubraces[] =
    {
        { RACE_HUMAN, "Stormwind" }, { RACE_HUMAN, "Lordaeron" }, { RACE_HUMAN, "Kul Tiran" },
        { RACE_DWARF, "Hill Dwarf" }, { RACE_DWARF, "Mountain Dwarf" }, { RACE_DWARF, "Dark Iron Dwarf" },
        { RACE_NIGHTELF, "Sentinel" }, { RACE_NIGHTELF, "Highborne" },
        { RACE_GNOME, "Rock Gnome" }, { RACE_GNOME, "Forest Gnome" },
        { RACE_DRAENEI, "Vindicator" }, { RACE_DRAENEI, "Anchorite" },
    };

    std::vector<char const*> SubracesOf(uint8 race)
    {
        std::vector<char const*> out;
        for (SubraceDef const& d : sSubraces)
            if (d.race == race)
                out.push_back(d.name);
        return out;
    }

    static std::string SubraceOf(Player* p)
    {
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind("subrace:", 0) == 0)
                return k.substr(8);
        return "";
    }

    bool IsSubrace(Player* p, char const* name) { return p && StringEqualI(SubraceOf(p), name); }

    // what a save is against, by its name (the spell's or the effect's)
    static bool SaveIs(std::string const& what, std::initializer_list<char const*> names)
    {
        for (char const* n : names)
            if (what.rfind(n, 0) == 0)
                return true;
        return false;
    }

    bool RacialSaveAdvantage(Player* p, Ability a, std::string const& what, bool poison)
    {
        uint8 race = p->getRace();
        bool charm = SaveIs(what, { "Hypnotic Pattern", "Hideous Laughter", "Command", "Fey Presence" });
        bool fear = SaveIs(what, { "Fear", "Menacing Attack", "Dissonant Whispers" });
        if (race == RACE_NIGHTELF && charm)
            return true;                                 // Fey Ancestry
        if (IsSubrace(p, "Dark Iron Dwarf") && (charm || SaveIs(what, { "Hold Person" })))
            return true;                                 // Duergar Resilience
        if ((IsSubrace(p, "Stormwind") || IsSubrace(p, "Vindicator")) && fear)
            return true;                                 // Brave
        if (IsSubrace(p, "Lordaeron") && poison)
            return true;                                 // Plague-Hardened
        if (IsSubrace(p, "Kul Tiran") && a == STR)
            return true;                                 // Brawn
        if (IsSubrace(p, "Highborne") && SaveIs(what, { "Concentration" }))
            return true;                                 // Focused Mind
        return false;
    }

    bool RacialResists(Player* p, DmgType type)
    {
        if (p->getRace() == RACE_DRAENEI && (type == DMG_RADIANT || type == DMG_NECROTIC))
            return true;                                 // Celestial Resistance
        return (IsSubrace(p, "Lordaeron") && type == DMG_NECROTIC) || (IsSubrace(p, "Dark Iron Dwarf") && type == DMG_FIRE) ||
            (IsSubrace(p, "Rock Gnome") && type == DMG_LIGHTNING);
    }

    bool RacialSkill(Player* p, char const* skill)
    {
        auto is = [skill](char const* k) { return StringEqualI(skill, k); };
        if (p->getRace() == RACE_NIGHTELF && is("Perception"))
            return true;                                 // Keen Senses
        return (IsSubrace(p, "Stormwind") && is("Persuasion")) || (IsSubrace(p, "Kul Tiran") && is("Athletics")) ||
            (IsSubrace(p, "Sentinel") && is("Stealth")) || (IsSubrace(p, "Highborne") && is("Arcana")) ||
            (IsSubrace(p, "Rock Gnome") && is("Investigation")) || (IsSubrace(p, "Forest Gnome") && (is("Stealth") || is("Nature"))) ||
            (IsSubrace(p, "Vindicator") && is("Athletics")) || (IsSubrace(p, "Anchorite") && is("Medicine"));
    }

    bool RacialWeapon(Player* p, uint32 sub)
    {
        if (p->getRace() == RACE_DWARF)                  // Dwarven Combat Training: battleaxes, handaxes, hammers
            return sub == ITEM_SUBCLASS_WEAPON_AXE || sub == ITEM_SUBCLASS_WEAPON_AXE2 || sub == ITEM_SUBCLASS_WEAPON_MACE ||
                sub == ITEM_SUBCLASS_WEAPON_MACE2;
        if (p->getRace() == RACE_HUMAN)                  // Civil Militia: spears and polearms
            return sub == ITEM_SUBCLASS_WEAPON_SPEAR || sub == ITEM_SUBCLASS_WEAPON_POLEARM;
        return false;
    }

    std::string FeatOption(Player* p, char const* feat)
    {
        std::string const* k = FeatPick(p, feat);
        std::size_t colon = k ? k->find(':', k->find(':') + 1) : std::string::npos;
        return colon == std::string::npos ? "" : k->substr(colon + 1);
    }

    int EvocationBonus(Player* p)
    {
        return p && IsSubclass(p, "Evocation") && Level(p) >= 2 ? std::max(0, Mod(GetSheet(p).abil[INT])) : 0;
    }

    void RaiseArcaneWard(Player* p, int slotLevel)
    {
        if (!IsSubclass(p, "Abjuration") || Level(p) < 2)
            return;
        Sheet& s = GetSheet(p);
        int max = 2 * Level(p) + Mod(s.abil[INT]);
        if (!s.wardMade)
        {
            s.wardMade = true;
            s.wardHp = max;
            Log(p, Acore::StringFormat("|cff9ad0ffArcane Ward rises: {} points of protection.|r", max));
        }
        else
        {
            int before = s.wardHp;
            s.wardHp = std::min(max, s.wardHp + 2 * slotLevel);
            if (s.wardHp > before)
                Log(p, Acore::StringFormat("|cff9ad0ffArcane Ward recharges: {} / {}.|r", s.wardHp, max));
        }
        SaveSheet(p);
        PushSheet(p);
    }

    int AbsorbWithWard(Player* p, int damage)
    {
        Sheet& s = GetSheet(p);
        int soak = std::min(s.wardHp, damage);
        s.wardHp -= soak;
        if (soak > 0 && s.wardHp == 0)
            Log(p, "|cff9ad0ffYour Arcane Ward shatters.|r");
        return damage - soak;
    }

    // ------------------------------------------------------------------ the class tables
    static int SubclassLevel(Player* p) { return ClassInfo(p)->subclassLevel; }
    static std::vector<char const*> const& Subclasses(Player* p) { return ClassInfo(p)->subclasses; }

    // the sorcerer's metamagic (two picks at 3): "meta:Quickened"
    static char const* const sMetamagic[] = { "Quickened", "Twinned", "Heightened", "Empowered" };
    static int MetaTaken(Player* p)
    {
        int n = 0;
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind("meta:", 0) == 0)
                ++n;
        return n;
    }
    static int MetaDue(Player* p) { return IsClass(p, CLASS_SORCERER) && Level(p) >= 3 ? 2 : 0; }

    // the warlock's Eldritch Invocations (two at 2, three at 5: "inv:Agonizing Blast") and Pact Boon at 3 ("pact:Blade")
    static char const* const sInvocations[] = { "Agonizing Blast", "Repelling Blast", "Armor of Shadows", "Fiendish Vigour",
        "Beguiling Influence", "Thirsting Blade" };
    static char const* const sPacts[] = { "Blade", "Tome" };
    static int PicksWith(Player* p, char const* prefix)
    {
        int n = 0;
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind(prefix, 0) == 0)
                ++n;
        return n;
    }
    static int InvocationsDue(Player* p)
    {
        if (!IsClass(p, CLASS_DND_WARLOCK))
            return 0;
        return Level(p) >= 5 ? 3 : (Level(p) >= 2 ? 2 : 0);
    }
    static int PactDue(Player* p) { return IsClass(p, CLASS_DND_WARLOCK) && Level(p) >= 3 ? 1 : 0; }
    // Thirsting Blade wants the Blade and level 5
    static bool InvocationAllowed(Player* p, char const* name)
    {
        return std::string(name) != "Thirsting Blade" || (Level(p) >= 5 && HasPact(p, "Blade"));
    }

    bool HasMetamagic(Player* p, char const* name)
    {
        std::string want = std::string("meta:") + name;
        for (std::string const& k : GetSheet(p).picks)
            if (StringEqualI(k, want))
                return true;
        return false;
    }

    // which feats a class may take: 1 heavy armour (fighters), 2 spellcasters, 3 medium armour (fighters, barbarians)
    static bool FeatAllowed(Player* p, uint8 who)
    {
        switch (who)
        {
            case 1: return IsFighter(p);
            case 2: return IsCaster(p);
            case 3: return IsFighter(p) || IsClass(p, CLASS_BARBARIAN);
            default: return true;
        }
    }

    // Ability Score Improvements due by now: 4 (both classes), 6 (fighters)
    static int AsiDue(Player* p)
    {
        int lvl = Level(p);
        return (lvl >= 4 ? 1 : 0) + (IsFighter(p) && lvl >= 6 ? 1 : 0);
    }

    static int AsiTaken(Player* p)
    {
        int n = 0;
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind("asi", 0) == 0 || k.rfind("feat:", 0) == 0)       // asi:X (+2), asi2:X+Y (+1 +1), feats
                ++n;
        return n;
    }

    static bool HasAnyStyle(Player* p)
    {
        for (std::string const& k : GetSheet(p).picks)
            if (k.rfind("style:", 0) == 0)
                return true;
        return false;
    }

    static std::string FeatureText(Player* p, int level)
    {
        static char const* const asi = "Ability Score Improvement - +2 to an ability, or a feat";
        switch (GetSheet(p).cls)
        {
            case CLASS_BARBARIAN:
                switch (level)
                {
                    case 2: return "Reckless Attack - advantage on your melee attacks, and on attacks against you|"
                        "Danger Sense - advantage on Dexterity saves";
                    case 3: return "Primal Path - choose Berserker or Bear Totem|A third rage";
                    case 4: return asi;
                    case 5: return "Extra Attack - the Attack action swings twice|Fast Movement - +3 m without heavy armour|"
                        "Proficiency bonus +3";
                    case 6: return IsSubclass(p, "Berserker") ? "Mindless Rage - you can't be frightened while raging|A fourth rage"
                        : "Aspect of the Bear - advantage on Strength checks|A fourth rage";
                    default: return "";
                }
            case CLASS_DND_ROGUE:
                switch (level)
                {
                    case 2: return "Cunning Action - Dash, Disengage or Hide as a Bonus Action";
                    case 3: return "Roguish Archetype - choose Thief or Assassin|Sneak Attack 2d6";
                    case 4: return asi;
                    case 5: return "Uncanny Dodge - your reaction halves an attack's damage|Sneak Attack 3d6|Proficiency bonus +3";
                    case 6: return "Expertise - Perception and Investigation";
                    default: return "";
                }
            case CLASS_MONK:
                switch (level)
                {
                    case 2: return "Ki - 2 points: Flurry of Blows, Patient Defense, Step of the Wind|Unarmored Movement +3 m";
                    case 3: return "Monastic Tradition - choose Open Hand or Shadow|Deflect Missiles";
                    case 4: return asi;
                    case 5: return "Extra Attack|Stunning Strike - 1 ki: a CON save or they lose their turn|Martial Arts d6|"
                        "Proficiency bonus +3";
                    case 6: return IsSubclass(p, "Shadow") ? "Shadow Step - step from shadow to shadow, and strike with advantage"
                        : "Wholeness of Body - heal 3 x your level, once a long rest";
                    default: return "";
                }
            case CLASS_DND_WARLOCK:
                switch (level)
                {
                    case 2: return "Eldritch Invocations - choose two|A second Pact Magic slot";
                    case 3: return "Pact Boon - choose Blade or Tome|Pact slots are now 2nd level: Hold Person, Misty Step, Shatter";
                    case 4: return asi;
                    case 5: return "Pact slots are now 3rd level: Vampiric Touch|A third invocation|Eldritch Blast fires two beams|"
                        "Proficiency bonus +3";
                    case 6:
                        if (IsSubclass(p, "Fiend"))
                            return "Dark One's Own Luck - +1d10 on a saving throw, once a short rest";
                        if (IsSubclass(p, "Archfey"))
                            return "Misty Escape - once a short rest, when you're hurt, you vanish into hiding";
                        return "Entropic Ward - once a short rest, a foe's attack on you is at disadvantage; if it misses, your "
                            "next attack on it has advantage";
                    default: return "";
                }
            case CLASS_SORCERER:
                switch (level)
                {
                    case 2: return "Font of Magic - 2 sorcery points: turn them into spell slots, or slots into points";
                    case 3: return "Metamagic - choose two|2nd-level spell slots and spells";
                    case 4: return asi;
                    case 5: return "3rd-level spell slots and spells|Proficiency bonus +3|Cantrips now roll two dice";
                    case 6: return IsSubclass(p, "Wild Magic") ? "Bend Luck - 2 sorcery points: a foe's save against your spell, -1d4"
                        : "Elemental Affinity - + your Charisma to your dragon's element";
                    default: return "";
                }
            case CLASS_BARD:
                switch (level)
                {
                    case 2: return "Jack of All Trades - half your proficiency bonus on checks you aren't proficient in, and initiative|"
                        "Song of Rest - a short rest heals you and your friends 1d6 more";
                    case 3: return "Bard College - choose Lore or Valor|Expertise - Persuasion and Deception|2nd-level spell slots and spells";
                    case 4: return asi;
                    case 5: return "Font of Inspiration - Bardic Inspiration comes back on a short rest, and its die is a d8|"
                        "3rd-level spell slots - Hypnotic Pattern|Proficiency bonus +3|Cantrips now roll two dice";
                    case 6: return IsSubclass(p, "Valor") ? "Extra Attack - the Attack action swings twice"
                        : "Magical Secrets - Fireball and Misty Step";
                    default: return "";
                }
            case CLASS_CLERIC:
                switch (level)
                {
                    case 2: return IsSubclass(p, "Tempest")
                        ? "Channel Divinity - Turn Undead, and Destructive Wrath: your next lightning or thunder at its fullest"
                        : "Channel Divinity - Turn Undead, and Preserve Life: heal your friends for 5 x your level";
                    case 3: return "2nd-level spell slots - Aid, Lesser Restoration, Prayer of Healing, Hold Person";
                    case 4: return asi;
                    case 5: return "Destroy Undead - turned undead of CR 1/2 or less are destroyed|3rd-level spell slots - Spirit Guardians, "
                        "Mass Healing Word|Proficiency bonus +3|Cantrips now roll two dice";
                    case 6: return IsSubclass(p, "Tempest") ? "Thunderbolt Strike - your lightning throws a creature 3 m|A second Channel Divinity"
                        : "Blessed Healer - healing someone else heals you 2 + the spell's level|A second Channel Divinity";
                    default: return "";
                }
            case CLASS_DND_DRUID:
                switch (level)
                {
                    case 2: return "Wild Shape - twice a short rest, become a beast|Druid Circle - choose Land or Moon";
                    case 3: return IsSubclass(p, "Land") ? "2nd-level spell slots - Moonbeam, Hold Person|Circle spell: Barkskin"
                        : "2nd-level spell slots - Moonbeam, Hold Person";
                    case 4: return asi;
                    case 5: return "3rd-level spell slots - Call Lightning|Proficiency bonus +3|Cantrips now roll two dice";
                    case 6: return IsSubclass(p, "Moon") ? "Primal Strike - Wild Shape as a sabre-tooth"
                        : "Land's Stride - difficult terrain doesn't slow you";
                    default: return "";
                }
            default:
                break;
        }
        if (IsFighter(p))
            switch (level)
            {
                case 2: return "Action Surge - once per short rest, take one more action on your turn";
                case 3: return "Martial Archetype - choose Champion or Battle Master";
                case 4: return "Ability Score Improvement - +2 to an ability, or a feat";
                case 5: return "Extra Attack - the Attack action swings twice|Proficiency bonus +3";
                case 6: return "Ability Score Improvement - +2 to an ability, or a feat";
                default: return "";
            }
        switch (level)
        {
            case 2: return "Arcane Tradition - choose your school of magic";
            case 3: return "2nd-level spell slots and spells";
            case 4: return "Ability Score Improvement - +2 to an ability, or a feat";
            case 5: return "3rd-level spell slots and spells|Proficiency bonus +3|Cantrips now roll two dice";
            case 6:
                if (IsSubclass(p, "Abjuration"))
                    return "Warded Rest - a short rest fully restores your Arcane Ward";
                if (IsSubclass(p, "Evocation"))
                    return "Potent Cantrip - your damage cantrips deal half damage even when they miss";
                return "Your Arcane Tradition grows stronger";
            default: return "";
        }
    }

    void PushPendingChoices(Player* p)
    {
        Sheet& s = GetSheet(p);
        if (!SubracesOf(p->getRace()).empty() && SubraceOf(p).empty())
            Send(p, "LP^subrace");
        else if (IsFighter(p) && !HasAnyStyle(p))
            Send(p, "LP^style");
        else if (!HasOriginFeat(p))
            Send(p, "LP^feat1");
        else if (Level(p) >= SubclassLevel(p) && s.subclass.empty())
            Send(p, "LP^subclass");
        else if (PicksWith(p, "pact:") < PactDue(p))
            Send(p, "LP^pact");
        else if (PicksWith(p, "inv:") < InvocationsDue(p))
            Send(p, "LP^inv");
        else if (MetaTaken(p) < MetaDue(p))
            Send(p, "LP^meta");
        else if (AsiTaken(p) < AsiDue(p))
            Send(p, "LP^asi");
        else
            Send(p, "LP^");
    }

    // the class actions / spells the character knows now (to show what a level or a choice just added)
    static std::vector<uint32> KnownKit(Player* p)
    {
        std::vector<uint32> out;
        for (auto const* kit : { &WizardKit(), &ClassActions() })
            for (uint32 id : *kit)
                if (p->HasSpell(id))
                    out.push_back(id);
        return out;
    }

    static std::string NewlyLearned(Player* p, std::vector<uint32> const& before)
    {
        std::string names;
        for (uint32 id : KnownKit(p))
            if (std::find(before.begin(), before.end(), id) == before.end())
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(id))
                    names += (names.empty() ? "" : ", ") + std::string(info->SpellName[0]);
        return names;
    }

    void OnLevelUp(Player* p, uint8 oldLevel)
    {
        int now = Level(p), before = std::clamp<int>(oldLevel, 1, MAX_DND_LEVEL);
        uint32 oldMax = p->GetMaxHealth();
        std::vector<uint32> known = KnownKit(p);
        EnsureKit(p);                        // learns what the new level unlocks and puts it on a bar
        std::string spells = NewlyLearned(p, known);
        p->UpdateMaxHealth();
        int gained = int(p->GetMaxHealth()) - int(oldMax);
        if (gained > 0)
            p->SetHealth(p->GetHealth() + gained);
        SaveSheet(p);
        PushSheet(p);
        if (now > before)
            Announce(p, Acore::StringFormat("{} has reached level {}.", p->GetName(), now));
        if (now <= before)
            return;
        std::string slots;
        for (int l = 1; l <= 6; ++l)
            if (SlotsMax(p, l) > 0)
                slots += (slots.empty() ? "" : "  ") + Acore::StringFormat("L{}: {}", l, SlotsMax(p, l));
        std::string features;
        for (int l = before + 1; l <= now; ++l)
            if (!FeatureText(p, l).empty())
                features += (features.empty() ? "" : "|") + FeatureText(p, l);
        Log(p, Acore::StringFormat("|cffffd100Level {}!|r +{} HP. {}", now, std::max(0, gained), features));
        Send(p, Acore::StringFormat("LU^{}^{}^{}^{}^{}^{}", now, std::max(0, gained), p->GetMaxHealth(), slots, spells, features));
        PushPendingChoices(p);
    }

    void HandlePick(Player* p, std::string const& what)
    {
        Sheet& s = GetSheet(p);
        std::size_t sep = what.find('^');
        std::string kind = what.substr(0, sep), value = sep == std::string::npos ? "" : what.substr(sep + 1);
        std::vector<uint32> known = KnownKit(p);
        if (kind == "subrace" && SubraceOf(p).empty())
        {
            for (char const* name : SubracesOf(p->getRace()))
                if (StringEqualI(value, name))
                {
                    s.picks.push_back(std::string("subrace:") + name);
                    Log(p, Acore::StringFormat("|cffffd100Subrace: {}.|r", name));
                }
        }
        else if (kind == "style" && IsFighter(p) && !HasAnyStyle(p))
        {
            for (char const* name : sStyles)
                if (StringEqualI(value, name))
                {
                    s.picks.push_back(std::string("style:") + name);
                    Log(p, Acore::StringFormat("|cffffd100Fighting Style: {}.|r", name));
                }
        }
        else if (kind == "subclass" && Level(p) >= SubclassLevel(p) && s.subclass.empty())
        {
            for (char const* name : Subclasses(p))
                if (StringEqualI(value, name))
                {
                    s.subclass = name;
                    Log(p, Acore::StringFormat("|cffffd100{}: {}.|r", IsFighter(p) ? "Martial Archetype" : "Subclass", name));
                }
        }
        else if (kind == "meta" && MetaTaken(p) < MetaDue(p))
        {
            for (char const* name : sMetamagic)
                if (StringEqualI(value, name) && !HasMetamagic(p, name))
                {
                    s.picks.push_back(std::string("meta:") + name);
                    Log(p, Acore::StringFormat("|cffffd100Metamagic: {} Spell.|r", name));
                }
        }
        else if (kind == "inv" && PicksWith(p, "inv:") < InvocationsDue(p))
        {
            for (char const* name : sInvocations)
                if (StringEqualI(value, name) && !HasInvocation(p, name) && InvocationAllowed(p, name))
                {
                    s.picks.push_back(std::string("inv:") + name);
                    Log(p, Acore::StringFormat("|cffffd100Eldritch Invocation: {}.|r", name));
                }
        }
        else if (kind == "pact" && PicksWith(p, "pact:") < PactDue(p))
        {
            for (char const* name : sPacts)
                if (StringEqualI(value, name))
                {
                    s.picks.push_back(std::string("pact:") + name);
                    Log(p, Acore::StringFormat("|cffffd100Pact of the {}.|r", name));
                }
        }
        else if (kind == "asi" && AsiTaken(p) < AsiDue(p))
        {
            for (int i = 0; i < ABILITY_COUNT; ++i)
                if (StringEqualI(value, AbilityNames[i]))
                {
                    s.abil[i] = std::min(20, s.abil[i] + 2);
                    s.picks.push_back(std::string("asi:") + AbilityNames[i]);
                    Log(p, Acore::StringFormat("|cffffd100Ability Score Improvement: {} +2 (now {}).|r", AbilityNames[i], s.abil[i]));
                }
        }
        else if (kind == "asi2" && AsiTaken(p) < AsiDue(p))
        {
            // +1 to two different abilities: "STR^DEX"
            std::size_t sep2 = value.find('^');
            int a = AbilityIndex(value.substr(0, sep2)), b = sep2 == std::string::npos ? -1 : AbilityIndex(value.substr(sep2 + 1));
            if (a >= 0 && b >= 0 && a != b)
            {
                s.abil[a] = std::min(20, s.abil[a] + 1);
                s.abil[b] = std::min(20, s.abil[b] + 1);
                s.picks.push_back(Acore::StringFormat("asi2:{}+{}", AbilityNames[a], AbilityNames[b]));   // (picks are comma-separated)
                Log(p, Acore::StringFormat("|cffffd100Ability Score Improvement: {} +1, {} +1.|r", AbilityNames[a], AbilityNames[b]));
            }
        }
        else if ((kind == "feat" && AsiTaken(p) < AsiDue(p)) || (kind == "feat1" && !HasOriginFeat(p)))
        {
            // "Name" or "Name^Variant"
            std::size_t sep2 = value.find('^');
            std::string name = value.substr(0, sep2), variant = sep2 == std::string::npos ? "" : value.substr(sep2 + 1);
            FeatDef const* f = FindFeat(name);
            bool allowed = f && !HasFeat(p, f->name) && FeatAllowed(p, f->who);
            if (allowed && *f->variants)
            {
                allowed = false;
                for (std::string_view v : Acore::Tokenize(f->variants, ',', false))
                    if (StringEqualI(std::string(v), variant))
                    {
                        variant = std::string(v);
                        allowed = true;
                    }
            }
            else if (allowed)
                variant.clear();
            if (allowed)
            {
                std::string pick = kind + ":" + f->name + (variant.empty() ? "" : ":" + variant);
                s.picks.push_back(pick);
                if (int ab = FeatAbility(pick); ab >= 0)
                    s.abil[ab] = std::min(20, s.abil[ab] + 1);
                Log(p, Acore::StringFormat("|cffffd100{}: {}{}.|r", kind == "feat1" ? "Starting feat" : "Feat", f->name,
                    variant.empty() ? "" : " (" + variant + ")"));
            }
        }
        EnsureKit(p);                        // a Battle Master learns the maneuvers now
        std::string learned = NewlyLearned(p, known);
        if (!learned.empty())
            Log(p, "|cffffd100New actions:|r " + learned + "  |cff9a8a70(on your lower bars)|r");
        uint32 oldMax = p->GetMaxHealth();
        p->UpdateMaxHealth();
        if (p->GetMaxHealth() > oldMax)
            p->SetHealth(p->GetHealth() + (p->GetMaxHealth() - oldMax));
        SaveSheet(p);
        PushSheet(p);
        PushPendingChoices(p);
    }

    // back to a fresh level 1 (.dnd restart): undo the ASIs, forget the subclass and the ward (a fighting style and the
    // starting feat, chosen at level 1, stay)
    void ClearLevelChoices(Player* p)
    {
        Sheet& s = GetSheet(p);
        std::vector<std::string> keep;
        for (std::string const& k : s.picks)
        {
            if (k.rfind("asi:", 0) == 0)
            {
                for (int i = 0; i < ABILITY_COUNT; ++i)
                    if (StringEqualI(k.substr(4), AbilityNames[i]))
                        s.abil[i] -= 2;
            }
            else if (k.rfind("asi2:", 0) == 0)
            {
                std::string both = k.substr(5);
                for (std::string_view v : Acore::Tokenize(both, '+', false))
                    if (int i = AbilityIndex(std::string(v)); i >= 0)
                        s.abil[i] -= 1;
            }
            else if (k.rfind("feat:", 0) == 0)
            {
                if (int i = FeatAbility(k); i >= 0)
                    s.abil[i] -= 1;
            }
            else if (k.rfind("boon:", 0) == 0)
            {
                if (int i = AbilityIndex(k.substr(5)); i >= 0)
                    s.abil[i] -= 1;                  // (a trial's +1: the finds come back on a restart, so it goes)
            }
            else if (k.rfind("style:", 0) == 0 || k.rfind("feat1:", 0) == 0)
                keep.push_back(k);                   // (the style and the starting feat are level 1's; metamagic goes)
        }
        s.picks = keep;
        s.subclass.clear();
        s.wardHp = 0;
        s.wardMade = false;
        s.secondWindUsed = s.actionSurgeUsed = s.supUsed = 0;
        SaveSheet(p);
    }
}
