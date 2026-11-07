/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#include <StdInc.h>

#include "CustomPlateRegistry.h"

#include <jitasm.h>
#include <Hooking.h>
#include <ScriptEngine.h>
#include <scrEngine.h>

#include <array>
#include <random>
#include <map>
#include <string>

#include <MinHook.h>
#include <udis86.h>
#include <grcTexture.h>
#include <CoreConsole.h>

// Forward declaration from TextureReplacementNatives.cpp
extern rage::grcTexture* LookupTexture(const std::string& txd, const std::string& txn);

static std::string g_customPlatePattern;
static std::map<int, std::string> g_customPlatePatterns;

static int plateBackOffset = 0;
static int g_vehiclePlateIndexOffset = 0;
static int g_vehiclePlateIndexSize = 4;
static int g_diffuseTexOffset = 0;
static int g_normalTexOffset = 0;
static int g_vanillaPlateCount = 6;

static std::unordered_map<void*, int> g_customPlateIndexByVehicle;

static int GetVehiclePlateIndexFromMemory(void* vehicle)
{
	if (!vehicle)
	{
		return 0;
	}

	if (g_vehiclePlateIndexOffset > 0)
	{
		if (g_vehiclePlateIndexSize == 1)
		{
			return *reinterpret_cast<uint8_t*>((char*)vehicle + g_vehiclePlateIndexOffset);
		}
		else if (g_vehiclePlateIndexSize == 2)
		{
			return *reinterpret_cast<uint16_t*>((char*)vehicle + g_vehiclePlateIndexOffset);
		}
		else
		{
			return *reinterpret_cast<int*>((char*)vehicle + g_vehiclePlateIndexOffset);
		}
	}

	auto it = g_customPlateIndexByVehicle.find(vehicle);
	if (it != g_customPlateIndexByVehicle.end())
	{
		return it->second;
	}

	return 0;
}

static void SetVehiclePlateIndexInMemory(void* vehicle, int plateIndex)
{
	if (!vehicle)
	{
		return;
	}

	g_customPlateIndexByVehicle[vehicle] = plateIndex;

	if (g_vehiclePlateIndexOffset > 0)
	{
		if (g_vehiclePlateIndexSize == 1)
		{
			*reinterpret_cast<uint8_t*>((char*)vehicle + g_vehiclePlateIndexOffset) = static_cast<uint8_t>(plateIndex);
		}
		else if (g_vehiclePlateIndexSize == 2)
		{
			*reinterpret_cast<uint16_t*>((char*)vehicle + g_vehiclePlateIndexOffset) = static_cast<uint16_t>(plateIndex);
		}
		else
		{
			*reinterpret_cast<int*>((char*)vehicle + g_vehiclePlateIndexOffset) = plateIndex;
		}
	}
}

static void (*g_origCCustomShaderEffectVehicle__setDefaultPlateBack)(void* self, void* vehicle) = nullptr;
static void (*g_origCCustomShaderEffectVehicle__setDefaultNumberPlate)(void* self, uint32_t seed) = nullptr;

static fx::TNativeHandler g_origSetVehicleNumberPlateTextIndex;
static fx::TNativeHandler g_origGetVehicleNumberPlateTextIndex;
static fx::TNativeHandler g_origGetNumberOfVehicleNumberPlates;

void SetCustomPlatePatternInternal(int index, const std::string& pattern)
{
	g_customPlatePatterns[index] = pattern;
}

void RemoveCustomPlatePatternInternal(int index)
{
	g_customPlatePatterns.erase(index);
}

static hook::thiscall_stub<void(void*, char[8])> CCustomShaderEffectVehicle__setNumberPlate([]
{
	return hook::get_pattern("40 32 FF 33 DB 33 C0 48 8B F2 4C", -0x16);
});

