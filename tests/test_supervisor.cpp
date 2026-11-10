/**
 * @file test_supervisor.cpp
 * @brief GoogleTest suite for the ADAS ECU Process Supervisor.
 *
 * One fixture per module. Every test starts from test_reset_system():
 * default signal handlers, cleared flags, an empty process table, a fresh
 * temporary folder and a fresh log. Test names follow
 * functionUnderTest_Scenario. Each function has positive, negative and
 * boundary cases where they apply. The last section holds the
 * cross-module integration tests (real child processes, real signals).
 */
#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_shim.h"

/* text checks used throughout */
#define EXPECT_CONTAINS(text, part) EXPECT_NE(std::strstr((text), (part)), nullptr) << "missing: " << (part)
#define EXPECT_NOT_CONTAINS(text, part) EXPECT_EQ(std::strstr((text), (part)), nullptr) << "unexpected: " << (part)

/* ========================================================================= */
/* Test fixtures                                                             */
/* ========================================================================= */
class SupTest : public ::testing::Test {
protected:
    void SetUp() override { test_reset_system(); }
    void TearDown() override { test_cleanup_system(); }
};

class LoggerTest : public SupTest {};
class ProcessTableTest : public SupTest {};
class ConfigTest : public SupTest {};
class LauncherTest : public SupTest {};
class MonitorTest : public SupTest {};
class RestartTest : public SupTest {};
class DashboardTest : public SupTest {};
class SupervisorCoreTest : public SupTest {};
class IntegrationTest : public SupTest {};

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */
static int parseLine(const char *line, ServiceConfig *cfg)
{
    return parseServiceLine(line, cfg);
}

/* Runs runSupervisor() inside the test folder with the given arguments. */
static int runInTestDir(int argc, const char *const args[])
{
    char *argv[8];
    char cwd[MAX_PATH];
    int code;

    for (int i = 0; i < argc; i++) {
        argv[i] = const_cast<char *>(args[i]);
    }
    argv[argc] = nullptr;
    EXPECT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
    EXPECT_EQ(chdir(testDir()), 0);
    captureStdoutBegin();
    code = runSupervisor(argc, argv);
    (void)captureStdoutEnd();
    EXPECT_EQ(chdir(cwd), 0);
    return code;
}

/* ========================================================================= */
/* Logger module                                                             */
/* ========================================================================= */
TEST_F(LoggerTest, logInit_ValidPath_CreatesFileWithMode0640)
{
    EXPECT_EQ(logIsOpen(), 1);
    logMsg(LOG_INFO, nullptr, "hello");
    EXPECT_EQ(fileMode("test.log"), 0640);
}

TEST_F(LoggerTest, logInit_NullOrEmptyPath_ReturnsErrFile)
{
    EXPECT_EQ(logInit(nullptr, 0), ERR_FILE);
    EXPECT_EQ(logIsOpen(), 0);
    EXPECT_EQ(logInit("", 0), ERR_FILE);
    EXPECT_EQ(logIsOpen(), 0);
}

TEST_F(LoggerTest, logInit_MissingFolder_ReturnsErrFile)
{
    EXPECT_EQ(logInit(testPath("no_such_dir/x.log"), 0), ERR_FILE);
    EXPECT_EQ(logIsOpen(), 0);
}

TEST_F(LoggerTest, logInit_PathTooLong_ReturnsErrFile)
{
    std::string longPath(MAX_PATH + 5, 'a');
    EXPECT_EQ(logInit(longPath.c_str(), 0), ERR_FILE);
}

TEST_F(LoggerTest, logMsg_Supervisor_HasTimestampLevelAndPid)
{
    logMsg(LOG_WARN, nullptr, "value %d", 42);
    const char *log = logText();
    char who[64];
    std::snprintf(who, sizeof(who), "[WARN ] [supervisor:%d] value 42", (int)getpid());
    EXPECT_CONTAINS(log, who);
    EXPECT_EQ(log[0], '[');
    EXPECT_EQ(log[20], ']');           /* "[YYYY-MM-DD HH:MM:SS]" */
}

TEST_F(LoggerTest, logMsg_Service_UsesNameAndPid)
{
    Service *svc = addService("camera_service", "/bin/true");
    svc->pid = 1234;
    logMsg(LOG_ERROR, svc, "boom");
    EXPECT_CONTAINS(logText(), "[ERROR] [camera_service:1234] boom");
}

TEST_F(LoggerTest, logMsg_LogClosed_FallsBackToStderr)
{
    logClose();
    EXPECT_EQ(logIsOpen(), 0);
    logMsg(LOG_INFO, nullptr, "to stderr");       /* must not crash */
    EXPECT_STREQ(logText(), "");
}

TEST_F(LoggerTest, logMsg_ConsoleEcho_PrintsToStdout)
{
    ASSERT_EQ(logInit(testPath("test.log"), 1), SUP_OK);
    captureStdoutBegin();
    logMsg(LOG_INFO, nullptr, "echo me");
    EXPECT_CONTAINS(captureStdoutEnd(), "echo me");
}

TEST_F(LoggerTest, logMsg_PreservesErrno)
{
    errno = ENOENT;
    logMsg(LOG_INFO, nullptr, "x");
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(LoggerTest, logError_AddsCodeMessageAndDetail)
{
    logError(nullptr, ERR_DUPLICATE, "camera_service");
    EXPECT_CONTAINS(logText(), "E003 Duplicate service name | camera_service");
    logError(nullptr, ERR_FILE, nullptr);
    EXPECT_CONTAINS(logText(), "E009 Log / PID / status file error\n");
}

TEST_F(LoggerTest, errorCodeText_Boundaries)
{
    EXPECT_STREQ(errorCodeText(SUP_OK), "E000");
    EXPECT_STREQ(errorCodeText(ERR_CONFIG_FILE), "E001");
    EXPECT_STREQ(errorCodeText(ERR_MAX_RESTARTS), "E011");
    EXPECT_STREQ(errorCodeText(ERR_MAX_RESTARTS + 1), "E000");
    EXPECT_STREQ(errorCodeText(-1), "E000");
}

TEST_F(LoggerTest, errorMessage_EveryCodeHasText)
{
    for (int code = SUP_OK; code <= ERR_USAGE; code++) {
        EXPECT_STRNE(errorMessage(code), "Unknown error") << code;
    }
    EXPECT_STREQ(errorMessage(99), "Unknown error");
}

TEST_F(LoggerTest, logLevelName_AllLevels)
{
    EXPECT_STREQ(logLevelName(LOG_INFO), "INFO ");
    EXPECT_STREQ(logLevelName(LOG_WARN), "WARN ");
    EXPECT_STREQ(logLevelName(LOG_ERROR), "ERROR");
    EXPECT_STREQ(logLevelName(static_cast<LogLevel>(9)), "?????");
}

TEST_F(LoggerTest, getTimestamp_FormatAndTinyBuffer)
{
    char buf[24];
    getTimestamp(buf, sizeof(buf));
    EXPECT_EQ(std::strlen(buf), 19U);
    EXPECT_EQ(buf[4], '-');
    EXPECT_EQ(buf[13], ':');
    char tiny[4] = "abc";
    getTimestamp(tiny, sizeof(tiny));           /* too small: empty, no overflow */
    EXPECT_STREQ(tiny, "");
    getTimestamp(nullptr, 10U);                 /* must not crash */
}

TEST_F(LoggerTest, rotateLog_AtOneMegabyte_KeepsOneBackup)
{
    std::string line(200, 'x');
    for (int i = 0; i < 6000; i++) {           /* ~1.4 MB */
        logMsg(LOG_INFO, nullptr, "%s", line.c_str());
    }
    EXPECT_EQ(fileExists("test.log.1"), 1);
    struct stat st;
    ASSERT_EQ(stat(testPath("test.log.1"), &st), 0);
    EXPECT_GE(st.st_size, LOG_MAX_SIZE);
    ASSERT_EQ(stat(testPath("test.log"), &st), 0);
    EXPECT_LT(st.st_size, LOG_MAX_SIZE);
}

/* ========================================================================= */
/* Process Table module                                                      */
/* ========================================================================= */
TEST_F(ProcessTableTest, ptAdd_KeepsConfigOrderAndCount)
{
    addService("a", "/bin/true");
    addService("b", "/bin/true");
    addService("c", "/bin/true");
    ASSERT_EQ(gTable.count, 3);
    EXPECT_STREQ(gTable.head->cfg.name, "a");
    EXPECT_STREQ(gTable.head->next->cfg.name, "b");
    EXPECT_STREQ(gTable.head->next->next->cfg.name, "c");
}

TEST_F(ProcessTableTest, ptAdd_NewNodeDefaults)
{
    Service *s = addService("a", "/bin/true");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->pid, 0);
    EXPECT_EQ(s->state, SVC_STOPPED);
    EXPECT_EQ(s->hbReadFd, -1);
    EXPECT_EQ(s->restartCount, 0);
}

