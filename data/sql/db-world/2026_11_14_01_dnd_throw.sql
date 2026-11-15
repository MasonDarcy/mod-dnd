-- Throw: the main-hand weapon, if it's made to be thrown (DndSpells.cpp; the spell is built by tools/make_patch.py)
DELETE FROM `spell_script_names` WHERE `spell_id` = 200012;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (200012, 'spell_dnd_generic');
