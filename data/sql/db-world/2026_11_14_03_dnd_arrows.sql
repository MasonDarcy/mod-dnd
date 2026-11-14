-- mod-dnd: special arrows and grenades (DndSpells.cpp ArrowShot / Grenade; the items are tools/shops.py, tooltips make_patch.py).
DELETE FROM `spell_script_names` WHERE `spell_id` IN (201401, 201402, 201403, 201404, 201405, 201411, 201412, 201413, 201414, 201415);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(201401, 'spell_dnd_generic'),
(201402, 'spell_dnd_generic'),
(201403, 'spell_dnd_generic'),
(201404, 'spell_dnd_generic'),
(201405, 'spell_dnd_generic'),
(201411, 'spell_dnd_generic'),
(201412, 'spell_dnd_generic'),
(201413, 'spell_dnd_generic'),
(201414, 'spell_dnd_generic'),
(201415, 'spell_dnd_generic');
