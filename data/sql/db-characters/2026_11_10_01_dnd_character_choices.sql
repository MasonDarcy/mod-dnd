-- mod-dnd: the character builder's raw choices (base scores, bonuses, picks) so it can reopen pre-filled
SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dnd_character' AND COLUMN_NAME = 'build');
SET @sql := IF(@have = 0, 'ALTER TABLE `dnd_character` ADD COLUMN `build` VARCHAR(96) NOT NULL DEFAULT \'\'', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
