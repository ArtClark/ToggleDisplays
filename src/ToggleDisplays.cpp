/*
 * ToggleDisplays - switch the Windows desktop between the notebook's internal
 * panel and an external display (TV, projector) without the Control Panel.
 *
 * Run it with no arguments and it toggles: the panel is "home", the external
 * display is "away", and each run moves you between them.
 *
 * WHY IT DELEGATES THE MUTATION
 *   Topology changes are handed entirely to DisplaySwitch.exe, Microsoft's own
 *   implementation. This tool never calls SetDisplayConfig. Two reasons:
 *
 *     1. Correctness. DisplaySwitch picks the external display's native mode,
 *        makes it primary at (0,0), and turns the panel off. Reimplementing
 *        that means hand-building a display configuration. Measured on the
 *        development machine, feeding a QueryDisplayConfig(QDC_ALL_PATHS)
 *        snapshot straight back into SetDisplayConfig does not validate -
 *        ERROR_INVALID_PARAMETER - because an all-paths snapshot is not a valid
 *        input configuration. It is a list of paths, not a topology.
 *
 *     2. Safety. DisplaySwitch is atomic: it either applies the whole topology
 *        change or does nothing. So this tool can never leave the desktop on a
 *        display that is not lit. A hand-rolled toggler invites exactly that
 *        failure - enable an external target that reports targetAvailable=1
 *        while the TV is asleep, disable the panel, report success, and leave
 *        a black screen. That is not hypothetical: a sleeping TV reports
 *        targetAvailable=1, so "available" is not "can be lit".
 *
 * WHY IT WAITS
 *   No API can wake a powered-off TV. Many TVs also sleep on their own timer,
 *   which will interrupt a switch that is already in flight. So the tool asks,
 *   verifies, and retries, treating "the display is asleep" as an expected
 *   state rather than an error. If you switch the TV on while it waits, the
 *   rest happens by itself.
 *
 * HOW IT VERIFIES
 *   Read-only, through the CCD API. Internal versus external is decided by
 *   output technology - INTERNAL / LVDS / DISPLAYPORT_EMBEDDED is the panel;
 *   HDMI / DVI / external DisplayPort is external. That is a property of the
 *   hardware, not of enumeration order, which is what keeps this correct on a
 *   laptop with two GPUs and several connectors. Note that availability must
 *   be counted from QDC_ALL_PATHS: an inactive target is absent from a
 *   QDC_ONLY_ACTIVE_PATHS result, so counting it from the active view always
 *   reports zero.
 *
 * USAGE
 *   ToggleDisplays                     toggle (default)
 *   ToggleDisplays --to-external       wait for an external display, show only it
 *   ToggleDisplays --to-internal       go back to the internal panel only
 *   ToggleDisplays --status            print the current topology and exit
 *   ToggleDisplays --dry-run           say what a toggle would do, change nothing
 *   ToggleDisplays --timeout N         seconds to keep trying (default 90)
 *   ToggleDisplays --verbose           print the topology after every attempt
 *   ToggleDisplays --quiet             no output at all
 *   ToggleDisplays --help
 *
 * EXIT CODES
 *   0  the requested topology is in place
 *   1  gave up; nothing was changed
 *   2  bad arguments
 *
 * Build (x64), or just run build.cmd:
 *   cl /nologo /EHsc /std:c++17 /W3 /O2 /Fe:ToggleDisplays.exe ^
 *      ToggleDisplays.cpp user32.lib
 */

#define WINVER       0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

#define DEFAULT_TIMEOUT_SEC 90
#define POLL_MS             400
#define SETTLE_MS        6000   /* how long to watch for the change after each ask */

static bool g_quiet   = false;
static bool g_verbose = false;

/* ------------------------------------------------------------------ output */

static void out(const wchar_t* fmt, ...)
{
    if (g_quiet) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    fwprintf(stdout, L"%s\n", buf);
    fflush(stdout);
}

/*
 * Failure reporting goes to the console only.
 *
 * An earlier revision popped a modal MessageBoxW on failure. That was wrong on
 * two counts: a modal dialog nobody dismisses is itself a hang, and it competes
 * with the console window already carrying live progress. The console is the
 * whole UI. Use --quiet when something else owns the notification.
 */
static void fail(const wchar_t* msg)
{
    fwprintf(stderr, L"\nFAILED\n");
    fwprintf(stderr, L"%s\n", msg);
    fflush(stderr);
}

/* -------------------------------------------------------------- topology -- */

static bool isInternalTech(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
{
    return t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL
        || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS
        || t == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED;
}

static const wchar_t* techLabel(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY t)
{
    switch (t) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:             return L"internal panel";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:                 return L"internal panel (LVDS)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"internal panel (eDP)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:                 return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:                  return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return L"DisplayPort (USB)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:             return L"MiraCast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:       return L"indirect (wired)";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:     return L"indirect (virtual)";
    default:                                                    return L"external";
    }
}

