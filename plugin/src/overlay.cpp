#include "overlay.h"
#include "frame.h"
#include "sampler.h"
#include "state.h"

#include <wincodec.h>

// Status box drawn with GDI from its own thread: splash, then a child window, then the game's frames (frame.cpp).

namespace
{
	constexpr auto ClassName = L"SkyLineWindow";
	constexpr UINT TickMs = 100;
	UINT_PTR       timer = 0;  // thread timer: ticks (and samples) before the window exists too
	bool           hooked = false;
	bool           presenting = false;

	Overlay::Settings settings;
	HWND              window = nullptr;
	HWND              splash = nullptr;
	HICON             logo = nullptr;
	HBITMAP           picture = nullptr;  // the splash image at its drawn size, else the logo is shown
	SIZE              pictureSize{};
	const std::uint32_t* picturePixels = nullptr;  // premultiplied, owned by picture
	bool                 pictureAlpha = false;     // has transparent pixels, the splash is a layered window then
	HWND              game = nullptr;
	RECT              gameRect{};
	HFONT             headFont = nullptr;
	HFONT             detailFont = nullptr;
	int               scalePct = 100;
	int               width = 0;  // only grows, so the box does not jump with every file name
	std::wstring      head;
	std::wstring      detail;
	bool              closing = false;
	double            readyAt = 0;
	double            windowAt = 0;  // when the game window appeared, the splash bar ends there and the game bar starts

	// A bar split into stages that end at set points on the bar, never moving back.
	template <int N>
	struct Estimate
	{
		double end[N]{};      // where each stage ends on the bar
		double seconds[N]{};  // how long each stage took last start, 0 when unknown
		double entered[N]{};  // when each stage was entered this start, saved for the next
		int    stage = -1;
		double from = 0;
		double value = 0;

		// Stage ends from the last start's times (start, each later stage entered, end), else a_steps equal steps per stage.
		void Init(const double (&a_marks)[N + 1], bool a_last, const int (&a_steps)[N])
		{
			const double total = a_marks[N] - a_marks[0];
			int          all = 0;
			for (const int s : a_steps) {
				all += s;
			}
			for (int s = 0, steps = 0; s < N; ++s) {
				steps += a_steps[s];
				seconds[s] = a_last && total > 0 ? a_marks[s + 1] - a_marks[s] : 0.0;
				end[s] = a_last && total > 0 ? (a_marks[s + 1] - a_marks[0]) / total : double(steps) / all;
			}
		}

		// a_done: the stage's own progress (0..1) when it is known, else time against the last start's, else it holds
		void Update(int a_stage, double a_done = -1)
		{
			const double t = Status::Uptime();
			if (a_stage > stage) {
				for (int s = std::max(stage, 0) + 1; s <= a_stage; ++s) {
					entered[s] = t;
				}
				if (a_stage > 0) {
					value = std::max(value, end[a_stage - 1]);  // a finished stage jumps to its end
				}
			}
			if (a_stage != stage) {
				stage = a_stage;
				from = std::min(value, end[stage]);
			}
			const double done = a_done >= 0 ? std::min(a_done, 1.0) :
			                    seconds[stage] > 0 ? std::min(0.95, (t - entered[stage]) / seconds[stage]) : 0.0;
			value = std::max(value, from + (end[stage] - from) * done);
		}

		// Stages skipped this start count as entered when the next one was.
		void Close(double a_end)
		{
			for (int s = N - 1; s > 0; --s) {
				if (!entered[s]) {
					entered[s] = s + 1 < N ? entered[s + 1] : a_end;
				}
			}
		}
	};
	// Splash: SKSE plugins, engine start, loose file scan (an equal step per top folder on a first start).
	Estimate<3> splashEstimate;
	// Game window: engine start, plugin loading (until 2 s without a plugin file), game data setup, main menu.
	Estimate<4> gameEstimate;
	constexpr double FinishSeconds = 0.5;  // a bar fills up from where it is once its part is done
	double           splashFrom = 0;
	std::uint32_t    scanFiles = 0;    // loose files listed before the game window
	double           closeFrom = 0;

	Sampler::Thread mainSampler;
	Sampler::Thread dataSampler;

	// The last few samples per thread, the label shown is the most frequent one (stable at 10 samples/s).
	struct Busy
	{
		std::string recent[5];
		std::size_t next = 0;
		std::string shown;
		std::string logged;
		double      since = 0;

		void Add(std::string a_s)
		{
			recent[next++ % std::size(recent)] = std::move(a_s);
			std::string best;
			int         bestCount = 0;
			for (const auto& r : recent) {
				const auto c = std::ranges::count(recent, r);
				if (!r.empty() && c > bestCount) {
					best = r;
					bestCount = static_cast<int>(c);
				}
			}
			if (best != shown) {
				shown = best;
				since = Status::Uptime();
			}
		}

		// Timeline in the log: each label that held for at least a second.
		void Log(std::string_view a_thread)
		{
			if (!shown.empty() && shown != logged && Status::Uptime() - since >= 1.0) {
				logged = shown;
				logger::info("[{:6.1f} s] {} thread busy in: {}", since, a_thread, shown);
			}
		}

		bool IsPlugin() const { return !shown.empty() && shown != "game code" && shown != "SKSE" && shown != "system code"; }
	};
	Busy mainBusy;
	Busy dataBusy;

	// Sampled time per module on one thread, logged at the end.
	struct ThreadTime
	{
		std::vector<std::pair<std::string, double>> modules;
		double                                      from = -1;
		double                                      to = 0;

		void Add(const std::string& a_label, double a_now, double a_seconds)
		{
			if (from < 0) {
				from = a_now;
			}
			to = a_now;
			const auto& name = a_label.empty() ? "not sampled"s : a_label;
			auto        it = std::ranges::find(modules, name, &std::pair<std::string, double>::first);
			if (it == modules.end()) {
				modules.emplace_back(name, a_seconds);
			} else {
				it->second += a_seconds;
			}
		}

		void Log(std::string_view a_what)
		{
			if (from < 0) {
				return;
			}
			std::ranges::sort(modules, std::greater{}, &std::pair<std::string, double>::second);
			std::string list;
			double      rest = 0;
			for (std::size_t i = 0; i < modules.size(); ++i) {
				if (i < 8 && modules[i].second >= 0.1) {
					list += std::format("{}{} {:.1f} s", list.empty() ? "" : ", ", modules[i].first, modules[i].second);
				} else {
					rest += modules[i].second;
				}
			}
			if (rest >= 0.1) {
				list += std::format(", others {:.1f} s", rest);
			}
			logger::info("  {} ({:.1f}-{:.1f} s): {}", a_what, from, to, list);
		}
	};
	ThreadTime mainTime;
	ThreadTime dataTime;
	ThreadTime handlerTime;
	double     sampledAt = 0;

	// The activity with the most file opens in the last second. The shown one stays until another has twice its rate.
	std::string shownOwner, shownNoun;
	const Status::Activity* PickActivity(const std::vector<Status::Activity>& a_list, double a_now)
	{
		const Status::Activity* best = nullptr;
		const Status::Activity* shown = nullptr;
		for (const auto& a : a_list) {
			if (a.Rate(a_now) && (!best || a.Rate(a_now) > best->Rate(a_now))) {
				best = &a;
			}
			if (a.owner == shownOwner && a.noun == shownNoun && a.Rate(a_now)) {
				shown = &a;
			}
		}
		if (shown && best->Rate(a_now) < 2 * shown->Rate(a_now)) {
			best = shown;
		}
		if (best) {
			shownOwner = best->owner;
			shownNoun = best->noun;
		}
		return best;
	}

