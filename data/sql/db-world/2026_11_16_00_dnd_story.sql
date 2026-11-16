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
('crown', 'The Crown of Ironforge', 1),
('lady', 'The Lady of Stormwind', 2),
('drums', 'The Drums of War', 3);
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
('crown', 14, 'story:refugees_home', 'The mountaineers who fled the regency to Stormwind have gone home.'),
('lady', 0, 'resolved:177', 'In an Old Town alley an SI:7 man named Renzik told us what the spymasters can\'t prove: the Lady Prestor on Stormwind\'s throne is not a lady.'),
('lady', 1, 'story:si7_letter', 'We carried his sealed letter to Archbishop Benedictus.'),
('lady', 2, 'story:si7_trusted', 'SI:7 counts us among its own now.'),
('lady', 3, 'resolved:172', 'Behind the Keep, Lord Gregor Lescovar was selling the Keep\'s secrets to the Defias Brotherhood.'),
('lady', 4, 'story:lescovar_names', 'He gave up the names of his Brotherhood friends.'),
('lady', 5, 'story:keep_secrets', 'We walked out of his garden carrying the Keep\'s watch rotas, meant for VanCleef.'),
('lady', 6, 'resolved:170', 'We stood before the Lady Prestor on the king\'s throne.'),
('lady', 7, 'story:lady_seen', 'When she saw the black scale her eyes went gold and slit, for a heartbeat. Whatever she is, she is no woman.'),
('lady', 8, 'story:lady_fled', 'She left the throne by the king\'s private stair and has not been seen since. Wherever she went, she went alive.'),
('lady', 9, 'story:lady_unmasked', 'With the Archbishop\'s priests at the door, her shadow betrayed her in front of the whole court. The Lady Prestor was a black dragon, and all Stormwind knows it now.'),
('lady', 10, 'story:lady_slain', 'It came to a fight in the throne room. What died there was not a woman.'),
('lady', 11, 'story:lady_gone', 'Highlord Bolvar Fordragon sits on the throne of Stormwind again.'),
('lady', 12, 'resolved:118', 'In Lethlor Ravine, a black drake was missing one egg from her clutch of seven.'),
('lady', 13, 'story:drake_egg_returned', 'We gave the egg back. She curled round all seven, and let us go.'),
('lady', 14, 'story:drake_sent_on_ogres', 'We told her the ogres had taken it. The Dustbelchers will be counting their dead.'),
('lady', 15, 'story:drake_slain', 'We killed her in the ravine.'),
('lady', 16, 'story:morgaine_retired', 'Morgaine the Sly of the Defias read her own name on VanCleef\'s list of the \'retired\', and retired.'),
('drums', 0, 'resolved:248', 'In Orgrimmar\'s Valley of Honor, Garrosh Hellscream - Grom\'s son - paces like a caged wolf, hungry for a war with the Alliance.'),
('drums', 1, 'story:garrosh_hunts_blade', 'We told him the Burning Blade was in his city. He went hunting demon-worshippers instead of us.'),
('drums', 2, 'story:garrosh_father', 'We gave him his father\'s pendant. For a moment, he looked very young.'),
('drums', 3, 'resolved:246', 'Eitrigg, the Warchief\'s old friend, commands his guard - and wears a token of the Silver Hand.'),
('drums', 4, 'story:eitrigg_peace', 'He remembered Tirion Fordring, who gave up everything for him. He will speak against the war.'),
('drums', 5, 'resolved:234', 'In Thunder Bluff, Baine Bloodhoof keeps the great war drum on the rise, at his father\'s word and Magatha\'s whispering.'),
('drums', 6, 'story:drums_silenced', 'The drums are silent. Baine laid the sticks down.'),
('drums', 7, 'resolved:232', 'Magatha Grimtotem, the Elder Crone, sits in her smoky lodge behind all the talk of war.'),
('drums', 8, 'story:magatha_exposed', 'Her ledger told the rest: every Grimtotem warband from Stonetalon to Feralas, waiting for the night Cairne falls.'),
('drums', 9, 'story:magatha_fled', 'We named the vial in her sleeve. She ran.'),
('drums', 10, 'resolved:218', 'In the Undercity\'s Apothecarium, Master Apothecary Faranell brews a new plague in copper vats.'),
('drums', 11, 'story:plague_stopped', 'The plague will not be leaving the Apothecarium. Not this year.'),
('drums', 12, 'story:plague_doubt', 'We showed him the queen\'s own orders: Stormwind first, then any Forsaken who argued. He is thinking about that.'),
('drums', 13, 'resolved:212', 'Varimathras, the queen\'s enslaved dreadlord, offered us a bargain against her.'),
('drums', 14, 'story:dreadlord_deal', 'We took it. Her doors will be unwatched on the night we come for her.'),
('drums', 15, 'story:dreadlord_leashed', 'We hold a letter that would see him unmade if she read it. He\'ll keep out of our way.'),
('drums', 16, 'story:drums_silenced,story:eitrigg_peace,story:plague_stopped', 'The drums are silent in Thunder Bluff, the plague is spilled in the Undercity, and Eitrigg speaks for peace in Orgrimmar. Whatever the Warchief decides, the Horde will not be marching on the Alliance this season.');
INSERT INTO `dnd_story_outcome` (`encounter_id`, `how`, `effects`) VALUES
(7, 'fight', 'grumbold_dead'),
(15, 'fight', 'thalgrim_freed'),
(150, 'fight', 'moira_dead,regent_gone,rep:darkiron-3,rep:ironforge+1,lean-1'),
(158, 'fight', 'loyalists_dead,rep:ironforge-2,rep:darkiron+1'),
(162, 'fight', 'forger_dead,rep:darkiron-1'),
(180, 'fight', 'envoy_dead,rep:darkiron-2'),
(170, 'fight', 'lady_slain,lady_gone,rep:stormwind+3'),
(177, 'fight', 'si7_agent_dead,rep:stormwind-2'),
(172, 'fight', 'lescovar_dead,rep:stormwind+1'),
(118, 'fight', 'drake_slain'),
(197, 'fight', 'morgaine_dead,rep:syndicate-1'),
(175, 'fight', 'bazil_dead,rep:syndicate-1'),
(248, 'fight', 'garrosh_fought,rep:horde-2'),
(246, 'fight', 'eitrigg_fought,rep:horde-2'),
(234, 'fight', 'baine_fought,rep:horde-2'),
(232, 'fight', 'magatha_dead,rep:horde+1'),
(218, 'fight', 'plague_stopped,rep:horde-1'),
(212, 'fight', 'dreadlord_dead');
INSERT INTO `dnd_story_start` (`encounter_id`, `idx`, `cond`, `node`) VALUES
(158, 0, 'story:regent_gone', 15808),
(158, 1, 'story:moira_council', 15809),
(178, 0, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone', 17807),
(178, 1, 'story:regent_gone', 17808),
(170, 0, 'story:drake_slain', 17008),
(170, 1, 'story:drake_egg_returned', 17009),
(234, 0, 'story:magatha_exposed', 23408),
(232, 0, 'story:drums_silenced', 23208);
INSERT INTO `dnd_story_absent` (`encounter_id`, `cond`) VALUES
(150, 'story:moira_murdered'),
(180, 'story:regent_gone'),
(170, 'story:lady_gone'),
(240, 'story:garrosh_hunts_blade');
INSERT INTO `dnd_story_world` (`guid`, `cond`) VALUES
(9004001, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone'),
(9004002, 'story:moira_council,story:loyalists_stand_down,!story:regent_gone'),
(9004003, 'story:moira_murdered'),
(9004004, 'story:magni_hope'),
(9004005, 'story:lady_gone');
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
(10, 2, 'story:copperveins_avenged', '"The Copperveins are standing rounds at Thunderbrew\'s. Caught their swindler in the Vault of Ironforge, of all places."'),
(11, 12, 'story:lady_unmasked', '"A dragon! On the king\'s throne, all this time! The Cathedral bells rang for an hour."'),
(12, 12, 'story:lady_fled', '"The Lady\'s gone - walked out of the Keep one evening and never came back. Bolvar\'s on the throne again, Light be thanked."'),
(13, 12, 'story:lady_slain', '"Something huge and black died in the throne room, they say. The masons are still scrubbing the floor."'),
(14, 12, 'story:keep_secrets,!story:lady_gone', '"The Blackwings doubled the watch at the Keep. Someone\'s been selling the rotas."'),
(15, 13, 'story:morgaine_retired', '"Morgaine the Sly\'s gone from the Goldshire road. Good riddance, I say."'),
(16, 6, 'story:drake_sent_on_ogres', '"The Dustbelchers are howling. A black drake came down on their camp in the night."'),
(17, 20, 'story:garrosh_hunts_blade', '"Hellscream tore through the Cleft of Shadow last night. Not a Burning Blade jar left standing."'),
(18, 19, 'story:drums_silenced', '"The great drum on the rise is quiet. Baine laid the sticks down himself."'),
(19, 19, 'story:magatha_exposed', '"The Grimtotem are leaving the Elder Rise in the night. Something about a ledger."'),
(20, 17, 'story:plague_stopped', '"The Apothecarium\'s vats are cold. Faranell won\'t say why."'),
(21, 4, 'story:drums_silenced,story:eitrigg_peace,story:plague_stopped', '"Word from the Barrens: the Horde\'s war-talk has gone quiet. Nobody knows why, and nobody\'s complaining."'),
(22, 12, 'story:drums_silenced,story:eitrigg_peace,story:plague_stopped', '"The Horde was gathering for war, they say - and then it stopped. The Lord Admiral can\'t make it out."');
DELETE FROM `creature` WHERE `guid` BETWEEN 9004001 AND 9004400;
INSERT INTO `creature` (`guid`, `id`, `map`, `spawnMask`, `phaseMask`, `equipment_id`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `currentwaypoint`, `MovementType`) VALUES
(9004001, 9100101, 0, 1, 1, 0, -4862.0, -1026.5, 505.8, 0.0, 300, 0, 0, 0),
(9004002, 9100102, 0, 1, 1, 0, -4862.0, -1019.0, 505.8, 0.0, 300, 0, 0, 0),
(9004003, 9100103, 0, 1, 1, 0, -4865.7, -1022.7, 505.8, 0.0, 300, 0, 0, 0),
(9004004, 9100104, 0, 1, 1, 0, -4869.5, -1022.7, 505.8, 0.0, 300, 0, 0, 0),
(9004005, 9100105, 0, 1, 1, 0, -8441.4, 333.1, 122.7, 2.23, 300, 0, 0, 0);