TEST_F(ProcessTableTest, ptAdd_NullArguments_ReturnNull)
{
    ServiceConfig cfg = makeConfig("a", "/bin/true");
    EXPECT_EQ(ptAdd(nullptr, &cfg), nullptr);
    EXPECT_EQ(ptAdd(&gTable, nullptr), nullptr);
    EXPECT_EQ(gTable.count, 0);
}

TEST_F(ProcessTableTest, ptFindByName_FoundNotFoundNull)
{
    Service *b = (addService("a", "x"), addService("b", "y"));
    EXPECT_EQ(ptFindByName(&gTable, "b"), b);
    EXPECT_EQ(ptFindByName(&gTable, "zzz"), nullptr);
    EXPECT_EQ(ptFindByName(&gTable, nullptr), nullptr);
    EXPECT_EQ(ptFindByName(nullptr, "a"), nullptr);
}

TEST_F(ProcessTableTest, ptFindByPid_FoundAndInvalidPids)
{
    Service *a = addService("a", "x");
    a->pid = 4321;
    EXPECT_EQ(ptFindByPid(&gTable, 4321), a);
    EXPECT_EQ(ptFindByPid(&gTable, 1), nullptr);
    EXPECT_EQ(ptFindByPid(&gTable, 0), nullptr);       /* 0 = "not running" never matches */
    EXPECT_EQ(ptFindByPid(&gTable, -1), nullptr);
    EXPECT_EQ(ptFindByPid(nullptr, 4321), nullptr);
}

TEST_F(ProcessTableTest, ptFree_TwiceIsSafeAndClosesPipes)
{
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);
    Service *a = addService("a", "x");
    a->hbReadFd = fds[0];
    ptFree(&gTable);
    EXPECT_EQ(gTable.head, nullptr);
    EXPECT_EQ(gTable.count, 0);
    EXPECT_EQ(fcntl(fds[0], F_GETFD), -1);             /* read end closed */
    ptFree(&gTable);
    ptFree(nullptr);
    close(fds[1]);
}

TEST_F(ProcessTableTest, stateToString_AllStates)
{
    EXPECT_STREQ(stateToString(SVC_STARTING), "STARTING");
    EXPECT_STREQ(stateToString(SVC_RUNNING), "RUNNING");
    EXPECT_STREQ(stateToString(SVC_CRASHED), "CRASHED");
    EXPECT_STREQ(stateToString(SVC_RESTARTING), "RESTARTING");
    EXPECT_STREQ(stateToString(SVC_FAILED), "FAILED");
    EXPECT_STREQ(stateToString(SVC_STOPPED), "STOPPED");
    EXPECT_STREQ(stateToString(static_cast<ServiceState>(42)), "UNKNOWN");
}

TEST_F(ProcessTableTest, dependsOnService_TrueFalseNull)
{
    Service *s = addService("fusion", "x");
    ASSERT_EQ(parseLine("depends_on=camera, radar", &s->cfg), SUP_OK);
    EXPECT_EQ(dependsOnService(s, "radar"), 1);
    EXPECT_EQ(dependsOnService(s, "lane"), 0);
    EXPECT_EQ(dependsOnService(nullptr, "radar"), 0);
    EXPECT_EQ(dependsOnService(s, nullptr), 0);
}

/* ========================================================================= */
/* Config Reader module                                                      */
/* ========================================================================= */
TEST_F(ConfigTest, initServiceConfig_Defaults)
{
    ServiceConfig cfg;
    initServiceConfig(&cfg);
    EXPECT_EQ(cfg.maxRestarts, DEFAULT_MAX_RESTARTS);
    EXPECT_EQ(cfg.restartDelay, DEFAULT_RESTART_DELAY);
    EXPECT_EQ(cfg.umaskValue, -1);
    EXPECT_EQ(cfg.heartbeatTimeout, 0);
    EXPECT_EQ(cfg.depCount, 0);
}

TEST_F(ConfigTest, parseServiceLine_AllKeys)
{
    ServiceConfig c;
    initServiceConfig(&c);
    EXPECT_EQ(parseLine("name=camera_service", &c), SUP_OK);
    EXPECT_EQ(parseLine("command = ./cam --fps 30 ", &c), SUP_OK);
    EXPECT_EQ(parseLine("max_restarts=5", &c), SUP_OK);
    EXPECT_EQ(parseLine("restart_delay=4", &c), SUP_OK);
    EXPECT_EQ(parseLine("depends_on=radar,gps", &c), SUP_OK);
    EXPECT_EQ(parseLine("user=adas", &c), SUP_OK);
    EXPECT_EQ(parseLine("group=adas", &c), SUP_OK);
    EXPECT_EQ(parseLine("umask=027", &c), SUP_OK);
    EXPECT_EQ(parseLine("heartbeat_timeout=5", &c), SUP_OK);
    EXPECT_EQ(parseLine("mem_limit_kb=51200", &c), SUP_OK);
    EXPECT_STREQ(c.name, "camera_service");
    EXPECT_STREQ(c.command, "./cam --fps 30");
    EXPECT_EQ(c.maxRestarts, 5);
    EXPECT_EQ(c.restartDelay, 4);
    EXPECT_EQ(c.depCount, 2);
    EXPECT_STREQ(c.dependsOn[1], "gps");
    EXPECT_STREQ(c.user, "adas");
    EXPECT_STREQ(c.group, "adas");
    EXPECT_EQ(c.umaskValue, 027);
    EXPECT_EQ(c.heartbeatTimeout, 5);
    EXPECT_EQ(c.memLimitKb, 51200L);
}

