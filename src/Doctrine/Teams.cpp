#include "Doctrine/Teams.h"
#include "Doctrine/Config.h"
#include "Doctrine/LaneTracker.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <TeamClass.h>
#include <TeamTypeClass.h>
#include <TaskForceClass.h>
#include <ScriptTypeClass.h>
#include <TechnoTypeClass.h>
#include <FactoryClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <BuildingClass.h>
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <AircraftTypeClass.h>
#include <Memory.h>
#include <Fundamentals.h>
#include <GeneralDefinitions.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace
{
	// One warning per distinct message per scenario, so a misconfigured rule
	// doesn't flood the log at tick rate.
	std::set<std::string> g_warned;
	void WarnOnce(const std::string& msg)
	{
		if (g_warned.insert(msg).second)
			Debug::Log("[DoctrineExt] WARNING: %s\n", msg.c_str());
	}

	int CountOwned(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		switch (pType->WhatAmI())
		{
		case AbstractType::InfantryType:
			return pHouse->CountOwnedAndPresent(static_cast<InfantryTypeClass*>(pType));
		case AbstractType::UnitType:
			return pHouse->CountOwnedAndPresent(static_cast<UnitTypeClass*>(pType));
		case AbstractType::AircraftType:
			return pHouse->CountOwnedAndPresent(static_cast<AircraftTypeClass*>(pType));
		default:
			return 0;
		}
	}

	bool IsTeamable(TechnoTypeClass* const pType)
	{
		auto const what = pType->WhatAmI();
		return what == AbstractType::InfantryType
			|| what == AbstractType::UnitType
			|| what == AbstractType::AircraftType;
	}

	// The OBJECT abstract a TechnoType produces (UnitType -> Unit, etc.).
	AbstractType ProducedObjectType(TechnoTypeClass* const pType)
	{
		switch (pType->WhatAmI())
		{
		case AbstractType::UnitType:     return AbstractType::Unit;
		case AbstractType::InfantryType: return AbstractType::Infantry;
		case AbstractType::AircraftType: return AbstractType::Aircraft;
		default:                         return AbstractType::None;
		}
	}

	// Find a factory of the house that can take a production demand for pType.
	// GetPrimaryFactory alone is unreliable for the AI (the "primary" flag is a
	// human-sidebar concept, so a $480k AI with a war factory returned null and
	// nothing was ever queued). Fall back to a factory already building this
	// type, then to any of the house's factories currently producing the same
	// object category (its own army) — DemandProduction(...,queue) appends to
	// that queue.
	FactoryClass* FindHouseFactory(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (auto const f = pHouse->GetPrimaryFactory(
			pType->WhatAmI(), pType->Naval, BuildCat::DontCare))
			return f;
		if (auto const f = FactoryClass::FindByOwnerAndProduct(pHouse, pType))
			return f;

		auto const objAbs = ProducedObjectType(pType);
		if (objAbs == AbstractType::None)
			return nullptr;
		for (auto const pFact : FactoryClass::Array)
		{
			if (pFact->Owner != pHouse) continue;
			if (pFact->Object && pFact->Object->WhatAmI() == objAbs)
				return pFact;
		}
		return nullptr;
	}

	// Walk the role's list best-first; take the first type the house can
	// build right now or already owns units of.
	TechnoTypeClass* PickType(HouseClass* const pHouse, const DoctrineArsenalRole& role)
	{
		for (auto const& id : role.Units)
		{
			auto const pType = TechnoTypeClass::Find(id.c_str());
			if (!pType)
			{
				WarnOnce("arsenal " + role.Role + ": unknown unit ID " + id);
				continue;
			}
			if (!IsTeamable(pType))
			{
				WarnOnce("arsenal " + role.Role + ": " + id + " is not a mobile unit, skipped");
				continue;
			}
			auto const canBuild = pHouse->CanBuild(pType, false, true);
			int const owned = CountOwned(pHouse, pType);
			// Live tests showed CanBuild returning Buildable cross-faction in
			// this modded stack (an Allied AI picked and built Flak Tracks),
			// so the build path re-checks Owner= itself unless the modder
			// opts out. Units the house physically owns are always usable.
			bool const ownerOK = !DoctrineConfig::Instance.StrictOwnership
				|| pHouse->InOwners(pType);
			if (DoctrineConfig::Instance.DebugTicks)
				Debug::Log("[DoctrineExt]   candidate %s for %s#%d: CanBuild=%d inOwners=%d owned=%d\n",
					id.c_str(), pHouse->get_ID(), pHouse->ArrayIndex,
					static_cast<int>(canBuild), ownerOK, owned);
			if ((canBuild == CanBuildResult::Buildable && ownerOK) || owned > 0)
				return pType;
		}
		return nullptr;
	}

	// The pool is per house: a global 4-slot pool starved ~25 AI houses in
	// the second live test. Slot IDs encode house and slot ("DCTR22_1TM"),
	// so each house cycles its own teams.
	struct PoolSlot
	{
		int House;
		int Index;
		TeamTypeClass* Team;
		TaskForceClass* TaskForce;
		ScriptTypeClass* Script;
	};

	// (houseIndex, slotIndex) -> frame of last dispatch, for the TTL reaper.
	std::map<std::pair<int, int>, int> g_slotDispatchFrame;
	// (houseIndex, slotIndex) -> priority of the rule holding the slot, so a
	// higher-priority rule can preempt a passive one when all slots are busy.
	std::map<std::pair<int, int>, int> g_slotPriority;
	// Slots currently holding an intercept team, steered toward the live raider
	// each tick. A slot leaves the set when reused for a non-intercept mission.
	std::set<std::pair<int, int>> g_interceptSlots;
	// Slots holding a DefendBase team, steered to the base's outer edge so they
	// hold the perimeter instead of camping at the war-factory centre.
	std::set<std::pair<int, int>> g_defendSlots;

	// Order a live doctrine team's members to move to (and hold at) a cell.
	void MoveDoctrineTeam(int const hIdx, int const slot, CellClass* const pCell)
	{
		if (!pCell) return;
		char id[0x18];
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, slot);
		auto const pType = TeamTypeClass::Find(id);
		if (!pType || pType->cntInstances <= 0) return;
		auto const pTeam = pType->FindFirstInstance();
		if (!pTeam) return;
		for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
		{
			if (pFoot->InLimbo || pFoot->Health <= 0) continue;
			pFoot->SetDestination(pCell, true);
			pFoot->QueueMission(Mission::Move, false);
		}
	}

	// Build (or reuse) the trio for a chosen slot index. Objects live in the
	// engine's own type arrays (created with the game's allocator via
	// GameCreate), so the engine owns their lifetime — ClearClasses destroys
	// them with everything else, and we simply re-create next scenario.
	bool FillSlot(PoolSlot& out, int const hIdx, int const i)
	{
		char id[0x18];
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, i);
		auto pTeam = TeamTypeClass::Find(id);
		if (!pTeam)
			pTeam = GameCreate<TeamTypeClass>(id);
		if (!pTeam)
			return false;

		std::snprintf(id, sizeof(id), "DCTR%d_%dTF", hIdx, i);
		auto pTF = TaskForceClass::Find(id);
		if (!pTF)
			pTF = GameCreate<TaskForceClass>(id);

		std::snprintf(id, sizeof(id), "DCTR%d_%dSC", hIdx, i);
		auto pScript = ScriptTypeClass::Find(id);
		if (!pScript)
			pScript = GameCreate<ScriptTypeClass>(id);

		if (!pTF || !pScript)
			return false;

		out = { hIdx, i, pTeam, pTF, pScript };
		return true;
	}

	// Find a slot for a dispatch of the given priority. Prefers a free slot
	// (empty, or a live team past its TTL); if all slots hold live teams, a
	// strictly-higher-priority request preempts the lowest-priority holder so
	// an urgent aggressive rule is never starved by passive guarding.
	bool AcquireSlot(PoolSlot& out, HouseClass* const pHouse, int const frame,
		int const ttl, int const priority)
	{
		int const perHouse = DoctrineConfig::Instance.TeamsPerHouse > 0
			? DoctrineConfig::Instance.TeamsPerHouse : 1;
		int const hIdx = pHouse->ArrayIndex;

		char id[0x18];
		int victim = -1, victimPriority = 0;
		for (int i = 0; i < perHouse; ++i)
		{
			std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, i);
			auto pTeam = TeamTypeClass::Find(id);
			bool const live = pTeam && pTeam->cntInstances > 0;
			if (live)
			{
				auto const it = g_slotDispatchFrame.find({ hIdx, i });
				bool const expired = it == g_slotDispatchFrame.end()
					|| frame - it->second >= ttl;
				if (!expired)
				{
					// Track the weakest live holder as a preemption candidate.
					int const held = g_slotPriority.count({ hIdx, i })
						? g_slotPriority[{ hIdx, i }] : 0;
					if (victim < 0 || held < victimPriority)
					{
						victim = i;
						victimPriority = held;
					}
					continue;
				}
				Debug::Log("[DoctrineExt] slot %s exceeded TeamTTL, disbanding.\n", id);
				pTeam->DestroyAllInstances();
			}
			return FillSlot(out, hIdx, i); // free slot
		}

		// No free slot — preempt the weakest holder if we outrank it.
		if (victim >= 0 && priority > victimPriority)
		{
			std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, victim);
			if (auto const pTeam = TeamTypeClass::Find(id))
			{
				Debug::Log("[DoctrineExt] slot %s preempted (prio %d > held %d).\n",
					id, priority, victimPriority);
				pTeam->DestroyAllInstances();
			}
			return FillSlot(out, hIdx, victim);
		}
		return false; // all slots busy with equal-or-higher priority teams
	}
}

