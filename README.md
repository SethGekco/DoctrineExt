# DoctrineExt

Standalone Syringe DLL for Red Alert 2: Yuri's Revenge — the AI's reflexive
**second brain**. Where aimd/AITriggerTypeExt is the scripted playbook
(pre-determined forces for anticipated scenarios), DoctrineExt is **doctrine**:
global, procedural, reactive. It senses the field (threat, kills, death zones,
travel lanes, anomalies), picks the right tool from a role arsenal, and sends
a right-sized force.

See [DESIGN.md](DESIGN.md) for the full design.

## INI surface (rulesmd.ini)

```ini
[Doctrine.General]
RulePeriod=150        ; frames between sense ticks
MaxTeamSize=12
MaxTeamCost=10000

[Doctrine.Arsenal]
AntiAir=HTK,FLAKT   ; role -> unit IDs, best first

[Doctrine.Rules]
0=CounterAir

[CounterAir]
When=EnemyAirDPS>200
Respond=AntiAir
Scale=1.5
Mission=DefendBase
Cooldown=900
; EOF guard — the engine drops the last INI line
```

### Per AI difficulty

Every tag can also be written as `<Tag>.Easy=`, `<Tag>.Normal=` or `<Tag>.Hard=`.
That value replaces `<Tag>=` for AI houses of that difficulty only:

```ini
[Doctrine.Capture]
Enable=yes
Enable.Hard=no            ; Hard AIs don't capture; Easy and Normal do

[Doctrine.CounterEngineer]
DenySell.Types.Easy=none  ; Easy AIs never sell to deny a capture
```

- Easy, Normal and Hard are the lobby's Easy, Medium and Brutal AI
  (`HouseClass::AIDifficulty` 2, 1, 0).
- In `[Doctrine.Arsenal]` and `[Doctrine.Rules]`, `AntiAir.Hard=` overrides a
  role or line for Hard AIs. A variant whose plain key doesn't exist adds the
  entry for that difficulty only.
- The same works inside each rule's own section (`When.Easy=`, `Scale.Hard=` …).
- Map-wide trackers read the plain tags, because every house shares them:
  the lane and death-zone heatmaps (`Lane*` / `DeathZone*` in
  `[Doctrine.General]`) and the AITrigger gate (`AITriggerDelay`,
  `CrateChaserGateCount`).
- Each override is logged once per game: `[DoctrineExt] Hard AI: [Doctrine.Capture] Enable.Hard=no`.

## Status

Phase 0: scaffold — parses the `[Doctrine.*]` sections and echoes every value
to `debug.log`. Nothing is evaluated yet.

## Build

CI builds `DevBuild` via MSBuild (see `.github/workflows/build.yml`). The
workflow also range-checks our hooks against the
[YR Hook Encyclopedia](https://github.com/SethGekco/YR-Hook-Encyclopedia)
registry before building.

Submodules: YRpp (`phobos-dev`) and Phobos (`develop`), pinned to known-good
commits — clone with `--recurse-submodules`.

Co-loads with Antares, Phobos, and AITriggerTypeExt.
