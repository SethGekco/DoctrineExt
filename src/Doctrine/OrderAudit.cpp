#include "Doctrine/OrderAudit.h"

#include <HouseClass.h>
#include <FootClass.h>
#include <InfantryClass.h>
#include <UnitClass.h>
#include <TeamClass.h>
#include <TeamTypeClass.h>
#include <TechnoTypeClass.h>
#include <Unsorted.h>
#include <Utilities/Debug.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace
{
	// What changed: the mission, the destination (moved 3+ cells, or set/cleared — the engine nudges it along a
	// route, FootClass::Destination is volatile), the team (recruited/liberated) or the garrison flags. Not the
	// target: units in a fight retarget all the time.
	enum : int { kMission = 1, kDest = 2, kTeam = 4, kGarrison = 8 };

	struct State
	{
		int Mission = -1;
		bool HasDest = false;
		CoordStruct Dest {};
		TeamClass* Team = nullptr;
		bool Garrison = false;
	};
	struct Event { int Frame; const char* Step; int What; };
	struct Track { State Last; std::deque<Event> Events; int Logged = -100000; };

	int const kWindow = 300;
	int const kMinEvents = 6;

	std::map<int, std::map<DWORD, Track>> g_tracks;                 // house -> unit UID -> track

	State Read(FootClass* const p)
	{
		State s;
		s.Mission = static_cast<int>(p->GetCurrentMission());
		s.HasDest = p->Destination != nullptr;
		if (s.HasDest) s.Dest = p->Destination->GetCoords();
		s.Team = p->Team;
		s.Garrison = p->ShouldEnterOccupiable || p->ShouldGarrisonStructure;
		return s;
	}

	int Diff(State const& a, State const& b)
	{
		int what = 0;
		if (a.Mission != b.Mission) what |= kMission;
		if (a.HasDest != b.HasDest
			|| (a.HasDest && std::hypot(double(a.Dest.X - b.Dest.X), double(a.Dest.Y - b.Dest.Y)) >= 3 * 256.0))
			what |= kDest;
		if (a.Team != b.Team) what |= kTeam;
		if (a.Garrison != b.Garrison) what |= kGarrison;
		return what;
	}

	std::string WhatList(int const what)
	{
		std::string s;
		auto const add = [&](int const bit, const char* const name)
			{ if (what & bit) { if (!s.empty()) s += ','; s += name; } };
		add(kMission, "mission");
		add(kDest, "dest");
		add(kTeam, "team");
		add(kGarrison, "garrison");
		return s;
	}

	// "Teams::GarrisonDoctrine(pHouse)" -> "GarrisonDoctrine".
	std::string StepName(const char* const step)
	{
		std::string s(step);
		auto const paren = s.find('(');
		if (paren != std::string::npos) s.resize(paren);
		auto const colon = s.rfind(':');
		return colon == std::string::npos ? s : s.substr(colon + 1);
	}

	void Note(HouseClass* const pHouse, FootClass* const pF, Track& t, const char* const step, int const what,
		int const now)
	{
		t.Events.push_back({ now, step, what });
		while (!t.Events.empty() && now - t.Events.front().Frame > kWindow) t.Events.pop_front();
		if (static_cast<int>(t.Events.size()) < kMinEvents || now - t.Logged < kWindow) return;
		std::map<const char*, std::pair<int, int>> by;                  // step -> {count, what}
		for (auto const& e : t.Events) { auto& b = by[e.Step]; ++b.first; b.second |= e.What; }
		int sources = 0;
		for (auto const& kv : by) if (kv.second.first >= 2) ++sources;
		if (sources < 2) return;
		t.Logged = now;
		std::string list;
		for (auto const& kv : by)
		{
			char part[160];
			std::snprintf(part, sizeof(part), "%s%s %dx (%s)", list.empty() ? "" : ", ",
				kv.first ? StepName(kv.first).c_str() : "engine", kv.second.first, WhatList(kv.second.second).c_str());
			list += part;
		}
		auto const pType = pF->GetTechnoType();
		Debug::Log("[DoctrineExt] orders churn: house=%s#%d %s#%u team=%s in %df: %s; now mission=%d dest=%s "
			"target=%s.\n", pHouse->get_ID(), pHouse->ArrayIndex, pType ? pType->ID : "?", pF->UniqueID,
			pF->Team && pF->Team->Type ? pF->Team->Type->ID : "-", kWindow, list.c_str(),
			static_cast<int>(pF->GetCurrentMission()), pF->Destination ? "set" : "none", pF->Target ? "set" : "none");
	}

	// Snapshot the house's live infantry and vehicles; step == nullptr is the engine (since the last tick).
	void Snapshot(HouseClass* const pHouse, const char* const step)
	{
		int const now = Unsorted::CurrentFrame;
		auto& tracks = g_tracks[pHouse->ArrayIndex];
		std::set<DWORD> seen;
		auto const visit = [&](FootClass* const pF)
		{
			if (!pF || pF->Owner != pHouse || pF->InLimbo || pF->Health <= 0) return;
			seen.insert(pF->UniqueID);
			auto const s = Read(pF);
			auto const it = tracks.find(pF->UniqueID);
			if (it == tracks.end()) { tracks[pF->UniqueID].Last = s; return; }
			int const what = Diff(it->second.Last, s);
			it->second.Last = s;
			if (what) Note(pHouse, pF, it->second, step, what, now);
		};
		for (auto const pI : InfantryClass::Array) visit(pI);
		for (auto const pU : UnitClass::Array) visit(pU);
		if (!step)                                                      // dead, captured or in limbo
			for (auto it = tracks.begin(); it != tracks.end(); )
				it = seen.count(it->first) ? std::next(it) : tracks.erase(it);
	}
}

void OrderAudit::Begin(HouseClass* const pHouse)
{
	if (pHouse) Snapshot(pHouse, nullptr);
}

void OrderAudit::After(HouseClass* const pHouse, const char* const step)
{
	if (pHouse) Snapshot(pHouse, step);
}

void OrderAudit::Reset()
{
	g_tracks.clear();
}
