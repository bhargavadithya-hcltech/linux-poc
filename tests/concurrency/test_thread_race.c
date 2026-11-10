/**
 * @file test_thread_race.c
 * @brief Race condition demo for Helgrind.
 *
 * LLD 9.1 Alternative 1 (rejected): one monitor thread per service. Here
 * four monitor threads record crashes of the same service in the shared
 * process table at the same time.
 *   Built without -DUSE_MUTEX: restartCount++ is not atomic, so updates
 *   are lost and Helgrind reports "Possible data race".
 *   Built with -DUSE_MUTEX: a table mutex protects the update, 0 errors.
 * The real supervisor avoids the problem by being single-threaded
 * (make helgrind-supervisor).
 *
 *   make helgrind-race-buggy | make helgrind-race-fixed
 */
#include <pthread.h>
#include <stdio.h>
#include "../../include/config.h"
#include "../../include/proctable.h"
#include "../../include/supervisor.h"

#define N_THREADS 4
#define CRASHES   100000

static Service *g_svc = NULL;

#ifdef USE_MUTEX
static pthread_mutex_t g_tableLock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void *monitorWorker(void *arg)
{
    long i;

    (void)arg;
    for (i = 0; i < CRASHES; i++) {
#ifdef USE_MUTEX
        (void)pthread_mutex_lock(&g_tableLock);
        g_svc->restartCount++;              /* protected critical section */
        g_svc->state = SVC_RESTARTING;
        (void)pthread_mutex_unlock(&g_tableLock);
#else
        g_svc->restartCount++;              /* BUG: unsynchronised write */
        g_svc->state = SVC_RESTARTING;
#endif
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[N_THREADS];
    ServiceConfig cfg;
    int i;

    ptInit(&gTable);
    initServiceConfig(&cfg);
    (void)snprintf(cfg.name, sizeof(cfg.name), "%s", "camera_service");
    (void)snprintf(cfg.command, sizeof(cfg.command), "%s", "/bin/true");
    g_svc = ptAdd(&gTable, &cfg);
    if (g_svc == NULL) {
        return 1;
    }

    for (i = 0; i < N_THREADS; i++) {
        (void)pthread_create(&threads[i], NULL, monitorWorker, NULL);
    }
    for (i = 0; i < N_THREADS; i++) {
        (void)pthread_join(threads[i], NULL);
    }

    (void)printf("Crashes recorded: expected %d, actual %d -> %s\n", N_THREADS * CRASHES, g_svc->restartCount,
                 (g_svc->restartCount == (N_THREADS * CRASHES)) ? "correct" : "UPDATES LOST (data race)");
    ptFree(&gTable);
    return 0;
}
