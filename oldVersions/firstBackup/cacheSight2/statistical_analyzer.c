#include "statistical_analyzer.h"
#include <math.h>
#include <float.h>

static bool g_initialized = false;

// Comparison function for qsort
static int compare_doubles(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

// Initialize statistical analyzer
int statistical_analyzer_init(void) {
    if (g_initialized) {
        LOG_WARNING("Statistical analyzer already initialized");
        return 0;
    }
    
    LOG_INFO("Initializing statistical analyzer");
    g_initialized = true;
    return 0;
}

// Cleanup statistical analyzer
void statistical_analyzer_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    LOG_INFO("Cleaning up statistical analyzer");
    g_initialized = false;
}

// Calculate basic statistics
int calculate_statistics(const double *data, int count, statistics_t *stats) {
    if (!data || count <= 0 || !stats) {
        LOG_ERROR("Invalid parameters for calculate_statistics");
        return -1;
    }
    
    LOG_DEBUG("Calculating statistics for %d data points", count);
    
    memset(stats, 0, sizeof(statistics_t));
    
    // Create sorted copy for percentiles
    double *sorted = MALLOC_LOGGED(count * sizeof(double));
    if (!sorted) {
        LOG_ERROR("Failed to allocate sorted array");
        return -1;
    }
    memcpy(sorted, data, count * sizeof(double));
    qsort(sorted, count, sizeof(double), compare_doubles);
    
    // Min and max
    stats->min = sorted[0];
    stats->max = sorted[count - 1];
    
    // Mean
    double sum = 0;
    for (int i = 0; i < count; i++) {
        sum += data[i];
    }
    stats->mean = sum / count;
    
    // Median and percentiles
    stats->median = sorted[count / 2];
    if (count % 2 == 0) {
        stats->median = (sorted[count/2 - 1] + sorted[count/2]) / 2.0;
    }
    
    stats->percentile_25 = sorted[(int)(count * 0.25)];
    stats->percentile_75 = sorted[(int)(count * 0.75)];
    stats->percentile_95 = sorted[(int)(count * 0.95)];
    stats->percentile_99 = sorted[(int)(count * 0.99)];
    
    // Variance and standard deviation
    double sum_sq_diff = 0;
    for (int i = 0; i < count; i++) {
        double diff = data[i] - stats->mean;
        sum_sq_diff += diff * diff;
    }
    stats->variance = sum_sq_diff / (count - 1);
    stats->std_dev = sqrt(stats->variance);
    
    // Skewness and kurtosis
    if (stats->std_dev > 0) {
        double sum_cubed = 0;
        double sum_fourth = 0;
        
        for (int i = 0; i < count; i++) {
            double z = (data[i] - stats->mean) / stats->std_dev;
            sum_cubed += z * z * z;
            sum_fourth += z * z * z * z;
        }
        
        stats->skewness = sum_cubed / count;
        stats->kurtosis = sum_fourth / count - 3.0;  // Excess kurtosis
    }
    
    FREE_LOGGED(sorted);
    
    LOG_DEBUG("Statistics: mean=%.2f, median=%.2f, std_dev=%.2f, skew=%.2f",
              stats->mean, stats->median, stats->std_dev, stats->skewness);
    
    return 0;
}

