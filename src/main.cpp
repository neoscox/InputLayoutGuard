// Input Layout Guard - Copyright (C) 2026 neoscox
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PCH.h"

#include <d3d11.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <mutex>
#include <unordered_set>

// Input Layout Guard
//
// Skyrim builds D3D11 input layouts lazily and caches them by vertex-format key (the mesh's vertex description masked by
// the vertex shader's input mask). The key does not identify the shader, and a FAILED creation is cached as NULL.
// One mesh whose shader needs an attribute its vertex data lacks (typically NORMAL/BINORMAL on a normal-less facegen or
// head-part shape that is missing the Model_Space_Normals flag) therefore makes CreateInputLayout fail, and the NULL it
// leaves behind is then bound for every other mesh with the same vertex format for the rest of the session: vanilla NPC
// faces render black indoors and sky-coloured outdoors, at random per launch depending on draw order.
//
// This plugin catches that failure at its source. When the engine's CreateInputLayout call fails, it reads the vertex
// shader's input signature, adds the attributes the mesh does not provide as elements on an unused input slot (an unbound
// slot reads as zero in D3D11), and creates the layout again. The cache then holds a valid layout: other meshes keep
// rendering normally, and only the broken mesh itself renders with zeroed missing attributes. Every failure is logged
// with the mesh, its owner and plugin, so the bad asset can be fixed.

namespace
{
	constexpr UINT kDummySlot = 15;  // the engine binds vertex streams 0 and 1 only

	struct Config
	{
		bool repair = true;
		bool consoleNotify = true;
	} g_cfg;

	std::string Hex(std::uint64_t v) { return std::format("0x{:X}", v); }

	std::uint32_t Crc32(const std::uint8_t* d, std::size_t n)
	{
		static std::uint32_t table[256] = {};
		if (!table[1]) {
			for (std::uint32_t i = 0; i < 256; ++i) {
				std::uint32_t r = i;
				for (int k = 0; k < 8; ++k) {
					r = (r & 1) ? (r >> 1) ^ 0xEDB88320u : r >> 1;
				}
				table[i] = r;
			}
		}
		std::uint32_t c = 0xFFFFFFFFu;
		for (std::size_t i = 0; i < n; ++i) {
			c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
		}
		return ~c;
	}

