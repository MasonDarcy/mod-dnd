-- mod-dnd: 4th-level spells (scrolls, and the strongest creatures), resolved in DndSpells.cpp
DELETE FROM `spell_script_names` WHERE `spell_id` IN (201741, 201742, 201743, 201744, 201745, 201746, 201747, 201748, 201749, 201750, 201751);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(201741, 'spell_dnd_generic'),
(201742, 'spell_dnd_generic'),
(201743, 'spell_dnd_generic'),
(201744, 'spell_dnd_generic'),
(201745, 'spell_dnd_generic'),
(201746, 'spell_dnd_generic'),
(201747, 'spell_dnd_generic'),
(201748, 'spell_dnd_generic'),
(201749, 'spell_dnd_generic'),
(201750, 'spell_dnd_generic'),
(201751, 'spell_dnd_generic');
