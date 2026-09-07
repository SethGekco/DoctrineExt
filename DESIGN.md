# DoctrineExt — design

Standalone Syringe DLL for Red Alert 2: Yuri's Revenge. The AI's reflexive
**second brain**: global, procedural, reactive combat doctrine.

Target framework: **Antares** (not Ares). Co-loads with Phobos and
AITriggerTypeExt. Deliberately **separate** from the AITriggerTypes engine —
own repo, own INI sections, no coupling to per-trigger tags.

Status: design. Nothing implemented yet.

---

## 1. Philosophy — playbook vs doctrine

Two complementary brains:

- **aimd / AITriggerTypeExt = the scripted playbook.** Pre-determined forces
  for anticipated scenarios. It knows the *specific play*: "at this tech level,
  against this faction, send this taskforce on this script."
- **DoctrineExt = doctrine.** It doesn't know the play. It knows *"when X
  happens on the field, pick the right tool and send a right-sized force."*
  Global rules, procedural forces, live sensing. Feels like a rulesmd
  `[General]`-style global config, not per-trigger tags.

The playbook covers the expected; doctrine covers the unexpected. Together
(with the wave-gen tool feeding both) the goal is an AI that feels **very
human**: it notices your ace unit, it stops feeding units into a kill zone, it
fortifies the lane you keep attacking down.

---

## 2. The core loop: sense → decide → act

Runs per AI house on a slow tick (configurable, e.g. every N frames,
staggered across houses):

1. **Sense** — read live threat: engine structures (§5) + our own tracking
   layers (§6). Produce a small set of *observations* (EnemyAirDPS, ace units,
   death zones, hot lanes, anomalies).
2. **Decide** — match observations against `[Doctrine.Rules]` (§4). For each
   firing rule: pick the role's arsenal (§3), filter to what this house can
   actually build/afford right now, and **scale the force to the threat**.
