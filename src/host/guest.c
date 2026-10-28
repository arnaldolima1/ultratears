// Glue between host threads and the recompiled game: per-thread Cpu, Horizon TLS blocks, host -> guest calls, SVCs.
#ifdef _WIN32
#include <malloc.h>
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static __thread Cpu *t_cpu;

Cpu *guest_cpu(void) { return t_cpu; }

// Which host thread runs which guest CPU (Windows thread ids): the hang watchdog prints a stuck thread's registers.
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#define CPU_THREADS 256
static struct { unsigned long tid; Cpu *c; } g_cpu_of[CPU_THREADS];
static _Atomic int g_ncpu;
Cpu *guest_cpu_of_thread(unsigned long tid) {
    int n = atomic_load(&g_ncpu);
    for (int i = n < CPU_THREADS ? n : CPU_THREADS; i-- > 0;) if (g_cpu_of[i].tid == tid) return g_cpu_of[i].c;
    return NULL;
}
#endif
void guest_cpu_set(Cpu *c) {
    t_cpu = c;
#ifdef _WIN32
    int i = atomic_fetch_add(&g_ncpu, 1);
    if (i < CPU_THREADS) { g_cpu_of[i].c = c; g_cpu_of[i].tid = GetCurrentThreadId(); }
#endif
}

// Horizon thread-local region (tpidrro_el0): 0x200 bytes; +0x000 IPC message buffer, +0x1f8 the SDK's thread pointer.
// MacNativeGames (loader.c runtime_thread_init) points +0x1f8 at a zeroed 0x400-byte placeholder until the SDK sets it.
uint64_t guest_tls_alloc(void) {
    static _Atomic uint64_t next = TLS_BASE;
    uint64_t a = atomic_fetch_add(&next, 0x200);
    if (a + 0x200 > TLS_BASE + TLS_SIZE) runtime_fail("out of guest TLS blocks");
    plat_commit(a, 0x200);
    memset((void *)(uintptr_t)a, 0, 0x200);
    uint64_t placeholder = (uint64_t)(uintptr_t)hle_alloc(0x400);
    memcpy((void *)(uintptr_t)(a + 0x1f8), &placeholder, 8);
    return a;
}

Cpu *guest_cpu_new(uint64_t stack_top, uint64_t tls) {
    Cpu *c = _aligned_malloc(sizeof(Cpu), 64);
    if (!c) runtime_fail("guest CPU allocation");
    memset(c, 0, sizeof *c);
    c->sp = stack_top & ~0xFull;
    c->tpidrro = tls;
    return c;
}

uint64_t guest_call_args(uint64_t fn, int n, const uint64_t *args) {
    Cpu *c = t_cpu;
    if (!c) runtime_fail("guest call from a host thread");
    if (n > 8) runtime_fail("guest call with more than 8 arguments");
    Cpu *saved = _aligned_malloc(sizeof(Cpu), 64);
    if (!saved) runtime_fail("guest call state allocation");
    memcpy(saved, c, sizeof *c);
    for (int i = 0; i < n; i++) c->x[i] = args[i];
    guest_call(c, fn);
    uint64_t r = c->x[0];
    // Only the caller's registers come back (x0-x30, sp, pc, flags, vector registers). Thread state the guest changed
    // during the call stays: TPIDR_EL0 (the SDK sets it with msr while initializing threads), FPCR/FPSR, the monitor.
    memcpy(c->x, saved->x, sizeof c->x);
    c->sp = saved->sp;
    c->pc = saved->pc;
    c->nzcv = saved->nzcv;
    memcpy(c->v, saved->v, sizeof c->v);
    _aligned_free(saved);
    return r;
}

// Every `svc #imm` of the recompiled code lands here (cpu.h).
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { runtime_svc(imm, c->x, pc); }

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "[totk] FATAL: %s\n", message);
    rt_fatal_trace();
}

uint64_t runtime_now_ns(void) { return plat_now_ns(); }

void *runtime_symbol(const char *name) { return (void *)(uintptr_t)loader_find_symbol(name); }

// Guest call chain from a frame pointer (x29) and link register (x30): AArch64 frame records are {fp, lr} pairs.
void runtime_trace_guest(uint64_t fp, uint64_t lr) {
    for (int depth = 0; depth < 32; depth++) {
        uint64_t off = 0;
        const char *s = lr ? loader_symbol_at(lr - 4, &off) : NULL;
        fprintf(stderr, "    #%-2d %#llx %s+%#llx\n", depth, (unsigned long long)lr, s ? s : "?", (unsigned long long)(off + 4));
        if (!fp || fp < GUEST_LO || fp >= GUEST_HI || (fp & 7)) break;
        uint64_t next = ((uint64_t *)(uintptr_t)fp)[0];
        lr = ((uint64_t *)(uintptr_t)fp)[1];
        if (next <= fp) break;
        fp = next;
    }
}

// Crash report: the faulting thread's guest registers and guest call chain (src/runtime/dispatch.c rt_fault_report).
static void guest_fault_report(void) {
    Cpu *c = t_cpu;
    if (!c) { fprintf(stderr, "    (no guest thread)\n"); return; }
    fprintf(stderr, "    guest sp=%#llx tpidrro=%#llx thread handle %#x\n", (unsigned long long)c->sp,
            (unsigned long long)c->tpidrro, runtime_thread_current_handle());
    for (int i = 0; i < 31; i += 4)
        fprintf(stderr, "    x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", i, (unsigned long long)c->x[i],
                i + 1, (unsigned long long)c->x[i + 1], i + 2, (unsigned long long)c->x[i + 2], i + 3,
                (unsigned long long)c->x[i + 3]);
    fprintf(stderr, "    guest call chain (x30 / frame records):\n");
    runtime_trace_guest(c->x[29], c->x[30]);
}
extern void (*rt_fault_extra)(void);
__attribute__((constructor)) static void install_fault_report(void) { rt_fault_extra = guest_fault_report; }
