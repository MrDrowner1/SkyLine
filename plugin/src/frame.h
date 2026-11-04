#pragma once

// Draws the status box into the game's frames from the loading screen on, since Proton copies frames over child windows.
namespace Frame
{
	// Hooks Present on the game's swapchain once it exists. Call until it returns true.
	bool TryHook();

	// Seconds since start of the last frame the game presented (0 = none yet).
	double LastPresent();

	// How many frames have been presented through the hook.
	std::uint64_t Presented();

	// Where the box goes: per axis anchor 0 start, 1 centre, 2 end, margin px from that edge, then moved by offset px.
	struct Spot
	{
		POINT anchor{};
		POINT offset{};
		int   margin = 0;
	};

	// Top-left corner of an a_box sized box in a_area, kept inside it.
	inline POINT Place(SIZE a_area, SIZE a_box, const Spot& a_spot)
	{
		const auto at = [&](LONG a_space, LONG a_len, LONG a_anchor, LONG a_offset) {
			const LONG p = (a_anchor == 0 ? a_spot.margin : a_anchor == 1 ? (a_space - a_len) / 2 : a_space - a_spot.margin - a_len) + a_offset;
			return std::clamp(p, 0L, std::max(0L, a_space - a_len));
		};
		return { at(a_area.cx, a_box.cx, a_spot.anchor.x, a_spot.offset.x), at(a_area.cy, a_box.cy, a_spot.anchor.y, a_spot.offset.y) };
	}

	// The box as top-down premultiplied BGRA (alpha 0 = game shows through), blended at a_spot.
	void Publish(std::vector<std::uint32_t> a_bgra, int a_width, int a_height, const Spot& a_spot);

	// The cover image as top-down opaque BGRA, stretched over the whole frame under the box.
	void PublishCover(std::vector<std::uint32_t> a_bgra, int a_width, int a_height);

	// Stop drawing the cover image, the box stays.
	void StopCover();

	// Stop drawing. The hook stays as a pass-through, since another mod may have hooked over it.
	void Stop();
}
