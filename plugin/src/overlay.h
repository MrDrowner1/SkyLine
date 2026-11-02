#pragma once

namespace Overlay
{
	struct Settings
	{
		int  fontSize = 16;  // px at 96 dpi
		int  lingerSeconds = 0;
		bool enabled = true;
		std::filesystem::path splashImage;  // empty shows the game's dragon icon
		bool     barOnly = false;  // only the progress bar, no text lines
		COLORREF barColor = RGB(110, 180, 230);
		POINT    anchor{ 0, 2 };  // place in the game window per axis: 0 left/top, 1 centre, 2 right/bottom
		POINT    offset{};        // px at 96 dpi from the anchored place, right and down
		int      barHeight = 4;   // px at 96 dpi
		int      barLength = 360;  // px at 96 dpi, 0 spans the game window
		bool     showDetail = true;  // the second line (times, counts, file names)
		bool     splash = true;      // the splash window before the game window exists
		int      splashWidth = 480;  // px at 96 dpi
		COLORREF textColor = RGB(236, 236, 236);  // the detail line is drawn a little dimmer
	};

	// Starts the overlay thread (own window + message loop, so it keeps updating while the game threads are busy).
	void Start(const Settings& a_settings);
	// Called on the game's thread just before it creates its window, only hooked under Wine's Wayland driver.
	void CloseSplash();
}
