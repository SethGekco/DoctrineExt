#pragma once

// Garrison hold-fire ([Doctrine.Garrison] HoldFire). AI units never shoot an empty neutral building they could
// garrison. The engine lets AI Occupiers take one as a target so they will go in, but only Mission_Hunt and
// Mission_Attack turn that target into the garrison order (SetDestination + Capture, 0x51F59A); a guarding
// occupier, one whose building filled up first, or a unit given the building by its team shot it down instead.
//
// GetFireError (0x6FC105) refuses the shot: an AI Occupier that may enter waits (FireError::REARM keeps the
// target), anything else is ILLEGAL (the engine drops the target). A pass then sends the occupier in the
// engine's way, or drops the target.
//
// Only AI-owned shooters; the human's are never touched. Sync-safe: synced state only, units by UniqueID.
// The GetFireError hook can run on PerfExt's target-scan worker threads: it only reads.
namespace GarrisonFire
{
	// Every frame from the house tick (once per frame however many houses tick); each unit is looked at
	// every HoldFire.Interval frames, staggered by its UniqueID.
	void Sweep();

	void Reset();
}
