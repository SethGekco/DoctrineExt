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

	// Move the house's DefendBase teams to its learned outer edge (perimeter),
	// fanned toward the nearest enemy, so they hold the edge not the centre.
	void SteerDefenders(HouseClass* pHouse);

	// True if the house has at least one live DefendBase team.
	bool HasActiveDefend(HouseClass* pHouse);

	// Steer the house's HuntTarget teams onto pTarget's weak side (away from
	// its supporting units, avoiding our death zones) — the 6c approach.
	void SteerHunters(HouseClass* pHouse, TechnoClass* pTarget);

	// True if the house has at least one live HuntTarget team.
	bool HasActiveHunt(HouseClass* pHouse);

	// True if the house has at least one live Decapitate team.
	bool HasActiveDecap(HouseClass* pHouse);

	// Steer Decapitate teams at the weakest enemy's current priority structure,
	// re-acquiring it each tick so they walk down the rebuild chain (§10h).
	void SteerDecap(HouseClass* pHouse);

	// Count the house's IDLE ARMED units — combat units on no team (hoarded).
	int CountIdleArmed(HouseClass* pHouse);

	// Diagnostic (DebugTicks): log any combat unit type this house OWNS that it
	// couldn't legitimately build now (CanBuild says no, or TechLevel over max)
	// — confirms whether the base AI acquires prereq/TechLevel-illegal units.
	void PrereqAudit(HouseClass* pHouse);

	// Base-flood relief (§10d C): when the house hoards more than FloodThreshold
	// idle armed units, send a fraction to Hunt so they stop clogging the base.
	// Self-throttled (FloodCooldown); opt-in (FloodThreshold=0 disables).
	void FloodResponse(HouseClass* pHouse);

	// Garrison doctrine (§10e): while vacant occupiable buildings exist, keep
	// building Occupier infantry and send idle occupiers to enter them via the
	// engine's own garrison flags (so they enter, not mistakenly attack).
	void GarrisonDoctrine(HouseClass* pHouse);

	// Crate doctrine (§10i): send a fast grabber at nearby crates, and — when
	// stuck with no route to an MCV in a long game — firesale to grab the
	// guaranteed FreeMCV crate. Self-throttled; opt-in.
	void CrateDoctrine(HouseClass* pHouse);

	// True if the house has a live crate-grab team.
	bool HasActiveCrate(HouseClass* pHouse);

	// Drive the crate-grab team onto the nearest crate each tick (a loose Move
	// gets countermanded by the base AI; a steered team member sticks).
	void SteerCrate(HouseClass* pHouse);

	// Persistent crate squad (§10i redesign): a dedicated squad, sized from
	// [CrateRules], that stands by spread out and races each nearby crate with
	// its closest member. Maintains its own membership (diverting units off AI
	// teams) and never repurposes them. Self-throttled; opt-in (CrateSquad).
	void CrateSquadDoctrine(HouseClass* pHouse);

	// Re-issue each squad chaser's Move to its cached crate EVERY base tick (the
	// detection pass is throttled to 90f; a once-per-90f order gets countermanded
	// by the base AI before the unit arrives). Cheap: no scan, just cached cells.
	void SteerCrateSquad(HouseClass* pHouse);

	// Water crate squad: mirror of the ground squad for crates on water cells,
	// using naval/amphibious/hover units, on a separate DCRW team.
	void CrateWaterSquad(HouseClass* pHouse);
	void SteerCrateWaterSquad(HouseClass* pHouse);

	// Air-defense (AA P1): keep own anti-air firepower >= enemy air DPS x ratio,
	// building AntiAir arsenal units when short (scales as the enemy adds air), and
	// screen AA forward of base split across incoming-air bearings.
	void AirDefense(HouseClass* pHouse);

	// Reposition drifted AA screen units each tick (placement assigned by
	// AirDefense); leaves in-position units alone so they fire.
	void SteerAirDefense(HouseClass* pHouse);

	// Naval doctrine: when there's water near base and the prereqs are met, force
	// the Naval Yard the base AI won't build, then field navy to a target. Also
	// gives the water crate squad units to divert. Self-throttled; opt-in.
	void NavalDoctrine(HouseClass* pHouse);

	// Commander takeover (§10 commander): if the AI has stayed non-aggressive (army
	// home, not committed at the enemy) for CommanderIdleTime frames, commandeer the
	// idle army (diverting units off their sitting aimd teams) into an assault team.
	// Self-throttled; opt-in (CommanderIdleTime=0 off).
	void CommanderTakeover(HouseClass* pHouse);

	// Drive the commandeered assault team at the enemy's priority building every
	// base tick (so the base AI can't countermand it), like SteerDecap.
	void SteerCommander(HouseClass* pHouse);

	// Reserve spender (§10f): while the house's cash exceeds ReserveAmount,
	// build the next affordable item from ReserveBuild — spend surplus instead
	// of hoarding. Self-throttled (ReserveCooldown); opt-in (Amount=0 off).
	void ReserveSpend(HouseClass* pHouse);

	// Force-comparison rush (§10g): when combined allied power dominates the
	// weakest enemy (and won't overextend), commit all idle armed units to an
	// all-in Hunt. Allied AIs converge on the same target deterministically
	// (silent coordination). Self-throttled; opt-in (RushRatio=0 off).
	void RushCheck(HouseClass* pHouse);

	void Reset();
}
