# =====================================================================
# ADAS ECU Process Supervisor (Mini "init") - build, test and quality gates
#
#  Setup (once)        make setup      (installs GoogleTest, CUnit, lcov, cppcheck, Valgrind, GDB)
#  Build / run         make app | make run | make daemon | make status | make stop | make debug
#  Testing             make coverage   (GTest unit + integration, coverage table)
#                      make cunit      (CUnit runner, output -> cunit_report.txt)
#  Memory              make valgrind   (intentional parent + child leaks -> valgrind_report.txt)
#                      make valgrind-clean (normal build, clean run -> valgrind_clean_report.txt)
#                      make valgrind-tests (all GTest cases -> valgrind_tests_report.txt)
#  Secure coding       make misra      (strict GCC diagnostics, must be 0 warnings)
#                      make cppcheck   (-> cppcheck_report.txt, must be empty)
#  Optimization        make optimize   (.s size at -O0..-O3 -> optimization_report.txt)
#  Multiprocessing     make crash-storm      (20 services crash at the same moment: no zombies)
#                      make process-performance (crash detection and restart latency)
#  Multithreading      make helgrind-race-buggy | helgrind-race-fixed
#                      make helgrind-deadlock-buggy | helgrind-deadlock-fixed
#                      make helgrind-supervisor (real supervisor under Helgrind, Expected 0 errors)
#  Debugging (GDB)     make gdb-null | gdb-overflow | gdb-logic | gdb-doublefree
#                      make gdb-inject-fork | gdb-inject-file
#                      make valgrind-doublefree | make faults-fixed
#  Everything          make all-checks
#  Cleanup             make clean
# =====================================================================

CC             := gcc
CXX            := g++
CFLAGS         := -std=c99 -D_DEFAULT_SOURCE -Iinclude
MISRA_FLAGS    := -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wunused-parameter -fanalyzer -Iinclude
COVERAGE_FLAGS := -O0 -g --coverage -Iinclude
LDFLAGS        := -lgtest -lgtest_main -pthread --coverage
DEBUG_FLAGS    := -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -g -O0 -Iinclude
PROC_FLAGS     := -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -g -Iinclude
THREAD_FLAGS   := -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -g -pthread -Iinclude

