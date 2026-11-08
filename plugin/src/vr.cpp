#include "vr.h"
#include "state.h"

#ifdef ENABLE_SKYRIM_VR
#	include <openvr.h>

namespace
{
	constexpr float  Distance = 2.0f;           // metres in front of the eyes
	constexpr float  Below = 0.35f;             // metres under eye level
	constexpr float  MetresPerPixel = 0.003f;   // at 96 dpi, 16 px text is about 1.4 degrees tall
	constexpr float  MaxMetres = 2.0f;
	constexpr double UpdateSeconds = 0.2;
	constexpr std::uint32_t Plate = 150;        // alpha of the dark plate behind the text

	std::atomic<bool> initialized{ false };
	std::atomic<bool> initHooked{ false };

	vr::IVROverlay*         api = nullptr;
	vr::VROverlayHandle_t   handle = vr::k_ulOverlayHandleInvalid;
	bool                    failed = false;
	bool                    visible = false;
	bool                    drawErrorLogged = false;
	double                  drawnAt = 0;
	float                   metres = 0;
	std::vector<std::uint32_t> rgba;

	void Fail(std::string_view a_what, vr::EVROverlayError a_error)
	{
		logger::warn("[{:6.1f} s] Headset overlay off, {} failed ({})", Status::Uptime(), a_what, static_cast<int>(a_error));
		failed = true;
		if (api && handle != vr::k_ulOverlayHandleInvalid) {
			api->DestroyOverlay(handle);
		}
		handle = vr::k_ulOverlayHandleInvalid;
	}

	bool Create()
	{
		const auto module = GetModuleHandleW(L"openvr_api.dll");
		const auto get = module ? reinterpret_cast<void*(*)(const char*, vr::EVRInitError*)>(GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
		if (!get) {
			logger::warn("[{:6.1f} s] Headset overlay off, no VR_GetGenericInterface in openvr_api.dll", Status::Uptime());
			failed = true;
			return false;
		}
		vr::EVRInitError initError = vr::VRInitError_None;
		api = static_cast<vr::IVROverlay*>(get(vr::IVROverlay_Version, &initError));
		if (!api) {
			logger::warn("[{:6.1f} s] Headset overlay off, no {} ({})", Status::Uptime(), vr::IVROverlay_Version, static_cast<int>(initError));
			failed = true;
			return false;
		}
		if (const auto e = api->CreateOverlay("skyline.status", "SkyLine", &handle); e != vr::VROverlayError_None) {
			handle = vr::k_ulOverlayHandleInvalid;
			Fail("CreateOverlay", e);
			return false;
		}
		// Fixed to the head, the world isn't there yet and the user can be facing any way.
		vr::HmdMatrix34_t pose{ { { 1, 0, 0, 0 }, { 0, 1, 0, -Below }, { 0, 0, 1, -Distance } } };
		if (const auto e = api->SetOverlayTransformTrackedDeviceRelative(handle, vr::k_unTrackedDeviceIndex_Hmd, &pose); e != vr::VROverlayError_None) {
			Fail("SetOverlayTransformTrackedDeviceRelative", e);
			return false;
		}
		api->SetOverlayFlag(handle, vr::VROverlayFlags_RGSS4X, true);  // cleaner text
		logger::info("[{:6.1f} s] Headset overlay created ({})", Status::Uptime(), vr::IVROverlay_Version);
		return true;
	}
}

namespace Vr
{
	void MarkInitialized()
	{
		if (!initialized.exchange(true)) {
			logger::info("[{:6.1f} s] OpenVR is up", Status::Uptime());
		}
	}

	void SetInitHooked(bool a_hooked) { initHooked = a_hooked; }

	void FirstFrame()
	{
		if (!initHooked) {
			MarkInitialized();
		}
	}

	bool Due()
	{
		if (failed || !initialized || Status::Uptime() - drawnAt < UpdateSeconds) {
			return false;
		}
		return handle != vr::k_ulOverlayHandleInvalid || Create();
	}

	void Show(const std::vector<std::uint32_t>& a_bgra, int a_width, int a_height, float a_scale)
	{
		drawnAt = Status::Uptime();
		rgba.resize(a_bgra.size());
		for (std::size_t i = 0; i < a_bgra.size(); ++i) {
			const auto p = a_bgra[i];
			const auto a = p >> 24;
			// The plate keeps the text readable on any background, and the overlay takes straight alpha.
			const auto alpha = a + (255 - a) * Plate / 255;
			const auto straight = [&](int a_shift) { return std::min<std::uint32_t>(((p >> a_shift) & 0xFF) * 255 / std::max<std::uint32_t>(alpha, 1), 255); };
			rgba[i] = straight(16) | (straight(8) << 8) | (straight(0) << 16) | (alpha << 24);
		}
		if (const auto e = api->SetOverlayRaw(handle, rgba.data(), a_width, a_height, 4); e != vr::VROverlayError_None) {
			if (!std::exchange(drawErrorLogged, true)) {
				logger::warn("[{:6.1f} s] SetOverlayRaw {}x{} failed ({})", Status::Uptime(), a_width, a_height, static_cast<int>(e));
			}
			return;
		}
		// The box widens with long text, the pixels must keep their size.
		const float want = std::min(a_width * a_scale * MetresPerPixel, MaxMetres);
		if (want != metres) {
			metres = want;
			api->SetOverlayWidthInMeters(handle, metres);
		}
		if (!visible) {
			visible = true;
			api->ShowOverlay(handle);
			logger::info("[{:6.1f} s] Headset overlay showing", Status::Uptime());
		}
	}

	void Stop()
	{
		failed = true;
		if (api && handle != vr::k_ulOverlayHandleInvalid) {
			api->DestroyOverlay(handle);
			handle = vr::k_ulOverlayHandleInvalid;
			logger::info("[{:6.1f} s] Headset overlay closed", Status::Uptime());
		}
	}
}
#endif
