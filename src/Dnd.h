/*
 * mod-dnd: turn-based, D&D 5e / BG3-style rules on top of AzerothCore 3.3.5a.
 *
 * Everything that decides a fight lives here: dice, ability scores, AC, saves, the action economy,
 * initiative and turns. WoW's own combat maths is switched off (UnitScript::DealDamage zeroes any
 * damage we did not deal ourselves), so the only way to hurt anything is through this engine.
 */
#ifndef MOD_DND_H
#define MOD_DND_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

class ChatHandler;
class Creature;
class CreatureAI;
class GameObject;
class Item;
class Player;
class Unit;
class SpellInfo;
class WorldObject;
class Map;

namespace Dnd
{
    // ---------------------------------------------------------------- spell ids (client patch + server DBC)
    enum Spells : uint32
    {
        SPELL_END_TURN          = 200001,
        SPELL_ATTACK            = 200002,
        SPELL_DASH              = 200003,
        SPELL_DISENGAGE         = 200004,
        SPELL_JUMP              = 200005,
        SPELL_SHOVE             = 200006,
        SPELL_HELP              = 200010,   // an ally who is down gets back up with 1 HP
        SPELL_FLEE              = 200011,   // leave the fight: nobody within 20 m of you
        SPELL_THROW             = 200012,   // the main-hand weapon, if it's made to be thrown (dagger, handaxe, spear...)
        // the bard (WoW Paladin)
        SPELL_VICIOUS_MOCKERY   = 201001,   // cantrip: WIS save, psychic, disadvantage on its next attack
        SPELL_BARDIC_INSPIRATION = 201002,  // an ally holds a die for their next roll
        SPELL_HEALING_WORD      = 201003,   // 1st level, bonus action, an ally within 18 m
        SPELL_CURE_WOUNDS       = 201004,   // 1st level, touch
        SPELL_THUNDERWAVE       = 201005,   // 1st level, everything within 4.5 m of you: CON save, thunder, pushed
        SPELL_HYPNOTIC_PATTERN  = 201006,   // 3rd level, a 4.5 m radius: WIS save or entranced until hurt (concentration)
        // the druid (WoW Hunter)
        SPELL_PRODUCE_FLAME     = 201101,   // cantrip: a ranged spell attack, fire
        SPELL_SHILLELAGH        = 201102,   // cantrip, bonus action: a staff or club strikes with WIS and a d8
        SPELL_ENTANGLE          = 201103,   // 1st level, a 6 m radius: STR save or restrained (concentration)
        SPELL_MOONBEAM          = 201104,   // 2nd level: radiant light that burns whoever starts a turn in it (concentration)
        SPELL_CALL_LIGHTNING    = 201105,   // 3rd level: a bolt; again each turn, no slot, while you concentrate
        SPELL_BARKSKIN          = 201106,   // 2nd level (Circle of the Land): AC at least 16 (concentration)
        SPELL_SHAPE_WOLF        = 201110,   // Wild Shape forms
        SPELL_SHAPE_BEAR        = 201111,
        SPELL_SHAPE_DIRE_WOLF   = 201112,
        SPELL_SHAPE_SABRETOOTH  = 201113,
        SPELL_REVERT_SHAPE      = 201114,
        // more spells for the casters (5e lists), built on mechanics already here
        SPELL_CHILL_TOUCH       = 201201,   // cantrip: spell attack, necrotic
        SPELL_ACID_SPLASH       = 201202,   // cantrip: DEX save, acid - the target and one creature next to it
        SPELL_POISON_SPRAY      = 201203,   // cantrip: CON save, poison, 3 m
        SPELL_RAY_OF_SICKNESS   = 201211,   // 1st: spell attack, poison; CON save or Poisoned
        SPELL_WITCH_BOLT        = 201212,   // 1st: spell attack, lightning; while concentrating, again with no slot (auto on the same target)
        SPELL_SLEEP             = 201213,   // 1st: a pool of hit points' worth of creatures fall asleep until hurt
        SPELL_HEROISM           = 201214,   // 1st (bard): temporary hit points every turn while concentrating
        SPELL_CLOUD_OF_DAGGERS  = 201221,   // 2nd: blades in a small spot, 4d4 to whoever is in it or starts a turn there
        SPELL_WEB               = 201222,   // 2nd: DEX save or restrained
        SPELL_BLUR              = 201223,   // 2nd: attacks on you have disadvantage (concentration)
        SPELL_FEAR              = 201231,   // 3rd: a cone, WIS save or frightened (concentration)
        SPELL_STINKING_CLOUD    = 201232,   // 3rd: CON save at the start of a turn in it, or the turn is lost
        SPELL_HUNGER_OF_HADAR   = 201233,   // 3rd (warlock): cold at the start of a turn in it; difficult terrain
        SPELL_SLEET_STORM       = 201234,   // 3rd: DEX save at the start of a turn in it, or prone; difficult terrain
        // the cleric (WoW Death Knight)
        SPELL_SACRED_FLAME      = 201301,   // cantrip: DEX save, radiant
        SPELL_TOLL_THE_DEAD     = 201302,   // cantrip: WIS save, necrotic - a d12 against the wounded
        SPELL_BLESS             = 201311,   // 1st: three allies add a d4 to attacks and saves (concentration)
        SPELL_GUIDING_BOLT      = 201312,   // 1st: spell attack, radiant; the next attack on it has advantage
        SPELL_INFLICT_WOUNDS    = 201313,   // 1st: melee spell attack, necrotic
        SPELL_SHIELD_OF_FAITH   = 201314,   // 1st: +2 AC to an ally (concentration)
        SPELL_AID               = 201321,   // 2nd: three allies heal 5 and gain 5 temporary hit points
        SPELL_LESSER_RESTORATION = 201322,  // 2nd: ends Poisoned
        SPELL_PRAYER_OF_HEALING = 201323,   // 2nd, out of a fight: six allies heal 2d8 + modifier
        SPELL_SPIRIT_GUARDIANS  = 201331,   // 3rd: spirits around you; WIS save at the start of a creature's turn near you
        SPELL_MASS_HEALING_WORD = 201332,   // 3rd, bonus action: six allies heal 1d4 + modifier
        SPELL_TURN_UNDEAD       = 201341,   // Channel Divinity: undead within 9 m, WIS save or turned (destroyed from 5)
        SPELL_PRESERVE_LIFE     = 201342,   // Channel Divinity (Life): 5 x level healing shared out, up to half health
        SPELL_DESTRUCTIVE_WRATH = 201343,
        // special arrows (bag items; a bow or crossbow shot, the arrow's rider on a hit) and grenades (anyone throws one:
        // a 3 m burst, Dexterity save DC 13) - tools/shops.py sells them, make_patch.py has their tooltips
        SPELL_ARROW_FIRE        = 201401,   // +1d6 Fire, and it catches fire
        SPELL_ARROW_ICE         = 201402,   // +1d6 Cold, and slowed
        SPELL_ARROW_ACID        = 201403,   // +1d6 Acid, and -2 AC for two rounds
        SPELL_ARROW_LIGHTNING   = 201404,   // +1d6 Lightning, and 1d6 arcs to the nearest foe within 3 m (DEX save, half)
        SPELL_ARROW_SLAYING     = 201405,   // +2d8 against anything that isn't a humanoid
        SPELL_ALCHEMISTS_FIRE   = 201411,   // 2d4 Fire; a failed save catches fire
        SPELL_ACID_VIAL         = 201412,   // 2d6 Acid; a failed save loses 2 AC for two rounds
        SPELL_SMOKEPOWDER       = 201413,   // 3d6 Fire
        SPELL_HOLY_WATER        = 201414,   // 2d6 Radiant, undead and demons only
        SPELL_FROST_BOMB        = 201415,   // 2d4 Cold; a failed save knocks it prone   // Channel Divinity (Tempest): the next lightning or thunder damage is maximised
        SPELL_SHORT_REST        = 200007,
        SPELL_RANGED_ATTACK     = 200008,   // the ranged-slot weapon (bow, crossbow, gun, thrown)
        SPELL_OFFHAND_ATTACK    = 200009,   // two-weapon fighting: the off-hand weapon, as a Bonus Action

