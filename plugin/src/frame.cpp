#include "frame.h"
#include "state.h"

#include <d3d11.h>
#include <dxgi.h>

// Present hook that blends the overlay's premultiplied box into the back buffer, saving and restoring the state it touches.
// No ID3DDeviceContextState: creating one makes RTSS see a D3D10 device for the rest of the run, and its overlay stays hidden.

namespace
{
	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	Present_t originalPresent = nullptr;
	bool      released = false;         // render thread only
	bool      loggedSwapChain = false;  // render thread only

	std::atomic<bool>          stopped = false;
	std::atomic<std::int64_t>  lastPresentMs = 0;

	// Published by the overlay thread.
	std::mutex                 pending;
	std::vector<std::uint32_t> pendingPixels;
	int                        pendingWidth = 0;
	int                        pendingHeight = 0;
	Frame::Spot                pendingSpot;
	std::uint64_t              pendingVersion = 0;

	// Render thread only.
	ID3D11Device*             device = nullptr;
	ID3D11DeviceContext*      context = nullptr;
	ID3D11VertexShader*       vs = nullptr;
	ID3D11PixelShader*        psSrgb = nullptr;    // back buffer stores sRGB-encoded values: write as is
	ID3D11PixelShader*        psLinear = nullptr;  // _SRGB views and scRGB float: convert to linear
	ID3D11BlendState*         blend = nullptr;
	ID3D11RasterizerState*    raster = nullptr;
	ID3D11Texture2D*          texture = nullptr;
	ID3D11ShaderResourceView* view = nullptr;
	std::uint64_t             textureVersion = 0;
	int                       width = 0;
	int                       height = 0;
	Frame::Spot               spot;
	bool                      failed = false;
	POINT                     loggedAt{ -1, -1 };
	bool                      loggedFormat = false;

	constexpr char Shader[] = R"(
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD; };
V vs(uint id : SV_VertexID)
{
	V o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4(o.uv.x * 2 - 1, 1 - o.uv.y * 2, 0, 1);
	return o;
}
Texture2D<float4> tex : register(t0);
float4 ps(V i) : SV_Target
{
	uint w, h;
	tex.GetDimensions(w, h);
	float4 c = tex.Load(int3(i.uv * float2(w, h), 0));
#if LINEAR
	float3 s = c.a > 0 ? c.rgb / c.a : 0;
	s = s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4);
	c.rgb = s * c.a;