// Calculate pattern statistics
int calculate_pattern_statistics(const cache_miss_sample_t *samples, int count,
                                pattern_statistics_t *stats) {
    if (!samples || count <= 0 || !stats) {
        LOG_ERROR("Invalid parameters for calculate_pattern_statistics");
        return -1;
    }
    
    LOG_INFO("Calculating pattern statistics for %d samples", count);
    
    memset(stats, 0, sizeof(pattern_statistics_t));
    
    // Extract addresses and calculate strides
    uint64_t *addresses = MALLOC_LOGGED(count * sizeof(uint64_t));
    double *strides = MALLOC_LOGGED((count - 1) * sizeof(double));
    double *intervals = MALLOC_LOGGED((count - 1) * sizeof(double));
    
    if (!addresses || !strides || !intervals) {
        LOG_ERROR("Failed to allocate arrays");
        if (addresses) FREE_LOGGED(addresses);
        if (strides) FREE_LOGGED(strides);
        if (intervals) FREE_LOGGED(intervals);
        return -1;
    }
    
    // Copy addresses
    for (int i = 0; i < count; i++) {
        addresses[i] = samples[i].memory_addr;
    }
    
    // Calculate strides and intervals
    for (int i = 1; i < count; i++) {
        strides[i-1] = (double)labs((long)(addresses[i] - addresses[i-1]));
        intervals[i-1] = (double)(samples[i].timestamp - samples[i-1].timestamp);
    }
    
    // Calculate stride statistics
    calculate_statistics(strides, count - 1, &stats->stride_stats);
    
    // Calculate access interval statistics
    calculate_statistics(intervals, count - 1, &stats->access_interval);
    
    // Detect dominant stride
    detect_stride_pattern(addresses, count, &stats->dominant_stride,
                         &stats->stride_regularity);
    
    // Calculate entropy
    stats->entropy = calculate_entropy(addresses, count);
    
    // Calculate autocorrelation
    stats->autocorrelation = calculate_autocorrelation(addresses, count, 1);
    
    // Calculate reuse distance (simplified)
    // This would need a more sophisticated stack distance algorithm
    double *reuse_distances = CALLOC_LOGGED(count, sizeof(double));
    if (reuse_distances) {
        int reuse_count = 0;
        
        for (int i = 0; i < count; i++) {
            // Find previous access to same cache line
            uint64_t cache_line = addresses[i] / 64;  // 64-byte cache lines
            
            for (int j = i - 1; j >= 0 && j >= i - 1000; j--) {
                if (addresses[j] / 64 == cache_line) {
                    reuse_distances[reuse_count++] = (double)(i - j);
                    break;
                }
            }
        }
        
        if (reuse_count > 0) {
            calculate_statistics(reuse_distances, reuse_count, &stats->reuse_distance);
        }
        
        FREE_LOGGED(reuse_distances);
    }
    
    FREE_LOGGED(addresses);
    FREE_LOGGED(strides);
    FREE_LOGGED(intervals);
    
    LOG_INFO("Pattern statistics: entropy=%.2f, autocorr=%.2f, dominant_stride=%d",
             stats->entropy, stats->autocorrelation, stats->dominant_stride);
    
    return 0;
}

// Calculate entropy of address sequence
double calculate_entropy(const uint64_t *addresses, int count) {
    if (!addresses || count <= 0) return 0;
    
    // Calculate histogram of address bits
    int bit_counts[64] = {0};
    int total_bits = 0;
    
    for (int i = 0; i < count; i++) {
        uint64_t addr = addresses[i];
        for (int bit = 0; bit < 64; bit++) {
            if (addr & (1ULL << bit)) {
                bit_counts[bit]++;
                total_bits++;
            }
        }
    }
    
    // Calculate entropy
    double entropy = 0;
    for (int bit = 0; bit < 64; bit++) {
        if (bit_counts[bit] > 0) {
            double p = (double)bit_counts[bit] / total_bits;
            entropy -= p * log2(p);
        }
    }
    
    // Normalize to 0-1 range
    entropy = entropy / 64.0;
    
    LOG_DEBUG("Address entropy: %.4f", entropy);
    return entropy;
}

