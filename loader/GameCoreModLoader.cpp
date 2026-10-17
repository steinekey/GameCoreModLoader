// SPDX-License-Identifier: MIT
#include "ConfigDb.h"
#include "ExeConfig.h"
#include "GameCorePlugin.h"
#include "LoaderLog.h"
#include "LoaderVersion.h"
#include "ModFolders.h"
#include "Paths.h"
#include <Windows.h>
#include <cstdio>
#include <string>
#include <vector>

// The GameCore Mod Loader: the shared fake GameCore that every native mod ships (see
// sdk/GameCorePlugin.h and sdk/README.md).
//
// The game loads it as Binaries/Win64/GameCore_XP2_GCML_FinalRelease.dll of the mod the GameCores
// row names. Every export of the real GameCore is forwarded to it (GameCoreForwards.inc), except
// the two the exe calls by name: DllCreateGameContext and DllDestroyGameContext.
//
//   DllMain               loads the real GameCore, so the forwarded exports resolve.
//   DllCreateGameContext  reads the enabled plugins from the configuration database, loads and
//                         attaches them, loads the next core (an old-style fake GameCore that took
//                         the row before us, if any), and calls its DllCreateGameContext.
//   DllDestroyGameContext calls the next core's, then detaches the plugins.
//   DLL_PROCESS_DETACH    releases the cores. Plugins stay loaded: they may have patched the exe.
//
// The exe unloads this DLL and the real GameCore whenever a save is loaded, and loads them again.
#include "GameCoreForwards.inc"

namespace {
	// ===== The real GameCore =====

	// The real core, relative to the game folder. DllPrefix GameCore_XP2 in the Expansion2 DLC.
	const wchar_t kRealCoreRelative[] =
		L"DLC\\Expansion2\\Binaries\\Win64\\GameCore_XP2_FinalRelease.dll";
	// Package and prefix of the real core in GameCores; GcmlNextCore holds them when no old-style
	// fake GameCore took the row first.
	const char kRealCorePrefix[] = "GameCore_XP2";
	// How the game turns a GameCores row into a file: <package folder>Binaries/Win64/<DllPrefix>
	// _FinalRelease.dll (CivilizationVI_DX12.exe+0x24993e).
	const char kCoreFolder[] = "Binaries/Win64/";
	const char kCoreSuffix[] = "_FinalRelease.dll";

	typedef void* (__cdecl* CreateContextFn)();
	typedef void (__cdecl* DestroyContextFn)(void* context);

	// ===== One plugin =====

	struct Plugin {
		std::string modId;
		std::string file;
		std::wstring modDir;
		HMODULE module = nullptr;
		GcmlAttachFn attach = nullptr;
		GcmlDetachFn detach = nullptr;
		// GcmlAttach returned nonzero for the current load of GameCore.
		bool attached = false;
	};

	// ===== State =====

	HMODULE gSelf = nullptr;
	HMODULE gRealCore = nullptr;
	DWORD gRealCoreError = 0;
	// The core DllCreateGameContext chains to: gRealCore, or an old-style fake GameCore.
	HMODULE gNextCore = nullptr;
	CreateContextFn gNextCreate = nullptr;
	DestroyContextFn gNextDestroy = nullptr;
	// The plugins registered for this load of GameCore. Rebuilt on every load, since this DLL is
	// unloaded with GameCore; the plugin modules themselves stay pinned.
	std::vector<Plugin> gPlugins;
	// StartForThisLoad ran (once per load of this DLL), and the load number it got.
	bool gStarted = false;
	uint32_t gLoadCount = 0;
	// The plugins are attached: from DllCreateGameContext to DllDestroyGameContext.
	bool gAttached = false;

	// Counts GameCore loads in this process (GcmlHost::loadCount). This DLL's globals start over
	// on every load, so the count is kept in an environment variable of the process.
	const wchar_t kLoadCountVariable[] = L"GCML_LOAD_COUNT";
	// The loader sits in <mod folder>\Binaries\Win64\.
	constexpr int kLoaderDepthInMod = 2;

	uint32_t NextLoadCount() {
		wchar_t text[16] = L"0";
		GetEnvironmentVariableW(kLoadCountVariable, text, 16);
		uint32_t count = (uint32_t)_wtoi(text) + 1;
		swprintf_s(text, L"%u", count);
		SetEnvironmentVariableW(kLoadCountVariable, text);
		return count;
	}

	// The folder above dir (both with a trailing backslash), or dir itself at a drive root.
	std::wstring ParentDir(const std::wstring& dir) {
		size_t slash = dir.find_last_of(L'\\', dir.size() >= 2 ? dir.size() - 2 : 0);
		return slash == std::wstring::npos ? dir : dir.substr(0, slash + 1);
	}

