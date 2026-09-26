# ToggleDisplays

Switch the Windows desktop between the notebook's internal panel and an
external display — TV, projector — without opening the Control Panel.

```
ToggleDisplays.exe                     toggle: the panel is home, the external is away
ToggleDisplays.exe --to-external       wait for an external display, then show only it
ToggleDisplays.exe --to-external TOSHIBA   ...and require that it is that display
ToggleDisplays.exe --to-internal       go back to the internal panel only
ToggleDisplays.exe --status            print the current topology and exit
ToggleDisplays.exe --list-externals    list external targets by EDID name
ToggleDisplays.exe --dry-run           say what a toggle would do, change nothing
ToggleDisplays.exe --probe             validate-only display-config probe
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

### How far those numbers actually go

Each cell above is a single run, `ERROR_GEN_FAILURE` is a generic code, and all
of it was measured against a *sleeping* TV — so a validate success is not a
promise that an apply would produce a visible desktop. Two further limits worth
stating, because this area attracted a wrong conclusion during development:

- The `0x440` row fails on **every** configuration tried, including the one that
  succeeds at `0x460`. So `SDC_USE_SUPPLIED_DISPLAY_CONFIG` is *not* removable.
  An earlier draft of this file argued for dropping it; that was wrong, and the
  measurement contradicts it. What the numbers support is narrower: when you
  supply your own path array without that flag, the mode array has to match the
  buffer size for those paths, and a `QDC_ALL_PATHS` mode array is too large.
- One failing row in the table is a bug in the probe rather than a fact about
  Windows, so treat it as absent. The workable recipe is the narrow one: start
  from `QDC_ONLY_ACTIVE_PATHS` and add exactly one external source path.

The reason this tool delegates is not that a direct implementation is impossible
— it is that the direct route has a large fiddly surface, and `DisplaySwitch.exe`
is Microsoft's own, already-correct, already-shipped answer to the same problem.

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

No code anywhere keys off an array index as an identifier. Paths are always
walked in a counted loop and classified by technology, availability and EDID
target name. Where a name is needed it comes from
`DISPLAYCONFIG_TARGET_DEVICE_NAME` via `DisplayConfigGetDeviceInfo`, and the
`friendlyNameFromEdid` flag is carried through to the output, because Windows
synthesises a placeholder such as `Generic PnP Monitor` when a display reports
no usable EDID. Matching on that placeholder would let the tool claim to have
found a particular television when it had found nothing of the sort. On the
development machine:

```
$ ToggleDisplays.exe --list-externals
  target 198147    HDMI    available    Toshiba TV
  target 200715    HDMI    unavailable  (unnamed)   (synthesised - no usable EDID)
```

### Paths are not targets

That listing is the other thing worth recording, because it corrected a number
this project had been reporting all along. The TV is reachable through **three**
source paths but is **one** physical target, so counting paths reports three
external displays where there is one. The tool now de-duplicates on
`(adapterId, targetId)` and reports `2` — the Toshiba, plus a second
unavailable, unnamed target. Earlier revisions said "3 targets available" and
were counting paths.

### Two traps this tool fell into first

**Availability must be counted from `QDC_ALL_PATHS`.** An inactive target does
not appear in a `QDC_ONLY_ACTIVE_PATHS` result at all, so counting it from the
active-paths view is structurally guaranteed to report zero. The first build
shipped exactly that bug.

**`vswhere` excludes prerelease channels by default.** On a machine whose only
toolchain is an Insiders or Preview build, a plain `-latest` query matches
nothing and the build fails with a misleading "no C++ tools" message even though
`cl.exe` is sitting right there. `build.cmd` passes `-prerelease` and falls back
to looking for `VsDevCmd.bat` directly.

## Targeting a specific display

`--to-external TOSHIBA` is a **verification constraint, not a selector.**
`DisplaySwitch.exe` cannot be told which external display to prefer — Windows
chooses. Naming one means "switch, and require that this is the display that
came up", and a mismatch is reported as a failure rather than quietly accepted.
The check is refused up front, before anything is switched, when no EDID name
matches at all.

## `--probe`

`--probe` calls `SetDisplayConfig` with `SDC_VALIDATE` and **never** passes
`SDC_APPLY`, so no configuration is committed. "We omit the apply flag" is an
argument rather than a guarantee, so there is a structural bound as well: the
internal panel's path is force-kept `ACTIVE` in the submitted array. Nothing
under test concerns the panel, so keeping it lit does not affect the
measurement, and it bounds the worst case to "nothing happens" rather than "the
panel goes dark". There is also a runtime assertion that refuses to run if the
flag set ever grows an `SDC_APPLY` bit.

It submits the one recipe that actually validated: the `QDC_ALL_PATHS` snapshot
with the panel `ACTIVE`, exactly **one** available external path `ACTIVE`, and
unavailable targets deactivated. On the development machine it returns
`ERROR_SUCCESS`, matching the standalone probe it was ported from, and the
display topology is byte-identical before and after.

What it cannot tell you: the TV was asleep throughout, so a validate success is
not a promise that an apply would produce a visible desktop.

## Scripting

`--status --json` and `--list-externals --json` emit a single-line JSON object
for consumption by other tools. Both were checked by parsing the output, not by
eyeballing it.

## Options

| option | effect |
|---|---|
| *(none)* | toggle |
| `--to-external [NAME]` | wait for an external display, then show only it; if `NAME` is given, require that display |
| `--to-internal` | go back to the internal panel only |
| `--status` | print the current topology and exit |
| `--list-externals` | list external targets by EDID name |
| `--dry-run` | say what a toggle would do, change nothing |
| `--probe` | validate-only display-config probe |
| `--json` | machine-readable output, with `--status` or `--list-externals` |
| `--timeout N` | seconds to keep trying (default 90) |
| `--verbose` | print the topology after every attempt |
| `--quiet` | no output at all |
| `--help` | usage |

Exit codes: `0` the requested topology is in place (or `--probe` validated),
`1` gave up / rejected and nothing was changed, `2` bad arguments, `3` `--probe`
found no external target to validate against.

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
cl /nologo /EHsc /std:c++17 /W4 /WX /O2 /Fe:ToggleDisplays.exe ToggleDisplays.cpp user32.lib
```

Requires the Visual C++ tools and `user32.lib` — `build.cmd` locates Visual
Studio via `vswhere`, so the version does not have to be hard-coded. The code
is warning-clean at `/W4` and builds with `/WX`, so a warning fails the build
rather than scrolling past. Output lands in the repository root and is
git-ignored.

## Requirements

Windows 7 or later (the CCD API and `DisplaySwitch.exe`). No runtime
dependencies beyond `user32.dll`. Nothing is installed and nothing is
registered.

## License

MIT — see [LICENSE](LICENSE). The file was added by GitHub when the repository
was created and is unrelated to the code commits.
