#pragma once

class HouseClass;

// The decide step: runs each rule for each eligible AI house on the sense
// tick (staggered so houses don't all evaluate on the same frame).
namespace Engine
{
	void TickHouse(HouseClass* pHouse);
	void Reset();
}