// Try to discover texture offsets in CCustomShaderEffectVehicle by examining plate 0 textures
static void TryValidateTextureOffsetsFromSelf(char* self)
{
	if (g_diffuseTexOffset != 0 && g_normalTexOffset != 0)
	{
		return;
	}

	auto pPlate0Diffuse = LookupTexture("vehshare", "plate01");
	auto pPlate0Normal = LookupTexture("vehshare", "plate01_n");
	if (!pPlate0Normal)
	{
		pPlate0Normal = LookupTexture("vehshare", "carplate_n");
	}

	if (!pPlate0Diffuse && !pPlate0Normal)
	{
		return;
	}

	uintptr_t* ptrs = reinterpret_cast<uintptr_t*>(self);
	for (size_t i = 0; i < 64; ++i)
	{
		int offset = static_cast<int>(i * sizeof(uintptr_t));
		if (offset == plateBackOffset)
		{
			continue;
		}

		if (pPlate0Diffuse && ptrs[i] == reinterpret_cast<uintptr_t>(pPlate0Diffuse))
		{
			g_diffuseTexOffset = offset;
		}
		else if (pPlate0Normal && ptrs[i] == reinterpret_cast<uintptr_t>(pPlate0Normal))
		{
			g_normalTexOffset = offset;
		}
	}
}

// Wrapped setDefaultPlateBack that intercepts custom plate indices (>= 13)
static void CCustomShaderEffectVehicle__setDefaultPlateBack_Custom(char* self, void* vehicle)
{
	if (!vehicle || !g_origCCustomShaderEffectVehicle__setDefaultPlateBack)
	{
		return;
	}

	int plateIndex = GetVehiclePlateIndexFromMemory(vehicle);

	// Vanilla plate index (< 13 and not registered as custom)
	if (plateIndex < CustomPlateRegistry::kFirstCustomPlateIndex &&
		!CustomPlateRegistry::GetInstance()->IsValidCustomIndex(plateIndex))
	{
		g_origCCustomShaderEffectVehicle__setDefaultPlateBack(self, vehicle);

		if (g_diffuseTexOffset == 0 || g_normalTexOffset == 0)
		{
			TryValidateTextureOffsetsFromSelf(self);
		}
		return;
	}

	// Custom plate index (>= 13):
	// 1. Temporarily set vehicle's plate index field to 0 so vanilla setDefaultPlateBack
	//    safely initializes all shader effect matrices, buffers, and state for plate 0.
	SetVehiclePlateIndexInMemory(vehicle, 0);

	g_origCCustomShaderEffectVehicle__setDefaultPlateBack(self, vehicle);

	// 2. Restore vehicle's actual custom plate index
	SetVehiclePlateIndexInMemory(vehicle, plateIndex);

	// 3. Set custom plate index on the shader effect
	if (plateBackOffset > 0)
	{
		*reinterpret_cast<int*>(self + plateBackOffset) = plateIndex;
	}

	// 4. Overwrite diffuse and normal texture pointers with custom textures
	const auto* customPlate = CustomPlateRegistry::GetInstance()->GetPlate(plateIndex);
	if (customPlate)
	{
		if (g_diffuseTexOffset > 0 && !customPlate->diffuseTex.empty())
		{
			auto customDiffuse = LookupTexture(customPlate->diffuseDict, customPlate->diffuseTex);
			if (customDiffuse)
			{
				*reinterpret_cast<rage::grcTexture**>(self + g_diffuseTexOffset) = customDiffuse;
			}
		}

		if (g_normalTexOffset > 0 && !customPlate->normalTex.empty())
		{
			std::string normDict = customPlate->normalDict.empty() ? customPlate->diffuseDict : customPlate->normalDict;
			auto customNormal = LookupTexture(normDict, customPlate->normalTex);
			if (customNormal)
			{
				*reinterpret_cast<rage::grcTexture**>(self + g_normalTexOffset) = customNormal;
			}
		}
	}
}

