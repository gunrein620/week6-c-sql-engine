CC = gcc
BASE_CFLAGS = -std=c99 -Wall -Wextra -I./src
RELEASE_FLAGS = -O2
DEBUG_FLAGS = -g -DDEBUG

BUILD_DIR = build
TEST_BUILD_DIR = $(BUILD_DIR)/tests

SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
LIB_OBJS = $(filter-out $(BUILD_DIR)/main.o,$(OBJS))

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_OBJS = $(patsubst tests/%.c,$(TEST_BUILD_DIR)/%.o,$(TEST_SRCS))
TEST_BINS = $(patsubst tests/%.c,$(TEST_BUILD_DIR)/%,$(TEST_SRCS))

CFLAGS = $(BASE_CFLAGS) $(RELEASE_FLAGS)
COMMA = ,

ifeq ($(OS),Windows_NT)
EXE_EXT = .exe
TARGET = sqlengine$(EXE_EXT)
TEST_TARGETS = $(addsuffix $(EXE_EXT),$(TEST_BINS))
SHELL = powershell.exe
.SHELLFLAGS = -NoProfile -Command
RUN_PREFIX = ./
else
EXE_EXT =
TARGET = sqlengine
TEST_TARGETS = $(TEST_BINS)
RUN_PREFIX = ./
endif

.PHONY: all debug test clean help run-f directories sqlengine

all: CFLAGS = $(BASE_CFLAGS) $(RELEASE_FLAGS)
all: directories $(TARGET)

debug: CFLAGS = $(BASE_CFLAGS) $(DEBUG_FLAGS)
debug: directories $(TARGET)

sqlengine: $(TARGET)

ifeq ($(OS),Windows_NT)
test: CFLAGS = $(BASE_CFLAGS) $(DEBUG_FLAGS)
test: directories $(TARGET) $(TEST_TARGETS)
	@$$passed = 0; $$failed = 0; $$bins = "$(TEST_TARGETS)".Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries); foreach ($$bin in $$bins) { & $$bin; if ($$LASTEXITCODE -eq 0) { $$passed++ } else { $$failed++ } }; Write-Host "Test executables: $$passed passed, $$failed failed"; if ($$failed -ne 0) { exit 1 }
else
test: CFLAGS = $(BASE_CFLAGS) $(DEBUG_FLAGS)
test: directories $(TARGET) $(TEST_TARGETS)
	@passed=0; failed=0; \
	for test_bin in $(TEST_TARGETS); do \
		$$test_bin; \
		if [ $$? -eq 0 ]; then \
			passed=$$((passed + 1)); \
		else \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo "Test executables: $$passed passed, $$failed failed"; \
	test $$failed -eq 0
endif

help:
	@echo "Targets:"
	@echo "  all      Build $(TARGET)"
	@echo "  debug    Build $(TARGET) with debug flags"
	@echo "  test     Build and run unit tests"
	@echo "  clean    Remove build artifacts"
	@echo "  run-f    Run $(RUN_PREFIX)$(TARGET) -f \$$SQL"
	@echo "  help     Show this help message"

ifeq ($(OS),Windows_NT)
run-f: all
	@if ('$(SQL)' -eq '') { \
		Write-Host 'Usage: mingw32-make run-f SQL=sql/members_demo.sql'; \
		exit 1 \
	}
	& '$(RUN_PREFIX)$(TARGET)' -f '$(SQL)'
else
run-f: all
	@if [ -z "$(SQL)" ]; then \
		echo "Usage: make run-f SQL=sql/members_demo.sql"; \
		exit 1; \
	fi
	$(RUN_PREFIX)$(TARGET) -f $(SQL)
endif

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

ifeq ($(OS),Windows_NT)
$(BUILD_DIR)/%.o: src/%.c | directories
	@New-Item -ItemType Directory -Force -Path '$(dir $@)' | Out-Null
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BUILD_DIR)/%.o: tests/%.c | directories
	@New-Item -ItemType Directory -Force -Path '$(dir $@)' | Out-Null
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BUILD_DIR)/%$(EXE_EXT): $(TEST_BUILD_DIR)/%.o $(LIB_OBJS) | directories $(TARGET)
	$(CC) $(CFLAGS) $^ -o $@

directories:
	@New-Item -ItemType Directory -Force -Path '$(BUILD_DIR)' | Out-Null
	@New-Item -ItemType Directory -Force -Path '$(TEST_BUILD_DIR)' | Out-Null
	@New-Item -ItemType Directory -Force -Path 'data' | Out-Null
	@New-Item -ItemType Directory -Force -Path 'schemas' | Out-Null
	@New-Item -ItemType Directory -Force -Path 'sql' | Out-Null

clean:
	@if (Test-Path '$(BUILD_DIR)') { Remove-Item -Recurse -Force '$(BUILD_DIR)' }
	@if (Test-Path '$(TARGET)') { Remove-Item -Force '$(TARGET)' }
else
$(BUILD_DIR)/%.o: src/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BUILD_DIR)/%.o: tests/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BUILD_DIR)/%: $(TEST_BUILD_DIR)/%.o $(LIB_OBJS) | directories $(TARGET)
	$(CC) $(CFLAGS) $^ -o $@

directories:
	@mkdir -p $(BUILD_DIR) $(TEST_BUILD_DIR) data schemas sql

clean:
	rm -rf build $(TARGET)
endif

-include $(OBJS:.o=.d)
-include $(TEST_OBJS:.o=.d)