	// ===== Loading the cores =====

	// The real core, by its path in the game folder. Runs in DllMain: LoadLibrary there is what a
	// static import would do, and the forwarded exports need the module in place before the exe
	// asks for any of them.
	void LoadRealCore() {
		std::wstring path = Paths::GameRoot() + kRealCoreRelative;
		gRealCore = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!gRealCore) {
			gRealCoreError = GetLastError();
		}
	}

	// The file the game would load for a GameCores row, or empty if the package is not found.
	std::wstring CoreFile(const std::string& packageId, const std::string& dllPrefix) {
		std::wstring folder = ModFolders::Find(packageId);
		if (folder.empty()) {
			return L"";
		}
		return Paths::Join(folder, std::string(kCoreFolder) + dllPrefix + kCoreSuffix);
	}

	// Picks the core to chain to from GcmlNextCore: the real core, or the old-style fake GameCore
	// that held the row before the first GCML mod ran.
	HMODULE LoadNextCore() {
		std::vector<ConfigDb::Row> rows = ConfigDb::Select(
			"SELECT PackageId, DllPrefix FROM GcmlNextCore WHERE GameCore = 'Expansion2'", 2);
		if (rows.empty() || rows[0][1] == kRealCorePrefix) {
			return gRealCore;
		}
		const std::string& packageId = rows[0][0];
		const std::string& dllPrefix = rows[0][1];
		std::wstring file = CoreFile(packageId, dllPrefix);
		if (file.empty()) {
			LoaderLog::Write(
				"next core %s (package %s): package folder not found; using the real core",
				dllPrefix.c_str(), packageId.c_str());
			return gRealCore;
		}
		HMODULE core = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!core) {
			LoaderLog::Write("next core %s: LoadLibrary failed (%lu); using the real core",
				Paths::Narrow(file).c_str(), GetLastError());
			return gRealCore;
		}
		LoaderLog::Write("chaining to %s", Paths::Narrow(file).c_str());
		return core;
	}

	bool ResolveNextCore() {
		gNextCore = LoadNextCore();
		gNextCreate = (CreateContextFn)GetProcAddress(gNextCore, "DllCreateGameContext");
		gNextDestroy = (DestroyContextFn)GetProcAddress(gNextCore, "DllDestroyGameContext");
		if (gNextCreate && gNextDestroy) {
			return true;
		}
		LoaderLog::Write("the next core has no DllCreateGameContext/DllDestroyGameContext");
		if (gNextCore != gRealCore) {
			FreeLibrary(gNextCore);
			gNextCore = gRealCore;
			gNextCreate = (CreateContextFn)GetProcAddress(gRealCore, "DllCreateGameContext");
			gNextDestroy = (DestroyContextFn)GetProcAddress(gRealCore, "DllDestroyGameContext");
		}
		return gNextCreate && gNextDestroy;
	}

	// ===== Plugins =====

	// Loads a plugin and pins it: a plugin may patch the exe, and that code must outlive every
	// unload of GameCore. Loading a pinned plugin again returns the same module.
	bool LoadPlugin(Plugin& plugin) {
		plugin.modDir = ModFolders::Find(plugin.modId);
		if (plugin.modDir.empty()) {
			LoaderLog::Write("plugin %s of mod %s: mod folder not found", plugin.file.c_str(),
				plugin.modId.c_str());
			return false;
		}
		std::wstring path = Paths::Join(plugin.modDir, plugin.file);
		plugin.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!plugin.module) {
			LoaderLog::Write("plugin %s: LoadLibrary failed (%lu)", Paths::Narrow(path).c_str(),
				GetLastError());
			return false;
		}
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
			(LPCWSTR)plugin.module, &pinned);
		plugin.attach = (GcmlAttachFn)GetProcAddress(plugin.module, GCML_ATTACH_EXPORT);
		plugin.detach = (GcmlDetachFn)GetProcAddress(plugin.module, GCML_DETACH_EXPORT);
		if (!plugin.attach) {
			LoaderLog::Write("plugin %s: no %s export", Paths::Narrow(path).c_str(),
				GCML_ATTACH_EXPORT);
			return false;
		}
		LoaderLog::Write("plugin %s loaded", Paths::Narrow(path).c_str());
		return true;
	}

	// The enabled plugins, in their load order.
	std::vector<Plugin> ReadPlugins() {
		std::vector<Plugin> plugins;
		std::vector<ConfigDb::Row> rows = ConfigDb::Select(
			"SELECT ModId, File FROM GcmlPlugins ORDER BY LoadOrder, ModId, File", 2);
		for (const ConfigDb::Row& row : rows) {
			Plugin plugin;
			plugin.modId = row[0];
			plugin.file = row[1];
			plugins.push_back(plugin);
		}
		return plugins;
	}

	void AttachPlugins() {
		std::wstring userDir = Paths::UserDir();
		for (Plugin& plugin : gPlugins) {
			if (!plugin.attach) {
				continue;
			}
			GcmlHost host = {};
			host.size = sizeof(GcmlHost);
			host.apiVersion = GCML_API_VERSION;
			host.loaderVersion = GCML_LOADER_VERSION;
			host.loadCount = gLoadCount;
			host.gameCore = gRealCore;
			host.modDir = plugin.modDir.c_str();
			host.userDir = userDir.c_str();
			host.modId = plugin.modId.c_str();
			host.Log = &LoaderLog::WriteLine;
			plugin.attached = plugin.attach(&host) != 0;
			LoaderLog::Write("plugin %s %s", plugin.file.c_str(),
				plugin.attached ? "attached" : "did not attach");
		}
		gAttached = true;
	}

	void DetachPlugins() {
		// Reverse order, so a plugin that hooks on top of another comes off first.
		for (auto it = gPlugins.rbegin(); it != gPlugins.rend(); ++it) {
			if (it->attached && it->detach) {
				it->detach();
			}
			it->attached = false;
		}
		gAttached = false;
	}

	// The first DllCreateGameContext of this load: finds and loads the plugins and the next core.
	void StartForThisLoad() {
		gStarted = true;
		gLoadCount = NextLoadCount();
		std::wstring loaderDir = Paths::ModuleDir(gSelf);
		LoaderLog::Write("GameCore Mod Loader %d (API %d), GameCore load %u, from %s",
			GCML_LOADER_VERSION, GCML_API_VERSION, gLoadCount, Paths::Narrow(loaderDir).c_str());
		if (!gRealCore) {
			LoaderLog::Write("the real GameCore did not load (%lu): %s", gRealCoreError,
				Paths::Narrow(Paths::GameRoot() + kRealCoreRelative).c_str());
			return;
		}
		// The loader's own mod folder is in the folder that holds the other mods (the Workshop
		// folder or Mods), which is where the plugins are most likely to be.
		std::wstring ownMod = Paths::ModuleDir(gSelf);
		for (int level = 0; level < kLoaderDepthInMod; level++) {
			ownMod = ParentDir(ownMod);
		}
		ModFolders::AddRoot(ParentDir(ownMod));
		ModFolders::AllowRescan();

		// The exe's own connection first: it reads the rows of the transaction the exe has open
		// right now, which a connection of our own cannot (see ExeConfig.h).
		bool haveDatabase = ConfigDb::OpenBorrowed(ExeConfig::Connection());
		if (haveDatabase) {
			LoaderLog::Write("configuration database: the exe's connection");
		}
		else {
			haveDatabase = ConfigDb::Open();
			if (haveDatabase) {
				LoaderLog::Write("configuration database %d", ConfigDb::OpenIndex());
			}
		}
		if (haveDatabase) {
			gPlugins = ReadPlugins();
			LoaderLog::Write("%zu plugins registered", gPlugins.size());
			for (Plugin& plugin : gPlugins) {
				LoadPlugin(plugin);
			}
		}
		if (!ResolveNextCore()) {
			LoaderLog::Write("cannot start the game context");
		}
		ConfigDb::Close();
	}
}

extern "C" {
	// The exe calls this right after loading the GameCores row's DLL (outside the loader lock).
	void* __cdecl GcmlCreateGameContext() {
		if (!gStarted) {
			StartForThisLoad();
		}
		if (!gAttached && gRealCore) {
			AttachPlugins();
		}
		if (!gNextCreate) {
			return nullptr;
		}
		return gNextCreate();
	}

	void __cdecl GcmlDestroyGameContext(void* context) {
		if (gNextDestroy) {
			gNextDestroy(context);
		}
		DetachPlugins();
	}
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		gSelf = self;
		DisableThreadLibraryCalls(self);
		LoadRealCore();
	}
	// reserved is non-null when the process is exiting: then nothing is released, the other
	// threads are already gone and the plugins must not touch the game any more.
	else if (reason == DLL_PROCESS_DETACH && !reserved) {
		if (gAttached) {
			DetachPlugins();
		}
		if (gNextCore && gNextCore != gRealCore) {
			FreeLibrary(gNextCore);
		}
		if (gRealCore) {
			FreeLibrary(gRealCore);
		}
	}
	return TRUE;
}
