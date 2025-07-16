#include "ast_analyzer.h"
#include "common.h"

#include <clang/AST/AST.h>
#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <memory>
#include <vector>
#include <string>
#include <unordered_map>

using namespace clang;
using namespace clang::tooling;

// Internal C++ structures
struct LoopContext {
    const ForStmt *stmt;
    int depth;
    std::string var_name;
    std::vector<static_pattern_t> patterns;
};

struct FunctionContext {
    const FunctionDecl *decl;
    std::string name;
    std::vector<LoopContext> loops;
};

// AST Visitor for analyzing cache patterns
class CachePatternVisitor : public RecursiveASTVisitor<CachePatternVisitor> {
private:
    ASTContext *context;
    SourceManager *source_mgr;
    std::vector<static_pattern_t> patterns;
    std::vector<loop_info_t> loops;
    std::vector<struct_info_t> structs;
    std::vector<std::string> diagnostics;
    
    int current_loop_depth = 0;
    std::vector<LoopContext*> loop_stack;
    FunctionContext *current_function = nullptr;

public:
    CachePatternVisitor(ASTContext *ctx) : context(ctx), source_mgr(&ctx->getSourceManager()) {
        LOG_DEBUG("Created CachePatternVisitor");
    }
    
    // Visit array subscript expressions
    bool VisitArraySubscriptExpr(ArraySubscriptExpr *expr) {
        if (!source_mgr->isInMainFile(expr->getBeginLoc())) {
            return true;
        }
        
        static_pattern_t pattern = {};
        
        // Get source location
        SourceLocation loc = expr->getBeginLoc();
        fillSourceLocation(loc, &pattern.location);
        
        // Analyze the array access pattern
        analyzeArrayAccess(expr, &pattern);
        
        patterns.push_back(pattern);
        
        // Add to current loop if we're in one
        if (!loop_stack.empty()) {
            loop_stack.back()->patterns.push_back(pattern);
        }
        
        LOG_DEBUG("Found array access at %s:%d - pattern: %s",
                  pattern.location.file, pattern.location.line,
                  access_pattern_to_string(pattern.pattern));
        
        return true;
    }
    
    // Visit for loops
    bool VisitForStmt(ForStmt *stmt) {
        if (!source_mgr->isInMainFile(stmt->getBeginLoc())) {
            return true;
        }
        
        current_loop_depth++;
        
        LoopContext *loop_ctx = new LoopContext;
        loop_ctx->stmt = stmt;
        loop_ctx->depth = current_loop_depth;
        
        // Analyze loop structure
        loop_info_t loop_info = {};
        analyzeForLoop(stmt, &loop_info);
        
        loop_stack.push_back(loop_ctx);
        loops.push_back(loop_info);
        
        LOG_DEBUG("Found for loop at %s:%d - depth: %d",
                  loop_info.location.file, loop_info.location.line,
                  loop_info.nest_level);
        
        return true;
    }
    
    // Post-visit for loops
    bool dataTraverseStmtPost(Stmt *stmt) {
        if (isa<ForStmt>(stmt) && !loop_stack.empty()) {
            current_loop_depth--;
            
            // Save patterns found in this loop
            LoopContext *ctx = loop_stack.back();
            if (!loops.empty()) {
                loop_info_t &loop = loops.back();
                loop.pattern_count = ctx->patterns.size();
                if (loop.pattern_count > 0) {
                    loop.patterns = new static_pattern_t[loop.pattern_count];
                    for (int i = 0; i < loop.pattern_count; i++) {
                        loop.patterns[i] = ctx->patterns[i];
                    }
                }
            }
            
            delete ctx;
            loop_stack.pop_back();
        }
        return true;
    }
    
    // Visit struct/class declarations
    bool VisitRecordDecl(RecordDecl *decl) {
        if (!source_mgr->isInMainFile(decl->getBeginLoc()) || !decl->isCompleteDefinition()) {
            return true;
        }
        
        struct_info_t struct_info = {};
        analyzeStruct(decl, &struct_info);
        structs.push_back(struct_info);
        
        LOG_DEBUG("Found struct %s with %d fields", struct_info.struct_name, struct_info.field_count);
        
        return true;
    }
    
    // Visit member expressions (struct field access)
    bool VisitMemberExpr(MemberExpr *expr) {
        if (!source_mgr->isInMainFile(expr->getBeginLoc())) {
            return true;
        }
        
        static_pattern_t pattern = {};
        fillSourceLocation(expr->getBeginLoc(), &pattern.location);
        
        // Analyze struct member access
        analyzeMemberAccess(expr, &pattern);
        patterns.push_back(pattern);
        
        return true;
    }
    
