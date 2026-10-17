// SPDX-License-Identifier: AGPL-3.0-only
#include "ModFolders.h"
#include "LoaderLog.h"
#include "Paths.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

namespace ModFolders {
	namespace {
		// The Steam Workshop folder of the game, from the game folder
		// (steamapps\common\<game>\ -> steamapps\workshop\content\<app id>\). Steam keeps Workshop
		// items in the same library as the game.
		const wchar_t kWorkshopFromGameRoot[] = L"..\\..\\workshop\\content\\289070\\";
		// How deep under a root a .modinfo may sit. Mods folders are sometimes nested
		// (Mods\Author\Mod\x.modinfo).
		constexpr int kModsDepth = 3;
		constexpr int kFlatDepth = 1;
		// The start of a .modinfo that is read looking for the id. The <Mod> tag is at the top.
		constexpr DWORD kModinfoHeadBytes = 4096;

		struct Root {
			std::wstring dir;
			int depth;
		};

		std::vector<std::wstring> gExtraRoots;
		// Lower-case id -> folder.
		std::map<std::string, std::wstring> gFolders;
		bool gScanned = false;
		bool gRescanned = false;

		std::string Lower(std::string text) {
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char character) { return (char)std::tolower(character); });
			return text;
		}

		// The id attribute of the <Mod> element, from the start of a .modinfo. Empty when missing.
		std::string ModIdOf(const std::string& head) {
			std::string lower = Lower(head);
			size_t tag = lower.find("<mod ");
			if (tag == std::string::npos) {
				return "";
			}
			size_t tagEnd = lower.find('>', tag);
			size_t attribute = lower.find(" id=", tag);
			bool pastTag = tagEnd != std::string::npos && attribute > tagEnd;
			if (attribute == std::string::npos || pastTag) {
				return "";
			}
			size_t quote = attribute + 4;
			if (quote >= head.size() || (head[quote] != '"' && head[quote] != '\'')) {
				return "";
			}
			size_t close = head.find(head[quote], quote + 1);
			if (close == std::string::npos) {
				return "";
			}
			return head.substr(quote + 1, close - quote - 1);
		}

		std::string ReadHead(const std::wstring& path) {
			HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (file == INVALID_HANDLE_VALUE) {
				return "";
			}
			std::string head(kModinfoHeadBytes, '\0');
			DWORD read = 0;
			ReadFile(file, &head[0], kModinfoHeadBytes, &read, nullptr);
			CloseHandle(file);
			head.resize(read);
			return head;
		}

		// Records every .modinfo directly in dir.
		void ScanModinfos(const std::wstring& dir) {
			WIN32_FIND_DATAW found;
			HANDLE search = FindFirstFileW((dir + L"*.modinfo").c_str(), &found);
			if (search == INVALID_HANDLE_VALUE) {
				return;
			}
			do {
				std::string id = ModIdOf(ReadHead(dir + found.cFileName));
				if (!id.empty()) {
					// The first folder found keeps the id: roots are scanned in priority order.
					gFolders.emplace(Lower(id), dir);
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
		}

		// Scans the subfolders of dir, down to depth levels.
		void ScanTree(const std::wstring& dir, int depth) {
			if (depth <= 0) {
				return;
			}
			WIN32_FIND_DATAW found;
			HANDLE search = FindFirstFileW((dir + L"*").c_str(), &found);
			if (search == INVALID_HANDLE_VALUE) {
				return;
			}
			do {
				bool isFolder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				bool isDots = found.cFileName[0] == L'.';
				if (isFolder && !isDots) {
					std::wstring child = dir + found.cFileName + L"\\";
					ScanModinfos(child);
					ScanTree(child, depth - 1);
				}
			} while (FindNextFileW(search, &found));
			FindClose(search);
		}

		// The roots in priority order: the game's own DLC first, so that no mod can pose as the
		// real GameCore's package.
		std::vector<Root> Roots() {
			std::vector<Root> roots;
			roots.push_back({ Paths::GameRoot() + L"DLC\\", kFlatDepth });
			for (const std::wstring& extra : gExtraRoots) {
				roots.push_back({ extra, kFlatDepth });
			}
			std::wstring workshop = Paths::Resolve(Paths::GameRoot() + kWorkshopFromGameRoot);
			roots.push_back({ workshop, kFlatDepth });
			roots.push_back({ Paths::UserDir() + L"Mods\\", kModsDepth });
			return roots;
		}

		void Scan() {
			gFolders.clear();
			for (const Root& root : Roots()) {
				ScanTree(root.dir, root.depth);
			}
			gScanned = true;
			LoaderLog::Write("found %zu mods and DLC", gFolders.size());
		}
	}

	std::wstring Find(const std::string& modId) {
		if (!gScanned) {
			Scan();
		}
		auto it = gFolders.find(Lower(modId));
		if (it == gFolders.end() && !gRescanned) {
			gRescanned = true;
			Scan();
			it = gFolders.find(Lower(modId));
		}
		return it == gFolders.end() ? L"" : it->second;
	}

	void AllowRescan() {
		gRescanned = false;
	}

	void AddRoot(const std::wstring& dir) {
		if (std::find(gExtraRoots.begin(), gExtraRoots.end(), dir) == gExtraRoots.end()) {
			gExtraRoots.push_back(dir);
		}
	}
}