        SPELL_FIRE_BOLT         = 200101,
        SPELL_RAY_OF_FROST      = 200102,
        SPELL_SHOCKING_GRASP    = 200103,

        SPELL_MAGIC_MISSILE     = 200111,
        SPELL_BURNING_HANDS     = 200112,
        SPELL_CHROMATIC_FIRE    = 200113,
        SPELL_CHROMATIC_COLD    = 200114,
        SPELL_CHROMATIC_LIGHT   = 200115,
        SPELL_ICE_KNIFE         = 200116,
        SPELL_MAGE_ARMOR        = 200117,
        SPELL_FALSE_LIFE        = 200118,
        SPELL_GREASE            = 200119,
        SPELL_ENHANCED_LEAP     = 200120,

        SPELL_SCORCHING_RAY     = 200121,   // 2nd level
        SPELL_MISTY_STEP        = 200122,
        SPELL_SHATTER           = 200123,
        SPELL_HOLD_PERSON       = 200124,
        SPELL_FIREBALL          = 200131,   // 3rd level
        SPELL_LIGHTNING_BOLT    = 200132,

        SPELL_SECOND_WIND       = 200201,   // fighter
        SPELL_ACTION_SURGE      = 200202,
        SPELL_TRIP_ATTACK       = 200203,   // Battle Master maneuvers
        SPELL_PUSHING_ATTACK    = 200204,
        SPELL_MENACING_ATTACK   = 200205,

        SPELL_RAGE              = 200401,   // barbarian
        SPELL_RECKLESS_ATTACK   = 200402,
        SPELL_FRENZIED_STRIKE   = 200403,   //   Berserker
        SPELL_RAGE_AURA         = 200409,   //   (the buff shown while raging)
        SPELL_HIDE              = 200501,   // rogue
        SPELL_CUNNING_DASH      = 200502,
        SPELL_CUNNING_DISENGAGE = 200503,
        SPELL_HIDDEN_AURA       = 200509,   //   (the buff shown while hidden)
        SPELL_SNEAK_AURA        = 200510,   //   sneaking out of combat: WoW's stealth look (MOD_STEALTH)
        SPELL_UNARMED_STRIKE    = 200601,   // monk: Martial Arts' bonus strike
        SPELL_FLURRY            = 200602,
        SPELL_PATIENT_DEFENSE   = 200603,
        SPELL_STEP_OF_THE_WIND  = 200604,
        SPELL_STUNNING_STRIKE   = 200605,
        SPELL_WHOLENESS         = 200606,   //   Open Hand, level 6
        SPELL_CLOAK_OF_SHADOWS  = 200607,   //   Shadow, level 3
        SPELL_SHADOW_STEP       = 200608,   //   Shadow, level 6
        SPELL_DODGE_AURA        = 200609,   //   (Patient Defense's buff)
        SPELL_QUICKENED         = 200701,   // sorcerer: metamagic
        SPELL_TWINNED           = 200702,
        SPELL_HEIGHTENED        = 200703,
        SPELL_EMPOWERED         = 200704,
        SPELL_CREATE_SLOT       = 200705,   //   Font of Magic
        SPELL_CONVERT_SLOT      = 200706,
        SPELL_TIDES_OF_CHAOS    = 200707,   //   Wild Magic
        SPELL_BEND_LUCK         = 200708,   //   Wild Magic, level 6
        SPELL_ELDRITCH_BLAST    = 200801,   // warlock
        SPELL_HEX               = 200802,
        SPELL_ARMOR_OF_AGATHYS  = 200803,
        SPELL_HELLISH_REBUKE    = 200804,   //   readied: it answers the next creature that hurts you (your reaction)
        SPELL_ARMS_OF_HADAR     = 200805,
        SPELL_DISSONANT_WHISPERS = 200806,  //   Great Old One
        SPELL_FAERIE_FIRE       = 200807,   //   Archfey
        SPELL_VAMPIRIC_TOUCH    = 200808,   //   3rd level
        SPELL_FEY_PRESENCE      = 200809,   //   Archfey
        SPELL_DARK_ONES_LUCK    = 200810,   //   Fiend, level 6
        SPELL_HEX_AURA          = 200811,   //   (shown on the hexed creature)
        SPELL_REBUKE_AURA       = 200812,   //   (Hellish Rebuke readied)
        SPELL_AGATHYS_AURA      = 200813,   //   (Armor of Agathys up)

        SPELL_CREATURE_THROW    = 200901,   // enemy missiles, by what they throw (CreatureMissile)
        SPELL_CREATURE_SPARK    = 200902,   //   lightning
        SPELL_CREATURE_FROST    = 200903,   //   cold
        SPELL_CREATURE_FIRE     = 200904,   //   fire
        SPELL_CREATURE_SHOOT    = 200905,   //   a bow / crossbow in the ranged slot (an arrow)
        SPELL_CREATURE_SPEAR    = 200906,   //   a piercing throw (a spear)
        SPELL_CREATURE_AXE      = 200907,   //   a slashing throw (a hand axe)
        SPELL_CREATURE_GUN      = 200908,   //   a gun in the ranged slot (a bullet)
        SPELL_FALLEN            = 200950,   // dead in a fight with others about: no releasing, wait for Brother Marrow

        SPELL_HEARTHSTONE       = 8690,
    };

    enum Ability : uint8 { STR, DEX, CON, INT, WIS, CHA, ABILITY_COUNT };
    extern char const* const AbilityNames[ABILITY_COUNT];

    enum DmgType : uint8
    {
        DMG_BLUDGEONING, DMG_PIERCING, DMG_SLASHING, DMG_FIRE, DMG_COLD, DMG_LIGHTNING, DMG_THUNDER,
        DMG_ACID, DMG_POISON, DMG_FORCE, DMG_NECROTIC, DMG_RADIANT, DMG_PSYCHIC, DMG_TYPE_COUNT
    };
    extern char const* const DmgTypeNames[DMG_TYPE_COUNT];
    uint32 SchoolMaskFor(DmgType t);
    DmgType ParseDmgType(std::string const& s);

    enum Cost : uint8 { COST_NONE, COST_ACTION, COST_BONUS };

    // ---------------------------------------------------------------- dice
    int D(int sides);
    int Mod(int score);

    struct Dice
    {
        int count = 0, sides = 0, bonus = 0;
        bool rerollLow = false;              // Great Weapon Fighting: 1s and 2s are rolled again (once)
        bool minTwo = false;                 // Elemental Adept: a 1 on a damage die counts as a 2
        bool rollTwice = false;              // Savage Attacker: roll the damage dice twice, keep the better set
        bool maximize = false;               // Destructive Wrath: every die at its highest
        static Dice Parse(std::string const& s);  // "2d6+3", "1d4", "5"
        int Roll(bool crit, std::string& detail, std::vector<int>* rolls = nullptr) const;
        std::string Str() const;
    };

