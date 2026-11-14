#include "Doctrine/Verses.h"

#include <CCINIClass.h>
#include <WarheadTypeClass.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	const char* const kVanilla[11] = { "none", "flak", "plate", "light", "medium", "heavy", "wood", "steel",
		"concrete", "special_1", "special_2" };

	// [ArmorTypes], in order: armor index 11 + i. Default = a percentage or another armor's name.
	struct Custom { std::string Name, Default; };
	std::vector<Custom> g_custom;
	bool g_customRead = false;
	std::map<WarheadTypeClass const*, std::vector<double>> g_tables;

	// "75%" -> 0.75, "0.75" -> 0.75 (Ares takes either). False when it isn't a number.
	bool Percent(const char* const s, double& out)
	{
		char* end = nullptr;
		double const v = std::strtod(s, &end);
		if (end == s)
			return false;
		while (*end == ' ' || *end == '\t') ++end;
		out = *end == '%' ? v / 100.0 : v;
		return true;
	}

	int IndexOf(const char* const name)
	{
		for (int i = 0; i < 11; ++i)
			if (_strcmpi(name, kVanilla[i]) == 0) return i;
		for (size_t i = 0; i < g_custom.size(); ++i)
			if (_strcmpi(name, g_custom[i].Name.c_str()) == 0) return 11 + static_cast<int>(i);
		return -1;
	}

	void ReadCustom(CCINIClass* const pINI)
	{
		g_customRead = true;
		g_custom.clear();
		if (!pINI)
			return;
		char buf[128];
		int const count = pINI->GetKeyCount("ArmorTypes");
		for (int i = 0; i < count; ++i)
		{
			auto const pName = pINI->GetKeyName("ArmorTypes", i);
			if (!pName || !*pName || IndexOf(pName) >= 0)
				continue;
			pINI->ReadString("ArmorTypes", pName, "", buf, sizeof(buf));
			g_custom.push_back({ pName, buf });
		}
	}

	std::vector<double> const& Table(WarheadTypeClass* const pWarhead)
	{
		auto const it = g_tables.find(pWarhead);
		if (it != g_tables.end())
			return it->second;

		auto& table = g_tables[pWarhead];
		table.assign(11 + g_custom.size(), 1.0);
		auto const pINI = CCINIClass::INI_Rules;
		if (!pINI)
		{
			for (int i = 0; i < 11; ++i)
				table[i] = pWarhead->Verses[i];
			return table;
		}

		char buf[512];
		if (pINI->ReadString(pWarhead->ID, "Verses", "", buf, sizeof(buf)) > 0)
		{
			const char* p = buf;
			for (int i = 0; i < 11 && *p; ++i)
			{
				Percent(p, table[i]);
				auto const pComma = std::strchr(p, ',');
				if (!pComma) break;
				p = pComma + 1;
			}
		}
		char key[96];
		for (int i = 0; i < 11; ++i)
		{
			std::snprintf(key, sizeof(key), "Versus.%s", kVanilla[i]);
			if (pINI->ReadString(pWarhead->ID, key, "", buf, sizeof(buf)) > 0)
				Percent(buf, table[i]);
		}
		// Custom armors: Versus.<name>, else the [ArmorTypes] default (a percentage, or an armor whose value it
		// takes; that armor read first, so a chain of custom armors resolves in order).
		for (size_t c = 0; c < g_custom.size(); ++c)
		{
			auto& value = table[11 + c];
			std::snprintf(key, sizeof(key), "Versus.%s", g_custom[c].Name.c_str());
			if (pINI->ReadString(pWarhead->ID, key, "", buf, sizeof(buf)) > 0 && Percent(buf, value))
				continue;
			auto const& def = g_custom[c].Default;
			if (Percent(def.c_str(), value))
				continue;
			int const base = IndexOf(def.c_str());
			value = base >= 0 && base < 11 + static_cast<int>(c) ? table[base] : 1.0;
		}
		return table;
	}
}

double Verses::Of(WarheadTypeClass* const pWarhead, int const armor)
{
	if (!pWarhead)
		return 1.0;
	if (!g_customRead)
		ReadCustom(CCINIClass::INI_Rules);
	auto const& table = Table(pWarhead);
	return armor >= 0 && armor < static_cast<int>(table.size()) ? table[armor] : 1.0;
}

void Verses::Reset()
{
	g_custom.clear();
	g_customRead = false;
	g_tables.clear();
}
