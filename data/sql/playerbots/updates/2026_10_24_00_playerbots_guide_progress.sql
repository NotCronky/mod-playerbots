DROP TABLE IF EXISTS `playerbots_guide_progress`;
CREATE TABLE `playerbots_guide_progress` (
  `guid` INT UNSIGNED NOT NULL,
  `guide` VARCHAR(100) NOT NULL,
  `step` INT UNSIGNED NOT NULL COMMENT 'RestedXP step number',
  `active` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
