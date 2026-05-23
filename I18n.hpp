#pragma once
#include "FnvHash.hpp"

std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

inline void LoadTranslateData()
{
	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), GetThreadUILanguage());
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				auto resourceSize = SizeofResource(g_hInst, hRes);
				// Guard against a corrupted resource: validate that the
				// declared entry count does not read past the available data
				// and that each table entry's offset stays within bounds.
				auto maxEntries = (resourceSize >= sizeof(YMOData))
					? (resourceSize - offsetof(YMOData, table)) / sizeof(ymo->table[0])
					: 0;
				auto count = (std::min)(static_cast<size_t>(ymo->len), maxEntries);
				hashToStrMap.reserve(count);

				for (size_t i = 0; i < count; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					// Each offset must point to a null-terminated wchar_t
					// string wholly within the resource. The +1 guards the
					// terminator so the pointer is always dereferenceable.
					if (offset + sizeof(wchar_t) > resourceSize ||
						(offset & (sizeof(wchar_t) - 1)) != 0)
					{
						continue;
					}
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(hResData) + offset);
					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

inline const wchar_t* Translate(const wchar_t* str)
{
	// ptrToStrMap is a hot cache: written once per unique input string on first
	// encounter, read on subsequent calls. ConnectDevice (a fire_and_forget
	// coroutine) calls this from arbitrary WinRT thread-pool threads after
	// co_await, so concurrent writes are possible when multiple devices connect
	// at the same time. The mutex serializes all access to prevent data races
	// on the unordered_map's internal structures (rehash, bucket list mutation).
	static std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;
	static std::mutex s_translateMtx;
	std::lock_guard<std::mutex> lock(s_translateMtx);

	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

inline const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)
