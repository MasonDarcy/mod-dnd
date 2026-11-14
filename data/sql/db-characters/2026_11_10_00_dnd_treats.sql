-- mod-dnd: treats each character has rolled for (1 missed, 2 found - not yet picked up, 3 claimed)
CREATE TABLE IF NOT EXISTS `dnd_character_treat` (
  `guid` INT UNSIGNED NOT NULL,
  `treat_id` INT UNSIGNED NOT NULL,
  `state` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `treat_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