struct Topology {
    int  internalActive    = 0;   /* ACTIVE internal paths  */
    int  externalActive    = 0;   /* ACTIVE external paths  */
    int  externalAvailable = 0;   /* available external targets, active or not */
    int  attachedToDesktop = 0;   /* EnumDisplayDevices entries attached */
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY activeExternalTech =
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
};

static int countAttachedDisplays()
{
    int n = 0;
    for (UINT32 i = 0; ; ++i) {
        DISPLAY_DEVICEA dd;
        ZeroMemory(&dd, sizeof(dd));
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesA(NULL, i, &dd, 0)) break;
        if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) ++n;
    }
    return n;
}

/*
 * Available-but-inactive external targets.
 *
 * This MUST use QDC_ALL_PATHS. An inactive target does not appear in a
 * QDC_ONLY_ACTIVE_PATHS result at all, so counting availability from the
 * active-paths view is structurally guaranteed to report zero - which is the
 * bug the first build of this tool shipped. Measured on the development
 * machine: the sleeping TV reports targetAvailable=1 on three paths, while
 * the active-paths view can only ever show the one internal path.
 */
static int countAvailableExternalTargets()
{
    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc) != ERROR_SUCCESS) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pc, paths.data(), &mc, modes.data(), NULL) != ERROR_SUCCESS) return 0;

    int n = 0;
    for (UINT32 i = 0; i < pc; ++i) {
        if (isInternalTech(paths[i].targetInfo.outputTechnology)) continue;
        if (paths[i].targetInfo.targetAvailable) ++n;
    }
    return n;
}

/* Read-only. Never changes anything. */
static Topology queryTopology()
{
    Topology t;
    t.attachedToDesktop = countAttachedDisplays();
    t.externalAvailable = countAvailableExternalTargets();

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS) return t;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths.data(), &mc, modes.data(), NULL) != ERROR_SUCCESS)
        return t;

    for (UINT32 i = 0; i < pc; ++i) {
        if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        if (isInternalTech(paths[i].targetInfo.outputTechnology)) ++t.internalActive;
        else {
            ++t.externalActive;
            t.activeExternalTech = paths[i].targetInfo.outputTechnology;
        }
    }
    return t;
}

static void reportTopology(const wchar_t* when)
{
    const Topology t = queryTopology();
    out(L"%s", when);
    out(L"    internal panel : %s (%d active path%s)",
        t.internalActive ? L"ON " : L"off", t.internalActive, t.internalActive == 1 ? L"" : L"s");
    out(L"    external       : %s (%d active path%s, %d target%s available)",
        t.externalActive ? L"ON " : L"off", t.externalActive, t.externalActive == 1 ? L"" : L"s",
        t.externalAvailable, t.externalAvailable == 1 ? L"" : L"s");
    if (t.externalActive)
        out(L"    external is    : %s", techLabel(t.activeExternalTech));
    out(L"    displays attached to the desktop: %d", t.attachedToDesktop);
}

static bool isExternalOnly() { const Topology t = queryTopology(); return t.internalActive == 0 && t.externalActive > 0; }
static bool isInternalOnly() { const Topology t = queryTopology(); return t.internalActive > 0 && t.externalActive == 0; }

/* ---------------------------------------------------------------- toggle -- */

enum class Target { ToExternal, ToInternal };

/*
 * The panel is home; the external display is away. If anything external is
 * currently lit, a toggle goes home, otherwise it goes out to the TV.
 * Consequence: from an extended desktop, one press collapses to the panel.
 * Use --to-external when you want the TV specifically rather than "not here".
 */
static Target resolveToggleTarget(const Topology& t)
{
    return t.externalActive > 0 ? Target::ToInternal : Target::ToExternal;
}

static const wchar_t* targetName(Target t)
{
    return t == Target::ToExternal ? L"the external display only" : L"the internal panel only";
}

/* --------------------------------------------------------------- the ask -- */

