#pragma once

#include <unordered_set>

// State shared between the game threads (file/library hooks, SKSE messages, menu events) and the overlay thread.

inline std::string Lower(std::string_view a_s)
{
	std::string r(a_s);
	for (auto& c : r) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return r;
}

inline std::string_view BaseName(std::string_view a_path)
{
	const auto slash = a_path.find_last_of("\\/");
	return slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);
}

namespace Status
{
	// Seconds since the game process was created (what the user waits through, including SKSE's own start).
	inline double Uptime()
	{
		static const std::uint64_t start = [] {
			FILETIME c{}, e{}, k{}, u{};
			GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
			return (std::uint64_t(c.dwHighDateTime) << 32) | c.dwLowDateTime;
		}();
		FILETIME now{};
		GetSystemTimeAsFileTime(&now);
		const auto t = (std::uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		return t > start ? double(t - start) / 1e7 : 0.0;
	}

	// Files of one kind opened by one owner (an SKSE plugin, or "" for the game) from the load pass to the main menu.
	struct Activity
	{
		std::string      owner;
		std::string_view noun;  // "scripts", "animations", ...
		std::uint32_t    count = 0;
		std::uint32_t    current = 0;  // opens in the 1 s bucket that started at bucketAt
		std::uint32_t    previous = 0;  // opens in the bucket before
		double           bucketAt = 0;
		double           firstAt = 0;
		double           lastAt = 0;

		std::uint32_t Rate(double a_now) const { return a_now - lastAt > 1.0 ? 0 : current + previous; }
	};

	struct Shared
	{
		std::mutex lock;
		std::string library;  // last DLL SKSE loaded
		double      libraryAt = 0;
		std::string plugin;  // ESP/ESM/ESL the game opened last
		std::uint32_t pluginIndex = 0;
		std::uint32_t pluginTotal = 0;
		double        pluginAt = 0;
		std::string   reopened;  // plugin the data thread opened again after loading it (game data setup)
		double        reopenedAt = 0;
		std::string   file;  // last other file the game opened (path below Data\ when it has one)
		double        fileAt = 0;
		std::string   folder;  // last folder the game listed (loose file scan), below Data\, at most 3 levels
		double        folderAt = 0;
		std::string   scanTop;  // top Data\ folder the scan is in (lower case)
		std::unordered_map<std::string, std::uint32_t> seen;    // header pass (main thread): lower-case name -> order
		std::unordered_map<std::string, std::uint32_t> loaded;  // load pass (data thread)
		std::unordered_set<std::string>                 setupFiles;  // other files opened from the load pass to the main menu
		std::vector<Activity>                           activities;
	};
	inline Shared shared;

	inline std::atomic<DWORD>  mainThread{ 0 };
	inline std::atomic<DWORD>  dataThread{ 0 };  // thread that opens the plugins and sends kDataLoaded
	inline std::atomic<std::uint32_t> looseFiles{ 0 };  // files the game listed in Data\ folders
	inline std::atomic<double>        scanStart{ 0 };   // first Data\ folder listed
	// The engine's loose file scan walks these Data\ folders in this order.
	inline constexpr std::string_view scanFolders[] = { "textures", "meshes", "interface", "music", "sound", "scripts",
		"grass", "strings" };
	// Share of the loose file scan done (0..1), an equal step per top folder, call with shared.lock held.
	double ScanEstimate();
	inline std::atomic<bool>   postLoad{ false };
	inline std::atomic<bool>   dataLoaded{ false };
	inline std::atomic<double> dataLoadedAt{ 0 };
	inline std::atomic<bool>   mainMenu{ false };
	inline std::atomic<bool>   done{ false };

	// A start's times in seconds since process start, kept for the next one. Invalid parts are 0.
	struct LastStart
	{
		std::uint32_t plugins = 0;
		double        seconds = 0;  // main menu
		double        window = 0;   // game window appeared
		std::uint32_t files = 0;    // loose files scanned before the game window
		double        splash[2]{};  // when each later stage of the splash and game bar was entered, valid with stages
		double        game[3]{};
		bool          stages = false;
	};
	// Loaded from SkyLine.yaml next to the DLL (Data\SKSE\Plugins, so under MO2 it lands in overwrite).
	inline LastStart          last;
	inline std::optional<POINT> splashPos;  // where the splash was dragged to
	// Rewrites SkyLine.yaml with a_last and splashPos.
	void SaveState(const LastStart& a_last);
}
