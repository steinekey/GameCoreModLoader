// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>
#include <vector>

// Reads the front end's configuration database: the in-memory SQLite database that mods'
// FrontEndActions write to, and that the game read the GameCores row from just before it loaded
// this DLL. It is the only place that knows which mods are enabled for the game being started.
//
// Access goes through the game's own DatabaseDLL_FinalRelease.dll, because an in-memory shared
// database is only visible to connections made by the same copy of SQLite.
namespace ConfigDb {
	typedef std::vector<std::string> Row;

	// Opens the configuration database whose GameCores row selects this loader (DllPrefix
	// GameCore_XP2_GCML). False when there is none; the log says why.
	bool Open();
	// The <n> of file:configuration-<n> that Open found, or -1.
	int OpenIndex();
	// Runs a SELECT on the open database. Each row has `columns` texts; NULL reads as "".
	std::vector<Row> Select(const char* sql, int columns);
	void Close();
}
