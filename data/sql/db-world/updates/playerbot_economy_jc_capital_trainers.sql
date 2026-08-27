-- Jewelcrafting trainers and supplies vendors for the six old-world capitals.
--
-- Every JC trainer Blizzard shipped stands on map 530 (Silvermoon, Exodar, Shattrath and the
-- outposts) or 571 (Dalaran), so an old-world race bot that planned Jewelcrafting has no
-- reachable trainer and re-books a dead objective forever. Blood elf and draenei bots reach
-- theirs through the same-map capital rule; these spawns give the other six capitals the same
-- coverage. Each pair (trainer plus supplies vendor) stands beside the city's enchanting
-- trainer, clones the canonical Silvermoon or Exodar NPC (trainer list 113, the standard JC
-- supplies stock), and carries the city's own faction so cross-faction bots keep refusing it.

DELETE FROM `creature` WHERE `id` BETWEEN 980000 AND 980011;
DELETE FROM `npc_vendor` WHERE `entry` BETWEEN 980000 AND 980011;
DELETE FROM `creature_default_trainer` WHERE `CreatureId` BETWEEN 980000 AND 980011;
DELETE FROM `creature_template_model` WHERE `CreatureID` BETWEEN 980000 AND 980011;
DELETE FROM `creature_template` WHERE `entry` BETWEEN 980000 AND 980011;

-- Horde trainers clone Kalinda (19775, Silvermoon); vendors clone Gelanthis (16624).
-- Alliance trainers clone Farii (19778, Exodar); vendors clone Arred (17512).
DROP TEMPORARY TABLE IF EXISTS `tmp_jc_clone`;
CREATE TEMPORARY TABLE `tmp_jc_clone` AS SELECT * FROM `creature_template` WHERE `entry` = 19775;

UPDATE `tmp_jc_clone` SET `entry` = 980000, `name` = 'Telvia Dawnfacet', `faction` = 29;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980002, `name` = 'Solanna Gemveil', `faction` = 104;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980004, `name` = 'Vessira Darkfacet', `faction` = 68;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;

DROP TEMPORARY TABLE `tmp_jc_clone`;
CREATE TEMPORARY TABLE `tmp_jc_clone` AS SELECT * FROM `creature_template` WHERE `entry` = 19778;

UPDATE `tmp_jc_clone` SET `entry` = 980006, `name` = 'Naleema', `faction` = 12;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980008, `name` = 'Oruunai', `faction` = 55;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980010, `name` = 'Vaanyra', `faction` = 80;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;

DROP TEMPORARY TABLE `tmp_jc_clone`;
CREATE TEMPORARY TABLE `tmp_jc_clone` AS SELECT * FROM `creature_template` WHERE `entry` = 16624;

UPDATE `tmp_jc_clone` SET `entry` = 980001, `name` = 'Zaralda Gemhold', `faction` = 29;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980003, `name` = 'Meliri Brightstone', `faction` = 104;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980005, `name` = 'Corvin Gravelgem', `faction` = 68;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;

DROP TEMPORARY TABLE `tmp_jc_clone`;
CREATE TEMPORARY TABLE `tmp_jc_clone` AS SELECT * FROM `creature_template` WHERE `entry` = 17512;

UPDATE `tmp_jc_clone` SET `entry` = 980007, `name` = 'Odaan', `faction` = 12;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980009, `name` = 'Muulo', `faction` = 55;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;
UPDATE `tmp_jc_clone` SET `entry` = 980011, `name` = 'Ishmaa', `faction` = 80;
INSERT INTO `creature_template` SELECT * FROM `tmp_jc_clone`;

DROP TEMPORARY TABLE `tmp_jc_clone`;

INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980000, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19775;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980002, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19775;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980004, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19775;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980006, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19778;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980008, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19778;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980010, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 19778;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980001, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 16624;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980003, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 16624;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980005, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 16624;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980007, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 17512;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980009, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 17512;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 980011, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 17512;

INSERT INTO `creature_default_trainer` (`CreatureId`, `TrainerId`) VALUES
(980000, 113),
(980002, 113),
(980004, 113),
(980006, 113),
(980008, 113),
(980010, 113);

INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980001, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 16624;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980003, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 16624;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980005, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 16624;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980007, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 17512;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980009, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 17512;
INSERT INTO `npc_vendor` (`entry`, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, `VerifiedBuild`)
SELECT 980011, `slot`, `item`, `maxcount`, `incrtime`, `ExtendedCost`, 0 FROM `npc_vendor` WHERE `entry` = 17512;

-- Spawns beside each city's enchanting trainer: Godan (Orgrimmar), Teg Dawnstrider
-- (Thunder Bluff), Lavinia Crowe (Undercity), Lucan Cordell (Stormwind), Gimble
-- Thistlefuzz (Ironforge), Taladan (Darnassus).
INSERT INTO `creature` (`id`, `map`, `spawnMask`, `phaseMask`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `curhealth`, `curmana`, `MovementType`) VALUES
(980000, 1, 1, 1, 1914.5, -4434.5, 24.9, 2.862, 300, 0, 1, 0, 0),
(980001, 1, 1, 1, 1915.5, -4438.5, 24.9, 2.862, 300, 0, 1, 0, 0),
(980002, 1, 1, 1, -1108.5, 45.9, 140.5, 5.288, 300, 0, 1, 0, 0),
(980003, 1, 1, 1, -1113.0, 45.5, 140.5, 5.288, 300, 0, 1, 0, 0),
(980004, 0, 1, 1, 1484.5, 276.0, -62.1, 3.194, 300, 0, 1, 0, 0),
(980005, 0, 1, 1, 1480.0, 271.5, -62.1, 3.194, 300, 0, 1, 0, 0),
(980006, 0, 1, 1, -8856.0, 806.0, 96.5, 5.358, 300, 0, 1, 0, 0),
(980007, 0, 1, 1, -8860.5, 801.5, 96.5, 5.358, 300, 0, 1, 0, 0),
(980008, 0, 1, 1, -4806.5, -1184.0, 512.6, 3.264, 300, 0, 1, 0, 0),
(980009, 0, 1, 1, -4811.5, -1188.0, 512.6, 3.264, 300, 0, 1, 0, 0),
(980010, 1, 1, 1, 10148.0, 2322.5, 1333.1, 0.995, 300, 0, 1, 0, 0),
(980011, 1, 1, 1, 10143.5, 2318.0, 1333.1, 0.995, 300, 0, 1, 0, 0);
