#include "Doctrine/Observations.h"

#include <HouseClass.h>
#include <AircraftTypeClass.h>
#include <WeaponTypeClass.h>
#include <BulletTypeClass.h>

namespace
{
	enum class ZoneKind { Air, Armor, Infantry };

	// Sum the owner's per-zone threat of one kind across all 5 zones.
	// ZoneInfos is the engine's own live estimate of the air/armor/infantry
	// threat to each zone of the house's base.
	double SumZoneThreat(HouseClass* const pHouse, ZoneKind const kind)
	{
		int total = 0;
		for (int i = 0; i < 5; ++i)
		{
			auto const& z = pHouse->ZoneInfos[i];
			switch (kind)
			{
			case ZoneKind::Air:      total += z.Aircraft; break;
			case ZoneKind::Armor:    total += z.Armor;    break;
			case ZoneKind::Infantry: total += z.Infantry; break;
			}
		}
		return static_cast<double>(total);
	}

	// Raw combat DPS of one type: weapons 0 and 1, Damage>1 (skips
	// detector/utility weapons), burst included. Ported from
	// AITriggerTypeExt::ComputeHouseDPS.
	double TypeDPS(TechnoTypeClass* const pType)
	{
		double total = 0.0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pType->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType) continue;
			auto const w = pWS->WeaponType;
			if (w->ROF <= 0 || w->Damage <= 1) continue;
			int const burst = w->Burst > 0 ? w->Burst : 1;
			total += static_cast<double>(w->Damage) * burst / (w->ROF / 10.0);
		}
		return total;
	}

	// Summed DPS of every aircraft owned by every enemy of pOwner.
	double EnemyAirDPS(HouseClass* const pOwner)
	{
		double total = 0.0;
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto const pOther = HouseClass::Array.GetItem(i);
			if (!pOther || pOther == pOwner) continue;
			if (pOther->Defeated || pOther->IsObserver() || pOther->IsNeutral()) continue;
			if (pOwner->IsAlliedWith(pOther)) continue;

			for (auto const pType : AircraftTypeClass::Array)
			{
				int const count = pOther->CountOwnedAndPresent(pType);
				if (count > 0)
					total += TypeDPS(pType) * count;
			}
		}
		return total;
	}
}

bool Observations::Get(HouseClass* pOwner, const std::string& name, double& outValue)
{
	if (!pOwner) return false;

	if (name == "OwnerZoneThreatAir")      { outValue = SumZoneThreat(pOwner, ZoneKind::Air);      return true; }
	if (name == "OwnerZoneThreatArmor")    { outValue = SumZoneThreat(pOwner, ZoneKind::Armor);    return true; }
	if (name == "OwnerZoneThreatInfantry") { outValue = SumZoneThreat(pOwner, ZoneKind::Infantry); return true; }
	if (name == "EnemyAirDPS")             { outValue = EnemyAirDPS(pOwner);                       return true; }

	return false;
}
