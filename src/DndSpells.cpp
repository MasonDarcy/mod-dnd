/*
 * mod-dnd actions and spells: the BG3 common actions (Attack, Dash, Disengage, Jump, Shove, End Turn, Short Rest),
 * the level 1 wizard kit, creature attacks and rests.
 *
 * Every custom spell is a DBC clone of a stock spell (for its icon, animation and missile) whose effects were
 * replaced by SPELL_EFFECT_DUMMY / SPELL_AURA_DUMMY (tools/make_patch.py). spell_dnd_generic routes the cast
 * through this file: CheckCast enforces turns, the action economy and spell slots; AfterCast pays; each dummy
 * hit resolves the dice.
 */
#include "Dnd.h"

#include "Creature.h"
#include "GameTime.h"
#include "Item.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "StringFormat.h"
#include <cmath>

namespace Dnd
{
    static ActionInfo const sActions[] =
    {
        //  spell                  cost          slot  combatOnly  outOfCombatOnly
        { SPELL_END_TURN,         COST_NONE,    -1,   true,       false },
        { SPELL_ATTACK,           COST_ACTION,  -1,   false,      false },
        { SPELL_DASH,             COST_ACTION,  -1,   true,       false },
        { SPELL_DISENGAGE,        COST_BONUS,   -1,   true,       false },
        { SPELL_JUMP,             COST_BONUS,   -1,   false,      false },
        { SPELL_SHOVE,            COST_BONUS,   -1,   false,      false },
        { SPELL_HELP,             COST_ACTION,  -1,   false,      false },
        { SPELL_VICIOUS_MOCKERY,  COST_ACTION,   0,   false,      false },
        { SPELL_BARDIC_INSPIRATION, COST_BONUS, -1,   false,      false },
        { SPELL_HEALING_WORD,     COST_BONUS,    1,   false,      false },
        { SPELL_CURE_WOUNDS,      COST_ACTION,   1,   false,      false },
        { SPELL_THUNDERWAVE,      COST_ACTION,   1,   false,      false },
        { SPELL_HYPNOTIC_PATTERN, COST_ACTION,   3,   false,      false },
        { SPELL_PRODUCE_FLAME,    COST_ACTION,   0,   false,      false },
        { SPELL_SHILLELAGH,       COST_BONUS,    0,   true,       false },
        { SPELL_ENTANGLE,         COST_ACTION,   1,   false,      false },
        { SPELL_MOONBEAM,         COST_ACTION,   2,   false,      false },
        { SPELL_CALL_LIGHTNING,   COST_ACTION,   3,   false,      false },
        { SPELL_BARKSKIN,         COST_ACTION,   2,   false,      false },
        { SPELL_SHAPE_WOLF,       COST_ACTION,  -1,   false,      false },
        { SPELL_SHAPE_BEAR,       COST_ACTION,  -1,   false,      false },
        { SPELL_SHAPE_DIRE_WOLF,  COST_ACTION,  -1,   false,      false },
        { SPELL_SHAPE_SABRETOOTH, COST_ACTION,  -1,   false,      false },
        { SPELL_REVERT_SHAPE,     COST_BONUS,   -1,   false,      false },
        { SPELL_CHILL_TOUCH,      COST_ACTION,   0,   false,      false },
        { SPELL_ACID_SPLASH,      COST_ACTION,   0,   false,      false },
        { SPELL_POISON_SPRAY,     COST_ACTION,   0,   false,      false },
        { SPELL_RAY_OF_SICKNESS,  COST_ACTION,   1,   false,      false },
        { SPELL_WITCH_BOLT,       COST_ACTION,   1,   false,      false },
        { SPELL_SLEEP,            COST_ACTION,   1,   false,      false },
        { SPELL_HEROISM,          COST_ACTION,   1,   false,      false },
        { SPELL_CLOUD_OF_DAGGERS, COST_ACTION,   2,   false,      false },
        { SPELL_WEB,              COST_ACTION,   2,   false,      false },
        { SPELL_BLUR,             COST_ACTION,   2,   false,      false },
        { SPELL_FEAR,             COST_ACTION,   3,   false,      false },
        { SPELL_STINKING_CLOUD,   COST_ACTION,   3,   false,      false },
        { SPELL_HUNGER_OF_HADAR,  COST_ACTION,   3,   false,      false },
        { SPELL_SLEET_STORM,      COST_ACTION,   3,   false,      false },
        { SPELL_SACRED_FLAME,     COST_ACTION,   0,   false,      false },
        { SPELL_TOLL_THE_DEAD,    COST_ACTION,   0,   false,      false },
        { SPELL_BLESS,            COST_ACTION,   1,   false,      false },
        { SPELL_GUIDING_BOLT,     COST_ACTION,   1,   false,      false },
        { SPELL_INFLICT_WOUNDS,   COST_ACTION,   1,   false,      false },
        { SPELL_SHIELD_OF_FAITH,  COST_BONUS,    1,   false,      false },
        { SPELL_AID,              COST_ACTION,   2,   false,      false },
        { SPELL_LESSER_RESTORATION, COST_ACTION, 2,   false,      false },
        { SPELL_PRAYER_OF_HEALING, COST_ACTION,  2,   false,      true  },
        { SPELL_SPIRIT_GUARDIANS, COST_ACTION,   3,   false,      false },
        { SPELL_MASS_HEALING_WORD, COST_BONUS,   3,   false,      false },
        { SPELL_TURN_UNDEAD,      COST_ACTION,  -1,   true,       false },
        { SPELL_PRESERVE_LIFE,    COST_ACTION,  -1,   false,      false },
        { SPELL_DESTRUCTIVE_WRATH, COST_NONE,   -1,   false,      false },
        { SPELL_FLEE,             COST_NONE,    -1,   true,       false },
        { SPELL_SHORT_REST,       COST_NONE,    -1,   false,      true  },
        { SPELL_RANGED_ATTACK,    COST_ACTION,  -1,   false,      false },
        { SPELL_THROW,            COST_ACTION,  -1,   false,      false },
        { SPELL_OFFHAND_ATTACK,   COST_BONUS,   -1,   false,      false },
        { SPELL_FIRE_BOLT,        COST_ACTION,   0,   false,      false },
        { SPELL_RAY_OF_FROST,     COST_ACTION,   0,   false,      false },
        { SPELL_SHOCKING_GRASP,   COST_ACTION,   0,   false,      false },
        { SPELL_MAGIC_MISSILE,    COST_ACTION,   1,   false,      false },
        { SPELL_BURNING_HANDS,    COST_ACTION,   1,   false,      false },
        { SPELL_CHROMATIC_FIRE,   COST_ACTION,   1,   false,      false },
        { SPELL_CHROMATIC_COLD,   COST_ACTION,   1,   false,      false },
        { SPELL_CHROMATIC_LIGHT,  COST_ACTION,   1,   false,      false },
        { SPELL_ICE_KNIFE,        COST_ACTION,   1,   false,      false },
        { SPELL_MAGE_ARMOR,       COST_ACTION,   1,   false,      false },
        { SPELL_FALSE_LIFE,       COST_ACTION,   1,   false,      false },
        { SPELL_GREASE,           COST_ACTION,   1,   false,      false },
        { SPELL_ENHANCED_LEAP,    COST_ACTION,   1,   false,      false },
        { SPELL_SCORCHING_RAY,    COST_ACTION,   2,   false,      false },
        { SPELL_MISTY_STEP,       COST_BONUS,    2,   false,      false },
        { SPELL_SHATTER,          COST_ACTION,   2,   false,      false },
        { SPELL_HOLD_PERSON,      COST_ACTION,   2,   false,      false },
        { SPELL_FIREBALL,         COST_ACTION,   3,   false,      false },
        { SPELL_LIGHTNING_BOLT,   COST_ACTION,   3,   false,      false },
        { SPELL_SECOND_WIND,      COST_BONUS,   -1,   false,      false },
        { SPELL_ACTION_SURGE,     COST_NONE,    -1,   true,       false },
        { SPELL_TRIP_ATTACK,      COST_ACTION,  -1,   false,      false },
        { SPELL_PUSHING_ATTACK,   COST_ACTION,  -1,   false,      false },
        { SPELL_MENACING_ATTACK,  COST_ACTION,  -1,   false,      false },
        { SPELL_RAGE,             COST_BONUS,   -1,   true,       false },
        { SPELL_RECKLESS_ATTACK,  COST_NONE,    -1,   true,       false },
        { SPELL_FRENZIED_STRIKE,  COST_BONUS,   -1,   true,       false },
        { SPELL_HIDE,             COST_ACTION,  -1,   false,      false },      // a Bonus Action with Cunning Action; out of combat: sneak
        { SPELL_CUNNING_DASH,     COST_BONUS,   -1,   true,       false },
        { SPELL_CUNNING_DISENGAGE,COST_BONUS,   -1,   true,       false },
        { SPELL_UNARMED_STRIKE,   COST_BONUS,   -1,   false,      false },
        { SPELL_FLURRY,           COST_BONUS,   -1,   false,      false },
        { SPELL_PATIENT_DEFENSE,  COST_BONUS,   -1,   true,       false },
        { SPELL_STEP_OF_THE_WIND, COST_BONUS,   -1,   true,       false },
        { SPELL_STUNNING_STRIKE,  COST_ACTION,  -1,   false,      false },
        { SPELL_WHOLENESS,        COST_ACTION,  -1,   false,      false },
        { SPELL_CLOAK_OF_SHADOWS, COST_BONUS,   -1,   true,       false },
        { SPELL_SHADOW_STEP,      COST_BONUS,   -1,   false,      false },
        { SPELL_QUICKENED,        COST_NONE,    -1,   false,      false },
        { SPELL_TWINNED,          COST_NONE,    -1,   false,      false },
        { SPELL_HEIGHTENED,       COST_NONE,    -1,   false,      false },
        { SPELL_EMPOWERED,        COST_NONE,    -1,   false,      false },
        { SPELL_CREATE_SLOT,      COST_BONUS,   -1,   false,      false },
        { SPELL_CONVERT_SLOT,     COST_BONUS,   -1,   false,      false },
        { SPELL_TIDES_OF_CHAOS,   COST_NONE,    -1,   false,      false },
        { SPELL_BEND_LUCK,        COST_NONE,    -1,   false,      false },
        { SPELL_ELDRITCH_BLAST,   COST_ACTION,   0,   false,      false },
        { SPELL_HEX,              COST_BONUS,    1,   true,       false },
        { SPELL_ARMOR_OF_AGATHYS, COST_ACTION,   1,   false,      false },
        { SPELL_HELLISH_REBUKE,   COST_NONE,     1,   true,       false },      // the slot goes when it answers
        { SPELL_ARMS_OF_HADAR,    COST_ACTION,   1,   false,      false },
        { SPELL_DISSONANT_WHISPERS, COST_ACTION, 1,   false,      false },
        { SPELL_FAERIE_FIRE,      COST_ACTION,   1,   false,      false },
        { SPELL_VAMPIRIC_TOUCH,   COST_ACTION,   3,   false,      false },
        { SPELL_FEY_PRESENCE,     COST_ACTION,  -1,   false,      false },
        { SPELL_DARK_ONES_LUCK,   COST_NONE,    -1,   true,       false },
        { SPELL_ARROW_FIRE,       COST_ACTION,  -1,   false,      false },
        { SPELL_ARROW_ICE,        COST_ACTION,  -1,   false,      false },
        { SPELL_ARROW_ACID,       COST_ACTION,  -1,   false,      false },
        { SPELL_ARROW_LIGHTNING,  COST_ACTION,  -1,   false,      false },
        { SPELL_ARROW_SLAYING,    COST_ACTION,  -1,   false,      false },
        { SPELL_ALCHEMISTS_FIRE,  COST_ACTION,  -1,   false,      false },
        { SPELL_ACID_VIAL,        COST_ACTION,  -1,   false,      false },
        { SPELL_SMOKEPOWDER,      COST_ACTION,  -1,   false,      false },
        { SPELL_HOLY_WATER,       COST_ACTION,  -1,   false,      false },
        { SPELL_FROST_BOMB,       COST_ACTION,  -1,   false,      false },
    };

    std::vector<uint32> AllActionIds()
    {
        std::vector<uint32> out;
        for (ActionInfo const& a : sActions)
            out.push_back(a.spellId);
        return out;
    }

    ActionInfo const* GetAction(uint32 spellId)
    {
        for (ActionInfo const& a : sActions)
            if (a.spellId == spellId)
                return &a;
        return nullptr;
    }

    bool IsSpellAction(uint32 spellId)
    {
        ActionInfo const* a = GetAction(spellId);
        return a && a->slotLevel >= 0;
    }

    std::vector<uint32> const& CommonActions()
    {
        static std::vector<uint32> const v = { SPELL_END_TURN, SPELL_ATTACK, SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE,
            SPELL_SHORT_REST, SPELL_HELP, SPELL_FLEE, SPELL_THROW };
        return v;
    }

    std::vector<uint32> const& WizardKit()
    {
        static std::vector<uint32> const v = { SPELL_FIRE_BOLT, SPELL_RAY_OF_FROST, SPELL_SHOCKING_GRASP, SPELL_MAGIC_MISSILE,
            SPELL_BURNING_HANDS, SPELL_CHROMATIC_FIRE, SPELL_CHROMATIC_COLD, SPELL_CHROMATIC_LIGHT, SPELL_ICE_KNIFE, SPELL_MAGE_ARMOR,
            SPELL_FALSE_LIFE, SPELL_GREASE, SPELL_ENHANCED_LEAP, SPELL_SCORCHING_RAY, SPELL_MISTY_STEP, SPELL_SHATTER,
            SPELL_HOLD_PERSON, SPELL_FIREBALL, SPELL_LIGHTNING_BOLT };
        return v;
    }

    // the fighter's own actions, by the level they arrive (maneuvers need the Battle Master archetype)
    std::vector<uint32> const& FighterKit()
    {
        static std::vector<uint32> const v = { SPELL_RANGED_ATTACK, SPELL_SECOND_WIND, SPELL_ACTION_SURGE, SPELL_TRIP_ATTACK,
            SPELL_PUSHING_ATTACK, SPELL_MENACING_ATTACK };
        return v;
    }

