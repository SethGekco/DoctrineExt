#pragma once

class WarheadTypeClass;

// A warhead's damage multiplier against an armor, as this mod's rules give it. Ares (Antares) keeps Verses in
// its own table and skips the engine's parser (its Verses_OrigParser hook at 0x75DDCC), so
// WarheadTypeClass::Verses stays at 100% for every armor. This reads them the Ares way from rulesmd.ini:
// Verses= (the 11 vanilla armors), then Versus.<armor>= for any armor, and [ArmorTypes] (custom armors from
// index 11 on, in order: a default percentage, or another armor whose value they take).
// Main thread only (caches per warhead; Reset per scenario).
namespace Verses
{
	// armor = TechnoTypeClass::Armor (custom armors included). 1.0 when the armor is unknown.
	double Of(WarheadTypeClass* pWarhead, int armor);

	void Reset();
}
