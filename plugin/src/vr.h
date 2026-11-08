#pragma once

#ifdef ENABLE_SKYRIM_VR
// Shows the status in the headset as an OpenVR overlay, the compositor keeps it up while the game is busy.
namespace Vr
{
	inline constexpr int MaxWidthPx = 560;  // at 96 dpi

	// The game's VR_Init returned, called on the game's thread.
	void MarkInitialized();
	// Whether the game's VR_Init import is hooked. When it isn't, the first frame stands in for it.
	void SetInitHooked(bool a_hooked);
	void FirstFrame();

	// True when the overlay should get a new image. Overlay thread only, like Show and Stop.
	bool Due();
	// Takes top-down premultiplied BGRA. a_scale converts pixels to pixels at 96 dpi.
	void Show(const std::vector<std::uint32_t>& a_bgra, int a_width, int a_height, float a_scale);
	void Stop();
}
#endif
