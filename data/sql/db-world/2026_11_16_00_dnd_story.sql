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
INSERT INTO `dnd_story_thread` (`id`, `title`, `sort`) VALUES
('copperveins', 'The Coppervein Claim', 0),
('crown', 'The Crown of Ironforge', 1);
INSERT INTO `dnd_story_entry` (`thread`, `idx`, `cond`, `text`) VALUES
('copperveins', 0, 'resolved:7', 'In Coldridge Valley an old prospector, Grumbold Coppervein, met us with a pick and a temper, sure we\'d come to jump his claim.'),
('copperveins', 1, 'story:grumbold_knows', 'His seam was fool\'s gold, and the claim was never anyone\'s to sell: he\'d bought it from a smooth-tongued stranger in Kharanos who left in a hurry. Grumbold went after him.'),
('copperveins', 2, 'story:grumbold_kin', 'He took us for kin and waved us through. As far as we know, he\'s still digging.'),
('copperveins', 3, 'story:grumbold_broke', 'His diggers walked out on him, three weeks unpaid. He sat down on his rocks and wanted only a drink.'),
('copperveins', 4, 'story:grumbold_dead', 'It came to blows at his claim. Grumbold Coppervein is dead.'),
('copperveins', 5, 'story:thalgrim_freed', 'At Gol\'Bolar Quarry, Rockjaw troggs had a dwarf in chains, tipping ore into a crack in the cliff - filling something in. He went free. His name is Thalgrim Coppervein.'),
('copperveins', 6, 'story:copperveins_hunt', 'Thalgrim is Grumbold\'s brother. Two Copperveins are looking for the man who sold that claim, and they aren\'t being quiet about it.'),
('copperveins', 7, 'story:thalgrim_mourns', 'We told Thalgrim his brother was dead. He took it the way dwarves do: quietly, and for a long time.'),
('crown', 0, 'resolved:150', 'In Ironforge, Moira Thaurissan - Magni\'s daughter, a Dark Iron by marriage and by blood - holds the High Seat as regent, while her father stands turned to diamond behind the throne.'),
('crown', 1, 'story:magni_hope', 'We laid a shard of the king\'s diamond before her. Magni is alive in there, and Moira means to find the way to bring him back.'),
('crown', 2, 'story:moira_council', 'She agreed to call the clans - Bronzebeard, Wildhammer and her own Dark Irons - to rule together, as a council.'),
('crown', 3, 'resolved:158', 'In the Military Ward, a cell of Bronzebeard loyalists was planning to kill the regent.'),
('crown', 4, 'story:loyalists_stand_down', 'They stood down, to wait for the clans.'),
('crown', 5, 'story:loyalists_signet', 'We gave them the king\'s own signet, and they knelt to it.'),
('crown', 6, 'story:moira_murdered', 'We left them to their plan, and they carried it out. Moira Thaurissan was killed at her evening audience; the loyalists hold the High Seat.'),
('crown', 7, 'story:loyalists_dead', 'It came to a fight in the Military Ward. The loyalist cell is no more.'),
('crown', 8, 'story:moira_dead', 'We killed the regent on her own throne. Ironforge\'s High Seat stands empty.'),
('crown', 9, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone', 'The clans came. Muradin Bronzebeard and Falstad Wildhammer sit beside Moira now: the Council of Three Hammers rules Ironforge.'),
('crown', 10, 'story:forger_exposed', 'In the Vault, a Dark Iron senator was forging deeds to the Bronzebeard holdings. The bankers have the true ones now.'),
('crown', 11, 'story:copperveins_avenged', 'He was also the man selling false claims in Kharanos. The Copperveins came for their reckoning.'),
('crown', 12, 'story:forger_shamed', 'A Dark Iron senator forging deeds in the Vault put down his pen - for now.'),
('crown', 13, 'story:envoy_recalled', 'In Stormwind, the regent\'s envoy to Lady Prestor turned for home without a treaty.'),
('crown', 14, 'story:refugees_home', 'The mountaineers who fled the regency to Stormwind have gone home.');
INSERT INTO `dnd_story_outcome` (`encounter_id`, `how`, `effects`) VALUES
(7, 'fight', 'grumbold_dead'),
(15, 'fight', 'thalgrim_freed'),
(150, 'fight', 'moira_dead,regent_gone,rep:darkiron-3,rep:ironforge+1,lean-1'),
(158, 'fight', 'loyalists_dead,rep:ironforge-2,rep:darkiron+1'),
(162, 'fight', 'forger_dead,rep:darkiron-1'),
(180, 'fight', 'envoy_dead,rep:darkiron-2');
INSERT INTO `dnd_story_start` (`encounter_id`, `idx`, `cond`, `node`) VALUES
(158, 0, 'story:regent_gone', 15808),
(158, 1, 'story:moira_council', 15809),
(178, 0, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone', 17807),
(178, 1, 'story:regent_gone', 17808);
INSERT INTO `dnd_story_absent` (`encounter_id`, `cond`) VALUES
(150, 'story:moira_murdered'),
(180, 'story:regent_gone');
INSERT INTO `dnd_story_world` (`guid`, `cond`) VALUES
(9004001, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone'),
(9004002, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone'),
(9004003, 'story:moira_murdered'),
(9004004, 'story:magni_hope');
INSERT INTO `dnd_story_rumor` (`id`, `zone`, `cond`, `text`) VALUES
(1, 2, 'story:copperveins_hunt', '"The Coppervein brothers came through asking after a smooth fellow who sells claims. Nobody liked the look on their faces."'),
(2, 2, 'story:thalgrim_freed,!story:copperveins_hunt', '"Thalgrim Coppervein walked out of Gol\'Bolar on his own two feet, they say. Someone pulled him out of there."'),
(3, 1, 'story:grumbold_dead', '"Old Grumbold\'s claim has gone quiet. Picked a fight with the wrong stranger, folk say."'),
(4, 4, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone', '"Three seats at the High Seat now - Bronzebeard, Wildhammer and Thaurissan, scowling at each other. Better than knives, I say."'),
(5, 4, 'story:moira_murdered', '"They say the regent was killed at her own audience. Loyalists hold the throne room, and every Dark Iron in the city is packing."'),
(6, 4, 'story:moira_dead', '"The High Seat\'s empty. Someone walked in and cut the regent down. Nobody knows who rules now."'),
(7, 4, 'story:magni_hope', '"The priests keep a vigil by the king\'s statue day and night. Word is he\'s still alive in there."'),
(8, 4, 'story:forger_exposed', '"A Dark Iron senator, caught forging deeds in the Vault! The bankers are still counting."'),
(9, 12, 'story:envoy_recalled', '"The Dark Iron envoy left without his treaty. The Lady was not pleased, they say."'),
(10, 2, 'story:copperveins_avenged', '"The Copperveins are standing rounds at Thunderbrew\'s. Caught their swindler in the Vault of Ironforge, of all places."');
DELETE FROM `creature` WHERE `guid` BETWEEN 9004001 AND 9004400;
INSERT INTO `creature` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `MovementType`) VALUES
(9004001, 9100101, 0, 1, 1, 0, -4862.0, -1026.5, 505.8, 0.0, 300, 0, 0, 0),
(9004002, 9100102, 0, 1, 1, 0, -4862.0, -1019.0, 505.8, 0.0, 300, 0, 0, 0),
(9004003, 9100103, 0, 1, 1, 0, -4865.7, -1022.7, 505.8, 0.0, 300, 0, 0, 0),
(9004004, 9100104, 0, 1, 1, 0, -4869.5, -1022.7, 505.8, 0.0, 300, 0, 0, 0);
