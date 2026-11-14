-- mod-dnd: the barbarian, rogue, monk and sorcerer (DndClasses.cpp). Dwarves and gnomes may be any of the six classes:
-- Shaman = Barbarian, Druid = Monk, Warlock = Sorcerer, and dwarven Mages; they start in Coldridge like everyone
DELETE FROM `playercreateinfo` WHERE (`race`, `class`) IN ((3, 7), (3, 8), (3, 9), (3, 11), (7, 7), (7, 11));
INSERT INTO `playercreateinfo` (`race`, `class`, `map`, `zone`, `position_x`, `position_y`, `position_z`, `orientation`) VALUES
(3, 7, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(3, 8, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(3, 9, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(3, 11, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(7, 7, 0, 1, -6240.32, 331.033, 382.758, 6.17716),
(7, 11, 0, 1, -6240.32, 331.033, 382.758, 6.17716);

-- the class actions resolve in DndSpells.cpp
DELETE FROM `spell_script_names` WHERE `spell_id` IN (200401, 200402, 200403, 200501, 200502, 200503, 200601, 200602, 200603, 200604, 200605, 200606, 200607, 200608, 200701, 200702, 200703, 200704, 200705, 200706, 200707, 200708);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200401, 'spell_dnd_generic'),
(200402, 'spell_dnd_generic'),
(200403, 'spell_dnd_generic'),
(200501, 'spell_dnd_generic'),
(200502, 'spell_dnd_generic'),
(200503, 'spell_dnd_generic'),
(200601, 'spell_dnd_generic'),
(200602, 'spell_dnd_generic'),
(200603, 'spell_dnd_generic'),
(200604, 'spell_dnd_generic'),
(200605, 'spell_dnd_generic'),
(200606, 'spell_dnd_generic'),
(200607, 'spell_dnd_generic'),
(200608, 'spell_dnd_generic'),
(200701, 'spell_dnd_generic'),
(200702, 'spell_dnd_generic'),
(200703, 'spell_dnd_generic'),
(200704, 'spell_dnd_generic'),
(200705, 'spell_dnd_generic'),
(200706, 'spell_dnd_generic'),
(200707, 'spell_dnd_generic'),
(200708, 'spell_dnd_generic');
