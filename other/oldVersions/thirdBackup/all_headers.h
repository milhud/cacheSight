#ifndef ALL_HEADERS_H
#define ALL_HEADERS_H

#include "common.h"
#include "hardware_detector.h"
#include "ast_analyzer.h"
#include "pattern_detector.h"
#include "loop_analyzer.h"
#include "data_layout_analyzer.h"
#include "perf_sampler.h"
#ifdef HAVE_PAPI
#include "papi_sampler.h"
#endif
#include "sample_collector.h"
#include "address_resolver.h"
#include "pattern_classifier.h"
#include "recommendation_engine.h"
#include "evaluator.h"
#include "config_parser.h"
#include "report_generator.h"

#endif // ALL_HEADERS_H
