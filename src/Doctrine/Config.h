#pragma once

#include <string>
#include <vector>

// The parsed doctrine INI surface (DESIGN.md §3/§4). Phase 0: parse + echo
// only — nothing is evaluated yet. All data lives here so later phases (rule
// engine, arsenal pick) read one struct instead of the INI.

// One [Doctrine.Rules] entry, e.g.
//   [CounterAir]
//   When=EnemyAirDPS>200
//   Respond=AntiAir
//   Scale=1.5
//   Mission=DefendBase
//   Cooldown=900
struct DoctrineRule
{
	std::string Name;

	// When=, split into observation / operator / threshold at parse time.
	// Op is one of > >= < <= = ; empty Obs means the rule failed to parse
	// and is inert (logged, never fired).
	std::string WhenRaw;
	std::string WhenObs;
	std::string WhenOp;
	double WhenValue = 0.0;

	std::string Respond;   // arsenal role name
	std::string Mission;   // canned mission id (DefendBase/HuntTarget/...)
	std::string Target;    // empty or "ThatUnit"
	double Scale = 1.0;
	int Cooldown = 0;      // frames; 0 = no cooldown
	int Priority = 0;      // higher wins slot contention + can preempt a
	                       // live lower-priority team (urgent > passive guard)
	int Period = 0;        // frames between this rule's evaluations; 0 = use
	                       // the global RulePeriod (e.g. air raids poll faster)
};

// One [Doctrine.Arsenal] line: role -> unit IDs, best first. Phase 0 keeps
// raw IDs; resolution to TechnoTypeClass* happens in phase 1 (after all type
// arrays exist).
struct DoctrineArsenalRole
{
	std::string Role;
	std::vector<std::string> Units;
};

class DoctrineConfig
{
public:
	// [Doctrine.General]
	int SenseInterval = 15;  // frames between base ticks (~1s): steering +
	                         // rule-due checks run this often (cheap work)
	int RulePeriod = 150;    // default frames between a rule's evaluations
	                         // (~10s); a rule's Period= overrides this
	int MaxTeamSize = 12;    // hard clamp on any procedural team
	int MaxTeamCost = 10000; // hard clamp on any procedural team's cost
	bool DebugTicks = false; // log every rule evaluation (verbose)
	int TeamTTL = 3600;      // frames before a stuck doctrine team is disbanded
	int TeamsPerHouse = 2;   // concurrent doctrine teams per AI house
	bool StrictOwnership = true; // re-check Owner= on arsenal picks (CanBuild
	                             // proved cross-faction-leaky in this stack)
	int AirAlertRadius = 40;     // cells: EnemyAirIncoming's alarm bubble
	int InterceptStandoff = 12;  // cells from base toward the raider where
	                             // interceptors form their screen (path block)
	bool AceMobileOnly = true;   // EnemyUnitKills targets only mobile units,
	                             // not base-defense buildings racking up kills
	bool AutoProduce = true;     // top up under-strength teams by demanding
	                             // production of the arsenal unit (hybrid)
	int MaxProducePerDispatch = 2; // cap on units queued per dispatch, so
	                             // doctrine never floods the AI's economy

	std::vector<DoctrineArsenalRole> Arsenal;
	std::vector<DoctrineRule> Rules;

	bool Parsed = false;

	// Parse [Doctrine.*] from the cached rules INI if not done yet, echoing
	// every value to debug.log. Safe to call any time; no-op until
	// CCINIClass::INI_Rules exists.
	static void EnsureParsed();

	// Forget parsed state (scenario change — game-mode INIs merge into the
	// rules INI, so each scenario re-parses).
	static void Reset();

	static DoctrineConfig Instance;
};
