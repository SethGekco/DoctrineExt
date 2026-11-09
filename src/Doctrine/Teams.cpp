#include "Doctrine/Teams.h"
#include "Doctrine/Config.h"
#include "Doctrine/LaneTracker.h"
#include "Doctrine/DeathZones.h"
#include "Doctrine/ArsenalGrader.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <FootClass.h>
#include <TeamClass.h>
#include <TeamTypeClass.h>
#include <TaskForceClass.h>
#include <ScriptTypeClass.h>
#include <TechnoTypeClass.h>
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>
#include <BulletTypeClass.h>
#include <FactoryClass.h>
#include <MapClass.h>
#include <CellClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <MissionClass.h>
#include <RulesClass.h>
#include <OverlayTypeClass.h>
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <AircraftTypeClass.h>
#include <Memory.h>
#include <Fundamentals.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <iterator>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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

	// Get a BUILDING built by the AI. DemandProduction is the WRONG tool for AI
	// buildings: the AI keeps no idle building factory and GetPrimaryFactory is a
	// human-sidebar concept (returns null), so demand-to-factory never queued
	// anything. The right path is the AI's OWN base plan: append a BaseNode and the
	// AI builds AND PLACES it via its normal base logic (handles the water-adjacent
	// naval-yard placement we can't easily do ourselves). `where` is the desired
	// cell; the AI searches outward from it. Skips if an unplaced node for this
	// type is already pending, so we don't spam the plan.
	bool QueueBaseNode(HouseClass* const pHouse, BuildingTypeClass* const pBt,
		CellStruct const& where)
	{
		if (!pBt) return false;
		int const idx = BuildingTypeClass::Array.FindItemIndex(pBt);
		if (idx < 0) return false;
		auto& nodes = pHouse->Base.BaseNodes;
		for (int i = 0; i < nodes.Count; ++i)
		{
			auto const n = nodes.GetItem(i);
			if (n.BuildingTypeIndex == idx && !n.Placed) return false; // already pending
		}
		BaseNodeClass node;
		node.BuildingTypeIndex = idx;
		node.MapCoords = where;
		node.Placed = false;
		node.Attempts = 0;
		return nodes.AddItem(node);
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
			bool const ownerOK = !DoctrineConfig::Get().StrictOwnership
				|| pHouse->InOwners(pType);
			if (DoctrineConfig::Get().DebugTicks)
				Debug::Log("[DoctrineExt]   candidate %s for %s#%d: CanBuild=%d inOwners=%d owned=%d\n",
					id.c_str(), pHouse->get_ID(), pHouse->ArrayIndex,
					static_cast<int>(canBuild), ownerOK, owned);
			if ((canBuild == CanBuildResult::Buildable && ownerOK) || owned > 0)
				return pType;
		}
		return nullptr;
	}

	// STRICT buildability for anything DoctrineExt PRODUCES. This stack's
	// CanBuild is leaky (it OK'd cross-faction builds, ignoring Owner=), so we
	// don't trust it alone — explicitly enforce TechLevel (a TechLevel=11 unit
	// is above the game's max-10 tech level, so never buildable) and Owner=,
	// then let CanBuild cover prerequisites + build limits. Used before any
	// DemandProduction; the recruit path may still USE already-owned units.
	// Is a generic-prerequisite category (POWER/FACTORY/BARRACKS/RADAR/TECH/PROC from
	// [GeneralPrerequisites]) SATISFIED for the house? True if the house owns at least
	// one building from the list — OR the list is EMPTY, which means the category isn't
	// restricted in this ruleset (this modpack has no loose [GeneralPrerequisites]; a
	// sibling PrerequisiteExt.dll may manage prereqs). Empty => satisfied keeps us from
	// ever over-blocking a legit build; we only tighten when the list is populated.
	bool OwnsPrereqCategory(HouseClass* const pHouse, TypeList<int> const& list)
	{
		bool any = false;
		for (int idx : list)
		{
			any = true;
			if (idx < 0 || idx >= BuildingTypeClass::Array.Count) continue;
			auto const bt = BuildingTypeClass::Array.GetItem(idx);
			if (bt && pHouse->CountOwnedAndPresent(bt) > 0) return true;
		}
		return !any; // empty list => category not restricted => satisfied
	}

	// Does the house ACTUALLY own pType's prerequisites? The engine's CanBuild leaks the
	// generic TECH prereq for AI houses — it greenlit CLEG (Prerequisite=GAPILE,TECH) at
	// ~54s with no Battle Lab, so the force-build built a Chrono Legionnaire with no
	// GATECH. We resolve prereqs ourselves: every SPECIFIC building must be present, and
	// every GENERIC category (negative-encoded) must be satisfied by a building from the
	// matching [GeneralPrerequisites] list. Owning a PrerequisiteOverride building
	// bypasses all of it (vanilla override semantics, e.g. CLEG via the Smithsonian).
	int CountList(TypeList<int> const& list) { int n = 0; for (int i : list) { (void)i; ++n; } return n; }

	bool HasRealPrerequisites(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (!pType) return false;
		auto const pRules = RulesClass::Instance;
		if (!pRules) return true; // can't resolve the category lists — don't block

		// One-time: dump the generic-prereq category sizes. If Tech=0 here, the ruleset
		// doesn't restrict TECH at the engine level (a sibling PrerequisiteExt may), so
		// this check can't enforce "needs a Battle Lab" and the CLEG-without-GATECH case
		// is a prereq-config matter, not DoctrineExt.
		static bool catLogged = false;
		if (!catLogged)
		{
			catLogged = true;
			Debug::Log("[DoctrineExt] prereq categories (0=unrestricted): Power=%d Factory=%d "
				"Barracks=%d Radar=%d Tech=%d Proc=%d\n",
				CountList(pRules->PrerequisitePower), CountList(pRules->PrerequisiteFactory),
				CountList(pRules->PrerequisiteBarracks), CountList(pRules->PrerequisiteRadar),
				CountList(pRules->PrerequisiteTech), CountList(pRules->PrerequisiteProc));
		}

		for (int idx : pType->PrerequisiteOverride)
		{
			if (idx < 0 || idx >= BuildingTypeClass::Array.Count) continue;
			auto const bt = BuildingTypeClass::Array.GetItem(idx);
			if (bt && pHouse->CountOwnedAndPresent(bt) > 0) return true;
		}

		for (int pre : pType->Prerequisite)
		{
			if (pre >= 0)
			{
				if (pre >= BuildingTypeClass::Array.Count) return false;
				auto const bt = BuildingTypeClass::Array.GetItem(pre);
				if (!bt || pHouse->CountOwnedAndPresent(bt) <= 0) return false;
			}
			else
			{
				bool ok = true;
				switch (static_cast<Prerequisite>(pre))
				{
				case Prerequisite::Power:    ok = OwnsPrereqCategory(pHouse, pRules->PrerequisitePower); break;
				case Prerequisite::Factory:  ok = OwnsPrereqCategory(pHouse, pRules->PrerequisiteFactory); break;
				case Prerequisite::Barracks: ok = OwnsPrereqCategory(pHouse, pRules->PrerequisiteBarracks); break;
				case Prerequisite::Radar:    ok = OwnsPrereqCategory(pHouse, pRules->PrerequisiteRadar); break;
				case Prerequisite::Tech:     ok = OwnsPrereqCategory(pHouse, pRules->PrerequisiteTech); break;
				case Prerequisite::Proc:     ok = OwnsPrereqCategory(pHouse, pRules->PrerequisiteProc); break;
				default: ok = true; break;  // unknown encoding — don't block
				}
				if (!ok) return false;
			}
		}
		return true;
	}

	// honorPrereq: when true, enforce our own prerequisite-ownership check (the engine's
	// CanBuild leaks the generic TECH prereq for AI houses, so it alone let the AI
	// force-build a CLEG with no Battle Lab). Callers pass the per-task config toggle
	// (HonorPrereqForCrate / HonorPrereqForRush); defaults to honoring.
	bool CanBuildStrict(HouseClass* const pHouse, TechnoTypeClass* const pType,
		bool const honorPrereq = true)
	{
		if (!pType) return false;
		int const tl = pType->TechLevel;
		if (tl < 0) return false;                          // -1 = disabled
		// Compare to the house's tech level, but never above the game's max (10);
		// so TechLevel=11 is unbuildable even if the house's field reads oddly.
		int maxTL = (pHouse->TechLevel >= 0 && pHouse->TechLevel <= 10)
			? pHouse->TechLevel : 10;
		if (tl > maxTL) return false;                      // e.g. TechLevel=11
		if (DoctrineConfig::Get().StrictOwnership && !pHouse->InOwners(pType))
			return false;
		// Our own prereq check FIRST (when honoring) — CanBuild leaks the generic TECH
		// prereq for AI houses, so don't trust it alone.
		if (honorPrereq && !HasRealPrerequisites(pHouse, pType))
			return false;
		return pHouse->CanBuild(pType, false, true) == CanBuildResult::Buildable;
	}

	// Is pType usable by pHouse right now (buildable respecting Owner=, or
	// already owned)? Same rule PickType applies, factored out for PickCounter.
	bool IsEligible(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (!pType || !IsTeamable(pType)) return false;
		bool const ownerOK = !DoctrineConfig::Get().StrictOwnership
			|| pHouse->InOwners(pType);
		return (pHouse->CanBuild(pType, false, true) == CanBuildResult::Buildable
			&& ownerOK) || CountOwned(pHouse, pType) > 0;
	}

	// ─── Counter-selection (§10b Phase 6a) ──────────────────────────────────
	// The engine's rock-paper-scissors: a warhead does Damage x Verses[armor]
	// to a given armor class. We score each candidate by the 1v1 it would win
	// against the problem unit, adjusted for reach.

	// Best armor-adjusted DPS attacker's weapons can deal to a target of the
	// given armor and domain (air vs ground). 0 if it cannot hit that domain.
	double EffDPSVs(TechnoTypeClass* const pAttacker, int const targetArmor, bool const targetAir)
	{
		double best = 0.0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pAttacker->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType) continue;
			auto const w = pWS->WeaponType;
			if (w->ROF <= 0 || w->Damage <= 1 || !w->Warhead || !w->Projectile) continue;
			bool const canHit = targetAir ? w->Projectile->AA : w->Projectile->AG;
			if (!canHit) continue;
			int const burst = w->Burst > 0 ? w->Burst : 1;
			double dps = double(w->Damage) * burst / (w->ROF / 10.0);
			if (targetArmor >= 0 && targetArmor < 11)
				dps *= w->Warhead->Verses[targetArmor];
			if (dps > best) best = dps;
		}
		return best;
	}

	// Longest range among the attacker's weapons that can hit the target domain.
	int BestRangeVs(TechnoTypeClass* const pAttacker, bool const targetAir)
	{
		int best = 0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pAttacker->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType || !pWS->WeaponType->Projectile) continue;
			auto const w = pWS->WeaponType;
			bool const canHit = targetAir ? w->Projectile->AA : w->Projectile->AG;
			if (canHit && w->Range > best) best = w->Range;
		}
		return best;
	}

	// How good a counter U is to the live target T. Higher = better; 0 = U
	// cannot meaningfully engage T (can't hit its domain / does no real damage).
	double CounterScore(TechnoTypeClass* const pU, TechnoClass* const pTarget)
	{
		auto const pT = pTarget->GetTechnoType();
		if (!pT) return 0.0;
		bool const tAir = pTarget->IsInAir();
		bool const uAir = pU->WhatAmI() == AbstractType::AircraftType;

		double const offense = EffDPSVs(pU, static_cast<int>(pT->Armor), tAir);
		if (offense <= 0.0) return 0.0; // can't hurt the target

		double const incoming = EffDPSVs(pT, static_cast<int>(pU->Armor), uAir);
		int const uStr = pU->Strength > 0 ? pU->Strength : 1;
		int const tStr = pT->Strength > 0 ? pT->Strength : 1;

		// Duel outcome: (time T survives us) vs (time we survive T). >1 means we
		// win the trade. incoming 0 (T can't hit us) = a free kill.
		double const winRatio = incoming <= 0.0
			? 4.0
			: (double(uStr) * offense) / (double(tStr) * incoming);

		// Reach: out-ranging T lets us hit for free; being out-ranged hurts.
		int const uRange = BestRangeVs(pU, tAir);
		int const tRange = BestRangeVs(pT, uAir);
		double reach = 1.0;
		if (tRange > 0)
			reach = uRange >= tRange
				? 1.0 + std::min(1.0, double(uRange - tRange) / tRange)
				: std::max(0.3, double(uRange) / tRange);

		return winRatio * reach;
	}

	// Pick the arsenal unit that best counters pTarget (highest CounterScore
	// among eligible candidates). Falls back to null so the caller can use the
	// plain best-first PickType when no counter is engageable.
	TechnoTypeClass* PickCounter(HouseClass* const pHouse, const DoctrineArsenalRole& role,
		TechnoClass* const pTarget, double& outScore)
	{
		TechnoTypeClass* best = nullptr;
		double bestScore = 0.0;
		for (auto const& id : role.Units)
		{
			auto const pType = TechnoTypeClass::Find(id.c_str());
			if (!pType || !IsEligible(pHouse, pType)) continue;
			double const score = CounterScore(pType, pTarget);
			if (DoctrineConfig::Get().DebugTicks)
				Debug::Log("[DoctrineExt]   counter %s vs %s: score=%.2f\n",
					id.c_str(), pTarget->GetTechnoType() ? pTarget->GetTechnoType()->ID : "?",
					score);
			if (score > bestScore)
			{
				bestScore = score;
				best = pType;
			}
		}
		outScore = bestScore;
		return best;
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
	// Slots holding a HuntTarget team, steered onto the target's weak side.
	std::set<std::pair<int, int>> g_huntSlots;
	// Slots holding a Decapitate team, steered at the enemy's priority structure.
	std::set<std::pair<int, int>> g_decapSlots;
	// houseIndex -> last frame the base-flood relief fired (its own cooldown).
	std::map<int, int> g_floodLastFire;
	// houseIndex -> last frame the reserve spender built (its own cooldown).
	std::map<int, int> g_reserveLastFire;
	// Forced-budget state: last pass frame, and whether the house is currently in a
	// production lockdown (factories suspended to save for a priority building).
	std::map<int, int> g_budgetLastFire;
	std::map<int, bool> g_budgetLocked;
	// houseIndex -> rolling (frame, money) samples, for the growth trigger.
	std::map<int, std::deque<std::pair<int, int>>> g_moneyHistory;
	// houseIndex -> last frame this house committed an all-in rush.
	std::map<int, int> g_rushLastFire;

	// Raw armor-agnostic DPS of a type (both weapons), for military-power sums.
	double RawDPS(TechnoTypeClass* const pType)
	{
		double total = 0.0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pType->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType) continue;
			auto const w = pWS->WeaponType;
			if (w->ROF <= 0 || w->Damage <= 1) continue;
			int const burst = w->Burst > 0 ? w->Burst : 1;
			total += double(w->Damage) * burst / (w->ROF / 10.0);
		}
		return total;
	}

	// Anti-air DPS of a type: only weapons whose projectile can hit air (§AA P1).
	double AADps(TechnoTypeClass* const pType)
	{
		double total = 0.0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pType->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType) continue;
			auto const w = pWS->WeaponType;
			if (w->ROF <= 0 || w->Damage <= 1) continue;
			if (!w->Projectile || !w->Projectile->AA) continue; // must reach aircraft
			int const burst = w->Burst > 0 ? w->Burst : 1;
			total += double(w->Damage) * burst / (w->ROF / 10.0);
		}
		return total;
	}

	// Per-house military power (Σ living armed technos' DPS: units + defensive
	// buildings, minus miners), cached per frame — one pass serves every house.
	std::map<int, double> g_powerTable;
	int g_powerFrame = -1;
	void BuildPowerTable(int const frame)
	{
		if (frame == g_powerFrame) return;
		g_powerFrame = frame;
		g_powerTable.clear();
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry
				&& what != AbstractType::Aircraft && what != AbstractType::Building)
				continue;
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer) continue;
			double const dps = RawDPS(pType);
			if (dps <= 0.0) continue;
			if (pT->Owner)
				g_powerTable[pT->Owner->ArrayIndex] += dps;
		}
	}
	double PowerOf(int const idx)
	{
		auto const it = g_powerTable.find(idx);
		return it != g_powerTable.end() ? it->second : 0.0;
	}

	// The weakest live enemy of pHouse by military power (powered-down counts
	// softer) — the one most worth pressing. Shared by the rush and Decapitate.
	HouseClass* WeakestEnemy(HouseClass* const pHouse)
	{
		BuildPowerTable(Unsorted::CurrentFrame);
		HouseClass* best = nullptr;
		double bestPow = 1e18;
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto const pH = HouseClass::Array.GetItem(i);
			if (!pH || pH->Defeated || pH->IsObserver() || pH->IsNeutral()) continue;
			if (pH == pHouse || pHouse->IsAlliedWith(pH)) continue;
			double eff = PowerOf(pH->ArrayIndex);
			if (pH->PowerDrain > pH->PowerOutput) eff *= 0.5;
			if (eff < bestPow) { bestPow = eff; best = pH; }
		}
		return best;
	}

	// ─── Tech-tree decapitation (§10h) ──────────────────────────────────────
	enum class DecapRole { ConYard, Refinery, Vehicle, Aircraft, Infantry, Defense, Other, COUNT };

	DecapRole RoleOf(BuildingTypeClass* const bt)
	{
		if (!bt) return DecapRole::Other;
		if (bt->ConstructionYard) return DecapRole::ConYard;
		if (bt->Refinery) return DecapRole::Refinery;
		if (bt->WeaponsFactory || bt->Factory == AbstractType::UnitType) return DecapRole::Vehicle;
		if (bt->Helipad || bt->Factory == AbstractType::AircraftType) return DecapRole::Aircraft;
		if (bt->Factory == AbstractType::InfantryType) return DecapRole::Infantry;
		if (bt->IsBaseDefense) return DecapRole::Defense;
		return DecapRole::Other;
	}

	int RoleWeight(DecapRole const r)
	{
		auto const& c = DoctrineConfig::Get();
		switch (r)
		{
		case DecapRole::ConYard:  return c.DecapConYard;
		case DecapRole::Refinery: return c.DecapRefinery;
		case DecapRole::Vehicle:  return c.DecapVehicle;
		case DecapRole::Aircraft: return c.DecapAircraft;
		case DecapRole::Infantry: return c.DecapInfantry;
		case DecapRole::Defense:  return c.DecapDefense;
		default:                  return c.DecapOther;
		}
	}

	// The enemy building most worth killing to dismantle its rebuild chain:
	// score = role weight / providers-of-that-role, so a role that's a cheaper
	// COMPLETE cut (fewer buildings to destroy to deny the category) wins — a
	// lone war factory beats three service depots that jointly gate the MCV.
	// Targets sitting in a proven killbox (where pHouse's units keep dying) are
	// SKIPPED — don't feed the farm; if every target is lethal, return null so
	// decap stands down instead of throwing units away repeatedly.
	BuildingClass* DecapTarget(HouseClass* const pHouse, HouseClass* const pEnemy)
	{
		int count[static_cast<int>(DecapRole::COUNT)] = {};
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || pB->Owner != pEnemy || pB->InLimbo || pB->Health <= 0) continue;
			count[static_cast<int>(RoleOf(pB->Type))]++;
		}

		int const killThreshold = DoctrineConfig::Get().DeathZoneMinStrength > 0
			? DoctrineConfig::Get().DeathZoneMinStrength : 48;
		BuildingClass* best = nullptr;
		double bestScore = -1.0;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || pB->Owner != pEnemy || pB->InLimbo || pB->Health <= 0) continue;
			auto const role = RoleOf(pB->Type);
			int const w = RoleWeight(role);
			if (w <= 0) continue;
			CellStruct bc; pB->GetMapCoords(&bc);
			if (DeathZones::ScoreNearCell(pHouse, bc.X, bc.Y, 10) >= killThreshold)
				continue; // killbox around this target — don't feed it
			int const n = count[static_cast<int>(role)];
			double const score = static_cast<double>(w) / (n > 0 ? n : 1);
			if (score > bestScore) { bestScore = score; best = pB; }
		}
		return best;
	}

	bool HasOffensiveWeapon(TechnoTypeClass* const pType)
	{
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pType->GetWeapon(wi);
			if (pWS && pWS->WeaponType && pWS->WeaponType->Damage > 1)
				return true;
		}
		return false;
	}

	// Unit the modder forbade as a crate chaser ([Doctrine.General] CrateChaseExclude
	// — e.g. heroes you don't want risked). Everything else fast stays eligible.
	bool IsExcludedChaser(TechnoTypeClass* const pType)
	{
		if (!pType) return true;
		for (auto const& ex : DoctrineConfig::Get().CrateChaseExclude)
			if (ex == pType->ID) return true;
		return false;
	}

	// An "idle armed" unit: owned by the house, on NO team, alive, and carrying
	// a real weapon — i.e. a combat unit that is hoarded, not a harvester /
	// engineer / MCV and not already tasked. ResourceGatherer excludes ore
	// miners AND armed War Miners (which DO have a weapon, so the weapon test
	// alone let them through — Rex saw them yanked off mining). A modder
	// FloodExclude/FloodInclude list overrides by unit ID.
	bool IsIdleArmed(TechnoClass* const pTechno, HouseClass* const pHouse)
	{
		if (!pTechno || pTechno->Owner != pHouse || pTechno->InLimbo
			|| pTechno->Health <= 0)
			return false;
		auto const what = pTechno->WhatAmI();
		if (what != AbstractType::Unit && what != AbstractType::Infantry
			&& what != AbstractType::Aircraft)
			return false;
		if (static_cast<FootClass*>(pTechno)->Team)
			return false; // already on a team (aimd wave, our team, etc.)
		auto const pType = pTechno->GetTechnoType();
		if (!pType) return false;

		auto const& cfg = DoctrineConfig::Get();
		std::string const id = pType->ID;
		// Explicit exclude always wins; explicit include forces eligibility.
		for (auto const& ex : cfg.FloodExclude)
			if (ex == id) return false;
		for (auto const& in : cfg.FloodInclude)
			if (in == id) return true;

		if (pType->ResourceGatherer) return false; // miners keep mining
		return HasOffensiveWeapon(pType);
	}

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
		int const perHouse = DoctrineConfig::Get().TeamsPerHouse > 0
			? DoctrineConfig::Get().TeamsPerHouse : 1;
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
	auto const& cfg = DoctrineConfig::Get();

	// Intercept behaves like HuntTarget (aggressive, target-bound) — the
	// difference is which observation feeds it: HuntAce chases the enemy's
	// deadliest unit, Intercept sallies out at the nearest incoming raider.
	bool const hunt = rule.Mission == "HuntTarget" || rule.Mission == "Intercept";
	bool const decap = rule.Mission == "Decapitate";
	bool const aggressive = hunt || decap;
	if (!aggressive && rule.Mission != "DefendBase")
	{
		WarnOnce("rule " + rule.Name + ": mission " + rule.Mission
			+ " not implemented yet (DefendBase, HuntTarget, Intercept, Decapitate)");
		return false;
	}
	if (hunt && rule.Target == "ThatUnit" && !pTarget)
		return false; // nothing concrete to hunt this tick

	// Decapitate (§10h standalone): the strike target is the weakest enemy's
	// most rebuild-critical STRUCTURE, computed here rather than from the
	// observation. A BuildingClass is a TechnoClass, so it drives both the
	// armor-matchup counter pick and the steering the same way a unit target does.
	TechnoClass* pStrike = pTarget;
	if (decap)
	{
		auto const pEnemy = WeakestEnemy(pHouse);
		auto const pBld = pEnemy ? DecapTarget(pHouse, pEnemy) : nullptr;
		if (!pBld) return false; // nothing to decapitate (or all targets are killboxes)
		pStrike = pBld;
	}

	const DoctrineArsenalRole* pRole = nullptr;
	for (auto const& role : cfg.Arsenal)
		if (role.Role == rule.Respond)
			pRole = &role;
	// Fall back to the auto-graded roster for any role the modder didn't list
	// explicitly (§10d A) — so roles populate themselves, faction-correct per
	// house via the pick-time filters.
	if (!pRole && cfg.AutoArsenal)
		pRole = ArsenalGrader::RoleUnits(rule.Respond);
	if (!pRole)
	{
		WarnOnce("rule " + rule.Name + ": no arsenal role " + rule.Respond);
		return false;
	}

	// Counter-selection (§10b 6a): when the rule is bound to a concrete target
	// (HuntTarget/Intercept on a specific unit), pick the arsenal unit that
	// best COUNTERS it by the armor/weapon/range matchup; otherwise (and as a
	// fallback if nothing can engage it) use the plain best-first pick.
	TechnoTypeClass* pType = nullptr;
	double counterScore = 0.0;
	if (pStrike)
		pType = PickCounter(pHouse, *pRole, pStrike, counterScore);

	// "Don't feed the farm" (§10b 6c): for an aggressive hunt into the field,
	// if the best available counter can't win the trade (score below the
	// threshold), decline rather than send units to their death — wait for a
	// better tool / more mass. Defensive missions (Intercept/DefendBase) still
	// respond regardless.
	if (pType && rule.Mission == "HuntTarget"
		&& counterScore < DoctrineConfig::Get().MinCounterScore)
	{
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] %s declines hunt for %s#%d: best counter %s "
				"scores %.2f < MinCounterScore %.2f (no viable counter).\n",
				rule.Name.c_str(), pHouse->get_ID(), pHouse->ArrayIndex, pType->ID,
				counterScore, DoctrineConfig::Get().MinCounterScore);
		return false;
	}

	if (!pType)
		pType = PickType(pHouse, *pRole);
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
	// Decapitate isn't threat-scaled — Scale sets the strike size (x4 base).
	if (decap)
		count = static_cast<int>(std::ceil(rule.Scale * 4.0));
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
	g_huntSlots.erase(slotKey);
	g_decapSlots.erase(slotKey);
	if (rule.Mission == "Intercept")
		g_interceptSlots.insert(slotKey);
	else if (rule.Mission == "DefendBase")
		g_defendSlots.insert(slotKey);
	else if (rule.Mission == "HuntTarget")
		g_huntSlots.insert(slotKey);
	else if (rule.Mission == "Decapitate")
		g_decapSlots.insert(slotKey);

	// Rewrite the trio for this dispatch. Only touch what we mean to set;
	// everything else keeps the game's own constructor defaults.
	slot.TaskForce->CountEntries = 1;
	slot.TaskForce->Entries[0] = { count, pType };
	slot.TaskForce->Group = -1;

	if (aggressive)
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
	pTT->IsBaseDefense = !aggressive; // slots are rewritten, so set BOTH ways
	pTT->Full = false;
	pTT->Aggressive = aggressive;
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

	if (aggressive && pStrike)
		pTeam->AssignMissionTarget(pStrike);

	// Hybrid fielding (Rex, 2026-09-02): the recruit loop above uses units the
	// house already owns; if that leaves the team short and the house doesn't
	// build this unit on its own (proven: a $479k Allied AI never made IFVs),
	// top up by DEMANDING production — capped so doctrine tops up to the team's
	// need but never floods the AI's economy or fights its own build order.
	int const money = static_cast<int>(pHouse->Available_Money());
	int queued = 0;
	// Only PRODUCE if the unit is legitimately buildable now (TechLevel + Owner=
	// + prereqs). If pType was chosen only because the house already owns some
	// (recruit path), we field those but never illegally build more.
	if (cfg.AutoProduce && got < count && CanBuildStrict(pHouse, pType, cfg.HonorPrereqForRush()))
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
		(aggressive && pStrike) ? " target=" : "",
		(aggressive && pStrike && pStrike->GetTechnoType()) ? pStrike->GetTechnoType()->ID : "",
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
	int const perHouse = DoctrineConfig::Get().TeamsPerHouse > 0
		? DoctrineConfig::Get().TeamsPerHouse : 1;
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

	// Ambush on the LEARNED approach (Phase 5 wiring): position the screen along
	// the hottest travel-lane bearing near the base — where raids actually come
	// from — so interceptors hold the known air corridor instead of only the
	// line to the current raider. Until the heatmap has data, fall back to the
	// bearing toward the current raider (the prior interference behaviour).
	auto const baseCoord = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	auto const raiderCoord = pRaider->GetCoords();
	double const dx = raiderCoord.X - baseCoord.X;
	double const dy = raiderCoord.Y - baseCoord.Y;
	double const dist = std::sqrt(dx * dx + dy * dy);

	double bearing = 0.0;
	int laneStrength = 0;
	if (!LaneTracker::HottestLaneBearing(pHouse, bearing, laneStrength))
	{
		if (dist <= 1.0) return; // raider on top of base, nothing sensible to do
		bearing = std::atan2(dy, dx); // toward the current raider
	}

	int const standoffCells = DoctrineConfig::Get().InterceptStandoff;
	double const standoff = standoffCells * 256.0; // leptons per cell
	// Don't screen past the raider when we're aiming at it; the lane path uses
	// the full standoff (the raid may not have reached the corridor yet).
	double const reach = (laneStrength == 0 && standoff > dist) ? dist : standoff;

	CoordStruct screen = baseCoord;
	screen.X = baseCoord.X + static_cast<int>(std::cos(bearing) * reach);
	screen.Y = baseCoord.Y + static_cast<int>(std::sin(bearing) * reach);
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
	if (!DoctrineConfig::Get().DefendPerimeter) return;
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
	radius += DoctrineConfig::Get().BaseEdgeMargin * 256.0;
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

	// Proof-of-orientation trace (throttled, DebugTicks): the bearing the
	// defenders are actually fanning toward and whether it came from the
	// learned lane or the geometric fallback.
	if (DoctrineConfig::Get().DebugTicks)
	{
		static std::map<int, int> lastLog;
		int const now = Unsorted::CurrentFrame;
		auto const it = lastLog.find(hIdx);
		if (it == lastLog.end() || now - it->second >= 150)
		{
			lastLog[hIdx] = now;
			Debug::Log("[DoctrineExt] defenders house=%s#%d fan bearing=%.2frad "
				"(%s%d)\n", pHouse->get_ID(), hIdx, baseAngle,
				laneStrength > 0 ? "lane strength=" : "fallback, no lane s=",
				laneStrength);
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

bool Teams::HasActiveDecap(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	char id[0x18];
	for (auto const& key : g_decapSlots)
	{
		if (key.first != hIdx) continue;
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, key.second);
		auto const pTeam = TeamTypeClass::Find(id);
		if (pTeam && pTeam->cntInstances > 0)
			return true;
	}
	return false;
}

void Teams::SteerDecap(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	// Re-acquire the current priority structure each tick: as the ConYard falls
	// the target advances to the war factory, then economy, etc. — the team
	// walks down the rebuild chain rather than fixating on a dead building.
	auto const pEnemy = WeakestEnemy(pHouse);
	auto const pBld = pEnemy ? DecapTarget(pHouse, pEnemy) : nullptr;
	if (!pBld) return; // no target, or all in killboxes — hold, don't feed the farm

	for (auto const& key : g_decapSlots)
	{
		if (key.first != hIdx) continue;
		char id[0x18];
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, key.second);
		auto const pType = TeamTypeClass::Find(id);
		if (!pType || pType->cntInstances <= 0) continue;
		auto const pTeam = pType->FindFirstInstance();
		if (!pTeam) continue;
		pTeam->AssignMissionTarget(pBld);
		for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
		{
			if (pFoot->InLimbo || pFoot->Health <= 0) continue;
			pFoot->SetTarget(pBld);
			pFoot->QueueMission(Mission::Attack, false);
		}
	}
}

bool Teams::HasActiveHunt(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	char id[0x18];
	for (auto const& key : g_huntSlots)
	{
		if (key.first != hIdx) continue;
		std::snprintf(id, sizeof(id), "DCTR%d_%dTM", hIdx, key.second);
		auto const pTeam = TeamTypeClass::Find(id);
		if (pTeam && pTeam->cntInstances > 0)
			return true;
	}
	return false;
}

void Teams::SteerHunters(HouseClass* pHouse, TechnoClass* pTarget)
{
	if (!pTarget) return;
	int const hIdx = pHouse->ArrayIndex;

	// Weak-point approach (§10b 6c): instead of charging straight at the target
	// (into its supporters and the killbox), approach from its WEAK side — the
	// bearing away from where its supporting units mass — and avoid our own
	// death-zone buckets on the way in.
	auto const tc = pTarget->GetCoords();

	// Support mass: sum unit-vectors from the target to nearby enemy units. The
	// weak side is opposite that mass.
	double sx = 0.0, sy = 0.0;
	int supporters = 0;
	int const scanCells = DoctrineConfig::Get().SupportScanRadius > 0
		? DoctrineConfig::Get().SupportScanRadius : 8;
	double const scanLep = scanCells * 256.0;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT2 = TechnoClass::Array.GetItem(i);
		if (!pT2 || pT2 == pTarget || pT2->InLimbo || pT2->Health <= 0) continue;
		auto const pO = pT2->Owner;
		if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;
		auto const c = pT2->GetCoords();
		double const dxx = c.X - tc.X, dyy = c.Y - tc.Y;
		double const d = std::sqrt(dxx * dxx + dyy * dyy);
		if (d < 1.0 || d > scanLep) continue;
		sx += dxx / d; sy += dyy / d;
		++supporters;
	}

	double wdx, wdy; // weak-side unit vector (direction to approach FROM)
	double const sl = std::sqrt(sx * sx + sy * sy);
	if (supporters > 0 && sl > 0.01)
	{
		wdx = -sx / sl; wdy = -sy / sl; // opposite the support mass
	}
	else
	{
		// No support read: approach from our own base side (the safe home lane).
		auto const bc = CellClass::Cell2Coord(pHouse->GetBaseCenter());
		double bx = bc.X - tc.X, by = bc.Y - tc.Y;
		double const bl = std::sqrt(bx * bx + by * by);
		if (bl < 1.0) return;
		wdx = bx / bl; wdy = by / bl;
	}

	int const standoffCells = DoctrineConfig::Get().HuntStandoff > 0
		? DoctrineConfig::Get().HuntStandoff : 5;
	double const standoff = standoffCells * 256.0;

	// Try the weak bearing; if the approach point sits in a hot death-zone
	// bucket, rotate the approach around the target until it's clear.
	static const double kRot[] = { 0.0, 0.52, -0.52, 1.05, -1.05, 1.57, -1.57 };
	CellClass* pCell = nullptr;
	for (double const rot : kRot)
	{
		double const ca = std::cos(rot), sa = std::sin(rot);
		double const rx = wdx * ca - wdy * sa;
		double const ry = wdx * sa + wdy * ca;
		CoordStruct pt = tc;
		pt.X = tc.X + static_cast<int>(rx * standoff);
		pt.Y = tc.Y + static_cast<int>(ry * standoff);
		auto const pC = MapClass::Instance.TryGetCellAt(pt);
		if (!pC) continue;
		CellStruct cs; cs.X = static_cast<short>(pt.X / 256); cs.Y = static_cast<short>(pt.Y / 256);
		if (DeathZones::ScoreAtCell(pHouse, cs.X, cs.Y)
			< DoctrineConfig::Get().DeathZoneMinStrength)
		{
			pCell = pC; // clear of the killbox
			break;
		}
		if (!pCell) pCell = pC; // remember first valid as fallback
	}
	if (!pCell) return;

	for (auto const& key : g_huntSlots)
		if (key.first == hIdx)
			MoveDoctrineTeam(hIdx, key.second, pCell);
}

namespace
{
	std::set<std::pair<int, void*>> g_prereqAudited;

	// Which of pType's SPECIFIC (positive) prerequisite buildings the house is
	// missing right now. Generic prereqs (POWER/RADAR/TECH... encoded negative)
	// are skipped — the concrete building check is the decisive signal (e.g. a
	// SEAL owner with no GAPILE). Returns the missing IDs joined for the log.
	bool MissingPrereqBuildings(HouseClass* const pHouse, TechnoTypeClass* const pType,
		std::string& out)
	{
		bool missing = false;
		for (int pre : pType->Prerequisite)
		{
			if (pre < 0 || pre >= BuildingTypeClass::Array.Count) continue;
			auto const bt = BuildingTypeClass::Array.GetItem(pre);
			if (!bt) continue;
			if (pHouse->CountOwnedAndPresent(bt) <= 0)
			{
				missing = true;
				if (!out.empty()) out += ",";
				out += bt->ID;
			}
		}
		return missing;
	}
}

