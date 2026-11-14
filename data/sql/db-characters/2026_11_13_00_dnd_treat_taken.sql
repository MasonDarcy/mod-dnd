-- mod-dnd: a treat claimed by anyone is gone for everyone (server-wide, for good; `.dnd treatsback` returns them all)
CREATE TABLE IF NOT EXISTS `dnd_treat_taken` (
  `treat_id` INT UNSIGNED NOT NULL,
  `guid` INT UNSIGNED NOT NULL,
  `name` VARCHAR(12) NOT NULL DEFAULT '',
  `taken_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`treat_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
