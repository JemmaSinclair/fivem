/*
 * This file is part of the Cfx project - https://cfx.re/
 *
 * See LICENSE in the root of the source tree for information
 * regarding licensing.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <cstdint>
#include <optional>
#include <algorithm>

struct CustomPlateData
{
	int index = -1;
	std::string name;
	std::string diffuseDict;
	std::string diffuseTex;
	std::string normalDict;
	std::string normalTex;
	std::string pattern;
	uint32_t textColor = 0xFF080808; // 32-bit ARGB (Default: Black)
	int fontSlot = 5;                // GTA V native plate font slot (Default: 5 = Yankton Black)
	float extentsX = 0.28f;          // Horizontal text start margin
	float extentsY = 0.355f;         // Vertical text start margin
	float extentsZ = 0.816f;         // Width span
	float extentsW = 0.92f;          // Height span
	bool hasCustomExtents = false;
	std::string resourceName;
};

class CustomPlateRegistry
{
public:
	static constexpr int kFirstCustomPlateIndex = 13;
	static constexpr int kMaxCustomPlateIndex = 32767;

	static CustomPlateRegistry* GetInstance();

	// Convert any 32-bit ARGB hex color to the best matching GTA V native plate font slot:
	// 5  = Black       (0xFF080808) - North Yankton / Las Venturas
	// 11 = White       (0xFFFFFFFF) - Panic / Pounders
	// 1  = Yellow/Gold (0xFFE0AF1F) - SA Black & Yellow
	// 0  = Blue        (0xFF14244B) - San Andreas Standard
	// 6  = Red         (0xFFB81D1D) - eCola / Exempt
	// 10 = Green       (0xFF22B14C) - Sprunk
	// 9  = Pink/Orange (0xFFFF69B4) - LS Car Meet
	static int HexColorToBestFontSlot(uint32_t argb);

	// Parse string into 32-bit ARGB color (supports "#RRGGBB", "0xRRGGBB", "RRGGBB", and named colors)
	static uint32_t ParseHexColorString(const char* str);

	// Registers a custom plate. If plate.index < 0, an available slot is automatically assigned.
	// Returns the assigned slot index (>= 13), or -1 on failure.
	int RegisterPlate(const CustomPlateData& plate);

	// Unregisters a specific custom plate slot index.
	bool UnregisterPlate(int index);

	// Unregisters all plates that were registered by a specific resource.
	void UnregisterPlatesForResource(const std::string& resourceName);

	// Retrieves plate data by slot index.
	const CustomPlateData* GetPlate(int index) const;

	// Finds a plate by its string identifier/name.
	const CustomPlateData* FindPlateByName(const std::string& name) const;

	// Sets the text color and automatically resolves the best native font slot for a plate.
	bool SetPlateTextColor(int index, uint32_t color);

	// Sets custom text bounding box extents for a plate.
	bool SetPlateExtents(int index, float x, float y, float width, float height);

	// Resolves target native GTA V font slot for a custom plate index.
	int GetPlateFontSlot(int index) const;

	// Returns the total number of registered custom plates.
	int GetCount() const;

	// Checks if an index is a registered custom plate slot (>= 13).
	bool IsValidCustomIndex(int index) const;

	// Parses XML data from a VEHICLE_PLATE_FILE (plates.meta) and registers each plate.
	// Returns the number of plates registered.
	int LoadPlatesFromXml(const std::string& xmlContent, const std::string& resourceName);

	// Clears all custom plates.
	void Clear();

private:
	CustomPlateRegistry() = default;
	~CustomPlateRegistry() = default;

	mutable std::shared_mutex m_mutex;
	std::unordered_map<int, CustomPlateData> m_platesByIndex;
	std::unordered_map<std::string, int> m_indexByName;
	int m_nextAutoIndex = kFirstCustomPlateIndex;
};
