/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "CustomPlateRegistry.h"

#include <tinyxml2.h>
#include <VFSManager.h>
#include <Resource.h>
#include <ResourceMetaDataComponent.h>
#include <CoreConsole.h>
#include <ScriptEngine.h>
#include <fxScripting.h>
#include <cstring>
#include <cstdlib>

extern void SetCustomPlatePatternInternal(int index, const std::string& pattern);
extern void RemoveCustomPlatePatternInternal(int index);

CustomPlateRegistry* CustomPlateRegistry::GetInstance()
{
	static CustomPlateRegistry s_instance;
	return &s_instance;
}

int CustomPlateRegistry::HexColorToBestFontSlot(uint32_t argb)
{
	int r = (argb >> 16) & 0xFF;
	int g = (argb >> 8) & 0xFF;
	int b = argb & 0xFF;

	// Perceived luminance: CCIR 601 formula
	int lum = (r * 299 + g * 587 + b * 114) / 1000;

	// Very dark -> Black text (Slot 5: North Yankton / Las Venturas)
	if (lum < 75) return 5;

	// Very bright and low saturation -> White text (Slot 11: Panic / Pounders)
	int maxC = (std::max)(r, (std::max)(g, b));
	int minC = (std::min)(r, (std::min)(g, b));
	int sat = (maxC > 0) ? ((maxC - minC) * 255 / maxC) : 0;

	if (lum > 180 && sat < 60) return 11; // White font

	// Dominant Red (Slot 6: eCola / Exempt)
	if (r > 130 && g < 110 && b < 110) return 6;

	// Dominant Green (Slot 10: Sprunk)
	if (g > 120 && r < 110 && b < 110) return 10;

	// Dominant Yellow / Gold (Slot 1: SA Black & Yellow)
	if (r > 130 && g > 110 && b < 90) return 1;

	// Dominant Blue (Slot 0: San Andreas Standard)
	if (b > 120 && r < 110) return 0;

	// Pink / Purple / Magenta / Orange (Slot 9: LS Car Meet)
	if (r > 150 && b > 80 && g < 110) return 9;

	// Fallback based on luminance
	if (lum >= 128) return 11; // White
	return 5;                  // Black
}

uint32_t CustomPlateRegistry::ParseHexColorString(const char* str)
{
	if (!str || !*str) return 0xFF080808;

	// Named color shortcuts
	if (_stricmp(str, "black") == 0) return 0xFF080808;
	if (_stricmp(str, "white") == 0) return 0xFFFFFFFF;
	if (_stricmp(str, "yellow") == 0) return 0xFFE0AF1F;
	if (_stricmp(str, "gold") == 0) return 0xFFFFD700;
	if (_stricmp(str, "blue") == 0) return 0xFF14244B;
	if (_stricmp(str, "red") == 0) return 0xFFB81D1D;
	if (_stricmp(str, "green") == 0) return 0xFF22B14C;
	if (_stricmp(str, "pink") == 0) return 0xFFFF69B4;
	if (_stricmp(str, "orange") == 0) return 0xFFFFA500;

	// Strip leading '#', '0x', '0X'
	if (str[0] == '#') str++;
	else if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;

	uint32_t val = static_cast<uint32_t>(strtoul(str, nullptr, 16));
	// If only 6 hex digits (RRGGBB), add full alpha 0xFF000000
	if ((val & 0xFF000000) == 0 && strlen(str) <= 6)
	{
		val |= 0xFF000000;
	}
	return val;
}