void Teams::PrereqAudit(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	int const frame = Unsorted::CurrentFrame;
	int const maxTL = (pHouse->TechLevel >= 0 && pHouse->TechLevel <= 10)
		? pHouse->TechLevel : 10;

	// Flag only a GENUINE illegality, filtering the benign cases the first pass
	// exposed: harvesters/miners (ResourceGatherer), MCVs (DeploysInto), the
	// starting ConYard (ConstructionYard), and build-limited units (CanBuild=-1
	// = TemporarilyUnbuildable, e.g. a hero at its limit). What remains:
	//  · TechLevel over the game max (e.g. TechLevel=11 owned anyway), or
	//  · CanBuild==0 (permanently unbuildable) AND a concrete prereq building
	//    missing = built/obtained without its prerequisite (the SEAL case).
	auto auditType = [&](TechnoTypeClass* const pType, int const owned, bool const isBuilding)
	{
		if (!pType || owned <= 0) return;
		auto const key = std::make_pair(hIdx, static_cast<void*>(pType));
		if (g_prereqAudited.count(key)) return;

		if (pType->ResourceGatherer) return;                 // free harvesters
		if (!isBuilding && pType->DeploysInto) return;       // MCV-type
		if (isBuilding && static_cast<BuildingTypeClass*>(pType)->ConstructionYard)
			return;                                          // starting ConYard

		int const cb = static_cast<int>(pHouse->CanBuild(pType, false, true));
		bool const techViolation = pType->TechLevel > maxTL; // e.g. TechLevel=11
		std::string miss;
		bool const prereqBypass = cb == static_cast<int>(CanBuildResult::Unbuildable)
			&& MissingPrereqBuildings(pHouse, pType, miss);
		if (!techViolation && !prereqBypass) return;         // benign / build-limited

		g_prereqAudited.insert(key);
		Debug::Log("[DoctrineExt] PREREQ-AUDIT: house=%s#%d frame=%d %s %s x%d "
			"TechLevel=%d (maxTL=%d) CanBuild=%d%s%s missing=[%s]\n",
			pHouse->get_ID(), hIdx, frame, isBuilding ? "STRUCT" : "unit",
			pType->ID, owned, pType->TechLevel, maxTL, cb,
			techViolation ? " TECHLEVEL" : "",
			prereqBypass ? " PREREQ-BYPASS" : "", miss.c_str());
	};

	for (auto const pType : InfantryTypeClass::Array) auditType(pType, CountOwned(pHouse, pType), false);
	for (auto const pType : UnitTypeClass::Array)     auditType(pType, CountOwned(pHouse, pType), false);
	for (auto const pType : AircraftTypeClass::Array) auditType(pType, CountOwned(pHouse, pType), false);
	for (auto const pType : BuildingTypeClass::Array)
		auditType(pType, pHouse->CountOwnedAndPresent(pType), true);
}

namespace
{
	std::map<int, int> g_crateLastFire;
	// Per-house cached crate targets {member, crate cell} from the throttled
	// detection pass. SteerCrateSquad re-issues the Move every base tick so the
	// base AI can't countermand it between the 90-frame detection passes (the
	// reason raced=1 fired for ages yet no crate was ever collected).
	std::map<int, std::vector<std::pair<FootClass*, CellClass*>>> g_squadTargets;
	// Give-up guard: per-house per-crate-cell {lastDistLep, stuckPasses} progress,
	// and a blacklist {cellKey -> expiryFrame} of crates deemed unreachable (a
	// chaser made no progress toward them, e.g. across a cliff). Keyed by cell.
	std::map<int, std::map<int, std::pair<int, int>>> g_crateProgress;
	std::map<int, std::map<int, int>> g_crateBlacklist;
	// Catcher-build give-up: per-house per-catcher-ID count of passes it was ordered
	// yet never appeared (leaky CanBuild greenlit something the factory refuses).
	// Past CrateCatcherGiveUp, BestCrateCatcher skips it and picks the next-best.
	std::map<int, std::map<std::string, int>> g_catcherFail;
	// Latched max dedicated-catcher count any AI house has owned this scenario, fed by
	// CrateSquadDoctrine each pass. The AITrigger gate reads it to release once the AI
	// has built enough chasers (CrateChaserGateCount). Latched (never decreases) so the
	// gate stays open once reached — we don't want to re-suppress mid-game.
	int g_chaserGateMax = 0;

	int CrateKey(CellClass* const pCell)
	{
		auto const c = pCell->GetCellCoords();
		return ((c.X / 256) << 16) | ((c.Y / 256) & 0xFFFF);
	}

	// Does the straight route from `from` to the crate cross a proven killbox
	// (death zone)? Sample a few points along the line. Stop chasing crates whose
	// path is dangerous (Rex's rule 1).
	bool CratePathDangerous(HouseClass* const pHouse, CoordStruct const& from, CellClass* const pCell)
	{
		auto const to = pCell->GetCellCoords();
		int const thresh = DoctrineConfig::Get().DeathZoneMinStrength > 0
			? DoctrineConfig::Get().DeathZoneMinStrength : 48;
		for (int s = 1; s <= 5; ++s)
		{
			double const t = s / 6.0;
			int const x = static_cast<int>((from.X + (to.X - from.X) * t) / 256.0);
			int const y = static_cast<int>((from.Y + (to.Y - from.Y) * t) / 256.0);
			if (DeathZones::ScoreAtCell(pHouse, x, y) >= thresh) return true;
		}
		return false;
	}

	// Cede the crate only if an enemy is BOTH closer than us AND actually heading to
	// it (its move Destination sits on the crate) — i.e. genuinely racing us for it
	// (Rex's rule 2). If an enemy is merely nearby but not going for it, snag it
	// anyway (quick grab). We can read move orders via FootClass::Destination.
	bool CrateEnemyRacing(HouseClass* const pHouse, CellClass* const pCell, double const ourDist)
	{
		auto const cc = pCell->GetCellCoords();
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pO = pT->Owner;
			if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue; // enemy only
			auto const c = pT->GetCoords();
			double const d = std::sqrt(double(cc.X - c.X) * (cc.X - c.X)
				+ double(cc.Y - c.Y) * (cc.Y - c.Y));
			if (d >= ourDist) continue; // not closer — we can still beat it
			auto const pDest = static_cast<FootClass*>(pT)->Destination;
			if (!pDest) continue; // closer but idle / not moving to it — snag it anyway
			auto const dc = pDest->GetCoords();
			double const dd = std::sqrt(double(cc.X - dc.X) * (cc.X - dc.X)
				+ double(cc.Y - dc.Y) * (cc.Y - dc.Y));
			if (dd <= 3.0 * 256.0) return true; // closer AND heading to the crate — cede
		}
		return false;
	}

	bool IsCrateOverlay(int const idx)
	{
		if (idx < 0 || idx >= OverlayTypeClass::Array.Count) return false;
		auto const pO = OverlayTypeClass::Array.GetItem(idx);
		return pO && pO->Crate;
	}

	// Nearest crate cell within radius (cells) of a coord; scans a bounded box
	// (crates are sparse and this is throttled, so a local scan is cheap).
	CellClass* NearestCrate(const CoordStruct& from, int const radiusCells)
	{
		CellStruct const c0 = CellClass::Coord2Cell(from);
		CellClass* best = nullptr;
		int bestD2 = radiusCells * radiusCells + 1;
		for (int dy = -radiusCells; dy <= radiusCells; ++dy)
			for (int dx = -radiusCells; dx <= radiusCells; ++dx)
			{
				int const d2 = dx * dx + dy * dy;
				if (d2 >= bestD2) continue;
				CellStruct cs;
				cs.X = static_cast<short>(c0.X + dx);
				cs.Y = static_cast<short>(c0.Y + dy);
				auto const pCell = MapClass::Instance.TryGetCellAt(cs);
				if (!pCell || !IsCrateOverlay(pCell->OverlayTypeIndex)) continue;
				best = pCell;
				bestD2 = d2;
			}
		return best;
	}

	// The house's best available crate grabber: prefer the modder's CrateChasers
	// list, else the fastest mobile unit. Prefers teamless units, but the AI
	// teams ~every unit (verified via garrison diag), so a teamed unit is a
	// fallback — the caller LiberateMembers it before use. Returns null if none.
	FootClass* PickChaser(HouseClass* const pHouse)
	{
		auto const& chasers = DoctrineConfig::Get().CrateChasers;
		FootClass* bySpeedFree = nullptr; int bestSpeedFree = -1;
		FootClass* byListFree = nullptr;  int bestListRankFree = 1 << 30;
		FootClass* bySpeedTeam = nullptr; int bestSpeedTeam = -1;
		FootClass* byListTeam = nullptr;  int bestListRankTeam = 1 << 30;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pFoot = static_cast<FootClass*>(pT);
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer) continue;
			if (IsExcludedChaser(pType)) continue; // modder-protected (e.g. heroes)
			bool const teamed = (pFoot->Team != nullptr);
			// The modder list may name anything; the SPEED fallback only takes a
			// real combat unit (has a weapon) so it never grabs a dummy/spawner
			// helper (e.g. DRONEDUMMY2) that can't actually collect the crate.
			if (HasOffensiveWeapon(pType))
			{
				if (!teamed && pType->Speed > bestSpeedFree)
					{ bestSpeedFree = pType->Speed; bySpeedFree = pFoot; }
				else if (teamed && pType->Speed > bestSpeedTeam)
					{ bestSpeedTeam = pType->Speed; bySpeedTeam = pFoot; }
			}
			for (int r = 0; r < static_cast<int>(chasers.size()); ++r)
			{
				if (chasers[r].ID != pType->ID) continue;
				if (!teamed && r < bestListRankFree) { bestListRankFree = r; byListFree = pFoot; }
				else if (teamed && r < bestListRankTeam) { bestListRankTeam = r; byListTeam = pFoot; }
			}
		}
		// A modder-listed grabber wins if we own one (their explicit choice), free
		// preferred. Otherwise take the FASTEST unit overall — free or teamed — so
		// crate chasers are genuinely the fastest units (Rex), not just whatever
		// teamless body happened to be lying around (which picked slow conscripts).
		if (byListFree) return byListFree;
		if (byListTeam) return byListTeam;
		if (bySpeedFree && (!bySpeedTeam || bestSpeedFree >= bestSpeedTeam))
			return bySpeedFree;
		return bySpeedTeam;
	}

	// Can this house recover an MCV normally (owns a ConYard, owns an MCV, or can
	// build one)? If not, its only route back is the FreeMCV crate.
	bool CanRecoverMCV(HouseClass* const pHouse)
	{
		int buildings = 0, conyards = 0;
		for (auto const pBt : BuildingTypeClass::Array)
		{
			int const n = pHouse->CountOwnedNow(pBt);
			buildings += n;
			if (n > 0 && pBt->ConstructionYard) conyards += n;
		}
		if (conyards > 0) return true;
		for (auto const pUt : UnitTypeClass::Array)
		{
			if (!pUt || !pUt->DeploysInto) continue; // MCV-type
			if (pHouse->CountOwnedAndPresent(pUt) > 0) return true; // owns an MCV
			if (CanBuildStrict(pHouse, pUt)) return true;           // can build one
		}
		return false;
	}

	int OwnedBuildingCount(HouseClass* const pHouse)
	{
		int n = 0;
		for (auto const pBt : BuildingTypeClass::Array)
			n += pHouse->CountOwnedNow(pBt);
		return n;
	}

	// The house's dedicated crate-grab team (ID "DCRG<house>*"), separate from
	// the combat slot pool. A loose Move order gets countermanded by the base
	// AI's own unit handling every frame; a TEAM member is driven by the team,
	// not the loose-unit AI, so it actually reaches the crate (same reason
	// intercept/hunt/defend steering sticks). Creates the trio + team on first
	// use and recruits one grabber if empty. Returns the live team, or null.
	TeamClass* EnsureCrateTeam(HouseClass* const pHouse, FootClass* const pChaser)
	{
		int const hIdx = pHouse->ArrayIndex;
		char id[0x18];
		std::snprintf(id, sizeof(id), "DCRG%dTM", hIdx);
		auto pTT = TeamTypeClass::Find(id);
		if (!pTT) pTT = GameCreate<TeamTypeClass>(id);
		if (!pTT) return nullptr;
		std::snprintf(id, sizeof(id), "DCRG%dTF", hIdx);
		auto pTF = TaskForceClass::Find(id);
		if (!pTF) pTF = GameCreate<TaskForceClass>(id);
		std::snprintf(id, sizeof(id), "DCRG%dSC", hIdx);
		auto pSC = ScriptTypeClass::Find(id);
		if (!pSC) pSC = GameCreate<ScriptTypeClass>(id);
		if (!pTF || !pSC) return nullptr;

		auto const pType = pChaser ? pChaser->GetTechnoType() : nullptr;
		pTF->CountEntries = 1;
		pTF->Entries[0] = { 1, pType };
		pTF->Group = -1;
		pSC->ActionsCount = 1;
		pSC->ScriptActions[0] = { 5, 120 }; // guard; we steer to the crate each tick
		pTT->TaskForce = pTF;
		pTT->ScriptType = pSC;
		pTT->Max = 1;
		pTT->Owner = nullptr;
		pTT->idxHouse = -1;
		pTT->Autocreate = false;
		pTT->Prebuild = false;
		pTT->Reinforce = false;
		pTT->Recruiter = false;   // one hand-picked grabber, no auto-fill
		pTT->LooseRecruit = false;
		pTT->AreTeamMembersRecruitable = false;
		pTT->IsBaseDefense = false;
		pTT->Full = false;
		pTT->Aggressive = false;
		pTT->Loadable = false;
		pTT->Suicide = false;
		pTT->Whiner = false;
		pTT->Annoyance = false;
		pTT->GuardSlower = false;
		pTT->Droppod = false;
		pTT->OnTransOnly = false;

		auto const pTeam = pTT->cntInstances > 0 ? pTT->FindFirstInstance()
			: pTT->CreateTeam(pHouse);
		if (!pTeam) return nullptr;
		// Recruit the grabber only if the team is empty (keep it to one unit).
		if (!pTeam->FirstUnit && pChaser && !pChaser->Team)
			pTeam->AddMember(pChaser, true);
		return pTeam;
	}
}

namespace
{
	std::map<int, int> g_garrisonLastFire;

	// True if any cell within `nearCells` of `c` holds ore/gems (§10e-2 ore
	// criterion — occupy buildings that overlook the enemy's, or our own,
	// income). Small box scan, throttled by the garrison interval.
	bool OreNear(CoordStruct const& c, int nearCells)
	{
		CellStruct const c0 = CellClass::Coord2Cell(c);
		for (int dy = -nearCells; dy <= nearCells; ++dy)
			for (int dx = -nearCells; dx <= nearCells; ++dx)
			{
				CellStruct cs;
				cs.X = static_cast<short>(c0.X + dx);
				cs.Y = static_cast<short>(c0.Y + dy);
				auto const pCell = MapClass::Instance.TryGetCellAt(cs);
				if (pCell && pCell->GetContainedTiberiumValue() > 0) return true;
			}
		return false;
	}

	// True if a capturable neutral tech structure sits within `nearCells` of `c`
	// (§10e-2 tech criterion — garrisoning nearby denies/guards the capture).
	bool TechNear(CoordStruct const& c, int nearCells)
	{
		double const nearLep = nearCells * 256.0;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || pB->InLimbo || pB->Health <= 0) continue;
			auto const bt = pB->Type;
			if (!bt || !bt->Capturable) continue;
			if (!pB->Owner || !pB->Owner->IsNeutral()) continue;
			auto const bc = pB->GetCoords();
			double const dx = bc.X - c.X, dy = bc.Y - c.Y;
			if (std::sqrt(dx * dx + dy * dy) <= nearLep) return true;
		}
		return false;
	}

	// One vacant slot to fill, with its building's priority score. We expand a
	// building into `free` entries so distribution across the top-scored
	// buildings is slot-aware (occupiers spread, don't all pile on one).
	struct GarrisonSlot { BuildingClass* pB; double score; bool bunker; };

	// True if a unit is on one of OUR doctrine teams (IDs DCTR/DCRG/DCRS/DGAR…),
	// so other doctrine modules don't steal each other's units — e.g. garrison
	// must not LiberateMember the crate squad's grabber out from under it.
	bool IsDoctrineTeam(TeamClass* const pTeam)
	{
		if (!pTeam || !pTeam->Type) return false;
		auto const id = pTeam->Type->ID;
		return id[0] == 'D' &&
			((id[1] == 'C' && (id[2] == 'T' || id[2] == 'R' || id[2] == 'M')) // DCTR/DCRG/DCRS/DCMD
			|| (id[1] == 'G' && id[2] == 'A')                   // DGAR
			|| (id[1] == 'A' && id[2] == 'A')                   // DAA (air-defense screen)
			|| (id[1] == 'P' && id[2] == 'K'));                 // DPKT (engineer pickets)
	}
}

void Teams::GarrisonDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.GarrisonInfantry) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.GarrisonInterval > 0 ? cfg.GarrisonInterval : 60;
	auto const it = g_garrisonLastFire.find(hIdx);
	if (it != g_garrisonLastFire.end() && now - it->second < interval) return;
	g_garrisonLastFire[hIdx] = now;

	// Creep outward over time: the search radius grows from RadiusStart to the
	// Radius cap at one cell per CreepRate frames. Early = hold the base; late =
	// reach deeper. The cap also BOUNDS production to the house's turf (the fix
	// for the map-wide 830-slot runaway occupier flood).
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const maxR = cfg.GarrisonRadius > 0 ? cfg.GarrisonRadius : 30;
	int const startR = cfg.GarrisonRadiusStart > 0 ? cfg.GarrisonRadiusStart : 10;
	int const rate = cfg.GarrisonCreepRate;
	int effR = (rate > 0) ? startR + now / rate : maxR;
	if (effR > maxR) effR = maxR;
	if (effR < startR) effR = startR;
	double const effLep = effR * 256.0;
	int const nearCells = cfg.GarrisonScanNear > 0 ? cfg.GarrisonScanNear : 6;

	// Score every vacant occupiable building inside the creep band. The weights
	// ARE the modder's order of operations ([Doctrine.Garrison]): perimeter
	// (close to base), creep (far out), ore, tech.
	std::vector<GarrisonSlot> slots;
	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		auto const pB = BuildingClass::Array.GetItem(i);
		if (!pB || pB->InLimbo || pB->Health <= 0) continue;
		auto const bt = pB->Type;
		if (!bt || !bt->CanBeOccupied) continue;
		int const free = bt->MaxNumberOccupants - pB->GetOccupantCount();
		if (free <= 0) continue;
		bool const bunker = (pB->Owner == pHouse);
		bool const neutral = (pB->Owner && pB->Owner->IsNeutral());
		if (!bunker && !neutral) continue; // ours (bunker) or a neutral city block

		auto const bc = pB->GetCoords();
		double const dx = bc.X - base.X, dy = bc.Y - base.Y;
		double const d = std::sqrt(dx * dx + dy * dy);
		if (d > effLep) continue; // outside the current creep band

		double const prox = 1.0 - d / effLep;             // 1 at base -> 0 at edge
		double const creep = d / effLep;                  // 0 at base -> 1 at edge
		double const ore = OreNear(bc, nearCells) ? 1.0 : 0.0;
		double const tech = TechNear(bc, nearCells) ? 1.0 : 0.0;
		double const score = cfg.GPerimeterW * prox + cfg.GCreepW * creep
			+ cfg.GOreW * ore + cfg.GTechW * tech;

		for (int s = 0; s < free; ++s) slots.push_back({ pB, score, bunker });
	}
	if (slots.empty()) return; // nothing in reach to fill

	std::sort(slots.begin(), slots.end(),
		[](GarrisonSlot const& a, GarrisonSlot const& b) { return a.score > b.score; });

	// Gather this house's occupier infantry, teamless first then teamed. The AI
	// puts ~every infantry on a combat team (verified: teamless=0, occAll=73),
	// so to garrison at all we must DIVERT some off their teams — gently, capped
	// by MaxDivert. Diag: allInf = infantry owned, occAll = Occupier=yes of
	// those, teamlessN = of those not on a team.
	int allInf = 0, occAll = 0, teamlessN = 0;
	std::vector<FootClass*> occFree;  // teamless: use freely
	std::vector<FootClass*> occTeam;  // teamed: liberate if we still need more
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
		if (pT->WhatAmI() != AbstractType::Infantry) continue;
		++allInf;
		auto const itc = static_cast<InfantryTypeClass*>(pT->GetTechnoType());
		if (!itc || !itc->Occupier) continue;
		++occAll;
		auto const pFoot = static_cast<FootClass*>(pT);
		if (pFoot->Team)
		{
			if (!IsDoctrineTeam(pFoot->Team)) occTeam.push_back(pFoot); // don't
			// steal crate-squad / combat-team members off our own teams
		}
		else { ++teamlessN; occFree.push_back(pFoot); }
	}

	// Build the working list: all teamless, then teamed up to MaxDivert (we'll
	// LiberateMember those before use). Bounded so we never gut the AI's army.
	int const maxDivert = cfg.GarrisonMaxDivert > 0 ? cfg.GarrisonMaxDivert : 4;
	std::vector<FootClass*> occ = occFree;
	int divert = 0;
	for (auto const pFoot : occTeam)
	{
		if (static_cast<int>(occ.size()) >= static_cast<int>(slots.size())) break;
		if (divert >= maxDivert) break;
		occ.push_back(pFoot);
		++divert;
	}

	// ENTRY: set the engine's own garrison flags and let IT path + distribute +
	// fill each building to capacity. An earlier build steered each occupier at a
	// specific scored building with Move orders; that FOUGHT the flag (which goes
	// to "closest") and, re-assigned by index every pass, made the conscripts
	// walk back and forth and never enter. Flags-only is the vanilla mechanism
	// and doesn't thrash. The creep radius still governs priority: occupiers only
	// exist to garrison within effR, so near-base fills first and the band widens
	// over time (perimeter + creep). ShouldEnterOccupiable routes to our battle
	// bunkers, ShouldGarrisonStructure to neutral city buildings.
	int ownedVac = 0, neutralVac = 0;
	for (auto const& s : slots) { if (s.bunker) ++ownedVac; else ++neutralVac; }
	int sent = 0;
	for (auto const pFoot : occ)
	{
		if (pFoot->Team) pFoot->Team->LiberateMember(pFoot); // free it to garrison
		if (ownedVac > 0)   pFoot->ShouldEnterOccupiable = true;
		if (neutralVac > 0) pFoot->ShouldGarrisonStructure = true;
		++sent;
	}

	// Produce occupiers only while in-band slots outnumber the occupiers we OWN
	// (occAll, not teamless — the AI already has plenty, we just divert them).
	// Bounded by the creep radius, so production can't run away map-wide.
	int queued = 0;
	const char* prodBest = "none";  // diag: what we picked to build
	bool facFound = false;          // diag: did FindHouseFactory return one
	if (occAll < static_cast<int>(slots.size()))
	{
		TechnoTypeClass* pBest = nullptr;
		double bestDps = -1.0;
		for (auto const pIt : InfantryTypeClass::Array)
		{
			if (!pIt || !pIt->Occupier) continue;
			if (!CanBuildStrict(pHouse, pIt)) continue;
			double const dps = RawDPS(pIt);        // prefer the stronger occupier
			if (dps > bestDps) { bestDps = dps; pBest = pIt; }
		}
		if (pBest)
		{
			prodBest = pBest->get_ID();
			if (auto const pFactory = FindHouseFactory(pHouse, pBest))
			{
				facFound = true;
				int const cap = cfg.GarrisonMaxProduce > 0 ? cfg.GarrisonMaxProduce : 2;
				int want = static_cast<int>(slots.size()) - occAll;
				if (want > cap) want = cap;
				for (int k = 0; k < want; ++k) { pFactory->DemandProduction(pBest, pHouse, true); ++queued; }
			}
		}
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] garrison: house=%s#%d effR=%d inband-slots=%d "
			"allInf=%d occAll=%d teamless=%d divert=%d sent=%d prod=%s fac=%d queued=%d.\n",
			pHouse->get_ID(), hIdx, effR, static_cast<int>(slots.size()),
			allInf, occAll, teamlessN, divert, sent, prodBest, facFound, queued);
}

void Teams::CrateDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CrateChase && !cfg.CrateFiresaleMCV) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.CrateInterval > 0 ? cfg.CrateInterval : 90;
	auto const it = g_crateLastFire.find(hIdx);
	if (it != g_crateLastFire.end() && now - it->second < interval) return;
	g_crateLastFire[hIdx] = now; // throttle the SCAN too, crate found or not

	auto const baseCoord = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	auto const pCrate = NearestCrate(baseCoord, cfg.CrateScanRadius > 0 ? cfg.CrateScanRadius : 30);
	if (!pCrate) return; // no crate nearby

	// Put a grabber on the dedicated crate team (only picks a new one if the
	// team is empty); SteerCrate drives it to the crate each tick. Loose Move
	// orders were being countermanded by the base AI, so the team is essential.
	char teamId[0x18];
	std::snprintf(teamId, sizeof(teamId), "DCRG%dTM", hIdx);
	auto const pExisting = TeamTypeClass::Find(teamId);
	bool const haveGrabber = pExisting && pExisting->cntInstances > 0
		&& pExisting->FindFirstInstance() && pExisting->FindFirstInstance()->FirstUnit;

	FootClass* pChaser = nullptr;
	if (!haveGrabber)
	{
		pChaser = PickChaser(pHouse);
		if (!pChaser)
		{
			if (cfg.DebugTicks)
				Debug::Log("[DoctrineExt] crate near %s#%d but no armed grabber at all.\n",
					pHouse->get_ID(), hIdx);
			return;
		}
		if (pChaser->Team) pChaser->Team->LiberateMember(pChaser); // divert off AI team
		EnsureCrateTeam(pHouse, pChaser);
	}

	// The grabber whose position gates the firesale (existing team member, or
	// the one just recruited).
	FootClass* pGrabber = pChaser;
	if (!pGrabber && pExisting && pExisting->FindFirstInstance())
		pGrabber = pExisting->FindFirstInstance()->FirstUnit;
	if (!pGrabber) return;

	// Comeback: with no way to get an MCV and a long game, sell everything so the
	// crate yields the guaranteed FreeMCV (needs zero buildings + money). Wait
	// until the grabber is within CrateFiresaleDist so the base drops just as it
	// arrives, not while it's still travelling and defenceless. ShortGame off
	// only — in a Quick Game losing your base loses you.
	if (cfg.CrateFiresaleMCV)
	{
		bool const canRecover = CanRecoverMCV(pHouse);
		int const buildings = OwnedBuildingCount(pHouse);
		int const money = static_cast<int>(pHouse->Available_Money());
		bool const shortGame = Unsorted::ShortGame != 0;
		auto const cc = pGrabber->GetCoords();
		auto const cr = pCrate->GetCellCoords();
		double const dist = std::sqrt(double(cc.X - cr.X) * (cc.X - cr.X)
			+ double(cc.Y - cr.Y) * (cc.Y - cr.Y));
		double const trigger = (cfg.CrateFiresaleDist > 0 ? cfg.CrateFiresaleDist : 10) * 256.0;

		// Readiness trace: shows every factor so we can see how close the AI is
		// to the comeback and confirm each condition reads correctly, even in
		// games where it never actually fires.
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] crate comeback check: house=%s#%d canRecoverMCV=%d "
				"buildings=%d money=%d shortGame=%d grabberDist=%.0f (trigger<=%.0f)\n",
				pHouse->get_ID(), hIdx, canRecover, buildings, money, shortGame, dist, trigger);

		if (!shortGame && !canRecover && buildings > 0 && money > 0 && dist <= trigger)
		{
			Debug::Log("[DoctrineExt] crate firesale-for-MCV: house=%s#%d grabber %.0f "
				"leptons out (<= %.0f) — selling all for a free-MCV crate.\n",
				pHouse->get_ID(), hIdx, dist, trigger);
			pHouse->Fire_Sale();
		}
	}

	Debug::Log("[DoctrineExt] crate chase: house=%s#%d grabber %s en route to a crate.\n",
		pHouse->get_ID(), hIdx,
		pGrabber->GetTechnoType() ? pGrabber->GetTechnoType()->ID : "?");
}

bool Teams::HasActiveCrate(HouseClass* pHouse)
{
	char id[0x18];
	std::snprintf(id, sizeof(id), "DCRG%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	return pTT && pTT->cntInstances > 0;
}

void Teams::SteerCrate(HouseClass* pHouse)
{
	char id[0x18];
	std::snprintf(id, sizeof(id), "DCRG%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	if (!pTT || pTT->cntInstances <= 0) return;
	auto const pTeam = pTT->FindFirstInstance();
	if (!pTeam || !pTeam->FirstUnit) return;

	// Track the nearest crate to the grabber (a bit wider than the base scan so
	// it keeps homing as it travels); when the crate's gone (collected or taken)
	// disband so the unit returns to the AI.
	int const r = (DoctrineConfig::Get().CrateScanRadius > 0
		? DoctrineConfig::Get().CrateScanRadius : 30) * 2;
	auto const pCrate = NearestCrate(pTeam->FirstUnit->GetCoords(), r);
	if (!pCrate)
	{
		pTT->DestroyAllInstances();
		return;
	}
	for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
	{
		if (pFoot->InLimbo || pFoot->Health <= 0) continue;
		pFoot->SetDestination(pCrate, true);
		pFoot->QueueMission(Mission::Move, false);
	}
}

namespace
{
	// Squad size from [CrateRules]: one racer per expected concurrent crate,
	// +1 for fast regen, capped so CrateMaximum=1 => a single racer and a busy
	// map (high CrateMinimum) => a bigger squad. CrateSquadSize>0 overrides.
	// The best DESIGNATED grabber (from CrateChasers) the house can build right now,
	// with a quality tier: 3 = teleporter (CLEG — instant grab), 2 = fast, 1 = slow.
	// outTier=0 and null if none is buildable. Only considers the modder's list (a
	// "designated" catcher), respects TechLevel/Owner/prereqs and the exclude list.
	TechnoTypeClass* BestCrateCatcher(HouseClass* const pHouse, int& outTier)
	{
		outTier = 0;
		TechnoTypeClass* best = nullptr;
		auto const& cfg = DoctrineConfig::Get();
		int const giveUp = cfg.CrateCatcherGiveUp > 0 ? cfg.CrateCatcherGiveUp : 6;
		auto const& fails = g_catcherFail[pHouse->ArrayIndex];
		for (auto const& e : cfg.CrateChasers)
		{
			auto const pTy = TechnoTypeClass::Find(e.ID.c_str());
			if (!pTy || IsExcludedChaser(pTy)) continue;
			if (pTy->ResourceGatherer || !HasOffensiveWeapon(pTy)) continue;
			if (!pHouse->InOwners(pTy)) continue; // faction-correct always (never a
			                                      // Soviet DRON for an Allied AI etc.),
			                                      // independent of StrictOwnership
			if (!CanBuildStrict(pHouse, pTy, cfg.HonorPrereqForCrate())) continue;
			auto const fit = fails.find(e.ID);
			if (fit != fails.end() && fit->second >= giveUp) continue; // proven unbuildable
			int const tier = pTy->Teleporter ? 3
				: (pTy->Speed >= cfg.CrateCatcherFastSpeed ? 2 : 1);
			// Prefer the better tier; among equal tiers prefer the actually-FASTEST unit
			// (Rex: "when more than one is buildable, pick the fastest"). The list stays
			// a progression — early only a dog is buildable, a teleporter wins once it is.
			if (tier > outTier || (tier == outTier && best && pTy->Speed > best->Speed))
				{ outTier = tier; best = pTy; }
		}
		return best;
	}

	int DesiredSquadSize()
	{
		auto const& cfg = DoctrineConfig::Get();
		auto const r = RulesClass::Instance;
		if (!r || !r->Crates) return 0;                 // crates disabled globally
		int const capCfg = cfg.CrateSquadMax > 0 ? cfg.CrateSquadMax : 6;
		if (cfg.CrateSquadSize > 0)                      // modder-fixed size
			return cfg.CrateSquadSize > capCfg ? capCfg : cfg.CrateSquadSize;
		int const maxC = r->CrateMaximum > 0 ? r->CrateMaximum : 255;
		int const minC = r->CrateMinimum > 0 ? r->CrateMinimum : 1;
		int concurrent = minC;                          // crates likely at once
		if (concurrent < 1) concurrent = 1;
		if (concurrent > maxC) concurrent = maxC;
		int size = concurrent;
		if (r->CrateRegen > 0.0 && r->CrateRegen <= 2.0) size += 1; // fast churn
		if (size > maxC) size = maxC;                   // never past what max allows
		if (size > capCfg) size = capCfg;
		if (size < 1) size = 1;
		return size;
	}

	// The house's PERSISTENT crate squad team (ID "<prefix><house>*", DCRS for the
	// ground squad / DCRW for the water squad). Created once and kept alive across
	// ticks (membership managed by hand, not auto-recruited); never destroyed on
	// idle, unlike the single-grabber team.
	TeamClass* EnsureSquadTeam(HouseClass* const pHouse, int const desired,
		TechnoTypeClass* const pRep, const char* const prefix = "DCRS",
		int const produceCount = 0)
	{
		int const hIdx = pHouse->ArrayIndex;
		char id[0x18];
		std::snprintf(id, sizeof(id), "%s%dTM", prefix, hIdx);
		auto pTT = TeamTypeClass::Find(id);
		if (!pTT) pTT = GameCreate<TeamTypeClass>(id);
		if (!pTT) return nullptr;
		std::snprintf(id, sizeof(id), "%s%dTF", prefix, hIdx);
		auto pTF = TaskForceClass::Find(id);
		if (!pTF) pTF = GameCreate<TaskForceClass>(id);
		std::snprintf(id, sizeof(id), "%s%dSC", prefix, hIdx);
		auto pSC = ScriptTypeClass::Find(id);
		if (!pSC) pSC = GameCreate<ScriptTypeClass>(id);
		if (!pTF || !pSC) return nullptr;

		// Production requirement: when pRep is set, the taskforce asks the AI to BUILD
		// `produceCount` of it (the only path that actually produces a unit for an AI
		// house — DemandProduction merely queues behind the AI's own plan and never
		// builds). pRep null / amount 0 = no production; membership comes from diversion.
		// Callers gate pRep by faction+prereq so the force-build can't pick a wrong unit.
		int const prod = pRep
			? (produceCount > 0 ? produceCount : (desired > 0 ? desired : 1)) : 0;
		pTF->CountEntries = pRep ? 1 : 0;
		pTF->Entries[0] = { prod, pRep };
		pTF->Group = -1;
		pSC->ActionsCount = 1;
		pSC->ScriptActions[0] = { 5, 120 }; // guard; we steer members each tick
		pTT->TaskForce = pTF;
		pTT->ScriptType = pSC;
		pTT->Max = desired > 0 ? desired : 1;
		pTT->Owner = nullptr;
		pTT->idxHouse = -1;
		pTT->Autocreate = false;
		pTT->Prebuild = false;
		pTT->Reinforce = false;
		pTT->Recruiter = false;         // we manage membership by hand
		pTT->LooseRecruit = false;
		pTT->AreTeamMembersRecruitable = false;
		pTT->IsBaseDefense = false;
		pTT->Full = false;
		pTT->Aggressive = false;
		pTT->Loadable = false;
		pTT->Suicide = false;
		pTT->Whiner = false;
		pTT->Annoyance = false;
		pTT->GuardSlower = false;
		pTT->Droppod = false;
		pTT->OnTransOnly = false;

		return pTT->cntInstances > 0 ? pTT->FindFirstInstance() : pTT->CreateTeam(pHouse);
	}

	// Priority for the DLL's hand-managed squads (capture, counter-engineer, rush). The engine's
	// recruit gate refuses a unit whose current team's Priority >= the recruiting team's, so this
	// lets AddMember take units off AI teams — and stops AI teams taking them back mid-task. Two of
	// our own squads tie, so they never poach from each other.
	constexpr int DoctrineSquadPriority = 1000000;

	// Move pFoot onto pTeam if the engine's recruit gate (TeamClass::AddMember 0x6EA500 → 0x6EA610)
	// will take it, checked FIRST so a refused unit stays on its old team instead of being stranded
	// teamless. With force=true the gate still refuses: a unit on a non-recruitable mission
	// ([MissionControl] Recruitable — Hunt, Sticky…), a non-recruitable unit (RecruitableA, cleared on
	// some map-placed objects; our squads aren't Autocreate so that's the flag checked), and one
	// inside a transport. AddMember itself liberates the unit from its old team once accepted.
	bool TryAddToTeam(TeamClass* const pTeam, FootClass* const pFoot)
	{
		if (!pTeam || !pTeam->Type || !pFoot || pFoot->Team == pTeam) return false;
		if (pFoot->InLimbo || pFoot->Health <= 0 || pFoot->Transporter) return false;
		if (!pFoot->RecruitableA) return false;
		auto const m = pFoot->GetCurrentMission();
		if (m != Mission::None && !MissionClass::IsRecruitableMission(m)) return false;
		pTeam->Type->Priority = DoctrineSquadPriority;
		if (!pTeam->AddMember(pFoot, true)) return false;
		pFoot->ShouldGarrisonStructure = false;                         // keep it on task
		pFoot->ShouldEnterOccupiable = false;
		return true;
	}

	// Is pFoot on one of the DLL's squads with this ID prefix ("DANT", "DRSH", …)?
	bool OnSquad(FootClass* const pFoot, const char* const prefix)
	{
		return pFoot && pFoot->Team && pFoot->Team->Type
			&& std::strncmp(pFoot->Team->Type->ID, prefix, std::strlen(prefix)) == 0;
	}

	double Dist2D(CoordStruct const& a, CoordStruct const& b)
	{
		return std::sqrt(static_cast<double>(a.X - b.X) * (a.X - b.X)
			+ static_cast<double>(a.Y - b.Y) * (a.Y - b.Y));
	}

	// Leptons from c to the nearest edge of the building's foundation — an engineer captures on
	// reaching the footprint, not the center. Built around GetCoords() (the foundation center).
	double DistToFootprint(CoordStruct const& c, BuildingClass* const pB)
	{
		auto const ctr = pB->GetCoords();
		double const hw = pB->Type->GetFoundationWidth() * 128.0;
		double const hh = pB->Type->GetFoundationHeight(false) * 128.0;
		double dx = std::fabs(static_cast<double>(c.X - ctr.X)) - hw;
		double dy = std::fabs(static_cast<double>(c.Y - ctr.Y)) - hh;
		if (dx < 0.0) dx = 0.0;
		if (dy < 0.0) dy = 0.0;
		return std::sqrt(dx * dx + dy * dy);
	}

	// Leptons per frame a unit moves (the locomotor's per-frame step), never below 1.
	double StepSpeed(FootClass* const pFoot)
	{
		int const s = pFoot ? pFoot->GetCurrentSpeed() : 0;
		return s > 1 ? static_cast<double>(s) : 1.0;
	}

	bool ListHas(std::vector<std::string> const& list, const char* const id)
	{
		for (auto const& s : list)
			if (s == id) return true;
		return false;
	}

	// Live members of a squad; engineersOnly counts just the engineers.
	int SquadCount(TeamClass* const pTeam, bool const engineersOnly)
	{
		int n = 0;
		for (auto pF = pTeam ? pTeam->FirstUnit : nullptr; pF; pF = pF->NextTeamMember)
		{
			if (pF->InLimbo || pF->Health <= 0) continue;
			if (engineersOnly)
			{
				auto const pType = pF->GetTechnoType();
				if (!pType || pType->WhatAmI() != AbstractType::InfantryType
					|| !static_cast<InfantryTypeClass*>(pType)->Engineer) continue;
			}
			++n;
		}
		return n;
	}

	// Best fast armed non-gatherer unit NOT already on pExclude, for squad
	// recruiting: prefers teamless (CrateChasers list, then fastest), falls back
	// to a teamed unit (caller liberates it) since the AI teams ~everything.
	bool IsWaterCapable(TechnoTypeClass* const pT2)
	{
		if (!pT2) return false;
		if (pT2->Naval) return true;
		auto const s = pT2->SpeedType;
		return s == SpeedType::Hover || s == SpeedType::Float
			|| s == SpeedType::Amphibious || s == SpeedType::FloatBeach;
	}

	// Speed of the house's fastest eligible mobile unit it currently OWNS (armed,
	// non-gatherer, not modder-excluded). Used by the catcher logic to decide whether
	// forcing a dedicated grabber is worth it: if a unit this fast already exists,
	// diverting it to crates beats spending production on a special catcher.
	int FastestOwnedEligibleSpeed(HouseClass* const pHouse)
	{
		int best = 0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer) continue;
			if (IsExcludedChaser(pType)) continue;
			if (!HasOffensiveWeapon(pType)) continue;
			if (pType->Speed > best) best = pType->Speed;
		}
		return best;
	}

	FootClass* PickSquadRecruit(HouseClass* const pHouse, TeamClass* const pExclude,
		bool const water = false)
	{
		auto const& chasers = DoctrineConfig::Get().CrateChasers;
		FootClass* bySpeedFree = nullptr; int bestSpeedFree = -1;
		FootClass* byListFree = nullptr;  int bestListFree = 1 << 30;
		FootClass* bySpeedTeam = nullptr; int bestSpeedTeam = -1;
		FootClass* byListTeam = nullptr;  int bestListTeam = 1 << 30;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pFoot = static_cast<FootClass*>(pT);
			if (pFoot->Team == pExclude && pExclude) continue; // already ours
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer) continue;
			if (IsExcludedChaser(pType)) continue; // modder-protected (e.g. heroes)
			// Terrain capability: water squad takes only water-capable units; the
			// ground squad takes anything except pure-naval hulls.
			if (water) { if (!IsWaterCapable(pType)) continue; }
			else       { if (pType->Naval) continue; }
			bool const teamed = (pFoot->Team != nullptr);
			if (HasOffensiveWeapon(pType))
			{
				if (!teamed && pType->Speed > bestSpeedFree)
					{ bestSpeedFree = pType->Speed; bySpeedFree = pFoot; }
				else if (teamed && pType->Speed > bestSpeedTeam)
					{ bestSpeedTeam = pType->Speed; bySpeedTeam = pFoot; }
			}
			for (int r = 0; r < static_cast<int>(chasers.size()); ++r)
			{
				if (chasers[r].ID != pType->ID) continue;
				if (!teamed && r < bestListFree) { bestListFree = r; byListFree = pFoot; }
				else if (teamed && r < bestListTeam) { bestListTeam = r; byListTeam = pFoot; }
			}
		}
		if (byListFree)  return byListFree;
		if (bySpeedFree) return bySpeedFree;
		if (byListTeam)  return byListTeam;
		return bySpeedTeam;
	}
}

