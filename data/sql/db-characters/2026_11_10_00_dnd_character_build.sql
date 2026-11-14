-- mod-dnd: character builds (background, skill proficiencies, whether the player has made their choices)
SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dnd_character' AND COLUMN_NAME = 'skills');
SET @sql := IF(@have = 0, 'ALTER TABLE `dnd_character` ADD COLUMN `background` VARCHAR(20) NOT NULL DEFAULT \'\', ADD COLUMN `skills` VARCHAR(255) NOT NULL DEFAULT \'\', ADD COLUMN `created` TINYINT UNSIGNED NOT NULL DEFAULT 0', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