    static bool IsManeuver(uint32 id) { return id == SPELL_TRIP_ATTACK || id == SPELL_PUSHING_ATTACK || id == SPELL_MENACING_ATTACK; }
    static bool IsArrow(uint32 id) { return id >= SPELL_ARROW_FIRE && id <= SPELL_ARROW_SLAYING; }
    static bool IsGrenade(uint32 id) { return id >= SPELL_ALCHEMISTS_FIRE && id <= SPELL_FROST_BOMB; }
    // (an arrow is a shot of the Attack action: Extra Attack can follow it)
    static bool IsAttackAction(uint32 id)
    {
        return id == SPELL_ATTACK || id == SPELL_RANGED_ATTACK || id == SPELL_THROW || IsManeuver(id) || id == SPELL_STUNNING_STRIKE ||
            IsArrow(id);
    }
    static bool HoldsBow(Player* p)
    {
        Item* r = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!r)
            return false;
        ItemTemplate const* t = r->GetTemplate();
        return t->Class == ITEM_CLASS_WEAPON && (t->SubClass == ITEM_SUBCLASS_WEAPON_BOW || t->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW);
    }

    // what an action costs this character right now: Cunning Action makes Hide a Bonus Action; Quickened Spell makes
    // an Action spell a Bonus Action
    Cost CostFor(Player* p, uint32 id, Cost base)
    {
        if (id == SPELL_HIDE && IsClass(p, CLASS_DND_ROGUE) && Level(p) >= 2)
            return COST_BONUS;
        if (GetBeastForm(id) && IsSubclass(p, "Moon"))
            return COST_BONUS;               // Combat Wild Shape
        if (base == COST_ACTION && IsSpellAction(id) && (MetaArmed(p, "quicken") || CastHas(p, id, "quicken")))
            return COST_BONUS;
        return base;
    }

    // a metamagic option's price in sorcery points
    int MetaCost(uint32 id)
    {
        switch (id)
        {
            case SPELL_QUICKENED: return 2;
            case SPELL_HEIGHTENED: return 3;
            case SPELL_BEND_LUCK: return 2;
            default: return 1;
        }
    }

    static char const* MetaKey(uint32 id)
    {
        switch (id)
        {
            case SPELL_QUICKENED: return "quicken";
            case SPELL_TWINNED: return "twin";
            case SPELL_HEIGHTENED: return "heighten";
            case SPELL_EMPOWERED: return "empower";
            default: return "bend";
        }
    }

    bool KitHas(Player* p, uint32 id);

    // ------------------------------------------------------------------ narration
    static std::string Who(Unit* u)
    {
        if (!u)
            return "?";
        return Acore::StringFormat(u->IsPlayer() ? "|cff40c0ff{}|r" : "|cffff6060{}|r", u->GetName());
    }

    // to everyone in the fight (or just the player when exploring)
    static void Narrate(Unit* a, Unit* b, std::string const& text)
    {
        Encounter* e = a ? EncounterOf(a->GetGUID()) : nullptr;
        if (!e && b)
            e = EncounterOf(b->GetGUID());
        if (e)
        {
            for (Combatant const& c : e->order)
                if (c.player && !c.gone)
                    if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                        SendLog(p, "L", text);
            FileLog(Acore::StringFormat("[E{} R{}] {}", e->id, e->round, text));
            return;
        }
        if (a && a->IsPlayer())
            Log(a->ToPlayer(), text);
        else if (b && b->IsPlayer())
            Log(b->ToPlayer(), text);
        else
            FileLog(text);
    }

    // structured rolls for the addon's roll feed, to the same people as Narrate
    static void SendRoll(Unit* a, Unit* b, std::string const& msg)
    {
        Encounter* e = a ? EncounterOf(a->GetGUID()) : nullptr;
        if (!e && b)
            e = EncounterOf(b->GetGUID());
        if (e)
        {
            for (Combatant const& c : e->order)
                if (c.player && !c.gone)
                    if (Player* p = ObjectAccessor::FindPlayer(c.guid))
                        Send(p, msg.substr(0, 250));
            return;
        }
        if (a && a->IsPlayer())
            Send(a->ToPlayer(), msg.substr(0, 250));
        else if (b && b->IsPlayer())
            Send(b->ToPlayer(), msg.substr(0, 250));
    }

    static std::string JoinRolls(std::vector<int> const& rolls)
    {
        std::string out;
        for (std::size_t i = 0; i < rolls.size() && i < 24; ++i)
            out += (i ? "," : "") + std::to_string(rolls[i]);
        return out;
    }

    static std::string Clean(std::string s)
    {
        for (char& ch : s)
            if (ch == '^')
                ch = ' ';
        return s;
    }

    // an enemy's attack, rolled at the start of its wind-up and applied when the blow lands
    struct PendingAttack
    {
        ObjectGuid target;
        bool hit = false;
        bool crit = false;
        int amount = 0;
        DmgType type = DMG_BLUDGEONING;
        bool melee = false;
    };
    static std::unordered_map<ObjectGuid, PendingAttack> sPending;

    static int CantripDice(Unit* caster)
    {
        int lvl = caster->IsPlayer() ? Level(caster->ToPlayer()) : 1;
        return lvl >= 11 ? 3 : (lvl >= 5 ? 2 : 1);
    }

    static bool HostileAdjacent(Unit* u)
    {
        Encounter* e = EncounterOf(u->GetGUID());
        if (!e)
            return false;
        for (Combatant const& c : e->order)
        {
            if (c.gone || !c.alive || c.player == u->IsPlayer() || c.prone)
                continue;
            if (Unit* o = e->GetUnit(c))
                if (o->IsAlive() && o->GetExactDist(u) <= MeleeRange(o, u))
                    return true;
        }
        return false;
    }

    static bool sPointBlank = false;         // Crossbow Expert: set while a crossbow shot is rolled
    static bool sLastCrit = false;           // the last AttackRoll was a critical hit (Great Weapon Master)

    // advantage/disadvantage from conditions: prone targets, ranged attacks in melee
    static int AttackAdv(Unit* attacker, Unit* target, bool melee, std::string& why)
    {
        int adv = 0;
        Combatant* tc = CombatantOf(target->GetGUID());
        if (tc && tc->held)
        {
            ++adv;
            why += " target held: advantage;";
        }
        if (tc && tc->prone)
        {
            if (melee || attacker->GetExactDist(target) <= MeleeRange(attacker, target))
            {
                ++adv;
                why += " target prone: advantage;";
            }
            else
            {
                --adv;
                why += " target prone at range: disadvantage;";
            }
        }
        if (!melee && WeatherRangedDisadvantage(attacker, target, why))
            --adv;
        if (!melee && HostileAdjacent(attacker))
        {
            if (sPointBlank)
                why += " Crossbow Expert: no disadvantage in melee;";
            else
            {
                --adv;
                why += " ranged attack in melee: disadvantage;";
            }
        }
        if (tc)
        {
            if (tc->hidden)
            {
                --adv;
                why += " target hidden: disadvantage;";
            }
            if (tc->dodging)
            {
                --adv;
                why += " Patient Defense: disadvantage;";
            }
            if (tc->reckless)
            {
                ++adv;
                why += " reckless target: advantage;";
            }
            if (tc->stunned)
            {
                ++adv;
                why += " target stunned: advantage;";
            }
        }
        if (Player* sp = attacker->ToPlayer())
            if (IsSneaking(sp) && !CombatantOf(sp->GetGUID()))
            {
                ++adv;
                why += " unseen: advantage;";
            }
        if (tc && tc->outlined)
        {
            ++adv;
            why += " outlined by Faerie Fire: advantage;";
        }
        if (tc && tc->restrained)
        {
            ++adv;
            why += " target restrained: advantage;";
        }
        if (tc && tc->asleep)
        {
            ++adv;
            why += " target asleep: advantage;";
        }
        if (tc && tc->guided)
        {
            ++adv;
            tc->guided = false;                  // Guiding Bolt: spent on this attack
            why += " lit by a Guiding Bolt: advantage;";
        }
        if (tc && tc->concentration == SPELL_BLUR)
        {
            --adv;
            why += " blurred: disadvantage;";
        }
        if (Combatant* fc = CombatantOf(attacker->GetGUID()); fc && fc->afraid)
        {
            --adv;
            why += " afraid: disadvantage;";
        }
        if (Combatant* mc = CombatantOf(attacker->GetGUID()))
        {
            if (mc->restrained)
            {
                --adv;
                why += " restrained: disadvantage;";
            }
            if (mc->mocked)
            {
                --adv;
                mc->mocked = false;              // Vicious Mockery: spent on this attack
                why += " mocked: disadvantage;";
            }
        }
        if (Player* ap = attacker->ToPlayer())
        {
            if (tc && !tc->hasActed && IsSubclass(ap, "Assassin"))
            {
                ++adv;
                why += " Assassinate: advantage;";
            }
            if (MetaArmed(ap, "tides"))
            {
                ++adv;                           // Tides of Chaos: spent on this roll
                why += " Tides of Chaos: advantage;";
                SetMeta(ap, "tides", false);
            }
        }
        if (Combatant* ac = CombatantOf(attacker->GetGUID()))
        {
            if (ac->hidden)
            {
                ++adv;
                why += " hidden: advantage;";
            }
            if (ac->reckless && melee)
            {
                ++adv;
                why += " Reckless Attack: advantage;";
            }
            if (ac->frightened)
            {
                --adv;
                why += " frightened: disadvantage;";
            }
            if (ac->poisoned)
            {
                --adv;
                why += " poisoned: disadvantage;";
            }
        }
        return adv > 0 ? 1 : (adv < 0 ? -1 : 0);
    }

    static int sCritMin = 20;                // Improved Critical lowers it for one weapon attack
    static bool sSneakReady = false;         // this weapon attack may carry Sneak Attack (finesse / ranged, once a turn)
    static int sLastAdv = 0;                 // the last attack roll's advantage (Sneak Attack wants it)
    static uint32 sCurSpell = 0;             // the spell whose hit is resolving (metamagic on its saves)
    static void VenomRider(Unit* attacker, Unit* target);

    // d20 attack roll + damage. Returns true on a hit.
    static bool AttackRoll(Unit* attacker, Unit* target, std::string const& what, int bonus, Dice dice, DmgType type,
        SpellInfo const* spell, bool melee, bool showMelee, int* dealt = nullptr, bool defer = false)
    {
        std::string why;
        int adv = AttackAdv(attacker, target, melee, why);
        // Entropic Ward (Great Old One 6): once a short rest a creature's attack on the warlock is at disadvantage; if it
        // misses, the warlock's next attack on it has advantage
        Combatant* ward = nullptr;
        if (Player* tp = target->ToPlayer())
            if (!attacker->IsPlayer() && IsClass(tp, CLASS_DND_WARLOCK) && Level(tp) >= 6 && IsSubclass(tp, "Great Old One") &&
                !GetSheet(tp).patron6Used)
                if (Combatant* tcw = CombatantOf(tp->GetGUID()); tcw && !tcw->reactionUsed)
                {
                    ward = tcw;
                    ward->reactionUsed = true;
                    GetSheet(tp).patron6Used = 1;
                    SaveSheet(tp);
                    adv = std::max(-1, adv - 1);
                    why += " Entropic Ward: disadvantage;";
                }
        if (Combatant* acw = CombatantOf(attacker->GetGUID()); acw && !acw->entropic.IsEmpty() && acw->entropic == target->GetGUID())
        {
            acw->entropic.Clear();
            adv = std::min(1, adv + 1);
            why += " Entropic Ward: advantage;";
        }
        if (!melee)
            if (int wp = WeatherRangedPenalty(attacker))
            {
                bonus -= wp;                     // snow and rain spoil the aim
                why += Acore::StringFormat(" {}: -{};", WeatherName(WeatherAt(attacker)), wp);
            }
        D20Result r = RollD20(bonus, adv);
        sLastAdv = adv;
        if (Player* sp = attacker->ToPlayer(); sp && IsSneaking(sp))
            MarkSneakStruck(sp);                 // the strike gives you away - the fight starts around it
        if (Combatant* hc = CombatantOf(attacker->GetGUID()); hc && hc->hidden)
            Unhide(attacker);                    // attacking gives you away
        int ac = ArmorClass(target);
        int critMin = sCritMin;
        Player* pc = attacker->ToPlayer();
        bool spellAttack = pc && spell && IsSpellAction(spell->Id);
        if (spellAttack && CastHas(pc, spell->Id, "empower"))
            dice.rerollLow = true;               // Empowered Spell: 1s and 2s rolled again
        if (spellAttack && Level(pc) >= 6 && DraconicElement(pc) == type)
            dice.bonus += AbilityMod(pc, CHA);   // Elemental Affinity
        if (spellAttack && HasFeat(pc, "Spell Sniper"))
            critMin -= 1;                    // Spell Sniper: spell attacks crit one lower
        if (spellAttack && FeatOption(pc, "Elemental Adept") == DmgTypeNames[type])
            dice.minTwo = true;
        if (pc && melee)
            if (Combatant* me = CombatantOf(pc->GetGUID()))
                me->meleeAttacked.push_back(target->GetGUID());     // Mobile: no Opportunity Attack from them this turn
        bool crit = r.natural >= critMin;
        bool hit = crit || (r.natural != 1 && r.total >= ac);
        if (pc && !hit && r.natural != 1 && HeldInspiration(pc) && r.total + HeldInspiration(pc) >= ac)
        {
            int bi = SpendInspiration(pc, "the attack");
            r.total += bi;
            r.text += Acore::StringFormat(" + {} (Bardic Inspiration) = {}", bi, r.total);
            hit = r.total >= ac;
        }
        if (hit && !crit && !attacker->IsPlayer() && target->IsPlayer())
            if (Encounter* ce = EncounterOf(target->GetGUID()))
                for (Combatant& bc : ce->order)
                {
                    // Cutting Words (College of Lore): a bard nearby spends a die and their reaction to spoil the blow
                    Player* bard = bc.player && !bc.gone && bc.alive && !bc.downed && !bc.reactionUsed ? ObjectAccessor::FindPlayer(bc.guid) : nullptr;
                    if (!bard || !IsSubclass(bard, "Lore") || Level(bard) < 3 || GetSheet(bard).inspirationUsed >= InspirationMax(bard) ||
                        bard->GetExactDist(target) > 18.f || r.total - ac >= InspirationDie(bard))
                        continue;
                    int cut = D(InspirationDie(bard));
                    ++GetSheet(bard).inspirationUsed;
                    SaveSheet(bard);
                    bc.reactionUsed = true;
                    r.total -= cut;
                    r.text += Acore::StringFormat(" - {} ({}'s Cutting Words) = {}", cut, bard->GetName(), r.total);
                    hit = r.total >= ac;
                    break;
                }
        if (ward && !hit)
            ward->entropic = attacker->GetGUID();
        if (hit && melee)
            if (Combatant* held = CombatantOf(target->GetGUID()))
                if (held->held && attacker->GetExactDist(target) <= 2.f)
                    crit = true;                 // a paralysed creature: melee hits from close by are critical
        if (hit && pc && IsSubclass(pc, "Assassin"))
            if (Combatant* tsc = CombatantOf(target->GetGUID()); tsc && tsc->surprised)
                crit = true;                     // Assassinate: a surprised foe takes a critical hit
        if (pc)
            if (Combatant* bc = CombatantOf(pc->GetGUID()); bc && !bc->blessedBy.IsEmpty())
                if (Combatant* by = CombatantOf(bc->blessedBy); by && by->concentration == SPELL_BLESS && !hit && r.natural != 1)
                {
                    int bl = D(4);
                    r.total += bl;
                    r.text += Acore::StringFormat(" + {} (Bless) = {}", bl, r.total);
                    hit = r.total >= ac;
                }
        if (hit && pc && (type == DMG_LIGHTNING || type == DMG_THUNDER))
            if (Combatant* wc = CombatantOf(pc->GetGUID()); wc && wc->wrathArmed)
            {
                dice.maximize = true;            // Destructive Wrath
                wc->wrathFired = true;
            }
        sLastCrit = hit && crit;
        std::string head = Acore::StringFormat("{} {} -> {}: {} vs AC {}{}", Who(attacker), what, Who(target), r.text, ac,
            why.empty() ? "" : " (" + why.substr(1, why.size() - 2) + ")");
        std::string detail;
        std::vector<int> rolls;
        int dmg = hit ? dice.Roll(crit, detail, &rolls) : 0;
        // Sneak Attack: a hit with advantage adds the rogue's d6s to the same blow (doubled on a crit)
        if (hit && sSneakReady && adv > 0 && pc)
        {
            sSneakReady = false;
            if (Combatant* me = CombatantOf(pc->GetGUID()))
                me->sneakUsed = true;
            std::string sd;
            std::vector<int> sr;
            int sneak = Dice{ SneakDice(pc), 6, 0 }.Roll(crit, sd, &sr);
            SendRoll(pc, target, Acore::StringFormat("RD^{}^{}^Sneak Attack^6^0^{}^{}^{}^0^0", Clean(pc->GetName()),
                Clean(target->GetName()), JoinRolls(sr), sneak, DmgTypeNames[type]));
            dmg += sneak;
            detail += Acore::StringFormat(" + |cffffd100Sneak Attack|r {}", sd);
        }
        // a rogue's Uncanny Dodge (5) halves an attack's damage; a monk's Deflect Missiles (3) takes 1d10 + DEX + level off
        // a ranged one - each their reaction
        if (hit && dmg > 0)
            if (Player* tp = target->ToPlayer())
                if (Combatant* tcr = CombatantOf(tp->GetGUID()); tcr && !tcr->reactionUsed)
                {
                    if (!melee && IsClass(tp, CLASS_MONK) && Level(tp) >= 3)
                    {
                        int cut = D(10) + AbilityMod(tp, DEX) + Level(tp);
                        dmg = std::max(0, dmg - cut);
                        tcr->reactionUsed = true;
                        detail += Acore::StringFormat(" (Deflect Missiles: -{})", cut);
                    }
                    else if (IsClass(tp, CLASS_DND_ROGUE) && Level(tp) >= 5)
                    {
                        dmg /= 2;
                        tcr->reactionUsed = true;
                        detail += " (Uncanny Dodge: halved)";
                    }
                }
        // "RA^attacker^target^what^d20^bonus^ac^outcome^damage^type^adv^attackerIsPlayer^sides^dmgBonus^r1,r2,..."
        //   outcome: 2 crit, 1 hit, 0 miss, -1 natural 1
        SendRoll(attacker, target, Acore::StringFormat("RA^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}", Clean(attacker->GetName()),
            Clean(target->GetName()), Clean(what), r.natural, bonus, ac, crit ? 2 : (hit ? 1 : (r.natural == 1 ? -1 : 0)), dmg,
            DmgTypeNames[type], adv, attacker->IsPlayer() ? 1 : 0, dice.sides, dice.bonus, JoinRolls(rolls)));
        if (defer)
        {
            Narrate(attacker, target, head + (!hit ? (r.natural == 1 ? "  |cffff4040CRITICAL MISS|r" : "  |cffaaaaaaMISS|r")
                : Acore::StringFormat("  {}  {} = |cffffffff{}|r {}", crit ? "|cffffd100CRITICAL HIT|r" : "|cff40ff40HIT|r", detail, dmg,
                    DmgTypeNames[type])));
            PendingAttack& pa = sPending[attacker->GetGUID()];
            pa = PendingAttack();
            pa.target = target->GetGUID();
            pa.hit = hit;
            pa.crit = crit;
            pa.amount = dmg;
            pa.type = type;
            pa.melee = showMelee;
            return hit;
        }
        if (!hit)
        {
            Narrate(attacker, target, head + (r.natural == 1 ? "  |cffff4040CRITICAL MISS|r" : "  |cffaaaaaaMISS|r"));
            // a creature that isn't hit visibly gets out of the way (as a player does from a creature's miss): the
            // dodge animation, and "Dodge" over its head for a blow (a spell that misses still reads "Miss")
            bool dodge = target->ToCreature() && target->IsAlive();
            if (showMelee || !spell)
                attacker->SendAttackStateUpdate(HITINFO_MISS, target, 1, SPELL_SCHOOL_MASK_NORMAL, 0, 0, 0,
                    dodge ? VICTIMSTATE_DODGE : VICTIMSTATE_INTACT, 0);
            else
                attacker->SendSpellMiss(target, spell->Id, SPELL_MISS_MISS);
            if (dodge)
                target->HandleEmoteCommand(EMOTE_ONESHOT_DODGE);
            Player* caster = attacker->ToPlayer();
            ActionInfo const* a = spell ? GetAction(spell->Id) : nullptr;
            if (caster && a && a->slotLevel == 0 && IsSubclass(caster, "Evocation") && Level(caster) >= 6)
            {
                std::string pd;
                int half = dice.Roll(false, pd) / 2;
                Narrate(attacker, target, Acore::StringFormat("   Potent Cantrip: {} halved = |cffffffff{}|r {}", pd, half, DmgTypeNames[type]));
                DamageCtx d;
                d.attacker = attacker;
                d.victim = target;
                d.amount = half;
                d.type = type;
                d.spell = spell;
                ApplyDamage(d);
            }
            return false;
        }
        Narrate(attacker, target, Acore::StringFormat("{}  {}  {} = |cffffffff{}|r {}", head, crit ? "|cffffd100CRITICAL HIT|r" : "|cff40ff40HIT|r",
            detail, dmg, DmgTypeNames[type]));
        DamageCtx d;
        d.attacker = attacker;
        d.victim = target;
        d.amount = dmg;
        d.type = type;
        d.spell = spell;
        d.crit = crit;
        d.melee = showMelee;
        int taken = ApplyDamage(d);
        if (dealt)
            *dealt = taken;
        if (pc && target->IsAlive())
            if (Combatant* hc = CombatantOf(target->GetGUID()); hc && hc->hexedBy == pc->GetGUID())
            {
                std::string hd;
                std::vector<int> hr;
                int hex = Dice{ 1, 6, 0 }.Roll(crit, hd, &hr);
                SendRoll(pc, target, Acore::StringFormat("RD^{}^{}^Hex^6^0^{}^{}^Necrotic^0^0", Clean(pc->GetName()), Clean(target->GetName()),
                    JoinRolls(hr), hex));
                Narrate(pc, target, Acore::StringFormat("   |cffa040ffHex|r: {} = |cffffffff{}|r Necrotic", hd, hex));
                DamageCtx hx;
                hx.attacker = pc;
                hx.victim = target;
                hx.amount = hex;
                hx.type = DMG_NECROTIC;
                hx.spell = spell;
                ApplyDamage(hx);
            }
        if (melee)
            VenomRider(attacker, target);
        return true;
    }

    // saving throw against a DC; returns true when the target saves
    static bool SavingThrow(Unit* caster, Unit* target, Ability a, int dc, bool magic, std::string const& what, bool poison = false,
        bool extraAdv = false)
    {
        int adv = SaveAdvantage(target, a, magic, poison) || extraAdv ? 1 : 0;
        Player* pcaster = caster ? caster->ToPlayer() : nullptr;
        if (pcaster && sCurSpell && CastHas(pcaster, sCurSpell, "heighten"))
            --adv;                               // Heightened Spell: the first save is at disadvantage
        D20Result r = RollD20(SaveBonus(target, a), adv);
        if (Combatant* lc = target->IsPlayer() ? CombatantOf(target->GetGUID()) : nullptr; lc && lc->luckArmed)
        {
            int luck = D(10);                    // Dark One's Own Luck: +1d10 on this save
            lc->luckArmed = false;
            r.total += luck;
            r.text += Acore::StringFormat(" + {} (Dark One's Own Luck) = {}", luck, r.total);
        }
        if (pcaster && sCurSpell && CastHas(pcaster, sCurSpell, "bend"))
        {
            int luck = D(4);                     // Bend Luck: -1d4 on the save
            r.total -= luck;
            r.text += Acore::StringFormat(" - {} (Bend Luck) = {}", luck, r.total);
        }
        bool ok = r.total >= dc;
        if (Player* tp = target->ToPlayer(); tp && !ok)
            if (Combatant* bc = CombatantOf(tp->GetGUID()); bc && !bc->blessedBy.IsEmpty())
                if (Combatant* by = CombatantOf(bc->blessedBy); by && by->concentration == SPELL_BLESS)
                {
                    int bl = D(4);
                    r.total += bl;
                    r.text += Acore::StringFormat(" + {} (Bless) = {}", bl, r.total);
                    ok = r.total >= dc;
                }
        if (Player* tp = target->ToPlayer(); tp && !ok && HeldInspiration(tp) && r.total + HeldInspiration(tp) >= dc)
        {
            int bi = SpendInspiration(tp, "the save");
            r.total += bi;
            r.text += Acore::StringFormat(" + {} (Bardic Inspiration) = {}", bi, r.total);
            ok = r.total >= dc;
        }
        // "RS^target^ability^what^d20^bonus^dc^saved^adv^targetIsPlayer"
        SendRoll(caster, target, Acore::StringFormat("RS^{}^{}^{}^{}^{}^{}^{}^{}^{}", Clean(target->GetName()), AbilityNames[a],
            Clean(what), r.natural, SaveBonus(target, a), dc, ok ? 1 : 0, adv, target->IsPlayer() ? 1 : 0));
        Narrate(caster, target, Acore::StringFormat("{} {} save vs {} DC {}: {}  {}", Who(target), AbilityNames[a], what, dc, r.text,
            ok ? "|cff40ff40SAVED|r" : "|cffff4040FAILED|r"));
        return ok;
    }

    bool LairSave(Unit* source, Unit* target, Ability a, int dc, std::string const& what)
    {
        return SavingThrow(source, target, a, dc, true, what);
    }

    // a venomous creature's melee hit: CON save, poison damage (half on a save) and Poisoned on a failure
    static void VenomRider(Unit* attacker, Unit* target)
    {
        Creature* c = attacker ? attacker->ToCreature() : nullptr;
        if (!c || !target || !target->IsAlive())
            return;
        CreatureStats const& s = StatsOrDefault(c);
        if (s.venomDc <= 0 || s.venom.sides <= 0)
            return;
        bool saved = SavingThrow(c, target, CON, s.venomDc, false, "Venom", true);
        std::string detail;
        std::vector<int> rolls;
        int dmg = s.venom.Roll(false, detail, &rolls);
        if (saved)
            dmg /= 2;
        SendRoll(c, target, Acore::StringFormat("RD^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}", Clean(c->GetName()), Clean(target->GetName()),
            "Venom", s.venom.sides, s.venom.bonus, JoinRolls(rolls), dmg, DmgTypeNames[DMG_POISON], saved ? 1 : 0,
            target->IsPlayer() ? 1 : 0));
        Narrate(c, target, Acore::StringFormat("   {} takes {} = |cffffffff{}|r Poison{}", Who(target), detail, dmg, saved ? " (halved)" : ""));
        DamageCtx d;
        d.attacker = c;
        d.victim = target;
        d.amount = dmg;
        d.type = DMG_POISON;
        ApplyDamage(d);
        if (saved || !target->IsAlive())
            return;
        if (Player* tp = target->ToPlayer(); tp && ItemImmune(tp, "poisoned"))
            Narrate(c, target, Acore::StringFormat("   {} shrugs the venom off (their gear): not Poisoned.", Who(target)));
        else if (Combatant* tc = CombatantOf(target->GetGUID()))
        {
            tc->poisoned = 2;
            Narrate(c, target, Acore::StringFormat("   {} is |cff40c040Poisoned|r: disadvantage on attack rolls and ability checks until the "
                "end of their next turn.", Who(target)));
        }
    }

    static bool SaveForDamage(Unit* caster, Unit* target, SpellInfo const* spell, Ability a, Dice dice, DmgType type, bool halfOnSave,
        std::string const& what)
    {
        int dc = caster->IsPlayer() ? SpellSaveDC(caster->ToPlayer()) : 12;
        if (caster->IsPlayer() && spell && (IsArrow(spell->Id) || IsGrenade(spell->Id)))
            dc = 13;                             // a grenade's (or an arrow's arc) is its own: DC 13 whoever throws it
        if (Player* pc = caster->ToPlayer())
        {
            if (spell && IsSpellAction(spell->Id) && FeatOption(pc, "Elemental Adept") == DmgTypeNames[type])
                dice.minTwo = true;
            if (spell && CastHas(pc, spell->Id, "empower"))
                dice.rerollLow = true;
            if (spell && Level(pc) >= 6 && DraconicElement(pc) == type)
                dice.bonus += AbilityMod(pc, CHA);
            if (type == DMG_LIGHTNING || type == DMG_THUNDER)
                if (Combatant* wc = CombatantOf(pc->GetGUID()); wc && wc->wrathArmed)
                {
                    dice.maximize = true;        // Destructive Wrath
                    wc->wrathFired = true;
                }
        }
        bool saved = SavingThrow(caster, target, a, dc, true, what);
        if (saved && !halfOnSave)
            return true;
        std::string detail;
        std::vector<int> rolls;
        int dmg = dice.Roll(false, detail, &rolls);
        if (saved)
            dmg /= 2;
        // "RD^caster^target^what^sides^bonus^r1,r2,...^total^type^halved^targetIsPlayer"
        SendRoll(caster, target, Acore::StringFormat("RD^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}", Clean(caster->GetName()),
            Clean(target->GetName()), Clean(what), dice.sides, dice.bonus, JoinRolls(rolls), dmg, DmgTypeNames[type],
            saved ? 1 : 0, target->IsPlayer() ? 1 : 0));
        Narrate(caster, target, Acore::StringFormat("   {} takes {} = |cffffffff{}|r {}{}", Who(target), detail, dmg, DmgTypeNames[type],
            saved ? " (halved)" : ""));
        DamageCtx d;
        d.attacker = caster;
        d.victim = target;
        d.amount = dmg;
        d.type = type;
        d.spell = spell;
        ApplyDamage(d);
        return saved;
    }

    // a shove's worth of push straight away from 'from' (Pushing Attack, Shove, Repelling Blast)
    // a shove's flight to (x, y, z); a player in a fight may be moved off their turn this way (OnPlayerMoved lets it)
    static void ShoveTo(Unit* target, float x, float y, float z)
    {
        if (Combatant* tc = CombatantOf(target->GetGUID()); tc && target->IsPlayer())
        {
            tc->ignoreMoveUntil = GameTime::GetGameTimeMS().count() + 1500;
            tc->lastPos.Relocate(x, y, z);
        }
        target->GetMotionMaster()->Clear();
        target->GetMotionMaster()->MoveJump(x, y, z, 12.f, 4.f);
    }

    static void PushAway(Unit* from, Unit* target, float dist)
    {
        float ang = from->GetAngle(target);
        float x = target->GetPositionX() + std::cos(ang) * dist;
        float y = target->GetPositionY() + std::sin(ang) * dist;
        float z = target->GetPositionZ();
        target->UpdateGroundPositionZ(x, y, z);
        ShoveTo(target, x, y, z);
    }

    // the creatures in the fight within r m of u (Arms of Hadar, Fey Presence)
    static std::vector<Unit*> FoesNear(Player* p, float r)
    {
        std::vector<Unit*> out;
        if (Encounter* e = EncounterOf(p->GetGUID()))
            for (Combatant const& c : e->order)
                if (!c.gone && c.alive && !c.player)
                    if (Unit* u = e->GetUnit(c))
                        if (u->IsAlive() && u->GetExactDist(p) <= r + u->GetCombatReach())
                            out.push_back(u);
        return out;
    }

    static void KnockProne(Unit* source, Unit* target)
    {
        Combatant* c = CombatantOf(target->GetGUID());
        if (!c || c->prone)
            return;
        if (Player* tp = target->ToPlayer(); tp && ItemImmune(tp, "prone"))
        {
            Narrate(source, target, Acore::StringFormat("{} keeps their footing (their gear).", Who(target)));
            return;
        }
        c->prone = true;
        // feet go out from under them: a stagger, then down (the combat tick keeps them down, see KeepProneDown)
        target->HandleEmoteCommand(EMOTE_ONESHOT_WOUND_CRITICAL);
        target->SetStandState(UNIT_STAND_STATE_SLEEP);
        Narrate(source, target, Acore::StringFormat("{} is knocked |cffff8000Prone|r.", Who(target)));
        if (Encounter* e = EncounterOf(target->GetGUID()))
            PushOrder(*e);
    }

    // ------------------------------------------------------------------ weapons
    struct WeaponInfo { Dice dice; DmgType type; Ability ability; bool proficient; std::string name; bool twoHanded = false;
        bool offhandWeapon = false; bool unarmed = false; bool thrown = false; bool crossbow = false; bool finesse = false;
        bool monk = false; };

    // Sneak Attack is possible on this attack: a rogue who hasn't used it this turn (the hit still needs advantage)
    static bool SneakReady(Player* p)
    {
        Combatant* me = CombatantOf(p->GetGUID());
        return SneakDice(p) > 0 && !(me && me->sneakUsed);
    }
    static bool sForceUnarmed = false;       // Martial Arts' and Flurry of Blows' strikes are fists, whatever's in hand

    // a magic weapon's +N (attack and damage): marked on the item as hit rating N (tools/make_treats_sql.py MAGIC)
    static int MagicOf(ItemTemplate const* t)
    {
        for (uint32 i = 0; t && i < t->StatsCount && i < MAX_ITEM_PROTO_STATS; ++i)
            if (t->ItemStat[i].ItemStatType == ITEM_MOD_HIT_RATING)
                return t->ItemStat[i].ItemStatValue;
        return 0;
    }

    int MagicBonus(Player* p, uint8 slot)
    {
        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        return item && item->GetTemplate()->Class == ITEM_CLASS_WEAPON ? MagicOf(item->GetTemplate()) : 0;
    }

    // proficiency by class: fighters and barbarians everything; rogues and monks what isn't big and two-handed (and no
    // guns); wizards and sorcerers daggers, staves, light crossbows and thrown
    static bool ClassProficient(uint8 cls, uint32 sub)
    {
        switch (cls)
        {
            case CLASS_FIGHTER:
            case CLASS_BARBARIAN:
                return true;
            case CLASS_DND_ROGUE:
            case CLASS_MONK:
                return sub != ITEM_SUBCLASS_WEAPON_AXE2 && sub != ITEM_SUBCLASS_WEAPON_MACE2 && sub != ITEM_SUBCLASS_WEAPON_SWORD2 &&
                    sub != ITEM_SUBCLASS_WEAPON_POLEARM && sub != ITEM_SUBCLASS_WEAPON_GUN;
            case CLASS_CLERIC:
            case CLASS_DND_WARLOCK:          // simple weapons
                return sub == ITEM_SUBCLASS_WEAPON_STAFF || sub == ITEM_SUBCLASS_WEAPON_DAGGER || sub == ITEM_SUBCLASS_WEAPON_MACE ||
                    sub == ITEM_SUBCLASS_WEAPON_SPEAR || sub == ITEM_SUBCLASS_WEAPON_CROSSBOW || sub == ITEM_SUBCLASS_WEAPON_THROWN;
            default:
                return sub == ITEM_SUBCLASS_WEAPON_STAFF || sub == ITEM_SUBCLASS_WEAPON_DAGGER ||
                    sub == ITEM_SUBCLASS_WEAPON_CROSSBOW || sub == ITEM_SUBCLASS_WEAPON_THROWN;
        }
    }

    // a monk: unarmed strikes and monk weapons use the better of STR and DEX, and the Martial Arts die if it's bigger
    static void MartialArts(Player* p, WeaponInfo& w, uint32 sub)
    {
        if (!IsClass(p, CLASS_MONK))
            return;
        bool monkWeapon = w.unarmed || ((sub == ITEM_SUBCLASS_WEAPON_DAGGER || sub == ITEM_SUBCLASS_WEAPON_STAFF ||
            sub == ITEM_SUBCLASS_WEAPON_MACE || sub == ITEM_SUBCLASS_WEAPON_AXE || sub == ITEM_SUBCLASS_WEAPON_SPEAR ||
            sub == ITEM_SUBCLASS_WEAPON_SWORD || sub == ITEM_SUBCLASS_WEAPON_FIST));
        if (!monkWeapon)
            return;
        w.monk = true;
        w.proficient = true;
        if (AbilityMod(p, DEX) > AbilityMod(p, STR))
            w.ability = DEX;
        int die = MartialDie(p);
        if (w.unarmed)
            w.dice = Dice{ 1, die, 0 };
        else if (w.dice.count == 1 && w.dice.sides < die)
            w.dice.sides = die;
    }

    static WeaponInfo PlayerWeapon(Player* p, uint8 slot = EQUIPMENT_SLOT_MAINHAND)
    {
        WeaponInfo w{ Dice::Parse("1"), DMG_BLUDGEONING, STR, true, "Unarmed Strike" };
        w.unarmed = true;
        Item* item = sForceUnarmed ? nullptr : p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            MartialArts(p, w, 0);
            return w;
        }
        w.unarmed = false;
        ItemTemplate const* t = item->GetTemplate();
        // weapon dice are written into item_template as dmg_min = dice count, dmg_max = count * sides (1d6 -> 1-6)
        int count = std::max(1, int(t->Damage[0].DamageMin));
        int sides = std::max(1, int(t->Damage[0].DamageMax) / count);
        w.dice.count = count;
        w.dice.sides = sides;
        w.dice.bonus = 0;
        w.name = t->Name1;
        w.twoHanded = t->InventoryType == INVTYPE_2HWEAPON;
        if (Item* off = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND))
            w.offhandWeapon = off->GetTemplate()->Class == ITEM_CLASS_WEAPON;
        switch (t->SubClass)
        {
            case ITEM_SUBCLASS_WEAPON_DAGGER:   w.type = DMG_PIERCING; w.ability = DEX; w.finesse = true; break;
            case ITEM_SUBCLASS_WEAPON_SWORD:    w.type = DMG_SLASHING; w.finesse = true; break;   // rapiers, scimitars
            case ITEM_SUBCLASS_WEAPON_SWORD2:
            case ITEM_SUBCLASS_WEAPON_AXE:
            case ITEM_SUBCLASS_WEAPON_AXE2:
            case ITEM_SUBCLASS_WEAPON_POLEARM:  w.type = DMG_SLASHING; break;
            case ITEM_SUBCLASS_WEAPON_SPEAR:    w.type = DMG_PIERCING; break;
            default:                            w.type = DMG_BLUDGEONING; break;
        }
        w.proficient = ClassProficient(GetSheet(p).cls, t->SubClass);
        if (p->getRace() == RACE_DWARF && (t->SubClass == ITEM_SUBCLASS_WEAPON_AXE || t->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
            t->SubClass == ITEM_SUBCLASS_WEAPON_MACE || t->SubClass == ITEM_SUBCLASS_WEAPON_MACE2))
            w.proficient = true;             // Dwarven Combat Training: battleaxes, handaxes, hammers
        if (HasFeat(p, "Weapon Master"))
            w.proficient = true;
        if (w.ability == DEX && AbilityMod(p, STR) > AbilityMod(p, DEX))
            w.ability = STR;
        if (w.finesse && AbilityMod(p, DEX) > AbilityMod(p, STR))
            w.ability = DEX;                 // a finesse weapon goes by the better of the two
        MartialArts(p, w, t->SubClass);
        if (IsSubclass(p, "Valor") || IsSubclass(p, "Tempest"))
            w.proficient = true;             // College of Valor, Tempest Domain: martial weapons
        if (Combatant* sc = CombatantOf(p->GetGUID()); sc && sc->shillelagh &&
            (t->SubClass == ITEM_SUBCLASS_WEAPON_STAFF || t->SubClass == ITEM_SUBCLASS_WEAPON_MACE))
        {
            w.proficient = true;             // Shillelagh: the druid's magic in the wood
            if (AbilityMod(p, WIS) > AbilityMod(p, w.ability))
                w.ability = WIS;
            if (w.dice.count * w.dice.sides < 8)
                w.dice = Dice{ 1, 8, 0 };
        }
        if (HasPact(p, "Blade"))
        {
            w.proficient = true;             // Pact of the Blade: your pact weapon, and Charisma behind it
            if (AbilityMod(p, CHA) > AbilityMod(p, w.ability))
                w.ability = CHA;
        }
        return w;
    }

    // the weapon in the ranged slot: bows, crossbows and guns shoot (DEX, piercing); thrown weapons use the better of
    // STR / DEX. Returns false when there's none.
    static bool PlayerRangedWeapon(Player* p, WeaponInfo& w)
    {
        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
        if (!item || item->GetTemplate()->Class != ITEM_CLASS_WEAPON)
            return false;
        ItemTemplate const* t = item->GetTemplate();
        if (t->SubClass == ITEM_SUBCLASS_WEAPON_WAND)
            return false;
        int count = std::max(1, int(t->Damage[0].DamageMin));
        w.dice = Dice{ count, std::max(1, int(t->Damage[0].DamageMax) / count), 0 };
        w.type = DMG_PIERCING;
        w.ability = DEX;
        w.name = t->Name1;
        bool thrown = t->SubClass == ITEM_SUBCLASS_WEAPON_THROWN;
        if (thrown && AbilityMod(p, STR) > AbilityMod(p, DEX))
            w.ability = STR;
        w.proficient = ClassProficient(GetSheet(p).cls, t->SubClass) || HasFeat(p, "Weapon Master");
        w.thrown = thrown;
        w.crossbow = t->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW;
        return true;
    }

    static void ApplyItemRiders(Player* p, Unit* target, uint8 slot);

    static bool PlayerRangedAttack(Player* p, Unit* target, SpellInfo const* spell)
    {
        WeaponInfo w{ Dice::Parse("1"), DMG_PIERCING, DEX, false, "" };
        if (!PlayerRangedWeapon(p, w))
            return false;
        int mod = AbilityMod(p, w.ability);
        int bonus = mod + (w.proficient ? Prof(Level(p)) : 0);
        if (HasStyle(p, "Archery"))
            bonus += 2;                      // Fighting Style: Archery
        w.dice.bonus = mod;
        if (int magic = MagicBonus(p, EQUIPMENT_SLOT_RANGED))
        {
            bonus += magic;
            w.dice.bonus += magic;
        }
        if (w.thrown && HasFeat(p, "Tavern Brawler"))
        {
            int str = AbilityMod(p, STR);    // Tavern Brawler: Strength again on thrown attacks and damage
            bonus += str;
            w.dice.bonus += str;
        }
        if (IsFighter(p) && Level(p) >= 3 && IsSubclass(p, "Champion"))
            sCritMin = 19;                   // Improved Critical
        p->SetFacingToObject(target);
        sPointBlank = w.crossbow && HasFeat(p, "Crossbow Expert");
        sSneakReady = SneakReady(p);
        bool hit = AttackRoll(p, target, w.name, bonus, w.dice, w.type, spell, false, false);
        sSneakReady = false;
        sPointBlank = false;
        if (hit)
            ApplyItemRiders(p, target, EQUIPMENT_SLOT_RANGED);
        sCritMin = 20;
        return hit;
    }

    // a main-hand weapon with 5e's "thrown" property: any dagger or thrown weapon; otherwise by name, since WoW's types
    // are too coarse (a Battleaxe is a one-handed axe like the Handaxe, but only the Handaxe flies)
    bool ThrowableWeapon(Player* p)
    {
        Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!item || item->GetTemplate()->Class != ITEM_CLASS_WEAPON)
            return false;
        ItemTemplate const* t = item->GetTemplate();
        if (t->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER || t->SubClass == ITEM_SUBCLASS_WEAPON_THROWN)
            return true;
        for (char const* name : { "Handaxe", "Hand Axe", "Hatchet", "Javelin", "Spear", "Light Hammer", "Throwing" })
            if (t->Name1.find(name) != std::string::npos)
                return true;
        return false;
    }

    // Throw: the main-hand weapon at range - its own dice and damage, the ability it swings with (a dagger the better of
    // STR and DEX), proficiency and its magic; Tavern Brawler adds Strength again. A ranged attack: disadvantage with an
    // enemy beside you (AttackRoll). It comes back to hand - nothing to pick up.
    static bool PlayerThrowAttack(Player* p, Unit* target, SpellInfo const* spell)
    {
        if (!ThrowableWeapon(p))
            return false;
        WeaponInfo w = PlayerWeapon(p, EQUIPMENT_SLOT_MAINHAND);
        w.thrown = true;
        int mod = AbilityMod(p, w.ability);
        int bonus = mod + (w.proficient ? Prof(Level(p)) : 0);
        w.dice.bonus = mod;
        if (int magic = MagicBonus(p, EQUIPMENT_SLOT_MAINHAND))
        {
            bonus += magic;
            w.dice.bonus += magic;
        }
        if (HasFeat(p, "Tavern Brawler"))
        {
            int str = AbilityMod(p, STR);    // Tavern Brawler: Strength again on thrown attacks and damage
            bonus += str;
            w.dice.bonus += str;
        }
        if (IsFighter(p) && Level(p) >= 3 && IsSubclass(p, "Champion"))
            sCritMin = 19;                   // Improved Critical
        p->SetFacingToObject(target);
        sSneakReady = SneakReady(p);
        bool hit = AttackRoll(p, target, w.name + " (thrown)", bonus, w.dice, w.type, spell, false, false);
        sSneakReady = false;
        if (hit)
            ApplyItemRiders(p, target, EQUIPMENT_SLOT_MAINHAND);
        sCritMin = 20;
        return hit;
    }

    // ---------------------------------------------------------------- arrows and grenades
    static void SetBurning(Player* p, Unit* target)
    {
        if (Combatant* tc = CombatantOf(target->GetGUID()))
        {
            tc->burningBy = p->GetGUID();
            Narrate(p, target, Acore::StringFormat("   {} |cffff8040catches fire|r: 1d4 Fire at the start of each of its turns until it "
                "beats the flames out (DC 10 Dexterity save).", Who(target)));
        }
    }

    static void Corrode(Player* p, Unit* target)
    {
        if (Combatant* tc = CombatantOf(target->GetGUID()))
        {
            tc->corroded = 2;
            Narrate(p, target, Acore::StringFormat("   {}'s armour |cff80ff40hisses and pits|r: -2 Armour Class for two rounds.", Who(target)));
        }
    }

    void BurnTurnStart(Unit* u)
    {
        Combatant* c = CombatantOf(u->GetGUID());
        if (!c || c->burningBy.IsEmpty())
            return;
        Player* by = ObjectAccessor::FindPlayer(c->burningBy);
        if (!by)
        {
            c->burningBy.Clear();                // (whoever lit it is gone: the fire goes out with them)
            return;
        }
        std::string detail;
        DamageCtx d;
        d.attacker = by;
        d.victim = u;
        d.amount = Dice{ 1, 4, 0 }.Roll(false, detail);
        d.type = DMG_FIRE;
        Narrate(by, u, Acore::StringFormat("   {} burns: {} = {} Fire", Who(u), detail, d.amount));
        ApplyDamage(d);
        if (!u->IsAlive())
            return;
        if (SavingThrow(by, u, DEX, 10, false, "beating out the flames"))
        {
            if (Combatant* again = CombatantOf(u->GetGUID()))
                again->burningBy.Clear();
            Narrate(by, u, Acore::StringFormat("   {} beats the flames out.", Who(u)));
        }
    }

    static bool PlayerRangedAttack(Player* p, Unit* target, SpellInfo const* spell);

    // a special arrow: a shot with the bow or crossbow, and on a hit the arrow's own dice (doubled on a critical) and effect
    static void ArrowShot(Player* p, Unit* target, SpellInfo const* spell)
    {
        uint32 id = spell->Id;
        if (!PlayerRangedAttack(p, target, spell) || !target->IsAlive())
            return;
        bool crit = sLastCrit;
        std::string name = spell->SpellName[0];
        Dice dice{ 1, 6, 0 };
        DmgType type = DMG_FIRE;
        switch (id)
        {
            case SPELL_ARROW_ICE:       type = DMG_COLD; break;
            case SPELL_ARROW_ACID:      type = DMG_ACID; break;
            case SPELL_ARROW_LIGHTNING: type = DMG_LIGHTNING; break;
            case SPELL_ARROW_SLAYING:
            {
                Creature* tc = target->ToCreature();
                if (!tc || tc->GetCreatureType() == CREATURE_TYPE_HUMANOID)
                {
                    Narrate(p, target, Acore::StringFormat("   The slaying runes stay dark: {} is no monster.", Who(target)));
                    return;
                }
                dice = Dice{ 2, 8, 0 };
                type = DMG_PIERCING;
                break;
            }
            default: break;
        }
        std::string detail;
        DamageCtx d;
        d.attacker = p;
        d.victim = target;
        d.amount = dice.Roll(crit, detail);
        d.type = type;
        d.spell = spell;
        Narrate(p, target, Acore::StringFormat("   {}: +{} = {} {}", name, detail, d.amount, DmgTypeNames[type]));
        ApplyDamage(d);
        if (id == SPELL_ARROW_LIGHTNING)
        {
            // the bolt leaps to the nearest other foe within 3 m of where it struck (even if that one fell)
            Unit* next = nullptr;
            if (Encounter* e = EncounterOf(p->GetGUID()))
                for (Combatant const& c : e->order)
                    if (!c.gone && c.alive && !c.player)
                        if (Unit* u = e->GetUnit(c))
                            if (u != target && u->IsAlive() && u->GetExactDist(target) <= 3.f + u->GetCombatReach() &&
                                (!next || u->GetExactDist(target) < next->GetExactDist(target)))
                                next = u;
            if (next)
            {
                Narrate(p, next, Acore::StringFormat("   The lightning leaps to {}.", Who(next)));
                SaveForDamage(p, next, spell, DEX, Dice{ 1, 6, 0 }, DMG_LIGHTNING, true, name + " (arc)");
            }
        }
        if (!target->IsAlive())
            return;
        if (id == SPELL_ARROW_FIRE)
            SetBurning(p, target);
        else if (id == SPELL_ARROW_ACID)
            Corrode(p, target);
        else if (id == SPELL_ARROW_ICE)
            if (Combatant* tc = CombatantOf(target->GetGUID()))
            {
                tc->slowed = true;
                Narrate(p, target, Acore::StringFormat("   Frost creeps over {}: slowed, -3 m on its next turn.", Who(target)));
            }
        if (Encounter* e = EncounterOf(target->GetGUID()))
            PushOrder(*e);
    }

    // a grenade: each creature in the burst (the spell's area finds them) makes a DC 13 Dexterity save
    static void Grenade(Player* p, Unit* target, SpellInfo const* spell)
    {
        switch (spell->Id)
        {
            case SPELL_ALCHEMISTS_FIRE:
                if (!SaveForDamage(p, target, spell, DEX, Dice{ 2, 4, 0 }, DMG_FIRE, true, "Alchemist's Fire") && target->IsAlive())
                    SetBurning(p, target);
                break;
            case SPELL_ACID_VIAL:
                if (!SaveForDamage(p, target, spell, DEX, Dice{ 2, 6, 0 }, DMG_ACID, true, "Acid Vial") && target->IsAlive())
                    Corrode(p, target);
                break;
            case SPELL_SMOKEPOWDER:
                SaveForDamage(p, target, spell, DEX, Dice{ 3, 6, 0 }, DMG_FIRE, true, "Smokepowder Bomb");
                break;
            case SPELL_HOLY_WATER:
            {
                Creature* tc = target->ToCreature();
                if (!tc || (tc->GetCreatureType() != CREATURE_TYPE_UNDEAD && tc->GetCreatureType() != CREATURE_TYPE_DEMON))
                {
                    Narrate(p, target, Acore::StringFormat("   The holy water splashes {} harmlessly.", Who(target)));
                    break;
                }
                SaveForDamage(p, target, spell, DEX, Dice{ 2, 6, 0 }, DMG_RADIANT, true, "Holy Water");
                break;
            }
            case SPELL_FROST_BOMB:
                if (!SaveForDamage(p, target, spell, DEX, Dice{ 2, 4, 0 }, DMG_COLD, true, "Frost Bomb") && target->IsAlive())
                    KnockProne(p, target);
                break;
            default:
                break;
        }
        if (Encounter* e = EncounterOf(target->GetGUID()))
            PushOrder(*e);
    }

    // magic gear on a weapon hit: extra dice (bane ones only against their creature type), a condition with a save, and a
    // little healing on a kill
    static void ApplyItemRiders(Player* p, Unit* target, uint8 slot)
    {
        bool crit = sLastCrit;
        std::vector<ItemRider> dmg = ItemRiders(p, slot, "on_hit");
        for (ItemRider const& r : ItemRiders(p, slot, "bane"))
            if (Creature* tc = target->ToCreature(); tc && int(tc->GetCreatureType()) == r.creatureType)
                dmg.push_back(r);
        for (ItemRider const& r : dmg)
        {
            if (!target->IsAlive())
                break;
            std::string detail;
            DamageCtx d;
            d.attacker = p;
            d.victim = target;
            d.amount = r.dice.Roll(crit, detail);
            d.type = r.type;
            Narrate(p, target, Acore::StringFormat("   {}: +{} = {} {}", r.item, detail, d.amount, DmgTypeNames[r.type]));
            ApplyDamage(d);
        }
        if (target->IsAlive())
            for (ItemRider const& r : ItemRiders(p, slot, "on_hit_cond"))
            {
                if (!target->IsAlive() || SavingThrow(p, target, r.ability, r.dc, true, r.item))
                    continue;
                Combatant* tc = CombatantOf(target->GetGUID());
                if (r.condition == "prone")
                    KnockProne(p, target);
                else if (tc && r.condition == "frightened")
                {
                    tc->frightened = true;
                    tc->frightenedBy = p->GetGUID();
                    Narrate(p, target, Acore::StringFormat("   {} is |cffc080ffFrightened|r until your next turn.", Who(target)));
                }
                else if (tc && r.condition == "poisoned")
                {
                    tc->poisoned = 2;
                    Narrate(p, target, Acore::StringFormat("   {} is |cff40c040Poisoned|r.", Who(target)));
                }
                else if (tc && r.condition == "slowed")
                {
                    tc->slowed = true;
                    Narrate(p, target, Acore::StringFormat("   {} is slowed: -3 m on its next turn.", Who(target)));
                }
                if (Encounter* e = EncounterOf(target->GetGUID()))
                    PushOrder(*e);
            }
        if (!target->IsAlive())
            if (int h = ItemHealOnKill(p))
            {
                ApplyHeal(p, p, h, nullptr);
                Narrate(p, nullptr, Acore::StringFormat("   {} drinks in the kill: |cff80ff80{}|r hit points.", Who(p), h));
            }
    }

    // the weapon in the off hand, if it's a weapon that can strike (not a shield or a held-in-off-hand item)
    static bool HasOffhandWeapon(Player* p)
    {
        Item* off = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        Item* main = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        return off && main && off->GetTemplate()->Class == ITEM_CLASS_WEAPON && main->GetTemplate()->InventoryType != INVTYPE_2HWEAPON;
    }

    // two-weapon fighting: a Bonus Action strike with the off-hand weapon after the Attack action. No ability modifier
    // on its damage (unless it's negative) without the Two-Weapon Fighting style; it can carry Sneak Attack.
    static bool PlayerOffhandAttack(Player* p, Unit* target, SpellInfo const* spell)
    {
        WeaponInfo w = PlayerWeapon(p, EQUIPMENT_SLOT_OFFHAND);
        int mod = AbilityMod(p, w.ability);
        int bonus = mod + (w.proficient ? Prof(Level(p)) : 0);
        w.dice.bonus = (mod < 0 || HasStyle(p, "Two-Weapon Fighting")) ? mod : 0;
        if (int magic = MagicBonus(p, EQUIPMENT_SLOT_OFFHAND))
        {
            bonus += magic;
            w.dice.bonus += magic;
        }
        Combatant* me = CombatantOf(p->GetGUID());
        if (me && me->raging && w.ability == STR)
            w.dice.bonus += 2;               // Rage
        p->SetFacingToObject(target);
        sSneakReady = w.finesse && SneakReady(p);
        bool hit = AttackRoll(p, target, "Off-Hand Attack (" + w.name + ")", bonus, w.dice, w.type, spell, true, false);
        sSneakReady = false;
        if (hit)
            ApplyItemRiders(p, target, EQUIPMENT_SLOT_OFFHAND);
        return hit;
    }

    // a player's swing with the main-hand weapon; returns true on a hit
    static bool PlayerWeaponAttack(Player* p, Unit* target, SpellInfo const* spell, bool opportunity, std::string const& label)
    {
        if (BeastForm const* bf = WildShapeOf(p))
        {
            // Wild Shape: the beast's own attack (its second, the bite, for the Attack action's follow-up)
            Combatant* bme = CombatantOf(p->GetGUID());
            bool second = bf->attack2 && bme && bme->actionUsed && bme->extraAttacks > 0;
            p->SetFacingToObject(target);
            bool hit = AttackRoll(p, target, opportunity ? Acore::StringFormat("Opportunity Attack ({})", bf->attack) :
                std::string(second ? bf->attack2 : bf->attack), bf->bonus, Dice::Parse(second ? bf->dice2 : bf->dice), second ? bf->type2 : bf->type,
                spell, true, spell == nullptr);
            if (hit && bf->proneDc && !second && target->IsAlive() && !SavingThrow(p, target, STR, bf->proneDc, false, bf->name))
                KnockProne(p, target);
            return hit;
        }
        WeaponInfo w = PlayerWeapon(p);
        int mod = AbilityMod(p, w.ability);
        int bonus = mod + (w.proficient ? Prof(Level(p)) : 0);
        w.dice.bonus = mod + (w.unarmed && !w.monk ? 1 : 0);  // an unarmed strike: 1 + Strength (a monk: the die)
        if (w.unarmed && HasFeat(p, "Tavern Brawler"))
        {
            int str = AbilityMod(p, STR);    // Tavern Brawler: Strength again on unarmed attacks and damage
            bonus += str;
            w.dice.bonus += str;
        }
        if (!w.unarmed && HasFeat(p, "Savage Attacker"))
            w.dice.rollTwice = true;
        Combatant* me = CombatantOf(p->GetGUID());
        if (me && me->raging && w.ability == STR)
            w.dice.bonus += 2;               // Rage
        if (!w.unarmed)
            if (int magic = MagicBonus(p, EQUIPMENT_SLOT_MAINHAND))
            {
                bonus += magic;              // a +1 weapon: +1 to hit and to damage
                w.dice.bonus += magic;
            }
        if (me && w.monk && spell && (spell->Id == SPELL_ATTACK || spell->Id == SPELL_STUNNING_STRIKE))
            me->attackedUnarmed = true;      // Martial Arts: a bonus strike follows
        if (me && spell && spell->Id == SPELL_ATTACK && !w.unarmed && !w.twoHanded)
            me->attackedLight = true;        // two-weapon fighting: the off-hand may follow as a Bonus Action
        if (w.name != "Unarmed Strike")
        {
            if (!w.twoHanded && !w.offhandWeapon && HasStyle(p, "Dueling"))
                w.dice.bonus += 2;           // Fighting Style: Dueling
            if (w.twoHanded && HasStyle(p, "Great Weapon Fighting"))
                w.dice.rerollLow = true;     // Fighting Style: Great Weapon Fighting
        }
        if (IsFighter(p) && Level(p) >= 3 && IsSubclass(p, "Champion"))
            sCritMin = 19;                   // Improved Critical
        p->SetFacingToObject(target);
        std::string what = opportunity ? "Opportunity Attack (" + w.name + ")" : (label.empty() ? w.name : label + " (" + w.name + ")");
        sSneakReady = !opportunity && w.finesse && SneakReady(p);
        bool hit = AttackRoll(p, target, what, bonus, w.dice, w.type, spell, true, spell == nullptr);
        sSneakReady = false;
        if (hit)
            ApplyItemRiders(p, target, EQUIPMENT_SLOT_MAINHAND);
        sCritMin = 20;
        // Great Weapon Master: a critical hit or a kill with a melee weapon earns one more attack as a Bonus Action
        if (hit && !opportunity && !w.unarmed && (sLastCrit || !target->IsAlive()) && HasFeat(p, "Great Weapon Master"))
            if (Combatant* me = CombatantOf(p->GetGUID()))
                if (!me->gwmReady && !me->bonusUsed)
                {
                    me->gwmReady = true;
                    Log(p, "|cffffd100Great Weapon Master:|r attack once more as a Bonus Action.");
                    PushEconomy(p);
                }
        return hit;
    }

    void WeaponAttack(Unit* attacker, Unit* target, SpellInfo const* spell, bool opportunity)
    {
        if (!attacker || !target || !target->IsAlive())
            return;
        if (Creature* c = attacker->ToCreature())
        {
            CreatureAttack(c, target, false, opportunity);
            return;
        }
        PlayerWeaponAttack(attacker->ToPlayer(), target, spell, opportunity, "");
    }

    // the missile a ranged attack flies as: Spark Bolt is lightning, a frostbolt is frost, a gun fires a shot,
    // and anything else is a thrown rock
    uint32 CreatureMissile(Creature* c)
    {
        if (!c)
            return SPELL_CREATURE_THROW;
        switch (StatsOrDefault(c).ranged.type)
        {
            case DMG_LIGHTNING: case DMG_THUNDER: return SPELL_CREATURE_SPARK;
            case DMG_COLD:                        return SPELL_CREATURE_FROST;
            case DMG_FIRE:                        return SPELL_CREATURE_FIRE;
            default: break;
        }
        if (uint32 ranged = c->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 2))
        {
            ItemTemplate const* t = sObjectMgr->GetItemTemplate(ranged);
            return t && t->Class == ITEM_CLASS_WEAPON && t->SubClass == ITEM_SUBCLASS_WEAPON_GUN ? SPELL_CREATURE_GUN
                                                                                                   : SPELL_CREATURE_SHOOT;
        }
        // nothing in the ranged slot: what flies is what the attack does - a spear, a hand axe, or a rock
        switch (StatsOrDefault(c).ranged.type)
        {
            case DMG_PIERCING: return SPELL_CREATURE_SPEAR;
            case DMG_SLASHING: return SPELL_CREATURE_AXE;
            default:           return SPELL_CREATURE_THROW;
        }
    }

    bool IsCreatureMissile(uint32 id) { return id >= SPELL_CREATURE_THROW && id <= SPELL_CREATURE_GUN; }

    // ------------------------------------------------------------------ concentration
    bool IsConcentrationSpell(uint32 id)
    {
        return id == SPELL_HOLD_PERSON || id == SPELL_HEX || id == SPELL_FAERIE_FIRE || id == SPELL_HYPNOTIC_PATTERN ||
            id == SPELL_ENTANGLE || id == SPELL_MOONBEAM || id == SPELL_CALL_LIGHTNING || id == SPELL_BARKSKIN ||
            id == SPELL_WITCH_BOLT || id == SPELL_HEROISM || id == SPELL_CLOUD_OF_DAGGERS || id == SPELL_WEB || id == SPELL_BLUR ||
            id == SPELL_FEAR || id == SPELL_STINKING_CLOUD || id == SPELL_HUNGER_OF_HADAR || id == SPELL_SLEET_STORM ||
            id == SPELL_BLESS || id == SPELL_SHIELD_OF_FAITH || id == SPELL_SPIRIT_GUARDIANS;
    }

    bool ConcentrationLive(Encounter const& e, Combatant const& caster)
    {
        if (!caster.concentration)
            return false;
        if (caster.concentration == SPELL_HEX || caster.concentration == SPELL_CALL_LIGHTNING || caster.concentration == SPELL_BARKSKIN ||
            caster.concentration == SPELL_BLUR)
            return true;                         // a hex outlives its target (it can be moved); the storm, bark and blur last
        for (Area const& a : e.areas)            // the lasting areas: Moonbeam, Cloud of Daggers, the clouds and storms
            if (a.spellId == caster.concentration && a.owner == caster.guid)
                return true;
        if (caster.concentration == SPELL_WITCH_BOLT)
        {
            for (Combatant const& o : e.order)
                if (o.guid == caster.witchTarget && o.alive && !o.gone)
                    return true;
            return false;
        }
        for (Combatant const& o : e.order)
            if (!o.gone && o.alive && ((caster.concentration == SPELL_HOLD_PERSON && o.held && o.heldBy == caster.guid) ||
                (caster.concentration == SPELL_FAERIE_FIRE && o.outlined && o.outlinedBy == caster.guid) ||
                (caster.concentration == SPELL_HYPNOTIC_PATTERN && o.entranced && o.entrancedBy == caster.guid) ||
                ((caster.concentration == SPELL_ENTANGLE || caster.concentration == SPELL_WEB) && o.restrained &&
                    o.restrainedBy == caster.guid) ||
                (caster.concentration == SPELL_FEAR && o.afraid && o.afraidBy == caster.guid) ||
                (caster.concentration == SPELL_HEROISM && o.heroismBy == caster.guid) ||
                (caster.concentration == SPELL_BLESS && o.blessedBy == caster.guid) ||
                (caster.concentration == SPELL_SHIELD_OF_FAITH && o.faithBy == caster.guid)))
                return true;
        return false;
    }

    static char const* SpellName(uint32 id)
    {
        switch (id)
        {
            case SPELL_HOLD_PERSON: return "Hold Person";
            case SPELL_HEX:         return "Hex";
            case SPELL_FAERIE_FIRE: return "Faerie Fire";
            case SPELL_HYPNOTIC_PATTERN: return "Hypnotic Pattern";
            case SPELL_ENTANGLE:    return "Entangle";
            case SPELL_MOONBEAM:    return "Moonbeam";
            case SPELL_CALL_LIGHTNING: return "Call Lightning";
            case SPELL_BARKSKIN:    return "Barkskin";
            case SPELL_WITCH_BOLT:  return "Witch Bolt";
            case SPELL_HEROISM:     return "Heroism";
            case SPELL_CLOUD_OF_DAGGERS: return "Cloud of Daggers";
            case SPELL_WEB:         return "Web";
            case SPELL_BLUR:        return "Blur";
            case SPELL_FEAR:        return "Fear";
            case SPELL_STINKING_CLOUD: return "Stinking Cloud";
            case SPELL_HUNGER_OF_HADAR: return "Hunger of Hadar";
            case SPELL_SLEET_STORM: return "Sleet Storm";
            case SPELL_BLESS:       return "Bless";
            case SPELL_SHIELD_OF_FAITH: return "Shield of Faith";
            case SPELL_SPIRIT_GUARDIANS: return "Spirit Guardians";
            default:                return "a spell";
        }
    }

    void EndConcentration(Unit* caster, std::string const& why)
    {
        Encounter* e = caster ? EncounterOf(caster->GetGUID()) : nullptr;
        Combatant* me = e ? e->Find(caster->GetGUID()) : nullptr;
        if (!me || !me->concentration)
            return;
        uint32 spell = me->concentration;
        bool live = ConcentrationLive(*e, *me);
        me->concentration = 0;
        for (Combatant& o : e->order)
        {
            Unit* u = e->GetUnit(o);
            if (spell == SPELL_HOLD_PERSON && o.heldBy == me->guid)
            {
                if (o.held && u)
                    u->SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
                o.held = false;
                o.heldBy.Clear();
            }
            if (spell == SPELL_FAERIE_FIRE && o.outlinedBy == me->guid)
            {
                o.outlined = false;
                o.outlinedBy.Clear();
            }
            if (spell == SPELL_HYPNOTIC_PATTERN && o.entrancedBy == me->guid)
            {
                o.entranced = false;
                o.entrancedBy.Clear();
            }
            if ((spell == SPELL_ENTANGLE || spell == SPELL_WEB) && o.restrainedBy == me->guid)
            {
                o.restrained = false;
                o.restrainedBy.Clear();
            }
            if (spell == SPELL_FEAR && o.afraidBy == me->guid)
            {
                o.afraid = false;
                o.afraidBy.Clear();
            }
            if (spell == SPELL_HEROISM && o.heroismBy == me->guid)
                o.heroismBy.Clear();
            if (spell == SPELL_BLESS && o.blessedBy == me->guid)
                o.blessedBy.Clear();
            if (spell == SPELL_SHIELD_OF_FAITH && o.faithBy == me->guid)
                o.faithBy.Clear();
            if (spell == SPELL_HEX && o.hexedBy == me->guid)
            {
                o.hexedBy.Clear();
                if (u)
                    u->RemoveAurasDueToSpell(SPELL_HEX_AURA);
            }
        }
        if (spell == SPELL_HEX)
            me->hexFree = false;
        e->areas.erase(std::remove_if(e->areas.begin(), e->areas.end(), [&](Area const& a)
            { return a.spellId == spell && a.owner == me->guid; }), e->areas.end());   // its lasting area goes with it
        if (spell == SPELL_WITCH_BOLT)
            me->witchTarget.Clear();
        if (live)
            Narrate(caster, nullptr, Acore::StringFormat("   |cffb48cff{} loses concentration: {} ends{}.|r", me->name, SpellName(spell), why));
        PushOrder(*e);
    }

    void ConcentrationCheck(Player* p, int damage, Unit* attacker)
    {
        Encounter* e = p && p->IsAlive() && damage > 0 ? EncounterOf(p->GetGUID()) : nullptr;
        Combatant* me = e ? e->Find(p->GetGUID()) : nullptr;
        if (!me || !me->concentration)
            return;
        if (!ConcentrationLive(*e, *me))
        {
            me->concentration = 0;               // nobody left under it: nothing to hold on to
            PushOrder(*e);
            return;
        }
        int dc = std::max(10, damage / 2);
        if (!SavingThrow(attacker ? attacker : p, p, CON, dc, false, Acore::StringFormat("Concentration ({})", SpellName(me->concentration)),
            false, HasFeat(p, "War Caster")))
            EndConcentration(p, "");
    }

    // a legendary action: one swing (or shot) out of turn, resolved on the spot
    void CreatureLegendaryAttack(Creature* c, Unit* target, bool ranged)
    {
        if (!c || !target || !target->IsAlive())
            return;
        CreatureStats const& s = StatsOrDefault(c);
        Attack const& a = ranged ? s.ranged : s.melee;
        c->SetFacingToObject(target);
        c->HandleEmoteCommand(c->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID) ? EMOTE_ONESHOT_ATTACK1H : EMOTE_ONESHOT_ATTACK_UNARMED);
        AttackRoll(c, target, "Legendary Action (" + a.name + ")", a.bonus, a.dice, a.type, nullptr, !ranged, true);
    }

    void CreatureAttack(Creature* c, Unit* target, bool ranged, bool opportunity)
    {
        if (!c || !target || !target->IsAlive())
            return;
        CreatureStats const& s = StatsOrDefault(c);
        c->SetFacingToObject(target);
        if (ranged && !opportunity)
        {
            // the missile carries the attack; the roll happens when it lands (ResolveHit)
            c->CastSpell(target, CreatureMissile(c), true);
            return;
        }
        Attack const& a = s.melee;
        if (opportunity)                     // reactions swing on the spot (turn attacks wind up in the engine)
            c->HandleEmoteCommand(c->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID) ? EMOTE_ONESHOT_ATTACK1H : EMOTE_ONESHOT_ATTACK_UNARMED);
        AttackRoll(c, target, opportunity ? "Opportunity Attack (" + a.name + ")" : a.name, a.bonus, a.dice, a.type, nullptr, true, true);
    }

    void PrepareCreatureAttack(Creature* c, Unit* target, bool ranged)
    {
        if (!c)
            return;
        sPending.erase(c->GetGUID());
        if (!target || !target->IsAlive())
            return;
        CreatureStats const& s = StatsOrDefault(c);
        Attack const& a = ranged ? s.ranged : s.melee;
        SpellInfo const* throwSpell = ranged ? sSpellMgr->GetSpellInfo(CreatureMissile(c)) : nullptr;
        AttackRoll(c, target, a.name, a.bonus, a.dice, a.type, throwSpell, !ranged, !ranged, nullptr, true);
    }

    bool HasPendingAttack(Unit* attacker) { return attacker && sPending.count(attacker->GetGUID()); }

    void ApplyPendingAttack(Unit* attacker)
    {
        auto itr = attacker ? sPending.find(attacker->GetGUID()) : sPending.end();
        if (itr == sPending.end())
            return;
        PendingAttack pa = itr->second;
        sPending.erase(itr);
        Unit* target = ObjectAccessor::GetUnit(*attacker, pa.target);
        if (!target || !target->IsAlive())
            return;
        SpellInfo const* throwSpell = pa.melee ? nullptr : sSpellMgr->GetSpellInfo(CreatureMissile(attacker->ToCreature()));
        if (!pa.hit)
        {
            // a player who isn't hit visibly gets out of the way ("Dodge" over their head, the dodge animation)
            bool dodge = target->IsPlayer();
            if (pa.melee || !throwSpell)
                attacker->SendAttackStateUpdate(HITINFO_MISS, target, 1, SPELL_SCHOOL_MASK_NORMAL, 0, 0, 0,
                    dodge ? VICTIMSTATE_DODGE : VICTIMSTATE_INTACT, 0);
            else
                attacker->SendSpellMiss(target, throwSpell->Id, dodge ? SPELL_MISS_DODGE : SPELL_MISS_MISS);
            if (dodge)
                target->HandleEmoteCommand(EMOTE_ONESHOT_DODGE);
            return;
        }
        DamageCtx d;
        d.attacker = attacker;
        d.victim = target;
        d.amount = pa.amount;
        d.type = pa.type;
        d.spell = throwSpell;
        d.crit = pa.crit;
        d.melee = pa.melee;
        ApplyDamage(d);
        if (pa.melee)
            VenomRider(attacker, target);
    }

    // ------------------------------------------------------------------ resolution
    void ResolveHit(Unit* caster, Unit* target, SpellInfo const* spell)
    {
        if (!caster || !target || !spell)
            return;
        uint32 id = spell->Id;

        if (Creature* c = caster->ToCreature())
        {
            if (IsCreatureMissile(id))
            {
                if (sPending.count(c->GetGUID()))
                    ApplyPendingAttack(c);
                else
                {
                    Attack const& a = StatsOrDefault(c).ranged;
                    AttackRoll(c, target, a.name, a.bonus, a.dice, a.type, spell, false, false);
                }
            }
            return;
        }
        Player* p = caster->ToPlayer();
        if (p && id == SPELL_HEROISM)
        {
            if (Combatant* hc = CombatantOf(target->GetGUID()))
            {
                hc->heroismBy = p->GetGUID();
                Narrate(p, target, Acore::StringFormat("{} fills {} with courage: temporary hit points at the start of each of their turns.",
                    Who(p), Who(target)));
            }
            return;
        }
        if (p && (id == SPELL_SHIELD_OF_FAITH || id == SPELL_LESSER_RESTORATION))
        {
            if (Combatant* tc = CombatantOf(target->GetGUID()))
            {
                if (id == SPELL_SHIELD_OF_FAITH)
                    tc->faithBy = p->GetGUID();
                else
                    tc->poisoned = 0;
            }
            Narrate(p, target, id == SPELL_SHIELD_OF_FAITH ? Acore::StringFormat("{} wraps {} in a shimmering field: +2 Armour Class.",
                Who(p), Who(target)) : Acore::StringFormat("{} cleanses {}: no longer Poisoned.", Who(p), Who(target)));
            if (Encounter* e = EncounterOf(target->GetGUID()))
                PushOrder(*e);
            return;
        }
        if (p && (id == SPELL_BLESS || id == SPELL_AID))
        {
            // the target and the two allies nearest the caster within 9 m (the caster among them)
            std::vector<Player*> who;
            if (Player* tp = target->ToPlayer())
                who.push_back(tp);
            std::vector<Player*> others;
            p->GetMap()->DoForAllPlayers([&](Player* o)
            {
                if (o->IsAlive() && o->GetExactDist(p) <= 9.f && std::find(who.begin(), who.end(), o) == who.end())
                    others.push_back(o);
            });
            std::sort(others.begin(), others.end(), [p](Player* x, Player* y) { return x->GetExactDist(p) < y->GetExactDist(p); });
            for (Player* o : others)
                if (who.size() < 3)
                    who.push_back(o);
            for (Player* o : who)
            {
                if (id == SPELL_BLESS)
                {
                    if (Combatant* oc = CombatantOf(o->GetGUID()))
                        oc->blessedBy = p->GetGUID();
                    Narrate(p, o, Acore::StringFormat("   {} is blessed: +1d4 on attacks and saves that would fall short.", Who(o)));
                }
                else
                {
                    ApplyHeal(p, o, 5 + HealBonus(p, 2), spell);
                    Sheet& os = GetSheet(o);
                    os.tempHp = std::max(os.tempHp, 5);
                    PushSheet(o);
                    Narrate(p, o, Acore::StringFormat("   {} is bolstered: healed {} and 5 temporary hit points.", Who(o), 5 + HealBonus(p, 2)));
                }
            }
            return;
        }
        if (p && (id == SPELL_HEALING_WORD || id == SPELL_CURE_WOUNDS))
        {
            int lvl = CastLevel(p, 1);
            int mod = CastingMod(p) + HealBonus(p, lvl);
            std::string detail;
            int amount = std::max(1, Dice{ lvl, id == SPELL_HEALING_WORD ? 4 : 8, mod }.Roll(false, detail));
            if (target != p && IsSubclass(p, "Life") && Level(p) >= 6)
                ApplyHeal(p, p, 2 + lvl, spell);     // Blessed Healer
            Narrate(p, target, Acore::StringFormat("{} {} -> {}: {} = |cff80ff80{}|r hit points", Who(p),
                id == SPELL_HEALING_WORD ? "Healing Word" : "Cure Wounds", Who(target), detail, amount));
            ApplyHeal(p, target, amount, spell);
            return;
        }
        if (!p || target == caster)
            return;
        int atk = SpellAttackBonus(p);
        int cd = CantripDice(p);
        sCurSpell = id;

        switch (id)
        {
            case SPELL_ATTACK:
                WeaponAttack(p, target, spell, false);
                break;
            case SPELL_RANGED_ATTACK:
                PlayerRangedAttack(p, target, spell);
                break;
            case SPELL_THROW:
                PlayerThrowAttack(p, target, spell);
                break;
            case SPELL_OFFHAND_ATTACK:
                PlayerOffhandAttack(p, target, spell);
                break;
            case SPELL_TRIP_ATTACK:
            case SPELL_PUSHING_ATTACK:
            case SPELL_MENACING_ATTACK:
            {
                char const* name = id == SPELL_TRIP_ATTACK ? "Trip Attack" : (id == SPELL_PUSHING_ATTACK ? "Pushing Attack" : "Menacing Attack");
                if (!PlayerWeaponAttack(p, target, spell, false, name) || !target->IsAlive())
                    break;
                Sheet& s = GetSheet(p);
                ++s.supUsed;                     // the die is only spent on a hit
                SaveSheet(p);
                PushSheet(p);
                std::string detail;
                std::vector<int> rolls;
                Dice sup{ 1, 8, 0 };
                int extra = sup.Roll(false, detail, &rolls);
                SendRoll(p, target, Acore::StringFormat("RD^{}^{}^{}^{}^{}^{}^{}^{}^{}^{}", Clean(p->GetName()), Clean(target->GetName()),
                    Clean(std::string(name) + " (superiority die)"), sup.sides, 0, JoinRolls(rolls), extra, DmgTypeNames[PlayerWeapon(p).type],
                    0, 0));
                Narrate(p, target, Acore::StringFormat("   Superiority die: {} = |cffffffff{}|r extra damage ({} left).", detail, extra,
                    std::max(0, SuperiorityMax(p) - s.supUsed)));
                DamageCtx d;
                d.attacker = p;
                d.victim = target;
                d.amount = extra;
                d.type = PlayerWeapon(p).type;
                d.melee = true;
                ApplyDamage(d);
                if (!target->IsAlive())
                    break;
                int dc = ManeuverDC(p);
                if (id == SPELL_TRIP_ATTACK)
                {
                    if (!SavingThrow(p, target, STR, dc, false, "Trip Attack"))
                        KnockProne(p, target);
                }
                else if (id == SPELL_PUSHING_ATTACK)
                {
                    if (!SavingThrow(p, target, STR, dc, false, "Pushing Attack"))
                    {
                        float ang = p->GetAngle(target);
                        float x = target->GetPositionX() + std::cos(ang) * 4.5f;
                        float y = target->GetPositionY() + std::sin(ang) * 4.5f;
                        float z = target->GetPositionZ();
                        target->UpdateGroundPositionZ(x, y, z);
                        ShoveTo(target, x, y, z);
                        Narrate(p, target, Acore::StringFormat("   {} is driven back 4.5 m.", Who(target)));
                    }
                }
                else if (!SavingThrow(p, target, WIS, dc, false, "Menacing Attack"))
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->frightened = true;
                        c->frightenedBy = p->GetGUID();
                        Narrate(p, target, Acore::StringFormat("   {} is |cffc080ffFrightened|r: disadvantage on its attacks until your next turn.",
                            Who(target)));
                        if (Encounter* e = EncounterOf(target->GetGUID()))
                            PushOrder(*e);
                    }
                break;
            }
            case SPELL_HELP:
                if (Player* tp = target->ToPlayer())
                    GetUp(tp, 1, "helped up by " + std::string(p->GetName()));
                break;
            case SPELL_ARROW_FIRE:
            case SPELL_ARROW_ICE:
            case SPELL_ARROW_ACID:
            case SPELL_ARROW_LIGHTNING:
            case SPELL_ARROW_SLAYING:
                ArrowShot(p, target, spell);
                break;
            case SPELL_ALCHEMISTS_FIRE:
            case SPELL_ACID_VIAL:
            case SPELL_SMOKEPOWDER:
            case SPELL_HOLY_WATER:
            case SPELL_FROST_BOMB:
                Grenade(p, target, spell);
                break;
            case SPELL_VICIOUS_MOCKERY:
                if (!SaveForDamage(p, target, spell, WIS, Dice{ cd, 4, 0 }, DMG_PSYCHIC, false, "Vicious Mockery") && target->IsAlive())
                    if (Combatant* vc = CombatantOf(target->GetGUID()))
                    {
                        vc->mocked = true;
                        Narrate(p, target, Acore::StringFormat("   {} is rattled: disadvantage on its next attack.", Who(target)));
                    }
                break;
            case SPELL_BARDIC_INSPIRATION:
                if (Player* tp = target->ToPlayer())
                    Inspire(p, tp);
                break;
            case SPELL_PRODUCE_FLAME:
                AttackRoll(p, target, "Produce Flame", atk, Dice{ cd, 8, 0 }, DMG_FIRE, spell, false, false);
                break;
            case SPELL_CHILL_TOUCH:
                AttackRoll(p, target, "Chill Touch", atk, Dice{ cd, 8, 0 }, DMG_NECROTIC, spell, false, false);
                break;
            case SPELL_SACRED_FLAME:
                SaveForDamage(p, target, spell, DEX, Dice{ cd, 8, 0 }, DMG_RADIANT, false, "Sacred Flame");
                break;
            case SPELL_TOLL_THE_DEAD:
                SaveForDamage(p, target, spell, WIS, Dice{ cd, target->GetHealth() < target->GetMaxHealth() ? 12 : 8, 0 }, DMG_NECROTIC, false,
                    "Toll the Dead");
                break;
            case SPELL_GUIDING_BOLT:
            {
                int lvl = CastLevel(p, 1);
                if (AttackRoll(p, target, "Guiding Bolt", atk, Dice{ lvl + 3, 6, 0 }, DMG_RADIANT, spell, false, false) && target->IsAlive())
                    if (Combatant* gc = CombatantOf(target->GetGUID()))
                    {
                        gc->guided = true;
                        Narrate(p, target, Acore::StringFormat("   {} glows: the next attack on it has advantage.", Who(target)));
                    }
                break;
            }
            case SPELL_INFLICT_WOUNDS:
                AttackRoll(p, target, "Inflict Wounds", atk, Dice{ CastLevel(p, 1) + 2, 10, 0 }, DMG_NECROTIC, spell, true, false);
                break;
            case SPELL_POISON_SPRAY:
                SaveForDamage(p, target, spell, CON, Dice{ cd, 12, 0 }, DMG_POISON, false, "Poison Spray");
                break;
            case SPELL_ACID_SPLASH:
            {
                // the target, and one creature right next to it
                Unit* second = nullptr;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    for (Combatant const& o : e->order)
                        if (!o.player && !o.gone && o.alive && o.guid != target->GetGUID())
                            if (Unit* u = e->GetUnit(o); u && u->IsAlive() && u->GetExactDist(target) <= 1.5f + u->GetCombatReach() &&
                                !second)
                                second = u;
                SaveForDamage(p, target, spell, DEX, Dice{ cd, 6, 0 }, DMG_ACID, false, "Acid Splash");
                if (second)
                    SaveForDamage(p, second, spell, DEX, Dice{ cd, 6, 0 }, DMG_ACID, false, "Acid Splash");
                break;
            }
            case SPELL_RAY_OF_SICKNESS:
            {
                int lvl = CastLevel(p, 1);
                if (AttackRoll(p, target, "Ray of Sickness", atk, Dice{ lvl + 1, 8, 0 }, DMG_POISON, spell, false, false) &&
                    target->IsAlive() && !SavingThrow(p, target, CON, SpellSaveDC(p), true, "Ray of Sickness", true))
                    if (Combatant* rc = CombatantOf(target->GetGUID()))
                    {
                        rc->poisoned = 2;
                        Narrate(p, target, Acore::StringFormat("   {} is |cff40c040Poisoned|r.", Who(target)));
                    }
                break;
            }
            case SPELL_WITCH_BOLT:
            {
                // while the arc holds (concentration) a bolt at the same creature hits by itself; anyone else: a new arc
                Combatant* me = CombatantOf(p->GetGUID());
                if (me && me->concentration == SPELL_WITCH_BOLT && !me->witchTarget.IsEmpty() && me->witchTarget == target->GetGUID())
                {
                    std::string detail;
                    DamageCtx d;
                    d.attacker = p;
                    d.victim = target;
                    d.amount = Dice{ 1, 12, 0 }.Roll(false, detail);
                    d.type = DMG_LIGHTNING;
                    Narrate(p, target, Acore::StringFormat("{} Witch Bolt: the arc holds - {} = {} Lightning", Who(p), detail, d.amount));
                    ApplyDamage(d);
                    break;
                }
                int lvl = CastLevel(p, 1);
                if (AttackRoll(p, target, "Witch Bolt", atk, Dice{ lvl, 12, 0 }, DMG_LIGHTNING, spell, false, false) && me)
                    me->witchTarget = target->GetGUID();
                break;
            }
            case SPELL_FEAR:
                if (!SavingThrow(p, target, WIS, SpellSaveDC(p), true, "Fear"))
                    if (Combatant* fc = CombatantOf(target->GetGUID()))
                    {
                        fc->afraid = true;
                        fc->afraidBy = p->GetGUID();
                        Narrate(p, target, Acore::StringFormat("   {} is |cffc080ffterrified|r: disadvantage on its attacks while you hold "
                            "the spell (a WIS save each turn).", Who(target)));
                    }
                break;
            case SPELL_SHOVE:
            {
                int a = AbilityMod(p, STR);
                int def = std::max(AbilityMod(target, STR), AbilityMod(target, DEX));
                Combatant* pc = CombatantOf(p->GetGUID());
                // Poisoned: disadvantage on ability checks; Rage / Aspect of the Bear: advantage on Strength checks
                D20Result r1 = RollD20(a, (pc && pc->poisoned ? -1 : 0) + (StrengthCheckAdvantage(p) ? 1 : 0));
                D20Result r2 = RollD20(def, 0);
                bool ok = r1.total > r2.total;
                Narrate(p, target, Acore::StringFormat("{} Shoves {}: Athletics {} vs {}  {}", Who(p), Who(target), r1.text, r2.text,
                    ok ? "|cff40ff40SUCCESS|r" : "|cffaaaaaaFAILED|r"));
                if (ok)
                {
                    float ang = p->GetAngle(target);
                    float x = target->GetPositionX() + std::cos(ang) * 4.5f;
                    float y = target->GetPositionY() + std::sin(ang) * 4.5f;
                    float z = target->GetPositionZ();
                    target->UpdateGroundPositionZ(x, y, z);
                    ShoveTo(target, x, y, z);
                }
                break;
            }
            case SPELL_FIRE_BOLT:
                AttackRoll(p, target, "Fire Bolt", atk, Dice{ cd, 10, EvocationBonus(p) }, DMG_FIRE, spell, false, false);
                break;
            case SPELL_RAY_OF_FROST:
                if (AttackRoll(p, target, "Ray of Frost", atk, Dice{ cd, 8, EvocationBonus(p) }, DMG_COLD, spell, false, false))
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->slowed = true;
                        Narrate(p, target, Acore::StringFormat("   {} is |cff80c0ffChilled|r: -3 m movement next turn.", Who(target)));
                    }
                break;
            case SPELL_SHOCKING_GRASP:
                if (AttackRoll(p, target, "Shocking Grasp", atk, Dice{ cd, 8, EvocationBonus(p) }, DMG_LIGHTNING, spell, true, false))
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->noReaction = true;
                        Narrate(p, target, Acore::StringFormat("   {} is |cffa0a0ffShocked|r: no reactions until its turn.", Who(target)));
                    }
                break;
            case SPELL_MAGIC_MISSILE:
            {
                Narrate(p, target, Acore::StringFormat("{} Magic Missile -> {}: 3 darts, auto-hit", Who(p), Who(target)));
                for (int i = 0; i < 3 && target->IsAlive(); ++i)
                {
                    std::string detail;
                    int dmg = Dice{ 1, 4, 1 + (i == 0 ? EvocationBonus(p) : 0) }.Roll(false, detail);
                    Narrate(p, target, Acore::StringFormat("   dart {}: {} = |cffffffff{}|r Force", i + 1, detail, dmg));
                    DamageCtx d;
                    d.attacker = p;
                    d.victim = target;
                    d.amount = dmg;
                    d.type = DMG_FORCE;
                    d.spell = spell;
                    ApplyDamage(d);
                }
                break;
            }
            case SPELL_BURNING_HANDS:
                SaveForDamage(p, target, spell, DEX, Dice{ 3, 6, EvocationBonus(p) }, DMG_FIRE, true, "Burning Hands");
                break;
            case SPELL_CHROMATIC_FIRE:
                AttackRoll(p, target, "Chromatic Orb (Fire)", atk, Dice{ 3, 8, EvocationBonus(p) }, DMG_FIRE, spell, false, false);
                break;
            case SPELL_CHROMATIC_COLD:
                AttackRoll(p, target, "Chromatic Orb (Cold)", atk, Dice{ 3, 8, EvocationBonus(p) }, DMG_COLD, spell, false, false);
                break;
            case SPELL_CHROMATIC_LIGHT:
                AttackRoll(p, target, "Chromatic Orb (Lightning)", atk, Dice{ 3, 8, EvocationBonus(p) }, DMG_LIGHTNING, spell, false, false);
                break;
            case SPELL_ICE_KNIFE:
            {
                AttackRoll(p, target, "Ice Knife", atk, Dice{ 1, 10, 0 }, DMG_PIERCING, spell, false, false);
                // the shard explodes whether it hit or not: everything within 1.5 m (+reach) of the target
                Position centre = target->GetPosition();
                std::vector<Unit*> caught;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                {
                    for (Combatant const& c : e->order)
                        if (!c.gone && c.alive)
                            if (Unit* u = e->GetUnit(c))
                                if (u->IsAlive() && u->GetExactDist(&centre) <= 3.f + u->GetCombatReach())   // 3 m (5e: 1.5)
                                    caught.push_back(u);
                }
                else if (target->IsAlive())
                    caught.push_back(target);
                Narrate(p, target, "   The ice shard explodes!");
                for (Unit* u : caught)
                    SaveForDamage(p, u, spell, DEX, Dice{ 2, 6, 0 }, DMG_COLD, false, "Ice Knife");
                break;
            }
            case SPELL_GREASE:
            {
                if (!SavingThrow(p, target, DEX, SpellSaveDC(p), true, "Grease"))
                    KnockProne(p, target);
                break;
            }
            case SPELL_SCORCHING_RAY:
            {
                // three rays, each its own attack (BG3 lets you split them; here they all go at the target)
                Narrate(p, target, Acore::StringFormat("{} Scorching Ray -> {}: three rays", Who(p), Who(target)));
                for (int i = 0; i < 3 && target->IsAlive(); ++i)
                    AttackRoll(p, target, Acore::StringFormat("Scorching Ray ({})", i + 1), atk,
                        Dice{ 2, 6, i == 0 ? EvocationBonus(p) : 0 }, DMG_FIRE, spell, false, false);
                break;
            }
            case SPELL_SHATTER:          // everything in the 3 m blast (the spell's area finds them)
                SaveForDamage(p, target, spell, CON, Dice{ 3, 8, EvocationBonus(p) }, DMG_THUNDER, true, "Shatter");
                break;
            case SPELL_FIREBALL:         // everything in the 6 m blast
                SaveForDamage(p, target, spell, DEX, Dice{ 8, 6, EvocationBonus(p) }, DMG_FIRE, true, "Fireball");
                break;
            case SPELL_HOLD_PERSON:
            {
                Creature* cr = target->ToCreature();
                if (cr && cr->GetCreatureType() != CREATURE_TYPE_HUMANOID)
                {
                    Narrate(p, target, Acore::StringFormat("   {} is not a person - Hold Person has no effect.", Who(target)));
                    break;
                }
                if (!SavingThrow(p, target, WIS, SpellSaveDC(p), true, "Hold Person"))
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->held = true;
                        c->heldBy = p->GetGUID();
                        target->SetUInt32Value(UNIT_NPC_EMOTESTATE, EMOTE_STATE_STUN);
                        Narrate(p, target, Acore::StringFormat("   {} is |cffff8000Held|r: paralysed until it shakes it off (WIS save "
                            "each turn). Attacks against it have advantage; melee hits from close by are critical.", Who(target)));
                    }
                break;
            }
            case SPELL_LIGHTNING_BOLT:
            {
                // a 30 m line from the caster through the target: everything within 1.5 m of it saves
                float ang = p->GetAngle(target);
                float sx = p->GetPositionX(), sy = p->GetPositionY();
                std::vector<Unit*> caught;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                {
                    for (Combatant const& c : e->order)
                        if (!c.gone && c.alive && !c.player)
                            if (Unit* u = e->GetUnit(c))
                            {
                                float dx = u->GetPositionX() - sx, dy = u->GetPositionY() - sy;
                                float along = dx * std::cos(ang) + dy * std::sin(ang);
                                float off = std::fabs(-dx * std::sin(ang) + dy * std::cos(ang));
                                if (u->IsAlive() && along >= 0.f && along <= 30.f && off <= 1.5f + u->GetCombatReach())
                                    caught.push_back(u);
                            }
                }
                else if (target->IsAlive())
                    caught.push_back(target);
                Narrate(p, target, Acore::StringFormat("{} Lightning Bolt: a 30 m line - {} caught.", Who(p), caught.size()));
                for (Unit* u : caught)
                    SaveForDamage(p, u, spell, DEX, Dice{ 8, 6, EvocationBonus(p) }, DMG_LIGHTNING, true, "Lightning Bolt");
                break;
            }
            case SPELL_FRENZIED_STRIKE:
                PlayerWeaponAttack(p, target, spell, false, "Frenzied Strike");
                break;
            case SPELL_ELDRITCH_BLAST:
            {
                // a beam per cantrip die (two at 5), each its own attack; Agonizing Blast adds CHA, Repelling Blast pushes 3 m
                int beams = cd;
                int agonizing = HasInvocation(p, "Agonizing Blast") ? AbilityMod(p, CHA) : 0;
                for (int i = 0; i < beams && target->IsAlive(); ++i)
                    if (AttackRoll(p, target, beams > 1 ? Acore::StringFormat("Eldritch Blast ({})", i + 1) : "Eldritch Blast", atk,
                        Dice{ 1, 10, agonizing }, DMG_FORCE, spell, false, false) && target->IsAlive() && HasInvocation(p, "Repelling Blast"))
                    {
                        PushAway(p, target, 3.f);
                        Narrate(p, target, Acore::StringFormat("   {} is hurled back 3 m.", Who(target)));
                    }
                break;
            }
            case SPELL_HEX:
            {
                Combatant* tc = CombatantOf(target->GetGUID());
                if (!tc)
                    break;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    for (Combatant& o : e->order)
                        if (o.hexedBy == p->GetGUID())
                        {
                            o.hexedBy.Clear();       // one hex at a time: it moves
                            if (Unit* u = e->GetUnit(o))
                                u->RemoveAurasDueToSpell(SPELL_HEX_AURA);
                        }
                tc->hexedBy = p->GetGUID();
                p->AddAura(SPELL_HEX_AURA, target);
                Narrate(p, target, Acore::StringFormat("{} lays a |cffa040ffHex|r on {}: your attacks deal +1d6 Necrotic to it.", Who(p),
                    Who(target)));
                break;
            }
            case SPELL_DISSONANT_WHISPERS:
            {
                int lvl = CastLevel(p, 1);
                if (!SaveForDamage(p, target, spell, WIS, Dice{ lvl + 2, 6, 0 }, DMG_PSYCHIC, true, "Dissonant Whispers") && target->IsAlive())
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->frightened = true;
                        c->frightenedBy = p->GetGUID();
                        Narrate(p, target, Acore::StringFormat("   {} reels from the whispers: |cffc080ffFrightened|r until your next turn.",
                            Who(target)));
                        if (Encounter* e = EncounterOf(target->GetGUID()))
                            PushOrder(*e);
                    }
                break;
            }
            case SPELL_FAERIE_FIRE:
                if (!SavingThrow(p, target, DEX, SpellSaveDC(p), true, "Faerie Fire"))
                    if (Combatant* c = CombatantOf(target->GetGUID()))
                    {
                        c->outlined = true;
                        c->outlinedBy = p->GetGUID();
                        Narrate(p, target, Acore::StringFormat("   {} is outlined in faerie light: attacks against it have advantage.",
                            Who(target)));
                    }
                break;
            case SPELL_VAMPIRIC_TOUCH:
            {
                int dealt = 0;
                int lvl = CastLevel(p, 3);
                if (AttackRoll(p, target, "Vampiric Touch", atk, Dice{ lvl, 6, 0 }, DMG_NECROTIC, spell, true, false, &dealt) && dealt > 1)
                {
                    ApplyHeal(p, p, dealt / 2, spell);
                    Narrate(p, nullptr, Acore::StringFormat("   {} drinks the life: |cff80ff80{}|r hit points.", Who(p), dealt / 2));
                }
                break;
            }
            case SPELL_UNARMED_STRIKE:
                sForceUnarmed = true;
                PlayerWeaponAttack(p, target, spell, false, "Martial Arts");
                sForceUnarmed = false;
                break;
            case SPELL_FLURRY:
            {
                Sheet& s = GetSheet(p);
                ++s.kiUsed;
                SaveSheet(p);
                PushSheet(p);
                Narrate(p, target, Acore::StringFormat("{} unleashes a Flurry of Blows (1 ki).", Who(p)));
                for (int i = 0; i < 2 && target->IsAlive(); ++i)
                {
                    sForceUnarmed = true;
                    bool hit = PlayerWeaponAttack(p, target, spell, false, "Flurry of Blows");
                    sForceUnarmed = false;
                    // Open Hand: each blow can topple them
                    if (hit && target->IsAlive() && IsSubclass(p, "Open Hand") && Level(p) >= 3 &&
                        !SavingThrow(p, target, DEX, KiDC(p), false, "Open Hand Technique"))
                        KnockProne(p, target);
                }
                break;
            }
            case SPELL_STUNNING_STRIKE:
                if (PlayerWeaponAttack(p, target, spell, false, "Stunning Strike") && target->IsAlive())
                {
                    Sheet& s = GetSheet(p);
                    ++s.kiUsed;                      // spent on the hit
                    SaveSheet(p);
                    PushSheet(p);
                    if (!SavingThrow(p, target, CON, KiDC(p), false, "Stunning Strike"))
                        if (Combatant* c = CombatantOf(target->GetGUID()))
                        {
                            c->stunned = 1;
                            Narrate(p, target, Acore::StringFormat("   {} is |cffffff40Stunned|r: it loses its next turn, and attacks "
                                "against it have advantage.", Who(target)));
                            if (Encounter* e = EncounterOf(target->GetGUID()))
                                PushOrder(*e);
                        }
                }
                break;
            default:
                break;
        }
        // an attack from hiding: now that it has landed, a survivor starts the fight, a kill may go unseen
        if (IsSneaking(p) && SneakStruck(p) && !InCombat(p))
            ResolveAmbush(p, target);
        // Twinned Spell: a single-target spell strikes a second foe too
        static bool twinning = false;
        bool single = id == SPELL_FIRE_BOLT || id == SPELL_RAY_OF_FROST || id == SPELL_SHOCKING_GRASP || id == SPELL_MAGIC_MISSILE ||
            id == SPELL_CHROMATIC_FIRE || id == SPELL_CHROMATIC_COLD || id == SPELL_CHROMATIC_LIGHT || id == SPELL_HOLD_PERSON;
        if (single && !twinning && CastHas(p, id, "twin"))
            if (Encounter* e = EncounterOf(p->GetGUID()))
            {
                Unit* other = nullptr;
                for (Combatant const& c : e->order)
                    if (!c.player && c.alive && !c.gone)
                        if (Unit* u = e->GetUnit(c))
                            if (u != target && u->IsAlive() && u->GetExactDist(p) <= 18.f && (!other || u->GetExactDist(target) < other->GetExactDist(target)))
                                other = u;
                UseCastMeta(p, "twin");
                if (other)
                {
                    twinning = true;
                    Narrate(p, other, Acore::StringFormat("   |cffd080ffTwinned Spell|r: the magic splits toward {}!", Who(other)));
                    ResolveHit(p, other, spell);
                    twinning = false;
                }
            }
    }

    // every creature in the caster's fight within r of a spot
    static std::vector<Unit*> CreaturesNear(Player* p, Position const& at, float r)
    {
        std::vector<Unit*> out;
        if (Encounter* e = EncounterOf(p->GetGUID()))
            for (Combatant const& o : e->order)
                if (!o.player && !o.gone && o.alive)
                    if (Unit* u = e->GetUnit(o); u && u->IsAlive() && u->GetExactDist2d(&at) <= r + u->GetCombatReach())
                        out.push_back(u);
        return out;
    }

    bool AreaTurnStart(Player* owner, Unit* u, uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        switch (spellId)
        {
            case SPELL_MOONBEAM:
                MoonbeamBurn(owner, u);
                return false;
            case SPELL_CLOUD_OF_DAGGERS:
            {
                std::string detail;
                DamageCtx d;
                d.attacker = owner;
                d.victim = u;
                d.amount = Dice{ 4, 4, 0 }.Roll(false, detail);
                d.type = DMG_SLASHING;
                Narrate(owner, u, Acore::StringFormat("   {} starts its turn among the daggers: {} = {} Slashing", Who(u), detail, d.amount));
                ApplyDamage(d);
                return false;
            }
            case SPELL_STINKING_CLOUD:
                if (!SavingThrow(owner, u, CON, SpellSaveDC(owner), true, "Stinking Cloud", true))
                {
                    Narrate(owner, u, Acore::StringFormat("   {} retches in the stinking cloud and loses its turn.", Who(u)));
                    return true;
                }
                return false;
            case SPELL_HUNGER_OF_HADAR:
            {
                std::string detail;
                DamageCtx d;
                d.attacker = owner;
                d.victim = u;
                d.amount = Dice{ 2, 6, 0 }.Roll(false, detail);
                d.type = DMG_COLD;
                Narrate(owner, u, Acore::StringFormat("   The freezing dark bites {}: {} = {} Cold", Who(u), detail, d.amount));
                ApplyDamage(d);
                return false;
            }
            case SPELL_SLEET_STORM:
                if (!SavingThrow(owner, u, DEX, SpellSaveDC(owner), true, "Sleet Storm"))
                    KnockProne(owner, u);
                return false;
            case SPELL_SPIRIT_GUARDIANS:
                Narrate(owner, u, Acore::StringFormat("   {} starts its turn among {}'s spirit guardians.", Who(u), Who(owner)));
                SaveForDamage(owner, u, info, WIS, Dice{ 3, 8, 0 }, DMG_RADIANT, true, "Spirit Guardians");
                if (Combatant* uc = CombatantOf(u->GetGUID()))
                    uc->moveMax *= 0.5f;
                return false;
            default:
                (void)info;
                return false;
        }
    }

    void MoonbeamBurn(Player* owner, Unit* u)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(SPELL_MOONBEAM);
        Narrate(owner, u, Acore::StringFormat("   {} starts its turn in the moonbeam.", Who(u)));
        SaveForDamage(owner, u, info, CON, Dice{ 2, 10, 0 }, DMG_RADIANT, true, "Moonbeam");
    }

    void ResolveDest(Unit* caster, Position const& dest, SpellInfo const* spell)
    {
        Player* p = caster->ToPlayer();
        if (!p)
            return;
        switch (spell->Id)
        {
            case SPELL_HYPNOTIC_PATTERN:
            {
                std::vector<Unit*> caught = CreaturesNear(p, dest, 4.5f);
                Narrate(p, nullptr, Acore::StringFormat("{} Hypnotic Pattern: a twisting web of colour - {} caught.", Who(p), caught.size()));
                for (Unit* u : caught)
                    if (!SavingThrow(p, u, WIS, SpellSaveDC(p), true, "Hypnotic Pattern"))
                        if (Combatant* uc = CombatantOf(u->GetGUID()))
                        {
                            uc->entranced = true;
                            uc->entrancedBy = p->GetGUID();
                            Narrate(p, u, Acore::StringFormat("   {} is |cffd080ffentranced|r: it does nothing until something hurts it.", Who(u)));
                        }
                break;
            }
            case SPELL_ENTANGLE:
            {
                std::vector<Unit*> caught = CreaturesNear(p, dest, 6.f);
                Narrate(p, nullptr, Acore::StringFormat("{} Entangle: grasping weeds and vines - {} caught.", Who(p), caught.size()));
                for (Unit* u : caught)
                    if (!SavingThrow(p, u, STR, SpellSaveDC(p), true, "Entangle"))
                        if (Combatant* uc = CombatantOf(u->GetGUID()))
                        {
                            uc->restrained = true;
                            uc->restrainedBy = p->GetGUID();
                            Narrate(p, u, Acore::StringFormat("   {} is |cff60c060restrained|r: going nowhere, easy to hit, and its own "
                                "attacks at disadvantage.", Who(u)));
                        }
                break;
            }
            case SPELL_MOONBEAM:
            {
                if (Encounter* e = EncounterOf(p->GetGUID()))
                {
                    Area a{ SPELL_MOONBEAM, dest, 1.5f, uint32(GameTime::GetGameTimeMS().count()) + 60000u, p->GetGUID() };
                    e->areas.push_back(a);
                }
                int lvl = CastLevel(p, 2);
                std::vector<Unit*> caught = CreaturesNear(p, dest, 1.5f);
                Narrate(p, nullptr, Acore::StringFormat("{} Moonbeam: a column of pale light - {} caught; it burns whoever starts a turn in it.",
                    Who(p), caught.size()));
                for (Unit* u : caught)
                    SaveForDamage(p, u, spell, CON, Dice{ lvl, 10, 0 }, DMG_RADIANT, true, "Moonbeam");
                break;
            }
            case SPELL_SLEEP:
            {
                // 5d8 hit points' worth (+2d8 a slot level above 1st), the weakest first: they fall asleep until hurt
                int lvl = CastLevel(p, 1);
                std::string detail;
                int pool = Dice{ 3 + 2 * lvl, 8, 0 }.Roll(false, detail);
                std::vector<Unit*> caught = CreaturesNear(p, dest, 6.f);
                std::sort(caught.begin(), caught.end(), [](Unit* x, Unit* y) { return x->GetHealth() < y->GetHealth(); });
                Narrate(p, nullptr, Acore::StringFormat("{} Sleep: {} = {} hit points' worth of drowsiness.", Who(p), detail, pool));
                for (Unit* u : caught)
                {
                    if (int(u->GetHealth()) > pool)
                        break;
                    pool -= int(u->GetHealth());
                    if (Combatant* uc = CombatantOf(u->GetGUID()))
                    {
                        uc->asleep = true;
                        Narrate(p, u, Acore::StringFormat("   {} falls |cff9ad0ffasleep|r.", Who(u)));
                    }
                }
                break;
            }
            case SPELL_WEB:
            {
                std::vector<Unit*> caught = CreaturesNear(p, dest, 3.f);
                Narrate(p, nullptr, Acore::StringFormat("{} Web: thick sticky strands - {} caught.", Who(p), caught.size()));
                for (Unit* u : caught)
                    if (!SavingThrow(p, u, DEX, SpellSaveDC(p), true, "Web"))
                        if (Combatant* uc = CombatantOf(u->GetGUID()))
                        {
                            uc->restrained = true;
                            uc->restrainedBy = p->GetGUID();
                            Narrate(p, u, Acore::StringFormat("   {} is |cff60c060restrained|r in the web.", Who(u)));
                        }
                break;
            }
            case SPELL_CLOUD_OF_DAGGERS:
            case SPELL_STINKING_CLOUD:
            case SPELL_HUNGER_OF_HADAR:
            case SPELL_SLEET_STORM:
            {
                uint32 id = spell->Id;
                float r = id == SPELL_CLOUD_OF_DAGGERS ? 1.f : (id == SPELL_SLEET_STORM ? 9.f : 6.f);
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    e->areas.push_back(Area{ id, dest, r, uint32(GameTime::GetGameTimeMS().count()) + 60000u, p->GetGUID() });
                char const* what = id == SPELL_CLOUD_OF_DAGGERS ? "a whirl of spectral daggers" : id == SPELL_STINKING_CLOUD ?
                    "a cloud of yellow, nauseating gas" : id == SPELL_HUNGER_OF_HADAR ? "a sphere of freezing blackness" :
                    "freezing rain and sleet";
                Narrate(p, nullptr, Acore::StringFormat("{} {}: {} - whoever starts a turn in it suffers.", Who(p), SpellName(id), what));
                if (id == SPELL_CLOUD_OF_DAGGERS)
                {
                    int lvl = CastLevel(p, 2);
                    for (Unit* u : CreaturesNear(p, dest, r))
                    {
                        std::string detail;
                        DamageCtx d;
                        d.attacker = p;
                        d.victim = u;
                        d.amount = Dice{ 4 + 2 * (lvl - 2), 4, 0 }.Roll(false, detail);
                        d.type = DMG_SLASHING;
                        Narrate(p, u, Acore::StringFormat("   The daggers slash {}: {} = {} Slashing", Who(u), detail, d.amount));
                        ApplyDamage(d);
                    }
                }
                break;
            }
            case SPELL_CALL_LIGHTNING:
            {
                int lvl = CastLevel(p, 3);
                std::vector<Unit*> caught = CreaturesNear(p, dest, 1.5f);
                Narrate(p, nullptr, Acore::StringFormat("{} Call Lightning: a bolt from the storm cloud - {} caught. (Again next turn, no "
                    "slot, while you concentrate.)", Who(p), caught.size()));
                for (Unit* u : caught)
                    SaveForDamage(p, u, spell, DEX, Dice{ lvl, 10, 0 }, DMG_LIGHTNING, true, "Call Lightning");
                break;
            }
            case SPELL_JUMP:
            {
                float max = JumpDistance(p);
                float dist = p->GetExactDist2d(&dest);
                float go = std::min(max, dist);
                float ang = p->GetAngle(&dest);
                float x = p->GetPositionX() + std::cos(ang) * go;
                float y = p->GetPositionY() + std::sin(ang) * go;
                float z = dist <= max ? dest.GetPositionZ() : p->GetPositionZ();
                p->UpdateGroundPositionZ(x, y, z);
                if (Combatant* c = CombatantOf(p->GetGUID()))
                {
                    // jumping doesn't spend movement and doesn't provoke opportunity attacks (BG3 rules)
                    c->ignoreMoveUntil = GameTime::GetGameTimeMS().count() + 1100;   // the flight itself
                    c->lastPos.Relocate(x, y, z);
                    SetRooted(p, false);
                }
                p->GetMotionMaster()->MoveJump(x, y, z, 14.f, 7.f);
                Narrate(p, nullptr, Acore::StringFormat("{} jumps {:.1f} m (max {:.1f} m).", Who(p), go, max));
                break;
            }
            case SPELL_MISTY_STEP:
            {
                float ang = p->GetAngle(&dest);
                // as far as the way is clear (up to 18 m): a wall or a tree trunk stops the step, never through it
                Position to = p->GetFirstCollisionPosition(std::min(18.f, p->GetExactDist2d(&dest)), ang - p->GetOrientation());
                float x = to.GetPositionX(), y = to.GetPositionY(), z = to.GetPositionZ();
                float dist = p->GetExactDist2d(x, y);
                if (Combatant* c = CombatantOf(p->GetGUID()))
                {
                    c->ignoreMoveUntil = GameTime::GetGameTimeMS().count() + 1000;
                    c->lastPos.Relocate(x, y, z);
                }
                p->NearTeleportTo(x, y, z, ang);
                Narrate(p, nullptr, Acore::StringFormat("{} steps through the mist: {:.1f} m.", Who(p), dist));
                break;
            }
            case SPELL_SHADOW_STEP:
            {
                float ang = p->GetAngle(&dest);
                // as far as the way is clear (up to 18 m): a wall or a tree trunk stops the step, never through it
                Position to = p->GetFirstCollisionPosition(std::min(18.f, p->GetExactDist2d(&dest)), ang - p->GetOrientation());
                float x = to.GetPositionX(), y = to.GetPositionY(), z = to.GetPositionZ();
                float dist = p->GetExactDist2d(x, y);
                if (Combatant* c = CombatantOf(p->GetGUID()))
                {
                    c->ignoreMoveUntil = GameTime::GetGameTimeMS().count() + 1000;
                    c->lastPos.Relocate(x, y, z);
                    c->hidden = true;                // out of the shadows: the next attack has advantage
                }
                p->AddAura(SPELL_HIDDEN_AURA, p);
                p->NearTeleportTo(x, y, z, ang);
                Narrate(p, nullptr, Acore::StringFormat("{} steps through the shadows: {:.1f} m - the next attack has advantage.", Who(p), dist));
                break;
            }
            case SPELL_GREASE:
                if (Encounter* e = EncounterOf(p->GetGUID()))
                {
                    e->areas.push_back({ SPELL_GREASE, dest, 3.6f, uint32(GameTime::GetGameTimeMS().count() + 60000) });
                    Narrate(p, nullptr, "   The ground is covered in grease: difficult terrain (double movement cost) for 1 minute.");
                }
                break;
            default:
                break;
        }
    }

    void ResolveCast(Unit* caster, SpellInfo const* spell)
    {
        Player* p = caster->ToPlayer();
        if (!p)
            return;
        Combatant* c = CombatantOf(p->GetGUID());
        switch (spell->Id)
        {
            case SPELL_END_TURN:
                EndTurn(p);
                break;
            case SPELL_THUNDERWAVE:
            {
                // everything within 4.5 m: CON save, 2d8 thunder (+1d8 a slot level above 1st), half on a save; a failure is
                // thrown 3 m back
                int lvl = CastLevel(p, 1);
                std::vector<Unit*> caught;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    for (Combatant const& o : e->order)
                        if (!o.player && !o.gone && o.alive)
                            if (Unit* u = e->GetUnit(o); u && u->IsAlive() && p->GetExactDist(u) <= 4.5f + u->GetCombatReach())
                                caught.push_back(u);
                Narrate(p, nullptr, Acore::StringFormat("{} Thunderwave: a crack of thunder - {} caught.", Who(p), caught.size()));
                for (Unit* u : caught)
                    if (!SaveForDamage(p, u, spell, CON, Dice{ 1 + lvl, 8, 0 }, DMG_THUNDER, true, "Thunderwave") && u->IsAlive())
                    {
                        PushAway(p, u, 3.f);
                        Narrate(p, u, Acore::StringFormat("   {} is thrown 3 m back.", Who(u)));
                    }
                break;
            }
            case SPELL_SHILLELAGH:
                if (c)
                {
                    c->shillelagh = true;
                    Narrate(p, nullptr, Acore::StringFormat("{} wakes the wood: a staff or club strikes with Wisdom and a d8 this fight.",
                        Who(p)));
                }
                break;
            case SPELL_BLUR:
                Narrate(p, nullptr, Acore::StringFormat("{}'s outline blurs: attacks on them have disadvantage while they concentrate.", Who(p)));
                break;
            case SPELL_SPIRIT_GUARDIANS:
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    e->areas.push_back(Area{ SPELL_SPIRIT_GUARDIANS, p->GetPosition(), 4.5f,
                        uint32(GameTime::GetGameTimeMS().count()) + 60000u, p->GetGUID() });
                Narrate(p, nullptr, Acore::StringFormat("{} calls Spirit Guardians: within 4.5 m of them, a creature starting its turn saves "
                    "(WIS) against 3d8 Radiant, and moves at half speed.", Who(p)));
                break;
            case SPELL_MASS_HEALING_WORD:
            case SPELL_PRAYER_OF_HEALING:
            {
                bool mass = spell->Id == SPELL_MASS_HEALING_WORD;
                int lvl = mass ? CastLevel(p, 3) : CastLevel(p, 2);
                int mod = CastingMod(p) + HealBonus(p, lvl);
                std::vector<Player*> who;
                p->GetMap()->DoForAllPlayers([&](Player* o)
                {
                    if (o->IsAlive() && o->GetExactDist(p) <= (mass ? 18.f : 9.f))
                        who.push_back(o);
                });
                std::sort(who.begin(), who.end(), [](Player* x, Player* y)
                    { return x->GetHealthPct() < y->GetHealthPct(); });       // the most hurt first
                if (who.size() > 6)
                    who.resize(6);
                for (Player* o : who)
                {
                    std::string detail;
                    int amount = std::max(1, (mass ? Dice{ lvl - 2, 4, mod } : Dice{ lvl, 8, mod }).Roll(false, detail));
                    Narrate(p, o, Acore::StringFormat("   {} heals {}: {} = |cff80ff80{}|r", mass ? "Mass Healing Word" : "Prayer of Healing",
                        Who(o), detail, amount));
                    ApplyHeal(p, o, amount, spell);
                }
                break;
            }
            case SPELL_TURN_UNDEAD:
            {
                ++GetSheet(p).channelUsed;
                SaveSheet(p);
                PushSheet(p);
                int turned = 0;
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    for (Combatant& o : e->order)
                        if (!o.player && !o.gone && o.alive)
                            if (Unit* u = e->GetUnit(o); u && u->IsAlive() && u->ToCreature() &&
                                u->ToCreature()->GetCreatureType() == CREATURE_TYPE_UNDEAD && p->GetExactDist(u) <= 9.f)
                            {
                                if (SavingThrow(p, u, WIS, SpellSaveDC(p), true, "Turn Undead"))
                                    continue;
                                ++turned;
                                if (Level(p) >= 5 && StatsOrDefault(u->ToCreature()).xp <= 100)
                                {
                                    Narrate(p, u, Acore::StringFormat("   {} crumbles before the holy power: |cffffe080destroyed|r.", Who(u)));
                                    DamageCtx d;
                                    d.attacker = p;
                                    d.victim = u;
                                    d.amount = int(u->GetHealth());
                                    d.type = DMG_RADIANT;
                                    ApplyDamage(d);
                                    continue;
                                }
                                o.turned = true;
                                Narrate(p, u, Acore::StringFormat("   {} is |cffffe080turned|r: it cowers until something hurts it.", Who(u)));
                            }
                Narrate(p, nullptr, Acore::StringFormat("{} presents their holy symbol: Turn Undead ({} affected).", Who(p), turned));
                break;
            }
            case SPELL_PRESERVE_LIFE:
            {
                // 5 x cleric level of healing, shared out among allies within 9 m - never above half their health
                ++GetSheet(p).channelUsed;
                SaveSheet(p);
                PushSheet(p);
                int pool = 5 * Level(p);
                std::vector<Player*> who;
                p->GetMap()->DoForAllPlayers([&](Player* o)
                {
                    if (o->IsAlive() && o->GetExactDist(p) <= 9.f)
                        who.push_back(o);
                });
                std::sort(who.begin(), who.end(), [](Player* x, Player* y) { return x->GetHealthPct() < y->GetHealthPct(); });
                Narrate(p, nullptr, Acore::StringFormat("{} channels Preserve Life: {} hit points to share.", Who(p), pool));
                for (Player* o : who)
                {
                    int cap = int(o->GetMaxHealth() / 2) - (IsDowned(o) ? 0 : int(o->GetHealth()));
                    int give = std::min(pool, cap);
                    if (give <= 0)
                        continue;
                    pool -= give;
                    ApplyHeal(p, o, give, spell);
                    Narrate(p, o, Acore::StringFormat("   {} regains |cff80ff80{}|r.", Who(o), give));
                }
                break;
            }
            case SPELL_DESTRUCTIVE_WRATH:
                if (c)
                {
                    ++GetSheet(p).channelUsed;
                    SaveSheet(p);
                    PushSheet(p);
                    c->wrathArmed = true;
                    Narrate(p, nullptr, Acore::StringFormat("{} channels Destructive Wrath: their next lightning or thunder roll is at its "
                        "full fury.", Who(p)));
                }
                break;
            case SPELL_BARKSKIN:
                Narrate(p, nullptr, Acore::StringFormat("{}'s skin turns to bark: Armour Class at least 16 while they concentrate.", Who(p)));
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    PushOrder(*e);
                break;
            case SPELL_SHAPE_WOLF:
            case SPELL_SHAPE_BEAR:
            case SPELL_SHAPE_DIRE_WOLF:
            case SPELL_SHAPE_SABRETOOTH:
                StartWildShape(p, spell->Id);
                break;
            case SPELL_REVERT_SHAPE:
                EndWildShape(p, "");
                break;
            case SPELL_FLEE:
                Flee(p);
                break;
            case SPELL_STEP_OF_THE_WIND:
                ++GetSheet(p).kiUsed;
                SaveSheet(p);
                PushSheet(p);
                if (c)
                    c->stepJump = true;              // and jumps go twice as far this turn
                [[fallthrough]];
            case SPELL_DASH:
            case SPELL_CUNNING_DASH:
                if (c)
                {
                    c->moveMax += Speed(p);
                    c->dashed = true;
                    if (c->moved < c->moveMax)
                        SetRooted(p, false);
                    Narrate(p, nullptr, Acore::StringFormat("{} Dashes: +{:.1f} m movement.", Who(p), Speed(p)));
                    PushEconomy(p);
                }
                break;
            case SPELL_DISENGAGE:
            case SPELL_CUNNING_DISENGAGE:
                if (c)
                {
                    c->disengaged = true;
                    Narrate(p, nullptr, Acore::StringFormat("{} Disengages: no opportunity attacks this turn.", Who(p)));
                }
                break;
            case SPELL_FALSE_LIFE:
            {
                std::string detail;
                int t = Dice{ 1, 4, 4 }.Roll(false, detail);
                Sheet& s = GetSheet(p);
                s.tempHp = std::max(s.tempHp, t);
                Narrate(p, nullptr, Acore::StringFormat("{} casts False Life: {} = {} temporary HP.", Who(p), detail, t));
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_MAGE_ARMOR:
                Narrate(p, nullptr, Acore::StringFormat("{} casts Mage Armor: base AC 13 + DEX until long rest.", Who(p)));
                RaiseArcaneWard(p, 1);
                break;
            case SPELL_ENHANCED_LEAP:
                Narrate(p, nullptr, Acore::StringFormat("{} casts Enhanced Leap: jump distance tripled.", Who(p)));
                break;
            case SPELL_SHORT_REST:
                ShortRest(p);
                break;
            case SPELL_SECOND_WIND:
            {
                Sheet& s = GetSheet(p);
                ++s.secondWindUsed;
                std::string detail;
                int heal = Dice{ 1, 10, Level(p) }.Roll(false, detail);
                ApplyHeal(p, p, heal, spell);
                Narrate(p, nullptr, Acore::StringFormat("{} catches a Second Wind: {} = |cff80ff80{}|r hit points.", Who(p), detail, heal));
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_RAGE:
            {
                Sheet& s = GetSheet(p);
                ++s.rageUsed;
                if (c)
                    c->raging = true;
                p->AddAura(SPELL_RAGE_AURA, p);
                Narrate(p, nullptr, Acore::StringFormat("{} flies into a |cffff4020Rage|r: +2 melee damage, resistant to weapons, "
                    "advantage on Strength ({} left).", Who(p), std::max(0, RageMax(p) - s.rageUsed)));
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_RECKLESS_ATTACK:
                if (c)
                    c->reckless = true;
                Narrate(p, nullptr, Acore::StringFormat("{} attacks recklessly: advantage on melee attacks this turn - and against "
                    "them until their next.", Who(p)));
                break;
            case SPELL_HIDE:
            {
                if (!InCombat(p))
                {
                    if (IsSneaking(p))
                        StopSneaking(p, "You step out of the shadows.");
                    else
                        StartSneaking(p);
                    break;
                }
                std::string why;
                if (!TryHide(p, why) && !why.empty())
                    Error(p, why);
                break;
            }
            case SPELL_PATIENT_DEFENSE:
                ++GetSheet(p).kiUsed;
                if (c)
                    c->dodging = true;
                p->AddAura(SPELL_DODGE_AURA, p);
                Narrate(p, nullptr, Acore::StringFormat("{} settles into Patient Defense: attacks against them have disadvantage.", Who(p)));
                SaveSheet(p);
                PushSheet(p);
                break;
            case SPELL_WHOLENESS:
            {
                GetSheet(p).wholenessUsed = 1;
                int heal = 3 * Level(p);
                ApplyHeal(p, p, heal, spell);
                Narrate(p, nullptr, Acore::StringFormat("{} draws on Wholeness of Body: |cff80ff80{}|r hit points.", Who(p), heal));
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_CLOAK_OF_SHADOWS:
                GetSheet(p).kiUsed += 2;
                if (c)
                    c->hidden = true;
                p->AddAura(SPELL_HIDDEN_AURA, p);
                Narrate(p, nullptr, Acore::StringFormat("{} wraps themselves in shadow (2 ki): hidden.", Who(p)));
                SaveSheet(p);
                PushSheet(p);
                break;
            case SPELL_QUICKENED:
            case SPELL_TWINNED:
            case SPELL_HEIGHTENED:
            case SPELL_EMPOWERED:
            case SPELL_BEND_LUCK:
            {
                Sheet& s = GetSheet(p);
                char const* key = MetaKey(spell->Id);
                int cost = MetaCost(spell->Id);
                if (MetaArmed(p, key))
                {
                    SetMeta(p, key, false);          // cast again: called off, the points come back
                    s.spUsed = std::max(0, s.spUsed - cost);
                    Narrate(p, nullptr, Acore::StringFormat("{} lets the {} go.", Who(p), spell->SpellName[0]));
                }
                else
                {
                    SetMeta(p, key, true);
                    s.spUsed += cost;
                    Narrate(p, nullptr, Acore::StringFormat("|cffd080ff{}|r ({} sorcery points): it shapes your next spell.",
                        spell->SpellName[0], cost));
                }
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_ARMOR_OF_AGATHYS:
            {
                Sheet& s = GetSheet(p);
                int amount = 5 * CastLevel(p, 1);
                s.tempHp = std::max(s.tempHp, amount);
                s.agathys = amount;
                p->AddAura(SPELL_AGATHYS_AURA, p);
                Narrate(p, nullptr, Acore::StringFormat("{} is sheathed in spectral frost: {} temporary HP, and {} Cold to anything that "
                    "hits them in melee while it lasts.", Who(p), amount, amount));
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_HELLISH_REBUKE:
                if (c)
                    c->rebukeReady = true;
                p->AddAura(SPELL_REBUKE_AURA, p);
                Narrate(p, nullptr, Acore::StringFormat("{} readies a Hellish Rebuke: the next creature to hurt them burns (your reaction, "
                    "and a slot when it fires).", Who(p)));
                break;
            case SPELL_ARMS_OF_HADAR:
            {
                int lvl = CastLevel(p, 1);
                std::vector<Unit*> caught = FoesNear(p, 3.f);
                Narrate(p, nullptr, Acore::StringFormat("{} calls the Arms of Hadar: tendrils lash out 3 m - {} caught.", Who(p), caught.size()));
                sCurSpell = spell->Id;
                for (Unit* u : caught)
                    if (!SaveForDamage(p, u, spell, STR, Dice{ lvl + 1, 6, 0 }, DMG_NECROTIC, true, "Arms of Hadar") && u->IsAlive())
                        if (Combatant* uc = CombatantOf(u->GetGUID()))
                            uc->noReaction = true;   // a failed save: no reactions until its turn
                break;
            }
            case SPELL_FEY_PRESENCE:
            {
                GetSheet(p).patronUsed = 1;
                std::vector<Unit*> caught = FoesNear(p, 3.f);
                Narrate(p, nullptr, Acore::StringFormat("{} lets their Fey Presence show: {} within 3 m must face it.", Who(p), caught.size()));
                for (Unit* u : caught)
                    if (!SavingThrow(p, u, WIS, SpellSaveDC(p), true, "Fey Presence"))
                        if (Combatant* uc = CombatantOf(u->GetGUID()))
                        {
                            uc->frightened = true;
                            uc->frightenedBy = p->GetGUID();
                            Narrate(p, u, Acore::StringFormat("   {} is |cffc080ffFrightened|r until your next turn.", Who(u)));
                        }
                if (Encounter* e = EncounterOf(p->GetGUID()))
                    PushOrder(*e);
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_DARK_ONES_LUCK:
                GetSheet(p).patron6Used = 1;
                if (c)
                    c->luckArmed = true;
                Narrate(p, nullptr, Acore::StringFormat("{} calls on the Dark One's luck: their next saving throw gets +1d10.", Who(p)));
                SaveSheet(p);
                PushSheet(p);
                break;
            case SPELL_TIDES_OF_CHAOS:
                GetSheet(p).tidesUsed = 1;
                SetMeta(p, "tides", true);
                Narrate(p, nullptr, Acore::StringFormat("{} calls on the Tides of Chaos: the next attack roll has advantage.", Who(p)));
                SaveSheet(p);
                PushSheet(p);
                break;
            case SPELL_CREATE_SLOT:
            {
                // Font of Magic: sorcery points into the lowest spent slot they can pay for (2 / 3 / 5 for levels 1 / 2 / 3)
                Sheet& s = GetSheet(p);
                static int const price[4] = { 0, 2, 3, 5 };
                for (int l = 1; l <= 3; ++l)
                    if (s.slotsUsed[l] > 0 && SorceryMax(p) - s.spUsed >= price[l])
                    {
                        --s.slotsUsed[l];
                        s.spUsed += price[l];
                        Narrate(p, nullptr, Acore::StringFormat("{} shapes {} sorcery points into a level {} spell slot.", Who(p), price[l], l));
                        break;
                    }
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_CONVERT_SLOT:
            {
                // Font of Magic: the lowest unspent slot melts into sorcery points (its level)
                Sheet& s = GetSheet(p);
                for (int l = 1; l <= 3; ++l)
                    if (SlotsLeft(p, l) > 0)
                    {
                        ++s.slotsUsed[l];
                        s.spUsed = std::max(0, s.spUsed - l);
                        Narrate(p, nullptr, Acore::StringFormat("{} unravels a level {} slot into {} sorcery points.", Who(p), l, l));
                        break;
                    }
                SaveSheet(p);
                PushSheet(p);
                break;
            }
            case SPELL_ACTION_SURGE:
                if (c)
                {
                    ++GetSheet(p).actionSurgeUsed;
                    c->actionUsed = false;
                    c->extraAttacks = 0;
                    Narrate(p, nullptr, Acore::StringFormat("{} surges into |cffffd100Action|r: one more action this turn.", Who(p)));
                    SaveSheet(p);
                    PushSheet(p);
                    PushEconomy(p);
                }
                break;
            default:
                break;
        }
    }

    // ------------------------------------------------------------------ rests
    void LongRest(Player* p)
    {
        Sheet& s = GetSheet(p);
        s.slotsUsed.fill(0);
        s.shortRestsUsed = 0;
        s.arcaneRecoveryUsed = false;
        s.tempHp = 0;
        s.wardHp = 0;                        // the Arcane Ward fades; the next abjuration cast raises it anew
        s.wardMade = false;
        s.secondWindUsed = s.actionSurgeUsed = s.supUsed = 0;
        s.rageUsed = s.kiUsed = s.spUsed = s.tidesUsed = s.wholenessUsed = 0;
        s.patronUsed = s.patron6Used = s.agathys = 0;
        s.inspirationUsed = s.wildShapeUsed = s.naturalRecoveryUsed = s.channelUsed = s.wrathUsed = 0;
        EndWildShape(p, "");
        p->RemoveAurasDueToSpell(SPELL_AGATHYS_AURA);
        p->RemoveAurasDueToSpell(SPELL_MAGE_ARMOR);
        p->RemoveAurasDueToSpell(SPELL_FALSE_LIFE);
        p->RemoveAurasDueToSpell(SPELL_ENHANCED_LEAP);
        p->SetFullHealth();
        SaveSheet(p);
        Log(p, IsFighter(p) ? "|cff80ff80Long Rest: hit points, short rests and your fighter's resources restored.|r"
            : "|cff80ff80Long Rest: hit points, spell slots and short rests restored.|r");
        PushSheet(p);
    }

    void ShortRest(Player* p)
    {
        Sheet& s = GetSheet(p);
        if (s.shortRestsUsed >= 2)
        {
            Error(p, "No short rests left. Take a long rest (Hearthstone).");
            return;
        }
        ++s.shortRestsUsed;
        // Durable: a short rest brings you back to full
        int heal = HasFeat(p, "Durable") ? int(p->GetMaxHealth() - p->GetHealth()) : std::max<int>(1, p->GetMaxHealth() / 2);
        p->SetHealth(std::min<uint32>(p->GetMaxHealth(), p->GetHealth() + heal));
        std::string extra;
        if (s.cls == CLASS_WIZARD && !s.arcaneRecoveryUsed)
        {
            // Arcane Recovery: recover spell slot levels up to half the wizard level (rounded up)
            int budget = (Level(p) + 1) / 2;
            for (int lvl = std::min(5, budget); lvl >= 1 && budget > 0; --lvl)
                while (lvl <= budget && s.slotsUsed[lvl] > 0)
                {
                    --s.slotsUsed[lvl];
                    budget -= lvl;
                    s.arcaneRecoveryUsed = true;
                }
            if (s.arcaneRecoveryUsed)
                extra = " Arcane Recovery restores spell slots.";
        }
        if (s.cls == CLASS_FIGHTER && (s.secondWindUsed || s.actionSurgeUsed || s.supUsed))
        {
            s.secondWindUsed = s.actionSurgeUsed = s.supUsed = 0;
            extra += Level(p) >= 2 ? " Second Wind and Action Surge are ready again." : " Second Wind is ready again.";
            if (SuperiorityMax(p) > 0)
                extra += " Superiority dice restored.";
        }
        else if (s.supUsed)
        {
            s.supUsed = 0;                   // Martial Adept on a wizard
            extra += " Superiority dice restored.";
        }
        if (s.kiUsed)
        {
            s.kiUsed = 0;                    // a monk's ki
            extra += " Your ki returns.";
        }
        if (s.cls == CLASS_DND_WARLOCK)
        {
            s.slotsUsed.fill(0);             // Pact Magic comes back on a short rest
            s.patronUsed = s.patron6Used = 0;
            extra += " Your patron restores your Pact Magic.";
        }
        if (s.cls == CLASS_BARD && Level(p) >= 5 && s.inspirationUsed)
        {
            s.inspirationUsed = 0;           // Font of Inspiration
            extra += " Your Bardic Inspiration returns.";
        }
        if (s.cls == CLASS_CLERIC && s.channelUsed)
        {
            s.channelUsed = 0;
            extra += " Channel Divinity is ready again.";
        }
        if (s.cls == CLASS_DND_DRUID && s.wildShapeUsed)
        {
            s.wildShapeUsed = 0;
            extra += " Wild Shape is ready again.";
        }
        if (IsSubclass(p, "Land") && Level(p) >= 2 && !s.naturalRecoveryUsed)
        {
            // Natural Recovery: spell slot levels back up to half the druid level (rounded up), once a long rest
            int budget = (Level(p) + 1) / 2;
            for (int lvl = std::min(5, budget); lvl >= 1 && budget > 0; --lvl)
                while (lvl <= budget && s.slotsUsed[lvl] > 0)
                {
                    --s.slotsUsed[lvl];
                    budget -= lvl;
                    s.naturalRecoveryUsed = 1;
                }
            if (s.naturalRecoveryUsed)
                extra += " Natural Recovery restores spell slots.";
        }
        // Song of Rest: a bard (level 2+) in the party - this one or another within 30 m - adds 1d6
        {
            bool song = s.cls == CLASS_BARD && Level(p) >= 2;
            p->GetMap()->DoForAllPlayers([&](Player* o)
            {
                if (o != p && IsClass(o, CLASS_BARD) && Level(o) >= 2 && o->GetExactDist(p) <= 30.f)
                    song = true;
            });
            if (song)
            {
                int tune = D(6);
                p->SetHealth(std::min<uint32>(p->GetMaxHealth(), p->GetHealth() + tune));
                heal += tune;
                extra += Acore::StringFormat(" Song of Rest: +{}.", tune);
            }
        }
        if (IsSubclass(p, "Abjuration") && Level(p) >= 6 && s.wardMade)
        {
            s.wardHp = 2 * Level(p) + Mod(s.abil[INT]);      // Warded Rest
            extra += " Your Arcane Ward is restored.";
        }
        SaveSheet(p);
        PushSheet(p);
        Log(p, Acore::StringFormat("|cff80ff80Short Rest ({}/2): healed {} HP.{}|r", s.shortRestsUsed, heal, extra));
        PushSheet(p);
    }

    // ------------------------------------------------------------------ new characters
    static bool AllowedSpell(uint32 id)
    {
        if (GetAction(id))
            return true;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info)
            return true;
        // class spells (any SpellFamily) go; skills, languages, professions and passives without a family stay
        if (info->SpellFamilyName != SPELLFAMILY_GENERIC)
            return false;
        static uint32 const racials[] = { 20589, 20549, 7744, 20594, 58984, 59752, 28880, 59542, 59543, 59544, 59545, 59547, 59548,
            26297, 20572, 33697, 33702, 28730, 25046, 50613, 20577, 20555, 20557, 20550, 20551, 20552, 20591, 20592, 20593, 20596,
            20579, 21009, 20582, 20583, 20585, 20573, 20574, 6562, 28875, 5227, 7744, 20598, 20599, 20864, 58943, 822, 20599, 20600,
            2481, 20595, 59224, 20597, 58985, 3018, 2764, 5019, 75 };
        for (uint32 r : racials)
            if (r == id)
                return false;
        return true;
    }

    // a spell learned on level-up goes on the first free button of the lower bars (bottom left, then bottom right)
    static void PutOnBar(Player* p, uint32 spellId)
    {
        for (uint8 b = 0; b < MAX_ACTION_BUTTONS; ++b)
            if (ActionButton const* ab = p->GetActionButton(b))
                if (ab->GetType() == ACTION_BUTTON_SPELL && ab->GetAction() == spellId)
                    return;
        for (uint8 first : { 60, 72 })
            for (uint8 b = first; b < first + 12; ++b)
                if (!p->GetActionButton(b))
                {
                    p->addActionButton(b, spellId, ACTION_BUTTON_SPELL);
                    p->SendActionButtons(1);
                    return;
                }
    }

    // every class action, and whether this character has it now (by class, level, subclass and picks)
    std::vector<uint32> const& ClassActions()
    {
        static std::vector<uint32> const v = { SPELL_RANGED_ATTACK, SPELL_SECOND_WIND, SPELL_ACTION_SURGE, SPELL_TRIP_ATTACK,
            SPELL_PUSHING_ATTACK, SPELL_MENACING_ATTACK, SPELL_RAGE, SPELL_RECKLESS_ATTACK, SPELL_FRENZIED_STRIKE, SPELL_HIDE,
            SPELL_CUNNING_DASH, SPELL_CUNNING_DISENGAGE, SPELL_UNARMED_STRIKE, SPELL_FLURRY, SPELL_PATIENT_DEFENSE,
            SPELL_STEP_OF_THE_WIND, SPELL_STUNNING_STRIKE, SPELL_WHOLENESS, SPELL_CLOAK_OF_SHADOWS, SPELL_SHADOW_STEP,
            SPELL_QUICKENED, SPELL_TWINNED, SPELL_HEIGHTENED, SPELL_EMPOWERED, SPELL_CREATE_SLOT, SPELL_CONVERT_SLOT,
            SPELL_TIDES_OF_CHAOS, SPELL_BEND_LUCK, SPELL_ELDRITCH_BLAST, SPELL_HEX, SPELL_ARMOR_OF_AGATHYS, SPELL_HELLISH_REBUKE,
            SPELL_ARMS_OF_HADAR, SPELL_DISSONANT_WHISPERS, SPELL_FAERIE_FIRE, SPELL_VAMPIRIC_TOUCH, SPELL_FEY_PRESENCE,
            SPELL_DARK_ONES_LUCK, SPELL_OFFHAND_ATTACK, SPELL_VICIOUS_MOCKERY, SPELL_BARDIC_INSPIRATION, SPELL_HEALING_WORD,
            SPELL_CURE_WOUNDS, SPELL_THUNDERWAVE, SPELL_HYPNOTIC_PATTERN, SPELL_PRODUCE_FLAME, SPELL_SHILLELAGH, SPELL_ENTANGLE,
            SPELL_MOONBEAM, SPELL_CALL_LIGHTNING, SPELL_BARKSKIN, SPELL_SHAPE_WOLF, SPELL_SHAPE_BEAR, SPELL_SHAPE_DIRE_WOLF,
            SPELL_SHAPE_SABRETOOTH, SPELL_REVERT_SHAPE, SPELL_CHILL_TOUCH, SPELL_ACID_SPLASH, SPELL_POISON_SPRAY,
            SPELL_RAY_OF_SICKNESS, SPELL_WITCH_BOLT, SPELL_SLEEP, SPELL_HEROISM, SPELL_CLOUD_OF_DAGGERS, SPELL_WEB, SPELL_BLUR,
            SPELL_FEAR, SPELL_STINKING_CLOUD, SPELL_HUNGER_OF_HADAR, SPELL_SLEET_STORM, SPELL_SACRED_FLAME, SPELL_TOLL_THE_DEAD,
            SPELL_BLESS, SPELL_GUIDING_BOLT, SPELL_INFLICT_WOUNDS, SPELL_SHIELD_OF_FAITH, SPELL_AID, SPELL_LESSER_RESTORATION,
            SPELL_PRAYER_OF_HEALING, SPELL_SPIRIT_GUARDIANS, SPELL_MASS_HEALING_WORD, SPELL_TURN_UNDEAD, SPELL_PRESERVE_LIFE,
            SPELL_DESTRUCTIVE_WRATH };
        return v;
    }

    bool KitHas(Player* p, uint32 id)
    {
        Sheet& s = GetSheet(p);
        int lvl = Level(p);
        bool barb = s.cls == CLASS_BARBARIAN, rogue = s.cls == CLASS_DND_ROGUE, monk = s.cls == CLASS_MONK, sorc = s.cls == CLASS_SORCERER;
        bool lock = s.cls == CLASS_DND_WARLOCK, bard = s.cls == CLASS_BARD, druid = s.cls == CLASS_DND_DRUID;
        bool arcane = s.cls == CLASS_WIZARD || s.cls == CLASS_SORCERER;
        bool cleric = s.cls == CLASS_CLERIC, tempest = cleric && IsSubclass(p, "Tempest");
        auto slot = [p](int l) { return SlotsMax(p, l) > 0; };
        switch (id)
        {
            case SPELL_VICIOUS_MOCKERY:
            case SPELL_BARDIC_INSPIRATION:  return bard;
            case SPELL_HEALING_WORD:
            case SPELL_CURE_WOUNDS:         return bard || druid || cleric;
            case SPELL_THUNDERWAVE:         return bard || druid || arcane || tempest;
            case SPELL_SACRED_FLAME:
            case SPELL_TOLL_THE_DEAD:
            case SPELL_BLESS:
            case SPELL_GUIDING_BOLT:
            case SPELL_INFLICT_WOUNDS:
            case SPELL_SHIELD_OF_FAITH:     return cleric;
            case SPELL_AID:
            case SPELL_LESSER_RESTORATION:
            case SPELL_PRAYER_OF_HEALING:   return cleric && slot(2);
            case SPELL_SPIRIT_GUARDIANS:
            case SPELL_MASS_HEALING_WORD:   return cleric && slot(3);
            case SPELL_TURN_UNDEAD:         return cleric && lvl >= 2;
            case SPELL_PRESERVE_LIFE:       return cleric && lvl >= 2 && IsSubclass(p, "Life");
            case SPELL_DESTRUCTIVE_WRATH:   return tempest && lvl >= 2;
            case SPELL_HYPNOTIC_PATTERN:    return (bard || arcane) && slot(3);
            case SPELL_CHILL_TOUCH:         return arcane || lock;
            case SPELL_ACID_SPLASH:         return arcane;
            case SPELL_POISON_SPRAY:        return arcane || lock || druid;
            case SPELL_RAY_OF_SICKNESS:     return arcane;
            case SPELL_WITCH_BOLT:          return arcane || lock;
            case SPELL_SLEEP:               return arcane || bard;
            case SPELL_HEROISM:             return bard;
            case SPELL_CLOUD_OF_DAGGERS:    return ((arcane || bard) && slot(2)) || (lock && PactLevel(p) >= 2);
            case SPELL_WEB:
            case SPELL_BLUR:                return arcane && slot(2);
            case SPELL_FEAR:                return ((arcane || bard) && slot(3)) || (lock && PactLevel(p) >= 3);
            case SPELL_STINKING_CLOUD:      return (arcane || bard) && slot(3);
            case SPELL_HUNGER_OF_HADAR:     return lock && PactLevel(p) >= 3;
            case SPELL_SLEET_STORM:         return (arcane || druid || tempest) && slot(3);
            case SPELL_PRODUCE_FLAME:
            case SPELL_SHILLELAGH:
            case SPELL_ENTANGLE:            return druid;
            case SPELL_MOONBEAM:            return druid && slot(2);
            case SPELL_CALL_LIGHTNING:      return (druid || tempest) && slot(3);
            case SPELL_BARKSKIN:            return druid && lvl >= 3 && IsSubclass(p, "Land");
            case SPELL_SHAPE_WOLF:
            case SPELL_REVERT_SHAPE:        return druid && lvl >= 2;
            case SPELL_SHAPE_BEAR:
            case SPELL_SHAPE_DIRE_WOLF:     return druid && lvl >= 2 && IsSubclass(p, "Moon");
            case SPELL_SHAPE_SABRETOOTH:    return druid && lvl >= 6 && IsSubclass(p, "Moon");
            case SPELL_RANGED_ATTACK:       return !IsCaster(p);
            case SPELL_OFFHAND_ATTACK:      return !IsCaster(p) || HasPact(p, "Blade");
            case SPELL_SECOND_WIND:         return s.cls == CLASS_FIGHTER;
            case SPELL_ACTION_SURGE:        return s.cls == CLASS_FIGHTER && lvl >= 2;
            case SPELL_TRIP_ATTACK:
            case SPELL_PUSHING_ATTACK:
            case SPELL_MENACING_ATTACK:     return SuperiorityMax(p) > 0;
            case SPELL_RAGE:                return barb;
            case SPELL_RECKLESS_ATTACK:     return barb && lvl >= 2;
            case SPELL_FRENZIED_STRIKE:     return barb && lvl >= 3 && IsSubclass(p, "Berserker");
            case SPELL_HIDE:                return rogue;
            case SPELL_CUNNING_DASH:
            case SPELL_CUNNING_DISENGAGE:   return rogue && lvl >= 2;
            case SPELL_UNARMED_STRIKE:      return monk;
            case SPELL_FLURRY:
            case SPELL_PATIENT_DEFENSE:
            case SPELL_STEP_OF_THE_WIND:    return monk && lvl >= 2;
            case SPELL_STUNNING_STRIKE:     return monk && lvl >= 5;
            case SPELL_CLOAK_OF_SHADOWS:    return monk && lvl >= 3 && IsSubclass(p, "Shadow");
            case SPELL_SHADOW_STEP:         return monk && lvl >= 6 && IsSubclass(p, "Shadow");
            case SPELL_WHOLENESS:           return monk && lvl >= 6 && IsSubclass(p, "Open Hand");
            case SPELL_QUICKENED:           return sorc && HasMetamagic(p, "Quickened");
            case SPELL_TWINNED:             return sorc && HasMetamagic(p, "Twinned");
            case SPELL_HEIGHTENED:          return sorc && HasMetamagic(p, "Heightened");
            case SPELL_EMPOWERED:           return sorc && HasMetamagic(p, "Empowered");
            case SPELL_CREATE_SLOT:
            case SPELL_CONVERT_SLOT:        return sorc && lvl >= 2;
            case SPELL_TIDES_OF_CHAOS:      return sorc && IsSubclass(p, "Wild Magic");
            case SPELL_BEND_LUCK:           return sorc && lvl >= 6 && IsSubclass(p, "Wild Magic");
            case SPELL_ELDRITCH_BLAST:
            case SPELL_HEX:
            case SPELL_ARMOR_OF_AGATHYS:
            case SPELL_HELLISH_REBUKE:
            case SPELL_ARMS_OF_HADAR:       return lock;
            case SPELL_VAMPIRIC_TOUCH:      return lock && PactLevel(p) >= 3;
            case SPELL_DISSONANT_WHISPERS:  return (lock && IsSubclass(p, "Great Old One")) || bard;
            case SPELL_FAERIE_FIRE:         return (lock && IsSubclass(p, "Archfey")) || bard || druid;
            case SPELL_FEY_PRESENCE:        return lock && IsSubclass(p, "Archfey");
            case SPELL_DARK_ONES_LUCK:      return lock && lvl >= 6 && IsSubclass(p, "Fiend");
            default:                        return false;
        }
    }

    // the casters' spells: a wizard knows them all, a sorcerer all but Grease (not on the sorcerer's list); leveled
    // spells once there's a slot for them
    static bool SpellKnown(Player* p, uint32 id)
    {
        if (IsClass(p, CLASS_DND_WARLOCK))
        {
            // a warlock's borrowings: the Tome's cantrips, the Fiend's fire, the invocations' at-will spells, and the
            // 2nd-level spells on the warlock list once the pact slots reach 2nd level
            int pl = PactLevel(p);
            switch (id)
            {
                case SPELL_FIRE_BOLT: case SPELL_RAY_OF_FROST: case SPELL_SHOCKING_GRASP: return HasPact(p, "Tome");
                case SPELL_BURNING_HANDS:   return IsSubclass(p, "Fiend");
                case SPELL_SCORCHING_RAY:   return IsSubclass(p, "Fiend") && pl >= 2;
                case SPELL_FIREBALL:        return IsSubclass(p, "Fiend") && pl >= 3;
                case SPELL_MAGE_ARMOR:      return HasInvocation(p, "Armor of Shadows");
                case SPELL_FALSE_LIFE:      return HasInvocation(p, "Fiendish Vigour");
                case SPELL_MISTY_STEP: case SPELL_SHATTER: case SPELL_HOLD_PERSON: return pl >= 2;
                default:                    return false;
            }
        }
        if (IsClass(p, CLASS_BARD))
            switch (id)
            {
                case SPELL_SHATTER: case SPELL_HOLD_PERSON: return SlotsMax(p, 2) > 0;
                case SPELL_FIREBALL: case SPELL_MISTY_STEP: return Level(p) >= 6 && IsSubclass(p, "Lore");   // Magical Secrets
                default: return false;
            }
        if (IsClass(p, CLASS_DND_DRUID))
            return (id == SPELL_HOLD_PERSON && SlotsMax(p, 2) > 0) || id == SPELL_ICE_KNIFE;
        if (IsClass(p, CLASS_CLERIC))
            return (id == SPELL_HOLD_PERSON && SlotsMax(p, 2) > 0) ||
                (id == SPELL_SHATTER && SlotsMax(p, 2) > 0 && IsSubclass(p, "Tempest"));   // a Tempest domain spell
        if (!IsCaster(p) || (IsClass(p, CLASS_SORCERER) && id == SPELL_GREASE))
            return false;
        ActionInfo const* a = GetAction(id);
        return !a || a->slotLevel < 1 || SlotsMax(p, a->slotLevel) > 0;
    }

    void EnsureKit(Player* p)
    {
        std::vector<uint32> remove;
        for (auto const& [id, spell] : p->GetSpellMap())
            if (spell->State != PLAYERSPELL_REMOVED && !AllowedSpell(id))
                remove.push_back(id);
        for (uint32 id : remove)
            p->removeSpell(id, SPEC_MASK_ALL, false);
        for (uint32 id : CommonActions())
            if (!p->HasSpell(id))
            {
                p->learnSpell(id);
                if (id == SPELL_HELP || id == SPELL_FLEE || id == SPELL_THROW)
                    PutOnBar(p, id);             // (added after characters were made: theirs too)
            }
        // WoW's weapon and armour skills, for everyone: who may wear and wield what is the D&D engine's business
        for (uint32 skill : { 196u, 197u, 198u, 199u, 200u, 201u, 202u, 227u, 1180u, 15590u, 264u, 5011u, 266u, 2567u, 9077u,
                              8737u, 750u, 9116u, 674u })
            if (!p->HasSpell(skill))
                p->learnSpell(skill);
        auto keep = [p](uint32 id, bool has)
        {
            if (has && !p->HasSpell(id))
            {
                p->learnSpell(id);
                PutOnBar(p, id);
            }
            else if (!has && p->HasSpell(id))
                p->removeSpell(id, SPEC_MASK_ALL, false);
        };
        for (uint32 id : WizardKit())
            keep(id, SpellKnown(p, id));
        for (uint32 id : ClassActions())
            keep(id, KitHas(p, id));
        if (!p->HasItemCount(6948, 1, true))
            p->StoreNewItemInBestSlots(6948, 1);
    }

    static void EquipItem(Player* p, uint8 slot, uint32 item)
    {
        if (p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        p->EquipNewItem((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, item, true);
    }

    // each class's kit: what they wear and carry, and their first bar
    void SetupNewCharacter(Player* p)
    {
        EnsureKit(p);
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)   // the stock starter food goes
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (item->GetTemplate()->Class == ITEM_CLASS_CONSUMABLE)
                    p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        uint8 cls = GetSheet(p).cls;
        std::vector<uint32> bar1, bar2;
        std::string welcome;
        switch (cls)
        {
            case CLASS_FIGHTER:
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 285);                         // scale mail
                for (uint32 item : { 2488u, 2362u, 15807u })                      // longsword, shield, light crossbow
                    if (!p->HasItemCount(item, 1, true))
                        p->StoreNewItemInBestSlots(item, 1);
                bar1 = { SPELL_ATTACK, SPELL_RANGED_ATTACK, SPELL_SECOND_WIND, SPELL_SHOVE, SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_HELP };
                welcome = "Welcome, Fighter. Your actions are on your bar; a longsword and shield are in your pack if you'd "
                    "rather fight sword-and-board. Choose a Fighting Style to begin.";
                break;
            case CLASS_BARBARIAN:
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 12282);                     // greataxe
                EquipItem(p, EQUIPMENT_SLOT_RANGED, 15807);                       // light crossbow
                p->StoreNewItemInBestSlots(2134, 1);                              // a spare handaxe
                bar1 = { SPELL_ATTACK, SPELL_RAGE, SPELL_RANGED_ATTACK, SPELL_SHOVE, SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_HELP };
                welcome = "Welcome, Barbarian. Rage is your Bonus Action: hit harder, shrug off blows. No armour needed - your "
                    "hide is Constitution.";
                break;
            case CLASS_DND_ROGUE:
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 799);                          // leather
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 2139);                      // dagger
                EquipItem(p, EQUIPMENT_SLOT_OFFHAND, 2139);                       // a second dagger: the off-hand strike
                EquipItem(p, EQUIPMENT_SLOT_RANGED, 2504);                        // shortbow
                bar1 = { SPELL_ATTACK, SPELL_OFFHAND_ATTACK, SPELL_RANGED_ATTACK, SPELL_HIDE, SPELL_SHOVE, SPELL_DASH, SPELL_DISENGAGE,
                    SPELL_JUMP, SPELL_HELP };
                welcome = "Welcome, Rogue. Strike with advantage - hidden, or at a foe who's down - and Sneak Attack adds its "
                    "dice. A dagger or a bow; never a big axe.";
                break;
            case CLASS_MONK:
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 35);                        // quarterstaff
                EquipItem(p, EQUIPMENT_SLOT_RANGED, 2504);                        // shortbow
                bar1 = { SPELL_ATTACK, SPELL_UNARMED_STRIKE, SPELL_RANGED_ATTACK, SPELL_SHOVE, SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_HELP };
                welcome = "Welcome, Monk. Attack, then strike again with your fists as a Bonus Action (Martial Arts). No armour: "
                    "your Wisdom guards you.";
                break;
            case CLASS_SORCERER:
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 2139);                      // dagger
                bar1 = { SPELL_ATTACK, SPELL_FIRE_BOLT, SPELL_RAY_OF_FROST, SPELL_SHOCKING_GRASP, SPELL_MAGIC_MISSILE,
                    SPELL_CHROMATIC_FIRE, SPELL_BURNING_HANDS, SPELL_ICE_KNIFE, SPELL_MAGE_ARMOR, SPELL_FALSE_LIFE };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_CHROMATIC_COLD, SPELL_CHROMATIC_LIGHT,
                    SPELL_ENHANCED_LEAP, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Sorcerer. Magic is in your blood - Charisma drives it. Choose your bloodline to begin.";
                break;
            case CLASS_DND_WARLOCK:
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 799);                          // leather
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 2139);                      // dagger
                bar1 = { SPELL_ATTACK, SPELL_ELDRITCH_BLAST, SPELL_HEX, SPELL_ARMOR_OF_AGATHYS, SPELL_ARMS_OF_HADAR,
                    SPELL_HELLISH_REBUKE };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Warlock. Your patron gave you Eldritch Blast and a little Pact Magic - few slots, but a short rest "
                    "brings them back. Choose your patron to begin.";
                break;
            case CLASS_BARD:
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 799);                          // leather
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 3455);                      // shortsword (finesse)
                bar1 = { SPELL_ATTACK, SPELL_VICIOUS_MOCKERY, SPELL_BARDIC_INSPIRATION, SPELL_HEALING_WORD, SPELL_CURE_WOUNDS,
                    SPELL_DISSONANT_WHISPERS, SPELL_FAERIE_FIRE, SPELL_THUNDERWAVE };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Bard. Inspire your friends (a Bonus Action: a die for their next roll that would fall short), "
                    "mock your foes, heal the fallen. Charisma drives your magic.";
                break;
            case CLASS_CLERIC:
                // (the WoW class's own starting clothes and weapon go - stock armour's numbers aren't D&D's)
                for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                    if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                        if (item->GetTemplate()->Class == ITEM_CLASS_ARMOR || item->GetTemplate()->Class == ITEM_CLASS_WEAPON)
                            p->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 285);                          // scale mail
                EquipItem(p, EQUIPMENT_SLOT_OFFHAND, 2362);                       // shield
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 2130);                      // mace
                bar1 = { SPELL_ATTACK, SPELL_SACRED_FLAME, SPELL_TOLL_THE_DEAD, SPELL_BLESS, SPELL_CURE_WOUNDS, SPELL_HEALING_WORD,
                    SPELL_GUIDING_BOLT, SPELL_INFLICT_WOUNDS, SPELL_SHIELD_OF_FAITH };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Cleric. Your god answers through you: heal the fallen, bless your friends, burn your foes. Wisdom "
                    "drives your magic. Choose your domain to begin.";
                break;
            case CLASS_DND_DRUID:
                EquipItem(p, EQUIPMENT_SLOT_CHEST, 799);                          // leather
                EquipItem(p, EQUIPMENT_SLOT_MAINHAND, 35);                        // quarterstaff
                bar1 = { SPELL_ATTACK, SPELL_PRODUCE_FLAME, SPELL_SHILLELAGH, SPELL_ENTANGLE, SPELL_HEALING_WORD, SPELL_CURE_WOUNDS,
                    SPELL_THUNDERWAVE, SPELL_FAERIE_FIRE };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Druid. Fire from your hand, vines from the ground, healing for your friends - and from level 2, "
                    "Wild Shape. Wisdom drives your magic.";
                break;
            default:
                bar1 = { SPELL_ATTACK, SPELL_FIRE_BOLT, SPELL_RAY_OF_FROST, SPELL_SHOCKING_GRASP, SPELL_MAGIC_MISSILE,
                    SPELL_CHROMATIC_FIRE, SPELL_BURNING_HANDS, SPELL_ICE_KNIFE, SPELL_GREASE, SPELL_MAGE_ARMOR, SPELL_FALSE_LIFE };
                bar2 = { SPELL_DASH, SPELL_DISENGAGE, SPELL_JUMP, SPELL_SHOVE, SPELL_CHROMATIC_COLD, SPELL_CHROMATIC_LIGHT,
                    SPELL_ENHANCED_LEAP, SPELL_SHORT_REST, SPELL_HELP };
                welcome = "Welcome, Wizard. Your spells and actions are on your bars; the dice log is the window on the right.";
                break;
        }
        if (bar2.empty())
            bar1.resize(10, 0), bar1.push_back(SPELL_SHORT_REST);
        for (uint8 i = 0; i < 12; ++i)
        {
            p->removeActionButton(i);
            uint32 id = i == 11 ? SPELL_END_TURN : (i < bar1.size() ? bar1[i] : 0);
            if (id)
                p->addActionButton(i, id, ACTION_BUTTON_SPELL);
            p->removeActionButton(60 + i);
            if (i < bar2.size() && bar2[i])
                p->addActionButton(60 + i, bar2[i], ACTION_BUTTON_SPELL);
            p->removeActionButton(72 + i);     // (the kit's auto-placement may have used it; level-ups fill it later)
        }
        PutOnBar(p, SPELL_FLEE);                 // (wherever there's room, every class)
        p->UpdateMaxHealth();
        p->SetFullHealth();
        SaveSheet(p);
        Log(p, "|cffffd100" + welcome + "|r");
        PushPendingChoices(p);
    }

    // ------------------------------------------------------------------ the warlock's answers to damage (ApplyDamage calls this)
    void AfterDamage(DamageCtx const& d, int amount, int absorbed)
    {
        static int depth = 0;                // the answers deal damage too: one level of reply, no ping-pong
        if (depth > 0 || !d.victim)
            return;
        struct Guard { Guard() { ++depth; } ~Guard() { --depth; } } guard;
        Player* by = d.attacker ? d.attacker->ToPlayer() : nullptr;
        // Wrath of the Storm (Tempest): a creature that hits the cleric from close by takes the storm back - automatic, the
        // cleric's reaction, WIS-modifier times a long rest
        if (Player* tp = d.victim->ToPlayer(); tp && amount + absorbed > 0 && d.attacker && d.attacker->ToCreature() &&
            d.attacker->IsAlive() && d.attacker->GetExactDist(tp) <= MeleeRange(d.attacker, tp) + 0.5f &&
            GetSheet(tp).wrathUsed < WrathMax(tp))
            if (Combatant* tc = CombatantOf(tp->GetGUID()); tc && !tc->reactionUsed && !tc->downed)
            {
                tc->reactionUsed = true;
                ++GetSheet(tp).wrathUsed;
                SaveSheet(tp);
                Narrate(tp, d.attacker, Acore::StringFormat("{} answers with the Wrath of the Storm!", Who(tp)));
                SaveForDamage(tp, d.attacker, nullptr, DEX, Dice{ 2, 8, 0 }, DMG_LIGHTNING, true, "Wrath of the Storm");
            }
        // Thunderbolt Strike (Tempest 6): lightning from the cleric throws a creature 3 m back
        if (by && d.type == DMG_LIGHTNING && amount > 0 && IsSubclass(by, "Tempest") && Level(by) >= 6 && d.victim->ToCreature() &&
            d.victim->IsAlive())
            PushAway(by, d.victim, 3.f);
        if (!d.victim->IsAlive() && d.victim->ToCreature())
        {
            // Dark One's Blessing (Fiend): a kill gives temporary hit points
            if (by && IsClass(by, CLASS_DND_WARLOCK) && IsSubclass(by, "Fiend"))
            {
                Sheet& s = GetSheet(by);
                int t = std::max(1, AbilityMod(by, CHA) + Level(by));
                if (t > s.tempHp)
                {
                    s.tempHp = t;
                    Narrate(by, nullptr, Acore::StringFormat("   |cffff6020Dark One's Blessing|r: {} temporary HP.", t));
                    SaveSheet(by);
                    PushSheet(by);
                }
            }
            // Hex: the cursed creature died - the warlock may move it without a slot
            if (Combatant* vc = CombatantOf(d.victim->GetGUID()); vc && !vc->hexedBy.IsEmpty())
            {
                if (Combatant* wc = CombatantOf(vc->hexedBy))
                {
                    wc->hexFree = true;
                    if (Player* w = ObjectAccessor::FindPlayer(vc->hexedBy))
                        Log(w, "|cffa040ffHex|r: your cursed foe is dead - cast Hex again (Bonus Action) to move it, no slot needed.");
                }
                vc->hexedBy.Clear();
            }
            return;
        }
        Player* p = d.victim->ToPlayer();
        Creature* foe = d.attacker ? d.attacker->ToCreature() : nullptr;
        if (!p || !foe || !IsClass(p, CLASS_DND_WARLOCK) || amount + absorbed <= 0)
            return;
        Sheet& s = GetSheet(p);
        // Armor of Agathys: a melee hit while its temporary hit points last costs the attacker cold
        if (s.agathys > 0 && d.melee && absorbed > 0 && foe->IsAlive())
        {
            Narrate(p, foe, Acore::StringFormat("   The frost of Agathys bites back: |cffffffff{}|r Cold.", s.agathys));
            DamageCtx cold;
            cold.attacker = p;
            cold.victim = foe;
            cold.amount = s.agathys;
            cold.type = DMG_COLD;
            ApplyDamage(cold);
        }
        if (s.agathys > 0 && s.tempHp <= 0)
        {
            s.agathys = 0;                       // the armour is gone with its hit points
            p->RemoveAurasDueToSpell(SPELL_AGATHYS_AURA);
            SaveSheet(p);
        }
        Combatant* me = CombatantOf(p->GetGUID());
        if (!me || me->reactionUsed)
            return;
        // Hellish Rebuke, readied: the attacker makes a DEX save against (pact level + 1)d10 Fire, half on a success
        int pl = PactLevel(p);
        if (me->rebukeReady && foe->IsAlive() && SlotsLeft(p, pl) > 0)
        {
            me->rebukeReady = false;
            me->reactionUsed = true;
            ++s.slotsUsed[pl];
            p->RemoveAurasDueToSpell(SPELL_REBUKE_AURA);
            SaveSheet(p);
            PushSheet(p);
            Narrate(p, foe, Acore::StringFormat("{} answers with a |cffff6020Hellish Rebuke|r!", Who(p)));
            SaveForDamage(p, foe, sSpellMgr->GetSpellInfo(SPELL_HELLISH_REBUKE), DEX, Dice{ pl + 1, 10, 0 }, DMG_FIRE, true,
                "Hellish Rebuke");
            return;
        }
        // Misty Escape (Archfey 6): hurt, the warlock vanishes - hidden - once a short rest
        if (p->IsAlive() && Level(p) >= 6 && IsSubclass(p, "Archfey") && !s.patron6Used && amount > 0)
        {
            s.patron6Used = 1;
            me->reactionUsed = true;
            me->hidden = true;
            p->AddAura(SPELL_HIDDEN_AURA, p);
            SaveSheet(p);
            PushSheet(p);
            Narrate(p, foe, Acore::StringFormat("{} fades into the Feywild's mist (|cffc0a0ffMisty Escape|r): hidden.", Who(p)));
        }
    }
}

