-- mod-dnd: two-weapon fighting - the off-hand strike (a Bonus Action after the Attack action; DndSpells.cpp)
DELETE FROM `spell_script_names` WHERE `spell_id` = 200009;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200009, 'spell_dnd_generic');