static void CCustomShaderEffectVehicle__setDefaultNumberPlate(char* self, uint32_t seed, void* vehicle)
{
	CCustomShaderEffectVehicle__setDefaultPlateBack_Custom(self, vehicle);

	auto cpp = g_customPlatePattern;
	auto plateType = *(int*)(self + plateBackOffset);

	if (auto it = g_customPlatePatterns.find(plateType); it != g_customPlatePatterns.end())
	{
		cpp = it->second;
	}

	if (cpp.empty() || cpp == "11AAA111")
	{
		return g_origCCustomShaderEffectVehicle__setDefaultNumberPlate(self, seed);
	}

	// 8 for Five
	static constexpr const size_t kPlateLength = 8;

	std::array<char, kPlateLength + 1> plateString;
	std::default_random_engine rng{ seed };
	std::uniform_int_distribution<short> number{ '0', '9' };
	std::uniform_int_distribution<short> letter{ 'A', 'Z' };
	std::uniform_int_distribution any{ 1, 2 };

	// fill the plate string with '        \0'
	for (size_t idx = 0; idx < kPlateLength; idx++)
	{
		plateString[idx] = ' ';
	}

	plateString[kPlateLength] = '\0';

	// execute the pattern
	size_t outCur = 0;

	auto emit = [&plateString, &outCur](auto c)
	{
		if (outCur >= kPlateLength)
		{
			return;
		}

		plateString[outCur] = char(c);
		++outCur;
	};

	for (size_t inCur = 0; inCur < cpp.size(); ++inCur)
	{
		char c = cpp[inCur];
		char nextC = ((inCur + 1) < cpp.size()) ? cpp[inCur + 1] : '\0';

		if (c == '1')
		{
			emit(number(rng));
		}
		else if (c == 'A')
		{
			emit(letter(rng));
		}
		else if (c == '.')
		{
			auto which = any(rng);

			if (which == 1)
			{
				emit(letter(rng));
			}
			else
			{
				emit(number(rng));
			}
		}
		// escape code
		else if (c == '^' && (nextC == '1' || nextC == 'A' || nextC == '.'))
		{
			emit(nextC);
			++inCur;
		}
		else
		{
			emit(c);
		}
	}

	return CCustomShaderEffectVehicle__setNumberPlate(self, plateString.data());
}

static void AnalyzePlateBackFunction()
{
	if (!g_origCCustomShaderEffectVehicle__setDefaultPlateBack)
	{
		return;
	}

	ud_t ud;
	ud_init(&ud);
	ud_set_mode(&ud, 64);
	ud_set_input_buffer(&ud, reinterpret_cast<const uint8_t*>(g_origCCustomShaderEffectVehicle__setDefaultPlateBack), 256);

	bool foundStoreToPlateBack = false;
	ud_type vehReg = UD_R_RDX;

	while (ud_disassemble(&ud))
	{
		auto mnemonic = ud_insn_mnemonic(&ud);
		const auto* op0 = ud_insn_opr(&ud, 0);
		const auto* op1 = ud_insn_opr(&ud, 1);

		// Track register copies of rdx
		if (mnemonic == UD_Imov && op1 && op1->type == UD_OP_REG && op1->base == UD_R_RDX && op0 && op0->type == UD_OP_REG)
		{
			vehReg = op0->base;
		}

		// Look for reading plate index from vehicle (rdx or tracked alias)
		if (!foundStoreToPlateBack && op1 && op1->type == UD_OP_MEM && (op1->base == UD_R_RDX || op1->base == vehReg))
		{
			int disp = op1->lval.sdword;
			if (disp > 0x100 && disp < 0x2000)
			{
				g_vehiclePlateIndexOffset = disp;
				g_vehiclePlateIndexSize = op1->size / 8;
				if (g_vehiclePlateIndexSize <= 0)
				{
					g_vehiclePlateIndexSize = 4;
				}
			}
		}

		// Check for write to plateBackOffset (mov [r11 + plateBackOffset], edx)
		if (op0 && op0->type == UD_OP_MEM && op0->lval.sdword == plateBackOffset)
		{
			foundStoreToPlateBack = true;
			continue;
		}

		// After writing plateBackOffset, look for 64-bit pointer stores into self (r11 or rcx)
		if (foundStoreToPlateBack && mnemonic == UD_Imov && op0 && op0->type == UD_OP_MEM &&
			(op0->base == UD_R_R11 || op0->base == UD_R_RCX) && op0->size == 64)
		{
			int disp = op0->lval.sdword;
			if (disp != plateBackOffset && disp > 0 && disp < 0x500)
			{
				if (g_diffuseTexOffset == 0)
				{
					g_diffuseTexOffset = disp;
				}
				else if (g_normalTexOffset == 0 && disp != g_diffuseTexOffset)
				{
					g_normalTexOffset = disp;
					break;
				}
			}
		}
	}

	trace("CustomPlateRegistry: Disassembly scan results -> vehiclePlateIndexOffset=0x%X (size %d), diffuseTexOffset=0x%X, normalTexOffset=0x%X\n",
		g_vehiclePlateIndexOffset, g_vehiclePlateIndexSize, g_diffuseTexOffset, g_normalTexOffset);
}

