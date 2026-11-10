/**
 * @file config.c
 * @brief Config Reader Module: parses supervisor.conf, validates every entry
 *        and resolves the dependency start order.
 */
#include <ctype.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "logger.h"
#include "proctable.h"

Service *gStartOrder[MAX_SERVICES];
int gStartCount = 0;

/* ------------------------------------------------------------- helpers */
/* Removes leading and trailing blanks (and the newline) in place. */
static char *trim(char *text)
{
    char *start = text;
    size_t len;

    while ((*start != '\0') && (isspace((unsigned char)*start) != 0)) {
        start++;
    }
    len = strlen(start);
    while ((len > 0U) && (isspace((unsigned char)start[len - 1U]) != 0)) {
        start[len - 1U] = '\0';
        len--;
    }
    return start;
}

/* Copies value into dest (size bytes). Returns SUP_OK or ERR_CONFIG_ENTRY
 * when the value is empty or too long. */
static int copyValue(char *dest, size_t size, const char *value)
{
    int status = SUP_OK;

    if ((value[0] == '\0') || (strlen(value) >= size)) {
        status = ERR_CONFIG_ENTRY;
    } else {
        (void)snprintf(dest, size, "%s", value);
    }
    return status;
}

/* Converts value (in the given base) to an int in [minValue, maxValue]. */
static int parseNumber(const char *value, int base, long minValue, long maxValue, long *out)
{
    int status = SUP_OK;
    char *end = NULL;

    if (value[0] == '\0') {
        status = ERR_CONFIG_ENTRY;
    } else {
        long number = strtol(value, &end, base);
        if ((end == NULL) || (*end != '\0') || (number < minValue) || (number > maxValue)) {
            status = ERR_CONFIG_ENTRY;
        } else {
            *out = number;
        }
    }
    return status;
}

/* Service names: letters, digits, '_' and '-' only. */
static int isValidName(const char *name)
{
    int valid = (name[0] != '\0') ? 1 : 0;
    size_t i;

    for (i = 0U; (name[i] != '\0') && (valid == 1); i++) {
        if ((isalnum((unsigned char)name[i]) == 0) && (name[i] != '_') && (name[i] != '-')) {
            valid = 0;
        }
    }
    return valid;
}

