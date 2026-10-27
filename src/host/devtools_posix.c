// The development tools that are Windows-only so far (watch.c: TOTK_WATCH write watchpoints with guard pages;
// profiler.c: TOTK_PROF thread sampling), as stand-ins elsewhere (fork: macOS). Turning one on says so in the log.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#include "host.h"

void runtime_profiler_start(void) {
    if (getenv("TOTK_PROF")) fprintf(stderr, "[prof] TOTK_PROF: the sampling profiler is Windows-only so far\n");
}

void runtime_watch_tick(uint64_t present) {
    static int said;
    (void)present;
    if (!said && getenv("TOTK_WATCH")) { said = 1; fprintf(stderr, "[watch] TOTK_WATCH: Windows-only so far\n"); }
}

uint64_t runtime_symbolize(uint64_t rip, char *out, size_t n) {
    Dl_info di = {0};
    if (dladdr((void *)(uintptr_t)rip, &di) && di.dli_sname) {
        snprintf(out, n, "%s", di.dli_sname);
        return rip - (uint64_t)(uintptr_t)di.dli_saddr;
    }
    snprintf(out, n, "%#llx", (unsigned long long)rip);
    return 0;
}
