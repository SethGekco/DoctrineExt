#include "Doctrine/Config.h"
#include "Doctrine/Engine.h"
#include "Doctrine/Teams.h"
#include "Doctrine/KillTracker.h"
#include "Doctrine/LaneTracker.h"
#include "Doctrine/DeathZones.h"
#include "Doctrine/ArsenalGrader.h"
#include "Doctrine/AITriggerDifficulty.h"
#include "Doctrine/OrderAudit.h"

#include <CCINIClass.h>
#include <HouseClass.h>
#include <Utilities/Debug.h>
#include <Utilities/Macro.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

DoctrineConfig DoctrineConfig::Instance;
DoctrineConfig DoctrineConfig::ByDifficulty[3];
DoctrineConfig const* DoctrineConfig::Active = &DoctrineConfig::Instance;

namespace
{
	// Tag suffixes per HouseClass::AIDifficulty (0 = Hard, 1 = Normal, 2 = Easy). The lobby calls
	// Normal "Medium AI", so ".Medium=" reads too; ".Normal=" wins if a tag has both.
	const char* const kDifficulty[3][2] = { { "Hard", nullptr }, { "Normal", "Medium" }, { "Easy", nullptr } };

	bool SameWord(const char* a, const char* b)
	{
		for (; *a && *b; ++a, ++b)
			if ((*a | 0x20) != (*b | 0x20)) return false;
		return *a == *b;
	}

	// Which kDifficulty name a tag's last part is: {difficulty, name index}, or {-1, -1}.
	std::pair<int, int> DifficultyName(const char* const tail)
	{
		for (int d = 0; d < 3; ++d)
			for (int n = 0; n < 2; ++n)
				if (kDifficulty[d][n] && SameWord(tail, kDifficulty[d][n]))
					return { d, n };
		return { -1, -1 };
	}

	// Echo parsed values to debug.log: on for the tags as written, off for the per-difficulty sets
	// (those log only their overrides, from TagReader).
	bool g_echo = true;
	void Echo(const char* const pFormat, ...)
	{
		if (!g_echo) return;
		va_list args;
		va_start(args, pFormat);
		Debug::LogWithVArgs(pFormat, args);
		va_end(args);
	}

	// Reads the [Doctrine.*] tags for one AI difficulty: "<Key>.<Difficulty>" when the INI has it,
	// else "<Key>" (difficulty -1 = the tags as written). Same calls as CCINIClass, so the parser
	// reads through either.
	class TagReader
	{
	public:
		TagReader(CCINIClass* const pINI, int const difficulty) : INI(pINI), Difficulty(difficulty) { }

		int ReadInteger(const char* s, const char* k, int def) { return INI->ReadInteger(s, Key(s, k), def); }
		bool ReadBool(const char* s, const char* k, bool def) { return INI->ReadBool(s, Key(s, k), def); }
		double ReadDouble(const char* s, const char* k, double def) { return INI->ReadDouble(s, Key(s, k), def); }
		int ReadString(const char* s, const char* k, const char* def, char* buf, size_t size)
			{ return INI->ReadString(s, Key(s, k), def, buf, size); }
		int GetKeyCount(const char* s) { return INI->GetKeyCount(s); }
		const char* GetKeyName(const char* s, int i) { return INI->GetKeyName(s, i); }
		auto GetSection(const char* s) { return INI->GetSection(s); }

		// A listed entry ([Doctrine.Arsenal] roles, [Doctrine.Rules] lines): a plain key is itself. A
		// difficulty variant only stands in for a key the section lacks, and only for its own
		// difficulty ("AntiAir.Hard=" alone adds a role for Hard AIs). False = skip this key.
		bool EntryName(const char* s, const char* k, std::string& out)
		{
			out = k;
			auto const dot = out.rfind('.');
			if (dot == std::string::npos) return true;
			auto const [d, n] = DifficultyName(out.c_str() + dot + 1);
			if (d < 0) return true;
			if (d != Difficulty) return false;
			out.resize(dot);
			if (n > 0 && INI->Exists(s, (out + "." + kDifficulty[d][0]).c_str())) return false;
			return !INI->Exists(s, out.c_str());
		}

	private:
		const char* Key(const char* s, const char* k)
		{
			if (Difficulty < 0) return k;
			for (auto const pName : kDifficulty[Difficulty])
			{
				if (!pName) continue;
				std::snprintf(Variant, sizeof(Variant), "%s.%s", k, pName);
				if (!INI->Exists(s, Variant)) continue;
				char value[256] = { 0 };
				INI->ReadString(s, Variant, "", value, sizeof(value));
				Debug::Log("[DoctrineExt] %s AI: [%s] %s=%s (overrides %s=)\n", kDifficulty[Difficulty][0], s,
					Variant, value, k);
				return Variant;
			}
			return k;
		}

		CCINIClass* INI;
		int Difficulty;
		char Variant[160] = { 0 };
	};

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