    // Get results
    void getResults(analysis_results_t *results) {
        results->pattern_count = patterns.size();
        if (results->pattern_count > 0) {
            results->patterns = new static_pattern_t[results->pattern_count];
            std::copy(patterns.begin(), patterns.end(), results->patterns);
        }
        
        results->loop_count = loops.size();
        if (results->loop_count > 0) {
            results->loops = new loop_info_t[results->loop_count];
            std::copy(loops.begin(), loops.end(), results->loops);
        }
        
        results->struct_count = structs.size();
        if (results->struct_count > 0) {
            results->structs = new struct_info_t[results->struct_count];
            std::copy(structs.begin(), structs.end(), results->structs);
        }
        
        // Convert diagnostics
        if (!diagnostics.empty()) {
            size_t total_size = 0;
            for (const auto &diag : diagnostics) {
                total_size += diag.length() + 1;
            }
            results->diagnostics = new char[total_size];
            
            char *p = results->diagnostics;
            for (const auto &diag : diagnostics) {
                strcpy(p, diag.c_str());
                p += diag.length() + 1;
            }
            results->diagnostic_count = diagnostics.size();
        }
    }

private:
    void fillSourceLocation(SourceLocation loc, source_location_t *src_loc) {
        PresumedLoc ploc = source_mgr->getPresumedLoc(loc);
        strncpy(src_loc->file, ploc.getFilename(), sizeof(src_loc->file) - 1);
        src_loc->line = ploc.getLine();
        src_loc->column = ploc.getColumn();
        
        if (current_function) {
            strncpy(src_loc->function, current_function->name.c_str(), sizeof(src_loc->function) - 1);
        }
    }
    
    void analyzeArrayAccess(ArraySubscriptExpr *expr, static_pattern_t *pattern) {
        pattern->loop_depth = current_loop_depth;
        pattern->is_pointer_access = false;
        pattern->access_count = 1;
        
        // Get array name
        if (DeclRefExpr *base = dyn_cast<DeclRefExpr>(expr->getBase()->IgnoreParenCasts())) {
            strncpy(pattern->array_name, base->getNameInfo().getAsString().c_str(),
                    sizeof(pattern->array_name) - 1);
        }
        
        // Analyze index expression
        Expr *index = expr->getIdx();
        
        // Check for simple patterns
        if (DeclRefExpr *index_var = dyn_cast<DeclRefExpr>(index->IgnoreParenCasts())) {
            // Simple index variable (e.g., a[i])
            strncpy(pattern->variable_name, index_var->getNameInfo().getAsString().c_str(),
                    sizeof(pattern->variable_name) - 1);
            
            // Check if it's a loop variable
            if (!loop_stack.empty() && loop_stack.back()->var_name == pattern->variable_name) {
                pattern->pattern = SEQUENTIAL;
                pattern->stride = 1;
            } else {
                pattern->pattern = INDIRECT_ACCESS;
            }
        } else if (BinaryOperator *binop = dyn_cast<BinaryOperator>(index->IgnoreParenCasts())) {
            // Binary operation in index (e.g., a[i+1], a[2*i])
            analyzeBinaryIndexExpr(binop, pattern);
        } else {
            // Complex expression
            pattern->pattern = RANDOM;
        }
        
        // Estimate footprint based on array type
        if (expr->getType()->isArrayType()) {
            if (const ConstantArrayType *arr_type = 
                context->getAsConstantArrayType(expr->getBase()->getType())) {
                pattern->estimated_footprint = 
                    context->getTypeSize(arr_type->getElementType()) * 
                    arr_type->getSize().getZExtValue() / 8;
            }
        }
    }
    
