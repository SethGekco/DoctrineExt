#pragma once

#include "Doctrine/Config.h"
#include <string>

// Runtime arsenal auto-grader (DESIGN.md §10d A) — ports the wave-gen tool's
// role-weighting into the DLL. At first use it grades every combat unit type by
// the engine's real stats (armor-adjusted DPS vs infantry/vehicle/air/building,
// range, speed) and builds best-first role lists (AntiAir, AntiArmor,
// AntiInfantry, Siege, AceHunter, Scout).
//
// The lists are GLOBAL (all factions' units); per-house faction/build filtering
// still happens at pick time (StrictOwnership + CanBuild), so each house draws
// its own specialists — which is exactly what removes the need to hand-curate a
// faction-correct [Doctrine.Arsenal] per role.
namespace ArsenalGrader
{
	// Auto-graded list for a standard role name, or nullptr if not a known role.
	// Builds lazily on first call (needs the type arrays loaded).
	const DoctrineArsenalRole* RoleUnits(const std::string& role);

	void Reset();
}
