# ToggleDisplays

Switch the Windows desktop between the notebook's internal panel and an
external display — TV, projector — without opening the Control Panel.

```
ToggleDisplays.exe            toggle: the panel is home, the external is away
ToggleDisplays.exe --to-external
ToggleDisplays.exe --to-internal
ToggleDisplays.exe --status
ToggleDisplays.exe --dry-run
```

## Why it delegates instead of doing it itself

Topology changes are handed entirely to `DisplaySwitch.exe`, the utility
Microsoft ships for exactly this. This tool never calls `SetDisplayConfig`.

**Correctness.** `DisplaySwitch` picks the external display's native mode, makes
it primary at `(0,0)`, and turns the panel off. Reimplementing that means
hand-building a display configuration, and it turns out to be harder than it
looks. A `QueryDisplayConfig(QDC_ALL_PATHS)` snapshot fed straight back into
`SetDisplayConfig` does not validate — `ERROR_INVALID_PARAMETER` — because an
all-paths snapshot is a *list of paths*, not a *topology*. The measured results
from developing this, for the record:

| configuration submitted                              | flags                    | result |
|------------------------------------------------------|--------------------------|--------|
| every available external path switched on             | `0x460` validate+changes+supplied | `ERROR_GEN_FAILURE` |
| only the first available external path switched on    | `0x460`                  | `SUCCESS` |
| only the first available external path switched on    | `0x440` validate+changes | `ERROR_INVALID_PARAMETER` |

Two things fall out of that. Enabling *every* available external path fails
where enabling one succeeds — an external target is reachable by several source
paths, and switching them all on is not a configuration Windows will accept. And
"available" is not "usable": a sleeping TV reports `targetAvailable=1`, so any
toggler that gates on that flag alone will happily disable the panel and enable
a display that cannot light up. That is how you get a black screen and a success
message.

**Safety.** `DisplaySwitch` is atomic. It either applies the whole topology
change or does nothing, so this tool can never leave the desktop on a display
that is not lit.

## Why it waits

No API can wake a powered-off TV, and many TVs sleep on their own timer — which
will interrupt a switch already in flight. So the tool asks, verifies, and
retries, treating "the display is asleep" as an expected state rather than an
error. Run `ToggleDisplays --to-external`, then switch the TV on or press any
button on its remote, and the rest happens by itself.

## How it verifies

Read-only, through the Windows CCD API. Internal versus external is decided by
output technology — `INTERNAL` / `LVDS` / `DISPLAYPORT_EMBEDDED` is the panel;
`HDMI` / `DVI` / external DisplayPort is external. That is a property of the
hardware, not of enumeration order, which is what keeps it correct on a laptop
with two GPUs and several connectors.

One trap worth recording, because the first build of this tool fell into it:
**availability must be counted from `QDC_ALL_PATHS`.** An inactive target does
not appear in a `QDC_ONLY_ACTIVE_PATHS` result at all, so counting it from the
active-paths view is structurally guaranteed to report zero.

## Options

| option | effect |
|---|---|
| *(none)* | toggle |
| `--to-external` | wait for an external display, then show only that |
| `--to-internal` | go back to the internal panel only |
| `--status` | print the current topology and exit |
| `--dry-run` | say what a toggle would do, change nothing |
| `--timeout N` | seconds to keep trying (default 90) |
| `--verbose` | print the topology after every attempt |
| `--quiet` | no output at all |
| `--help` | usage |

Exit codes: `0` the requested topology is in place, `1` gave up and nothing was
changed, `2` bad arguments.

## Toggle direction

The panel is home and the external display is away. If anything external is
currently lit, a toggle goes home; otherwise it goes out to the TV. One
consequence worth knowing: from an *extended* desktop, one press collapses to
the panel rather than jumping to the TV. Use `--to-external` when you want the
TV specifically rather than "not here".

## Build

```
build.cmd
```

or directly:

```
cl /nologo /EHsc /std:c++17 /W3 /O2 /Fe:ToggleDisplays.exe ToggleDisplays.cpp user32.lib
```

Requires the Visual C++ tools and `user32.lib` — `build.cmd` locates Visual
Studio via `vswhere`, so the version does not have to be hard-coded. Output
lands in the repository root and is git-ignored.

## Requirements

Windows 7 or later (the CCD API and `DisplaySwitch.exe`). No runtime
dependencies beyond `user32.dll`. Nothing is installed and nothing is
registered.
