// macOS host layer (runtime/platform.h), Apple silicon. Fork: from linux.c, with Mach where macOS differs.
#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../platform.h"

// ---- guest address space
// macOS 26 (Apple silicon) keeps parts of the window for itself before main(): a permanent reservation over
// 0x10_0000_0000..0x70_0000_0000 and, at a random place below 64 GiB, the 12 GiB heap range whose unused parts are
// empty placeholders (protection none). So the guest gets only the three areas it uses, all below 2^39 as the SDK
// needs (host/svc.c): the modules and host areas at GUEST_LO (rt.h; the lifted code has these addresses built in),
// the 64 GiB alias region ending at GUEST_HI, and the 6 GiB of stacks and heap (svc.c's LOW, plat_guest_low) wherever
// it fits. An area may replace free space or placeholders only, never memory in use; if the heap range's live part
// covers the modules' area, the process starts itself again for another random layout.
#include "../rt.h"

#define AREA_MODULES_END (HLE_HEAP + HLE_HEAP_SIZE)
#define AREA_ALIAS_SIZE  (64ull << 30)   // svc.c ALIAS_SIZE, ending at GUEST_HI (ALIAS_START)
#define AREA_LOW_SIZE    (6ull << 30)    // svc.c LOW_SIZE
uint64_t plat_guest_low = 0x1000000000ull;   // svc.c LOW_START (set below)

static bool area_free(uint64_t a, uint64_t n) {   // only free space or empty placeholders in [a, a+n)
    mach_vm_address_t r = a;
    mach_vm_size_t s;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t i;
    mach_msg_type_number_t c = VM_REGION_SUBMAP_INFO_COUNT_64;
    while (!mach_vm_region_recurse(mach_task_self(), &r, &s, &depth, (vm_region_recurse_info_t)&i, &c) && r < a + n) {
        if (i.protection || i.max_protection || i.user_tag) return false;
        r += s;
        c = VM_REGION_SUBMAP_INFO_COUNT_64;
        depth = 0;
    }
    return true;
}

static bool area_claim(uint64_t a, uint64_t n) {   // zero-filled, read/write, backed on first touch
    mach_vm_address_t b = a;
    return mach_vm_allocate(mach_task_self(), &b, n, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE) == KERN_SUCCESS && b == a;
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    uint64_t alias = hi - AREA_ALIAS_SIZE;
    if (!area_free(lo, AREA_MODULES_END - lo)) {   // the heap range's live part is there this time: another layout
        const char *n = getenv("ULTRATEARS_LAYOUT_TRY");
        int tries = n ? atoi(n) : 0;
        if (tries < 8) {
            char v[8];
            snprintf(v, sizeof v, "%d", tries + 1);
            setenv("ULTRATEARS_LAYOUT_TRY", v, 1);
            plat_exec(NULL);
        }
        return false;
    }
    // stacks and heap: above the modules if there is room, else below them
    uint64_t low = 0;
    for (uint64_t a = (AREA_MODULES_END + (1ull << 30) - 1) & ~((1ull << 30) - 1); !low && a + AREA_LOW_SIZE <= 0xfc0000000ull; a += 1ull << 30)
        if (area_free(a, AREA_LOW_SIZE)) low = a;
    for (uint64_t a = 0x200000000ull; !low && a + AREA_LOW_SIZE <= lo; a += 1ull << 30)
        if (area_free(a, AREA_LOW_SIZE)) low = a;
    if (!low || !area_free(alias, AREA_ALIAS_SIZE)) return false;
    if (!area_claim(lo, AREA_MODULES_END - lo) || !area_claim(low, AREA_LOW_SIZE) || !area_claim(alias, AREA_ALIAS_SIZE)) return false;
    plat_guest_low = low;
    unsetenv("ULTRATEARS_LAYOUT_TRY");
    return true;
}

// Fresh zero pages over the three areas (the old ones are released at once).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return area_claim(lo, AREA_MODULES_END - lo) && area_claim(plat_guest_low, AREA_LOW_SIZE) &&
           area_claim(hi - AREA_ALIAS_SIZE, AREA_ALIAS_SIZE);
}

void plat_commit(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

void plat_memory_map(void) {}   // (TOTK_MEMMAP: Windows only)

int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page) {  // (no write tracking)
    (void)addr; (void)size; (void)pages; (void)max; (void)page;
    return -1;
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    int p = prot == PLAT_PROT_NONE ? PROT_NONE : prot == PLAT_PROT_READ ? PROT_READ : PROT_READ | PROT_WRITE;
    return mprotect((void *)(uintptr_t)addr, size, p);
}

// ---- time
static mach_timebase_info_data_t g_tb;

uint64_t plat_now_ns(void) {
    if (!g_tb.denom) mach_timebase_info(&g_tb);
    return mach_absolute_time() * g_tb.numer / g_tb.denom;
}

void plat_sleep_until_ns(uint64_t t) {
    if (!g_tb.denom) mach_timebase_info(&g_tb);
    mach_wait_until(t * g_tb.denom / g_tb.numer);
}

