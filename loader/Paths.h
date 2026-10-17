// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <string>

// The folders the loader works with. Every folder ends with a backslash.
namespace Paths {
	// The game's install folder (the one holding Base\ and DLC\). The exe lives in
	// Base\Binaries\<platform>\, three levels down. GCML_GAME_ROOT overrides it, for tests and
	// unusual layouts.
	const std::wstring& GameRoot();
	// The game's user folder: Documents\My Games\Sid Meier's Civilization VI\ (the Documents folder
	// may be redirected, for example to OneDrive).
	const std::wstring& UserDir();
	// The folder of the given module's file.
	std::wstring ModuleDir(void* module);
	// The path with its ".." parts resolved.
	std::wstring Resolve(const std::wstring& path);
	// Joins a folder and a relative path given with forward slashes, as mods write them in SQL.
	std::wstring Join(const std::wstring& dir, const std::string& relative);
	// UTF-8 / UTF-16 conversions for log lines and paths read from the database.
	std::wstring Widen(const std::string& text);
	std::string Narrow(const std::wstring& text);
}