void Teams::CrateSquadDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CrateSquad) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.CrateInterval > 0 ? cfg.CrateInterval : 90;
	auto const it = g_crateLastFire.find(hIdx);
	if (it != g_crateLastFire.end() && now - it->second < interval) return;
	g_crateLastFire[hIdx] = now;

	int desired = DesiredSquadSize();
	if (desired <= 0) return; // crates off globally

	// Crate-catcher PRODUCTION (opt-in): if the house can build a designated grabber,
	// raise the squad target by how GOOD that grabber is — a teleport grabber (CLEG)
	// is worth a full squad, a fast unit half, a slow one just one. We build toward
	// it below; diverting existing units still fills the rest.
	int catcherTier = 0;
	TechnoTypeClass* const pCatcher = cfg.CrateCatcherBuild
		? BestCrateCatcher(pHouse, catcherTier) : nullptr;
	int catcherTarget = 0;
	if (pCatcher)
	{
		int const cmax = cfg.CrateCatcherMax > 0 ? cfg.CrateCatcherMax : 3;
		catcherTarget = catcherTier >= 3 ? cmax
			: (catcherTier == 2 ? (cmax + 1) / 2 : 1);
		if (catcherTarget > desired) desired = catcherTarget;
		int const sqMax = cfg.CrateSquadMax > 0 ? cfg.CrateSquadMax : 6;
		if (desired > sqMax) desired = sqMax;
	}

	// Decide whether to FORCE a dedicated catcher this pass. DemandProduction only
	// queues behind the AI's own plan and never builds (confirmed in-game: fac=1 ok=1
	// owned=0), so the sole working path is the team taskforce below. We force one only
	// when: the modder opted in (CrateCatcherForceBuild), a faction+prereq-valid grabber
	// exists (pCatcher, from BestCrateCatcher), the house doesn't already own enough,
	// and no already-fast unit makes it pointless (CrateCatcherSkipSpeed).
	int const ownedCatchers = pCatcher ? CountOwned(pHouse, pCatcher) : 0;
	if (ownedCatchers > g_chaserGateMax) g_chaserGateMax = ownedCatchers; // feeds AITrigger gate
	int const fastestOwned = cfg.CrateCatcherSkipSpeed > 0
		? FastestOwnedEligibleSpeed(pHouse) : 0;
	bool const fastEnough = cfg.CrateCatcherSkipSpeed > 0
		&& fastestOwned >= cfg.CrateCatcherSkipSpeed;
	bool const doForce = cfg.CrateCatcherForceBuild && pCatcher
		&& !fastEnough && ownedCatchers < catcherTarget;
	// Slots held open for produced catchers: a team already full to `desired` by
	// diversion leaves the AI no room to place what the taskforce builds.
	int reserve = doForce ? (catcherTarget - ownedCatchers) : 0;
	if (reserve < 0) reserve = 0;
	if (reserve > desired) reserve = desired;

	// Create/refresh the squad team. When force-building, the taskforce carries the
	// validated catcher so the AI produces `catcherTarget` of it; otherwise null
	// (diversion-only — the squad still grabs the best grabber the AI already owns).
	auto const pTeam = EnsureSquadTeam(pHouse, desired,
		doForce ? pCatcher : nullptr, "DCRS", doForce ? catcherTarget : 0);
	if (!pTeam) return;

	// Maintain membership by diversion, leaving `reserve` slots free for taskforce
	// production. Liberate teamed recruits (the AI owns ~all units via teams).
	int members = 0;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) ++members;
	int recruited = 0;
	while (members < desired - reserve)
	{
		auto const pR = PickSquadRecruit(pHouse, pTeam);
		if (!pR) break;
		if (pR->Team) pR->Team->LiberateMember(pR);
		if (!pTeam->AddMember(pR, true)) break;
		// Clear any garrison flags the base AI (or our garrison doctrine) set on
		// this unit — otherwise the crate member wanders into a building and
		// leaves the squad, which is why members kept dropping to 0.
		pR->ShouldGarrisonStructure = false;
		pR->ShouldEnterOccupiable = false;
		++members; ++recruited;
	}

	// Give-up accounting: while force-building the catcher yet still owning none, bump
	// its fail streak; reset once one exists. BestCrateCatcher drops a type past
	// CrateCatcherGiveUp and falls to the next grabber (handles leaky CanBuild
	// greenlighting a unit the factory refuses — e.g. CLEG without a Battle Lab). The
	// streak only advances while doForce, so it tolerates normal build time.
	int catcherFail = 0;
	if (pCatcher)
	{
		auto& f = g_catcherFail[hIdx][pCatcher->get_ID()];
		if (ownedCatchers > 0) f = 0;
		else if (doForce) ++f;
		catcherFail = f;
	}

	// Live members, indexed for dispersal + assignment.
	std::vector<FootClass*> mem;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) mem.push_back(pF);
	int const N = static_cast<int>(mem.size());

	// Crate detection scoped to what the squad can actually REACH: each member
	// scans a box of ITS OWN grab radius (CrateChasers per-unit; 0 = map-wide),
	// so a chrono grabber (CLEG:0) sees the whole map while a Terror Drone
	// (DRON:100) only sees crates it could reach. If any member is map-wide we
	// sweep the whole map once; otherwise per-member boxes (deduped), plus a base
	// baseline so home crates are never missed.
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const scan = cfg.CrateScanRadius > 0 ? cfg.CrateScanRadius : 30;
	std::vector<CellClass*> crates;
	auto addCell = [&crates](CellClass* const pC)
	{
		if (!pC || !IsCrateOverlay(pC->OverlayTypeIndex)) return;
		if (pC->LandType == LandType::Water) return; // ground chasers can't reach
		                                             // water crates — skip them
		for (auto const pE : crates) if (pE == pC) return;
		crates.push_back(pC);
	};
	auto scanBox = [&addCell](CoordStruct const& center, int const r)
	{
		CellStruct const c0 = CellClass::Coord2Cell(center);
		for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				if (dx * dx + dy * dy > r * r) continue;
				CellStruct cs;
				cs.X = static_cast<short>(c0.X + dx);
				cs.Y = static_cast<short>(c0.Y + dy);
				addCell(MapClass::Instance.TryGetCellAt(cs));
			}
	};
	// Crate detection radius: CrateSquadScan=0 sweeps the WHOLE map (find and go for
	// crates anywhere — the default); >0 restricts to that many cells around base
	// for modders who prefer a home bubble. Crates are sparse + this is throttled,
	// so even a full sweep is cheap.
	if (cfg.CrateSquadScan <= 0)
	{
		auto const& b = MapClass::Instance.MapCoordBounds; // cell LTRB
		for (int y = b.Top; y <= b.Bottom; ++y)
			for (int x = b.Left; x <= b.Right; ++x)
			{
				CellStruct cs; cs.X = static_cast<short>(x); cs.Y = static_cast<short>(y);
				addCell(MapClass::Instance.TryGetCellAt(cs));
			}
	}
	else
	{
		scanBox(base, cfg.CrateSquadScan);
	}

	// Drop crates on the unreachable blacklist (a chaser gave up on them); prune
	// expired entries first so the squad retries once the cooldown passes.
	{
		auto& bl = g_crateBlacklist[hIdx];
		for (auto bit = bl.begin(); bit != bl.end(); )
			bit = (now >= bit->second) ? bl.erase(bit) : std::next(bit);
		if (!bl.empty())
			crates.erase(std::remove_if(crates.begin(), crates.end(),
				[&bl](CellClass* const pC) { return bl.count(CrateKey(pC)) > 0; }),
				crates.end());
	}

	// Assign: each crate is raced by its nearest free member (greedy, distinct);
	// members without a crate spread out discreetly on a standby ring, skipping
	// death-zone cells, so they're pre-positioned for the next spawn.
	std::vector<bool> crateTaken(crates.size(), false);
	auto& targets = g_squadTargets[hIdx];
	targets.clear(); // rebuilt this pass; SteerCrateSquad re-issues Move each tick
	int raced = 0;
	double nearestChase = 1e18; // diag: closest raced chaser's dist to its crate
	const char* nearestChaseType = "-"; // diag: that chaser's unit ID
	double const ringLep = scan * 256.0;
	for (int i = 0; i < N; ++i)
	{
		auto const pF = mem[i];
		auto const fc = pF->GetCoords();
		// Go for the nearest crate ANYWHERE on the map, but skip ones whose path is
		// dangerous (crosses a killbox) or that an enemy will reach first — Rex's
		// two stop conditions. No per-unit reach cap: whatever we have, send it.
		int best = -1; double bestD = 1e18;
		for (int c = 0; c < static_cast<int>(crates.size()); ++c)
		{
			if (crateTaken[c]) continue;
			auto const cc = crates[c]->GetCellCoords();
			double const d = std::sqrt(double(cc.X - fc.X) * (cc.X - fc.X)
				+ double(cc.Y - fc.Y) * (cc.Y - fc.Y));
			if (d >= bestD) continue;
			if (CratePathDangerous(pHouse, fc, crates[c])) continue; // danger in path
			if (CrateEnemyRacing(pHouse, crates[c], d)) continue;    // enemy racing us there
			bestD = d; best = c;
		}
		if (best >= 0)
		{
			// Give-up guard: track progress toward this crate. If the chaser hasn't
			// gotten meaningfully closer over several passes it's unreachable (e.g.
			// across a cliff) — blacklist it and free the unit instead of freezing
			// it there all game (a Boris sat stuck 16 passes on an unreachable one).
			int const key = CrateKey(crates[best]);
			auto& prog = g_crateProgress[hIdx];
			auto const pit = prog.find(key);
			int const stuck = (pit != prog.end() && bestD >= pit->second.first - 256.0)
				? pit->second.second + 1 : 0;
			int const giveUp = cfg.CrateGiveUpPasses > 0 ? cfg.CrateGiveUpPasses : 4;
			if (stuck >= giveUp)
			{
				g_crateBlacklist[hIdx][key] = now
					+ (cfg.CrateBlacklistTime > 0 ? cfg.CrateBlacklistTime : 1800);
				prog.erase(key);
				crateTaken[best] = true; // no other member should re-take it either
				if (cfg.DebugTicks)
					Debug::Log("[DoctrineExt] crate unreachable: house=%s#%d gave up at "
						"%.1f cells, blacklisted.\n", pHouse->get_ID(), hIdx, bestD / 256.0);
				continue; // free this member (it will disperse next pass)
			}
			prog[key] = { static_cast<int>(bestD), stuck };

			crateTaken[best] = true;
			pF->SetDestination(crates[best], true);
			pF->QueueMission(Mission::Move, false);
			targets.push_back({ pF, crates[best] }); // keep steering it each tick
			if (bestD < nearestChase)
			{
				nearestChase = bestD;
				if (auto const pTy = pF->GetTechnoType()) nearestChaseType = pTy->get_ID();
			}
			++raced;
		}
		else
		{
			// standby: spread on the ring at this member's angle, avoid killboxes
			double const ang = (N > 0) ? (6.2831853 * i / N) : 0.0;
			static const double kNudge[] = { 0.0, 0.4, -0.4, 0.8, -0.8 };
			CellClass* pDest = nullptr;
			for (double const nud : kNudge)
			{
				double const a = ang + nud;
				CoordStruct pt = base;
				pt.X = base.X + static_cast<int>(std::cos(a) * ringLep);
				pt.Y = base.Y + static_cast<int>(std::sin(a) * ringLep);
				auto const pC = MapClass::Instance.TryGetCellAt(pt);
				if (!pC) continue;
				CellStruct cs;
				cs.X = static_cast<short>(pt.X / 256);
				cs.Y = static_cast<short>(pt.Y / 256);
				if (DeathZones::ScoreAtCell(pHouse, cs.X, cs.Y) < cfg.DeathZoneMinStrength)
					{ pDest = pC; break; }
				if (!pDest) pDest = pC; // fallback: first valid
			}
			if (pDest)
			{
				auto const dc = pDest->GetCellCoords();
				double const d = std::sqrt(double(dc.X - fc.X) * (dc.X - fc.X)
					+ double(dc.Y - fc.Y) * (dc.Y - fc.Y));
				if (d > 4.0 * 256.0) // only re-issue if not already loitering there
				{
					pF->SetDestination(pDest, true);
					pF->QueueMission(Mission::Move, false);
				}
			}
		}
	}

	// Firesale-for-MCV — ONLY when FreeMCV=yes makes selling pay off (Rex: that
	// is the exploitable state). Timed off the member nearest a crate.
	if (cfg.CrateFiresaleMCV && RulesClass::Instance && RulesClass::Instance->FreeMCV
		&& !crates.empty() && N > 0)
	{
		bool const canRecover = CanRecoverMCV(pHouse);
		int const buildings = OwnedBuildingCount(pHouse);
		int const money = static_cast<int>(pHouse->Available_Money());
		bool const shortGame = Unsorted::ShortGame != 0;
		double nearest = 1e18;
		for (auto const pF : mem)
		{
			auto const fc = pF->GetCoords();
			for (auto const pC : crates)
			{
				auto const cc = pC->GetCellCoords();
				double const d = std::sqrt(double(cc.X - fc.X) * (cc.X - fc.X)
					+ double(cc.Y - fc.Y) * (cc.Y - fc.Y));
				if (d < nearest) nearest = d;
			}
		}
		double const trigger = (cfg.CrateFiresaleDist > 0 ? cfg.CrateFiresaleDist : 10) * 256.0;
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] crate squad comeback: house=%s#%d canRecoverMCV=%d "
				"buildings=%d money=%d shortGame=%d nearest=%.0f (trigger<=%.0f)\n",
				pHouse->get_ID(), hIdx, canRecover, buildings, money, shortGame, nearest, trigger);
		if (!shortGame && !canRecover && buildings > 0 && money > 0 && nearest <= trigger)
		{
			Debug::Log("[DoctrineExt] crate squad firesale-for-MCV: house=%s#%d selling all.\n",
				pHouse->get_ID(), hIdx);
			pHouse->Fire_Sale();
		}
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] crate squad: house=%s#%d desired=%d members=%d recruited=%d "
			"catcher=%s t%d owned=%d force=%d fastEnough=%d(spd%d) fail=%d reserve=%d "
			"crates=%d raced=%d nearestChaseCells=%.1f chaser=%s.\n",
			pHouse->get_ID(), hIdx, desired, N, recruited,
			pCatcher ? pCatcher->get_ID() : "-", catcherTier, ownedCatchers,
			doForce ? 1 : 0, fastEnough ? 1 : 0, fastestOwned, catcherFail, reserve,
			static_cast<int>(crates.size()), raced,
			nearestChase < 1e17 ? nearestChase / 256.0 : -1.0, nearestChaseType);
}

void Teams::SteerCrateSquad(HouseClass* pHouse)
{
	// Runs EVERY base tick (unlike the throttled detection pass): re-issue the
	// Move to each chaser's cached crate so the base AI can't countermand it in
	// the 90-frame gap. This is why the old single-grabber SteerCrate ran per
	// tick — a once-per-90-frames order gets pulled back before the unit arrives.
	auto const it = g_squadTargets.find(pHouse->ArrayIndex);
	if (it == g_squadTargets.end() || it->second.empty()) return;

	char id[0x18];
	std::snprintf(id, sizeof(id), "DCRS%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	if (!pTT || pTT->cntInstances <= 0) return;
	auto const pTeam = pTT->FindFirstInstance();
	if (!pTeam) return;

	// Live member set — only steer pointers still on the team (safe vs dangling).
	std::set<FootClass*> live;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) live.insert(pF);

	for (auto const& tgt : it->second)
	{
		auto const pF = tgt.first;
		auto const pCell = tgt.second;
		if (live.find(pF) == live.end()) continue;              // died / left squad
		if (!pCell || !IsCrateOverlay(pCell->OverlayTypeIndex)) continue; // crate gone

		auto const fc = pF->GetCoords();
		auto const cc = pCell->GetCellCoords();
		double const dx = cc.X - fc.X, dy = cc.Y - fc.Y;
		double const d = std::sqrt(dx * dx + dy * dy);

		// The engine's automatic pickup wasn't firing even with the unit sitting
		// ON the crate (log: 0.3 cells, crate still there), and a plain Move halts
		// ~1 cell short. So once the chaser is close enough — where a human would
		// grab it — call the engine's OWN collector directly (0x481A00). This is
		// the same routine a unit runs when it drives over a crate; we just trigger
		// it at a human-like tolerance instead of needing a pixel-perfect stop.
		if (d <= 1.5 * 256.0)
		{
			bool const got = pCell->CollectCrate(pF);
			if (got && DoctrineConfig::Get().DebugTicks)
				Debug::Log("[DoctrineExt] crate GRABBED: house=%s#%d by %s.\n",
					pHouse->get_ID(), pHouse->ArrayIndex,
					pF->GetTechnoType() ? pF->GetTechnoType()->get_ID() : "?");
			continue;
		}

		// Otherwise keep driving at the crate cell (re-issued every tick so the
		// base AI can't countermand it between the throttled detection passes).
		pF->SetDestination(pCell, true);
		pF->QueueMission(Mission::Move, false);
	}
}

namespace
{
	// Water crate squad state (separate team + throttle + caches from the ground
	// squad so the two don't fight over each other's slots).
	std::map<int, int> g_waterLastFire;
	std::map<int, std::vector<std::pair<FootClass*, CellClass*>>> g_waterTargets;
	std::map<int, std::map<int, std::pair<int, int>>> g_waterProgress;
	std::map<int, std::map<int, int>> g_waterBlacklist;
}

void Teams::CrateWaterSquad(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CrateSquad) return; // shares the squad toggle
	auto const r = RulesClass::Instance;
	if (!r || !r->Crates) return;

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.CrateInterval > 0 ? cfg.CrateInterval : 90;
	auto const it = g_waterLastFire.find(hIdx);
	if (it != g_waterLastFire.end() && now - it->second < interval) return;
	g_waterLastFire[hIdx] = now;

	int const desired = DesiredSquadSize();
	if (desired <= 0) return;

	auto const pTeam = EnsureSquadTeam(pHouse, desired, nullptr, "DCRW");
	if (!pTeam) return;

	// Maintain: recruit ONLY water-capable units (naval/hover/amphibious). If the
	// house owns none, the squad stays empty — graceful, water crates go ungrabbed.
	int members = 0;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) ++members;
	int recruited = 0;
	while (members < desired)
	{
		auto const pR = PickSquadRecruit(pHouse, pTeam, true);
		if (!pR) break;
		if (pR->Team) pR->Team->LiberateMember(pR);
		if (!pTeam->AddMember(pR, true)) break;
		pR->ShouldGarrisonStructure = false;
		pR->ShouldEnterOccupiable = false;
		++members; ++recruited;
	}

	std::vector<FootClass*> mem;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) mem.push_back(pF);
	int const N = static_cast<int>(mem.size());

	// Detect WATER crates within scan of base (LandType::Water only — the mirror of
	// the ground squad, which skips them).
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const scan = cfg.CrateSquadScan > 0 ? cfg.CrateSquadScan : 60;
	std::vector<CellClass*> crates;
	auto addCell = [&crates](CellClass* const pC)
	{
		if (!pC || !IsCrateOverlay(pC->OverlayTypeIndex)) return;
		if (pC->LandType != LandType::Water) return; // water squad: water crates only
		for (auto const pE : crates) if (pE == pC) return;
		crates.push_back(pC);
	};
	CellStruct const c0 = CellClass::Coord2Cell(base);
	for (int dy = -scan; dy <= scan; ++dy)
		for (int dx = -scan; dx <= scan; ++dx)
		{
			if (dx * dx + dy * dy > scan * scan) continue;
			CellStruct cs; cs.X = static_cast<short>(c0.X + dx); cs.Y = static_cast<short>(c0.Y + dy);
			addCell(MapClass::Instance.TryGetCellAt(cs));
		}
	{
		auto& bl = g_waterBlacklist[hIdx];
		for (auto bit = bl.begin(); bit != bl.end(); )
			bit = (now >= bit->second) ? bl.erase(bit) : std::next(bit);
		if (!bl.empty())
			crates.erase(std::remove_if(crates.begin(), crates.end(),
				[&bl](CellClass* const pC) { return bl.count(CrateKey(pC)) > 0; }), crates.end());
	}

	std::vector<bool> taken(crates.size(), false);
	auto& targets = g_waterTargets[hIdx];
	targets.clear();
	int raced = 0;
	for (int i = 0; i < N; ++i)
	{
		auto const pF = mem[i];
		auto const fc = pF->GetCoords();
		int best = -1; double bestD = 1e18;
		for (int c = 0; c < static_cast<int>(crates.size()); ++c)
		{
			if (taken[c]) continue;
			auto const cc = crates[c]->GetCellCoords();
			double const d = std::sqrt(double(cc.X - fc.X) * (cc.X - fc.X)
				+ double(cc.Y - fc.Y) * (cc.Y - fc.Y));
			if (d < bestD) { bestD = d; best = c; }
		}
		if (best < 0) continue;
		int const key = CrateKey(crates[best]);
		auto& prog = g_waterProgress[hIdx];
		auto const pit = prog.find(key);
		int const stuck = (pit != prog.end() && bestD >= pit->second.first - 256.0)
			? pit->second.second + 1 : 0;
		int const giveUp = cfg.CrateGiveUpPasses > 0 ? cfg.CrateGiveUpPasses : 4;
		if (stuck >= giveUp)
		{
			g_waterBlacklist[hIdx][key] = now
				+ (cfg.CrateBlacklistTime > 0 ? cfg.CrateBlacklistTime : 1800);
			prog.erase(key); taken[best] = true; continue;
		}
		prog[key] = { static_cast<int>(bestD), stuck };
		taken[best] = true;
		pF->SetDestination(crates[best], true);
		pF->QueueMission(Mission::Move, false);
		targets.push_back({ pF, crates[best] });
		++raced;
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] crate water squad: house=%s#%d members=%d recruited=%d "
			"watercrates=%d raced=%d.\n", pHouse->get_ID(), hIdx, N, recruited,
			static_cast<int>(crates.size()), raced);
}

void Teams::SteerCrateWaterSquad(HouseClass* pHouse)
{
	auto const it = g_waterTargets.find(pHouse->ArrayIndex);
	if (it == g_waterTargets.end() || it->second.empty()) return;
	char id[0x18];
	std::snprintf(id, sizeof(id), "DCRW%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	if (!pTT || pTT->cntInstances <= 0) return;
	auto const pTeam = pTT->FindFirstInstance();
	if (!pTeam) return;
	std::set<FootClass*> live;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) live.insert(pF);
	for (auto const& tgt : it->second)
	{
		auto const pF = tgt.first;
		auto const pCell = tgt.second;
		if (live.find(pF) == live.end()) continue;
		if (!pCell || !IsCrateOverlay(pCell->OverlayTypeIndex)) continue;
		auto const fc = pF->GetCoords();
		auto const cc = pCell->GetCellCoords();
		double const dx = cc.X - fc.X, dy = cc.Y - fc.Y;
		double const d = std::sqrt(dx * dx + dy * dy);
		if (d <= 1.5 * 256.0)
		{
			if (pCell->CollectCrate(pF) && DoctrineConfig::Get().DebugTicks)
				Debug::Log("[DoctrineExt] water crate GRABBED: house=%s#%d by %s.\n",
					pHouse->get_ID(), pHouse->ArrayIndex,
					pF->GetTechnoType() ? pF->GetTechnoType()->get_ID() : "?");
			continue;
		}
		CellClass* pDest = pCell;
		if (d > 1.0 && d < 5.0 * 256.0)
		{
			CoordStruct beyond = cc;
			beyond.X += static_cast<int>(dx / d * 768.0);
			beyond.Y += static_cast<int>(dy / d * 768.0);
			if (auto const pB = MapClass::Instance.TryGetCellAt(beyond)) pDest = pB;
		}
		pF->SetDestination(pDest, true);
		pF->QueueMission(Mission::Move, false);
	}
}

namespace
{
	// Commander takeover state, per house: g_cmdrIdleSince = last frame the AI was
	// seen ATTACKING on its own (idle duration = now - it); g_cmdrLastEval throttle.
	std::map<int, int> g_cmdrIdleSince;
	std::map<int, int> g_cmdrLastEval;

	// Fallback assault target when DecapTarget declines (all key buildings in
	// killboxes): the enemy's ConYard, else any building. A takeover hits SOMETHING.
	BuildingClass* AnyEnemyBuilding(HouseClass* const pEnemy)
	{
		if (!pEnemy) return nullptr;
		BuildingClass* con = nullptr; BuildingClass* any = nullptr;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || pB->Owner != pEnemy || pB->InLimbo || pB->Health <= 0) continue;
			if (!any) any = pB;
			if (pB->Type && pB->Type->ConstructionYard) { con = pB; break; }
		}
		return con ? con : any;
	}
}

void Teams::CommanderTakeover(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (cfg.CommanderIdleTime <= 0) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const period = cfg.CommanderPeriod > 0 ? cfg.CommanderPeriod : 150;
	auto const le = g_cmdrLastEval.find(hIdx);
	if (le != g_cmdrLastEval.end() && now - le->second < period) return;
	g_cmdrLastEval[hIdx] = now;

	// Measure the house's own aggression: of its armed mobile units NOT already on
	// one of our doctrine teams, how many are committed (out beyond the front) vs
	// sitting home. Home units are the pool we can commandeer.
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	double const front = (cfg.CommanderFront > 0 ? cfg.CommanderFront : 25) * 256.0;
	int armyAvail = 0, committed = 0;
	std::vector<FootClass*> home;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
		auto const what = pT->WhatAmI();
		if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
		auto const pFoot = static_cast<FootClass*>(pT);
		if (pFoot->Team && IsDoctrineTeam(pFoot->Team)) continue; // already ours
		auto const pType = pT->GetTechnoType();
		if (!pType || pType->ResourceGatherer) continue;         // no harvesters
		if (!HasOffensiveWeapon(pType)) continue;                // no MCVs/support
		++armyAvail;
		auto const c = pT->GetCoords();
		double const d = std::sqrt(double(c.X - base.X) * (c.X - base.X)
			+ double(c.Y - base.Y) * (c.Y - base.Y));
		if (d > front) ++committed;
		else home.push_back(pFoot);
	}

	int const need = std::max(1, static_cast<int>(armyAvail * cfg.CommanderCommitFrac));
	bool const aggressive = committed >= need;
	auto& idleSince = g_cmdrIdleSince[hIdx];
	if (idleSince == 0) idleSince = now;

	// Per-eval diagnostic (throttled): shows WHY the commander does or doesn't
	// take over — feature on?, army size, committed vs need, how long idle.
	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] commander eval: house=%s#%d idleTime=%d army=%d (min %d) "
			"committed=%d need=%d aggressive=%d idleFor=%d/%d\n",
			pHouse->get_ID(), hIdx, cfg.CommanderIdleTime, armyAvail, cfg.CommanderMinArmy,
			committed, need, aggressive ? 1 : 0, now - idleSince, cfg.CommanderIdleTime);

	if (aggressive) { idleSince = now; return; }  // AI is attacking on its own — stay out
	if (armyAvail < cfg.CommanderMinArmy) return; // not enough army to bother
	if (now - idleSince < cfg.CommanderIdleTime) return; // still within patience window

	// TAKE COMMAND: commandeer home units (diverting off their idle aimd teams)
	// onto the assault team, up to Batch. SteerCommander drives them each tick.
	int const batch = cfg.CommanderBatch > 0 ? cfg.CommanderBatch : 1 << 30;
	auto const pTeam = EnsureSquadTeam(pHouse,
		cfg.CommanderBatch > 0 ? cfg.CommanderBatch : 16, nullptr, "DCMD");
	if (!pTeam) return;
	int members = 0;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) ++members;
	int commandeered = 0;
	for (auto const pFoot : home)
	{
		if (members >= batch) break;
		if (pFoot->Team == pTeam) continue;
		if (pFoot->Team) pFoot->Team->LiberateMember(pFoot); // pull off its idle aimd team
		if (pTeam->AddMember(pFoot, true)) { ++members; ++commandeered; }
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] COMMANDER takeover: house=%s#%d idle=%d frames army=%d "
			"committed=%d commandeered=%d members=%d.\n",
			pHouse->get_ID(), hIdx, now - idleSince, armyAvail, committed, commandeered, members);
}

