#pragma once

class HouseClass;
class TechnoClass;

// Death-zone heatmap (DESIGN.md §6.2) — sensing system #2.
//
// A decaying, coarse grid per AI house of WHERE THAT HOUSE LOSES ITS UNITS.
// Clusters mark killboxes: a spot where the enemy camps and farms the AI's
// swarms. Combined with kill tracking (the farmer is a high-kill enemy unit
// sitting in the cluster) this tells the AI "solve this carefully / stop
// feeding it," and later (6c) "route around it / approach the weak side."
//
// Event-driven: fed from the RegisterDestruction hook KillTracker already owns
// (no new hook). Pointer-safe (only integers keyed by house index) and
// sync-safe (integer accumulators, deterministic decay, no RNG).
namespace DeathZones
{
	// Record a loss: if pVictim belongs to an AI consumer house, bump that
	// house's grid at the death location. Called from the kill hook.
	void Record(TechnoClass* pVictim);

	// Decay every house's grid on a slow schedule (self-throttled to
	// DeathZoneDecayInterval). Called from the base tick.
	void Decay(int frame);

	// Bearing (radians) from the base center toward the hottest death cluster
	// within DeathZoneRadius, and its strength. False if none clears
	// DeathZoneMinStrength.
	bool HottestBearing(HouseClass* pHouse, double& outAngle, int& outStrength);

	// Death-zone accumulation in the bucket containing a map cell (0 if none) —
	// used by 6c to judge how dangerous a route/approach point is.
	int ScoreAtCell(HouseClass* pHouse, int cellX, int cellY);

	// Max death-zone value within DeathZoneRadius of the base (the
	// DeathZoneScore observation).
	int MaxNear(HouseClass* pHouse);

	void Reset();
}
