-- mod-dnd: levelling choices - Arcane Tradition (subclass), the level-4 ASI / feat, and Abjuration's Arcane Ward
SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
               WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'dnd_character' AND COLUMN_NAME = 'subclass');
SET @sql := IF(@have = 0, 'ALTER TABLE `dnd_character` ADD COLUMN `subclass` VARCHAR(16) NOT NULL DEFAULT \'\', ADD COLUMN `picks` VARCHAR(128) NOT NULL DEFAULT \'\', ADD COLUMN `ward_hp` INT NOT NULL DEFAULT 0, ADD COLUMN `ward_made` TINYINT UNSIGNED NOT NULL DEFAULT 0', 'SELECT 1');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
