/**
 * @file supervisor.c
 * @brief Supervisor Core Module: command-line options, signal handling,
 *        daemon mode, the event loop and graceful shutdown.
 *
 * Signal handlers only set flags. All real work (reaping, logging,
 * restarting, printing) is done in runEventLoop(), never inside a handler.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "config.h"
#include "dashboard.h"
#include "launcher.h"
#include "logger.h"
#include "monitor.h"
#include "proctable.h"
#include "restart.h"
#include "supervisor.h"

SupervisorContext gCtx;
volatile sig_atomic_t gChildExited = 0;
volatile sig_atomic_t gStopRequested = 0;
volatile sig_atomic_t gStatusRequested = 0;

/* ------------------------------------------------------------ options */
static int setPath(char *dest, const char *value)
{
    int status = SUP_OK;

    if ((value == NULL) || (value[0] == '\0') || (strlen(value) >= (size_t)MAX_PATH)) {
        status = ERR_USAGE;
    } else {
        (void)snprintf(dest, (size_t)MAX_PATH, "%s", value);
    }
    return status;
}

int parseArguments(int argc, char *argv[], SupervisorContext *ctx)
{
    int status = SUP_OK;
    int opt;

    (void)memset(ctx, 0, sizeof(*ctx));
    (void)snprintf(ctx->configPath, sizeof(ctx->configPath), "%s", DEFAULT_CONFIG_FILE);
    (void)snprintf(ctx->logPath, sizeof(ctx->logPath), "%s", DEFAULT_LOG_FILE);
    (void)snprintf(ctx->pidPath, sizeof(ctx->pidPath), "%s", DEFAULT_PID_FILE);
    (void)snprintf(ctx->statusPath, sizeof(ctx->statusPath), "%s", DEFAULT_STATUS_FILE);

    opterr = 0;                  /* we print our own usage text */
    optind = 1;                  /* allow repeated calls (tests) */
    opt = getopt(argc, argv, "c:dh");
    while ((opt != -1) && (status == SUP_OK)) {
        switch (opt) {
        case 'c': status = setPath(ctx->configPath, optarg); break;
        case 'd': ctx->daemonMode = 1; break;
        case 'h': status = SUP_HELP; break;
        default:  status = ERR_USAGE; break;
        }
        opt = getopt(argc, argv, "c:dh");
    }
    if ((status == SUP_OK) && (optind < argc)) {
        status = ERR_USAGE;      /* unexpected extra argument */
    }
    return status;
}

void printUsage(void)
{
    (void)printf("Usage: ./supervisor [-c <config file>] [-d] [-h]\n"
                 "  -c <file>  Configuration file (default: %s)\n"
                 "  -d         Run as daemon (dashboard goes to %s)\n"
                 "  -h         Show this help\n"
                 "Control:  kill -USR1 $(cat %s)   show status dashboard\n"
                 "          kill -TERM $(cat %s)   stop supervisor (or Ctrl+C)\n",
                 DEFAULT_CONFIG_FILE, DEFAULT_STATUS_FILE, DEFAULT_PID_FILE, DEFAULT_PID_FILE);
}

/* ------------------------------------------------------------ signals */
void handleSignal(int sig)
{
    if (sig == SIGCHLD) {
        gChildExited = 1;
    } else if ((sig == SIGTERM) || (sig == SIGINT)) {
        gStopRequested = 1;
    } else if (sig == SIGUSR1) {
        gStatusRequested = 1;
    } else {
        /* not handled */
    }
}

int installSignalHandlers(void)
{
    int status = SUP_OK;
    struct sigaction sa;
    const int signals[] = { SIGCHLD, SIGTERM, SIGINT, SIGUSR1 };
    size_t i;

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handleSignal;
    (void)sigfillset(&sa.sa_mask);
    /* no SA_RESTART: poll() must return early when a signal arrives */
    for (i = 0U; i < (sizeof(signals) / sizeof(signals[0])); i++) {
        sa.sa_flags = (signals[i] == SIGCHLD) ? SA_NOCLDSTOP : 0;
        if (sigaction(signals[i], &sa, NULL) != 0) {
            status = ERR_USAGE;
        }
    }
    if (status != SUP_OK) {
        logMsg(LOG_ERROR, NULL, "sigaction failed: %s", strerror(errno));
    }
    return status;
}

/* ------------------------------------------------------------- daemon */
int daemonize(void)
{
    int status = SUP_OK;
    pid_t pid;

    (void)fflush(NULL);
    pid = fork();
    if (pid < 0) {
        logError(NULL, ERR_FORK, "daemonize");
        status = ERR_FORK;
    } else if (pid > 0) {
        _exit(0);                /* parent returns the shell prompt */
    } else {
        (void)setsid();          /* new session, no controlling terminal */
        (void)umask((mode_t)022);   /* PID / status files 0644, log 0640 (explicit) */
        if ((freopen("/dev/null", "r", stdin) == NULL) || (freopen("/dev/null", "w", stdout) == NULL) ||
            (freopen("/dev/null", "w", stderr) == NULL)) {
            logMsg(LOG_WARN, NULL, "could not redirect standard streams to /dev/null");
        }
    }
    return status;
}

int writePidFile(const char *path)
{
    int status = SUP_OK;
    FILE *fp = NULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, (mode_t)0644);

    if (fd >= 0) {
        fp = fdopen(fd, "w");
        if (fp == NULL) {
            (void)close(fd);
        }
    }
    if (fp == NULL) {
        logError(NULL, ERR_FILE, path);
        status = ERR_FILE;
    } else {
        (void)fprintf(fp, "%d\n", (int)getpid());
        (void)fclose(fp);
    }
    return status;
}

