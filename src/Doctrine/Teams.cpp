#include "Doctrine/Teams.h"
#include "Doctrine/Config.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <TeamClass.h>
#include <TeamTypeClass.h>
#include <TaskForceClass.h>
#include <ScriptTypeClass.h>
#include <TechnoTypeClass.h>
#include <FactoryClass.h>
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <AircraftTypeClass.h>
#include <Memory.h>
#include <Fundamentals.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

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

	// Find-or-create the slot's trio by ID. Objects live in the engine's own
	// type arrays (created with the game's allocator via GameCreate), so the
	// engine owns their lifetime — ClearClasses destroys them with everything
	// else, and we simply re-create next scenario.
	bool AcquireSlot(PoolSlot& out, HouseClass* const pHouse, int const frame, int const ttl)
	{
		int const perHouse = DoctrineConfig::Instance.TeamsPerHouse > 0
			? DoctrineConfig::Instance.TeamsPerHouse : 1;
		int const hIdx = pHouse->ArrayIndex;

		char id[0x18];
		for (int i = 0; i < perHouse; ++i)
		{
			std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, i);
			auto pTeam = TeamTypeClass::Find(id);
			if (pTeam && pTeam->cntInstances > 0)
			{
				auto const it = g_slotDispatchFrame.find({ hIdx, i });
				if (it != g_slotDispatchFrame.end() && frame - it->second < ttl)
					continue; // a live team is still using this slot
				Debug::Log("[DoctrineExt] slot %s exceeded TeamTTL, disbanding.\n", id);
				pTeam->DestroyAllInstances();
			}

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
		return false; // all of this house's slots busy
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
	if (!AcquireSlot(slot, pHouse, frame, cfg.TeamTTL > 0 ? cfg.TeamTTL : 3600))
	{
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] dispatch %s for %s#%d skipped: house's team slots busy.\n",
				rule.Name.c_str(), pHouse->get_ID(), pHouse->ArrayIndex);
		return false;
	}
	g_slotDispatchFrame[{ slot.House, slot.Index }] = frame;

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
		auto const pFactory = pHouse->GetPrimaryFactory(
			pType->WhatAmI(), pType->Naval, BuildCat::DontCare);
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

void Teams::Reset()
{
	g_warned.clear();
	g_slotDispatchFrame.clear();
}
