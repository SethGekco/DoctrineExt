#include "Doctrine/GarrisonFire.h"
#include "Doctrine/Config.h"

#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <HouseClass.h>
#include <TechnoClass.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
	// UniqueID -> the building it was last sent into and when ([Doctrine.Garrison] HoldFire.Retry).
	struct Sent { DWORD Building; int Frame; };
	std::map<DWORD, Sent> g_sent;
	int g_lastSweep = -1;

	// Since the last summary line (HoldFire.Debug).
	struct Counts { int SentIn = 0, Dropped = 0, Retried = 0; } g_counts;

	// Everything below up to Sweep() may run on PerfExt's target-scan worker threads (the GetFireError hook):
	// it reads, never writes, and calls no engine function that could.

	bool Listed(std::vector<std::string> const& list, const char* const id)
	{
		for (auto const& s : list)
			if (_strcmpi(s.c_str(), id) == 0) return true;
		return false;
	}

	// The target, when it's a neutral building that can be garrisoned (empty: an occupied one belongs to its
	// occupants' house); else null.
	BuildingClass* Garrisonable(AbstractClass* const pTarget)
	{
		if (!pTarget || pTarget->WhatAmI() != AbstractType::Building)
			return nullptr;
		auto const pBld = static_cast<BuildingClass*>(pTarget);
		if (!pBld->Type || !pBld->Type->CanBeOccupied || !pBld->Owner || !pBld->Owner->IsNeutral())
			return nullptr;
		return pBld;
	}

	// An AI's shooter whose difficulty holds fire at this building: that difficulty's tags, else null.
	DoctrineConfig const* HoldsFire(TechnoClass* const pShooter, BuildingClass* const pBld)
	{
		auto const pOwner = pShooter->Owner;
		if (!pOwner || pOwner->IsNeutral() || pOwner->IsControlledByHuman())
			return nullptr;
		auto const& cfg = DoctrineConfig::For(pOwner);   // never parses: before the first parse, all defaults (off)
		if (!cfg.HoldFireEnable || cfg.HoldFireUnitsNone || cfg.HoldFireBuildingsNone)
			return nullptr;
		if (!cfg.HoldFireBuildings.empty() && !Listed(cfg.HoldFireBuildings, pBld->Type->ID))
			return nullptr;
		if (!cfg.HoldFireUnits.empty())
		{
			auto const pType = pShooter->GetTechnoType();
			if (!pType || !Listed(cfg.HoldFireUnits, pType->ID))
				return nullptr;
		}
		return &cfg;
	}

	// An occupier that might go in: Occupier=yes infantry and a free slot. The pass asks the engine
	// (CanBeOccupiedBy, which other DLLs hook) before sending it.
	bool MightEnter(DoctrineConfig const& cfg, TechnoClass* const pShooter, BuildingClass* const pBld)
	{
		if (!cfg.HoldFireEnter || pShooter->WhatAmI() != AbstractType::Infantry)
			return false;
		auto const pType = static_cast<InfantryClass*>(pShooter)->Type;
		return pType && pType->Occupier && pBld->Occupants.Count < pBld->Type->MaxNumberOccupants;
	}

	const char* TypeId(TechnoClass* const pTechno)
	{
		auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;
		return pType ? pType->ID : "?";
	}

	void Log(DoctrineConfig const& cfg, TechnoClass* const pUnit, const char* const what, BuildingClass* const pBld)
	{
		if (!cfg.HoldFireDebug) return;
		Debug::Log("[DoctrineExt] garrison-fire: %s#%u of %s#%d (mission %d) %s %s#%u\n", TypeId(pUnit),
			pUnit->UniqueID, pUnit->Owner->get_ID(), pUnit->Owner->ArrayIndex,
			static_cast<int>(pUnit->GetCurrentMission()), what, pBld->Type->ID, pBld->UniqueID);
	}

	void Look(DoctrineConfig const& cfg, TechnoClass* const pUnit, BuildingClass* const pBld, int const now)
	{
		auto const mission = pUnit->GetCurrentMission();
		if (mission == Mission::Capture || mission == Mission::Enter)
			return;   // on its way in (the engine's own Hunt and Attack send it this way too)

		auto const bit = static_cast<int>(mission) >= 0 && static_cast<int>(mission) < 32
			? 1u << static_cast<int>(mission) : 0u;
		if (MightEnter(cfg, pUnit, pBld) && (cfg.HoldFireMissionMask & bit))
		{
			auto const pInf = static_cast<InfantryClass*>(pUnit);
			if (pBld->CanBeOccupiedBy(pInf))
			{
				auto const it = g_sent.find(pInf->UniqueID);
				if (it == g_sent.end() || it->second.Building != pBld->UniqueID
					|| now - it->second.Frame >= std::max(0, cfg.HoldFireRetry))
				{
					// The engine's garrison order (Mission_Hunt 0x51F59A, a left click on the building).
					pInf->SetDestination(pBld, true);
					pInf->ForceMission(Mission::Capture);
					g_sent[pInf->UniqueID] = { pBld->UniqueID, now };
					++g_counts.SentIn;
					Log(cfg, pUnit, "goes into", pBld);
					return;
				}
				++g_counts.Retried;
				Log(cfg, pUnit, "didn't get in lately, drops", pBld);
				pUnit->SetTarget(nullptr);
				return;
			}
		}
		++g_counts.Dropped;
		Log(cfg, pUnit, "holds fire, drops", pBld);
		pUnit->SetTarget(nullptr);
	}
}

