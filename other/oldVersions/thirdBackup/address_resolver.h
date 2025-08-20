#ifndef ADDRESS_RESOLVER_H
#define ADDRESS_RESOLVER_H

#include "common.h"
#include <stdint.h>

// Symbol information
typedef struct {
    uint64_t address;
    uint64_t size;
    char name[256];
    char demangled_name[512];
    source_location_t location;
    bool is_function;
    bool is_inlined;
} symbol_info_t;

// Memory mapping information
typedef struct {
    uint64_t start_addr;
    uint64_t end_addr;
    uint64_t file_offset;
    char pathname[256];
    bool is_executable;
    bool is_writable;
    bool is_shared;
} memory_mapping_t;

// Address resolver state
typedef struct address_resolver address_resolver_t;

// API functions
address_resolver_t* address_resolver_create(pid_t pid);
void address_resolver_destroy(address_resolver_t *resolver);

// Initialize from running process
int address_resolver_init_process(address_resolver_t *resolver);
int address_resolver_init_binary(address_resolver_t *resolver, const char *binary_path);

// Resolve addresses
int address_resolver_resolve(address_resolver_t *resolver,
                           uint64_t address, symbol_info_t *symbol);
int address_resolver_resolve_batch(address_resolver_t *resolver,
                                 const uint64_t *addresses, int count,
                                 symbol_info_t *symbols);

// Source location resolution
int address_resolver_get_source_location(address_resolver_t *resolver,
                                       uint64_t address, source_location_t *location);
int address_resolver_get_line_info(address_resolver_t *resolver,
                                 uint64_t address, char *filename, 
                                 size_t filename_size, int *line, int *column);

// Memory mapping functions
int address_resolver_get_mappings(address_resolver_t *resolver,
                                memory_mapping_t **mappings, int *count);
void address_resolver_free_mappings(memory_mapping_t *mappings);
const memory_mapping_t* address_resolver_find_mapping(address_resolver_t *resolver,
                                                    uint64_t address);

// Symbol table functions
int address_resolver_load_symbols(address_resolver_t *resolver);
int address_resolver_find_symbol(address_resolver_t *resolver,
                               const char *name, symbol_info_t *symbol);
int address_resolver_get_function_at(address_resolver_t *resolver,
                                   uint64_t address, symbol_info_t *function);

// Cache management
void address_resolver_clear_cache(address_resolver_t *resolver);
int address_resolver_set_cache_size(address_resolver_t *resolver, size_t max_entries);

// Utility functions
const char* address_resolver_demangle(const char *mangled_name, 
                                    char *buffer, size_t buffer_size);
void address_resolver_print_symbol(const symbol_info_t *symbol);
void address_resolver_print_mapping(const memory_mapping_t *mapping);

#endif // ADDRESS_RESOLVER_H