    struct D20Result { int total = 0; int natural = 0; std::string text; };
    // adv > 0 advantage, < 0 disadvantage
    D20Result RollD20(int bonus, int adv);

    // ---------------------------------------------------------------- characters
    // from the WoW class picked at creation: Mage, Warrior, Shaman, Rogue, Druid, Warlock (DndClasses.cpp)
    enum DndClass : uint8 { CLASS_NONE = 0, CLASS_WIZARD = 1, CLASS_FIGHTER = 2, CLASS_BARBARIAN = 3, CLASS_DND_ROGUE = 4,
        CLASS_MONK = 5, CLASS_SORCERER = 6, CLASS_DND_WARLOCK = 7, CLASS_BARD = 8, CLASS_DND_DRUID = 9, CLASS_CLERIC = 10 };
    static int const MAX_DND_LEVEL = 6;       // the levels built so far (spells, features, slots)

    struct Sheet
    {
        ObjectGuid guid;
        uint8 cls = CLASS_WIZARD;
        std::array<int, ABILITY_COUNT> abil{ {8, 14, 14, 17, 12, 10} };
        std::array<int, 10> slotsUsed{};       // index = spell level (1..9)
        int tempHp = 0;
        int shortRestsUsed = 0;
        bool arcaneRecoveryUsed = false;
        std::string background;              // a 5e background: two skill proficiencies
        std::vector<std::string> skills;     // every proficient skill (background + class picks)
        bool created = false;                // the player has made their build choices
        std::string build;                   // the builder's raw choices, to reopen it pre-filled
        std::string subclass;                // Arcane Tradition from level 2: "Abjuration" / "Evocation" ("" = not chosen)
        std::vector<std::string> picks;      // level-4 choice: "asi:INT" (+2) or "feat:Alert" / "feat:Tough"; level 1: "feat1:Mobile"
        int wardHp = 0;                      // Abjuration's Arcane Ward (0 = down)
        bool wardMade = false;               // the ward has been raised since the last long rest
        // fighter resources, spent since the last rest: Second Wind, Action Surge (short rest), superiority dice
        int secondWindUsed = 0, actionSurgeUsed = 0, supUsed = 0;
        // the other classes': rages (long rest), ki (short), sorcery points (long; converting slots can refill them),
        // Tides of Chaos (long, or a wild surge), Wholeness of Body (long)
        int rageUsed = 0, kiUsed = 0, spUsed = 0, tidesUsed = 0, wholenessUsed = 0;
        // the warlock's: the patron's level 1 feature (Fey Presence) and level 6 one (Dark One's Own Luck, Entropic Ward,
        // Misty Escape), each once a short rest; Armor of Agathys' cold while its temporary hit points last
        int patronUsed = 0, patron6Used = 0, agathys = 0;
        // the bard's Bardic Inspiration (long rest; a short one from level 5), the druid's Wild Shape (short rest) and
        // Natural Recovery (Circle of the Land, once a long rest)
        int inspirationUsed = 0, wildShapeUsed = 0, naturalRecoveryUsed = 0;
        // the cleric's Channel Divinity (short rest) and the Tempest's Wrath of the Storm (long rest)
        int channelUsed = 0, wrathUsed = 0;
        bool loaded = false;
    };

    Sheet& GetSheet(Player* p);
    void LoadSheet(Player* p);
    void SaveSheet(Player* p);
    void ForgetSheet(ObjectGuid guid);

    int Level(Player* p);
    int Prof(int level);
    int MaxHp(Player* p);
    int SlotsMax(Player* p, int spellLevel);
    int SlotsMaxAt(int level, int spellLevel);   // the slot table for a given character level
    int SlotsLeft(Player* p, int spellLevel);
    float Speed(Player* p);              // metres (= WoW yards) per turn
    int HeavyArmorStrNeeded(Player* p);  // STR the worn heavy armour asks for (0: none)
    float JumpDistance(Player* p);
    int SpellSaveDC(Player* p);
    int CastingMod(Player* p);               // the spellcasting ability's modifier (0 for a class without one; a scroll's own)
    // a spell read from a scroll: anyone can, no slot - and it goes off with the scroll's numbers (5e: DC 13 and +5 for
    // 1st and 2nd level, DC 15 and +7 for 3rd), for the next few seconds (a missile lands after the cast)
    void ReadScroll(Player* p, int spellLevel);
    int SpellAttackBonus(Player* p);
    bool IsFighter(Player* p);
    char const* ClassName(Player* p);
    int SuperiorityMax(Player* p);           // Battle Master: 4 dice from level 3 (0 otherwise)
    int ManeuverDC(Player* p);               // 8 + proficiency + STR or DEX
    bool HasStyle(Player* p, char const* style);   // Fighting Style: "Defense", "Dueling", "Great Weapon Fighting"

    // ---------------------------------------------------------------- creature stat blocks (world: dnd_creature)
    struct Attack
    {
        std::string name;
        int bonus = 0;
        Dice dice;
        DmgType type = DMG_BLUDGEONING;
        float range = 0.f;               // 0 = melee
        bool valid() const { return dice.sides > 0 || dice.bonus > 0; }
    };

    struct CreatureStats
    {
        uint32 entry = 0;
        int ac = 10, hp = 4;
        float speed = 9.f;
        std::array<int, ABILITY_COUNT> abil{ {10, 10, 10, 10, 10, 10} };
        Attack melee, ranged;
        uint32 xp = 10;
        uint32 resist = 0, immune = 0, vuln = 0;   // bit per DmgType
        int venomDc = 0;                     // melee hits: CON save or take 'venom' poison damage and be Poisoned (half on a save)
        Dice venom;
    };

    CreatureStats const* GetStats(uint32 entry);
    CreatureStats const& StatsOrDefault(Creature* c);

    struct EncounterDef
    {
        uint32 id = 0;
        std::string name;
        float aggroRange = 12.f;
        float joinRange = 30.f;
        uint32 zoneId = 0;                   // dnd_zone.id the encounter counts toward
        uint32 xp = 0;                       // per player, first clear only
        uint32 dialogueNode = 0;             // first dnd_dialogue_node (0: a plain fight)
        int patience = 0;                    // the talker's patience (0: no meter)
        uint32 snapNode = 0;                 // shown when patience runs out
        std::string meterLabel;              // what the meter is called ("Patience", "Restraint", ...)
        uint32 sourNode = 0;                 // a secured talk that goes wrong ends here instead of in a fight
        uint32 minion = 0;                   // a lone creature's underlings against a party (creature entry; 0: legendary actions)
        uint8 legendary = 0;                 // (an endgame boss) legendary actions a round whatever the party, even alone
        std::string afterText;               // shown when a character who already resolved it talks again
        std::vector<uint32> spawns;          // creature.guid
    };
    EncounterDef const* GetEncounterDefBySpawn(uint32 spawnId);
    EncounterDef const* GetEncounterDef(uint32 id);
    std::vector<EncounterDef const*> AllEncounters();   // by id
    void LoadWorldData();
    void CheckSpawns(ChatHandler* handler, bool fix);   // DndPlacement.cpp
    void ProbeGate(ChatHandler* handler, uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2);