static bool runDisplaySwitch(const wchar_t* mode)
{
    wchar_t cmd[128];
    swprintf_s(cmd, L"\"C:\\Windows\\System32\\DisplaySwitch.exe\" /%s", mode);
    STARTUPINFOW si;  ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    out(L"  asking DisplaySwitch.exe /%s ...", mode);
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        out(L"  could not launch DisplaySwitch.exe (error %lu)", GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* Watch for the requested topology to appear. */
static bool waitFor(bool (*predicate)(), int millis)
{
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)millis;
    for (;;) {
        if (predicate()) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(POLL_MS);
    }
}

/* ------------------------------------------------------------------ main -- */

static void usage()
{
    fwprintf(stdout,
        L"ToggleDisplays - switch between the internal panel and an external\n"
        L"                 display, without the Control Panel.\n\n"
        L"  (no arguments)   toggle: the panel is home, the external is away\n"
        L"  --to-external    wait for an external display, then show only that\n"
        L"  --to-internal    go back to the internal panel only\n"
        L"  --status         print the current topology and exit\n"
        L"  --dry-run        say what a toggle would do, change nothing\n"
        L"  --timeout N      seconds to keep trying (default %d)\n"
        L"  --verbose        print the topology after every attempt\n"
        L"  --quiet          no output at all\n"
        L"  --help\n\n"
        L"No API can wake a powered-off TV, so --to-external waits for one to\n"
        L"appear: switch the TV on, or press any button on its remote, while it\n"
        L"is waiting and the rest happens automatically.\n",
        DEFAULT_TIMEOUT_SEC);
}

int wmain(int argc, wchar_t** argv)
{
    int  timeout = DEFAULT_TIMEOUT_SEC;
    bool explicitExternal = false, explicitInternal = false;
    bool statusOnly = false, dryRun = false;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!_wcsicmp(a, L"--to-external") || !_wcsicmp(a, L"--to-tv"))       explicitExternal = true;
        else if (!_wcsicmp(a, L"--to-internal") || !_wcsicmp(a, L"--to-panel")) explicitInternal = true;
        else if (!_wcsicmp(a, L"--status") || !_wcsicmp(a, L"--list"))         statusOnly = true;
        else if (!_wcsicmp(a, L"--dry-run") || !_wcsicmp(a, L"--what-if"))      dryRun = true;
        else if (!_wcsicmp(a, L"--quiet"))     g_quiet = true;
        else if (!_wcsicmp(a, L"--verbose"))   g_verbose = true;
        else if (!_wcsicmp(a, L"--timeout") && i + 1 < argc) timeout = _wtoi(argv[++i]);
        else if (!_wcsicmp(a, L"--help") || !_wcsicmp(a, L"-h") || !_wcsicmp(a, L"/?")) { usage(); return 0; }
        else { fwprintf(stderr, L"unknown option: %s\n\n", a); usage(); return 2; }
    }

    if (explicitExternal && explicitInternal) {
        fwprintf(stderr, L"pick one of --to-external or --to-internal\n");
        return 2;
    }
    if (timeout < 1) timeout = 1;
    if (!statusOnly && !g_quiet && AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();

    if (statusOnly) {
        reportTopology(L"current topology:");
        return 0;
    }

    const Topology before = queryTopology();
    const Target target = explicitExternal ? Target::ToExternal
                         : explicitInternal ? Target::ToInternal
                         : resolveToggleTarget(before);

    reportTopology(L"before:");

    if (dryRun) {
        out(L"dry run - nothing was changed.");
        out(L"a toggle right now would switch to %s.", targetName(target));
        return 0;
    }

    if (target == Target::ToInternal) {
        if (isInternalOnly()) { out(L"already showing %s. Nothing to do.", targetName(target)); return 0; }
        SetConsoleTitleW(L"ToggleDisplays - switching to the internal panel");
        runDisplaySwitch(L"internal");
        if (waitFor(isInternalOnly, SETTLE_MS)) {
            reportTopology(L"after:");
            out(L"done - showing %s.", targetName(target));
            return 0;
        }
        fail(L"Could not switch back to the internal panel.\n"
             L"DisplaySwitch.exe did not produce the expected topology.");
        return 1;
    }

    /* ---- to the external display: this is the one that has to wait ---- */
    if (isExternalOnly()) { out(L"already showing %s. Nothing to do.", targetName(target)); return 0; }

    SetConsoleTitleW(L"ToggleDisplays - waiting for the external display");

    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout * 1000ULL;
    int  attempt = 0;
    bool toldAboutSleep = false;

    for (;;) {
        ++attempt;
        if (attempt > 1) out(L"  still not there - asking again (attempt %d)", attempt);
        runDisplaySwitch(L"external");

        if (waitFor(isExternalOnly, SETTLE_MS)) {
            reportTopology(L"after:");
            out(L"done - showing %s.", targetName(target));
            SetConsoleTitleW(L"ToggleDisplays - done");
            return 0;
        }
        if (g_verbose) reportTopology(L"    state after this attempt:");

        const Topology t = queryTopology();
        const bool nothingToTalkTo = (t.externalActive == 0 && t.attachedToDesktop <= 1);

        if (nothingToTalkTo && !toldAboutSleep) {
            toldAboutSleep = true;
            out(L"");
            out(L"  The external display is not lit yet - it is powered off or asleep.");
            out(L"  Windows reports %d display(s) attached, no external target active,", t.attachedToDesktop);
            out(L"  though %d external target(s) are present in the configuration.", t.externalAvailable);
            out(L"  No software can switch to a display that is not powered on, so:");
            out(L"     -> switch the TV on, or press any button on its remote, now.");
            out(L"     -> this window keeps trying for up to %d seconds.", timeout);
            out(L"");
        }

        if (GetTickCount64() >= deadline) {
            SetConsoleTitleW(L"ToggleDisplays - gave up");
            fail(L"No external display appeared.\n"
                 L"DisplaySwitch.exe could not find a second display to switch to.\n\n"
                 L"Most likely the TV is off or asleep: turn it on, or press a\n"
                 L"button on its remote, then run this again.\n\n"
                 L"Nothing was changed - your internal panel is still in use.");
            return 1;
        }
        Sleep(500);
    }
}