3. **Act** — build a procedural taskforce + mission and dispatch it via
   `TeamTypeClass::CreateTeam(HouseClass*)` (declared in YRpp
   `TeamTypeClass.h`; AITriggerTypeExt already hooks the engine's own
   CreateTeam dispatch loop at `0x4F8AAD/0x4F8AB2`, so the call path is
   proven). Some responses are not teams (e.g. "prioritize defenses on this
   lane" → nudge the base planner — but see §8 boundaries).

**Sync-safety is a hard requirement.** Every decision must be a deterministic
function of synced game state (same rules, same frame, same inputs on every
client). No wall-clock time, no unsynced RNG (see the KratosPP
minstd_rand desync lesson). If randomness is ever needed, use the game's
synced random stream.

---

## 3. `[Doctrine.Arsenal]` — role → best-units lists

Global role catalog, fed by the wave-gen tool's existing role-grading
(`taskforce_generator.py` → `role_weighted_grade()`, roles like ANTI_AIR /
ANTI_ARMOR / ANTI_INFANTRY / GENERAL_ATTACK; the tool will grow an export mode
that emits this section). Lists are ordered **best first**:

```ini
[Doctrine.Arsenal]
AntiAir=HTK,FLAKT,YTNK...        ; tool-graded, best first
AntiArmor=TNKD,DRON,...
AntiInfantry=DESO,...
AceHunter=SNIPE,BORIS,...         ; single-target killers
Scout=DOG,TERROR,...
```

At decide-time the DLL walks the list and takes the first units the house can
actually produce (owner/prereq/tech/veto checks — same buildability logic the
engine uses for AI production). The arsenal says *what's best*; the house's
tech state says *what's available*; the threat says *how many*.

- Roles are open-ended strings — a rule can reference any role that has an
  arsenal line. No hardcoded role enum in the DLL.
- Optional per-entry weight suffix later (`HTK:9.2`) if plain ordering
  proves too coarse. Start with ordering only.

---

## 4. `[Doctrine.Rules]` — condition → response → scale → mission

A named-section list (same pattern as every other list in rulesmd):

```ini
[Doctrine.Rules]
0=CounterAir
1=HuntAce

[CounterAir]
When=EnemyAirDPS>200          ; observation expression
Respond=AntiAir               ; arsenal role
Scale=1.5                     ; force size multiplier vs computed need
Mission=DefendBase            ; canned mission (procedural script)
Cooldown=900                  ; frames before this rule may fire again

[HuntAce]
When=EnemyUnitVeterancy>=Elite
Respond=AceHunter
Target=ThatUnit               ; bind the mission to the triggering object
Mission=HuntTarget
Scale=1.0
Cooldown=1800
```

Grammar, first cut (keep it small; grow honestly):

- **When=** `<Observation><op><value>`, ops `> >= < <= =`. One condition per
  rule at first; `And=` chaining later if real rules demand it.
- **Respond=** role name looked up in `[Doctrine.Arsenal]`.
- **Scale=** float. Need is computed per-observation (e.g. air need = enemy
  air DPS ÷ per-unit anti-air value), then × Scale, clamped by `MaxTeamCost=`
  / `MaxTeamSize=` globals.
- **Mission=** one of a small canned set the DLL renders as a procedural
  script: `DefendBase`, `HuntTarget`, `GuardLane`, `AttackZone`. Canned
  missions, procedural parameters — the playbook already covers bespoke
  scripts.
- **Target=ThatUnit** — observations that identify a specific object (ace
  unit, anomaly cluster) carry it; the mission binds to it.
- **Cooldown=** per-rule, per-house, in frames. Plus a global
  `[Doctrine.General]►RulePeriod=` for the sense tick.

Observation vocabulary at launch (each maps to a sensing source):

| Observation | Source |
|---|---|
| `EnemyAirDPS`, `EnemyArmorDPS`, ... | DPS-summing approach proven in AITriggerTypeExt |
| `OwnerZoneThreatAir/Armor/Infantry` | `HouseClass::ZoneInfos[5]` (§5) — the CounterAir gate |
| `EnemyUnitVeterancy`, `EnemyUnitKills` | kill tracking (§6.1) + engine veterancy |
| `DeathZoneScore` | death-zone heatmap (§6.2) |
| `LaneTraffic` | travel-lane heatmap (§6.3) |
| `AnomalyScore` | anomaly detector (§6.4) |

---

## 5. Engine data reused (not green-field)

Verified against the Antares-PDB symbol map
(`~/Claude/Antares/gamemd_names_from_antares_pdb.txt`) and the hook
encyclopedia registry:

| Structure / function | Where | Use |
|---|---|---|
| `HouseClass::ZoneInfos[5]` | YRpp `HouseClass.h:960` (`ZoneInfoStruct`, per-zone Aircraft/Armor/Infantry) | the engine's own live base-threat estimate; AITriggerTypeExt's `SumZoneThreat()` (Body.cpp ~1593) is the reference reader |
| `ThreatPosedEstimates[130][130]` | `ThreatPosedEstimates_GetIndex` @ `0x56BC54` (Ares+Antares bugfix-hook it) | map-wide threat field for zone/lane scoring |
| `TechnoClass::EvalThreatRating` | `0x70CF45` (head label; `Verses1` @ `0x70CEA0`) | per-object threat values, read-only |
| `CellClass::UpdateThreat` | via PDB map | cell threat maintenance, read-only |
| TechnoClass veterancy | `TechnoClass_Update_Veterancy` @ `0x6FA054` | Elite/ace detection |
| `CanBuildingTypeBePlacedHere` | via PDB map | (later, Antares-side) defense placement checks |
| `TeamTypeClass::CreateTeam` | YRpp `TeamTypeClass.h:42` | the act step |

DPS math, target-house resolution, and the buildability checks port from
AITriggerTypeExt's condition code (reference, not shared library — the two
DLLs stay independent).

---

## 6. Sensing systems (DoctrineExt's own)

All four are decaying accumulators: cheap to update, decay per tick so old
data fades, and fully serialized into savegames.

### 6.1 Kill tracking → priority targets