    // ---------------------------------------------------------------- zones and progression (DndProgress.cpp)
    constexpr uint8 CLOSED_ZONE_LEVEL = 99;  // a zone not built yet: shut, with a gate and a zone line
    struct ZoneDef
    {
        uint32 id = 0;
        std::string name;
        uint8 minLevel = 1;                  // below this level the zone warns you (CLOSED_ZONE_LEVEL: not built, shut)
        std::vector<uint32> areas;           // AreaTable ids (an area, or a whole zone)
        std::string gateText;
        std::string clearText;
        std::vector<uint32> encounters;      // dnd_encounter ids, in order
    };
    void LoadZones();
    ZoneDef const* GetZone(uint32 id);
    void LoadProgress(Player* p);
    void SyncXp(Player* p);                  // XP a crash lost (clears kept) comes back at login
    void FitGate(ChatHandler* handler, uint32 mapId, float x1, float y1, float z1, float x2, float y2, float z2);
    void ForgetProgress(ObjectGuid guid);
    void ResetProgress(Player* p);
    bool HasCleared(Player* p, uint32 encounterId);
    void OnEncounterWon(Player* p, EncounterDef const& def, bool peaceful = false, bool halfXp = false);
    // a win counts for everyone online: the winners, then every other player (a bot's win: only the other bots)
    void ShareEncounterWon(std::vector<Player*> const& winners, EncounterDef const& def, bool peaceful = false,
                           bool halfXp = false);
    void PushProgress(Player* p);
    void UpdateGate(Player* p, uint32 diff);

    // ---------------------------------------------------------------- dialogue (DndDialogue.cpp)
    void LoadDialogue();
    // clicked: a right-click (watches a busy talk again even after Escape closed it)
    bool StartDialogue(Player* p, Creature* speaker, bool clicked = false);
    ObjectGuid WatchedTalker(Player* p);     // whose conversation this player is watching (Empty: nobody's)
    void PullGroupIntoFight(Creature* c, Player* p);     // a group caught by your spell mid-fight joins it (DndCombat.cpp)
    void DialogueChoose(Player* p, uint32 idx);
    void DialogueHover(Player* p, uint32 idx);   // the talker's mouse over an option: onlookers see it (DH^idx)
    void ListenIn(Player* p, std::string const& talker);   // "Listen in": watch a nearby player's villager chat
    void CloseDialogue(Player* p, bool leave, uint32 cooldownMs = 45000);
    int SkillBonusByName(Player* p, std::string const& skill);
    bool MeetsRequirement(Player* p, std::string const& req);

    // treats (DndTreats.cpp): small discoveries in the world
    void LoadTreats();
    void LoadTreatProgress(Player* p);
    void ResetTreats(Player* p);              // (also gives back the treats this character took)
    uint32 ReturnAllTreats();                 // every treat back for everyone (GM: .dnd treatsback); how many came back
    void ForgetTreats(ObjectGuid guid);
    void UpdateTreats(Player* p, uint32 diff);
    std::vector<std::string> TreatFlags(Player* p);
    bool TreatTellHiddenFor(Player const* p, GameObject const* go);    // claimed: the tell's gone, for them only
    void UpdateFood(Player* p, uint32 diff);                          // DndSpells.cpp: food's slow heal
    void TreatGameResult(Player* p, std::string const& args);      // the addon's minigame answer
    // tavern games (DndTavern.cpp): dice, cards and drinking contests with villagers, for a wager
    void OpenTavern(Player* p, Creature* npc, std::string const& games);
    void MakeHome(Player* p, Creature* npc);    // an innkeeper's bind: the hearthstone comes back to this inn
    void UpdateTavern(Player* p, uint32 diff);  // a player at a table: offer "Watch the game" to whoever is near
    void TavernWatch(Player* w, std::string const& player);   // "Watch the game" pressed: the table, read-only
    void TavernMessage(Player* p, std::string const& msg);
    void ForgetTavern(ObjectGuid guid);
    void ResetTavern(Player* p);
    void SetTestTemper(int temper);           // self-test: every table's host calm (0) / hot-headed (1); -1 = their own
    std::string MoneyText(uint32 copper);     // "1 gold 20 silver": money as the bag shows it
    // an encounter's wager (a "wager:game:copper" dialogue option): one hand at a table against the talker, whose result
    // the talk picks up (WagerResult: 1 won, 0 push, -1 lost, -2 caught cheating)
    bool OpenWager(Player* p, Creature* npc, std::string const& game, uint32 stake);
    void WagerResult(Player* p, int outcome);
    // tavern brawls (DndCombat.cpp): a villager squares up to a player - a fight where 0 HP is out cold, not dying
    bool StartBrawl(Player* p, Creature* npc);
    void BrawlKnockOut(Unit* u);               // the damage gate: a brawler at 1 HP, out of the fight
    bool IsBrawler(ObjectGuid g);
    CreatureStats const& BrawlerStats();
    void TavernBrawlOver(Player* p, Creature* host, bool won);   // DndTavern.cpp: the brawl's stakes settle
    void AddTestTreat(uint32 id, Player* at, std::string const& game, int dc, uint32 money, bool passive = false);   // self-test jig
    void SetTestTreat(uint32 id, std::string const& flag, std::string const& hint, std::string const& grant, uint32 item);
    int MagicBonus(Player* p, uint8 slot);         // DndSpells.cpp: a +N weapon's bonus to attack and damage
    void RemoveTestTreat(uint32 id);
    void UseTestTreat(Player* p, uint32 id);   // leave: no re-trigger for cooldownMs
    void SendCard(Player* p, std::string const& kind, std::string const& title, std::string const& text,
        std::string const& reward);

    // townsfolk (DndDialogue.cpp): any friendly villager in the open lands has a short talk with one check and a
    // small once-per-character reward for winning it
    void LoadTownsfolk();
    bool IsTownsfolk(Creature* c);
    std::string TownRoleName(Creature* c);           // the villager's role ("guard", "folk"...), "" if none
    bool StartTownTalk(Player* p, Creature* c);
    void LoadTownProgress(Player* p);
    void ResetTownsfolk(Player* p);
    void UpdateDialogue(Player* p, uint32 diff);
    void ForgetDialogue(ObjectGuid guid);
    bool InDialogue(Player* p);
    bool DialogueDeclined(Player* p, uint32 encounterId);
    // once a talker's group has fought a character, it never talks to them again: it attacks on sight
    void AddGrudge(Player* p, uint32 encounterId);
    bool HasGrudge(Player* p, uint32 encounterId);
    void ClearGrudges(Player* p);
    void ForgetGrudge(Player* p, uint32 encounterId);   // (a wipe: the fight never happened, so they'd talk again)
    bool IsLeaving(ObjectGuid creature);     // walking away after a talk: not part of any fight
    void ClearLeaving(ObjectGuid creature);

    // ---------------------------------------------------------------- generic queries used by both sides
    int AbilityScore(Unit* u, Ability a);
    int AbilityMod(Unit* u, Ability a);
    int ArmorClass(Unit* u);
    int SaveBonus(Unit* u, Ability a);
    bool SaveAdvantage(Unit* u, Ability a, bool magic, bool poison = false);
    float MeleeRange(Unit const* a, Unit const* b);
    std::string Name(Unit* u);

    // ---------------------------------------------------------------- damage
    struct DamageCtx
    {
        Unit* attacker = nullptr;
        Unit* victim = nullptr;
        int amount = 0;
        DmgType type = DMG_BLUDGEONING;
        SpellInfo const* spell = nullptr;
        bool crit = false;
        bool melee = false;                  // shown as a white swing instead of a spell hit
    };
    int ApplyDamage(DamageCtx const& d);     // returns damage actually taken (after resistances / temp HP)
    void ApplyHeal(Unit* healer, Unit* target, int amount, SpellInfo const* spell);
    bool IsDealing();                        // true while ApplyDamage runs (the WoW damage gate)

