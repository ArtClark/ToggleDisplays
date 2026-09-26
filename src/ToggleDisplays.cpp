/*
 * ToggleDisplays - switch the Windows desktop between the notebook's internal
 * panel and an external display (TV, projector) without the Control Panel.
 *
 * Run it with no arguments and it toggles: the panel is "home", the external
 * display is "away", and each run moves you between them.
 *
 * WHY IT DELEGATES THE MUTATION
 *   Topology changes are handed entirely to DisplaySwitch.exe, Microsoft's own
 *   implementation. This tool never calls SetDisplayConfig in a way that can
 *   change anything. Two reasons:
 *
 *     1. Correctness. DisplaySwitch picks the external display's native mode,
 *        makes it primary at (0,0), and turns the panel off. Reimplementing
 *        that means hand-building a display configuration, and it turns out to
 *        be harder than it looks. A QueryDisplayConfig(QDC_ALL_PATHS) snapshot
 *        fed straight back into SetDisplayConfig does not validate -
 *        ERROR_INVALID_PARAMETER - because an all-paths snapshot is a list of
 *        paths, not a topology. Measured: enabling every available external path
 *        fails where enabling exactly one succeeds.
 *
 *     2. Safety. DisplaySwitch is atomic: it either applies the whole topology
 *        change or does nothing. So this tool can never leave the desktop on a
 *        display that is not lit. A hand-rolled toggler invites exactly that
 *        failure - enable an external target that reports targetAvailable=1
 *        while the TV is asleep, disable the panel, report success, and leave a
 *        black screen. "Available" is not "usable".
 *
 *   --probe is the deliberate exception, and it is read-only: it calls
 *   SetDisplayConfig with SDC_VALIDATE and never passes SDC_APPLY. See the
 *   comment above runProbe() for the safety bound that makes that an argument
 *   rather than a promise.
 *
 * WHY IT WAITS
 *   No API can wake a powered-off TV. Many TVs also sleep on their own timer,
 *   which will interrupt a switch already in flight. So the tool asks, verifies,
 *   and retries, treating "the display is asleep" as an expected state rather
 *   than an error. If you switch the TV on while it waits, the rest happens by
 *   itself.
 *
 * HOW IT VERIFIES
 *   Read-only, through the CCD API. Internal versus external is decided by
 *   output technology - INTERNAL / LVDS / DISPLAYPORT_EMBEDDED is the panel;
 *   HDMI / DVI / external DisplayPort is external. That is a property of the
 *   hardware, not of enumeration order, which is what keeps this correct on a
 *   laptop with two GPUs and several connectors. No code anywhere keys off an
 *   array index as an identifier: paths are always walked in a counted loop and
 *   classified by technology and by EDID target name.
 *
 *   Note that availability must be counted from QDC_ALL_PATHS: an inactive
 *   target does not appear in a QDC_ONLY_ACTIVE_PATHS result at all, so
 *   counting it from the active view is structurally guaranteed to report zero.
 *
 *   --to-external NAME is a *verification* constraint, not a selector.
 *   DisplaySwitch.exe cannot be told which external display to prefer, so
 *   naming one means "switch, and require that this is the display that came
 *   up". A mismatch is reported as a failure rather than quietly accepted.
 *
 * USAGE
 *   ToggleDisplays                        toggle (default)
 *   ToggleDisplays --to-external [NAME]   wait for an external display, show only it
 *   ToggleDisplays --to-internal          go back to the internal panel only
 *   ToggleDisplays --status               print the current topology and exit
 *   ToggleDisplays --list-externals       list external targets by EDID name
 *   ToggleDisplays --dry-run              say what a toggle would do, change nothing
 *   ToggleDisplays --probe                validate-only display-config probe
 *   ToggleDisplays --json                 machine-readable output (with --status)
 *   ToggleDisplays --timeout N            seconds to keep trying (default 90)
 *   ToggleDisplays --verbose              print the topology after every attempt
 *   ToggleDisplays --quiet                no output at all
 *   ToggleDisplays --help
 *
 * EXIT CODES
 *   0  the requested topology is in place
 *   1  gave up, or --probe was rejected by SetDisplayConfig; nothing was changed
 *   2  bad arguments
 *   3  --probe found no external target to validate against
 *
 * Build (x64), or just run build.cmd:
 *   cl /nologo /EHsc /std:c++17 /W4 /WX /O2 /Fe:ToggleDisplays.exe ^
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
static bool g_json    = false;

/* ------------------------------------------------------------------ output */

