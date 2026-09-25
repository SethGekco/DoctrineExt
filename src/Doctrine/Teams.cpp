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

#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdio>
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

	// STRICT buildability for anything DoctrineExt PRODUCES. This stack's
	// CanBuild is leaky (it OK'd cross-faction builds, ignoring Owner=), so we
	// don't trust it alone — explicitly enforce TechLevel (a TechLevel=11 unit
	// is above the game's max-10 tech level, so never buildable) and Owner=,
	// then let CanBuild cover prerequisites + build limits. Used before any
	// DemandProduction; the recruit path may still USE already-owned units.
	bool CanBuildStrict(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (!pType) return false;
		int const tl = pType->TechLevel;
		if (tl < 0) return false;                          // -1 = disabled
		// Compare to the house's tech level, but never above the game's max (10);
		// so TechLevel=11 is unbuildable even if the house's field reads oddly.
		int maxTL = (pHouse->TechLevel >= 0 && pHouse->TechLevel <= 10)
			? pHouse->TechLevel : 10;
		if (tl > maxTL) return false;                      // e.g. TechLevel=11
		if (DoctrineConfig::Instance.StrictOwnership && !pHouse->InOwners(pType))
			return false;
		return pHouse->CanBuild(pType, false, true) == CanBuildResult::Buildable;
	}

	// Is pType usable by pHouse right now (buildable respecting Owner=, or
	// already owned)? Same rule PickType applies, factored out for PickCounter.
	bool IsEligible(HouseClass* const pHouse, TechnoTypeClass* const pType)
	{
		if (!pType || !IsTeamable(pType)) return false;
		bool const ownerOK = !DoctrineConfig::Instance.StrictOwnership
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
			if (DoctrineConfig::Instance.DebugTicks)
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
		auto const& c = DoctrineConfig::Instance;
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

		int const killThreshold = DoctrineConfig::Instance.DeathZoneMinStrength > 0
			? DoctrineConfig::Instance.DeathZoneMinStrength : 48;
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

		auto const& cfg = DoctrineConfig::Instance;
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
		&& counterScore < DoctrineConfig::Instance.MinCounterScore)
	{
		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] %s declines hunt for %s#%d: best counter %s "
				"scores %.2f < MinCounterScore %.2f (no viable counter).\n",
				rule.Name.c_str(), pHouse->get_ID(), pHouse->ArrayIndex, pType->ID,
				counterScore, DoctrineConfig::Instance.MinCounterScore);
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
	if (cfg.AutoProduce && got < count && CanBuildStrict(pHouse, pType))
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

	int const standoffCells = DoctrineConfig::Instance.InterceptStandoff;
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

	// Proof-of-orientation trace (throttled, DebugTicks): the bearing the
	// defenders are actually fanning toward and whether it came from the
	// learned lane or the geometric fallback.
	if (DoctrineConfig::Instance.DebugTicks)
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
	int const scanCells = DoctrineConfig::Instance.SupportScanRadius > 0
		? DoctrineConfig::Instance.SupportScanRadius : 8;
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

	int const standoffCells = DoctrineConfig::Instance.HuntStandoff > 0
		? DoctrineConfig::Instance.HuntStandoff : 5;
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
			< DoctrineConfig::Instance.DeathZoneMinStrength)
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

	int CrateKey(CellClass* const pCell)
	{
		auto const c = pCell->GetCellCoords();
		return ((c.X / 256) << 16) | ((c.Y / 256) & 0xFFFF);
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
		auto const& chasers = DoctrineConfig::Instance.CrateChasers;
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
		if (byListFree)  return byListFree;   // teamless, on the list — ideal
		if (bySpeedFree) return bySpeedFree;  // teamless, fastest
		if (byListTeam)  return byListTeam;   // divert: teamed, on the list
		return bySpeedTeam;                   // divert: teamed, fastest (or null)
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
			|| (id[1] == 'G' && id[2] == 'A'));                 // DGAR
	}
}

