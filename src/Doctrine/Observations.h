#pragma once

#include <string>

class HouseClass;
class TechnoClass;

// The sensing layer (DESIGN.md §2/§4). Vocabulary:
//   OwnerZoneThreatAir / OwnerZoneThreatArmor / OwnerZoneThreatInfantry
//     — the engine's own live per-zone base-threat estimate, summed
//       (HouseClass::ZoneInfos[5]; same read AITriggerTypeExt proved).
//   EnemyAirDPS
//     — summed weapon DPS of all aircraft owned by every enemy of the
//       observing house (the DPS math ported from AITriggerTypeExt).
//   EnemyUnitKills
//     — kill count of the deadliest living enemy unit (KillTracker);
//       identifies that unit as the observation's target, so rules can say
//       Target=ThatUnit.
namespace Observations
{
	// Evaluate a named observation for the given house. Returns false if the
	// name is unknown (caller warns once per name). Observations that point
	// at a specific object set *outTarget when the caller provides one.
	bool Get(HouseClass* pOwner, const std::string& name, double& outValue,
		TechnoClass** outTarget = nullptr);
}