bool Teams::Dispatch(HouseClass* pHouse, const DoctrineRule& rule, double obsValue,
	TechnoClass* pTarget)
{
	auto const& cfg = DoctrineConfig::Instance;

	// Intercept behaves like HuntTarget (aggressive, target-bound) — the
	// difference is which observation feeds it: HuntAce chases the enemy's
	// deadliest unit, Intercept sallies out at the nearest incoming raider.
	bool const hunt = rule.Mission == "HuntTarget" || rule.Mission == "Intercept";
	if (!hunt && rule.Mission != "DefendBase")
	{
		WarnOnce("rule " + rule.Name + ": mission " + rule.Mission
			+ " not implemented yet (DefendBase, HuntTarget, Intercept)");
		return false;
	}
	if (hunt && rule.Target == "ThatUnit" && !pTarget)
		return false; // nothing concrete to hunt this tick

	const DoctrineArsenalRole* pRole = nullptr;
	for (auto const& role : cfg.Arsenal)
		if (role.Role == rule.Respond)
			pRole = &role;
	if (!pRole)
	{
		WarnOnce("rule " + rule.Name + ": no arsenal role " + rule.Respond);
		return false;
	}

	auto const pType = PickType(pHouse, *pRole);
	if (!pType)
	{
		WarnOnce("rule " + rule.Name + ": house " + std::string(pHouse->get_ID())
			+ "#" + std::to_string(pHouse->ArrayIndex)
			+ " has no buildable/owned unit in role " + rule.Respond);
		return false;
	}

	// Scale the force to the threat: how many thresholds' worth of trouble
	// are we looking at, times the rule's Scale, clamped by the globals.
	double need = 1.0;
	if (rule.WhenValue > 0.0 && rule.WhenOp[0] == '>')
		need = obsValue / rule.WhenValue;
	int count = static_cast<int>(std::ceil(need * rule.Scale));
	if (count < 1) count = 1;
	if (count > cfg.MaxTeamSize) count = cfg.MaxTeamSize;
	int const cost = pType->GetCost();
	if (cost > 0 && count * cost > cfg.MaxTeamCost)
	{
		count = cfg.MaxTeamCost / cost;
		if (count < 1) count = 1;
	}

	int const frame = Unsorted::CurrentFrame;
	PoolSlot slot;
	if (!AcquireSlot(slot, pHouse, frame, cfg.TeamTTL > 0 ? cfg.TeamTTL : 3600, rule.Priority))
	{
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] dispatch %s for %s#%d skipped: house's team slots busy.\n",
				rule.Name.c_str(), pHouse->get_ID(), pHouse->ArrayIndex);
		return false;
	}
	g_slotDispatchFrame[{ slot.House, slot.Index }] = frame;
	g_slotPriority[{ slot.House, slot.Index }] = rule.Priority;
	auto const slotKey = std::make_pair(slot.House, slot.Index);
	g_interceptSlots.erase(slotKey);
	g_defendSlots.erase(slotKey);
	if (rule.Mission == "Intercept")
		g_interceptSlots.insert(slotKey);
	else if (rule.Mission == "DefendBase")
		g_defendSlots.insert(slotKey);

	// Rewrite the trio for this dispatch. Only touch what we mean to set;
	// everything else keeps the game's own constructor defaults.
	slot.TaskForce->CountEntries = 1;
	slot.TaskForce->Entries[0] = { count, pType };
	slot.TaskForce->Group = -1;

	if (hunt)
	{
		// Set Mission -> Hunt: engage the assigned target, then roam (the
		// wave tool's own "keep engaging afterward" idiom, action 11 arg 7).
		slot.Script->ActionsCount = 1;
		slot.Script->ScriptActions[0] = { 11, 7 };
	}
	else
	{
		slot.Script->ActionsCount = 1;
		slot.Script->ScriptActions[0] = { 5, 60 }; // Guard Area, then disband
	}

	auto const pTT = slot.Team;
	pTT->TaskForce = slot.TaskForce;
	pTT->ScriptType = slot.Script;
	pTT->Max = 1;
	pTT->Priority = 50;
	pTT->VeteranLevel = 1;
	pTT->TechLevel = 0;
	pTT->MindControlDecision = 0;
	pTT->Owner = nullptr;
	pTT->idxHouse = -1;
	pTT->Autocreate = false;  // never picked up by the AI's own team logic
	pTT->Prebuild = false;
	pTT->Reinforce = false;
	pTT->Recruiter = true;    // under-strength teams keep pulling free units
	pTT->LooseRecruit = true;
	pTT->AreTeamMembersRecruitable = false; // no poaching by other teams
	pTT->IsBaseDefense = !hunt; // slots are rewritten, so set BOTH ways
	pTT->Full = false;
	pTT->Aggressive = hunt;
	pTT->Whiner = false;
	pTT->Annoyance = false;
	pTT->GuardSlower = false;
	pTT->Loadable = false;
	pTT->Suicide = false;
	pTT->Droppod = false;
	pTT->OnTransOnly = false;

	auto const pTeam = pTT->CreateTeam(pHouse);
	if (!pTeam)
	{
		Debug::Log("[DoctrineExt] dispatch %s for %s: CreateTeam failed.\n",
			rule.Name.c_str(), pHouse->get_ID());
		return false;
	}

	// Recruit loose units of the chosen type ourselves for an immediate
	// response; Recruiter=yes keeps filling the remainder over time.
	int got = 0;
	for (int i = 0; i < TechnoClass::Array.Count && got < count; ++i)
	{
		auto const pTechno = TechnoClass::Array.GetItem(i);
		if (!pTechno || pTechno->Owner != pHouse || pTechno->InLimbo) continue;
		auto const what = pTechno->WhatAmI();
		if (what != AbstractType::Infantry && what != AbstractType::Unit
			&& what != AbstractType::Aircraft) continue;
		if (pTechno->GetTechnoType() != pType) continue;
		auto const pFoot = static_cast<FootClass*>(pTechno);
		if (pFoot->Team) continue;
		if (pTeam->AddMember(pFoot, true))
			++got;
	}

	if (hunt && pTarget)
		pTeam->AssignMissionTarget(pTarget);

	// Hybrid fielding (Rex, 2026-09-02): the recruit loop above uses units the
	// house already owns; if that leaves the team short and the house doesn't
	// build this unit on its own (proven: a $479k Allied AI never made IFVs),
	// top up by DEMANDING production — capped so doctrine tops up to the team's
	// need but never floods the AI's economy or fights its own build order.
	int const money = static_cast<int>(pHouse->Available_Money());
	int queued = 0;
	if (cfg.AutoProduce && got < count)
	{
		auto const pFactory = FindHouseFactory(pHouse, pType);
		if (pFactory)
		{
			// Produce enough that owned + already-queued reaches the team's
			// need (owned already includes the units we just recruited), then
			// cap per dispatch and by affordability.
			int const inProduction = pFactory->CountTotal(pType);
			int const owned = CountOwned(pHouse, pType);
			int deficit = count - owned - inProduction;
			int const perDispatchCap = cfg.MaxProducePerDispatch > 0
				? cfg.MaxProducePerDispatch : 2;
			if (deficit > perDispatchCap) deficit = perDispatchCap;
			int const affordable = cost > 0 ? money / cost : deficit;
			if (deficit > affordable) deficit = affordable;
			for (int i = 0; i < deficit; ++i)
			{
				pFactory->DemandProduction(pType, pHouse, true);
				++queued;
			}
		}
		else
		{
			WarnOnce("rule " + rule.Name + ": house " + std::string(pHouse->get_ID())
				+ " has no factory to build " + std::string(pType->ID));
		}
	}

	Debug::Log("[DoctrineExt] DISPATCH %s: house=%s#%d obs=%.1f -> %d x %s (%s%s%s), "
		"recruited %d, queued %d, $%d vs need $%d, slot=%s.\n",
		rule.Name.c_str(), pHouse->get_ID(), pHouse->ArrayIndex, obsValue, count,
		pType->ID, rule.Mission.c_str(),
		(hunt && pTarget) ? " target=" : "",
		(hunt && pTarget) ? pTarget->GetTechnoType()->ID : "",
		got, queued, money, count * cost, pTT->ID);
	return true;
}