#endif
	return c;
}
)";

	template <class T>
	void SafeRelease(T*& a_p)
	{
		if (a_p) {
			a_p->Release();
			a_p = nullptr;
		}
	}

	void ReleaseAll()
	{
		SafeRelease(view);
		SafeRelease(texture);
		SafeRelease(raster);
		SafeRelease(blend);
		SafeRelease(psLinear);
		SafeRelease(psSrgb);
		SafeRelease(vs);
		SafeRelease(context);
		SafeRelease(device);
	}

	ID3DBlob* Compile(const char* a_entry, const char* a_target, bool a_linear)
	{
		using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
			UINT, UINT, ID3DBlob**, ID3DBlob**);
		static const auto compile = [] {
			const auto dll = LoadLibraryW(L"d3dcompiler_47.dll");
			return dll ? reinterpret_cast<Compile_t>(GetProcAddress(dll, "D3DCompile")) : nullptr;
		}();
		if (!compile) {
			logger::error("d3dcompiler_47.dll not available, status not drawn into frames");
			return nullptr;
		}
		const D3D_SHADER_MACRO defines[] = { { "LINEAR", a_linear ? "1" : "0" }, { nullptr, nullptr } };
		ID3DBlob*              code = nullptr;
		ID3DBlob*              errors = nullptr;
		const auto hr = compile(Shader, sizeof(Shader) - 1, "SkyLine", defines, nullptr, a_entry, a_target, 0, 0, &code,
			&errors);
		if (FAILED(hr)) {
			logger::error("Shader {} failed to compile: {}", a_entry,
				errors ? std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "?");
		}
		SafeRelease(errors);
		return code;
	}

	bool Init(IDXGISwapChain* a_swapChain)
	{
		if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
			return false;
		}
		device->GetImmediateContext(&context);
		const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
		auto* vsCode = Compile("vs", "vs_4_0", false);
		auto* psCode = Compile("ps", "ps_4_0", false);
		auto* psLinCode = Compile("ps", "ps_4_0", true);
		bool  ok = vsCode && psCode && psLinCode &&
		          SUCCEEDED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) &&
		          SUCCEEDED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &psSrgb)) &&
		          SUCCEEDED(device->CreatePixelShader(psLinCode->GetBufferPointer(), psLinCode->GetBufferSize(), nullptr, &psLinear));
		SafeRelease(vsCode);
		SafeRelease(psCode);
		SafeRelease(psLinCode);

		D3D11_BLEND_DESC bd{};
		auto&            rt = bd.RenderTarget[0];
		rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_ONE;  // premultiplied
		rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ZERO;  // keep the back buffer's alpha
		rt.DestBlendAlpha = D3D11_BLEND_ONE;
		rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		ok = ok && SUCCEEDED(device->CreateBlendState(&bd, &blend)) && SUCCEEDED(device->CreateRasterizerState(&rd, &raster));
		if (!ok) {
			logger::error("Shader setup failed, status not drawn into frames");
		} else {
			logger::info("[{:6.1f} s] Frame drawing ready (feature level {:x})", Status::Uptime(), static_cast<int>(level));
		}
		return ok;
	}

	// The render target view format for the back buffer, and whether the shader must write linear values.
	bool ViewFormat(DXGI_FORMAT a_format, DXGI_FORMAT& a_view, bool& a_linear)
	{
		a_view = a_format;
		a_linear = false;
		switch (a_format) {
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
			a_view = DXGI_FORMAT_B8G8R8A8_UNORM;
			return true;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
			a_view = DXGI_FORMAT_R8G8B8A8_UNORM;
			return true;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS:
			a_view = DXGI_FORMAT_R10G10B10A2_UNORM;
			return true;
		case DXGI_FORMAT_R16G16B16A16_TYPELESS:
			a_view = DXGI_FORMAT_R16G16B16A16_FLOAT;
			a_linear = true;
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8X8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:  // scRGB: linear, 1.0 = SDR white
			a_linear = true;
			return true;
		default:
			return false;
		}
	}

	// Everything the draw changes. The game tracks its own bindings, so they must be back as they were before Present.
	struct SavedState
	{
		D3D11_PRIMITIVE_TOPOLOGY  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11InputLayout*        layout = nullptr;
		ID3D11VertexShader*       vertex = nullptr;
		ID3D11HullShader*         hull = nullptr;
		ID3D11DomainShader*       domain = nullptr;
		ID3D11GeometryShader*     geometry = nullptr;
		ID3D11PixelShader*        pixel = nullptr;
		ID3D11ShaderResourceView* resource = nullptr;
		ID3D11RasterizerState*    rasterizer = nullptr;
		UINT                      viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT            viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		ID3D11BlendState*         blending = nullptr;
		FLOAT                     blendFactor[4]{};
		UINT                      sampleMask = 0;
		ID3D11DepthStencilState*  depth = nullptr;
		UINT                      stencilRef = 0;
		ID3D11RenderTargetView*   targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView*   depthTarget = nullptr;

		void Save()
		{
			context->IAGetPrimitiveTopology(&topology);
			context->IAGetInputLayout(&layout);
			context->VSGetShader(&vertex, nullptr, nullptr);
			context->HSGetShader(&hull, nullptr, nullptr);
			context->DSGetShader(&domain, nullptr, nullptr);
			context->GSGetShader(&geometry, nullptr, nullptr);
			context->PSGetShader(&pixel, nullptr, nullptr);
			context->PSGetShaderResources(0, 1, &resource);
			context->RSGetState(&rasterizer);
			context->RSGetViewports(&viewportCount, viewports);
			context->OMGetBlendState(&blending, blendFactor, &sampleMask);
			context->OMGetDepthStencilState(&depth, &stencilRef);
			context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthTarget);
		}

		// Also drops the references Save took.
		void Restore()
		{
			context->IASetPrimitiveTopology(topology);
			context->IASetInputLayout(layout);
			context->VSSetShader(vertex, nullptr, 0);
			context->HSSetShader(hull, nullptr, 0);
			context->DSSetShader(domain, nullptr, 0);
			context->GSSetShader(geometry, nullptr, 0);
			context->PSSetShader(pixel, nullptr, 0);
			context->PSSetShaderResources(0, 1, &resource);
			context->RSSetState(rasterizer);
			context->RSSetViewports(viewportCount, viewports);
			context->OMSetBlendState(blending, blendFactor, sampleMask);
			context->OMSetDepthStencilState(depth, stencilRef);
			context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, depthTarget);
			SafeRelease(layout);
			SafeRelease(vertex);
			SafeRelease(hull);
			SafeRelease(domain);
			SafeRelease(geometry);
			SafeRelease(pixel);
			SafeRelease(resource);
			SafeRelease(rasterizer);
			SafeRelease(blending);
			SafeRelease(depth);
			for (auto*& target : targets) {
				SafeRelease(target);
			}
			SafeRelease(depthTarget);
		}
	};

	void Draw(IDXGISwapChain* a_swapChain)
	{
		if (failed) {
			return;
		}
		if (!device && !Init(a_swapChain)) {
			failed = true;
			ReleaseAll();
			return;
		}

		// New pixels: rebuild the texture (never wait on the overlay thread).
		if (std::unique_lock l{ pending, std::try_to_lock }; l && pendingWidth && pendingVersion != textureVersion) {
			D3D11_TEXTURE2D_DESC td{};
			td.Width = pendingWidth;
			td.Height = pendingHeight;
			td.MipLevels = 1;
			td.ArraySize = 1;
			td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_IMMUTABLE;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			const D3D11_SUBRESOURCE_DATA init{ pendingPixels.data(), static_cast<UINT>(pendingWidth * 4), 0 };
			ID3D11Texture2D*             t = nullptr;
			ID3D11ShaderResourceView*    v = nullptr;
			if (SUCCEEDED(device->CreateTexture2D(&td, &init, &t)) && SUCCEEDED(device->CreateShaderResourceView(t, nullptr, &v))) {
				SafeRelease(view);
				SafeRelease(texture);
				texture = t;
				view = v;
				width = pendingWidth;
				height = pendingHeight;
				spot = pendingSpot;
			} else {
				SafeRelease(t);
			}
			textureVersion = pendingVersion;
		}
		if (!view) {
			return;
		}

		ID3D11Texture2D* back = nullptr;
		if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		DXGI_FORMAT format;
		bool        linear;
		if (!ViewFormat(bd.Format, format, linear)) {
			if (!loggedFormat) {
				loggedFormat = true;
				logger::warn("Back buffer format {} not handled, status not drawn into frames", static_cast<int>(bd.Format));
			}
			back->Release();
			return;
		}
		// The RTV is made per frame and released before Present, a held back buffer reference would make ResizeBuffers fail.
		D3D11_RENDER_TARGET_VIEW_DESC rd{};
		rd.Format = format;
		rd.ViewDimension = bd.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
		ID3D11RenderTargetView* rtv = nullptr;
		const auto at = Frame::Place({ static_cast<LONG>(bd.Width), static_cast<LONG>(bd.Height) }, { width, height }, spot);
		if (at.x >= 0 && at.y >= 0 && at.x + width <= static_cast<int>(bd.Width) && at.y + height <= static_cast<int>(bd.Height) &&
			SUCCEEDED(device->CreateRenderTargetView(back, &rd, &rtv))) {
			if (at.x != loggedAt.x || at.y != loggedAt.y) {
				loggedAt = at;
				logger::info("[{:6.1f} s] Status drawn into frames at {},{} ({}x{} in {}x{})", Status::Uptime(), at.x, at.y, width,
					height, bd.Width, bd.Height);
			}
			SavedState game;
			game.Save();
			const D3D11_VIEWPORT vp{ static_cast<float>(at.x), static_cast<float>(at.y), static_cast<float>(width),
				static_cast<float>(height), 0.0f, 1.0f };
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			context->IASetInputLayout(nullptr);
			context->VSSetShader(vs, nullptr, 0);
			context->HSSetShader(nullptr, nullptr, 0);
			context->DSSetShader(nullptr, nullptr, 0);
			context->GSSetShader(nullptr, nullptr, 0);
			context->PSSetShader(linear ? psLinear : psSrgb, nullptr, 0);
			context->PSSetShaderResources(0, 1, &view);
			context->RSSetState(raster);
			context->RSSetViewports(1, &vp);
			context->OMSetBlendState(blend, nullptr, 0xFFFFFFFF);
			context->OMSetDepthStencilState(nullptr, 0);
			context->OMSetRenderTargets(1, &rtv, nullptr);
			context->Draw(4, 0);
			game.Restore();
			rtv->Release();
		}
		back->Release();
	}

	// An exclusive fullscreen or D3D12 output can hide the child window, this tells which one a report is about.
	// Runs on the game's own thread, since a report's log stopped at these calls when the overlay thread made them.
	void LogSwapChain(IDXGISwapChain* a_swapChain)
	{
		logger::info("[{:6.1f} s] First Present on thread {} (main thread {})", Status::Uptime(), GetCurrentThreadId(), Status::mainThread.load());
		DXGI_SWAP_CHAIN_DESC desc{};
		BOOL                 fullscreen = FALSE;
		a_swapChain->GetDesc(&desc);
		a_swapChain->GetFullscreenState(&fullscreen, nullptr);
		logger::info("[{:6.1f} s] Swapchain: swap effect {}, fullscreen {}, d3d12.dll {}", Status::Uptime(),
			static_cast<int>(desc.SwapEffect), fullscreen != FALSE, GetModuleHandleW(L"d3d12.dll") != nullptr ? "loaded" : "not loaded");
	}

	HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* a_swapChain, UINT a_sync, UINT a_flags)
	{
		if (!(a_flags & DXGI_PRESENT_TEST)) {
			if (!stopped) {
				if (!loggedSwapChain) {
					loggedSwapChain = true;
					LogSwapChain(a_swapChain);
				}
				lastPresentMs = static_cast<std::int64_t>(Status::Uptime() * 1000.0);
				Draw(a_swapChain);
			} else if (!released) {
				// The slot is never put back: a mod may have detoured this function's code, Community Shaders does, and restoring the slot would cut it out.
				released = true;
				ReleaseAll();
				logger::info("[{:6.1f} s] Present hook stays as a pass-through", Status::Uptime());
			}
		}
		return originalPresent(a_swapChain, a_sync, a_flags);
	}
}

