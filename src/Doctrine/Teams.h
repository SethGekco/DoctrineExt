#pragma once

class HouseClass;
struct DoctrineRule;

// The act step (DESIGN.md §7): a small pool of DLL-owned TeamType/TaskForce/
// ScriptType trios, rewritten per dispatch. Pool objects are looked up by ID
// on every use (never cached across frames) so scenario clears and savegame
// loads can't leave dangling pointers.
namespace Teams
{
	// Build and dispatch a procedural team answering `rule` for `pHouse`.
	// obsValue is the observation reading that fired the rule (sizes the
	// force). Returns true if a team was created.
	bool Dispatch(HouseClass* pHouse, const DoctrineRule& rule, double obsValue);

	void Reset();
}
