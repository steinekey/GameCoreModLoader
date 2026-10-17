// SPDX-License-Identifier: AGPL-3.0-only
#include "Paths.h"
#include <Windows.h>
#include <ShlObj.h>

namespace Paths {
	namespace {
		// From the exe's folder (Base\Binaries\Win64Steam) up to the game folder.
		const wchar_t kExeToGameRoot[] = L"..\\..\\..\\";
		// Inside Documents.
		const wchar_t kUserSubfolder[] = L"\\My Games\\Sid Meier's Civilization VI\\";

		std::wstring WithBackslash(std::wstring dir) {
			if (!dir.empty() && dir.back() != L'\\') {
				dir += L'\\';
			}
			return dir;
		}

		std::wstring FindGameRoot() {
			wchar_t overrideDir[MAX_PATH];
			DWORD length = GetEnvironmentVariableW(L"GCML_GAME_ROOT", overrideDir, MAX_PATH);
			if (length > 0 && length < MAX_PATH) {
				return WithBackslash(Resolve(overrideDir));
			}
			return WithBackslash(Resolve(ModuleDir(nullptr) + kExeToGameRoot));
		}

		std::wstring FindUserDir() {
			PWSTR documents = nullptr;
			std::wstring dir;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
				dir = std::wstring(documents) + kUserSubfolder;
			}
			CoTaskMemFree(documents);
			return dir;
		}
	}

	const std::wstring& GameRoot() {
		static const std::wstring root = FindGameRoot();
		return root;
	}

	const std::wstring& UserDir() {
		static const std::wstring dir = FindUserDir();
		return dir;
	}

	std::wstring ModuleDir(void* module) {
		wchar_t path[MAX_PATH];
		DWORD length = GetModuleFileNameW((HMODULE)module, path, MAX_PATH);
		if (length == 0 || length >= MAX_PATH) {
			return L"";
		}
		std::wstring dir(path, length);
		size_t slash = dir.find_last_of(L'\\');
		return slash == std::wstring::npos ? L"" : dir.substr(0, slash + 1);
	}

	// Resolves "..\" parts, so that log lines show a plain path.
	std::wstring Resolve(const std::wstring& path) {
		wchar_t full[MAX_PATH * 2];
		DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH * 2, full, nullptr);
		if (length == 0 || length >= MAX_PATH * 2) {
			return path;
		}
		return full;
	}

	std::wstring Join(const std::wstring& dir, const std::string& relative) {
		std::wstring path = dir + Widen(relative);
		for (wchar_t& character : path) {
			if (character == L'/') {
				character = L'\\';
			}
		}
		return path;
	}

	std::wstring Widen(const std::string& text) {
		if (text.empty()) {
			return L"";
		}
		int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
		std::wstring wide(length, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wide[0], length);
		return wide;
	}

	std::string Narrow(const std::wstring& text) {
		if (text.empty()) {
			return "";
		}
		int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0,
			nullptr, nullptr);
		std::string narrow(length, '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &narrow[0], length,
			nullptr, nullptr);
		return narrow;
	}
}
