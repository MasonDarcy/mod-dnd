-- the story (tools/story.py, make_story_sql.py): what carries over between encounters, server-wide
DROP TABLE IF EXISTS `dnd_story_faction`;
CREATE TABLE `dnd_story_faction` (`id` VARCHAR(16) NOT NULL, `name` VARCHAR(64) NOT NULL, `description` VARCHAR(255) NOT NULL,
  `sort` TINYINT UNSIGNED NOT NULL, PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_thread`;
CREATE TABLE `dnd_story_thread` (`id` VARCHAR(32) NOT NULL, `title` VARCHAR(64) NOT NULL, `sort` SMALLINT UNSIGNED NOT NULL,
  PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_entry`;
CREATE TABLE `dnd_story_entry` (`thread` VARCHAR(32) NOT NULL, `idx` TINYINT UNSIGNED NOT NULL, `cond` VARCHAR(255) NOT NULL,
  `text` VARCHAR(1000) NOT NULL, PRIMARY KEY (`thread`, `idx`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_outcome`;
CREATE TABLE `dnd_story_outcome` (`encounter_id` INT UNSIGNED NOT NULL, `how` VARCHAR(8) NOT NULL, `effects` VARCHAR(255) NOT NULL,
  PRIMARY KEY (`encounter_id`, `how`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_start`;
CREATE TABLE `dnd_story_start` (`encounter_id` INT UNSIGNED NOT NULL, `idx` TINYINT UNSIGNED NOT NULL, `cond` VARCHAR(255) NOT NULL,
  `node` INT UNSIGNED NOT NULL, PRIMARY KEY (`encounter_id`, `idx`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_absent`;
CREATE TABLE `dnd_story_absent` (`encounter_id` INT UNSIGNED NOT NULL, `cond` VARCHAR(255) NOT NULL,
  PRIMARY KEY (`encounter_id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_world`;
CREATE TABLE `dnd_story_world` (`guid` INT UNSIGNED NOT NULL, `cond` VARCHAR(255) NOT NULL,
  PRIMARY KEY (`guid`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
DROP TABLE IF EXISTS `dnd_story_rumor`;
CREATE TABLE `dnd_story_rumor` (`id` INT UNSIGNED NOT NULL, `zone` INT UNSIGNED NOT NULL, `cond` VARCHAR(255) NOT NULL,
  `text` VARCHAR(500) NOT NULL, PRIMARY KEY (`id`)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT INTO `dnd_story_faction` (`id`, `name`, `description`, `sort`) VALUES
('ironforge', 'Ironforge', 'King Magni\'s hold and the Mountaineers who keep its roads.', 0),
('explorers', 'The Explorers\' League', 'Diggers, scholars and map-makers, always somewhere they shouldn\'t be.', 1),
('stormwind', 'Stormwind', 'The human kingdom, its watch and its nobles.', 2),
('darkiron', 'The Dark Irons', 'Ironforge\'s old enemy, back in the mountains in force.', 3),
('syndicate', 'The Underworld', 'Smugglers, thieves and the Syndicate - who owes whom, and who knows.', 4),
('horde', 'The Horde', 'Orgrimmar, Thunder Bluff and the Undercity, and what they make of you.', 5);
DELETE FROM `creature` WHERE `guid` BETWEEN 9004001 AND 9004400;
