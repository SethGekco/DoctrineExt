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
	bool DefendPerimeter = true; // DefendBase teams hold the base outer edge
	                             // (learned from buildings) not the centre
	int BaseEdgeMargin = 2;      // cells beyond the outermost building for the
	                             // defenders' perimeter ring
	// Travel-lane heatmap (§6.3)
	int LaneBucket = 6;          // cells per heatmap bucket
	int LaneSampleInterval = 60; // frames between movement samples
	int LaneDecayShift = 4;      // each sample a bucket loses 1/(2^shift)
	int LaneBumpWeight = 16;     // per-sample increment for a moving enemy unit
	int LaneRadius = 30;         // cells around the base considered for lanes
	int LaneMinStrength = 32;    // min bucket value to trust a learned lane
	// Death-zone heatmap (§6.2)
	int DeathZoneBucket = 6;         // cells per bucket
	int DeathZoneDecayInterval = 150;// frames between decays (deaths are rarer)
	int DeathZoneDecayShift = 3;     // each decay a bucket loses 1/(2^shift)
	int DeathZoneBumpWeight = 64;    // per-death increment (a strong signal)
	int DeathZoneRadius = 30;        // cells around the base considered
	int DeathZoneMinStrength = 48;   // min bucket value to trust a killbox
	// Weak-point approach (§6c)
	double MinCounterScore = 1.0;    // below this, decline a HuntTarget (don't
	                                 // feed the farm) — 1.0 = must at least trade
	int SupportScanRadius = 8;       // cells around a target scanned for its
	                                 // supporting units (defines its weak side)
	int HuntStandoff = 5;            // cells from the target the hunt force
	                                 // stages on its weak side
	// Base-flood relief (§10d C)
	int FloodThreshold = 0;          // idle armed units before relief fires
	                                 // (0 = feature off)
	double FloodHuntFraction = 0.5;  // fraction of the hoard sent to Hunt
	int FloodCooldown = 900;         // frames between flood alerts per house
	std::vector<std::string> FloodExclude; // unit IDs never sent by flood relief
	std::vector<std::string> FloodInclude; // unit IDs always eligible (override)
	// Reserve spender ([Doctrine.Reserve], §10f first cut) — spend surplus cash
	int ReserveAmount = 0;           // build from the list while money > this
	                                 // (0 = feature off); generalises InfantryReserve
	int ReserveCooldown = 300;       // frames between reserve builds per house
	int ReserveGrowth = 0;           // if net cash rose >= this over GrowthWindow
	                                 // (accumulating faster than spending), build
	                                 // — spend to keep up on rich maps (0 = off)
	int ReserveGrowthWindow = 1800;  // frames the growth is measured over
	std::vector<std::string> ReserveBuild; // ordered unit IDs to build w/ surplus
	// Force-comparison rush ([Doctrine.General], §10g)
	double RushRatio = 0.0;          // allied power must be >= target x this to
	                                 // rush (0 = feature off)
	double RushSafety = 1.0;         // AND allied power >= total enemy x this
	                                 // (don't overextend in multi-way games)
	int RushCooldown = 1800;         // frames between all-in rushes per house
	int RushMinUnits = 6;            // need at least this many idle armed units
	                                 // to commit (a rush, not a lone scout)
	double RushMinPower = 400.0;     // need at least this much allied power — so
	                                 // an enemy at 0 power doesn't trivially arm it
	// Tech-tree decapitation ([Doctrine.Decap], §10h) — priority target roles.
	// A rush aims at the highest-scored enemy building; score = weight / how many
	// of that role the enemy has (fewer providers = a cheaper COMPLETE cut, so a
	// lone war factory outranks three service depots that jointly gate the MCV).
	bool DecapEnable = true;
	int DecapConYard = 100;          // construction yard — kills rebuild ability
	int DecapRefinery = 60;          // economy
	int DecapVehicle = 50;           // war factory
	int DecapAircraft = 40;          // helipad / air
	int DecapInfantry = 30;          // barracks
	int DecapDefense = 10;           // base defenses
	int DecapOther = 5;              // everything else
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
