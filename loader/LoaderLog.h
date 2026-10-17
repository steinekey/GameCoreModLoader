// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// GameCoreModLoader.log, in the Logs folder of the game's user folder (next to the game's own
// logs). A new file each process.
namespace LoaderLog {
	void Write(const char* fmt, ...);
	// For plugins (GcmlHost::Log): one line, no formatting.
	void __cdecl WriteLine(const char* line);
}