    // ---------------------------------------------------------------- messaging / log
    void Send(Player* p, std::string const& msg);           // addon message, prefix DND
    void Log(Player* p, std::string const& text);           // one player's dice log + file
    // a line in everyone's chat: "[Update] Burter has fallen fighting Old Icebeard in Dun Morogh." (deaths, finds,
    // victories, zones cleared, levels). About a self-test bot: kept by the jig instead
    void Announce(Player* subject, std::string const& text);
    std::string ZoneName(Player* p);                         // the zone the player stands in, as the map names it
    void AnnounceToAll(std::string const& line);             // a system line in every session's chat
    void FileLog(std::string const& text);
    void Error(Player* p, std::string const& text);         // red UI error
    void PushSheet(Player* p);
    void HandleClientMessage(Player* p, std::string const& msg);
    void HandleBuild(Player* p, std::string const& args);   // the character creation screen's choices
    void PushSkills(Player* p);

    // ---------------------------------------------------------------- combat
    struct Combatant
    {
        ObjectGuid guid;
        bool player = false;
        std::string name;
        int initiative = 0;
        int dexMod = 0;
        bool alive = true;
        bool gone = false;                   // logged out / left the map
        // turn economy
        bool actionUsed = false, bonusUsed = false, reactionUsed = false;
        float moveMax = 0.f, moved = 0.f;
        bool disengaged = false;
        Position lastPos;
        uint32 ignoreMoveUntil = 0;
        // conditions
        bool prone = false;
        bool slowed = false;                 // Ray of Frost: -3 m on its next turn
        bool noReaction = false;             // Shocking Grasp: no reactions until its next turn
        bool surprised = false;              // ambushed: loses its first turn
        bool held = false;                   // Hold Person: paralysed until it makes its save
        bool frightened = false;             // Menacing Attack: disadvantage on its attacks...
        ObjectGuid frightenedBy;             // ...until the start of this combatant's next turn
        uint8 poisoned = 0;                  // venom: disadvantage on attacks and checks; turn starts until it wears off (2 = through its next turn)
        uint8 extraAttacks = 0;              // Extra Attack: swings left in the Attack action already taken
        bool dashed = false;                 // took the Dash action this turn (Mobile: difficult terrain doesn't slow)
        bool gwmReady = false;               // Great Weapon Master: a melee crit or kill this turn - one Bonus Action attack
        std::vector<ObjectGuid> meleeAttacked;   // attacked in melee this turn (Mobile: they get no Opportunity Attack)
        bool raging = false;                 // Rage: +2 melee damage, resistant to weapons (to the end of the fight)
        bool reckless = false;               // Reckless Attack: advantage on STR melee, attacks on you too (to your next turn)
        bool hidden = false;                 // Hide: your next attack has advantage, attacks on you disadvantage
        bool dodging = false;                // Patient Defense: attacks on you have disadvantage (to your next turn)
        uint8 stunned = 0;                   // Stunning Strike: turns lost; attacks on it have advantage
        bool hasActed = false;               // its first turn has begun (Assassinate: advantage before that)
        bool attackedUnarmed = false;        // took the Attack action with a monk weapon (Martial Arts' bonus strike)
        bool sneakUsed = false;              // Sneak Attack: once a turn
        uint8 bonusExtra = 0;                // Fast Hands (Thief): a second Bonus Action
        bool stepJump = false;               // Step of the Wind: jumps doubled this turn
        bool attackedLight = false;          // took the Attack action with a one-handed weapon (the off-hand may follow)
        ObjectGuid hexedBy;                  // Hex: the warlock whose attacks add 1d6 necrotic against it
        bool hexFree = false;                // (a warlock) the hexed creature died: Hex moves without a slot
        bool rebukeReady = false;            // Hellish Rebuke readied
        bool outlined = false;               // Faerie Fire: attacks against it have advantage
        bool luckArmed = false;              // Dark One's Own Luck: +1d10 on the next save
        ObjectGuid entropic;                 // Entropic Ward missed: the next attack on this foe has advantage
        uint32 baseHp = 0;                   // (a creature) its stat block's hit points, before scaling for the party
        bool extra = false;                  // (a creature) brought in for a bigger party: gone when the fight ends, no XP
        uint32 concentration = 0;            // the concentration spell this caster keeps up (Hold Person, Hex, Faerie Fire)
        bool downed = false, stable = false; // (a player) at 0 HP: down, rolling death saves until stable, up or dead
        bool mocked = false;                 // Vicious Mockery: disadvantage on its next attack
        bool entranced = false;              // Hypnotic Pattern: does nothing until hurt (or the caster lets go)
        bool restrained = false;             // Entangle: speed 0, attacks on it have advantage, its own disadvantage
        bool shillelagh = false;             // (a druid) Shillelagh on the staff or club for the fight
        ObjectGuid entrancedBy, restrainedBy;
        bool asleep = false;                 // Sleep: out until hurt (attacks on it have advantage)
        bool afraid = false;                 // Fear: disadvantage on its attacks; a WIS save each turn ends it
        ObjectGuid afraidBy, heroismBy;      // whose Fear / Heroism it is
        ObjectGuid witchTarget;              // (a caster) the one Witch Bolt's arc is fixed on
        bool turned = false;                 // Turn Undead: cowers, does nothing until hurt
        bool guided = false;                 // Guiding Bolt: the next attack on it has advantage
        ObjectGuid blessedBy, faithBy;       // whose Bless / Shield of Faith is on it
        bool wrathArmed = false, wrathFired = false;   // (a Tempest cleric) Destructive Wrath ready / spent on this cast
        uint8 saveOk = 0, saveFail = 0;      // death saves so far
        ObjectGuid heldBy, outlinedBy;       // whose Hold Person / Faerie Fire is on it (it ends with their concentration)
        ObjectGuid burningBy;                // on fire (Alchemist's Fire, an Arrow of Fire): 1d4 at its turn start, DC 10 DEX puts it out
        uint8 corroded = 0;                  // acid: -2 AC; turn starts until it wears off
        uint8 legendary = 0, legendaryMax = 0;   // (a lone creature against a party) legendary actions left / a round
        bool lairHeld = false;               // held by a lair action's grip until the next round's top
    };

    struct Area
    {
        uint32 spellId;
        Position pos;
        float radius;
        uint32 expiresMs;
        ObjectGuid owner;                    // whose spell it is (Moonbeam: whose save DC, and whose concentration)
    };

    struct Encounter
    {
        enum Phase : uint8 { PHASE_WAIT_PLAYER, PHASE_AI_START, PHASE_AI_MOVING, PHASE_AI_ACT, PHASE_AI_STRIKE, PHASE_AI_END, PHASE_NEXT,
            PHASE_DONE };

        uint32 id = 0;
        uint32 defId = 0;
        std::string name;
        uint32 mapId = 0, instanceId = 0;
        bool brawl = false;                  // a tavern brawl: nobody dies - 0 HP is out cold (DndTavern.cpp starts them)
        ObjectGuid brawlPlayer;              // who it started with (the stakes are theirs)
        int party = 1;                       // players in the fight: the creatures scale with it (ScaleForParty)
        int extras = 0;                      // creatures brought in for the party so far
        bool legendaryHelp = false;          // a lone creature fights the party with legendary actions
        uint32 legendaryKey = 0;             // the turn whose end a legendary action was last considered at
        std::vector<Combatant> order;
        int cur = -1;
        int round = 0;
        Phase phase = PHASE_NEXT;
        int32 timer = 0;
        uint32 turnStartedMs = 0;
        uint32 lastEconomyPush = 0;
        uint32 lastOrderPush = 0;
        uint32 lastPositionPush = 0;
        std::vector<Area> areas;
        ObjectGuid aiTarget;
        bool aiRanged = false;
        uint8 aiStrike = 0;                  // wind-up in progress: 1 melee, 2 ranged
        uint8 aiStrikeStage = 0;             // 0 = rolled, ready stance; 1 = swinging / missile away
        int weatherRound = 0;                // the last round the weather had its say (blizzard cold, lightning)
        int lairLast = -1;                   // the lair action taken last round (never the same twice running)
        std::vector<ObjectGuid> lairCalled;  // who a lair action has called in