int CustomPlateRegistry::RegisterPlate(const CustomPlateData& plate)
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);

	CustomPlateData entry = plate;

	if (entry.index < 0)
	{
		while (m_platesByIndex.find(m_nextAutoIndex) != m_platesByIndex.end())
		{
			++m_nextAutoIndex;
		}

		if (m_nextAutoIndex > kMaxCustomPlateIndex)
		{
			trace("CustomPlateRegistry: Exceeded maximum allowed plate index (%d)\n", kMaxCustomPlateIndex);
			return -1;
		}

		entry.index = m_nextAutoIndex++;
	}
	else if (entry.index < kFirstCustomPlateIndex)
	{
		trace("CustomPlateRegistry: Slot index %d is reserved for vanilla plates (0-%d)\n", entry.index, kFirstCustomPlateIndex - 1);
		return -1;
	}

	if (entry.normalDict.empty())
	{
		entry.normalDict = entry.diffuseDict;
	}

	if (entry.fontSlot <= 0 || entry.fontSlot > 12)
	{
		entry.fontSlot = HexColorToBestFontSlot(entry.textColor);
	}

	int assignedIndex = entry.index;
	m_platesByIndex[assignedIndex] = entry;

	if (!entry.name.empty())
	{
		m_indexByName[entry.name] = assignedIndex;
	}

	if (!entry.pattern.empty())
	{
		SetCustomPlatePatternInternal(assignedIndex, entry.pattern);
	}

	trace("CustomPlateRegistry: Registered plate '%s' at slot index %d [diffuse: %s/%s, normal: %s/%s, fontSlot: %d]\n",
		entry.name.c_str(), assignedIndex,
		entry.diffuseDict.c_str(), entry.diffuseTex.c_str(),
		entry.normalDict.c_str(), entry.normalTex.c_str(),
		entry.fontSlot);

	return assignedIndex;
}

bool CustomPlateRegistry::UnregisterPlate(int index)
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);

	auto it = m_platesByIndex.find(index);
	if (it == m_platesByIndex.end())
	{
		return false;
	}

	if (!it->second.name.empty())
	{
		m_indexByName.erase(it->second.name);
	}

	RemoveCustomPlatePatternInternal(index);
	m_platesByIndex.erase(it);

	return true;
}

void CustomPlateRegistry::UnregisterPlatesForResource(const std::string& resourceName)
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);

	for (auto it = m_platesByIndex.begin(); it != m_platesByIndex.end(); )
	{
		if (it->second.resourceName == resourceName)
		{
			if (!it->second.name.empty())
			{
				m_indexByName.erase(it->second.name);
			}

			RemoveCustomPlatePatternInternal(it->first);
			it = m_platesByIndex.erase(it);
		}
		else
		{
			++it;
		}
	}
}

const CustomPlateData* CustomPlateRegistry::GetPlate(int index) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);

	auto it = m_platesByIndex.find(index);
	if (it != m_platesByIndex.end())
	{
		return &it->second;
	}

	return nullptr;
}

const CustomPlateData* CustomPlateRegistry::FindPlateByName(const std::string& name) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);

	auto itName = m_indexByName.find(name);
	if (itName != m_indexByName.end())
	{
		auto itPlate = m_platesByIndex.find(itName->second);
		if (itPlate != m_platesByIndex.end())
		{
			return &itPlate->second;
		}
	}

	return nullptr;
}

bool CustomPlateRegistry::SetPlateTextColor(int index, uint32_t color)
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_platesByIndex.find(index);
	if (it != m_platesByIndex.end())
	{
		it->second.textColor = color;
		it->second.fontSlot = HexColorToBestFontSlot(color);
		return true;
	}
	return false;
}

bool CustomPlateRegistry::SetPlateExtents(int index, float x, float y, float width, float height)
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_platesByIndex.find(index);
	if (it != m_platesByIndex.end())
	{
		it->second.extentsX = x;
		it->second.extentsY = y;
		it->second.extentsZ = width;
		it->second.extentsW = height;
		it->second.hasCustomExtents = true;
		return true;
	}
	return false;
}

int CustomPlateRegistry::GetPlateFontSlot(int index) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	auto it = m_platesByIndex.find(index);
	if (it != m_platesByIndex.end())
	{
		return it->second.fontSlot;
	}
	return 5; // Default Black
}

int CustomPlateRegistry::GetCount() const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return static_cast<int>(m_platesByIndex.size());
}

bool CustomPlateRegistry::IsValidCustomIndex(int index) const
{
	std::shared_lock<std::shared_mutex> lock(m_mutex);
	return m_platesByIndex.find(index) != m_platesByIndex.end();
}

void CustomPlateRegistry::Clear()
{
	std::unique_lock<std::shared_mutex> lock(m_mutex);
	for (const auto& [idx, data] : m_platesByIndex)
	{
		RemoveCustomPlatePatternInternal(idx);
	}
	m_platesByIndex.clear();
	m_indexByName.clear();
	m_nextAutoIndex = kFirstCustomPlateIndex;
}

