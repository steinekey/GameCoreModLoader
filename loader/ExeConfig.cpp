// SPDX-License-Identifier: AGPL-3.0-only
#include "ExeConfig.h"
#include "LoaderLog.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

// What the exe's code does (CivilizationVI_DX12.exe, Steam build 15038592, read 2026-10-17):
//   exe+0x249827  mov rcx, [rip+X]       the configuration manager's global (exe+0x12fa2c8)
//                 mov rsi, rax
//                 call exe+0x64150       -> its connection
//                 ...                    then CreateQuery("SELECT PackageId, DllPrefix ...
//                                        FROM GameCores ...") on it: the row that names us
//   exe+0x64150   mov rcx, [rcx+0x54f0]  the database object
//                 jmp exe+0x861660
//   exe+0x861660  mov rax, [rcx+8]       its connection
//                 ret
// 32 places in the exe get the configuration connection through exe+0x64150.
//
// Every byte is checked before anything is read through it, so on another build (or in another
// program, such as the loader test) Connection() returns nullptr and the loader probes for the
// database itself.
namespace ExeConfig {
	namespace {
		constexpr uintptr_t kGameCoresReadRva = 0x249827;
		const uint8_t kGameCoresRead[] = {
			0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00,   // mov rcx, [rip+global]
			0x48, 0x8B, 0xF0,                           // mov rsi, rax
			0xE8, 0x00, 0x00, 0x00, 0x00,               // call ConnectionOf
		};
		constexpr size_t kGlobalDisplacementOffset = 3;
		constexpr size_t kGlobalInstructionEnd = 7;
		constexpr size_t kCallOffset = 10;

		constexpr uintptr_t kConnectionOfRva = 0x64150;
		const uint8_t kConnectionOf[] = {
			0x48, 0x8B, 0x89, 0xF0, 0x54, 0x00, 0x00,   // mov rcx, [rcx+0x54f0]
			0xE9, 0x00, 0x00, 0x00, 0x00,               // jmp DatabaseConnection
		};
		constexpr size_t kJumpOffset = 7;
		constexpr uintptr_t kDatabaseOffset = 0x54f0;

		constexpr uintptr_t kDatabaseConnectionRva = 0x861660;
		const uint8_t kDatabaseConnection[] = {
			0x48, 0x8B, 0x41, 0x08,                     // mov rax, [rcx+8]
			0xC3,                                       // ret
		};
		constexpr uintptr_t kConnectionOffset = 0x08;

		// The displacement fields (0x00 in the patterns) are compared separately.
		bool Matches(const uint8_t* code, const uint8_t* pattern, size_t size,
			size_t skipFrom, size_t skipCount) {
			for (size_t i = 0; i < size; i++) {
				if (i >= skipFrom && i < skipFrom + skipCount) {
					continue;
				}
				if (code[i] != pattern[i]) {
					return false;
				}
			}
			return true;
		}

		// Where a rel32 call or jump whose displacement is at `field` lands.
		uintptr_t Rel32Target(const uint8_t* field) {
			int32_t displacement;
			memcpy(&displacement, field, sizeof(displacement));
			return (uintptr_t)(field + 4) + displacement;
		}

		uintptr_t ImageSize(uintptr_t exe) {
			const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
			const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + dos->e_lfanew);
			return nt->OptionalHeader.SizeOfImage;
		}

		// The global that holds the configuration manager, once the code around it checks out.
		const void* const* FindGlobal(uintptr_t exe) {
			if (ImageSize(exe) < kDatabaseConnectionRva + sizeof(kDatabaseConnection)) {
				return nullptr;
			}
			const uint8_t* read = (const uint8_t*)(exe + kGameCoresReadRva);
			const uint8_t* connectionOf = (const uint8_t*)(exe + kConnectionOfRva);
			const uint8_t* databaseConnection = (const uint8_t*)(exe + kDatabaseConnectionRva);
			if (!Matches(read, kGameCoresRead, sizeof(kGameCoresRead), kGlobalDisplacementOffset, 4)
				|| !Matches(read, kGameCoresRead, sizeof(kGameCoresRead), kCallOffset + 1, 4)
				|| Rel32Target(read + kCallOffset + 1) != (uintptr_t)connectionOf
				|| !Matches(connectionOf, kConnectionOf, sizeof(kConnectionOf), kJumpOffset + 1, 4)
				|| Rel32Target(connectionOf + kJumpOffset + 1) != (uintptr_t)databaseConnection
				|| memcmp(databaseConnection, kDatabaseConnection, sizeof(kDatabaseConnection)) != 0) {
				return nullptr;
			}
			int32_t displacement;
			memcpy(&displacement, read + kGlobalDisplacementOffset, sizeof(displacement));
			return (const void* const*)(read + kGlobalInstructionEnd + displacement);
		}

		// The pointer chain, read under SEH: a half-built manager must not take the game down.
		void* Follow(const void* const* global) {
			__try {
				const uint8_t* manager = (const uint8_t*)*global;
				if (!manager) {
					return nullptr;
				}
				const uint8_t* database = *(const uint8_t* const*)(manager + kDatabaseOffset);
				if (!database) {
					return nullptr;
				}
				return *(void* const*)(database + kConnectionOffset);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}
	}

	void* Connection() {
		const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
		const void* const* global = FindGlobal(exe);
		if (!global) {
			LoaderLog::Write("the exe's configuration connection: unknown exe build, not used");
			return nullptr;
		}
		void* connection = Follow(global);
		if (!connection) {
			LoaderLog::Write("the exe's configuration connection: none right now");
		}
		return connection;
	}
}
