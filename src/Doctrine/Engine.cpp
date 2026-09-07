#include "Doctrine/Engine.h"
#include "Doctrine/Config.h"
#include "Doctrine/Observations.h"
#include "Doctrine/Teams.h"
#include "Doctrine/LaneTracker.h"
#include "Doctrine/DeathZones.h"

#include <HouseClass.h>
#include <TechnoClass.h>
#include <Fundamentals.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace
{
	// (houseIndex, ruleIndex) -> frame the rule may fire again.
	std::map<std::pair<int, int>, int> g_cooldownUntil;
	// (houseIndex, ruleIndex) -> frame the rule was last evaluated, so each
	// rule checks at its own Period even though the base tick is fast.
	std::map<std::pair<int, int>, int> g_lastRuleEval;
	// houseIndex -> last frame we ticked. The game re-runs HouseClass::Update
	// with a frozen frame counter while paused, which turned one tick into a
	// burst of identical ones.
	std::map<int, int> g_lastTickFrame;
	std::set<std::string> g_unknownObsWarned;
	// Houses already logged in the human/AI census (once each per game).
	std::set<int> g_censusLogged;

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

	if (!pHouse)
		return;

	// One-time census of every house's human/AI status, so we can prove which
	// houses DoctrineExt will act on vs skip — the human player must never be
	// acted upon. IsControlledByHuman()/IsHumanPlayer are the sync-safe checks;
	// IsCurrentPlayer() is per-machine (logged for context only, never used for
	// logic — it would desync).
	if (cfg.DebugTicks && g_censusLogged.insert(pHouse->ArrayIndex).second)
		Debug::Log("[DoctrineExt] census house=%s#%d human=%d isHumanPlayer=%d "
			"isCurrentPlayer=%d defeated=%d observer=%d neutral=%d\n",
			pHouse->get_ID(), pHouse->ArrayIndex,
			pHouse->IsControlledByHuman(), pHouse->IsHumanPlayer,
			pHouse->IsCurrentPlayer(), pHouse->Defeated,
			pHouse->IsObserver(), pHouse->IsNeutral());

	if (pHouse->Defeated || pHouse->IsControlledByHuman()
		|| pHouse->IsObserver() || pHouse->IsNeutral())
		return;

	// Base tick: fast and cheap (steering + due-checks), staggered per house,
	// deterministically (sync-safe — a pure function of synced state).
	int const sense = cfg.SenseInterval > 0 ? cfg.SenseInterval : 15;
	int const defPeriod = cfg.RulePeriod > 0 ? cfg.RulePeriod : 150;
	int const frame = Unsorted::CurrentFrame;
	if ((frame + pHouse->ArrayIndex * 7) % sense != 0)
		return;

	auto const lastIt = g_lastTickFrame.find(pHouse->ArrayIndex);
	if (lastIt != g_lastTickFrame.end() && lastIt->second == frame)
		return;
	g_lastTickFrame[pHouse->ArrayIndex] = frame;

	// Learn the map's movement lanes (self-throttled). Cheap and pointer-safe.
	LaneTracker::Sample(frame);
	// Fade the death-zone heatmap (self-throttled; deaths feed it via the hook).
	DeathZones::Decay(frame);

	// Steering pass (every base tick, so pursuit tracks the raid closely): keep
	// live intercept teams pointed at the current nearest raider. Skipped for
	// houses without an active intercept so the raider scan isn't paid for.
	if (Teams::HasActiveIntercept(pHouse))
	{
		double v = 0.0;
		TechnoClass* pRaider = nullptr;
		Observations::Get(pHouse, "EnemyAirIncoming", v, &pRaider);
		Teams::SteerIntercepts(pHouse, pRaider);
	}

	// Hold the base perimeter (learned outer edge), not the centre.
	if (Teams::HasActiveDefend(pHouse))
		Teams::SteerDefenders(pHouse);

	// Hunt teams approach the current top ace from its weak side (6c).
	if (Teams::HasActiveHunt(pHouse))
	{
		double v = 0.0;
		TechnoClass* pAce = nullptr;
		Observations::Get(pHouse, "EnemyUnitKills", v, &pAce);
		Teams::SteerHunters(pHouse, pAce);
	}

	// Team-fill trace (~every 5s under DebugTicks): shows produced units
	// joining live doctrine teams via engine-side recruiting over time.
	if (cfg.DebugTicks && (frame % (sense * 5)) < sense)
		Teams::LogTeamFill(pHouse);

	// Lane-heatmap trace (~every 10s under DebugTicks): shows the learned
	// hottest enemy-approach bearing/strength near this house's base.
	if (cfg.DebugTicks && (frame % (sense * 10)) < sense)
	{
		double a = 0.0; int s = 0;
		if (LaneTracker::HottestLaneBearing(pHouse, a, s))
			Debug::Log("[DoctrineExt] lane house=%s#%d hottest bearing=%.2frad strength=%d\n",
				pHouse->get_ID(), pHouse->ArrayIndex, a, s);
		double da = 0.0; int ds = 0;
		if (DeathZones::HottestBearing(pHouse, da, ds))
			Debug::Log("[DoctrineExt] deathzone house=%s#%d hottest bearing=%.2frad strength=%d\n",
				pHouse->get_ID(), pHouse->ArrayIndex, da, ds);
	}

	// Evaluate high-priority rules first so an urgent aggressive rule claims
	// (and can preempt for) a team slot before passive rules fill them. Stable
	// order within equal priority keeps behaviour predictable.
	std::vector<size_t> order(cfg.Rules.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
		return cfg.Rules[a].Priority > cfg.Rules[b].Priority;
	});

	for (size_t oi = 0; oi < order.size(); ++oi)
	{
		size_t const ri = order[oi];
		auto const& rule = cfg.Rules[ri];
		if (rule.WhenObs.empty())
			continue; // inert (unparseable When=, warned at parse time)

		auto const key = std::make_pair(pHouse->ArrayIndex, static_cast<int>(ri));

		// Per-rule evaluation cadence: a rule only checks every its Period
		// (default RulePeriod), so fast air polls often while expensive rules
		// stay slow even though the base tick is frequent.
		int const rulePeriod = rule.Period > 0 ? rule.Period : defPeriod;
		auto const evIt = g_lastRuleEval.find(key);
		if (evIt != g_lastRuleEval.end() && frame - evIt->second < rulePeriod)
			continue;
		g_lastRuleEval[key] = frame;

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
			int const wait = rule.Cooldown > 0 ? rule.Cooldown : rulePeriod;
			g_cooldownUntil[key] = frame + wait;
		}
		else
		{
			// A failed dispatch (no slot, nothing buildable) retries after
			// one period instead of spamming every tick.
			g_cooldownUntil[key] = frame + rulePeriod;
		}
	}
}

void Engine::Reset()
{
	g_cooldownUntil.clear();
	g_lastRuleEval.clear();
	g_lastTickFrame.clear();
	g_unknownObsWarned.clear();
	g_censusLogged.clear();
}

// HouseClass::Update, at the same entry point Ares/Antares hook for their
// per-house per-frame work (same-address hooks chain legally). ECX = this.
DEFINE_HOOK(0x4F8440, DoctrineExt_HouseClass_Update_SenseTick, 0x5)
{
	GET(HouseClass* const, pThis, ECX);
	Engine::TickHouse(pThis);
	return 0;
}
