#include "Version.h"
#include "overlay.h"
#include "sampler.h"
#include "state.h"

// Startup status from import-table hooks, SKSE messages, menu events and the sampler. No game code is patched.

namespace
{
	HMODULE self = nullptr;

	void* PatchImport(HMODULE a_module, const char* a_dll, std::string_view a_func, void* a_hook)
	{
		const auto  base = reinterpret_cast<std::uintptr_t>(a_module);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (!dir.VirtualAddress) {
			return nullptr;
		}
		for (auto* imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
			if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), a_dll) != 0 || !imp->OriginalFirstThunk) {
				continue;
			}
			auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + imp->OriginalFirstThunk);
			auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots) {
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) {
					continue;
				}
				const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
				if (a_func != reinterpret_cast<const char*>(byName->Name)) {
					continue;
				}
				DWORD old;
				VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old);
				const auto original = reinterpret_cast<void*>(slots->u1.Function);
				slots->u1.Function = reinterpret_cast<std::uintptr_t>(a_hook);
				VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
				return original;
			}
		}
		return nullptr;
	}

	std::string Narrow(const wchar_t* a_s) { return SKSE::stl::utf16_to_utf8(a_s).value_or(""s); }

	// Offset just past the Data\ folder in a path (at the start or after a separator, not facegendata\), npos if none.
	std::size_t BelowData(std::string_view a_path)
	{
		const auto lower = Lower(a_path);
		if (lower.starts_with("data\\")) {
			return 5;
		}
		const auto data = lower.find("\\data\\");
		return data == std::string::npos ? data : data + 6;
	}

	std::uint32_t LoadOrderCount()
	{
		const auto dh = RE::TESDataHandler::GetSingleton();
		if (!dh) {
			return 0;
		}
		std::uint32_t n = 0;
		for (const auto* f : dh->files) {
			n += f != nullptr;
		}
		return n;
	}

	// What a file is, by the engine's file types, for "loading scripts" and the like.
	std::string_view Noun(std::string_view a_lowerName)
	{
		const auto dot = a_lowerName.find_last_of('.');
		const auto ext = dot == std::string_view::npos ? ""sv : a_lowerName.substr(dot + 1);
		static constexpr std::pair<std::string_view, std::string_view> nouns[] = {
			{ "pex", "scripts" }, { "hkx", "animations" }, { "nif", "models" }, { "btr", "models" }, { "bto", "models" },
			{ "dds", "textures" }, { "seq", "quest start lists" }, { "strings", "strings" }, { "dlstrings", "strings" },
			{ "ilstrings", "strings" }, { "tri", "morphs" }, { "lod", "LOD settings" }, { "bsa", "archives" }, { "ba2", "archives" },
			{ "swf", "interface files" }, { "wav", "sounds" }, { "xwm", "sounds" }, { "fuz", "sounds" },
			{ "json", "config files" }, { "ini", "config files" }, { "toml", "config files" }, { "yaml", "config files" },
			{ "yml", "config files" }, { "xml", "config files" }, { "txt", "config files" }
		};
		for (const auto& [e, n] : nouns) {
			if (ext == e) {
				return n;
			}
		}
		return "files"sv;
	}

	void OnFileOpen(std::string_view a_path)
	{
		const auto name = BaseName(a_path);
		const auto lower = Lower(name);
		const bool plugin = lower.ends_with(".esp") || lower.ends_with(".esm") || lower.ends_with(".esl");
		const auto now = Status::Uptime();
		if (!plugin) {
			// Path below Data\ where there is one, else the file name.
			const auto data = BelowData(a_path);
			std::string shown = data == std::string::npos ? std::string(name) : std::string(a_path.substr(data));
			// From the plugin load pass to the main menu, each open counts towards an activity and each file is logged once.
			const bool  track = Status::dataThread && !Status::mainMenu && lower.find('.') != std::string::npos;
			std::string chain;
			const auto  owner = track ? Sampler::Owner(&chain) : ""s;
			const auto  noun = Noun(lower);
			bool        log = false;
			{
				std::scoped_lock l{ Status::shared.lock };
				if (track) {
					log = Status::shared.setupFiles.insert(Lower(shown)).second;
					auto& list = Status::shared.activities;
					auto  it = std::ranges::find_if(list, [&](const auto& a) { return a.owner == owner && a.noun == noun; });
					if (it == list.end()) {
						it = list.insert(list.end(), Status::Activity{ .owner = owner, .noun = noun, .bucketAt = now, .firstAt = now });
					}
					if (now - it->bucketAt >= 1.0) {
						it->previous = now - it->bucketAt < 2.0 ? it->current : 0;
						it->current = 0;
						it->bucketAt = now;
					}
					++it->current;
					++it->count;
					it->lastAt = now;
				}
				Status::shared.file = shown;
				Status::shared.fileAt = now;
			}
			if (log) {
				const auto tid = GetCurrentThreadId();
				const bool plugins = chain.find(".dll") != std::string::npos;
				logger::info("[{:6.1f} s] file ({} thread, {}): {}{}", now,
					tid == Status::mainThread ? "main" : tid == Status::dataThread ? "data" : "other",
					owner.empty() ? "game"s : owner, shown, plugins ? " [" + chain + "]" : ""s);
			}
			return;
		}

		// Each plugin is opened twice: a header pass on the main thread, then the real load on a separate data thread.
		const auto tid = GetCurrentThreadId();
		const bool load = tid != Status::mainThread;
		if (load && Status::dataThread.exchange(tid) != tid) {
			logger::info("[{:6.1f} s] Plugins are loaded on thread {} (main thread {})", now, tid, Status::mainThread.load());
		}
		std::uint32_t index = 0, total = 0;
		bool          first, setup = false;
		{
			std::scoped_lock l{ Status::shared.lock };
			auto& seen = load ? Status::shared.loaded : Status::shared.seen;
			first = seen.try_emplace(lower, static_cast<std::uint32_t>(seen.size() + 1)).second;
			if (load && !first) {
				// The data thread opens loaded plugins again while it sets up the game data, which is not loading.
				Status::shared.reopened = name;
				Status::shared.reopenedAt = now;
				setup = now - Status::shared.pluginAt > 2.0;  // the load pass opens each plugin twice too
			} else {
				// The game's plugin list grows with each file loaded, so only the last start knows the total (0 without one).
				index = static_cast<std::uint32_t>(seen.size());
				if (load && first && Status::last.plugins) {
					Status::shared.pluginTotal = std::max(index, Status::last.plugins);
				}
				total = load ? Status::shared.pluginTotal : 0;
				Status::shared.plugin = name;
				Status::shared.pluginIndex = index;
				Status::shared.pluginAt = now;
			}
		}
		static std::atomic<bool> reopenLogged{ false };
		if (first) {
			logger::info("[{:6.1f} s] {} {}/{} {}", now, load ? "load" : "header", index, total, name);
		} else if (setup && !reopenLogged.exchange(true)) {
			logger::info("[{:6.1f} s] Game data setup opens loaded plugins again, first: {}", now, name);
		}
	}

	decltype(&CreateFileA) _CreateFileA;
	decltype(&CreateFileW) _CreateFileW;

	// Only opens that succeed count. The game probes every Skyrim.ccc entry and archive name whether it exists or not.

	HANDLE WINAPI HookCreateFileA(LPCSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_sa, DWORD a_disp,
		DWORD a_flags, HANDLE a_template)
	{
		const auto handle = _CreateFileA(a_name, a_access, a_share, a_sa, a_disp, a_flags, a_template);
		if (handle != INVALID_HANDLE_VALUE && a_name && !Status::done.load(std::memory_order_relaxed)) {
			const auto error = GetLastError();
			OnFileOpen(a_name);
			SetLastError(error);
		}
		return handle;
	}

	HANDLE WINAPI HookCreateFileW(LPCWSTR a_name, DWORD a_access, DWORD a_share, LPSECURITY_ATTRIBUTES a_sa, DWORD a_disp,
		DWORD a_flags, HANDLE a_template)
	{
		const auto handle = _CreateFileW(a_name, a_access, a_share, a_sa, a_disp, a_flags, a_template);
		if (handle != INVALID_HANDLE_VALUE && a_name && !Status::done.load(std::memory_order_relaxed)) {
			const auto error = GetLastError();
			OnFileOpen(Narrow(a_name));
			SetLastError(error);
		}
		return handle;
	}

	// The engine indexes every loose file below Data\ before the game window exists, one FindFirstFileA per folder.
	void OnFolder(std::string_view a_pattern)
	{
		const auto data = BelowData(a_pattern);
		if (data == std::string::npos || !a_pattern.ends_with("\\*")) {
			return;
		}
		// Data\Textures\Armor\\* -> textures\armor (the walker ends its folders with a backslash)
		auto path = Lower(a_pattern.substr(data, a_pattern.size() - data - 1));
		while (path.ends_with('\\')) {
			path.pop_back();
		}
		const auto top = std::string_view(path).substr(0, path.find('\\'));
		if (std::ranges::find(Status::scanFolders, top) == std::end(Status::scanFolders)) {
			return;  // not one of the walked folders
		}
		// textures\armor\iron\steel -> textures\armor\iron, so the box stays narrow
		std::string_view folder = std::string_view(a_pattern).substr(data, path.size());
		for (std::size_t cut = 0, levels = 0; (cut = folder.find('\\', cut)) != std::string_view::npos; ++cut) {
			if (++levels == 3) {
				folder = folder.substr(0, cut);
				break;
			}
		}
		const auto now = Status::Uptime();
		if (double none = 0; Status::scanStart.compare_exchange_strong(none, now)) {
			logger::info("[{:6.1f} s] Loose file scan started ({} files last start)", now, Status::last.files);
		}
		const auto files = Status::looseFiles.load();
		std::scoped_lock l{ Status::shared.lock };
		auto& sh = Status::shared;
		if (top != sh.scanTop) {
			sh.scanTop = top;
			logger::info("[{:6.1f} s] Scanning loose files in {} ({} files so far, estimate {:.0f}%)", now, top, files,
				Status::ScanEstimate() * 100);
		}
		sh.folder = folder;
		sh.folderAt = now;
	}

	decltype(&FindFirstFileA) _FindFirstFileA;
	decltype(&FindNextFileA)  _FindNextFileA;

	HANDLE WINAPI HookFindFirstFileA(LPCSTR a_name, LPWIN32_FIND_DATAA a_data)
	{
		if (a_name && !Status::done.load(std::memory_order_relaxed)) {
			OnFolder(a_name);
		}
		return _FindFirstFileA(a_name, a_data);
	}

	BOOL WINAPI HookFindNextFileA(HANDLE a_find, LPWIN32_FIND_DATAA a_data)
	{
		const auto found = _FindNextFileA(a_find, a_data);
		if (found && !(a_data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !Status::done.load(std::memory_order_relaxed)) {
			Status::looseFiles.fetch_add(1, std::memory_order_relaxed);
		}
		return found;
	}

	void OnLibrary(std::string_view a_path)
	{
		if (Status::postLoad) {
			return;
		}
		const auto name = BaseName(a_path);
		const auto now = Status::Uptime();
		logger::info("[{:6.1f} s] SKSE loads {}", now, name);
		std::scoped_lock l{ Status::shared.lock };
		Status::shared.library = name;
		Status::shared.libraryAt = now;
	}

	decltype(&LoadLibraryA)   _LoadLibraryA;
	decltype(&LoadLibraryExA) _LoadLibraryExA;
	decltype(&LoadLibraryExW) _LoadLibraryExW;

	HMODULE WINAPI HookLoadLibraryA(LPCSTR a_name)
	{
		if (a_name) {
			OnLibrary(a_name);
		}
		return _LoadLibraryA(a_name);
	}

	HMODULE WINAPI HookLoadLibraryExA(LPCSTR a_name, HANDLE a_file, DWORD a_flags)
	{
		if (a_name) {
			OnLibrary(a_name);
		}
		return _LoadLibraryExA(a_name, a_file, a_flags);
	}

	HMODULE WINAPI HookLoadLibraryExW(LPCWSTR a_name, HANDLE a_file, DWORD a_flags)
	{
		if (a_name) {
			OnLibrary(Narrow(a_name));
		}
		return _LoadLibraryExW(a_name, a_file, a_flags);
	}

	template <class T>
	void Patch(HMODULE a_module, std::string_view a_moduleName, const char* a_func, T a_hook, T& a_original)
	{
		const auto original = PatchImport(a_module, "KERNEL32.dll", a_func, reinterpret_cast<void*>(a_hook));
		a_original = reinterpret_cast<T>(original);
		if (!original) {
			logger::warn("{} does not import {}; that source is off", a_moduleName, a_func);
		}
	}

	decltype(&CreateWindowExA) _CreateWindowExA;
	decltype(&CreateWindowExW) _CreateWindowExW;

	HWND WINAPI HookCreateWindowExA(DWORD a_exStyle, LPCSTR a_class, LPCSTR a_name, DWORD a_style, int a_x, int a_y, int a_width,
		int a_height, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		if (!a_parent) {
			Overlay::CloseSplash();
		}
		return _CreateWindowExA(a_exStyle, a_class, a_name, a_style, a_x, a_y, a_width, a_height, a_parent, a_menu, a_instance, a_param);
	}

	HWND WINAPI HookCreateWindowExW(DWORD a_exStyle, LPCWSTR a_class, LPCWSTR a_name, DWORD a_style, int a_x, int a_y, int a_width,
		int a_height, HWND a_parent, HMENU a_menu, HINSTANCE a_instance, LPVOID a_param)
	{
		if (!a_parent) {
			Overlay::CloseSplash();
		}
		return _CreateWindowExW(a_exStyle, a_class, a_name, a_style, a_x, a_y, a_width, a_height, a_parent, a_menu, a_instance, a_param);
	}

	void InstallHooks(const SKSE::LoadInterface* a_skse)
	{
		const auto game = GetModuleHandleW(nullptr);
		Patch(game, "The game exe", "CreateFileA", &HookCreateFileA, _CreateFileA);
		Patch(game, "The game exe", "CreateFileW", &HookCreateFileW, _CreateFileW);
		Patch(game, "The game exe", "FindFirstFileA", &HookFindFirstFileA, _FindFirstFileA);
		Patch(game, "The game exe", "FindNextFileA", &HookFindNextFileA, _FindNextFileA);
		// The load interface lives in the runtime's skse64_1_x_y.dll or sksevr_1_4_15.dll, whatever the game version.
		HMODULE skse = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(a_skse), &skse)) {
			Patch(skse, "skse64", "LoadLibraryA", &HookLoadLibraryA, _LoadLibraryA);
			Patch(skse, "skse64", "LoadLibraryExA", &HookLoadLibraryExA, _LoadLibraryExA);
			Patch(skse, "skse64", "LoadLibraryExW", &HookLoadLibraryExW, _LoadLibraryExW);
		}
	}

	struct MenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (a_event && a_event->opening && !Status::mainMenu &&
				a_event->menuName == RE::MainMenu::MENU_NAME) {
				logger::info("[{:6.1f} s] {} opened", Status::Uptime(), a_event->menuName.c_str());
				Status::mainMenu = true;
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	MenuSink menuSink;
	bool     menuSinkAdded = false;

	void AddMenuSink()
	{
		if (const auto ui = RE::UI::GetSingleton(); ui && !menuSinkAdded) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&menuSink);
			menuSinkAdded = true;
			if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
				Status::mainMenu = true;
			}
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		const auto t = Status::Uptime();
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			logger::info("[{:6.1f} s] SKSE: all plugins loaded (PostLoad)", t);
			Status::postLoad = true;
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			logger::info("[{:6.1f} s] SKSE: PostPostLoad", t);
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			logger::info("[{:6.1f} s] SKSE: InputLoaded", t);
			AddMenuSink();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			{
				const auto tid = GetCurrentThreadId();
				if (!Status::dataThread) {
					Status::dataThread = tid;
				}
				logger::info("[{:6.1f} s] SKSE: kDataLoaded on thread {} ({} plugins in the load order)", t, tid,
					LoadOrderCount());
				AddMenuSink();
				Status::dataLoadedAt = t;
				Status::dataLoaded = true;
			}
			break;
		default:
			break;
		}
	}

	std::filesystem::path DllPath()
	{
		wchar_t dll[1024];
		const auto n = GetModuleFileNameW(self, dll, static_cast<DWORD>(std::size(dll)));
		return !n || n >= std::size(dll) ? std::filesystem::path{} : std::filesystem::path(dll);
	}

	// SkyLine.yaml next to the DLL in Data\SKSE\Plugins, so under MO2 it lands in overwrite.
	std::filesystem::path StatePath()
	{
		const auto dll = DllPath();
		return dll.empty() ? dll : std::filesystem::path(dll).replace_filename(L"SkyLine.yaml");
	}

	std::string_view Trim(std::string_view a_s)
	{
		const auto first = a_s.find_first_not_of(" \t\r\n");
		return first == std::string_view::npos ? std::string_view{} : a_s.substr(first, a_s.find_last_not_of(" \t\r\n") - first + 1);
	}

	// "12.5" or "[12.5, 14.8]"
	std::vector<double> Numbers(std::string_view a_value)
	{
		std::vector<double> v;
		std::string         s(Trim(a_value));
		std::ranges::replace_if(s, [](char c) { return c == '[' || c == ']' || c == ','; }, ' ');
		const char* p = s.c_str();
		for (char* end = nullptr;; p = end) {
			const double d = std::strtod(p, &end);
			if (end == p) {
				break;
			}
			v.push_back(d);
		}
		return v;
	}

	// Only the subset SaveState writes: top-level sections of "key: value" lines, # comments.
	void LoadState()
	{
		const auto path = StatePath();
		std::FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"r");
		if (!f) {
			return;
		}
		std::unordered_map<std::string, std::vector<double>> values;  // "section.key"
		std::string section;
		char        buf[256];
		while (std::fgets(buf, sizeof(buf), f)) {
			std::string_view line = buf;
			line = line.substr(0, line.find('#'));
			const auto colon = line.find(':');
			if (Trim(line).empty() || colon == std::string_view::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, colon));
			if (line.front() != ' ') {
				section = key;
			} else {
				values[section + "." + std::string(key)] = Numbers(line.substr(colon + 1));
			}
		}
		std::fclose(f);
		const auto get = [&](const char* a_key, std::size_t a_n = 1) {
			const auto it = values.find(a_key);
			return it != values.end() && it->second.size() == a_n ? it->second : std::vector<double>(a_n, 0.0);
		};

		auto& l = Status::last;
		l.seconds = get("last_start.main_menu")[0];
		if (l.seconds > 0) {
			l.plugins = static_cast<std::uint32_t>(get("last_start.plugins")[0]);
			const double window = get("last_start.game_window")[0];
			l.window = window < l.seconds ? window : 0.0;
		}
		if (l.window > 0) {
			l.files = static_cast<std::uint32_t>(get("last_start.loose_files")[0]);
		}
		// stage times only count when they follow each other within the start
		const auto splash = get("last_start.splash_stages", 2), game = get("last_start.game_stages", 3);
		const double marks[] = { 0, splash[0], splash[1], l.window, game[0], game[1], game[2], l.seconds };
		if (l.window > 0 && splash[0] > 0 && std::ranges::is_sorted(marks)) {
			std::ranges::copy(splash, l.splash);
			std::ranges::copy(game, l.game);
			l.stages = true;
		}
		if (values.contains("splash.x") && values.contains("splash.y")) {
			Status::splashPos = POINT{ static_cast<LONG>(get("splash.x")[0]), static_cast<LONG>(get("splash.y")[0]) };
		}
	}

	std::filesystem::path IniPath()
	{
		auto ini = DllPath();
		return ini.empty() ? ini : ini.replace_extension(L".ini");
	}

	Overlay::Settings LoadSettings()
	{
		Overlay::Settings s;
		const auto        ini = IniPath();
		if (ini.empty()) {
			return s;
		}
		const auto get = [&](const wchar_t* a_key, int a_default) {
			return static_cast<int>(GetPrivateProfileIntW(L"Overlay", a_key, a_default, ini.c_str()));
		};
		s.enabled = get(L"bEnabled", 1) != 0;
		s.fontSize = std::clamp(get(L"iFontSize", s.fontSize), 8, 48);
		s.lingerSeconds = std::clamp(get(L"iLingerSeconds", s.lingerSeconds), 0, 60);
		wchar_t face[LF_FACESIZE + 1];
		GetPrivateProfileStringW(L"Overlay", L"sFont", L"", face, static_cast<DWORD>(std::size(face)), ini.c_str());
		if (std::wstring_view f = face; !f.empty()) {
			if (f.size() < LF_FACESIZE) {
				s.font = f;
			} else {
				logger::warn("sFont is longer than {} characters, using the default", LF_FACESIZE - 1);
			}
		}
		// Relative to the DLL's folder, an absolute path replaces it.
		wchar_t image[1024];
		GetPrivateProfileStringW(L"Overlay", L"sSplashImage", L"SkyLineSplash.jpg", image, static_cast<DWORD>(std::size(image)), ini.c_str());
		if (*image) {
			s.splashImage = ini.parent_path() / image;
		}
		const auto text = [&](const wchar_t* a_key) {
			wchar_t v[64];
			GetPrivateProfileStringW(L"Overlay", a_key, L"", v, static_cast<DWORD>(std::size(v)), ini.c_str());
			std::string r;
			for (const wchar_t* c = v; *c; ++c) {
				if (*c < 128 && !std::isspace(*c) && *c != '-' && *c != '_' && *c != '#') {
					r += static_cast<char>(std::tolower(*c));
				}
			}
			return r;
		};
		s.barOnly = get(L"bBarOnly", 0) != 0;
		s.showDetail = get(L"bShowDetail", 1) != 0;
		s.textShadow = get(L"bTextShadow", 1) != 0;
		s.dpiScaling = get(L"bDpiScaling", 1) != 0;
		s.splash = get(L"bSplash", 1) != 0;
		s.splashWidth = std::clamp(get(L"iSplashWidth", s.splashWidth), 240, 8192);
		s.barHeight = std::clamp(get(L"iBarHeight", s.barHeight), 1, 64);
		s.barLength = get(L"iBarLength", s.barLength) <= 0 ? 0 : std::clamp(get(L"iBarLength", s.barLength), 50, 8192);
		s.offset = { std::clamp(get(L"iOffsetX", 0), -8192, 8192), std::clamp(get(L"iOffsetY", 0), -8192, 8192) };
		// RRGGBB in hex, like 6EB4E6.
		const auto color = [&](const wchar_t* a_key, COLORREF& a_color) {
			const auto c = text(a_key);
			if (c.empty()) {
				return;
			}
			unsigned rgb = 0;
			const auto [end, ec] = std::from_chars(c.data(), c.data() + c.size(), rgb, 16);
			if (c.size() == 6 && ec == std::errc{} && end == c.data() + c.size()) {
				a_color = RGB(rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
			} else {
				logger::warn("{}={} is not RRGGBB hex, using the default", SKSE::stl::utf16_to_utf8(a_key).value_or("?"), c);
			}
		};
		color(L"sBarColor", s.barColor);
		color(L"sTextColor", s.textColor);
		// TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight.
		if (auto p = text(L"sPosition"); !p.empty()) {
			POINT a{ 1, 1 };
			// Each word sets one axis (-1 leaves it), an axis without a word stays centred.
			for (const auto& [word, x, y] : { std::tuple{ "top", -1, 0 }, std::tuple{ "bottom", -1, 2 }, std::tuple{ "left", 0, -1 },
					 std::tuple{ "right", 2, -1 }, std::tuple{ "center", -1, -1 }, std::tuple{ "centre", -1, -1 }, std::tuple{ "middle", -1, -1 } }) {
				if (const auto at = p.find(word); at != std::string::npos) {
					p.erase(at, std::strlen(word));
					a.x = x < 0 ? a.x : x;
					a.y = y < 0 ? a.y : y;
				}
			}
			if (p.empty()) {
				s.anchor = a;
			} else {
				logger::warn("sPosition has an unknown word '{}', using BottomLeft", p);
			}
		}
		const auto hex = [](COLORREF a_c) { return std::format("{:02X}{:02X}{:02X}", GetRValue(a_c), GetGValue(a_c), GetBValue(a_c)); };
		logger::info("Settings: font {} {} px, linger {} s, splash {} width {}, bar only {}, detail {}, bar {}x{} color {}, text color {}, text shadow {}, dpi scaling {}, "
			"position {},{} offset {},{}", SKSE::stl::utf16_to_utf8(s.font).value_or("?"), s.fontSize, s.lingerSeconds, s.splash, s.splashWidth, s.barOnly, s.showDetail, s.barLength, s.barHeight,
			hex(s.barColor), hex(s.textColor), s.textShadow, s.dpiScaling, s.anchor.x, s.anchor.y, s.offset.x, s.offset.y);
		return s;
	}

	// bLog=0 leaves the log file alone and drops every message.
	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (const auto ini = IniPath(); !path || (!ini.empty() && !GetPrivateProfileIntW(L"Overlay", L"bLog", 1, ini.c_str()))) {
			auto off = std::make_shared<spdlog::logger>("global log"s);
			off->set_level(spdlog::level::off);
			spdlog::set_default_logger(std::move(off));
			return;
		}
		*path /= std::format("{}.log", Version::PROJECT);
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] %v"s);
	}
}

