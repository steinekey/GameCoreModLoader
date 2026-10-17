// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

// The exe's own connection to the configuration database: the one it read the GameCores row with
// just before it loaded the loader.
//
// Why the loader needs it: when the game loads GameCore, the exe is in the middle of a write
// transaction on the configuration database that has changed its tables. SQLite's shared cache
// then refuses every other connection with "database schema is locked", so a connection of our
// own (ConfigDb::Open) cannot read GcmlPlugins at that moment. The exe's connection is the one
// holding the transaction, and it reads its own uncommitted rows.
namespace ExeConfig {
	// The exe's configuration connection (a DatabaseDLL connection object), or nullptr when the
	// exe is not the build this was decoded from, or it has no configuration database right now.
	// The connection belongs to the exe: use it, never release it.
	void* Connection();
}