void plat_thread_boost(void) { pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); }

// A time-constraint thread (vsync): the scheduler runs it on time even while the guest threads load every core.
void plat_thread_realtime(uint64_t period_ns) {
    if (!g_tb.denom) mach_timebase_info(&g_tb);
    uint64_t period = (period_ns ? period_ns : 16666667) * g_tb.denom / g_tb.numer;
    thread_time_constraint_policy_data_t p = {.period = (uint32_t)period, .computation = (uint32_t)(period / 20),
                                              .constraint = (uint32_t)(period / 4), .preemptible = 1};
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

// ---- process
// Started by the launcher (or Finder) there is no terminal: stdout / stderr go to lasrecomp.log next to the
// executable, as on Windows (the launcher shows its end when the game stops). Processes started by this one append.
static char g_log_path[1024];

bool plat_exe_path(char *buf, size_t size) {
    uint32_t n = (uint32_t)size;
    char raw[4096];
    uint32_t rn = sizeof raw;
    if (_NSGetExecutablePath(raw, &rn) != 0) return false;
    if (!realpath(raw, buf)) { if (strlen(raw) >= size) return false; strcpy(buf, raw); }
    (void)n;
    return true;
}

static void log_to_file_without_terminal(void) {
    if (isatty(STDERR_FILENO)) return;
    char exe[1024];
    if (!plat_exe_path(exe, sizeof exe)) return;
    char *slash = strrchr(exe, '/');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%slasrecomp.log", exe);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!freopen(g_log_path, append ? "a" : "w", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    setenv("LASRECOMP_LOG_APPEND", "1", 1);
}

void plat_init(void) { log_to_file_without_terminal(); }

void plat_exec(char *const argv[]) {
    if (argv) { execv(argv[0], argv); return; }
    char path[4096];
    extern char ***_NSGetArgv(void);
    if (plat_exe_path(path, sizeof path)) execv(path, *_NSGetArgv());
}

// ---- diagnostics
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    Dl_info di = {0};
    if (!dladdr((void *)(uintptr_t)pc, &di) || !di.dli_sname) return NULL;
    if (offset) *offset = pc - (uint64_t)(uintptr_t)di.dli_saddr;
    return di.dli_sname;
}

// Fatal error without a terminal: the last FATAL line of the log in a message box rather than vanishing.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1200] = "UltraTears stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, "UltraTears stopped because of an error:\n\n%.600s", line);
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UltraTears", text, NULL);
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    int n = backtrace(bt, 64);
    Dl_info self = {0};
    dladdr((void *)plat_print_backtrace, &self);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        Dl_info di = {0};
        if (!dladdr(bt[i], &di)) di.dli_fname = NULL;
        bool ours = di.dli_fbase && di.dli_fbase == self.dli_fbase;
        const char *s = plat_symbol(pc, &off);
        if (!s && ours) s = rt_host_fn_name(pc, &off);
        const char *mod = di.dli_fname ? (strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname) : "?";
        uint64_t moff = pc - (uint64_t)(uintptr_t)di.dli_fbase;
        if (s && ours) fprintf(stderr, "  #%-2d %016llx %s+%llx (UltraTears+%llx)\n", i, (unsigned long long)pc, s,
                               (unsigned long long)off, (unsigned long long)moff);
        else if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx (%s)\n", i, (unsigned long long)pc, s, (unsigned long long)off, mod);
        else fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, mod, (unsigned long long)moff);
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    static volatile int depth;
    if (sig == SIGABRT && depth++ > 0) { signal(SIGABRT, SIG_DFL); abort(); }  // (nested: just die)
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    uint64_t pc = __darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    uint64_t lr = __darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
    if (rt_fault_watch(fa, pc)) return;
    char what[32];
    snprintf(what, sizeof what, "signal %d", sig);
    rt_fault_report(what, fa, pc, lr);
}

void plat_crash_handler_install(void) {
    stack_t ss = {.ss_sp = malloc(1 << 20), .ss_size = 1 << 20};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
}

// ---- child processes
#include <ftw.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path ? log_path : "/dev/null",
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC), 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawnattr_t at;  // (its own process group: plat_proc_kill ends what it starts)
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, argv[0], &fa, &at, (char *const *)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    return rc ? 0 : (PlatProc)pid;
}

void plat_proc_kill(PlatProc p) {
    kill(-(pid_t)p, SIGKILL);
    waitpid((pid_t)p, NULL, 0);
}

int plat_proc_wait(PlatProc p, bool block) {
    int st = 0;
    pid_t r = waitpid((pid_t)p, &st, block ? 0 : WNOHANG);
    if (r == 0) return -1;
    if (r < 0) return 255;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 255;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw) {
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}
bool plat_remove_tree(const char *dir) { return nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS) == 0; }

// End the process at once, skipping the library teardown (the GPU driver's threads while the game still submits).
void plat_terminate(void) { _exit(0); }