using namespace Dnd;

// One script for every mod-dnd spell (spell_script_names maps all of them here).
class spell_dnd_generic : public SpellScript
{
    PrepareSpellScript(spell_dnd_generic);

    bool fromScroll = false;                 // read from a scroll: no slot spent (the scroll is gone by AfterCast)
    int slotBefore = -1;                     // the slot this cast costs, decided before it takes up concentration

    SpellCastResult CheckCast()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!p)
            return SPELL_CAST_OK;
        ActionInfo const* a = GetAction(GetSpellInfo()->Id);
        if (!a)
            return SPELL_CAST_OK;
        bool fighting = InCombat(p);
        std::string why;
        uint32 id = GetSpellInfo()->Id;
        if (IsDowned(p))
        {
            Error(p, "You're down - you can't act. Death saves come on your turn.");
            return SPELL_FAILED_DONT_REPORT;
        }
        {
            std::string bdWhy;
            Unit* bt = GetExplTargetUnit();
            if (WildShapeOf(p) && IsSpellAction(id))
                bdWhy = "You can't cast spells in beast form. Revert first (a Bonus Action).";
            else if (id == SPELL_BARDIC_INSPIRATION && GetSheet(p).inspirationUsed >= InspirationMax(p))
                bdWhy = Level(p) >= 5 ? "No Bardic Inspiration left. A short rest restores it."
                    : "No Bardic Inspiration left. A long rest restores it.";
            else if (id == SPELL_BARDIC_INSPIRATION && (!bt || bt == p || !bt->IsPlayer()))
                bdWhy = "Bardic Inspiration: another player, within 18 m.";
            else if (GetBeastForm(id) && GetSheet(p).wildShapeUsed >= WildShapeMax(p))
                bdWhy = "No Wild Shape left. A short rest restores it.";
            else if (id == SPELL_REVERT_SHAPE && !WildShapeOf(p))
                bdWhy = "You're already in your own shape.";
            else if ((id == SPELL_TURN_UNDEAD || id == SPELL_PRESERVE_LIFE || id == SPELL_DESTRUCTIVE_WRATH) &&
                GetSheet(p).channelUsed >= ChannelMax(p))
                bdWhy = "No Channel Divinity left. A short rest restores it.";
            else if (id == SPELL_DESTRUCTIVE_WRATH && !CombatantOf(p->GetGUID()))
                bdWhy = "Destructive Wrath: only in a fight.";
            else if (IsArrow(id) && !HoldsBow(p))
                bdWhy = "Arrows need a bow or crossbow in your ranged slot.";
            if (!bdWhy.empty())
            {
                Error(p, bdWhy);
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        if (id == SPELL_HELP)
            if (Unit* ht = GetExplTargetUnit(); !ht || ht == p || !ht->IsPlayer() || !IsDowned(ht))
            {
                Error(p, "Help: an ally who is down, next to you.");
                return SPELL_FAILED_DONT_REPORT;
            }
        if (id == SPELL_FLEE && fighting && IsTurnOf(p))
        {
            std::string fleeWhy;
            if (!CanFlee(p, fleeWhy))
            {
                Error(p, fleeWhy);
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        // outside any fight, aimed at someone in one (a shot at its monsters, a spell on a friend fighting it, a blast
        // landing in it): you join that fight - your initiative is rolled and you act when your turn comes
        if (!fighting && a->cost != COST_NONE && !a->outOfCombatOnly)
        {
            Unit* t = GetExplTargetUnit();
            WorldLocation const* dest = GetExplTargetDest();
            float radius = std::max(2.f, GetSpellInfo()->Effects[EFFECT_0].CalcRadius(p));
            if ((t && t != p && JoinFightOf(p, t)) || (!t && dest && JoinFightAt(p, *dest, radius)))
            {
                Error(p, "You join the fight. Wait for your turn.");
                return SPELL_FAILED_DONT_REPORT;
            }
        }
        Combatant* me = CombatantOf(p->GetGUID());
        bool extra = IsAttackAction(id) && me && IsTurnOf(p) && me->actionUsed && me->extraAttacks > 0;
        bool gwm = id == SPELL_ATTACK && !extra && me && IsTurnOf(p) && me->actionUsed && me->gwmReady && !me->bonusUsed;
        Sheet& sh = GetSheet(p);
        if (id == SPELL_SECOND_WIND && sh.secondWindUsed >= 1)
            why = "Second Wind is spent. A short or long rest restores it.";
        else if (id == SPELL_ACTION_SURGE && sh.actionSurgeUsed >= 1)
            why = "Action Surge is spent. A short or long rest restores it.";
        else if (id == SPELL_ACTION_SURGE && me && !me->actionUsed)
            why = "You still have your Action - use it first.";
        else if (IsManeuver(id) && sh.supUsed >= SuperiorityMax(p))
            why = "No superiority dice left. A short or long rest restores them.";
        else if (id == SPELL_RAGE && me && me->raging)
            why = "You're already raging.";
        else if (id == SPELL_RAGE && sh.rageUsed >= RageMax(p))
            why = "No rage left. A long rest restores it.";
        else if (id == SPELL_FRENZIED_STRIKE && !(me && me->raging))
            why = "Frenzy: only while you rage.";
        else if ((id == SPELL_UNARMED_STRIKE || id == SPELL_FLURRY) && !(me && me->attackedUnarmed))
            why = "Martial Arts: take the Attack action with your fists or a monk weapon first.";
        else if ((id == SPELL_FLURRY || id == SPELL_PATIENT_DEFENSE || id == SPELL_STEP_OF_THE_WIND || id == SPELL_STUNNING_STRIKE) &&
            KiMax(p) - sh.kiUsed < 1)
            why = "No ki left. A short or long rest restores it.";
        else if (id == SPELL_CLOAK_OF_SHADOWS && KiMax(p) - sh.kiUsed < 2)
            why = "Cloak of Shadows takes 2 ki.";
        else if (id == SPELL_WHOLENESS && sh.wholenessUsed)
            why = "Wholeness of Body is spent. A long rest restores it.";
        else if (id == SPELL_HIDE && HostileAdjacent(p))
            why = "You can't hide with an enemy right next to you.";
        else if ((id == SPELL_QUICKENED || id == SPELL_TWINNED || id == SPELL_HEIGHTENED || id == SPELL_EMPOWERED || id == SPELL_BEND_LUCK) &&
            !MetaArmed(p, MetaKey(id)) && SorceryMax(p) - sh.spUsed < MetaCost(id))
            why = Acore::StringFormat("Not enough sorcery points ({} needed).", MetaCost(id));
        else if (id == SPELL_FEY_PRESENCE && sh.patronUsed)
            why = "Fey Presence is spent until a short rest.";
        else if (id == SPELL_DARK_ONES_LUCK && sh.patron6Used)
            why = "Dark One's Own Luck is spent until a short rest.";
        else if (id == SPELL_DARK_ONES_LUCK && me && me->luckArmed)
            why = "The Dark One's luck is already with you.";
        else if (id == SPELL_HELLISH_REBUKE && me && me->rebukeReady)
            why = "Hellish Rebuke is already readied.";
        else if (id == SPELL_TIDES_OF_CHAOS && sh.tidesUsed)
            why = "Tides of Chaos is spent until a long rest - or a wild surge.";
        else if (id == SPELL_CREATE_SLOT && [&] { static int const price[4] = { 0, 2, 3, 5 };
            for (int l = 1; l <= 3; ++l) if (sh.slotsUsed[l] > 0 && SorceryMax(p) - sh.spUsed >= price[l]) return false; return true; }())
            why = "No spent slot you have the sorcery points for (level 1: 2, level 2: 3, level 3: 5).";
        else if (id == SPELL_CONVERT_SLOT && SlotsLeft(p, 1) + SlotsLeft(p, 2) + SlotsLeft(p, 3) == 0)
            why = "No spell slot left to convert.";
        else if (id == SPELL_OFFHAND_ATTACK && !HasOffhandWeapon(p))
            why = "Off-hand attack: hold a one-handed weapon in each hand.";
        else if (id == SPELL_OFFHAND_ATTACK && !(me && me->attackedLight))
            why = "Off-hand attack: take the Attack action with your main-hand weapon first.";
        else if (id == SPELL_THROW && !ThrowableWeapon(p))
            why = "Throw: hold a weapon made for throwing in your main hand (dagger, handaxe, spear, light hammer or javelin).";
        else if (id == SPELL_RANGED_ATTACK && !p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
            why = "You have no ranged weapon equipped (bow, crossbow, gun or thrown weapon in the ranged slot).";
        else if (a->combatOnly && !fighting)
            why = "You can only do that in combat.";
        else if (a->outOfCombatOnly && fighting)
            why = "You can't do that in combat.";
        else if (!extra && !gwm && !CanPay(p, CostFor(p, id, a->cost), why))
            ;
        else if (int sl = SlotToSpend(p, id, a->slotLevel); sl >= 1 && SlotsLeft(p, sl) <= 0 && !GetCastItem())
            why = IsClass(p, CLASS_DND_WARLOCK) ? std::string("No Pact Magic slots left. A short rest restores them.")
                : Acore::StringFormat("No level {} spell slots left. Rest to recover them.", sl);
        if (why.empty())
            return SPELL_CAST_OK;
        Error(p, why);
        return SPELL_FAILED_DONT_REPORT;
    }

    void AfterCastHandler()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        SpellInfo const* info = GetSpellInfo();
        if (Player* p = caster->ToPlayer())
        {
            ActionInfo const* a = GetAction(info->Id);
            Combatant* me = CombatantOf(p->GetGUID());
            if (a && IsAttackAction(info->Id) && me && me->actionUsed && me->extraAttacks > 0)
            {
                --me->extraAttacks;              // the second swing of an Attack action already taken
                PushEconomy(p);
            }
            else if (a && info->Id == SPELL_ATTACK && me && me->actionUsed && me->gwmReady && !me->bonusUsed)
            {
                me->bonusUsed = true;            // Great Weapon Master's attack
                me->gwmReady = false;
                PushEconomy(p);
            }
            else if (a)
            {
                Pay(p, CostFor(p, info->Id, a->cost));
                UseCastMeta(p, "quicken");
                bool extraAttack = IsFighter(p) || IsClass(p, CLASS_BARBARIAN) || IsClass(p, CLASS_MONK) ||
                    HasInvocation(p, "Thirsting Blade") || (IsSubclass(p, "Valor") && Level(p) >= 6);
                BeastForm const* bf = WildShapeOf(p);
                if (bf && bf->attack2 && IsAttackAction(info->Id) && me && a->cost == COST_ACTION)
                    me->extraAttacks = 1;            // the beast's second attack (claws, then the bite)
                else if (IsAttackAction(info->Id) && me && extraAttack && Level(p) >= 5 && a->cost == COST_ACTION)
                {
                    me->extraAttacks = 1;
                    Dnd::Log(p, "|cffffd100Extra Attack:|r you can attack once more this action.");
                    PushEconomy(p);
                }
                if (int sl = slotBefore >= 0 ? slotBefore : SlotToSpend(p, info->Id, a->slotLevel);
                    sl >= 1 && info->Id != SPELL_HELLISH_REBUKE && !fromScroll)
                {
                    ++GetSheet(p).slotsUsed[sl];
                    SaveSheet(p);
                    PushSheet(p);
                }
                if (info->Id == SPELL_HEX && me)
                    me->hexFree = false;

            }
            if (WorldLocation const* dest = GetExplTargetDest())
                if (info->Id == SPELL_JUMP || info->Id == SPELL_GREASE || info->Id == SPELL_MISTY_STEP || info->Id == SPELL_SHADOW_STEP ||
                    info->Id == SPELL_HYPNOTIC_PATTERN || info->Id == SPELL_ENTANGLE || info->Id == SPELL_MOONBEAM ||
                    info->Id == SPELL_CALL_LIGHTNING || info->Id == SPELL_SLEEP || info->Id == SPELL_CLOUD_OF_DAGGERS ||
                    info->Id == SPELL_WEB || info->Id == SPELL_STINKING_CLOUD || info->Id == SPELL_HUNGER_OF_HADAR ||
                    info->Id == SPELL_SLEET_STORM)
                    ResolveDest(p, *dest, info);
            if (a && a->slotLevel >= 1)
                WildSurge(p);                    // a wild sorcerer's magic may surge
        }
        ResolveCast(caster, info);
        if (Player* p = caster->ToPlayer())
            if (Combatant* wc = CombatantOf(p->GetGUID()); wc && wc->wrathFired)
            {
                wc->wrathArmed = wc->wrathFired = false;
                Dnd::Log(p, "|cffffd100Destructive Wrath: the storm's full fury, spent.|r");
            }
    }

    // before the effects: a spell takes the armed metamagic with it (an instant spell's hits come before AfterCast)
    void BeforeCastHandler()
    {
        if (Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr)
        {
            if (ActionInfo const* sa = GetAction(GetSpellInfo()->Id))
                slotBefore = SlotToSpend(p, GetSpellInfo()->Id, sa->slotLevel);
            if (IsSpellAction(GetSpellInfo()->Id))
                TakeMeta(p, GetSpellInfo()->Id);
            if (GetCastItem() && IsSpellAction(GetSpellInfo()->Id))
                if (ActionInfo const* sa = GetAction(GetSpellInfo()->Id))
                {
                    fromScroll = true;
                    ReadScroll(p, std::max(1, int(sa->slotLevel)));
                    Dnd::Log(p, Acore::StringFormat("|cffb48cffYou read the scroll aloud: {} (save DC {}, +{} to hit).|r",
                        GetSpellInfo()->SpellName[0], SpellSaveDC(p), SpellAttackBonus(p)));
                }
            // a concentration spell: the one you held ends, this one begins
            if (IsConcentrationSpell(GetSpellInfo()->Id))
                if (Combatant* me = CombatantOf(p->GetGUID()))
                {
                    // (Hex cast again moves the hex: the same concentration, and a free move stays free)
                    if (!(me->concentration == GetSpellInfo()->Id && (GetSpellInfo()->Id == SPELL_HEX ||
                        GetSpellInfo()->Id == SPELL_CALL_LIGHTNING || GetSpellInfo()->Id == SPELL_WITCH_BOLT)))
                        EndConcentration(p, Acore::StringFormat(" - {} casts {}", me->name, SpellName(GetSpellInfo()->Id)));
                    me->concentration = GetSpellInfo()->Id;
                }
            // an attack from hiding is under way: WoW engages the target as the blow lands, before the dice - the fight
            // waits for ResolveAmbush at the end of the hit
            if (Unit* t = GetExplTargetUnit(); t && t != p && IsSneaking(p) && !InCombat(p))
                MarkSneakStruck(p);
        }
    }

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        if (GetSpellInfo()->Effects[EFFECT_0].Effect != SPELL_EFFECT_DUMMY)
            return;
        ResolveHit(GetCaster(), GetHitUnit(), GetSpellInfo());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_dnd_generic::CheckCast);
        BeforeCast += SpellCastFn(spell_dnd_generic::BeforeCastHandler);
        AfterCast += SpellCastFn(spell_dnd_generic::AfterCastHandler);
        OnEffectHitTarget += SpellEffectFn(spell_dnd_generic::HandleHit, EFFECT_0, SPELL_EFFECT_ANY);
    }
};

