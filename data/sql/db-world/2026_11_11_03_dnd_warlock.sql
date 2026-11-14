-- mod-dnd: the warlock (WoW Warlock) and the sorcerer's move to WoW Priest (DndClasses.cpp). Gnome priests don't exist
-- in 3.3.5, so they get a start row like the other new pairs (dwarf priests are stock, and start in Coldridge already)
DELETE FROM `playercreateinfo` WHERE (`race`, `class`) IN ((7, 5));
INSERT INTO `playercreateinfo` (`race`, `class`, `map`, `zone`, `position_x`, `position_y`, `position_z`, `orientation`) VALUES
(7, 5, 0, 1, -6240.32, 331.033, 382.758, 6.17716);

-- the warlock's spells and patron features resolve in DndSpells.cpp (200811-200813 are display auras)
DELETE FROM `spell_script_names` WHERE `spell_id` IN (200801, 200802, 200803, 200804, 200805, 200806, 200807, 200808, 200809, 200810);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200801, 'spell_dnd_generic'),
(200802, 'spell_dnd_generic'),
(200803, 'spell_dnd_generic'),
(200804, 'spell_dnd_generic'),
(200805, 'spell_dnd_generic'),
(200806, 'spell_dnd_generic'),
(200807, 'spell_dnd_generic'),
(200808, 'spell_dnd_generic'),
(200809, 'spell_dnd_generic'),
(200810, 'spell_dnd_generic');
