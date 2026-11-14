-- mod-dnd: healing potions (Minor 439, Lesser 440) cost a Bonus Action in combat (DndSpells.cpp spell_dnd_potion)
DELETE FROM `spell_script_names` WHERE `spell_id` IN (439, 440);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (439, 'spell_dnd_potion'), (440, 'spell_dnd_potion');
