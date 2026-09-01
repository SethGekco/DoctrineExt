#include "Doctrine/Engine.h"
#include "Doctrine/Config.h"
#include "Doctrine/Observations.h"
#include "Doctrine/Teams.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <Fundamentals.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <map>
#include <set>

namespace
{
	// (houseIndex, ruleIndex) -> frame the rule may fire again.
	std::map<std::pair<int, int>, int> g_cooldownUntil;
	// houseIndex -> last frame we ticked. The game re-runs HouseClass::Update
	// with a frozen frame counter while paused, which turned one tick into a
	// burst of identical ones.
	std::map<int, int> g_lastTickFrame;
	std::set<std::string> g_unknownObsWarned;

	bool Compare(double const value, const std::string& op, double const threshold)
	{
		if (op == ">")  return value > threshold;
		if (op == ">=") return value >= threshold;
		if (op == "<")  return value < threshold;
		if (op == "<=") return value <= threshold;
		if (op == "=")  return value == threshold;
		return false;
	}
}

void Engine::TickHouse(HouseClass* pHouse)
{
	auto& cfg = DoctrineConfig::Instance;
	if (!cfg.Parsed)
		DoctrineConfig::EnsureParsed();
	if (cfg.Rules.empty())
		return;

	if (!pHouse || pHouse->Defeated || pHouse->IsControlledByHuman()
		|| pHouse->IsObserver() || pHouse->IsNeutral())
		return;

	// Stagger: each house gets its own evaluation frame inside the period,
	// deterministically (sync-safe — a pure function of synced state).
	int const period = cfg.RulePeriod > 0 ? cfg.RulePeriod : 150;
	int const frame = Unsorted::CurrentFrame;
	if ((frame + pHouse->ArrayIndex * 7) % period != 0)
		return;

	auto const lastIt = g_lastTickFrame.find(pHouse->ArrayIndex);
	if (lastIt != g_lastTickFrame.end() && lastIt->second == frame)
		return;
	g_lastTickFrame[pHouse->ArrayIndex] = frame;

	for (size_t ri = 0; ri < cfg.Rules.size(); ++ri)
	{
		auto const& rule = cfg.Rules[ri];
		if (rule.WhenObs.empty())
			continue; // inert (unparseable When=, warned at parse time)

		auto const key = std::make_pair(pHouse->ArrayIndex, static_cast<int>(ri));
		auto const it = g_cooldownUntil.find(key);
		if (it != g_cooldownUntil.end() && frame < it->second)
			continue;

		double value = 0.0;
		TechnoClass* pTarget = nullptr;
		if (!Observations::Get(pHouse, rule.WhenObs, value, &pTarget))
		{
			if (g_unknownObsWarned.insert(rule.WhenObs).second)
				Debug::Log("[DoctrineExt] WARNING: rule %s uses unknown observation "
					"%s, rule is inert.\n", rule.Name.c_str(), rule.WhenObs.c_str());
			continue;
		}

		if (cfg.DebugTicks)
			Debug::Log("[DoctrineExt] tick house=%s#%d rule=%s %s=%.1f (want %s%.1f)\n",
				pHouse->get_ID(), pHouse->ArrayIndex, rule.Name.c_str(),
				rule.WhenObs.c_str(), value, rule.WhenOp.c_str(), rule.WhenValue);

		if (!Compare(value, rule.WhenOp, rule.WhenValue))
			continue;

		if (Teams::Dispatch(pHouse, rule, value, pTarget))
		{
			// Cooldown=0 still waits one period, or a satisfied condition
			// would re-dispatch every tick until the threat clears.
			int const wait = rule.Cooldown > 0 ? rule.Cooldown : period;
			g_cooldownUntil[key] = frame + wait;
		}
		else
		{
			// A failed dispatch (no slot, nothing buildable) retries after
			// one period instead of spamming every tick.
			g_cooldownUntil[key] = frame + period;
		}
	}
}

void Engine::Reset()
{
	g_cooldownUntil.clear();
	g_lastTickFrame.clear();
	g_unknownObsWarned.clear();
}

// HouseClass::Update, at the same entry point Ares/Antares hook for their
// per-house per-frame work (same-address hooks chain legally). ECX = this.
DEFINE_HOOK(0x4F8440, DoctrineExt_HouseClass_Update_SenseTick, 0x5)
{
	GET(HouseClass* const, pThis, ECX);
	Engine::TickHouse(pThis);
	return 0;
}