	std::string ModuleOf(const void* a_addr)
	{
		HMODULE h = nullptr;
		if (!a_addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							reinterpret_cast<LPCWSTR>(a_addr), &h) ||
			!h) {
			return "?";
		}
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(h, path, MAX_PATH);
		std::wstring_view w(path);
		const auto        slash = w.find_last_of(L"\\/");
		std::string       out;
		for (wchar_t c : (slash == std::wstring_view::npos ? w : w.substr(slash + 1))) {
			out.push_back(static_cast<char>(c < 128 ? c : '?'));
		}
		return std::format("{}+0x{:X}", out, reinterpret_cast<std::uintptr_t>(a_addr) - reinterpret_cast<std::uintptr_t>(h));
	}

	bool IsEngineAddress(const void* a_addr)
	{
		const auto  base = REL::Module::get().base();
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
		const auto  a = reinterpret_cast<std::uintptr_t>(a_addr);
		return a >= base && a < base + nt->OptionalHeader.SizeOfImage;
	}

	// ---- What is being drawn: the last shader pass set up on this thread ------------------------------------------------
	thread_local RE::BSGeometry* t_geometry = nullptr;
	thread_local const char*     t_shader = nullptr;

	using SetupGeometry_t = void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t);

	template <int N>
	struct SetupGeometryHook
	{
		static inline SetupGeometry_t orig = nullptr;
		static inline const char*     name = nullptr;
		static void                   Thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
		{
			t_geometry = a_pass ? a_pass->geometry : nullptr;
			t_shader = name;
			orig(a_shader, a_pass, a_flags);
		}
	};

	template <int N>
	void HookSetupGeometry(std::uintptr_t a_vtbl, const char* a_name)
	{
		auto** slot = reinterpret_cast<void**>(a_vtbl) + 6;  // BSShader::SetupGeometry
		DWORD  old = 0;
		VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
		SetupGeometryHook<N>::orig = reinterpret_cast<SetupGeometry_t>(*slot);
		SetupGeometryHook<N>::name = a_name;
		*slot = reinterpret_cast<void*>(&SetupGeometryHook<N>::Thunk);
		VirtualProtect(slot, sizeof(void*), old, &old);
	}

	// Describes a geometry and its owning reference into a plain buffer. SEH-guarded: the pointer comes from a render pass
	// and is only read here, never kept.
	void DescribeGeometryRaw(RE::BSGeometry* g, char* out, std::size_t n)
	{
		__try {
			if (!g) {
				snprintf(out, n, "(no shader pass recorded on this thread)");
				return;
			}
			const char* shape = g->name.c_str();
			const char* tex = nullptr;
			auto*       sp = g->GetGeometryRuntimeData().shaderProperty.get();
			auto*       lsp = sp ? netimmerse_cast<RE::BSLightingShaderProperty*>(sp) : nullptr;
			if (lsp && lsp->material) {
				auto* m = static_cast<RE::BSLightingShaderMaterialBase*>(lsp->material);
				if (m->textureSet) {
					tex = m->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
				}
			}
			RE::TESObjectREFR* owner = nullptr;
			const char*        root = nullptr;
			for (RE::NiAVObject* o = g; o; o = o->parent) {
				root = o->name.c_str();
				if (!owner) {
					owner = o->GetUserData();
				}
			}
			int w = snprintf(out, n, "mesh shape '%s' (diffuse '%s', shader flags 0x%llX, root node '%s')", shape ? shape : "?", tex ? tex : "",
				sp ? static_cast<unsigned long long>(sp->flags.underlying()) : 0ull, root ? root : "?");
			if (owner && w > 0 && static_cast<std::size_t>(w) < n) {
				auto* base = owner->GetBaseObject();
				auto* rf = owner->GetFile(0);
				auto* bf = base ? base->GetFile(0) : nullptr;
				const char* nm = owner->GetDisplayFullName();
				snprintf(out + w, n - w, " on '%s' ref %08X [%s] base %08X [%s]", nm ? nm : "", owner->GetFormID(), rf ? rf->GetFilename().data() : "?",
					base ? base->GetFormID() : 0u, bf ? bf->GetFilename().data() : "?");
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			snprintf(out, n, "(mesh could not be read)");
		}
	}

	// ---- Vertex shader input signature (DXBC ISGN chunk) ----------------------------------------------------------------
	struct SigElement
	{
		const char*   name;
		std::uint32_t index;
		std::uint32_t componentType;  // 1 uint, 2 sint, 3 float
	};

	std::vector<SigElement> ReadInputSignature(const void* a_bc, SIZE_T a_len)
	{
		std::vector<SigElement> out;
		const auto*             b = static_cast<const std::uint8_t*>(a_bc);
		if (!b || a_len < 32 || std::memcmp(b, "DXBC", 4) != 0) {
			return out;
		}
		auto rd = [&](std::size_t o) -> std::uint32_t {
			std::uint32_t v = 0;
			if (o + 4 <= a_len) {
				std::memcpy(&v, b + o, 4);
			}
			return v;
		};
		const auto chunks = rd(28);
		for (std::uint32_t c = 0; c < chunks && 32 + 4 * c + 4 <= a_len; ++c) {
			const auto off = rd(32 + 4 * c);
			if (off + 8 > a_len || std::memcmp(b + off, "ISGN", 4) != 0) {
				continue;
			}
			const auto data = off + 8;
			const auto count = rd(data);
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto e = data + 8 + 24 * i;
				if (e + 24 > a_len) {
					break;
				}
				const auto nameOff = rd(e);
				const auto sysValue = rd(e + 8);
				if (sysValue != 0 || data + nameOff >= a_len) {
					continue;  // SV_VertexID etc. come from the pipeline, not the layout
				}
				out.push_back({ reinterpret_cast<const char*>(b + data + nameOff), rd(e + 4), rd(e + 12) });
			}
		}
		return out;
	}

	DXGI_FORMAT DummyFormat(std::uint32_t a_componentType)
	{
		switch (a_componentType) {
		case 1:
			return DXGI_FORMAT_R32G32B32A32_UINT;
		case 2:
			return DXGI_FORMAT_R32G32B32A32_SINT;
		default:
			return DXGI_FORMAT_R32G32B32A32_FLOAT;
		}
	}

	std::string Describe(const D3D11_INPUT_ELEMENT_DESC* e, UINT n)
	{
		std::string s;
		for (UINT i = 0; i < n; ++i) {
			s += std::format("{}{}@{}+{} ", e[i].SemanticName ? e[i].SemanticName : "?", e[i].SemanticIndex, e[i].InputSlot, e[i].AlignedByteOffset);
		}
		return s;
	}

	// ---- CreateInputLayout guard -----------------------------------------------------------------------------------------
	using CreateInputLayout_t = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
	CreateInputLayout_t g_orig = nullptr;

	std::mutex                      g_seenLock;
	std::unordered_set<std::string> g_seen;
	std::atomic<std::uint32_t>      g_failures{ 0 }, g_repaired{ 0 };
	std::atomic<std::uint64_t>      g_swaps{ 0 };

	// ---- Per-shader variants of a repaired layout -------------------------------------------------------------------------
	// The engine's layout cache is keyed by vertex format, not by shader, so the layout we repaired for the broken mesh's
	// shader is later bound for every other shader drawing that vertex format (e.g. vanilla faces). A layout built against
	// one shader's input signature does not fit another's register assignment, so those draws break the same way a NULL
	// layout does. Whenever the engine binds a repaired layout we therefore substitute a layout built for the vertex shader
	// actually in use, created once per shader from the same vertex elements.
	struct Repaired
	{
		std::vector<std::string>                                  names;     // owns the semantic strings
		std::vector<D3D11_INPUT_ELEMENT_DESC>                     elements;  // the engine's original elements
		std::unordered_map<const void*, ID3D11InputLayout*>       perShader;
	};
	constexpr std::size_t                             kMaxRepaired = 32;
	std::array<std::atomic<ID3D11InputLayout*>, kMaxRepaired> g_repairedIL{};
	std::atomic<std::size_t>                          g_repairedCount{ 0 };
	std::mutex                                        g_repLock;
	std::unordered_map<ID3D11InputLayout*, Repaired>  g_rep;

	void RememberRepaired(ID3D11InputLayout* a_il, const D3D11_INPUT_ELEMENT_DESC* e, UINT n)
	{
		std::scoped_lock l(g_repLock);
		const auto       idx = g_repairedCount.load();
		if (idx >= kMaxRepaired) {
			return;
		}
		auto& r = g_rep[a_il];
		r.names.reserve(n);
		for (UINT i = 0; i < n; ++i) {
			r.names.emplace_back(e[i].SemanticName ? e[i].SemanticName : "");
		}
		r.elements.assign(e, e + n);
		for (UINT i = 0; i < n; ++i) {
			r.elements[i].SemanticName = r.names[i].c_str();
		}
		g_repairedIL[idx].store(a_il);
		g_repairedCount.store(idx + 1);
	}

	void Notify(std::string a_msg)
	{
		if (!g_cfg.consoleNotify) {
			return;
		}
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask([msg = std::move(a_msg)] {
				if (auto* console = RE::ConsoleLog::GetSingleton()) {
					console->Print(msg.c_str());
				}
			});
		}
	}

	HRESULT STDMETHODCALLTYPE HkCreateInputLayout(ID3D11Device* d, const D3D11_INPUT_ELEMENT_DESC* e, UINT n, const void* bc, SIZE_T bcl,
		ID3D11InputLayout** out)
	{
		const auto hr = g_orig(d, e, n, bc, bcl, out);
		if (SUCCEEDED(hr) || !e || !bc) {
			return hr;
		}
		void* const ret = _ReturnAddress();
		const bool  fromEngine = IsEngineAddress(ret);
		++g_failures;

		// what the shader wants that the mesh does not have
		std::vector<SigElement> missing;
		for (auto& s : ReadInputSignature(bc, bcl)) {
			bool found = false;
			for (UINT i = 0; i < n && !found; ++i) {
				found = e[i].SemanticName && _stricmp(e[i].SemanticName, s.name) == 0 && e[i].SemanticIndex == s.index;
			}
			if (!found) {
				missing.push_back(s);
			}
		}
		std::string missingStr;
		for (auto& m : missing) {
			missingStr += std::format("{}{} ", m.name, m.index);
		}

		char mesh[768];
		DescribeGeometryRaw(t_geometry, mesh, sizeof(mesh));
		std::uint64_t vdesc = 0;
		if (auto* ss = RE::BSGraphics::RendererShadowState::GetSingleton()) {
			vdesc = ss->GetRuntimeData().vertexDesc;
		}
		const auto vsCrc = Crc32(static_cast<const std::uint8_t*>(bc), bcl);

		HRESULT fixed = hr;
		if (g_cfg.repair && fromEngine && !missing.empty()) {
			std::vector<D3D11_INPUT_ELEMENT_DESC> elems(e, e + n);
			for (auto& m : missing) {
				elems.push_back({ m.name, m.index, DummyFormat(m.componentType), kDummySlot, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 });
			}
			fixed = g_orig(d, elems.data(), static_cast<UINT>(elems.size()), bc, bcl, out);
			if (SUCCEEDED(fixed) && out && *out) {
				++g_repaired;
				RememberRepaired(*out, e, n);
			}
		}

		const auto key = std::format("{:08X}|{}", vsCrc, Describe(e, n));
		bool       first = false;
		{
			std::scoped_lock l(g_seenLock);
			first = g_seen.insert(key).second;
		}
		if (first) {
			logger::warn("CreateInputLayout FAILED (hr=0x{:08X}) called from {}", static_cast<std::uint32_t>(hr), ModuleOf(ret));
			logger::warn("    while drawing: {} [{} shader]", mesh, t_shader ? t_shader : "?");
			logger::warn("    vertex data provides: {}| vertexDesc {}", Describe(e, n), Hex(vdesc));
			logger::warn("    vertex shader (crc {:08X}, {} bytes) also needs: {}", vsCrc, bcl, missingStr.empty() ? "(nothing missing - other mismatch)" : missingStr);
			if (SUCCEEDED(fixed)) {
				logger::warn("    REPAIRED: layout rebuilt with the missing inputs reading zero (slot {}); other shaders drawing this vertex format get their own layout.",
					kDummySlot);
			} else if (!g_cfg.repair) {
				logger::warn("    not repaired (bRepair=0): the engine caches this failure and every mesh sharing this vertex format may render wrong this session.");
			} else if (!fromEngine) {
				logger::warn("    not repaired: the call did not come from the game engine.");
			} else {
				logger::warn("    repair FAILED (hr=0x{:08X}): the engine caches this failure.", static_cast<std::uint32_t>(fixed));
			}
			Notify(std::format("InputLayoutGuard: broken mesh {} - {}", SUCCEEDED(fixed) ? "repaired" : "NOT repaired", mesh));
		}
		return fixed;
	}

	// ---- IASetInputLayout: swap a repaired layout for the variant matching the current vertex shader ----------------------
	using IASetInputLayout_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11InputLayout*);
	ID3D11Device*      g_device = nullptr;
	IASetInputLayout_t g_ilOrig0 = nullptr;
	IASetInputLayout_t g_ilOrig1 = nullptr;

	ID3D11InputLayout* VariantFor(ID3D11InputLayout* a_il)
	{
		auto* ss = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto* vs = ss ? ss->GetRuntimeData().currentVertexShader : nullptr;
		if (!vs || vs->byteCodeSize < 32 || std::memcmp(vs->rawBytecode, "DXBC", 4) != 0) {
			return a_il;
		}
		std::scoped_lock l(g_repLock);
		auto             it = g_rep.find(a_il);
		if (it == g_rep.end()) {
			return a_il;
		}
		auto& r = it->second;
		if (auto v = r.perShader.find(vs); v != r.perShader.end()) {
			return v->second ? v->second : a_il;
		}
		// first bind with this shader: build its own layout from the same vertex elements
		std::vector<D3D11_INPUT_ELEMENT_DESC> elems = r.elements;
		std::string                           added;
		for (auto& s : ReadInputSignature(vs->rawBytecode, vs->byteCodeSize)) {
			bool found = false;
			for (auto& el : r.elements) {
				found = found || (_stricmp(el.SemanticName, s.name) == 0 && el.SemanticIndex == s.index);
			}
			if (!found) {
				elems.push_back({ s.name, s.index, DummyFormat(s.componentType), kDummySlot, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 });
				added += std::format("{}{} ", s.name, s.index);
			}
		}
		ID3D11InputLayout* v = nullptr;
		const auto         hr = g_orig(g_device, elems.data(), static_cast<UINT>(elems.size()), vs->rawBytecode, vs->byteCodeSize, &v);
		r.perShader[vs] = SUCCEEDED(hr) ? v : nullptr;
		logger::info("    variant for vertex shader crc {:08X}: {} (hr=0x{:08X}){}", Crc32(vs->rawBytecode, vs->byteCodeSize),
			SUCCEEDED(hr) ? "created" : "FAILED, keeping repaired layout", static_cast<std::uint32_t>(hr),
			added.empty() ? "" : std::format(", zero-filled: {}", added));
		return SUCCEEDED(hr) ? v : a_il;
	}

	inline ID3D11InputLayout* MaybeSwap(ID3D11InputLayout* a_il)
	{
		const auto count = g_repairedCount.load(std::memory_order_acquire);
		if (!a_il || count == 0) {
			return a_il;
		}
		for (std::size_t i = 0; i < count; ++i) {
			if (g_repairedIL[i].load(std::memory_order_relaxed) == a_il) {
				auto* v = VariantFor(a_il);
				if (v != a_il) {
					++g_swaps;
				}
				return v;
			}
		}
		return a_il;
	}

	void STDMETHODCALLTYPE HkIASetIL0(ID3D11DeviceContext* c, ID3D11InputLayout* il) { g_ilOrig0(c, MaybeSwap(il)); }
	void STDMETHODCALLTYPE HkIASetIL1(ID3D11DeviceContext* c, ID3D11InputLayout* il) { g_ilOrig1(c, MaybeSwap(il)); }

	void HookIASetInputLayout(ID3D11Device* a_dev)
	{
		// the d3d11 function itself, not a context vtable slot: the immediate context's table is rewritten by d3d11
		std::vector<void*>   targets;
		ID3D11DeviceContext* ctx = nullptr;
		a_dev->GetImmediateContext(&ctx);
		if (ctx) {
			targets.push_back((*reinterpret_cast<void***>(ctx))[17]);
			ctx->Release();
		}
		ID3D11DeviceContext* def = nullptr;
		if (SUCCEEDED(a_dev->CreateDeferredContext(0, &def)) && def) {
			targets.push_back((*reinterpret_cast<void***>(def))[17]);
			def->Release();
		}
		int slot = 0;
		for (std::size_t i = 0; i < targets.size() && slot < 2; ++i) {
			if (i > 0 && targets[i] == targets[0]) {
				continue;
			}
			void*      tramp = nullptr;
			const auto r1 = MH_CreateHook(targets[i], slot == 0 ? reinterpret_cast<void*>(&HkIASetIL0) : reinterpret_cast<void*>(&HkIASetIL1), &tramp);
			if (r1 == MH_OK) {
				(slot == 0 ? g_ilOrig0 : g_ilOrig1) = reinterpret_cast<IASetInputLayout_t>(tramp);
			}
			const auto r2 = r1 == MH_OK ? MH_EnableHook(targets[i]) : r1;
			logger::info("IASetInputLayout hook on {}: {}", ModuleOf(targets[i]), r2 == MH_OK ? "OK" : std::format("failed ({})", static_cast<int>(r2)));
			if (r2 == MH_OK) {
				++slot;
			}
		}
	}

	// ---- Install -----------------------------------------------------------------------------------------------------------
	bool g_installed = false;

	void TryInstall(const char* a_when)
	{
		if (g_installed) {
			return;
		}
		auto* dev = reinterpret_cast<ID3D11Device*>(RE::BSGraphics::Renderer::GetDevice());
		if (!dev) {
			logger::info("{}: renderer device not created yet", a_when);
			return;
		}
		g_installed = true;
		if (MH_Initialize() != MH_OK) {
			logger::error("MinHook init failed; guard disabled");
			return;
		}
		void* target = (*reinterpret_cast<void***>(dev))[11];  // ID3D11Device::CreateInputLayout
		void* tramp = nullptr;
		const auto r1 = MH_CreateHook(target, reinterpret_cast<void*>(&HkCreateInputLayout), &tramp);
		if (r1 == MH_OK) {
			g_orig = reinterpret_cast<CreateInputLayout_t>(tramp);  // set before enabling: the hook may fire at once
		}
		const auto r2 = r1 == MH_OK ? MH_EnableHook(target) : r1;
		if (r2 != MH_OK) {
			logger::error("CreateInputLayout hook failed ({}) on {}", static_cast<int>(r2), ModuleOf(target));
			return;
		}
		g_device = dev;
		HookIASetInputLayout(dev);

		// mesh attribution: remember the geometry each shader sets up before it draws
		HookSetupGeometry<0>(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSLightingShader[0] }.address(), "Lighting");
		HookSetupGeometry<1>(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSEffectShader[0] }.address(), "Effect");
		HookSetupGeometry<2>(REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSUtilityShader[0] }.address(), "Utility");

		logger::info("{}: guard installed on CreateInputLayout ({}); repair={} consoleNotify={}", a_when, ModuleOf(target), g_cfg.repair, g_cfg.consoleNotify);
	}

	void LoadConfig()
	{
		const auto path = std::filesystem::current_path() / "Data/SKSE/Plugins/InputLayoutGuard.ini";
		const auto p = path.wstring();
		g_cfg.repair = GetPrivateProfileIntW(L"General", L"bRepair", 1, p.c_str()) != 0;
		g_cfg.consoleNotify = GetPrivateProfileIntW(L"General", L"bConsoleNotify", 1, p.c_str()) != 0;
	}

	void InitializeLog()
	{
		auto path = logger::log_directory();
		if (!path) {
			stl::report_and_fail("no log directory"sv);
		}
		*path /= "InputLayoutGuard.log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v"s);
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitializeLog();
	SKSE::Init(a_skse, false);
	LoadConfig();
	logger::info("Input Layout Guard {} loaded ({})", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string(), REL::Module::get().version().string());
	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* m) {
		switch (m->type) {
		case SKSE::MessagingInterface::kPostLoad:
			TryInstall("PostLoad");
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			TryInstall("PostPostLoad");
			break;
		case SKSE::MessagingInterface::kInputLoaded:
			TryInstall("InputLoaded");
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			TryInstall("DataLoaded");
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			logger::info("game loaded: {} layout failure(s) seen, {} repaired, {} per-shader swaps so far", g_failures.load(), g_repaired.load(), g_swaps.load());
			break;
		default:
			break;
		}
	});
	return true;
}