void Teams::SteerCommander(HouseClass* pHouse)
{
	char id[0x18];
	std::snprintf(id, sizeof(id), "DCMD%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	if (!pTT || pTT->cntInstances <= 0) return;
	auto const pTeam = pTT->FindFirstInstance();
	if (!pTeam || !pTeam->FirstUnit) return;

	// Re-acquire the enemy's priority building each tick (as buildings fall the
	// target advances down the rebuild chain), then drive every member at it — a
	// loose order gets countermanded, a re-issued team order sticks (like decap).
	auto const pEnemy = WeakestEnemy(pHouse);
	auto pBld = pEnemy ? DecapTarget(pHouse, pEnemy) : nullptr;
	if (!pBld) pBld = AnyEnemyBuilding(pEnemy); // all key targets in killboxes: still hit something
	if (!pBld) return;

	pTeam->AssignMissionTarget(pBld);
	for (auto pFoot = pTeam->FirstUnit; pFoot; pFoot = pFoot->NextTeamMember)
	{
		if (pFoot->InLimbo || pFoot->Health <= 0) continue;
		pFoot->SetTarget(pBld);
		pFoot->QueueMission(Mission::Attack, false);
	}
}

namespace
{
	std::map<int, int> g_airLastFire;
	// Per-house cached AA screen assignments {unit, screen cell} from the throttled
	// placement pass; SteerAirDefense repositions stragglers each tick.
	std::map<int, std::vector<std::pair<FootClass*, CellClass*>>> g_aaTargets;

	// OPTIONAL dependency on WarZoneExt (AA P2): persistent, cross-game, per-map
	// memory of where aircraft get their kills ("AirDeath" zone). Bound lazily via
	// GetModuleHandle/GetProcAddress so DoctrineExt runs unchanged when WarZoneExt
	// isn't installed and lights up when it is. WZ_Version() is the gate.
	using WZ_Version_t = int(__cdecl*)();
	using WZ_ZoneWeight_t = int(__cdecl*)(const char*, int, int);
	WZ_ZoneWeight_t g_wzZoneWeight = nullptr;
	bool g_wzChecked = false;

	void EnsureWarZoneLink()
	{
		if (g_wzChecked) return;
		g_wzChecked = true;
		HMODULE const h = GetModuleHandleA("WarZoneExt.dll");
		if (!h) return;
		auto const ver = reinterpret_cast<WZ_Version_t>(GetProcAddress(h, "WZ_Version"));
		if (!ver || ver() < 1) return; // absent or too old — don't bind the rest
		g_wzZoneWeight = reinterpret_cast<WZ_ZoneWeight_t>(GetProcAddress(h, "WZ_ZoneWeight"));
		Debug::Log("[DoctrineExt] WarZoneExt linked (v%d): persistent AirDeath zone available.\n", ver());
	}

	int WarZoneAirDeath(int const cellX, int const cellY)
	{
		return g_wzZoneWeight ? g_wzZoneWeight("AirDeath", cellX, cellY) : 0;
	}

	// Persistent ore value at a cell from WarZoneExt's Wealth zone (0 if unbound).
	int WarZoneWealth(int const cellX, int const cellY)
	{
		return g_wzZoneWeight ? g_wzZoneWeight("Wealth", cellX, cellY) : 0;
	}
}

void Teams::AirDefense(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.AirDefenseEnable) return; // opt-in

	EnsureWarZoneLink(); // optional persistent air-death memory (AA P2)

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const period = cfg.AirDefensePeriod > 0 ? cfg.AirDefensePeriod : 150;
	auto const it = g_airLastFire.find(hIdx);
	if (it != g_airLastFire.end() && now - it->second < period) return;
	g_airLastFire[hIdx] = now;

	// Enemy air threat = Σ living enemy aircraft DPS (carriers' craft are aircraft
	// too, so they count). No threat -> nothing to do.
	double airDPS = 0.0;
	for (int i = 0; i < HouseClass::Array.Count; ++i)
	{
		auto const pO = HouseClass::Array.GetItem(i);
		if (!pO || pO == pHouse || pO->Defeated || pO->IsObserver() || pO->IsNeutral()) continue;
		if (pHouse->IsAlliedWith(pO)) continue;
		for (auto const pAt : AircraftTypeClass::Array)
		{
			int const n = pO->CountOwnedAndPresent(pAt);
			if (n > 0) airDPS += RawDPS(pAt) * n;
		}
	}
	// AA P1c: historical air-death hotspot near base (where aircraft keep hurting
	// us). Act even with no LIVE air if history says this base gets raided.
	int const maxAir = DeathZones::MaxAirNear(pHouse);
	bool const airHotspot = maxAir >= (cfg.DeathZoneMinStrength > 0 ? cfg.DeathZoneMinStrength : 48);

	// AA P2: persistent per-map air-death memory from WarZoneExt (optional). On a
	// KNOWN map this is hot from game 1, before any local death has happened — so
	// AA can pre-deploy toward the map's air corridor immediately. WarZone counts
	// events (1 per air kill, accumulated across games), a much smaller scale than
	// the local grid, so it uses its own low threshold.
	auto const airBase = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const airScanR = cfg.AirScreenRadius > 0 ? cfg.AirScreenRadius : 30;
	double wzBearing = 0.0; int wzBest = 0;
	if (g_wzZoneWeight)
	{
		CellStruct const bc = CellClass::Coord2Cell(airBase);
		for (int dy = -airScanR; dy <= airScanR; dy += 2)
			for (int dx = -airScanR; dx <= airScanR; dx += 2)
			{
				if (dx * dx + dy * dy > airScanR * airScanR) continue;
				int const w = WarZoneAirDeath(bc.X + dx, bc.Y + dy);
				if (w > wzBest) { wzBest = w; wzBearing = std::atan2(double(dy), double(dx)); }
			}
	}
	bool const wzHotspot = wzBest >= 3; // >=3 air kills in a bucket across games = a real corridor

	if (airDPS <= 0.0 && !airHotspot && !wzHotspot) return;

	// Own anti-air firepower (units + defensive buildings with an AA weapon).
	double ownAA = 0.0;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
		if (auto const pTy = pT->GetTechnoType()) ownAA += AADps(pTy);
	}

	// Ratio the AI trusts for survival: keep own AA >= threat x AirThreatRatio.
	// Re-evaluated each period, so building more air makes it build more AA.
	double desired = airDPS * (cfg.AirThreatRatio > 0.0 ? cfg.AirThreatRatio : 1.5);
	if ((airHotspot || wzHotspot) && desired < cfg.AirDeathFloor)
		desired = cfg.AirDeathFloor; // baseline where air has repeatedly hit (this game or in map history)
	int queued = 0;
	const char* built = "none";
	if (ownAA < desired)
	{
		const DoctrineArsenalRole* pRole = nullptr;
		for (auto const& role : cfg.Arsenal)
			if (role.Role == "AntiAir") { pRole = &role; break; }
		TechnoTypeClass* const pBest = pRole ? PickType(pHouse, *pRole) : nullptr;
		if (pBest && CanBuildStrict(pHouse, pBest))
		{
			if (auto const pFactory = FindHouseFactory(pHouse, pBest))
			{
				built = pBest->get_ID();
				int const cap = cfg.AAMaxProduce > 0 ? cfg.AAMaxProduce : 2;
				for (int k = 0; k < cap; ++k)
				{
					pFactory->DemandProduction(pBest, pHouse, true);
					++queued;
				}
			}
		}
	}

	// --- Placement (P1b): screen AA forward of base, split across the bearings the
	// air is actually coming from (so a two-sided carrier attack gets covered both
	// ways instead of the AA parking to one side). ---
	auto& aaTargets = g_aaTargets[hIdx];
	aaTargets.clear();
	int screens = 0;
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const screenR = cfg.AirScreenRadius > 0 ? cfg.AirScreenRadius : 30;
	double const screenLep = screenR * 256.0;

	// Bin incoming enemy aircraft into 8 bearing sectors, weighted by DPS.
	double secDps[8] = {}, secSin[8] = {}, secCos[8] = {};
	bool anyAir = false;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->InLimbo || pT->Health <= 0) continue;
		if (pT->WhatAmI() != AbstractType::Aircraft) continue;
		auto const pO = pT->Owner;
		if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;
		auto const c = pT->GetCoords();
		double const dx = c.X - base.X, dy = c.Y - base.Y;
		double const dist = std::sqrt(dx * dx + dy * dy);
		if (dist > screenLep || dist < 1.0) continue;
		double const bearing = std::atan2(dy, dx);
		int const s = static_cast<int>(std::floor((bearing + 3.14159265) / (3.14159265 / 4.0))) & 7;
		double const w = RawDPS(pT->GetTechnoType());
		secDps[s] += w; secSin[s] += std::sin(bearing) * w; secCos[s] += std::cos(bearing) * w;
		anyAir = true;
	}

	// Screen bearings come from LIVE incoming air if present (P1b); otherwise from
	// the historical air-death hotspot (P1c) so AA pre-deploys where air keeps
	// hitting even between raids. Up to two axes (a split fleet).
	double b1 = 0.0, b2 = 0.0, w1 = 1.0, w2 = 0.0;
	bool have = false, has2 = false;
	if (anyAir)
	{
		// Strongest sector, then strongest NON-adjacent sector with >= 1/3 its
		// weight (a genuine second axis).
		int s1 = 0; for (int s = 1; s < 8; ++s) if (secDps[s] > secDps[s1]) s1 = s;
		int s2 = -1; double best2 = 0.0;
		for (int s = 0; s < 8; ++s)
		{
			if (s == s1 || s == ((s1 + 1) & 7) || s == ((s1 + 7) & 7)) continue;
			if (secDps[s] > best2) { best2 = secDps[s]; s2 = s; }
		}
		if (s2 >= 0 && secDps[s2] < secDps[s1] / 3.0) s2 = -1;
		b1 = std::atan2(secSin[s1], secCos[s1]); w1 = secDps[s1];
		if (s2 >= 0) { b2 = std::atan2(secSin[s2], secCos[s2]); w2 = secDps[s2]; has2 = true; }
		have = true;
	}
	else if (airHotspot)
	{
		double ang; int str;
		if (DeathZones::HottestAirBearing(pHouse, ang, str)) { b1 = ang; have = true; }
	}
	else if (wzHotspot)
	{
		// No live air, no local history yet — pre-deploy toward the map's persistent
		// air corridor (WarZoneExt), which is hot from game 1 on a known map.
		b1 = wzBearing; have = true;
	}

	if (!have)
	{
		// Nothing to screen — release the team back to the AI.
		char rid[0x18]; std::snprintf(rid, sizeof(rid), "DAA%dTM", hIdx);
		if (auto const pTT = TeamTypeClass::Find(rid)) pTT->DestroyAllInstances();
	}
	else
	{
		// Gather own AA-capable mobile units (teamless / on aimd / on our own DAA —
		// but never poach another doctrine team's units).
		auto const pTeam = EnsureSquadTeam(pHouse, 24, nullptr, "DAA");
		std::vector<FootClass*> aa;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pFoot = static_cast<FootClass*>(pT);
			if (pFoot->Team && pFoot->Team != pTeam && IsDoctrineTeam(pFoot->Team)) continue;
			auto const pTy = pT->GetTechnoType();
			if (!pTy || AADps(pTy) <= 0.0) continue;
			aa.push_back(pFoot);
		}

		if (pTeam && !aa.empty())
		{
			double const standoff = (cfg.AirDefenseStandoff > 0 ? cfg.AirDefenseStandoff : 12) * 256.0;
			int const n1 = has2
				? static_cast<int>(aa.size() * w1 / (w1 + w2) + 0.5)
				: static_cast<int>(aa.size());
			for (int k = 0; k < static_cast<int>(aa.size()); ++k)
			{
				double const bearing = (has2 && k >= n1) ? b2 : b1;
				CoordStruct pt = base;
				pt.X = base.X + static_cast<int>(std::cos(bearing) * standoff);
				pt.Y = base.Y + static_cast<int>(std::sin(bearing) * standoff);
				auto const pCell = MapClass::Instance.TryGetCellAt(pt);
				if (!pCell) continue;
				auto const pFoot = aa[k];
				if (pFoot->Team != pTeam)
				{
					if (pFoot->Team) pFoot->Team->LiberateMember(pFoot);
					pTeam->AddMember(pFoot, true);
				}
				aaTargets.push_back({ pFoot, pCell });
			}
			screens = has2 ? 2 : 1;
		}
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] air-defense: house=%s#%d enemyAirDPS=%.0f ownAA=%.0f "
			"desired=%.0f maxAirDeath=%d wzAir=%d build=%s queued=%d screens=%d aa=%d.\n",
			pHouse->get_ID(), hIdx, airDPS, ownAA, desired, maxAir, wzBest, built, queued,
			screens, static_cast<int>(aaTargets.size()));
}

void Teams::SteerAirDefense(HouseClass* pHouse)
{
	auto const it = g_aaTargets.find(pHouse->ArrayIndex);
	if (it == g_aaTargets.end() || it->second.empty()) return;
	char id[0x18]; std::snprintf(id, sizeof(id), "DAA%dTM", pHouse->ArrayIndex);
	auto const pTT = TeamTypeClass::Find(id);
	if (!pTT || pTT->cntInstances <= 0) return;
	auto const pTeam = pTT->FindFirstInstance();
	if (!pTeam) return;
	std::set<FootClass*> live;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) live.insert(pF);
	for (auto const& tgt : it->second)
	{
		auto const pF = tgt.first;
		auto const pCell = tgt.second;
		if (live.find(pF) == live.end() || !pCell) continue;
		// Reposition only if it has drifted off its screen point; once there, leave
		// it be so it actually FIRES at aircraft in range instead of re-pathing.
		auto const fc = pF->GetCoords();
		auto const cc = pCell->GetCellCoords();
		double const dx = cc.X - fc.X, dy = cc.Y - fc.Y;
		if (std::sqrt(dx * dx + dy * dy) > 3.0 * 256.0)
		{
			pF->SetDestination(pCell, true);
			pF->QueueMission(Mission::Move, false);
		}
	}
}

namespace
{
	std::map<int, int> g_navalLastFire;
	std::map<int, int> g_expandLastFire;

	// First buildable building of a given category for the house (WeaponsFactory /
	// Refinery), respecting TechLevel + Owner + prereqs.
	BuildingTypeClass* PickBuildable(HouseClass* const pHouse, bool wantWarFactory, bool wantRefinery)
	{
		for (auto const pBt : BuildingTypeClass::Array)
		{
			if (!pBt) continue;
			if (wantWarFactory && !pBt->WeaponsFactory) continue;
			if (wantRefinery && !pBt->Refinery) continue;
			if (pBt->Naval) continue; // naval yard handled by the naval doctrine
			if (!CanBuildStrict(pHouse, pBt)) continue;
			return pBt;
		}
		return nullptr;
	}

	int CountOwnedBuildings(HouseClass* const pHouse, bool wantWarFactory, bool wantRefinery)
	{
		int n = 0;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || pB->Owner != pHouse || pB->InLimbo || pB->Health <= 0) continue;
			auto const bt = pB->Type;
			if (!bt) continue;
			if (wantWarFactory && bt->WeaponsFactory && !bt->Naval) ++n;
			else if (wantRefinery && bt->Refinery) ++n;
		}
		return n;
	}
}

void Teams::BaseExpansion(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.ExpansionEnable) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.ExpansionInterval > 0 ? cfg.ExpansionInterval : 450;
	auto const it = g_expandLastFire.find(hIdx);
	if (it != g_expandLastFire.end() && now - it->second < interval) return;
	g_expandLastFire[hIdx] = now;

	EnsureWarZoneLink(); // persistent Wealth zone, if WarZoneExt is installed

	// Scan near base: buildable LAND cells (room to grow) and reachable WEALTH
	// (WarZoneExt's persistent Wealth zone if present, else current ore on the map).
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const radius = cfg.ExpansionRadius > 0 ? cfg.ExpansionRadius : 30;
	CellStruct const c0 = CellClass::Coord2Cell(base);
	long long land = 0, wealth = 0;
	bool const haveWZ = (g_wzZoneWeight != nullptr); // persistent Wealth zone bound?
	for (int dy = -radius; dy <= radius; dy += 2) // coarse stride — approximate is fine
		for (int dx = -radius; dx <= radius; dx += 2)
		{
			if (dx * dx + dy * dy > radius * radius) continue;
			int const cx = c0.X + dx, cy = c0.Y + dy;
			CellStruct cs; cs.X = static_cast<short>(cx); cs.Y = static_cast<short>(cy);
			auto const pC = MapClass::Instance.TryGetCellAt(cs);
			if (!pC) continue;
			auto const lt = pC->LandType;
			if (lt == LandType::Clear || lt == LandType::Road) land += 4; // 4 = the stride cell's area
			wealth += haveWZ ? WarZoneWealth(cx, cy) : pC->GetContainedTiberiumValue();
		}

	// Greedy targets: more production where the land allows, more economy where the
	// money is. Capped so it can't run away, tunable in [Doctrine.Expansion].
	int const lpf = cfg.LandPerFactory > 0 ? cfg.LandPerFactory : 120;
	int const wpr = cfg.WealthPerRefinery > 0 ? cfg.WealthPerRefinery : 5000;
	int warTarget = 1 + static_cast<int>(land / lpf);
	if (warTarget > cfg.WarFactoryMax) warTarget = cfg.WarFactoryMax;
	int refTarget = 1 + static_cast<int>(wealth / wpr);
	if (refTarget > cfg.RefineryMax) refTarget = cfg.RefineryMax;

	int const wf = CountOwnedBuildings(pHouse, true, false);
	int const ref = CountOwnedBuildings(pHouse, false, true);

	int queued = 0;
	const char* wfId = "none"; const char* refId = "none";
	if (wf < warTarget)
	{
		if (auto const pBt = PickBuildable(pHouse, true, false))
		{
			wfId = pBt->get_ID();
			if (QueueBaseNode(pHouse, pBt, c0)) ++queued;
		}
	}
	if (ref < refTarget)
	{
		if (auto const pBt = PickBuildable(pHouse, false, true))
		{
			refId = pBt->get_ID();
			if (QueueBaseNode(pHouse, pBt, c0)) ++queued;
		}
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] expansion: house=%s#%d land=%lld wealth=%lld wz=%d "
			"warFac=%d/%d(%s) refinery=%d/%d(%s) queued=%d.\n",
			pHouse->get_ID(), hIdx, land, wealth, haveWZ ? 1 : 0,
			wf, warTarget, wfId, ref, refTarget, refId, queued);
}

void Teams::NavalDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.NavalEnable) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.NavalInterval > 0 ? cfg.NavalInterval : 300;
	auto const it = g_navalLastFire.find(hIdx);
	if (it != g_navalLastFire.end() && now - it->second < interval) return;
	g_navalLastFire[hIdx] = now;

	// Water access near base — no point building a fleet with nowhere to float it.
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	int const scan = cfg.NavalScanRadius > 0 ? cfg.NavalScanRadius : 30;
	CellStruct const c0 = CellClass::Coord2Cell(base);
	int water = 0;
	for (int dy = -scan; dy <= scan; ++dy)
		for (int dx = -scan; dx <= scan; ++dx)
		{
			if (dx * dx + dy * dy > scan * scan) continue;
			CellStruct cs; cs.X = static_cast<short>(c0.X + dx); cs.Y = static_cast<short>(c0.Y + dy);
			auto const pC = MapClass::Instance.TryGetCellAt(cs);
			if (pC && pC->LandType == LandType::Water) ++water;
		}
	if (water < cfg.NavalMinWater) return; // landlocked base — no navy

	// A Naval Yard is a naval-flagged building that produces units.
	int yards = 0;
	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		auto const pB = BuildingClass::Array.GetItem(i);
		if (!pB || pB->Owner != pHouse || pB->InLimbo || pB->Health <= 0) continue;
		auto const bt = pB->Type;
		if (bt && bt->Naval && bt->Factory == AbstractType::UnitType) ++yards;
	}

	int queuedYard = 0;
	const char* yardId = "none";
	if (yards == 0)
	{
		// Force the yard the base AI won't: pick a buildable naval-producing
		// building and demand it at the ConYard (the AI then places it on water).
		BuildingTypeClass* pYard = nullptr;
		for (auto const pBt : BuildingTypeClass::Array)
		{
			if (!pBt || !pBt->Naval || pBt->Factory != AbstractType::UnitType) continue;
			if (!CanBuildStrict(pHouse, pBt)) continue;
			pYard = pBt; break;
		}
		if (pYard)
		{
			yardId = pYard->get_ID();
			// Add it to the AI's base plan so the AI builds AND places it (its own
			// logic finds the water-adjacent spot). c0 = base cell; it searches out.
			if (QueueBaseNode(pHouse, pYard, c0))
				++queuedYard;
		}
	}

	// With a yard up, build naval combat units toward the target.
	int navy = 0;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
		if (pT->WhatAmI() != AbstractType::Unit) continue;
		auto const ut = static_cast<UnitTypeClass*>(pT->GetTechnoType());
		if (ut && ut->Naval && !ut->ResourceGatherer && HasOffensiveWeapon(ut)) ++navy;
	}

	int queuedNavy = 0;
	const char* navyId = "none";
	if (yards > 0 && navy < cfg.NavyTarget)
	{
		UnitTypeClass* pBest = nullptr; double bestDps = -1.0;
		for (auto const pUt : UnitTypeClass::Array)
		{
			if (!pUt || !pUt->Naval || pUt->ResourceGatherer) continue;
			if (!HasOffensiveWeapon(pUt) || !CanBuildStrict(pHouse, pUt)) continue;
			double const dps = RawDPS(pUt);
			if (dps > bestDps) { bestDps = dps; pBest = pUt; }
		}
		if (pBest)
		{
			navyId = pBest->get_ID();
			if (auto const pFactory = FindHouseFactory(pHouse, pBest))
			{
				int const cap = cfg.NavyMaxProduce > 0 ? cfg.NavyMaxProduce : 2;
				int want = cfg.NavyTarget - navy;
				if (want > cap) want = cap;
				for (int k = 0; k < want; ++k)
				{
					pFactory->DemandProduction(pBest, pHouse, true);
					++queuedNavy;
				}
			}
		}
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] naval: house=%s#%d water=%d yards=%d navy=%d "
			"yard=%s queuedYard=%d navy=%s queuedNavy=%d.\n",
			pHouse->get_ID(), hIdx, water, yards, navy, yardId, queuedYard, navyId, queuedNavy);
}

int Teams::CountIdleArmed(HouseClass* pHouse)
{
	int n = 0;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
		if (IsIdleArmed(TechnoClass::Array.GetItem(i), pHouse))
			++n;
	return n;
}

void Teams::FloodResponse(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (cfg.FloodThreshold <= 0) return; // opt-in (off by default)

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const cd = cfg.FloodCooldown > 0 ? cfg.FloodCooldown : 900;
	auto const it = g_floodLastFire.find(hIdx);
	if (it != g_floodLastFire.end() && now - it->second < cd)
		return;

	std::vector<FootClass*> idle;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pTechno = TechnoClass::Array.GetItem(i);
		if (IsIdleArmed(pTechno, pHouse))
			idle.push_back(static_cast<FootClass*>(pTechno));
	}
	if (static_cast<int>(idle.size()) <= cfg.FloodThreshold)
		return; // not flooded

	g_floodLastFire[hIdx] = now;

	// Send a fraction of the hoard to Hunt (roam + engage) to relieve the clog;
	// keep the rest. Deterministic selection (array order) keeps it sync-safe.
	double const frac = cfg.FloodHuntFraction > 0.0 ? cfg.FloodHuntFraction : 0.5;
	int send = static_cast<int>(idle.size() * frac);
	if (send < 1) send = 1;
	int sent = 0;
	for (int k = 0; k < send && k < static_cast<int>(idle.size()); ++k)
	{
		idle[k]->ForceMission(Mission::Hunt);
		++sent;
	}
	Debug::Log("[DoctrineExt] base flood: house=%s#%d idle=%u > %d, sent %d to Hunt.\n",
		pHouse->get_ID(), hIdx, idle.size(), cfg.FloodThreshold, sent);
}

void Teams::ReserveSpend(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (cfg.ReserveBuild.empty()) return;
	if (cfg.ReserveAmount <= 0 && cfg.ReserveGrowth <= 0) return; // opt-in

	int const money = static_cast<int>(pHouse->Available_Money());
	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;

	// Sample the money history each tick and prune to the growth window, so the
	// growth trigger measures net accumulation over a rolling window.
	int const window = cfg.ReserveGrowthWindow > 0 ? cfg.ReserveGrowthWindow : 1800;
	auto& hist = g_moneyHistory[hIdx];
	hist.emplace_back(now, money);
	while (!hist.empty() && now - hist.front().first > window)
		hist.pop_front();

	// Trigger A: cash above the reserve floor. Trigger B: cash has grown by at
	// least Growth over the window (accumulating faster than it spends) — with
	// enough history to trust it. Either one arms a build.
	bool const overAmount = cfg.ReserveAmount > 0 && money > cfg.ReserveAmount;
	bool growing = false;
	if (cfg.ReserveGrowth > 0 && hist.size() >= 2
		&& now - hist.front().first >= window / 2)
		growing = (money - hist.front().second) >= cfg.ReserveGrowth;
	if (!overAmount && !growing) return;

	// The floor to protect: the Amount reserve if set, else 0 (growth-only can
	// spend the house down — expected; it self-regulates as growth then falls).
	int const floor = cfg.ReserveAmount > 0 ? cfg.ReserveAmount : 0;

	int const cd = cfg.ReserveCooldown > 0 ? cfg.ReserveCooldown : 300;
	auto const it = g_reserveLastFire.find(hIdx);
	if (it != g_reserveLastFire.end() && now - it->second < cd)
		return;

	// Build the first list item the house can build (prereqs + Owner=) and
	// afford, queued behind its own production. Spends surplus toward the
	// reserve floor over time instead of letting the AI hoard cash.
	for (auto const& id : cfg.ReserveBuild)
	{
		auto const pType = TechnoTypeClass::Find(id.c_str());
		if (!pType) continue;
		if (!IsTeamable(pType)) continue; // units only in this first cut (buildings
		                                  // need the Antares placement path)
		// Strict: TechLevel (no TechLevel=11), Owner=, prereqs — don't trust the
		// leaky CanBuild alone.
		if (!CanBuildStrict(pHouse, pType)) continue;
		int const cost = pType->GetCost();
		if (cost > 0 && money - cost < floor) continue; // keep the protected floor

		auto const pFactory = FindHouseFactory(pHouse, pType);
		if (!pFactory) continue;
		pFactory->DemandProduction(pType, pHouse, true);
		g_reserveLastFire[hIdx] = now;
		Debug::Log("[DoctrineExt] reserve spend: house=%s#%d builds %s ($%d, %s).\n",
			pHouse->get_ID(), hIdx, pType->ID, money,
			growing ? "growth" : "over-amount");
		return;
	}
}

void Teams::RushCheck(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (cfg.RushRatio <= 0.0) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const cd = cfg.RushCooldown > 0 ? cfg.RushCooldown : 1800;
	auto const it = g_rushLastFire.find(hIdx);
	if (it != g_rushLastFire.end() && now - it->second < cd)
		return;

	BuildPowerTable(now);

	// Combined allied power (self + allies) vs the enemies. Weakest enemy is the
	// rush target; a powered-DOWN enemy counts as weaker (defenses offline).
	double allied = 0.0, totalEnemy = 0.0, weakestPow = 1e18;
	HouseClass* pTarget = nullptr;
	for (int i = 0; i < HouseClass::Array.Count; ++i)
	{
		auto const pH = HouseClass::Array.GetItem(i);
		if (!pH || pH->Defeated || pH->IsObserver() || pH->IsNeutral()) continue;
		double const p = PowerOf(pH->ArrayIndex);
		if (pH == pHouse || pHouse->IsAlliedWith(pH))
		{
			allied += p;
			continue;
		}
		totalEnemy += p;
		double eff = p;
		if (pH->PowerDrain > pH->PowerOutput) eff *= 0.5; // power-down = softer
		if (eff < weakestPow) { weakestPow = eff; pTarget = pH; }
	}
	if (!pTarget) return; // no enemies

	// Need a real army first — otherwise an enemy at 0 power (early game) makes
	// "allied >= target x ratio" trivially true and we "rush" with a lone scout.
	if (allied < cfg.RushMinPower) return;

	// Only commit when we dominate the target AND won't leave ourselves weaker
	// than the rest of the enemies (don't overextend in a multi-way game).
	if (allied < weakestPow * cfg.RushRatio) return;
	if (allied < totalEnemy * (cfg.RushSafety > 0 ? cfg.RushSafety : 1.0)) return;

	// Aim the rush at the decapitation target — the enemy building whose loss
	// most cripples its rebuild chain (§10h) — so an all-in strikes the ConYard
	// / production rather than just roaming. Fall back to Hunt if none found.
	BuildingClass* const pDecap = cfg.DecapEnable ? DecapTarget(pHouse, pTarget) : nullptr;

	int sent = 0;
	for (int i = 0; i < TechnoClass::Array.Count; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!IsIdleArmed(pT, pHouse)) continue;
		auto const pFoot = static_cast<FootClass*>(pT);
		if (pDecap)
		{
			pFoot->SetTarget(pDecap);
			pFoot->QueueMission(Mission::Attack, false);
		}
		else
		{
			pFoot->ForceMission(Mission::Hunt);
		}
		++sent;
	}
	if (sent < (cfg.RushMinUnits > 0 ? cfg.RushMinUnits : 1))
		return; // too few idle units to be a real rush; retry when massed

	g_rushLastFire[hIdx] = now;
	Debug::Log("[DoctrineExt] RUSH: house=%s#%d -> %s#%d alliedPow=%.0f targetPow=%.0f "
		"totalEnemy=%.0f, committed %d, decap=%s.\n",
		pHouse->get_ID(), hIdx, pTarget->get_ID(), pTarget->ArrayIndex,
		allied, weakestPow, totalEnemy, sent,
		pDecap && pDecap->Type ? pDecap->Type->ID : "(roam)");
}

// Hold OFFENSIVE production (vehicles / aircraft / navy) so income funnels into the
// priority building — but leave INFANTRY and the ConYard free, so a defensive screen and
// base defenses still go up while the economy scales. blockRepair also halts the repair
// cash drain (only wanted for a brief one-off rush, not the long economy phase, where we
// want defenses kept healthy). Re-applied each pass while the hold is in force.
static void BudgetSeizeProduction(HouseClass* const pHouse, bool const blockRepair)
{
	for (auto const pFact : FactoryClass::Array)
	{
		if (!pFact || pFact->Owner != pHouse) continue;
		auto const pObj = pFact->Object;
		if (!pObj) continue;                              // idle — leave it free
		auto const what = pObj->WhatAmI();
		// Keep infantry (defensive formations) and buildings (ConYard = refineries +
		// base defenses) producing; suspend only offensive vehicle/aircraft/ship output.
		if (what == AbstractType::Building || what == AbstractType::Infantry) continue;
		pFact->Suspend(true);
	}
	if (!blockRepair) return;
	pHouse->Repairing = false;
	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		auto const pB = BuildingClass::Array.GetItem(i);
		if (!pB || pB->Owner != pHouse) continue;
		pB->IsBeingRepaired = false;
		pB->NeedsRepairs = false;
	}
}

// Undo a lockdown: resume every factory. Logs only on the actual transition.
static void BudgetRelease(HouseClass* const pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	auto const it = g_budgetLocked.find(hIdx);
	if (it == g_budgetLocked.end() || !it->second) return;
	for (auto const pFact : FactoryClass::Array)
		if (pFact && pFact->Owner == pHouse) pFact->Unsuspend(true);
	it->second = false;
	if (DoctrineConfig::Get().DebugTicks)
		Debug::Log("[DoctrineExt] budget: house=%s#%d lockdown released.\n",
			pHouse->get_ID(), hIdx);
}

// Refuse to let the AI liquidate its essentials. Each tick, cancel any in-progress sale
// (CurrentMission/QueuedMission == Selling) of the ConYard or a priority-list building by
// forcing it back to Guard. This stops the base-center firesale/relocate seen even at
// AIMode=0 with healthy cash — whatever decides to sell them (engine, modpack AI, a
// trigger), we veto it, so the economy the budget is scaling isn't torn down under it.
static void BudgetProtectEssentials(HouseClass* const pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	for (auto const pBld : pHouse->Buildings)
	{
		if (!pBld || !pBld->Type) continue;
		if (pBld->CurrentMission != Mission::Selling
			&& pBld->QueuedMission != Mission::Selling) continue;
		bool essential = pBld->Type->ConstructionYard;
		for (auto it = cfg.BudgetTiers.begin(); !essential && it != cfg.BudgetTiers.end(); ++it)
			for (auto const& id : it->Buildings)
				if (BuildingTypeClass::Find(id.c_str()) == pBld->Type) { essential = true; break; }
		if (!essential) continue;
		pBld->ForceMission(Mission::Guard);              // veto the sale
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] budget: house=%s#%d VETOED sale of essential %s.\n",
				pHouse->get_ID(), pHouse->ArrayIndex, pBld->Type->ID);
	}
}

// Forced budget (§ Rex 2026-10-30): an Easy AI that can't afford a pricey essential (a
// FreeUnit-inflated refinery) sells its base to pay for it, then — once that's stopped —
// stays stuck at one refinery, perpetually broke, under-defended. Walk the priority tiers,
// find the highest-priority building the house is below Count of and can build, plan it,
// and hold OFFENSIVE production so income funnels there — economy buildings (Count>1)
// continuously until the target, one-offs (Count==1) once the Reserve is reached. Infantry
// and the ConYard keep working throughout, so defenses/formations still go up.
void Teams::ForcedBudget(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.BudgetEnable || cfg.BudgetTiers.empty()) return;
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral() || pHouse->IsControlledByHuman())
		return;                                           // AI houses only

	// Protect essentials every tick (not throttled), so a sale is vetoed before it
	// completes (~24 frames) — this is cheap (only acts when something is selling).
	BudgetProtectEssentials(pHouse);

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.BudgetInterval > 0 ? cfg.BudgetInterval : 15;
	auto const it = g_budgetLastFire.find(hIdx);
	if (it != g_budgetLastFire.end() && now - it->second < interval) return;
	g_budgetLastFire[hIdx] = now;

	// Highest-priority building the house hasn't reached Count of yet AND can build now
	// (so we never hold forever on something whose prerequisites aren't met — those
	// earlier buildings get focused first, which is exactly the right order: power before
	// the refineries that need it).
	BuildingTypeClass* focus = nullptr; int focusTier = -1; int focusCount = 1;
	for (int t = 0; t < static_cast<int>(cfg.BudgetTiers.size()) && !focus; ++t)
	{
		auto const& tier = cfg.BudgetTiers[t];
		for (auto const& id : tier.Buildings)
		{
			auto const pBt = BuildingTypeClass::Find(id.c_str());
			if (!pBt || !pHouse->InOwners(pBt)) continue;
			if (pHouse->CountOwnedAndPresent(pBt) >= tier.Count) continue; // reached target
			if (!CanBuildStrict(pHouse, pBt)) continue;                    // can't build yet
			focus = pBt; focusTier = t; focusCount = tier.Count; break;
		}
	}

	if (!focus) { BudgetRelease(pHouse); return; }        // every priority satisfied

	// Make sure the AI actually plans it (no-op if a node for it is already pending).
	QueueBaseNode(pHouse, focus, pHouse->GetBaseCenter());

	int const reserve = cfg.BudgetTiers[focusTier].Reserve;
	int const cash = static_cast<int>(pHouse->Available_Money());

	// Economy scaling (Count>1, e.g. 3 refineries): hold offensive production CONTINUOUSLY
	// while below the target, breaking the one-refinery poverty trap — income funnels into
	// the economy while infantry + base defenses still build (repair left on). One-off
	// essential (Count==1): only once cash reaches the Reserve, rush it (also blocking
	// repair), then release. Either way infantry and the ConYard keep working.
	bool const economyMode = focusCount > 1;
	bool const engage = economyMode || cash >= reserve;

	if (engage)
	{
		bool const wasLocked = g_budgetLocked[hIdx];
		BudgetSeizeProduction(pHouse, /*blockRepair=*/!economyMode);
		g_budgetLocked[hIdx] = true;
		if (!wasLocked && cfg.DebugTicks)
			Debug::Log("[DoctrineExt] budget %s: house=%s#%d focus=%s tier=%d (%d/%d) cash=%d "
				"reserve=%d — holding offensive production%s (infantry+defenses keep building).\n",
				economyMode ? "ECONOMY" : "RUSH", pHouse->get_ID(), hIdx, focus->ID, focusTier,
				pHouse->CountOwnedAndPresent(focus), focusCount, cash, reserve,
				economyMode ? "" : " + repair");
	}
	else
	{
		BudgetRelease(pHouse);                            // below reserve: build normally
	}
}

// ─── Capture doctrine (§10j) ─────────────────────────────────────────────────
namespace
{
	std::map<int, int> g_captureLastFire;                                // house -> last pass frame
	// Per-house capture-target progress + temporary blacklist for UNREACHABLE targets
	// (e.g. an island derrick), keyed by the target cell (buildings don't move). Mirrors
	// the crate squad's guard so an engineer never freezes on an unreachable building.
	std::map<int, std::map<int, std::pair<int, int>>> g_captureProgress; // cellKey -> {lastDist, stuck}
	std::map<int, std::map<int, int>> g_captureBlacklist;                // cellKey -> expire frame

	// House -> who needs a type built now (counter-engineer, rush, capture) -> that type and the frame
	// the need lapses: the economy pause lets those types through until then (each Build.IgnorePause).
	enum PauseExemptFor { ExemptCounterEng, ExemptEngRush, ExemptCapture };
	std::map<int, std::map<int, std::pair<TechnoTypeClass*, int>>> g_pauseExempt;

	void ClearPauseExempt(int const hIdx, PauseExemptFor const who)
	{
		auto const it = g_pauseExempt.find(hIdx);
		if (it != g_pauseExempt.end()) it->second.erase(who);
	}

	void SetPauseExempt(int const hIdx, PauseExemptFor const who, TechnoTypeClass* const pType, int const until)
	{
		if (pType) g_pauseExempt[hIdx][who] = { pType, until };
		else ClearPauseExempt(hIdx, who);
	}

	int InProduction(HouseClass* pHouse, TechnoTypeClass* pType);   // urgent response, below
	void BuildUrgently(HouseClass* pHouse, TechnoTypeClass* pType);
	void ClearStaleUrgentPick(HouseClass* pHouse, int now);

	int CaptureCellKey(CellStruct const& c)
		{ return (static_cast<int>(c.X) << 16) | (static_cast<int>(c.Y) & 0xFFFF); }

	// Cheapest buildable, faction-correct engineer the house can field. Engineers are the
	// only unit that captures a building; honor prerequisites (barracks) per the toggle so
	// we never force one the house has no legitimate way to build.
	TechnoTypeClass* BestEngineer(HouseClass* const pHouse, bool const honorPrereq)
	{
		TechnoTypeClass* best = nullptr; int bestCost = 1 << 30;
		for (auto const pIt : InfantryTypeClass::Array)
		{
			if (!pIt || !pIt->Engineer) continue;
			if (!pHouse->InOwners(pIt)) continue;                        // faction-correct
			if (!CanBuildStrict(pHouse, pIt, honorPrereq)) continue;
			if (pIt->Cost < bestCost) { bestCost = pIt->Cost; best = pIt; }
		}
		return best;
	}

	// Does this building type match the modder's capture list? Empty list = any Capturable
	// building qualifies (so CAOILD and every tech structure are fair game).
	bool WantsCapture(BuildingTypeClass* const pBt)
	{
		if (!pBt || !pBt->Capturable) return false;
		auto const& list = DoctrineConfig::Get().CaptureTargets;
		if (list.empty()) return true;
		for (auto const& id : list)
			if (id == pBt->ID) return true;
		return false;
	}

	// Capturable buildings this house should grab, NEAREST base-center first. Skips
	// own/allied owners, (optionally) enemy-owned, out-of-range, and blacklisted ones.
	std::vector<std::pair<BuildingClass*, double>> FindCaptureTargets(HouseClass* const pHouse)
	{
		auto const& cfg = DoctrineConfig::Get();
		int const now = Unsorted::CurrentFrame;
		int const hIdx = pHouse->ArrayIndex;
		auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
		double const maxD = cfg.CaptureRadius > 0 ? static_cast<double>(cfg.CaptureRadius) * 256.0 : 0.0;
		auto const& bl = g_captureBlacklist[hIdx];
		std::vector<std::pair<BuildingClass*, double>> out;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0) continue;
			if (!WantsCapture(pB->Type)) continue;
			auto const pO = pB->Owner;
			if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;    // ours / an ally's
			if (!pO->IsNeutral() && !cfg.CaptureEnemyOwned) continue;         // enemy-owned off
			auto const c = pB->GetCoords();
			double const d = std::sqrt(static_cast<double>(c.X - base.X) * (c.X - base.X)
				+ static_cast<double>(c.Y - base.Y) * (c.Y - base.Y));
			if (maxD > 0.0 && d > maxD) continue;
			auto const blit = bl.find(CaptureCellKey(pB->GetMapCoords()));
			if (blit != bl.end() && blit->second > now) continue;            // temporarily given up
			out.push_back({ pB, d });
		}
		std::sort(out.begin(), out.end(),
			[](std::pair<BuildingClass*, double> const& a,
			   std::pair<BuildingClass*, double> const& b) { return a.second < b.second; });
		return out;
	}

	// Live engineers currently on a capture team.
	int EngineersOnTeam(TeamClass* const pTeam)
	{
		int n = 0;
		for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
			if (!pF->InLimbo && pF->Health > 0) ++n;
		return n;
	}

	// Existing capture team instance for the house, or null.
	TeamClass* FindCaptureTeam(int const hIdx)
	{
		char id[0x18];
		std::snprintf(id, sizeof(id), "DCAP%dTM", hIdx);
		auto const pTT = TeamTypeClass::Find(id);
		return (pTT && pTT->cntInstances > 0) ? pTT->FindFirstInstance() : nullptr;
	}
}

