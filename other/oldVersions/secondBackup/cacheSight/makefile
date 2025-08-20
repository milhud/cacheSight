# Cache Optimizer Tool Makefile

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -g -pthread
CXXFLAGS = -Wall -Wextra -O2 -g -pthread -std=c++14
LDFLAGS = -pthread -lm -lpfm -lpapi -ldl

# LLVM/Clang flags
LLVM_CONFIG = llvm-config
CLANG_CXXFLAGS = $(shell $(LLVM_CONFIG) --cxxflags)
CLANG_LDFLAGS = $(shell $(LLVM_CONFIG) --ldflags --libs --system-libs) \
                 -lclang -lclangAST -lclangBasic -lclangFrontend \
                 -lclangTooling -lclangSerialization -lclangDriver \
                 -lclangParse -lclangSema -lclangAnalysis -lclangEdit \
                 -lclangLex -lclangASTMatchers

# Source files
C_SOURCES = \
    common.c \
    hardware_detector.c \
    cache_topology.c \
    bandwidth_benchmark.c \
    pattern_detector.c \
    loop_analyzer.c \
    data_layout_analyzer.c \
    perf_sampler.c \
    papi_sampler.c \
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
    main.c

CXX_SOURCES = \
    ast_analyzer.cpp

# Object files
C_OBJECTS = $(C_SOURCES:.c=.o)
CXX_OBJECTS = $(CXX_SOURCES:.cpp=.o)
OBJECTS = $(C_OBJECTS) $(CXX_OBJECTS)

# Target executable
TARGET = cache_optimizer

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS) $(CLANG_LDFLAGS)

# Compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Compile C++ files (AST analyzer)
ast_analyzer.o: ast_analyzer.cpp
	$(CXX) $(CXXFLAGS) $(CLANG_CXXFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -f $(OBJECTS) $(TARGET)
	rm -f cache_optimizer.log cache_info.txt report.html

# Install (optional)
install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

# Test target
test: $(TARGET)
	./$(TARGET) -v test_matrix.c

.PHONY: all clean install test