static std::string GetElementTextSafe(const tinyxml2::XMLElement* element)
{
	if (!element)
	{
		return "";
	}

	const char* text = element->GetText();
	return text ? text : "";
}

int CustomPlateRegistry::LoadPlatesFromXml(const std::string& xmlContent, const std::string& resourceName)
{
	tinyxml2::XMLDocument doc;
	tinyxml2::XMLError err = doc.Parse(xmlContent.c_str(), xmlContent.size());
	if (err != tinyxml2::XML_SUCCESS)
	{
		trace("CustomPlateRegistry: Failed to parse XML plate file from resource %s (error %d)\n", resourceName.c_str(), err);
		return 0;
	}

	const tinyxml2::XMLElement* root = doc.RootElement();
	if (!root)
	{
		return 0;
	}

	const tinyxml2::XMLElement* platesContainer = root->FirstChildElement("plates");
	if (!platesContainer)
	{
		platesContainer = root->FirstChildElement("VehiclePlates");
	}
	if (!platesContainer)
	{
		platesContainer = root;
	}

	int registeredCount = 0;

	for (const tinyxml2::XMLElement* item = platesContainer->FirstChildElement("Item"); item != nullptr; item = item->NextSiblingElement("Item"))
	{
		CustomPlateData plate;
		plate.resourceName = resourceName;

		plate.name = GetElementTextSafe(item->FirstChildElement("name"));

		// Diffuse dict
		plate.diffuseDict = GetElementTextSafe(item->FirstChildElement("diffuseDict"));
		if (plate.diffuseDict.empty())
		{
			plate.diffuseDict = GetElementTextSafe(item->FirstChildElement("diffuseTexture"));
		}

		// Diffuse texture name
		plate.diffuseTex = GetElementTextSafe(item->FirstChildElement("diffuseTex"));
		if (plate.diffuseTex.empty())
		{
			plate.diffuseTex = GetElementTextSafe(item->FirstChildElement("fileName"));
		}

		// Normal dict
		plate.normalDict = GetElementTextSafe(item->FirstChildElement("normalDict"));
		if (plate.normalDict.empty())
		{
			plate.normalDict = plate.diffuseDict;
		}

		// Normal texture name
		plate.normalTex = GetElementTextSafe(item->FirstChildElement("normalTex"));
		if (plate.normalTex.empty())
		{
			plate.normalTex = GetElementTextSafe(item->FirstChildElement("normalName"));
		}

		// Pattern
		plate.pattern = GetElementTextSafe(item->FirstChildElement("pattern"));

		// Text color (supports hex string, named color, or integer)
		const tinyxml2::XMLElement* colorElem = item->FirstChildElement("textColor");
		if (colorElem)
		{
			const char* colorText = colorElem->GetText();
			if (colorText)
			{
				plate.textColor = ParseHexColorString(colorText);
				plate.fontSlot = HexColorToBestFontSlot(plate.textColor);
			}
		}

		// Font extents / placement
		const tinyxml2::XMLElement* extentsElem = item->FirstChildElement("fontExtents");
		if (!extentsElem) extentsElem = item->FirstChildElement("extents");
		if (extentsElem)
		{
			const tinyxml2::XMLElement* xElem = extentsElem->FirstChildElement("x");
			const tinyxml2::XMLElement* yElem = extentsElem->FirstChildElement("y");
			const tinyxml2::XMLElement* wElem = extentsElem->FirstChildElement("width");
			if (!wElem) wElem = extentsElem->FirstChildElement("w");
			const tinyxml2::XMLElement* hElem = extentsElem->FirstChildElement("height");
			if (!hElem) hElem = extentsElem->FirstChildElement("h");

			if (xElem) xElem->QueryFloatText(&plate.extentsX);
			if (yElem) yElem->QueryFloatText(&plate.extentsY);
			if (wElem) wElem->QueryFloatText(&plate.extentsZ);
			if (hElem) hElem->QueryFloatText(&plate.extentsW);
			plate.hasCustomExtents = true;
		}

		// Explicit index
		const tinyxml2::XMLElement* indexElem = item->FirstChildElement("index");
		if (indexElem)
		{
			plate.index = indexElem->IntText(-1);
		}

		if (plate.diffuseTex.empty() && !plate.diffuseDict.empty())
		{
			plate.diffuseTex = plate.diffuseDict;
		}

		if (!plate.diffuseTex.empty())
		{
			if (plate.diffuseDict.empty())
			{
				plate.diffuseDict = plate.diffuseTex;
			}

			if (RegisterPlate(plate) >= 0)
			{
				++registeredCount;
			}
		}
	}

	return registeredCount;
}