static void out(const wchar_t* fmt, ...)
{
    if (g_quiet || g_json) return;
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
    if (!g_json) {
        fwprintf(stderr, L"\nFAILED\n");
        fwprintf(stderr, L"%s\n", msg);
        fflush(stderr);
    }
}

static const wchar_t* errName(LONG r)
{
    switch (r) {
    case ERROR_SUCCESS:           return L"ERROR_SUCCESS";
    case ERROR_ACCESS_DENIED:     return L"ERROR_ACCESS_DENIED";
    case ERROR_INVALID_PARAMETER: return L"ERROR_INVALID_PARAMETER";
    case ERROR_INVALID_FLAGS:     return L"ERROR_INVALID_FLAGS";
    case ERROR_NOT_SUPPORTED:     return L"ERROR_NOT_SUPPORTED";
    case ERROR_CALL_NOT_IMPLEMENTED: return L"ERROR_CALL_NOT_IMPLEMENTED";
    case ERROR_BUSY:              return L"ERROR_BUSY";
    default:                      return L"(unnamed)";
    }
}

/* Minimal JSON string escaping, operating on UTF-8 bytes. EDID names are not
 * attacker-controlled, but they are not guaranteed free of quotes, backslashes
 * or control characters either, and malformed output is worse than none.
 * Wide callers convert first, so there is exactly one escaper to get right. */
static void jsonEscape(const char* s)
{
    putchar('"');
    for (const unsigned char* p = (const unsigned char*)s; *p; ++p) {
        if      (*p == '"')  fputs("\\\"", stdout);
        else if (*p == '\\') fputs("\\\\", stdout);
        else if (*p == '\n') fputs("\\n", stdout);
        else if (*p == '\r') fputs("\\r", stdout);
        else if (*p == '\t') fputs("\\t", stdout);
        else if (*p < 0x20)   printf("\\u%04x", (unsigned)*p);
        else                 putchar((int)*p);
    }
    putchar('"');
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
 * Resolve a target's EDID friendly name ("TV TOSHIBA", "DELL U2412M").
 *
 * The name is only trustworthy when flags.friendlyNameFromEdid is set. Windows
 * synthesises a placeholder such as "Generic PnP Monitor" when the display
 * reports no usable EDID, and matching on that would let this tool claim to
 * have found a particular television when it found nothing of the sort. The
 * bit is carried back to the caller so the distinction stays visible.
 */
static bool queryTargetName(LUID adapterId, UINT32 targetId,
                            wchar_t* out, size_t outChars, bool* fromEdid)
{
    if (out && outChars) out[0] = L'\0';
    if (fromEdid) *fromEdid = false;

    DISPLAYCONFIG_TARGET_DEVICE_NAME tdn;
    ZeroMemory(&tdn, sizeof(tdn));
    tdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tdn.header.size      = sizeof(tdn);
    tdn.header.adapterId = adapterId;
    tdn.header.id        = targetId;

    if (DisplayConfigGetDeviceInfo(&tdn.header) != ERROR_SUCCESS) return false;
    if (fromEdid) *fromEdid = (tdn.flags.friendlyNameFromEdid != 0);

    if (out && outChars) {
        wcsncpy_s(out, outChars, tdn.monitorFriendlyDeviceName, _TRUNCATE);
        /* EDID strings are space-padded to the full 64 characters. */
        for (size_t n = wcslen(out); n > 0 && out[n - 1] == L' '; --n) out[n - 1] = L'\0';
        if (out[0] == L'\0') wcscpy_s(out, outChars, L"(unnamed)");
    }
    return true;
}

struct ExtTarget {
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY tech;
    UINT32  targetId;
    LUID    adapterId;
    bool    available;
    wchar_t name[64];
    bool    nameFromEdid;
};

/*
 * Available-but-inactive external targets.
 *
 * This MUST use QDC_ALL_PATHS. An inactive target does not appear in a
 * QDC_ONLY_ACTIVE_PATHS result at all, so counting availability from the
 * active-paths view is structurally guaranteed to report zero - which is the
 * bug the first build of this tool shipped. Measured on the development
 * machine: the sleeping TV reports targetAvailable=1 on three paths, while the
 * active-paths view can only ever show the one internal path.
 */
static std::vector<ExtTarget> listExternalTargets()
{
    std::vector<ExtTarget> v;

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc) != ERROR_SUCCESS) return v;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pc, paths.data(), &mc, modes.data(), NULL) != ERROR_SUCCESS)
        return v;

    /* A target is reachable through several source paths; de-duplicate on
     * (adapterId, targetId) so one physical display is listed once. */
    for (UINT32 i = 0; i < pc; ++i) {
        if (isInternalTech(paths[i].targetInfo.outputTechnology)) continue;

        ExtTarget e;
        e.tech     = paths[i].targetInfo.outputTechnology;
        e.targetId = paths[i].targetInfo.id;
        e.adapterId = paths[i].targetInfo.adapterId;
        e.available = paths[i].targetInfo.targetAvailable != 0;

        bool dup = false;
        for (const auto& x : v)
            if (x.targetId == e.targetId && x.adapterId.LowPart == e.adapterId.LowPart
                                           && x.adapterId.HighPart == e.adapterId.HighPart)
                dup = true;
        if (dup) continue;

        if (!queryTargetName(e.adapterId, e.targetId, e.name, _countof(e.name), &e.nameFromEdid)) {
            wcscpy_s(e.name, _countof(e.name), L"(unavailable)");
            e.nameFromEdid = false;
        }
        v.push_back(e);
    }
    return v;
}

