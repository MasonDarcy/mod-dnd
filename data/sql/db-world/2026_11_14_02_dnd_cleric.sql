-- mod-dnd: the cleric (WoW Priest - the sorcerer's old slot), DndClasses.cpp / DndSpells.cpp. Priests already start in
-- Coldridge (make_world_sql.py; the gnome priest's row is 2026_11_11_03_dnd_warlock.sql).
DELETE FROM `spell_script_names` WHERE `spell_id` IN (201301, 201302, 201311, 201312, 201313, 201314, 201321, 201322, 201323, 201331, 201332, 201341, 201342, 201343);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(201301, 'spell_dnd_generic'),
(201302, 'spell_dnd_generic'),
(201311, 'spell_dnd_generic'),
(201312, 'spell_dnd_generic'),
(201313, 'spell_dnd_generic'),
(201314, 'spell_dnd_generic'),
(201321, 'spell_dnd_generic'),
(201322, 'spell_dnd_generic'),
(201323, 'spell_dnd_generic'),
(201331, 'spell_dnd_generic'),
(201332, 'spell_dnd_generic'),
(201341, 'spell_dnd_generic'),
(201342, 'spell_dnd_generic'),
(201343, 'spell_dnd_generic');
