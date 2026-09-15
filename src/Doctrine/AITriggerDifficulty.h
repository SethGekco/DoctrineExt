#pragma once

// Per-difficulty AITriggerType remapping (Rex, 2026-09-15).
//
// Each AITriggerType carries three trailing enable flags (easy/normal/hard).
// The engine picks ONE by the house's AIDifficulty — which is INVERTED
// (Hard==0, Easy==2, per the YRpp comment). This lets the modder redefine, per
// rulesmd difficulty section, WHICH flag index(es) that difficulty inherits:
//
//   [Easy]      AITriggerTypes.Inherit=2      ; vanilla (Easy section = the
//   [Normal]    AITriggerTypes.Inherit=1      ;  hard AI, uses the hard flag)
//   [Difficult] AITriggerTypes.Inherit=0
//
//   [Easy]      AITriggerTypes.Inherit=2,1    ; union: gets hard AND normal
//   [Difficult] AITriggerTypes.Inherit=-1     ; nothing (dead AI)
//
// Inherit values: 0/1/2 = the trigger's easy/normal/hard flag; >=3 = a named
// bundle from aimd [DifficultyTypes] whose triggers are listed explicitly in
// [<Name>.AITriggerTypes]; -1 = none. The union of the inherited sources is
// written back onto the flag that difficulty reads — so the engine's own check
// then produces the mapping (no evaluation hook, no AITriggerTypeExt conflict).
//
// Implemented by REWRITING the flags once per scenario, sync-safe (deterministic
// read of the same INIs on every client).
namespace AITriggerDifficulty
{
	// Parse the config and rewrite the flags, once per scenario. Called from the
	// base tick (guaranteed after AITriggerTypes have loaded). No-op if no
	// difficulty section defines AITriggerTypes.Inherit=.
	void EnsureApplied();

	void Reset();
}
