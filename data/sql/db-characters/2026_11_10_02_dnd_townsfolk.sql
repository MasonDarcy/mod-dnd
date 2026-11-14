-- mod-dnd: villagers each character has talked round (1 won their gift, 2 lost the check - they just chat now)
CREATE TABLE IF NOT EXISTS `dnd_character_townsfolk` (
  `guid` INT UNSIGNED NOT NULL,
  `spawn` INT UNSIGNED NOT NULL,
  `result` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `spawn`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
