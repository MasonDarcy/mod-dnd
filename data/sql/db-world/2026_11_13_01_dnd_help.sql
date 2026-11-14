-- mod-dnd: Help (an ally who is down gets back up with 1 HP) and Flee (leave a fight); DndSpells.cpp spell_dnd_generic
DELETE FROM `spell_script_names` WHERE `spell_id` IN (200010, 200011);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (200010, 'spell_dnd_generic'), (200011, 'spell_dnd_generic');