namespace Status
{
	bool HookGameWindow()
	{
		const auto game = GetModuleHandleW(nullptr);
		_CreateWindowExA = reinterpret_cast<decltype(&CreateWindowExA)>(
			PatchImport(game, "USER32.dll", "CreateWindowExA", reinterpret_cast<void*>(&HookCreateWindowExA)));
		_CreateWindowExW = reinterpret_cast<decltype(&CreateWindowExW)>(
			PatchImport(game, "USER32.dll", "CreateWindowExW", reinterpret_cast<void*>(&HookCreateWindowExW)));
		return _CreateWindowExA || _CreateWindowExW;
	}

	double ScanEstimate()
	{
		const auto it = std::ranges::find(scanFolders, shared.scanTop);
		return it == std::end(scanFolders) ? 0.0 : double(it - std::begin(scanFolders)) / std::size(scanFolders);
	}

	void SaveState(const LastStart& a_last)
	{
		const auto path = StatePath();
		std::FILE* f = path.empty() ? nullptr : _wfopen(path.c_str(), L"w");
		if (!f) {
			return;
		}
		std::fprintf(f, "# SkyLine state, rewritten by the plugin. Times are seconds since the game process started.\n");
		if (a_last.seconds > 0) {
			std::fprintf(f, "last_start:\n  plugins: %u\n  main_menu: %.1f\n  game_window: %.1f\n", a_last.plugins,
				a_last.seconds, a_last.window);
			if (a_last.files) {
				std::fprintf(f, "  loose_files: %u\n", a_last.files);
			}
			if (a_last.stages) {
				std::fprintf(f, "  splash_stages: [%.1f, %.1f]  # engine start, loose file scan\n", a_last.splash[0], a_last.splash[1]);
				std::fprintf(f, "  game_stages: [%.1f, %.1f, %.1f]  # plugin loading, game data setup, main menu\n",
					a_last.game[0], a_last.game[1], a_last.game[2]);
			}
		}
		if (splashPos) {
			std::fprintf(f, "splash:  # where the splash window was dragged to, screen pixels\n  x: %ld\n  y: %ld\n",
				splashPos->x, splashPos->y);
		}
		std::fclose(f);
	}
}