	void UpdateEstimate(double a_pluginAt, std::uint32_t a_index, std::uint32_t a_total, double a_scan)
	{
		if (!windowAt) {
			// the scan follows the file count when the last start gave a total, else the top folder it is in
			const int stage = !Status::postLoad ? 0 : !Status::scanStart ? 1 : 2;
			splashEstimate.Update(stage, stage < 2 ? -1 : Status::last.files ? double(Status::looseFiles) / Status::last.files : a_scan);
		} else {
			const bool loading = Status::Uptime() - a_pluginAt < 2.0;
			const int  stage = !Status::dataThread ? 0 : Status::dataLoaded ? 3 : loading ? 1 : 2;
			// plugin loading follows the file count when the last start gave a total
			const bool counted = stage == 1 && a_total;
			gameEstimate.Update(stage, counted ? double(a_index) / a_total : -1);
		}
	}

	std::wstring Wide(std::string_view a_s) { return SKSE::stl::utf8_to_utf16(a_s).value_or(L"?"s); }

	void ComposeText()
	{
		const double t = closing ? readyAt : Status::Uptime();
		std::string  h;
		std::string  d = std::format("{:.0f}s", t);
		if (Status::last.seconds > 0) {
			d += std::format("/{:.0f}s", Status::last.seconds);
		}

		std::string plugin, reopened, library, file, folder;
		std::vector<Status::Activity> activities;
		std::uint32_t index = 0, total = 0;
		double        pluginAt = 0, reopenedAt = 0, libraryAt = 0, fileAt = 0, folderAt = 0, scan = 0;
		{
			std::scoped_lock l{ Status::shared.lock };
			plugin = Status::shared.plugin;
			index = Status::shared.pluginIndex;
			total = Status::shared.pluginTotal;
			pluginAt = Status::shared.pluginAt;
			reopened = Status::shared.reopened;
			reopenedAt = Status::shared.reopenedAt;
			library = Status::shared.library;
			libraryAt = Status::shared.libraryAt;
			file = Status::shared.file;
			fileAt = Status::shared.fileAt;
			folder = Status::shared.folder;
			folderAt = Status::shared.folderAt;
			scan = Status::ScanEstimate();
			activities = Status::shared.activities;
		}
		const bool scanning = !folder.empty() && t - folderAt < 1.5;
		UpdateEstimate(pluginAt, index, total, scan);
		const bool loading = Status::dataThread != 0;  // the load pass has started

		// Game data setup and main menu preparation: files being opened by kind and owner, else a plugin running.
		const auto* activity = loading && !Status::mainMenu ? PickActivity(activities, t) : nullptr;
		std::string doing;
		if (activity) {
			doing = (activity->owner.empty() ? "loading "s : activity->owner + " loading ") + std::string(activity->noun);
		} else if (dataBusy.IsPlugin()) {
			doing = dataBusy.shown;
		} else if (mainBusy.IsPlugin()) {
			doing = mainBusy.shown;
		}

		if (closing) {
			h = std::format("Startup finished in {:.1f} s", readyAt);
		} else if (!Status::postLoad) {
			if (!library.empty() && t - libraryAt < 1.5) {
				h = "Loading SKSE plugin: " + library;
			} else if (mainBusy.IsPlugin()) {
				h = "Starting SKSE plugin: " + mainBusy.shown;
			} else {
				h = "Loading SKSE plugins";
			}
		} else if (!Status::dataLoaded && !loading && index && t - pluginAt < 1.0) {
			h = std::format("Checking plugin files {}: {}", index, plugin);
		} else if (!Status::dataLoaded && !loading && scanning) {
			h = "Scanning loose files: " + folder;
		} else if (!Status::dataLoaded && !loading) {
			h = mainBusy.IsPlugin() ? "Starting engine: " + mainBusy.shown : "Starting engine"s;
		} else if (!Status::dataLoaded) {
			if (index >= total && t - pluginAt > 2.0) {
				if (doing.empty() && !reopened.empty() && t - reopenedAt < 1.5) {
					doing = reopened;
				}
				h = doing.empty() ? "Setting up game data"s : "Setting up game data: " + doing;
			} else if (total) {
				h = std::format("Loading plugin {}/{}: {}", index, total, plugin);
			} else {
				h = std::format("Loading plugin {}: {}", index, plugin);
			}
		} else if (!Status::mainMenu) {
			h = doing.empty() ? "Preparing main menu"s : "Preparing main menu: " + doing;
		} else {
			h = std::format("Main menu shown, still loading plugin {}/{}: {}", index, total, plugin);
		}

		if (!closing) {
			if (scanning && Status::last.files) {
				d += std::format(" · {}/{} loose files", Status::looseFiles.load(), Status::last.files);
			} else if (scanning) {
				d += std::format(" · {} loose files", Status::looseFiles.load());
			}
			if (activity && (Status::dataLoaded || (index >= total && t - pluginAt > 2.0))) {
				d += std::format(" · {} {}", activity->count, activity->noun);
			}
			if (!file.empty() && t - fileAt < 1.5) {
				d += " · file: " + file;
			}
		}
		head = Wide(h);
		detail = Wide(d);
	}

	int Px(int a_v) { return a_v * scalePct / 100; }

	constexpr COLORREF Track = RGB(72, 72, 78);
	constexpr int      MarginPx = 16;  // at 96 dpi

	int BarHeight() { return std::max(1, Px(settings.barHeight)); }
	int Pad() { return settings.barOnly ? 0 : Px(10); }

	// Splash bar until the game window, game bar from there to the main menu, each from its stage estimate.
	double Progress(bool a_splash)
	{
		const double t = Status::Uptime();
		const auto   finish = [&](double a_from, double a_at) { return std::min(1.0, a_from + (1.0 - a_from) * (t - a_at) / FinishSeconds); };
		if (a_splash && windowAt) {
			return finish(splashFrom, windowAt);
		}
		if (!a_splash && closing) {
			return finish(closeFrom, readyAt);
		}
		return std::clamp(a_splash ? splashEstimate.value : gameEstimate.value, 0.0, 0.99);
	}

