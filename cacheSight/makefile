# Cache Optimizer Tool Makefile

# Compiler settings
CC := clang
CXX := clang++
LLVM_CONFIG := llvm-config-14

# Base flags
CFLAGS := -Wall -Wextra -O2 -g -pthread -fPIC
CXXFLAGS := -Wall -Wextra -O2 -g -pthread -fPIC -std=c++14

# Include paths
INCLUDES := -I. \
            -I/usr/lib/llvm-14/include \
            -I/usr/include/llvm-14 \
            -I/usr/include/llvm-c-14

# LLVM/Clang specific flags
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags)

# Update compiler flags with LLVM flags
CXXFLAGS += $(LLVM_CXXFLAGS)

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
              -lclangIndex \
              -lclangFrontendTool

# System libraries
SYS_LIBS := -lpthread -ldl -lm -lpfm -lpapi

# Full linker flags
LDFLAGS := -L/usr/lib/llvm-14/lib \
           $(LLVM_LDFLAGS) \
           $(CLANG_LIBS) \
           -lLLVM-14 \
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

# Run tests
test: $(TARGET)
	./$(TARGET) --test

# Build and run
run: $(TARGET)
	./$(TARGET)

.PHONY: all clean install uninstall depend debug test run