# GameCore Mod Loader (GCML)

One shared way for native (DLL) mods of Civilization VI to load from the Steam Workshop, with no
installer, and to work together in any combination and any load order.

## The problem

The game loads exactly one GameCore DLL. It finds it through one row of the front end's
configuration database:

```sql
SELECT PackageId, DllPrefix, GameCores.GameCore FROM GameCores
INNER JOIN Rulesets ON Rulesets.GameCore = GameCores.GameCore WHERE RuleSetType = ? LIMIT 1
```

and loads `<folder of the PackageId mod>/Binaries/Win64/<DllPrefix>_FinalRelease.dll`. A mod's
FrontEndActions SQL can rewrite that row, so a mod can ship a fake GameCore that forwards every
export to the real one and loads its own code. The Assets Limit Fix and the Community Extension both
do this. But there is only one row, so only one such mod can win: today they guard their `UPDATE`
with `DllPrefix = 'GameCore_XP2'`, and whichever runs second silently does nothing.

Chaining fake GameCores ("each stub loads whatever the row pointed at before it") does not work on
its own either: a stub cannot see what the row held before its SQL ran, so it does not know what to
chain to. It has to read the database for that anyway, and once it reads the database, one shared
loader with a list of plugins is simpler than a chain of stubs.

## How GCML works

Every native mod ships the same three things:

| File | What it is |
|---|---|
| `Binaries/Win64/GameCore_XP2_GCML_FinalRelease.dll` | the loader, the same file in every mod (built from `loader/`) |
| your plugin DLL, anywhere in your mod | your code, exporting `GcmlAttach` (see `GameCorePlugin.h`) |
| `GameCoreModLoader.sql` as a FrontEndActions `UpdateDatabase`, LoadOrder 1000000 | registers your plugin and points the row at the loader |

The SQL (`GameCoreModLoader.sql`, change only the line marked EDIT):

1. creates `GcmlPlugins` (one row per plugin of every enabled GCML mod) and `GcmlNextCore`,
2. adds your plugin's row, with the build of the loader you ship,
3. records what the GameCores row held before the first GCML mod changed it (`INSERT OR IGNORE`),
4. sets the row to the loader of the mod with the highest loader build, ties going to the lowest
   mod id. Every GCML mod sets the same answer, so the order the mods run in does not matter, and
   copies of one loader build are identical, so it does not matter whose copy it is.

The loader, when the game loads it:

1. **DllMain** loads the real `GameCore_XP2_FinalRelease.dll` (from the Expansion2 DLC folder), so
   every export it forwards resolves.
2. **DllCreateGameContext** (the exe calls it right after loading the DLL) opens the configuration
   database, reads `GcmlPlugins` and `GcmlNextCore`, finds each mod's folder from its id (by
   reading the `.modinfo` files of the DLC, the Workshop folder and the user's Mods folder), loads
   each plugin, pins it (it is never unloaded) and calls its `GcmlAttach`. Then it calls the next
   core's `DllCreateGameContext`: the real one, or an old-style fake GameCore that held the row
   before the first GCML mod ran (so the Assets Limit Fix keeps working: give GCML mods a higher
   LoadOrder than old-style mods).
3. **DllDestroyGameContext** calls the next core's, then every plugin's `GcmlDetach`.
4. The exe unloads the loader and the real GameCore whenever a save is loaded, and loads them again;
   steps 1-3 repeat, with the plugins already loaded.

Only enabled mods' SQL runs, so only enabled mods' plugins are registered: nothing has to read
`Mods.sqlite` or guess.

### Reading the configuration database from native code

The configuration database is a shared in-memory SQLite database named
`file:configuration-<n>?mode=memory&cache=shared`, where `<n>` is a byte the exe increments every
time it rebuilds it (`CivilizationVI_DX12.exe+0xbd1f0`). Any connection made by the same copy of
SQLite in the same process sees it, so the loader opens it through the game's own
`DatabaseDLL_FinalRelease.dll`:

- export `Database::CreateConnection(const char* name, int sqliteFlags, IDatabaseLogger*)`, called
  with flags `0x41` (read only, URI), the same as the exe's own read-only configuration connections;
- connection vtable slot 3 `CreateQuery(sql, length)` (length -1: up to the terminating zero),
  query slot 4 `Step()`, slot 30 `ColumnText(column)`, slot 1 `Release()` on both. These are the slots the exe itself reads `GameCores` with
  (`CivilizationVI_DX12.exe+0x249840`), so they are the ABI between the game's own DLLs.

**But when the game loads GameCore, it is in the middle of a write transaction on that database
that has changed its tables.** SQLite's shared cache then refuses every other connection with
"database schema is locked", whatever it reads. So the loader first borrows the exe's own
configuration connection, the one it read `GameCores` with a moment earlier, which reads its own
uncommitted rows: `*(*(*(exe+0x12fa2c8) + 0x54f0) + 8)`, found through the code at
`exe+0x249827`, `exe+0x64150` and `exe+0x861660` and only after every byte of it checks out
(`loader/ExeConfig.cpp`). The loader never releases that connection.