void Teams::CaptureDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CaptureEnable) return;                                       // opt-in
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;                         // AI houses only

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.CaptureInterval > 0 ? cfg.CaptureInterval : 90;
	auto const it = g_captureLastFire.find(hIdx);
	if (it != g_captureLastFire.end() && now - it->second < interval) return;
	g_captureLastFire[hIdx] = now;
	ClearStaleUrgentPick(pHouse, now);

	auto const targets = FindCaptureTargets(pHouse);
	if (targets.empty()) { ClearPauseExempt(hIdx, ExemptCapture); return; }   // nothing to grab

	// One engineer per target, capped by MaxTeams.
	int const cap = cfg.CaptureMaxTeams > 0 ? cfg.CaptureMaxTeams : 2;
	int desired = static_cast<int>(targets.size());
	if (desired > cap) desired = cap;

	// How many engineers are already committed (so we only build the shortfall).
	auto const pPrev = FindCaptureTeam(hIdx);
	int const have = pPrev ? EngineersOnTeam(pPrev) : 0;

	// The only unit that captures. Honor prereqs (needs a barracks) per the toggle.
	TechnoTypeClass* const pEng = BestEngineer(pHouse, cfg.HonorPrereqForCapture());

	int const perPass = cfg.CaptureEngineerMax > 0 ? cfg.CaptureEngineerMax : 2;
	int want = desired - have;                                            // engineers short
	if (want < 0) want = 0;
	if (want > perPass) want = perPass;

	// Create/refresh the capture team. When a buildable engineer exists and we're short,
	// the taskforce carries it so the AI PRODUCES `want` (the proven build path for an AI
	// house); otherwise null — recruit-only, grabbing any idle engineer it already owns.
	auto const pTeam = EnsureSquadTeam(pHouse, desired,
		(pEng && want > 0) ? pEng : nullptr, "DCAP", want);
	if (!pTeam) return;
	pTeam->Type->Priority = DoctrineSquadPriority;                       // AI teams can't take them back

	// Recruit any engineer the house already owns (diversion), so a spare engineer goes to work
	// instead of sitting — off AI teams too (they own ~all infantry). One the recruit gate
	// refuses is skipped, never pulled off its team and stranded.
	for (int i = 0; i < TechnoClass::Array.Count && EngineersOnTeam(pTeam) < desired; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
		if (pT->WhatAmI() != AbstractType::Infantry) continue;
		auto const pType = pT->GetTechnoType();
		if (!pType || pType->WhatAmI() != AbstractType::InfantryType) continue;
		if (!static_cast<InfantryTypeClass*>(pType)->Engineer) continue;
		TryAddToTeam(pTeam, static_cast<FootClass*>(pT));
	}
	// Still short after taking the spares: make the engineer the barracks' next build (Build.Urgent),
	// unless one is already in production, and let it (in production too) past the economy pause
	// (Build.IgnorePause).
	bool const shortOf = pEng && want > 0 && EngineersOnTeam(pTeam) < desired;
	if (shortOf && cfg.CaptureBuildUrgent && EngineersOnTeam(pTeam) + InProduction(pHouse, pEng) < desired)
		BuildUrgently(pHouse, pEng);
	SetPauseExempt(hIdx, ExemptCapture, shortOf && cfg.CaptureBuildIgnorePause ? pEng : nullptr,
		now + 2 * interval);

	// Unreachable guard (once per pass): for each engineer, track progress toward its
	// nearest target; if it hasn't closed the gap over several passes, blacklist that
	// building briefly so the engineer is freed for a reachable one instead of freezing.
	auto& prog = g_captureProgress[hIdx];
	int const giveUp = cfg.CrateGiveUpPasses > 0 ? cfg.CrateGiveUpPasses : 4;
	std::vector<bool> taken(targets.size(), false);
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (pF->InLimbo || pF->Health <= 0) continue;
		auto const fc = pF->GetCoords();
		int best = -1; double bestD = 1e18;
		for (int k = 0; k < static_cast<int>(targets.size()); ++k)
		{
			if (taken[k]) continue;
			auto const c = targets[k].first->GetCoords();
			double const d = std::sqrt(static_cast<double>(c.X - fc.X) * (c.X - fc.X)
				+ static_cast<double>(c.Y - fc.Y) * (c.Y - fc.Y));
			if (d < bestD) { bestD = d; best = k; }
		}
		if (best < 0) break;                                            // more engineers than targets
		taken[best] = true;
		if (bestD <= 2.0 * 256.0) continue;                            // within ~2 cells: mid-capture
		int const key = CaptureCellKey(targets[best].first->GetMapCoords());
		auto const pit = prog.find(key);
		int const stuck = (pit != prog.end() && bestD >= pit->second.first - 256.0)
			? pit->second.second + 1 : 0;
		if (stuck >= giveUp)
		{
			g_captureBlacklist[hIdx][key] = now
				+ (cfg.CrateBlacklistTime > 0 ? cfg.CrateBlacklistTime : 1800);
			prog.erase(key);
			if (cfg.DebugTicks)
				Debug::Log("[DoctrineExt] capture unreachable: house=%s#%d gave up at %.1f "
					"cells, blacklisted %s.\n", pHouse->get_ID(), hIdx, bestD / 256.0,
					targets[best].first->Type->ID);
		}
		else prog[key] = { static_cast<int>(bestD), stuck };
	}

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] capture: house=%s#%d targets=%d desired=%d have=%d "
			"eng=%s build=%d nearest=%s@%.1f.\n",
			pHouse->get_ID(), hIdx, static_cast<int>(targets.size()), desired,
			EngineersOnTeam(pTeam), pEng ? pEng->get_ID() : "(none)", want,
			targets[0].first->Type->ID, targets[0].second / 256.0);
}

void Teams::SteerCapture(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CaptureEnable) return;
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;

	auto const pTeam = FindCaptureTeam(pHouse->ArrayIndex);
	if (!pTeam || !pTeam->FirstUnit) return;

	auto const targets = FindCaptureTargets(pHouse);
	if (targets.empty()) return;

	// Assign each engineer to its nearest un-taken target and (re)issue the capture
	// order every tick, so the base AI can't countermand it before the engineer lands.
	std::vector<bool> taken(targets.size(), false);
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (pF->InLimbo || pF->Health <= 0) continue;
		auto const fc = pF->GetCoords();
		int best = -1; double bestD = 1e18;
		for (int k = 0; k < static_cast<int>(targets.size()); ++k)
		{
			if (taken[k]) continue;
			auto const c = targets[k].first->GetCoords();
			double const d = std::sqrt(static_cast<double>(c.X - fc.X) * (c.X - fc.X)
				+ static_cast<double>(c.Y - fc.Y) * (c.Y - fc.Y));
			if (d < bestD) { bestD = d; best = k; }
		}
		if (best < 0) break;                                            // more engineers than targets
		taken[best] = true;
		auto const pB = targets[best].first;
		// Canonical engineer-capture order: target the building + Mission::Capture (the
		// mission handles the approach). Re-issued each tick so the base AI can't steal it.
		pF->SetTarget(pB);
		pF->QueueMission(Mission::Capture, false);
	}
}

// ─── Counter-engineer / urgent response (§10l) ──────────────────────────────
namespace
{
	std::map<int, int> g_counterEngLastFire;                      // house -> last production pass frame
	// house -> responder UniqueID -> UniqueID of the engineer it was retasked onto. Squad members and
	// loose responders alike: a unit the recruit gate refuses (e.g. one on Hunt) still gets the order.
	std::map<int, std::map<DWORD, DWORD>> g_responders;
	// house -> responder UniqueID -> the cell it was sent to cut its engineer off (Predict.Intercept); our
	// own record, since the engine rewrites FootClass::Destination mid-route.
	std::map<int, std::map<DWORD, CellClass*>> g_interceptCells;

	// True only while DoctrineExt itself sells a building to deny a capture, so the firesale block
	// (ShouldBlockSell, reached from the BuildingClass::Sell hook inside that same call) lets it through.
	bool g_denySelling = false;

	// UniqueIDs of buildings an AI captured from another player and kept ([Doctrine.Captured]), so
	// DenyRecapture can sell one before its old owner's engineer takes it back.
	std::set<DWORD> g_keptCaptures;

	// Every live engineer's last two position samples (>= 8 frames apart), so each has a MEASURED
	// velocity from its second sighting on — the arrival estimate the deny-sell trusts. Sampled at
	// most once per frame, by whichever house ticks.
	struct EngTrack { int F0 = 0, F1 = 0; CoordStruct C0{}, C1{}; };
	std::map<DWORD, EngTrack> g_engTracks;
	int g_engTrackFrame = -1;

	// Capture-denial judgement, per house: the sales each engineer has caused (DenySell.PerEngineer)
	// and recent sale frames (DenySell.Max per DenySell.Window), so one engineer bouncing from building
	// to building can't walk the AI into selling its whole base; the last sacrifice made against each
	// engineer (its survivors' expected damage is credited until they're on foot and counted as
	// responders); and the (engineer, building) pairs already logged as let go.
	struct SacrificeSale { int Frame = 0; int Count = 0; double Damage = 0.0; };
	std::map<int, std::map<DWORD, int>> g_denySells;
	std::map<int, std::deque<int>> g_denySellFrames;
	std::map<int, std::map<DWORD, SacrificeSale>> g_sacrifices;
	std::map<int, std::set<std::pair<DWORD, DWORD>>> g_letGo;

	int IncomeKindOf(BuildingTypeClass* pBt);                        // economy section, below

	bool IsLiveEngineer(TechnoClass* const pT)
	{
		if (!pT || pT->InLimbo || pT->Health <= 0) return false;
		if (pT->WhatAmI() != AbstractType::Infantry) return false;
		auto const pType = pT->GetTechnoType();
		return pType && pType->WhatAmI() == AbstractType::InfantryType
			&& static_cast<InfantryTypeClass*>(pType)->Engineer;
	}

	// A unit the urgent response treats as an engineer threat (ThreatTypes): every Engineer=yes
	// infantry by default; with a modder list, exactly those infantry/vehicle types whatever their
	// flags (a C4 commando, a spy, a demo truck); with none/null, nothing. Our own capturers stay
	// Engineer=yes (IsLiveEngineer) — only those can capture.
	bool IsThreatType(TechnoClass* const pT)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngThreatNone) return false;
		if (cfg.CounterEngThreatTypes.empty()) return IsLiveEngineer(pT);
		if (!pT || pT->InLimbo || pT->Health <= 0) return false;
		auto const what = pT->WhatAmI();
		if (what != AbstractType::Infantry && what != AbstractType::Unit) return false;
		auto const pType = pT->GetTechnoType();
		return pType && ListHas(cfg.CounterEngThreatTypes, pType->ID);
	}

	// A passenger that ThreatTypes would count on foot (it rides in limbo, so IsThreatType says no).
	bool IsThreatAboard(FootClass* const p)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngThreatNone || !p || p->Health <= 0) return false;
		auto const pType = p->GetTechnoType();
		if (!pType) return false;
		if (cfg.CounterEngThreatTypes.empty())
			return pType->WhatAmI() == AbstractType::InfantryType && static_cast<InfantryTypeClass*>(pType)->Engineer;
		return ListHas(cfg.CounterEngThreatTypes, pType->ID);
	}

	// A transport type Transports watches: listed, else (auto) any type that carries passengers.
	bool IsWatchedTransport(TechnoClass* const pT)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngTransportsNone || !pT || pT->InLimbo || pT->Health <= 0) return false;
		auto const what = pT->WhatAmI();
		if (what != AbstractType::Unit && what != AbstractType::Aircraft && what != AbstractType::Infantry) return false;
		auto const pType = pT->GetTechnoType();
		if (!pType) return false;
		return cfg.CounterEngTransports.empty() ? pType->Passengers > 0 : ListHas(cfg.CounterEngTransports, pType->ID);
	}

	// An enemy transport treated as an engineer threat (Transports.Cargo): with assume, every watched
	// transport, its cargo unseen; with know, one an engineer (ThreatTypes) rides in.
	bool IsThreatCarrier(TechnoClass* const pT)
	{
		int const cargo = DoctrineConfig::Get().CounterEngTransportCargo;
		if (cargo <= 0 || !IsWatchedTransport(pT)) return false;
		if (cargo == 1) return true;
		for (auto p = pT->Passengers.FirstPassenger; p; p = static_cast<FootClass*>(p->NextObject))
			if (IsThreatAboard(p)) return true;
		return false;
	}

	void SampleEngineers(int const now)
	{
		if (g_engTrackFrame == now) return;
		g_engTrackFrame = now;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			// Transports are sampled whatever this house's Transports.Cargo, so a house of another
			// difficulty sampling this frame doesn't leave them unmeasured.
			if (!IsThreatType(pT) && !IsWatchedTransport(pT)) continue;
			auto& t = g_engTracks[pT->UniqueID];
			auto const c = pT->GetCoords();
			if (t.F1 <= 0 || now - t.F1 > 60)                   // new, or back out of a transport
				{ t.F0 = t.F1 = now; t.C0 = t.C1 = c; continue; }
			if (now - t.F1 < 8) continue;
			t.F0 = t.F1; t.C0 = t.C1; t.F1 = now; t.C1 = c;
		}
		for (auto it = g_engTracks.begin(); it != g_engTracks.end(); )
			if (now - it->second.F1 > 300) it = g_engTracks.erase(it); else ++it;
	}

	struct EngThreat
	{
		FootClass* Eng = nullptr;
		BuildingClass* Goal = nullptr;   // the building it is (most likely) going to enter
		bool Ours = false;               // Goal is ours (sellable), not neutral tech we want
		bool Intent = false;             // its own order (Target/Destination) names Goal
		const char* Guess = "";          // how Goal was picked when its order doesn't name it (Predict.Goal):
		                                 // "order" (its move order's cell), "heading" or "nearest"
		bool Measured = false;           // has a measured velocity
		bool Approaching = false;        // measurably closing on Goal
		double Dist = 0.0;               // leptons to Goal's footprint
		double Speed = 1.0;              // leptons/frame: measured, else its locomotor step
		double Eta = 0.0;                // frames until it reaches Goal
		int Hp = 0;
		int Armor = 0;
		double Need = 0.0;               // expected damage wanted: HP x RespondMargin
		double Exp = 0.0;                // from covering defenses + committed responders
		double ExpMax = 0.0;             // ... + every responder that could still be committed
		int Resp = 0;                    // responders committed
		int Defs = 0;                    // defenses covering the door
		bool Sold = false;               // Goal deny-sold this tick
		bool Carrier = false;            // Eng is a transport with (or assumed to have) engineers aboard
		bool Air = false;                // ... an aircraft: only anti-air can shoot it
	};

	// Buildings an enemy engineer could be coming for: ours, plus neutral capturables we want (when the
	// capture doctrine is on). Never enemy-owned ones — an enemy engineer at home is not a threat.
	std::vector<BuildingClass*> EngineerAnchors(HouseClass* const pHouse)
	{
		std::vector<BuildingClass*> out;
		bool const contest = DoctrineConfig::Get().CaptureEnable;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0) continue;
			if (!pB->Type->Capturable) continue;                     // an engineer can't take it anyway
			if (pB->Owner == pHouse) out.push_back(pB);
			else if (contest && pB->Owner && pB->Owner->IsNeutral() && WantsCapture(pB->Type))
				out.push_back(pB);
		}
		return out;
	}

	// The anchor an engineer's move order leads to (Predict.Goal): its destination or attack-move cell
	// within two cells of an anchor's footprint, the nearest such. A human player often walks engineers
	// to a cell by the door and only orders the capture there, so its order never names the building.
	// Destination is the engine's own live pointer (it paths to it every frame).
	BuildingClass* OrderedGoal(FootClass* const pE, std::vector<BuildingClass*> const& anchors)
	{
		AbstractClass* const dests[2] = { pE->Destination, pE->MegaDestination };
		for (auto const pD : dests)
		{
			if (!pD || pD->WhatAmI() != AbstractType::Cell) continue;
			auto const dc = pD->GetCoords();
			BuildingClass* best = nullptr;
			double bestD = 2.0 * 256.0;
			for (auto const pB : anchors)
			{
				double const d = DistToFootprint(dc, pB);
				if (d <= bestD) { bestD = d; best = pB; }
			}
			if (best) return best;
		}
		return nullptr;
	}

	// True if the engineer's own order names another house's building, or its move order ends within two cells
	// of one (Predict.Elsewhere): it is going there, so walking past ours is no threat. Target and Destination
	// are the engine's live pointers. Run after the anchor checks, so an order ending by ours as well wins.
	bool OrderedElsewhere(FootClass* const pE, HouseClass* const pHouse, std::vector<BuildingClass*> const& anchors)
	{
		auto const other = [&](AbstractClass* const p) {
			if (!p || p->WhatAmI() != AbstractType::Building) return false;
			auto const pB = static_cast<BuildingClass*>(p);
			return pB->Owner != pHouse && std::find(anchors.begin(), anchors.end(), pB) == anchors.end();
		};
		if (other(pE->Target) || other(pE->Destination)) return true;
		AbstractClass* const dests[2] = { pE->Destination, pE->MegaDestination };
		for (auto const pD : dests)
		{
			if (!pD || pD->WhatAmI() != AbstractType::Cell) continue;
			auto const mc = static_cast<CellClass*>(pD)->MapCoords;
			for (short dx = -2; dx <= 2; ++dx)
				for (short dy = -2; dy <= 2; ++dy)
					if (auto const pC = MapClass::Instance.TryGetCellAt(
						CellStruct { static_cast<short>(mc.X + dx), static_cast<short>(mc.Y + dy) }))
						if (other(pC->GetBuilding())) return true;
		}
		return false;
	}

	// The anchor an engineer is walking at (Predict.Goal): ahead of it along its measured heading, within
	// Predict.Width cells of that line (past the building's own half-size), the first it would reach.
	BuildingClass* HeadingGoal(CoordStruct const& c, EngTrack const& tr, std::vector<BuildingClass*> const& anchors)
	{
		double const vx = tr.C1.X - tr.C0.X, vy = tr.C1.Y - tr.C0.Y;
		double const len = std::sqrt(vx * vx + vy * vy);
		if (len < 32.0) return nullptr;                               // standing: no heading
		double const ux = vx / len, uy = vy / len;
		int const w = DoctrineConfig::Get().CounterEngPredictWidth;
		double const width = (w > 0 ? w : 3) * 256.0;
		BuildingClass* best = nullptr;
		double bestAlong = 1e18;
		for (auto const pB : anchors)
		{
			auto const bc = pB->GetCoords();
			double const rx = bc.X - c.X, ry = bc.Y - c.Y;
			double const along = rx * ux + ry * uy;
			if (along <= 0.0) continue;                               // behind it
			double const half = std::max(pB->Type->GetFoundationWidth(), pB->Type->GetFoundationHeight(false)) * 128.0;
			if (std::fabs(rx * uy - ry * ux) - half > width) continue;  // off its line
			if (along < bestAlong) { bestAlong = along; best = pB; }
		}
		return best;
	}

	// Enemy engineers coming for an anchor, soonest arrival first. One whose own order names an anchor
	// (or, with Predict.Goal, whose move order ends at one) is a threat at any range; otherwise it must be
	// within Radius AND measurably closing in (or already at the door) — one idling by our wall or walking
	// past is left alone, as is one whose order names another house's building (Predict.Elsewhere). Its goal
	// is then the anchor it's walking at (Predict.Goal), else the nearest.
	std::vector<EngThreat> FindEngineerThreats(HouseClass* const pHouse)
	{
		auto const& cfg = DoctrineConfig::Get();
		std::vector<EngThreat> out;
		auto const anchors = EngineerAnchors(pHouse);
		if (anchors.empty()) return out;
		double const radius = (cfg.CounterEngRadius > 0 ? cfg.CounterEngRadius : 12) * 256.0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			bool const carrier = !IsThreatType(pT) && IsThreatCarrier(pT);
			if (!carrier && !IsThreatType(pT)) continue;
			auto const pO = pT->Owner;
			if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;    // enemy only
			auto const pE = static_cast<FootClass*>(pT);
			auto const c = pE->GetCoords();
			auto const tit = g_engTracks.find(pE->UniqueID);
			bool const moved = tit != g_engTracks.end() && tit->second.F1 > tit->second.F0;

			EngThreat t;
			for (auto const pB : anchors)                         // pointer compare only, never dereferenced
				if (pE->Target == pB || pE->Destination == pB) { t.Goal = pB; t.Intent = true; break; }
			if (!t.Goal && cfg.CounterEngPredictGoal && (t.Goal = OrderedGoal(pE, anchors)) != nullptr)
				{ t.Intent = true; t.Guess = "order"; }
			if (!t.Goal && cfg.CounterEngPredictElsewhere && OrderedElsewhere(pE, pHouse, anchors))
				continue;
			if (!t.Goal && cfg.CounterEngPredictGoal && moved
				&& (t.Goal = HeadingGoal(c, tit->second, anchors)) != nullptr)
				t.Guess = "heading";
			if (t.Goal) t.Dist = DistToFootprint(c, t.Goal);
			else
			{
				t.Dist = 1e18;
				for (auto const pB : anchors)
				{
					double const d = DistToFootprint(c, pB);
					if (d < t.Dist) { t.Dist = d; t.Goal = pB; }
				}
				t.Guess = "nearest";
			}
			if (!t.Intent && t.Dist > radius) continue;
			t.Eng = pE;
			t.Ours = t.Goal->Owner == pHouse;

			if (moved)
			{
				auto const& tr = tit->second;
				t.Measured = true;
				t.Speed = Dist2D(tr.C0, tr.C1) / (tr.F1 - tr.F0);
				t.Approaching = DistToFootprint(tr.C0, t.Goal) - DistToFootprint(tr.C1, t.Goal) > 16.0;
			}
			if (t.Speed < 1.0) t.Speed = StepSpeed(pE);      // standing or unmeasured: it can walk
			if (!t.Intent && !t.Approaching && t.Dist > 2.0 * 256.0) continue;
			t.Eta = t.Dist / t.Speed;                         // a carrier: until it's at the door to unload
			t.Hp = pE->Health;
			t.Armor = static_cast<int>(pE->GetTechnoType()->Armor);
			t.Carrier = carrier;
			t.Air = pE->WhatAmI() == AbstractType::Aircraft;
			out.push_back(t);
		}
		std::sort(out.begin(), out.end(),
			[](EngThreat const& a, EngThreat const& b) { return a.Eta < b.Eta; });
		return out;
	}

	// Can this defense fire now? Not while being built, sold or EMP'd. BuildingClass::IsPowerOnline
	// (0x4555D0) also gates on per-building power state, so it's only asked of Powered= types — a
	// pillbox shoots in a blacked-out base.
	bool DefenseOnline(BuildingClass* const pD)
	{
		auto const m = pD->GetCurrentMission();
		if (m == Mission::Construction || m == Mission::Selling || pD->IsUnderEMP()) return false;
		return !pD->Type->Powered || pD->IsPowerOnline();
	}

	// Expected damage a defense deals the engineer before it reaches the door: it fires only once the
	// engineer walks into its range, and only if the door is inside that range. (EffDPSVs is per 10
	// frames.) With freeAt (ShareDefenses) it shoots one engineer at a time: it starts on this one no
	// sooner than freeAt frames from now (busy with engineers arriving sooner), and freeAt moves on to
	// when it has killed this one or the engineer is in.
	double DefenseContribution(BuildingClass* const pD, EngThreat const& t, double& range, double* const freeAt = nullptr)
	{
		double const dps = EffDPSVs(pD->Type, t.Armor, t.Air) / 10.0;
		if (dps <= 0.0) return 0.0;
		range = BestRangeVs(pD->Type, t.Air);
		auto const dc = pD->GetCoords();
		if (DistToFootprint(dc, t.Goal) > range) return 0.0;
		double start = std::max(0.0, Dist2D(dc, t.Eng->GetCoords()) - range) / t.Speed;
		if (freeAt) start = std::max(start, *freeAt);
		double const window = t.Eta - start;
		if (window <= 0.0) return 0.0;
		if (freeAt) *freeAt = start + std::min(window, t.Hp / dps);
		return dps * window;
	}

	// Expected damage a responder deals before the engineer reaches the door: after turning onto it
	// (ReactionFrames, waived if it's already on it) and closing to weapon range of the engineer or the
	// door, whichever is nearer.
	double ResponderContribution(FootClass* const pR, EngThreat const& t, bool const already)
	{
		auto const pType = pR->GetTechnoType();
		double const dps = EffDPSVs(pType, t.Armor, t.Air) / 10.0;
		if (dps <= 0.0) return 0.0;
		double const range = BestRangeVs(pType, t.Air);
		auto const rc = pR->GetCoords();
		double const gap = std::min(Dist2D(rc, t.Eng->GetCoords()), DistToFootprint(rc, t.Goal)) - range;
		double const react = already ? 0.0 : static_cast<double>(DoctrineConfig::Get().CounterEngReaction);
		double const window = t.Eta - (std::max(0.0, gap) / StepSpeed(pR) + react);
		return window > 0.0 ? dps * window : 0.0;
	}

	// Where a responder cuts the engineer off (Predict.Intercept), as AA screens the bearing incoming air
	// comes in on: the first point on the engineer's straight path to its building that the responder
	// gets within weapon range of no later than the engineer gets there; the door if there's none. Null
	// when the engineer is already in range (attack it) or the path is degenerate.
	CellClass* InterceptCell(FootClass* const pR, EngThreat const& t)
	{
		auto const rc = pR->GetCoords();
		auto const ec = t.Eng->GetCoords();
		double const range = BestRangeVs(pR->GetTechnoType(), t.Air);
		if (Dist2D(rc, ec) <= range + 128.0) return nullptr;
		auto const gc = t.Goal->GetCoords();
		double const len = Dist2D(ec, gc);
		if (len < 1.0) return nullptr;
		double const ux = (gc.X - ec.X) / len, uy = (gc.Y - ec.Y) / len;
		double const speed = StepSpeed(pR);
		double s = 0.0;
		for (; s < t.Dist; s += 128.0)
		{
			CoordStruct const p { ec.X + static_cast<int>(ux * s), ec.Y + static_cast<int>(uy * s), ec.Z };
			if (std::max(0.0, Dist2D(rc, p) - range) / speed <= s / t.Speed) break;
		}
		s = std::min(s, t.Dist);
		return MapClass::Instance.TryGetCellAt(
			CoordStruct { ec.X + static_cast<int>(ux * s), ec.Y + static_cast<int>(uy * s), ec.Z });
	}

	// A unit that could be retasked onto an engineer: our armed ground/naval unit that isn't an
	// engineer, miner or MCV, and isn't busy on a capture or rush squad.
	bool IsResponderCandidate(TechnoClass* const pT, HouseClass* const pHouse)
	{
		if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) return false;
		auto const what = pT->WhatAmI();
		if (what != AbstractType::Unit && what != AbstractType::Infantry) return false;
		auto const pType = pT->GetTechnoType();
		if (!pType || pType->ResourceGatherer || pType->DeploysInto) return false;
		if (pType->WhatAmI() == AbstractType::InfantryType
			&& static_cast<InfantryTypeClass*>(pType)->Engineer) return false;
		if (!HasOffensiveWeapon(pType)) return false;
		auto const pF = static_cast<FootClass*>(pT);
		return !OnSquad(pF, "DCAP") && !OnSquad(pF, "DRSH");
	}

	bool CanDenySell(BuildingClass* const pB, HouseClass* const pHouse)
	{
		return pB->Owner == pHouse && !pB->Type->Unsellable
			&& !ListHas(DoctrineConfig::Get().CounterEngDenySellExclude, pB->Type->ID)
			&& pB->GetCurrentMission() != Mission::Selling;
	}

	// Denied even when DenySell.PerEngineer or DenySell.Max is used up (DenySell.Uncapped; it must still
	// be worth denying). Auto: a construction yard — cheaper sales must never cost the house its yard.
	bool DenyUncapped(BuildingClass* const pB)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngDenySellUncappedNone) return false;
		if (!cfg.CounterEngDenySellUncapped.empty()) return ListHas(cfg.CounterEngDenySellUncapped, pB->Type->ID);
		return pB->Type->ConstructionYard;
	}

	// Could the house build this building again (DenySell.KeepIrreplaceable)? A yard: it owns another yard
	// or an MCV, or can build an MCV ([General] BaseUnit) and pay for it. Anything else: it owns a yard
	// and can pay for a new one.
	bool Replaceable(BuildingClass* const pB, HouseClass* const pHouse)
	{
		int const cash = static_cast<int>(pHouse->Available_Money());
		bool yard = false;
		for (auto const pO : pHouse->Buildings)
			if (pO && pO != pB && pO->Type && !pO->InLimbo && pO->Health > 0 && pO->Type->ConstructionYard
				&& pO->GetCurrentMission() != Mission::Selling)
				yard = true;
		if (!pB->Type->ConstructionYard) return yard && cash >= pB->Type->GetActualCost(pHouse);
		if (yard) return true;
		for (auto const pU : UnitClass::Array)
			if (pU && pU->Owner == pHouse && !pU->InLimbo && pU->Health > 0 && pU->Type->DeploysInto
				&& pU->Type->DeploysInto->ConstructionYard)
				return true;
		for (auto const pMcv : RulesClass::Instance->BaseUnit)
			if (CanBuildStrict(pHouse, pMcv) && cash >= pMcv->GetActualCost(pHouse)) return true;
		return false;
	}

	// What a house's army is worth: the cost of its armed units (no miners, MCVs or engineers).
	int ArmyValue(HouseClass* const pHouse)
	{
		int v = 0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry && what != AbstractType::Aircraft) continue;
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer || pType->DeploysInto || !HasOffensiveWeapon(pType)) continue;
			if (pType->WhatAmI() == AbstractType::InfantryType && static_cast<InfantryTypeClass*>(pType)->Engineer) continue;
			v += pType->GetActualCost(pHouse);
		}
		return v;
	}

	// Is losing this building to a capture worse than selling it (DenySell.Types)? Auto: a
	// construction yard or superweapon always; a factory or income building only when it's the
	// house's last of that kind — a spare barracks, a power plant or a lab can go, since losing it
	// doesn't cost the house its production.
	bool WorthDenying(BuildingClass* const pB, HouseClass* const pHouse)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngDenySellNone) return false;
		auto const pBt = pB->Type;
		if (!cfg.CounterEngDenySellTypes.empty()) return ListHas(cfg.CounterEngDenySellTypes, pBt->ID);
		if (pBt->ConstructionYard || pBt->HasSuperWeapon()) return true;
		bool const factory = pBt->Factory != AbstractType::None;
		bool const income = IncomeKindOf(pBt) > 0;
		if (!factory && !income) return false;
		bool spareFactory = false, spareIncome = false;
		for (auto const pO : pHouse->Buildings)
		{
			if (!pO || pO == pB || !pO->Type || pO->InLimbo || pO->Health <= 0) continue;
			if (pO->GetCurrentMission() == Mission::Selling) continue;
			if (factory && pO->Type->Factory == pBt->Factory && pO->Type->Naval == pBt->Naval) spareFactory = true;
			if (income && IncomeKindOf(pO->Type) > 0) spareIncome = true;
		}
		return (factory && !spareFactory) || (income && !spareIncome);
	}

	// Could selling this building put survivors in an engineer's way (Sacrifice.Types)? Auto: a
	// sellable, unimportant building — not a construction yard, factory, income building,
	// superweapon, defense, wall or armed building, and not a power plant the base can't spare.
	bool Sacrificable(BuildingClass* const pB, HouseClass* const pHouse, BuildingClass* const pGoal)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngSacrificeNone || pB == pGoal) return false;
		if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0 || pB->Owner != pHouse) return false;
		auto const pBt = pB->Type;
		if (pBt->Unsellable) return false;
		auto const m = pB->GetCurrentMission();
		if (m == Mission::Selling || m == Mission::Construction) return false;
		if (!cfg.CounterEngSacrificeTypes.empty()) return ListHas(cfg.CounterEngSacrificeTypes, pBt->ID);
		if (pBt->ConstructionYard || pBt->HasSuperWeapon() || pBt->Factory != AbstractType::None) return false;
		if (IncomeKindOf(pBt) > 0 || pBt->IsBaseDefense || pBt->Wall || HasOffensiveWeapon(pBt)) return false;
		return pBt->PowerBonus <= 0 || pHouse->PowerOutput - pBt->PowerBonus >= pHouse->PowerDrain;
	}

	// The infantry a sold building of this house releases: the side's crew (RulesClass AlliedCrew /
	// SovietCrew / ThirdCrew, else Technician), as TechnoClass::GetCrew (0x707D20) picks it. Not
	// called through GetCrew itself — that rolls the game's synchronized random number.
	InfantryTypeClass* CrewTypeOf(HouseClass* const pHouse)
	{
		auto const r = RulesClass::Instance;
		switch (pHouse->SideIndex)
		{
		case 0: return r->AlliedCrew;
		case 1: return r->SovietCrew;
		case 2: return r->ThirdCrew;
		default: return r->Technician;
		}
	}

	// Leptons from c to the segment a-b (the engineer's straight path to its goal).
	double SegDist(CoordStruct const& c, CoordStruct const& a, CoordStruct const& b)
	{
		double const vx = b.X - a.X, vy = b.Y - a.Y;
		double const len2 = vx * vx + vy * vy;
		double u = len2 > 0.0 ? ((c.X - a.X) * vx + (c.Y - a.Y) * vy) / len2 : 0.0;
		u = std::clamp(u, 0.0, 1.0);
		double const dx = a.X + u * vx - c.X, dy = a.Y + u * vy - c.Y;
		return std::sqrt(dx * dx + dy * dy);
	}

	// Expected damage the survivors of selling pB deal the engineer before it reaches the door, and
	// the frames they need for it (`window`, out): they walk out Sacrifice.Delay frames after the sale
	// (BuildingClass::GetCrewCount, 0x451330: the refund over the side's SurvivorDivisor, 1..5), then
	// close to weapon range of the engineer or the door like a responder.
	double SacrificeContribution(BuildingClass* const pB, EngThreat const& t, HouseClass* const pHouse,
		int& count, double& window)
	{
		auto const& cfg = DoctrineConfig::Get();
		count = pB->GetCrewCount();
		window = 0.0;
		auto const pCrew = CrewTypeOf(pHouse);
		if (count <= 0 || !pCrew) return 0.0;
		double const dps = EffDPSVs(pCrew, t.Armor, t.Air) / 10.0;
		if (dps <= 0.0) return 0.0;
		double const range = BestRangeVs(pCrew, t.Air);
		double const gap = std::min(DistToFootprint(t.Eng->GetCoords(), pB),
			DistToFootprint(pB->GetCoords(), t.Goal)) - range;
		double const speed = pCrew->Speed > 1 ? static_cast<double>(pCrew->Speed) : 1.0;
		double const delay = cfg.CounterEngSacrificeDelay > 0 ? cfg.CounterEngSacrificeDelay : 60;
		window = t.Eta - (delay + std::max(0.0, gap) / speed + cfg.CounterEngReaction);
		return window > 0.0 ? count * dps * window : 0.0;
	}

	// Interceptors of this type the house is building right now: counted as owned, so an urgent
	// build doesn't queue a second one while the first is still in the barracks.
	int InProduction(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		int n = 0;
		for (auto const pFact : FactoryClass::Array)
			if (pFact && pFact->Owner == pHouse && pFact->Object && pFact->Object->GetTechnoType() == pType) ++n;
		return n;
	}

	// Make pType the house's next infantry pick: HouseClass::ProducingInfantryTypeIndex (+0x5654), the
	// slot HouseClass::AI_Infantry (0x4FEEE0) fills from what its teams are missing and the barracks
	// takes its next build from. A squad's taskforce request competes there with every AI team's and
	// loses to any bigger demand (no house ever built an interceptor that way); an urgent unit replaces
	// the AI's pending pick instead. What a barracks is already building is left alone.
	//
	// The barracks clears the slot when it takes the pick. One it never takes (no factory can build it
	// after all) would stall the AI's infantry for good — AI_Infantry returns at once while the slot is
	// full — so a pick of ours left untaken UrgentPickTimeout frames is cleared and not made again for
	// UrgentPickBackoff frames.
	struct UrgentPick { int Index = -1; int Since = 0; int BlockedUntil = 0; };
	std::map<int, UrgentPick> g_urgentPicks;                          // house -> our last pick
	constexpr int UrgentPickTimeout = 450;
	constexpr int UrgentPickBackoff = 1800;

	void ClearStaleUrgentPick(HouseClass* const pHouse, int const now)
	{
		auto const it = g_urgentPicks.find(pHouse->ArrayIndex);
		if (it == g_urgentPicks.end() || it->second.Index < 0) return;
		auto& p = it->second;
		bool const log = DoctrineConfig::Get().DebugTicks;
		if (pHouse->ProducingInfantryTypeIndex != p.Index)                          // taken, or replaced
		{
			if (log)
				Debug::Log("[DoctrineExt] urgent-build: house=%s#%d %s left the slot after %df (now %d).\n",
					pHouse->get_ID(), pHouse->ArrayIndex, InfantryTypeClass::Array.GetItem(p.Index)->ID,
					now - p.Since, pHouse->ProducingInfantryTypeIndex);
			p.Index = -1;
			return;
		}
		if (now - p.Since <= UrgentPickTimeout) return;
		if (log)
			Debug::Log("[DoctrineExt] urgent-build: house=%s#%d %s never taken in %df; slot cleared, no urgent "
				"builds for %df.\n", pHouse->get_ID(), pHouse->ArrayIndex,
				InfantryTypeClass::Array.GetItem(p.Index)->ID, now - p.Since, UrgentPickBackoff);
		pHouse->ProducingInfantryTypeIndex = -1;
		p.Index = -1;
		p.BlockedUntil = now + UrgentPickBackoff;
	}

	void BuildUrgently(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (!pType || pType->WhatAmI() != AbstractType::InfantryType) return;
		if (pHouse->Available_Money() < pType->Cost) return;
		int const now = Unsorted::CurrentFrame;
		ClearStaleUrgentPick(pHouse, now);
		auto& p = g_urgentPicks[pHouse->ArrayIndex];
		if (now < p.BlockedUntil) return;
		if (p.Index >= 0) return;               // another urgent pick of ours is still waiting: first come
		                                        // first served (the counter-engineer ticks before the rush)
		int const idx = InfantryTypeClass::Array.FindItemIndex(static_cast<InfantryTypeClass*>(pType));
		if (idx < 0 || pHouse->ProducingInfantryTypeIndex == idx) return;            // already next
		if (DoctrineConfig::Get().DebugTicks)
			Debug::Log("[DoctrineExt] urgent-build: house=%s#%d next infantry %s (was %d).\n", pHouse->get_ID(),
				pHouse->ArrayIndex, pType->ID, pHouse->ProducingInfantryTypeIndex);
		pHouse->ProducingInfantryTypeIndex = idx;
		p.Index = idx;
		p.Since = now;
	}

	// Fastest buildable, faction-correct armed infantry to keep as a standing interceptor (attack dogs
	// win on speed). Honors the modder list (Units) if set; else any armed infantry. Never an engineer.
	TechnoTypeClass* BestAntiEngineer(HouseClass* const pHouse, bool const honorPrereq)
	{
		auto const& list = DoctrineConfig::Get().CounterEngUnits;
		TechnoTypeClass* best = nullptr; int bestSpeed = -1;
		for (auto const pIt : InfantryTypeClass::Array)
		{
			if (!pIt || pIt->Engineer) continue;
			if (!list.empty()) { if (!ListHas(list, pIt->ID)) continue; }
			else if (!HasOffensiveWeapon(pIt)) continue;          // auto: must be armed
			if (!pHouse->InOwners(pIt)) continue;
			if (!CanBuildStrict(pHouse, pIt, honorPrereq)) continue;
			if (pIt->Speed > bestSpeed) { bestSpeed = pIt->Speed; best = pIt; }
		}
		return best;
	}

	TeamClass* FindCounterEngTeam(int const hIdx)
	{
		char id[0x18];
		std::snprintf(id, sizeof(id), "DANT%dTM", hIdx);
		auto const pTT = TeamTypeClass::Find(id);
		return (pTT && pTT->cntInstances > 0) ? pTT->FindFirstInstance() : nullptr;
	}

	// Pickets (Picket), per house: the bearing from the base center each enemy engineer coming for us was
	// first seen on, and when; the engineers already counted; the posts of the last placement pass; and
	// which picket (UniqueID) holds which post.
	struct PicketSighting { int Frame = 0; double Bearing = 0.0; };
	std::map<int, std::vector<PicketSighting>> g_picketSightings;
	std::map<int, std::set<DWORD>> g_picketSeen;
	std::map<int, std::vector<CellClass*>> g_picketPosts;
	std::map<int, std::map<DWORD, int>> g_picketAssign;
	std::map<int, int> g_picketLastFire;
	constexpr int PicketSectors = 16;

	int PicketSector(double const bearing)
	{
		double const step = 2.0 * 3.14159265358979 / PicketSectors;
		return static_cast<int>(std::floor((bearing + 3.14159265358979) / step)) & (PicketSectors - 1);
	}

	// Learn where engineers come from: every enemy engineer (or carrier) that comes for one of OUR
	// buildings counts once, on the bearing it was first seen on. Learned whether or not this house
	// pickets, so turning Picket on for a difficulty later in a game still has the history.
	void RecordPicketSightings(HouseClass* const pHouse, std::vector<EngThreat> const& threats, int const now)
	{
		auto const center = CellClass::Cell2Coord(pHouse->GetBaseCenter());
		if (center.X == 0 && center.Y == 0) return;
		auto& seen = g_picketSeen[pHouse->ArrayIndex];
		if (seen.size() > 1024) seen.clear();
		auto& sights = g_picketSightings[pHouse->ArrayIndex];
		for (auto const& t : threats)
		{
			if (!t.Ours || !(t.Intent || t.Approaching) || !seen.insert(t.Eng->UniqueID).second) continue;
			auto const c = t.Eng->GetCoords();
			if (c.X == center.X && c.Y == center.Y) continue;
			sights.push_back({ now, std::atan2(static_cast<double>(c.Y - center.Y), static_cast<double>(c.X - center.X)) });
		}
	}

	TeamClass* FindPicketTeam(int const hIdx)
	{
		char id[0x18];
		std::snprintf(id, sizeof(id), "DPKT%dTM", hIdx);
		auto const pTT = TeamTypeClass::Find(id);
		return (pTT && pTT->cntInstances > 0) ? pTT->FindFirstInstance() : nullptr;
	}

	// Send every picket back to the AI: the team goes, its members go back on guard.
	void ReleasePickets(int const hIdx)
	{
		g_picketPosts.erase(hIdx);
		g_picketAssign.erase(hIdx);
		char id[0x18];
		std::snprintf(id, sizeof(id), "DPKT%dTM", hIdx);
		if (auto const pTT = TeamTypeClass::Find(id))
			if (pTT->cntInstances > 0) pTT->DestroyAllInstances();
	}

	// May this unit picket (Picket.Units)? Listed, else (auto) any armed ground unit that hurts an
	// engineer ([General] Engineer's armor) — the responder rules, minus our other squads' members.
	bool IsPicketCandidate(TechnoClass* const pT, HouseClass* const pHouse, TeamClass* const pTeam)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.CounterEngPicketUnitsNone || !IsResponderCandidate(pT, pHouse)) return false;
		auto const pF = static_cast<FootClass*>(pT);
		if (pF->Team && pF->Team != pTeam && (IsDoctrineTeam(pF->Team) || OnSquad(pF, "DANT"))) return false;
		auto const pType = pT->GetTechnoType();
		if (!cfg.CounterEngPicketUnits.empty()) return ListHas(cfg.CounterEngPicketUnits, pType->ID);
		auto const pEng = RulesClass::Instance->Engineer;
		int const armor = pEng ? static_cast<int>(pEng->Armor) : 0;
		return EffDPSVs(pType, armor, false) > 0.0;
	}

	// What Picket.Build builds: the first buildable of Picket.Units, else the interceptor (Units).
	TechnoTypeClass* PicketBuildType(HouseClass* const pHouse)
	{
		auto const& cfg = DoctrineConfig::Get();
		bool const honor = cfg.HonorPrereqForCounterEng();
		for (auto const& id : cfg.CounterEngPicketUnits)
		{
			TechnoTypeClass* pType = InfantryTypeClass::Find(id.c_str());
			if (!pType) pType = UnitTypeClass::Find(id.c_str());
			if (pType && pHouse->InOwners(pType) && CanBuildStrict(pHouse, pType, honor)) return pType;
		}
		return cfg.CounterEngPicketUnits.empty() ? BestAntiEngineer(pHouse, honor) : nullptr;
	}
}

