// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

// Finds a mod's folder from its id. The database only holds ids (GcmlPlugins.ModId,
// GameCores.PackageId), so the loader reads the id out of every .modinfo it can find: the game's
// DLC, the Steam Workshop items of the game, the user's Mods folder, and the folders next to the
// loader's own mod.
namespace ModFolders {
	// The folder of the mod or DLC with this id, with a trailing backslash; empty if not found.
	// The first call scans the folders; a miss scans them again, once per GameCore load, in case a
	// mod was subscribed while the game was running.
	std::wstring Find(const std::string& modId);
	// Forgets the "already rescanned" mark. Called on each GameCore load.
	void AllowRescan();
	// Also scan this folder's subfolders (the parent of the loader's own mod folder).
	void AddRoot(const std::wstring& dir);
}
