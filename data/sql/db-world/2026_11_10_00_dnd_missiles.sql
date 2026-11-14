-- enemy missiles by damage type (CreatureMissile in DndSpells.cpp; the spells are built by tools/make_patch.py)
DELETE FROM `spell_script_names` WHERE `spell_id` BETWEEN 200902 AND 200905;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200902, 'spell_dnd_generic'), (200903, 'spell_dnd_generic'), (200904, 'spell_dnd_generic'), (200905, 'spell_dnd_generic');
