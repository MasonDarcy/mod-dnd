-- mod-dnd: creature stat blocks, encounters, spell scripts
CREATE TABLE IF NOT EXISTS `dnd_creature` (
  `entry` INT UNSIGNED NOT NULL,
  `comment` VARCHAR(100) NOT NULL DEFAULT '',
  `ac` TINYINT NOT NULL DEFAULT 10,
  `hp` INT NOT NULL DEFAULT 4,
  `speed` FLOAT NOT NULL DEFAULT 9,
  `str` TINYINT NOT NULL DEFAULT 10,
  `dex` TINYINT NOT NULL DEFAULT 10,
  `con` TINYINT NOT NULL DEFAULT 10,
  `intel` TINYINT NOT NULL DEFAULT 10,
  `wis` TINYINT NOT NULL DEFAULT 10,
  `cha` TINYINT NOT NULL DEFAULT 10,
  `melee_name` VARCHAR(40) NOT NULL DEFAULT 'Strike',
  `melee_bonus` TINYINT NOT NULL DEFAULT 2,
  `melee_dice` VARCHAR(16) NOT NULL DEFAULT '1d4',
  `melee_type` VARCHAR(16) NOT NULL DEFAULT 'Bludgeoning',
  `ranged_name` VARCHAR(40) NOT NULL DEFAULT '',
  `ranged_bonus` TINYINT NOT NULL DEFAULT 0,
  `ranged_dice` VARCHAR(16) NOT NULL DEFAULT '',
  `ranged_type` VARCHAR(16) NOT NULL DEFAULT 'Bludgeoning',
  `ranged_range` FLOAT NOT NULL DEFAULT 0,
  `xp` INT UNSIGNED NOT NULL DEFAULT 10,
  `resist` VARCHAR(100) NOT NULL DEFAULT '',
  `immune` VARCHAR(100) NOT NULL DEFAULT '',
  `vuln` VARCHAR(100) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dnd_encounter` (
  `id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(100) NOT NULL,
  `aggro_range` FLOAT NOT NULL DEFAULT 12,
  `join_range` FLOAT NOT NULL DEFAULT 30,
  `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
  `xp` INT UNSIGNED NOT NULL DEFAULT 0,
  `dialogue_node` INT UNSIGNED NOT NULL DEFAULT 0,
  `after_text` VARCHAR(255) NOT NULL DEFAULT '',
  `patience` TINYINT NOT NULL DEFAULT 0,
  `snap_node` INT UNSIGNED NOT NULL DEFAULT 0,
  `meter_label` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dnd_zone` (
  `id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(100) NOT NULL,
  `min_level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `areas` VARCHAR(255) NOT NULL DEFAULT '' COMMENT 'AreaTable ids: areas or whole zones',
  `gate_text` VARCHAR(255) NOT NULL DEFAULT '',
  `clear_text` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dnd_dialogue_node` (
  `id` INT UNSIGNED NOT NULL,
  `text` VARCHAR(1000) NOT NULL,
  `shot` VARCHAR(16) NOT NULL DEFAULT '',
  `actor` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  `emote` INT UNSIGNED NOT NULL DEFAULT 0,
  `sets` VARCHAR(64) NOT NULL DEFAULT '',
  `patience` TINYINT NOT NULL DEFAULT 0,
  `leaves` VARCHAR(16) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- next_ok / next_fail: a node id, 0 = end the talk, -1 = fight, -2 = resolved peacefully (full XP)
CREATE TABLE IF NOT EXISTS `dnd_dialogue_option` (
  `node_id` INT UNSIGNED NOT NULL,
  `idx` TINYINT UNSIGNED NOT NULL,
  `text` VARCHAR(255) NOT NULL,
  `skill` VARCHAR(20) NOT NULL DEFAULT '',
  `dc` TINYINT NOT NULL DEFAULT 0,
  `next_ok` INT NOT NULL DEFAULT 0,
  `next_fail` INT NOT NULL DEFAULT 0,
  `req` VARCHAR(20) NOT NULL DEFAULT '',
  `need` VARCHAR(64) NOT NULL DEFAULT '',
  `adv` VARCHAR(32) NOT NULL DEFAULT '',
  `once` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`node_id`, `idx`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dnd_encounter_member` (
  `spawn_guid` INT UNSIGNED NOT NULL,
  `encounter_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`spawn_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DELETE FROM `spell_script_names` WHERE `spell_id` BETWEEN 200001 AND 200999 OR (`spell_id` = 8690 AND `ScriptName` = 'spell_dnd_hearthstone');
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(200001, 'spell_dnd_generic'), (200002, 'spell_dnd_generic'), (200003, 'spell_dnd_generic'), (200004, 'spell_dnd_generic'),
(200005, 'spell_dnd_generic'), (200006, 'spell_dnd_generic'), (200007, 'spell_dnd_generic'),
(200101, 'spell_dnd_generic'), (200102, 'spell_dnd_generic'), (200103, 'spell_dnd_generic'),
(200111, 'spell_dnd_generic'), (200112, 'spell_dnd_generic'), (200113, 'spell_dnd_generic'), (200114, 'spell_dnd_generic'),
(200115, 'spell_dnd_generic'), (200116, 'spell_dnd_generic'), (200117, 'spell_dnd_generic'), (200118, 'spell_dnd_generic'),
(200119, 'spell_dnd_generic'), (200120, 'spell_dnd_generic'), (200901, 'spell_dnd_generic'),
(8690, 'spell_dnd_hearthstone');
