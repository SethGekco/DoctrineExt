#include "Doctrine/Deploy.h"
#include "Doctrine/Config.h"
#include "Doctrine/Verses.h"

#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <AircraftClass.h>
#include <HouseClass.h>
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>
#include <BulletTypeClass.h>
#include <Unsorted.h>
#include <GeneralDefinitions.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
	// Per config (Types / Exclude can differ per AI difficulty): InfantryTypeClass::Array index -> 1 deploys,
	// 0 doesn't.
	std::map<DoctrineConfig const*, std::vector<signed char>> g_tables;
	// UniqueID -> frame DoctrineExt last deployed it or stood it up ([Doctrine.Deploy] Settle).
	std::map<DWORD, int> g_changed;
	// Enemies of anyone in the air (aircraft, jumpjets, balloons), gathered once per frame when asked for.
	std::vector<TechnoClass*> g_air;
	int g_airFrame = -1;
	int g_lastSweep = -1;
	std::set<std::string> g_warned;

	// Decisions since the last summary line (Debug).
	struct Counts { int Approach = 0, InRange = 0, Air = 0, StoodUp = 0, Held = 0, AirWait = 0, LetStand = 0; } g_counts;

	unsigned Bit(Mission const m)
	{
		auto const v = static_cast<int>(m);
		return v >= 0 && v < 32 ? 1u << v : 0u;
	}

	void WarnOnce(std::string const& key, const char* const pFormat, const char* const pArg)
	{
		if (g_warned.insert(key).second)
			Debug::Log(pFormat, pArg);
	}

	// The weapon a deployed one fires: its DeployFireWeapon (default 1, the secondary).
	int DeployedIndex(InfantryTypeClass const* const pType)
	{
		return pType->DeployFireWeapon >= 0 ? pType->DeployFireWeapon : 1;
	}

	// Auto: deploys and stays deployed to fire (Deployer, DeployFire, no UndeployDelay, like GI and Guardian GI;
	// Yuri and Desolator deploy for one blast instead), with a deployed weapon other than the first.
	bool AutoDeploys(InfantryTypeClass* const pType)
	{
		if (!pType->Deployer || !pType->DeployFire || pType->UndeployDelay >= 0) return false;
		auto const pDep = pType->GetWeapon(DeployedIndex(pType));
		if (!pDep || !pDep->WeaponType || pDep->WeaponType->AreaFire) return false;
		auto const pUp = pType->GetWeapon(0);
		return !pUp || pUp->WeaponType != pDep->WeaponType;
	}

	std::vector<signed char> const& TableFor(DoctrineConfig const& cfg)
	{
		auto& table = g_tables[&cfg];
		int const count = InfantryTypeClass::Array.Count;
		if (static_cast<int>(table.size()) == count)
			return table;

		table.assign(count, 0);
		if (cfg.DeployTypesNone)
			return table;
		auto const listed = [](std::vector<std::string> const& list, const char* const id)
		{
			for (auto const& s : list)
				if (_strcmpi(s.c_str(), id) == 0) return true;
			return false;
		};
		for (int i = 0; i < count; ++i)
		{
			auto const pType = InfantryTypeClass::Array.GetItem(i);
			if (!pType || listed(cfg.DeployExclude, pType->ID)) continue;
			if (cfg.DeployTypes.empty())
				table[i] = AutoDeploys(pType);
			else if (listed(cfg.DeployTypes, pType->ID))
			{
				table[i] = pType->Deployer && pType->DeployFire;
				if (!table[i])
					WarnOnce(std::string("type:") + pType->ID, "[DoctrineExt] WARNING: [Doctrine.Deploy] Types= lists "
						"%s, which can't deploy to fire (needs Deployer=yes and DeployFire=yes): skipped.\n", pType->ID);
			}
		}
		for (auto const& s : cfg.DeployTypes)
			if (!InfantryTypeClass::Find(s.c_str()))
				WarnOnce("unknown:" + s, "[DoctrineExt] WARNING: [Doctrine.Deploy] Types= lists %s, which isn't an "
					"infantry type: skipped.\n", s.c_str());
		return table;
	}

	bool Manages(DoctrineConfig const& cfg, InfantryClass* const pInf)
	{
		auto const& managed = TableFor(cfg);
		int const i = pInf->Type ? pInf->Type->ArrayIndex : -1;
		return i >= 0 && i < static_cast<int>(managed.size()) && managed[i] != 0;
	}

	// An AI's infantry, deploy doctrine on for its difficulty: that difficulty's tags, else null.
	DoctrineConfig const* ConfigFor(InfantryClass* const pInf)
	{
		auto const pOwner = pInf->Owner;
		if (!pOwner || pOwner->Defeated || pOwner->IsNeutral() || pOwner->IsControlledByHuman())
			return nullptr;
		if (!DoctrineConfig::Instance.Parsed)
			DoctrineConfig::EnsureParsed();
		auto const& cfg = DoctrineConfig::For(pOwner);
		return cfg.DeployEnable && Manages(cfg, pInf) ? &cfg : nullptr;
	}

	bool IsDeployed(Sequence const s)
	{
		return s == Sequence::Deploy || s == Sequence::Deployed || s == Sequence::DeployedFire
			|| s == Sequence::DeployedIdle;
	}

	WeaponTypeClass* WeaponOf(InfantryClass* const pInf, int const index)
	{
		auto const pWS = pInf->GetWeapon(index);   // the elite one for a veteran
		return pWS ? pWS->WeaponType : nullptr;
	}

	bool HitsAir(WeaponTypeClass* const pWeapon)
	{
		return pWeapon && pWeapon->Projectile && pWeapon->Projectile->AA;
	}

	// Damage per frame the weapon does to the target: Damage x Burst x Verses for its armor / ROF. 0 when it
	// can't hit it (an air target and no AA, a ground one and no AG).
	double Dps(WeaponTypeClass* const pWeapon, TechnoClass* const pTarget)
	{
		if (!pWeapon || pWeapon->ROF <= 0 || pWeapon->Damage <= 0 || !pWeapon->Warhead || !pWeapon->Projectile)
			return 0.0;
		if (pTarget->IsInAir() ? !pWeapon->Projectile->AA : !pWeapon->Projectile->AG)
			return 0.0;
		auto const pType = pTarget->GetTechnoType();
		double const verses = pType ? Verses::Of(pWeapon->Warhead, static_cast<int>(pType->Armor)) : 1.0;
		int const burst = pWeapon->Burst > 0 ? pWeapon->Burst : 1;
		return pWeapon->Damage * burst * verses / pWeapon->ROF;
	}

	// The unit's target, when it's a fight: a neutral one (a civilian building, say) never decides a deploy.
	TechnoClass* FightTarget(InfantryClass* const pInf)
	{
		auto const pTarget = abstract_cast<TechnoClass*>(pInf->Target);
		return pTarget && pTarget->Owner && !pTarget->Owner->IsNeutral() ? pTarget : nullptr;
	}

	bool Settled(DoctrineConfig const& cfg, InfantryClass* const pInf, int const now)
	{
		auto const it = g_changed.find(pInf->UniqueID);
		return it == g_changed.end() || now - it->second >= cfg.DeploySettle;
	}

	const char* TypeId(TechnoClass* const pTechno)
	{
		auto const pType = pTechno ? pTechno->GetTechnoType() : nullptr;
		return pType ? pType->ID : "?";
	}

	void Log(DoctrineConfig const& cfg, InfantryClass* const pInf, const char* const what, TechnoClass* const pTarget,
		double const dep, double const up)
	{
		if (!cfg.DeployDebug) return;
		Debug::Log("[DoctrineExt] deploy: %s#%u of %s#%d %s %s (deployed %.2f vs %.2f damage/frame)\n", pInf->Type->ID,
			pInf->UniqueID, pInf->Owner->get_ID(), pInf->Owner->ArrayIndex, what, TypeId(pTarget), dep, up);
	}

	// Deploy the way the engine's AI does it (ApproachTarget 0x522444): a moving unit stops and deploys once
	// stopped (ShouldDeploy, 0x521B40), a standing one deploys now.
	void DeployNow(InfantryClass* const pInf, int const now)
	{
		auto const pLoco = pInf->Locomotor.GetInterfacePtr();
		if (pLoco && pLoco->Is_Moving())
		{
			pLoco->Stop_Moving();
			pInf->ShouldDeploy = true;
		}
		else
			pInf->PlayAnim(Sequence::Deploy, false, false);
		g_changed[pInf->UniqueID] = now;
	}

	std::vector<TechnoClass*> const& Airborne(int const now)
	{
		if (g_airFrame == now)
			return g_air;
		g_airFrame = now;
		g_air.clear();
		auto const add = [](TechnoClass* const pT)
		{
			if (pT && !pT->InLimbo && pT->IsAlive && pT->Health > 0 && pT->IsInAir())
				g_air.push_back(pT);
		};
		for (int i = 0; i < AircraftClass::Array.Count; ++i)
			add(AircraftClass::Array.GetItem(i));
		for (int i = 0; i < InfantryClass::Array.Count; ++i)
		{
			auto const pInf = InfantryClass::Array.GetItem(i);
			if (pInf && pInf->Type && (pInf->Type->JumpJet || pInf->Type->BalloonHover))
				add(pInf);
		}
		for (int i = 0; i < UnitClass::Array.Count; ++i)
		{
			auto const pUnit = UnitClass::Array.GetItem(i);
			if (pUnit && pUnit->Type && (pUnit->Type->JumpJet || pUnit->Type->BalloonHover))
				add(pUnit);
		}
		return g_air;
	}

	// The nearest enemy in the air within the deployed weapon's range + reach cells, or null. Reach 0: in
	// range as the engine measures it.
	TechnoClass* AirInReach(InfantryClass* const pInf, int const depIndex, WeaponTypeClass* const pWeapon,
		int const reach, int const now)
	{
		double const r = pWeapon->Range + std::max(0, reach) * 256.0;
		auto const c = pInf->GetCoords();
		TechnoClass* pBest = nullptr;
		double best = r * r;
		for (auto const pAir : Airborne(now))
		{
			if (!pAir->Owner || pInf->Owner->IsAlliedWith(pAir->Owner)) continue;
			auto const a = pAir->GetCoords();
			double const dx = a.X - c.X, dy = a.Y - c.Y, d = dx * dx + dy * dy;
			if (d <= best && (reach > 0 || pInf->IsCloseEnough(pAir, depIndex))) { best = d; pBest = pAir; }
		}
		return pBest;
	}

	void Look(DoctrineConfig const& cfg, InfantryClass* const pInf, int const now)
	{
		auto const seq = pInf->SequenceAnim;
		if (seq == Sequence::Undeploy || pInf->IsInAir())
			return;
		auto const mission = pInf->GetCurrentMission();
		auto const pTarget = FightTarget(pInf);
		int const depIndex = DeployedIndex(pInf->Type);
		auto const pDep = WeaponOf(pInf, depIndex), pUp = WeaponOf(pInf, 0);
		bool const antiAir = HitsAir(pDep) && !HitsAir(pUp);
		if (!Settled(cfg, pInf, now))
			return;

		if (!IsDeployed(seq))
		{
			if (!(cfg.DeployMissionMask & Bit(mission)))
				return;
			if (cfg.DeployInRange && pTarget)
			{
				double const dep = Dps(pDep, pTarget), up = Dps(pUp, pTarget);
				if (dep > 0.0 && dep >= up * cfg.DeployRatio && pInf->IsCloseEnough(pTarget, depIndex))
				{
					DeployNow(pInf, now);
					++g_counts.InRange;
					Log(cfg, pInf, "deploys, in range of", pTarget, dep, up);
					return;
				}
			}
			// Aircraft only the deployed weapon hits; an Attack order's own target comes first. A unit that stays
			// put (guarding) deploys AntiAir.Reach cells early. A hunting one deploys for an aircraft in range
			// and takes it as its target: deployed with a target out of range, the engine stands it up.
			if (cfg.DeployAntiAir && antiAir && (mission != Mission::Attack || (pTarget && pTarget->IsInAir())))
			{
				bool const chases = mission == Mission::Hunt || mission == Mission::Attack;
				if (auto const pAir = AirInReach(pInf, depIndex, pDep, chases ? 0 : cfg.DeployAntiAirReach, now))
				{
					DeployNow(pInf, now);
					if (chases && pTarget != pAir)
						pInf->SetTarget(pAir);
					++g_counts.Air;
					Log(cfg, pInf, "deploys for aircraft", pAir, Dps(pDep, pAir), 0.0);
				}
			}
			return;
		}

		// Deployed: stand up for a target the undeployed weapon does more to, unless aircraft need the
		// deployed one. Hunt and Attack only (a guard would deploy it again: the engine's AIAutoDeployFrameDelay).
		if (!cfg.DeployUndeploy || !pTarget || (mission != Mission::Hunt && mission != Mission::Attack))
			return;
		double const dep = Dps(pDep, pTarget), up = Dps(pUp, pTarget);
		if (up <= 0.0 || up < dep * cfg.DeployUndeployRatio)
			return;
		if (antiAir && AirInReach(pInf, depIndex, pDep, cfg.DeployAntiAirReach, now))
			return;
		pInf->PlayAnim(Sequence::Undeploy, false, false);
		g_changed[pInf->UniqueID] = now;
		++g_counts.StoodUp;
		Log(cfg, pInf, "stands up to fight", pTarget, dep, up);
	}

	// ApproachTarget, undeployed (0x522373): deploy here if the target is in the deployed weapon's range and
	// that weapon does more to it. True sends the engine to its own AI deploy check, which deploys.
	bool DeployOnApproach(InfantryClass* const pInf, bool const approachOnly)
	{
		auto const pCfg = ConfigFor(pInf);
		if (!pCfg || !pCfg->DeployApproach || approachOnly)
			return false;
		auto const pTarget = FightTarget(pInf);
		int const now = Unsorted::CurrentFrame;
		if (!pTarget || !Settled(*pCfg, pInf, now))
			return false;
		int const depIndex = DeployedIndex(pInf->Type);
		auto const pDep = WeaponOf(pInf, depIndex);
		double const dep = Dps(pDep, pTarget), up = Dps(WeaponOf(pInf, 0), pTarget);
		if (dep <= 0.0 || dep < up * pCfg->DeployRatio || !pInf->IsCloseEnough(pTarget, depIndex))
			return false;
		g_changed[pInf->UniqueID] = now;
		++g_counts.Approach;
		Log(*pCfg, pInf, "deploys, closing on", pTarget, dep, up);
		return true;
	}

	enum class HuntIdle { StandUp, Hunt, Wait };

	// Mission_Hunt, deployed with no target (0x51F5E9): take the next enemy in range rather than stand up, unless
	// the undeployed weapon does more to it; with none in range, wait deployed while enemy aircraft that only the
	// deployed weapon hits are within AntiAir.Reach.
	HuntIdle HoldDeployed(InfantryClass* const pInf, int& wait)
	{
		auto const pCfg = ConfigFor(pInf);
		if (!pCfg || !pCfg->DeployHold)
			return HuntIdle::StandUp;
		auto coords = pInf->GetCoords();
		pInf->TargetAndEstimateDamage(coords, ThreatType::Range);
		auto const pTarget = FightTarget(pInf);
		int const depIndex = DeployedIndex(pInf->Type);
		auto const pDep = WeaponOf(pInf, depIndex), pUp = WeaponOf(pInf, 0);
		if (!pTarget)
		{
			if (pCfg->DeployAntiAir && HitsAir(pDep) && !HitsAir(pUp)
				&& AirInReach(pInf, depIndex, pDep, pCfg->DeployAntiAirReach, Unsorted::CurrentFrame))
			{
				++g_counts.AirWait;
				wait = std::max(1, pCfg->DeployInterval);
				return HuntIdle::Wait;
			}
			++g_counts.LetStand;
			return HuntIdle::StandUp;
		}
		double const dep = Dps(pDep, pTarget), up = Dps(pUp, pTarget);
		if (pCfg->DeployUndeploy && up > 0.0 && up >= dep * pCfg->DeployUndeployRatio)
		{
			++g_counts.StoodUp;
			Log(*pCfg, pInf, "stands up for its next target", pTarget, dep, up);
			return HuntIdle::StandUp;
		}
		++g_counts.Held;
		Log(*pCfg, pInf, "stays deployed for its next target", pTarget, dep, up);
		return HuntIdle::Hunt;
	}
}

