-- mod-dnd: Ranged Attack (the ranged-slot weapon)
DELETE FROM `spell_script_names` WHERE `spell_id` = 200008;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (200008, 'spell_dnd_generic');
