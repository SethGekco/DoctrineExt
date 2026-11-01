#include "Doctrine/Config.h"
#include "Doctrine/Engine.h"
#include "Doctrine/Teams.h"
#include "Doctrine/KillTracker.h"
#include "Doctrine/LaneTracker.h"
#include "Doctrine/DeathZones.h"
#include "Doctrine/ArsenalGrader.h"
#include "Doctrine/AITriggerDifficulty.h"

#include <CCINIClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

DoctrineConfig DoctrineConfig::Instance;

namespace
{
	// Split a comma-separated INI value into trimmed, non-empty tokens.
	std::vector<std::string> SplitList(const char* value)
	{
		std::vector<std::string> out;
		std::string token;
		for (const char* p = value; ; ++p)
		{
			if (*p == ',' || *p == '\0')
			{
				while (!token.empty() && token.back() == ' ')
					token.pop_back();
				if (!token.empty())
					out.push_back(token);
				token.clear();
				if (*p == '\0')
					break;
			}
			else if (*p != ' ' || !token.empty())
				token += *p;
		}
		return out;
	}

	// Split When= into observation / op / threshold. Two-char ops first so
	// ">=" doesn't parse as ">" + "=200".
	bool ParseWhen(DoctrineRule& rule)
	{
		static const char* const ops[] = { ">=", "<=", ">", "<", "=" };
		for (auto const op : ops)
		{
			auto const pos = rule.WhenRaw.find(op);
			if (pos == std::string::npos || pos == 0)
				continue;
			rule.WhenObs = rule.WhenRaw.substr(0, pos);
			rule.WhenOp = op;
			rule.WhenValue = std::strtod(rule.WhenRaw.c_str() + pos + std::strlen(op), nullptr);
			while (!rule.WhenObs.empty() && rule.WhenObs.back() == ' ')
				rule.WhenObs.pop_back();
			return !rule.WhenObs.empty();
		}
		return false;
	}
}

void DoctrineConfig::Reset()
{
	Instance = DoctrineConfig{};
}

