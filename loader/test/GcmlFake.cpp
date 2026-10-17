// SPDX-License-Identifier: AGPL-3.0-only
// A stand-in for every DLL the loader starts, for LoaderDbTest: a plugin (GcmlAttach, GcmlDetach),
// an old-style fake GameCore and the real GameCore (DllCreateGameContext, DllDestroyGameContext,
// and one ordinary export the loader forwards). The real GameCore refuses to initialise outside
// the game, so the test copies this DLL under each name into fake mod and game folders; each copy
// keeps its own state, which the test reads through the Fake* exports.
#include "GameCorePlugin.h"
#include <cwchar>

namespace {
	int gAttachCount = 0;
	int gDetachCount = 0;
	uint32_t gLoadCount = 0;
	HMODULE gGameCore = nullptr;
	wchar_t gModDir[MAX_PATH] = L"";
	void* gDestroyedContext = nullptr;
	// DllCreateGameContext returns its address, which differs between copies, so the test can
	// tell which copy the loader chained to.
	int gContextMarker = 0;
}

extern "C" {
	// Stands in for the real GameCore's exports that the loader forwards.
	__declspec(dllexport) void* __cdecl EXP_GetTelemetrySessionHash() {
		return &gContextMarker;
	}

	__declspec(dllexport) int __cdecl GcmlAttach(const GcmlHost* host) {
		gAttachCount++;
		gLoadCount = host->loadCount;
		gGameCore = host->gameCore;
		wcsncpy_s(gModDir, host->modDir, _TRUNCATE);
		host->Log("GcmlFake: attached");
		return 1;
	}

	__declspec(dllexport) void __cdecl GcmlDetach() {
		gDetachCount++;
	}

	__declspec(dllexport) void* __cdecl DllCreateGameContext() {
		return &gContextMarker;
	}

	__declspec(dllexport) void __cdecl DllDestroyGameContext(void* context) {
		gDestroyedContext = context;
	}

	__declspec(dllexport) int __cdecl FakeAttachCount() { return gAttachCount; }
	__declspec(dllexport) int __cdecl FakeDetachCount() { return gDetachCount; }
	__declspec(dllexport) uint32_t __cdecl FakeLoadCount() { return gLoadCount; }
	__declspec(dllexport) HMODULE __cdecl FakeGameCore() { return gGameCore; }
	__declspec(dllexport) const wchar_t* __cdecl FakeModDir() { return gModDir; }
	__declspec(dllexport) void* __cdecl FakeDestroyedContext() { return gDestroyedContext; }
	__declspec(dllexport) void* __cdecl FakeContext() { return &gContextMarker; }
}