void Teams::CounterEngineers(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CounterEngEnable) return;                                    // opt-in
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;                         // AI houses only

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	ClearStaleUrgentPick(pHouse, now);
	SampleEngineers(now);
	auto threats = FindEngineerThreats(pHouse);
	RecordPicketSightings(pHouse, threats, now);
	auto& resp = g_responders[hIdx];

	// Our side, in one scan: responder candidates, and the live units already committed.
	std::vector<FootClass*> cands;
	std::map<DWORD, FootClass*> committed;
	if (!threats.empty() || !resp.empty())
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!IsResponderCandidate(pT, pHouse)) continue;
			auto const pF = static_cast<FootClass*>(pT);
			if (resp.count(pF->UniqueID)) committed[pF->UniqueID] = pF;
			else cands.push_back(pF);
		}

	std::map<DWORD, FootClass*> threatEng;
	for (auto const& t : threats) threatEng[t.Eng->UniqueID] = t.Eng;

	// Stand down responders whose engineer is dead or no longer a threat (stop chasing it home).
	auto pSquad = FindCounterEngTeam(hIdx);
	for (auto it = resp.begin(); it != resp.end(); )
	{
		auto const cit = committed.find(it->first);
		if (cit != committed.end() && threatEng.count(it->second)) { ++it; continue; }
		if (cit != committed.end())
		{
			auto const pR = cit->second;
			if (pSquad && pR->Team == pSquad) pSquad->LiberateMember(pR);
			pR->SetTarget(nullptr);
			pR->QueueMission(Mission::Guard, false);
			committed.erase(cit);
		}
		it = resp.erase(it);
	}

	double const margin = cfg.CounterEngRespondMargin > 0.0 ? cfg.CounterEngRespondMargin : 1.5;
	int const perEng = cfg.CounterEngRespondMax > 0 ? cfg.CounterEngRespondMax : 4;
	double const respondR = (cfg.CounterEngRespondRadius > 0 ? cfg.CounterEngRespondRadius : 15) * 256.0;
	int const sense = cfg.SenseInterval > 0 ? cfg.SenseInterval : 15;
	int const lead = cfg.CounterEngDenySellLead > 0 ? cfg.CounterEngDenySellLead : 2 * sense;
	double const sellMargin = cfg.CounterEngDenySellMargin > 0.0 ? cfg.CounterEngDenySellMargin : 1.0;
	int const sacDelay = cfg.CounterEngSacrificeDelay > 0 ? cfg.CounterEngSacrificeDelay : 60;
	double const sacRadius = (cfg.CounterEngSacrificeRadius > 0 ? cfg.CounterEngSacrificeRadius : 6) * 256.0;
	std::set<BuildingClass*> retargeted;                                  // soonest threat keeps a tower
	std::map<BuildingClass*, double> defFree;                             // ShareDefenses: busy until (frames)

	// Forget engineers no longer tracked (dead, or out of sight 300+ frames), and sales out of the window.
	auto& sells = g_denySells[hIdx];
	auto& sacs = g_sacrifices[hIdx];
	for (auto it = sells.begin(); it != sells.end(); )
		if (!g_engTracks.count(it->first)) it = sells.erase(it); else ++it;
	for (auto it = sacs.begin(); it != sacs.end(); )
		if (!g_engTracks.count(it->first)) it = sacs.erase(it); else ++it;
	auto& sellFrames = g_denySellFrames[hIdx];
	int const window = cfg.CounterEngDenySellWindow > 0 ? cfg.CounterEngDenySellWindow : 1800;
	while (!sellFrames.empty() && now - sellFrames.front() > window) sellFrames.pop_front();
	auto& letGo = g_letGo[hIdx];
	if (letGo.size() > 256) letGo.clear();

	for (auto& t : threats)
	{
		t.Need = t.Hp * margin;

		// Defenses covering the door count for free; the ones that can already reach the engineer
		// are pointed at it (it outranks whatever they were shooting).
		for (auto const pD : pHouse->Buildings)
		{
			if (!pD || !pD->Type || pD->InLimbo || pD->Health <= 0) continue;
			if (!HasOffensiveWeapon(pD->Type) || !DefenseOnline(pD)) continue;
			double range = 0.0;
			double const c = DefenseContribution(pD, t, range,
				cfg.CounterEngShareDefenses ? &defFree[pD] : nullptr);
			if (c <= 0.0) continue;
			t.Exp += c; ++t.Defs;
			if (cfg.CounterEngRetargetDefenses && !retargeted.count(pD)
				&& Dist2D(pD->GetCoords(), t.Eng->GetCoords()) <= range)
			{
				retargeted.insert(pD);
				if (pD->Target != t.Eng) pD->SetTarget(t.Eng);
			}
		}

		for (auto const& kv : resp)
		{
			if (kv.second != t.Eng->UniqueID) continue;
			auto const cit = committed.find(kv.first);
			if (cit == committed.end()) continue;
			t.Exp += ResponderContribution(cit->second, t, true);
			++t.Resp;
		}

		// Units near the door or the engineer, best expected damage first.
		std::vector<std::pair<double, FootClass*>> pool;
		auto const ec = t.Eng->GetCoords();
		for (auto const pR : cands)
		{
			if (resp.count(pR->UniqueID)) continue;              // taken by an earlier threat
			auto const rc = pR->GetCoords();
			if (DistToFootprint(rc, t.Goal) > respondR && Dist2D(rc, ec) > respondR) continue;
			double const c = ResponderContribution(pR, t, false);
			if (c > 0.0) pool.push_back({ c, pR });
		}
		std::sort(pool.begin(), pool.end(),
			[](std::pair<double, FootClass*> const& a, std::pair<double, FootClass*> const& b)
			{ return a.first > b.first; });
		t.ExpMax = t.Exp;
		for (auto const& p : pool) t.ExpMax += p.first;

		// Retask until the engineer is expected to die with margin to spare.
		if (cfg.CounterEngRespond)
			for (auto const& p : pool)
			{
				if (t.Exp >= t.Need || t.Resp >= perEng) break;
				auto const pR = p.second;
				if (!pSquad) pSquad = EnsureSquadTeam(pHouse, 1, nullptr, "DANT", 0);
				TryAddToTeam(pSquad, pR);             // refused (e.g. on Hunt): a loose responder, still ordered
				resp[pR->UniqueID] = t.Eng->UniqueID;
				committed[pR->UniqueID] = pR;
				t.Exp += p.first; ++t.Resp;
			}

		// Nobody gets there before it goes in (it's at the door, or standing by it): still send the
		// Respond.Late nearest units that can hurt it, soonest arrival first — an engineer that hasn't gone
		// in yet can still be caught. They add nothing to the expected damage DenySell trusts.
		int const lateMax = std::min(cfg.CounterEngRespondLate, perEng);
		if (cfg.CounterEngRespond && lateMax > 0 && t.Exp < t.Need && t.Resp < lateMax)
		{
			std::vector<std::pair<double, FootClass*>> late;
			for (auto const pR : cands)
			{
				if (resp.count(pR->UniqueID)) continue;
				auto const rc = pR->GetCoords();
				if (DistToFootprint(rc, t.Goal) > respondR && Dist2D(rc, ec) > respondR) continue;
				if (EffDPSVs(pR->GetTechnoType(), t.Armor, t.Air) <= 0.0) continue;
				late.push_back({ Dist2D(rc, ec) / StepSpeed(pR), pR });
			}
			std::sort(late.begin(), late.end(),
				[](std::pair<double, FootClass*> const& a, std::pair<double, FootClass*> const& b)
				{ return a.first < b.first; });
			for (auto const& p : late)
			{
				if (t.Resp >= lateMax) break;
				auto const pR = p.second;
				if (!pSquad) pSquad = EnsureSquadTeam(pHouse, 1, nullptr, "DANT", 0);
				TryAddToTeam(pSquad, pR);
				resp[pR->UniqueID] = t.Eng->UniqueID;
				committed[pR->UniqueID] = pR;
				++t.Resp;
			}
		}

		// A sacrifice already made against this engineer counts until its survivors are on foot; from
		// then on they're ordinary responders.
		auto const sit = sacs.find(t.Eng->UniqueID);
		if (sit != sacs.end() && now - sit->second.Frame < sacDelay + 2 * sense)
			t.ExpMax += sit->second.Damage;

		// Everything below needs evidence it's coming for THIS building of ours: its own order, a
		// measured approach, or it's at the door. A carrier is only shot at unless Transports.DenySell.
		if (!t.Ours || !(t.Intent || t.Approaching || t.Dist <= 128.0)) continue;
		if (t.Carrier && !cfg.CounterEngTransportDenySell) continue;

		// Short of firepower: first a sacrifice — sell a cheap building by the engineer's path so its
		// survivors finish the job. Only a building whose survivors alone cover the shortfall (the
		// cheapest such), sold at the last moment that still works (two base ticks of slack).
		int const sacMade = sit != sacs.end() ? sit->second.Count : 0;
		if (cfg.CounterEngSacrifice && t.ExpMax < t.Need
			&& (cfg.CounterEngSacrificeMax <= 0 || sacMade < cfg.CounterEngSacrificeMax))
		{
			double const shortfall = t.Need - t.ExpMax;
			auto const ec = t.Eng->GetCoords();
			auto const gc = t.Goal->GetCoords();
			BuildingClass* best = nullptr;
			int bestCount = 0;
			double bestDmg = 0.0, bestWin = 0.0;
			for (auto const pB : pHouse->Buildings)
			{
				if (!Sacrificable(pB, pHouse, t.Goal)) continue;
				double const half = std::max(pB->Type->GetFoundationWidth(),
					pB->Type->GetFoundationHeight(false)) * 128.0;
				if (SegDist(pB->GetCoords(), ec, gc) - half > sacRadius) continue;
				int count = 0;
				double win = 0.0;
				double const dmg = SacrificeContribution(pB, t, pHouse, count, win);
				if (dmg < shortfall) continue;
				if (!best || pB->Type->Cost < best->Type->Cost)
					{ best = pB; bestCount = count; bestDmg = dmg; bestWin = win; }
			}
			// Frames of survivor fire the shortfall takes; until the window shrinks to that (+ slack),
			// a later sale still works and an interceptor or responder may yet make it unnecessary.
			if (best && bestWin <= bestWin * shortfall / bestDmg + 2.0 * sense)
			{
				g_denySelling = true;
				best->Sell(-1);
				g_denySelling = false;
				auto& s = sacs[t.Eng->UniqueID];
				s.Frame = now; ++s.Count; s.Damage = bestDmg;
				t.ExpMax += bestDmg;
				if (cfg.DebugTicks)
					Debug::Log("[DoctrineExt] counter-eng SACRIFICE: house=%s#%d sold %s for %d survivors vs "
						"%s#%u -> %s, %.1f cells out, eta=%.0ff, their expected damage %.0f covers shortfall %.0f.\n",
						pHouse->get_ID(), hIdx, best->Type->ID, bestCount, t.Eng->GetTechnoType()->ID,
						t.Eng->UniqueID, t.Goal->Type->ID, t.Dist / 256.0, t.Eta, bestDmg, shortfall);
				continue;
			}
		}

		// Nothing we have kills it before it walks in: sell the building first — an engineer reaching a
		// building that is mid-sale is turned away (0x519EB2). But only a building worth denying
		// (DenySell.Types), and never more than DenySell.PerEngineer sales to one engineer or
		// DenySell.Max per DenySell.Window for the house: a bounced engineer just walks to the next
		// building, and selling the whole base to stop one engineer is worse than losing a power plant.
		// A building we captured and kept is sold before it's retaken whatever DenySell says
		// ([Doctrine.Captured] DenyRecapture): every kept capture was worth keeping, and keeping it for
		// a retake hands it back. The sale caps still apply.
		bool const recapture = cfg.CapturedDenyRecapture && g_keptCaptures.count(t.Goal->UniqueID);
		if (!(cfg.CounterEngDenySell || recapture) || t.Eta > lead || t.ExpMax >= t.Hp * sellMargin
			|| !CanDenySell(t.Goal, pHouse)) continue;
		DWORD const uid = t.Eng->UniqueID;
		const char* why = nullptr;
		const char* capped = nullptr;
		if (cfg.CounterEngDenySellPerEngineer > 0 && sells[uid] >= cfg.CounterEngDenySellPerEngineer)
			capped = "this engineer already cost us a sale (DenySell.PerEngineer)";
		else if (cfg.CounterEngDenySellMax > 0 && static_cast<int>(sellFrames.size()) >= cfg.CounterEngDenySellMax)
			capped = "house sale cap reached (DenySell.Max per DenySell.Window)";
		bool const uncapped = capped && DenyUncapped(t.Goal);
		if (!recapture && !WorthDenying(t.Goal, pHouse))
			why = "not worth denying (DenySell.Types)";
		else if (capped && !uncapped)
			why = capped;
		else if (!recapture && cfg.CounterEngDenySellKeepIrreplaceable && !Replaceable(t.Goal, pHouse))
		{
			// Selling a building we can't build again loses it for good; captured, we can retake it.
			// Only an army that can win without it (SellIfStronger) sells it anyway, to deny it.
			int const ours = ArmyValue(pHouse);
			int const theirs = t.Eng->Owner ? ArmyValue(t.Eng->Owner) : 0;
			if (cfg.CounterEngDenySellIfStronger <= 0.0 || ours < theirs * cfg.CounterEngDenySellIfStronger)
			{
				static char keepWhy[192];
				std::snprintf(keepWhy, sizeof(keepWhy), "we couldn't build it again and our army (%d) isn't "
					"%.1fx theirs (%d): retake it instead (DenySell.KeepIrreplaceable / SellIfStronger)",
					ours, cfg.CounterEngDenySellIfStronger, theirs);
				why = keepWhy;
			}
		}
		if (why)
		{
			if (cfg.DebugTicks && letGo.insert({ uid, t.Goal->UniqueID }).second)
				Debug::Log("[DoctrineExt] counter-eng LET-GO: house=%s#%d keeps %s — %s#%u %.1f cells out, "
					"eta=%.0ff, best-case damage %.0f < hp %d: %s.\n", pHouse->get_ID(), hIdx,
					t.Goal->Type->ID, t.Eng->GetTechnoType()->ID, uid, t.Dist / 256.0, t.Eta, t.ExpMax, t.Hp, why);
			continue;
		}
		g_denySelling = true;
		t.Goal->Sell(-1);
		g_denySelling = false;
		t.Sold = true;
		++sells[uid];
		sellFrames.push_back(now);
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] counter-eng DENY-SELL: house=%s#%d sold %s — %s#%u %.1f cells out, "
				"eta=%.0ff, best-case damage %.0f < hp %d%s%s.\n", pHouse->get_ID(), hIdx,
				t.Goal->Type->ID, t.Eng->GetTechnoType()->ID, uid, t.Dist / 256.0,
				t.Eta, t.ExpMax, t.Hp, uncapped ? " (DenySell.Uncapped: past the sale caps)" : "",
				recapture ? " (DenyRecapture: we captured it)" : "");
	}

	// Steer every responder onto its engineer (re-issued only when it drifted, so the attack isn't
	// restarted each tick; the base AI and old teams can't countermand it in between). Out of range, with
	// Predict.Intercept, it heads for where it cuts the engineer off instead, and waits there.
	std::map<DWORD, EngThreat const*> threatOf;
	for (auto const& t : threats) threatOf[t.Eng->UniqueID] = &t;
	auto& cut = g_interceptCells[hIdx];
	for (auto it = cut.begin(); it != cut.end(); )
		if (!resp.count(it->first)) it = cut.erase(it); else ++it;
	for (auto const& kv : resp)
	{
		auto const cit = committed.find(kv.first);
		auto const eit = threatEng.find(kv.second);
		if (cit == committed.end() || eit == threatEng.end()) continue;
		auto const pR = cit->second;
		auto const pCut = cfg.CounterEngPredictIntercept ? InterceptCell(pR, *threatOf[kv.second]) : nullptr;
		if (pCut)
		{
			auto& prev = cut[pR->UniqueID];
			auto const cc = pCut->GetCellCoords();
			bool const shifted = !prev || Dist2D(prev->GetCellCoords(), cc) > 256.0;   // cut-off point moved
			bool const strayed = pR->GetCurrentMission() != Mission::Move               // countermanded short
				&& Dist2D(pR->GetCoords(), prev ? prev->GetCellCoords() : cc) > 384.0;  // of it
			if (shifted || strayed)
			{
				pR->SetTarget(nullptr);
				pR->SetDestination(pCut, true);
				pR->QueueMission(Mission::Move, false);
				prev = pCut;
			}
			continue;
		}
		cut.erase(pR->UniqueID);
		if (pR->Target != eit->second || pR->GetCurrentMission() != Mission::Attack)
		{
			pR->SetTarget(eit->second);
			pR->QueueMission(Mission::Attack, false);
		}
	}

	// Standing interceptors (throttled): while some threat outruns everything we could send, keep
	// up to Max of the fast interceptor around. Sized from what the house OWNS, not squad members —
	// built units start teamless and are retasked as responders when needed.
	int const interval = cfg.CounterEngInterval > 0 ? cfg.CounterEngInterval : 30;
	auto const fit = g_counterEngLastFire.find(hIdx);
	if (fit != g_counterEngLastFire.end() && now - fit->second < interval) return;
	g_counterEngLastFire[hIdx] = now;

	int shortOf = 0;
	for (auto const& t : threats)
		if (!t.Carrier && t.ExpMax < t.Need) ++shortOf;                 // a dog can't catch a transport
	auto const pDog = cfg.CounterEngBuild ? BestAntiEngineer(pHouse, cfg.HonorPrereqForCounterEng()) : nullptr;
	int owned = 0, building = 0;
	if (pDog)
	{
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->Health > 0 && pT->GetTechnoType() == pDog)
				++owned;
		}
		building = InProduction(pHouse, pDog);
	}
	int const cap = cfg.CounterEngMax > 0 ? cfg.CounterEngMax : 3;
	int want = (pDog ? std::min(shortOf, cap) : 0) - owned - building;
	if (want < 0) want = 0;
	if (want > 0)
	{
		EnsureSquadTeam(pHouse, 1, pDog, "DANT", want);
		if (cfg.CounterEngBuildUrgent) BuildUrgently(pHouse, pDog);
	}
	else if (pSquad) EnsureSquadTeam(pHouse, 1, nullptr, "DANT", 0);   // stop asking
	SetPauseExempt(hIdx, ExemptCounterEng,                           // lapses two passes after the last need
		pDog && shortOf > 0 && cfg.CounterEngBuildIgnorePause ? pDog : nullptr, now + 2 * interval);

	if (!cfg.DebugTicks || threats.empty()) return;
	Debug::Log("[DoctrineExt] counter-eng: house=%s#%d threats=%d responders=%d cutting-off=%d dog=%s owned=%d "
		"building=%d build=%d sold=%d/%d.\n", pHouse->get_ID(), hIdx, static_cast<int>(threats.size()),
		static_cast<int>(resp.size()), static_cast<int>(cut.size()), pDog ? pDog->get_ID() : "(none)", owned,
		building, want, static_cast<int>(sellFrames.size()), cfg.CounterEngDenySellMax);
	int shown = 0;
	for (auto const& t : threats)
	{
		if (++shown > 4) break;
		Debug::Log("[DoctrineExt]   eng=%s#%u -> %s%s%s%s dist=%.1fc eta=%.0ff v=%.1f(%s step=%d) intent=%d "
			"approach=%d hp=%d exp=%.0f max=%.0f need=%.0f resp=%d defs=%d%s%s.\n",
			t.Eng->GetTechnoType()->ID, t.Eng->UniqueID, t.Goal->Type->ID, t.Ours ? "" : "(neutral)",
			*t.Guess ? " by " : "", t.Guess,
			t.Dist / 256.0, t.Eta, t.Speed, t.Measured ? "measured" : "assumed", t.Eng->GetCurrentSpeed(),
			t.Intent, t.Approaching, t.Hp, t.Exp, t.ExpMax, t.Need, t.Resp, t.Defs, t.Sold ? " SOLD" : "",
			!t.Carrier ? "" : cfg.CounterEngTransportCargo == 2 ? " CARRIER(engineer aboard)"
				: " CARRIER(cargo assumed)");
	}
}


void Teams::EngineerPickets(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral() || pHouse->IsControlledByHuman()) return;
	int const hIdx = pHouse->ArrayIndex;
	if (!cfg.CounterEngEnable || !cfg.CounterEngPicket || cfg.CounterEngPicketUnitsNone)
	{
		if (g_picketPosts.count(hIdx)) ReleasePickets(hIdx);
		return;
	}
	int const now = Unsorted::CurrentFrame;
	int const interval = cfg.CounterEngPicketInterval > 0 ? cfg.CounterEngPicketInterval : 150;
	auto const fit = g_picketLastFire.find(hIdx);
	if (fit != g_picketLastFire.end() && now - fit->second < interval) return;
	g_picketLastFire[hIdx] = now;

	auto const center = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	if (center.X == 0 && center.Y == 0) return;

	// The posts ring the base just past its outermost building, like the defenders' perimeter. Only the
	// base itself: walking out from the center, a gap of more than 6 cells ends it, so a building captured
	// across the map doesn't move the ring there (11-08 test: a 166-cell "edge").
	std::vector<double> dists;
	for (auto const pBld : pHouse->Buildings)
		if (pBld && !pBld->InLimbo)
			dists.push_back(Dist2D(pBld->GetCoords(), center));
	std::sort(dists.begin(), dists.end());
	double radius = 0.0;
	for (double const d : dists)
	{
		if (d - radius > 6.0 * 256.0) break;
		radius = d;
	}
	radius += std::max(0, cfg.CounterEngPicketDistance) * 256.0;
	radius = std::max(radius, 5.0 * 256.0);

	// Bearings: the sectors engineers have come from most (Picket.MinSightings or more), never two side
	// by side; then, with Picket.TowardEnemy, the nearest enemy bases' bearings for the posts left.
	auto& sights = g_picketSightings[hIdx];
	if (cfg.CounterEngPicketMemory > 0)
		sights.erase(std::remove_if(sights.begin(), sights.end(),
			[&](PicketSighting const& s) { return now - s.Frame > cfg.CounterEngPicketMemory; }), sights.end());
	int count[PicketSectors] = {};
	double sx[PicketSectors] = {}, sy[PicketSectors] = {};
	for (auto const& s : sights)
	{
		int const k = PicketSector(s.Bearing);
		++count[k]; sx[k] += std::cos(s.Bearing); sy[k] += std::sin(s.Bearing);
	}
	int const maxPosts = std::max(1, cfg.CounterEngPicketPosts);
	int const minSight = std::max(1, cfg.CounterEngPicketMinSightings);
	bool used[PicketSectors] = {};
	auto const taken = [&](int const k)
		{ return used[k] || used[(k + 1) % PicketSectors] || used[(k + PicketSectors - 1) % PicketSectors]; };
	std::vector<double> bearings;
	int learned = 0;
	while (static_cast<int>(bearings.size()) < maxPosts)
	{
		int best = -1;
		for (int k = 0; k < PicketSectors; ++k)
			if (count[k] >= minSight && !taken(k) && (best < 0 || count[k] > count[best])) best = k;
		if (best < 0) break;
		used[best] = true;
		bearings.push_back(std::atan2(sy[best], sx[best]));
		++learned;
	}
	if (cfg.CounterEngPicketTowardEnemy && static_cast<int>(bearings.size()) < maxPosts)
	{
		std::vector<std::pair<double, double>> enemies;                 // distance, bearing
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto const pO = HouseClass::Array.GetItem(i);
			if (!pO || pO == pHouse || pO->Defeated || pO->IsObserver() || pO->IsNeutral()) continue;
			if (pHouse->IsAlliedWith(pO)) continue;
			auto const ec = CellClass::Cell2Coord(pO->GetBaseCenter());
			if (ec.X == 0 && ec.Y == 0) continue;
			enemies.push_back({ Dist2D(ec, center),
				std::atan2(static_cast<double>(ec.Y - center.Y), static_cast<double>(ec.X - center.X)) });
		}
		std::sort(enemies.begin(), enemies.end());
		for (auto const& e : enemies)
		{
			if (static_cast<int>(bearings.size()) >= maxPosts) break;
			int const k = PicketSector(e.second);
			if (taken(k)) continue;
			used[k] = true;
			bearings.push_back(e.second);
		}
	}

	// A post is the ring cell on its bearing, stepped inward off water and cliffs (up to half way in).
	std::vector<CellClass*> posts;
	for (double const b : bearings)
		for (double r = radius; r >= radius * 0.5; r -= 256.0)
		{
			CoordStruct const pt { center.X + static_cast<int>(std::cos(b) * r),
				center.Y + static_cast<int>(std::sin(b) * r), center.Z };
			auto const pCell = MapClass::Instance.TryGetCellAt(pt);
			if (pCell && pCell->LandType != LandType::Water && pCell->LandType != LandType::Rock)
				{ posts.push_back(pCell); break; }
		}
	if (posts.empty()) { ReleasePickets(hIdx); return; }
	g_picketPosts[hIdx] = posts;

	// Keep each picket on its post; fill the empty places from the cheapest willing units, teamless
	// first, nearest the post; units the counter-engineer has retasked are left to it.
	int const perPost = std::max(1, cfg.CounterEngPicketPerPost);
	int const want = static_cast<int>(posts.size()) * perPost;
	auto const pTeam = EnsureSquadTeam(pHouse, want, nullptr, "DPKT", 0);
	if (!pTeam) return;
	auto& assign = g_picketAssign[hIdx];
	auto const& resp = g_responders[hIdx];
	std::vector<int> filled(posts.size(), 0);
	std::map<DWORD, int> kept;
	std::vector<FootClass*> loose;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (pF->InLimbo || pF->Health <= 0) continue;
		auto const a = assign.find(pF->UniqueID);
		if (a != assign.end() && a->second < static_cast<int>(posts.size()) && filled[a->second] < perPost)
			{ ++filled[a->second]; kept[pF->UniqueID] = a->second; }
		else loose.push_back(pF);
	}
	auto const vacancy = [&]() { for (size_t k = 0; k < posts.size(); ++k) if (filled[k] < perPost) return static_cast<int>(k); return -1; };
	for (auto const pF : loose)
	{
		int const k = vacancy();
		if (k < 0) { pTeam->LiberateMember(pF); pF->QueueMission(Mission::Guard, false); continue; }
		++filled[k]; kept[pF->UniqueID] = k;
	}
	int recruited = 0;
	if (vacancy() >= 0)
	{
		struct Cand { bool OnTeam; int Cost; FootClass* Unit; };
		std::vector<Cand> cands;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!IsPicketCandidate(pT, pHouse, pTeam)) continue;
			auto const pF = static_cast<FootClass*>(pT);
			if (pF->Team == pTeam || resp.count(pF->UniqueID)) continue;
			cands.push_back({ pF->Team != nullptr, pT->GetTechnoType()->GetActualCost(pHouse), pF });
		}
		std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b)
			{ return a.OnTeam != b.OnTeam ? !a.OnTeam : a.Cost < b.Cost; });
		for (int k; (k = vacancy()) >= 0; )
		{
			auto const pc = posts[k]->GetCellCoords();
			auto best = cands.end();
			for (auto it = cands.begin(); it != cands.end(); ++it)
			{
				if (!it->Unit) continue;
				if (best == cands.end()) { best = it; continue; }
				if (it->OnTeam != best->OnTeam || it->Cost != best->Cost) continue;   // sorted: first tier only
				if (Dist2D(it->Unit->GetCoords(), pc) < Dist2D(best->Unit->GetCoords(), pc)) best = it;
			}
			if (best == cands.end()) break;
			auto const pF = best->Unit;
			best->Unit = nullptr;
			if (!TryAddToTeam(pTeam, pF)) continue;
			++filled[k]; kept[pF->UniqueID] = k; ++recruited;
		}
	}
	assign = kept;

	// Short of pickets: build them (Picket.Build), as the interceptor squad does.
	int const have = static_cast<int>(kept.size());
	auto const pBuild = cfg.CounterEngPicketBuild && have < want ? PicketBuildType(pHouse) : nullptr;
	EnsureSquadTeam(pHouse, want, pBuild, "DPKT", pBuild ? want - have : 0);
	if (pBuild && cfg.CounterEngPicketBuildUrgent) BuildUrgently(pHouse, pBuild);   // else it rarely wins
	                                                                               // the barracks

	if (cfg.DebugTicks)
	{
		char where[160] = { 0 };
		size_t n = 0;
		for (size_t k = 0; k < posts.size() && n < sizeof(where) - 16; ++k)
		{
			auto const pc = posts[k]->MapCoords;
			n += std::snprintf(where + n, sizeof(where) - n, "%s(%d,%d)x%d", k ? " " : "", pc.X, pc.Y, filled[k]);
		}
		Debug::Log("[DoctrineExt] pickets: house=%s#%d posts=%d (learned %d, toward enemy %d) sightings=%d "
			"edge=%.1fc units=%d/%d recruited=%d build=%s: %s.\n", pHouse->get_ID(), hIdx,
			static_cast<int>(posts.size()), learned, static_cast<int>(bearings.size()) - learned,
			static_cast<int>(sights.size()), radius / 256.0, have, want, recruited,
			pBuild ? pBuild->get_ID() : "(none)", where);
	}
}