Hook where a kill is registered and credit the killer. The site is
`TechnoClass`'s override of `ObjectClass::RegisterDestruction` (YRpp declares
it `RX` — the no-address virtual footgun, so we hook the gamemd address, never
call it qualified). The region is **busy**: Antares hooks `0x702DD6`
(Trigger), `0x702E64` (Bounty), `0x702E9D` (Veterancy — also Ares+Kratos);
Phobos release hooks `0x702E4E`. `UnitClass` has its own override
(`0x744745` region).

- **RE-VERIFY:** the exact function-entry address of the TechnoClass override
  (the labels above are *inside* it), via objdump on gamemd + the PDB map.
- Pick an address clear of the four existing consumers, run the hook-overlap
  CI check against the encyclopedia registry, and contribute a Tier-2
  encyclopedia page for the chosen site afterwards (standing workflow).
- Data: per-TechnoClass kill count + kill value (cost of victims) in our ext
  container. A unit whose kill value crosses a threshold becomes an **ace** →
  `EnemyUnitKills` / `EnemyUnitVeterancy` observations, `Target=ThatUnit`.

### 6.2 Death zones

Same hook, victim side: when *our AI house* loses a unit, bump a coarse cell
grid (e.g. map / 8×8 cells per bucket) at the death location. High-scoring
buckets = "we keep dying here" → `DeathZoneScore` observation; missions avoid
routing through hot buckets; later feeds defense-placement hints (§8).

### 6.3 Travel lanes

Periodic sample (piggyback on the sense tick): for each foot/vehicle/aircraft
on the map, bump the movement grid at its cell if it moved since last sample.
Persistent hot buckets near our base = attack lanes → `LaneTraffic`
observation → `GuardLane` missions and (Antares-side) defense priority.

### 6.4 Anomaly detection

Statistical layer over 6.2/6.3, not a new collector: flag (a) unusually large
clusters of enemy units moving together (count within radius vs rolling
mean), (b) traffic in buckets that historically had none (path the AI never
planned for). Emits `AnomalyScore` + a location → `AttackZone`/`GuardLane`
response. This is the "the AI noticed your sneaky flank" feature.

---

## 7. Acting: procedural teams

`CreateTeam` needs a `TeamTypeClass` with a taskforce + script. DoctrineExt
builds these at runtime:

- A small pool of DLL-owned `TeamTypeClass`/`TaskForceClass`/`ScriptTypeClass`
  instances created at scenario start and **rewritten** per dispatch
  (rewrite-and-reuse avoids unbounded array growth and keeps savegame
  serialization sane — the engine already saves its type arrays).
- Taskforce = chosen units × computed count. Script = the canned mission
  rendered with parameters (waypoint/target/zone).
- **RE-VERIFY:** that the AI will fill a team whose TeamType was mutated at
  runtime (recruit vs build paths), and that nothing caches TeamType contents
  at CreateTeam time. Phase 1's first live test answers this.

---

## 8. Boundaries

| Concern | Owner |
|---|---|
| Force generation, kill tracking, reactive squads, all sensing | **DoctrineExt** |
| Scripted playbook (per-trigger conditions, wave design) | aimd + AITriggerTypeExt |
| Defense **placement** (where to build) | Antares base planner (`Hooks.BasePlan.cpp`) — an Antares-fork job; DoctrineExt only *exports* death-zone/lane scores for it |
| MCV smart-deploy | Antares territory |

---

## 9. Phase plan

- **Phase 0 — scaffold.** Repo layout per house style (YRpp + Phobos
  submodules **pinned to TechnoAttachmentExt's known-good commits**, CI
  Windows build, hook-overlap CI check wired from day one). Parse
  `[Doctrine.General]/[Doctrine.Arsenal]/[Doctrine.Rules]` from rulesmd and
  **echo every parsed value to debug.log** (INI-last-line trap: keep an EOF
  guard comment in test INIs; the echo is how we catch silent drops).
- **Phase 1 — CounterAir.** First real rule, reusing the zone-threat gate
  (`ZoneInfos` read, calibration already underway in AITriggerTypeExt's live
  test rig). Sense tick + rule engine + arsenal pick + procedural team +
  DefendBase mission. Proves the whole loop end-to-end.
- **Phase 2 — kill tracking + HuntAce.** The RegisterDestruction hook, ace
  detection, `Target=ThatUnit` missions.
