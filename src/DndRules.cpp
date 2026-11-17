/*
 * mod-dnd rules: dice, character sheets, creature stat blocks, AC / saves, damage and messaging.
 */
#include "Dnd.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Random.h"
#include "SpellInfo.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <mutex>

namespace Dnd
{
    char const* const AbilityNames[ABILITY_COUNT] = { "STR", "DEX", "CON", "INT", "WIS", "CHA" };
    char const* const DmgTypeNames[DMG_TYPE_COUNT] = { "Bludgeoning", "Piercing", "Slashing", "Fire", "Cold", "Lightning",
        "Thunder", "Acid", "Poison", "Force", "Necrotic", "Radiant", "Psychic" };

    uint32 SchoolMaskFor(DmgType t)
    {
        switch (t)
        {
            case DMG_FIRE:      return SPELL_SCHOOL_MASK_FIRE;
            case DMG_COLD:      return SPELL_SCHOOL_MASK_FROST;
            case DMG_LIGHTNING:
            case DMG_THUNDER:
            case DMG_ACID:
            case DMG_POISON:    return SPELL_SCHOOL_MASK_NATURE;
            case DMG_FORCE:     return SPELL_SCHOOL_MASK_ARCANE;
            case DMG_NECROTIC:
            case DMG_PSYCHIC:   return SPELL_SCHOOL_MASK_SHADOW;
            case DMG_RADIANT:   return SPELL_SCHOOL_MASK_HOLY;
            default:            return SPELL_SCHOOL_MASK_NORMAL;
        }
    }

    DmgType ParseDmgType(std::string const& s)
    {
        for (uint8 i = 0; i < DMG_TYPE_COUNT; ++i)
            if (StringEqualI(s, DmgTypeNames[i]))
                return DmgType(i);
        return DMG_BLUDGEONING;
    }

    // ------------------------------------------------------------------ dice
    int D(int sides)
    {
        if (Test::Running())
            if (int forced = Test::ForcedRoll(sides))
                return forced;
        return sides <= 1 ? 1 : int(urand(1, sides));
    }
    int Mod(int score) { return int(std::floor((score - 10) / 2.0)); }

    Dice Dice::Parse(std::string const& s)
    {
        Dice d;
        if (s.empty())
            return d;
        std::size_t dpos = s.find('d');
        if (dpos == std::string::npos)
        {
            d.bonus = std::atoi(s.c_str());
            return d;
        }
        d.count = dpos == 0 ? 1 : std::atoi(s.substr(0, dpos).c_str());
        std::size_t sign = s.find_first_of("+-", dpos);
        d.sides = std::atoi(s.substr(dpos + 1, sign == std::string::npos ? std::string::npos : sign - dpos - 1).c_str());
        if (sign != std::string::npos)
            d.bonus = std::atoi(s.substr(sign).c_str());
        return d;
    }

    int Dice::Roll(bool crit, std::string& detail, std::vector<int>* out) const
    {
        int n = crit ? count * 2 : count;
        auto rollSet = [&](std::vector<int>& set)
        {
            int sum = 0;
            for (int i = 0; i < n; ++i)
            {
                int r = maximize ? sides : D(sides);
                if (!maximize && rerollLow && r <= 2 && sides > 2)
                    r = D(sides);            // Great Weapon Fighting: a 1 or 2 is rolled again, and the new roll stands
                if (minTwo && r == 1)
                    r = 2;                   // Elemental Adept
                set.push_back(r);
                sum += r;
            }
            return sum;
        };
        auto join = [](std::vector<int> const& set)
        {
            std::string s;
            for (std::size_t i = 0; i < set.size(); ++i)
                s += (i ? "+" : "") + std::to_string(set[i]);
            return s;
        };
        std::vector<int> best;
        int total = rollSet(best);
        std::string twice;
        if (rollTwice && n > 0)
        {
            std::vector<int> other;
            int alt = rollSet(other);
            twice = Acore::StringFormat(" (Savage Attacker: {} or {})", join(best), join(other));
            if (alt > total)
            {
                total = alt;
                best = other;
            }
        }
        if (out)
            out->insert(out->end(), best.begin(), best.end());
        std::string rolls = join(best);
        total += bonus;
        detail = Acore::StringFormat("{}d{}{}{} [{}{}]{}", n, sides, bonus > 0 ? "+" : "", bonus ? std::to_string(bonus) : "",
            rolls.empty() ? "0" : rolls, bonus ? (bonus > 0 ? "+" : "") + std::to_string(bonus) : "", twice);
        return std::max(0, total);
    }

    std::string Dice::Str() const
    {
        if (!sides)
            return std::to_string(bonus);
        return Acore::StringFormat("{}d{}{}{}", count, sides, bonus > 0 ? "+" : "", bonus ? std::to_string(bonus) : "");
    }

    D20Result RollD20(int bonus, int adv)
    {
        D20Result r;
        int a = D(20);
        int b = D(20);
        if (adv > 0)
            r.natural = std::max(a, b);
        else if (adv < 0)
            r.natural = std::min(a, b);
        else
            r.natural = a;
        r.total = r.natural + bonus;
        std::string dice = adv ? Acore::StringFormat("d20({}/{} {})", a, b, adv > 0 ? "adv" : "dis") : Acore::StringFormat("d20({})", a);
        r.text = Acore::StringFormat("{} {}{} = {}", dice, bonus >= 0 ? "+" : "-", std::abs(bonus), r.total);
        return r;
    }

    // ------------------------------------------------------------------ sheets
    static std::unordered_map<ObjectGuid, Sheet> sSheets;

    Sheet& GetSheet(Player* p)
    {
        Sheet& s = sSheets[p->GetGUID()];
        if (!s.loaded)
            LoadSheet(p);
        return s;
    }