namespace Frame
{
	bool TryHook()
	{
		const auto data = RE::BSGraphics::Renderer::GetRendererDataSingleton();
		const auto swapChain = data ? reinterpret_cast<IDXGISwapChain*>(data->renderWindows[0].swapChain) : nullptr;
		if (!swapChain) {
			return false;
		}
		auto** vtable = *reinterpret_cast<void***>(swapChain);
		auto*  slot = &vtable[8];  // IDXGISwapChain::Present
		DWORD  old = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
			logger::error("VirtualProtect on the swapchain vtable failed ({})", GetLastError());
			return true;  // do not retry
		}
		originalPresent = reinterpret_cast<Present_t>(*slot);
		InterlockedExchangePointer(slot, reinterpret_cast<void*>(&HookedPresent));
		VirtualProtect(slot, sizeof(void*), old, &old);
		logger::info("[{:6.1f} s] Present hooked on the game swapchain", Status::Uptime());
		return true;
	}

	double LastPresent() { return lastPresentMs / 1000.0; }

	void Publish(std::vector<std::uint32_t> a_bgra, int a_width, int a_height, const Spot& a_spot)
	{
		std::scoped_lock l{ pending };
		pendingPixels = std::move(a_bgra);
		pendingWidth = a_width;
		pendingHeight = a_height;
		pendingSpot = a_spot;
		++pendingVersion;
	}

	void Stop() { stopped = true; }
}
