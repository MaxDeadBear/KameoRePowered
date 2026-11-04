"""A sampling profiler for the running game that needs no admin rights, for when WPR can't be used.

    python tools/profiling/sample_game.py <seconds> <out.txt>

Suspends the busiest threads in turn, reads where each one is, and names those places with llvm-symbolizer and the PDB
beside the exe. It sees the function a thread is in, not who called it. A thread's time in ntdll is mostly waiting.
"""
import collections, ctypes, ctypes.wintypes as w, os, subprocess, sys, time

k = ctypes.WinDLL('kernel32', use_last_error=True)
TH32CS_SNAPTHREAD = 4
THREAD_ALL = 0x0002 | 0x0008 | 0x0040 | 0x0010  # suspend, query, get/set context
PROCESS_ALL = 0x0400 | 0x0010
CONTEXT_FULL = 0x100000 | 1 | 2 | 8
EXE = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'out', 'build', 'release', 'kameorepowered.exe')
SYMBOLIZER = 'C:/Program Files/LLVM/bin/llvm-symbolizer.exe'


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', w.DWORD), ('cntUsage', w.DWORD), ('th32ThreadID', w.DWORD), ('th32OwnerProcessID', w.DWORD), ('tpBasePri', w.LONG), ('tpDeltaPri', w.LONG), ('dwFlags', w.DWORD)]


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [('head', ctypes.c_ubyte * 48), ('ContextFlags', w.DWORD), ('MxCsr', w.DWORD), ('segs', ctypes.c_ubyte * 16), ('Dr', ctypes.c_ulonglong * 6), ('Rax', ctypes.c_ulonglong), ('Rcx', ctypes.c_ulonglong), ('Rdx', ctypes.c_ulonglong), ('Rbx', ctypes.c_ulonglong), ('Rsp', ctypes.c_ulonglong), ('Rbp', ctypes.c_ulonglong), ('Rsi', ctypes.c_ulonglong), ('Rdi', ctypes.c_ulonglong), ('R', ctypes.c_ulonglong * 8), ('Rip', ctypes.c_ulonglong), ('tail', ctypes.c_ubyte * 976)]


def process_id(name):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {name}', '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if name.lower() in line.lower():
            return int(line.split('","')[1])
    return None


def thread_ids(pid):
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k.Thread32Next(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return out


def modules_of(pid):
    psapi = ctypes.WinDLL('psapi')
    k.OpenProcess.restype = w.HANDLE
    process = k.OpenProcess(PROCESS_ALL, False, pid)
    handles = (ctypes.c_void_p * 1024)()
    needed = w.DWORD()
    psapi.EnumProcessModules(w.HANDLE(process), handles, ctypes.sizeof(handles), ctypes.byref(needed))
    psapi.GetModuleBaseNameW.argtypes = [w.HANDLE, ctypes.c_void_p, w.LPWSTR, w.DWORD]

    class INFO(ctypes.Structure):
        _fields_ = [('base', ctypes.c_void_p), ('size', w.DWORD), ('entry', ctypes.c_void_p)]

    psapi.GetModuleInformation.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(INFO), w.DWORD]
    out = []
    for i in range(needed.value // ctypes.sizeof(ctypes.c_void_p)):
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(process, handles[i], name, 260)
        info = INFO()
        psapi.GetModuleInformation(process, handles[i], ctypes.byref(info), ctypes.sizeof(info))
        out.append((int(info.base), int(info.base) + info.size, name.value))
    return out


def cpu_times(handle):
    a, b, c, d = w.FILETIME(), w.FILETIME(), w.FILETIME(), w.FILETIME()
    k.GetThreadTimes(handle, ctypes.byref(a), ctypes.byref(b), ctypes.byref(c), ctypes.byref(d))
    return (c.dwHighDateTime << 32 | c.dwLowDateTime) + (d.dwHighDateTime << 32 | d.dwLowDateTime)


def main():
    seconds, out = float(sys.argv[1]), sys.argv[2]
    pid = process_id('kameorepowered.exe')
    if pid is None:
        print('game not running')
        return 1
    modules = modules_of(pid)
    base, top = next((a, b) for a, b, n in modules if n.lower() == 'kameorepowered.exe')
    k.OpenThread.restype = w.HANDLE
    handles = {t: k.OpenThread(THREAD_ALL, False, t) for t in thread_ids(pid)}
    # The busy threads only: suspending the rest costs the game time for nothing.
    start = {t: cpu_times(h) for t, h in handles.items()}
    time.sleep(1.0)
    busy = sorted(handles, key=lambda t: start[t] - cpu_times(handles[t]))[:4]
    print('busiest threads', [(t, (cpu_times(handles[t]) - start[t]) // 100000) for t in busy], flush=True)
    samples = {t: collections.Counter() for t in busy}
    end = time.time() + seconds
    ctx = (ctypes.c_ubyte * 1400)()
    aligned = ctypes.cast(ctypes.addressof(ctx) + (16 - ctypes.addressof(ctx) % 16) % 16, ctypes.POINTER(CONTEXT))
    taken = 0
    begin = {t: cpu_times(handles[t]) for t in busy}
    while time.time() < end:
        for t in busy:
            h = handles[t]
            if k.SuspendThread(h) == 0xFFFFFFFF:
                continue
            aligned.contents.ContextFlags = CONTEXT_FULL
            if k.GetThreadContext(h, aligned):
                rip = aligned.contents.Rip
                samples[t][rip] += 1
            k.ResumeThread(h)
        taken += 1
        time.sleep(0.002)
    use = {t: (cpu_times(handles[t]) - begin[t]) / 1e7 / seconds for t in busy}
    print('sample rounds', taken, 'cpu use', {t: round(u, 2) for t, u in use.items()}, flush=True)

    rips = sorted({r for c in samples.values() for r in c if base <= r < top})
    print('symbolizing', len(rips), flush=True)
    query = '\n'.join(hex(r - base) for r in rips) + '\n'
    result = subprocess.run([SYMBOLIZER, '--obj=' + EXE, '--relative-address', '--functions=linkage', '--inlines'], input=query, capture_output=True, text=True)
    # One block per address, blank line between, innermost frame first: the last frame is the real function.
    blocks = [b.split('\n') for b in result.stdout.split('\n\n')]
    names = {}
    for r, block in zip(rips, blocks):
        frames = [line for i, line in enumerate(block) if i % 2 == 0]
        names[r] = frames[-1] if frames else '?'

    def label(r):
        if r in names:
            return names[r]
        for a, b, name in modules:
            if a <= r < b:
                return '[' + name + ']'
        return '[unknown code]'

    with open(out, 'w', encoding='utf-8') as handle:
        for t in busy:
            total = sum(samples[t].values()) or 1
            by = collections.Counter()
            for r, n in samples[t].items():
                by[label(r)] += n
            handle.write(f'thread {t}: {total} samples, cpu {use[t]:.2f} cores\n')
            for name, n in by.most_common(40):
                handle.write(f'  {100 * n / total:5.1f}%  {name[:90]}\n')
            handle.write('\n')
    print('wrote', out)
    return 0


if __name__ == '__main__':
    sys.exit(main())

