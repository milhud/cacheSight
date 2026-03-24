# Cache Optimizer Tool Makefile

# Compiler settings - use AOCC (AMD Clang 17) or fall back to system clang
AOCC_PREFIX := /gpfsm/dulocal15/sles15/amd/aocc-compiler-5.0.0
CC := $(AOCC_PREFIX)/bin/clang
CXX := $(AOCC_PREFIX)/bin/clang++
LLVM_CONFIG := $(AOCC_PREFIX)/bin/llvm-config

# Base flags
CFLAGS := -Wall -Wextra -O2 -g -pthread -fPIC
CXXFLAGS := -Wall -Wextra -O2 -g -pthread -fPIC -std=c++17

# Include paths
INCLUDES := -I. \
            -I$(AOCC_PREFIX)/include \
            -I$(AOCC_PREFIX)/include/clang \
            -I$(AOCC_PREFIX)/include/clang-c

# LLVM/Clang specific flags
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags 2>/dev/null | sed 's/-std=c++[^ ]*//g')
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags 2>/dev/null)
LLVM_LIBS := $(shell $(LLVM_CONFIG) --libs 2>/dev/null)

# Clang resource directory (for builtins like stddef.h)
CLANG_RESOURCE_DIR := $(AOCC_PREFIX)/lib/clang/17

# Update compiler flags with LLVM flags
CXXFLAGS += $(LLVM_CXXFLAGS) -DCLANG_RESOURCE_DIR=\"$(CLANG_RESOURCE_DIR)\"

# Check for optional libraries
HAS_PFM := $(shell pkg-config --exists libpfm 2>/dev/null && echo 1 || (test -f /usr/lib64/libpfm.so -o -f /usr/lib/x86_64-linux-gnu/libpfm.so -o -f /usr/lib/libpfm.so) && echo 1 || echo 0)
HAS_PAPI := $(shell pkg-config --exists papi 2>/dev/null && echo 1 || (test -f /usr/lib64/libpapi.so -o -f /usr/lib/x86_64-linux-gnu/libpapi.so -o -f /usr/lib/libpapi.so) && echo 1 || echo 0)

ifeq ($(HAS_PFM),1)
  PFM_FLAG := -lpfm
  CFLAGS += -DHAVE_PFM
  CXXFLAGS += -DHAVE_PFM
else
  PFM_FLAG :=
endif

ifeq ($(HAS_PAPI),1)
  PAPI_FLAG := -lpapi
  CFLAGS += -DHAVE_PAPI
  CXXFLAGS += -DHAVE_PAPI
else
  PAPI_FLAG :=
endif

# Source files
C_SOURCES := common.c \
             hardware_detector.c \
             cache_topology.c \
             bandwidth_benchmark.c \
             pattern_detector.c \
             loop_analyzer.c \
             data_layout_analyzer.c \
             perf_sampler.c \
             sample_collector.c \
             address_resolver.c \
             pattern_classifier.c \
             statistical_analyzer.c \
             false_sharing_detector.c \
             bank_conflict_analyzer.c \
             recommendation_engine.c \
             evaluator.c \
             config_parser.c \
             report_generator.c \
             main.c \
             papi_sampler.c

CXX_SOURCES := ast_analyzer.cpp

# Object files
C_OBJS := $(C_SOURCES:.c=.o)
CXX_OBJS := $(CXX_SOURCES:.cpp=.o)
OBJS := $(C_OBJS) $(CXX_OBJS)

# Target executable
TARGET := cache_optimizer

# Clang libraries (order matters!)
CLANG_LIBS := -lclangTooling \
              -lclangFrontendTool \
              -lclangFrontend \
              -lclangDriver \
              -lclangSerialization \
              -lclangCodeGen \
              -lclangParse \
              -lclangSema \
              -lclangStaticAnalyzerFrontend \
              -lclangStaticAnalyzerCheckers \
              -lclangStaticAnalyzerCore \
              -lclangAnalysis \
              -lclangARCMigrate \
              -lclangRewriteFrontend \
              -lclangRewrite \
              -lclangEdit \
              -lclangAST \
              -lclangASTMatchers \
              -lclangLex \
              -lclangBasic \
              -lclangToolingCore \
              -lclangToolingInclusions \
              -lclangFormat \
              -lclangToolingRefactoring \
              -lclangDynamicASTMatchers \
              -lclangIndex

# System libraries
SYS_LIBS := -lpthread -ldl -lm $(PFM_FLAG) $(PAPI_FLAG)

# Full linker flags
LDFLAGS := -L$(AOCC_PREFIX)/lib \
           -Wl,-rpath,$(AOCC_PREFIX)/lib \
           $(LLVM_LDFLAGS) \
           $(CLANG_LIBS) \
           $(LLVM_LIBS) \
           $(SYS_LIBS)

# Default target
all: $(TARGET)

# Build the executable
$(TARGET): $(OBJS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Compile C source files
%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile C++ source files
%.o: %.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJS) $(TARGET)
	rm -f *.d

# Install target
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Uninstall target
uninstall:
	rm -f /usr/local/bin/$(TARGET)

# Generate dependencies
depend: $(C_SOURCES) $(CXX_SOURCES)
	$(CC) $(CFLAGS) $(INCLUDES) -MM $(C_SOURCES) > .depend
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MM $(CXX_SOURCES) >> .depend

# Include dependencies if they exist
-include .depend

# Debug target to print variables
debug:
	@echo "CC = $(CC)"
	@echo "CXX = $(CXX)"
	@echo "CFLAGS = $(CFLAGS)"
	@echo "CXXFLAGS = $(CXXFLAGS)"
	@echo "LDFLAGS = $(LDFLAGS)"
	@echo "OBJS = $(OBJS)"
	@echo "HAS_PFM = $(HAS_PFM)"
	@echo "HAS_PAPI = $(HAS_PAPI)"

# Run tests
test: $(TARGET)
	./$(TARGET) --test

# Build and run
run: $(TARGET)
	./$(TARGET)

.PHONY: all clean install uninstall depend debug test run