bool Teams::HasActiveIntercept(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	char id[0x18];
	for (auto const& key : g_interceptSlots)
	{
		if (key.first != hIdx) continue;
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, key.second);
		auto const pTeam = TeamTypeClass::Find(id);
		if (pTeam && pTeam->cntInstances > 0)
			return true;
	}
	return false;
}

void Teams::LogTeamFill(HouseClass* pHouse)
{
	int const perHouse = DoctrineConfig::Instance.TeamsPerHouse > 0
		? DoctrineConfig::Instance.TeamsPerHouse : 1;
	int const hIdx = pHouse->ArrayIndex;
	char id[0x18];
	for (int i = 0; i < perHouse; ++i)
	{
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, i);
		auto const pType = TeamTypeClass::Find(id);
		if (!pType || pType->cntInstances <= 0) continue;
		auto const pTeam = pType->FindFirstInstance();
		if (!pTeam) continue;
		int members = 0;
		for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
			++members;
		Debug::Log("[DoctrineExt] team %s: %d members (want %d).\n",
			id, members, pTeam->Type ? pTeam->Type->TaskForce
				? pTeam->Type->TaskForce->Entries[0].Amount : 0 : 0);
	}
}

bool Teams::HasActiveDefend(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	char id[0x18];
	for (auto const& key : g_defendSlots)
	{
		if (key.first != hIdx) continue;
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, key.second);
		auto const pTeam = TeamTypeClass::Find(id);
		if (pTeam && pTeam->cntInstances > 0)
			return true;
	}
	return false;
}