/* --------------------------------------------------------- event loop */
void runEventLoop(const SupervisorContext *ctx, ProcessTable *table)
{
    while (gStopRequested == 0) {
        (void)poll(NULL, 0U, TICK_MS);           /* sleeps; a signal ends it early */
        if (gStopRequested == 0) {
            /* waitpid(WNOHANG) is cheap, so reap on every pass: a SIGCHLD that
             * arrives just before poll() is then never missed */
            gChildExited = 0;
            reapChildren(ctx, table);
            checkHeartbeats(table);
            updateResourceUsage(table);
            processPendingRestarts(table);
            if (gStatusRequested != 0) {
                gStatusRequested = 0;
                (void)showDashboard(ctx, table);
            }
        }
    }
}

/* ----------------------------------------------------------- shutdown */
static int countRunning(const ProcessTable *table)
{
    int running = 0;
    const Service *svc;

    for (svc = table->head; svc != NULL; svc = svc->next) {
        if (svc->pid > 0) {
            running++;
        }
    }
    return running;
}

static void stopService(Service *svc)
{
    if (svc->pid > 0) {
        logMsg(LOG_INFO, svc, "sending SIGTERM");
        (void)kill(svc->pid, SIGTERM);
    }
}

void shutdownSupervisor(SupervisorContext *ctx, ProcessTable *table)
{
    const struct timespec pause = { 0, 100000000L };   /* 100 ms */
    time_t deadline;
    Service *svc;
    int status = 0;

    ctx->stopping = 1;
    logMsg(LOG_INFO, NULL, "stop requested - stopping %d service(s)", countRunning(table));

    /* reverse start order: dependents stop before what they depend on */
    if (gStartCount == table->count) {
        int i;

        for (i = gStartCount - 1; i >= 0; i--) {
            stopService(gStartOrder[i]);
        }
    } else {
        for (svc = table->head; svc != NULL; svc = svc->next) {
            stopService(svc);
        }
    }

    deadline = time(NULL) + (time_t)STOP_TIMEOUT_SEC;
    reapChildren(ctx, table);
    while ((countRunning(table) > 0) && (time(NULL) < deadline)) {
        (void)nanosleep(&pause, NULL);
        reapChildren(ctx, table);
    }
    for (svc = table->head; svc != NULL; svc = svc->next) {
        if (svc->pid > 0) {
            logMsg(LOG_WARN, svc, "did not stop within %d s - sending SIGKILL", STOP_TIMEOUT_SEC);
            (void)kill(svc->pid, SIGKILL);
            (void)waitpid(svc->pid, &status, 0);
            svc->pid = 0;
            svc->state = SVC_STOPPED;
        }
    }

    logMsg(LOG_INFO, NULL, "I004 supervisor stopped");
    (void)unlink(ctx->pidPath);
    ptFree(table);
    gStartCount = 0;
    logClose();
}

/* ----------------------------------------------------- program flow */
#ifdef LEAK_DEMO
/* Valgrind demo only (make valgrind): 128 bytes allocated in the PARENT
 * process (the supervisor) and never freed -> "definitely lost" under the
 * parent's PID. Not compiled into the normal build. */
static void parentLeakDemo(void)
{
    char *leak = (char *)malloc(128U);

    if (leak != NULL) {
        (void)snprintf(leak, 128U, "%s", "parent (supervisor) leak");
    }
    /* cppcheck-suppress memleak ; intentional leak for the Valgrind demo */
}
#endif

int runSupervisor(int argc, char *argv[])
{
    int exitCode = 0;
    int status = parseArguments(argc, argv, &gCtx);

    if (status == SUP_HELP) {
        printUsage();
    } else if (status != SUP_OK) {
        (void)fprintf(stderr, "Invalid option.\n");
        printUsage();
        exitCode = 1;
    } else {
        ptInit(&gTable);
        gStartCount = 0;
        if (logInit(gCtx.logPath, (gCtx.daemonMode == 0) ? 1 : 0) != SUP_OK) {
            (void)fprintf(stderr, "%s %s: %s (logging to stderr)\n", errorCodeText(ERR_FILE),
                          errorMessage(ERR_FILE), gCtx.logPath);
        }
        status = loadConfig(gCtx.configPath, &gTable);
        if (status == SUP_OK) {
            status = validateConfig(&gTable);
        }
        if (status == SUP_OK) {
            status = resolveStartOrder(&gTable, gStartOrder, &gStartCount);
        }
        if (status != SUP_OK) {
            (void)fprintf(stderr, "Supervisor not started: %s %s. Fix %s and start again.\n",
                          errorCodeText(status), errorMessage(status), gCtx.configPath);
            ptFree(&gTable);
            logClose();
            exitCode = 1;
        } else {
            logMsg(LOG_INFO, NULL, "configuration valid: %d service(s) from %s", gStartCount, gCtx.configPath);
            if (gCtx.daemonMode == 0) {
                printStartupSummary(stdout, &gCtx, gStartOrder, gStartCount);
            }
            (void)installSignalHandlers();
            if ((gCtx.daemonMode != 0) && (daemonize() != SUP_OK)) {
                gCtx.daemonMode = 0;     /* could not detach: keep running in the foreground */
            }
            gCtx.startTime = time(NULL);
            (void)writePidFile(gCtx.pidPath);
            logMsg(LOG_INFO, NULL, "I001 supervisor started (%s mode)", (gCtx.daemonMode != 0) ? "daemon" : "foreground");
            (void)launchAll(gStartOrder, gStartCount);
#ifdef LEAK_DEMO
            parentLeakDemo();
#endif
            runEventLoop(&gCtx, &gTable);
            shutdownSupervisor(&gCtx, &gTable);
        }
    }
    return exitCode;
}
