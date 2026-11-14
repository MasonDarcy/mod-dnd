-- mod-dnd: fighter resources spent since the last rest ("second wind,action surge,superiority dice")
SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dnd_character' AND COLUMN_NAME = 'res');
SET @sql := IF(@have = 0, 'ALTER TABLE `dnd_character` ADD COLUMN `res` VARCHAR(32) NOT NULL DEFAULT \'0,0,0\'', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