static void AnalyzeSetPlateNative(void* nativeHandler)
{
	if (!nativeHandler || g_vehiclePlateIndexOffset > 0)
	{
		return;
	}

	ud_t ud;
	ud_init(&ud);
	ud_set_mode(&ud, 64);
	ud_set_input_buffer(&ud, reinterpret_cast<const uint8_t*>(nativeHandler), 256);

	while (ud_disassemble(&ud))
	{
		auto mnemonic = ud_insn_mnemonic(&ud);
		const auto* op0 = ud_insn_opr(&ud, 0);

		if (mnemonic == UD_Imov && op0 && op0->type == UD_OP_MEM && op0->base != UD_R_RSP && op0->base != UD_R_RBP)
		{
			int disp = op0->lval.sdword;
			if (disp > 0x100 && disp < 0x2000)
			{
				g_vehiclePlateIndexOffset = disp;
				g_vehiclePlateIndexSize = op0->size / 8;
				if (g_vehiclePlateIndexSize <= 0)
				{
					g_vehiclePlateIndexSize = 4;
				}
				trace("CustomPlateRegistry: Discovered vehiclePlateIndexOffset=0x%X (size %d) from SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX native\n", disp, g_vehiclePlateIndexSize);
				break;
			}
		}
	}
}

static fwEntity* GetVehicleEntityFromContext(fx::ScriptContext& context, const char* nativeName)
{
	if (context.GetArgumentCount() < 1)
	{
		return nullptr;
	}

	fwEntity* vehicle = rage::fwScriptGuid::GetBaseFromGuid(context.GetArgument<int>(0));
	if (!vehicle || !vehicle->IsOfType<CVehicle>())
	{
		return nullptr;
	}

	return vehicle;
}

static InitFunction initFunction([]()
{
	fx::ScriptEngine::RegisterNativeHandler("SET_DEFAULT_VEHICLE_NUMBER_PLATE_TEXT_PATTERN", [](fx::ScriptContext& cxt)
	{
		int index = cxt.GetArgument<int>(0);
		const char* newString = cxt.GetArgument<const char*>(1);
		std::string str = (newString) ? newString : "";

		if (index >= 0)
		{
			g_customPlatePatterns[index] = str;
		}
		else
		{
			g_customPlatePattern = str;
		}
	});
});