When the exe is another build, the loader falls back to a connection of its own: it tries every
`<n>` and keeps the database whose `GameCores` row selects the loader. That works only while no
such transaction is open. `bin\test\LoaderDbTest.exe` checks both cases.

## Writing a plugin

```cpp
#include "GameCorePlugin.h"

extern "C" __declspec(dllexport) int __cdecl GcmlAttach(const GcmlHost* host) {
	// host->gameCore: the real GameCore, initialised; no game object exists yet.
	// host->modDir, host->userDir, host->modId, host->Log(...)
	return 1;   // started
}

extern "C" __declspec(dllexport) void __cdecl GcmlDetach() {
	// GameCore is about to be unloaded: remove hooks into it.
}
```

Rules:

- Do nothing in `DllMain`. Start in `GcmlAttach`; it is called once per GameCore load.
- Your DLL is never unloaded, so code you patch into the exe stays valid. Turning your mod off takes
  effect after a restart for anything you patched into the exe.
- Keep settings and logs under `host->userDir` (a Workshop update can replace your mod folder).
- To check the loader against a new game version, run `bin\test\LoaderDbTest.exe` (built from
  `loader/test/`): it drives the game's DatabaseDLL and the loader without starting the game.

## Using a release in your mod

`powershell -File release.ps1` (after building, see below) assembles `bin\release\gcml\`:

| File | What you do with it |
|---|---|
| `GameCore_XP2_GCML_FinalRelease.dll` | ship it as `Binaries/Win64/GameCore_XP2_GCML_FinalRelease.dll` |
| `GameCorePlugin.h` | include it in your plugin |
| `GameCoreModLoader.sql` | ship it as a FrontEndActions `UpdateDatabase` (LoadOrder 1000000), with the EDIT line filled in |
| `loader-version.txt` | the loader build: the last value of the EDIT line |

Copy the folder into your mod's repository (as `gcml/`, say) and commit it. To move to a newer
loader, replace the folder with the new release's and rebuild your package. Nothing else changes:
the SQL tables and `GcmlAttach` stay compatible.

## Moving an existing fake-GameCore mod to GCML

- Assets Limit Fix style (all work in `DllMain`, every export forwarded): move the `DllMain` work
  into `GcmlAttach`, drop the fake GameCore, ship the loader and the SQL.
- Community Extension style (`DllCreateGameContext` wrapper that installs hooks): the body before
  `return base_DllCreateGameContext()` becomes `GcmlAttach`; `GetModuleHandle` of
  `GameCore_XP2_FinalRelease.dll` or `host->gameCore` replaces its own `LoadLibrary`.

Until a mod moves, the loader chains to it as the next core, as long as it ran first (lower
LoadOrder). Two old-style mods still exclude each other, as they do today.

## Versioning

- `GCML_LOADER_VERSION` (loader/LoaderVersion.h): raise it for every released change to the loader.
  The game uses the newest loader any enabled mod ships, so an old copy in someone else's mod never
  holds a fix back.
- `GCML_API_VERSION` (GameCorePlugin.h): fields are only added at the end of `GcmlHost`; check
  `host->size` before reading a field a later version added.
- The SQL tables never change: the first mod to run creates them, whatever version it ships. Later
  protocol versions add new tables.

## Limits and open points

- Gathering Storm (`GameCore_XP2`) only.
- The real GameCore is found at `DLC/Expansion2/Binaries/Win64/` relative to the game folder, which
  is three folders above the exe. `GCML_GAME_ROOT` overrides it.
- If the game ever builds the configuration database 256 times in one session, `<n>` wraps; the
  loader then takes the highest `<n>` that selects it, and logs when there is more than one.
- Mods are found in the DLC folder, the Workshop folder of the game's Steam library, the user's Mods
  folder (three levels deep) and the folder next to the loader's own mod.

## This repository

```
loader/        the loader DLL (GameCore_XP2_GCML_FinalRelease.dll); loader/test/ checks it against
               the installed game
sdk/           what plugins use: GameCorePlugin.h and GameCoreModLoader.sql
tools/         gen_gamecore_forwards.py, which writes the loader's export table
release.ps1    assembles bin\release\gcml\ for mods to vendor
```

Build (Visual Studio 2022+, x64, Release):
`msbuild loader\GameCoreModLoader.vcxproj -p:Configuration=Release -p:Platform=x64` writes
`bin\GameCore_XP2_GCML_FinalRelease.dll`. Its export table (`loader\GameCoreForwards.inc`) is
generated: `python tools/gen_gamecore_forwards.py <dumpbin.exe> > loader/GameCoreForwards.inc`.

Loader test: build `loader	est\GcmlFake.vcxproj` and `loader	est\LoaderDbTest.vcxproj`, copy the
game's `DatabaseDLL_FinalRelease_Orig.dll` (or `DatabaseDLL_FinalRelease.dll` without the Assets
Limit Fix) somewhere as `DatabaseDLL_FinalRelease.dll`, and run
`bin	est\LoaderDbTest.exe <that dll> <repo folder>` with `GCML_GAME_ROOT` set to the game folder.

Licensed AGPL-3.0-only.