        Combatant* Find(ObjectGuid g);
        Combatant* Current() { return (cur >= 0 && cur < int(order.size())) ? &order[cur] : nullptr; }
        Unit* GetUnit(Combatant const& c) const;
    };

    Encounter* EncounterOf(ObjectGuid g);
    // every running fight, one line per side member: health, faction, unit flags, where, distance / line of sight /
    // hostility to the nearest player (`.dnd fightdump`; the campaign writes it to the dice log when a fight stalls)
    std::string FightDump();
    Combatant* CombatantOf(ObjectGuid g);
    bool IsTurnOf(Unit* u);
    bool InCombat(Unit* u);
    void GrantGrace(Player* p, uint32 ms);   // no fight can start with this player for ms (just revived)
    bool InGrace(Unit* u);
    bool RespawnSpawn(Map* map, ObjectGuid::LowType spawnId);   // revive a spawn by creature.guid (dynamic respawns)

    void StartEncounter(Creature* source, Unit* trigger);
    void TryJoin(Player* p);
    // a player outside any fight who aims at someone in one (a shot, a spell, a blast's ground) joins that fight
    bool JoinFightOf(Player* p, Unit* target);
    // a talker's group between fights: one that would be friendly stands neutral with a talk cursor, so anyone can still
    // attack it (a right-click talks; a spell or a swing starts the fight - and cuts short anyone's conversation with it)
    void TalkerAtRest(Creature* c);
    // downed (DndCombat.cpp): 0 HP in a fight knocks a player down instead of killing them. Death saves on their turns
    // (10+, three to be stable; three failures and they die; a natural 20 gets them up, a 1 counts twice); damage while
    // down is a failure (two from close by). Help (an Action, next to them), healing or a thrown potion gets them up.
    // A won fight leaves them down until someone does; a fight with nobody left standing kills them.
    // fleeing (DndCombat.cpp): on your turn, with no living enemy within FLEE_RANGE of you, you can leave the fight. The
    // last one standing to flee ends it as an escape; a party fights on without you (walking near won't pull you back
    // in; casting at it will)
    bool CanFlee(Player* p, std::string& why);
    void Flee(Player* p);
    bool IsDowned(Unit* u);
    void KnockDown(Player* p);
    void GetUp(Player* p, int hp, std::string const& how);
    void DownedHit(Player* p, bool close);
    void RollDeathSave(Player* p);           // the downed player's turn (and the self-test's way in)
    bool SomeoneElseOnline(Player* p);       // another living player in the world (a friend who can raise the fallen)
    void UpdateFallen(Player* p, uint32 diff); // the fallen may release after all once everyone else has gone
    void UpdateDowned(Player* p);            // keeps the downed lying there
    void ForgetDowned(ObjectGuid g);
    void KillOutright(Unit* u);              // DndRules.cpp: a death the damage gate lets through
    // Brother Marrow (every graveyard's spirit healer, dnd_loader.cpp): raises the dead beside him - for a little silver
    // paid by a living friend, or for nothing when a ghost comes to him. Returns false if it couldn't (and says why)
    bool MarrowRaise(Creature* marrow, Player* dead, Player* payer);
    uint32 RaiseCost(Player* dead);          // 2 silver a level
    // more players, a bigger fight (every encounter is written for one): each player beyond the first brings one more
    // creature (a copy of a group's rank and file, or a lone creature's underlings), three at most; a lone creature
    // without underlings takes that many legendary actions a round instead. Hit points rise too:
    float PartyHpScale(int players, bool legendary);   // a group 1, 1.5, 2, 2.5; a loner 1, 1.75, 2.5, 3.25
    int TunedPartySize();                    // the party every fight is tuned for (Dnd.PartySize, 2)
    void SetTunedPartySize(int players);     // (the self-test's: 0 = back to the setting)
    int PartyHelp(int players);              // a loner's legendary actions: 0, 1, 2, 3
    constexpr uint32 TALKER_FACTION = 7;     // neutral to everyone
    bool JoinFightAt(Player* p, Position const& pos, float radius);
    void EndTurn(Unit* u);
    void UpdateCombat(uint32 diff);
    void OnUnitDied(Unit* u);
    void OnPlayerLeft(Player* p);
    void OnPlayerMoved(Player* p, Position const& pos);
    void OnCreatureMoveDone(Creature* c);
    void PushEconomy(Player* p);
    void PushOrder(Encounter& e);
    void PushPositions(Encounter& e);

    // action economy helpers
    bool CanPay(Unit* u, Cost cost, std::string& why);
    void Pay(Unit* u, Cost cost);
    void SetRooted(Player* p, bool rooted);

