-- mod-dnd: the XP each clear actually paid (a sour resolution pays half): login tops up XP that a crash lost
SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dnd_character_encounter' AND COLUMN_NAME = 'xp');
SET @sql := IF(@have = 0, 'ALTER TABLE `dnd_character_encounter` ADD COLUMN `xp` INT NULL DEFAULT NULL', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