- **Phase 3 — death zones + travel lanes.** Grids, decay, GuardLane; export
  scores for the future Antares placement work.
- **Phase 4 — anomaly detection.** The statistical layer.

## 10. Future doctrine families (Rex, 2026-09-01)

Two requested extensions, both natural consumers of the sensing layer:

**Construction doctrine — base building takeover.** Vanilla places defenses
blindly (a NASAM in the back corner). Wanted: placement feasibility driven by
the same sensing (§6.2 death zones, §6.3 travel lanes → defenses cover real
approach paths, maximise a defense's strategic overshadow), plus modder
control of AI build order and direct directives (build X, sell Y). Boundary
caveat from §8: Antares owns base-node planning (`Hooks.BasePlan.cpp`) — the
DoctrineExt-side approach is to *steer or veto* through downstream funnels
(`CanBuildingTypeBePlacedHere` is the placement gate) rather than replace the
planner; consult the encyclopedia for the placement-decision sites before
committing to a hook map.

**Interception & ambush (started 2026-09-01).** "Detect incoming harriers and
sally out to meet them": the `EnemyAirIncoming` observation (airborne enemy
aircraft inside `AirAlertRadius` of the base, nearest one targeted) feeds a
`Mission=Intercept` team that drives at the raid instead of guarding in
place. True *ambush* — lurking on the predicted approach lane before the
raid arrives — needs the §6.3 travel-lane heatmap (per-bearing air-approach
history → intercept waypoint on the lane); that's the follow-up. Longer term:
`[Doctrine.Personality]` profiles (per-house multipliers on Scale/thresholds/
aggression) so different AIs read as different commanders.

**Garrison doctrine.** Two parts: (1) fix the known vanilla bug where the AI
*attacks* a building it meant to garrison (needs RE of the garrison-decision
path — separate investigation); (2) a weight-and-grade system scoring every
garrisonable building: near own base, overlooking high-traffic lanes (§6.3
feeds this directly), near tech buildings, near ore deposits, near spawn
exits (enemy spawns especially) — and *low* for corner buildings near no
strategic interest. Doctrine teams then garrison the top-scored buildings.
Score = Σ(weight × factor), weights modder-tunable in a
`[Doctrine.Garrison]` section.

## 10b. Threat-aware engagement — counters, scanning, safe approach (Rex, 2026-09-06)

The vision: enemy humans camp a killer unit (or a few) in a strong spot and
farm the AI's swarms. The AI should (1) notice the killer, (2) bring the right
*counter*, and (3) approach the killer's **weak** side, not charge its
advantageous point. This is the deepest AI layer and phases on top of the
sensors already built.

**What it reuses (all verified present):**
- **Kill tracking** ([[§6.1]]) already ranks enemy units by kills/value — the
  "problem unit" is the top enemy ace near a contested area. Overlaps with:
- **Death zones (§6.2, to build)** — a pointer-safe decaying grid (mirror of
  the §6.3 lane grid) of *where this house loses its own units*. A camping
  killer shows up as a high-kill enemy unit sitting in a death-zone cluster.
  That intersection = "priority problem to solve carefully," not "feed more
  units into."
- **The engine's rock-paper-scissors data** (ground truth, no guessing):
  `WarheadTypeClass::Verses[11]` = damage multiplier vs each `Armor` class
  (None/Flak/Plate/Light/Medium/Heavy/Wood/Steel/Concrete/Special1/2);
  `ObjectTypeClass::Armor`; `WeaponTypeClass::Range/Projectile(AA,AG)/Warhead/
  Damage/ROF/Speed`.

**The engagement model:**
1. **Scan the problem unit.** Read its Armor, its weapon(s) (Range, Warhead,
   AA/AG, effective DPS), and its position.
2. **Scan around it.** Count/assess supporting enemy units within a radius and
   their bearings — this is the unit's "advantageous point" (where its support
   and its own firepower face). Cross-reference the death-zone grid for the
   killbox extent.