    void analyzeBinaryIndexExpr(BinaryOperator *binop, static_pattern_t *pattern) {
        if (binop->getOpcode() == BO_Add || binop->getOpcode() == BO_Sub) {
            // Check for patterns like i+1, i-1
            if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(binop->getRHS()->IgnoreParenCasts())) {
                pattern->pattern = STRIDED;
                pattern->stride = lit->getValue().getSExtValue();
                if (binop->getOpcode() == BO_Sub) {
                    pattern->stride = -pattern->stride;
                }
            }
        } else if (binop->getOpcode() == BO_Mul) {
            // Check for patterns like 2*i, i*stride
            if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(binop->getRHS()->IgnoreParenCasts())) {
                pattern->pattern = STRIDED;
                pattern->stride = lit->getValue().getSExtValue();
            }
        }
    }
    
    void analyzeForLoop(ForStmt *stmt, loop_info_t *loop) {
        fillSourceLocation(stmt->getBeginLoc(), &loop->location);
        loop->nest_level = current_loop_depth;
        
        // Analyze init statement
        if (stmt->getInit()) {
            if (DeclStmt *decl = dyn_cast<DeclStmt>(stmt->getInit())) {
                if (decl->isSingleDecl()) {
                    if (VarDecl *var = dyn_cast<VarDecl>(decl->getSingleDecl())) {
                        strncpy(loop->loop_var, var->getNameAsString().c_str(),
                               sizeof(loop->loop_var) - 1);
                    }
                }
            }
        }
        
        // Get condition expression
        if (stmt->getCond()) {
            std::string cond_str = getSourceText(stmt->getCond());
            strncpy(loop->condition_expr, cond_str.c_str(), sizeof(loop->condition_expr) - 1);
            
            // Try to estimate iterations
            estimateLoopIterations(stmt, loop);
        }
        
        // Get increment expression
        if (stmt->getInc()) {
            std::string inc_str = getSourceText(stmt->getInc());
            strncpy(loop->increment_expr, inc_str.c_str(), sizeof(loop->increment_expr) - 1);
        }
        
        // Check for nested loops
        loop->has_nested_loops = hasNestedLoops(stmt->getBody());
        loop->has_function_calls = hasFunctionCalls(stmt->getBody());
    }
    
    void analyzeStruct(RecordDecl *decl, struct_info_t *info) {
        strncpy(info->struct_name, decl->getNameAsString().c_str(), sizeof(info->struct_name) - 1);
        fillSourceLocation(decl->getBeginLoc(), &info->location);
        
        const ASTRecordLayout &layout = context->getASTRecordLayout(decl);
        info->total_size = layout.getSize().getQuantity();
        
        int field_idx = 0;
        for (auto field : decl->fields()) {
            if (field_idx >= 32) break;
            
            strncpy(info->field_names[field_idx], field->getNameAsString().c_str(),
                    sizeof(info->field_names[field_idx]) - 1);
            
            info->field_offsets[field_idx] = layout.getFieldOffset(field_idx) / 8;
            info->field_sizes[field_idx] = context->getTypeSize(field->getType()) / 8;
            
            if (field->getType()->isPointerType()) {
                info->has_pointer_fields = true;
            }
            
            field_idx++;
        }
        
        info->field_count = field_idx;
        info->is_packed = decl->hasAttr<PackedAttr>();
    }
    
    void analyzeMemberAccess(MemberExpr *expr, static_pattern_t *pattern) {
        pattern->is_struct_access = true;
        pattern->pattern = GATHER_SCATTER;  // Default for struct access
        
        if (FieldDecl *field = dyn_cast<FieldDecl>(expr->getMemberDecl())) {
            strncpy(pattern->variable_name, field->getNameAsString().c_str(),
                    sizeof(pattern->variable_name) - 1);
            
            // Get struct name if possible
            if (DeclRefExpr *base = dyn_cast<DeclRefExpr>(expr->getBase()->IgnoreParenCasts())) {
                strncpy(pattern->struct_name, base->getNameInfo().getAsString().c_str(),
                        sizeof(pattern->struct_name) - 1);
            }
        }
    }
    
    std::string getSourceText(Stmt *stmt) {
        SourceLocation start = stmt->getBeginLoc();
        SourceLocation end = Lexer::getLocForEndOfToken(stmt->getEndLoc(), 0, 
                                                        *source_mgr, context->getLangOpts());
        
        CharSourceRange range = CharSourceRange::getCharRange(start, end);
        return Lexer::getSourceText(range, *source_mgr, context->getLangOpts()).str();
    }
    
    void estimateLoopIterations(ForStmt *stmt, loop_info_t *loop) {
        // Simple estimation for common patterns
        if (BinaryOperator *cond = dyn_cast<BinaryOperator>(stmt->getCond())) {
            if (cond->getOpcode() == BO_LT || cond->getOpcode() == BO_LE) {
                if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(cond->getRHS()->IgnoreParenCasts())) {
                    loop->estimated_iterations = lit->getValue().getZExtValue();
                    if (cond->getOpcode() == BO_LE) {
                        loop->estimated_iterations++;
                    }
                }
            }
        }
    }
    
    bool hasNestedLoops(Stmt *stmt) {
        for (auto child : stmt->children()) {
            if (!child) continue;
            if (isa<ForStmt>(child) || isa<WhileStmt>(child) || isa<DoStmt>(child)) {
                return true;
            }
            if (hasNestedLoops(child)) {
                return true;
            }
        }
        return false;
    }
    
    bool hasFunctionCalls(Stmt *stmt) {
        for (auto child : stmt->children()) {
            if (!child) continue;
            if (isa<CallExpr>(child)) {
                return true;
            }
            if (hasFunctionCalls(child)) {
                return true;
            }
        }
        return false;
    }
};