// The VR build has no version data, so SKSE for AE refuses it instead of loading it on the wrong game.
#ifndef ENABLE_SKYRIM_VR
extern "C" DLLEXPORT constinit auto SKSEPlugin_Version = []() {
	SKSE::PluginVersionData v;
	v.PluginVersion({ Version::MAJOR, Version::MINOR, Version::PATCH, 0 });
	v.PluginName(Version::PROJECT);
	v.AuthorName("local build");
	v.UsesAddressLibrary();
	// The structures read (renderer windows, UI, data handler files) are the same before and after 1.6.629.
	v.UsesNoStructs();
	return v;
}();
#endif

// SKSE for SE 1.5.97 and for VR asks this instead of reading SKSEPlugin_Version.
extern "C" DLLEXPORT bool SKSEPlugin_Query([[maybe_unused]] const SKSE::QueryInterface* a_skse, SKSE::PluginInfo* a_info)
{
	a_info->infoVersion = SKSE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = REL::Version(Version::MAJOR, Version::MINOR, Version::PATCH).pack();
#ifdef ENABLE_SKYRIM_VR
	return a_skse->RuntimeVersion() == SKSE::RUNTIME_VR_1_4_15;
#else
	return true;
#endif
}

extern "C" DLLEXPORT bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&InitializeLog), &self);
	InitializeLog();
	SKSE::Init(a_skse, { .log = false });

	const auto game = REL::Module::get().version();
	logger::info("[{:6.1f} s] {} {} (game {})", Status::Uptime(), Version::PROJECT, Version::NAME, game.string());
	const auto settings = LoadSettings();
	if (!settings.enabled) {
		logger::info("bEnabled=0 in SkyLine.ini; nothing to do");
		return true;
	}

	LoadState();
	Status::mainThread = GetCurrentThreadId();
	Sampler::SetSelf(self);
	InstallHooks(a_skse);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	Overlay::Start(settings);
	return true;
}
