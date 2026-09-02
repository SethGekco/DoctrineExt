#include "Doctrine/KillTracker.h"
#include "Doctrine/Config.h"

#include <TechnoClass.h>
#include <TechnoTypeClass.h>
#include <HouseClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <map>

namespace
{
	struct KillStats
	{
		int Kills = 0;
		int ValueKilled = 0;
		// Identity guard: the map is keyed by pointer, and the game reuses
		// freed object memory. A unit that dies is erased here (its death
		// funnels through the same hook), but units removed without dying
		// (sold, despawned) can leave an entry a NEW object inherits — the
		// stored type+owner unmasks that and resets the entry.
		TechnoTypeClass* Type = nullptr;
		HouseClass* Owner = nullptr;
	};

	std::map<TechnoClass*, KillStats> g_kills;

	bool IsStale(TechnoClass* const pUnit, const KillStats& stats)
	{
		return stats.Type != pUnit->GetTechnoType() || stats.Owner != pUnit->Owner;
	}
}

KillTracker::Ace KillTracker::TopEnemyAce(HouseClass* pOwner)
{
	Ace best;
	DWORD bestId = 0;
	bool const mobileOnly = DoctrineConfig::Instance.AceMobileOnly;
	for (auto const& [pUnit, stats] : g_kills)
	{
		if (!pUnit || IsStale(pUnit, stats)) continue;
		if (pUnit->InLimbo || pUnit->Health <= 0) continue;
		// An "ace" is a mobile threat by default — a base defense (Building)
		// racking up kills should not send the AI charging into a base to
		// hunt a turret. Its kills are still tracked (useful for the future
		// death-zone sensor), just not selectable as a hunt target here.
		if (mobileOnly)
		{
			auto const what = pUnit->WhatAmI();
			if (what != AbstractType::Infantry && what != AbstractType::Unit
				&& what != AbstractType::Aircraft)
				continue;
		}
		auto const pEnemy = pUnit->Owner;
		if (!pEnemy || pEnemy == pOwner || pEnemy->Defeated) continue;
		if (pEnemy->IsNeutral() || pEnemy->IsObserver()) continue;
		if (pOwner->IsAlliedWith(pEnemy)) continue;

		// Deterministic ranking: kills, then value, then synced UniqueID.
		// Pointer-order iteration must never decide a tie — heap addresses
		// differ across clients and would desync the pick.
		bool better = stats.Kills > best.Kills
			|| (stats.Kills == best.Kills && stats.ValueKilled > best.ValueKilled)
			|| (stats.Kills == best.Kills && stats.ValueKilled == best.ValueKilled
				&& (!best.Unit || pUnit->UniqueID < bestId));
		if (better)
		{
			best = { pUnit, stats.Kills, stats.ValueKilled };
			bestId = pUnit->UniqueID;
		}
	}
	return best;
}

void KillTracker::Reset()
{
	g_kills.clear();
}

// Entry of TechnoClass::RegisterDestruction — ECX = the dying object,
// [ESP+0x4] = the destroyer. First instructions are five 1-2 byte pushes
// (0x702D40..44), so 0x5 lands on an instruction boundary. Existing
// consumers in this function (Antares 0x702DD6/0x702E64/0x702E9D, Phobos
// 0x702E4E, Kratos 0x702E9D) are all deeper inside — no overlap.
DEFINE_HOOK(0x702D40, DoctrineExt_TechnoClass_RegisterDestruction_KillTracker, 0x5)
{
	GET(TechnoClass* const, pVictim, ECX);
	GET_STACK(TechnoClass* const, pKiller, 0x4);

	// The victim's own scoreboard dies with it.
	g_kills.erase(pVictim);

	if (pKiller && pKiller != pVictim && pKiller->Owner && pVictim->Owner
		&& !pVictim->Owner->IsNeutral()
		&& !pKiller->Owner->IsAlliedWith(pVictim->Owner))
	{
		auto& stats = g_kills[pKiller];
		if (stats.Type && IsStale(pKiller, stats))
			stats = KillStats{};
		if (!stats.Type)
		{
			stats.Type = pKiller->GetTechnoType();
			stats.Owner = pKiller->Owner;
		}
		++stats.Kills;
		if (auto const pVType = pVictim->GetTechnoType())
			stats.ValueKilled += pVType->GetCost();

		// Rare, notable: announce a rising ace at a few milestones.
		if (stats.Kills == 3 || stats.Kills == 5 || stats.Kills == 10
			|| stats.Kills == 20)
			Debug::Log("[DoctrineExt] ace watch: %s#%d %s reached %d kills (%d credits).\n",
				pKiller->Owner->get_ID(), pKiller->Owner->ArrayIndex,
				stats.Type ? stats.Type->ID : "?", stats.Kills, stats.ValueKilled);
	}

	return 0;
}
