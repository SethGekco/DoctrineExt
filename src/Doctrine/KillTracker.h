#pragma once

class HouseClass;
class TechnoClass;

// The kill-tracking sensor (DESIGN.md §6.1): credits every registered kill to
// the destroying unit, so rules can react to enemy units racking up kills
// ("aces"). Fed by a hook at the entry of TechnoClass::RegisterDestruction
// (0x702D40) — the single funnel every kill path goes through (UnitClass's
// override calls into it; verified by disassembly, sole caller 0x744790).
namespace KillTracker
{
	struct Ace
	{
		TechnoClass* Unit = nullptr;
		int Kills = 0;
		int ValueKilled = 0;
	};

	// The top-killing living unit owned by an enemy of pOwner. Kills == 0
	// means no tracked enemy killer exists. Deterministic across clients:
	// ranked by kills, then value killed, then the synced AbstractClass
	// UniqueID — never by pointer order.
	Ace TopEnemyAce(HouseClass* pOwner);

	void Reset();
}