// Calculate autocorrelation
double calculate_autocorrelation(const uint64_t *addresses, int count, int lag) {
    if (!addresses || count <= lag) return 0;
    
    // Convert to differences
    double *diffs = MALLOC_LOGGED((count - 1) * sizeof(double));
    if (!diffs) return 0;
    
    double mean = 0;
    for (int i = 1; i < count; i++) {
        diffs[i-1] = (double)(addresses[i] - addresses[i-1]);
        mean += diffs[i-1];
    }
    mean /= (count - 1);
    
    // Calculate autocorrelation
    double numerator = 0;
    double denominator = 0;
    
    for (int i = lag; i < count - 1; i++) {
        numerator += (diffs[i] - mean) * (diffs[i - lag] - mean);
    }
    
    for (int i = 0; i < count - 1; i++) {
        denominator += (diffs[i] - mean) * (diffs[i] - mean);
    }
    
    double autocorr = denominator > 0 ? numerator / denominator : 0;
    
    FREE_LOGGED(diffs);
    
    LOG_DEBUG("Autocorrelation at lag %d: %.4f", lag, autocorr);
    return autocorr;
}

// Detect stride pattern
int detect_stride_pattern(const uint64_t *addresses, int count,
                         int *stride, double *confidence) {
    if (!addresses || count < 3 || !stride || !confidence) return -1;
    
    // Count stride occurrences
    typedef struct {
        int stride_value;
        int count;
    } stride_count_t;
    
    stride_count_t *stride_counts = CALLOC_LOGGED(count, sizeof(stride_count_t));
    if (!stride_counts) return -1;
    
    int unique_strides = 0;
    
    // Calculate strides and count occurrences
    for (int i = 1; i < count; i++) {
        int current_stride = (int)(addresses[i] - addresses[i-1]);
        
        // Find or add stride
        bool found = false;
        for (int j = 0; j < unique_strides; j++) {
            if (stride_counts[j].stride_value == current_stride) {
                stride_counts[j].count++;
                found = true;
                break;
            }
        }
        
        if (!found && unique_strides < count) {
            stride_counts[unique_strides].stride_value = current_stride;
            stride_counts[unique_strides].count = 1;
            unique_strides++;
        }
    }
    
    // Find dominant stride
    int max_count = 0;
    int dominant_stride = 0;
    
    for (int i = 0; i < unique_strides; i++) {
        if (stride_counts[i].count > max_count) {
            max_count = stride_counts[i].count;
            dominant_stride = stride_counts[i].stride_value;
        }
    }
    
    *stride = dominant_stride;
    *confidence = (double)max_count / (count - 1);
    
    FREE_LOGGED(stride_counts);
    
    LOG_DEBUG("Dominant stride: %d (confidence: %.2f%%)", *stride, *confidence * 100);
    return 0;
}

// Analyze correlation between two variables
int analyze_correlation(const double *x, const double *y, int count,
                       correlation_result_t *result) {
    if (!x || !y || count < 3 || !result) return -1;
    
    memset(result, 0, sizeof(correlation_result_t));
    
    // Calculate means
    double mean_x = 0, mean_y = 0;
    for (int i = 0; i < count; i++) {
        mean_x += x[i];
        mean_y += y[i];
    }
    mean_x /= count;
    mean_y /= count;
    
    // Calculate correlation coefficient
    double sum_xy = 0, sum_x2 = 0, sum_y2 = 0;
    
    for (int i = 0; i < count; i++) {
        double dx = x[i] - mean_x;
        double dy = y[i] - mean_y;
        sum_xy += dx * dy;
        sum_x2 += dx * dx;
        sum_y2 += dy * dy;
    }
    
    if (sum_x2 > 0 && sum_y2 > 0) {
        result->correlation_coefficient = sum_xy / sqrt(sum_x2 * sum_y2);
    }
    
    // Calculate p-value (simplified t-test)
    if (fabs(result->correlation_coefficient) < 1.0) {
        double t = result->correlation_coefficient * 
                   sqrt((count - 2) / (1 - result->correlation_coefficient * 
                                      result->correlation_coefficient));
        
        // Approximate p-value
        double p = 2 * (1 - 0.5 * (1 + erf(fabs(t) / sqrt(2))));
        result->p_value = p;
        result->is_significant = (p < 0.05);
    }
    
    // Generate description
    const char *strength;
    double abs_corr = fabs(result->correlation_coefficient);
    
    if (abs_corr > 0.9) strength = "very strong";
    else if (abs_corr > 0.7) strength = "strong";
    else if (abs_corr > 0.5) strength = "moderate";
    else if (abs_corr > 0.3) strength = "weak";
    else strength = "very weak";
    
    snprintf(result->description, sizeof(result->description),
             "%s %s correlation (r=%.3f, p=%.4f)",
             strength,
             result->correlation_coefficient >= 0 ? "positive" : "negative",
             result->correlation_coefficient,
             result->p_value);
    
    LOG_DEBUG("Correlation: %s", result->description);
    return 0;
}