void Teams::SteerPickets(HouseClass* pHouse)
{
	int const hIdx = pHouse->ArrayIndex;
	auto const pit = g_picketPosts.find(hIdx);
	auto const ait = g_picketAssign.find(hIdx);
	if (pit == g_picketPosts.end() || ait == g_picketAssign.end()) return;
	auto const pTeam = FindPicketTeam(hIdx);
	if (!pTeam) return;
	auto const& resp = g_responders[hIdx];
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (pF->InLimbo || pF->Health <= 0 || resp.count(pF->UniqueID) || pF->Target) continue;   // busy
		auto const a = ait->second.find(pF->UniqueID);
		if (a == ait->second.end() || a->second >= static_cast<int>(pit->second.size())) continue;
		auto const pCell = pit->second[a->second];
		// Back to its post only once it has drifted off it; there, it stands guard and shoots.
		if (Dist2D(pF->GetCoords(), pCell->GetCellCoords()) > 2.0 * 256.0)
		{
			pF->SetDestination(pCell, true);
			pF->QueueMission(Mission::Move, false);
		}
	}
}

// ─── Walls ([Doctrine.Walls]) ────────────────────────────────────────────────
namespace
{
	std::map<int, int> g_wallLastFire;
	std::map<int, std::set<int>> g_wallMine;          // house -> ring cells (CaptureCellKey) whose wall node we queued
	std::map<int, std::map<int, int>> g_wallRetry;    // house -> ring cell -> frame it may be queued again
	std::map<int, bool> g_wallTypeOk;                 // house -> it could build its wall type at the last pass

	// The wall type a house builds (Types): the list's first it can build now; auto = [AI] ConcreteWalls,
	// our side's (AIBasePlanningSide) first, as the engine's own wall nodes pick it.
	BuildingTypeClass* WallTypeFor(HouseClass* const pHouse)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (!cfg.WallsTypes.empty())
		{
			for (auto const& id : cfg.WallsTypes)
				if (auto const pBt = BuildingTypeClass::Find(id.c_str()))
					if (pBt->Wall && CanBuildStrict(pHouse, pBt)) return pBt;
			return nullptr;
		}
		BuildingTypeClass* other = nullptr;
		int const side = pHouse->Type ? pHouse->Type->SideIndex : -1;
		for (auto const pBt : RulesClass::Instance->ConcreteWalls)
		{
			if (!pBt || !CanBuildStrict(pHouse, pBt)) continue;
			if (pBt->AIBasePlanningSide == side || pBt->AIBasePlanningSide == -1) return pBt;
			if (!other) other = pBt;
		}
		return other;
	}

	// Is this building walled in (Protect)? Auto: a construction yard.
	bool WallProtects(BuildingClass* const pB)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (!cfg.WallsProtect.empty()) return ListHas(cfg.WallsProtect, pB->Type->ID);
		return pB->Type->ConstructionYard;
	}
}

void Teams::WallDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.WallsEnable || cfg.WallsProtectNone || cfg.WallsTypesNone) return;
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral() || pHouse->IsControlledByHuman()) return;
	int const hIdx = pHouse->ArrayIndex;
	int const now = Unsorted::CurrentFrame;
	int const interval = cfg.WallsInterval > 0 ? cfg.WallsInterval : 150;
	auto const fit = g_wallLastFire.find(hIdx);
	if (fit != g_wallLastFire.end() && now - fit->second < interval) return;
	g_wallLastFire[hIdx] = now;

	auto const pWall = WallTypeFor(pHouse);
	int const wallIdx = pWall ? BuildingTypeClass::Array.FindItemIndex(pWall) : -1;
	int const cash = static_cast<int>(pHouse->Available_Money());
	bool income = false;
	for (auto const pB : pHouse->Buildings)
		if (pB && pB->Type && !pB->InLimbo && pB->Health > 0 && IncomeKindOf(pB->Type) > 0) income = true;
	// NeedIncome: with no income building standing, walls go right behind the plan's next unbuilt one (the
	// refinery first); with none in the plan either, they wait.
	auto& nodes = pHouse->Base.BaseNodes;
	auto const nodeType = [](BaseNodeClass const& n)
	{
		int const t = n.BuildingTypeIndex;
		return t >= 0 && t < BuildingTypeClass::Array.Count ? BuildingTypeClass::Array.GetItem(t) : nullptr;
	};
	auto const nextIncome = [&]()
	{
		for (int i = 0; i < nodes.Count; ++i)
			if (!nodes.Items[i].Placed && IncomeKindOf(nodeType(nodes.Items[i])) > 0) return i;
		return -1;
	};
	bool const behindIncome = cfg.WallsNeedIncome && !income;
	const char* wait = nullptr;
	if (wallIdx < 0) wait = "no wall type we can build yet (Types)";
	else if (behindIncome && nextIncome() < 0) wait = "no income building yet, none in the plan (NeedIncome)";
	else if (cfg.WallsOnSighting && g_picketSightings[hIdx].empty()) wait = "no enemy engineer seen yet (OnSighting)";
	else if (cash < cfg.WallsMinCash) wait = "cash below MinCash";

	// The ring Ring cells out from each protected building's foundation. A cell already walled or built on
	// is done (a building stops an engineer as well as a wall), one a wall can't stand on now is skipped
	// (retried next pass), one with someone else's node pending is left alone; the rest are queued, ours
	// still pending again with them (so they keep their place as the plan grows). A cell whose node of ours
	// went from the plan unbuilt (the engine couldn't place the wall there) waits Retry frames, so a wall
	// that won't go in can't hold the plan's front forever.
	auto& mine = g_wallMine[hIdx];
	auto& retry = g_wallRetry[hIdx];
	bool const typeWasOk = g_wallTypeOk[hIdx];
	g_wallTypeOk[hIdx] = wallIdx >= 0;
	std::set<int> seen;
	int const ring = std::max(1, cfg.WallsRing);
	int protect = 0, cells = 0, walled = 0, builtOn = 0, blocked = 0, pending = 0;
	int vanished = 0, retrying = 0, occupied = 0, attempts = 0;
	std::vector<CellStruct> want;
	std::set<int> ours;                                               // our pending nodes' cells
	for (auto const pB : pHouse->Buildings)
	{
		if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0 || !WallProtects(pB)) continue;
		++protect;
		auto const tl = pB->GetMapCoords();
		int const x0 = tl.X - ring, x1 = tl.X + pB->Type->GetFoundationWidth() - 1 + ring;
		int const y0 = tl.Y - ring, y1 = tl.Y + pB->Type->GetFoundationHeight(false) - 1 + ring;
		for (int x = x0; x <= x1; ++x)
			for (int y = y0; y <= y1; ++y)
			{
				if (x != x0 && x != x1 && y != y0 && y != y1) continue;   // inside the ring
				CellStruct const c{ static_cast<short>(x), static_cast<short>(y) };
				int const key = CaptureCellKey(c);
				++cells;
				seen.insert(key);
				auto const pCell = MapClass::Instance.TryGetCellAt(c);
				if (!pCell) { ++blocked; continue; }
				int const ov = pCell->OverlayTypeIndex;
				if (ov >= 0 && ov < OverlayTypeClass::Array.Count && OverlayTypeClass::Array.GetItem(ov)->Wall)
					{ ++walled; mine.erase(key); continue; }
				if (pCell->GetBuilding()) { ++builtOn; mine.erase(key); continue; }
				int node = -1;
				for (int i = 0; i < nodes.Count && node < 0; ++i)
					if (!nodes.Items[i].Placed && nodes.Items[i].MapCoords == c) node = i;
				if (node >= 0)
				{
					++pending;
					attempts = std::max(attempts, static_cast<int>(nodes.Items[node].Attempts));
					for (auto p = pCell->GetContent(); p; p = p->NextObject)
						if (p->WhatAmI() == AbstractType::Infantry || p->WhatAmI() == AbstractType::Unit)
							{ ++occupied; break; }
					auto const pT = nodeType(nodes.Items[node]);
					if (mine.count(key) && pT && pT->Wall && ours.insert(key).second) want.push_back(c);
					continue;
				}
				if (mine.erase(key) && typeWasOk && pWall)     // dropped unbuilt, not for want of the type
				{
					++vanished;
					retry[key] = now + std::max(0, cfg.WallsRetry);
				}
				auto const rit = retry.find(key);
				if (rit != retry.end() && now < rit->second) { ++retrying; continue; }
				if (!pWall || !pCell->CanThisExistHere(pWall->SpeedType, pWall, pHouse)) { ++blocked; continue; }
				if (std::find(want.begin(), want.end(), c) == want.end()) want.push_back(c);
			}
	}
	for (auto it = mine.begin(); it != mine.end();)                  // rings gone with their building
		it = seen.count(*it) ? std::next(it) : mine.erase(it);

	// Queue them, the side facing the nearest enemy base first; with Urgent, ahead of the plan's next
	// unbuilt node (where the engine puts its own wall nodes), else after the plan; NeedIncome keeps them
	// behind the next unbuilt income building. Our pending nodes come out first and go back in with them.
	int queuedNow = 0, at = -1, afterIncome = -1;
	if (!wait && !want.empty())
	{
		int kept = 0;
		for (int i = 0; i < nodes.Count; ++i)
		{
			auto const& n = nodes.Items[i];
			auto const pT = nodeType(n);
			if (!n.Placed && pT && pT->Wall && ours.count(CaptureCellKey(n.MapCoords))) continue;
			nodes.Items[kept++] = n;
		}
		nodes.Count = kept;
		auto const home = pHouse->GetBaseCenter();
		CellStruct foe{ 0, 0 };
		double foeD = 1e18;
		for (int i = 0; i < HouseClass::Array.Count; ++i)
		{
			auto const pH = HouseClass::Array.GetItem(i);
			if (!pH || pH == pHouse || pH->Defeated || pH->IsNeutral() || pHouse->IsAlliedWith(pH)) continue;
			auto const c = pH->GetBaseCenter();
			if (c.X == 0 && c.Y == 0) continue;
			double const d = std::hypot(double(c.X - home.X), double(c.Y - home.Y));
			if (d < foeD) { foeD = d; foe = c; }
		}
		if (foeD < 1e18)
			std::stable_sort(want.begin(), want.end(), [&](CellStruct const& a, CellStruct const& b)
				{ return std::hypot(double(a.X - foe.X), double(a.Y - foe.Y))
					< std::hypot(double(b.X - foe.X), double(b.Y - foe.Y)); });
		int first = nodes.Count;
		if (cfg.WallsUrgent)
			for (int i = 0; i < nodes.Count; ++i)
				if (!nodes.Items[i].Placed) { first = i; break; }
		if (behindIncome && (afterIncome = nextIncome()) >= 0) first = std::max(first, ++afterIncome);
		int const before = nodes.Count;
		for (auto const& c : want)
		{
			BaseNodeClass node;
			node.BuildingTypeIndex = wallIdx;
			node.MapCoords = c;
			node.Placed = false;
			node.Attempts = 0;
			if (nodes.AddItem(node) && mine.insert(CaptureCellKey(c)).second) ++queuedNow;
		}
		if (first < before)
			std::rotate(nodes.Items + first, nodes.Items + before, nodes.Items + nodes.Count);
		at = std::min(first, before);
	}

	if (cfg.DebugTicks && protect > 0)
		Debug::Log("[DoctrineExt] walls: house=%s#%d type=%s protect=%d ring=%d cells=%d walled=%d built-on=%d "
			"blocked=%d pending=%d (occupied %d, attempts %d) dropped=%d retrying=%d queued=%d at=%d/%d%s cash=%d%s%s.\n",
			pHouse->get_ID(), hIdx, pWall ? pWall->ID : "-", protect, ring, cells, walled, builtOn, blocked,
			pending, occupied, attempts, vanished, retrying, queuedNow, at, nodes.Count,
			at < 0 ? "" : afterIncome >= 0 ? " (behind the income building)"
				: cfg.WallsUrgent ? " (front of the plan)" : " (after the plan)", cash, wait ? "; waiting: " : "",
			wait ? wait : "");
}

// ─── Engineer rush (§10m) ────────────────────────────────────────────────────
namespace
{
	std::map<int, int> g_engRushLastFire;                         // house -> last pass frame
	std::map<int, std::map<DWORD, DWORD>> g_engRushAssign;        // house -> engineer UID -> target UID
	// Rush engineer UID -> {frame first seen within 2 cells of its target, frame last logged} (DebugTicks:
	// an engineer that stands at the door instead of going in, Rex 11-08).
	std::map<DWORD, std::pair<int, int>> g_rushAtDoor;
	std::map<int, std::map<DWORD, int>> g_engRushCooldown;        // house -> target UID -> retry frame
	std::map<int, std::set<DWORD>> g_engRushMine;                 // house -> its building UIDs last pass
	std::map<int, std::set<DWORD>> g_engRushLost;                 // house -> its buildings the enemy captured
	std::map<int, int> g_recaptureSeen;                           // house -> last pass with a recapture target

	TeamClass* FindRushTeam(int const hIdx)
	{
		char id[0x18];
		std::snprintf(id, sizeof(id), "DRSH%dTM", hIdx);
		auto const pTT = TeamTypeClass::Find(id);
		return (pTT && pTT->cntInstances > 0) ? pTT->FindFirstInstance() : nullptr;
	}

	bool IsHostileTo(HouseClass* const pHouse, HouseClass* const pO)
	{
		return pO && pO != pHouse && !pHouse->IsAlliedWith(pO) && !pO->IsNeutral();
	}

	bool WantsRush(BuildingTypeClass* const pBt)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (!pBt || !pBt->Capturable || ListHas(cfg.EngRushExclude, pBt->ID)) return false;
		return cfg.EngRushTargets.empty() || ListHas(cfg.EngRushTargets, pBt->ID);
	}

	int RushRank(BuildingTypeClass* const pBt)
	{
		auto const& list = DoctrineConfig::Get().EngRushPriority;
		for (size_t i = 0; i < list.size(); ++i)
			if (list[i] == pBt->ID) return static_cast<int>(i);
		return INT_MAX;
	}

	// Would taking pB cripple its owner (Cripple)? Its last construction yard (and no MCV), its last factory
	// of a kind, its last income building, or a power plant it can't run its base without.
	bool Cripples(BuildingClass* const pB)
	{
		auto const pO = pB->Owner;
		auto const pBt = pB->Type;
		if (!pO) return false;
		bool const yard = pBt->ConstructionYard;
		bool const factory = !yard && pBt->Factory != AbstractType::None;
		bool const income = IncomeKindOf(pBt) > 0;
		bool const power = pBt->PowerBonus > 0 && pO->PowerOutput - pBt->PowerBonus < pO->PowerDrain;
		if (power) return true;
		if (!yard && !factory && !income) return false;
		bool spareYard = false, spareFactory = false, spareIncome = false;
		for (auto const pX : pO->Buildings)
		{
			if (!pX || pX == pB || !pX->Type || pX->InLimbo || pX->Health <= 0) continue;
			if (pX->GetCurrentMission() == Mission::Selling) continue;
			if (pX->Type->ConstructionYard) spareYard = true;
			if (pX->Type->Factory == pBt->Factory && pX->Type->Naval == pBt->Naval) spareFactory = true;
			if (IncomeKindOf(pX->Type) > 0) spareIncome = true;
		}
		if (yard && !spareYard)
			for (auto const pU : UnitClass::Array)
				if (pU && pU->Owner == pO && !pU->InLimbo && pU->Health > 0 && pU->Type->DeploysInto
					&& pU->Type->DeploysInto->ConstructionYard)
					{ spareYard = true; break; }
		return (yard && !spareYard) || (factory && !spareFactory) || (income && !spareIncome);
	}

	// A hostile armed object, as the rush's risk model sees it.
	struct Shooter { TechnoClass* T; TechnoTypeClass* Type; bool Building; double Range; double Speed; };

	std::vector<Shooter> HostileShooters(HouseClass* const pHouse)
	{
		std::vector<Shooter> out;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pX = TechnoClass::Array.GetItem(i);
			if (!pX || pX->InLimbo || pX->Health <= 0 || !IsHostileTo(pHouse, pX->Owner)) continue;
			auto const what = pX->WhatAmI();
			bool const bld = what == AbstractType::Building;
			if (!bld && what != AbstractType::Unit && what != AbstractType::Infantry) continue;
			auto const pXT = pX->GetTechnoType();
			if (!pXT || !HasOffensiveWeapon(pXT)) continue;
			if (bld && !DefenseOnline(static_cast<BuildingClass*>(pX))) continue;
			out.push_back({ pX, pXT, bld, static_cast<double>(BestRangeVs(pXT, false)),
				bld ? 0.0 : StepSpeed(static_cast<FootClass*>(pX)) });
		}
		return out;
	}

	// Expected damage the enemy deals an engineer of type pEng walking into pB — the counter-engineer's
	// model from the attacker's side. A defense whose range covers the door fires while it crosses that
	// range (about one range-length of walking); an enemy unit within a few cells closes in and fires
	// over the last stretch. A heuristic: it separates an open door from a covered one.
	double RushRisk(std::vector<Shooter> const& shooters, BuildingClass* const pB, TechnoTypeClass* const pEng)
	{
		int const armor = static_cast<int>(pEng->Armor);
		double const v = pEng->Speed > 1 ? static_cast<double>(pEng->Speed) : 1.0;   // leptons/frame
		double const stretch = 4.0 * 256.0;
		double dmg = 0.0;
		for (auto const& s : shooters)
		{
			double const dps = EffDPSVs(s.Type, armor, false) / 10.0;
			if (dps <= 0.0) continue;
			double const d = DistToFootprint(s.T->GetCoords(), pB);
			if (s.Building)
			{
				if (d <= s.Range) dmg += dps * s.Range / v;
			}
			else if (d <= s.Range + stretch)
			{
				double const window = (s.Range + stretch) / v - std::max(0.0, d - s.Range) / s.Speed;
				if (window > 0.0) dmg += dps * window;
			}
		}
		return dmg;
	}
}

void Teams::EngineerRush(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.EngRushEnable) return;                                       // opt-in
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;                         // AI houses only

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.EngRushInterval > 0 ? cfg.EngRushInterval : 60;
	auto const lit = g_engRushLastFire.find(hIdx);
	if (lit != g_engRushLastFire.end() && now - lit->second < interval) return;
	g_engRushLastFire[hIdx] = now;
	ClearStaleUrgentPick(pHouse, now);

	auto& assign = g_engRushAssign[hIdx];
	auto& cool = g_engRushCooldown[hIdx];
	for (auto it = cool.begin(); it != cool.end(); )
		if (it->second <= now) it = cool.erase(it); else ++it;

	auto pTeam = FindRushTeam(hIdx);
	auto const pEng = BestEngineer(pHouse, cfg.HonorPrereqForEngRush());
	TechnoTypeClass* pEngType = pEng;                                     // risk is priced for this engineer
	std::map<DWORD, FootClass*> onTeam;                                   // squad engineers by UID
	for (auto pF = pTeam ? pTeam->FirstUnit : nullptr; pF; pF = pF->NextTeamMember)
		if (IsLiveEngineer(pF))
		{
			onTeam[pF->UniqueID] = pF;
			if (!pEngType) pEngType = pF->GetTechnoType();
		}

	// The capture squad's engineers, which a recapture may take (Recapture.UseCaptureSquad) — so a house
	// that can't build one any more (no barracks) still prices the risk for the one it has.
	auto const pCapTeam = cfg.EngRushRecapture && cfg.EngRushRecaptureUseCapture ? FindCaptureTeam(hIdx) : nullptr;
	for (auto pF = pCapTeam ? pCapTeam->FirstUnit : nullptr; pF && !pEngType; pF = pF->NextTeamMember)
		if (IsLiveEngineer(pF)) pEngType = pF->GetTechnoType();

	// Hostile buildings by UID, and the rushable ones near base whose door the enemy can't cover. One the
	// enemy captured from us (ours last pass, hostile now — a capture keeps the object and its UniqueID)
	// comes first when Recapture is on, whatever Targets and Priority say; Exclude still applies.
	struct Cand { BuildingClass* B; int Rank; int Value; double Dist; double Risk; };
	std::vector<Cand> cands;
	std::map<DWORD, BuildingClass*> hostile;
	auto& mine = g_engRushMine[hIdx];
	auto& lost = g_engRushLost[hIdx];
	auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
	double const maxD = (cfg.EngRushRadius > 0 ? cfg.EngRushRadius : 30) * 256.0;
	// Priority and Cripple targets may be farther (Priority.Radius; 0 = anywhere) — a yard across the map
	// beats a power plant next door (11-08: Rex's yard was out of Radius, his forward plant wasn't).
	double const maxPriD = std::max(maxD, cfg.EngRushPriorityRadius < 0 ? maxD
		: cfg.EngRushPriorityRadius == 0 ? 1e18 : cfg.EngRushPriorityRadius * 256.0);
	int const nPri = static_cast<int>(cfg.EngRushPriority.size());
	double const safety = cfg.EngRushSafety > 0.0 ? cfg.EngRushSafety : 1.0;
	auto const shooters = pEngType ? HostileShooters(pHouse) : std::vector<Shooter>();
	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		auto const pB = BuildingClass::Array.GetItem(i);
		if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0) continue;
		if (!IsHostileTo(pHouse, pB->Owner)) continue;
		DWORD const uid = pB->UniqueID;
		bool const ours = cfg.EngRushRecapture && (lost.count(uid) || mine.count(uid))
			&& pB->Type->Capturable && !ListHas(cfg.EngRushExclude, pB->Type->ID);
		if (ours) lost.insert(uid);
		else if (!WantsRush(pB->Type)) continue;
		hostile[uid] = pB;
		if (!pEngType) continue;
		double const d = Dist2D(base, pB->GetCoords());
		if (d > maxPriD || cool.count(uid)) continue;
		int rank = ours ? -1 : RushRank(pB->Type);
		if (rank == INT_MAX && cfg.EngRushCripple && Cripples(pB)) rank = nPri;   // right after Priority
		if (rank == INT_MAX && d > maxD) continue;
		double const risk = RushRisk(shooters, pB, pEngType);
		if (risk * safety >= pEngType->Strength) continue;               // its door is covered
		cands.push_back({ pB, rank, pB->Type->Cost, d, risk });
	}
	mine.clear();
	for (auto const pB : pHouse->Buildings)
		if (pB && !pB->InLimbo && pB->Health > 0) mine.insert(pB->UniqueID);
	for (auto it = lost.begin(); it != lost.end(); )                      // retaken, destroyed or excluded
		if (!hostile.count(*it)) it = lost.erase(it); else ++it;
	std::sort(cands.begin(), cands.end(), [](Cand const& a, Cand const& b)
	{
		if (a.Rank != b.Rank) return a.Rank < b.Rank;
		if (a.Value != b.Value) return a.Value > b.Value;
		return a.Dist < b.Dist;
	});

	// An engineer that left the squad while its target is still hostile didn't make it (one that
	// captured is consumed, and the building is no longer hostile): rest that target before retrying.
	for (auto it = assign.begin(); it != assign.end(); )
	{
		if (onTeam.count(it->first)) { ++it; continue; }
		if (hostile.count(it->second))
			cool[it->second] = now + (cfg.EngRushRetryDelay > 0 ? cfg.EngRushRetryDelay : 900);
		it = assign.erase(it);
	}

	// Engineers each target takes: one, plus Priority.Backup for a Priority target.
	auto const slotsFor = [&](Cand const& c)
		{ return c.Rank >= 0 && c.Rank < nPri ? 1 + std::max(0, cfg.EngRushPriorityBackup) : 1; };
	int slots = 0;
	for (auto const& c : cands) slots += slotsFor(c);
	int const cap = cfg.EngRushMax > 0 ? cfg.EngRushMax : 2;
	int const desired = std::min(slots, cap);
	int const have = static_cast<int>(onTeam.size());
	if (desired == 0)
	{
		// Nothing safe to rush: stand the squad down (escorts and any engineer back to the pool).
		if (pTeam)
			for (auto pF = pTeam->FirstUnit; pF; )
			{
				auto const next = pF->NextTeamMember;
				pTeam->LiberateMember(pF);
				pF = next;
			}
		assign.clear();
		ClearPauseExempt(hIdx, ExemptEngRush);
		return;
	}

	bool const flush = pHouse->Available_Money() >= cfg.EngRushMinCash;
	int want = flush ? desired - have : 0;
	if (want < 0) want = 0;
	pTeam = EnsureSquadTeam(pHouse, desired, (pEng && want > 0) ? pEng : nullptr, "DRSH", want);
	if (!pTeam) return;
	pTeam->Type->Priority = DoctrineSquadPriority;

	// Spare engineers first (a capture-squad engineer ties on priority, so it's never poached).
	for (int i = 0; i < TechnoClass::Array.Count && static_cast<int>(onTeam.size()) < desired; ++i)
	{
		auto const pT = TechnoClass::Array.GetItem(i);
		if (!IsLiveEngineer(pT) || pT->Owner != pHouse) continue;
		auto const pF = static_cast<FootClass*>(pT);
		if (TryAddToTeam(pTeam, pF)) onTeam[pF->UniqueID] = pF;
	}
	// Still short for a recapture: take the capture squad's engineer nearest the building we lost
	// (Recapture.UseCaptureSquad) — getting our own building back beats a derrick, and with no refinery
	// or yard the engine cancels every new build. Our squads tie on priority, so the capture squad's is
	// lowered for the one AddMember call; one per pass.
	int recaptures = 0;
	for (auto const& c : cands) if (c.Rank < 0) ++recaptures;
	if (recaptures > 0) g_recaptureSeen[hIdx] = now;                     // Salvage.BlockWhilePlan
	if (pCapTeam && static_cast<int>(onTeam.size()) < std::min(recaptures, desired))
	{
		auto const tc = cands[0].B->GetCoords();
		FootClass* pTake = nullptr;
		double bestD = 1e18;
		for (auto pF = pCapTeam->FirstUnit; pF; pF = pF->NextTeamMember)
			if (IsLiveEngineer(pF) && Dist2D(tc, pF->GetCoords()) < bestD)
				{ bestD = Dist2D(tc, pF->GetCoords()); pTake = pF; }
		if (pTake)
		{
			int const prio = pCapTeam->Type->Priority;
			pCapTeam->Type->Priority = DoctrineSquadPriority - 1;
			bool const moved = TryAddToTeam(pTeam, pTake);
			pCapTeam->Type->Priority = prio;
			if (moved) onTeam[pTake->UniqueID] = pTake;
			if (cfg.DebugTicks)
				Debug::Log("[DoctrineExt] eng-rush recapture: house=%s#%d %s capture-squad %s#%u (%.1f cells "
					"from %s).\n", pHouse->get_ID(), hIdx, moved ? "took" : "could not take",
					pTake->GetTechnoType()->ID, pTake->UniqueID, bestD / 256.0, cands[0].B->Type->ID);
		}
	}
	// Still short after taking the spares: make the engineer the barracks' next build (Build.Urgent),
	// unless one is already in production, and let it (in production too) past the economy pause
	// (Build.IgnorePause).
	bool const shortOf = flush && pEng && static_cast<int>(onTeam.size()) < desired;
	if (shortOf && cfg.EngRushBuildUrgent
		&& static_cast<int>(onTeam.size()) + InProduction(pHouse, pEng) < desired)
		BuildUrgently(pHouse, pEng);
	SetPauseExempt(hIdx, ExemptEngRush, shortOf && cfg.EngRushBuildIgnorePause ? pEng : nullptr,
		now + 2 * interval);

	// Escorts: armed units nearest the base, up to Escorts per engineer; extras (an engineer died) go home.
	int const escortCap = cfg.EngRushEscorts > 0 ? cfg.EngRushEscorts * static_cast<int>(onTeam.size()) : 0;
	int escorts = SquadCount(pTeam, false) - SquadCount(pTeam, true);
	if (escorts < escortCap)
	{
		auto const& resp = g_responders[hIdx];
		std::vector<std::pair<double, FootClass*>> pool;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!IsResponderCandidate(pT, pHouse)) continue;
			auto const pF = static_cast<FootClass*>(pT);
			if (OnSquad(pF, "DANT") || resp.count(pF->UniqueID)) continue;   // busy defending
			pool.push_back({ Dist2D(base, pF->GetCoords()), pF });
		}
		std::sort(pool.begin(), pool.end(),
			[](std::pair<double, FootClass*> const& a, std::pair<double, FootClass*> const& b)
			{ return a.first < b.first; });
		for (auto const& p : pool)
		{
			if (escorts >= escortCap) break;
			if (TryAddToTeam(pTeam, p.second)) ++escorts;
		}
	}
	for (auto pF = pTeam->FirstUnit; pF && escorts > escortCap; )
	{
		auto const next = pF->NextTeamMember;
		if (!IsLiveEngineer(pF)) { pTeam->LiberateMember(pF); --escorts; }
		pF = next;
	}

	// Assignments, best target first: an engineer within 3 cells of its target keeps it (it's about to go
	// in); then each target, in rank order, keeps the engineers already on it and takes the nearest others
	// up to its slots — so a better target that turns up (the yard, once its door is open) pulls an
	// engineer off a lesser one.
	std::map<DWORD, BuildingClass*> candById;
	for (auto const& c : cands) candById[c.B->UniqueID] = c.B;
	std::map<DWORD, DWORD> next;
	std::map<DWORD, int> filled;
	for (auto const& kv : onTeam)
	{
		auto const a = assign.find(kv.first);
		if (a == assign.end()) continue;
		auto const t = candById.find(a->second);
		if (t != candById.end() && DistToFootprint(kv.second->GetCoords(), t->second) <= 3.0 * 256.0)
			{ next[kv.first] = a->second; ++filled[a->second]; }
	}
	for (auto const& c : cands)
	{
		DWORD const tid = c.B->UniqueID;
		int const room = slotsFor(c);
		for (auto const& kv : onTeam)
		{
			if (filled[tid] >= room) break;
			auto const a = assign.find(kv.first);
			if (!next.count(kv.first) && a != assign.end() && a->second == tid) { next[kv.first] = tid; ++filled[tid]; }
		}
		auto const bc = c.B->GetCoords();
		while (filled[tid] < room)
		{
			FootClass* best = nullptr;
			double bestD = 1e18;
			for (auto const& kv : onTeam)
				if (!next.count(kv.first) && Dist2D(bc, kv.second->GetCoords()) < bestD)
					{ bestD = Dist2D(bc, kv.second->GetCoords()); best = kv.second; }
			if (!best) break;
			next[best->UniqueID] = tid;
			++filled[tid];
		}
	}
	assign = next;

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] eng-rush: house=%s#%d targets=%d desired=%d eng=%d escorts=%d/%d "
			"build=%d%s best=%s@%.1fc%s risk=%.0f/%d.\n", pHouse->get_ID(), hIdx,
			static_cast<int>(cands.size()), desired, static_cast<int>(onTeam.size()), escorts, escortCap,
			want, flush ? "" : " (below MinCash)", cands[0].B->Type->ID, cands[0].Dist / 256.0,
			cands[0].Rank < 0 ? " (recapture)" : cands[0].Rank < nPri ? " (priority)"
				: cands[0].Rank == nPri ? " (cripple)" : "", cands[0].Risk, pEngType->Strength);
}

void Teams::SteerEngineerRush(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.EngRushEnable) return;
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;
	int const hIdx = pHouse->ArrayIndex;
	auto const pTeam = FindRushTeam(hIdx);
	if (!pTeam || !pTeam->FirstUnit) return;
	auto const ait = g_engRushAssign.find(hIdx);
	if (ait == g_engRushAssign.end() || ait->second.empty()) return;
	auto const& assign = ait->second;

	std::set<DWORD> wanted;
	for (auto const& kv : assign) wanted.insert(kv.second);
	std::map<DWORD, BuildingClass*> targets;
	for (int i = 0; i < BuildingClass::Array.Count; ++i)
	{
		auto const pB = BuildingClass::Array.GetItem(i);
		if (pB && wanted.count(pB->UniqueID) && !pB->InLimbo && pB->Health > 0
			&& IsHostileTo(pHouse, pB->Owner))
			targets[pB->UniqueID] = pB;
	}

	// Engineers: capture order onto the assigned building (re-issued only when it drifted).
	std::vector<std::pair<FootClass*, BuildingClass*>> engs;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (!IsLiveEngineer(pF)) continue;
		auto const a = assign.find(pF->UniqueID);
		auto const t = a == assign.end() ? targets.end() : targets.find(a->second);
		if (t == targets.end())                                         // unassigned, or captured/lost:
		{                                                               // next pass reassigns
			if (cfg.DebugTicks)
			{
				int const now = Unsorted::CurrentFrame;
				auto& door = g_rushAtDoor[pF->UniqueID];
				if (!door.first) door = { now, now - 30 };
				if (now - door.second >= 30)
				{
					door.second = now;
					Debug::Log("[DoctrineExt] eng-rush idle: house=%s#%d %s#%u has no live target for %df: "
						"mission=%d target=%s.\n", pHouse->get_ID(), hIdx, pF->GetTechnoType()->ID, pF->UniqueID,
						now - door.first, static_cast<int>(pF->GetCurrentMission()),
						pF->Target ? "set" : "none");
				}
			}
			continue;
		}
		bool const reissue = pF->Target != t->second || pF->GetCurrentMission() != Mission::Capture;
		if (cfg.DebugTicks)
		{
			// At the door: what it's doing each second it stays there, and whether we just re-ordered it.
			int const now = Unsorted::CurrentFrame;
			if (DistToFootprint(pF->GetCoords(), t->second) <= 2.0 * 256.0)
			{
				auto& door = g_rushAtDoor[pF->UniqueID];
				if (!door.first) door = { now, now - 30 };
				if (now - door.second >= 30)
				{
					door.second = now;
					Debug::Log("[DoctrineExt] eng-rush door: house=%s#%d %s#%u at %s of %s#%d for %df: mission=%d "
						"queued=%d target=%s destination=%s%s; building mission=%d health=%d/%d.\n",
						pHouse->get_ID(), hIdx, pF->GetTechnoType()->ID, pF->UniqueID, t->second->Type->ID,
						t->second->Owner->get_ID(), t->second->Owner->ArrayIndex, now - door.first,
						static_cast<int>(pF->GetCurrentMission()), static_cast<int>(pF->QueuedMission),
						pF->Target == t->second ? "ours" : pF->Target ? "other" : "none",
						pF->Destination == t->second ? "ours" : pF->Destination ? "other" : "none",
						reissue ? ", re-ordered" : "", static_cast<int>(t->second->GetCurrentMission()),
						t->second->Health, t->second->Type->Strength);
				}
			}
			else
				g_rushAtDoor.erase(pF->UniqueID);
		}
		if (reissue)
		{
			pF->SetTarget(t->second);
			pF->QueueMission(Mission::Capture, false);
		}
		engs.push_back({ pF, t->second });
	}
	if (engs.empty()) return;

	// Per engineer, the most dangerous hostile it's walking into: the biggest damage-dealer to an
	// engineer within reach of it now, or covering its door.
	std::vector<TechnoClass*> danger(engs.size(), nullptr);
	for (size_t k = 0; k < engs.size(); ++k)
	{
		auto const pE = engs[k].first;
		int const armor = static_cast<int>(pE->GetTechnoType()->Armor);
		auto const ec = pE->GetCoords();
		double bestDps = 0.0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pX = TechnoClass::Array.GetItem(i);
			if (!pX || pX->InLimbo || pX->Health <= 0 || !IsHostileTo(pHouse, pX->Owner)) continue;
			auto const what = pX->WhatAmI();
			if (what != AbstractType::Building && what != AbstractType::Unit
				&& what != AbstractType::Infantry) continue;
			auto const pXT = pX->GetTechnoType();
			if (!pXT) continue;
			double const dps = EffDPSVs(pXT, armor, false);
			if (dps <= bestDps) continue;
			double const range = BestRangeVs(pXT, false);
			auto const xc = pX->GetCoords();
			if (Dist2D(xc, ec) <= range + 2.0 * 256.0 || DistToFootprint(xc, engs[k].second) <= range)
				{ bestDps = dps; danger[k] = pX; }
		}
	}

	// Escorts, shared round-robin: hit the engineer's danger, else shadow the engineer.
	size_t k = 0;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
	{
		if (pF->InLimbo || pF->Health <= 0 || IsLiveEngineer(pF)) continue;
		size_t const e = k++ % engs.size();
		auto const pX = danger[e];
		if (pX && EffDPSVs(pF->GetTechnoType(), static_cast<int>(pX->GetTechnoType()->Armor), false) > 0.0)
		{
			if (pF->Target != pX || pF->GetCurrentMission() != Mission::Attack)
			{
				pF->SetTarget(pX);
				pF->QueueMission(Mission::Attack, false);
			}
		}
		else if (Dist2D(pF->GetCoords(), engs[e].first->GetCoords()) > 3.0 * 256.0
			&& (pF->Destination != engs[e].first || pF->GetCurrentMission() != Mission::Move))
		{
			pF->SetDestination(engs[e].first, true);
			pF->QueueMission(Mission::Move, false);
		}
	}
}

// ─── Economy opening (§10k) ──────────────────────────────────────────────────
namespace
{
	std::map<int, int> g_economyLastFire;                        // house -> last pass frame
	std::map<int, bool> g_economyLocked;                         // house -> any production paused

	enum class EcoKind { Refinery, Barracks, Power };

	bool InIdList(const char* const id, std::vector<std::string> const& list)
	{
		for (auto const& s : list) if (s == id) return true;
		return false;
	}