	SIZE Measure(HFONT a_font, const std::wstring& a_text)
	{
		const auto dc = GetDC(window);
		const auto old = SelectObject(dc, a_font);
		RECT       r{ 0, 0, 0, 0 };
		DrawTextW(dc, a_text.c_str(), static_cast<int>(a_text.size()), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		SelectObject(dc, old);
		ReleaseDC(window, dc);
		return { r.right, r.bottom };
	}

	// With bBarOnly the status is the bar alone.
	int StatusHeight()
	{
		if (settings.barOnly) {
			return BarHeight();
		}
		const int second = settings.showDetail ? Measure(detailFont, detail).cy + Px(2) : 0;
		return Measure(headFont, head).cy + second + 2 * Pad() + BarHeight();
	}

	SIZE BoxSize()
	{
		RECT area{};
		GetClientRect(game, &area);  // child window: client coordinates
		const int pad = Pad();
		if (!settings.barLength) {
			return { std::max<int>(Px(100), area.right - area.left - 2 * Px(MarginPx)), StatusHeight() };
		}
		if (settings.barOnly) {
			return { Px(settings.barLength), StatusHeight() };
		}
		const auto h1 = Measure(headFont, head);
		const auto h2 = settings.showDetail ? Measure(detailFont, detail) : SIZE{};
		const int  maxWidth = std::max<int>(Px(200), (area.right - area.left) * 3 / 4);
		width = std::min(std::max<int>({ width, h1.cx + 2 * pad, h2.cx + 2 * pad, Px(settings.barLength) + 2 * pad }), maxWidth);
		return { width, StatusHeight() };
	}

	Frame::Spot Where() { return { settings.anchor, { Px(settings.offset.x), Px(settings.offset.y) }, Px(MarginPx) }; }

	void Layout()
	{
		if (!window || !game) {
			return;
		}
		RECT area{};
		GetClientRect(game, &area);
		const auto size = BoxSize();
		const auto at = Frame::Place({ area.right - area.left, area.bottom - area.top }, size, Where());
		SetWindowPos(window, nullptr, area.left + at.x, area.top + at.y, size.cx, size.cy, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
	}

	void Fill(HDC a_dc, const RECT& a_rc, COLORREF a_color)
	{
		const auto brush = CreateSolidBrush(a_color);
		FillRect(a_dc, &a_rc, brush);
		DeleteObject(brush);
	}

	// Top-down 32-bit DIB section, its pixels in a_bits.
	HBITMAP CreateDib(int a_width, int a_height, void*& a_bits)
	{
		BITMAPINFO bi{};
		bi.bmiHeader = { sizeof(bi.bmiHeader), a_width, -a_height, 1, 32, BI_RGB };
		return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &a_bits, nullptr, 0);
	}

	// The two status lines inside rc. a_mask draws all white for the alpha pass, a_shadow adds a drop shadow.
	// The box lines keep to its anchor side like the bar, the splash lines stay left.
	void DrawLines(HDC mem, const RECT& rc, bool a_mask, bool a_shadow, bool a_splash = false)
	{
		if (settings.barOnly) {
			return;
		}
		SetBkMode(mem, TRANSPARENT);
		const int  pad = Pad();
		const int  shadow = std::max(1, Px(1));
		const auto h1 = Measure(headFont, head);
		const RECT r1{ rc.left + pad, rc.top + pad, rc.right - pad, rc.top + pad + h1.cy };
		const RECT r2{ rc.left + pad, r1.bottom + Px(2), rc.right - pad, rc.bottom - pad - BarHeight() };
		const auto line = [&](HFONT a_font, const std::wstring& a_text, RECT a_r, COLORREF a_color) {
			SelectObject(mem, a_font);
			const UINT align = a_splash ? DT_LEFT : settings.anchor.x == 1 ? DT_CENTER : settings.anchor.x == 2 ? DT_RIGHT : DT_LEFT;
			const UINT flags = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | align;
			if (a_shadow) {
				SetTextColor(mem, a_mask ? RGB(255, 255, 255) : RGB(0, 0, 0));
				RECT s = a_r;
				OffsetRect(&s, shadow, shadow);
				DrawTextW(mem, a_text.c_str(), static_cast<int>(a_text.size()), &s, flags);
			}
			SetTextColor(mem, a_mask ? RGB(255, 255, 255) : a_color);
			DrawTextW(mem, a_text.c_str(), static_cast<int>(a_text.size()), &a_r, flags);
		};
		const auto oldFont = SelectObject(mem, headFont);
		const auto c = settings.textColor;
		line(headFont, head, r1, c);
		if (settings.showDetail) {
			line(detailFont, detail, r2, RGB(GetRValue(c) * 85 / 100, GetGValue(c) * 85 / 100, GetBValue(c) * 85 / 100));
		}
		SelectObject(mem, oldFont);
	}

	// The box bar has a set length under the text since the box widens with long text, the splash bar spans the window.
	// It sits on the box's anchor side, so it stays put in the game window when the box widens.
	RECT BarTrack(const RECT& rc, bool a_full)
	{
		if (a_full) {
			return { rc.left, rc.bottom - BarHeight(), rc.right, rc.bottom };
		}
		const LONG space = rc.right - rc.left - 2 * Pad();
		const LONG len = settings.barLength ? std::min<LONG>(space, Px(settings.barLength)) : space;
		const LONG left = rc.left + Pad() + (settings.anchor.x == 1 ? (space - len) / 2 : settings.anchor.x == 2 ? space - len : 0);
		return { left, rc.bottom - BarHeight(), left + len, rc.bottom };
	}

	RECT BarFill(const RECT& rc, bool a_full)
	{
		RECT fill = BarTrack(rc, a_full);
		fill.right = fill.left + static_cast<LONG>((fill.right - fill.left) * Progress(a_full));
		return fill;
	}

	void DrawStatus(HDC mem, const RECT& rc, bool a_fullBar, bool a_shadow = false)
	{
		DrawLines(mem, rc, false, a_shadow, a_fullBar);
		Fill(mem, BarTrack(rc, a_fullBar), Track);
		Fill(mem, BarFill(rc, a_fullBar), settings.barColor);
	}

	// The child window (engine start): black like the game window around it.
	void Render(HDC mem, const RECT& rc)
	{
		Fill(mem, rc, RGB(0, 0, 0));
		DrawStatus(mem, rc, false);
	}

	// With an image the splash is the image inside the border, the status drawn over its bottom.
	RECT WorkArea()
	{
		MONITORINFO mi{ sizeof(mi) };
		GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
		return mi.rcWork;
	}

	// iSplashWidth and the height, limited to what fits on the primary monitor.
	SIZE SplashLimit()
	{
		const auto wa = WorkArea();
		return { std::min<LONG>(Px(settings.splashWidth), wa.right - wa.left), wa.bottom - wa.top };
	}

	SIZE SplashSize()
	{
		if (picture) {
			return { pictureSize.cx + 2, pictureSize.cy + 2 };
		}
		const auto limit = SplashLimit();
		const LONG cx = std::min<LONG>(limit.cx, limit.cy * 8 / 5);
		return { cx, cx * 5 / 8 };
	}

	// The pixels of any format WIC decodes (jpg, png, bmp...) as premultiplied 32-bit BGRA.
	HRESULT DecodeFile(const std::filesystem::path& a_path, std::vector<std::uint32_t>& src, UINT& w, UINT& h)
	{
		IWICImagingFactory*    factory = nullptr;
		IWICBitmapDecoder*     decoder = nullptr;
		IWICBitmapFrameDecode* frame = nullptr;
		IWICFormatConverter*   converter = nullptr;
		const auto             release = [](auto* a_p) {
			if (a_p) {
				a_p->Release();
			}
		};
		HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
		if (SUCCEEDED(hr)) {
			hr = factory->CreateDecoderFromFilename(a_path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
		}
		if (SUCCEEDED(hr)) {
			hr = decoder->GetFrame(0, &frame);
		}
		if (SUCCEEDED(hr)) {
			hr = factory->CreateFormatConverter(&converter);
		}
		if (SUCCEEDED(hr)) {
			hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
		}
		if (SUCCEEDED(hr)) {
			hr = converter->GetSize(&w, &h);
		}
		if (SUCCEEDED(hr) && (!w || !h || w > 8192 || h > 8192)) {
			hr = E_INVALIDARG;
		}
		if (SUCCEEDED(hr)) {
			src.resize(std::size_t(w) * h);
			hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(src.size() * 4), reinterpret_cast<BYTE*>(src.data()));
		}
		release(converter);
		release(frame);
		release(decoder);
		release(factory);
		return hr;
	}

	// The background of the _SkyrimSELauncher.exe launcher
	bool DecodeLauncher(std::vector<std::uint32_t>& src, UINT& w, UINT& h)
	{
		wchar_t    exe[MAX_PATH];
		const auto n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
		if (!n || n == MAX_PATH) {
			return false;
		}
		const auto dir = std::filesystem::path(exe).parent_path();
		for (const auto* name : { L"SkyrimSELauncher.exe", L"_SkyrimSELauncher.exe" }) {
			const auto mod = LoadLibraryExW((dir / name).c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
			if (!mod) {
				continue;
			}
			bool ok = false;
			if (const auto bmp = static_cast<HBITMAP>(LoadImageW(mod, MAKEINTRESOURCEW(103), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION))) {
				BITMAP bm{};
				if (GetObjectW(bmp, sizeof(bm), &bm) && bm.bmWidth == 835 && bm.bmHeight == 400) {
					BITMAPINFO bi{};
					bi.bmiHeader = { sizeof(BITMAPINFOHEADER), bm.bmWidth, -bm.bmHeight, 1, 32, BI_RGB };
					src.resize(std::size_t(bm.bmWidth) * bm.bmHeight);
					const auto dc = GetDC(nullptr);
					ok = GetDIBits(dc, bmp, 0, bm.bmHeight, src.data(), &bi, DIB_RGB_COLORS) == bm.bmHeight;
					ReleaseDC(nullptr, dc);
					w = bm.bmWidth;
					h = bm.bmHeight;
				}
				DeleteObject(bmp);
			}
			FreeLibrary(mod);
			if (ok) {
				return true;
			}
		}
		return false;
	}

	// The splash image, else the launcher's, centre-cropped to between 3:1 and 4:3, scaled to fit a_limit with a box
	// filter since Wine's WIC scaler is nearest neighbour only.
	HBITMAP LoadPicture(const std::filesystem::path& a_path, SIZE a_limit, SIZE& a_size)
	{
		UINT                       w = 0, h = 0;
		std::vector<std::uint32_t> src;
		std::string                name = a_path.filename().string();
		if (!a_path.empty()) {
			if (const auto hr = DecodeFile(a_path, src, w, h); FAILED(hr)) {
				logger::warn("Splash image {} not loaded ({:08X})", name, static_cast<std::uint32_t>(hr));
				src.clear();
			}
		}
		if (src.empty()) {
			if (!DecodeLauncher(src, w, h)) {
				logger::warn("No launcher image found, showing the icon");
				return nullptr;
			}
			name = "launcher image";
			for (auto& p : src) {
				p |= 0xFF000000;  // GetDIBits leaves the alpha byte undefined
			}
		}

		// The source rectangle: full width unless wider than 3:1, full height unless taller than 4:3.
		const double sw = std::min<double>(w, h * 3.0), sh = std::min<double>(h, w * 0.75);
		const double sx = (w - sw) / 2, sy = (h - sh) / 2;
		const int    cx = std::min<int>(a_limit.cx, static_cast<int>(a_limit.cy * sw / sh + 0.5));
		const int    cy = std::max(StatusHeight() + Px(40), static_cast<int>(cx * sh / sw + 0.5));
		void*        bits = nullptr;
		const auto   bmp = CreateDib(cx, cy, bits);
		if (!bmp) {
			return nullptr;
		}
		auto* out = static_cast<std::uint32_t*>(bits);
		for (int y = 0; y < cy; ++y) {
			const int y0 = static_cast<int>(sy + sh * y / cy), y1 = std::max(y0 + 1, static_cast<int>(sy + sh * (y + 1) / cy));
			for (int x = 0; x < cx; ++x) {
				const int x0 = static_cast<int>(sx + sw * x / cx), x1 = std::max(x0 + 1, static_cast<int>(sx + sw * (x + 1) / cx));
				std::uint32_t sum[4]{}, n = 0;
				for (int v = y0; v < std::min<int>(y1, h); ++v) {
					for (int u = x0; u < std::min<int>(x1, w); ++u, ++n) {
						const auto p = src[std::size_t(v) * w + u];
						sum[0] += p & 0xFF;
						sum[1] += (p >> 8) & 0xFF;
						sum[2] += (p >> 16) & 0xFF;
						sum[3] += p >> 24;
					}
				}
				const auto a = n ? sum[3] / n : 0;
				out[std::size_t(y) * cx + x] = n ? (sum[0] / n) | (sum[1] / n) << 8 | (sum[2] / n) << 16 | a << 24 : 0;
				pictureAlpha |= a < 255;
			}
		}
		// Darkened towards the bottom behind the status text, so it stays readable on bright images.
		const int band = StatusHeight() + Px(40);
		for (int y = std::max(0, cy - band); y < cy; ++y) {
			const double f = double(y - (cy - band)) / band;
			const auto   keep = static_cast<std::uint32_t>(256 * (1.0 - 0.7 * f * f * (3 - 2 * f)));
			for (int x = 0; x < cx; ++x) {
				auto& p = out[std::size_t(y) * cx + x];
				p = ((p & 0xFF) * keep >> 8) | (((p >> 8) & 0xFF) * keep >> 8) << 8 | (((p >> 16) & 0xFF) * keep >> 8) << 16 | (p & 0xFF000000);
			}
		}
		a_size = { cx, cy };
		picturePixels = out;
		logger::info("Splash image {} ({}x{}) shown at {}x{}{}", name, w, h, cx, cy, pictureAlpha ? ", see-through" : "");
		return bmp;
	}

	// Minimize and close buttons in the splash's top right corner.
	enum class Button
	{
		None,
		Minimize,
		Close
	};
	Button hovered = Button::None;
	Button pressed = Button::None;  // held down, acts on release over the same button

	RECT ButtonRect(Button a_button)
	{
		RECT rc{};
		GetClientRect(splash, &rc);
		const int  w = Px(40), h = Px(28);
		const LONG right = rc.right - 1 - (a_button == Button::Minimize ? w : 0);
		return { right - w, rc.top + 1, right, rc.top + 1 + h };
	}

	Button ButtonAt(POINT a_pt)
	{
		for (const auto b : { Button::Minimize, Button::Close }) {
			const auto r = ButtonRect(b);
			if (PtInRect(&r, a_pt)) {
				return b;
			}
		}
		return Button::None;
	}

	// A translucent white layer over a_r, so the image stays visible under the hovered minimize button.
	void Lighten(HDC mem, const RECT& a_r, BYTE a_alpha)
	{
		const auto src = CreateCompatibleDC(mem);
		const auto bmp = CreateCompatibleBitmap(mem, 1, 1);
		const auto old = SelectObject(src, bmp);
		SetPixel(src, 0, 0, RGB(255, 255, 255));
		AlphaBlend(mem, a_r.left, a_r.top, a_r.right - a_r.left, a_r.bottom - a_r.top, src, 0, 0, 1, 1, { AC_SRC_OVER, 0, a_alpha, 0 });
		SelectObject(src, old);
		DeleteObject(bmp);
		DeleteDC(src);
	}

	void DrawButtons(HDC mem)
	{
		for (const auto b : { Button::Minimize, Button::Close }) {
			const auto r = ButtonRect(b);
			const bool lit = hovered == b && (pressed == Button::None || pressed == b);
			if (lit && b == Button::Close) {
				Fill(mem, r, RGB(196, 43, 28));
			} else if (lit) {
				Lighten(mem, r, 48);
			}
			const COLORREF glyph = lit ? RGB(255, 255, 255) : RGB(200, 200, 206);
			const int      cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, half = Px(5);
			const int      stroke = std::max(1, Px(1));
			if (b == Button::Minimize) {
				Fill(mem, { cx - half, cy, cx + half, cy + stroke }, glyph);
				continue;
			}
			const auto pen = CreatePen(PS_SOLID, stroke, glyph);
			const auto old = SelectObject(mem, pen);
			MoveToEx(mem, cx - half, cy - half, nullptr);
			LineTo(mem, cx + half + 1, cy + half + 1);  // LineTo leaves out the last pixel
			MoveToEx(mem, cx + half, cy - half, nullptr);
			LineTo(mem, cx - half - 1, cy + half + 1);
			SelectObject(mem, old);
			DeleteObject(pen);
		}
	}

	// The image or the logo centred in the upper part, the status at the bottom.
	void RenderSplash(HDC mem, const RECT& rc)
	{
		Fill(mem, rc, RGB(18, 18, 20));
		const auto border = CreateSolidBrush(RGB(58, 58, 64));
		FrameRect(mem, &rc, border);
		DeleteObject(border);

		const int statusHeight = StatusHeight();
		const int logoSize = Px(150);
		if (picture) {
			const auto src = CreateCompatibleDC(mem);
			const auto old = SelectObject(src, picture);
			BitBlt(mem, 1, 1, pictureSize.cx, pictureSize.cy, src, 0, 0, SRCCOPY);
			SelectObject(src, old);
			DeleteDC(src);
		} else if (logo) {
			const int top = std::max(Px(16), static_cast<int>(rc.bottom - statusHeight - logoSize) / 2);
			DrawIconEx(mem, (rc.right - logoSize) / 2, top, logo, logoSize, logoSize, 0, nullptr, DI_NORMAL);
		}
		DrawStatus(mem, { rc.left + 1, rc.bottom - statusHeight - 1, rc.right - 1, rc.bottom - 1 }, true, picture != nullptr && settings.textShadow);
		DrawButtons(mem);
	}

	// A soft dark glow around the text coverage: two box blurs each way, then boosted so it is dense next to the strokes.
	std::vector<std::uint8_t> Halo(const std::vector<std::uint8_t>& a_cover, LONG a_w, LONG a_h)
	{
		const int                  r = std::max(1, Px(3));
		std::vector<std::uint32_t> a(a_cover.begin(), a_cover.end());
		std::vector<std::uint32_t> b(a.size());
		const auto blur = [&](std::vector<std::uint32_t>& a_src, std::vector<std::uint32_t>& a_dst, LONG a_len, LONG a_lines, std::size_t a_step, std::size_t a_stride) {
			for (LONG l = 0; l < a_lines; ++l) {
				const std::size_t base = l * a_stride;
				for (LONG i = 0; i < a_len; ++i) {
					std::uint32_t sum = 0;
					for (LONG k = std::max(0L, i - r); k <= std::min(a_len - 1, i + static_cast<LONG>(r)); ++k) {
						sum += a_src[base + k * a_step];
					}
					a_dst[base + i * a_step] = sum / (2 * r + 1);
				}
			}
		};
		for (int pass = 0; pass < 2; ++pass) {
			blur(a, b, a_w, a_h, 1, a_w);
			blur(b, a, a_h, a_w, a_w, 1);
		}
		std::vector<std::uint8_t> out(a.size());
		for (std::size_t i = 0; i < a.size(); ++i) {
			out[i] = static_cast<std::uint8_t>(std::min<std::uint32_t>(a[i] * 4, 255) * 200 / 255);
		}
		return out;
	}

	// Premultiplied text lines: a color pass on black plus an all-white pass whose grayscale coverage is the alpha.
	std::vector<std::uint32_t> TextPixels(SIZE a_size, bool a_splash)
	{
		void*      bits = nullptr;
		const auto bmp = CreateDib(a_size.cx, a_size.cy, bits);
		if (!bmp) {
			return {};
		}
		const auto  mem = CreateCompatibleDC(nullptr);
		const auto  oldBmp = SelectObject(mem, bmp);
		const RECT  rc{ 0, 0, a_size.cx, a_size.cy };
		const auto* px = static_cast<const std::uint32_t*>(bits);
		const auto  n = static_cast<std::size_t>(a_size.cx) * a_size.cy;

		Fill(mem, rc, RGB(0, 0, 0));
		DrawLines(mem, rc, false, false, a_splash);
		GdiFlush();
		std::vector<std::uint32_t> out(px, px + n);
		Fill(mem, rc, RGB(0, 0, 0));
		DrawLines(mem, rc, true, false, a_splash);
		GdiFlush();
		std::vector<std::uint8_t> cover(n);
		for (std::size_t i = 0; i < n; ++i) {
			cover[i] = static_cast<std::uint8_t>((px[i] >> 8) & 0xFF);
		}
		const auto halo = settings.textShadow ? Halo(cover, a_size.cx, a_size.cy) : std::vector<std::uint8_t>(n);
		for (std::size_t i = 0; i < n; ++i) {
			const std::uint32_t a = cover[i];
			const auto          c = out[i];
			const auto          ch = [&](int a_shift) { return std::min<std::uint32_t>((c >> a_shift) & 0xFF, a) << a_shift; };
			// The text over its black halo, so it reads on bright frames like the Bethesda logo.
			const std::uint32_t alpha = a + halo[i] * (255 - a) / 255;
			out[i] = ch(0) | ch(8) | ch(16) | (alpha << 24);
		}
		SelectObject(mem, oldBmp);
		DeleteDC(mem);
		DeleteObject(bmp);
		return out;
	}

	// A see-through image: the window takes premultiplied pixels, GDI writes no alpha so it is set here for what GDI draws.
	void PresentLayered()
	{
		RECT rc{};
		GetClientRect(splash, &rc);
		void*      bits = nullptr;
		const auto bmp = CreateDib(rc.right, rc.bottom, bits);
		if (!bmp) {
			return;
		}
		const auto mem = CreateCompatibleDC(nullptr);
		const auto oldBmp = SelectObject(mem, bmp);
		auto*      px = static_cast<std::uint32_t*>(bits);
		const auto at = [&](LONG a_x, LONG a_y) -> std::uint32_t& { return px[std::size_t(a_y) * rc.right + a_x]; };
		const auto mix = [](std::uint32_t a_top, std::uint32_t a_under) {
			const std::uint32_t keep = 255 - (a_top >> 24);
			std::uint32_t       out = 0;
			for (const int shift : { 0, 8, 16, 24 }) {
				out |= (((a_top >> shift) & 0xFF) + ((a_under >> shift) & 0xFF) * keep / 255) << shift;
			}
			return out;
		};
		const auto opaque = [&](const RECT& a_r) {
			for (LONG y = a_r.top; y < a_r.bottom; ++y) {
				for (LONG x = a_r.left; x < a_r.right; ++x) {
					at(x, y) |= 0xFF000000;
				}
			}
		};

		for (LONG y = 0; y < pictureSize.cy; ++y) {
			std::copy_n(picturePixels + std::size_t(y) * pictureSize.cx, pictureSize.cx, &at(1, y + 1));
		}
		const RECT box{ 1, rc.bottom - StatusHeight() - 1, rc.right - 1, rc.bottom - 1 };
		const SIZE boxSize{ box.right - box.left, box.bottom - box.top };
		if (const auto text = TextPixels(boxSize, true); !text.empty()) {
			for (LONG y = 0; y < boxSize.cy; ++y) {
				for (LONG x = 0; x < boxSize.cx; ++x) {
					auto& p = at(box.left + x, box.top + y);
					p = mix(text[std::size_t(y) * boxSize.cx + x], p);
				}
			}
		}
		// The buttons get an opaque backing, they could not be seen or clicked over a transparent part of the image.
		const auto minimize = ButtonRect(Button::Minimize);
		const auto close = ButtonRect(Button::Close);
		const RECT buttons{ minimize.left, minimize.top, close.right, close.bottom };
		for (LONG y = buttons.top; y < buttons.bottom; ++y) {
			for (LONG x = buttons.left; x < buttons.right; ++x) {
				auto& p = at(x, y);
				p = mix(p, 0xFF121214);
			}
		}
		const auto track = BarTrack(box, true);
		Fill(mem, track, Track);
		Fill(mem, BarFill(box, true), settings.barColor);
		DrawButtons(mem);
		GdiFlush();
		opaque(track);
		opaque(buttons);

		SIZE          size{ rc.right, rc.bottom };
		POINT         from{};
		BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
		if (!UpdateLayeredWindow(splash, nullptr, nullptr, &size, mem, &from, 0, &blend, ULW_ALPHA)) {
			static bool logged = false;
			if (!std::exchange(logged, true)) {
				logger::error("UpdateLayeredWindow failed ({})", GetLastError());
			}
		}
		SelectObject(mem, oldBmp);
		DeleteDC(mem);
		DeleteObject(bmp);
	}

	void Paint(HWND a_hwnd)
	{
		PAINTSTRUCT ps;
		const auto  dc = BeginPaint(a_hwnd, &ps);
		if (a_hwnd == splash && pictureAlpha) {
			PresentLayered();
			EndPaint(a_hwnd, &ps);
			return;
		}
		RECT        rc;
		GetClientRect(a_hwnd, &rc);
		const auto mem = CreateCompatibleDC(dc);
		const auto bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
		const auto oldBmp = SelectObject(mem, bmp);
		if (a_hwnd == splash) {
			RenderSplash(mem, rc);
		} else {
			Render(mem, rc);
		}
		BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
		SelectObject(mem, oldBmp);
		DeleteObject(bmp);
		DeleteDC(mem);
		EndPaint(a_hwnd, &ps);
	}

	POINT dragOffset{};  // cursor position inside the splash while it is dragged
	void  CloseGame();

	void SetHovered(Button a_button)
	{
		if (hovered != a_button) {
			hovered = a_button;
			InvalidateRect(splash, nullptr, FALSE);
		}
	}

	constexpr UINT    CloseSplashMsg = WM_APP + 1;
	std::atomic<HWND> hookedSplash = nullptr;  // under Wine's Wayland driver, closed just before the game creates its window
	void              DestroySplash();

	LRESULT CALLBACK WndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wp, LPARAM a_lp)
	{
		const POINT pt{ GET_X_LPARAM(a_lp), GET_Y_LPARAM(a_lp) };
		switch (a_msg) {
		case CloseSplashMsg:
			DestroySplash();
			return 0;
		case WM_PAINT:
			if (a_hwnd == window || a_hwnd == splash) {
				Paint(a_hwnd);
				return 0;
			}
			break;
		case WM_NCHITTEST:
			if (a_hwnd != splash) {
				return HTTRANSPARENT;
			}
			break;
		// Splash: dragged anywhere by hand, HTCAPTION hands the move to the WM and stutters on the first drag under Wine.
		case WM_LBUTTONDOWN:
			if (a_hwnd == splash) {
				if (const auto b = ButtonAt(pt); b != Button::None) {
					pressed = b;
					SetCapture(splash);
					InvalidateRect(splash, nullptr, FALSE);
					return 0;
				}
				POINT cursor{};
				RECT  r{};
				GetCursorPos(&cursor);
				GetWindowRect(splash, &r);
				dragOffset = { cursor.x - r.left, cursor.y - r.top };
				SetCapture(splash);
				return 0;
			}
			break;
		case WM_MOUSEMOVE:
			if (a_hwnd == splash && GetCapture() == splash && pressed == Button::None) {
				POINT cursor{};
				GetCursorPos(&cursor);
				SetWindowPos(splash, nullptr, cursor.x - dragOffset.x, cursor.y - dragOffset.y, 0, 0,
					SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				return 0;
			}
			if (a_hwnd == splash) {
				TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, splash, 0 };
				TrackMouseEvent(&tme);
				SetHovered(ButtonAt(pt));
				return 0;
			}
			break;
		case WM_MOUSELEAVE:
			if (a_hwnd == splash) {
				SetHovered(Button::None);
				return 0;
			}
			break;
		case WM_LBUTTONUP:
			if (a_hwnd == splash && GetCapture() == splash) {
				const auto was = pressed;
				ReleaseCapture();
				if (was != Button::None && ButtonAt(pt) == was) {
					SetHovered(Button::None);
					if (was == Button::Close) {
						CloseGame();
					} else {
						ShowWindow(splash, SW_MINIMIZE);
					}
				}
				return 0;
			}
			break;
		case WM_CAPTURECHANGED:
			if (a_hwnd == splash && pressed != Button::None) {
				pressed = Button::None;
				InvalidateRect(splash, nullptr, FALSE);
			}
			break;
		case WM_CLOSE:
			if (a_hwnd == splash) {
				CloseGame();  // Alt+F4 or the taskbar close quits the game like the close button
				return 0;
			}
			break;
		case WM_MOUSEACTIVATE:
			if (a_hwnd != splash) {
				return MA_NOACTIVATE;
			}
			break;
		case WM_ERASEBKGND:
			return 1;
		}
		return DefWindowProcW(a_hwnd, a_msg, a_wp, a_lp);
	}

	void CreateOverlay(HWND a_parent)
	{
		window = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE, ClassName,
			L"SkyLine", WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0, Px(300), Px(48), a_parent, nullptr,
			GetModuleHandleW(nullptr), nullptr);
		if (!window) {
			logger::error("CreateWindowEx failed ({})", GetLastError());
			return;
		}
		// DPI awareness of the game window and ours (0 unaware, 1 system, 2 per monitor), the child must stay 2 to be sized right.
		using Context_t = HANDLE(WINAPI*)(HWND);
		using Awareness_t = int(WINAPI*)(HANDLE);
		const auto user32 = GetModuleHandleW(L"user32.dll");
		const auto context = reinterpret_cast<Context_t>(GetProcAddress(user32, "GetWindowDpiAwarenessContext"));
		const auto awareness = reinterpret_cast<Awareness_t>(GetProcAddress(user32, "GetAwarenessFromDpiAwarenessContext"));
		if (context && awareness) {
			logger::info("[{:6.1f} s] DPI awareness: game window {}, status window {}", Status::Uptime(),
				awareness(context(a_parent)), awareness(context(window)));
		}
		Layout();
		ShowWindow(window, SW_SHOWNA);
	}

