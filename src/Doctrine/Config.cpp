#include "Doctrine/Config.h"
#include "Doctrine/Engine.h"
#include "Doctrine/Teams.h"
#include "Doctrine/KillTracker.h"

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
	cfg.AceMobileOnly = pINI->ReadBool("Doctrine.General", "AceMobileOnly", cfg.AceMobileOnly);
	cfg.AutoProduce = pINI->ReadBool("Doctrine.General", "AutoProduce", cfg.AutoProduce);
	cfg.MaxProducePerDispatch = pINI->ReadInteger("Doctrine.General", "MaxProducePerDispatch", cfg.MaxProducePerDispatch);
	Debug::Log("[DoctrineExt] [Doctrine.General]: SenseInterval=%d RulePeriod=%d MaxTeamSize=%d MaxTeamCost=%d DebugTicks=%d TeamTTL=%d TeamsPerHouse=%d StrictOwnership=%d AirAlertRadius=%d InterceptStandoff=%d AceMobileOnly=%d AutoProduce=%d MaxProducePerDispatch=%d\n",
		cfg.SenseInterval, cfg.RulePeriod, cfg.MaxTeamSize, cfg.MaxTeamCost, cfg.DebugTicks, cfg.TeamTTL, cfg.TeamsPerHouse, cfg.StrictOwnership, cfg.AirAlertRadius, cfg.InterceptStandoff, cfg.AceMobileOnly, cfg.AutoProduce, cfg.MaxProducePerDispatch);

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
	DoctrineConfig::EnsureParsed();
	return 0;
}