// AST Consumer
class CacheAnalysisConsumer : public ASTConsumer {
private:
    CachePatternVisitor visitor;
    analysis_results_t *results;
    
public:
    CacheAnalysisConsumer(ASTContext *ctx, analysis_results_t *res) 
        : visitor(ctx), results(res) {
        LOG_DEBUG("Created CacheAnalysisConsumer");
    }
    
    void HandleTranslationUnit(ASTContext &context) override {
        LOG_INFO("Analyzing translation unit");
        visitor.TraverseDecl(context.getTranslationUnitDecl());
        visitor.getResults(results);
    }
};

// Frontend Action
class CacheAnalysisAction : public ASTFrontendAction {
private:
    analysis_results_t *results;
    
public:
    CacheAnalysisAction(analysis_results_t *res) : results(res) {}
    
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
        LOG_INFO("Creating AST consumer for file: %s", file.str().c_str());
        return std::make_unique<CacheAnalysisConsumer>(&CI.getASTContext(), results);
    }
};

// AST Analyzer implementation
struct ast_analyzer {
    std::vector<std::string> include_paths;
    std::vector<std::string> defines;
    std::string std_version;
    
    ast_analyzer() : std_version("c11") {
        LOG_INFO("Created AST analyzer");
    }
};

// C API implementation
extern "C" {

ast_analyzer_t* ast_analyzer_create(void) {
    return new ast_analyzer();
}

void ast_analyzer_destroy(ast_analyzer_t *analyzer) {
    if (analyzer) {
        LOG_INFO("Destroying AST analyzer");
        delete analyzer;
    }
}

int ast_analyzer_add_include_path(ast_analyzer_t *analyzer, const char *path) {
    if (!analyzer || !path) return -1;
    
    analyzer->include_paths.push_back(std::string("-I") + path);
    LOG_DEBUG("Added include path: %s", path);
    return 0;
}

int ast_analyzer_add_define(ast_analyzer_t *analyzer, const char *define) {
    if (!analyzer || !define) return -1;
    
    analyzer->defines.push_back(std::string("-D") + define);
    LOG_DEBUG("Added define: %s", define);
    return 0;
}

int ast_analyzer_set_std(ast_analyzer_t *analyzer, const char *std) {
    if (!analyzer || !std) return -1;
    
    analyzer->std_version = std;
    LOG_DEBUG("Set C standard: %s", std);
    return 0;
}

int ast_analyzer_analyze_file(ast_analyzer_t *analyzer, const char *filename,
                             analysis_results_t *results) {
    if (!analyzer || !filename || !results) return -1;
    
    LOG_INFO("Analyzing file: %s", filename);
    
    // Build compilation arguments
    std::vector<std::string> args;
    args.push_back("clang");
    args.push_back("-std=" + analyzer->std_version);
    args.push_back("-fsyntax-only");
    
    // Add include paths
    for (const auto &inc : analyzer->include_paths) {
        args.push_back(inc);
    }
    
    // Add defines
    for (const auto &def : analyzer->defines) {
        args.push_back(def);
    }
    
    args.push_back(filename);
    
    // Convert to char* array
    std::vector<const char*> argv;
    for (const auto &arg : args) {
        argv.push_back(arg.c_str());
    }
    
    // Create compilation database
    std::string err;
    auto compilation_db = FixedCompilationDatabase::loadFromCommandLine(
        argv.size(), argv.data(), err);
    
    if (!compilation_db) {
        LOG_ERROR("Failed to create compilation database: %s", err.c_str());
        return -1;
    }
    
    // Run the tool
    ClangTool tool(*compilation_db, {filename});
    
    memset(results, 0, sizeof(analysis_results_t));
    
    int ret = tool.run(newFrontendActionFactory<CacheAnalysisAction>(results).get());
    
    if (ret != 0) {
        LOG_ERROR("Failed to analyze file: %s", filename);
        return -1;
    }
    
    LOG_INFO("Analysis complete: %d patterns, %d loops, %d structs found",
             results->pattern_count, results->loop_count, results->struct_count);
    
    return 0;
}

int ast_analyzer_analyze_files(ast_analyzer_t *analyzer, const char **filenames,
                              int file_count, analysis_results_t *results) {
    // For simplicity, analyze each file and combine results
    // In a real implementation, you'd want to analyze them together
    
    memset(results, 0, sizeof(analysis_results_t));
    
    for (int i = 0; i < file_count; i++) {
        analysis_results_t file_results = {};
        
        if (ast_analyzer_analyze_file(analyzer, filenames[i], &file_results) != 0) {
            LOG_ERROR("Failed to analyze file: %s", filenames[i]);
            continue;
        }
        
        // Combine results (simplified - just append)
        // In real implementation, would merge properly
        results->pattern_count += file_results.pattern_count;
        results->loop_count += file_results.loop_count;
        results->struct_count += file_results.struct_count;
    }
    
    return 0;
}

void ast_analyzer_free_results(analysis_results_t *results) {
    if (!results) return;
    
    LOG_DEBUG("Freeing analysis results");
    
    if (results->patterns) {
        delete[] results->patterns;
        results->patterns = nullptr;
    }
    
    if (results->loops) {
        for (int i = 0; i < results->loop_count; i++) {
            if (results->loops[i].patterns) {
                delete[] results->loops[i].patterns;
            }
        }
        delete[] results->loops;
        results->loops = nullptr;
    }
    
    if (results->structs) {
        delete[] results->structs;
        results->structs = nullptr;
    }
    
    if (results->diagnostics) {
        delete[] results->diagnostics;
        results->diagnostics = nullptr;
    }
    
    results->pattern_count = 0;
    results->loop_count = 0;
    results->struct_count = 0;
    results->diagnostic_count = 0;
}

void ast_analyzer_print_results(const analysis_results_t *results) {
    printf("\n=== AST Analysis Results ===\n");
    
    printf("\nAccess Patterns Found: %d\n", results->pattern_count);
    for (int i = 0; i < results->pattern_count && i < 10; i++) {
        const static_pattern_t *p = &results->patterns[i];
        printf("  [%d] %s:%d - %s access to %s (pattern: %s, stride: %d)\n",
               i, p->location.file, p->location.line,
               p->is_struct_access ? "Struct" : "Array",
               p->is_struct_access ? p->struct_name : p->array_name,
               access_pattern_to_string(p->pattern),
               p->stride);
    }
    
    printf("\nLoops Found: %d\n", results->loop_count);
    for (int i = 0; i < results->loop_count && i < 10; i++) {
        const loop_info_t *l = &results->loops[i];
        printf("  [%d] %s:%d - Loop var: %s, depth: %d, est. iterations: %zu\n",
               i, l->location.file, l->location.line,
               l->loop_var, l->nest_level, l->estimated_iterations);
        if (l->has_nested_loops) {
            printf("      Has nested loops\n");
        }
        if (l->pattern_count > 0) {
            printf("      Contains %d access patterns\n", l->pattern_count);
        }
    }
    
    printf("\nStructs Found: %d\n", results->struct_count);
    for (int i = 0; i < results->struct_count && i < 10; i++) {
        const struct_info_t *s = &results->structs[i];
        printf("  [%d] %s - %d fields, %zu bytes total\n",
               i, s->struct_name, s->field_count, s->total_size);
        for (int j = 0; j < s->field_count && j < 5; j++) {
            printf("      %s: offset %zu, size %zu\n",
                   s->field_names[j], s->field_offsets[j], s->field_sizes[j]);
        }
    }
}

const char* get_pattern_description(static_pattern_t *pattern) {
    static char buffer[256];
    
    snprintf(buffer, sizeof(buffer), "%s access pattern with stride %d at depth %d",
             access_pattern_to_string(pattern->pattern),
             pattern->stride,
             pattern->loop_depth);
    
    return buffer;
}

int estimate_cache_footprint(loop_info_t *loop) {
    size_t total_footprint = 0;
    
    for (int i = 0; i < loop->pattern_count; i++) {
        total_footprint += loop->patterns[i].estimated_footprint;
    }
    
    // Multiply by estimated iterations if reasonable
    if (loop->estimated_iterations > 0 && loop->estimated_iterations < 1000000) {
        total_footprint *= loop->estimated_iterations;
    }
    
    return total_footprint;
}

} // extern "C"
