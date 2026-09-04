#pragma once

class HouseClass;
class TechnoClass;
struct DoctrineRule;

// The act step (DESIGN.md §7): a small pool of DLL-owned TeamType/TaskForce/
// ScriptType trios, rewritten per dispatch. Pool objects are looked up by ID
// on every use (never cached across frames) so scenario clears and savegame
// loads can't leave dangling pointers.
namespace Teams
{
	// Build and dispatch a procedural team answering `rule` for `pHouse`.
	// obsValue is the observation reading that fired the rule (sizes the
	// force); pTarget is the object the observation identified, if any
	// (rules with Target=ThatUnit bind the mission to it). Returns true if
	// a team was created.
	bool Dispatch(HouseClass* pHouse, const DoctrineRule& rule, double obsValue,
		TechnoClass* pTarget);

	// True if the house has at least one live intercept team (so the caller
	// can skip the raider scan for houses that aren't intercepting).
	bool HasActiveIntercept(HouseClass* pHouse);

	// Log each live doctrine team's current member count (under DebugTicks),
	// so we can see engine-side recruiting fill produced units in over time —
	// the dispatch-moment count only sees units that already existed.
	void LogTeamFill(HouseClass* pHouse);

	// Re-point every live intercept team of the house at the current raider,
	// so interceptors keep moving toward the raid as it moves (the visible
	// pursuit) instead of parking where they were first dispatched.
	void SteerIntercepts(HouseClass* pHouse, TechnoClass* pRaider);

	void Reset();
}
