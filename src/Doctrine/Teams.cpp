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
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <AircraftTypeClass.h>
#include <Memory.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <cstdio>
#include <set>

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
			if (pHouse->CanBuild(pType, false, true) == CanBuildResult::Buildable
				|| CountOwned(pHouse, pType) > 0)
				return pType;
		}
		return nullptr;
	}

	// The pool: 4 concurrent doctrine teams per game, rewritten per dispatch.
	constexpr int PoolSize = 4;

	struct PoolSlot
	{
		TeamTypeClass* Team;
		TaskForceClass* TaskForce;
		ScriptTypeClass* Script;
	};

	// Find-or-create the slot's trio by ID. Objects live in the engine's own
	// type arrays (created with the game's allocator via GameCreate), so the
	// engine owns their lifetime — ClearClasses destroys them with everything
	// else, and we simply re-create next scenario.
	bool AcquireSlot(PoolSlot& out)
	{
		char id[0x18];
		for (int i = 0; i < PoolSize; ++i)
		{
			std::snprintf(id, sizeof(id), "DCTRTM%d", i);
			auto pTeam = TeamTypeClass::Find(id);
			if (pTeam && pTeam->cntInstances > 0)
				continue; // a live team is still using this slot

			if (!pTeam)
				pTeam = GameCreate<TeamTypeClass>(id);
			if (!pTeam)
				return false;

			std::snprintf(id, sizeof(id), "DCTRTF%d", i);
			auto pTF = TaskForceClass::Find(id);
			if (!pTF)
				pTF = GameCreate<TaskForceClass>(id);

			std::snprintf(id, sizeof(id), "DCTRSC%d", i);
			auto pScript = ScriptTypeClass::Find(id);
			if (!pScript)
				pScript = GameCreate<ScriptTypeClass>(id);

			if (!pTF || !pScript)
				return false;

			out = { pTeam, pTF, pScript };
			return true;
		}
		return false; // all slots busy
	}
}

bool Teams::Dispatch(HouseClass* pHouse, const DoctrineRule& rule, double obsValue)
{
	auto const& cfg = DoctrineConfig::Instance;

	// Phase 1 renders one canned mission.
	if (rule.Mission != "DefendBase")
	{
		WarnOnce("rule " + rule.Name + ": mission " + rule.Mission
			+ " not implemented yet, only DefendBase");
		return false;
	}

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

	PoolSlot slot;
	if (!AcquireSlot(slot))
	{
		Debug::Log("[DoctrineExt] dispatch %s for %s skipped: all %d team slots busy.\n",
			rule.Name.c_str(), pHouse->get_ID(), PoolSize);
		return false;
	}

	// Rewrite the trio for this dispatch. Only touch what we mean to set;
	// everything else keeps the game's own constructor defaults.
	slot.TaskForce->CountEntries = 1;
	slot.TaskForce->Entries[0] = { count, pType };
	slot.TaskForce->Group = -1;

	slot.Script->ActionsCount = 1;
	slot.Script->ScriptActions[0] = { 5, 60 }; // Guard Area, then disband

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
	pTT->IsBaseDefense = true;
	pTT->Full = false;
	pTT->Aggressive = false;
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

	Debug::Log("[DoctrineExt] DISPATCH %s: house=%s obs=%.1f -> %d x %s (%s), "
		"recruited %d now, slot=%s.\n",
		rule.Name.c_str(), pHouse->get_ID(), obsValue, count, pType->ID,
		rule.Mission.c_str(), got, pTT->ID);
	return true;
}

void Teams::Reset()
{
	g_warned.clear();
}