    // ---------------------------------------------------------------- actions / spells (DndSpells.cpp)
    struct ActionInfo
    {
        uint32 spellId;
        Cost cost;
        int8 slotLevel;      // -1 none, 0 cantrip, 1+ spell slot
        bool combatOnly;
        bool outOfCombatOnly;
    };
    ActionInfo const* GetAction(uint32 spellId);
    std::vector<uint32> const& WizardKit();
    std::vector<uint32> const& FighterKit();
    std::vector<uint32> const& ClassActions();       // every class action (fighter, barbarian, rogue, monk, sorcerer)
    bool KitHas(Player* p, uint32 id);
    std::vector<uint32> const& CommonActions();
    void ResolveHit(Unit* caster, Unit* target, SpellInfo const* spell);
    void ResolveDest(Unit* caster, Position const& dest, SpellInfo const* spell);
    void ResolveCast(Unit* caster, SpellInfo const* spell);
    void WeaponAttack(Unit* attacker, Unit* target, SpellInfo const* spell, bool opportunity);
    void CreatureAttack(Creature* c, Unit* target, bool ranged, bool opportunity);
    void CreatureLegendaryAttack(Creature* c, Unit* target, bool ranged);
    // concentration (DndSpells.cpp): one such spell at a time; damage calls for a CON save (DC 10 or half the damage,
    // War Caster: advantage) or it ends; so do falling and leaving the fight
    // magic item effects (DndRules.cpp; world dnd_item_effect, from MAGIC in tools/make_treats_sql.py), summed over what
    // a player has equipped
    void LoadItemEffects();
    int ItemSaveBonus(Player* p, Ability a);
    int ItemSkillBonus(Player* p, std::string const& skill);
    bool ItemSkillAdvantage(Player* p, std::string const& skill);
    bool ItemResists(Player* p, DmgType t);
    int ItemScore(Player* p, Ability a);             // the highest score an item sets the ability to (0: none)
    float ItemSpeed(Player* p);
    int ItemInitiative(Player* p);
    int ItemTempHp(Player* p);                       // temporary HP when a fight starts
    bool ItemImmune(Player* p, std::string const& condition);
    int ItemDamageReduction(Player* p);
    int ItemHealOnKill(Player* p);
    struct ItemRider
    {
        std::string item;                            // the item's name, for the log
        Dice dice;
        DmgType type = DMG_FIRE;
        int creatureType = 0;                        // bane: only against this creature type
        std::string condition;                       // on_hit_cond: prone / frightened / poisoned / slowed
        int dc = 0;
        Ability ability = CON;
    };
    // kind: on_hit, bane, on_hit_cond (from the weapon in weaponSlot and from non-weapon gear), thorns (anything worn)
    std::vector<ItemRider> ItemRiders(Player* p, uint8 weaponSlot, std::string const& kind);
    // the bard and the druid (DndClasses.cpp)
    int InspirationMax(Player* p);           // CHA modifier (at least 1) a long rest
    int InspirationDie(Player* p);           // d6, d8 from level 5
    void Inspire(Player* bard, Player* ally);
    int HeldInspiration(Player* p);          // the die an ally holds (0: none)
    int SpendInspiration(Player* p, std::string const& on);   // rolls and gives up the held die
    struct BeastForm
    {
        uint32 spell;
        char const* name;
        uint32 display;
        int ac, hp;
        float speed;
        char const* attack;
        int bonus;
        char const* dice;
        DmgType type;
        int proneDc;                         // a hit knocks a creature prone unless it makes a STR save (0: never)
        char const* attack2;                 // a second attack in the Attack action (nullptr: one)
        char const* dice2;
        DmgType type2;
    };
    int ChannelMax(Player* p);               // Channel Divinity a short rest: 1 from level 2, 2 from 6
    int WrathMax(Player* p);                 // Wrath of the Storm a long rest (Tempest): WIS modifier, at least 1
    int HealBonus(Player* p, int spellLevel);   // Disciple of Life: + 2 + the spell's level
    BeastForm const* GetBeastForm(uint32 spell);
    BeastForm const* WildShapeOf(Player* p); // the form they are in (nullptr: themselves)
    int WildShapeMax(Player* p);
    void StartWildShape(Player* p, uint32 spell);
    void EndWildShape(Player* p, std::string const& why);
    void MoonbeamBurn(Player* owner, Unit* u);
    void BurnTurnStart(Unit* u);             // DndSpells.cpp: a creature on fire starts its turn   // DndSpells.cpp: a creature starting its turn in the light
    // a creature starting its turn in a lasting spell's area (Moonbeam, Cloud of Daggers, Stinking Cloud, Hunger of Hadar,
    // Sleet Storm); true if it loses the turn
    bool AreaTurnStart(Player* owner, Unit* u, uint32 spellId);
    bool IsConcentrationSpell(uint32 id);
    bool ConcentrationLive(Encounter const& e, Combatant const& caster);   // still holding someone (Hex: always)
    void EndConcentration(Unit* caster, std::string const& why);
    void ConcentrationCheck(Player* p, int damage, Unit* attacker);
    void PrepareCreatureAttack(Creature* c, Unit* target, bool ranged);   // roll now (the roll feed shows it)...
    void ApplyPendingAttack(Unit* attacker);                              // ...land it later
    uint32 CreatureMissile(Creature* c);                                 // the missile spell a creature's ranged attack flies as
    bool ThrowableWeapon(Player* p);                                     // the main-hand weapon can be thrown (Throw)
    bool IsCreatureMissile(uint32 spellId);
    bool HasPendingAttack(Unit* attacker);
    void LongRest(Player* p);
    void ShortRest(Player* p);
    void SetupNewCharacter(Player* p);
    void EnsureKit(Player* p);

    // levelling (DndLevels.cpp): what each level brings, the subclass / ASI choices and the level-up popup
    void OnLevelUp(Player* p, uint8 oldLevel);
    void PushPendingChoices(Player* p);
    void HandlePick(Player* p, std::string const& what);
    void ClearLevelChoices(Player* p);
    bool HasFeat(Player* p, char const* feat);                  // "Resilient" matches any of its variants
    std::string FeatOption(Player* p, char const* feat);        // the variant taken: "CON", "Fire" ("" none / not taken)
    bool WearsHeavyArmour(Player* p);
    bool IsSpellAction(uint32 spellId);      // a cantrip or a slot spell (not a weapon attack or a class action)
    bool IsSubclass(Player* p, char const* name);
    int EvocationBonus(Player* p);           // Evocation: + INT to an evocation spell's damage (0 otherwise)
    void RaiseArcaneWard(Player* p, int slotLevel);
    int AbsorbWithWard(Player* p, int damage);   // returns the damage left over

    // ---------------------------------------------------------------- classes (DndClasses.cpp)
    struct ClassDef
    {
        uint8 id;
        char const* name;
        uint8 wowClass;
        int hitDie;
        Ability save1, save2;
        Ability cast;                        // spellcasting ability (ABILITY_COUNT: not a caster)
        int subclassLevel;
        std::vector<char const*> subclasses;
        std::array<int, ABILITY_COUNT> abil; // the recommended build until the player makes their own
        std::vector<char const*> skills;     // the class's skill list (the builder picks two)
    };
    ClassDef const* ClassInfo(uint8 cls);
    ClassDef const* ClassInfo(Player* p);
    uint8 DndClassForWow(uint8 wowClass);
    bool IsClass(Player* p, uint8 cls);
    bool IsCaster(Player* p);
    int UnarmoredBase(Player* p);            // Unarmored Defense / Draconic Resilience (0: none)
    float ClassSpeedBonus(Player* p);
    int ClassHpBonus(Player* p);
    DmgType DraconicElement(Player* p);
    int RageMax(Player* p);
    int KiMax(Player* p);
    int SorceryMax(Player* p);
    int SneakDice(Player* p);
    int MartialDie(Player* p);
    int KiDC(Player* p);
    bool HasExpertise(Player* p, std::string const& skill);
    // advantage on Strength checks (Shove, Athletics): a raging barbarian; Aspect of the Bear (Bear Totem 6) always
    bool StrengthCheckAdvantage(Player* p);
    // a long log line in packet-sized pieces, cut between words, each piece closing the colour it opened and the next
    // reopening it; the addon gets the first as <kind>^ and the rest as <kind>+^ (indented under it)
    std::vector<std::string> LogChunks(std::string const& text);
    void SendLog(Player* p, std::string const& kind, std::string const& text);
    void LogFind(Player* p, std::string const& text);   // what a treat taught you: the log's Finds tab keeps it
    bool TryHide(Player* p, std::string& why);
    // sneaking out of combat (Hide outside a fight): one Stealth roll; each hostile creature that comes within its aggro
    // range rolls Perception against it once. Strike from hiding with advantage; whoever never noticed starts Surprised.
    void StartSneaking(Player* p);
    bool IsSneaking(Player* p);
    bool SneakStruck(Player* p);                 // already attacked from hiding (the fight is about to start)
    void MarkSneakStruck(Player* p);
    void ClearSneakStruck(Player* p);            // the strike started no fight (a silent kill): still unseen
    void StopSneaking(Player* p, std::string const& why = "");
    bool SpotsSneaker(Creature* c, Player* p, int adv = 0);   // rolls once per creature; true = noticed (or not sneaking)
    // a creature looking around: only what's in its sight cone (120 degrees ahead, in line of sight) can be seen; a
    // sneaker who walks up within 3 m in front of it is seen outright; anyone else in the cone gets SpotsSneaker's roll
    bool InSightCone(Creature* c, Unit* u);
    bool NoticesSneaker(Creature* c, Player* p);
    // a kill that started no fight: every creature nearby that had the sneaker or the victim in its cone looks again
    void SneakKillWitnessed(Player* p, Unit* victim);
    // after an attack from hiding has resolved (ResolveHit): a survivor starts the fight (the sneaker revealed);
    // a kill only does if a creature that saw it spots them
    void ResolveAmbush(Player* p, Unit* victim);
    // a kill made unseen (no fight): the last of an encounter's group down wins it, as a fight would (XP, cleared);
    // a creature outside any encounter pays its own XP
    void OnUnseenKill(Player* p, Creature* victim);
    bool SneakRevealed(Player* p);
    void Unhide(Unit* u);
    bool MetaArmed(Player* p, char const* which);
    void SetMeta(Player* p, char const* which, bool on);
    void TakeMeta(Player* p, uint32 spellId);
    bool CastHas(Player* p, uint32 spellId, char const* which);
    void ForgetClasses(ObjectGuid g);
    void UseCastMeta(Player* p, char const* which);
    void WildSurge(Player* p);
    bool HasMetamagic(Player* p, char const* name);      // DndLevels.cpp: the sorcerer's picks
    // the warlock: Pact Magic (a few slots, all of one level, back on a short rest), invocations and the pact boon
    int PactLevel(Player* p);
    int PactSlots(Player* p);
    bool HasInvocation(Player* p, char const* name);
    bool HasPact(Player* p, char const* name);
    int CastLevel(Player* p, int spellLevel);            // the level a spell goes off at (a warlock's: the pact level)
    int SlotToSpend(Player* p, uint32 spellId, int spellLevel);   // 0: free (invocations, a moving Hex)
    void AfterDamage(DamageCtx const& d, int amount, int absorbed);   // DndSpells.cpp: Agathys, Rebuke, patrons

