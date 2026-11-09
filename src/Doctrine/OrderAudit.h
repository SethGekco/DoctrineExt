#pragma once

class HouseClass;

// The order-churn audit ([Doctrine.General] DebugOrders): finds AI units whose orders bounce between
// sources — DoctrineExt steps and the engine (team scripts, the base AI) taking turns at the same unit,
// which shows in game as units that start one thing, turn, start another (Rex 11-08: "GIs being
// indecisive ... like they're given multiple orders at the same time"). Each house tick snapshots the
// house's infantry and vehicles first (what changed since its last tick, the engine did), then after
// each DoctrineExt step (what changed, that step did). A unit changed 6+ times in 300 frames by two or
// more sources, each at least twice, is logged ("orders churn:") with who changed what.
namespace OrderAudit
{
	// Start of the house's tick: credit the engine with changes since the last snapshot.
	void Begin(HouseClass* pHouse);

	// After one DoctrineExt step: credit `step` (a string literal) with the changes it made.
	void After(HouseClass* pHouse, const char* step);

	void Reset();
}