3. **Pick the counter from the arsenal.** Score each buildable/owned arsenal
   unit U against target T by:
   - offense = effDPS(U vs T) = U.weapon.Damage·burst/(ROF/10)·Verses[T.Armor];
   - survivability = 1 / effDPS(T vs U) (resist T's warhead vs U's armor);
   - reach = U.weapon.Range vs T.weapon.Range (out-range = free hits);
   - plus AA/AG match (can U even hit T's domain?).
   Best composite = the counter. (Roles in `[Doctrine.Arsenal]` become
   *candidate pools*; this scorer chooses within/across them per target.)
4. **Plan a safe approach.** Choose an approach bearing that (a) avoids
   death-zone buckets, (b) comes from the target's weak side (fewest
   supporters / away from its facing), and (c) exploits reach (if we out-range,
   stage at max range on the weak side). Use the lane/threat grids to route
   around the strong axis rather than through it.
5. **Commit** the counter force via the existing team/steering plumbing, with
   the approach waypoint as the steering target (same MoveDoctrineTeam loop).

**Phasing (build + test each):**
- **6a — Counter selection.** The Verses/armor/range scorer, used first by
  HuntAce to pick the arsenal unit that best counters the ace. Contained,
  testable (log the chosen counter + why). *Recommended next build.*
- **6b — Death zones.** The loss-location grid + `DeathZoneScore` observation;
  make forces avoid routing through hot death buckets.
- **6c — Weak-point approach.** Scan-around + approach-bearing planner (avoid
  death zones + target's strong axis); steer the counter force in on the weak
  side.
- **6d — Anomaly detection (§6.4).** Unusual clusters/paths on top of the grids.

Boundary: all sync-safe (integer grids, deterministic scans, no RNG); AI-only
by default ([[unit-behavior-primitive-separate-dll]] keeps the human untouched).

## 10c. Letting aimd/AITriggers use Doctrine's engine (Rex, 2026-09-07)

Question: can aimd.ini scripts use Doctrine's findings — avoid hotzones, seek
weak points, hunt high-value targets — without breaking Doctrine's independence?
Yes, and cleanly, via **custom ScriptType actions** rather than a DLL-to-DLL
dependency.

The plan: DoctrineExt registers a few new script-action numbers (above the
vanilla/Ares/Phobos range — check the encyclopedia for a free block) by hooking
the script-action execution point (TeamClass::Update script step; AITriggerTypeExt
already co-hooks TeamClass::Update 0x6E9443). New verbs, each backed by the
sensors/scorers already built:
- **AvoidDeathZone / MoveSafeToWaypoint** — route the team around hot death-zone
  buckets (§6.2 ScoreAtCell) to a waypoint.
- **ApproachWeakPoint <target-selector>** — the 6c weak-side planner: position on
  the target's weak side, avoiding death zones.
- **HuntHighValue** — retarget the team onto the current top-kill enemy ace
  (§6.1 TopEnemyAce), optionally counter-checked (6a CounterScore).
- **HoldLane** — sit on the hottest learned approach lane (§6.3).

Why this preserves independence: aimd merely *references action numbers* Doctrine
defines — the standard modder extension path (numeric actions in aimd.ini) — and
DoctrineExt owns the data + the implementation. No hard linkage; if DoctrineExt
isn't loaded the actions are simply unavailable (author around them). The wave-gen
tool would learn the new action ids and the script_validator grammar. This is the
natural bridge between the *scripted playbook* (aimd) and the *reactive doctrine*
(DoctrineExt): the playbook gains smart verbs. Candidate Phase 7.

Traps: pick action ids clear of vanilla + Ares + Phobos custom actions
(encyclopedia); the handler MUST advance the script (StepCompleted) or the team
hangs; stay sync-safe (deterministic reads, as now); AI teams only.

## 11. Standing traps that apply here

Carried over from the other Ext projects: Syringe overlapping-hook corruption
(run the range check), submodule-gitlink `git add -A` deletion, YRpp RX/R0
no-address virtuals, the Phobos co-loaded-ext `AbstractClass+0x18` trap,
engine drops the last INI line, deploy every green DLL to the RA2 folder with
backup + byte-verify, and when a crash appears diff co-loaded DLL mtimes
before blaming the newest change.