void DoctrineConfig::EnsureParsed()
{
	auto& cfg = Instance;
	if (cfg.Parsed)
		return;

	auto const pINI = CCINIClass::INI_Rules;
	if (!pINI)
		return;

	cfg.Parsed = true;
	char buf[1024] = { 0 };

	// ─── [Doctrine.General] ─────────────────────────────────────────────
	cfg.SenseInterval = pINI->ReadInteger("Doctrine.General", "SenseInterval", cfg.SenseInterval);
	cfg.RulePeriod = pINI->ReadInteger("Doctrine.General", "RulePeriod", cfg.RulePeriod);
	cfg.MaxTeamSize = pINI->ReadInteger("Doctrine.General", "MaxTeamSize", cfg.MaxTeamSize);
	cfg.MaxTeamCost = pINI->ReadInteger("Doctrine.General", "MaxTeamCost", cfg.MaxTeamCost);
	cfg.DebugTicks = pINI->ReadBool("Doctrine.General", "DebugTicks", cfg.DebugTicks);
	cfg.TeamTTL = pINI->ReadInteger("Doctrine.General", "TeamTTL", cfg.TeamTTL);
	cfg.TeamsPerHouse = pINI->ReadInteger("Doctrine.General", "TeamsPerHouse", cfg.TeamsPerHouse);
	cfg.StrictOwnership = pINI->ReadBool("Doctrine.General", "StrictOwnership", cfg.StrictOwnership);
	cfg.AirAlertRadius = pINI->ReadInteger("Doctrine.General", "AirAlertRadius", cfg.AirAlertRadius);
	cfg.InterceptStandoff = pINI->ReadInteger("Doctrine.General", "InterceptStandoff", cfg.InterceptStandoff);
	cfg.DefendPerimeter = pINI->ReadBool("Doctrine.General", "DefendPerimeter", cfg.DefendPerimeter);
	cfg.BaseEdgeMargin = pINI->ReadInteger("Doctrine.General", "BaseEdgeMargin", cfg.BaseEdgeMargin);
	cfg.LaneBucket = pINI->ReadInteger("Doctrine.General", "LaneBucket", cfg.LaneBucket);
	cfg.LaneSampleInterval = pINI->ReadInteger("Doctrine.General", "LaneSampleInterval", cfg.LaneSampleInterval);
	cfg.LaneDecayShift = pINI->ReadInteger("Doctrine.General", "LaneDecayShift", cfg.LaneDecayShift);
	cfg.LaneBumpWeight = pINI->ReadInteger("Doctrine.General", "LaneBumpWeight", cfg.LaneBumpWeight);
	cfg.LaneRadius = pINI->ReadInteger("Doctrine.General", "LaneRadius", cfg.LaneRadius);
	cfg.LaneMinStrength = pINI->ReadInteger("Doctrine.General", "LaneMinStrength", cfg.LaneMinStrength);
	cfg.DeathZoneBucket = pINI->ReadInteger("Doctrine.General", "DeathZoneBucket", cfg.DeathZoneBucket);
	cfg.DeathZoneDecayInterval = pINI->ReadInteger("Doctrine.General", "DeathZoneDecayInterval", cfg.DeathZoneDecayInterval);
	cfg.DeathZoneDecayShift = pINI->ReadInteger("Doctrine.General", "DeathZoneDecayShift", cfg.DeathZoneDecayShift);
	cfg.DeathZoneBumpWeight = pINI->ReadInteger("Doctrine.General", "DeathZoneBumpWeight", cfg.DeathZoneBumpWeight);
	cfg.DeathZoneRadius = pINI->ReadInteger("Doctrine.General", "DeathZoneRadius", cfg.DeathZoneRadius);
	cfg.DeathZoneMinStrength = pINI->ReadInteger("Doctrine.General", "DeathZoneMinStrength", cfg.DeathZoneMinStrength);
	cfg.MinCounterScore = pINI->ReadDouble("Doctrine.General", "MinCounterScore", cfg.MinCounterScore);
	cfg.SupportScanRadius = pINI->ReadInteger("Doctrine.General", "SupportScanRadius", cfg.SupportScanRadius);
	cfg.HuntStandoff = pINI->ReadInteger("Doctrine.General", "HuntStandoff", cfg.HuntStandoff);
	cfg.FloodThreshold = pINI->ReadInteger("Doctrine.General", "FloodThreshold", cfg.FloodThreshold);
	cfg.FloodHuntFraction = pINI->ReadDouble("Doctrine.General", "FloodHuntFraction", cfg.FloodHuntFraction);
	cfg.FloodCooldown = pINI->ReadInteger("Doctrine.General", "FloodCooldown", cfg.FloodCooldown);
	pINI->ReadString("Doctrine.General", "FloodExclude", "", buf, sizeof(buf));
	cfg.FloodExclude = SplitList(buf);
	pINI->ReadString("Doctrine.General", "FloodInclude", "", buf, sizeof(buf));
	cfg.FloodInclude = SplitList(buf);
	// [Doctrine.Reserve] — spend surplus cash on a modder-defined build list.
	cfg.ReserveAmount = pINI->ReadInteger("Doctrine.Reserve", "Amount", cfg.ReserveAmount);
	cfg.ReserveCooldown = pINI->ReadInteger("Doctrine.Reserve", "Cooldown", cfg.ReserveCooldown);
	cfg.ReserveGrowth = pINI->ReadInteger("Doctrine.Reserve", "Growth", cfg.ReserveGrowth);
	cfg.ReserveGrowthWindow = pINI->ReadInteger("Doctrine.Reserve", "GrowthWindow", cfg.ReserveGrowthWindow);
	pINI->ReadString("Doctrine.Reserve", "Build", "", buf, sizeof(buf));
	cfg.ReserveBuild = SplitList(buf);
	if (cfg.ReserveAmount > 0 || cfg.ReserveGrowth > 0)
		Debug::Log("[DoctrineExt] [Doctrine.Reserve]: Amount=%d Growth=%d/%df Cooldown=%d Build=%s\n",
			cfg.ReserveAmount, cfg.ReserveGrowth, cfg.ReserveGrowthWindow, cfg.ReserveCooldown, buf);
	// [Doctrine.Budget] — PriorityBuildings[0..] + PriorityBuildings.Reserve[0..].
	cfg.BudgetTiers.clear();
	for (int i = 0; ; ++i)
	{
		char key[48];
		std::snprintf(key, sizeof(key), "PriorityBuildings[%d]", i);
		pINI->ReadString("Doctrine.Budget", key, "", buf, sizeof(buf));
		if (!*buf) break;                         // first gap ends the list
		DoctrineConfig::BudgetTier tier;
		tier.Buildings = SplitList(buf);
		if (tier.Buildings.empty()) continue;
		std::snprintf(key, sizeof(key), "PriorityBuildings.Reserve[%d]", i);
		tier.Reserve = pINI->ReadInteger("Doctrine.Budget", key, 0);
		std::snprintf(key, sizeof(key), "PriorityBuildings.Count[%d]", i);
		tier.Count = pINI->ReadInteger("Doctrine.Budget", key, 1);
		if (tier.Count < 1) tier.Count = 1;
		cfg.BudgetTiers.push_back(tier);
	}
	cfg.BudgetEnable = !cfg.BudgetTiers.empty()
		&& pINI->ReadBool("Doctrine.Budget", "Enable", true);
	cfg.BudgetInterval = pINI->ReadInteger("Doctrine.Budget", "Interval", cfg.BudgetInterval);
	if (cfg.BudgetEnable)
		Debug::Log("[DoctrineExt] [Doctrine.Budget]: %d priority tier(s), interval=%d.\n",
			static_cast<int>(cfg.BudgetTiers.size()), cfg.BudgetInterval);

	cfg.RushRatio = pINI->ReadDouble("Doctrine.General", "RushRatio", cfg.RushRatio);
	cfg.RushSafety = pINI->ReadDouble("Doctrine.General", "RushSafety", cfg.RushSafety);
	cfg.RushCooldown = pINI->ReadInteger("Doctrine.General", "RushCooldown", cfg.RushCooldown);
	cfg.RushMinUnits = pINI->ReadInteger("Doctrine.General", "RushMinUnits", cfg.RushMinUnits);
	cfg.RushMinPower = pINI->ReadDouble("Doctrine.General", "RushMinPower", cfg.RushMinPower);
	cfg.CommanderIdleTime = pINI->ReadInteger("Doctrine.Commander", "IdleTime", cfg.CommanderIdleTime);
	cfg.CommanderMinArmy = pINI->ReadInteger("Doctrine.Commander", "MinArmy", cfg.CommanderMinArmy);
	cfg.CommanderFront = pINI->ReadInteger("Doctrine.Commander", "Front", cfg.CommanderFront);
	cfg.CommanderCommitFrac = pINI->ReadDouble("Doctrine.Commander", "CommitFraction", cfg.CommanderCommitFrac);
	cfg.CommanderBatch = pINI->ReadInteger("Doctrine.Commander", "Batch", cfg.CommanderBatch);
	cfg.CommanderPeriod = pINI->ReadInteger("Doctrine.Commander", "Period", cfg.CommanderPeriod);
	cfg.AirDefenseEnable = pINI->ReadBool("Doctrine.AirDefense", "Enable", cfg.AirDefenseEnable);
	cfg.AirThreatRatio = pINI->ReadDouble("Doctrine.AirDefense", "ThreatRatio", cfg.AirThreatRatio);
	cfg.AirDefensePeriod = pINI->ReadInteger("Doctrine.AirDefense", "Period", cfg.AirDefensePeriod);
	cfg.AAMaxProduce = pINI->ReadInteger("Doctrine.AirDefense", "MaxProduce", cfg.AAMaxProduce);
	cfg.AirDefenseStandoff = pINI->ReadInteger("Doctrine.AirDefense", "Standoff", cfg.AirDefenseStandoff);
	cfg.AirScreenRadius = pINI->ReadInteger("Doctrine.AirDefense", "ScreenRadius", cfg.AirScreenRadius);
	cfg.AirDeathFloor = pINI->ReadDouble("Doctrine.AirDefense", "AirDeathFloor", cfg.AirDeathFloor);
	cfg.NavalEnable = pINI->ReadBool("Doctrine.Naval", "Enable", cfg.NavalEnable);
	cfg.NavalInterval = pINI->ReadInteger("Doctrine.Naval", "Interval", cfg.NavalInterval);
	cfg.NavalScanRadius = pINI->ReadInteger("Doctrine.Naval", "ScanRadius", cfg.NavalScanRadius);
	cfg.NavalMinWater = pINI->ReadInteger("Doctrine.Naval", "MinWater", cfg.NavalMinWater);
	cfg.NavyTarget = pINI->ReadInteger("Doctrine.Naval", "NavyTarget", cfg.NavyTarget);
	cfg.NavyMaxProduce = pINI->ReadInteger("Doctrine.Naval", "MaxProduce", cfg.NavyMaxProduce);
	cfg.ExpansionEnable = pINI->ReadBool("Doctrine.Expansion", "Enable", cfg.ExpansionEnable);
	cfg.ExpansionInterval = pINI->ReadInteger("Doctrine.Expansion", "Interval", cfg.ExpansionInterval);
	cfg.ExpansionRadius = pINI->ReadInteger("Doctrine.Expansion", "Radius", cfg.ExpansionRadius);
	cfg.WarFactoryMax = pINI->ReadInteger("Doctrine.Expansion", "WarFactoryMax", cfg.WarFactoryMax);
	cfg.RefineryMax = pINI->ReadInteger("Doctrine.Expansion", "RefineryMax", cfg.RefineryMax);
	cfg.LandPerFactory = pINI->ReadInteger("Doctrine.Expansion", "LandPerFactory", cfg.LandPerFactory);
	cfg.WealthPerRefinery = pINI->ReadInteger("Doctrine.Expansion", "WealthPerRefinery", cfg.WealthPerRefinery);
	cfg.DecapEnable = pINI->ReadBool("Doctrine.Decap", "Enable", cfg.DecapEnable);
	cfg.DecapConYard = pINI->ReadInteger("Doctrine.Decap", "ConYard", cfg.DecapConYard);
	cfg.DecapRefinery = pINI->ReadInteger("Doctrine.Decap", "Refinery", cfg.DecapRefinery);
	cfg.DecapVehicle = pINI->ReadInteger("Doctrine.Decap", "Vehicle", cfg.DecapVehicle);
	cfg.DecapAircraft = pINI->ReadInteger("Doctrine.Decap", "Aircraft", cfg.DecapAircraft);
	cfg.DecapInfantry = pINI->ReadInteger("Doctrine.Decap", "Infantry", cfg.DecapInfantry);
	cfg.DecapDefense = pINI->ReadInteger("Doctrine.Decap", "Defense", cfg.DecapDefense);
	cfg.DecapOther = pINI->ReadInteger("Doctrine.Decap", "Other", cfg.DecapOther);
	cfg.AutoArsenal = pINI->ReadBool("Doctrine.General", "AutoArsenal", cfg.AutoArsenal);
	cfg.AutoArsenalDepth = pINI->ReadInteger("Doctrine.General", "AutoArsenalDepth", cfg.AutoArsenalDepth);
	cfg.CrateChase = pINI->ReadBool("Doctrine.General", "CrateChase", cfg.CrateChase);
	cfg.CrateScanRadius = pINI->ReadInteger("Doctrine.General", "CrateScanRadius", cfg.CrateScanRadius);
	cfg.CrateInterval = pINI->ReadInteger("Doctrine.General", "CrateInterval", cfg.CrateInterval);
	cfg.CrateFiresaleMCV = pINI->ReadBool("Doctrine.General", "CrateFiresaleMCV", cfg.CrateFiresaleMCV);
	cfg.CrateFiresaleDist = pINI->ReadInteger("Doctrine.General", "CrateFiresaleDist", cfg.CrateFiresaleDist);
	cfg.CrateSquad = pINI->ReadBool("Doctrine.General", "CrateSquad", cfg.CrateSquad);
	cfg.CrateSquadSize = pINI->ReadInteger("Doctrine.General", "CrateSquadSize", cfg.CrateSquadSize);
	cfg.CrateSquadMax = pINI->ReadInteger("Doctrine.General", "CrateSquadMax", cfg.CrateSquadMax);
	cfg.CrateSquadScan = pINI->ReadInteger("Doctrine.General", "CrateSquadScan", cfg.CrateSquadScan);
	cfg.CrateCatcherBuild = pINI->ReadBool("Doctrine.General", "CrateCatcherBuild", cfg.CrateCatcherBuild);
	cfg.CrateCatcherMax = pINI->ReadInteger("Doctrine.General", "CrateCatcherMax", cfg.CrateCatcherMax);
	cfg.CrateCatcherFastSpeed = pINI->ReadInteger("Doctrine.General", "CrateCatcherFastSpeed", cfg.CrateCatcherFastSpeed);
	cfg.CrateCatcherMaxProduce = pINI->ReadInteger("Doctrine.General", "CrateCatcherMaxProduce", cfg.CrateCatcherMaxProduce);
	cfg.CrateCatcherGiveUp = pINI->ReadInteger("Doctrine.General", "CrateCatcherGiveUp", cfg.CrateCatcherGiveUp);
	cfg.CrateCatcherForceBuild = pINI->ReadBool("Doctrine.General", "CrateCatcherForceBuild", cfg.CrateCatcherForceBuild);
	cfg.CrateCatcherSkipSpeed = pINI->ReadInteger("Doctrine.General", "CrateCatcherSkipSpeed", cfg.CrateCatcherSkipSpeed);
	cfg.CrateChaserGateCount = pINI->ReadInteger("Doctrine.General", "CrateChaserGateCount", cfg.CrateChaserGateCount);
	cfg.HonorPrerequisites = pINI->ReadBool("Doctrine.General", "HonorPrerequisites", cfg.HonorPrerequisites);
	cfg.CrateChaserHonorPrereq = pINI->ReadInteger("Doctrine.General", "CrateChaserHonorPrereq", cfg.CrateChaserHonorPrereq);
	cfg.RushHonorPrereq = pINI->ReadInteger("Doctrine.General", "RushHonorPrereq", cfg.RushHonorPrereq);
	cfg.CrateGiveUpPasses = pINI->ReadInteger("Doctrine.General", "CrateGiveUpPasses", cfg.CrateGiveUpPasses);
	cfg.CrateBlacklistTime = pINI->ReadInteger("Doctrine.General", "CrateBlacklistTime", cfg.CrateBlacklistTime);
	cfg.GarrisonInfantry = pINI->ReadBool("Doctrine.General", "GarrisonInfantry", cfg.GarrisonInfantry);
	cfg.GarrisonInterval = pINI->ReadInteger("Doctrine.General", "GarrisonInterval", cfg.GarrisonInterval);
	cfg.GarrisonMaxProduce = pINI->ReadInteger("Doctrine.General", "GarrisonMaxProduce", cfg.GarrisonMaxProduce);
	cfg.GarrisonRadius = pINI->ReadInteger("Doctrine.Garrison", "Radius", cfg.GarrisonRadius);
	cfg.GarrisonRadiusStart = pINI->ReadInteger("Doctrine.Garrison", "RadiusStart", cfg.GarrisonRadiusStart);
	cfg.GarrisonCreepRate = pINI->ReadInteger("Doctrine.Garrison", "CreepRate", cfg.GarrisonCreepRate);
	cfg.GarrisonScanNear = pINI->ReadInteger("Doctrine.Garrison", "ScanNear", cfg.GarrisonScanNear);
	cfg.GarrisonMaxDivert = pINI->ReadInteger("Doctrine.Garrison", "MaxDivert", cfg.GarrisonMaxDivert);
	cfg.GPerimeterW = pINI->ReadDouble("Doctrine.Garrison", "PerimeterWeight", cfg.GPerimeterW);
	cfg.GCreepW = pINI->ReadDouble("Doctrine.Garrison", "CreepWeight", cfg.GCreepW);
	cfg.GOreW = pINI->ReadDouble("Doctrine.Garrison", "OreWeight", cfg.GOreW);
	cfg.GTechW = pINI->ReadDouble("Doctrine.Garrison", "TechWeight", cfg.GTechW);
	pINI->ReadString("Doctrine.General", "CrateChasers", "", buf, sizeof(buf));
	cfg.CrateChasers.clear();
	for (auto const& tok : SplitList(buf))
	{
		DoctrineConfig::CrateChaserEntry e;
		auto const pos = tok.find(':');
		if (pos == std::string::npos) { e.ID = tok; e.Radius = -1; }
		else { e.ID = tok.substr(0, pos); e.Radius = std::atoi(tok.substr(pos + 1).c_str()); }
		if (!e.ID.empty()) cfg.CrateChasers.push_back(e);
	}
	pINI->ReadString("Doctrine.General", "CrateChaseExclude", "", buf, sizeof(buf));
	cfg.CrateChaseExclude = SplitList(buf);
	// ─── [Doctrine.Capture] — capture neutral/enemy tech buildings w/ engineers ──
	cfg.CaptureEnable = pINI->ReadBool("Doctrine.Capture", "Enable", cfg.CaptureEnable);
	pINI->ReadString("Doctrine.Capture", "Targets", "", buf, sizeof(buf));
	cfg.CaptureTargets = SplitList(buf);
	cfg.CaptureInterval = pINI->ReadInteger("Doctrine.Capture", "Interval", cfg.CaptureInterval);
	cfg.CaptureMaxTeams = pINI->ReadInteger("Doctrine.Capture", "MaxTeams", cfg.CaptureMaxTeams);
	cfg.CaptureRadius = pINI->ReadInteger("Doctrine.Capture", "Radius", cfg.CaptureRadius);
	cfg.CaptureEngineerMax = pINI->ReadInteger("Doctrine.Capture", "EngineerMax", cfg.CaptureEngineerMax);
	cfg.CaptureEnemyOwned = pINI->ReadBool("Doctrine.Capture", "EnemyOwned", cfg.CaptureEnemyOwned);
	cfg.CaptureHonorPrereq = pINI->ReadInteger("Doctrine.Capture", "HonorPrereq", cfg.CaptureHonorPrereq);
	if (cfg.CaptureEnable)
		Debug::Log("[DoctrineExt] [Doctrine.Capture]: Enable=1 Targets=%u (empty=any) Interval=%d "
			"MaxTeams=%d Radius=%d EngineerMax=%d EnemyOwned=%d HonorPrereq=%d\n",
			static_cast<unsigned>(cfg.CaptureTargets.size()), cfg.CaptureInterval, cfg.CaptureMaxTeams,
			cfg.CaptureRadius, cfg.CaptureEngineerMax, cfg.CaptureEnemyOwned, cfg.CaptureHonorPrereq);
	// ─── [Doctrine.Economy] — broke-proof opening (fully tag-driven budget) ──
	cfg.EconomyEnable = pINI->ReadBool("Doctrine.Economy", "Enable", cfg.EconomyEnable);
	cfg.EconomyInterval = pINI->ReadInteger("Doctrine.Economy", "Interval", cfg.EconomyInterval);
	cfg.EconomyRadius = pINI->ReadInteger("Doctrine.Economy", "Radius", cfg.EconomyRadius);
	pINI->ReadString("Doctrine.Economy", "IncomeBuildings", "", buf, sizeof(buf));
	cfg.EconomyIncomeBuildings = SplitList(buf);
	pINI->ReadString("Doctrine.Economy", "DerrickTargets", "", buf, sizeof(buf));
	cfg.EconomyDerrickTargets = SplitList(buf);
	pINI->ReadString("Doctrine.Economy", "RefineryBuild", "", buf, sizeof(buf));
	cfg.EconomyRefineryBuild = SplitList(buf);
	pINI->ReadString("Doctrine.Economy", "BarracksBuild", "", buf, sizeof(buf));
	cfg.EconomyBarracksBuild = SplitList(buf);
	cfg.EconomyCaptureForIncome = pINI->ReadBool("Doctrine.Economy", "CaptureForIncome", cfg.EconomyCaptureForIncome);
	cfg.EconomyBarracksFirst = pINI->ReadBool("Doctrine.Economy", "BarracksFirst", cfg.EconomyBarracksFirst);
	cfg.EconomyBarracksFirstCash = pINI->ReadInteger("Doctrine.Economy", "BarracksFirstCash", cfg.EconomyBarracksFirstCash);
	cfg.EconomyIncomeRequiresMiner = pINI->ReadBool("Doctrine.Economy", "IncomeRequiresMiner", cfg.EconomyIncomeRequiresMiner);
	pINI->ReadString("Doctrine.Economy", "MinerUnits", "", buf, sizeof(buf));
	cfg.EconomyMinerUnits = SplitList(buf);
	// Per-category pause: threshold + .Exclude + BreakProductionRules.<cat>.
	auto parseEcoCat = [&](const char* const thrKey, const char* const brkName,
		DoctrineConfig::EconomyPauseCat& c)
	{
		c.Threshold = pINI->ReadInteger("Doctrine.Economy", thrKey, c.Threshold);
		char key[96];
		std::snprintf(key, sizeof(key), "%s.Exclude", thrKey);
		pINI->ReadString("Doctrine.Economy", key, "", buf, sizeof(buf));
		c.Exclude = SplitList(buf);
		std::snprintf(key, sizeof(key), "BreakProductionRules.%s", brkName);
		pINI->ReadString("Doctrine.Economy", key, "", buf, sizeof(buf));
		c.BreakRules = SplitList(buf);
	};
	parseEcoCat("PauseProduction.Infantry", "Infantry", cfg.EcoInfantry);
	parseEcoCat("PauseProduction.Vehicle", "Vehicle", cfg.EcoVehicle);
	parseEcoCat("PauseProduction.Aircraft", "Aircraft", cfg.EcoAircraft);
	parseEcoCat("PauseProduction.Navy", "Navy", cfg.EcoNavy);
	parseEcoCat("PauseProduction.Buildings", "Buildings", cfg.EcoBuildings);
	parseEcoCat("PauseProduction.Defenses", "Defenses", cfg.EcoDefenses);
	parseEcoCat("PausePricedSuperWeapons", "SuperWeapons", cfg.EcoSuperWeapons);
	pINI->ReadString("Doctrine.Economy", "BreakProductionRulesCondition.NeutralBuildings", "", buf, sizeof(buf));
	cfg.EconomyBreakConditionNeutral = SplitList(buf);
	cfg.EconomyEmergencyRadius = pINI->ReadInteger("Doctrine.Economy", "EmergencyRadius", cfg.EconomyEmergencyRadius);
	cfg.EconomyHonorPrereq = pINI->ReadInteger("Doctrine.Economy", "HonorPrereq", cfg.EconomyHonorPrereq);
	if (cfg.EconomyEnable)
		Debug::Log("[DoctrineExt] [Doctrine.Economy]: Enable=1 Interval=%d Radius=%d IncomeBuildings=%u "
			"DerrickTargets=%u CaptureForIncome=%d BarracksFirst=%d/%d IncomeRequiresMiner=%d "
			"Pause(I/V/A/N/B/D/SW)=%d/%d/%d/%d/%d/%d/%d BreakCond=%u EmergencyRadius=%d HonorPrereq=%d\n",
			cfg.EconomyInterval, cfg.EconomyRadius, static_cast<unsigned>(cfg.EconomyIncomeBuildings.size()),
			static_cast<unsigned>(cfg.EconomyDerrickTargets.size()), cfg.EconomyCaptureForIncome,
			cfg.EconomyBarracksFirst, cfg.EconomyBarracksFirstCash, cfg.EconomyIncomeRequiresMiner,
			cfg.EcoInfantry.Threshold, cfg.EcoVehicle.Threshold, cfg.EcoAircraft.Threshold, cfg.EcoNavy.Threshold,
			cfg.EcoBuildings.Threshold, cfg.EcoDefenses.Threshold, cfg.EcoSuperWeapons.Threshold,
			static_cast<unsigned>(cfg.EconomyBreakConditionNeutral.size()), cfg.EconomyEmergencyRadius,
			cfg.EconomyHonorPrereq);
	cfg.AITriggerDelay = pINI->ReadInteger("Doctrine.General", "AITriggerDelay", cfg.AITriggerDelay);
	cfg.AceMobileOnly = pINI->ReadBool("Doctrine.General", "AceMobileOnly", cfg.AceMobileOnly);
	cfg.AutoProduce = pINI->ReadBool("Doctrine.General", "AutoProduce", cfg.AutoProduce);
	cfg.MaxProducePerDispatch = pINI->ReadInteger("Doctrine.General", "MaxProducePerDispatch", cfg.MaxProducePerDispatch);
	Debug::Log("[DoctrineExt] [Doctrine.General]: SenseInterval=%d RulePeriod=%d MaxTeamSize=%d MaxTeamCost=%d DebugTicks=%d TeamTTL=%d TeamsPerHouse=%d StrictOwnership=%d AirAlertRadius=%d InterceptStandoff=%d DefendPerimeter=%d BaseEdgeMargin=%d LaneBucket=%d LaneSampleInterval=%d LaneRadius=%d LaneMinStrength=%d AceMobileOnly=%d AutoProduce=%d MaxProducePerDispatch=%d\n",
		cfg.SenseInterval, cfg.RulePeriod, cfg.MaxTeamSize, cfg.MaxTeamCost, cfg.DebugTicks, cfg.TeamTTL, cfg.TeamsPerHouse, cfg.StrictOwnership, cfg.AirAlertRadius, cfg.InterceptStandoff, cfg.DefendPerimeter, cfg.BaseEdgeMargin, cfg.LaneBucket, cfg.LaneSampleInterval, cfg.LaneRadius, cfg.LaneMinStrength, cfg.AceMobileOnly, cfg.AutoProduce, cfg.MaxProducePerDispatch);

	// ─── [Doctrine.Arsenal] — key = role, value = unit IDs best-first ───
	int const roleCount = pINI->GetKeyCount("Doctrine.Arsenal");
	for (int i = 0; i < roleCount; ++i)
	{
		auto const pKey = pINI->GetKeyName("Doctrine.Arsenal", i);
		if (!pKey || !*pKey)
			continue;
		pINI->ReadString("Doctrine.Arsenal", pKey, "", buf, sizeof(buf));
		DoctrineArsenalRole role;
		role.Role = pKey;
		role.Units = SplitList(buf);
		if (role.Units.empty())
		{
			Debug::Log("[DoctrineExt] WARNING: arsenal role %s has no units, dropped.\n", pKey);
			continue;
		}
		Debug::Log("[DoctrineExt] arsenal %s (%u units): %s\n",
			role.Role.c_str(), role.Units.size(), buf);
		cfg.Arsenal.push_back(std::move(role));
	}

	// ─── [Doctrine.Rules] — indexed list of rule section names ──────────
	int const ruleCount = pINI->GetKeyCount("Doctrine.Rules");
	for (int i = 0; i < ruleCount; ++i)
	{
		auto const pKey = pINI->GetKeyName("Doctrine.Rules", i);
		if (!pKey)
			continue;
		pINI->ReadString("Doctrine.Rules", pKey, "", buf, sizeof(buf));
		if (!*buf)
			continue;

		DoctrineRule rule;
		rule.Name = buf;
		auto const section = rule.Name.c_str();
		if (!pINI->GetSection(section))
		{
			Debug::Log("[DoctrineExt] WARNING: rule %s listed but section missing, dropped.\n", section);
			continue;
		}

		pINI->ReadString(section, "When", "", buf, sizeof(buf));
		rule.WhenRaw = buf;
		pINI->ReadString(section, "Respond", "", buf, sizeof(buf));
		rule.Respond = buf;
		pINI->ReadString(section, "Mission", "", buf, sizeof(buf));
		rule.Mission = buf;
		pINI->ReadString(section, "Target", "", buf, sizeof(buf));
		rule.Target = buf;
		rule.Scale = pINI->ReadDouble(section, "Scale", rule.Scale);
		rule.Cooldown = pINI->ReadInteger(section, "Cooldown", rule.Cooldown);
		rule.Priority = pINI->ReadInteger(section, "Priority", rule.Priority);
		rule.Period = pINI->ReadInteger(section, "Period", rule.Period);

		if (!ParseWhen(rule))
			Debug::Log("[DoctrineExt] WARNING: rule %s has unparseable When=%s, rule is inert.\n",
				section, rule.WhenRaw.c_str());

		bool roleKnown = false;
		for (auto const& role : cfg.Arsenal)
			if (role.Role == rule.Respond)
				roleKnown = true;
		if (!roleKnown)
			Debug::Log("[DoctrineExt] WARNING: rule %s responds with unknown role %s.\n",
				section, rule.Respond.c_str());

		Debug::Log("[DoctrineExt] rule %s: When=%s (obs=%s op=%s val=%.2f) Respond=%s Scale=%.2f Mission=%s Target=%s Cooldown=%d Priority=%d Period=%d\n",
			rule.Name.c_str(), rule.WhenRaw.c_str(), rule.WhenObs.c_str(), rule.WhenOp.c_str(),
			rule.WhenValue, rule.Respond.c_str(), rule.Scale, rule.Mission.c_str(),
			rule.Target.c_str(), rule.Cooldown, rule.Priority, rule.Period);
		cfg.Rules.push_back(std::move(rule));
	}

	Debug::Log("[DoctrineExt] doctrine loaded: %u roles, %u rules.\n",
		cfg.Arsenal.size(), cfg.Rules.size());
}

// Scenario::ClearClasses — game-mode INIs merge into the rules INI per
// scenario, so drop the parsed doctrine and re-read it fresh each match.
// Same site IntelExt uses for its per-scenario clear (same-address hooks
// chain legally).
DEFINE_HOOK(0x685659, DoctrineExt_Scenario_ClearClasses, 0xA)
{
	DoctrineConfig::Reset();
	Engine::Reset();
	Teams::Reset();
	KillTracker::Reset();
	LaneTracker::Reset();
	DeathZones::Reset();
	ArsenalGrader::Reset();
	AITriggerDifficulty::Reset();
	DoctrineConfig::EnsureParsed();
	return 0;
}
