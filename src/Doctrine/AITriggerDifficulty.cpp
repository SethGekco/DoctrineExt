#include "Doctrine/AITriggerDifficulty.h"

#include <AITriggerTypeClass.h>
#include <CCINIClass.h>
#include <Utilities/Debug.h>

#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	bool g_applied = false;

	// The rules difficulty section for each AIDifficulty value, and the flag
	// index that value READS. AIDifficulty is inverted (0=Hard, 1=Normal,
	// 2=Easy), and the rules section names map so that the vanilla
	// [Easy]=2/[Normal]=1/[Difficult]=0 config is the identity rewrite:
	//   d=0 -> section "Easy",      reads flag 2 (Enabled_Hard)
	//   d=1 -> section "Normal",    reads flag 1 (Enabled_Normal)
	//   d=2 -> section "Difficult", reads flag 0 (Enabled_Easy)
	const char* const kSection[3] = { "Easy", "Normal", "Difficult" };
	const int         kReadFlag[3] = { 2, 1, 0 };

	// flag 0=Enabled_Easy, 1=Enabled_Normal, 2=Enabled_Hard.
	bool GetFlag(AITriggerTypeClass* const pT, int const i)
	{
		return i == 0 ? pT->Enabled_Easy : i == 1 ? pT->Enabled_Normal : pT->Enabled_Hard;
	}
	void SetFlag(AITriggerTypeClass* const pT, int const i, bool const v)
	{
		if (i == 0) pT->Enabled_Easy = v;
		else if (i == 1) pT->Enabled_Normal = v;
		else pT->Enabled_Hard = v;
	}

	std::vector<int> ParseInts(const char* s)
	{
		std::vector<int> out;
		std::string tok;
		for (const char* p = s; ; ++p)
		{
			if (*p == ',' || *p == '\0')
			{
				if (!tok.empty()) out.push_back(std::atoi(tok.c_str()));
				tok.clear();
				if (*p == '\0') break;
			}
			else if (*p != ' ') tok += *p;
		}
		return out;
	}
}

void AITriggerDifficulty::EnsureApplied()
{
	if (g_applied) return;

	auto const pRules = CCINIClass::INI_Rules;
	auto const pAI = &CCINIClass::INI_AI;
	if (!pRules) return;
	if (AITriggerTypeClass::Array.Count <= 0) return; // triggers not loaded yet
	g_applied = true;

	char buf[512] = {};

	// Read each difficulty's Inherit list; note if any override exists at all.
	std::vector<int> inherit[3];
	bool any = false;
	for (int d = 0; d < 3; ++d)
	{
		pRules->ReadString(kSection[d], "AITriggerTypes.Inherit", "", buf, sizeof(buf));
		if (*buf) { inherit[d] = ParseInts(buf); any = true; }
	}
	if (!any) return; // vanilla — leave every flag alone

	// Resolve named bundles (Inherit index >=3): aimd [DifficultyTypes] gives
	// the name, [<Name>.AITriggerTypes] the explicit trigger-ID list.
	std::map<int, std::set<std::string>> bundle;
	auto ensureBundle = [&](int const idx)
	{
		if (idx < 3 || bundle.count(idx)) return;
		auto& ids = bundle[idx];
		if (!pAI) return;
		char nameKey[8];
		std::snprintf(nameKey, sizeof(nameKey), "%d", idx);
		char name[64] = {};
		pAI->ReadString("DifficultyTypes", nameKey, "", name, sizeof(name));
		if (!*name) return;
		std::string section = std::string(name) + ".AITriggerTypes";
		int const n = pAI->GetKeyCount(section.c_str());
		for (int k = 0; k < n; ++k)
		{
			auto const pKey = pAI->GetKeyName(section.c_str(), k);
			if (!pKey) continue;
			pAI->ReadString(section.c_str(), pKey, "", buf, sizeof(buf));
			if (*buf) ids.insert(buf);
		}
	};
	for (int d = 0; d < 3; ++d)
		for (int idx : inherit[d])
			ensureBundle(idx);

	// Rewrite: for each trigger, snapshot its original flags, then for every
	// difficulty that overrides, OR together the inherited sources and write the
	// result onto the flag that difficulty reads.
	int rewritten = 0;
	for (auto const pT : AITriggerTypeClass::Array)
	{
		if (!pT) continue;
		bool const orig[3] = { pT->Enabled_Easy, pT->Enabled_Normal, pT->Enabled_Hard };
		for (int d = 0; d < 3; ++d)
		{
			if (inherit[d].empty()) continue; // this difficulty keeps vanilla
			bool enabled = false;
			for (int idx : inherit[d])
			{
				if (idx < 0) continue;                       // -1 = none
				if (idx <= 2) { if (orig[idx]) enabled = true; }
				else if (bundle[idx].count(pT->ID)) enabled = true;
			}
			SetFlag(pT, kReadFlag[d], enabled);
		}
		++rewritten;
	}

	Debug::Log("[DoctrineExt] AITrigger difficulty remap applied: Easy=[%s] Normal=[%s] "
		"Difficult=[%s], %d triggers rewritten, %u named bundles.\n",
		inherit[0].empty() ? "vanilla" : "set", inherit[1].empty() ? "vanilla" : "set",
		inherit[2].empty() ? "vanilla" : "set", rewritten, bundle.size());
}

void AITriggerDifficulty::Reset()
{
	g_applied = false;
}