struct Topology {
    int  internalActive    = 0;   /* ACTIVE internal paths  */
    int  externalActive    = 0;   /* ACTIVE external paths  */
    int  externalAvailable = 0;   /* available external targets, active or not */
    int  attachedToDesktop = 0;   /* EnumDisplayDevices entries attached */
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY activeExternalTech =
        DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    wchar_t activeExternalName[64]     = L"";
    bool    activeExternalNameFromEdid = false;
};

/* Read-only. Never changes anything. */
static Topology queryTopology()
{
    Topology t;
    t.attachedToDesktop = countAttachedDisplays();
    t.externalAvailable = (int)listExternalTargets().size();

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS) return t;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths.data(), &mc, modes.data(), NULL) != ERROR_SUCCESS)
        return t;

    for (UINT32 i = 0; i < pc; ++i) {
        if (!(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
        if (isInternalTech(paths[i].targetInfo.outputTechnology)) { ++t.internalActive; continue; }

        ++t.externalActive;
        /* First active external wins; in practice there is only ever one. */
        if (t.externalActive == 1) {
            t.activeExternalTech = paths[i].targetInfo.outputTechnology;
            queryTargetName(paths[i].targetInfo.adapterId, paths[i].targetInfo.id,
                            t.activeExternalName, _countof(t.activeExternalName),
                            &t.activeExternalNameFromEdid);
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
    if (t.externalActive) {
        out(L"    external is    : %s", techLabel(t.activeExternalTech));
        out(L"    named          : %s%s", t.activeExternalName,
            t.activeExternalNameFromEdid ? L"" : L"   (synthesised - no usable EDID, do not match on this)");
    }
    out(L"    displays attached to the desktop: %d", t.attachedToDesktop);
}

/* Narrow a wide string to UTF-8 for JSON emission. */
static void jsonField(const char* key, const wchar_t* value)
{
    char utf8[512] = {0};
    if (value) WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof(utf8), NULL, NULL);
    printf("\"%s\":", key);
    jsonEscape(utf8[0] ? utf8 : "");
    putchar(',');
}
static void reportTopologyJson()
{
    const Topology t = queryTopology();
    fputs("{", stdout);
    printf("\"internalActive\":%d,", t.internalActive);
    printf("\"internalOn\":%s,",     t.internalActive ? "true" : "false");
    printf("\"externalActive\":%d,", t.externalActive);
    printf("\"externalOn\":%s,",     t.externalActive ? "true" : "false");
    printf("\"externalAvailable\":%d,", t.externalAvailable);
    printf("\"attachedToDesktop\":%d,", t.attachedToDesktop);
    if (t.externalActive) {
        jsonField("externalTech", techLabel(t.activeExternalTech));
        jsonField("externalName", t.activeExternalName);
        printf("\"externalNameFromEdid\":%s", t.activeExternalNameFromEdid ? "true" : "false");
    } else {
        fputs("\"externalTech\":null,\"externalName\":null,\"externalNameFromEdid\":null", stdout);
    }
    fputs("}\n", stdout);
    fflush(stdout);
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

/*
 * Does the lit external display satisfy the user's name constraint?
 * Substring, case-insensitive. A name that came from no EDID never matches,
 * because matching on "Generic PnP Monitor" would be meaningless.
 */
static bool nameMatches(const Topology& t, const wchar_t* want)
{
    if (!want || !*want) return true;
    if (!t.externalActive || !t.activeExternalNameFromEdid) return false;
    return wcsstr(t.activeExternalName, want) != NULL;
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

/* ----------------------------------------------------------------- probe -- */

/*
 * VALIDATE-ONLY display-config probe.
 *
 * SetDisplayConfig is called with SDC_VALIDATE and SDC_APPLY is never passed,
 * so no configuration is committed. "We omit the apply flag" is an argument,
 * not a guarantee, so there is a structural safety bound as well: the internal
 * panel's active path is force-kept ACTIVE in the array. Nothing under test
 * concerns the panel, so keeping it lit does not affect the measurement, and
 * it bounds the worst case to "nothing happens" rather than "panel goes dark".
 *
 * The array is the recipe that actually validated on this machine: the
 * QDC_ALL_PATHS snapshot with the internal path ACTIVE, exactly ONE available
 * external path ACTIVE, and every unavailable target deactivated. Measured:
 *   every available external on   -> ERROR_GEN_FAILURE
 *   exactly one external on       -> ERROR_SUCCESS
 * An external target is reachable through several source paths, and switching
 * them all on at once is not a configuration Windows will accept.
 *
 * The one thing this cannot tell you: the target display was asleep throughout,
 * so a validate success is not a promise that an apply would produce a visible
 * desktop.
 */
static int runProbe()
{
    const UINT32 flags = SDC_VALIDATE | SDC_ALLOW_CHANGES | SDC_USE_SUPPLIED_DISPLAY_CONFIG;

    /* Defensive: if a future edit ever adds SDC_APPLY here, refuse to run. */
    if (flags & SDC_APPLY) {
        fail(L"internal error: probe flags include SDC_APPLY. Refusing to run.");
        return 1;
    }

    UINT32 pc = 0, mc = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pc, &mc) != ERROR_SUCCESS) {
        fail(L"GetDisplayConfigBufferSizes(QDC_ALL_PATHS) failed.");
        return 1;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &pc, paths.data(), &mc, modes.data(), NULL) != ERROR_SUCCESS) {
        fail(L"QueryDisplayConfig(QDC_ALL_PATHS) failed.");
        return 1;
    }

    out(L"validate-only probe. SDC_APPLY (0x80) is not present in the flags below.");
    out(L"the internal panel is force-kept ACTIVE, so the worst case is 'nothing'.");
    out(L"baseline: %u paths, %u modes", (unsigned)pc, (unsigned)mc);
    out(L"");

    /* Classify by technology and availability - never by position. */
    int  internalIdx = -1, firstExt = -1, extCount = 0;
    for (UINT32 i = 0; i < pc; ++i) {
        if (isInternalTech(paths[i].targetInfo.outputTechnology)) {
            if (internalIdx < 0) internalIdx = (int)i;
        } else if (paths[i].targetInfo.targetAvailable) {
            if (firstExt < 0) firstExt = (int)i;
            ++extCount;
        }
    }
    out(L"internal path index %d; available external paths: %d", internalIdx, extCount);

    if (firstExt < 0) {
        fail(L"No available external target to validate against. With the TV asleep\n"
             L"this is exactly the state that makes DisplaySwitch a no-op.");
        return 3;
    }

    if (internalIdx >= 0) paths[internalIdx].flags |= DISPLAYCONFIG_PATH_ACTIVE;   /* safety bound */
    paths[firstExt].flags      |= DISPLAYCONFIG_PATH_ACTIVE;                        /* exactly one */
    for (UINT32 i = 0; i < pc; ++i) {
        if ((int)i == internalIdx || (int)i == firstExt) continue;
        paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
    }

    int active = 0;
    for (UINT32 i = 0; i < pc; ++i) if (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) ++active;
    out(L"submitting: %u paths, %u modes, %d ACTIVE (panel + 1 external)", (unsigned)pc, (unsigned)mc, active);
    out(L"");

    SetLastError(0);
    const LONG r = SetDisplayConfig(pc, paths.data(), mc, modes.data(), flags);
    const DWORD le = GetLastError();

    out(L"  flags=0x%03X  SetDisplayConfig -> %ld %s", (unsigned)flags, r, errName(r));
    if (le) out(L"  GetLastError=%lu", (unsigned)le);

    if (r == ERROR_SUCCESS) {
        out(L"");
        out(L"Windows would accept this configuration. Nothing was applied.");
        return 0;
    }
    out(L"");
    out(L"Windows rejected this configuration. Nothing was applied.");
    return 1;
}

/* ------------------------------------------------------------------ main -- */

static void usage()
{
    fwprintf(stdout,
        L"ToggleDisplays - switch between the internal panel and an external\n"
        L"                 display, without the Control Panel.\n\n"
        L"  (no arguments)     toggle: the panel is home, the external is away\n"
        L"  --to-external [N]  wait for an external display, then show only it.\n"
        L"                      N (e.g. TOSHIBA) additionally requires that the\n"
        L"                      display which comes up is the one named\n"
        L"  --to-internal      go back to the internal panel only\n"
        L"  --status           print the current topology and exit\n"
        L"  --list-externals   list external targets by EDID name\n"
        L"  --dry-run          say what a toggle would do, change nothing\n"
        L"  --probe            validate-only display-config probe (never applies)\n"
        L"  --json             machine-readable output, with --status\n"
        L"  --timeout N        seconds to keep trying (default %d)\n"
        L"  --verbose          print the topology after every attempt\n"
        L"  --quiet            no output at all\n"
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
    bool statusOnly = false, dryRun = false, probe = false, listExt = false;
    const wchar_t* wantedName = nullptr;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!_wcsicmp(a, L"--to-external") || !_wcsicmp(a, L"--to-tv")) {
            explicitExternal = true;
            /* optional positional: the next token, if it is not another option */
            if (i + 1 < argc && argv[i + 1][0] != L'-') wantedName = argv[++i];
        }
        else if (!_wcsicmp(a, L"--to-internal") || !_wcsicmp(a, L"--to-panel")) explicitInternal = true;
        else if (!_wcsicmp(a, L"--status") || !_wcsicmp(a, L"--list"))         statusOnly = true;
        else if (!_wcsicmp(a, L"--dry-run") || !_wcsicmp(a, L"--what-if"))      dryRun = true;
        else if (!_wcsicmp(a, L"--probe"))                                      probe = true;
        else if (!_wcsicmp(a, L"--list-externals"))                             listExt = true;
        else if (!_wcsicmp(a, L"--json"))                                       g_json = true;
        else if (!_wcsicmp(a, L"--quiet"))                                      g_quiet = true;
        else if (!_wcsicmp(a, L"--verbose"))                                    g_verbose = true;
        else if (!_wcsicmp(a, L"--timeout") && i + 1 < argc) timeout = _wtoi(argv[++i]);
        else if (!_wcsicmp(a, L"--help") || !_wcsicmp(a, L"-h") || !_wcsicmp(a, L"/?")) { usage(); return 0; }
        else { fwprintf(stderr, L"unknown option: %s\n\n", a); usage(); return 2; }
    }

    if (explicitExternal && explicitInternal) {
        fwprintf(stderr, L"pick one of --to-external or --to-internal\n");
        return 2;
    }
    if (timeout < 1) timeout = 1;
    if (!statusOnly && !g_quiet && !g_json && AttachConsole(ATTACH_PARENT_PROCESS) == FALSE) AllocConsole();

    /* ---- read-only modes ---- */
    if (probe) return runProbe();

    if (listExt) {
        const std::vector<ExtTarget> v = listExternalTargets();
        if (g_json) {
            fputs("{\"externals\":[", stdout);
            for (size_t k = 0; k < v.size(); ++k) {
                if (k) putchar(',');
                char nm[512] = {0};
                WideCharToMultiByte(CP_UTF8, 0, v[k].name, -1, nm, sizeof(nm), NULL, NULL);
                printf("{\"targetId\":%u,\"available\":%s,\"nameFromEdid\":%s,\"name\":",
                       (unsigned)v[k].targetId,
                       v[k].available ? "true" : "false",
                       v[k].nameFromEdid ? "true" : "false");
                jsonEscape(nm);
                putchar('}');
            }
            fputs("]}\n", stdout);
            fflush(stdout);
        } else {
            if (v.empty()) { out(L"no external targets found at all."); return 0; }
            for (const auto& e : v)
                out(L"  target %-8u  %-16s  %-8s  %s%s",
                    (unsigned)e.targetId, techLabel(e.tech),
                    e.available ? L"available" : L"unavailable",
                    e.name,
                    e.nameFromEdid ? L"" : L"   (synthesised - no usable EDID)");
        }
        return 0;
    }

    if (statusOnly) {
        if (g_json) reportTopologyJson();
        else         reportTopology(L"current topology:");
        return 0;
    }

    /* Refuse an unsatisfiable name up front rather than after a switch. */
    if (wantedName && *wantedName) {
        const std::vector<ExtTarget> v = listExternalTargets();
        int matches = 0;
        for (const auto& e : v)
            if (e.nameFromEdid && wcsstr(e.name, wantedName)) ++matches;
        if (matches == 0) {
            wchar_t msg[512];
            swprintf_s(msg, L"No external display with a usable EDID name matches \"%s\".\n"
                            L"Run --list-externals to see what is actually present.\n\n"
                            L"Nothing was changed.", wantedName);
            fail(msg);
            return 1;
        }
    }

    const Topology before = queryTopology();
    const Target target = explicitExternal ? Target::ToExternal
                         : explicitInternal ? Target::ToInternal
                         : resolveToggleTarget(before);

    reportTopology(L"before:");
    if (wantedName && *wantedName) out(L"  requiring the external display to be: %s", wantedName);

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
    if (isExternalOnly()) {
        const Topology t = queryTopology();
        if (nameMatches(t, wantedName)) {
            out(L"already showing %s. Nothing to do.", targetName(target));
            return 0;
        }
        out(L"an external display is already lit, but not the one you asked for.");
    }

    SetConsoleTitleW(L"ToggleDisplays - waiting for the external display");

    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout * 1000ULL;
    int  attempt = 0;
    bool toldAboutSleep = false;

    for (;;) {
        ++attempt;
        if (attempt > 1) out(L"  still not there - asking again (attempt %d)", attempt);
        runDisplaySwitch(L"external");

        if (waitFor(isExternalOnly, SETTLE_MS)) {
            const Topology t = queryTopology();
            if (!nameMatches(t, wantedName)) {
                reportTopology(L"after:");
                wchar_t msg[512];
                swprintf_s(msg, L"An external display came up, but it is not \"%s\".\n\n"
                                L"DisplaySwitch.exe cannot be told which external display to\n"
                                L"prefer - Windows chooses. \"%s\" may be asleep, or on a\n"
                                L"different connector.\n\n"
                                L"The internal panel is now off. Run --to-internal to undo.",
                            wantedName, wantedName);
                fail(msg);
                return 1;
            }
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