static HookFunction hookFunction([]()
{
	static struct : jitasm::Frontend
	{
		virtual void InternalMain() override
		{
			// CVehicle* rsi -> a3
			mov(r8, rsi);

			mov(rax, (uint64_t)CCustomShaderEffectVehicle__setDefaultNumberPlate);
			jmp(rax);
		}
	} setDefaultNumberPlateStub;

	plateBackOffset = *hook::get_pattern<int>("45 33 C9 41 89 93 ? ? ? ? 85 D2", 6);

	// no-op plate back selector so we can do this *before* setting default plate
	{
		auto location = hook::get_pattern("FF 50 ? 48 8B D6 48 8B CB E8 ? ? ? ? 48 8B D6", 9);
		hook::set_call(&g_origCCustomShaderEffectVehicle__setDefaultPlateBack, location);
		hook::nop(location, 5);
	}

	// Analyze function disassembly to locate vehiclePlateIndexOffset, diffuseTexOffset, normalTexOffset
	AnalyzePlateBackFunction();

	// default plate setter hook
	MH_Initialize();
	MH_CreateHook(hook::get_pattern("48 8D 4D F0 45 33 C0 E8 ? ? ? ? 8B 45 F4", -0x1A), setDefaultNumberPlateStub.GetCode(), (void**)&g_origCCustomShaderEffectVehicle__setDefaultNumberPlate);
	MH_EnableHook(MH_ALL_HOOKS);

	rage::scrEngine::OnScriptInit.Connect([]()
	{
		trace("CustomPlateRegistry: rage::scrEngine::OnScriptInit triggered - hooking plate natives\n");

		// Capture original native handlers
		g_origSetVehicleNumberPlateTextIndex = fx::ScriptEngine::GetNativeHandler(0x9088EB5A43FFB0A1);
		g_origGetVehicleNumberPlateTextIndex = fx::ScriptEngine::GetNativeHandler(0xF11BC2DD9A3E7195);
		g_origGetNumberOfVehicleNumberPlates = fx::ScriptEngine::GetNativeHandler(0x4C4D6B2644F458CB);

		if (g_vehiclePlateIndexOffset == 0)
		{
			auto rawSetPlate = rage::scrEngine::GetNativeHandler(0x9088EB5A43FFB0A1);
			AnalyzeSetPlateNative((void*)rawSetPlate);
		}

		// SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX (0x9088EB5A43FFB0A1)
		fx::ScriptEngine::RegisterNativeHandler(0x9088EB5A43FFB0A1, [](fx::ScriptContext& context)
		{
			fwEntity* vehicle = GetVehicleEntityFromContext(context, "SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX");
			if (!vehicle)
			{
				return;
			}

			int plateIndex = context.GetArgument<int>(1);
			trace("CustomPlateRegistry: SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX(veh=%p, slot=%d)\n", (void*)vehicle, plateIndex);

			// If vanilla index and not registered as custom, pass through to vanilla native
			if (plateIndex < CustomPlateRegistry::kFirstCustomPlateIndex &&
				!CustomPlateRegistry::GetInstance()->IsValidCustomIndex(plateIndex))
			{
				if (g_origSetVehicleNumberPlateTextIndex)
				{
					g_origSetVehicleNumberPlateTextIndex(context);
				}
				SetVehiclePlateIndexInMemory(vehicle, plateIndex);
				return;
			}

			// Custom plate index (>= 13):
			// 1. Resolve matching native font slot so GTA V renders the intended font color
			int targetFontIndex = 5; // Default Black text
			const auto* plateData = CustomPlateRegistry::GetInstance()->GetPlate(plateIndex);
			if (plateData)
			{
				targetFontIndex = plateData->fontSlot;
			}
			else
			{
				targetFontIndex = (plateIndex - 13) % 5;
			}

			// Invoke original native with target font slot so GTA V marks vehicle appearance dirty,
			// applies proper font shader palette (black, white, yellow, blue, etc.), and triggers draw refresh.
			context.SetArgument(1, targetFontIndex);
			if (g_origSetVehicleNumberPlateTextIndex)
			{
				g_origSetVehicleNumberPlateTextIndex(context);
			}
			context.SetArgument(1, plateIndex);

			// 2. Write custom plate index to CVehicle memory and tracker
			SetVehiclePlateIndexInMemory(vehicle, plateIndex);
		});

		// GET_VEHICLE_NUMBER_PLATE_TEXT_INDEX (0xF11BC2DD9A3E7195)
		fx::ScriptEngine::RegisterNativeHandler(0xF11BC2DD9A3E7195, [](fx::ScriptContext& context)
		{
			fwEntity* vehicle = GetVehicleEntityFromContext(context, "GET_VEHICLE_NUMBER_PLATE_TEXT_INDEX");
			if (!vehicle)
			{
				context.SetResult<int>(-1);
				return;
			}

			int index = GetVehiclePlateIndexFromMemory(vehicle);
			if (index > 0)
			{
				context.SetResult<int>(index);
				return;
			}

			if (g_origGetVehicleNumberPlateTextIndex)
			{
				g_origGetVehicleNumberPlateTextIndex(context);
			}
			else
			{
				context.SetResult<int>(0);
			}
		});

		// GET_NUMBER_OF_VEHICLE_NUMBER_PLATES (0x4C4D6B2644F458CB)
		fx::ScriptEngine::RegisterNativeHandler(0x4C4D6B2644F458CB, [](fx::ScriptContext& context)
		{
			int vanillaCount = g_vanillaPlateCount;
			if (g_origGetNumberOfVehicleNumberPlates)
			{
				fx::ScriptContextBuffer subContext;
				g_origGetNumberOfVehicleNumberPlates(subContext);
				int res = subContext.GetResult<int>();
				if (res > 0 && res < 100)
				{
					vanillaCount = res;
					g_vanillaPlateCount = res;
				}
			}

			context.SetResult<int>(vanillaCount + CustomPlateRegistry::GetInstance()->GetCount());
		});
	});
});