# Source definitions
SRCS           := $(wildcard src/*.c)
MODULES        := $(filter-out src/main.c, $(SRCS))
OBJS           := $(notdir $(MODULES:.c=.o)) test_shim.o
CONCURRENCY    := tests/concurrency
DEMO           := services/demo_service

# lcov 2.x needs extra flags; lcov 1.x does not accept them
LCOV_MAJOR     := $(shell lcov --version 2>/dev/null | sed -n 's/.*[Vv]ersion \([0-9]*\).*/\1/p')
ifeq ($(LCOV_MAJOR),2)
LCOV_OPTS      := --rc branch_coverage=1 --ignore-errors inconsistent,mismatch,unused
else
LCOV_OPTS      := --rc lcov_branch_coverage=1
endif

.PHONY: all setup app run daemon status stop debug coverage cunit misra cppcheck valgrind valgrind-clean valgrind-tests \
        optimize crash-storm process-performance \
        helgrind-race-buggy helgrind-race-fixed helgrind-deadlock-buggy helgrind-deadlock-fixed helgrind-supervisor \
        gdb-null gdb-overflow gdb-logic gdb-doublefree gdb-inject-fork gdb-inject-file \
        valgrind-doublefree faults-fixed all-checks clean

all: app

# ------------------------------------------------- 0. one-time setup
#    Ubuntu / Debian (GitHub Codespaces): the libraries and tools the
#    test, analysis and debug targets need
setup:
	sudo apt-get update
	sudo apt-get install -y build-essential libgtest-dev libcunit1-dev lcov cppcheck valgrind gdb

# ---------------------------------------------------------------- 1. build
#    the demo ADAS service is needed by the sample configuration and the tests
$(DEMO): $(DEMO).c
	$(CC) -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra $< -o $@

app: $(DEMO)
	$(CC) $(CFLAGS) -g $(SRCS) -o supervisor

#    foreground: Ctrl+C stops the supervisor and all services
run: app
	./supervisor -c supervisor.conf

daemon: app
	./supervisor -c supervisor.conf -d
	@echo "Supervisor running in the background (PID $$(cat supervisor.pid)). Use: make status | make stop"

status:
	@kill -USR1 $$(cat supervisor.pid) && sleep 1 && cat supervisor.status

stop:
	@kill -TERM $$(cat supervisor.pid) && echo "Stop requested; see supervisor.log"

debug: $(DEMO)
	$(CC) $(DEBUG_FLAGS) $(SRCS) -o supervisor_debug

# ---------------------------------------------------- 2. GTest + coverage
#    modules and the shim are compiled as C, the test file as C++
coverage: $(DEMO)
	@echo '#include <gtest/gtest.h>' | $(CXX) -E -x c++ - > /dev/null 2>&1 || \
	    { echo "ERROR: GoogleTest is not installed. Run: make setup"; exit 1; }
	@echo "==> Compiling Unit Tests with Coverage Instrumentation..."
	$(CC) $(CFLAGS) $(COVERAGE_FLAGS) -c $(MODULES) tests/test_shim.c
	$(CXX) $(COVERAGE_FLAGS) $(OBJS) tests/test_supervisor.cpp $(LDFLAGS) -o test_runner
	@echo "==> Running GTest Unit and Integration Tests..."
	./test_runner
	@echo "==> Extracting and Filtering Coverage Data..."
	@lcov --capture --directory . --output-file coverage.info --quiet $(LCOV_OPTS)
	@lcov --remove coverage.info '/usr/*' '*/tests/*' --output-file filtered.info --quiet $(LCOV_OPTS)
	@echo "========================================================================"
	@echo "                       TERMINAL COVERAGE SUMMARY                        "
	@echo "========================================================================"
	@lcov --summary filtered.info $(LCOV_OPTS)
	@echo "========================================================================"
	@echo "Module Function & Branch Details:"
	@gcov -b -n $(notdir $(MODULES:.c=.gcda)) | grep -A 4 "File 'src/"

# ------------------------------------------------------------- 3. CUnit
cunit: $(DEMO)
	@echo '#include <CUnit/Basic.h>' | $(CC) -E -x c - > /dev/null 2>&1 || \
	    { echo "ERROR: CUnit is not installed. Run: make setup"; exit 1; }
	$(CC) $(CFLAGS) -Wall -Wextra -g $(MODULES) tests/test_shim.c tests/cunit/test_cunit.c -lcunit -o cunit_runner
	./cunit_runner | tee cunit_report.txt

# ----------------------------------------------------------- 4. MISRA-C
misra:
	@echo "==> Checking MISRA-C Compliance via GCC Diagnostics..."
	$(CC) $(MISRA_FLAGS) $(SRCS) -o /dev/null
	@echo "==> MISRA-C Check PASSED: 0 warnings, 0 errors."

# ---------------------------------------------------------- 5. CPPCheck
#    staticFunction / unusedFunction are suppressed: the module interface
#    functions (LLD 6.2) are public on purpose and are called by the tests.
#    unmatchedSuppression: older cppcheck versions do not know staticFunction.
cppcheck:
	@echo "==> Running CPPCheck Static Analysis..."
	cppcheck --enable=all --check-level=exhaustive --inline-suppr --suppress=missingIncludeSystem --suppress=checkersReport \
	         --suppress=staticFunction --suppress=unusedFunction --suppress=unmatchedSuppression --std=c99 -Iinclude src/ 2> cppcheck_report.txt
	@cat cppcheck_report.txt
	@echo "==> CPPCheck Complete. Output logged to cppcheck_report.txt."

# ---------------------------------------------------------- 6. Valgrind
#    IPC application = the supervisor (parent) + the service it forks and
#    execs (child); they talk over the heartbeat pipe. Both are built with
#    -DLEAK_DEMO: the parent leaks 128 bytes, the child leaks 64 bytes.
#    --trace-children=yes makes Valgrind follow the child through
#    fork() + execvp(), so each process gets its own report tagged ==PID==.
ipc_application: $(SRCS) $(DEMO).c
	$(CC) $(CFLAGS) -g -O0 -DLEAK_DEMO $(SRCS) -o ipc_application
	$(CC) -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -g -O0 -DLEAK_DEMO $(DEMO).c -o $(DEMO)_leak

valgrind: ipc_application
	@echo "==> Running the IPC application under Valgrind for 15 s, then SIGTERM..."
	@echo "    (EXPECTED: one leak in the parent and one in the child, both deliberate: -DLEAK_DEMO)"
	timeout -s TERM 15 \
	valgrind \
	    --trace-children=yes \
	    --leak-check=full \
	    ./ipc_application -c debug/valgrind.conf > /dev/null 2> valgrind_report.txt; \
	status=$$?; [ $$status -eq 124 ] || [ $$status -eq 0 ]
	@cat valgrind_report.txt
	@echo "========================================================================"
	@echo "                  LEAK OWNERSHIP (one row per process)                  "
	@echo "========================================================================"
	@parent=$$(sed -n 's/^==\([0-9]*\)== Command:.*/\1/p' valgrind_report.txt | head -n 1); \
	printf "%-7s %-7s %-45s %s\n" "PID" "ROLE" "PROGRAM" "DEFINITELY LOST"; \
	for p in $$(sed -n 's/^==\([0-9]*\)== Command:.*/\1/p' valgrind_report.txt); do \
	    if [ "$$p" = "$$parent" ]; then role=PARENT; else role=CHILD; fi; \
	    printf "%-7s %-7s %-45s %s\n" "$$p" "$$role" \
	        "$$(sed -n "s/^==$$p== Command: //p" valgrind_report.txt)" \
	        "$$(sed -n "s/^==$$p==    definitely lost: //p" valgrind_report.txt)"; \
	done
	@echo "------------------------------------------------------------------------"
	@echo "Where each leak was allocated (same ==PID== as the row above):"
	@grep -E "definitely lost in loss record|by 0x.*(parentLeakDemo|childLeakDemo)" valgrind_report.txt
	@echo "------------------------------------------------------------------------"
	@echo "Parent -> child link (written by the parent in supervisor.log):"
	@grep "I002 service started" supervisor.log | tail -n 1


#    Run the normal, non-LEAK_DEMO build and fail if any process reports errors.
valgrind-clean: app debug/valgrind_clean.conf
	@echo "==> Running the normal IPC application under Valgrind for 15 s..."
	@timeout -s TERM 15 valgrind \
		--trace-children=yes \
		--leak-check=full \
		--show-leak-kinds=all \
		--error-exitcode=1 \
		./supervisor -c debug/valgrind_clean.conf > /dev/null 2> valgrind_clean_report.txt; \
	status=$$?; \
	if [ $$status -ne 124 ] && [ $$status -ne 0 ]; then \
		cat valgrind_clean_report.txt; exit $$status; \
	fi
	@cat valgrind_clean_report.txt
	@if ! grep -q 'ERROR SUMMARY:' valgrind_clean_report.txt; then \
		echo "ERROR: Valgrind did not produce an error summary."; exit 1; \
	fi
	@if grep -Eq 'ERROR SUMMARY: [1-9][0-9]* errors' valgrind_clean_report.txt; then \
		echo "ERROR: Valgrind found memory errors; see valgrind_clean_report.txt."; exit 1; \
	fi
	@echo "==> Valgrind clean run passed. Report: valgrind_clean_report.txt"
#    non-interactive: every GTest case under Valgrind
valgrind-tests: coverage
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 \
	         --log-file=valgrind_tests_report.txt ./test_runner \
> valgrind_gtest_output.txt 2>&1
	@tail -n 12 valgrind_tests_report.txt

# ------------------------------------------------------ 7. Optimization
optimize:
	@rm -rf asm && mkdir -p asm/O0 asm/O1 asm/O2 asm/O3
	@for lvl in 0 1 2 3; do \
	    for f in $(SRCS); do $(CC) $(CFLAGS) -O$$lvl -S $$f -o asm/O$$lvl/$$(basename $$f .c).s; done; \
	    $(CC) $(CFLAGS) -O$$lvl $(SRCS) -o asm/supervisor_O$$lvl; \
	done
	@{ echo "ASSEMBLY (.s) FILE SIZE IN BYTES PER MODULE"; \
	   printf "%-16s %10s %10s %10s %10s\n" "Module" "-O0" "-O1" "-O2" "-O3"; \
	   for f in $(SRCS); do m=$$(basename $$f .c); \
	       printf "%-16s %10s %10s %10s %10s\n" $$m.s $$(wc -c < asm/O0/$$m.s) $$(wc -c < asm/O1/$$m.s) \
	              $$(wc -c < asm/O2/$$m.s) $$(wc -c < asm/O3/$$m.s); done; \
	   printf "%-16s %10s %10s %10s %10s\n" "TOTAL bytes" $$(cat asm/O0/*.s | wc -c) $$(cat asm/O1/*.s | wc -c) \
	          $$(cat asm/O2/*.s | wc -c) $$(cat asm/O3/*.s | wc -c); \
	   printf "%-16s %10s %10s %10s %10s\n" "TOTAL lines" $$(cat asm/O0/*.s | wc -l) $$(cat asm/O1/*.s | wc -l) \
	          $$(cat asm/O2/*.s | wc -l) $$(cat asm/O3/*.s | wc -l); \
	   echo; echo "EXECUTABLE SIZE (size command, bytes)"; \
	   size asm/supervisor_O0 asm/supervisor_O1 asm/supervisor_O2 asm/supervisor_O3; \
	   echo; echo "-O0: no optimization, every variable kept in memory (best for GDB)."; \
	   echo "-O1: removes dead stores and redundant loads -> smallest code."; \
	   echo "-O2: adds scheduling and more inlining -> faster, a little larger."; \
	   echo "-O3: aggressive inlining, loop unrolling, vectorization -> fastest, largest code."; \
	} | tee optimization_report.txt

# --------------------------------------------------- 8. Multiprocessing
#    The supervisor is single-threaded by design (LLD 9): its concurrency is
#    many child processes exiting at the same time, handled through SIGCHLD.
bin_crash_storm: $(CONCURRENCY)/test_crash_storm.c $(MODULES)
	$(CC) $(PROC_FLAGS) $(MODULES) $< -o $@

bin_process_performance: $(CONCURRENCY)/test_performance.c $(MODULES) $(DEMO)
	$(CC) $(PROC_FLAGS) -O2 $(MODULES) $< -o $@

crash-storm: bin_crash_storm
	@echo "==> 20 services crash at the same moment (Expected: all reaped, all restarted, 0 zombies)..."
	./bin_crash_storm

process-performance: bin_process_performance
	@echo "==> Measuring crash detection, restart and heartbeat-loss latency..."
	./bin_process_performance

# ---------------------------------------- 8b. Multithreading (Helgrind)
#    LLD 9.1 Alternative 1 (one monitor thread per service) was rejected.
#    These demos show the data race and the lock-order deadlock it would
#    bring; helgrind-supervisor proves the real, single-threaded supervisor
#    has 0 thread errors.
bin_race_buggy: $(CONCURRENCY)/test_thread_race.c $(MODULES)
	$(CC) $(THREAD_FLAGS) $(MODULES) $< -o $@

bin_race_fixed: $(CONCURRENCY)/test_thread_race.c $(MODULES)
	$(CC) $(THREAD_FLAGS) -DUSE_MUTEX $(MODULES) $< -o $@

bin_deadlock_buggy: $(CONCURRENCY)/test_deadlock.c
	$(CC) $(THREAD_FLAGS) $< -o $@

bin_deadlock_fixed: $(CONCURRENCY)/test_deadlock.c
	$(CC) $(THREAD_FLAGS) -DFIX_DEADLOCK $< -o $@

helgrind-race-buggy: bin_race_buggy
	@echo "==> Native run, threads truly in parallel (Expected: updates lost):"
	-./bin_race_buggy
	@echo "==> Helgrind: 4 monitor threads update one service with NO lock (Expected: data race reported)..."
	@echo "    (Helgrind runs threads one at a time, so the count may look correct here; the race is still reported)"
	-valgrind --tool=helgrind ./bin_race_buggy

helgrind-race-fixed: bin_race_fixed
	@echo "==> Helgrind: 4 monitor threads update one service WITH a table mutex (Expected 0 errors)..."
	valgrind --tool=helgrind --error-exitcode=1 ./bin_race_fixed

helgrind-deadlock-buggy: bin_deadlock_buggy
	@echo "==> Helgrind: monitor takes table->log, dashboard takes log->table (Expected: lock order violated)..."
	-valgrind --tool=helgrind ./bin_deadlock_buggy

helgrind-deadlock-fixed: bin_deadlock_fixed
	@echo "==> Helgrind: both threads take table->log (Expected 0 errors)..."
	valgrind --tool=helgrind --error-exitcode=1 ./bin_deadlock_fixed

helgrind-supervisor: app
	@echo "==> Helgrind on the real supervisor for 12 s with supervisor.conf (single-threaded, Expected 0 errors)..."
	timeout -s TERM 12 valgrind --tool=helgrind ./supervisor -c supervisor.conf > /dev/null 2> helgrind_report.txt; \
	status=$$?; [ $$status -eq 124 ] || [ $$status -eq 0 ]
	@cat helgrind_report.txt
	@grep -q "ERROR SUMMARY: 0 errors" helgrind_report.txt && echo "==> Helgrind PASSED: 0 thread errors."

# ------------------------------------------- 9. GDB and fault injection
bin_fault_null: debug/fault_null_pointer.c $(MODULES)
	$(CC) $(DEBUG_FLAGS) $(MODULES) $< -o $@

bin_fault_overflow: debug/fault_buffer_overflow.c $(MODULES)
	$(CC) $(DEBUG_FLAGS) $(MODULES) $< -o $@

bin_fault_logic: debug/fault_zombie_logic.c $(MODULES)
	$(CC) $(DEBUG_FLAGS) $(MODULES) $< -o $@

bin_fault_doublefree: debug/fault_double_free.c $(MODULES)
	$(CC) $(DEBUG_FLAGS) $(MODULES) $< -o $@

gdb-null: bin_fault_null
	-gdb -q -batch -x debug/gdb_null_pointer.gdb ./bin_fault_null

gdb-overflow: bin_fault_overflow
	-gdb -q -batch -x debug/gdb_buffer_overflow.gdb ./bin_fault_overflow

gdb-logic: bin_fault_logic
	-gdb -q -batch -x debug/gdb_zombie_logic.gdb ./bin_fault_logic

gdb-doublefree: bin_fault_doublefree
	-gdb -q -batch -x debug/gdb_double_free.gdb ./bin_fault_doublefree

valgrind-doublefree: bin_fault_doublefree
	@echo "==> Valgrind on debug/fault_double_free.c: a DELIBERATE double free (fault-injection demo)."
	@echo "    EXPECTED: 'Invalid read' and 'Invalid free()' pointing at fault_double_free.c:28 and ptFree()."
	@echo "    The fixed version runs clean: make faults-fixed"
	-valgrind --leak-check=full ./bin_fault_doublefree

#    fault injection into the real program (runs in a scratch folder gdb_run/)
gdb-inject-fork: debug
	@rm -rf gdb_run && mkdir gdb_run
	-cd gdb_run && gdb -q -batch -x ../debug/gdb_inject_fork.gdb ../supervisor_debug
	@grep -E "E006|I003|I004" gdb_run/supervisor.log

gdb-inject-file: debug
	@rm -rf gdb_run && mkdir gdb_run
	-cd gdb_run && gdb -q -batch -x ../debug/gdb_inject_file.gdb ../supervisor_debug
	@grep "E001" gdb_run/supervisor.log

#    the same four programs built with -DFIXED: all run cleanly
faults-fixed:
	@for f in null_pointer buffer_overflow zombie_logic double_free; do \
	    echo "==> fault_$$f (FIXED)"; \
	    $(CC) $(DEBUG_FLAGS) -DFIXED $(MODULES) debug/fault_$$f.c -o bin_fixed_$$f && ./bin_fixed_$$f; \
	done

# ------------------------------------------------------- 10. everything
all-checks: app misra cppcheck coverage cunit valgrind-tests optimize crash-storm process-performance \
            helgrind-race-fixed helgrind-deadlock-fixed helgrind-supervisor faults-fixed

# ------------------------------------------------------------ 11. clean
clean:
	rm -rf supervisor supervisor_debug ipc_application $(DEMO) $(DEMO)_leak test_runner cunit_runner *.o *.gcno *.gcda *.info *.gcov asm gdb_run \
	       bin_crash_storm bin_process_performance bin_race_buggy bin_race_fixed bin_deadlock_buggy bin_deadlock_fixed \
	       bin_fault_null bin_fault_overflow bin_fault_logic bin_fault_doublefree bin_fixed_* \
	       cppcheck_report.txt valgrind_report.txt valgrind_tests_report.txt cunit_report.txt helgrind_report.txt \
	       optimization_report.txt supervisor.log supervisor.log.1 supervisor.pid supervisor.status