	// A type list whose empty value means "auto" can still be switched off outright: a "none" or
	// "null" token (any case) means nothing at all.
	bool IsNoneList(std::vector<std::string> const& list)
	{
		auto const same = [](std::string const& s, const char* const word)
		{
			if (s.size() != std::strlen(word)) return false;
			for (size_t i = 0; i < s.size(); ++i)
				if ((s[i] | 0x20) != word[i]) return false;
			return true;
		};
		for (auto const& s : list)
			if (same(s, "none") || same(s, "null")) return true;
		return false;
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
	for (auto& c : ByDifficulty)
		c = DoctrineConfig{};
	Active = &Instance;
}

DoctrineConfig const& DoctrineConfig::For(HouseClass* const pHouse)
{
	if (!pHouse) return Instance;
	auto const d = static_cast<unsigned>(pHouse->AIDifficulty);
	return d < 3 && ByDifficulty[d].Parsed ? ByDifficulty[d] : Instance;
}

DoctrineConfig::HouseScope::HouseScope(HouseClass* const pHouse) : Prev(Active)
{
	Active = &For(pHouse);
}

DoctrineConfig::HouseScope::~HouseScope()
{
	Active = Prev;
}

// One full read of the [Doctrine.*] tags into cfg, through a TagReader (the tags as written, or one
// difficulty's view of them).
static void Parse(DoctrineConfig& cfg, TagReader* const pINI)
{
	char buf[1024] = { 0 };

	// ─── [Doctrine.General] ─────────────────────────────────────────────
	cfg.SenseInterval = pINI->ReadInteger("Doctrine.General", "SenseInterval", cfg.SenseInterval);
	cfg.RulePeriod = pINI->ReadInteger("Doctrine.General", "RulePeriod", cfg.RulePeriod);
	cfg.MaxTeamSize = pINI->ReadInteger("Doctrine.General", "MaxTeamSize", cfg.MaxTeamSize);
	cfg.MaxTeamCost = pINI->ReadInteger("Doctrine.General", "MaxTeamCost", cfg.MaxTeamCost);
	cfg.DebugTicks = pINI->ReadBool("Doctrine.General", "DebugTicks", cfg.DebugTicks);
	cfg.DebugOrders = pINI->ReadBool("Doctrine.General", "DebugOrders", cfg.DebugOrders);
	cfg.SteerReissue = pINI->ReadInteger("Doctrine.General", "SteerReissue", cfg.SteerReissue);
	cfg.SquadEngineControl = pINI->ReadBool("Doctrine.General", "SquadEngineControl", cfg.SquadEngineControl);
	cfg.TeamTTL = pINI->ReadInteger("Doctrine.General", "TeamTTL", cfg.TeamTTL);
	cfg.TeamsPerHouse = pINI->ReadInteger("Doctrine.General", "TeamsPerHouse", cfg.TeamsPerHouse);
	cfg.StrictOwnership = pINI->ReadBool("Doctrine.General", "StrictOwnership", cfg.StrictOwnership);
	cfg.AirAlertRadius = pINI->ReadInteger("Doctrine.General", "AirAlertRadius", cfg.AirAlertRadius);
	cfg.InterceptStandoff = pINI->ReadInteger("Doctrine.General", "InterceptStandoff", cfg.InterceptStandoff);
	cfg.InterceptLeash = pINI->ReadInteger("Doctrine.General", "InterceptLeash", cfg.InterceptLeash);
	cfg.DefendLeash = pINI->ReadInteger("Doctrine.General", "DefendLeash", cfg.DefendLeash);
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
	cfg.HuntLeash = pINI->ReadInteger("Doctrine.General", "HuntLeash", cfg.HuntLeash);
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
		Echo("[DoctrineExt] [Doctrine.Reserve]: Amount=%d Growth=%d/%df Cooldown=%d Build=%s\n",
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
		Echo("[DoctrineExt] [Doctrine.Budget]: %d priority tier(s), interval=%d.\n",
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
	cfg.AirScreenLeash = pINI->ReadInteger("Doctrine.AirDefense", "ScreenLeash", cfg.AirScreenLeash);
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
	cfg.GarrisonRedivertDelay = pINI->ReadInteger("Doctrine.Garrison", "RedivertDelay", cfg.GarrisonRedivertDelay);
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
	cfg.CaptureBuildUrgent = pINI->ReadBool("Doctrine.Capture", "Build.Urgent", cfg.CaptureBuildUrgent);
	cfg.CaptureBuildIgnorePause = pINI->ReadBool("Doctrine.Capture", "Build.IgnorePause", cfg.CaptureBuildIgnorePause);
	cfg.CaptureHonorPrereq = pINI->ReadInteger("Doctrine.Capture", "HonorPrereq", cfg.CaptureHonorPrereq);
	if (cfg.CaptureEnable)
		Echo("[DoctrineExt] [Doctrine.Capture]: Enable=1 Targets=%u (empty=any) Interval=%d "
			"MaxTeams=%d Radius=%d EngineerMax=%d EnemyOwned=%d Build.Urgent=%d Build.IgnorePause=%d "
			"HonorPrereq=%d\n",
			static_cast<unsigned>(cfg.CaptureTargets.size()), cfg.CaptureInterval, cfg.CaptureMaxTeams,
			cfg.CaptureRadius, cfg.CaptureEngineerMax, cfg.CaptureEnemyOwned, cfg.CaptureBuildUrgent,
			cfg.CaptureBuildIgnorePause, cfg.CaptureHonorPrereq);
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
	cfg.EconomySolvencyReserve = pINI->ReadInteger("Doctrine.Economy", "SolvencyReserve", cfg.EconomySolvencyReserve);
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
	cfg.EconomyPreventSelling = pINI->ReadBool("Doctrine.Economy", "PreventSelling", cfg.EconomyPreventSelling);
	cfg.EconomyPreventSellingAlways = pINI->ReadBool("Doctrine.Economy", "PreventSellingAlways", cfg.EconomyPreventSellingAlways);
	cfg.EconomyHonorPrereq = pINI->ReadInteger("Doctrine.Economy", "HonorPrereq", cfg.EconomyHonorPrereq);
	// ─── [Doctrine.CounterEngineer] — attack dogs vs enemy engineer rushes ──
	cfg.CounterEngEnable = pINI->ReadBool("Doctrine.CounterEngineer", "Enable", cfg.CounterEngEnable);
	pINI->ReadString("Doctrine.CounterEngineer", "Units", "", buf, sizeof(buf));
	cfg.CounterEngUnits = SplitList(buf);
	cfg.CounterEngInterval = pINI->ReadInteger("Doctrine.CounterEngineer", "Interval", cfg.CounterEngInterval);
	cfg.CounterEngRadius = pINI->ReadInteger("Doctrine.CounterEngineer", "Radius", cfg.CounterEngRadius);
	cfg.CounterEngMax = pINI->ReadInteger("Doctrine.CounterEngineer", "Max", cfg.CounterEngMax);
	cfg.CounterEngHonorPrereq = pINI->ReadInteger("Doctrine.CounterEngineer", "HonorPrereq", cfg.CounterEngHonorPrereq);
	pINI->ReadString("Doctrine.CounterEngineer", "ThreatTypes", "", buf, sizeof(buf));
	cfg.CounterEngThreatTypes = SplitList(buf);
	cfg.CounterEngThreatNone = IsNoneList(cfg.CounterEngThreatTypes);
	if (cfg.CounterEngThreatNone) cfg.CounterEngThreatTypes.clear();
	cfg.CounterEngBuild = pINI->ReadBool("Doctrine.CounterEngineer", "Build", cfg.CounterEngBuild);
	cfg.CounterEngRespond = pINI->ReadBool("Doctrine.CounterEngineer", "Respond", cfg.CounterEngRespond);
	cfg.CounterEngRespondRadius = pINI->ReadInteger("Doctrine.CounterEngineer", "RespondRadius", cfg.CounterEngRespondRadius);
	cfg.CounterEngRespondMargin = pINI->ReadDouble("Doctrine.CounterEngineer", "RespondMargin", cfg.CounterEngRespondMargin);
	cfg.CounterEngRespondMax = pINI->ReadInteger("Doctrine.CounterEngineer", "RespondMax", cfg.CounterEngRespondMax);
	cfg.CounterEngRespondLate = pINI->ReadInteger("Doctrine.CounterEngineer", "Respond.Late", cfg.CounterEngRespondLate);
	cfg.CounterEngReaction = pINI->ReadInteger("Doctrine.CounterEngineer", "ReactionFrames", cfg.CounterEngReaction);
	cfg.CounterEngRetargetDefenses = pINI->ReadBool("Doctrine.CounterEngineer", "RetargetDefenses", cfg.CounterEngRetargetDefenses);
	cfg.CounterEngPredictGoal = pINI->ReadBool("Doctrine.CounterEngineer", "Predict.Goal", cfg.CounterEngPredictGoal);
	cfg.CounterEngPredictWidth = pINI->ReadInteger("Doctrine.CounterEngineer", "Predict.Width", cfg.CounterEngPredictWidth);
	cfg.CounterEngPredictElsewhere = pINI->ReadBool("Doctrine.CounterEngineer", "Predict.Elsewhere",
		cfg.CounterEngPredictElsewhere);
	cfg.CounterEngPredictIntercept = pINI->ReadBool("Doctrine.CounterEngineer", "Predict.Intercept",
		cfg.CounterEngPredictIntercept);
	cfg.CounterEngShareDefenses = pINI->ReadBool("Doctrine.CounterEngineer", "ShareDefenses", cfg.CounterEngShareDefenses);
	cfg.CounterEngPicket = pINI->ReadBool("Doctrine.CounterEngineer", "Picket", cfg.CounterEngPicket);
	pINI->ReadString("Doctrine.CounterEngineer", "Picket.Units", "", buf, sizeof(buf));
	cfg.CounterEngPicketUnits = SplitList(buf);
	cfg.CounterEngPicketUnitsNone = IsNoneList(cfg.CounterEngPicketUnits);
	if (cfg.CounterEngPicketUnitsNone) cfg.CounterEngPicketUnits.clear();
	cfg.CounterEngPicketPosts = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.Posts", cfg.CounterEngPicketPosts);
	cfg.CounterEngPicketPerPost = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.PerPost", cfg.CounterEngPicketPerPost);
	cfg.CounterEngPicketDistance = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.Distance",
		cfg.CounterEngPicketDistance);
	cfg.CounterEngPicketMinSightings = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.MinSightings",
		cfg.CounterEngPicketMinSightings);
	cfg.CounterEngPicketMemory = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.Memory", cfg.CounterEngPicketMemory);
	cfg.CounterEngPicketTowardEnemy = pINI->ReadBool("Doctrine.CounterEngineer", "Picket.TowardEnemy",
		cfg.CounterEngPicketTowardEnemy);
	cfg.CounterEngPicketBuild = pINI->ReadBool("Doctrine.CounterEngineer", "Picket.Build", cfg.CounterEngPicketBuild);
	cfg.CounterEngPicketBuildUrgent = pINI->ReadBool("Doctrine.CounterEngineer", "Picket.Build.Urgent",
		cfg.CounterEngPicketBuildUrgent);
	cfg.CounterEngPicketInterval = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.Interval",
		cfg.CounterEngPicketInterval);
	cfg.CounterEngPicketLeash = pINI->ReadInteger("Doctrine.CounterEngineer", "Picket.Leash", cfg.CounterEngPicketLeash);
	pINI->ReadString("Doctrine.CounterEngineer", "Transports", "", buf, sizeof(buf));
	cfg.CounterEngTransports = SplitList(buf);
	cfg.CounterEngTransportsNone = IsNoneList(cfg.CounterEngTransports);
	if (cfg.CounterEngTransportsNone) cfg.CounterEngTransports.clear();
	pINI->ReadString("Doctrine.CounterEngineer", "Transports.Cargo", "", buf, sizeof(buf));
	if (auto const cargo = SplitList(buf); !cargo.empty())
		cfg.CounterEngTransportCargo = SameWord(cargo[0].c_str(), "know") ? 2
			: SameWord(cargo[0].c_str(), "assume") ? 1 : 0;                // no / none / anything else
	cfg.CounterEngTransportDenySell = pINI->ReadBool("Doctrine.CounterEngineer", "Transports.DenySell",
		cfg.CounterEngTransportDenySell);
	cfg.CounterEngDenySell = pINI->ReadBool("Doctrine.CounterEngineer", "DenySell", cfg.CounterEngDenySell);
	pINI->ReadString("Doctrine.CounterEngineer", "DenySell.Exclude", "", buf, sizeof(buf));
	cfg.CounterEngDenySellExclude = SplitList(buf);
	cfg.CounterEngDenySellLead = pINI->ReadInteger("Doctrine.CounterEngineer", "DenySell.Lead", cfg.CounterEngDenySellLead);
	cfg.CounterEngDenySellMargin = pINI->ReadDouble("Doctrine.CounterEngineer", "DenySell.Margin", cfg.CounterEngDenySellMargin);
	pINI->ReadString("Doctrine.CounterEngineer", "DenySell.Types", "", buf, sizeof(buf));
	cfg.CounterEngDenySellTypes = SplitList(buf);
	cfg.CounterEngDenySellNone = IsNoneList(cfg.CounterEngDenySellTypes);
	if (cfg.CounterEngDenySellNone) cfg.CounterEngDenySellTypes.clear();
	cfg.CounterEngDenySellPerEngineer = pINI->ReadInteger("Doctrine.CounterEngineer", "DenySell.PerEngineer", cfg.CounterEngDenySellPerEngineer);
	cfg.CounterEngDenySellMax = pINI->ReadInteger("Doctrine.CounterEngineer", "DenySell.Max", cfg.CounterEngDenySellMax);
	cfg.CounterEngDenySellWindow = pINI->ReadInteger("Doctrine.CounterEngineer", "DenySell.Window", cfg.CounterEngDenySellWindow);
	pINI->ReadString("Doctrine.CounterEngineer", "DenySell.Uncapped", "", buf, sizeof(buf));
	cfg.CounterEngDenySellUncapped = SplitList(buf);
	cfg.CounterEngDenySellUncappedNone = IsNoneList(cfg.CounterEngDenySellUncapped);
	if (cfg.CounterEngDenySellUncappedNone) cfg.CounterEngDenySellUncapped.clear();
	cfg.CounterEngDenySellKeepIrreplaceable = pINI->ReadBool("Doctrine.CounterEngineer", "DenySell.KeepIrreplaceable",
		cfg.CounterEngDenySellKeepIrreplaceable);
	cfg.CounterEngDenySellIfStronger = pINI->ReadDouble("Doctrine.CounterEngineer", "DenySell.SellIfStronger",
		cfg.CounterEngDenySellIfStronger);
	cfg.CounterEngSacrifice = pINI->ReadBool("Doctrine.CounterEngineer", "Sacrifice", cfg.CounterEngSacrifice);
	pINI->ReadString("Doctrine.CounterEngineer", "Sacrifice.Types", "", buf, sizeof(buf));
	cfg.CounterEngSacrificeTypes = SplitList(buf);
	cfg.CounterEngSacrificeNone = IsNoneList(cfg.CounterEngSacrificeTypes);
	if (cfg.CounterEngSacrificeNone) cfg.CounterEngSacrificeTypes.clear();
	cfg.CounterEngSacrificeMax = pINI->ReadInteger("Doctrine.CounterEngineer", "Sacrifice.Max", cfg.CounterEngSacrificeMax);
	cfg.CounterEngSacrificeRadius = pINI->ReadInteger("Doctrine.CounterEngineer", "Sacrifice.Radius", cfg.CounterEngSacrificeRadius);
	cfg.CounterEngSacrificeDelay = pINI->ReadInteger("Doctrine.CounterEngineer", "Sacrifice.Delay", cfg.CounterEngSacrificeDelay);
	cfg.CounterEngBuildUrgent = pINI->ReadBool("Doctrine.CounterEngineer", "Build.Urgent", cfg.CounterEngBuildUrgent);
	cfg.CounterEngBuildIgnorePause = pINI->ReadBool("Doctrine.CounterEngineer", "Build.IgnorePause", cfg.CounterEngBuildIgnorePause);
	if (cfg.CounterEngEnable)
		Echo("[DoctrineExt] [Doctrine.CounterEngineer]: Enable=1 Units=%u (empty=auto) "
			"ThreatTypes=%u (empty=Engineer=yes)%s Interval=%d Radius=%d Max=%d Build=%d Build.Urgent=%d "
			"Build.IgnorePause=%d Respond=%d RespondRadius=%d RespondMargin=%.2f RespondMax=%d "
			"Respond.Late=%d ReactionFrames=%d RetargetDefenses=%d DenySell=%d DenySell.Types=%u (empty=auto)%s "
			"DenySell.Exclude=%u DenySell.Lead=%d DenySell.Margin=%.2f DenySell.PerEngineer=%d "
			"DenySell.Max=%d DenySell.Window=%d DenySell.Uncapped=%u (empty=auto)%s "
			"DenySell.KeepIrreplaceable=%d DenySell.SellIfStronger=%.2f "
			"Sacrifice=%d Sacrifice.Types=%u (empty=auto)%s "
			"Sacrifice.Max=%d Sacrifice.Radius=%d Sacrifice.Delay=%d HonorPrereq=%d\n",
			static_cast<unsigned>(cfg.CounterEngUnits.size()),
			static_cast<unsigned>(cfg.CounterEngThreatTypes.size()), cfg.CounterEngThreatNone ? " NONE" : "",
			cfg.CounterEngInterval, cfg.CounterEngRadius, cfg.CounterEngMax, cfg.CounterEngBuild,
			cfg.CounterEngBuildUrgent, cfg.CounterEngBuildIgnorePause,
			cfg.CounterEngRespond, cfg.CounterEngRespondRadius, cfg.CounterEngRespondMargin,
			cfg.CounterEngRespondMax, cfg.CounterEngRespondLate, cfg.CounterEngReaction,
			cfg.CounterEngRetargetDefenses,
			cfg.CounterEngDenySell, static_cast<unsigned>(cfg.CounterEngDenySellTypes.size()),
			cfg.CounterEngDenySellNone ? " NONE" : "", static_cast<unsigned>(cfg.CounterEngDenySellExclude.size()),
			cfg.CounterEngDenySellLead, cfg.CounterEngDenySellMargin, cfg.CounterEngDenySellPerEngineer,
			cfg.CounterEngDenySellMax, cfg.CounterEngDenySellWindow,
			static_cast<unsigned>(cfg.CounterEngDenySellUncapped.size()),
			cfg.CounterEngDenySellUncappedNone ? " NONE" : "", cfg.CounterEngDenySellKeepIrreplaceable,
			cfg.CounterEngDenySellIfStronger, cfg.CounterEngSacrifice,
			static_cast<unsigned>(cfg.CounterEngSacrificeTypes.size()), cfg.CounterEngSacrificeNone ? " NONE" : "",
			cfg.CounterEngSacrificeMax, cfg.CounterEngSacrificeRadius, cfg.CounterEngSacrificeDelay,
			cfg.CounterEngHonorPrereq);
	if (cfg.CounterEngEnable)
		Echo("[DoctrineExt] [Doctrine.CounterEngineer]: Predict.Goal=%d Predict.Width=%d Predict.Elsewhere=%d "
			"Predict.Intercept=%d ShareDefenses=%d Picket=%d Picket.Units=%u (empty=auto)%s Picket.Posts=%d "
			"Picket.PerPost=%d Picket.Distance=%d Picket.MinSightings=%d Picket.Memory=%d Picket.TowardEnemy=%d "
			"Picket.Build=%d Picket.Build.Urgent=%d Picket.Interval=%d Transports=%u (empty=auto)%s Transports.Cargo=%s "
			"Transports.DenySell=%d\n", cfg.CounterEngPredictGoal, cfg.CounterEngPredictWidth,
			cfg.CounterEngPredictElsewhere, cfg.CounterEngPredictIntercept, cfg.CounterEngShareDefenses,
			cfg.CounterEngPicket, static_cast<unsigned>(cfg.CounterEngPicketUnits.size()),
			cfg.CounterEngPicketUnitsNone ? " NONE" : "", cfg.CounterEngPicketPosts, cfg.CounterEngPicketPerPost,
			cfg.CounterEngPicketDistance, cfg.CounterEngPicketMinSightings, cfg.CounterEngPicketMemory,
			cfg.CounterEngPicketTowardEnemy, cfg.CounterEngPicketBuild, cfg.CounterEngPicketBuildUrgent,
			cfg.CounterEngPicketInterval,
			static_cast<unsigned>(cfg.CounterEngTransports.size()), cfg.CounterEngTransportsNone ? " NONE" : "",
			cfg.CounterEngTransportCargo == 2 ? "know" : cfg.CounterEngTransportCargo == 1 ? "assume" : "no",
			cfg.CounterEngTransportDenySell);
	// ─── [Doctrine.EngineerRush] — capture enemy buildings near base ──
	cfg.EngRushEnable = pINI->ReadBool("Doctrine.EngineerRush", "Enable", cfg.EngRushEnable);
	cfg.EngRushInterval = pINI->ReadInteger("Doctrine.EngineerRush", "Interval", cfg.EngRushInterval);
	cfg.EngRushRadius = pINI->ReadInteger("Doctrine.EngineerRush", "Radius", cfg.EngRushRadius);
	pINI->ReadString("Doctrine.EngineerRush", "Targets", "", buf, sizeof(buf));
	cfg.EngRushTargets = SplitList(buf);
	pINI->ReadString("Doctrine.EngineerRush", "Exclude", "", buf, sizeof(buf));
	cfg.EngRushExclude = SplitList(buf);
	pINI->ReadString("Doctrine.EngineerRush", "Priority", "", buf, sizeof(buf));
	cfg.EngRushPriority = SplitList(buf);
	cfg.EngRushPriorityRadius = pINI->ReadInteger("Doctrine.EngineerRush", "Priority.Radius", cfg.EngRushPriorityRadius);
	cfg.EngRushPriorityBackup = pINI->ReadInteger("Doctrine.EngineerRush", "Priority.Backup", cfg.EngRushPriorityBackup);
	cfg.EngRushCripple = pINI->ReadBool("Doctrine.EngineerRush", "Cripple", cfg.EngRushCripple);
	cfg.EngRushMax = pINI->ReadInteger("Doctrine.EngineerRush", "Max", cfg.EngRushMax);
	pINI->ReadString("Doctrine.EngineerRush", "Spare", "", buf, sizeof(buf));
	if (auto const spare = SplitList(buf); !spare.empty())
		cfg.EngRushSpare = SameWord(spare[0].c_str(), "release") ? 2
			: SameWord(spare[0].c_str(), "nearest") ? 1 : 0;                  // best / anything else
	cfg.EngRushEscorts = pINI->ReadInteger("Doctrine.EngineerRush", "Escorts", cfg.EngRushEscorts);
	cfg.EngRushEscortsLeash = pINI->ReadInteger("Doctrine.EngineerRush", "Escorts.Leash", cfg.EngRushEscortsLeash);
	cfg.EngRushSafety = pINI->ReadDouble("Doctrine.EngineerRush", "Safety", cfg.EngRushSafety);
	cfg.EngRushMinCash = pINI->ReadInteger("Doctrine.EngineerRush", "MinCash", cfg.EngRushMinCash);
	cfg.EngRushRetryDelay = pINI->ReadInteger("Doctrine.EngineerRush", "RetryDelay", cfg.EngRushRetryDelay);
	cfg.EngRushBuildUrgent = pINI->ReadBool("Doctrine.EngineerRush", "Build.Urgent", cfg.EngRushBuildUrgent);
	cfg.EngRushBuildIgnorePause = pINI->ReadBool("Doctrine.EngineerRush", "Build.IgnorePause", cfg.EngRushBuildIgnorePause);
	cfg.EngRushRecapture = pINI->ReadBool("Doctrine.EngineerRush", "Recapture", cfg.EngRushRecapture);
	cfg.EngRushRecaptureUseCapture = pINI->ReadBool("Doctrine.EngineerRush", "Recapture.UseCaptureSquad",
		cfg.EngRushRecaptureUseCapture);
	cfg.EngRushHonorPrereq = pINI->ReadInteger("Doctrine.EngineerRush", "HonorPrereq", cfg.EngRushHonorPrereq);
	if (cfg.EngRushEnable)
		Echo("[DoctrineExt] [Doctrine.EngineerRush]: Enable=1 Interval=%d Radius=%d Targets=%u "
			"(empty=any) Exclude=%u Priority=%u Max=%d Escorts=%d Safety=%.2f MinCash=%d RetryDelay=%d "
			"Build.Urgent=%d Build.IgnorePause=%d Recapture=%d Recapture.UseCaptureSquad=%d HonorPrereq=%d "
			"Priority.Radius=%d Priority.Backup=%d Cripple=%d Spare=%s\n",
			cfg.EngRushInterval, cfg.EngRushRadius,
			static_cast<unsigned>(cfg.EngRushTargets.size()), static_cast<unsigned>(cfg.EngRushExclude.size()),
			static_cast<unsigned>(cfg.EngRushPriority.size()), cfg.EngRushMax, cfg.EngRushEscorts,
			cfg.EngRushSafety, cfg.EngRushMinCash, cfg.EngRushRetryDelay, cfg.EngRushBuildUrgent,
			cfg.EngRushBuildIgnorePause, cfg.EngRushRecapture, cfg.EngRushRecaptureUseCapture,
			cfg.EngRushHonorPrereq, cfg.EngRushPriorityRadius, cfg.EngRushPriorityBackup, cfg.EngRushCripple,
			cfg.EngRushSpare == 2 ? "release" : cfg.EngRushSpare == 1 ? "nearest" : "best");
	// ─── [Doctrine.Salvage] — decide before the engine's raise-money base sale ──
	cfg.SalvageEnable = pINI->ReadBool("Doctrine.Salvage", "Enable", cfg.SalvageEnable);
	cfg.SalvageBlockWithIncome = pINI->ReadBool("Doctrine.Salvage", "BlockWithIncome", cfg.SalvageBlockWithIncome);
	cfg.SalvageBlockWithoutYard = pINI->ReadBool("Doctrine.Salvage", "BlockWithoutYard", cfg.SalvageBlockWithoutYard);
	cfg.SalvageBlockWhilePlan = pINI->ReadBool("Doctrine.Salvage", "BlockWhilePlan", cfg.SalvageBlockWhilePlan);
	pINI->ReadString("Doctrine.Salvage", "Keep", "", buf, sizeof(buf));
	cfg.SalvageKeep = SplitList(buf);
	cfg.SalvageKeepNone = IsNoneList(cfg.SalvageKeep);
	if (cfg.SalvageKeepNone) cfg.SalvageKeep.clear();
	if (cfg.SalvageEnable)
		Echo("[DoctrineExt] [Doctrine.Salvage]: Enable=1 BlockWithIncome=%d BlockWithoutYard=%d "
			"BlockWhilePlan=%d Keep=%u (empty=auto)%s\n", cfg.SalvageBlockWithIncome, cfg.SalvageBlockWithoutYard,
			cfg.SalvageBlockWhilePlan, static_cast<unsigned>(cfg.SalvageKeep.size()),
			cfg.SalvageKeepNone ? " NONE" : "");
	// ─── [Doctrine.Captured] — keep or sell a building captured from another player ──
	cfg.CapturedEnable = pINI->ReadBool("Doctrine.Captured", "Enable", cfg.CapturedEnable);
	pINI->ReadString("Doctrine.Captured", "Keep", "", buf, sizeof(buf));
	cfg.CapturedKeep = SplitList(buf);
	cfg.CapturedKeepNone = IsNoneList(cfg.CapturedKeep);
	if (cfg.CapturedKeepNone) cfg.CapturedKeep.clear();
	pINI->ReadString("Doctrine.Captured", "Sell", "", buf, sizeof(buf));
	cfg.CapturedSell = SplitList(buf);
	if (IsNoneList(cfg.CapturedSell)) cfg.CapturedSell.clear();
	cfg.CapturedKeepOnlyYard = pINI->ReadBool("Doctrine.Captured", "KeepOnlyYard", cfg.CapturedKeepOnlyYard);
	cfg.CapturedKeepYard = pINI->ReadBool("Doctrine.Captured", "KeepYard", cfg.CapturedKeepYard);
	cfg.CapturedKeepYardMustHold = pINI->ReadBool("Doctrine.Captured", "KeepYard.MustHold",
		cfg.CapturedKeepYardMustHold);
	cfg.CapturedDenyLastYard = pINI->ReadBool("Doctrine.Captured", "DenyLastYard", cfg.CapturedDenyLastYard);
	cfg.CapturedKeepIfNeeded = pINI->ReadBool("Doctrine.Captured", "KeepIfNeeded", cfg.CapturedKeepIfNeeded);
	cfg.CapturedKeepNewTech = pINI->ReadBool("Doctrine.Captured", "KeepNewTech", cfg.CapturedKeepNewTech);
	cfg.CapturedHoldRadius = pINI->ReadInteger("Doctrine.Captured", "Hold.Radius", cfg.CapturedHoldRadius);
	cfg.CapturedHoldRatio = pINI->ReadDouble("Doctrine.Captured", "Hold.Ratio", cfg.CapturedHoldRatio);
	cfg.CapturedHoldEnemyBarracks = pINI->ReadInteger("Doctrine.Captured", "Hold.EnemyBarracks",
		cfg.CapturedHoldEnemyBarracks);
	cfg.CapturedDenyRecapture = pINI->ReadBool("Doctrine.Captured", "DenyRecapture", cfg.CapturedDenyRecapture);
	if (cfg.CapturedEnable)
		Echo("[DoctrineExt] [Doctrine.Captured]: Enable=1 Keep=%u (empty=auto)%s Sell=%u KeepOnlyYard=%d "
			"KeepYard=%d KeepYard.MustHold=%d DenyLastYard=%d KeepIfNeeded=%d KeepNewTech=%d Hold.Radius=%d "
			"Hold.Ratio=%.2f Hold.EnemyBarracks=%d DenyRecapture=%d\n",
			static_cast<unsigned>(cfg.CapturedKeep.size()), cfg.CapturedKeepNone ? " NONE" : "",
			static_cast<unsigned>(cfg.CapturedSell.size()), cfg.CapturedKeepOnlyYard, cfg.CapturedKeepYard,
			cfg.CapturedKeepYardMustHold, cfg.CapturedDenyLastYard,
			cfg.CapturedKeepIfNeeded, cfg.CapturedKeepNewTech, cfg.CapturedHoldRadius, cfg.CapturedHoldRatio,
			cfg.CapturedHoldEnemyBarracks, cfg.CapturedDenyRecapture);
	// ─── [Doctrine.Walls] — wall in what engineers go for, early ─────────────
	cfg.WallsEnable = pINI->ReadBool("Doctrine.Walls", "Enable", cfg.WallsEnable);
	pINI->ReadString("Doctrine.Walls", "Protect", "", buf, sizeof(buf));
	cfg.WallsProtect = SplitList(buf);
	cfg.WallsProtectNone = IsNoneList(cfg.WallsProtect);
	if (cfg.WallsProtectNone) cfg.WallsProtect.clear();
	pINI->ReadString("Doctrine.Walls", "Types", "", buf, sizeof(buf));
	cfg.WallsTypes = SplitList(buf);
	cfg.WallsTypesNone = IsNoneList(cfg.WallsTypes);
	if (cfg.WallsTypesNone) cfg.WallsTypes.clear();
	cfg.WallsRing = pINI->ReadInteger("Doctrine.Walls", "Ring", cfg.WallsRing);
	cfg.WallsUrgent = pINI->ReadBool("Doctrine.Walls", "Urgent", cfg.WallsUrgent);
	cfg.WallsNeedIncome = pINI->ReadBool("Doctrine.Walls", "NeedIncome", cfg.WallsNeedIncome);
	cfg.WallsOnSighting = pINI->ReadBool("Doctrine.Walls", "OnSighting", cfg.WallsOnSighting);
	cfg.WallsMinCash = pINI->ReadInteger("Doctrine.Walls", "MinCash", cfg.WallsMinCash);
	cfg.WallsInterval = pINI->ReadInteger("Doctrine.Walls", "Interval", cfg.WallsInterval);
	cfg.WallsRetry = pINI->ReadInteger("Doctrine.Walls", "Retry", cfg.WallsRetry);
	if (cfg.WallsEnable)
		Echo("[DoctrineExt] [Doctrine.Walls]: Enable=1 Protect=%u (empty=auto)%s Types=%u (empty=auto)%s Ring=%d "
			"Urgent=%d NeedIncome=%d OnSighting=%d MinCash=%d Interval=%d Retry=%d\n",
			static_cast<unsigned>(cfg.WallsProtect.size()), cfg.WallsProtectNone ? " NONE" : "",
			static_cast<unsigned>(cfg.WallsTypes.size()), cfg.WallsTypesNone ? " NONE" : "", cfg.WallsRing,
			cfg.WallsUrgent, cfg.WallsNeedIncome, cfg.WallsOnSighting, cfg.WallsMinCash, cfg.WallsInterval,
			cfg.WallsRetry);
	if (cfg.EconomyEnable)
		Echo("[DoctrineExt] [Doctrine.Economy]: Enable=1 Interval=%d Radius=%d IncomeBuildings=%u "
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
	Echo("[DoctrineExt] [Doctrine.General]: SenseInterval=%d RulePeriod=%d MaxTeamSize=%d MaxTeamCost=%d DebugTicks=%d DebugOrders=%d TeamTTL=%d TeamsPerHouse=%d StrictOwnership=%d AirAlertRadius=%d InterceptStandoff=%d DefendPerimeter=%d BaseEdgeMargin=%d LaneBucket=%d LaneSampleInterval=%d LaneRadius=%d LaneMinStrength=%d AceMobileOnly=%d AutoProduce=%d MaxProducePerDispatch=%d\n",
		cfg.SenseInterval, cfg.RulePeriod, cfg.MaxTeamSize, cfg.MaxTeamCost, cfg.DebugTicks, cfg.DebugOrders, cfg.TeamTTL, cfg.TeamsPerHouse, cfg.StrictOwnership, cfg.AirAlertRadius, cfg.InterceptStandoff, cfg.DefendPerimeter, cfg.BaseEdgeMargin, cfg.LaneBucket, cfg.LaneSampleInterval, cfg.LaneRadius, cfg.LaneMinStrength, cfg.AceMobileOnly, cfg.AutoProduce, cfg.MaxProducePerDispatch);
	Echo("[DoctrineExt] steering: SteerReissue=%d SquadEngineControl=%d InterceptLeash=%d DefendLeash=%d HuntLeash=%d "
		"[Doctrine.AirDefense] ScreenLeash=%d [Doctrine.CounterEngineer] Picket.Leash=%d [Doctrine.EngineerRush] "
		"Escorts.Leash=%d [Doctrine.Garrison] RedivertDelay=%d\n",
		cfg.SteerReissue, cfg.SquadEngineControl, cfg.InterceptLeash, cfg.DefendLeash, cfg.HuntLeash, cfg.AirScreenLeash,
		cfg.CounterEngPicketLeash, cfg.EngRushEscortsLeash, cfg.GarrisonRedivertDelay);

	// ─── [Doctrine.Arsenal] — key = role, value = unit IDs best-first ───
	int const roleCount = pINI->GetKeyCount("Doctrine.Arsenal");
	for (int i = 0; i < roleCount; ++i)
	{
		auto const pRawKey = pINI->GetKeyName("Doctrine.Arsenal", i);
		std::string keyName;
		if (!pRawKey || !*pRawKey || !pINI->EntryName("Doctrine.Arsenal", pRawKey, keyName))
			continue;
		auto const pKey = keyName.c_str();
		pINI->ReadString("Doctrine.Arsenal", pKey, "", buf, sizeof(buf));
		DoctrineArsenalRole role;
		role.Role = pKey;
		role.Units = SplitList(buf);
		if (role.Units.empty())
		{
			Echo("[DoctrineExt] WARNING: arsenal role %s has no units, dropped.\n", pKey);
			continue;
		}
		Echo("[DoctrineExt] arsenal %s (%u units): %s\n",
			role.Role.c_str(), role.Units.size(), buf);
		cfg.Arsenal.push_back(std::move(role));
	}

	// ─── [Doctrine.Rules] — indexed list of rule section names ──────────
	int const ruleCount = pINI->GetKeyCount("Doctrine.Rules");
	for (int i = 0; i < ruleCount; ++i)
	{
		auto const pRawKey = pINI->GetKeyName("Doctrine.Rules", i);
		std::string keyName;
		if (!pRawKey || !pINI->EntryName("Doctrine.Rules", pRawKey, keyName))
			continue;
		pINI->ReadString("Doctrine.Rules", keyName.c_str(), "", buf, sizeof(buf));
		if (!*buf)
			continue;

		DoctrineRule rule;
		rule.Name = buf;
		auto const section = rule.Name.c_str();
		if (!pINI->GetSection(section))
		{
			Echo("[DoctrineExt] WARNING: rule %s listed but section missing, dropped.\n", section);
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
			Echo("[DoctrineExt] WARNING: rule %s has unparseable When=%s, rule is inert.\n",
				section, rule.WhenRaw.c_str());

		bool roleKnown = false;
		for (auto const& role : cfg.Arsenal)
			if (role.Role == rule.Respond)
				roleKnown = true;
		if (!roleKnown)
			Echo("[DoctrineExt] WARNING: rule %s responds with unknown role %s.\n",
				section, rule.Respond.c_str());

		Echo("[DoctrineExt] rule %s: When=%s (obs=%s op=%s val=%.2f) Respond=%s Scale=%.2f Mission=%s Target=%s Cooldown=%d Priority=%d Period=%d\n",
			rule.Name.c_str(), rule.WhenRaw.c_str(), rule.WhenObs.c_str(), rule.WhenOp.c_str(),
			rule.WhenValue, rule.Respond.c_str(), rule.Scale, rule.Mission.c_str(),
			rule.Target.c_str(), rule.Cooldown, rule.Priority, rule.Period);
		cfg.Rules.push_back(std::move(rule));
	}

	Echo("[DoctrineExt] doctrine loaded: %u roles, %u rules.\n",
		cfg.Arsenal.size(), cfg.Rules.size());
}

void DoctrineConfig::EnsureParsed()
{
	if (Instance.Parsed)
		return;

	auto const pRules = CCINIClass::INI_Rules;
	if (!pRules)
		return;

	Instance.Parsed = true;
	g_echo = true;
	TagReader asWritten(pRules, -1);
	Parse(Instance, &asWritten);

	// One more set per AI difficulty: the same tags, each overridden by its "<Tag>.<Difficulty>=".
	// Only the overrides are logged.
	g_echo = false;
	for (int d = 0; d < 3; ++d)
	{
		auto& c = ByDifficulty[d];
		c = DoctrineConfig{};
		c.Parsed = true;
		TagReader variant(pRules, d);
		Parse(c, &variant);
	}
	g_echo = true;
	Active = &Instance;
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
	OrderAudit::Reset();
	DoctrineConfig::EnsureParsed();
	return 0;
}