	// Does the house own a working MINER — any owned ResourceGatherer unit (harvester, or a
	// slave that mines), or a type on the modder's MinerUnits list? Used so a refinery with no
	// miner (dead harvester / slave-miner moved off) isn't mistaken for income (Rex's SMIN point).
	bool OwnsMiner(HouseClass* const pHouse)
	{
		auto const& list = DoctrineConfig::Get().EconomyMinerUnits;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->Owner != pHouse || pT->InLimbo || pT->Health <= 0) continue;
			auto const pType = pT->GetTechnoType();
			if (!pType) continue;
			if (pType->ResourceGatherer) return true;
			if (!list.empty() && InIdList(pType->ID, list)) return true;
		}
		return false;
	}

	// Classify an owned building as an income source: 2 = passive cash (a derrick:
	// ProduceCashAmount>0, income on its own), 1 = refinery (needs a miner to earn), 0 = not.
	// Honors the modder IncomeBuildings list (listed = income, passive if it also makes cash);
	// empty list = auto by flag.
	int IncomeKindOf(BuildingTypeClass* const pBt)
	{
		if (!pBt) return 0;
		auto const& list = DoctrineConfig::Get().EconomyIncomeBuildings;
		bool const passive = pBt->ProduceCashAmount > 0;
		if (!list.empty())
			return InIdList(pBt->ID, list) ? (passive ? 2 : 1) : 0;
		if (passive) return 2;
		return pBt->Refinery ? 1 : 0;
	}

	// Real income? A passive cash building (derrick) counts alone; a refinery counts only when
	// the house also owns a miner (unless IncomeRequiresMiner=no).
	bool HasIncome(HouseClass* const pHouse)
	{
		bool ownsRefinery = false;
		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Type || pBld->InLimbo || pBld->Health <= 0) continue;
			int const k = IncomeKindOf(pBld->Type);
			if (k == 2) return true;                          // passive cash — income by itself
			if (k == 1) ownsRefinery = true;
		}
		if (!ownsRefinery) return false;
		if (!DoctrineConfig::Get().EconomyIncomeRequiresMiner) return true;
		return OwnsMiner(pHouse);                             // refinery needs a miner to earn
	}

	// Owns any income STRUCTURE (refinery or cash building), miner or not? Used so we don't
	// queue a second refinery when it already has one but no miner.
	bool OwnsIncomeStructure(HouseClass* const pHouse)
	{
		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Type || pBld->InLimbo || pBld->Health <= 0) continue;
			if (IncomeKindOf(pBld->Type) > 0) return true;
		}
		return false;
	}

	// Is a capturable neutral/enemy building whose ID is in `ids` within `radiusCells` of base?
	// Empty `ids` = any Capturable building with ProduceCashAmount>0 (auto oil-derrick).
	bool NeutralCapturableNear(HouseClass* const pHouse, std::vector<std::string> const& ids,
		int const radiusCells)
	{
		auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
		double const maxD = static_cast<double>(radiusCells > 0 ? radiusCells : 20) * 256.0;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || !pB->Type || pB->InLimbo || pB->Health <= 0) continue;
			if (!pB->Type->Capturable) continue;
			bool const match = ids.empty()
				? (pB->Type->ProduceCashAmount > 0) : InIdList(pB->Type->ID, ids);
			if (!match) continue;
			auto const pO = pB->Owner;
			if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;
			auto const c = pB->GetCoords();
			double const d = std::sqrt(static_cast<double>(c.X - base.X) * (c.X - base.X)
				+ static_cast<double>(c.Y - base.Y) * (c.Y - base.Y));
			if (d <= maxD) return true;
		}
		return false;
	}

	// Cheapest buildable, faction-correct building of a kind. Refinery/Barracks honor the
	// modder lists; empty = auto by flag (Refinery / Factory==InfantryType / PowerBonus>0).
	BuildingTypeClass* BestEconomyBuilding(HouseClass* const pHouse, EcoKind const kind,
		bool const honorPrereq)
	{
		auto const& cfg = DoctrineConfig::Get();
		std::vector<std::string> const* pList =
			kind == EcoKind::Refinery ? &cfg.EconomyRefineryBuild
			: kind == EcoKind::Barracks ? &cfg.EconomyBarracksBuild : nullptr;
		bool const useList = pList && !pList->empty();
		BuildingTypeClass* best = nullptr; int bestCost = 1 << 30;
		for (auto const pBt : BuildingTypeClass::Array)
		{
			if (!pBt) continue;
			bool match;
			if (useList)
				match = InIdList(pBt->ID, *pList);
			else switch (kind)
			{
			case EcoKind::Refinery: match = pBt->Refinery; break;
			case EcoKind::Barracks: match = (pBt->Factory == AbstractType::InfantryType); break;
			default:                match = (pBt->PowerBonus > 0); break;
			}
			if (!match) continue;
			if (!pHouse->InOwners(pBt)) continue;
			if (!CanBuildStrict(pHouse, pBt, honorPrereq)) continue;
			if (pBt->Cost < bestCost) { bestCost = pBt->Cost; best = pBt; }
		}
		return best;
	}

	// Owns a barracks (configurable; empty = Factory==InfantryType)?
	bool OwnsBarracks(HouseClass* const pHouse)
	{
		auto const& list = DoctrineConfig::Get().EconomyBarracksBuild;
		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Type || pBld->InLimbo || pBld->Health <= 0) continue;
			if (list.empty() ? (pBld->Type->Factory == AbstractType::InfantryType)
				: InIdList(pBld->Type->ID, list))
				return true;
		}
		return false;
	}

	// Owns a power plant (any building that produces power)? The refinery's prerequisite and what
	// runs it, so the opening builds one first if missing.
	bool OwnsPowerPlant(HouseClass* const pHouse)
	{
		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->Type || pBld->InLimbo || pBld->Health <= 0) continue;
			if (pBld->Type->PowerBonus > 0) return true;
		}
		return false;
	}

	// Emergency: an enemy COMBAT unit within `radiusCells` of base — lifts every pause so a
	// cornered opening AI builds defenders instead of sitting paused under attack.
	bool EnemyCombatNear(HouseClass* const pHouse, int const radiusCells)
	{
		if (radiusCells <= 0) return false;
		auto const base = CellClass::Cell2Coord(pHouse->GetBaseCenter());
		double const maxD = static_cast<double>(radiusCells) * 256.0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || pT->InLimbo || pT->Health <= 0) continue;
			auto const pO = pT->Owner;
			if (!pO || pO == pHouse || pHouse->IsAlliedWith(pO)) continue;           // enemy only
			auto const what = pT->WhatAmI();
			if (what != AbstractType::Unit && what != AbstractType::Infantry
				&& what != AbstractType::Aircraft) continue;
			auto const pType = pT->GetTechnoType();
			if (!pType || !HasOffensiveWeapon(pType)) continue;
			auto const c = pT->GetCoords();
			double const d = std::sqrt(static_cast<double>(c.X - base.X) * (c.X - base.X)
				+ static_cast<double>(c.Y - base.Y) * (c.Y - base.Y));
			if (d <= maxD) return true;
		}
		return false;
	}

	// The pause category for a factory's current Object, or null for "never pause". Income
	// infrastructure (refineries, cash buildings, miners) is always exempt so the AI can
	// always build its economy regardless of pause config.
	DoctrineConfig::EconomyPauseCat const* EcoCatFor(TechnoClass* const pObj)
	{
		auto const& cfg = DoctrineConfig::Get();
		auto const pType = pObj->GetTechnoType();
		if (!pType) return nullptr;
		if (pType->ResourceGatherer) return nullptr;          // miners always build
		switch (pObj->WhatAmI())
		{
		case AbstractType::Infantry: return &cfg.EcoInfantry;
		case AbstractType::Aircraft: return &cfg.EcoAircraft;
		case AbstractType::Unit:     return pType->Naval ? &cfg.EcoNavy : &cfg.EcoVehicle;
		case AbstractType::Building:
		{
			auto const pBt = static_cast<BuildingTypeClass*>(pType);
			if (IncomeKindOf(pBt) > 0) return nullptr;        // refinery / cash building always builds
			if (pBt->HasSuperWeapon()) return &cfg.EcoSuperWeapons;
			if (pBt->IsBaseDefense) return &cfg.EcoDefenses;
			return &cfg.EcoBuildings;
		}
		default: return nullptr;
		}
	}

	// Apply per-category pauses. Threshold semantics: 0 = never pause; -1 (any negative) = ALWAYS
	// halt that category (debug: production never turns on, ignoring income AND emergency); >0 =
	// pause while the house has NO income and cash is at/below it (lifted by an emergency or once
	// income exists). Always exempt: income infrastructure (EcoCatFor null), an Object in the
	// category's Exclude, or in BreakRules while the break condition holds. Returns true if
	// anything was paused this pass. Runs regardless of income so a -1 halt keeps applying.
	bool EconomyApplyPauses(HouseClass* const pHouse, int const cash, bool const hasIncome,
		bool const emergency)
	{
		auto const& cfg = DoctrineConfig::Get();
		bool const breakCond = cfg.EconomyBreakConditionNeutral.empty()
			|| NeutralCapturableNear(pHouse, cfg.EconomyBreakConditionNeutral, cfg.EconomyRadius);
		bool anyPaused = false;
		for (auto const pFact : FactoryClass::Array)
		{
			if (!pFact || pFact->Owner != pHouse) continue;
			auto const pObj = pFact->Object;
			if (!pObj) continue;                              // idle — leave free
			bool pause = false;
			auto const pCat = EcoCatFor(pObj);
			if (pCat)
			{
				int const thr = pCat->Threshold;
				bool want;
				if (thr == 0) want = false;                   // never halt
				else if (thr < 0) want = true;                // -1: always halt (debug); ignores income/emergency
				else want = (!hasIncome && !emergency && cash <= thr); // budget: halt while broke + no income
				if (want)
				{
					auto const pType = pObj->GetTechnoType();
					const char* const id = pType ? pType->ID : "";
					bool const exempt = InIdList(id, pCat->Exclude)
						|| (breakCond && InIdList(id, pCat->BreakRules));
					if (!exempt) pause = true;
				}
			}
			if (pause)                                        // a type an urgent need wants now goes through
			{
				auto const nit = g_pauseExempt.find(pHouse->ArrayIndex);
				if (nit != g_pauseExempt.end())
					for (auto const& kv : nit->second)
						if (Unsorted::CurrentFrame <= kv.second.second && pObj->GetTechnoType() == kv.second.first)
							pause = false;
			}
			if (pause) { pFact->Suspend(true); anyPaused = true; }
			else pFact->Unsuspend(true);
		}
		return anyPaused;
	}
}

void Teams::EconomyOpening(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.EconomyEnable) return;                                       // opt-in
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral()
		|| pHouse->IsControlledByHuman()) return;                         // AI houses only

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.EconomyInterval > 0 ? cfg.EconomyInterval : 15;
	auto const it = g_economyLastFire.find(hIdx);
	if (it != g_economyLastFire.end() && now - it->second < interval) return;
	g_economyLastFire[hIdx] = now;

	bool const honorPrereq = cfg.HonorPrereqForEconomy();
	int const cash = static_cast<int>(pHouse->Available_Money());
	bool const hasIncome = HasIncome(pHouse);
	bool const emergency = EnemyCombatNear(pHouse, cfg.EconomyEmergencyRadius);

	// What to BUILD toward income — only while we don't have it yet. If it owns an income
	// structure but still has no income, it's the "refinery but no miner" case — don't queue
	// another refinery; a miner (never paused) is what then builds. Otherwise pick the opening:
	// capture a near derrick, or force a refinery (barracks first for rush safety when affordable).
	// (Pause enforcement below runs regardless of income, so a -1 debug halt keeps applying.)
	bool const derrickNear = cfg.EconomyCaptureForIncome && cfg.CaptureEnable
		&& NeutralCapturableNear(pHouse, cfg.EconomyDerrickTargets, cfg.EconomyRadius);
	const char* branch = hasIncome ? "has-income" : "wait-miner";

	if (!hasIncome && !OwnsIncomeStructure(pHouse))
	{
		if (derrickNear)
		{
			branch = "derrick";
			if (!OwnsBarracks(pHouse))
			{
				auto const pBar = BestEconomyBuilding(pHouse, EcoKind::Barracks, honorPrereq);
				if (pBar) QueueBaseNode(pHouse, pBar, pHouse->GetBaseCenter());
			}
			// CaptureDoctrine builds the engineers (allowed through the pause by a
			// BreakProductionRules.Infantry=ENGINEER + Condition.NeutralBuildings rule) and grabs it.
		}
		else
		{
			branch = "refinery";
			auto const pRef = BestEconomyBuilding(pHouse, EcoKind::Refinery, honorPrereq);
			auto const pBar = BestEconomyBuilding(pHouse, EcoKind::Barracks, honorPrereq);
			auto const pPow = BestEconomyBuilding(pHouse, EcoKind::Power, honorPrereq);
			int const powCost = pPow ? pPow->Cost : 0;
			int const barCost = pBar ? pBar->Cost : 0;
			// refCost: if the refinery isn't buildable yet (its prereq power plant is missing, so
			// honorPrereq makes pRef null), still price it via the no-prereq lookup for the budget math.
			auto const pRefCost = pRef ? pRef : BestEconomyBuilding(pHouse, EcoKind::Refinery, false);
			int const refCost = pRefCost ? pRefCost->Cost : 0;

			// The refinery's PREREQUISITE (and what powers it) is a power plant — build it FIRST if
			// the house has none, so the refinery becomes buildable and actually runs. honorPrereq
			// keeps pRef null until the power plant exists, so this naturally sequences power -> refinery.
			bool const ownsPower = OwnsPowerPlant(pHouse);
			if (!ownsPower && pPow)
			{
				QueueBaseNode(pHouse, pPow, pHouse->GetBaseCenter());
				branch = "power(prereq)";
			}

			// Barracks-first delays the refinery, so income arrives LATER — the cash bottoms out
			// (power + barracks + refinery all spent, no income yet) and the modpack AI firesales
			// its base in that low-cash window. Only do barracks-first when the whole opening would
			// still leave at least SolvencyReserve BEFORE income — otherwise build the refinery
			// FIRST so its income leads and the AI never dips that low. (Left alone the Easy AI
			// naturally builds the refinery before the barracks, which is exactly what we want.)
			int const threshold = cfg.EconomyBarracksFirstCash > 0
				? cfg.EconomyBarracksFirstCash : (powCost + barCost + refCost);
			bool const openingSolvent = (cash - powCost - barCost - refCost) >= cfg.EconomySolvencyReserve;
			bool const barracksFirst = cfg.EconomyBarracksFirst && pBar
				&& !OwnsBarracks(pHouse) && cash >= threshold && openingSolvent;

			if (pRef)                                          // refinery buildable (power is up)
			{
				if (barracksFirst)
				{
					QueueBaseNode(pHouse, pBar, pHouse->GetBaseCenter());       // rush safety first
					QueueBaseNode(pHouse, pRef, pHouse->GetBaseCenter());       // refinery next
					branch = "barracks+refinery";
				}
				else { QueueBaseNode(pHouse, pRef, pHouse->GetBaseCenter()); branch = "refinery(income-first)"; }
			}
			else if (ownsPower && pBar && !OwnsBarracks(pHouse))
			{
				// Refinery not buildable despite power (some other prereq) — don't stall; get a barracks.
				QueueBaseNode(pHouse, pBar, pHouse->GetBaseCenter());
				branch = "barracks(fallback)";
			}
			// else: waiting on the power plant (prereq) queued above before the refinery can go up.
		}
	}

	// Enforce the budget: pause production per the thresholds so the cash funds income, not an
	// early army. Emergency (enemy near) or income (top of fn) lifts it. Safe to hold: the
	// firesale is the modpack AI going broke, NOT our production hold (proven with DoctrineExt
	// removed entirely).
	bool const anyPaused = EconomyApplyPauses(pHouse, cash, hasIncome, emergency);
	g_economyLocked[hIdx] = anyPaused;

	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] economy: house=%s#%d branch=%s cash=%d hasIncome=%d paused=%d "
			"emergency=%d ownsBarracks=%d ownsIncomeStruct=%d.\n", pHouse->get_ID(), hIdx, branch,
			cash, hasIncome, anyPaused, emergency, OwnsBarracks(pHouse), OwnsIncomeStructure(pHouse));
}

// ─── Salvage (§10n) ─────────────────────────────────────────────────────────────
// The engine's raise-money strategy: HouseClass::AI (0x4FD500) asks 0x4FD9A0 whether the house lacks
// income it can afford (no refinery, or no miner, and not enough cash for the refinery / war factory /
// miner — so ALWAYS with no yard), and if so runs AI_RaiseMoney (0x4FDD10): sell base buildings newest
// first, yard included, until the cash covers it, then abandon every factory's production. In Rex's
// 11-08 game it sold a just-retaken yard with $5,542 in the bank and the house was defeated 50 frames
// later; with no yard it cancels every build each pass, so nothing — no engineer to retake the yard —
// ever gets built. Salvage decides first.
namespace
{
	struct SalvageCall { int Frame = -1; bool Block = false; const char* Why = ""; };
	std::map<int, SalvageCall> g_salvage;                 // house -> last raise-money decision
	std::map<int, int> g_raiseMoneyFrame;                 // house -> frame the engine's sale was let through

	bool OwnsYard(HouseClass* const pHouse)
	{
		for (auto const pB : pHouse->Buildings)
			if (pB && pB->Type && !pB->InLimbo && pB->Health > 0 && pB->Type->ConstructionYard) return true;
		return false;
	}

	// Never sold to raise money (Salvage.Keep). Auto: a construction yard, or the house's last factory of
	// its kind — selling the yard to afford a refinery only the yard can build is how the AI gives up.
	bool SalvageKeeps(BuildingClass* const pB)
	{
		auto const& cfg = DoctrineConfig::Get();
		if (cfg.SalvageKeepNone) return false;
		auto const pBt = pB->Type;
		if (!cfg.SalvageKeep.empty()) return ListHas(cfg.SalvageKeep, pBt->ID);
		if (pBt->ConstructionYard) return true;
		if (pBt->Factory == AbstractType::None) return false;
		for (auto const pO : pB->Owner->Buildings)
			if (pO && pO != pB && pO->Type && !pO->InLimbo && pO->Health > 0 && pO->Type->Factory == pBt->Factory
				&& pO->Type->Naval == pBt->Naval && pO->GetCurrentMission() != Mission::Selling)
				return false;
		return true;
	}

	// Can the house field an engineer: it owns one, or can build one and pay for it.
	bool EngineerObtainable(HouseClass* const pHouse)
	{
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (pT && pT->Owner == pHouse && IsLiveEngineer(pT)) return true;
		}
		auto const pEng = BestEngineer(pHouse, DoctrineConfig::Get().HonorPrereqForEngRush());
		return pEng && pHouse->Available_Money() >= pEng->GetActualCost(pHouse);
	}
}

bool Teams::SalvageBlocksRaiseMoney(HouseClass* pHouse)
{
	if (!pHouse || pHouse->Defeated || pHouse->IsNeutral() || pHouse->IsControlledByHuman()) return false;
	if (pHouse->CountResourceDestinations > 0 && pHouse->CountResourceGatherers > 0)
		return false;                                     // the engine's own check passes: nothing to decide
	DoctrineConfig::HouseScope const scope(pHouse);       // called from the engine's AI, outside any tick
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.SalvageEnable) return false;
	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	auto& s = g_salvage[hIdx];
	if (s.Frame < 0 || now - s.Frame >= 15)               // decided at most every 15 frames
	{
		bool block = true;
		const char* why = nullptr;
		if (cfg.SalvageBlockWithIncome && HasIncome(pHouse))
			why = "it has income by [Doctrine.Economy]'s measure (BlockWithIncome)";
		else if (cfg.SalvageBlockWithoutYard && !OwnsYard(pHouse))
			why = "it has no construction yard, so the money can't buy a refinery (BlockWithoutYard)";
		else if (cfg.SalvageBlockWhilePlan && EngineerObtainable(pHouse))
		{
			auto const rit = g_recaptureSeen.find(hIdx);
			int const rushInterval = cfg.EngRushInterval > 0 ? cfg.EngRushInterval : 60;
			if (cfg.EngRushEnable && cfg.EngRushRecapture && rit != g_recaptureSeen.end()
				&& now - rit->second <= 2 * rushInterval)
				why = "it can retake a building it lost (BlockWhilePlan, Recapture)";
			else if (cfg.CaptureEnable && !FindCaptureTargets(pHouse).empty())
				why = "it can capture income (BlockWhilePlan, [Doctrine.Capture])";
		}
		if (!why)
		{
			block = false;
			why = "nothing better to do; Keep buildings are still never sold";
		}
		bool const changed = s.Frame < 0 || block != s.Block || std::strcmp(why, s.Why) != 0;
		s.Frame = now; s.Block = block; s.Why = why;
		if (changed && cfg.DebugTicks)
			Debug::Log("[DoctrineExt] salvage: house=%s#%d %s the engine's raise-money base sale: %s. cash=%d "
				"refineries=%d miners=%d.\n", pHouse->get_ID(), hIdx, block ? "blocks" : "allows", why,
				static_cast<int>(pHouse->Available_Money()), pHouse->CountResourceDestinations,
				pHouse->CountResourceGatherers);
	}
	if (!s.Block) g_raiseMoneyFrame[hIdx] = now;
	return s.Block;
}

// ─── Captured buildings (§10o) ──────────────────────────────────────────────────
// The engine sells every building an AI captures from another player, the same frame (6 of the 7 yards
// captured in the 11-08 30-player run). The capturer of the 7th had no income and kept it only because
// [Doctrine.Economy] PreventSelling blocked that sale too. Captured decides on purpose: keep what we lack and can hold (our
// only yard always), sell the rest for the refund, and sell the enemy's last yard so they can't rebuild.
namespace
{
	BuildingClass* g_captureSell = nullptr;               // the sale Captured chose, let past ShouldBlockSell
	int g_captureSellFrame = -1;

	bool OwnsOtherYard(HouseClass* const pHouse, BuildingClass* const pExcept)
	{
		for (auto const pB : pHouse->Buildings)
			if (pB && pB != pExcept && pB->Type && !pB->InLimbo && pB->Health > 0 && pB->Type->ConstructionYard
				&& pB->GetCurrentMission() != Mission::Selling)
				return true;
		return false;
	}

	// Was it the previous owner's last way to build: no other yard, no MCV (DenyLastYard)?
	bool LastYardOf(BuildingClass* const pB, HouseClass* const pPrev)
	{
		if (!pB->Type->ConstructionYard || !pPrev || pPrev->Defeated || OwnsOtherYard(pPrev, pB)) return false;
		for (auto const pU : UnitClass::Array)
			if (pU && pU->Owner == pPrev && !pU->InLimbo && pU->Health > 0 && pU->Type->DeploysInto
				&& pU->Type->DeploysInto->ConstructionYard)
				return false;
		return true;
	}

	// The gap a captured building fills for the house (KeepIfNeeded), or null.
	const char* CapturedGap(BuildingClass* const pB, HouseClass* const pHouse)
	{
		auto const pBt = pB->Type;
		bool const factory = pBt->Factory != AbstractType::None;
		bool sameFactory = false, income = false;
		int power = 0, drain = 0;
		for (auto const pO : pHouse->Buildings)
		{
			if (!pO || pO == pB || !pO->Type || pO->InLimbo || pO->Health <= 0) continue;
			if (pO->GetCurrentMission() == Mission::Selling) continue;
			auto const pOt = pO->Type;
			if (factory && pOt->Factory == pBt->Factory && pOt->Naval == pBt->Naval) sameFactory = true;
			if (IncomeKindOf(pOt) > 0) income = true;
			power += pOt->PowerBonus;
			drain += pOt->PowerDrain;
		}
		if (factory && !sameFactory) return "our only factory of its kind";
		if (IncomeKindOf(pBt) > 0 && !income) return "our only income building";
		if (pBt->PowerBonus > 0 && power < drain) return "we're short of power without it";
		return nullptr;
	}

	// Can our side build this at all (KeepNewTech)? Owner= and TechLevel, not today's prerequisites.
	bool NewTech(BuildingTypeClass* const pBt, HouseClass* const pHouse)
	{
		int const maxTL = (pHouse->TechLevel >= 0 && pHouse->TechLevel <= 10) ? pHouse->TechLevel : 10;
		return pBt->TechLevel < 0 || pBt->TechLevel > maxTL || !pHouse->InOwners(pBt);
	}

	// Armed value within range of a point (Hold.Radius): ours and our allies', or everyone hostile to us.
	// Units and defenses at cost; miners, MCVs and engineers don't count.
	int ArmedValueNear(HouseClass* const pHouse, CoordStruct const& at, double const range, bool const hostile)
	{
		int v = 0;
		for (int i = 0; i < TechnoClass::Array.Count; ++i)
		{
			auto const pT = TechnoClass::Array.GetItem(i);
			if (!pT || !pT->Owner || pT->InLimbo || pT->Health <= 0) continue;
			bool const side = hostile ? IsHostileTo(pHouse, pT->Owner)
				: (pT->Owner == pHouse || pHouse->IsAlliedWith(pT->Owner));
			if (!side || Dist2D(at, pT->GetCoords()) > range) continue;
			auto const pType = pT->GetTechnoType();
			if (!pType || pType->ResourceGatherer || pType->DeploysInto || !HasOffensiveWeapon(pType)) continue;
			if (pType->WhatAmI() == AbstractType::InfantryType && static_cast<InfantryTypeClass*>(pType)->Engineer) continue;
			v += pType->GetActualCost(pT->Owner);
		}
		return v;
	}

	// Cells from a point to the nearest barracks (infantry factory) of a house hostile to us
	// (Hold.EnemyBarracks); -1 if none.
	double NearestEnemyBarracks(HouseClass* const pHouse, CoordStruct const& at)
	{
		double best = -1.0;
		for (int i = 0; i < BuildingClass::Array.Count; ++i)
		{
			auto const pB = BuildingClass::Array.GetItem(i);
			if (!pB || !pB->Type || !pB->Owner || pB->InLimbo || pB->Health <= 0) continue;
			if (pB->Type->Factory != AbstractType::InfantryType || !IsHostileTo(pHouse, pB->Owner)) continue;
			double const d = Dist2D(at, pB->GetCoords()) / 256.0;
			if (best < 0.0 || d < best) best = d;
		}
		return best;
	}
}

bool Teams::KeepCaptured(BuildingClass* pBld, HouseClass* pOld, HouseClass* pNew)
{
	if (!pBld || !pBld->Type || !pNew || pBld->Owner != pNew || pOld == pNew || pNew->IsControlledByHuman())
		return false;
	DoctrineConfig::HouseScope const scope(pNew);         // called from the engine's ownership change
	auto const& cfg = DoctrineConfig::Get();
	if (!cfg.CapturedEnable) return false;                // the engine's sale, as before
	auto const pBt = pBld->Type;
	bool keep = false;
	const char* why = nullptr;
	static char whyBuf[192];
	int ours = -1, theirs = -1;
	double barracks = -1.0;
	auto const holds = [&]()                              // Hold.Radius / Hold.Ratio / Hold.EnemyBarracks, measured once
	{
		if (ours < 0)
		{
			auto const at = pBld->GetCoords();
			double const range = (cfg.CapturedHoldRadius > 0 ? cfg.CapturedHoldRadius : 10) * 256.0;
			ours = ArmedValueNear(pNew, at, range, false);
			theirs = ArmedValueNear(pNew, at, range, true);
			barracks = NearestEnemyBarracks(pNew, at);
		}
		// An enemy barracks next door walks an engineer in before whatever stands here now (often just
		// the rush's escorts, who move on) can be counted on.
		if (cfg.CapturedHoldEnemyBarracks > 0 && barracks >= 0.0 && barracks <= cfg.CapturedHoldEnemyBarracks)
			return false;
		return cfg.CapturedHoldRatio <= 0.0 || ours >= theirs * cfg.CapturedHoldRatio;
	};
	if (cfg.CapturedKeepNone) why = "Keep=none: every capture is sold";
	else if (!cfg.CapturedKeep.empty())
	{
		keep = ListHas(cfg.CapturedKeep, pBt->ID);
		why = keep ? "on the Keep list" : "not on the Keep list";
	}
	else if (ListHas(cfg.CapturedSell, pBt->ID)) why = "on the Sell list";
	else if (cfg.CapturedKeepOnlyYard && pBt->ConstructionYard && !OwnsOtherYard(pNew, pBld))
	{
		keep = true;
		why = "our only construction yard: the only way we can build (KeepOnlyYard)";
	}
	else if (cfg.CapturedKeepYard && pBt->ConstructionYard && (!cfg.CapturedKeepYardMustHold || holds()))
	{
		keep = true;
		why = cfg.CapturedKeepYardMustHold ? "a forward construction yard we can hold (KeepYard)"
			: "a forward construction yard (KeepYard, MustHold=no)";
	}
	else if (cfg.CapturedDenyLastYard && LastYardOf(pBld, pOld))
		why = "their last construction yard: sold, they can't rebuild (DenyLastYard)";
	else
	{
		const char* const gap = cfg.CapturedKeepIfNeeded ? CapturedGap(pBld, pNew) : nullptr;
		bool const tech = cfg.CapturedKeepNewTech && NewTech(pBt, pNew);
		if (!gap && !tech) why = "nothing we lack: take the refund";
		else
		{
			keep = holds();
			std::snprintf(whyBuf, sizeof(whyBuf), "%s, and we %s hold it (%s)", gap ? gap : "our side can't build it",
				keep ? "can" : "can't", keep ? "KeepIfNeeded/KeepNewTech" : "take the refund before it's retaken");
			why = whyBuf;
		}
	}
	if (!keep)
	{
		g_captureSell = pBld;                             // past [Doctrine.Economy] PreventSelling
		g_captureSellFrame = Unsorted::CurrentFrame;
	}
	else
		g_keptCaptures.insert(pBld->UniqueID);            // DenyRecapture sells it before it's retaken
	if (cfg.DebugTicks)
		Debug::Log("[DoctrineExt] captured: house=%s#%d %s %s taken from %s#%d: %s. refund=%d cash=%d "
			"armed near (Hold.Radius): ours=%d theirs=%d, enemy barracks %.1fc.\n", pNew->get_ID(), pNew->ArrayIndex,
			keep ? "keeps" : "sells", pBt->ID, pOld ? pOld->get_ID() : "?", pOld ? pOld->ArrayIndex : -1, why,
			static_cast<int>(pBt->GetActualCost(pNew) * RulesClass::Instance->RefundPercent),
			static_cast<int>(pNew->Available_Money()), ours, theirs, barracks);
	return keep;
}

bool Teams::ShouldBlockSell(BuildingClass* pBld)
{
	if (g_denySelling) return false;                      // our own capture-denial sale (§10l)
	if (pBld && pBld == g_captureSell && g_captureSellFrame == Unsorted::CurrentFrame)
	{
		g_captureSell = nullptr;                          // the sale [Doctrine.Captured] chose (§10o)
		return false;
	}
	if (!pBld || !pBld->Type) return false;
	DoctrineConfig::HouseScope const scope(pBld->Owner);  // called from the Sell hook, outside any tick
	auto const& cfg = DoctrineConfig::Get();
	auto const pH = pBld->Owner;
	if (!pH || pH->Defeated || pH->IsNeutral() || pH->IsControlledByHuman())
		return false;                                     // AI houses only; never the human player
	// Salvage: the engine's raise-money sale let through this frame never takes a Keep building.
	if (cfg.SalvageEnable)
	{
		auto const rit = g_raiseMoneyFrame.find(pH->ArrayIndex);
		if (rit != g_raiseMoneyFrame.end() && rit->second == Unsorted::CurrentFrame && SalvageKeeps(pBld))
		{
			if (cfg.DebugTicks)
				Debug::Log("[DoctrineExt] salvage: house=%s#%d keeps %s from the engine's raise-money sale "
					"(Salvage.Keep).\n", pH->get_ID(), pH->ArrayIndex, pBld->Type->ID);
			return true;
		}
	}
	if (!cfg.EconomyEnable || !cfg.EconomyPreventSelling) return false;
	// Protect only while the house is still securing income (the firesale window) unless the modder
	// wants it always. Once it owns a working refinery+miner (HasIncome) it may sell normally again.
	if (!cfg.EconomyPreventSellingAlways && HasIncome(pH)) return false;
	return true;
}

// BuildingClass::Sell (ObjectClass::Sell override, entry 0x447110) — both the control==-1 and the
// "other control" paths jmp to 0x44717d to QueueMission(Mission::Selling); control==0 only continues
// an already-started sale. So 0x44717d is the single point a NEW sale is committed. For a protected
// AI building we jump to the engine's own don't-sell return (0x447203 = pop esi; ret 4), so the
// building never enters Mission::Selling — no sell-down animation, no Guard ping-pong. 6 stolen bytes
// (mov edx,(esi); push 0; push 0x13) land on a clean boundary and don't overlap the Ares/Antares
// hooks at 0x447113 / 0x4471D5.
DEFINE_HOOK(0x44717D, BuildingClass_Sell_BlockAIFiresale, 0x6)
{
	GET(BuildingClass* const, pThis, ESI);
	return Teams::ShouldBlockSell(pThis) ? 0x447203 : 0;
}

// The raise-money urgency (0x4FD9A0, __thiscall, no arguments; its only caller is HouseClass::AI at
// 0x4FD500): when Salvage blocks, jump to 0x4104B0 (xor eax,eax; ret) — at entry ESP holds the return
// address, so that returns "not urgent" and AI_RaiseMoney (0x4FDD10) doesn't run. 8 stolen bytes = two
// whole instructions (sub esp,14h; mov eax,[0A8B238h]), nothing branches into them. Phobos's
// AI_RaiseMoney hook at 0x4FE0A3 is inside 0x4FDD10 and untouched.
DEFINE_HOOK(0x4FD9A0, HouseClass_RaiseMoneyUrgency_Salvage, 0x8)
{
	GET(HouseClass* const, pThis, ECX);
	return Teams::SalvageBlocksRaiseMoney(pThis) ? 0x4104B0 : 0;
}

// BuildingClass::ChangeOwnership (0x448260): once an AI house has taken a building from another player
// (not in a campaign, not neutral tech, not its own retaken; checked at 0x4492D7..0x449304), the engine
// ejects any occupants and sells it (0x449306..0x449368). Keep jumps to 0x44936E, past the sale; sell
// falls through. ESI = the building, EBX = the old owner (read at 0x448713), EBP = the new owner (the
// argument). 6 stolen bytes = one instruction (mov al,[esi+702h]); no other loaded DLL hooks
// 0x4492D7..0x44936E.
DEFINE_HOOK(0x449306, BuildingClass_ChangeOwnership_Captured, 0x6)
{
	GET(BuildingClass* const, pThis, ESI);
	GET(HouseClass* const, pOld, EBX);
	GET(HouseClass* const, pNew, EBP);
	return Teams::KeepCaptured(pThis, pOld, pNew) ? 0x44936E : 0;
}

int Teams::ChaserGateMax() { return g_chaserGateMax; }

void Teams::Reset()
{
	g_chaserGateMax = 0;
	g_budgetLastFire.clear();
	g_budgetLocked.clear();
	g_warned.clear();
	g_rushLastFire.clear();
	g_powerTable.clear();
	g_powerFrame = -1;
	g_slotDispatchFrame.clear();
	g_slotPriority.clear();
	g_interceptSlots.clear();
	g_defendSlots.clear();
	g_huntSlots.clear();
	g_decapSlots.clear();
	g_floodLastFire.clear();
	g_reserveLastFire.clear();
	g_crateLastFire.clear();
	g_squadTargets.clear();
	g_catcherFail.clear();
	g_crateProgress.clear();
	g_crateBlacklist.clear();
	g_waterLastFire.clear();
	g_waterTargets.clear();
	g_waterProgress.clear();
	g_waterBlacklist.clear();
	g_cmdrIdleSince.clear();
	g_cmdrLastEval.clear();
	g_airLastFire.clear();
	g_aaTargets.clear();
	g_navalLastFire.clear();
	g_expandLastFire.clear();
	g_garrisonLastFire.clear();
	g_prereqAudited.clear();
	g_moneyHistory.clear();
	g_captureLastFire.clear();
	g_captureProgress.clear();
	g_captureBlacklist.clear();
	g_economyLastFire.clear();
	g_economyLocked.clear();
	g_counterEngLastFire.clear();
	g_responders.clear();
	g_interceptCells.clear();
	g_picketSightings.clear();
	g_picketSeen.clear();
	g_picketPosts.clear();
	g_picketAssign.clear();
	g_picketLastFire.clear();
	g_wallLastFire.clear();
	g_wallMine.clear();
	g_wallRetry.clear();
	g_wallTypeOk.clear();
	g_denySelling = false;
	g_engTracks.clear();
	g_keptCaptures.clear();
	g_engTrackFrame = -1;
	g_denySells.clear();
	g_denySellFrames.clear();
	g_sacrifices.clear();
	g_letGo.clear();
	g_pauseExempt.clear();
	g_urgentPicks.clear();
	g_engRushLastFire.clear();
	g_engRushAssign.clear();
	g_rushAtDoor.clear();
	g_engRushCooldown.clear();
	g_engRushMine.clear();
	g_engRushLost.clear();
	g_recaptureSeen.clear();
	g_salvage.clear();
	g_raiseMoneyFrame.clear();
	g_captureSell = nullptr;
	g_captureSellFrame = -1;
}
