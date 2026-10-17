// SPDX-License-Identifier: AGPL-3.0-only
// Checks the loader against the installed game, without running the game:
//   1. loads the game's DatabaseDLL, builds a configuration database the way the front end does
//      (GameCores + the mods' FrontEndActions SQL), and runs ConfigDb and ModFolders on it;
//   2. sets up fake mods (a plugin mod that ships the loader, and an old-style fake GameCore that
//      took the row first), loads the loader DLL as the game would, and checks that it loads the
//      real GameCore, attaches the plugin, chains to the old-style core, detaches, unloads and
//      loads again.
// Run it after a game update to see whether the DatabaseDLL interface still matches.
//
// usage: LoaderDbTest.exe <DatabaseDLL_FinalRelease.dll to load> <repo folder>
//   GCML_GAME_ROOT must point at the game folder (ModFolders reads DLC and the Workshop from it).
//   The repo's bin\ must hold GameCore_XP2_GCML_FinalRelease.dll and test\GcmlFake.dll.
#include "ConfigDb.h"
#include "ModFolders.h"
#include "Paths.h"
#include <Windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
	const char kCreateConnectionExport[] =
		"?CreateConnection@Database@@YAPEAVIDatabaseConnection1@1@PEBDHPEAVIDatabaseLogger@1@@Z";
	const char kInitializeExport[] = "?Initialize@Database@@YA_NXZ";
	typedef void* (__cdecl* CreateConnectionFn)(const char* name, int flags, void* logger);
	typedef bool (__cdecl* InitializeFn)();
	// connection slot 5: bool ExecuteMultiple(const char* sql, int length), length < 0 = strlen
	typedef bool (__cdecl* ExecuteFn)(void* self, const char* sql, int length);
	constexpr int kExecuteMultipleSlot = 5;
	// SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, as the exe's writer uses.
	constexpr int kWriterFlags = 0x46;

	const char kAlfModId[] = "f1418488-253c-4aa4-aa5a-7e062f2a4c8c";
	const char kExpansion2Id[] = "4873eb62-8ccc-4574-b784-dda455e74e68";

	int gFailures = 0;

	void Check(bool ok, const char* what) {
		printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
		if (!ok) {
			gFailures++;
		}
	}

	std::string ReadFile(const std::string& path) {
		std::ifstream file(path, std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		return text.str();
	}

	std::string Replace(std::string text, const std::string& from, const std::string& to) {
		size_t at = text.find(from);
		if (at != std::string::npos) {
			text.replace(at, from.size(), to);
		}
		return text;
	}

	bool Execute(void* connection, const std::string& sql) {
		void** vtable = *reinterpret_cast<void***>(connection);
		return reinterpret_cast<ExecuteFn>(vtable[kExecuteMultipleSlot])(connection, sql.c_str(), -1);
	}

	// One mod's copy of the protocol SQL.
	std::string ProtocolSql(const std::string& repo, const char* modId, const char* file, int version) {
		std::string sql = ReadFile(repo + "\\sdk\\GameCoreModLoader.sql");
		char values[256];
		snprintf(values, sizeof(values), "VALUES('%s', '%s', 0, %d);", modId, file, version);
		return Replace(sql, "VALUES('EDIT-your-mod-id', 'Binaries/Win64/EDIT-your-plugin.dll', 0, 1);",
			values);
	}

	std::string Core(void) {
		std::vector<ConfigDb::Row> rows = ConfigDb::Select(
			"SELECT PackageId || '|' || DllPrefix FROM GameCores WHERE GameCore = 'Expansion2'", 1);
		return rows.empty() ? "" : rows[0][0];
	}

	// Builds configuration database <index> with the stock rows and the given mod SQL, in order.
	void* Build(CreateConnectionFn create, int index, const std::string& stockSql,
		const std::vector<std::string>& modSql) {
		char name[96];
		snprintf(name, sizeof(name), "file:configuration-%d?mode=memory&cache=shared", index);
		void* writer = create(name, kWriterFlags, nullptr);
		Check(writer != nullptr, "writer connection opens");
		Check(Execute(writer, stockSql), "stock GameCores/Rulesets rows");
		for (const std::string& sql : modSql) {
			Check(Execute(writer, sql), "mod SQL runs");
		}
		return writer;
	}

	void Release(void* connection) {
		void** vtable = *reinterpret_cast<void***>(connection);
		reinterpret_cast<void(__cdecl*)(void*)>(vtable[1])(connection);
	}

	// ===== End to end =====

	const char kPluginModId[] = "dddd0000-0000-0000-0000-00000000000d";
	const char kOldCoreModId[] = "eeee0000-0000-0000-0000-00000000000e";

	typedef void* (__cdecl* CreateContextFn)();
	typedef void (__cdecl* DestroyContextFn)(void*);

	void WriteText(const std::wstring& path, const std::string& text) {
		std::ofstream file(path, std::ios::binary);
		file << text;
	}

	// <mods>\<name>\<name>.modinfo with the id, and <mods>\<name>\Binaries\Win64\.
	std::wstring MakeMod(const std::wstring& mods, const std::wstring& name, const char* id) {
		std::wstring dir = mods + name + L"\\";
		CreateDirectoryW(dir.c_str(), nullptr);
		CreateDirectoryW((dir + L"Binaries").c_str(), nullptr);
		CreateDirectoryW((dir + L"Binaries\\Win64").c_str(), nullptr);
		WriteText(dir + name + L".modinfo",
			std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Mod id=\"") + id + "\" version=\"1\">\n</Mod>\n");
		return dir;
	}

	template<typename Fn>
	Fn Export(HMODULE module, const char* name) {
		return module ? (Fn)GetProcAddress(module, name) : nullptr;
	}

	int FakeCount(HMODULE fake, const char* name) {
		auto count = Export<int(__cdecl*)()>(fake, name);
		return count ? count() : -1;
	}

	void* ContextOf(HMODULE fake) {
		auto context = Export<void*(__cdecl*)()>(fake, "FakeContext");
		return context ? context() : nullptr;
	}

	// The fake mods and game folder of the end-to-end test.
	struct Setup {
		std::wstring pluginMod;
		std::wstring loaderPath;
		std::wstring pluginPath;
		std::wstring oldCorePath;
		std::wstring realCorePath;
		std::wstring log;
	};

	// One GameCore load as the game does it: load the loader, create a context, destroy it,
	// unload. expectOldCore: DllCreateGameContext should reach the old-style core, not the real one.
	void RunLoad(const Setup& setup, uint32_t load, bool expectOldCore) {
		printf("-- end to end, GameCore load %u, %s\n", load,
			expectOldCore ? "an old-style core took the row first" : "no old-style core");
		HMODULE loader = LoadLibraryW(setup.loaderPath.c_str());
		Check(loader != nullptr, "the game would load the loader");
		HMODULE realCore = GetModuleHandleW(setup.realCorePath.c_str());
		Check(realCore != nullptr, "its DllMain loaded the real GameCore");
		auto forwarded = Export<void*>(loader, "EXP_GetTelemetrySessionHash");
		Check(forwarded && forwarded == Export<void*>(realCore, "EXP_GetTelemetrySessionHash"),
			"a forwarded export resolves into the real GameCore");

		auto createContext = Export<CreateContextFn>(loader, "DllCreateGameContext");
		auto destroyContext = Export<DestroyContextFn>(loader, "DllDestroyGameContext");
		void* context = createContext ? createContext() : nullptr;
		HMODULE plugin = GetModuleHandleW(setup.pluginPath.c_str());
		HMODULE oldCore = GetModuleHandleW(setup.oldCorePath.c_str());
		HMODULE chained = expectOldCore ? oldCore : realCore;
		Check(chained && context == ContextOf(chained),
			expectOldCore ? "DllCreateGameContext chained to the old-style core"
			: "DllCreateGameContext chained to the real GameCore");
		Check(expectOldCore == (oldCore != nullptr), "the old-style core is loaded only when it took the row");

		Check(FakeCount(plugin, "FakeAttachCount") == (int)load, "plugin attached once per load");
		auto loadCount = Export<uint32_t(__cdecl*)()>(plugin, "FakeLoadCount");
		Check(loadCount && loadCount() == load, "host.loadCount counts the loads");
		auto gameCore = Export<HMODULE(__cdecl*)()>(plugin, "FakeGameCore");
		Check(gameCore && gameCore() == realCore, "host.gameCore is the real GameCore");
		auto modDir = Export<const wchar_t*(__cdecl*)()>(plugin, "FakeModDir");
		Check(modDir && _wcsicmp(modDir(), setup.pluginMod.c_str()) == 0, "host.modDir is the plugin's mod folder");
		Check(FakeCount(chained, "FakeAttachCount") == 0, "the chained core is not attached as a plugin");

		if (destroyContext) {
			destroyContext(context);
		}
		auto destroyed = Export<void*(__cdecl*)()>(chained, "FakeDestroyedContext");
		Check(destroyed && destroyed() == context, "DllDestroyGameContext reached the same core");
		Check(FakeCount(plugin, "FakeDetachCount") == (int)load, "plugin detached");

		FreeLibrary(loader);
		Check(GetModuleHandleW(setup.realCorePath.c_str()) == nullptr, "unloading the loader unloads the real GameCore");
		Check(GetModuleHandleW(setup.oldCorePath.c_str()) == nullptr, "and the old-style core");
		Check(GetModuleHandleW(setup.pluginPath.c_str()) != nullptr, "the plugin stays loaded (pinned)");
	}

	void EndToEnd(CreateConnectionFn create, const std::string& repo, const std::string& stockSql) {
		wchar_t temp[MAX_PATH];
		GetTempPathW(MAX_PATH, temp);
		std::wstring work = std::wstring(temp) + L"gcml_e2e_" + std::to_wstring(GetTickCount64()) + L"\\";
		std::wstring mods = work + L"Mods\\";
		std::wstring game = work + L"Game\\";
		for (const std::wstring& dir : { work, mods, game, game + L"DLC\\" }) {
			CreateDirectoryW(dir.c_str(), nullptr);
		}
		std::wstring bin = Paths::Widen(repo) + L"\\bin\\";
		std::wstring fake = bin + L"test\\GcmlFake.dll";

		Setup setup;
		setup.pluginMod = MakeMod(mods, L"PluginMod", kPluginModId);
		setup.loaderPath = setup.pluginMod + L"Binaries\\Win64\\GameCore_XP2_GCML_FinalRelease.dll";
		setup.pluginPath = setup.pluginMod + L"Binaries\\Win64\\Fake.dll";
		CopyFileW((bin + L"GameCore_XP2_GCML_FinalRelease.dll").c_str(), setup.loaderPath.c_str(), FALSE);
		CopyFileW(fake.c_str(), setup.pluginPath.c_str(), FALSE);
		setup.oldCorePath = MakeMod(mods, L"OldCoreMod", kOldCoreModId) +
			L"Binaries\\Win64\\GameCore_XP2_OLD_FinalRelease.dll";
		CopyFileW(fake.c_str(), setup.oldCorePath.c_str(), FALSE);
		// The real GameCore refuses to initialise outside the game (error 1114), so the fake game
		// folder gets a fake one, in the place and under the id of the Expansion2 DLC.
		setup.realCorePath = MakeMod(game + L"DLC\\", L"Expansion2", kExpansion2Id) +
			L"Binaries\\Win64\\GameCore_XP2_FinalRelease.dll";
		CopyFileW(fake.c_str(), setup.realCorePath.c_str(), FALSE);
		setup.log = work + L"GameCoreModLoader.log";

		// Read by the loader DLL's own copy of Paths and LoaderLog, on first use.
		SetEnvironmentVariableW(L"GCML_GAME_ROOT", game.c_str());
		SetEnvironmentVariableW(L"GCML_LOG_FILE", setup.log.c_str());

		std::string pluginSql = ProtocolSql(repo, kPluginModId, "Binaries/Win64/Fake.dll", 1);
		std::string oldCoreSql = std::string("UPDATE GameCores SET PackageId = '") + kOldCoreModId +
			"', DllPrefix = 'GameCore_XP2_OLD' WHERE GameCore = 'Expansion2' AND DllPrefix = 'GameCore_XP2';";
		void* withOldCore = Build(create, 11, stockSql, { oldCoreSql, pluginSql });
		RunLoad(setup, 1, true);
		RunLoad(setup, 2, true);
		Release(withOldCore);
		void* plain = Build(create, 12, stockSql, { pluginSql });
		RunLoad(setup, 3, false);
		Release(plain);

		printf("-- the loader's log\n%s", ReadFile(Paths::Narrow(setup.log)).c_str());
	}
}