TEST_F(ConfigTest, parseServiceLine_NumericBoundaries)
{
    ServiceConfig c;
    initServiceConfig(&c);
    EXPECT_EQ(parseLine("max_restarts=0", &c), SUP_OK);
    EXPECT_EQ(parseLine("max_restarts=10", &c), SUP_OK);
    EXPECT_EQ(parseLine("max_restarts=11", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("max_restarts=-1", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(c.maxRestarts, 10);                       /* unchanged by the bad lines */
    EXPECT_EQ(parseLine("restart_delay=0", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("restart_delay=1", &c), SUP_OK);
    EXPECT_EQ(parseLine("restart_delay=60", &c), SUP_OK);
    EXPECT_EQ(parseLine("restart_delay=61", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("heartbeat_timeout=0", &c), SUP_OK);
    EXPECT_EQ(parseLine("heartbeat_timeout=60", &c), SUP_OK);
    EXPECT_EQ(parseLine("heartbeat_timeout=61", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("umask=0", &c), SUP_OK);
    EXPECT_EQ(parseLine("umask=777", &c), SUP_OK);
    EXPECT_EQ(parseLine("umask=1000", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("umask=028", &c), ERR_CONFIG_ENTRY);  /* 8 is not octal */
    EXPECT_EQ(parseLine("mem_limit_kb=0", &c), SUP_OK);
    EXPECT_EQ(parseLine("mem_limit_kb=-5", &c), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, parseServiceLine_InvalidLines)
{
    ServiceConfig c;
    initServiceConfig(&c);
    EXPECT_EQ(parseLine("no equals sign", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("colour=blue", &c), ERR_CONFIG_ENTRY);       /* unknown key */
    EXPECT_EQ(parseLine("name=", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("command=", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("name=bad name", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("name=bad/name", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("max_restarts=3x", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("max_restarts=", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine(nullptr, &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("name=x", nullptr), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, parseServiceLine_LengthBoundaries)
{
    ServiceConfig c;
    initServiceConfig(&c);
    std::string name31(MAX_NAME - 1, 'n');
    std::string name32(MAX_NAME, 'n');
    EXPECT_EQ(parseLine(("name=" + name31).c_str(), &c), SUP_OK);
    EXPECT_EQ(parseLine(("name=" + name32).c_str(), &c), ERR_CONFIG_ENTRY);
    std::string cmd255(MAX_CMD - 1, 'c');
    std::string cmd256(MAX_CMD, 'c');
    EXPECT_EQ(parseLine(("command=" + cmd255).c_str(), &c), SUP_OK);
    EXPECT_EQ(parseLine(("command=" + cmd256).c_str(), &c), ERR_CONFIG_ENTRY);
    std::string tooLongLine(LINE_SIZE + 10, 'x');
    EXPECT_EQ(parseLine(("command=" + tooLongLine).c_str(), &c), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, parseServiceLine_Dependencies)
{
    ServiceConfig c;
    initServiceConfig(&c);
    EXPECT_EQ(parseLine("depends_on=", &c), SUP_OK);
    EXPECT_EQ(c.depCount, 0);
    EXPECT_EQ(parseLine("depends_on= a , b ,,c ", &c), SUP_OK);
    EXPECT_EQ(c.depCount, 3);
    EXPECT_STREQ(c.dependsOn[0], "a");
    EXPECT_STREQ(c.dependsOn[2], "c");
    EXPECT_EQ(parseLine("depends_on=a,b,c,d,e,f,g,h", &c), SUP_OK);       /* 8 = MAX_DEPS */
    EXPECT_EQ(parseLine("depends_on=a,b,c,d,e,f,g,h,i", &c), ERR_CONFIG_ENTRY);
    EXPECT_EQ(parseLine("depends_on=good,bad name", &c), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, loadConfig_ValidFile_LoadsServicesWithDefaults)
{
    writeFile("supervisor.conf",
              "# comment\n\n"
              "[service]\n"
              "name=camera\n"
              "command=/bin/true\n"
              "   \n"
              "[service]\n"
              "name=radar\n"
              "command=/bin/true\n"
              "max_restarts=1\n");
    ASSERT_EQ(loadConfig(gCtx.configPath, &gTable), SUP_OK);
    ASSERT_EQ(gTable.count, 2);
    EXPECT_EQ(gTable.head->cfg.maxRestarts, DEFAULT_MAX_RESTARTS);
    EXPECT_EQ(gTable.head->next->cfg.maxRestarts, 1);
}

TEST_F(ConfigTest, loadConfig_WindowsLineEndings_Accepted)
{
    writeFile("supervisor.conf", "[service]\r\nname=a\r\ncommand=/bin/true\r\n");
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), SUP_OK);
    EXPECT_STREQ(gTable.head->cfg.command, "/bin/true");
}

TEST_F(ConfigTest, loadConfig_MissingFile_E001)
{
    EXPECT_EQ(loadConfig(testPath("nope.conf"), &gTable), ERR_CONFIG_FILE);
    EXPECT_CONTAINS(logText(), "E001 Configuration file not found");
    EXPECT_EQ(loadConfig(nullptr, &gTable), ERR_CONFIG_FILE);
}

TEST_F(ConfigTest, loadConfig_KeyOutsideBlock_E002WithLineNumber)
{
    writeFile("supervisor.conf", "# header\nname=x\n");
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "E002");
    EXPECT_CONTAINS(logText(), "line 2");
}

TEST_F(ConfigTest, loadConfig_BadValue_E002WithLineNumber)
{
    writeFile("supervisor.conf", "[service]\nname=x\ncommand=/bin/true\nrestart_delay=99\n");
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "line 4");
}

TEST_F(ConfigTest, loadConfig_NoServices_E002)
{
    writeFile("supervisor.conf", "# only comments\n\n");
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "has no [service] entries");
}

TEST_F(ConfigTest, loadConfig_LineLongerThanBuffer_E002)
{
    std::string text = "[service]\nname=x\ncommand=" + std::string(LINE_SIZE + 20, 'c') + "\n";
    writeFile("supervisor.conf", text.c_str());
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, loadConfig_MaxServicesBoundary)
{
    std::string text;
    for (int i = 0; i < MAX_SERVICES; i++) {
        text += "[service]\nname=s" + std::to_string(i) + "\ncommand=/bin/true\n";
    }
    writeFile("supervisor.conf", text.c_str());
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), SUP_OK);
    EXPECT_EQ(gTable.count, MAX_SERVICES);
    ptFree(&gTable);
    text += "[service]\nname=extra\ncommand=/bin/true\n";
    writeFile("supervisor.conf", text.c_str());
    EXPECT_EQ(loadConfig(gCtx.configPath, &gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "more than 32 services");
}

TEST_F(ConfigTest, validateConfig_ValidTable_Ok)
{
    addService("a", "/bin/true");
    Service *b = addService("b", "/bin/true");
    ASSERT_EQ(parseLine("depends_on=a", &b->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), SUP_OK);
}

TEST_F(ConfigTest, validateConfig_MissingNameOrCommand_E002)
{
    addService("a", "");
    EXPECT_EQ(validateConfig(&gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "name and command are mandatory");
    EXPECT_EQ(validateConfig(nullptr), ERR_CONFIG_ENTRY);
}

TEST_F(ConfigTest, validateConfig_Duplicate_E003)
{
    addService("a", "/bin/true");
    addService("a", "/bin/false");
    EXPECT_EQ(validateConfig(&gTable), ERR_DUPLICATE);
    EXPECT_CONTAINS(logText(), "E003");
}

TEST_F(ConfigTest, validateConfig_UnknownAndSelfDependency_E004)
{
    Service *a = addService("a", "/bin/true");
    ASSERT_EQ(parseLine("depends_on=ghost", &a->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), ERR_DEPENDENCY);
    EXPECT_CONTAINS(logText(), "E004 Unknown or circular dependency | ghost");
    ASSERT_EQ(parseLine("depends_on=a", &a->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), ERR_DEPENDENCY);
}

TEST_F(ConfigTest, validateConfig_ReportsAllErrorsKeepsFirst)
{
    addService("a", "");
    addService("b", "/bin/true");
    addService("b", "/bin/true");
    EXPECT_EQ(validateConfig(&gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "E003");                  /* later error is still logged */
}

TEST_F(ConfigTest, validateConfig_UnknownUserOrGroup_E002)
{
    Service *a = addService("a", "/bin/true");
    ASSERT_EQ(parseLine("user=no_such_user_xyz", &a->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "unknown user");
    a->cfg.user[0] = '\0';
    ASSERT_EQ(parseLine("group=no_such_group_xyz", &a->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), ERR_CONFIG_ENTRY);
    EXPECT_CONTAINS(logText(), "unknown group");
}

TEST_F(ConfigTest, validateConfig_KnownUser_KeptAsRootClearedOtherwise)
{
    Service *a = addService("a", "/bin/true");
    ASSERT_EQ(parseLine("user=nobody", &a->cfg), SUP_OK);
    EXPECT_EQ(validateConfig(&gTable), SUP_OK);
    if (geteuid() == 0) {
        EXPECT_STREQ(a->cfg.user, "nobody");
    } else {
        EXPECT_STREQ(a->cfg.user, "");
        EXPECT_CONTAINS(logText(), "W001");
    }
}

TEST_F(ConfigTest, resolveStartOrder_DependenciesStartFirst)
{
    Service *lane = addService("lane", "x");
    Service *fusion = addService("fusion", "x");
    addService("camera", "x");
    addService("radar", "x");
    ASSERT_EQ(parseLine("depends_on=fusion", &lane->cfg), SUP_OK);
    ASSERT_EQ(parseLine("depends_on=camera,radar", &fusion->cfg), SUP_OK);
    ASSERT_EQ(resolveStartOrder(&gTable, gStartOrder, &gStartCount), SUP_OK);
    ASSERT_EQ(gStartCount, 4);
    EXPECT_STREQ(gStartOrder[0]->cfg.name, "camera");
    EXPECT_STREQ(gStartOrder[1]->cfg.name, "radar");
    EXPECT_STREQ(gStartOrder[2]->cfg.name, "fusion");
    EXPECT_STREQ(gStartOrder[3]->cfg.name, "lane");
}

TEST_F(ConfigTest, resolveStartOrder_NoDependencies_KeepsFileOrder)
{
    addService("x", "x");
    addService("y", "x");
    ASSERT_EQ(resolveStartOrder(&gTable, gStartOrder, &gStartCount), SUP_OK);
    EXPECT_STREQ(gStartOrder[0]->cfg.name, "x");
    EXPECT_STREQ(gStartOrder[1]->cfg.name, "y");
}

TEST_F(ConfigTest, resolveStartOrder_Circular_E004)
{
    Service *a = addService("a", "x");
    Service *b = addService("b", "x");
    addService("c", "x");
    ASSERT_EQ(parseLine("depends_on=b", &a->cfg), SUP_OK);
    ASSERT_EQ(parseLine("depends_on=a", &b->cfg), SUP_OK);
    EXPECT_EQ(resolveStartOrder(&gTable, gStartOrder, &gStartCount), ERR_DEPENDENCY);
    EXPECT_EQ(gStartCount, 1);                           /* only c could be placed */
    EXPECT_EQ(countOccurrences(logText(), "| circular dependency"), 2);
}

TEST_F(ConfigTest, resolveStartOrder_NullArguments)
{
    int count = 0;
    EXPECT_EQ(resolveStartOrder(nullptr, gStartOrder, &count), ERR_DEPENDENCY);
    EXPECT_EQ(resolveStartOrder(&gTable, nullptr, &count), ERR_DEPENDENCY);
    EXPECT_EQ(resolveStartOrder(&gTable, gStartOrder, nullptr), ERR_DEPENDENCY);
}

/* ========================================================================= */
/* Launcher module                                                           */
/* ========================================================================= */
TEST_F(LauncherTest, buildArgv_SplitsOnBlanksAndTabs)
{
    char cmd[] = "  ./cam\t--fps   30 ";
    char *argv[MAX_ARGS + 1];
    ASSERT_EQ(buildArgv(cmd, argv, MAX_ARGS), 3);
    EXPECT_STREQ(argv[0], "./cam");
    EXPECT_STREQ(argv[1], "--fps");
    EXPECT_STREQ(argv[2], "30");
    EXPECT_EQ(argv[3], nullptr);
}

TEST_F(LauncherTest, buildArgv_EmptyOrInvalid_ReturnsMinusOne)
{
    char empty[] = "   ";
    char *argv[4];
    EXPECT_EQ(buildArgv(empty, argv, 3), -1);
    EXPECT_EQ(buildArgv(nullptr, argv, 3), -1);
    char cmd[] = "a";
    EXPECT_EQ(buildArgv(cmd, nullptr, 3), -1);
    EXPECT_EQ(buildArgv(cmd, argv, 0), -1);
}

TEST_F(LauncherTest, buildArgv_ArgumentCountBoundary)
{
    char exact[] = "a b c";
    char over[] = "a b c d";
    char *argv[4];
    EXPECT_EQ(buildArgv(exact, argv, 3), 3);
    EXPECT_EQ(buildArgv(over, argv, 3), -1);
}

TEST_F(LauncherTest, launchService_ValidCommand_StartingWithPid)
{
    Service *s = addService("sleeper", "/bin/sleep 30");
    ASSERT_EQ(launchService(s), SUP_OK);
    EXPECT_GT(s->pid, 0);
    EXPECT_EQ(s->state, SVC_STARTING);
    EXPECT_EQ(s->hbReadFd, -1);
    EXPECT_EQ(kill(s->pid, 0), 0);                      /* the process exists */
    EXPECT_CONTAINS(logText(), "I002 service started: /bin/sleep 30");
}

TEST_F(LauncherTest, launchService_OwnProcessGroup)
{
    Service *s = addService("sleeper", "/bin/sleep 30");
    ASSERT_EQ(launchService(s), SUP_OK);
    sleepMs(200);
    EXPECT_EQ(getpgid(s->pid), s->pid);                 /* Ctrl+C does not reach it */
}

TEST_F(LauncherTest, launchService_Heartbeat_NonBlockingCloseOnExecPipe)
{
    Service *s = addService("hb", "/bin/sleep 30");
    s->cfg.heartbeatTimeout = 5;
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_GE(s->hbReadFd, 0);
    EXPECT_NE(fcntl(s->hbReadFd, F_GETFL) & O_NONBLOCK, 0);
    EXPECT_NE(fcntl(s->hbReadFd, F_GETFD) & FD_CLOEXEC, 0);
}

TEST_F(LauncherTest, launchService_CommandNotFound_E007HandledAsCrash)
{
    Service *s = addService("ghost", "/no/such/program");
    s->cfg.maxRestarts = 1;
    ASSERT_EQ(launchService(s), SUP_OK);                /* fork worked; exec fails in the child */
    ASSERT_EQ(pumpUntil(s, SVC_RESTARTING, 3000), 1);
    EXPECT_CONTAINS(logText(), "E007 exit code 127");
}

TEST_F(LauncherTest, launchService_Null_ReturnsErrFork)
{
    EXPECT_EQ(launchService(nullptr), ERR_FORK);
}

TEST_F(LauncherTest, launchAll_SkipsFailedAndRunningServices)
{
    Service *a = addService("a", "/bin/sleep 30");
    Service *b = addService("b", "/bin/sleep 30");
    Service *c = addService("c", "/bin/sleep 30");
    b->state = SVC_FAILED;
    c->pid = 99999;                                      /* already running */
    Service *order[] = { a, b, c };
    EXPECT_EQ(launchAll(order, 3), SUP_OK);
    EXPECT_GT(a->pid, 0);
    EXPECT_EQ(b->pid, 0);
    EXPECT_EQ(c->pid, 99999);
    c->pid = 0;
    EXPECT_EQ(launchAll(order, 0), SUP_OK);
}

TEST_F(LauncherTest, applyPermissions_UserGroupUmask_InChild)
{
    if (geteuid() != 0) {
        GTEST_SKIP() << "needs root";
    }
    const struct passwd *pw = getpwnam("nobody");
    ASSERT_NE(pw, nullptr);
    ServiceConfig cfg = makeConfig("p", "x");
    std::snprintf(cfg.user, sizeof(cfg.user), "%s", "nobody");
    cfg.umaskValue = 027;
    pid_t pid = fork();
    if (pid == 0) {
        int ok = (applyPermissions(&cfg) == SUP_OK) && (getuid() == pw->pw_uid) &&
                 (getgid() == pw->pw_gid) && (umask(0) == 027);
        std::exit(ok ? 0 : 1);            /* exit(): lets gcov save the child counters */
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}

TEST_F(LauncherTest, applyPermissions_GroupOnly_InChild)
{
    if (geteuid() != 0) {
        GTEST_SKIP() << "needs root";
    }
    const struct group *gr = getgrnam("nogroup");
    if (gr == nullptr) {
        GTEST_SKIP() << "no 'nogroup' group";
    }
    ServiceConfig cfg = makeConfig("p", "x");
    std::snprintf(cfg.group, sizeof(cfg.group), "%s", "nogroup");
    gid_t expected = gr->gr_gid;
    pid_t pid = fork();
    if (pid == 0) {
        std::exit(((applyPermissions(&cfg) == SUP_OK) && (getgid() == expected)) ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
}

TEST_F(LauncherTest, applyPermissions_UnknownUserOrGroup_Fails)
{
    ServiceConfig cfg = makeConfig("p", "x");
    std::snprintf(cfg.user, sizeof(cfg.user), "%s", "no_such_user_xyz");
    EXPECT_EQ(applyPermissions(&cfg), ERR_PERMISSION);
    cfg = makeConfig("p", "x");
    std::snprintf(cfg.group, sizeof(cfg.group), "%s", "no_such_group_xyz");
    EXPECT_EQ(applyPermissions(&cfg), ERR_PERMISSION);
}

TEST_F(LauncherTest, launchService_PermissionFailure_E008HandledAsCrash)
{
    Service *s = addService("perm", "/bin/sleep 30");
    std::snprintf(s->cfg.user, sizeof(s->cfg.user), "%s", "no_such_user_xyz");
    s->cfg.maxRestarts = 0;
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_EQ(pumpUntil(s, SVC_FAILED, 3000), 1);
    EXPECT_CONTAINS(logText(), "E008 exit code 126");
    EXPECT_CONTAINS(logText(), "E011");
}

/* ========================================================================= */
/* Monitor module                                                            */
/* ========================================================================= */
TEST_F(MonitorTest, reapChildren_NormalExit_Stopped)
{
    Service *s = addService("once", "/bin/true");
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_EQ(pumpUntil(s, SVC_STOPPED, 3000), 1);
    EXPECT_EQ(s->pid, 0);
    EXPECT_EQ(s->restartCount, 0);
    EXPECT_CONTAINS(logText(), "service exited normally");
}

TEST_F(MonitorTest, reapChildren_NonZeroExit_IsCrash)
{
    Service *s = addService("fail", "/bin/false");
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_EQ(pumpUntil(s, SVC_RESTARTING, 3000), 1);
    EXPECT_CONTAINS(logText(), "service crashed (exit code 1)");
}

TEST_F(MonitorTest, reapChildren_Signal_IsCrash)
{
    Service *s = addService("seg", demoCommand("seg", "--crash-after 0"));
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_EQ(pumpUntil(s, SVC_RESTARTING, 3000), 1);
    EXPECT_CONTAINS(logText(), "service crashed (signal 11)");
    EXPECT_EQ(s->restartCount, 1);
}

TEST_F(MonitorTest, reapChildren_UnknownChild_ReapedAndIgnored)
{
    pid_t stranger = fork();
    if (stranger == 0) {
        _exit(0);
    }
    ASSERT_GT(stranger, 0);

    siginfo_t childInfo = {};
    int waitResult;
    do {
        waitResult = waitid(P_PID, static_cast<id_t>(stranger), &childInfo, WEXITED | WNOWAIT);
    } while ((waitResult == -1) && (errno == EINTR));
    ASSERT_EQ(waitResult, 0) << std::strerror(errno);
    ASSERT_EQ(childInfo.si_pid, stranger);

    reapChildren(&gCtx, &gTable);
    EXPECT_EQ(countZombieChildren(), 0);
    errno = 0;
    EXPECT_EQ(waitpid(stranger, nullptr, WNOHANG), -1);  /* already reaped */
    EXPECT_EQ(errno, ECHILD);
}

TEST_F(MonitorTest, reapChildren_DuringShutdown_MarksStopped)
{
    Service *s = addService("x", "/bin/sleep 30");
    ASSERT_EQ(launchService(s), SUP_OK);
    sleepMs(100);
    gCtx.stopping = 1;
    kill(s->pid, SIGTERM);
    ASSERT_EQ(pumpUntil(s, SVC_STOPPED, 3000), 1);
    EXPECT_EQ(s->restartCount, 0);
}

TEST_F(MonitorTest, reapChildren_ManyExitsAtOnce_AllReaped)
{
    Service *svcs[6];
    for (int i = 0; i < 6; i++) {
        svcs[i] = addService(("t" + std::to_string(i)).c_str(), "/bin/true");
        ASSERT_EQ(launchService(svcs[i]), SUP_OK);
    }
    sleepMs(500);
    reapChildren(&gCtx, &gTable);                       /* one call, six children */
    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(svcs[i]->state, SVC_STOPPED) << i;
    }
    EXPECT_EQ(countZombieChildren(), 0);
}

TEST_F(MonitorTest, checkHeartbeats_StartingBecomesRunningAfterGrace)
{
    Service *s = addService("x", "/bin/sleep 30");
    ASSERT_EQ(launchService(s), SUP_OK);
    checkHeartbeats(&gTable);
    EXPECT_EQ(s->state, SVC_STARTING);                  /* not yet 1 s */
    s->startTime -= START_GRACE_SEC;
    checkHeartbeats(&gTable);
    EXPECT_EQ(s->state, SVC_RUNNING);
    EXPECT_CONTAINS(logText(), "service running");
}

TEST_F(MonitorTest, checkHeartbeats_HeartbeatsArrive_StaysRunning)
{
    Service *s = addService("hb", demoCommand("hb", ""));
    s->cfg.heartbeatTimeout = 2;
    ASSERT_EQ(launchService(s), SUP_OK);
    pumpFor(3500);
    EXPECT_EQ(s->state, SVC_RUNNING);
    EXPECT_GE(s->lastHeartbeat, time(nullptr) - 2);
    EXPECT_NOT_CONTAINS(logText(), "E010");
}

TEST_F(MonitorTest, checkHeartbeats_NoHeartbeat_E010KilledAndRestarted)
{
    Service *s = addService("hang", "/bin/sleep 30");   /* never writes heartbeats */
    s->cfg.heartbeatTimeout = 1;
    ASSERT_EQ(launchService(s), SUP_OK);
    pid_t first = s->pid;
    ASSERT_EQ(pumpUntil(s, SVC_RESTARTING, 4000), 1);
    EXPECT_CONTAINS(logText(), "E010 Heartbeat timeout");
    EXPECT_CONTAINS(logText(), "service crashed (signal 9)");
    EXPECT_EQ(kill(first, 0), -1);                      /* the hung process is gone */
}

TEST_F(MonitorTest, checkHeartbeats_HangAfterSomeBeats_Detected)
{
    Service *s = addService("hb", demoCommand("hb", "--hang-after 1"));
    s->cfg.heartbeatTimeout = 1;
    ASSERT_EQ(launchService(s), SUP_OK);
    ASSERT_EQ(pumpUntil(s, SVC_RESTARTING, 6000), 1);
    EXPECT_CONTAINS(logText(), "E010");
}

TEST_F(MonitorTest, readProcStats_OwnProcess_Ok)
{
    unsigned long ticks = 0;
    long mem = 0;
    EXPECT_EQ(readProcStats(getpid(), &ticks, &mem), SUP_OK);
    EXPECT_GT(mem, 0L);
}

TEST_F(MonitorTest, readProcStats_NoSuchProcess_ErrFile)
{
    unsigned long ticks = 7;
    long mem = 7;
    EXPECT_EQ(readProcStats((pid_t)INT_MAX, &ticks, &mem), ERR_FILE);
    EXPECT_EQ(ticks, 7UL);                              /* outputs untouched */
}

TEST_F(MonitorTest, updateResourceUsage_SamplesMemoryAndCpu)
{
    Service *s = addService("x", demoCommand("x", "--mem-kb 4096"));
    ASSERT_EQ(launchService(s), SUP_OK);
    pumpFor(2200);
    EXPECT_GT(s->memKb, 4000L);
    EXPECT_NE(s->lastSampleTime, 0);
    EXPECT_GE(s->cpuPercent, 0.0);
}

TEST_F(MonitorTest, updateResourceUsage_MemoryLimit_W002LoggedOnce)
{
    Service *s = addService("big", demoCommand("big", "--mem-kb 8192"));
    s->cfg.memLimitKb = 1024;
    ASSERT_EQ(launchService(s), SUP_OK);
    pumpFor(2500);
    EXPECT_EQ(countOccurrences(logText(), "W002 memory limit exceeded"), 1);
    EXPECT_EQ(s->memWarned, 1);
}

/* ========================================================================= */
/* Restart Manager module                                                    */
/* ========================================================================= */
TEST_F(RestartTest, handleCrash_RestartsLeft_SchedulesAfterDelay)
{
    Service *s = addService("x", "x");
    s->cfg.restartDelay = 5;
    s->pid = 1234;
    time_t before = time(nullptr);
    handleCrash(s, 11);                                 /* raw status: killed by SIGSEGV */
    EXPECT_EQ(s->state, SVC_RESTARTING);
    EXPECT_EQ(s->restartCount, 1);
    EXPECT_EQ(s->pid, 0);
    EXPECT_GE(s->nextRestartTime, before + 5);
    EXPECT_CONTAINS(logText(), "[x:1234] service crashed (signal 11)");
    EXPECT_CONTAINS(logText(), "restart 1/3 scheduled in 5 s");
}

TEST_F(RestartTest, handleCrash_LastRestartUsed_FailedE011)
{
    Service *s = addService("x", "x");
    s->cfg.maxRestarts = 2;
    s->restartCount = 2;
    handleCrash(s, 11);
    EXPECT_EQ(s->state, SVC_FAILED);
    EXPECT_CONTAINS(logText(), "E011 service FAILED (2/2 restarts used)");
}

TEST_F(RestartTest, handleCrash_ZeroRestarts_FailsAtOnce)
{
    Service *s = addService("x", "x");
    s->cfg.maxRestarts = 0;
    handleCrash(s, 11);
    EXPECT_EQ(s->state, SVC_FAILED);
    EXPECT_EQ(s->restartCount, 0);
}

TEST_F(RestartTest, handleCrash_Null_NoCrash)
{
    handleCrash(nullptr, 0);
    EXPECT_STREQ(logText(), "");
}

TEST_F(RestartTest, describeExit_AllCases)
{
    char buf[96];
    describeExit(0, buf, sizeof(buf));
    EXPECT_STREQ(buf, "exit code 0");
    describeExit(3 << 8, buf, sizeof(buf));
    EXPECT_STREQ(buf, "exit code 3");
    describeExit(EXIT_EXEC_FAIL << 8, buf, sizeof(buf));
    EXPECT_CONTAINS(buf, "E007");
    describeExit(EXIT_PERM_FAIL << 8, buf, sizeof(buf));
    EXPECT_CONTAINS(buf, "E008");
    describeExit(9, buf, sizeof(buf));
    EXPECT_STREQ(buf, "signal 9");
    describeExit(-1, buf, sizeof(buf));
    EXPECT_CONTAINS(buf, "fork()");
    describeExit(0x137f, buf, sizeof(buf));             /* stopped, not exited */
    EXPECT_CONTAINS(buf, "status");
    char small[4] = "xyz";
    describeExit(0, small, 0);                          /* size 0: untouched */
    EXPECT_STREQ(small, "xyz");
}

TEST_F(RestartTest, processPendingRestarts_DelayNotElapsed_Waits)
{
    Service *s = addService("x", "/bin/sleep 30");
    s->state = SVC_RESTARTING;
    s->nextRestartTime = time(nullptr) + 60;
    processPendingRestarts(&gTable);
    EXPECT_EQ(s->pid, 0);
    EXPECT_EQ(s->state, SVC_RESTARTING);
}

TEST_F(RestartTest, processPendingRestarts_DelayElapsed_RelaunchesI003)
{
    Service *s = addService("x", "/bin/sleep 30");
    s->state = SVC_RESTARTING;
    s->restartCount = 1;
    s->nextRestartTime = time(nullptr);
    processPendingRestarts(&gTable);
    EXPECT_GT(s->pid, 0);
    EXPECT_EQ(s->state, SVC_STARTING);
    EXPECT_CONTAINS(logText(), "I003 service restarted (restarts used 1/3)");
}

TEST_F(RestartTest, processPendingRestarts_FailedLaunch_AppliesPolicy)
{
    Service *s = addService("x", "/bin/sleep 30");
    s->state = SVC_CRASHED;                             /* what launchService() leaves on fork failure */
    s->lastStatus = -1;
    processPendingRestarts(&gTable);
    EXPECT_EQ(s->restartCount, 1);
    EXPECT_CONTAINS(logText(), "launch failed: fork()");
}

TEST_F(RestartTest, processPendingRestarts_FailedServiceIsNotTouched)
{
    Service *s = addService("x", "/bin/sleep 30");
    s->state = SVC_FAILED;
    processPendingRestarts(&gTable);
    EXPECT_EQ(s->pid, 0);
    EXPECT_EQ(s->state, SVC_FAILED);
}

TEST_F(RestartTest, restartDependents_DependentRestartsWithoutUsingItsLimit)
{
    Service *camera = addService("camera", "/bin/sleep 30");
    Service *fusion = addService("fusion", "/bin/sleep 30");
    Service *other = addService("other", "/bin/sleep 30");
    ASSERT_EQ(parseLine("depends_on=camera", &fusion->cfg), SUP_OK);
    ASSERT_EQ(launchService(camera), SUP_OK);
    ASSERT_EQ(launchService(fusion), SUP_OK);
    ASSERT_EQ(launchService(other), SUP_OK);
    pid_t oldFusion = fusion->pid;
    pid_t oldOther = other->pid;
    restartDependents(&gTable, camera);
    EXPECT_EQ(fusion->state, SVC_RESTARTING);
    ASSERT_EQ(pumpUntil(fusion, SVC_STARTING, 3000), 1);
    EXPECT_NE(fusion->pid, oldFusion);
    EXPECT_EQ(fusion->restartCount, 0);
    EXPECT_EQ(other->pid, oldOther);                    /* not a dependent */
    EXPECT_CONTAINS(logText(), "stopping dependent of camera for restart");
    EXPECT_CONTAINS(logText(), "service stopped for dependency restart");
}

TEST_F(RestartTest, markFailed_ClearsPid)
{
    Service *s = addService("x", "x");
    s->pid = 77;
    markFailed(s);
    EXPECT_EQ(s->state, SVC_FAILED);
    EXPECT_EQ(s->pid, 0);
}

/* ========================================================================= */
/* Status Dashboard module                                                   */
/* ========================================================================= */
TEST_F(DashboardTest, formatDuration_Values)
{
    char buf[24];
    formatDuration(0L, buf, sizeof(buf));
    EXPECT_STREQ(buf, "00:00:00");
    formatDuration(3661L, buf, sizeof(buf));
    EXPECT_STREQ(buf, "01:01:01");
    formatDuration(-5L, buf, sizeof(buf));
    EXPECT_STREQ(buf, "00:00:00");
    formatDuration(100L * 3600L, buf, sizeof(buf));
    EXPECT_STREQ(buf, "100:00:00");
}

TEST_F(DashboardTest, showDashboard_Foreground_PrintsAllServicesAndTotals)
{
    Service *a = addService("camera_service", "/bin/sleep 30");
    Service *b = addService("lane_assist", "x");
    ASSERT_EQ(launchService(a), SUP_OK);
    b->state = SVC_FAILED;
    b->restartCount = 3;
    captureStdoutBegin();
    EXPECT_EQ(showDashboard(&gCtx, &gTable), SUP_OK);
    const char *out = captureStdoutEnd();
    EXPECT_CONTAINS(out, "STATUS DASHBOARD");
    EXPECT_CONTAINS(out, "camera_service");
    EXPECT_CONTAINS(out, "STARTING");
    EXPECT_CONTAINS(out, "lane_assist");
    EXPECT_CONTAINS(out, "FAILED");
    EXPECT_CONTAINS(out, "3/3");
    EXPECT_CONTAINS(out, "Total : 2   Running : 1   Restarting : 0   Failed : 1");
}

TEST_F(DashboardTest, showDashboard_Daemon_WritesStatusFile0644)
{
    addService("x", "x")->state = SVC_RESTARTING;
    gCtx.daemonMode = 1;
    mode_t old = umask(022);
    EXPECT_EQ(showDashboard(&gCtx, &gTable), SUP_OK);
    umask(old);
    EXPECT_CONTAINS(readFile("supervisor.status"), "Restarting : 1");
    EXPECT_EQ(fileMode("supervisor.status"), 0644);
    EXPECT_CONTAINS(logText(), "status dashboard written");
}

TEST_F(DashboardTest, showDashboard_BadStatusPath_E009)
{
    gCtx.daemonMode = 1;
    std::snprintf(gCtx.statusPath, sizeof(gCtx.statusPath), "%s", testPath("missing/dir/status"));
    EXPECT_EQ(showDashboard(&gCtx, &gTable), ERR_FILE);
    EXPECT_CONTAINS(logText(), "E009");
    EXPECT_EQ(showDashboard(nullptr, &gTable), ERR_FILE);
    EXPECT_EQ(showDashboard(&gCtx, nullptr), ERR_FILE);
}

TEST_F(DashboardTest, showDashboard_EmptyTable_TotalsZero)
{
    captureStdoutBegin();
    EXPECT_EQ(showDashboard(&gCtx, &gTable), SUP_OK);
    EXPECT_CONTAINS(captureStdoutEnd(), "Total : 0");
}

TEST_F(DashboardTest, printServiceRow_StoppedService_ShowsDashes)
{
    Service *s = addService("idle", "x");
    captureStdoutBegin();
    printServiceRow(stdout, s);
    const char *out = captureStdoutEnd();
    EXPECT_CONTAINS(out, "idle");
    EXPECT_CONTAINS(out, "STOPPED");
    EXPECT_CONTAINS(out, "--");
}

TEST_F(DashboardTest, printStartupSummary_ListsOrderDependenciesHeartbeat)
{
    Service *a = addService("camera", "x");
    Service *b = addService("fusion", "x");
    ASSERT_EQ(parseLine("depends_on=camera,radar", &b->cfg), SUP_OK);
    a->cfg.heartbeatTimeout = 5;
    Service *order[] = { a, b };
    captureStdoutBegin();
    printStartupSummary(stdout, &gCtx, order, 2);
    const char *out = captureStdoutEnd();
    EXPECT_CONTAINS(out, "Services Loaded : 2");
    EXPECT_CONTAINS(out, "Foreground");
    EXPECT_CONTAINS(out, "camera,radar");
    EXPECT_CONTAINS(out, "yes");
    EXPECT_CONTAINS(out, "Validation      : OK");
}

/* ========================================================================= */
/* Supervisor Core module                                                    */
/* ========================================================================= */
TEST_F(SupervisorCoreTest, parseArguments_Defaults)
{
    char prog[] = "supervisor";
    char *argv[] = { prog, nullptr };
    SupervisorContext ctx;
    EXPECT_EQ(parseArguments(1, argv, &ctx), SUP_OK);
    EXPECT_STREQ(ctx.configPath, DEFAULT_CONFIG_FILE);
    EXPECT_STREQ(ctx.logPath, DEFAULT_LOG_FILE);
    EXPECT_STREQ(ctx.pidPath, DEFAULT_PID_FILE);
    EXPECT_STREQ(ctx.statusPath, DEFAULT_STATUS_FILE);
    EXPECT_EQ(ctx.daemonMode, 0);
}

TEST_F(SupervisorCoreTest, parseArguments_ConfigAndDaemon)
{
    char prog[] = "supervisor", c[] = "-c", file[] = "/etc/adas.conf", dflag[] = "-d";
    char *argv[] = { prog, c, file, dflag, nullptr };
    SupervisorContext ctx;
    EXPECT_EQ(parseArguments(4, argv, &ctx), SUP_OK);
    EXPECT_STREQ(ctx.configPath, "/etc/adas.conf");
    EXPECT_EQ(ctx.daemonMode, 1);
}

TEST_F(SupervisorCoreTest, parseArguments_Help)
{
    char prog[] = "supervisor", h[] = "-h";
    char *argv[] = { prog, h, nullptr };
    SupervisorContext ctx;
    EXPECT_EQ(parseArguments(2, argv, &ctx), SUP_HELP);
}

TEST_F(SupervisorCoreTest, parseArguments_InvalidInputs_ErrUsage)
{
    SupervisorContext ctx;
    char prog[] = "supervisor", x[] = "-x", c[] = "-c", extra[] = "extra";
    char *unknown[] = { prog, x, nullptr };
    EXPECT_EQ(parseArguments(2, unknown, &ctx), ERR_USAGE);
    char *missing[] = { prog, c, nullptr };
    EXPECT_EQ(parseArguments(2, missing, &ctx), ERR_USAGE);
    char *positional[] = { prog, extra, nullptr };
    EXPECT_EQ(parseArguments(2, positional, &ctx), ERR_USAGE);
    std::string longPath(MAX_PATH, 'p');
    char *tooLong[] = { prog, c, const_cast<char *>(longPath.c_str()), nullptr };
    EXPECT_EQ(parseArguments(3, tooLong, &ctx), ERR_USAGE);
}

TEST_F(SupervisorCoreTest, handleSignal_SetsMatchingFlag)
{
    handleSignal(SIGCHLD);
    EXPECT_EQ(gChildExited, 1);
    handleSignal(SIGUSR1);
    EXPECT_EQ(gStatusRequested, 1);
    EXPECT_EQ(gStopRequested, 0);
    handleSignal(SIGINT);
    EXPECT_EQ(gStopRequested, 1);
    gStopRequested = 0;
    handleSignal(SIGTERM);
    EXPECT_EQ(gStopRequested, 1);
    gStopRequested = 0;
    handleSignal(SIGHUP);                               /* not handled */
    EXPECT_EQ(gStopRequested, 0);
}

TEST_F(SupervisorCoreTest, installSignalHandlers_RealSignalsSetFlags)
{
    ASSERT_EQ(installSignalHandlers(), SUP_OK);
    raise(SIGUSR1);
    EXPECT_EQ(gStatusRequested, 1);
    raise(SIGTERM);
    EXPECT_EQ(gStopRequested, 1);
}

TEST_F(SupervisorCoreTest, writePidFile_WritesPidOrFails)
{
    EXPECT_EQ(writePidFile(gCtx.pidPath), SUP_OK);
    EXPECT_EQ(std::atoi(readFile("supervisor.pid")), (int)getpid());
    EXPECT_EQ(writePidFile(testPath("missing/dir/x.pid")), ERR_FILE);
    EXPECT_CONTAINS(logText(), "E009");
}

TEST_F(SupervisorCoreTest, printUsage_ListsOptions)
{
    captureStdoutBegin();
    printUsage();
    const char *out = captureStdoutEnd();
    EXPECT_CONTAINS(out, "-c <file>");
    EXPECT_CONTAINS(out, "-d");
    EXPECT_CONTAINS(out, "kill -USR1");
}

TEST_F(SupervisorCoreTest, daemonize_DetachesIntoNewSession)
{
    pid_t child = fork();
    if (child == 0) {
        /* intermediate parent exits inside daemonize(); the daemon reports back */
        if (daemonize() == SUP_OK) {
            FILE *fp = std::fopen(testPath("daemon.txt"), "w");
            if (fp != nullptr) {
                std::fprintf(fp, "%d %d\n", (int)getsid(0), (int)getpid());
                std::fclose(fp);
            }
        }
        std::exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status) && (WEXITSTATUS(status) == 0));
    for (int i = 0; (i < 40) && (std::strlen(readFile("daemon.txt")) == 0U); i++) {
        sleepMs(50);
    }
    int sid = 0, pid = 0;
    ASSERT_EQ(std::sscanf(readFile("daemon.txt"), "%d %d", &sid, &pid), 2);
    EXPECT_EQ(sid, pid);                                /* session leader */
    EXPECT_NE(sid, (int)getsid(0));
}

TEST_F(SupervisorCoreTest, runEventLoop_StopsOnSigterm)
{
    ASSERT_EQ(installSignalHandlers(), SUP_OK);
    Service *s = addService("x", "/bin/sleep 30");
    ASSERT_EQ(launchService(s), SUP_OK);
    sendSignalLater(SIGTERM, 1500);
    runEventLoop(&gCtx, &gTable);
    EXPECT_EQ(gStopRequested, 1);
    EXPECT_EQ(s->state, SVC_RUNNING);
}

TEST_F(SupervisorCoreTest, runEventLoop_Sigusr1_WritesDashboard)
{
    ASSERT_EQ(installSignalHandlers(), SUP_OK);
    gCtx.daemonMode = 1;
    addService("x", "x");
    sendSignalLater(SIGUSR1, 300);
    sendSignalLater(SIGTERM, 1600);
    runEventLoop(&gCtx, &gTable);
    EXPECT_CONTAINS(readFile("supervisor.status"), "STATUS DASHBOARD");
    EXPECT_EQ(gStatusRequested, 0);
}

TEST_F(SupervisorCoreTest, runEventLoop_CrashDetectedFastBySigchld)
{
    ASSERT_EQ(installSignalHandlers(), SUP_OK);
    Service *s = addService("seg", demoCommand("seg", "--crash-after 0"));
    s->cfg.maxRestarts = 0;
    ASSERT_EQ(launchService(s), SUP_OK);
    sendSignalLater(SIGTERM, 1500);
    runEventLoop(&gCtx, &gTable);
    EXPECT_EQ(s->state, SVC_FAILED);
}

TEST_F(SupervisorCoreTest, shutdownSupervisor_ReverseOrderPidFileRemoved)
{
    Service *a = addService("first", "/bin/sleep 30");
    Service *b = addService("second", "/bin/sleep 30");
    ASSERT_EQ(resolveStartOrder(&gTable, gStartOrder, &gStartCount), SUP_OK);
    ASSERT_EQ(launchAll(gStartOrder, gStartCount), SUP_OK);
    ASSERT_EQ(writePidFile(gCtx.pidPath), SUP_OK);
    (void)a;
    (void)b;
    shutdownSupervisor(&gCtx, &gTable);
    ASSERT_EQ(logInit(gCtx.logPath, 0), SUP_OK);      /* shutdown closed it; reopen to read */
    const char *log = logText();
    const char *second = std::strstr(log, "[second:");
    const char *first = std::strstr(log, "[first:");
    ASSERT_NE(second, nullptr);
    ASSERT_NE(first, nullptr);
    EXPECT_LT(std::strstr(std::strstr(log, "stop requested"), "[second:"), std::strstr(std::strstr(log, "stop requested"), "[first:"));
    EXPECT_CONTAINS(log, "stop requested - stopping 2 service(s)");
    EXPECT_CONTAINS(log, "I004 supervisor stopped");
    EXPECT_EQ(fileExists("supervisor.pid"), 0);
    EXPECT_EQ(gTable.head, nullptr);
    EXPECT_EQ(countZombieChildren(), 0);
}

TEST_F(SupervisorCoreTest, shutdownSupervisor_IgnoresSigterm_SigkillAfterTimeout)
{
    Service *s = addService("stubborn", demoCommand("stubborn", "--ignore-term"));
    ASSERT_EQ(launchService(s), SUP_OK);
    sleepMs(300);                                      /* let it install SIG_IGN */
    pid_t pid = s->pid;
    time_t start = time(nullptr);
    shutdownSupervisor(&gCtx, &gTable);
    EXPECT_GE(time(nullptr) - start, STOP_TIMEOUT_SEC - 1);
    EXPECT_EQ(kill(pid, 0), -1);
    ASSERT_EQ(logInit(gCtx.logPath, 0), SUP_OK);
    EXPECT_CONTAINS(logText(), "sending SIGKILL");
}

TEST_F(SupervisorCoreTest, runSupervisor_HelpAndBadOption)
{
    const char *help[] = { "supervisor", "-h" };
    EXPECT_EQ(runInTestDir(2, help), 0);
    const char *bad[] = { "supervisor", "-z" };
    EXPECT_EQ(runInTestDir(2, bad), 1);
}

TEST_F(SupervisorCoreTest, runSupervisor_MissingConfig_ExitsOneWithE001)
{
    logClose();
    const char *args[] = { "supervisor", "-c", "missing.conf" };
    EXPECT_EQ(runInTestDir(3, args), 1);
    EXPECT_CONTAINS(readFile("supervisor.log"), "E001");
    EXPECT_EQ(fileExists("supervisor.pid"), 0);
}

TEST_F(SupervisorCoreTest, runSupervisor_CircularConfig_ExitsOneWithE004)
{
    logClose();
    writeFile("bad.conf", "[service]\nname=a\ncommand=/bin/true\ndepends_on=b\n"
                          "[service]\nname=b\ncommand=/bin/true\ndepends_on=a\n");
    const char *args[] = { "supervisor", "-c", "bad.conf" };
    EXPECT_EQ(runInTestDir(3, args), 1);
    EXPECT_CONTAINS(readFile("supervisor.log"), "E004");
}

/* ========================================================================= */
/* Integration tests (whole program, real processes and signals)             */
/* ========================================================================= */
TEST_F(IntegrationTest, FullRun_LaunchCrashRestartFailedStop)
{
    logClose();
    std::string conf = "[service]\nname=camera\ncommand=" + std::string(demoCommand("camera", "")) +
                       "\nheartbeat_timeout=3\n"
                       "[service]\nname=lane\ncommand=" + std::string(demoCommand("lane", "--crash-after 0")) +
                       "\ndepends_on=camera\nmax_restarts=1\nrestart_delay=1\n";
    writeFile("it.conf", conf.c_str());
    sendSignalLater(SIGTERM, 4500);
    const char *args[] = { "supervisor", "-c", "it.conf" };
    EXPECT_EQ(runInTestDir(3, args), 0);
    const char *log = readFile("supervisor.log");
    EXPECT_CONTAINS(log, "configuration valid: 2 service(s)");
    EXPECT_CONTAINS(log, "I001 supervisor started (foreground mode)");
    EXPECT_EQ(countOccurrences(log, "I002 service started"), 3);   /* camera, lane, lane again */
    EXPECT_EQ(countOccurrences(log, "service crashed (signal 11)"), 2);
    EXPECT_CONTAINS(log, "I003 service restarted (restarts used 1/1)");
    EXPECT_CONTAINS(log, "E011 service FAILED (1/1 restarts used)");
    EXPECT_CONTAINS(log, "[camera:");
    EXPECT_NOT_CONTAINS(log, "E010");
    EXPECT_CONTAINS(log, "I004 supervisor stopped");
    EXPECT_LT(std::strstr(log, "[camera:"), std::strstr(log, "[lane:"));   /* dependency first */
    EXPECT_EQ(fileExists("supervisor.pid"), 0);
    EXPECT_EQ(countZombieChildren(), 0);
}

TEST_F(IntegrationTest, HeartbeatLoss_RestartsServiceAndItsDependents)
{
    logClose();
    std::string conf = "[service]\nname=radar\ncommand=" + std::string(demoCommand("radar", "--hang-after 1")) +
                       "\nheartbeat_timeout=1\nrestart_delay=1\n"
                       "[service]\nname=fusion\ncommand=" + std::string(demoCommand("fusion", "")) +
                       "\ndepends_on=radar\n";
    writeFile("hb.conf", conf.c_str());
    sendSignalLater(SIGTERM, 6000);
    const char *args[] = { "supervisor", "-c", "hb.conf" };
    EXPECT_EQ(runInTestDir(3, args), 0);
    const char *log = readFile("supervisor.log");
    EXPECT_CONTAINS(log, "E010 Heartbeat timeout");
    EXPECT_CONTAINS(log, "[radar:");
    EXPECT_CONTAINS(log, "stopping dependent of radar for restart");
    EXPECT_CONTAINS(log, "I004 supervisor stopped");
}

TEST_F(IntegrationTest, StatusRequestWhileRunning_DashboardOnStdout)
{
    logClose();
    std::string conf = "[service]\nname=camera\ncommand=" + std::string(demoCommand("camera", "")) + "\n";
    writeFile("st.conf", conf.c_str());
    sendSignalLater(SIGUSR1, 1500);
    sendSignalLater(SIGTERM, 2500);
    char cwd[MAX_PATH];
    ASSERT_NE(getcwd(cwd, sizeof(cwd)), nullptr);
    ASSERT_EQ(chdir(testDir()), 0);
    char prog[] = "supervisor", c[] = "-c", file[] = "st.conf";
    char *argv[] = { prog, c, file, nullptr };
    captureStdoutBegin();
    int code = runSupervisor(3, argv);
    const char *out = captureStdoutEnd();
    ASSERT_EQ(chdir(cwd), 0);
    EXPECT_EQ(code, 0);
    EXPECT_CONTAINS(out, "START-UP CONFIGURATION SUMMARY");
    EXPECT_CONTAINS(out, "STATUS DASHBOARD");
    EXPECT_CONTAINS(out, "camera");
    EXPECT_CONTAINS(out, "RUNNING");
}