    void LoadSheet(Player* p)
    {
        Sheet& s = sSheets[p->GetGUID()];
        s.guid = p->GetGUID();
        s.loaded = true;
        QueryResult r = CharacterDatabase.Query("SELECT class, str, dex, con, intel, wis, cha, slots_used, temp_hp, short_rests_used, "
            "arcane_recovery_used, background, skills, created, build, subclass, picks, ward_hp, ward_made, res FROM dnd_character "
            "WHERE guid = {}", p->GetGUID().GetCounter());
        // the D&D class follows the WoW class chosen at character creation (DndClasses.cpp: Mage = Wizard, Warrior =
        // Fighter, Shaman = Barbarian, Rogue = Rogue, Druid = Monk, Warlock = Sorcerer)
        uint8 cls = DndClassForWow(p->getClass());
        if (!r)
        {
            s.cls = cls;
            s.abil = ClassInfo(cls)->abil;           // the builder's recommended build until they make their own
            static char const* const bgs[] = { "", "", "Soldier", "Outlander", "Criminal", "Acolyte", "Charlatan", "Charlatan",
                "Entertainer", "Outlander", "Acolyte" };
            s.background = cls < 11 ? bgs[cls] : "";
            SaveSheet(p);
            return;
        }
        Field* f = r->Fetch();
        s.cls = f[0].Get<uint8>();
        for (int i = 0; i < ABILITY_COUNT; ++i)
            s.abil[i] = f[1 + i].Get<int32>();
        std::string used = f[7].Get<std::string>();
        int i = 1;
        for (std::string_view v : Acore::Tokenize(used, ',', false))
            if (i < 10)
                s.slotsUsed[i++] = std::atoi(std::string(v).c_str());
        s.tempHp = f[8].Get<int32>();
        s.shortRestsUsed = f[9].Get<int32>();
        s.arcaneRecoveryUsed = f[10].Get<bool>();
        s.background = f[11].Get<std::string>();
        s.skills.clear();
        std::string skills = f[12].Get<std::string>();
        for (std::string_view v : Acore::Tokenize(skills, ',', false))
            if (!v.empty())
                s.skills.emplace_back(v);
        s.created = f[13].Get<bool>();
        s.build = f[14].Get<std::string>();
        s.subclass = f[15].Get<std::string>();
        s.picks.clear();
        std::string picks = f[16].Get<std::string>();
        for (std::string_view v : Acore::Tokenize(picks, ',', false))
            if (!v.empty())
                s.picks.emplace_back(v);
        s.wardHp = f[17].Get<int32>();
        s.wardMade = f[18].Get<bool>();
        std::string res = f[19].Get<std::string>();
        std::vector<std::string_view> rv = Acore::Tokenize(res, ',', false);
        s.secondWindUsed = rv.size() > 0 ? std::atoi(std::string(rv[0]).c_str()) : 0;
        s.actionSurgeUsed = rv.size() > 1 ? std::atoi(std::string(rv[1]).c_str()) : 0;
        s.supUsed = rv.size() > 2 ? std::atoi(std::string(rv[2]).c_str()) : 0;
        int* more[] = { &s.rageUsed, &s.kiUsed, &s.spUsed, &s.tidesUsed, &s.wholenessUsed, &s.patronUsed, &s.patron6Used, &s.agathys,
            &s.inspirationUsed, &s.wildShapeUsed, &s.naturalRecoveryUsed, &s.channelUsed, &s.wrathUsed };
        for (std::size_t k = 0; k < 13; ++k)
            *more[k] = rv.size() > 3 + k ? std::atoi(std::string(rv[3 + k]).c_str()) : 0;
        if (s.cls != cls)
        {
            // the class under them changed (the Priest slot was the sorcerer's): what belonged to the old class goes, and they
            // choose again
            std::vector<char const*> const& subs = ClassInfo(cls)->subclasses;
            if (!s.subclass.empty() && std::none_of(subs.begin(), subs.end(), [&](char const* n) { return s.subclass == n; }))
                s.subclass.clear();
            s.picks.erase(std::remove_if(s.picks.begin(), s.picks.end(), [](std::string const& k)
                { return k.rfind("meta:", 0) == 0; }), s.picks.end());
        }
        s.cls = cls;
    }

    void SaveSheet(Player* p)
    {
        auto itr = sSheets.find(p->GetGUID());
        if (itr == sSheets.end())
            return;
        Sheet const& s = itr->second;
        std::string used;
        for (int i = 1; i < 10; ++i)
            used += (i > 1 ? "," : "") + std::to_string(s.slotsUsed[i]);
        std::string skills;
        for (std::string const& k : s.skills)
            skills += (skills.empty() ? "" : ",") + k;
        std::string picks;
        for (std::string const& k : s.picks)
            picks += (picks.empty() ? "" : ",") + k;
        CharacterDatabase.Execute("REPLACE INTO dnd_character (guid, class, str, dex, con, intel, wis, cha, slots_used, temp_hp, "
            "short_rests_used, arcane_recovery_used, background, skills, created, build, subclass, picks, ward_hp, ward_made, res) VALUES "
            "({}, {}, {}, {}, {}, {}, {}, {}, '{}', {}, {}, {}, '{}', '{}', {}, '{}', '{}', '{}', {}, {}, '{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}')",
            p->GetGUID().GetCounter(), s.cls, s.abil[STR], s.abil[DEX], s.abil[CON], s.abil[INT], s.abil[WIS], s.abil[CHA], used,
            s.tempHp, s.shortRestsUsed, s.arcaneRecoveryUsed ? 1 : 0, s.background, skills, s.created ? 1 : 0, s.build,
            s.subclass, picks, s.wardHp, s.wardMade ? 1 : 0, s.secondWindUsed, s.actionSurgeUsed, s.supUsed, s.rageUsed, s.kiUsed,
            s.spUsed, s.tidesUsed, s.wholenessUsed, s.patronUsed, s.patron6Used, s.agathys, s.inspirationUsed, s.wildShapeUsed,
            s.naturalRecoveryUsed, s.channelUsed, s.wrathUsed);
    }

    void ForgetSheet(ObjectGuid guid) { sSheets.erase(guid); }

    int Level(Player* p) { return std::clamp<int>(p->GetLevel(), 1, MAX_DND_LEVEL); }
    int Prof(int level) { return 2 + (level - 1) / 4; }

    int MaxHp(Player* p)
    {
        Sheet& s = GetSheet(p);
        int hitDie = ClassInfo(s.cls)->hitDie;          // barbarian d12, fighter d10, rogue and monk d8, wizard and sorcerer d6
        int con = Mod(s.abil[CON]);
        int lvl = Level(p);
        int hp = hitDie + con + (lvl - 1) * (hitDie / 2 + 1 + con);
        if (HasFeat(p, "Tough"))
            hp += 2 * lvl;                                // Tough: +2 HP per level
        if (IsSubrace(p, "Hill Dwarf"))
            hp += lvl;                                    // Dwarven Toughness (hill dwarves): +1 HP per level
        hp += ClassHpBonus(p);                            // Draconic Resilience
        return std::max(1, hp);
    }

    // 5e full-caster table, levels 1-12
    static int const sFullCaster[12][6] =
    {
        {2, 0, 0, 0, 0, 0}, {3, 0, 0, 0, 0, 0}, {4, 2, 0, 0, 0, 0}, {4, 3, 0, 0, 0, 0}, {4, 3, 2, 0, 0, 0}, {4, 3, 3, 0, 0, 0},
        {4, 3, 3, 1, 0, 0}, {4, 3, 3, 2, 0, 0}, {4, 3, 3, 3, 1, 0}, {4, 3, 3, 3, 2, 0}, {4, 3, 3, 3, 2, 1}, {4, 3, 3, 3, 2, 1},
    };

    int SlotsMaxAt(int level, int spellLevel)
    {
        if (spellLevel < 1 || spellLevel > 6)
            return 0;
        return sFullCaster[std::clamp(level, 1, MAX_DND_LEVEL) - 1][spellLevel - 1];
    }

    int SlotsMax(Player* p, int spellLevel)
    {
        if (IsClass(p, CLASS_DND_WARLOCK))
            return spellLevel == PactLevel(p) ? PactSlots(p) : 0;     // Pact Magic: all at one level
        return IsCaster(p) ? SlotsMaxAt(Level(p), spellLevel) : 0;
    }

    int SlotsLeft(Player* p, int spellLevel)
    {
        return std::max(0, SlotsMax(p, spellLevel) - GetSheet(p).slotsUsed[spellLevel]);
    }

