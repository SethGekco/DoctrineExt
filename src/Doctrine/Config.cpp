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