static InitFunction initFunction([]()
{
	fx::Resource::OnInitializeInstance.Connect([](fx::Resource* resource)
	{
		resource->OnStart.Connect([resource]()
		{
			fwRefContainer<fx::ResourceMetaDataComponent> metaData = resource->GetComponent<fx::ResourceMetaDataComponent>();
			if (!metaData.GetRef())
			{
				return;
			}

			std::string resourcePath = resource->GetPath();
			std::string resourceName = resource->GetName();

			std::vector<std::string> plateFiles;

			// 1. vehicle_plate_file 'filename'
			for (const auto& entry : metaData->GetEntries("vehicle_plate_file"))
			{
				plateFiles.push_back(entry.second);
			}

			// 2. data_file 'VEHICLE_PLATE_FILE' 'filename'
			auto dataFileEntries = metaData->GetEntries("data_file");
			auto dataFileExtraEntries = metaData->GetEntries("data_file_extra");

			auto itData = dataFileEntries.begin();
			auto itExtra = dataFileExtraEntries.begin();

			while (itData != dataFileEntries.end() && itExtra != dataFileExtraEntries.end())
			{
				if (itData->second == "VEHICLE_PLATE_FILE")
				{
					plateFiles.push_back(itExtra->second);
				}
				++itData;
				++itExtra;
			}

			for (const auto& relativePath : plateFiles)
			{
				std::string cleanPath = relativePath;
				if (!cleanPath.empty() && cleanPath.front() == '"' && cleanPath.back() == '"')
				{
					cleanPath = cleanPath.substr(1, cleanPath.length() - 2);
				}

				std::string fullPath = resourcePath + "/" + cleanPath;
				fwRefContainer<vfs::Stream> stream = vfs::OpenRead(fullPath);
				if (!stream.GetRef())
				{
					// Try without slash prefix or directly
					fullPath = cleanPath;
					stream = vfs::OpenRead(fullPath);
				}

				if (stream.GetRef())
				{
					std::vector<uint8_t> buffer(stream->GetLength());
					stream->Read(buffer.data(), buffer.size());
					std::string content(buffer.begin(), buffer.end());

					CustomPlateRegistry::GetInstance()->LoadPlatesFromXml(content, resourceName);
				}
				else
				{
					trace("CustomPlateRegistry: Could not open plate file '%s' for resource '%s'\n", cleanPath.c_str(), resourceName.c_str());
				}
			}
		});

		resource->OnStop.Connect([resource]()
		{
			CustomPlateRegistry::GetInstance()->UnregisterPlatesForResource(resource->GetName());
		});
	});

	// Register script helper natives
	fx::ScriptEngine::RegisterNativeHandler("ADD_VEHICLE_NUMBER_PLATE_STYLE", [](fx::ScriptContext& context)
	{
		const char* name = context.GetArgument<const char*>(0);
		const char* diffuseDict = context.GetArgument<const char*>(1);
		const char* diffuseTex = context.GetArgument<const char*>(2);
		const char* normalDict = context.GetArgument<const char*>(3);
		const char* normalTex = context.GetArgument<const char*>(4);
		const char* pattern = context.GetArgument<const char*>(5);

		CustomPlateData data;
		data.name = name ? name : "";
		data.diffuseDict = diffuseDict ? diffuseDict : "";
		data.diffuseTex = diffuseTex ? diffuseTex : "";
		data.normalDict = normalDict ? normalDict : "";
		data.normalTex = normalTex ? normalTex : "";
		data.pattern = pattern ? pattern : "";

		// Optional argument 6: textColor (string hex or integer)
		if (context.GetArgumentCount() > 6)
		{
			const char* colorStr = context.GetArgument<const char*>(6);
			if (colorStr && reinterpret_cast<uintptr_t>(colorStr) > 0x10000)
			{
				data.textColor = CustomPlateRegistry::ParseHexColorString(colorStr);
			}
			else
			{
				data.textColor = static_cast<uint32_t>(context.GetArgument<int>(6));
			}
			data.fontSlot = CustomPlateRegistry::HexColorToBestFontSlot(data.textColor);
		}

		fx::OMPtr<IScriptRuntime> runtime;
		if (FX_SUCCEEDED(fx::GetCurrentScriptRuntime(&runtime)))
		{
			auto resource = static_cast<fx::Resource*>(runtime->GetParentObject());
			if (resource)
			{
				data.resourceName = resource->GetName();
			}
		}

		int result = CustomPlateRegistry::GetInstance()->RegisterPlate(data);
		context.SetResult<int>(result);
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_VEHICLE_NUMBER_PLATE_INDEX_BY_NAME", [](fx::ScriptContext& context)
	{
		const char* name = context.GetArgument<const char*>(0);
		if (!name)
		{
			context.SetResult<int>(-1);
			return;
		}

		const auto* plate = CustomPlateRegistry::GetInstance()->FindPlateByName(name);
		context.SetResult<int>(plate ? plate->index : -1);
	});

	fx::ScriptEngine::RegisterNativeHandler("GET_VEHICLE_NUMBER_PLATE_NAME", [](fx::ScriptContext& context)
	{
		int index = context.GetArgument<int>(0);
		const auto* plate = CustomPlateRegistry::GetInstance()->GetPlate(index);
		context.SetResult<const char*>(plate ? plate->name.c_str() : "");
	});

	// SET_CUSTOM_PLATE_TEXT_COLOR(slot, hexColorStringOrInt)
	fx::ScriptEngine::RegisterNativeHandler("SET_CUSTOM_PLATE_TEXT_COLOR", [](fx::ScriptContext& context)
	{
		int slot = context.GetArgument<int>(0);
		uint32_t color = 0xFFFFFFFF;
		const char* str = context.GetArgument<const char*>(1);
		if (str && reinterpret_cast<uintptr_t>(str) > 0x10000)
		{
			color = CustomPlateRegistry::ParseHexColorString(str);
		}
		else
		{
			color = static_cast<uint32_t>(context.GetArgument<int>(1));
		}

		bool success = CustomPlateRegistry::GetInstance()->SetPlateTextColor(slot, color);
		context.SetResult<bool>(success);
	});

	// GET_CUSTOM_PLATE_TEXT_COLOR(slot) -> returns 32-bit ARGB hex int
	fx::ScriptEngine::RegisterNativeHandler("GET_CUSTOM_PLATE_TEXT_COLOR", [](fx::ScriptContext& context)
	{
		int slot = context.GetArgument<int>(0);
		const auto* plate = CustomPlateRegistry::GetInstance()->GetPlate(slot);
		context.SetResult<uint32_t>(plate ? plate->textColor : 0xFFFFFFFF);
	});

	// GET_CUSTOM_PLATE_FONT_SLOT(slot) -> returns GTA V font slot (0-12)
	fx::ScriptEngine::RegisterNativeHandler("GET_CUSTOM_PLATE_FONT_SLOT", [](fx::ScriptContext& context)
	{
		int slot = context.GetArgument<int>(0);
		context.SetResult<int>(CustomPlateRegistry::GetInstance()->GetPlateFontSlot(slot));
	});

	// SET_CUSTOM_PLATE_EXTENTS(slot, x, y, width, height)
	fx::ScriptEngine::RegisterNativeHandler("SET_CUSTOM_PLATE_EXTENTS", [](fx::ScriptContext& context)
	{
		int slot = context.GetArgument<int>(0);
		float x = context.GetArgument<float>(1);
		float y = context.GetArgument<float>(2);
		float w = context.GetArgument<float>(3);
		float h = context.GetArgument<float>(4);

		bool success = CustomPlateRegistry::GetInstance()->SetPlateExtents(slot, x, y, w, h);
		context.SetResult<bool>(success);
	});
});