void Deploy::Sweep()
{
	int const now = Unsorted::CurrentFrame;
	if (now == g_lastSweep)
		return;
	g_lastSweep = now;

	for (int i = 0; i < InfantryClass::Array.Count; ++i)
	{
		auto const pInf = InfantryClass::Array.GetItem(i);
		if (!pInf || pInf->InLimbo || !pInf->IsAlive || pInf->Health <= 0)
			continue;
		auto const pOwner = pInf->Owner;
		if (!pOwner || pOwner->Defeated || pOwner->IsNeutral() || pOwner->IsControlledByHuman())
			continue;
		auto const& cfg = DoctrineConfig::For(pOwner);
		if (!cfg.DeployEnable)
			continue;
		unsigned const every = static_cast<unsigned>(std::max(1, cfg.DeployInterval));
		if ((static_cast<unsigned>(now) + pInf->UniqueID) % every != 0 || !Manages(cfg, pInf))
			continue;
		Look(cfg, pInf, now);
	}

	if (now % 1800 == 0)
	{
		auto const& base = DoctrineConfig::Instance;
		bool const debug = base.DeployDebug || DoctrineConfig::ByDifficulty[0].DeployDebug
			|| DoctrineConfig::ByDifficulty[1].DeployDebug || DoctrineConfig::ByDifficulty[2].DeployDebug;
		if (debug)
			Debug::Log("[DoctrineExt] deploy: last 1800 frames: deployed closing on %d, in range %d, for aircraft %d; "
				"stood up %d; hunting deployed kept %d, waited for aircraft %d, let stand %d\n", g_counts.Approach,
				g_counts.InRange, g_counts.Air, g_counts.StoodUp, g_counts.Held, g_counts.AirWait, g_counts.LetStand);
		g_counts = Counts{};
		for (auto it = g_changed.begin(); it != g_changed.end(); )
			it = now - it->second > 3600 ? g_changed.erase(it) : std::next(it);
	}
}

