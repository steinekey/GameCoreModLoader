// SPDX-License-Identifier: MIT
#include "LoaderLog.h"
#include "Paths.h"
#include <Windows.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace LoaderLog {
	namespace {
		// The longest line kept, in bytes. Longer lines are cut.
		constexpr size_t kLineBytes = 1024;

		std::mutex gMutex;
		FILE* gFile = nullptr;
		bool gOpenFailed = false;

		// The log's path. GCML_LOG_FILE overrides it (the tests use that, since they run two
		// copies of this code in one process).
		std::wstring Path() {
			wchar_t overridePath[MAX_PATH];
			DWORD length = GetEnvironmentVariableW(L"GCML_LOG_FILE", overridePath, MAX_PATH);
			if (length > 0 && length < MAX_PATH) {
				return overridePath;
			}
			return Paths::UserDir() + L"Logs\\GameCoreModLoader.log";
		}

		// The loader DLL is unloaded and loaded again with every GameCore load, so the first copy
		// in a process starts a new file and the later ones append. This process environment
		// variable says a copy already started the file.
		const wchar_t kStartedVariable[] = L"GCML_LOG_STARTED";

		// Opens the log the first time a line is written. A plain C runtime file: the loader must
		// not depend on anything the game loads later.
		FILE* File() {
			if (!gFile && !gOpenFailed) {
				bool started = GetEnvironmentVariableW(kStartedVariable, nullptr, 0) > 0;
				gFile = _wfsopen(Path().c_str(), started ? L"a" : L"w", _SH_DENYWR);
				SetEnvironmentVariableW(kStartedVariable, L"1");
				gOpenFailed = gFile == nullptr;
			}
			return gFile;
		}
	}

	void Write(const char* fmt, ...) {
		char text[kLineBytes];
		va_list args;
		va_start(args, fmt);
		vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		WriteLine(text);
	}

	void __cdecl WriteLine(const char* line) {
		std::lock_guard<std::mutex> guard(gMutex);
		FILE* file = File();
		if (!file) {
			return;
		}
		SYSTEMTIME now;
		GetLocalTime(&now);
		fprintf(file, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond,
			now.wMilliseconds, line ? line : "");
		fflush(file);
	}
}
