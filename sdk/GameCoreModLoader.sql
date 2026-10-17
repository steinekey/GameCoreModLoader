-- SPDX-License-Identifier: AGPL-3.0-only
-- GameCore Mod Loader (GCML), protocol version 1. See sdk/README.md.
--
-- Every native mod runs this file, unchanged except for the one INSERT marked EDIT, as a
-- FrontEndActions UpdateDatabase with LoadOrder 1000000. The high load order makes it run after
-- mods that still swap the GameCore the old way (the Assets Limit Fix uses 10), so the loader can
-- chain to them instead of being skipped by their "DllPrefix = 'GameCore_XP2'" guard.
--
-- Rules for later protocol versions: never change these two tables (the first mod to run creates
-- them, whatever version it ships). New data goes in new tables.

-- One row per plugin DLL of every enabled native mod. LoaderVersion is the build of the loader
-- DLL that mod ships; the row with the highest one decides which copy of the loader the game uses.
CREATE TABLE IF NOT EXISTS GcmlPlugins(
	ModId TEXT NOT NULL,
	File TEXT NOT NULL,
	LoadOrder INTEGER NOT NULL DEFAULT 0,
	LoaderVersion INTEGER NOT NULL,
	PRIMARY KEY(ModId, File)
);

-- The core the loader starts after the plugins: what the GameCores row held before the first
-- GCML mod took it over. Normally the real GameCore_XP2; if an old-style fake GameCore (such as
-- the Assets Limit Fix) took the row first, it is that one, and it loads the real core itself.
CREATE TABLE IF NOT EXISTS GcmlNextCore(
	GameCore TEXT NOT NULL PRIMARY KEY,
	PackageId TEXT,
	DllPrefix TEXT NOT NULL
);

-- EDIT: your mod id (the id attribute of <Mod> in your .modinfo), your plugin's path inside your
-- mod folder, your plugin's LoadOrder among plugins (lower starts first; ties go by mod id) and
-- the build of the loader DLL you ship (GCML_LOADER_VERSION in loader/LoaderVersion.h).
INSERT OR REPLACE INTO GcmlPlugins(ModId, File, LoadOrder, LoaderVersion)
VALUES('EDIT-your-mod-id', 'Binaries/Win64/EDIT-your-plugin.dll', 0, 1);

-- Only the first GCML mod to run records anything here (INSERT OR IGNORE on the primary key).
INSERT OR IGNORE INTO GcmlNextCore(GameCore, PackageId, DllPrefix)
SELECT GameCore, PackageId, DllPrefix FROM GameCores
WHERE GameCore = 'Expansion2' AND DllPrefix <> 'GameCore_XP2_GCML';

-- Every GCML mod sets the row to the same answer, so the order the mods run in does not matter:
-- the newest loader, ties broken by the lowest mod id (copies of one loader build are identical).
UPDATE GameCores
SET PackageId = (SELECT ModId FROM GcmlPlugins ORDER BY LoaderVersion DESC, ModId LIMIT 1),
	DllPrefix = 'GameCore_XP2_GCML'
WHERE GameCore = 'Expansion2';