int main(int argc, char** argv) {
	if (argc < 3) {
		printf("usage: LoaderDbTest <DatabaseDLL_FinalRelease.dll> <repo folder>\n");
		return 2;
	}
	HMODULE database = LoadLibraryA(argv[1]);
	Check(database != nullptr, "DatabaseDLL loads");
	if (!database) {
		return 1;
	}
	auto initialize = (InitializeFn)GetProcAddress(database, kInitializeExport);
	auto create = (CreateConnectionFn)GetProcAddress(database, kCreateConnectionExport);
	Check(initialize && create && initialize(), "Database::Initialize");
	std::string repo = argv[2];

	const std::string stockSql =
		"CREATE TABLE GameCores(GameCore TEXT NOT NULL, PackageId TEXT, DllPrefix TEXT NOT NULL, "
		"PRIMARY KEY(GameCore));"
		"CREATE TABLE Rulesets(RulesetType TEXT NOT NULL PRIMARY KEY, GameCore TEXT);"
		"INSERT INTO GameCores VALUES('Base', NULL, 'GameCore_Base');"
		"INSERT INTO GameCores VALUES('Expansion1', '1B28771A-C749-434B-9053-D1380C553DE9', 'GameCore_XP1');"
		"INSERT INTO GameCores VALUES('Expansion2', '4873eb62-8ccc-4574-b784-dda455e74e68', 'GameCore_XP2');"
		"INSERT INTO Rulesets VALUES('RULESET_EXPANSION_2', 'Expansion2');";
	const std::string alfSql =
		"UPDATE GameCores SET PackageId = 'f1418488-253c-4aa4-aa5a-7e062f2a4c8c', "
		"DllPrefix = 'GameCore_XP2_ALF' WHERE GameCore = 'Expansion2' AND DllPrefix = 'GameCore_XP2';";
	const std::string modA = ProtocolSql(repo, "aaaa0000-0000-0000-0000-000000000001", "Binaries/Win64/A.dll", 1);
	const std::string modB = ProtocolSql(repo, "bbbb0000-0000-0000-0000-000000000002", "Binaries/Win64/B.dll", 1);
	const std::string modNewer = ProtocolSql(repo, "cccc0000-0000-0000-0000-000000000003", "Binaries/Win64/C.dll", 2);

	printf("-- no configuration database yet\n");
	Check(!ConfigDb::Open(), "Open finds nothing");

	printf("-- ALF first, then mods A and B\n");
	void* first = Build(create, 7, stockSql, { alfSql, modA, modB });
	Check(ConfigDb::Open(), "Open finds the database");
	Check(ConfigDb::OpenIndex() == 7, "it is configuration-7");
	Check(Core() == "aaaa0000-0000-0000-0000-000000000001|GameCore_XP2_GCML", "row -> loader of the lowest mod id");
	auto next = ConfigDb::Select("SELECT PackageId, DllPrefix FROM GcmlNextCore", 2);
	Check(next.size() == 1 && next[0][0] == kAlfModId && next[0][1] == "GameCore_XP2_ALF", "next core is ALF");
	auto plugins = ConfigDb::Select("SELECT ModId, File FROM GcmlPlugins ORDER BY LoadOrder, ModId, File", 2);
	Check(plugins.size() == 2 && plugins[0][1] == "Binaries/Win64/A.dll" && plugins[1][1] == "Binaries/Win64/B.dll",
		"both plugins, in order");
	ConfigDb::Close();
	Release(first);

	printf("-- the same mods in the other order, and a newer loader\n");
	void* second = Build(create, 9, stockSql, { modNewer, modB, modA });
	Check(ConfigDb::Open() && ConfigDb::OpenIndex() == 9, "Open finds configuration-9 (the other one is gone)");
	Check(Core() == "cccc0000-0000-0000-0000-000000000003|GameCore_XP2_GCML", "row -> the newest loader");
	next = ConfigDb::Select("SELECT PackageId, DllPrefix FROM GcmlNextCore", 2);
	Check(next.size() == 1 && next[0][1] == "GameCore_XP2", "next core is the real one");
	ConfigDb::Close();
	Release(second);

	printf("-- the game's state when it loads GameCore: a schema change not yet committed\n");
	void* busy = Build(create, 11, stockSql, { alfSql, modA });
	Check(Execute(busy, "BEGIN; CREATE TABLE GcmlTestUncommitted(Unused TEXT);"),
		"the writer starts a transaction that changes the schema");
	Check(!ConfigDb::Open(), "a connection of our own cannot read it (schema locked)");
	Check(ConfigDb::OpenBorrowed(busy), "the writer's own connection can");
	plugins = ConfigDb::Select("SELECT File FROM GcmlPlugins", 1);
	Check(plugins.size() == 1 && plugins[0][0] == "Binaries/Win64/A.dll", "and reads the plugins");
	ConfigDb::Close();
	Check(Execute(busy, "ROLLBACK;"), "Close leaves a borrowed connection open");
	Release(busy);

	printf("-- mod folders\n");
	std::wstring dlc = ModFolders::Find(kExpansion2Id);
	printf("     Expansion2 -> %s\n", Paths::Narrow(dlc).c_str());
	Check(dlc.find(L"DLC\\Expansion2\\") != std::wstring::npos, "Expansion2 is the DLC folder");
	std::wstring alf = ModFolders::Find(kAlfModId);
	printf("     ALF -> %s\n", Paths::Narrow(alf).c_str());
	Check(!alf.empty(), "ALF's folder is found");
	Check(ModFolders::Find("no-such-mod").empty(), "unknown id -> empty");

	EndToEnd(create, repo, stockSql);

	printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
	return gFailures ? 1 : 0;
}
