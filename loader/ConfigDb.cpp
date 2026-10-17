// SPDX-License-Identifier: MIT
#include "ConfigDb.h"
#include "LoaderLog.h"
#include <Windows.h>
#include <cstdio>

namespace ConfigDb {
	namespace {
		// ===== The game's database interface =====

		// DatabaseDLL_FinalRelease.dll wraps SQLite 3.13 in C++ interfaces. Its one useful export
		// creates a connection; everything else is a virtual call. The slots used here are the ones
		// the exe itself reads GameCores with (CivilizationVI_DX12.exe+0x249840 calls CreateQuery,
		// Step and ColumnText through them), so they are part of the ABI between the game's own
		// DLLs. When the Assets Limit Fix replaces the DLL, its copy forwards this export to the
		// original.
		const wchar_t kDatabaseDll[] = L"DatabaseDLL_FinalRelease.dll";
		// IDatabaseConnection* Database::CreateConnection(const char* name, int sqliteOpenFlags,
		// IDatabaseLogger* logger)
		const char kCreateConnectionExport[] =
			"?CreateConnection@Database@@"
			"YAPEAVIDatabaseConnection1@1@PEBDHPEAVIDatabaseLogger@1@@Z";
		typedef void* (__cdecl* CreateConnectionFn)(const char* name, int flags, void* logger);

		// SQLITE_OPEN_READONLY | SQLITE_OPEN_URI. The exe opens its read-only configuration
		// connections with the same flags.
		constexpr int kReadOnlyUriFlags = 0x41;

		// The configuration database is a shared in-memory database named
		// file:configuration-<n>?mode=memory&cache=shared. n is a byte that the exe increments
		// every time it builds the database again (CivilizationVI_DX12.exe+0xbd1f0), so every
		// value is tried. Opening a name nobody uses makes an empty database that goes away on
		// close.
		const char kNameFormat[] = "file:configuration-%d?mode=memory&cache=shared";
		constexpr int kNameCount = 256;

		// Virtual slots, counted from 0. Connections and queries both start with a vtable pointer.
		constexpr int kReleaseSlot = 1;      // both: void Release()
		// connection: IDatabaseQuery* CreateQuery(const char* sql, int length). The length is
		// SQLite's nByte: -1 reads up to the terminating zero, which is what the exe passes.
		constexpr int kCreateQuerySlot = 3;
		constexpr int kWholeString = -1;
		constexpr int kStepSlot = 4;         // query: bool Step(), true while a row is ready
		constexpr int kColumnTextSlot = 30;  // query: const char* ColumnText(int column)

		typedef void (__cdecl* ReleaseFn)(void* self);
		typedef void* (__cdecl* CreateQueryFn)(void* self, const char* sql, int length);
		typedef bool (__cdecl* StepFn)(void* self);
		typedef const char* (__cdecl* ColumnTextFn)(void* self, int column);

		template<typename Fn>
		Fn Slot(void* object, int slot) {
			void** vtable = *reinterpret_cast<void***>(object);
			return reinterpret_cast<Fn>(vtable[slot]);
		}

		// ===== Logger =====

		// The connection reports SQLite errors to a logger (slot 1: Write(message, level)) and
		// deletes it when the connection closes (slot 0: deleting destructor). Ours is one static
		// object, so its destructor does nothing.
		struct Logger {
			void* const* vtable;
		};
		void __cdecl LoggerDestroy(Logger*, int) {}
		void __cdecl LoggerWrite(Logger*, const char* message, int) {
			LoaderLog::Write("database: %s", message ? message : "");
		}
		void* const kLoggerVtable[2] = { (void*)&LoggerDestroy, (void*)&LoggerWrite };
		Logger gLogger = { kLoggerVtable };

		// ===== State =====

		CreateConnectionFn gCreateConnection = nullptr;
		void* gConnection = nullptr;
		// gConnection belongs to the exe: never release it.
		bool gBorrowed = false;
		int gIndex = -1;

		// ===== Helpers =====

		std::vector<Row> SelectOn(void* connection, const char* sql, int columns) {
			std::vector<Row> rows;
			void* query = Slot<CreateQueryFn>(connection, kCreateQuerySlot)(connection, sql,
				kWholeString);
			if (!query) {
				return rows;
			}
			while (Slot<StepFn>(query, kStepSlot)(query)) {
				Row row;
				for (int column = 0; column < columns; column++) {
					const char* text = Slot<ColumnTextFn>(query, kColumnTextSlot)(query, column);
					row.push_back(text ? text : "");
				}
				rows.push_back(row);
			}
			Slot<ReleaseFn>(query, kReleaseSlot)(query);
			return rows;
		}

		void Release(void* connection) {
			Slot<ReleaseFn>(connection, kReleaseSlot)(connection);
		}

		// True when this database is a configuration database that selects the loader. Asks
		// sqlite_master first, so that the empty databases made by probing unused names never run
		// a query that fails (and logs).
		bool SelectsLoader(void* connection) {
			const char* tableCheck =
				"SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
				"AND name IN ('GameCores', 'GcmlPlugins')";
			std::vector<Row> tables = SelectOn(connection, tableCheck, 1);
			if (tables.empty() || tables[0][0] != "2") {
				return false;
			}
			const char* coreCheck =
				"SELECT COUNT(*) FROM GameCores "
				"WHERE GameCore = 'Expansion2' AND DllPrefix = 'GameCore_XP2_GCML'";
			std::vector<Row> cores = SelectOn(connection, coreCheck, 1);
			return !cores.empty() && cores[0][0] == "1";
		}

		bool FindCreateConnection() {
			HMODULE database = GetModuleHandleW(kDatabaseDll);
			if (!database) {
				LoaderLog::Write("DatabaseDLL_FinalRelease.dll is not loaded");
				return false;
			}
			gCreateConnection =
				(CreateConnectionFn)GetProcAddress(database, kCreateConnectionExport);
			if (!gCreateConnection) {
				LoaderLog::Write("DatabaseDLL_FinalRelease.dll has no Database::CreateConnection");
				return false;
			}
			return true;
		}
	}

	bool Open() {
		Close();
		if (!gCreateConnection && !FindCreateConnection()) {
			return false;
		}
		int matches = 0;
		for (int index = 0; index < kNameCount; index++) {
			char name[96];
			snprintf(name, sizeof(name), kNameFormat, index);
			void* connection = gCreateConnection(name, kReadOnlyUriFlags, &gLogger);
			if (!connection) {
				continue;
			}
			if (!SelectsLoader(connection)) {
				Release(connection);
				continue;
			}
			matches++;
			// Keep the highest index: the exe counts up as it rebuilds the database.
			if (gConnection) {
				Release(gConnection);
			}
			gConnection = connection;
			gIndex = index;
		}
		if (!gConnection) {
			LoaderLog::Write("no configuration database selects the loader");
			return false;
		}
		if (matches > 1) {
			LoaderLog::Write("%d configuration databases select the loader; using the newest",
				matches);
		}
		return true;
	}

	bool OpenBorrowed(void* connection) {
		Close();
		if (!connection || !SelectsLoader(connection)) {
			return false;
		}
		gConnection = connection;
		gBorrowed = true;
		return true;
	}

	int OpenIndex() {
		return gIndex;
	}

	std::vector<Row> Select(const char* sql, int columns) {
		if (!gConnection) {
			return {};
		}
		return SelectOn(gConnection, sql, columns);
	}

	void Close() {
		if (gConnection && !gBorrowed) {
			Release(gConnection);
		}
		gConnection = nullptr;
		gBorrowed = false;
		gIndex = -1;
	}
}
