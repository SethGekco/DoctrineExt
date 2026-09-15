#include "Doctrine/ArsenalGrader.h"

#include <TechnoTypeClass.h>
#include <InfantryTypeClass.h>
#include <UnitTypeClass.h>
#include <AircraftTypeClass.h>
#include <WeaponTypeClass.h>
#include <WarheadTypeClass.h>
#include <BulletTypeClass.h>
#include <Utilities/Debug.h>

#include <algorithm>
#include <map>
#include <vector>

namespace
{
	// Armor class indices into WarheadTypeClass::Verses[11].
	enum { ARM_NONE = 0, ARM_FLAK = 1, ARM_PLATE = 2, ARM_LIGHT = 3, ARM_MEDIUM = 4,
	       ARM_HEAVY = 5, ARM_WOOD = 6, ARM_STEEL = 7, ARM_CONCRETE = 8 };

	// Best armor-adjusted DPS the type's weapons deal to a target of the given
	// armor and domain (air vs ground). 0 if it cannot hit that domain.
	double EffDPS(TechnoTypeClass* const pT, int const armor, bool const air)
	{
		double best = 0.0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pT->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType) continue;
			auto const w = pWS->WeaponType;
			if (w->ROF <= 0 || w->Damage <= 1 || !w->Warhead || !w->Projectile) continue;
			bool const canHit = air ? w->Projectile->AA : w->Projectile->AG;
			if (!canHit) continue;
			int const burst = w->Burst > 0 ? w->Burst : 1;
			double dps = double(w->Damage) * burst / (w->ROF / 10.0);
			if (armor >= 0 && armor < 11)
				dps *= w->Warhead->Verses[armor];
			if (dps > best) best = dps;
		}
		return best;
	}

	int RangeDomain(TechnoTypeClass* const pT, bool const air)
	{
		int best = 0;
		for (int wi = 0; wi < 2; ++wi)
		{
			auto const pWS = pT->GetWeapon(wi);
			if (!pWS || !pWS->WeaponType || !pWS->WeaponType->Projectile) continue;
			auto const w = pWS->WeaponType;
			bool const canHit = air ? w->Projectile->AA : w->Projectile->AG;
			if (canHit && w->Range > best) best = w->Range;
		}
		return best;
	}

	std::map<std::string, DoctrineArsenalRole> g_roles;
	bool g_built = false;

	// Score one type for one role; 0 = ineligible (can't perform the role).
	double ScoreFor(const std::string& role, TechnoTypeClass* const pT)
	{
		double const offInf = EffDPS(pT, ARM_NONE, false);
		double const offVeh = std::max(EffDPS(pT, ARM_MEDIUM, false), EffDPS(pT, ARM_HEAVY, false));
		double const offAir = EffDPS(pT, ARM_LIGHT, true);
		double const offBld = std::max(EffDPS(pT, ARM_CONCRETE, false), EffDPS(pT, ARM_STEEL, false));
		int const rngG = RangeDomain(pT, false);
		int const speed = pT->Speed;

		if (role == "AntiAir")      return offAir > 0 ? offAir + RangeDomain(pT, true) * 0.1 : 0.0;
		if (role == "AntiArmor")    return offVeh > 0 ? offVeh + rngG * 0.1 : 0.0;
		if (role == "AntiInfantry") return offInf > 0 ? offInf + speed * 0.5 : 0.0;
		if (role == "Siege")        return offBld > 0 ? offBld * (1.0 + rngG / 512.0) : 0.0;
		if (role == "AceHunter")
		{
			double const dmg = std::max(offInf, offVeh);
			return dmg > 0 ? dmg * (1.0 + speed / 100.0) : 0.0;
		}
		if (role == "Scout")        return (offInf + offVeh + offAir) > 0 ? double(speed) : 0.0;
		return 0.0;
	}

	void Build()
	{
		g_built = true;
		static const char* const kRoles[] = {
			"AntiAir", "AntiArmor", "AntiInfantry", "Siege", "AceHunter", "Scout" };

		int const depth = DoctrineConfig::Instance.AutoArsenalDepth > 0
			? DoctrineConfig::Instance.AutoArsenalDepth : 6;

		// One pooled list of all combat unit types (skip miners / disabled).
		std::vector<TechnoTypeClass*> pool;
		auto add = [&](TechnoTypeClass* const pT)
		{
			if (!pT || pT->ResourceGatherer || pT->TechLevel < 0) return;
			pool.push_back(pT);
		};
		for (auto const pT : InfantryTypeClass::Array) add(pT);
		for (auto const pT : UnitTypeClass::Array)     add(pT);
		for (auto const pT : AircraftTypeClass::Array) add(pT);

		for (auto const role : kRoles)
		{
			std::vector<std::pair<double, TechnoTypeClass*>> scored;
			for (auto const pT : pool)
			{
				double const s = ScoreFor(role, pT);
				if (s > 0.0) scored.emplace_back(s, pT);
			}
			std::stable_sort(scored.begin(), scored.end(),
				[](auto const& a, auto const& b) { return a.first > b.first; });

			DoctrineArsenalRole r;
			r.Role = role;
			for (int i = 0; i < static_cast<int>(scored.size()) && i < depth; ++i)
				r.Units.push_back(scored[i].second->ID);
			g_roles[role] = std::move(r);

			std::string ids;
			for (auto const& id : g_roles[role].Units) { if (!ids.empty()) ids += ","; ids += id; }
			Debug::Log("[DoctrineExt] auto-arsenal %s (%u): %s\n",
				role, g_roles[role].Units.size(), ids.c_str());
		}
	}
}

const DoctrineArsenalRole* ArsenalGrader::RoleUnits(const std::string& role)
{
	if (!g_built)
		Build();
	auto const it = g_roles.find(role);
	return (it != g_roles.end() && !it->second.Units.empty()) ? &it->second : nullptr;
}

void ArsenalGrader::Reset()
{
	g_roles.clear();
	g_built = false;
}