// Hearthstone = long rest, no cooldown - and never a way out of a fight (refused in one, and its teleport cancelled if a
// fight starts while it's being cast).
class spell_dnd_hearthstone : public SpellScript
{
    PrepareSpellScript(spell_dnd_hearthstone);

    void AfterCastHandler()
    {
        if (Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr)
        {
            if (InCombat(p))
                return;
            LongRest(p);
        }
    }

    SpellCastResult CheckCast()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (p && InCombat(p))
        {
            Error(p, "You can't hearth out of a fight.");
            return SPELL_FAILED_DONT_REPORT;
        }
        if (p && IsDowned(p))
        {
            Error(p, "You can't hearth while you're down.");
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    // the ten-second cast began before a fight did (or before a fall): no teleport out of it
    void HandleTeleport(SpellEffIndex effIndex)
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (p && (InCombat(p) || IsDowned(p)))
        {
            PreventHitDefaultEffect(effIndex);
            Error(p, "The fight's still on - the hearthstone's pull fades.");
        }
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_dnd_hearthstone::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_dnd_hearthstone::HandleTeleport, EFFECT_0, SPELL_EFFECT_TELEPORT_UNITS);
        AfterCast += SpellCastFn(spell_dnd_hearthstone::AfterCastHandler);
    }
};