void GarrisonFire::Sweep()
{
	int const now = Unsorted::CurrentFrame;
	if (now == g_lastSweep)
		return;
	g_lastSweep = now;

	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pUnit = TechnoClass::Array.GetItem(i);
		if (!pUnit || !pUnit->Target || pUnit->InLimbo || !pUnit->IsAlive || pUnit->Health <= 0)
			continue;
		auto const pBld = Garrisonable(pUnit->Target);
		if (!pBld)
			continue;
		auto const pCfg = HoldsFire(pUnit, pBld);
		if (!pCfg)
			continue;
		unsigned const every = static_cast<unsigned>(std::max(1, pCfg->HoldFireInterval));
		if ((static_cast<unsigned>(now) + pUnit->UniqueID) % every == 0)
			Look(*pCfg, pUnit, pBld, now);
	}

	if (now % 1800 == 0)
	{
		bool const debug = DoctrineConfig::Instance.HoldFireDebug || DoctrineConfig::ByDifficulty[0].HoldFireDebug
			|| DoctrineConfig::ByDifficulty[1].HoldFireDebug || DoctrineConfig::ByDifficulty[2].HoldFireDebug;
		if (debug)
			Debug::Log("[DoctrineExt] garrison-fire: last 1800 frames: sent in %d, dropped %d, dropped after a "
				"failed entry %d\n", g_counts.SentIn, g_counts.Dropped, g_counts.Retried);
		g_counts = Counts{};
		for (auto it = g_sent.begin(); it != g_sent.end(); )
			it = now - it->second.Frame > 7200 ? g_sent.erase(it) : std::next(it);
	}
}

void GarrisonFire::Reset()
{
	g_sent.clear();
	g_lastSweep = -1;
	g_counts = Counts{};
}

// TechnoClass::GetFireError, past the "no target" and "already firing at it" checks: ESI = this,
// EBX = the target. An AI shooter, an empty neutral garrisonable building: an occupier that might go in
// waits (REARM, as 0x6FC0DF: it keeps the target for the pass to send it in), anything else may not fire
// (ILLEGAL, as 0x6FC86A: the engine drops the target). Both exits pop the same frame.
DEFINE_HOOK(0x6FC105, DoctrineExt_TechnoClass_GetFireError_GarrisonHoldFire, 0xA)
{
	enum { Rearm = 0x6FC0DF, Illegal = 0x6FC86A };
	GET(TechnoClass* const, pThis, ESI);
	GET(AbstractClass* const, pTarget, EBX);
	auto const pBld = Garrisonable(pTarget);
	if (!pBld)
		return 0;
	auto const pCfg = HoldsFire(pThis, pBld);
	if (!pCfg)
		return 0;
	return MightEnter(*pCfg, pThis, pBld) ? Rearm : Illegal;
}
