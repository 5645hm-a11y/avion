// media_stay_awake (docs/session.md, "Staying awake"): keeps the machine and the display out of idle standby for a whole CTest
// run, the gaps between test processes included. CTest runs it first (the stay_awake fixture every test requires); it starts a
// detached copy of itself that holds a power request (visible in `powercfg /requests` as "PLAYER test suite running"), resets
// the idle timers every 30 s, and ends when the CTest process does (or after 12 hours).
//   media_stay_awake          find CTest (the parent), start the holder, return at once
//   media_stay_awake --hold N hold until process N ends
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {
DWORD Parent(DWORD pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); DWORD parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        if (entry.th32ProcessID == pid) { parent = entry.th32ParentProcessID; break; }
    CloseHandle(snapshot);
    return parent;
}
int Hold(DWORD pid) {
    const HANDLE suite = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!suite) return 1;
    REASON_CONTEXT reason{}; reason.Version = POWER_REQUEST_CONTEXT_VERSION; reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    wchar_t text[] = L"PLAYER test suite running";
    reason.Reason.SimpleReasonString = text;
    const HANDLE request = PowerCreateRequest(&reason);
    if (request != INVALID_HANDLE_VALUE) { (void)PowerSetRequest(request, PowerRequestSystemRequired); (void)PowerSetRequest(request, PowerRequestDisplayRequired); }
    (void)SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    for (unsigned waited = 0; waited < 12u * 3600 / 30 && WaitForSingleObject(suite, 30000) == WAIT_TIMEOUT; ++waited)
        (void)SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); // The idle timers restart (belt and braces).
    if (request != INVALID_HANDLE_VALUE) {
        (void)PowerClearRequest(request, PowerRequestDisplayRequired); (void)PowerClearRequest(request, PowerRequestSystemRequired); CloseHandle(request);
    }
    (void)SetThreadExecutionState(ES_CONTINUOUS); (void)SetThreadExecutionState(ES_SYSTEM_REQUIRED); // The usual countdown from now.
    CloseHandle(suite);
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wcscmp(argv[1], L"--hold") == 0) return Hold(static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10)));
    const DWORD suite = Parent(GetCurrentProcessId());
    if (!suite) { std::puts("FAIL no parent process"); return 1; }
    wchar_t self[MAX_PATH]{}, line[MAX_PATH + 64]{};
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return 1;
    std::swprintf(line, MAX_PATH + 64, L"\"%ls\" --hold %lu", self, static_cast<unsigned long>(suite));
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    // Out of CTest's job when it allows it (else the job ends the holder with CTest, which is when it ends anyway).
    BOOL started = CreateProcessW(self, line, nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &process);
    if (!started) started = CreateProcessW(self, line, nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup, &process);
    if (!started) { std::printf("FAIL holder not started (%lu)\n", GetLastError()); return 1; }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    std::printf("Staying awake while process %lu (CTest) runs\n", static_cast<unsigned long>(suite));
    return 0;
}
