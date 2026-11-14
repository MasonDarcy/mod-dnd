-- mod-dnd: fighter actions (tools/make_patch.py builds them; DndSpells.cpp resolves them)
DELETE FROM `spell_script_names` WHERE `spell_id` IN (200201, 200202, 200203, 200204, 200205);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200201, 'spell_dnd_generic'), (200202, 'spell_dnd_generic'), (200203, 'spell_dnd_generic'), (200204, 'spell_dnd_generic'),
(200205, 'spell_dnd_generic');
