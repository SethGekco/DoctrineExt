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
	// Forced budget / priority buildings ([Doctrine.Budget]). Fixes an AI that can't
	// afford a pricey essential (e.g. a FreeUnit-inflated refinery) so it sells its base
	// to pay for it — AND the follow-on poverty trap where it stays at one refinery,
	// perpetually broke, under-defended. Each tier is a list of building IDs, highest
	// priority first; the AI focuses on the highest-priority building it hasn't reached
	// Count of yet and can build. Two modes, so economy scales WITHOUT going defenceless:
	//  · Count==1 (a one-off essential): once cash reaches the tier's Reserve, seize
	//    offensive production + block repair to rush it, then release.
	//  · Count>1 (economy scaling, e.g. 3 refineries): while below Count, continuously
	//    hold OFFENSIVE production (vehicles/aircraft/navy) so income funnels into the
	//    buildings — but leave INFANTRY and the ConYard free, so a defensive screen and
	//    base defenses still go up (repair stays on). Released once Count is reached.
	struct BudgetTier { std::vector<std::string> Buildings; int Reserve = 0; int Count = 1; };
	std::vector<BudgetTier> BudgetTiers;
	bool BudgetEnable = true;        // active only when BudgetTiers is non-empty
	int BudgetInterval = 15;         // frames between budget passes per house
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
	// Commander takeover ([Doctrine.Commander]) — if the AI sits idle (army home,
	// not attacking) for CommanderIdleTime frames, DoctrineExt takes command:
	// commandeers the idle army (diverting units off their sitting aimd teams) and
	// drives it at the enemy. Fixes a base AI that hoards but never attacks.
	int CommanderIdleTime = 0;       // frames of AI idleness before takeover
	                                 // (0 = feature OFF). e.g. 3600 = ~4 min
	int CommanderMinArmy = 8;        // need at least this many armed units owned
	                                 // before it's worth taking command
	int CommanderFront = 25;         // cells from base beyond which a unit counts
	                                 // as "committed/attacking" (else "home")
	double CommanderCommitFrac = 0.34; // if >= this fraction of the army is
	                                 // committed, the AI is attacking on its own —
	                                 // don't take over (resets the idle timer)
	int CommanderBatch = 16;         // max units DoctrineExt commandeers into the
	                                 // assault (0 = uncapped — the whole home army)
	int CommanderPeriod = 150;       // frames between takeover evaluations
	// Air-defense doctrine ([Doctrine.AirDefense], AA P1) — keep AA scaled to the
	// enemy's air. Opt-in (Enable=no).
	bool AirDefenseEnable = false;
	double AirThreatRatio = 1.5;     // own AA firepower target = enemy air DPS x this
	int AirDefensePeriod = 150;      // frames between air-defense evaluations
	int AAMaxProduce = 2;            // AntiAir units queued per pass when short
	int AirDefenseStandoff = 12;     // cells forward of base the AA screen forms
	int AirScreenRadius = 30;        // cells around base scanned for incoming air
	double AirDeathFloor = 200.0;    // baseline AA firepower kept once air has
	                                 // repeatedly hit near base (AA P1c), even
	                                 // between raids (0 = no floor)
	// Naval doctrine ([Doctrine.Naval]) — the base AI rarely builds a Naval Yard
	// (placement is water-adjacent and its planner fumbles it), so it never fields
	// navy. When there's water near base and the naval prereqs are met, force the
	// yard then build navy to a target. Also feeds the water crate squad. Opt-in.
	bool NavalEnable = false;
	int NavalInterval = 300;         // frames between naval passes per house
	int NavalScanRadius = 30;        // cells around base scanned for water
	int NavalMinWater = 20;          // min water cells near base to bother building
	int NavyTarget = 6;              // desired naval combat units once a yard exists
	int NavyMaxProduce = 2;          // naval units queued per pass
	// Greedy base-expansion / economy doctrine ([Doctrine.Expansion]) — build MORE
	// production where the land allows, and MORE economy where the wealth is
	// (reading WarZoneExt's Wealth zone when present, else current ore). Opt-in.
	bool ExpansionEnable = false;
	int ExpansionInterval = 450;     // frames between expansion passes per house
	int ExpansionRadius = 30;        // cells around base scanned for land + wealth
	int WarFactoryMax = 4;           // greedy cap on total war factories
	int RefineryMax = 6;             // greedy cap on total refineries
	int LandPerFactory = 120;        // buildable land cells per extra war factory
	int WealthPerRefinery = 5000;    // reachable ore value per extra refinery
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
	// Arsenal auto-grader (§10d A)
	bool AutoArsenal = true;         // auto-fill roles not explicitly listed in
	                                 // [Doctrine.Arsenal] by grading the roster
	int AutoArsenalDepth = 6;        // units kept per auto-graded role
	// Crate doctrine ([Doctrine.General], §10i)
	bool CrateChase = false;         // send a fast grabber at nearby crates
	// Preferred grabber IDs (best first), each with an optional per-unit chase
	// radius: "CLEG:0,DRON:100,HTK,FV" — Radius in cells, 0 = map-wide (a chrono
	// grabber can take any crate), omitted = use the squad default. A unit only
	// races a crate within its own radius.
	struct CrateChaserEntry { std::string ID; int Radius = -1; }; // -1 = default
	std::vector<CrateChaserEntry> CrateChasers;
	// Unit IDs NEVER used as crate chasers (e.g. heroes you don't want risked).
	// Everything else fast stays eligible — dogs, Terror Drones, chrono legionnaires
	// are all good grabbers, so only list what you truly want to protect.
	std::vector<std::string> CrateChaseExclude;
	int CrateScanRadius = 30;        // cells around the base scanned for crates
	int CrateInterval = 90;          // frames between crate scans per house
	bool CrateFiresaleMCV = false;   // when stuck with no way to get an MCV and
	                                 // ShortGame is off, firesale then grab a
	                                 // crate for the guaranteed FreeMCV comeback
	int CrateFiresaleDist = 10;      // only firesale once the grabber is within
	                                 // this many cells of the crate (so the base
	                                 // sells just as it arrives, not while it travels)
	// Persistent crate squad ([Doctrine.General], §10i redesign) — a dedicated
	// squad that stands by (spread out) and races for crates. Size auto-derived
	// from [CrateRules] (CrateMinimum/Maximum/Regen), so a busy crate map keeps a
	// bigger squad and CrateMaximum=1 keeps exactly one racer.
	bool CrateSquad = false;         // enable the persistent squad (else old
	                                 // single-grabber CrateChase path)
	int CrateSquadSize = 0;          // 0 = auto from [CrateRules]; >0 = fixed
	int CrateSquadMax = 6;           // hard cap on the auto-derived squad size
	int CrateSquadScan = 60;         // cells around base the squad watches for
	                                 // crates (wider than CrateScanRadius — the
	                                 // squad races across the map, not just home);
	                                 // also scans CrateScanRadius around each member
	// Crate-catcher PRODUCTION ([Doctrine.General]) — DoctrineExt builds dedicated
	// grabbers (from CrateChasers) and MORE of them the better the grabber it can
	// build: a teleport grabber (CLEG) is worth a full squad, a fast unit half, a
	// slow one just one. Opt-in; diverting existing units still fills the rest.
	bool CrateCatcherBuild = false;
	int CrateCatcherMax = 3;         // cap on dedicated catchers built
	int CrateCatcherFastSpeed = 8;   // Speed at/above which a grabber counts "fast"
	int CrateCatcherMaxProduce = 1;  // catchers queued per pass
	int CrateCatcherGiveUp = 15;     // passes a catcher can be force-built without ever
	                                 // appearing before we deem it unbuildable (the
	                                 // leaky CanBuild greenlights units the factory
	                                 // then refuses) and fall to the next-best grabber.
	                                 // Must exceed real build time (taskforce builds
	                                 // aren't instant) so a slow build isn't abandoned.
	// How catchers are acquired. DemandProduction (the old path) only QUEUES on an AI
	// house — the item sits behind the AI's own production and never builds. The only
	// path that actually PRODUCES a unit for the AI is a team taskforce. ForceBuild=yes
	// puts the validated catcher in the squad's taskforce so the AI builds it; =no is
	// diversion-only (the squad still grabs the best grabber the AI already owns).
	bool CrateCatcherForceBuild = false;
	// If the house's fastest eligible unit (owned or buildable) is at least this fast,
	// skip forcing a dedicated catcher — diverting that unit is enough. 0 = always
	// force the dedicated catcher when ForceBuild=yes (never rely on a merely-fast unit).
	int CrateCatcherSkipSpeed = 0;
	// How many chasers an AI house must own before AITriggerTypes stop being cancelled.
	// While below this, DoctrineExt keeps AITriggerTypes disabled so the AI's production
	// bandwidth goes to crate chasers first; once reached, AITriggerTypes are released
	// (latched for the game). 0 = off (don't tie the AITrigger gate to chaser count).
	int CrateChaserGateCount = 0;
	// Whether DoctrineExt makes the AI obey real build prerequisites for the units it
	// drives the AI to PRODUCE. The engine's own CanBuild leaks generic prereqs (TECH)
	// for AI houses, so without this the AI force-built a Chrono Legionnaire with no
	// Battle Lab. HonorPrerequisites is the GLOBAL default; the per-task knobs override
	// it (-1 = inherit the global, 0 = ignore prereqs / let the AI "cheat", 1 = obey).
	//   CrateChaserHonorPrereq — crate-catcher force-build (BestCrateCatcher).
	//   RushHonorPrereq        — rush/attack production (Dispatch).
	bool HonorPrerequisites = true;
	int CrateChaserHonorPrereq = -1;
	int RushHonorPrereq = -1;
	bool HonorPrereqForCrate() const
		{ return CrateChaserHonorPrereq < 0 ? HonorPrerequisites : (CrateChaserHonorPrereq != 0); }
	bool HonorPrereqForRush() const
		{ return RushHonorPrereq < 0 ? HonorPrerequisites : (RushHonorPrereq != 0); }
	int CrateGiveUpPasses = 4;       // detection passes a chaser can make no
	                                 // progress toward a crate before it's deemed
	                                 // unreachable and abandoned (frees the unit)
	int CrateBlacklistTime = 1800;   // frames an unreachable crate is ignored
	                                 // before the squad will try it again
	// Garrison doctrine ([Doctrine.General], §10e)
	bool GarrisonInfantry = false;   // keep building Occupier infantry while
	                                 // vacant occupiable buildings exist, and
	                                 // send idle occupiers to garrison correctly
	int GarrisonInterval = 60;       // frames between garrison passes per house
	int GarrisonMaxProduce = 2;      // occupiers queued per pass
	// Garrison priority + creep ([Doctrine.Garrison], §10e-2)
	int GarrisonRadius = 30;         // max creep radius (cells) — bounds it to the
	                                 // house's turf so production can't run away
	int GarrisonRadiusStart = 10;    // initial radius (holds the base first)
	int GarrisonCreepRate = 300;     // frames per +1 cell the radius grows outward
	int GarrisonScanNear = 6;        // cells checked for ore/tech near a building
	int GarrisonMaxDivert = 4;       // occupiers pulled off AI combat teams per
	                                 // pass for garrison duty (AI teams own ~all
	                                 // infantry, so we must divert, but gently)
	double GPerimeterW = 3.0;        // weight: closer to base = higher priority
	double GCreepW = 1.0;            // weight: farther out = higher (creep bias)
	double GOreW = 2.0;              // weight: building near ore
	double GTechW = 2.0;             // weight: building near a neutral tech structure
	// AITrigger gate ([Doctrine.General]) — DoctrineExt disables ALL AITriggerTypes
	// at scenario start and re-enables them after this many frames, so the AI can
	// do DoctrineExt work (crate chasing, etc.) before the AITrigger load kicks in.
	// 0 = enable instantly (no gate — vanilla timing).
	int AITriggerDelay = 0;
	bool AceMobileOnly = true;   // EnemyUnitKills targets only mobile units,
	                             // not base-defense buildings racking up kills
	bool AutoProduce = true;     // top up under-strength teams by demanding
	                             // production of the arsenal unit (hybrid)
	int MaxProducePerDispatch = 2; // cap on units queued per dispatch, so
	                             // doctrine never floods the AI's economy

	// ─── Capture doctrine ([Doctrine.Capture], §10j) ───────────────────────────
	// Drive the AI to capture neutral/enemy tech buildings (oil derricks etc.) with
	// engineers, NEAREST base-first. Purely ADDITIVE: it builds engineers through the
	// proven taskforce path and steers our own units — it never seizes the AI's own
	// production or force-missions its buildings, so (unlike the forced budget) it
	// can't destabilise the base into sell/relocate.
	bool CaptureEnable = false;      // opt-in master switch
	std::vector<std::string> CaptureTargets; // building IDs to grab (CAOILD, ...);
	                                 // empty = ANY Capturable neutral/enemy building
	int CaptureInterval = 90;        // frames between capture passes per house
	int CaptureMaxTeams = 2;         // concurrent targets (engineers committed at once)
	int CaptureRadius = 0;           // max cells from base to consider (0 = whole map)
	int CaptureEngineerMax = 2;      // engineers queued per pass toward targets
	bool CaptureEnemyOwned = false;  // also capture ENEMY-owned capturables (not just
	                                 // neutral tech) when an engineer can reach them
	int CaptureHonorPrereq = -1;     // -1 inherit HonorPrerequisites, 0 ignore, 1 obey
	bool HonorPrereqForCapture() const
		{ return CaptureHonorPrereq < 0 ? HonorPrerequisites : (CaptureHonorPrereq != 0); }

	// ─── Economy opening ([Doctrine.Economy], §10k) ─────────────────────────────
	// The budgeting Rex asked for, fully tag-driven (all $ values are mod-specific). This
	// modpack's Easy AI has starting cash enough for a refinery but spends it on power +
	// barracks + infantry, never builds income, goes bankrupt and firesales its base to
	// death (reproduced with DoctrineExt entirely OUT — it's the modpack AI, not us). The
	// opening makes the AI secure income FIRST: if a capturable oil derrick is near AND
	// worth it, build a barracks and let the capture doctrine grab it (cheap passive cash
	// — an engineer is far cheaper than a refinery); otherwise force a refinery. It PAUSES
	// production per category (below a cash threshold, while no income) so the cash funds
	// income, not an early army — lifted by an emergency (enemy near) or once income exists.
	// Everything is a tag; 0 / empty = sensible auto-default. AI houses only.
	bool EconomyEnable = false;      // Enable — opt-in master switch
	int EconomyInterval = 15;        // Interval — frames between opening passes
	int EconomyRadius = 20;          // Radius — cells scanned around base for a derrick
	// Which owned buildings mean "income is secured" (lifts all pauses). Empty = auto:
	// any Refinery-flagged building, or any building that produces cash (a captured derrick).
	std::vector<std::string> EconomyIncomeBuildings;    // IncomeBuildings
	// Capturable tech worth BREAKING the refinery rule to grab for income. Empty = auto:
	// any Capturable building with ProduceCashAmount>0 (oil derricks).
	std::vector<std::string> EconomyDerrickTargets;     // DerrickTargets
	// The refinery/economy building(s) to force when no derrick. Empty = auto: cheapest
	// buildable Refinery-flagged building.
	std::vector<std::string> EconomyRefineryBuild;      // RefineryBuild
	// The barracks to force for barracks-first / derrick capture. Empty = auto: cheapest
	// buildable building whose Factory produces infantry.
	std::vector<std::string> EconomyBarracksBuild;      // BarracksBuild
	bool EconomyCaptureForIncome = true; // CaptureForIncome — prefer a near derrick over a refinery
	bool EconomyBarracksFirst = true;    // BarracksFirst — barracks before refinery when affordable
	int EconomyBarracksFirstCash = 0;    // BarracksFirstCash — cash to allow barracks-first;
	                                     // 0 = auto (power + barracks + refinery cost)
	int EconomySolvencyReserve = 1000;   // SolvencyReserve — barracks-first only if the whole
	                                     // opening (power+barracks+refinery) would still leave at
	                                     // least this much BEFORE income; else refinery-first so the
	                                     // refinery's income leads and the AI never dips into the
	                                     // low-cash window where the modpack AI firesales its base.
	// "Income" needs a MINER, not just a refinery (Rex's SMIN point): a plain refinery counts
	// as income only when the house also owns a miner; a passive cash building (derrick) always
	// counts. So a refinery whose harvester/slaves are gone keeps the opening active (and a
	// miner — never paused — is what then gets built, not a second refinery).
	bool EconomyIncomeRequiresMiner = true;         // IncomeRequiresMiner
	std::vector<std::string> EconomyMinerUnits;     // MinerUnits — empty = auto (ResourceGatherer)
	// Per-category production pause. Threshold: >0 = pause while the house has NO income and cash
	// <= this; 0 = never pause; -1 (any negative) = ALWAYS halt this category — a debug switch to
	// keep that production permanently off, ignoring income and emergencies. Exclude: IDs never
	// paused (unconditional). BreakRules: IDs allowed through the pause WHEN the break condition
	// holds. Income infrastructure (refineries, cash buildings, miners) is ALWAYS exempt so the
	// AI can always build its economy even under a -1 halt.
	struct EconomyPauseCat
	{
		int Threshold = 0;
		std::vector<std::string> Exclude;
		std::vector<std::string> BreakRules;
	};
	EconomyPauseCat EcoInfantry;      // PauseProduction.Infantry[.Exclude] + BreakProductionRules.Infantry
	EconomyPauseCat EcoVehicle;       // PauseProduction.Vehicle...
	EconomyPauseCat EcoAircraft;      // PauseProduction.Aircraft...
	EconomyPauseCat EcoNavy;          // PauseProduction.Navy...
	EconomyPauseCat EcoBuildings;     // PauseProduction.Buildings... (non-defense structures)
	EconomyPauseCat EcoDefenses;      // PauseProduction.Defenses... (IsBaseDefense structures)
	EconomyPauseCat EcoSuperWeapons;  // PausePricedSuperWeapons[.Exclude] + BreakProductionRules.SuperWeapons
	// BreakProductionRulesCondition.NeutralBuildings — the BreakRules apply only while a
	// capturable neutral building of one of these IDs is within Radius of the base (e.g. a
	// CAOILD worth sending engineers for). Empty = the BreakRules apply unconditionally.
	std::vector<std::string> EconomyBreakConditionNeutral;
	int EconomyEmergencyRadius = 12; // EmergencyRadius — enemy combat unit within = lift pauses
	// Prevent the broke-window firesale. Building a refinery necessarily drains the AI into a
	// low-cash window; the modpack AI firesales its base there, BEFORE the refinery's income
	// recovers it (seen selling at ~$448 the frame before income would have hit $3400). This
	// blocks that sale at the source — sets AI_Sellable=false on the AI's own buildings (the AI
	// never decides to sell them, so no mid-sale animation) — with a ForceMission(Guard) veto
	// as a guaranteed fallback if one still enters Selling. AI houses only; the human is
	// untouched. By default only while the house has no income (the vulnerable window).
	bool EconomyPreventSelling = true;       // PreventSelling
	bool EconomyPreventSellingAlways = false;// PreventSellingAlways — else only while no income
	int EconomyHonorPrereq = -1;     // HonorPrereq — -1 inherit, 0 ignore, 1 obey
	bool HonorPrereqForEconomy() const
		{ return EconomyHonorPrereq < 0 ? HonorPrerequisites : (EconomyHonorPrereq != 0); }

	// ─── Counter-engineer / urgent response ([Doctrine.CounterEngineer], §10l) ───
	// Deny enemy engineer rushes. Every base tick: find enemy engineers heading for the AI's
	// buildings (or neutral tech it wants), estimate whether the firepower that can reach them
	// kills them before they arrive, retask the fastest-engaging nearby armed units (any type)
	// onto them and point in-range defenses at them; if nothing can kill one in time, sell the
	// building first (an engineer bounces off a building that is mid-sale). Builds Units as
	// standing interceptors while the house's own firepower falls short.
	bool CounterEngEnable = false;   // Enable — opt-in master switch
	std::vector<std::string> CounterEngUnits; // Units — interceptors to BUILD; empty = auto (fastest
	                                 // buildable armed infantry; attack dogs win on speed)
	std::vector<std::string> CounterEngThreatTypes; // ThreatTypes — enemy types that trigger the
	                                 // response: empty = every Engineer=yes infantry; a list = exactly
	                                 // these infantry/vehicle types (a C4 commando, a spy, a demo truck)
	bool CounterEngThreatNone = false; // ThreatTypes=none (or null): nothing triggers it
	int CounterEngInterval = 30;     // Interval — frames between production passes / debug lines
	int CounterEngRadius = 12;       // Radius — cells from our building/contested tech an approaching
	                                 // engineer becomes a threat (any range if it targets one)
	int CounterEngMax = 3;           // Max — interceptors (Units) to own while firepower falls short
	bool CounterEngBuild = true;     // Build — produce Units when firepower falls short
	bool CounterEngRespond = true;   // Respond — retask nearby armed units onto threatening engineers
	int CounterEngRespondRadius = 15;// RespondRadius — cells from the threatened building to pull responders
	double CounterEngRespondMargin = 1.5; // RespondMargin — commit until expected damage >= HP x this
	int CounterEngRespondMax = 4;    // RespondMax — responders per engineer
	int CounterEngReaction = 15;     // ReactionFrames — a retasked unit's turn/acquire delay
	bool CounterEngRetargetDefenses = true; // RetargetDefenses — point in-range defenses at the engineer
	bool CounterEngDenySell = true;  // DenySell — sell a building an engineer reaches before it can die
	std::vector<std::string> CounterEngDenySellExclude; // DenySell.Exclude — never deny-sell these
	int CounterEngDenySellLead = 0;  // DenySell.Lead — sell when the engineer is this many frames out
	                                 // (0 = auto: two base ticks)
	double CounterEngDenySellMargin = 1.0; // DenySell.Margin — sell when ALL reachable firepower's
	                                 // expected damage < HP x this
	std::vector<std::string> CounterEngDenySellTypes; // DenySell.Types — buildings worth selling to
	                                 // deny a capture: empty = auto (construction yards, superweapons,
	                                 // and the house's LAST factory of a kind / last income building);
	                                 // a list = exactly these; none/null = never (still kill engineers)
	bool CounterEngDenySellNone = false; // DenySell.Types=none (or null)
	int CounterEngDenySellPerEngineer = 1; // DenySell.PerEngineer — sales one engineer may cause (0 = no cap)
	int CounterEngDenySellMax = 2;   // DenySell.Max — sales per house per DenySell.Window (0 = no cap)
	int CounterEngDenySellWindow = 1800; // DenySell.Window — frames DenySell.Max counts over
	bool CounterEngSacrifice = false; // Sacrifice — sell a cheap building by the engineer's path so its
	                                 // survivors (the side's crew infantry) kill the engineer
	std::vector<std::string> CounterEngSacrificeTypes; // Sacrifice.Types — buildings that may be
	                                 // sacrificed: empty = auto (crewed, not a construction yard,
	                                 // factory, income building, superweapon, defense or wall, and not
	                                 // a power plant we can't spare); a list = exactly; none/null = never
	bool CounterEngSacrificeNone = false; // Sacrifice.Types=none (or null)
	int CounterEngSacrificeMax = 1;  // Sacrifice.Max — sacrifices per engineer
	int CounterEngSacrificeRadius = 6; // Sacrifice.Radius — cells from the engineer's path to its goal
	int CounterEngSacrificeDelay = 60; // Sacrifice.Delay — frames from the sale to survivors on foot
	bool CounterEngBuildUrgent = true; // Build.Urgent — the interceptor jumps the AI's own infantry
	                                 // pick (else it waits for the AI to fill the squad's taskforce)
	bool CounterEngBuildIgnorePause = true; // Build.IgnorePause — the interceptor is exempt from
	                                 // [Doctrine.Economy] PauseProduction while a threat outruns us
	int CounterEngHonorPrereq = -1;  // HonorPrereq — -1 inherit, 0 ignore, 1 obey
	bool HonorPrereqForCounterEng() const
		{ return CounterEngHonorPrereq < 0 ? HonorPrerequisites : (CounterEngHonorPrereq != 0); }

	// ─── Engineer rush ([Doctrine.EngineerRush], §10m) ───────────────────────────
	// Capture ENEMY buildings near the AI's base with engineers. Each candidate is priced with the
	// same damage model the counter-engineer uses, in reverse: rush only where the enemy's reachable
	// firepower won't kill the engineer on its approach. Escorts attack the most dangerous defender.
	bool EngRushEnable = false;      // Enable — opt-in master switch
	int EngRushInterval = 60;        // Interval — frames between passes (steering is every tick)
	int EngRushRadius = 30;          // Radius — cells from our base center an enemy building may be
	std::vector<std::string> EngRushTargets;  // Targets — rushable buildings; empty = any Capturable
	std::vector<std::string> EngRushExclude;  // Exclude — never rush these
	std::vector<std::string> EngRushPriority; // Priority — rush these first, in list order; the
	                                 // rest by value (Cost)
	int EngRushMax = 2;              // Max — concurrent rush engineers
	int EngRushEscorts = 2;          // Escorts — armed units per engineer (0 = none)
	double EngRushSafety = 1.0;      // Safety — rush only if expected enemy damage x this < engineer HP
	int EngRushMinCash = 0;          // MinCash — build new rush engineers only at or above this cash
	int EngRushRetryDelay = 900;     // RetryDelay — frames before re-rushing a target that killed one
	bool EngRushBuildUrgent = true;  // Build.Urgent — rush engineers jump the AI's own infantry pick
	int EngRushHonorPrereq = -1;     // HonorPrereq — -1 inherit, 0 ignore, 1 obey
	bool HonorPrereqForEngRush() const
		{ return EngRushHonorPrereq < 0 ? HonorPrerequisites : (EngRushHonorPrereq != 0); }

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
