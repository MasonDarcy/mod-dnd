-- mod-dnd: the bard (WoW Paladin) and the druid (WoW Hunter), DndClasses.cpp. Gnome paladins and hunters don't exist in
-- 3.3.5: they get start rows like the other new pairs (dwarven ones are stock; every start is Coldridge anyway)
DELETE FROM `playercreateinfo` WHERE (`race`, `class`) IN ((7, 2), (7, 3));
INSERT INTO `playercreateinfo` (`race`, `class`, `map`, `zone`, `position_x`, `position_y`, `position_z`, `orientation`) VALUES
(7, 2, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(7, 3, 0, 1, -6240.32, 331.033, 382.758, 6.17716);

-- their spells and Wild Shape forms resolve in DndSpells.cpp
DELETE FROM `spell_script_names` WHERE `spell_id` IN (201001, 201002, 201003, 201004, 201005, 201006, 201101, 201102, 201103,
  201104, 201105, 201106, 201110, 201111, 201112, 201113, 201114);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(201001, 'spell_dnd_generic'), (201002, 'spell_dnd_generic'), (201003, 'spell_dnd_generic'), (201004, 'spell_dnd_generic'),
(201005, 'spell_dnd_generic'), (201006, 'spell_dnd_generic'), (201101, 'spell_dnd_generic'), (201102, 'spell_dnd_generic'),
(201103, 'spell_dnd_generic'), (201104, 'spell_dnd_generic'), (201105, 'spell_dnd_generic'), (201106, 'spell_dnd_generic'),
(201110, 'spell_dnd_generic'), (201111, 'spell_dnd_generic'), (201112, 'spell_dnd_generic'), (201113, 'spell_dnd_generic'),
(201114, 'spell_dnd_generic');
