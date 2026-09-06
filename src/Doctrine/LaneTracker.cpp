#include "Doctrine/LaneTracker.h"
#include "Doctrine/Config.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <CellClass.h>
#include <MapClass.h>
#include <Utilities/Debug.h>

#include <map>
#include <vector>
#include <cmath>

namespace
{
	struct Grid
	{
		int Left = 0, Top = 0, W = 0, H = 0, Bucket = 0;
		std::vector<int> Cells; // W*H, enemy-traffic accumulator
		int Index(int cellX, int cellY) const
		{
			int const bx = (cellX - Left) / Bucket;
			int const by = (cellY - Top) / Bucket;
			if (bx < 0 || by < 0 || bx >= W || by >= H) return -1;
			return by * W + bx;
		}
	};

	// One grid per AI house index (the consumers). Rebuilt lazily to map size.
	std::map<int, Grid> g_grids;
	int g_lastSample = -1;

	int Bucket() { return DoctrineConfig::Instance.LaneBucket > 0 ? DoctrineConfig::Instance.LaneBucket : 6; }

	Grid& GridFor(int houseIdx)
	{
		auto& g = g_grids[houseIdx];
		if (g.Cells.empty())
		{
			auto const& b = MapClass::Instance.MapCoordBounds;
			g.Bucket = Bucket();
			g.Left = b.Left;
			g.Top = b.Top;
			g.W = (b.Right - b.Left) / g.Bucket + 1;
			g.H = (b.Bottom - b.Top) / g.Bucket + 1;
			if (g.W < 1) g.W = 1;
			if (g.H < 1) g.H = 1;
			g.Cells.assign(static_cast<size_t>(g.W) * g.H, 0);
		}
		return g;
	}

	bool IsConsumer(HouseClass* const pHouse)
	{
		return pHouse && !pHouse->Defeated && !pHouse->IsControlledByHuman()
			&& !pHouse->IsObserver() && !pHouse->IsNeutral();
	}

	bool IsMobile(TechnoClass* const pTechno)
	{
		auto const what = pTechno->WhatAmI();
		return what == AbstractType::Infantry || what == AbstractType::Unit
			|| what == AbstractType::Aircraft;
	}
}

void LaneTracker::Sample(int frame)
{
	auto const& cfg = DoctrineConfig::Instance;
	int const interval = cfg.LaneSampleInterval > 0 ? cfg.LaneSampleInterval : 60;
	if (g_lastSample >= 0 && frame - g_lastSample < interval)
		return;
	g_lastSample = frame;

	// Collect the consumer AI houses once.
	std::vector<HouseClass*> consumers;
	for (int i = 0; i < HouseClass::Array.Count; ++i)
	{
		auto const pH = HouseClass::Array.GetItem(i);
		if (IsConsumer(pH))
			consumers.push_back(pH);
	}
	if (consumers.empty())
		return;

	// Decay every consumer's grid (lose 1/(2^shift) each sample).
	int const shift = cfg.LaneDecayShift > 0 ? cfg.LaneDecayShift : 4;
	for (auto pH : consumers)
	{
		auto& g = GridFor(pH->ArrayIndex);
		for (auto& v : g.Cells)
			if (v) v -= (v >> shift) + 1; // +1 so small values still fade to 0
	}

	int const weight = cfg.LaneBumpWeight > 0 ? cfg.LaneBumpWeight : 16;

	// Accumulate enemy movement: each moving mobile unit bumps the grid of
	// every consumer house hostile to its owner.
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pTechno = TechnoClass::Array.GetItem(i);
		if (!pTechno || pTechno->InLimbo || pTechno->Health <= 0) continue;
		if (!IsMobile(pTechno)) continue;
		auto const pFoot = static_cast<FootClass*>(pTechno);
		if (!pFoot->Destination) continue; // only units actually in transit
		auto const pOwner = pTechno->Owner;
		if (!pOwner) continue;

		CellStruct cell;
		pTechno->GetMapCoords(&cell);

		for (auto pH : consumers)
		{
			if (pH == pOwner || pH->IsAlliedWith(pOwner)) continue;
			auto& g = GridFor(pH->ArrayIndex);
			int const idx = g.Index(cell.X, cell.Y);
			if (idx >= 0)
				g.Cells[idx] += weight;
		}
	}
}

bool LaneTracker::HottestLaneBearing(HouseClass* pHouse, double& outAngle, int& outStrength)
{
	auto const it = g_grids.find(pHouse->ArrayIndex);
	if (it == g_grids.end()) return false;
	auto const& g = it->second;
	if (g.Cells.empty()) return false;

	auto const& base = pHouse->GetBaseCenter();
	int const radiusCells = DoctrineConfig::Instance.LaneRadius > 0
		? DoctrineConfig::Instance.LaneRadius : 30;
	int const rBuckets = radiusCells / g.Bucket + 1;
	int const baseBx = (base.X - g.Left) / g.Bucket;
	int const baseBy = (base.Y - g.Top) / g.Bucket;

	int best = 0, bestBx = -1, bestBy = -1;
	for (int by = baseBy - rBuckets; by <= baseBy + rBuckets; ++by)
	{
		if (by < 0 || by >= g.H) continue;
		for (int bx = baseBx - rBuckets; bx <= baseBx + rBuckets; ++bx)
		{
			if (bx < 0 || bx >= g.W) continue;
			int const v = g.Cells[by * g.W + bx];
			if (v > best) { best = v; bestBx = bx; bestBy = by; }
		}
	}

	int const minStrength = DoctrineConfig::Instance.LaneMinStrength > 0
		? DoctrineConfig::Instance.LaneMinStrength : 32;
	if (best < minStrength || bestBx < 0)
		return false;

	// Bucket center cell -> bearing from base center.
	int const cellX = g.Left + bestBx * g.Bucket + g.Bucket / 2;
	int const cellY = g.Top + bestBy * g.Bucket + g.Bucket / 2;
	outAngle = std::atan2(double(cellY - base.Y), double(cellX - base.X));
	outStrength = best;
	return true;
}

int LaneTracker::MaxLaneNear(HouseClass* pHouse)
{
	double a; int s = 0;
	return HottestLaneBearing(pHouse, a, s) ? s : 0;
}

void LaneTracker::Reset()
{
	g_grids.clear();
	g_lastSample = -1;
}
