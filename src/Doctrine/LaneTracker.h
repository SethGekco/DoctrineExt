#pragma once

class HouseClass;

// Travel-lane heatmap (DESIGN.md §6.3) — sensing system #3.
//
// A decaying, coarse grid of ENEMY movement, maintained per AI house: where do
// the units hostile to this house actually travel? High-scoring buckets are the
// approach lanes / chokepoints the enemy really uses, so defenders can hold and
// interceptors can ambush where raids come from — not merely "toward the enemy
// base" geometrically.
//
// Pointer-safe by construction: stores only integers keyed by house index, and
// re-reads live units each sample, so it never holds a raw TechnoClass* (no
// dangling-pointer class — cf. the KillTracker crash).
//
// Sync-safe: integer accumulators, deterministic iteration of the synced unit
// and house arrays, no wall-clock, no RNG.
namespace LaneTracker
{
	// Sample the map's movement into the per-house grids and decay them. Called
	// on the base tick; self-throttles to LaneSampleInterval frames.
	void Sample(int frame);

	// Bearing (radians, atan2 convention) from the house's base center toward
	// the hottest enemy-traffic bucket within LaneRadius cells, and its
	// strength. Returns false if no bucket clears LaneMinStrength (caller then
	// falls back to a geometric heuristic).
	bool HottestLaneBearing(HouseClass* pHouse, double& outAngle, int& outStrength);

	// Max enemy-traffic bucket value within LaneRadius of the base (the
	// LaneTraffic observation).
	int MaxLaneNear(HouseClass* pHouse);

	void Reset();
}
