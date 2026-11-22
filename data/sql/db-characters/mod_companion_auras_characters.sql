-- mod-companion-auras: saved companion (vanity pet) names, one per character per companion type.
-- See src/CompanionNames.cpp. Rows are removed when the character is deleted.
CREATE TABLE IF NOT EXISTS `mod_companion_auras_names` (
    `guid`  INT UNSIGNED NOT NULL COMMENT 'Character GUID',
    `entry` INT UNSIGNED NOT NULL COMMENT 'Companion creature entry',
    `name`  VARCHAR(24)  NOT NULL COMMENT 'Custom name (max 12 characters, UTF-8)',
    PRIMARY KEY (`guid`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- The companion each character had out, so it can come back after logging in.
-- Removed when the player dismisses their companion or the character is deleted.
CREATE TABLE IF NOT EXISTS `mod_companion_auras_resummon` (
    `guid`  INT UNSIGNED NOT NULL COMMENT 'Character GUID',
    `spell` INT UNSIGNED NOT NULL COMMENT 'Summon spell of the companion to bring back',
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
