#include "Doctrine/DeathZones.h"
#include "Doctrine/Config.h"

#include <HouseClass.h>
#include <TechnoClass.h>
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
		std::vector<int> Cells;
		int Index(int cellX, int cellY) const
		{
			int const bx = (cellX - Left) / Bucket;
			int const by = (cellY - Top) / Bucket;
			if (bx < 0 || by < 0 || bx >= W || by >= H) return -1;
			return by * W + bx;
		}
	};

	std::map<int, Grid> g_grids;
	int g_lastDecay = -1;

	int Bucket() { return DoctrineConfig::Instance.DeathZoneBucket > 0 ? DoctrineConfig::Instance.DeathZoneBucket : 6; }

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

	const Grid* FindGrid(int houseIdx)
	{
		auto const it = g_grids.find(houseIdx);
		return (it != g_grids.end() && !it->second.Cells.empty()) ? &it->second : nullptr;
	}

	// Hottest bucket within radius of the base; returns strength + its center.
	int Hottest(const Grid& g, const CellStruct& base, int radiusCells, int& outCellX, int& outCellY)
	{
		int const rB = radiusCells / g.Bucket + 1;
		int const baseBx = (base.X - g.Left) / g.Bucket;
		int const baseBy = (base.Y - g.Top) / g.Bucket;
		int best = 0; outCellX = outCellY = -1;
		for (int by = baseBy - rB; by <= baseBy + rB; ++by)
		{
			if (by < 0 || by >= g.H) continue;
			for (int bx = baseBx - rB; bx <= baseBx + rB; ++bx)
			{
				if (bx < 0 || bx >= g.W) continue;
				int const v = g.Cells[by * g.W + bx];
				if (v > best)
				{
					best = v;
					outCellX = g.Left + bx * g.Bucket + g.Bucket / 2;
					outCellY = g.Top + by * g.Bucket + g.Bucket / 2;
				}
			}
		}
		return best;
	}
}

void DeathZones::Record(TechnoClass* pVictim)
{
	if (!pVictim) return;
	auto const pOwner = pVictim->Owner;
	if (!IsConsumer(pOwner)) return;

	CellStruct cell;
	pVictim->GetMapCoords(&cell);
	auto& g = GridFor(pOwner->ArrayIndex);
	int const idx = g.Index(cell.X, cell.Y);
	if (idx >= 0)
	{
		int const weight = DoctrineConfig::Instance.DeathZoneBumpWeight > 0
			? DoctrineConfig::Instance.DeathZoneBumpWeight : 64;
		g.Cells[idx] += weight;
	}
}

void DeathZones::Decay(int frame)
{
	auto const& cfg = DoctrineConfig::Instance;
	int const interval = cfg.DeathZoneDecayInterval > 0 ? cfg.DeathZoneDecayInterval : 150;
	if (g_lastDecay >= 0 && frame - g_lastDecay < interval)
		return;
	g_lastDecay = frame;

	int const shift = cfg.DeathZoneDecayShift > 0 ? cfg.DeathZoneDecayShift : 3;
	for (auto& [idx, g] : g_grids)
		for (auto& v : g.Cells)
			if (v) v -= (v >> shift) + 1;
}

bool DeathZones::HottestBearing(HouseClass* pHouse, double& outAngle, int& outStrength)
{
	auto const pG = FindGrid(pHouse->ArrayIndex);
	if (!pG) return false;
	auto const& base = pHouse->GetBaseCenter();
	int const radius = DoctrineConfig::Instance.DeathZoneRadius > 0
		? DoctrineConfig::Instance.DeathZoneRadius : 30;
	int cx, cy;
	int const best = Hottest(*pG, base, radius, cx, cy);
	int const minStrength = DoctrineConfig::Instance.DeathZoneMinStrength > 0
		? DoctrineConfig::Instance.DeathZoneMinStrength : 48;
	if (best < minStrength || cx < 0) return false;
	outAngle = std::atan2(double(cy - base.Y), double(cx - base.X));
	outStrength = best;
	return true;
}

int DeathZones::ScoreAtCell(HouseClass* pHouse, int cellX, int cellY)
{
	auto const pG = FindGrid(pHouse->ArrayIndex);
	if (!pG) return 0;
	int const idx = pG->Index(cellX, cellY);
	return idx >= 0 ? pG->Cells[idx] : 0;
}

int DeathZones::MaxNear(HouseClass* pHouse)
{
	auto const pG = FindGrid(pHouse->ArrayIndex);
	if (!pG) return 0;
	auto const& base = pHouse->GetBaseCenter();
	int const radius = DoctrineConfig::Instance.DeathZoneRadius > 0
		? DoctrineConfig::Instance.DeathZoneRadius : 30;
	int cx, cy;
	return Hottest(*pG, base, radius, cx, cy);
}

void DeathZones::Reset()
{
	g_grids.clear();
	g_lastDecay = -1;
}