/* depends_on=a,b,c  (empty value = no dependencies) */
static int parseDependencies(char *value, ServiceConfig *cfg)
{
    int status = SUP_OK;
    char *saveptr = NULL;
    char *token;

    cfg->depCount = 0;
    token = strtok_r(value, ",", &saveptr);
    while ((token != NULL) && (status == SUP_OK)) {
        const char *name = trim(token);
        if (name[0] != '\0') {
            if ((cfg->depCount >= MAX_DEPS) || (isValidName(name) == 0) ||
                (copyValue(cfg->dependsOn[cfg->depCount], MAX_NAME, name) != SUP_OK)) {
                status = ERR_CONFIG_ENTRY;
            } else {
                cfg->depCount++;
            }
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    return status;
}

/* ------------------------------------------------------------- parsing */
void initServiceConfig(ServiceConfig *cfg)
{
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->maxRestarts = DEFAULT_MAX_RESTARTS;
    cfg->restartDelay = DEFAULT_RESTART_DELAY;
    cfg->umaskValue = -1;
}

int parseServiceLine(const char *line, ServiceConfig *cfg)
{
    int status = SUP_OK;
    char buf[LINE_SIZE];
    char *eq;
    long number = 0;

    if ((line == NULL) || (cfg == NULL) || (strlen(line) >= sizeof(buf))) {
        status = ERR_CONFIG_ENTRY;
    } else {
        (void)snprintf(buf, sizeof(buf), "%s", line);
        eq = strchr(buf, '=');
        if (eq == NULL) {
            status = ERR_CONFIG_ENTRY;
        } else {
            const char *key;
            char *value;

            *eq = '\0';
            key = trim(buf);
            value = trim(eq + 1);
            if (strcmp(key, "name") == 0) {
                status = (isValidName(value) == 1) ? copyValue(cfg->name, sizeof(cfg->name), value)
                                                   : ERR_CONFIG_ENTRY;
            } else if (strcmp(key, "command") == 0) {
                status = copyValue(cfg->command, sizeof(cfg->command), value);
            } else if (strcmp(key, "max_restarts") == 0) {
                status = parseNumber(value, 10, 0L, (long)MAX_RESTART_LIMIT, &number);
                cfg->maxRestarts = (status == SUP_OK) ? (int)number : cfg->maxRestarts;
            } else if (strcmp(key, "restart_delay") == 0) {
                status = parseNumber(value, 10, (long)MIN_RESTART_DELAY, (long)MAX_RESTART_DELAY, &number);
                cfg->restartDelay = (status == SUP_OK) ? (int)number : cfg->restartDelay;
            } else if (strcmp(key, "heartbeat_timeout") == 0) {
                status = parseNumber(value, 10, 0L, (long)MAX_HEARTBEAT_TIMEOUT, &number);
                cfg->heartbeatTimeout = (status == SUP_OK) ? (int)number : cfg->heartbeatTimeout;
            } else if (strcmp(key, "umask") == 0) {
                status = parseNumber(value, 8, 0L, (long)MAX_UMASK, &number);
                cfg->umaskValue = (status == SUP_OK) ? (int)number : cfg->umaskValue;
            } else if (strcmp(key, "mem_limit_kb") == 0) {
                status = parseNumber(value, 10, 0L, LONG_MAX, &number);
                cfg->memLimitKb = (status == SUP_OK) ? number : cfg->memLimitKb;
            } else if (strcmp(key, "depends_on") == 0) {
                status = parseDependencies(value, cfg);
            } else if (strcmp(key, "user") == 0) {
                status = copyValue(cfg->user, sizeof(cfg->user), value);
            } else if (strcmp(key, "group") == 0) {
                status = copyValue(cfg->group, sizeof(cfg->group), value);
            } else {
                status = ERR_CONFIG_ENTRY;   /* unknown key */
            }
        }
    }
    return status;
}

/* Adds a finished [service] block to the table. */
static int addEntry(ProcessTable *table, const ServiceConfig *cfg)
{
    int status = SUP_OK;

    if (table->count >= MAX_SERVICES) {
        logError(NULL, ERR_CONFIG_ENTRY, "more than 32 services configured");
        status = ERR_CONFIG_ENTRY;
    } else if (ptAdd(table, cfg) == NULL) {
        logError(NULL, ERR_MEMORY, "ptAdd");
        status = ERR_MEMORY;
    } else {
        /* added */
    }
    return status;
}

int loadConfig(const char *path, ProcessTable *table)
{
    int status = SUP_OK;
    FILE *fp = NULL;
    char raw[LINE_SIZE];
    ServiceConfig cfg;
    int lineNo = 0;

    initServiceConfig(&cfg);
    if ((path == NULL) || (table == NULL)) {
        status = ERR_CONFIG_FILE;
    } else {
        fp = fopen(path, "r");
        if (fp == NULL) {
            status = ERR_CONFIG_FILE;
        }
    }
    if (fp != NULL) {
        int inService = 0;

        while ((status == SUP_OK) && (fgets(raw, (int)sizeof(raw), fp) != NULL)) {
            lineNo++;
            if ((strchr(raw, '\n') == NULL) && (feof(fp) == 0)) {
                status = ERR_CONFIG_ENTRY;          /* line longer than LINE_SIZE */
            } else {
                const char *line = trim(raw);
                if ((line[0] == '\0') || (line[0] == '#')) {
                    /* blank line or comment */
                } else if (strcmp(line, "[service]") == 0) {
                    if (inService == 1) {
                        status = addEntry(table, &cfg);
                    }
                    initServiceConfig(&cfg);
                    inService = 1;
                } else if (inService == 0) {
                    status = ERR_CONFIG_ENTRY;      /* key outside a [service] block */
                } else {
                    status = parseServiceLine(line, &cfg);
                }
            }
        }
        if ((status == SUP_OK) && (inService == 1)) {
            status = addEntry(table, &cfg);
        }
        if ((status == SUP_OK) && (table->count == 0)) {
            status = ERR_CONFIG_ENTRY;
            lineNo = 0;
        }
        (void)fclose(fp);
    }

    if (status == ERR_CONFIG_FILE) {
        logError(NULL, status, (path != NULL) ? path : "(null)");
    } else if (status == ERR_CONFIG_ENTRY) {
        char detail[MAX_PATH + 64];

        if (lineNo > 0) {
            (void)snprintf(detail, sizeof(detail), "%s line %d", path, lineNo);
        } else {
            (void)snprintf(detail, sizeof(detail), "%s has no [service] entries", path);
        }
        logError(NULL, status, detail);
    } else {
        /* SUP_OK or ERR_MEMORY (already logged) */
    }
    return status;
}

/* ---------------------------------------------------------- validation */
/* Unknown users/groups are errors; when not running as root they are
 * cleared with warning W001 because setuid()/setgid() would fail anyway. */
static int validatePermissions(Service *svc)
{
    int status = SUP_OK;

    if ((svc->cfg.user[0] != '\0') || (svc->cfg.group[0] != '\0')) {
        if ((svc->cfg.user[0] != '\0') && (getpwnam(svc->cfg.user) == NULL)) {
            logError(svc, ERR_CONFIG_ENTRY, "unknown user");
            status = ERR_CONFIG_ENTRY;
        } else if ((svc->cfg.group[0] != '\0') && (getgrnam(svc->cfg.group) == NULL)) {
            logError(svc, ERR_CONFIG_ENTRY, "unknown group");
            status = ERR_CONFIG_ENTRY;
        } else if (geteuid() != 0U) {
            logMsg(LOG_WARN, svc, "W001 not running as root - user/group settings ignored");
            svc->cfg.user[0] = '\0';
            svc->cfg.group[0] = '\0';
        } else {
            /* root: settings will be applied by the launcher */
        }
    }
    return status;
}

int validateConfig(ProcessTable *table)
{
    int status = SUP_OK;
    int i;
    Service *svc;

    svc = (table != NULL) ? table->head : NULL;
    while (svc != NULL) {
        int result = SUP_OK;

        if ((svc->cfg.name[0] == '\0') || (svc->cfg.command[0] == '\0')) {
            logError(svc, ERR_CONFIG_ENTRY, "name and command are mandatory");
            result = ERR_CONFIG_ENTRY;
        } else if (ptFindByName(table, svc->cfg.name) != svc) {
            logError(svc, ERR_DUPLICATE, svc->cfg.name);
            result = ERR_DUPLICATE;
        } else {
            for (i = 0; (i < svc->cfg.depCount) && (result == SUP_OK); i++) {
                if ((strcmp(svc->cfg.dependsOn[i], svc->cfg.name) == 0) ||
                    (ptFindByName(table, svc->cfg.dependsOn[i]) == NULL)) {
                    logError(svc, ERR_DEPENDENCY, svc->cfg.dependsOn[i]);
                    result = ERR_DEPENDENCY;
                }
            }
            if (result == SUP_OK) {
                result = validatePermissions(svc);
            }
        }
        if (status == SUP_OK) {
            status = result;     /* keep the first error, but report all of them */
        }
        svc = svc->next;
    }
    if (table == NULL) {
        status = ERR_CONFIG_ENTRY;
    }
    return status;
}

/* --------------------------------------------------------- start order */
/* cppcheck-suppress constParameter ; Service *order[] is passed in, C cannot add an inner const */
static int isPlaced(Service *const order[], int count, const char *name)
{
    int found = 0;
    int i;

    for (i = 0; (i < count) && (found == 0); i++) {
        if (strcmp(order[i]->cfg.name, name) == 0) {
            found = 1;
        }
    }
    return found;
}

static int dependenciesPlaced(const Service *svc, Service *const order[], int count)
{
    int ready = 1;
    int i;

    for (i = 0; (i < svc->cfg.depCount) && (ready == 1); i++) {
        ready = isPlaced(order, count, svc->cfg.dependsOn[i]);
    }
    return ready;
}

int resolveStartOrder(ProcessTable *table, Service *order[], int *count)
{
    int status = SUP_OK;
    Service *svc;

    if ((table == NULL) || (order == NULL) || (count == NULL) || (table->count > MAX_SERVICES)) {
        status = ERR_DEPENDENCY;
    } else {
        int placed = 0;
        int progress = 1;

        /* Repeated passes: place every service whose dependencies are placed. */
        while ((placed < table->count) && (progress == 1)) {
            progress = 0;
            for (svc = table->head; svc != NULL; svc = svc->next) {
                if ((isPlaced(order, placed, svc->cfg.name) == 0) &&
                    (dependenciesPlaced(svc, order, placed) == 1)) {
                    order[placed] = svc;
                    placed++;
                    progress = 1;
                }
            }
        }
        if (placed < table->count) {
            for (svc = table->head; svc != NULL; svc = svc->next) {
                if (isPlaced(order, placed, svc->cfg.name) == 0) {
                    logError(svc, ERR_DEPENDENCY, "circular dependency");
                }
            }
            status = ERR_DEPENDENCY;
        }
        *count = placed;
    }
    return status;
}