    // heavy armour too heavy for the wearer's STR (Chain Mail 13, Splint and Plate 15): -3 m. Dwarves ignore it.
    int HeavyArmorStrNeeded(Player* p)
    {
        Item* chest = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST);
        ItemTemplate const* t = chest ? chest->GetTemplate() : nullptr;
        if (!t || t->Class != ITEM_CLASS_ARMOR || t->SubClass != ITEM_SUBCLASS_ARMOR_PLATE)
            return 0;
        int ac = int(t->Armor);
        return ac >= 17 ? 15 : (ac >= 16 ? 13 : 0);
    }

    bool WearsHeavyArmour(Player* p)
    {
        Item* chest = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST);
        ItemTemplate const* t = chest ? chest->GetTemplate() : nullptr;
        return t && t->Class == ITEM_CLASS_ARMOR && t->SubClass == ITEM_SUBCLASS_ARMOR_PLATE && t->Armor >= 10;
    }

    float Speed(Player* p)
    {
        float speed = (p->getRace() == RACE_DWARF || (p->getRace() == RACE_GNOME && !IsSubrace(p, "Forest Gnome"))) ? 7.5f : 9.f;
        if (IsSubrace(p, "Sentinel"))
            speed += 1.5f;                   // Fleet of Foot
        if (p->getRace() != RACE_DWARF && GetSheet(p).abil[STR] < HeavyArmorStrNeeded(p))
            speed -= 3.f;
        if (HasFeat(p, "Mobile"))
            speed += 3.f;
        if (BeastForm const* f = WildShapeOf(p))
            return f->speed;                 // Wild Shape: the beast's legs
        return speed + ClassSpeedBonus(p) + ItemSpeed(p); // Fast Movement, Unarmored Movement, boots
    }

    float JumpDistance(Player* p)
    {
        float d = std::max(3.f, GetSheet(p).abil[STR] / 2.f);
        if (p->HasAura(SPELL_ENHANCED_LEAP))
            d *= 3.f;
        if (HasFeat(p, "Athlete"))
            d *= 1.5f;                       // Athlete: jump distance +50%
        d *= WeatherJump(p);                 // a gale at your back
        if (Combatant* c = CombatantOf(p->GetGUID()))
            if (c->stepJump)
                d *= 2.f;                        // Step of the Wind
        return d;
    }

    static int MartialMod(Player* p) { return std::max(Mod(GetSheet(p).abil[STR]), Mod(GetSheet(p).abil[DEX])); }
    // (a fighter's "spell" numbers are its maneuver DC and weapon attack bonus)
    // casters by their casting ability (wizard INT, sorcerer CHA); a monk's DC is ki (WIS); the others' are martial
    struct ScrollNumbers { int dc = 0, atk = 0, mod = 0; uint32 untilMs = 0; };
    static std::unordered_map<ObjectGuid, ScrollNumbers> sScroll;

    static ScrollNumbers const* ScrollOf(Player* p)
    {
        auto itr = p ? sScroll.find(p->GetGUID()) : sScroll.end();
        if (itr == sScroll.end())
            return nullptr;
        if (getMSTimeDiff(getMSTime(), itr->second.untilMs) > 0x80000000u)
        {
            sScroll.erase(itr);                  // (expired)
            return nullptr;
        }
        return &itr->second;
    }

    void ReadScroll(Player* p, int spellLevel)
    {
        bool high = spellLevel >= 3;
        sScroll[p->GetGUID()] = { high ? 15 : 13, high ? 7 : 5, high ? 4 : 3, getMSTime() + 4000 };
    }

    int CastingMod(Player* p)
    {
        if (ScrollNumbers const* sc = ScrollOf(p))
            return sc->mod;
        ClassDef const* c = ClassInfo(p);
        return c->cast != ABILITY_COUNT ? Mod(GetSheet(p).abil[c->cast]) : 0;
    }

    int SpellSaveDC(Player* p)
    {
        if (ScrollNumbers const* sc = ScrollOf(p))
            return sc->dc;
        ClassDef const* c = ClassInfo(p);
        if (c->cast != ABILITY_COUNT)
            return 8 + Prof(Level(p)) + Mod(GetSheet(p).abil[c->cast]);
        return IsClass(p, CLASS_MONK) ? KiDC(p) : ManeuverDC(p);
    }
    int SpellAttackBonus(Player* p)
    {
        if (ScrollNumbers const* sc = ScrollOf(p))
            return sc->atk;
        ClassDef const* c = ClassInfo(p);
        return Prof(Level(p)) + (c->cast != ABILITY_COUNT ? Mod(GetSheet(p).abil[c->cast]) : MartialMod(p));
    }
    bool IsFighter(Player* p) { return p && GetSheet(p).cls == CLASS_FIGHTER; }
    char const* ClassName(Player* p) { return ClassInfo(p)->name; }
    int ManeuverDC(Player* p) { return 8 + Prof(Level(p)) + MartialMod(p); }
    // Battle Master: 4 dice from level 3; Martial Adept: one more (or one, for anyone else)
    int SuperiorityMax(Player* p)
    {
        int n = IsFighter(p) && Level(p) >= 3 && IsSubclass(p, "Battle Master") ? 4 : 0;
        return n + (HasFeat(p, "Martial Adept") ? 1 : 0);
    }

    bool HasStyle(Player* p, char const* style)
    {
        std::string want = std::string("style:") + style;
        for (std::string const& k : GetSheet(p).picks)
            if (StringEqualI(k, want))
                return true;
        return false;
    }

    // ------------------------------------------------------------------ world data
    static std::unordered_map<uint32, CreatureStats> sStats;
    static std::unordered_map<uint32, CreatureStats> sTestStats;
    static std::unordered_map<uint32, EncounterDef> sEncounters;
    static std::unordered_map<uint32, uint32> sSpawnToEncounter;

    void SetTestStats(CreatureStats const& s) { sTestStats[s.entry] = s; }
    void ClearTestStats() { sTestStats.clear(); }

    CreatureStats const* GetStats(uint32 entry)
    {
        if (!sTestStats.empty())
            if (auto t = sTestStats.find(entry); t != sTestStats.end())
                return &t->second;
        auto itr = sStats.find(entry);
        return itr == sStats.end() ? nullptr : &itr->second;
    }

    CreatureStats const& StatsOrDefault(Creature* c)
    {
        static CreatureStats def;
        if (IsBrawler(c->GetGUID()))
            return BrawlerStats();
        if (IsJigDummy(c->GetGUID()))
            return JigDummyStats(c->GetGUID());
        CreatureStats const* s = GetStats(c->GetEntry());
        if (s)
            return *s;
        def.entry = c->GetEntry();
        def.melee.name = "Strike";
        def.melee.bonus = 2;
        def.melee.dice = Dice::Parse("1d4");
        return def;
    }

    EncounterDef const* GetEncounterDefBySpawn(uint32 spawnId)
    {
        auto itr = sSpawnToEncounter.find(spawnId);
        return itr == sSpawnToEncounter.end() ? nullptr : GetEncounterDef(itr->second);
    }

    EncounterDef const* GetEncounterDef(uint32 id)
    {
        auto itr = sEncounters.find(id);
        return itr == sEncounters.end() ? nullptr : &itr->second;
    }

    std::vector<EncounterDef const*> AllEncounters()
    {
        std::vector<EncounterDef const*> out;
        for (auto const& [id, e] : sEncounters)
            out.push_back(&e);
        std::sort(out.begin(), out.end(), [](EncounterDef const* a, EncounterDef const* b) { return a->id < b->id; });
        return out;
    }

    static uint32 ParseTypeMask(std::string const& s)
    {
        uint32 mask = 0;
        for (std::string_view v : Acore::Tokenize(s, ',', false))
            if (!v.empty())
                mask |= 1u << ParseDmgType(std::string(v));
        return mask;
    }

    void LoadWorldData()
    {
        sStats.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT entry, ac, hp, speed, str, dex, con, intel, wis, cha, "
            "melee_name, melee_bonus, melee_dice, melee_type, ranged_name, ranged_bonus, ranged_dice, ranged_type, ranged_range, "
            "xp, resist, immune, vuln, venom, spells, carries FROM dnd_creature"))
        {
            do
            {
                Field* f = r->Fetch();
                CreatureStats s;
                s.entry = f[0].Get<uint32>();
                s.ac = f[1].Get<int32>();
                s.hp = f[2].Get<int32>();
                s.speed = f[3].Get<float>();
                for (int i = 0; i < ABILITY_COUNT; ++i)
                    s.abil[i] = f[4 + i].Get<int32>();
                s.melee.name = f[10].Get<std::string>();
                s.melee.bonus = f[11].Get<int32>();
                s.melee.dice = Dice::Parse(f[12].Get<std::string>());
                s.melee.type = ParseDmgType(f[13].Get<std::string>());
                s.ranged.name = f[14].Get<std::string>();
                s.ranged.bonus = f[15].Get<int32>();
                s.ranged.dice = Dice::Parse(f[16].Get<std::string>());
                s.ranged.type = ParseDmgType(f[17].Get<std::string>());
                s.ranged.range = f[18].Get<float>();
                s.xp = f[19].Get<uint32>();
                s.resist = ParseTypeMask(f[20].Get<std::string>());
                s.immune = ParseTypeMask(f[21].Get<std::string>());
                s.vuln = ParseTypeMask(f[22].Get<std::string>());
                std::string venom = f[23].Get<std::string>();          // "DC:dice", e.g. "11:2d6"
                if (size_t colon = venom.find(':'); colon != std::string::npos)
                {
                    s.venomDc = std::atoi(venom.substr(0, colon).c_str());
                    s.venom = Dice::Parse(venom.substr(colon + 1));
                }
                // "201301,201311:2,200124:1": spell ids, each with its uses a fight (none: at will)
                std::string spellList = f[24].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(spellList, ',', false))
                {
                    std::string one(v);
                    if (one.empty() || s.spells.size() >= 8)
                        continue;
                    CreatureSpell cs;
                    size_t colon = one.find(':');
                    cs.id = uint32(std::atoi(one.substr(0, colon).c_str()));
                    cs.uses = colon == std::string::npos ? 0 : uint8(std::atoi(one.substr(colon + 1).c_str()));
                    if (CanCreatureCast(cs.id))
                        s.spells.push_back(cs);
                    else
                        LOG_ERROR("module", "mod-dnd: creature {} lists spell {}, which creatures can't cast", s.entry, cs.id);
                }
                // "118:30,10305:10": items it may have on it, each with its chance in percent
                std::string carryList = f[25].Get<std::string>();
                for (std::string_view v : Acore::Tokenize(carryList, ',', false))
                {
                    std::string one(v);
                    size_t colon = one.find(':');
                    if (one.empty() || colon == std::string::npos)
                        continue;
                    s.carries.emplace_back(uint32(std::atoi(one.substr(0, colon).c_str())),
                        uint8(std::clamp(std::atoi(one.substr(colon + 1).c_str()), 0, 100)));
                }
                sStats[s.entry] = s;
            } while (r->NextRow());
        }

        sEncounters.clear();
        sSpawnToEncounter.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT id, name, aggro_range, join_range, zone_id, xp, dialogue_node, after_text, patience, snap_node, meter_label, sour_node, minion, "
            "legendary FROM dnd_encounter"))
        {
            do
            {
                Field* f = r->Fetch();
                EncounterDef& e = sEncounters[f[0].Get<uint32>()];
                e.id = f[0].Get<uint32>();
                e.name = f[1].Get<std::string>();
                e.aggroRange = f[2].Get<float>();
                e.joinRange = f[3].Get<float>();
                e.zoneId = f[4].Get<uint32>();
                e.xp = f[5].Get<uint32>();
                e.dialogueNode = f[6].Get<uint32>();
                e.afterText = f[7].Get<std::string>();
                e.patience = f[8].Get<int32>();
                e.snapNode = f[9].Get<uint32>();
                e.meterLabel = f[10].Get<std::string>();
                e.sourNode = f[11].Get<uint32>();
                e.minion = f[12].Get<uint32>();
                e.legendary = f[13].Get<uint8>();
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT spawn_guid, encounter_id FROM dnd_encounter_member"))
        {
            do
            {
                Field* f = r->Fetch();
                uint32 spawn = f[0].Get<uint32>();
                uint32 enc = f[1].Get<uint32>();
                auto itr = sEncounters.find(enc);
                if (itr == sEncounters.end())
                    continue;
                itr->second.spawns.push_back(spawn);
                sSpawnToEncounter[spawn] = enc;
            } while (r->NextRow());
        }
        LoadZones();
        LoadDialogue();
        LoadStory();
        LoadTreats();
        LoadItemEffects();
        LoadTownsfolk();
        LoadEndgame();
        LOG_INFO("module", "mod-dnd: {} stat blocks, {} encounters, {} encounter spawns", sStats.size(), sEncounters.size(),
            sSpawnToEncounter.size());
    }

    // ------------------------------------------------------------------ generic queries
    // ------------------------------------------------------------------ magic item effects
    struct ItemEffect { std::string key, value; };
    static std::unordered_map<uint32, std::vector<ItemEffect>> sItemEffects;

    void LoadItemEffects()
    {
        sItemEffects.clear();
        if (QueryResult r = WorldDatabase.Query("SELECT entry, effect, value FROM dnd_item_effect"))
            do
                sItemEffects[r->Fetch()[0].Get<uint32>()].push_back({ r->Fetch()[1].Get<std::string>(), r->Fetch()[2].Get<std::string>() });
            while (r->NextRow());
    }

    bool NeedsAttunement(uint32 itemEntry) { return sItemEffects.count(itemEntry) != 0; }

    // fn(value, item template) for every effect of this kind on what the player wears. weaponSlot >= 0: of the weapons,
    // only the one in that slot counts (an on-hit effect rides the weapon that hit). Attunement: only the first
    // ATTUNEMENT_SLOTS items with effects work (equipping more is refused - DndEndgame.cpp - but a character may carry
    // more from before the rule). An elixir's effects count as worn while it lasts
    template <class F>
    static void EachItemEffect(Player* p, char const* key, F fn, int weaponSlot = -1)
    {
        if (!p || sItemEffects.empty())
            return;
        int attuned = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemTemplate const* t = item->GetTemplate();
            auto itr = sItemEffects.find(t->ItemId);
            if (itr == sItemEffects.end())
                continue;
            if (++attuned > ATTUNEMENT_SLOTS)
                continue;
            bool weaponSlotItem = (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND || slot == EQUIPMENT_SLOT_RANGED) &&
                t->Class == ITEM_CLASS_WEAPON;
            if (weaponSlot >= 0 && weaponSlotItem && slot != weaponSlot)
                continue;
            for (ItemEffect const& e : itr->second)
                if (e.key == key)
                    fn(e.value, t);
        }
        if (uint32 elixir = ActiveElixir(p))
            if (auto itr = sItemEffects.find(elixir); itr != sItemEffects.end())
                if (ItemTemplate const* t = sObjectMgr->GetItemTemplate(elixir))
                    for (ItemEffect const& e : itr->second)
                        if (e.key == key)
                            fn(e.value, t);
    }

    // "Sleight of Hand 2" -> ("Sleight of Hand", 2)
    static std::pair<std::string, int> NameAndNumber(std::string const& v)
    {
        std::size_t sp = v.rfind(' ');
        if (sp == std::string::npos)
            return { v, 0 };
        return { v.substr(0, sp), std::atoi(v.c_str() + sp + 1) };
    }

    static int AbilityByName(std::string const& n)
    {
        for (int i = 0; i < ABILITY_COUNT; ++i)
            if (StringEqualI(n, AbilityNames[i]))
                return i;
        return -1;
    }

    int ItemSaveBonus(Player* p, Ability a)
    {
        int b = 0;
        EachItemEffect(p, "save_all", [&](std::string const& v, ItemTemplate const*) { b += std::atoi(v.c_str()); });
        EachItemEffect(p, "save", [&](std::string const& v, ItemTemplate const*)
        {
            auto [name, n] = NameAndNumber(v);
            if (AbilityByName(name) == int(a))
                b += n;
        });
        return b;
    }

    int ItemSkillBonus(Player* p, std::string const& skill)
    {
        int b = 0;
        EachItemEffect(p, "skill", [&](std::string const& v, ItemTemplate const*)
        {
            auto [name, n] = NameAndNumber(v);
            if (StringEqualI(name, skill))
                b += n;
        });
        return b;
    }

    bool ItemSkillAdvantage(Player* p, std::string const& skill)
    {
        bool adv = false;
        EachItemEffect(p, "skill_adv", [&](std::string const& v, ItemTemplate const*) { adv = adv || StringEqualI(v, skill); });
        return adv;
    }

    bool ItemResists(Player* p, DmgType t)
    {
        bool r = false;
        EachItemEffect(p, "resist", [&](std::string const& v, ItemTemplate const*) { r = r || ParseDmgType(v) == t; });
        return r;
    }

    int ItemScore(Player* p, Ability a)
    {
        int best = 0;
        EachItemEffect(p, "set_abil", [&](std::string const& v, ItemTemplate const*)
        {
            auto [name, n] = NameAndNumber(v);
            if (AbilityByName(name) == int(a))
                best = std::max(best, n);
        });
        return best;
    }

    float ItemSpeed(Player* p)
    {
        float s = 0.f;
        EachItemEffect(p, "speed", [&](std::string const& v, ItemTemplate const*) { s += float(std::atof(v.c_str())); });
        return s;
    }

    int ItemInitiative(Player* p)
    {
        int b = 0;
        EachItemEffect(p, "init", [&](std::string const& v, ItemTemplate const*) { b += std::atoi(v.c_str()); });
        return b;
    }

    int ItemTempHp(Player* p)
    {
        int best = 0;
        EachItemEffect(p, "temp_hp", [&](std::string const& v, ItemTemplate const*) { best = std::max(best, std::atoi(v.c_str())); });
        return best;
    }

    bool ItemImmune(Player* p, std::string const& condition)
    {
        bool im = false;
        EachItemEffect(p, "immune", [&](std::string const& v, ItemTemplate const*) { im = im || StringEqualI(v, condition); });
        return im;
    }

    int ItemDamageReduction(Player* p)
    {
        int b = 0;
        EachItemEffect(p, "dr", [&](std::string const& v, ItemTemplate const*) { b += std::atoi(v.c_str()); });
        return b;
    }

    int ItemHealOnKill(Player* p)
    {
        int b = 0;
        EachItemEffect(p, "heal_kill", [&](std::string const& v, ItemTemplate const*) { b += std::atoi(v.c_str()); });
        return b;
    }

    std::vector<ItemRider> ItemRiders(Player* p, uint8 weaponSlot, std::string const& kind)
    {
        std::vector<ItemRider> out;
        EachItemEffect(p, kind.c_str(), [&](std::string const& v, ItemTemplate const* t)
        {
            std::vector<std::string_view> f = Acore::Tokenize(v, ' ', false);
            ItemRider r;
            r.item = t->Name1;
            if (kind == "on_hit" || kind == "thorns")             // "1d4 Fire"
            {
                if (f.size() < 2)
                    return;
                r.dice = Dice::Parse(std::string(f[0]));
                r.type = ParseDmgType(std::string(f[1]));
            }
            else if (kind == "bane")                              // "6 1d6 Radiant": against creature type 6 (undead)
            {
                if (f.size() < 3)
                    return;
                r.creatureType = std::atoi(std::string(f[0]).c_str());
                r.dice = Dice::Parse(std::string(f[1]));
                r.type = ParseDmgType(std::string(f[2]));
            }
            else if (kind == "on_hit_cond")                       // "prone 12 DEX"
            {
                if (f.size() < 3)
                    return;
                r.condition = std::string(f[0]);
                r.dc = std::atoi(std::string(f[1]).c_str());
                int ab = AbilityByName(std::string(f[2]));
                r.ability = Ability(ab < 0 ? CON : ab);
            }
            out.push_back(r);
        }, kind == "thorns" ? -1 : int(weaponSlot));
        return out;
    }

    int AbilityScore(Unit* u, Ability a)
    {
        if (Player* p = u->ToPlayer())
            return std::max(GetSheet(p).abil[a], ItemScore(p, a));   // (a headband that sets INT 17 wins over a lower one)
        if (Creature* c = u->ToCreature())
            return StatsOrDefault(c).abil[a];
        return 10;
    }

    int AbilityMod(Unit* u, Ability a) { return Mod(AbilityScore(u, a)); }

    // Haste +2, Slow -2 (while their casters hold them)
    static int SpellAcMods(Combatant* c)
    {
        if (!c)
            return 0;
        auto held = [](ObjectGuid by, uint32 spell)
        {
            Combatant* b = by.IsEmpty() ? nullptr : CombatantOf(by);
            return b && b->concentration == spell;
        };
        return (held(c->hastedBy, SPELL_HASTE) ? 2 : 0) - (held(c->slowBy, SPELL_SLOW) ? 2 : 0);
    }

    int ArmorClass(Unit* u)
    {
        if (Creature* c = u->ToCreature())
        {
            Combatant* cc = CombatantOf(c->GetGUID());
            int ac = StatsOrDefault(c).ac - (cc && cc->corroded ? 2 : 0);   // acid eating at its hide
            if (cc && !cc->faithBy.IsEmpty())
                if (Combatant* fc = CombatantOf(cc->faithBy); fc && fc->concentration == SPELL_SHIELD_OF_FAITH)
                    ac += 2;                     // a creature's Shield of Faith
            return ac + SpellAcMods(cc);
        }
        Player* p = u->ToPlayer();
        if (!p)
            return 10;
        int dex = AbilityMod(p, DEX);
        int base = 10 + dex;
        int bonus = 0;
        bool armoured = false;
        // Body armour: item 'armor' >= 10 is a base AC (cloth/leather light: full DEX, mail medium: DEX max 2,
        // plate heavy: no DEX). Anything else with armor > 0 is a flat magic bonus (+1 AC rings, cloaks...).
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;
            ItemTemplate const* t = item->GetTemplate();
            int armor = int(t->Armor);
            if (armor <= 0)
                continue;
            if (slot == EQUIPMENT_SLOT_CHEST && armor >= 10 && t->Class == ITEM_CLASS_ARMOR && t->SubClass != ITEM_SUBCLASS_ARMOR_SHIELD)
            {
                int capped = dex;
                if (t->SubClass == ITEM_SUBCLASS_ARMOR_MAIL)
                    capped = std::min(dex, HasFeat(p, "Medium Armour Master") ? 3 : 2);
                else if (t->SubClass == ITEM_SUBCLASS_ARMOR_PLATE)
                    capped = 0;
                base = std::max(base, armor + capped);
                armoured = true;
            }
            else if (t->Class == ITEM_CLASS_ARMOR && t->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                bonus += 2;                       // shields: +2
            else if (armor < 10)
                bonus += armor;                   // magic: +1 rings and cloaks (stock armour off the chest doesn't count)
        }
        if (p->HasAura(SPELL_MAGE_ARMOR))
            base = std::max(base, 13 + dex);
        base = std::max(base, UnarmoredBase(p));         // Unarmored Defense, Draconic Resilience
        if (Item* chest = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_CHEST); armoured && chest && IsSubrace(p, "Mountain Dwarf") &&
            (chest->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_MAIL || chest->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_PLATE))
            bonus += 1;                      // Dwarven Armour Training (mountain dwarves): medium or heavy armour
        if (armoured && HasStyle(p, "Defense"))
            bonus += 1;                      // Fighting Style: Defense
        if (BeastForm const* f = WildShapeOf(p))
            return f->ac;                    // Wild Shape: the beast's hide
        int ac = base + bonus;
        if (Combatant* c = CombatantOf(p->GetGUID()))
        {
            if (c->concentration == SPELL_BARKSKIN)
                ac = std::max(ac, 16);       // Barkskin
            if (!c->faithBy.IsEmpty())
                if (Combatant* fc = CombatantOf(c->faithBy); fc && fc->concentration == SPELL_SHIELD_OF_FAITH)
                    ac += 2;                 // Shield of Faith
            if (c->corroded)
                ac -= 2;                     // acid
            ac += SpellAcMods(c);
        }
        return ac;
    }

    int SaveBonus(Unit* u, Ability a)
    {
        int b = AbilityMod(u, a);
        if (Player* p = u->ToPlayer())
        {
            ClassDef const* c = ClassInfo(p);
            bool resilient = FeatOption(p, "Resilient") == AbilityNames[a];
            if (a == c->save1 || a == c->save2 || resilient)
                b += Prof(Level(p));
            // Shield Master: +2 to Dexterity saves with a shield on the arm
            if (a == DEX && HasFeat(p, "Shield Master"))
                if (Item* off = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
                    if (off->GetTemplate()->Class == ITEM_CLASS_ARMOR && off->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                        b += 2;
            b += ItemSaveBonus(p, a);            // rings and amulets of warding
        }
        return b;
    }

    bool SaveAdvantage(Unit* u, Ability a, bool magic, bool poison)
    {
        if (Player* p = u->ToPlayer())
        {
            // Gnome Cunning: advantage on INT/WIS/CHA saves against magic
            if (p->getRace() == RACE_GNOME && magic && (a == INT || a == WIS || a == CHA))
                return true;
            // Dwarven Resilience: advantage on saves against poison
            if (p->getRace() == RACE_DWARF && poison)
                return true;
            // Danger Sense (barbarian 2): DEX saves; a raging barbarian's STR saves
            if (IsClass(p, CLASS_BARBARIAN) && ((a == DEX && Level(p) >= 2) || (a == STR && CombatantOf(p->GetGUID()) &&
                CombatantOf(p->GetGUID())->raging)))
                return true;
        }
        return false;
    }

    float AreaEdge(Unit const* u)
    {
        return std::max(0.3f, u->GetFloatValue(UNIT_FIELD_BOUNDINGRADIUS));
    }

    float MeleeRange(Unit const* a, Unit const* b)
    {
        return std::max(NOMINAL_MELEE_RANGE, a->GetCombatReach() + b->GetCombatReach() + 4.0f / 3.0f);
    }

    std::string Name(Unit* u) { return u ? u->GetName() : "?"; }

    // ------------------------------------------------------------------ damage
    static bool sDealing = false;
    bool IsDealing() { return sDealing; }

    int ApplyDamage(DamageCtx const& d)
    {
        if (!d.victim || !d.victim->IsAlive())
            return 0;
        int amount = d.amount;
        std::string note;
        if (amount > 0)
            if (Combatant* vc = CombatantOf(d.victim->GetGUID()); vc && (vc->entranced || vc->asleep || vc->turned))
            {
                note += vc->asleep ? " (wakes up)" : (vc->turned ? " (its terror breaks)" : " (snaps out of the pattern)");
                if (vc->asleep)
                    d.victim->SetStandState(UNIT_STAND_STATE_STAND);
                vc->entranced = vc->asleep = vc->turned = false;
                vc->entrancedBy.Clear();
            }
        if (Creature* c = d.victim->ToCreature())
        {
            CreatureStats const& s = StatsOrDefault(c);
            uint32 bit = 1u << d.type;
            if (s.immune & bit)
            {
                amount = 0;
                note = " (immune)";
            }
            else if (s.resist & bit)
            {
                Player* adept = d.attacker ? d.attacker->ToPlayer() : nullptr;
                if (adept && FeatOption(adept, "Elemental Adept") == DmgTypeNames[d.type])
                    note = " (Elemental Adept: resistance ignored)";
                else
                {
                    amount /= 2;
                    note = " (resisted)";
                }
            }
            else if (s.vuln & bit)
            {
                amount *= 2;
                note = " (vulnerable)";
            }
        }
        {
            // the weather where the victim stands (DndWeather.cpp): drenched, damp, snow
            bool vuln = false, resisted = false;
            if (Creature* c = d.victim->ToCreature())
            {
                vuln = StatsOrDefault(c).vuln & (1u << d.type);
                resisted = StatsOrDefault(c).resist & (1u << d.type);
            }
            amount = WeatherDamage(d.victim, amount, d.type, vuln, resisted, note);
        }
        bool physical = d.type == DMG_BLUDGEONING || d.type == DMG_PIERCING || d.type == DMG_SLASHING;
        if (Combatant* vb = CombatantOf(d.victim->GetGUID()); vb && !vb->banishedBy.IsEmpty())
            return 0;                        // banished: it isn't here
        if (amount > 0 && physical)
            if (Combatant* sk = CombatantOf(d.victim->GetGUID()); sk && !sk->stoneskinBy.IsEmpty())
                if (Combatant* by = CombatantOf(sk->stoneskinBy); by && by->concentration == SPELL_STONESKIN)
                {
                    amount /= 2;
                    note += " (Stoneskin: halved)";
                }
        if (amount > 0 && d.type == DMG_COLD)
            if (Combatant* fs = CombatantOf(d.victim->GetGUID()); fs && fs->fireShield)
            {
                amount /= 2;
                note += " (Fire Shield: halved)";
            }
        if (amount > 0 && physical)
            if (Combatant* bw = CombatantOf(d.victim->GetGUID()); bw && bw->bladeWard)
            {
                amount /= 2;
                note += " (Blade Ward: halved)";
            }
        if (amount > 0 && d.attacker && (physical || d.melee))
            if (Combatant* en = CombatantOf(d.attacker->GetGUID()); en && !en->enfeebledBy.IsEmpty())
                if (Combatant* by = CombatantOf(en->enfeebledBy); by && by->concentration == SPELL_RAY_OF_ENFEEBLEMENT)
                {
                    amount /= 2;
                    note += " (enfeebled: halved)";
                }
        uint32 absorbed = 0;
        if (Player* p = d.victim->ToPlayer())
        {
            if (p->getRace() == RACE_DWARF && d.type == DMG_POISON && amount > 0)
            {
                amount /= 2;
                note += " (Dwarven Resilience: resisted)";
            }
            // Rage: resistant to bludgeoning, piercing and slashing (Bear Totem: to everything but psychic)
            if (amount > 0)
                if (Combatant* rc = CombatantOf(p->GetGUID()); rc && rc->raging)
                {
                    bool weapon = d.type == DMG_BLUDGEONING || d.type == DMG_PIERCING || d.type == DMG_SLASHING;
                    if (weapon || (IsSubclass(p, "Bear Totem") && d.type != DMG_PSYCHIC))
                    {
                        amount /= 2;
                        note += " (Rage: resisted)";
                    }
                }
            // gear: resistance to a damage type (it doesn't stack with Rage's), and flat reduction of damage from enemies
            if (amount > 0 && (RacialResists(p, d.type) || BoonResists(p, d.type)) && note.find("resisted") == std::string::npos)
            {
                amount /= 2;
                note += RacialResists(p, d.type) ? " (resisted: their blood)" : " (resisted: a hard-won gift)";
            }
            if (amount > 0 && ItemResists(p, d.type) && note.find("resisted") == std::string::npos)
            {
                amount /= 2;
                note += " (resisted)";
            }
            if (amount > 0 && d.attacker && !d.attacker->IsPlayer())
                if (int dr = ItemDamageReduction(p))
                {
                    int cut = std::min(dr, amount);
                    amount -= cut;
                    note += Acore::StringFormat(" (-{} from gear)", cut);
                }
            // Heavy Armour Master: weapon damage (bludgeoning, piercing, slashing) from enemies drops by 3 in heavy armour
            if (amount > 0 && (d.type == DMG_BLUDGEONING || d.type == DMG_PIERCING || d.type == DMG_SLASHING) &&
                !(d.attacker && d.attacker->IsPlayer()) && HasFeat(p, "Heavy Armour Master") && WearsHeavyArmour(p))
            {
                int cut = std::min(3, amount);
                amount -= cut;
                note += Acore::StringFormat(" (Heavy Armour Master: -{})", cut);
            }
            Sheet& s = GetSheet(p);
            if (s.wardHp > 0 && amount > 0)
            {
                int left = AbsorbWithWard(p, amount);
                note += Acore::StringFormat(" ({} absorbed by the Arcane Ward)", amount - left);
                amount = left;
            }
            if (s.tempHp > 0 && amount > 0)
            {
                absorbed = std::min(s.tempHp, amount);
                s.tempHp -= absorbed;
                amount -= absorbed;
                note += Acore::StringFormat(" ({} absorbed by temporary HP)", absorbed);
                PushSheet(p);
            }
            if (WildShapeOf(p) && s.tempHp <= 0)
                EndWildShape(p, " - the beast falls, and the rest of the blow is yours");
        }

        if (amount > 0 && amount >= int(d.victim->GetHealth()) && !IsDowned(d.victim))
            if (Combatant* dw = CombatantOf(d.victim->GetGUID()); dw && dw->deathWard)
            {
                dw->deathWard = false;
                amount = int(d.victim->GetHealth()) - 1;
                note += " (Death Ward: still standing, at 1 hit point)";
            }
        // a brawl: a blow that would drop anyone to 0 leaves them at 1 and out cold instead (nobody dies in a bar fight)
        bool knockOut = false;
        if (Encounter* be = EncounterOf(d.victim->GetGUID()); be && be->brawl && amount > 0 && amount >= int(d.victim->GetHealth()))
        {
            amount = int(d.victim->GetHealth()) - 1;
            knockOut = true;
            note += " (out cold)";
        }
        // a player in a fight: 0 HP knocks them down (only damage past 0 as big as their whole HP kills outright);
        // hitting someone who is already down costs them death saves instead of hit points
        bool knockDown = false;
        if (Player* dp = d.victim->ToPlayer())
        {
            if (IsDowned(dp))
            {
                DownedHit(dp, d.attacker && d.attacker != dp && d.attacker->GetExactDist(dp) <= MeleeRange(d.attacker, dp));
                return 0;
            }
            if (amount > 0 && amount >= int(dp->GetHealth()) && EncounterOf(dp->GetGUID()) &&
                amount - int(dp->GetHealth()) < int(dp->GetMaxHealth()))
            {
                amount = int(dp->GetHealth()) - 1;
                knockDown = true;
                note += " (knocked down)";
            }
        }

        SpellSchoolMask mask = SpellSchoolMask(SchoolMaskFor(d.type));
        Unit* attacker = d.attacker ? d.attacker : d.victim;
        sDealing = true;
        if (d.melee || !d.spell)
        {
            uint32 hitInfo = HITINFO_AFFECTS_VICTIM | (d.crit ? HITINFO_CRITICALHIT : 0);
            attacker->SendAttackStateUpdate(hitInfo, d.victim, 1, mask, amount, absorbed, 0, VICTIMSTATE_HIT, 0);
            Unit::DealDamage(attacker, d.victim, amount, nullptr, DIRECT_DAMAGE, mask, nullptr, false);
        }
        else
        {
            attacker->SendSpellNonMeleeDamageLog(d.victim, d.spell, amount, mask, absorbed, 0, false, 0, d.crit);
            Unit::DealDamage(attacker, d.victim, amount, nullptr, SPELL_DIRECT_DAMAGE, mask, d.spell, false);
        }
        sDealing = false;
        // gore: weapons, claws and teeth draw blood (Blood Strike impact kits; the big splash on a crit)
        if (amount > 0 && (d.type == DMG_PIERCING || d.type == DMG_SLASHING || d.type == DMG_BLUDGEONING))
            d.victim->SendPlaySpellImpact(d.victim->GetGUID(), d.crit ? 11211 : 10467);
        if (!note.empty())
            if (Encounter* e = EncounterOf(d.victim->GetGUID()))
                for (Combatant const& c : e->order)
                    if (c.player)
                        if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                            Log(p, "   " + Name(d.victim) + note);
        if (Player* p = d.victim->ToPlayer())
            PushSheet(p);
        if (Player* tp = d.victim->ToPlayer(); tp && d.attacker && d.attacker->ToCreature() && d.attacker->IsAlive() &&
            amount + int(absorbed) > 0 && d.attacker->GetExactDist(tp) <= MeleeRange(d.attacker, tp))
        {
            static int thornDepth = 0;
            if (thornDepth == 0)
                for (ItemRider const& r : ItemRiders(tp, 0, "thorns"))
                {
                    ++thornDepth;
                    std::string detail;
                    DamageCtx back;
                    back.attacker = tp;
                    back.victim = d.attacker;
                    back.amount = r.dice.Roll(false, detail);
                    back.type = r.type;
                    if (Encounter* te = EncounterOf(tp->GetGUID()))
                        for (Combatant const& c : te->order)
                            if (c.player)
                                if (Player* lp = ObjectAccessor::FindPlayer(c.guid))
                                    Log(lp, Acore::StringFormat("   {}'s {} bites back: {} = {} {}", tp->GetName(), r.item, detail, back.amount,
                                        DmgTypeNames[r.type]));
                    ApplyDamage(back);
                    --thornDepth;
                }
        }
        if (d.attacker && d.attacker != d.victim && d.attacker->IsAlive() && d.melee && amount + int(absorbed) > 0 &&
            d.attacker->IsPlayer() != d.victim->IsPlayer())
            if (Combatant* fs = CombatantOf(d.victim->GetGUID()); fs && fs->fireShield)
            {
                static int shieldDepth = 0;
                if (shieldDepth == 0)
                {
                    ++shieldDepth;
                    std::string detail;
                    DamageCtx back;
                    back.attacker = d.victim;
                    back.victim = d.attacker;
                    back.amount = Dice{ 2, 8, 0 }.Roll(false, detail);
                    back.type = DMG_FIRE;
                    if (Encounter* fe = EncounterOf(d.victim->GetGUID()))
                        for (Combatant const& c : fe->order)
                            if (c.player)
                                if (Player* lp = ObjectAccessor::FindPlayer(c.guid))
                                    Log(lp, Acore::StringFormat("   {}'s Fire Shield burns {}: {} = {} Fire", Name(d.victim), Name(d.attacker),
                                        detail, back.amount));
                    ApplyDamage(back);
                    --shieldDepth;
                }
            }
        if (knockDown)
            KnockDown(d.victim->ToPlayer());        // (concentration ends with it)
        if (knockOut)
            BrawlKnockOut(d.victim);
        AfterDamage(d, amount, int(absorbed));     // Armor of Agathys, Hellish Rebuke, the patrons' gifts
        if (Player* p = d.victim->ToPlayer(); p && !knockDown)
            ConcentrationCheck(p, amount + int(absorbed), d.attacker);   // (damage soaked by temporary HP still counts)
        else if (d.victim->ToCreature())
            ConcentrationCheck(d.victim, amount, d.attacker);            // a casting creature's hold on its spell
        return amount;
    }

    void ApplyHeal(Unit* healer, Unit* target, int amount, SpellInfo const* spell)
    {
        if (!target || !target->IsAlive() || amount <= 0)
            return;
        if (IsDowned(target))
        {
            GetUp(target->ToPlayer(), amount, healer && healer != target ? "healed by " + std::string(healer->GetName()) : "healed");
            return;
        }
        if (Player* tp = target->ToPlayer())
            if (BeastForm const* f = WildShapeOf(tp))
            {
                Sheet& ts = GetSheet(tp);
                ts.tempHp = std::min(f->hp, ts.tempHp + amount);   // healing a beast heals the beast
                PushSheet(tp);
                return;
            }
        uint32 before = target->GetHealth();
        target->SetHealth(std::min<uint32>(target->GetMaxHealth(), before + amount));
        if (spell && healer)
            {
            HealInfo info(healer, target, target->GetHealth() - before, spell, spell->GetSchoolMask());
            healer->SendHealSpellLog(info);
        }
        if (Player* p = target->ToPlayer())
            PushSheet(p);
    }

    // ------------------------------------------------------------------ messaging
    static std::mutex sFileMutex;

    void FileLog(std::string const& text)
    {
        static std::string path = sConfigMgr->GetOption<std::string>("Dnd.LogFile", "logs/dnd.log");
        // strip |cffxxxxxx and |r colour codes
        std::string plain;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '|' && i + 1 < text.size())
            {
                if (text[i + 1] == 'c' && i + 9 < text.size())
                {
                    i += 9;
                    continue;
                }
                if (text[i + 1] == 'r')
                {
                    i += 1;
                    continue;
                }
            }
            plain += text[i];
        }
        std::lock_guard<std::mutex> lock(sFileMutex);
        // a self-test run's dice go to their own file next to it, keeping the play log readable
        static std::string testPath = path.substr(0, path.find_last_of("/\\") + 1) + "dnd_selftest_dice.log";
        std::ofstream out(Test::Running() ? testPath : path, std::ios::app);
        if (!out)
            return;
        std::time_t t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        out << buf << "  " << plain << '\n';
    }

    void Send(Player* p, std::string const& msg)
    {
        if (Test::Running() && Test::Capture(p, msg))
            return;
        if (!p || !p->GetSession())
            return;
        // CHAT_MSG_WHISPER + LANG_ADDON from the player to itself arrives as CHAT_MSG_ADDON("DND", msg)
        std::string full = "DND\t" + msg;
        if (full.size() > 254)
            full.resize(254);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, p, p, full);
        p->SendDirectMessage(&data);
    }

    std::vector<std::string> LogChunks(std::string const& text)
    {
        static std::size_t const MAX = 210;      // + "L+^" + a reopened colour + "|r" stays under the packet limit
        std::vector<std::string> out;
        std::string carry;                       // the colour still open at the end of the last piece
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t len = std::min(MAX, text.size() - pos);
            if (pos + len < text.size())
            {
                std::size_t sp = text.rfind(' ', pos + len);
                if (sp != std::string::npos && sp > pos)
                    len = sp - pos + 1;          // between words (a colour code has no spaces, so it's never cut)
            }
            std::string piece = carry + text.substr(pos, len);
            pos += len;
            std::size_t lc = piece.rfind("|c"), lr = piece.rfind("|r");
            carry = lc != std::string::npos && (lr == std::string::npos || lr < lc) && lc + 10 <= piece.size()
                ? piece.substr(lc, 10) : "";
            if (!carry.empty())
                piece += "|r";
            while (!piece.empty() && piece.back() == ' ')
                piece.pop_back();
            out.push_back(piece);
        }
        return out;
    }

    void SendLog(Player* p, std::string const& kind, std::string const& text)
    {
        if (!p)
            return;
        std::vector<std::string> chunks = LogChunks(text);
        for (std::size_t i = 0; i < chunks.size(); ++i)
            Send(p, kind + (i ? "+^" : "^") + chunks[i]);
    }

    void Log(Player* p, std::string const& text)
    {
        FileLog(Acore::StringFormat("[{}] {}", p ? p->GetName() : "-", text));
        SendLog(p, "L", text);
    }

    void KillOutright(Unit* u)
    {
        if (!u || !u->IsAlive())
            return;
        sDealing = true;
        Unit::Kill(u, u, false);
        sDealing = false;
    }

    std::string ZoneName(Player* p)
    {
        if (p)
            if (AreaTableEntry const* a = sAreaTableStore.LookupEntry(p->GetZoneId()))
                return a->area_name[0];
        return "the wilds";
    }

    void Announce(Player* subject, std::string const& text)
    {
        if (text.empty())
            return;
        FileLog("[update] " + text);
        if (subject && Test::IsBot(subject->GetGUID()))
        {
            Test::Announced(text);
            return;
        }
        AnnounceToAll("|cffffcc00[Update]|r " + text);
    }

    // to every session (this core's ChatHandler::SendWorldText only reaches the handler's own session - with none, the
    // first real announcement crashed the server; the self-tests' bots never got this far: announce.reaches_every_session)
    void AnnounceToAll(std::string const& line)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, line);
        sWorldSessionMgr->SendGlobalMessage(&data);
    }

    void LogFind(Player* p, std::string const& text)
    {
        FileLog(Acore::StringFormat("[{}] {}", p ? p->GetName() : "-", text));
        SendLog(p, "LF", text);
    }

    void Error(Player* p, std::string const& text)
    {
        Send(p, "X^" + text);
    }

    void PushSheet(Player* p)
    {
        if (!p)
            return;
        Sheet& s = GetSheet(p);
        int lvl = Level(p);
        std::string slots;
        for (int i = 1; i <= 6; ++i)
            slots += Acore::StringFormat("^{}^{}", SlotsLeft(p, i), SlotsMax(p, i));
        std::string res;
        if (s.cls == CLASS_FIGHTER)
        {
            res = Acore::StringFormat("Second Wind:{}:1", std::max(0, 1 - s.secondWindUsed));
            if (lvl >= 2)
                res += Acore::StringFormat(",Action Surge:{}:1", std::max(0, 1 - s.actionSurgeUsed));
        }
        if (SuperiorityMax(p) > 0)
            res += Acore::StringFormat("{}Superiority:{}:{}", res.empty() ? "" : ",", std::max(0, SuperiorityMax(p) - s.supUsed),
                SuperiorityMax(p));
        auto add = [&res](char const* name, int left, int max)
        {
            if (max > 0)
                res += Acore::StringFormat("{}{}:{}:{}", res.empty() ? "" : ",", name, std::max(0, left), max);
        };
        add("Rage", RageMax(p) - s.rageUsed, RageMax(p));
        add("Ki", KiMax(p) - s.kiUsed, KiMax(p));
        add("Sorcery", SorceryMax(p) - s.spUsed, SorceryMax(p));
        if (IsSubclass(p, "Wild Magic"))
            add("Tides of Chaos", 1 - s.tidesUsed, 1);
        if (IsSubclass(p, "Open Hand") && lvl >= 6)
            add("Wholeness of Body", 1 - s.wholenessUsed, 1);
        add("Bardic Inspiration", InspirationMax(p) - s.inspirationUsed, InspirationMax(p));
        add("Channel Divinity", ChannelMax(p) - s.channelUsed, ChannelMax(p));
        add("Wrath of the Storm", WrathMax(p) - s.wrathUsed, WrathMax(p));
        add("Wild Shape", WildShapeMax(p) - s.wildShapeUsed, WildShapeMax(p));
        if (s.cls == CLASS_DND_WARLOCK)
        {
            if (IsSubclass(p, "Archfey"))
                add("Fey Presence", 1 - s.patronUsed, 1);
            if (lvl >= 6)
                add(IsSubclass(p, "Fiend") ? "Dark One's Own Luck" : (IsSubclass(p, "Archfey") ? "Misty Escape" : "Entropic Ward"),
                    1 - s.patron6Used, 1);
        }
        Send(p, Acore::StringFormat("S^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}{}", lvl, ClassName(p), s.abil[STR], s.abil[DEX],
            s.abil[CON], s.abil[INT], s.abil[WIS], s.abil[CHA], ArmorClass(p), p->GetHealth(), p->GetMaxHealth(), s.tempHp, Prof(lvl),
            SpellSaveDC(p), SpellAttackBonus(p), Speed(p), 2 - s.shortRestsUsed, slots) +
            Acore::StringFormat("^{}^{}^{}^{}", s.wardHp, s.subclass, [&] { std::string r; for (auto const& k : s.picks)
                r += (r.empty() ? "" : ",") + k; return r; }(), res));
    }
}
