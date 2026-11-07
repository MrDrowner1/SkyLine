#include "sampler.h"
#include "state.h"

namespace
{
	struct Module
	{
		std::uintptr_t base = 0;
		std::uintptr_t end = 0;
		std::string    name;
		enum class Kind { kSystem, kGame, kSKSE, kPlugin } kind = Kind::kSystem;
		std::vector<std::pair<std::uintptr_t, std::uintptr_t>> code;  // executable sections
	};

	// Only touched from the overlay thread.
	std::vector<Module> modules;
	std::size_t         moduleCount = 0;
	HMODULE             self = nullptr;

	// Only DLLs in SKSE\Plugins are plugins. Runtimes, graphics proxies and injected overlays (Steam, RivaTuner, Discord) are system.
	Module::Kind Classify(const std::string& a_pathLower, std::string_view a_nameLower, HMODULE a_module)
	{
		if (a_module == self) {
			return Module::Kind::kSystem;
		}
		if (a_module == GetModuleHandleW(nullptr)) {
			return Module::Kind::kGame;
		}
		if (a_nameLower.starts_with("skse64_") || a_nameLower.starts_with("sksevr_")) {
			return Module::Kind::kSKSE;
		}
		return a_pathLower.find("\\skse\\plugins\\") != std::string::npos ? Module::Kind::kPlugin : Module::Kind::kSystem;
	}

	// File name and kind of a loaded module, a system module when it has no file name.
	Module Describe(HMODULE a_module)
	{
		Module  m;
		wchar_t wpath[1024];
		if (GetModuleFileNameW(a_module, wpath, static_cast<DWORD>(std::size(wpath)))) {
			const auto path = SKSE::stl::utf16_to_utf8(wpath).value_or(""s);
			m.name = BaseName(path);
			m.kind = Classify(Lower(path), Lower(m.name), a_module);
		}
		return m;
	}

	const Module* Find(std::uintptr_t a_addr)
	{
		auto it = std::upper_bound(modules.begin(), modules.end(), a_addr,
			[](std::uintptr_t a, const Module& m) { return a < m.base; });
		if (it == modules.begin()) {
			return nullptr;
		}
		--it;
		return a_addr < it->end ? &*it : nullptr;
	}

	// Executable address whose preceding bytes are a call: a real return address rather than a stale value.
	const Module* ReturnTarget(std::uintptr_t a_addr)
	{
		const auto* m = Find(a_addr);
		if (!m || m->kind == Module::Kind::kSystem) {
			return nullptr;
		}
		for (const auto& [b, e] : m->code) {
			if (a_addr >= b + 7 && a_addr < e) {
				const auto* p = reinterpret_cast<const std::uint8_t*>(a_addr);
				const auto  modrmCall = [](std::uint8_t r) { return ((r >> 3) & 7) == 2; };
				if (p[-5] == 0xE8 ||                                       // call rel32
					(p[-6] == 0xFF && p[-5] == 0x15) ||                    // call [rip+disp32]
					(p[-2] == 0xFF && modrmCall(p[-1]) && (p[-1] >> 6) == 3) ||  // call reg
					(p[-3] == 0xFF && modrmCall(p[-2]) && (p[-2] >> 6) == 1) ||  // call [reg+disp8]
					(p[-6] == 0xFF && modrmCall(p[-5]) && (p[-5] >> 6) == 2) ||  // call [reg+disp32]
					(p[-7] == 0xFF && modrmCall(p[-6]) && (p[-6] >> 6) == 2) ||  // call [reg+sib+disp32]
					(p[-4] == 0xFF && modrmCall(p[-3]) && (p[-3] >> 6) == 1)) {  // call [reg+sib+disp8]
					return m;
				}
				return nullptr;
			}
		}
		return nullptr;
	}

	std::string Label(const Module& a_m)
	{
		switch (a_m.kind) {
		case Module::Kind::kGame:
			return "game code";
		case Module::Kind::kSKSE:
			return "SKSE";
		default:
			return a_m.name;
		}
	}

	struct ThreadBasicInformation
	{
		LONG      exitStatus;
		PVOID     tebBaseAddress;
		HANDLE    clientId[2];
		ULONG_PTR affinityMask;
		LONG      priority;
		LONG      basePriority;
	};
	using NtQueryInformationThread_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

	constexpr std::size_t StackWords = 8192;  // 64 KB copied per sample
	std::uintptr_t        stackCopy[StackWords];

	// No C++ objects here: __try, and nothing that allocates or locks while the thread is suspended.
	bool Capture(HANDLE a_thread, std::uintptr_t a_stackBase, std::uintptr_t* a_rip, std::size_t* a_words)
	{
		if (SuspendThread(a_thread) == static_cast<DWORD>(-1)) {
			return false;
		}
		bool ok = false;
		__try {
			alignas(16) CONTEXT ctx;
			std::memset(&ctx, 0, sizeof(ctx));
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(a_thread, &ctx) && ctx.Rsp && ctx.Rsp < a_stackBase) {
				std::size_t n = (a_stackBase - ctx.Rsp) / sizeof(std::uintptr_t);
				if (n > StackWords) {
					n = StackWords;
				}
				std::memcpy(stackCopy, reinterpret_cast<const void*>(ctx.Rsp), n * sizeof(std::uintptr_t));
				*a_rip = ctx.Rip;
				*a_words = n;
				ok = true;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		ResumeThread(a_thread);
		return ok;
	}
}

namespace Sampler
{
	void SetSelf(HMODULE a_self) { self = a_self; }

