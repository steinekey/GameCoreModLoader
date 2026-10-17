// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <Windows.h>
#include <stdint.h>

// GameCore Mod Loader (GCML) plugin interface, version 1.
//
// The problem it solves: Civilization VI loads exactly one GameCore DLL, picked by the GameCores
// row of the ruleset. Every native mod so far (Assets Limit Fix, Community Extension, ...) ships a
// fake GameCore that loads the real one, and points that row at itself. Only one of them can win.
//
// GCML replaces "one fake GameCore per mod" with "one shared fake GameCore, many plugins". Every
// native mod ships the same loader DLL (Binaries/Win64/GameCore_XP2_GCML_FinalRelease.dll), its own
// plugin DLL, and a copy of GameCoreModLoader.sql that registers the plugin. Whichever mods are
// enabled and in whatever order their SQL runs, the row ends up on the newest loader any of them
// ships, and that loader starts every registered plugin. See sdk/README.md for the full protocol.
//
// A plugin is an ordinary DLL. It exports GcmlAttach (required) and GcmlDetach (optional), with
// the signatures below, and does nothing in its DllMain. The loader:
//   - loads the plugin once per process and never unloads it (the module is pinned), so code the
//     plugin patched into the exe stays valid when the game unloads GameCore between games;
//   - calls GcmlAttach every time GameCore is loaded (a new game, a loaded save) while the
//     plugin's mod is enabled: after the real GameCore's DllMain and before the game creates its
//     game context, so no game object exists yet;
//   - calls GcmlDetach after the game destroyed its context and before GameCore is unloaded, for
//     every plugin whose GcmlAttach returned nonzero.

// The interface version this header describes. A loader fills GcmlHost::apiVersion with its own
// version; fields are only ever added at the end of GcmlHost, so check size before using a field
// that a later version added.
#define GCML_API_VERSION 1

#define GCML_ATTACH_EXPORT "GcmlAttach"
#define GCML_DETACH_EXPORT "GcmlDetach"

#ifdef __cplusplus
extern "C" {
#endif

// What the loader tells a plugin. Valid only during the GcmlAttach call; copy what you keep.
typedef struct GcmlHost {
	// sizeof(GcmlHost) as the loader built it.
	uint32_t size;
	// The loader's GCML_API_VERSION.
	uint32_t apiVersion;
	// The loader build (the LoaderVersion it registers in SQL). Higher is newer.
	uint32_t loaderVersion;
	// 1 for the first GameCore load in this process, 2 for the next, and so on.
	uint32_t loadCount;
	// The real GameCore_XP2_FinalRelease.dll, already initialised.
	HMODULE gameCore;
	// This plugin's mod folder, with a trailing backslash.
	const wchar_t* modDir;
	// The game's user folder (Documents\My Games\Sid Meier's Civilization VI\), with a trailing
	// backslash. The place for settings and logs: a Workshop update can replace the mod folder.
	const wchar_t* userDir;
	// This plugin's mod id, as in its .modinfo.
	const char* modId;
	// Writes one line to the loader's log (Logs\GameCoreModLoader.log in the user folder).
	void (__cdecl* Log)(const char* line);
} GcmlHost;

// GcmlAttach: start the plugin for this load of GameCore. Return nonzero when it started; zero is
// logged and GcmlDetach will not be called for this load.
typedef int (__cdecl* GcmlAttachFn)(const GcmlHost* host);
// GcmlDetach: GameCore is about to be unloaded. Remove hooks into it.
typedef void (__cdecl* GcmlDetachFn)(void);

#ifdef __cplusplus
}
#endif