void Deploy::Reset()
{
	g_tables.clear();
	g_changed.clear();
	g_air.clear();
	g_airFrame = -1;
	g_lastSweep = -1;
	g_warned.clear();
	g_counts = Counts{};
}

// InfantryClass::ApproachTarget, undeployed, just after its FootClass::ApproachTarget call: the vanilla
// `pop esi / pop ebx / ret 4` here returns before the AI deploy check at 0x522378. Phobos hooks the same address
// (InfantryClass_ApproachTarget_InfantryAutoDeploy; same-address hooks chain). ESI = this, [ESP+0xC] = the
// ApproachTarget argument the deploy check wants false.
DEFINE_HOOK(0x522373, DoctrineExt_InfantryClass_ApproachTarget_Deploy, 0x5)
{
	enum { DeployCheck = 0x522378 };
	GET(InfantryClass* const, pThis, ESI);
	GET_STACK(bool const, approachOnly, 0xC);
	return DeployOnApproach(pThis, approachOnly) ? DeployCheck : 0;
}

// InfantryClass::Mission_Hunt, an AI's deployed infantry (0x51F5E9: mov eax,[esi+0x2B4]; test; jne): with no
// target the engine stands it up at once (0x51F5F3), even with the next enemy in range. Every way returns an
// address, so the stolen bytes (and their short jne) never run. Wait returns from Mission_Hunt (0x51F608:
// pop esi; ret) with EAX = frames until the next call ([Doctrine.Deploy] Interval; the stand-up exit returns 1).
DEFINE_HOOK(0x51F5E9, DoctrineExt_InfantryClass_MissionHunt_HoldDeployed, 0xA)
{
	enum { Hunt = 0x51F60A, StandUp = 0x51F5F3, Return = 0x51F608 };
	GET(InfantryClass* const, pThis, ESI);
	if (pThis->Target)
		return Hunt;
	int wait = 1;
	switch (HoldDeployed(pThis, wait))
	{
	case HuntIdle::Hunt:
		return Hunt;
	case HuntIdle::Wait:
		R->EAX(wait);
		return Return;
	default:
		return StandUp;
	}
}
