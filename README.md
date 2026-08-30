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
AntiAir=FLAKTRK,SAM   ; role -> unit IDs, best first

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