	std::string Owner(std::string* a_chain, bool* a_skse)
	{
		static std::mutex                          lock;
		static std::unordered_map<HMODULE, Module> cache;
		void*      frames[48];
		const auto n = RtlCaptureStackBackTrace(1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
		// Runs of frames in the same module, innermost first, system modules left out.
		std::vector<std::pair<const Module*, int>> runs;
		for (USHORT i = 0; i < n; ++i) {
			HMODULE m = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					static_cast<LPCWSTR>(frames[i]), &m)) {
				continue;
			}
			std::scoped_lock l{ lock };
			auto it = cache.find(m);
			if (it == cache.end()) {
				it = cache.emplace(m, Describe(m)).first;
			}
			if (it->second.kind == Module::Kind::kSystem) {
				continue;
			}
			if (!runs.empty() && runs.back().first == &it->second) {
				++runs.back().second;
			} else {
				runs.emplace_back(&it->second, 1);
			}
		}
		if (a_chain) {
			a_chain->clear();
			for (const auto& [m, count] : runs) {
				*a_chain += (a_chain->empty() ? "" : " > ") +
				            (m->kind == Module::Kind::kGame ? "game"s : m->kind == Module::Kind::kSKSE ? "SKSE"s : m->name);
				if (count > 1) {
					*a_chain += std::format(" x{}", count);
				}
			}
		}
		if (a_skse) {
			*a_skse = std::ranges::any_of(runs, [](const auto& r) { return r.first->kind == Module::Kind::kSKSE; });
		}
		// Hooks (also chained ones) show as 1-2 frames per plugin, a plugin's own work as 3 or more.
		for (const auto& [m, count] : runs) {
			if (m->kind == Module::Kind::kPlugin && count >= 3) {
				return m->name;
			}
		}
		return ""s;
	}

	void RefreshModules()
	{
		HMODULE list[2048];
		DWORD   needed = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &needed)) {
			return;
		}
		const auto count = std::min<std::size_t>(needed / sizeof(HMODULE), std::size(list));
		if (count == moduleCount) {
			return;
		}
		moduleCount = count;
		modules.clear();
		for (std::size_t i = 0; i < count; ++i) {
			MODULEINFO info{};
			if (!K32GetModuleInformation(GetCurrentProcess(), list[i], &info, sizeof(info))) {
				continue;
			}
			auto m = Describe(list[i]);
			m.base = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll);
			m.end = m.base + info.SizeOfImage;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
				if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
					const auto b = m.base + sec->VirtualAddress;
					m.code.emplace_back(b, b + sec->Misc.VirtualSize);
				}
			}
			modules.push_back(std::move(m));
		}
		std::ranges::sort(modules, {}, &Module::base);
	}

	Thread::~Thread()
	{
		if (_handle) {
			CloseHandle(_handle);
		}
	}

	void Thread::Open(DWORD a_id)
	{
		if (_handle) {
			CloseHandle(_handle);
			_handle = nullptr;
		}
		_id = a_id;
		_stackBase = 0;
		_handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, a_id);
		if (!_handle) {
			return;
		}
		static const auto query = reinterpret_cast<NtQueryInformationThread_t>(
			GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
		ThreadBasicInformation tbi{};
		if (query && query(_handle, 0, &tbi, sizeof(tbi), nullptr) == 0 && tbi.tebBaseAddress) {
			_stackBase = reinterpret_cast<std::uintptr_t>(static_cast<NT_TIB*>(tbi.tebBaseAddress)->StackBase);
		}
	}

	std::string Thread::Sample(bool* a_skse)
	{
		std::uintptr_t rip = 0;
		std::size_t    words = 0;
		if (a_skse) {
			*a_skse = false;
		}
		if (!_handle || !_stackBase || !Capture(_handle, _stackBase, &rip, &words)) {
			return {};
		}
		// Innermost non-system frame only, game code called from a plugin's hook stays "game code".
		const Module* first = nullptr;
		if (const auto* m = Find(rip); m && m->kind != Module::Kind::kSystem) {
			first = m;
		}
		std::size_t i = 0;
		for (; !first && i < words; ++i) {
			first = ReturnTarget(stackCopy[i]);
		}
		if (a_skse) {
			*a_skse = first && first->kind == Module::Kind::kSKSE;
			for (; !*a_skse && i < words; ++i) {
				const auto* m = ReturnTarget(stackCopy[i]);
				*a_skse = m && m->kind == Module::Kind::kSKSE;
			}
		}
		return first ? Label(*first) : "system code"s;
	}
}
