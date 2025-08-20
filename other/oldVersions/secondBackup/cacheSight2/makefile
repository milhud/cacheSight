# Cache Optimizer Tool Makefile - Fixed Version

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -O2 -g -pthread -fPIC
CXXFLAGS = -Wall -Wextra -O2 -g -pthread -std=c++14 -fPIC
LDFLAGS = -pthread -lm -ldl

# Check for required libraries
HAS_PFM := $(shell ldconfig -p | grep libpfm >/dev/null && echo yes || echo no)
HAS_PAPI := $(shell ldconfig -p | grep libpapi >/dev/null && echo yes || echo no)

ifeq ($(HAS_PFM),yes)
    LDFLAGS += -lpfm
else
    $(error libpfm4 not found. Please install libpfm4-dev)
endif

ifeq ($(HAS_PAPI),yes)
    CFLAGS += -DHAVE_PAPI
    LDFLAGS += -lpapi
endif

# LLVM/Clang configuration
LLVM_CONFIG := llvm-config
ifeq ($(shell which $(LLVM_CONFIG) 2>/dev/null),)
    $(error llvm-config not found. Please install LLVM development packages)
endif

CLANG_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags) -fno-rtti
CLANG_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --libs)
CLANG_LIBS := -lclang -lclangAST -lclangBasic -lclangFrontend \
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

ifeq ($(HAS_PAPI),yes)
    C_SOURCES += papi_sampler.c
endif

CXX_SOURCES = ast_analyzer.cpp

OBJECTS = $(C_SOURCES:.c=.o) $(CXX_SOURCES:.cpp=.o)
TARGET = cache_optimizer

# Build rules
.PHONY: all clean install test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CXX) $(OBJECTS) -o $@ $(LDFLAGS) $(CLANG_LDFLAGS) $(CLANG_LIBS)

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

ast_analyzer.o: ast_analyzer.cpp
	@echo "Compiling $<..."
	$(CXX) $(CXXFLAGS) $(CLANG_CXXFLAGS) -c $< -o $@

# Dependencies
common.o: common.c common.h
hardware_detector.o: hardware_detector.c hardware_detector.h common.h
cache_topology.o: cache_topology.c cache_topology.h hardware_detector.h common.h
bandwidth_benchmark.o: bandwidth_benchmark.c bandwidth_benchmark.h hardware_detector.h common.h
pattern_detector.o: pattern_detector.c pattern_detector.h ast_analyzer.h common.h
loop_analyzer.o: loop_analyzer.c loop_analyzer.h ast_analyzer.h hardware_detector.h common.h
data_layout_analyzer.o: data_layout_analyzer.c data_layout_analyzer.h ast_analyzer.h hardware_detector.h common.h
perf_sampler.o: perf_sampler.c perf_sampler.h hardware_detector.h common.h
sample_collector.o: sample_collector.c sample_collector.h perf_sampler.h hardware_detector.h common.h
address_resolver.o: address_resolver.c address_resolver.h common.h
pattern_classifier.o: pattern_classifier.c pattern_classifier.h ast_analyzer.h sample_collector.h hardware_detector.h common.h
statistical_analyzer.o: statistical_analyzer.c statistical_analyzer.h sample_collector.h common.h
false_sharing_detector.o: false_sharing_detector.c false_sharing_detector.h sample_collector.h hardware_detector.h common.h
bank_conflict_analyzer.o: bank_conflict_analyzer.c bank_conflict_analyzer.h sample_collector.h hardware_detector.h common.h
recommendation_engine.o: recommendation_engine.c recommendation_engine.h pattern_classifier.h hardware_detector.h loop_analyzer.h common.h
evaluator.o: evaluator.c evaluator.h hardware_detector.h recommendation_engine.h common.h
config_parser.o: config_parser.c config_parser.h common.h
report_generator.o: report_generator.c report_generator.h hardware_detector.h ast_analyzer.h sample_collector.h pattern_classifier.h recommendation_engine.h common.h
main.o: main.c common.h hardware_detector.h ast_analyzer.h pattern_detector.h loop_analyzer.h data_layout_analyzer.h perf_sampler.h sample_collector.h address_resolver.h pattern_classifier.h recommendation_engine.h evaluator.h config_parser.h report_generator.h

ifeq ($(HAS_PAPI),yes)
papi_sampler.o: papi_sampler.c papi_sampler.h perf_sampler.h common.h
endif

clean:
	rm -f $(OBJECTS) $(TARGET)
	rm -f cache_optimizer.log cache_info.txt report.html *.bak

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

test: $(TARGET)
	./$(TARGET) -v test_cache_patterns.c