#pragma once

#include <string>

class HouseClass;

// The sensing layer (DESIGN.md §2/§4). Phase 1 vocabulary:
//   OwnerZoneThreatAir / OwnerZoneThreatArmor / OwnerZoneThreatInfantry
//     — the engine's own live per-zone base-threat estimate, summed
//       (HouseClass::ZoneInfos[5]; same read AITriggerTypeExt proved).
//   EnemyAirDPS
//     — summed weapon DPS of all aircraft owned by every enemy of the
//       observing house (the DPS math ported from AITriggerTypeExt).
namespace Observations
{
	// Evaluate a named observation for the given house. Returns false if the
	// name is unknown (caller warns once per name).
	bool Get(HouseClass* pOwner, const std::string& name, double& outValue);
}