// Identify distribution type
distribution_type_t identify_distribution(const double *data, int count) {
    if (!data || count < 30) return DIST_UNKNOWN;
    
    statistics_t stats;
    if (calculate_statistics(data, count, &stats) != 0) {
        return DIST_UNKNOWN;
    }
    
    // Use skewness and kurtosis to identify distribution
    double abs_skew = fabs(stats.skewness);
    double abs_kurt = fabs(stats.kurtosis);
    
    // Normal distribution: skewness ≈ 0, kurtosis ≈ 0
    if (abs_skew < 0.5 && abs_kurt < 0.5) {
        LOG_DEBUG("Distribution appears to be normal");
        return DIST_NORMAL;
    }
    
    // Exponential: positive skew, high kurtosis
    if (stats.skewness > 1.0 && stats.kurtosis > 1.0) {
        LOG_DEBUG("Distribution appears to be exponential");
        return DIST_EXPONENTIAL;
    }
    
    // Uniform: negative kurtosis
    if (stats.kurtosis < -1.0) {
        LOG_DEBUG("Distribution appears to be uniform");
        return DIST_UNIFORM;
    }
    
    // Poisson: skewness ≈ 1/sqrt(mean)
    if (stats.mean > 0) {
        double expected_skew = 1.0 / sqrt(stats.mean);
        if (fabs(stats.skewness - expected_skew) < 0.3) {
            LOG_DEBUG("Distribution appears to be Poisson");
            return DIST_POISSON;
        }
    }
    
    LOG_DEBUG("Distribution type unknown");
    return DIST_UNKNOWN;
}

// Print statistics
void print_statistics(const statistics_t *stats, const char *name) {
    if (!stats) return;
    
    printf("\n%s Statistics:\n", name ? name : "Data");
    printf("  Count: N/A\n");  // Not stored in structure
    printf("  Mean: %.2f\n", stats->mean);
    printf("  Median: %.2f\n", stats->median);
    printf("  Std Dev: %.2f\n", stats->std_dev);
    printf("  Min: %.2f\n", stats->min);
    printf("  Max: %.2f\n", stats->max);
    printf("  25th percentile: %.2f\n", stats->percentile_25);
    printf("  75th percentile: %.2f\n", stats->percentile_75);
    printf("  95th percentile: %.2f\n", stats->percentile_95);
    printf("  99th percentile: %.2f\n", stats->percentile_99);
    printf("  Skewness: %.3f\n", stats->skewness);
    printf("  Kurtosis: %.3f\n", stats->kurtosis);
}

// Print pattern statistics
void print_pattern_statistics(const pattern_statistics_t *stats) {
    if (!stats) return;
    
    printf("\n=== Access Pattern Statistics ===\n");
    
    printf("\nStride Distribution:\n");
    print_statistics(&stats->stride_stats, "Stride");
    printf("  Dominant stride: %d\n", stats->dominant_stride);
    printf("  Stride regularity: %.2f%%\n", stats->stride_regularity * 100);
    
    printf("\nTemporal Reuse:\n");
    print_statistics(&stats->reuse_distance, "Reuse Distance");
    
    printf("\nAccess Intervals:\n");
    print_statistics(&stats->access_interval, "Time Interval");
    
    printf("\nPattern Metrics:\n");
    printf("  Entropy: %.4f\n", stats->entropy);
    printf("  Autocorrelation: %.4f\n", stats->autocorrelation);
}