// Drinking a potion takes a Bonus Action in combat (BG3), and only on your turn
class spell_dnd_potion : public SpellScript
{
    PrepareSpellScript(spell_dnd_potion);

    bool thrown = false;

    // the friend you have targeted (not yourself, not a foe): the potion is thrown to them instead of drunk
    static Player* ThrowTarget(Player* p)
    {
        Player* t = p->GetSelectedPlayer();
        return t && t != p && t->IsAlive() && !t->IsHostileTo(p) ? t : nullptr;
    }

    SpellCastResult CheckCast()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        std::string why;
        if (p && IsDowned(p))
            why = "You're down - you can't act.";
        else if (Player* t = p ? ThrowTarget(p) : nullptr; t && p->GetExactDist(t) > 18.f)
            why = "Too far to throw the potion (18 m).";
        else if (t && !p->IsWithinLOSInMap(t))
            why = "You can't see them to throw it.";
        if (why.empty() && (!p || CanPay(p, COST_BONUS, why)))
            return SPELL_CAST_OK;
        Error(p, why);
        return SPELL_FAILED_DONT_REPORT;
    }

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Player* t = p ? ThrowTarget(p) : nullptr;
        if (!t || p->GetExactDist(t) > 18.f)
            return;
        int amount = GetHitHeal();
        PreventHitHeal();                        // not into the thrower: into the friend it lands on
        thrown = true;
        Dnd::Log(p, Acore::StringFormat("You throw a healing potion to {}.", t->GetName()));
        Dnd::Log(t, Acore::StringFormat("{} throws you a healing potion.", p->GetName()));
        ApplyHeal(p, t, amount, GetSpellInfo());
    }

    void AfterCastHandler()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!p)
            return;
        if (InCombat(p))
        {
            Pay(p, COST_BONUS);
            if (!thrown)
                Dnd::Log(p, "You drink a healing potion (Bonus Action).");
            PushEconomy(p);
        }
        PushSheet(p);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_dnd_potion::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_dnd_potion::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
        AfterCast += SpellCastFn(spell_dnd_potion::AfterCastHandler);
    }
};

