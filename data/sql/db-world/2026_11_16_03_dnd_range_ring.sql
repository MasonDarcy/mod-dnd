-- mod-dnd: the range ring (DndCombat.cpp ShowRangeRing) - a glowing rune on the ground under a player hovering a spell in a
-- fight, scaled to the spell's reach. Generic (type 5), nothing to click (GO_FLAG_NOT_SELECTABLE).
DELETE FROM `gameobject_template` WHERE `entry` = 9100195;
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `size`) VALUES
(9100195, 5, 7783, 'Spell Range', 1);
DELETE FROM `gameobject_template_addon` WHERE `entry` = 9100195;
INSERT INTO `gameobject_template_addon` (`entry`, `faction`, `flags`) VALUES
(9100195, 35, 16);
