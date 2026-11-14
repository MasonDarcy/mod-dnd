-- mod-dnd: more spells for the casters (5e lists), resolved in DndSpells.cpp
DELETE FROM `spell_script_names` WHERE `spell_id` IN (201201, 201202, 201203, 201211, 201212, 201213, 201214, 201221, 201222, 201223, 201231, 201232, 201233, 201234);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(201201, 'spell_dnd_generic'),
(201202, 'spell_dnd_generic'),
(201203, 'spell_dnd_generic'),
(201211, 'spell_dnd_generic'),
(201212, 'spell_dnd_generic'),
(201213, 'spell_dnd_generic'),
(201214, 'spell_dnd_generic'),
(201221, 'spell_dnd_generic'),
(201222, 'spell_dnd_generic'),
(201223, 'spell_dnd_generic'),
(201231, 'spell_dnd_generic'),
(201232, 'spell_dnd_generic'),
(201233, 'spell_dnd_generic'),
(201234, 'spell_dnd_generic');
