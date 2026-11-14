-- more enemy missiles (CreatureMissile in DndSpells.cpp; the spells are built by tools/make_patch.py): a thrown spear,
-- a thrown hand axe, a gun's bullet
DELETE FROM `spell_script_names` WHERE `spell_id` BETWEEN 200906 AND 200908;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200906, 'spell_dnd_generic'), (200907, 'spell_dnd_generic'), (200908, 'spell_dnd_generic');
