/**
 * @file demo_service.c
 * @brief Sample ADAS service used to run and test the supervisor.
 *
 * Usage: demo_service <name> [--crash-after N] [--hang-after N]
 *                            [--exit-after N] [--mem-kb N] [--ignore-term]
 *
 *   --crash-after N  raise SIGSEGV after N seconds (simulated crash)
 *   --hang-after N   stop sending heartbeats after N seconds (simulated hang)
 *   --exit-after N   exit normally (code 0) after N seconds
 *   --mem-kb N       allocate and touch N KB (tests the memory-limit warning)
 *   --ignore-term    ignore SIGTERM (tests the SIGKILL fallback on shutdown)
 *
 * When the supervisor sets HEARTBEAT_FD, one byte is written to that file
 * descriptor every second.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void onTerm(int sig)
{
    (void)sig;
    g_stop = 1;
}

#ifdef LEAK_DEMO
/* Valgrind demo only (make valgrind): 64 bytes allocated in the CHILD
 * process (the service) and never freed -> "definitely lost" under the
 * child's PID. Not compiled into the normal build. */
static void childLeakDemo(const char *name)
{
    char *leak = (char *)malloc(64U);

    if (leak != NULL) {
        (void)snprintf(leak, 64U, "child leak: %s", name);
    }
}
#endif

static int hasFlag(int argc, char *argv[], const char *name)
{
    int found = 0;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            found = 1;
        }
    }
    return found;
}

static long optionValue(int argc, char *argv[], const char *name)
{
    long value = -1L;
    int i;

    for (i = 2; i < (argc - 1); i++) {
        if (strcmp(argv[i], name) == 0) {
            value = strtol(argv[i + 1], NULL, 10);
        }
    }
    return value;
}

int main(int argc, char *argv[])
{
    const char *name = (argc > 1) ? argv[1] : "demo_service";
    const char *hbEnv = getenv("HEARTBEAT_FD");
    int hbFd = (hbEnv != NULL) ? (int)strtol(hbEnv, NULL, 10) : -1;
    long crashAfter = optionValue(argc, argv, "--crash-after");
    long hangAfter = optionValue(argc, argv, "--hang-after");
    long exitAfter = optionValue(argc, argv, "--exit-after");
    long memKb = optionValue(argc, argv, "--mem-kb");
    char *block = NULL;
    long seconds = 0L;
    struct sigaction sa;

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = (hasFlag(argc, argv, "--ignore-term") == 1) ? SIG_IGN : onTerm;
    (void)sigaction(SIGTERM, &sa, NULL);

    if (memKb > 0L) {
        block = (char *)malloc((size_t)memKb * 1024U);
        if (block != NULL) {
            (void)memset(block, 1, (size_t)memKb * 1024U);
        }
    }
    (void)printf("[%s] started (pid %d)\n", name, (int)getpid());
    (void)fflush(stdout);
#ifdef LEAK_DEMO
    childLeakDemo(name);
#endif

    while (g_stop == 0) {
        if ((hbFd >= 0) && ((hangAfter < 0L) || (seconds < hangAfter))) {
            (void)write(hbFd, "h", 1U);
        }
        if ((crashAfter >= 0L) && (seconds >= crashAfter)) {
            (void)printf("[%s] simulating crash (SIGSEGV)\n", name);
            (void)fflush(stdout);
            (void)raise(SIGSEGV);
        }
        if ((exitAfter >= 0L) && (seconds >= exitAfter)) {
            g_stop = 1;
        } else {
            (void)sleep(1U);
            seconds++;
        }
    }
    free(block);
    (void)printf("[%s] stopping\n", name);
    return 0;
}