void Teams::SteerIntercepts(HouseClass* pHouse, TechnoClass* pRaider)
{
	if (!pRaider) return; // no live raider in the bubble; leave teams be
	int const hIdx = pHouse->ArrayIndex;

	// Interference, not pursuit (Rex, 2026-09-03): move to a point ON the line
	// between the raider and the base — a forward screen in the raid's path —
	// rather than chasing the aircraft around. The point tracks the raider each
	// tick, so the team repositions to stay between it and the base.
	auto const baseCoord = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	auto const raiderCoord = pRaider->GetCoords();
	double const dx = raiderCoord.X - baseCoord.X;
	double const dy = raiderCoord.Y - baseCoord.Y;
	double const dist = std::sqrt(dx * dx + dy * dy);

	int const standoffCells = DoctrineConfig::Instance.InterceptStandoff;
	double const standoff = standoffCells * 256.0; // leptons per cell
	double const reach = dist > 1.0 ? (standoff < dist ? standoff : dist) : 0.0;

	CoordStruct screen = baseCoord;
	if (dist > 1.0)
	{
		screen.X = baseCoord.X + static_cast<int>(dx / dist * reach);
		screen.Y = baseCoord.Y + static_cast<int>(dy / dist * reach);
	}
	auto const pCell = MapClass::Instance.TryGetCellAt(screen);
	if (!pCell) return;

	// Drive members to the screen point; AA units auto-fire on aircraft in
	// range as they hold the line, so they interfere without abandoning the
	// base to chase a faster aircraft across the map.
	for (auto const& key : g_interceptSlots)
		if (key.first == hIdx)
			MoveDoctrineTeam(hIdx, key.second, pCell);
}