	POINT splashAt{};

	// Not topmost, draggable anywhere, and reopened where it was left if that is still on a monitor.
	void CreateSplash()
	{
		const auto size = SplashSize();
		const auto wa = WorkArea();
		splashAt = { wa.left + (wa.right - wa.left - size.cx) / 2, wa.top + (wa.bottom - wa.top - size.cy) / 2 };
		if (const auto& p = Status::splashPos) {
			const RECT r{ p->x, p->y, p->x + size.cx, p->y + size.cy };
			if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
				splashAt = *p;
			}
		}
		// A normal app window, so it is in the taskbar and alt-tab (Wine leaves NOACTIVATE windows out of both).
		// WS_MINIMIZEBOX tells the window manager (via Wine's Motif hints) that it may be minimized.
		splash = CreateWindowExW(WS_EX_APPWINDOW | (pictureAlpha ? WS_EX_LAYERED : 0), ClassName, L"Skyrim startup", WS_POPUP | WS_MINIMIZEBOX, splashAt.x,
			splashAt.y, size.cx, size.cy, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (!splash) {
			logger::error("CreateWindowEx (splash) failed ({})", GetLastError());
			return;
		}
		for (const auto [type, metric] : { std::pair{ ICON_SMALL, SM_CXSMICON }, std::pair{ ICON_BIG, SM_CXICON } }) {
			const auto px = GetSystemMetrics(metric);
			if (const auto icon = LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_SHARED)) {
				SendMessageW(splash, WM_SETICON, type, reinterpret_cast<LPARAM>(icon));
			}
		}
		if (pictureAlpha) {
			PresentLayered();  // a layered window shows nothing until it has pixels
		}
		// Activated like any starting program, KWin keeps a new window that is not activated behind the active one.
		ShowWindow(splash, SW_SHOW);
	}

	void DestroySplash()
	{
		hookedSplash = nullptr;
		if (splash) {
			RECT r{};
			GetWindowRect(splash, &r);
			if (r.left != splashAt.x || r.top != splashAt.y) {
				Status::splashPos = POINT{ r.left, r.top };
				Status::SaveState(Status::last);  // the last start stays as loaded until this one finishes
			}
			DestroyWindow(splash);
			splash = nullptr;
		}
	}

	// Closing the splash ends the whole game. The game's threads are busy loading and can't be asked to quit.
	void CloseGame()
	{
		logger::info("[{:6.1f} s] Splash closed, closing the game", Status::Uptime());
		DestroySplash();
		TerminateProcess(GetCurrentProcess(), 0);
	}

	// Premultiplied pixels for the Present hook.
	void PublishFrame()
	{
		const auto size = BoxSize();
		auto       out = TextPixels(size, false);
		if (out.empty()) {
			return;
		}
		const RECT rc{ 0, 0, size.cx, size.cy };
		// Opaque, so the track stays visible over dark loading screens.
		const auto opaque = [](COLORREF a_c) { return 0xFF000000u | (GetRValue(a_c) << 16) | (GetGValue(a_c) << 8) | GetBValue(a_c); };
		const auto put = [&](const RECT& a_r, std::uint32_t a_pixel) {
			for (LONG y = std::max(a_r.top, 0L); y < std::min(a_r.bottom, static_cast<LONG>(size.cy)); ++y) {
				for (LONG x = std::max(a_r.left, 0L); x < std::min(a_r.right, static_cast<LONG>(size.cx)); ++x) {
					out[static_cast<std::size_t>(y) * size.cx + x] = a_pixel;
				}
			}
		};
		put(BarTrack(rc, false), opaque(Track));
		put(BarFill(rc, false), opaque(settings.barColor));
		Frame::Publish(std::move(out), size.cx, size.cy, Where());
	}

	// What made a window that can't be the game window, e.g. another plugin's splash, "" when it can be.
	std::string ForeignWindow(HWND a_hwnd)
	{
		const auto instance = reinterpret_cast<HMODULE>(GetClassLongPtrW(a_hwnd, GCLP_HMODULE));
		wchar_t    wpath[1024];
		if (instance && GetModuleFileNameW(instance, wpath, static_cast<DWORD>(std::size(wpath)))) {
			const auto path = SKSE::stl::utf16_to_utf8(wpath).value_or(""s);
			if (Lower(path).find("\\skse\\plugins\\") != std::string::npos) {
				return std::string(BaseName(path));
			}
		}
		// The game window is an unowned window, neither layered nor a tool window.
		const auto exStyle = GetWindowLongPtrW(a_hwnd, GWL_EXSTYLE);
		if (GetWindow(a_hwnd, GW_OWNER) || (exStyle & (WS_EX_LAYERED | WS_EX_TOOLWINDOW))) {
			char name[256]{};
			GetClassNameA(a_hwnd, name, static_cast<int>(std::size(name)));
			return "window class "s + name;
		}
		return {};
	}

	// a_foreign gets what made the first window of this process left out as not the game's.
	HWND FindGameWindow(std::string* a_foreign = nullptr)
	{
		struct Search
		{
			DWORD        pid;
			HWND         best;
			long         area;
			std::string* foreign;
		} s{ GetCurrentProcessId(), nullptr, 0, a_foreign };
		EnumWindows(
			[](HWND a_hwnd, LPARAM a_lp) -> BOOL {
				auto& s = *reinterpret_cast<Search*>(a_lp);
				DWORD pid = 0;
				GetWindowThreadProcessId(a_hwnd, &pid);
				if (pid != s.pid || a_hwnd == window || a_hwnd == splash || !IsWindowVisible(a_hwnd)) {
					return TRUE;
				}
				RECT r{};
				GetClientRect(a_hwnd, &r);
				const long area = (r.right - r.left) * (r.bottom - r.top);
				if (r.right - r.left < 320 || r.bottom - r.top < 240) {
					return TRUE;
				}
				if (auto by = ForeignWindow(a_hwnd); !by.empty()) {
					if (s.foreign && s.foreign->empty()) {
						*s.foreign = std::move(by);
					}
					return TRUE;
				}
				if (area > s.area) {
					s.best = a_hwnd;
					s.area = area;
				}
				return TRUE;
			},
			reinterpret_cast<LPARAM>(&s));
		return s.best;
	}

	void Tick()
	{
		static int ticks = 0;
		++ticks;
		if (ticks % 10 == 1) {
			Sampler::RefreshModules();
		}

		if (!closing) {
			if (const auto id = Status::mainThread.load(); id && id != mainSampler.Id()) {
				mainSampler.Open(id);
			}
			if (const auto id = Status::dataThread.load(); id && id != Status::mainThread && id != dataSampler.Id()) {
				dataSampler.Open(id);
			}
			// Each sample stands for the time since the last one, at most 0.5 s when the overlay thread was held up.
			const double now = Status::Uptime();
			const double step = sampledAt > 0 ? std::min(now - sampledAt, 0.5) : 0.1;
			sampledAt = now;
			const auto mainLabel = mainSampler.Sample();
			mainBusy.Add(mainLabel);
			mainBusy.Log("main");
			mainTime.Add(mainLabel, now, step);
			if (dataSampler.Id()) {
				bool       skse = false;
				const auto data = dataSampler.Sample(&skse);
				dataBusy.Add(data);
				dataBusy.Log("data");
				if (!Status::dataLoaded) {
					dataTime.Add(data, now, step);
					if (skse) {
						Status::DataLoadedSent(now, data.empty() ? "a sample"s : "a sample in " + data);
					}
					if (Status::dataLoadedSentAt > 0) {
						handlerTime.Add(data, now, step);
					}
				}
			}

			// Done once the main menu is up and data is loaded, or 3 minutes after kDataLoaded if no main menu shows up.
			const bool ready = Status::dataLoaded && Status::mainMenu;
			const bool timeout = Status::dataLoaded && Status::Uptime() - Status::dataLoadedAt > 180.0;
			if (ready || timeout) {
				closeFrom = Progress(false);
				closing = true;
				readyAt = Status::Uptime();
				Status::done = true;
				std::uint32_t                 plugins;
				std::vector<Status::Activity> activities;
				{
					std::scoped_lock l{ Status::shared.lock };
					plugins = static_cast<std::uint32_t>(Status::shared.loaded.size());
					activities = Status::shared.activities;
				}
				logger::info("[{:6.1f} s] {} ({} plugins)", readyAt,
					ready ? "Main menu up, startup finished" : "No main menu 180 s after kDataLoaded, closing", plugins);
				std::ranges::sort(activities, {}, &Status::Activity::firstAt);
				for (const auto& a : activities) {
					logger::info("  {} loading {}: {} files, {:.1f}-{:.1f} s", a.owner.empty() ? "game"s : a.owner, a.noun,
						a.count, a.firstAt, a.lastAt);
				}
				logger::info("Sampled time by the plugin or game code running, system DLLs skipped:");
				mainTime.Log("main thread");
				dataTime.Log("data thread until kDataLoaded");
				handlerTime.Log("data thread in kDataLoaded handlers before SkyLine's");
				if (ready) {
					splashEstimate.Close(windowAt);
					gameEstimate.Close(readyAt);
					Status::SaveState({ .plugins = plugins, .seconds = readyAt, .window = windowAt, .files = scanFiles,
						.splash = { splashEstimate.entered[1], splashEstimate.entered[2] },
						.game = { gameEstimate.entered[1], gameEstimate.entered[2], gameEstimate.entered[3] }, .stages = true });
				}
			}
		} else if (Status::Uptime() - readyAt > settings.lingerSeconds) {
			KillTimer(nullptr, timer);
			Frame::Stop();
			DestroySplash();
			if (window) {
				DestroyWindow(window);
				window = nullptr;
			}
			PostQuitMessage(0);
			return;
		}

		if (!game) {
			static bool foreignLogged = false;
			std::string foreign;
			const auto  g = FindGameWindow(splash || !foreignLogged ? &foreign : nullptr);
			if (!foreign.empty()) {
				if (!foreignLogged) {
					foreignLogged = true;
					logger::info("[{:6.1f} s] Not the game window: {}", Status::Uptime(), foreign);
				}
				// Another plugin's splash came up after ours, so ours gives way as it would have at the start.
				if (splash && !g) {
					DestroySplash();
					logger::info("[{:6.1f} s] Another splash is up ({}), splash closed", Status::Uptime(), foreign);
				}
			}
			if (g) {
				game = g;
				GetWindowRect(game, &gameRect);
				logger::info("[{:6.1f} s] Game window found ({}x{})", Status::Uptime(), gameRect.right - gameRect.left,
					gameRect.bottom - gameRect.top);
				splashFrom = Progress(true);
				windowAt = Status::Uptime();
				gameEstimate.entered[0] = windowAt;
				scanFiles = Status::looseFiles;
				// A splash shown behind another app never had focus but is active on this thread, and the child window's input link would make it the active window the game sets its keyboard up with.
				if (splash && GetActiveWindow() == splash && GetForegroundWindow() != splash) {
					DestroySplash();
					logger::info("[{:6.1f} s] Splash closed early, it was active but not in front", Status::Uptime());
				}

				CreateOverlay(game);
			}
		} else if (!IsWindow(game)) {
			game = nullptr;
		}
		// The splash stays until its bar has filled up.
		if (splash && windowAt && Status::Uptime() - windowAt > FinishSeconds) {
			DestroySplash();
		}

		const auto oldHead = head;
		const auto oldDetail = detail;
		ComposeText();
		// The text is fixed once startup is done, but the bar still fills up then.
		static double drawnSplash = -1, drawnGame = -1;
		const double  splashProgress = Progress(true);
		const double  gameProgress = Progress(false);
		const bool    changed = head != oldHead || detail != oldDetail || splashProgress != drawnSplash || gameProgress != drawnGame;
		drawnSplash = splashProgress;
		drawnGame = gameProgress;
		RECT now{};
		if (game) {
			GetClientRect(game, &now);
		}
		if (game && !hooked) {
			hooked = Frame::TryHook();
		}
		// The game is presenting frames: draw into them instead of the child window (which they would cover).
		const double last = Frame::LastPresent();
		const bool   nowPresenting = last > 0 && Status::Uptime() - last < 2.0;  // hitches of a second are normal
		if (nowPresenting != presenting) {
			presenting = nowPresenting;
			// Wine's Wayland driver keeps showing a child brought back over frames, even after it is hidden and destroyed.
			const bool stayHidden = !presenting && GetModuleHandleW(L"winewayland.drv") != nullptr;
			logger::info("[{:6.1f} s] Game {}, status {}", Status::Uptime(), presenting ? "presenting frames" : "stopped presenting",
				presenting ? "drawn into frames" : stayHidden ? "stays in the last frame" : "in window");
			if (window && !stayHidden) {
				ShowWindow(window, presenting ? SW_HIDE : SW_SHOWNA);
			}
		}
		if (splash && changed) {
			InvalidateRect(splash, nullptr, FALSE);
		}
		if (presenting) {
			if (changed || !EqualRect(&now, &gameRect)) {
				gameRect = now;
				PublishFrame();
			}
		} else if (window && (changed || !EqualRect(&now, &gameRect))) {
			gameRect = now;
			Layout();
			InvalidateRect(window, nullptr, FALSE);
		}
	}

	void Run()
	{
		// Physical pixels on any DPI (Proton is often 120-144 dpi), looked up by name since this SDK config hides the Win10 API.
		using SetContext_t = HANDLE(WINAPI*)(HANDLE);
		if (const auto set = reinterpret_cast<SetContext_t>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"))) {
			set(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
		}
		// SkyrimSE.exe is DPI unaware, without mixed hosting Windows forces the child window to that too and stretches it.
		using SetHosting_t = int(WINAPI*)(int);
		if (const auto hosting = reinterpret_cast<SetHosting_t>(
				GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiHostingBehavior"))) {
			hosting(1);  // DPI_HOSTING_BEHAVIOR_MIXED
		}
		const auto dc = GetDC(nullptr);
		const int  dpi = GetDeviceCaps(dc, LOGPIXELSY);
		ReleaseDC(nullptr, dc);
		scalePct = settings.dpiScaling ? std::clamp(dpi * 100 / 96, 100, 400) : 100;

		const auto font = [](int a_px, int a_weight) {
			return CreateFontW(-Px(a_px), 0, 0, 0, a_weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, settings.font.c_str());
		};
		headFont = font(settings.fontSize, FW_SEMIBOLD);
		detailFont = font(settings.fontSize * 13 / 16, FW_NORMAL);
		if (const auto screen = GetDC(nullptr)) {
			const auto previous = SelectObject(screen, headFont);
			wchar_t    used[LF_FACESIZE]{};
			GetTextFaceW(screen, static_cast<int>(std::size(used)), used);
			SelectObject(screen, previous);
			ReleaseDC(nullptr, screen);
			const auto name = SKSE::stl::utf16_to_utf8(used).value_or("?"s);
			if (_wcsicmp(used, settings.font.c_str()) == 0) {
				logger::info("Font: {}", name);
			} else {
				const auto asked = SKSE::stl::utf16_to_utf8(settings.font).value_or("?"s);
				if (settings.font == Overlay::Settings{}.font) {
					logger::info("Font {} isn't installed, the system used {} instead", asked, name);
				} else {
					logger::warn("Font {} isn't installed, the system used {} instead", asked, name);
				}
			}
		}
		// The dragon icon in SkyrimSE.exe's own resources (group 101, 256 px), so the mod ships no artwork.
		logo = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, Px(150), Px(150), LR_DEFAULTCOLOR));

		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
		wc.lpszClassName = ClassName;
		RegisterClassExW(&wc);

		const double splashMarks[] = { 0, Status::last.splash[0], Status::last.splash[1], Status::last.window };
		splashEstimate.Init(splashMarks, Status::last.stages, { 1, 1, static_cast<int>(std::size(Status::scanFolders)) });
		const double gameMarks[] = { Status::last.window, Status::last.game[0], Status::last.game[1], Status::last.game[2],
			Status::last.seconds };
		gameEstimate.Init(gameMarks, Status::last.stages, { 1, 1, 1, 1 });

		logger::info("[{:6.1f} s] Overlay thread started (dpi {}, stage ends {} splash {:.2f} {:.2f} game {:.2f} {:.2f} {:.2f})",
			Status::Uptime(), dpi, Status::last.stages ? "from last start" : "in equal steps", splashEstimate.end[0],
			splashEstimate.end[1], gameEstimate.end[0], gameEstimate.end[1], gameEstimate.end[2]);
		ComposeText();
		// Wine's Wayland driver makes the game window a popup of the active window, and destroying that parent ends the Wayland connection and the game.
		// So there the splash has to be gone before the game creates its window.
		const bool waylandDriver = GetModuleHandleW(L"winewayland.drv") != nullptr;
		std::string otherSplash;
		bool        showSplash = settings.splash && !FindGameWindow(&otherSplash);
		// Another plugin's splash is already up, two splashes would cover each other.
		if (showSplash && !otherSplash.empty()) {
			logger::info("[{:6.1f} s] Another splash is up ({}), no splash", Status::Uptime(), otherSplash);
			showSplash = false;
		}
		if (showSplash && waylandDriver && !Status::HookGameWindow()) {
			logger::warn("Wine's Wayland driver is in use and the game window can't be hooked, no splash");
			showSplash = false;
		}
		if (showSplash) {
			// The splash image replaces the logo. Loaded after the text, which sets the height of its darkened bottom.
			const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
			const auto limit = SplashLimit();
			picture = LoadPicture(settings.splashImage, { limit.cx - 2, limit.cy - 2 }, pictureSize);
			if (com) {
				CoUninitialize();
			}
			CreateSplash();
			if (waylandDriver) {
				hookedSplash = splash;
			}
		}
		// With a timer proc, so ticks go on inside any modal loop of the thread.
		timer = SetTimer(nullptr, 0, TickMs, [](HWND, UINT, UINT_PTR, DWORD) { Tick(); });

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			DispatchMessageW(&msg);
		}
		DeleteObject(headFont);
		DeleteObject(detailFont);
		if (logo) {
			DestroyIcon(logo);
		}
		if (picture) {
			DeleteObject(picture);
		}
		UnregisterClassW(ClassName, GetModuleHandleW(nullptr));
		logger::info("[{:6.1f} s] Overlay closed", Status::Uptime());
	}
}

namespace Overlay
{
	void CloseSplash()
	{
		// Only the overlay thread can destroy it. The timeout keeps the game from hanging here.
		if (const auto s = hookedSplash.exchange(nullptr)) {
			DWORD_PTR result = 0;
			const auto sent = SendMessageTimeoutW(s, CloseSplashMsg, 0, 0, SMTO_NORMAL, 2000, &result);
			logger::info("[{:6.1f} s] Splash closed before the game window{}", Status::Uptime(), sent ? "" : " timed out");
		}
	}

	void Start(const Settings& a_settings)
	{
		settings = a_settings;
		std::thread(Run).detach();
	}
}