    // ---------------------------------------------------------------- weather (DndWeather.cpp)
    enum WeatherKind : uint8 { WEATHER_CLEAR, WEATHER_FOG, WEATHER_FLURRIES, WEATHER_SNOWFALL, WEATHER_BLIZZARD, WEATHER_DRIZZLE,
        WEATHER_DOWNPOUR, WEATHER_THUNDERSTORM, WEATHER_GALE, WEATHER_COUNT };
    WeatherKind WeatherAt(Unit* u);                      // what's falling on u (clear indoors)
    char const* WeatherName(WeatherKind k);
    void SetWeather(uint32 mapId, uint32 zone, WeatherKind kind, bool forced);
    void UpdateWeather(uint32 diff);
    void UpdateWeatherAura(Player* p, uint32 diff);
    void ForgetWeather(ObjectGuid guid);
    void SendZoneWeatherTo(Player* p);
    int WeatherSlow(Unit* u);                            // tenths of a metre off the turn's movement
    int WeatherRangedPenalty(Unit* attacker);
    bool WeatherRangedDisadvantage(Unit* attacker, Unit* target, std::string& why);
    int WeatherDamage(Unit* victim, int amount, DmgType type, bool alreadyVulnerable, bool alreadyResisted, std::string& note);
    float WeatherJump(Unit* u);
    int WeatherPerception(Unit* u);

    // ---------------------------------------------------------------- the self-test jig (DndTest.cpp, ".dnd selftest")
    // Headless bot characters cast every action through the real spell path against test creatures, with the dice
    // rigged so every outcome can be checked exactly. Hooks below are inert unless a run is in progress.
    namespace Test
    {
        bool Running();
        int ForcedRoll(int sides);                          // a rigged die (0: roll for real)
        bool Capture(Player* p, std::string const& msg);    // a bot's addon message: kept by the jig, not sent
        bool IsBot(ObjectGuid g);                           // one of the jig's bots
        void Announced(std::string const& text);            // an announcement about a bot (the jig keeps it)
        void Start(ChatHandler* handler, std::string const& filter);
        void Update(uint32 diff);
    }
    void SetTestStats(CreatureStats const& s);           // DndRules.cpp: test stat blocks, ahead of dnd_creature
    void ClearTestStats();
    bool AddCreatureToFight(Creature* c, Unit* inFightWith);
    void AbandonFight(Player* p);
    void GoFirst(Player* p);                     // DndCombat.cpp: before the first turn, this player goes first (the jig)                // DndCombat.cpp: the fight just ends - no wipe, no grace (the spell jig)
    // the spell jig (DndJig.cpp, `.dnd jig`): every spell, class action and consumable, tested by hand on the Shimmering Flats
    bool InJig(Player* p);
    bool IsJigDummy(ObjectGuid g);
    CreatureStats const& JigDummyStats(ObjectGuid g);
    void JigRefill(Player* p);                   // the start of the player's turn in the jig: everything back
    void OpenJig(Player* p, bool teleport = true);
    void LeaveJig(Player* p, bool teleport = true);
    void JigMessage(Player* p, std::string const& msg);
    void ForgetJig(Player* p);                   // logging out: the build goes back first
    std::vector<uint32> AllActionIds();          // DndSpells.cpp: every action the rules know (sActions)   // DndCombat.cpp: one more enemy in a running fight
    void AbortEncounter(Unit* u);                        // end u's fight on the spot: no XP, nothing reset
    CreatureAI* MakeCombatantAI(Creature* c);      // dnd_loader.cpp: the encounter AI, for summoned creatures

    // ---------------------------------------------------------------- the endgame (DndEndgame.cpp)
    // a boss's home acting at the top of each round (5e lair actions; DndCombat.cpp LairRound)
    struct LairAction
    {
        std::string kind;                    // "burst": damage around someone (save for half); "grip": save or restrained
                                             // to the next round; "call": one more of the boss's people
        std::string name, text;
        Dice dice;
        DmgType type = DMG_NECROTIC;
        Ability save = DEX;
        int dc = 13;
        float radius = 3.f;
        uint32 entry = 0;                    // call: who comes
        uint8 most = 2;                      // call: at most this many at once
        std::string stop;                    // "cleared:<encounter id>" / "found:<treat flag>": off if anyone in the fight has it
    };
    void LoadEndgame();
    std::vector<LairAction> const* GetLair(uint32 encounterId);
    void SetTestLair(uint32 encounterId, std::vector<LairAction> const& actions);   // self-test (empty: none)
    bool LairStopped(LairAction const& a, std::vector<Player*> const& party);
    bool LairSave(Unit* source, Unit* target, Ability a, int dc, std::string const& what);   // DndSpells.cpp
    // endgame encounters pay items, coin and titles instead of XP (first clear, like XP)
    bool HasRewards(uint32 encounterId);
    void GiveRewards(Player* p, EncounterDef const& def);
    void SetTestReward(uint32 encounterId, uint32 item, uint32 count, uint32 money, uint32 title);   // all 0: none
    // attunement (5e): only this many magic items with effects work at once
    constexpr int ATTUNEMENT_SLOTS = 3;
    bool NeedsAttunement(uint32 itemEntry);  // DndRules.cpp: it has effects (dnd_item_effect)
    int AttunedCount(Player* p, Item const* except, uint8 exceptSlot);
    // elixirs: an item's effects worn until the end of the next fight (or an hour); one at a time
    uint32 ActiveElixir(Player* p);
    uint32 ElixirItemForSpell(uint32 spell);
    void DrinkElixir(Player* p, uint32 item);
    void EndElixir(Player* p, std::string const& why);
    void UpdateElixir(Player* p, uint32 diff);
    void ForgetElixir(ObjectGuid g);
    // a portal keeper sends the player to their idx-th destination (false: why says why not)
    bool PortalSend(Player* p, Creature* keeper, uint32 idx, std::string& why);
}

#endif
