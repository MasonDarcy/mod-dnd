-- mod-dnd: encounters each character has cleared (their XP is paid once)
CREATE TABLE IF NOT EXISTS `dnd_character_encounter` (
  `guid` INT UNSIGNED NOT NULL,
  `encounter_id` INT UNSIGNED NOT NULL,
  `cleared_at` INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `encounter_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
