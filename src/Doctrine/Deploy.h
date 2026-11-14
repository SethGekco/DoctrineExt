#pragma once

// AI deploy doctrine ([Doctrine.Deploy]). AI infantry that fight better deployed (GI, Guardian GI) deploy to
// fight the targets their deployed weapon does more to (the weapons' Verses for the target's armor), stand up
// for the targets the undeployed weapon does more to, keep fighting deployed while an enemy is in range, and
// deploy for aircraft only the deployed weapon can hit (the Guardian GI's missiles).
//
// The engine's own AI deploy check sits in InfantryClass::ApproachTarget behind an early return that skips it
// (Phobos InfantryAutoDeploy restores it, for every target alike), so in a fight AI GIs only ever deployed while
// standing in Guard. Two engine sites decide it the engine's way: ApproachTarget (0x522373, deploy once the
// target is in the deployed weapon's range) and Mission_Hunt (0x51F5E9, a deployed hunter whose target is gone
// takes the next one in range before it stands up). A pass over AI infantry covers the rest: a target that
// came into range, aircraft, standing up.
//
// Only AI-owned infantry; the human's are never touched. Sync-safe: synced state only, units by UniqueID.
namespace Deploy
{
	// Every frame from the house tick (once per frame however many houses tick); each unit is looked at
	// every [Doctrine.Deploy] Interval frames, staggered by its UniqueID.
	void Sweep();

	void Reset();
}
