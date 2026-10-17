// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// The build of the GameCore Mod Loader. Mods register it as LoaderVersion in GcmlPlugins (see
// sdk/GameCoreModLoader.sql); the game uses the copy shipped by the mod with the highest value.
// Raise it for every released change to the loader DLL.
#define GCML_LOADER_VERSION 4