void Teams::SteerDefenders(HouseClass* pHouse)
{
	if (!DoctrineConfig::Instance.DefendPerimeter) return;
	int const hIdx = pHouse->ArrayIndex;

	// Collect this house's live defend slots.
	std::vector<int> slots;
	for (auto const& key : g_defendSlots)
		if (key.first == hIdx)
			slots.push_back(key.second);
	if (slots.empty()) return;

	auto const center = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	if (center.X == 0 && center.Y == 0) return;

	// Learn the base's outer edge: the farthest owned building from the centre,
	// plus a margin. This is the perimeter the defenders should hold instead of
	// camping at the centre.
	double radius = 0.0;
	for (auto const pBld : pHouse->Buildings)
	{
		if (!pBld) continue;
		auto const c = pBld->GetCoords();
		double const d = std::sqrt(double(c.X - center.X) * (c.X - center.X)
			+ double(c.Y - center.Y) * (c.Y - center.Y));
		if (d > radius) radius = d;
	}
	radius += DoctrineConfig::Instance.BaseEdgeMargin * 256.0;
	if (radius < 5 * 256.0) radius = 5 * 256.0; // floor for tiny/new bases

	// Face the fan toward where the enemy ACTUALLY comes from: the hottest
	// learned travel lane near the base (§6.3). Until the heatmap has data,
	// fall back to the geometric bearing to the nearest enemy base.
	double baseAngle = 0.0;
	int laneStrength = 0;
	if (!LaneTracker::HottestLaneBearing(pHouse, baseAngle, laneStrength))
	{
		double bestD = 1e18;
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto const pOther = HouseClass::Array.GetItem(i);
			if (!pOther || pOther == pHouse || pOther->Defeated) continue;
			if (pOther->IsObserver() || pOther->IsNeutral()) continue;
			if (pHouse->IsAlliedWith(pOther)) continue;
			auto const ec = CellClass::Cell2Coord(pOther->GetBaseCenter());
			double const d = std::sqrt(double(ec.X - center.X) * (ec.X - center.X)
				+ double(ec.Y - center.Y) * (ec.Y - center.Y));
			if (d < bestD)
			{
				bestD = d;
				baseAngle = std::atan2(double(ec.Y - center.Y), double(ec.X - center.X));
			}
		}
	}

	// Fan the defenders across the enemy-facing arc of the perimeter.
	int const n = static_cast<int>(slots.size());
	double const spread = 1.047; // ~60 degrees total arc
	for (int k = 0; k < n; ++k)
	{
		double const off = n > 1 ? (k - (n - 1) / 2.0) * (spread / (n - 1)) : 0.0;
		double const a = baseAngle + off;
		CoordStruct pt = center;
		pt.X = center.X + static_cast<int>(std::cos(a) * radius);
		pt.Y = center.Y + static_cast<int>(std::sin(a) * radius);
		MoveDoctrineTeam(hIdx, slots[k], MapClass::Instance.TryGetCellAt(pt));
	}
}

void Teams::Reset()
{
	g_warned.clear();
	g_slotDispatchFrame.clear();
	g_slotPriority.clear();
	g_interceptSlots.clear();
	g_defendSlots.clear();
}