void Teams::GarrisonDoctrine(HouseClass* pHouse)
{
	auto const& cfg = DoctrineConfig::Instance;
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
	auto const& cfg = DoctrineConfig::Instance;
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
	int const r = (DoctrineConfig::Instance.CrateScanRadius > 0
		? DoctrineConfig::Instance.CrateScanRadius : 30) * 2;
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
	int DesiredSquadSize()
	{
		auto const& cfg = DoctrineConfig::Instance;
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

	// A grabber type's chase radius in CELLS: its [Doctrine.General] CrateChasers
	// entry (0 = map-wide), or the CrateSquadScan default if unlisted/omitted.
	// 0 is returned verbatim to mean "unlimited" — callers treat 0 specially.
	int GrabRadius(TechnoTypeClass* const pType)
	{
		auto const& cfg = DoctrineConfig::Instance;
		int const def = cfg.CrateSquadScan > 0 ? cfg.CrateSquadScan : 60;
		if (!pType) return def;
		for (auto const& e : cfg.CrateChasers)
			if (e.ID == pType->ID)
				return e.Radius >= 0 ? e.Radius : def;  // 0 = map-wide
		return def;
	}

	// The house's PERSISTENT crate squad team (ID "<prefix><house>*", DCRS for the
	// ground squad / DCRW for the water squad). Created once and kept alive across
	// ticks (membership managed by hand, not auto-recruited); never destroyed on
	// idle, unlike the single-grabber team.
	TeamClass* EnsureSquadTeam(HouseClass* const pHouse, int const desired,
		TechnoTypeClass* const pRep, const char* const prefix = "DCRS")
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

		// No production requirement: pRep is null (the squad only diverts existing
		// units), and amount 0 means the team never force-builds anything — the
		// force-build path ignores prereqs/faction, so we keep it shut off.
		pTF->CountEntries = pRep ? 1 : 0;
		pTF->Entries[0] = { pRep ? (desired > 0 ? desired : 1) : 0, pRep };
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

	// Best fast armed non-gatherer unit NOT already on pExclude, for squad
	// recruiting: prefers teamless (CrateChasers list, then fastest), falls back
	// to a teamed unit (caller liberates it) since the AI teams ~everything.
	FootClass* PickSquadRecruit(HouseClass* const pHouse, TeamClass* const pExclude)
	{
		auto const& chasers = DoctrineConfig::Instance.CrateChasers;
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
	auto const& cfg = DoctrineConfig::Instance;
	if (!cfg.CrateSquad) return; // opt-in

	int const now = Unsorted::CurrentFrame;
	int const hIdx = pHouse->ArrayIndex;
	int const interval = cfg.CrateInterval > 0 ? cfg.CrateInterval : 90;
	auto const it = g_crateLastFire.find(hIdx);
	if (it != g_crateLastFire.end() && now - it->second < interval) return;
	g_crateLastFire[hIdx] = now;

	int const desired = DesiredSquadSize();
	if (desired <= 0) return; // crates off globally

	// The squad NEVER produces — it only diverts existing units. A team taskforce
	// force-builds its entry type ignoring prerequisites AND faction (that is how
	// a Soviet AI built an Allied CLEG, and an Allied AI built CLEG before owning
	// its GATECH prereq). So the taskforce type is left null: no production vector
	// at all, no illegal builds. Membership is filled purely by PickSquadRecruit.
	auto const pTeam = EnsureSquadTeam(pHouse, desired, nullptr);
	if (!pTeam) return;

	// Maintain membership: divert fast armed units onto the squad until at
	// `desired`, liberating teamed ones (the AI owns ~all units via teams).
	int members = 0;
	for (auto pF = pTeam->FirstUnit; pF; pF = pF->NextTeamMember)
		if (!pF->InLimbo && pF->Health > 0) ++members;
	int recruited = 0;
	while (members < desired)
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
	bool anyMapwide = false;
	for (auto const pF : mem) if (GrabRadius(pF->GetTechnoType()) == 0) { anyMapwide = true; break; }
	if (anyMapwide)
	{
		auto const& b = MapClass::Instance.MapCoordBounds; // whole map (cell LTRB)
		for (int y = b.Top; y <= b.Bottom; ++y)
			for (int x = b.Left; x <= b.Right; ++x)
			{
				CellStruct cs; cs.X = static_cast<short>(x); cs.Y = static_cast<short>(y);
				addCell(MapClass::Instance.TryGetCellAt(cs));
			}
	}
	else
	{
		scanBox(base, cfg.CrateSquadScan > scan ? cfg.CrateSquadScan : scan);
		for (auto const pF : mem)
			scanBox(pF->GetCoords(), GrabRadius(pF->GetTechnoType()));
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
		int const gr = GrabRadius(pF->GetTechnoType());          // 0 = map-wide
		double const grLep = gr > 0 ? gr * 256.0 : 1e18;         // this unit's reach
		int best = -1; double bestD = 1e18;
		for (int c = 0; c < static_cast<int>(crates.size()); ++c)
		{
			if (crateTaken[c]) continue;
			auto const cc = crates[c]->GetCellCoords();
			double const d = std::sqrt(double(cc.X - fc.X) * (cc.X - fc.X)
				+ double(cc.Y - fc.Y) * (cc.Y - fc.Y));
			if (d > grLep) continue;                              // beyond its reach
			if (d < bestD) { bestD = d; best = c; }
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
			"crates=%d raced=%d nearestChaseCells=%.1f chaser=%s.\n",
			pHouse->get_ID(), hIdx, desired, N, recruited, static_cast<int>(crates.size()), raced,
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
			if (got && DoctrineConfig::Instance.DebugTicks)
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
	auto const& cfg = DoctrineConfig::Instance;
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
	auto const& cfg = DoctrineConfig::Instance;
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
	auto const& cfg = DoctrineConfig::Instance;
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
	auto const& cfg = DoctrineConfig::Instance;
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

void Teams::Reset()
{
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
	g_crateProgress.clear();
	g_crateBlacklist.clear();
	g_cmdrIdleSince.clear();
	g_cmdrLastEval.clear();
	g_garrisonLastFire.clear();
	g_prereqAudited.clear();
	g_moneyHistory.clear();
}
