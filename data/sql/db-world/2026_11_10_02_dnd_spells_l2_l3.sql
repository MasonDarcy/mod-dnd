-- mod-dnd: 2nd- and 3rd-level wizard spells (tools/make_patch.py builds them; DndSpells.cpp resolves them)
DELETE FROM `spell_script_names` WHERE `spell_id` IN (200121, 200122, 200123, 200124, 200131, 200132);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200121, 'spell_dnd_generic'), (200122, 'spell_dnd_generic'), (200123, 'spell_dnd_generic'), (200124, 'spell_dnd_generic'),
(200131, 'spell_dnd_generic'), (200132, 'spell_dnd_generic');