// Food (spell_script_names: every food spell with a regen aura, 2026_11_11_01_dnd_food.sql): not in a fight, and
// instead of WoW's big regen it heals 10 HP over 30 s (UpdateFood) - healing between fights that needs no rest.
class spell_dnd_food : public SpellScript
{
    PrepareSpellScript(spell_dnd_food);

    SpellCastResult CheckCast()
    {
        Player* p = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (p && InCombat(p))
        {
            Error(p, "You can't eat in the middle of a fight.");
            return SPELL_FAILED_DONT_REPORT;
        }
        return SPELL_CAST_OK;
    }

    void Register() override { OnCheckCast += SpellCheckCastFn(spell_dnd_food::CheckCast); }
};

class spell_dnd_food_aura : public AuraScript
{
    PrepareAuraScript(spell_dnd_food_aura);

    void CalcAmount(AuraEffect const* /*eff*/, int32& amount, bool& /*canBeRecalculated*/) { amount = 0; }   // no WoW regen

    void AfterApply(AuraEffect const* /*eff*/, AuraEffectHandleModes /*mode*/)
    {
        GetAura()->SetMaxDuration(30000);
        GetAura()->SetDuration(30000);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dnd_food_aura::CalcAmount, EFFECT_ALL, SPELL_AURA_MOD_REGEN);
        AfterEffectApply += AuraEffectApplyFn(spell_dnd_food_aura::AfterApply, EFFECT_ALL, SPELL_AURA_MOD_REGEN,
            AURA_EFFECT_HANDLE_REAL);
    }
};

namespace Dnd
{
    // 1 HP every 3 s while a food aura lasts (30 s: 10 HP), never in a fight
    void UpdateFood(Player* p, uint32 diff)
    {
        static std::unordered_map<ObjectGuid, uint32> timers;
        uint32& t = timers[p->GetGUID()];
        t += diff;
        if (t < 3000)
            return;
        t = 0;
        if (InCombat(p) || !p->IsAlive() || p->GetAuraEffectsByType(SPELL_AURA_MOD_REGEN).empty() || p->IsFullHealth())
            return;
        ApplyHeal(p, p, 1, nullptr);
    }
}

void AddSC_dnd_spells()
{
    RegisterSpellScript(spell_dnd_food);
    RegisterSpellScript(spell_dnd_food_aura);
    RegisterSpellScript(spell_dnd_generic);
    RegisterSpellScript(spell_dnd_hearthstone);
    RegisterSpellScript(spell_dnd_potion);
}
