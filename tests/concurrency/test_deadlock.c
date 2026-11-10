/**
 * @file test_deadlock.c
 * @brief Deadlock demo for Helgrind.
 *
 * In a multi-threaded supervisor (LLD 9.1 Alternative 1, rejected) two
 * locks would be needed: the process-table lock and the log lock.
 *   Monitor thread  : table lock -> log lock (logs a crash while updating the table)
 *   Dashboard thread: log lock -> table lock (writes the header, then reads the table)
 *   Built without -DFIX_DEADLOCK: the dashboard thread takes the locks in
 *   the opposite order. The threads run one after the other so the demo
 *   does not hang, and Helgrind still reports "lock order violated": if
 *   the two threads ever overlapped, each would wait for the other forever.
 *   Built with -DFIX_DEADLOCK: both threads take table -> log, 0 errors.
 *
 *   make helgrind-deadlock-buggy | make helgrind-deadlock-fixed
 */
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t g_tableLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_logLock = PTHREAD_MUTEX_INITIALIZER;
static int g_restartCount = 0;
static int g_logLines = 0;

/* Monitor thread: records a crash in the table, then logs it. */
static void *monitorWorker(void *arg)
{
    (void)arg;
    (void)pthread_mutex_lock(&g_tableLock);
    (void)pthread_mutex_lock(&g_logLock);
    g_restartCount++;
    g_logLines++;
    (void)pthread_mutex_unlock(&g_logLock);
    (void)pthread_mutex_unlock(&g_tableLock);
    return NULL;
}

/* Dashboard thread: writes the header line, then reads the table. */
static void *dashboardWorker(void *arg)
{
    (void)arg;
#ifdef FIX_DEADLOCK
    (void)pthread_mutex_lock(&g_tableLock);          /* same order as the monitor */
    (void)pthread_mutex_lock(&g_logLock);
#else
    (void)pthread_mutex_lock(&g_logLock);            /* BUG: opposite order */
    (void)pthread_mutex_lock(&g_tableLock);
#endif
    g_logLines++;
    (void)printf("Dashboard: %d restart(s) recorded\n", g_restartCount);
    (void)pthread_mutex_unlock(&g_tableLock);
    (void)pthread_mutex_unlock(&g_logLock);
    return NULL;
}

int main(void)
{
    pthread_t t1;
    pthread_t t2;

#ifdef FIX_DEADLOCK
    (void)pthread_create(&t1, NULL, monitorWorker, NULL);
    (void)pthread_create(&t2, NULL, dashboardWorker, NULL);
    (void)pthread_join(t1, NULL);
    (void)pthread_join(t2, NULL);
#else
    (void)pthread_create(&t1, NULL, monitorWorker, NULL);
    (void)pthread_join(t1, NULL);      /* buggy demo: run one after the other */
    (void)pthread_create(&t2, NULL, dashboardWorker, NULL);
    (void)pthread_join(t2, NULL);
#endif
    (void)printf("Log lines written: %d\n", g_logLines);
    return 0;
}
