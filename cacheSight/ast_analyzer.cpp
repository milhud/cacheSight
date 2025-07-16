#include "ast_analyzer.h"
#include "common.h"

#include <clang/AST/RecordLayout.h>
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
#include <cstring>

using namespace clang;
using namespace clang::tooling;

// Internal C++ structures
struct LoopContext {
    const ForStmt *stmt;
    int depth;
    std::string var_name;
    int stride;
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

    // Add these helper functions to the CachePatternVisitor class

    bool isOuterLoopVariable(const std::string &varName) {
        if (varName.empty() || loop_stack.size() < 2) return false;
        
        // Check all loops except the innermost
        for (size_t i = 0; i < loop_stack.size() - 1; i++) {
            if (loop_stack[i]->stmt && loop_stack[i]->stmt->getInit()) {
                if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(loop_stack[i]->stmt->getInit())) {
                    if (declStmt->isSingleDecl()) {
                        if (const VarDecl *varDecl = dyn_cast<VarDecl>(declStmt->getSingleDecl())) {
                            if (varDecl->getNameAsString() == varName) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    int calculateStrideForOuterLoop(const std::string &varName) {
        // This is a heuristic - in reality we'd need to analyze the array dimensions
        // For now, assume outer loop has stride = inner loop size
        // This would need more sophisticated analysis of array declarations
        return 1024; // Default assumption for matrix-like access
    }

    bool isColumnMajorAccess(ArraySubscriptExpr *expr) {
        if (loop_stack.size() < 2) return false;
        
        // Check if this is accessing a 2D array with reversed indices
        // This is a simplified check - full implementation would trace the full expression
        Expr *base = expr->getBase()->IgnoreParenCasts();
        if (ArraySubscriptExpr *outerArray = dyn_cast<ArraySubscriptExpr>(base)) {
            // We have array[x][y] - check if x and y are reversed loop variables
            std::string inner_loop_var, outer_loop_var;
            
            if (loop_stack.size() >= 2) {
                // Get loop variables
                auto getLoopVar = [](const ForStmt *stmt) -> std::string {
                    if (stmt && stmt->getInit()) {
                        if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(stmt->getInit())) {
                            if (declStmt->isSingleDecl()) {
                                if (const VarDecl *varDecl = dyn_cast<VarDecl>(declStmt->getSingleDecl())) {
                                    return varDecl->getNameAsString();
                                }
                            }
                        }
                    }
                    return "";
                };
                
                inner_loop_var = getLoopVar(loop_stack.back()->stmt);
                outer_loop_var = getLoopVar(loop_stack[loop_stack.size()-2]->stmt);
                
                // Check if inner loop variable is used for outer array dimension
                if (DeclRefExpr *outerIdx = dyn_cast<DeclRefExpr>(outerArray->getIdx()->IgnoreParenCasts())) {
                    if (DeclRefExpr *innerIdx = dyn_cast<DeclRefExpr>(expr->getIdx()->IgnoreParenCasts())) {
                        if (outerIdx->getNameInfo().getAsString() == inner_loop_var &&
                            innerIdx->getNameInfo().getAsString() == outer_loop_var) {
                            return true; // Column-major access pattern detected
                        }
                    }
                }
            }
        }
        return false;
    }

    int getMatrixRowSize(ArraySubscriptExpr *expr) {
        // This would need to look up the array declaration to get actual size
        // For now, return a default
        return 1024;
    }

    void analyzeMultiDimArray(ArraySubscriptExpr *expr, static_pattern_t *pattern) {
        // Walk up the chain to get the base array name
        Expr *current = expr;
        while (ArraySubscriptExpr *nested = dyn_cast<ArraySubscriptExpr>(current->IgnoreParenCasts())) {
            current = nested->getBase();
        }
        
        if (DeclRefExpr *baseRef = dyn_cast<DeclRefExpr>(current->IgnoreParenCasts())) {
            strncpy(pattern->array_name, baseRef->getNameInfo().getAsString().c_str(),
                    sizeof(pattern->array_name) - 1);
        }
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

        // 3. ADD: Function to consolidate multiple array accesses into one pattern
    static_pattern_t consolidateLoopPatterns(const std::vector<static_pattern_t> &loop_patterns) {
        static_pattern_t master = {};
        
        if (loop_patterns.empty()) return master;
        
        // Use first pattern as base
        master = loop_patterns[0];
        
        // Analyze all patterns to determine the dominant access pattern
        bool has_strided = false;
        bool has_sequential = false;
        int max_stride = 1;
        
        for (const auto &p : loop_patterns) {
            if (p.pattern == STRIDED) {
                has_strided = true;
                max_stride = std::max(max_stride, p.stride);
            } else if (p.pattern == SEQUENTIAL) {
                has_sequential = true;
            }
        }
        
        // Determine master pattern based on analysis
        if (has_strided && max_stride > 8) {
            master.pattern = STRIDED;
            master.stride = max_stride;
            snprintf(master.array_name, sizeof(master.array_name), 
                    "MatrixLoop_%d", master.location.line);
        } else if (has_sequential) {
            master.pattern = SEQUENTIAL;
            master.stride = 1;
        }
        
        master.loop_depth = loop_patterns.size();  // Number of accesses in loop
        return master;
    }
        
    // Post-visit for loops
    bool dataTraverseStmtPost(Stmt *stmt) {
        if (isa<ForStmt>(stmt) && !loop_stack.empty()) {
            current_loop_depth--;
            
            LoopContext *ctx = loop_stack.back();
            if (!loops.empty()) {
                loop_info_t &loop = loops.back();
                loop.pattern_count = ctx->patterns.size();
                
                // CRITICAL: Create ONE consolidated pattern for the entire loop
                if (loop.pattern_count > 0) {
                    // Analyze the loop's access patterns and create ONE master pattern
                    static_pattern_t master_pattern = consolidateLoopPatterns(ctx->patterns);
                    
                    // Add the master pattern to main patterns array
                    patterns.push_back(master_pattern);
                    
                    // Also store individual patterns in loop for detailed analysis
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
        pattern->stride = 0;
        pattern->pattern = SEQUENTIAL; // Default to sequential
        
        // Get array name and base information
        Expr *base = expr->getBase()->IgnoreParenCasts();
        if (DeclRefExpr *declRef = dyn_cast<DeclRefExpr>(base)) {
            strncpy(pattern->array_name, declRef->getNameInfo().getAsString().c_str(),
                    sizeof(pattern->array_name) - 1);
                    
            // Check if it's a pointer (for dynamic arrays)
            QualType baseType = declRef->getType();
            pattern->is_pointer_access = baseType->isPointerType();
        } else if (ArraySubscriptExpr *nestedArray = dyn_cast<ArraySubscriptExpr>(base)) {
            // This is a multi-dimensional array access like matrix[i][j]
            // Get the base array name from the nested expression
            analyzeMultiDimArray(nestedArray, pattern);
        } else if (MemberExpr *memberExpr = dyn_cast<MemberExpr>(base)) {
            // Structure member access
            if (FieldDecl *field = dyn_cast<FieldDecl>(memberExpr->getMemberDecl())) {
                strncpy(pattern->array_name, field->getNameAsString().c_str(),
                        sizeof(pattern->array_name) - 1);
                pattern->is_struct_access = true;
            }
        }
        
        // Get loop context information
        std::string loop_var;
        const ForStmt *innermost_loop = nullptr;
        if (!loop_stack.empty()) {
            LoopContext *loop_ctx = loop_stack.back();
            innermost_loop = loop_ctx->stmt;
            
            // Extract loop variable name
            if (innermost_loop && innermost_loop->getInit()) {
                if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(innermost_loop->getInit())) {
                    if (declStmt->isSingleDecl()) {
                        if (const VarDecl *varDecl = dyn_cast<VarDecl>(declStmt->getSingleDecl())) {
                            loop_var = varDecl->getNameAsString();
                            // Store loop variable name in the loop context, not the pattern
                            loop_ctx->var_name = loop_var;
                        }
                    }
                }
            }
        }
        
        // Analyze the index expression - this is the critical part
        Expr *index = expr->getIdx()->IgnoreParenCasts();
        
        // Case 1: Direct loop variable access (e.g., arr[i])
        if (DeclRefExpr *indexVar = dyn_cast<DeclRefExpr>(index)) {
            std::string varName = indexVar->getNameInfo().getAsString();
            strncpy(pattern->variable_name, varName.c_str(), sizeof(pattern->variable_name) - 1);
            
            // In analyzeArrayAccess, when checking for direct loop variable:
            if (!loop_var.empty() && varName == loop_var) {
                // Check the loop's stride
                int loop_stride = 1;
                if (!loop_stack.empty() && loop_stack.back()->stride > 0) {
                    loop_stride = loop_stack.back()->stride;
                }
                
                if (loop_stride > 1) {
                    pattern->pattern = STRIDED;
                    pattern->stride = loop_stride;
                } else {
                    pattern->pattern = SEQUENTIAL;
                    pattern->stride = 1;
                }
            }
        }
        // Case 2: Binary operation (e.g., arr[i+1], arr[i*8])
        else if (BinaryOperator *binOp = dyn_cast<BinaryOperator>(index)) {
            analyzeBinaryIndexExpr(binOp, pattern, loop_var);
        }
        // Case 3: Array subscript in index (e.g., arr[indices[i]])
        else if (ArraySubscriptExpr *nestedArray = dyn_cast<ArraySubscriptExpr>(index)) {
            pattern->pattern = INDIRECT_ACCESS;
            pattern->is_indirect_index = true;
            // Extract the actual index variable if possible
            if (DeclRefExpr *nestedIndex = dyn_cast<DeclRefExpr>(nestedArray->getIdx()->IgnoreParenCasts())) {
                strncpy(pattern->variable_name, nestedIndex->getNameInfo().getAsString().c_str(),
                        sizeof(pattern->variable_name) - 1);
            }
        }
        // Case 4: Function call in index (e.g., arr[rand()])
        else if (CallExpr *call = dyn_cast<CallExpr>(index)) {
            pattern->pattern = RANDOM;
            pattern->stride = 0;
            
            // Check if it's a known random function
            if (FunctionDecl *func = call->getDirectCallee()) {
                std::string funcName = func->getNameAsString();
                if (funcName == "rand" || funcName == "random") {
                    strncpy(pattern->variable_name, "rand()", sizeof(pattern->variable_name) - 1);
                }
            }
        }
        // Case 5: Unary operation (e.g., arr[*ptr])
        else if (UnaryOperator *unaryOp = dyn_cast<UnaryOperator>(index)) {
            if (unaryOp->getOpcode() == UO_Deref) {
                pattern->pattern = INDIRECT_ACCESS;
                pattern->is_indirect_index = true;
            }
        }
        // Case 6: Integer literal (e.g., arr[0])
        else if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(index)) {
            pattern->pattern = SEQUENTIAL; // Constant index
            pattern->stride = 0;
            snprintf(pattern->variable_name, sizeof(pattern->variable_name), "%lld", 
                    lit->getValue().getSExtValue());
        }
        // Default case
        else {
            pattern->pattern = RANDOM;
            pattern->stride = 0;
        }
        
        // Special case: Check if we're in a nested loop accessing a 2D array incorrectly
        if (loop_stack.size() >= 2 && pattern->pattern == SEQUENTIAL) {
            // Check if this is column-major access in row-major layout
            if (isColumnMajorAccess(expr)) {
                pattern->pattern = NESTED_LOOP;
                pattern->stride = getMatrixRowSize(expr);
            }
        }
        
        // Don't override patterns based on pointer type - that was the bug!
        // A pointer can still have sequential access pattern
        
        LOG_DEBUG("=== PATTERN ANALYSIS RESULT ===");
        LOG_DEBUG("Location: %s:%d", pattern->location.file, pattern->location.line);
        LOG_DEBUG("Array: %s[%s]", pattern->array_name, pattern->variable_name);
        LOG_DEBUG("Pattern: %s (stride: %d)", 
                access_pattern_to_string(pattern->pattern), pattern->stride);
        LOG_DEBUG("Loop variable: %s, Loop depth: %d", loop_var.c_str(), pattern->loop_depth);
        LOG_DEBUG("Is pointer: %s, Is indirect: %s", 
                pattern->is_pointer_access ? "yes" : "no",
                pattern->is_indirect_index ? "yes" : "no");
        LOG_DEBUG("=== END ANALYSIS ===\n");
    }
    
    void analyzeBinaryIndexExpr(BinaryOperator *binop, static_pattern_t *pattern, 
                            const std::string &loop_var) {
        Expr *lhs = binop->getLHS()->IgnoreParenCasts();
        Expr *rhs = binop->getRHS()->IgnoreParenCasts();
        
        // Identify which side has the loop variable (if any)
        bool lhs_is_loop_var = false;
        bool rhs_is_loop_var = false;
        std::string lhs_var_name, rhs_var_name;
        
        if (DeclRefExpr *lhsVar = dyn_cast<DeclRefExpr>(lhs)) {
            lhs_var_name = lhsVar->getNameInfo().getAsString();
            lhs_is_loop_var = (!loop_var.empty() && lhs_var_name == loop_var);
        }
        if (DeclRefExpr *rhsVar = dyn_cast<DeclRefExpr>(rhs)) {
            rhs_var_name = rhsVar->getNameInfo().getAsString();
            rhs_is_loop_var = (!loop_var.empty() && rhs_var_name == loop_var);
        }
        
        // Get the primary variable name for the pattern
        if (lhs_is_loop_var) {
            strncpy(pattern->variable_name, lhs_var_name.c_str(), sizeof(pattern->variable_name) - 1);
        } else if (rhs_is_loop_var) {
            strncpy(pattern->variable_name, rhs_var_name.c_str(), sizeof(pattern->variable_name) - 1);
        } else if (!lhs_var_name.empty()) {
            strncpy(pattern->variable_name, lhs_var_name.c_str(), sizeof(pattern->variable_name) - 1);
        } else if (!rhs_var_name.empty()) {
            strncpy(pattern->variable_name, rhs_var_name.c_str(), sizeof(pattern->variable_name) - 1);
        }
        
        // Analyze based on operator type
        switch (binop->getOpcode()) {
            case BO_Add:
            case BO_Sub: {
                // Addition/Subtraction: i+1, i-1, i+k
                if (lhs_is_loop_var || rhs_is_loop_var) {
                    // Get the offset
                    Expr *offsetExpr = lhs_is_loop_var ? rhs : lhs;
                    
                    if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(offsetExpr)) {
                        int64_t offset = lit->getValue().getSExtValue();
                        
                        if (binop->getOpcode() == BO_Sub && lhs_is_loop_var) {
                            offset = -offset;
                        } else if (binop->getOpcode() == BO_Sub && rhs_is_loop_var) {
                            // k - i is unusual but possible
                            pattern->pattern = RANDOM;
                            return;
                        }
                        
                        // Check for specific patterns
                        if (offset == -1) {
                            pattern->pattern = ACCESS_LOOP_CARRIED_DEP;
                            pattern->stride = -1;
                            pattern->has_dependencies = true;
                        } else if (abs(offset) <= 1) {
                            pattern->pattern = SEQUENTIAL;
                            pattern->stride = 1;
                        } else {
                            pattern->pattern = STRIDED;
                            pattern->stride = abs(offset);
                        }
                    } else if (DeclRefExpr *varRef = dyn_cast<DeclRefExpr>(offsetExpr)) {
                        // i + j or similar - could be strided if j is constant in loop
                        pattern->pattern = STRIDED;
                        pattern->stride = 0; // Unknown at compile time
                    } else {
                        pattern->pattern = RANDOM;
                    }
                } else if (isOuterLoopVariable(lhs_var_name) || isOuterLoopVariable(rhs_var_name)) {
                    // Outer loop variable arithmetic
                    pattern->pattern = STRIDED;
                    pattern->stride = calculateStrideForOuterLoop(lhs_var_name.empty() ? rhs_var_name : lhs_var_name);
                } else {
                    pattern->pattern = RANDOM;
                }
                break;
            }
            
            case BO_Mul: {
                // Multiplication: i*8, 8*i
                if (lhs_is_loop_var || rhs_is_loop_var) {
                    Expr *factorExpr = lhs_is_loop_var ? rhs : lhs;
                    
                    if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(factorExpr)) {
                        pattern->pattern = STRIDED;
                        pattern->stride = lit->getValue().getSExtValue();
                    } else {
                        // i * j - stride depends on j
                        pattern->pattern = STRIDED;
                        pattern->stride = 0; // Unknown at compile time
                    }
                } else {
                    pattern->pattern = RANDOM;
                }
                break;
            }
            
            case BO_Div:
            case BO_Rem: {
                // Division/Modulo: i/2, i%8
                if (lhs_is_loop_var) {
                    if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(rhs)) {
                        if (binop->getOpcode() == BO_Div) {
                            // i/k - this creates a pattern where multiple loop iterations
                            // access the same element
                            pattern->pattern = GATHER_SCATTER;
                            pattern->stride = 0;
                        } else {
                            // i%k - cyclic pattern
                            pattern->pattern = STRIDED;
                            pattern->stride = 1; // But with wrap-around
                        }
                    } else {
                        pattern->pattern = RANDOM;
                    }
                } else {
                    pattern->pattern = RANDOM;
                }
                break;
            }
            
            case BO_Shl:
            case BO_Shr: {
                // Bit shift: i<<2, i>>1
                if (lhs_is_loop_var) {
                    if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(rhs)) {
                        int shift = lit->getValue().getSExtValue();
                        if (binop->getOpcode() == BO_Shl) {
                            pattern->pattern = STRIDED;
                            pattern->stride = 1 << shift; // 2^shift
                        } else {
                            pattern->pattern = GATHER_SCATTER;
                            pattern->stride = 0;
                        }
                    } else {
                        pattern->pattern = RANDOM;
                    }
                } else {
                    pattern->pattern = RANDOM;
                }
                break;
            }
            
            default:
                // Other operators (AND, OR, XOR, etc.) typically create random patterns
                pattern->pattern = RANDOM;
                pattern->stride = 0;
                break;
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
        
        // CRITICAL: Analyze the increment to detect stride
        if (stmt->getInc()) {
            std::string inc_str = getSourceText(stmt->getInc());
            strncpy(loop->increment_expr, inc_str.c_str(), sizeof(loop->increment_expr) - 1);
            
            // Check for strided increments (i += k where k > 1)
            if (BinaryOperator *incOp = dyn_cast<BinaryOperator>(stmt->getInc())) {
                if (incOp->isCompoundAssignmentOp() || incOp->getOpcode() == BO_Assign) {
                    // For i += k or i = i + k
                    Expr *rhs = incOp->getRHS();
                    if (BinaryOperator *addOp = dyn_cast<BinaryOperator>(rhs)) {
                        if (addOp->getOpcode() == BO_Add) {
                            // Check for i = i + k pattern
                            if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(addOp->getRHS())) {
                                int stride = lit->getValue().getSExtValue();
                                if (stride > 1) {
                                    // Store this stride information in the loop context
                                    if (!loop_stack.empty()) {
                                        loop_stack.back()->stride = stride;
                                    }
                                }
                            }
                        }
                    }
                    // Direct compound assignment: i += k
                    else if (incOp->getOpcode() == BO_AddAssign) {
                        if (IntegerLiteral *lit = dyn_cast<IntegerLiteral>(incOp->getRHS())) {
                            int stride = lit->getValue().getSExtValue();
                            if (stride > 1) {
                                if (!loop_stack.empty()) {
                                    loop_stack.back()->stride = stride;
                                }
                            }
                        }
                    }
                }
            }
            // Pre/post increment
            else if (UnaryOperator *unaryOp = dyn_cast<UnaryOperator>(stmt->getInc())) {
                // ++i or i++ means stride = 1
                if (!loop_stack.empty()) {
                    loop_stack.back()->stride = 1;
                }
            }
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

// Add these helper functions to the CachePatternVisitor class
/*
bool isOuterLoopVariable(const std::string &varName) {
    if (varName.empty() || loop_stack.size() < 2) return false;
    
    // Check all loops except the innermost
    for (size_t i = 0; i < loop_stack.size() - 1; i++) {
        if (loop_stack[i]->stmt && loop_stack[i]->stmt->getInit()) {
            if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(loop_stack[i]->stmt->getInit())) {
                if (declStmt->isSingleDecl()) {
                    if (const VarDecl *varDecl = dyn_cast<VarDecl>(declStmt->getSingleDecl())) {
                        if (varDecl->getNameAsString() == varName) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

int calculateStrideForOuterLoop(const std::string &varName) {
    // This is a heuristic - in reality we'd need to analyze the array dimensions
    // For now, assume outer loop has stride = inner loop size
    // This would need more sophisticated analysis of array declarations
    return 1024; // Default assumption for matrix-like access
}

bool isColumnMajorAccess(ArraySubscriptExpr *expr) {
    if (loop_stack.size() < 2) return false;
    
    // Check if this is accessing a 2D array with reversed indices
    // This is a simplified check - full implementation would trace the full expression
    Expr *base = expr->getBase()->IgnoreParenCasts();
    if (ArraySubscriptExpr *outerArray = dyn_cast<ArraySubscriptExpr>(base)) {
        // We have array[x][y] - check if x and y are reversed loop variables
        std::string inner_loop_var, outer_loop_var;
        
        if (loop_stack.size() >= 2) {
            // Get loop variables
            auto getLoopVar = [](const ForStmt *stmt) -> std::string {
                if (stmt && stmt->getInit()) {
                    if (const DeclStmt *declStmt = dyn_cast<DeclStmt>(stmt->getInit())) {
                        if (declStmt->isSingleDecl()) {
                            if (const VarDecl *varDecl = dyn_cast<VarDecl>(declStmt->getSingleDecl())) {
                                return varDecl->getNameAsString();
                            }
                        }
                    }
                }
                return "";
            };
            
            inner_loop_var = getLoopVar(loop_stack.back()->stmt);
            outer_loop_var = getLoopVar(loop_stack[loop_stack.size()-2]->stmt);
            
            // Check if inner loop variable is used for outer array dimension
            if (DeclRefExpr *outerIdx = dyn_cast<DeclRefExpr>(outerArray->getIdx()->IgnoreParenCasts())) {
                if (DeclRefExpr *innerIdx = dyn_cast<DeclRefExpr>(expr->getIdx()->IgnoreParenCasts())) {
                    if (outerIdx->getNameInfo().getAsString() == inner_loop_var &&
                        innerIdx->getNameInfo().getAsString() == outer_loop_var) {
                        return true; // Column-major access pattern detected
                    }
                }
            }
        }
    }
    return false;
}

int getMatrixRowSize(ArraySubscriptExpr *expr) {
    // This would need to look up the array declaration to get actual size
    // For now, return a default
    return 1024;
}

void analyzeMultiDimArray(ArraySubscriptExpr *expr, static_pattern_t *pattern) {
    // Walk up the chain to get the base array name
    Expr *current = expr;
    while (ArraySubscriptExpr *nested = dyn_cast<ArraySubscriptExpr>(current->IgnoreParenCasts())) {
        current = nested->getBase();
    }
    
    if (DeclRefExpr *baseRef = dyn_cast<DeclRefExpr>(current->IgnoreParenCasts())) {
        strncpy(pattern->array_name, baseRef->getNameInfo().getAsString().c_str(),
                sizeof(pattern->array_name) - 1);
    }
}

*/

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
    
    // Initialize results
    memset(results, 0, sizeof(analysis_results_t));

    // Create compilation database
    std::vector<const char*> argv;
    argv.push_back("cache_optimizer");
    argv.push_back(filename);  // Changed from 'file' to 'filename'
    argv.push_back("--");
    argv.push_back("-std=c11");

    // Add include paths
    for (const auto &inc : analyzer->include_paths) {  // Use the vector directly
        argv.push_back(inc.c_str());
    }

    std::string err;
    int argc = static_cast<int>(argv.size());
    std::unique_ptr<CompilationDatabase> compilations(
        FixedCompilationDatabase::loadFromCommandLine(
            argc, const_cast<char**>(argv.data()), err));  // Note: need const_cast

    if (!compilations) {
        LOG_ERROR("Failed to create compilation database: %s", err.c_str());
        return -1;
    }

    // Create and run tool
    std::vector<std::string> source_paths;
    source_paths.push_back(filename);
    ClangTool tool(*compilations, source_paths);  // Pass vector of strings

    // Create a custom factory that passes results to each action
    class CacheAnalysisActionFactory : public FrontendActionFactory {
        analysis_results_t *results;
    public:
        CacheAnalysisActionFactory(analysis_results_t *r) : results(r) {}
        
        std::unique_ptr<FrontendAction> create() override {
            return std::make_unique<CacheAnalysisAction>(results);
        }
    };

    CacheAnalysisActionFactory factory(results);
    int ret = tool.run(&factory);
    
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
    memset(results, 0, sizeof(analysis_results_t));
    
    // First pass: count total items
    int total_patterns = 0;
    int total_loops = 0;
    int total_structs = 0;
    
    std::vector<analysis_results_t> file_results_vec;
    
    for (int i = 0; i < file_count; i++) {
        analysis_results_t file_results = {};
        
        if (ast_analyzer_analyze_file(analyzer, filenames[i], &file_results) != 0) {
            LOG_ERROR("Failed to analyze file: %s", filenames[i]);
            continue;
        }
        
        total_patterns += file_results.pattern_count;
        total_loops += file_results.loop_count;
        total_structs += file_results.struct_count;
        
        file_results_vec.push_back(file_results);
    }
    
    // Allocate arrays
    if (total_patterns > 0) {
        results->patterns = new static_pattern_t[total_patterns];
    }
    if (total_loops > 0) {
        results->loops = new loop_info_t[total_loops];
    }
    if (total_structs > 0) {
        results->structs = new struct_info_t[total_structs];
    }
    
    // Second pass: copy data
    int pattern_offset = 0;
    int loop_offset = 0;
    int struct_offset = 0;
    
    for (const auto& file_results : file_results_vec) {
        // Copy patterns
        if (file_results.patterns && file_results.pattern_count > 0) {
            std::copy(file_results.patterns, 
                     file_results.patterns + file_results.pattern_count,
                     results->patterns + pattern_offset);
            pattern_offset += file_results.pattern_count;
        }
        
        // Copy loops
        if (file_results.loops && file_results.loop_count > 0) {
            std::copy(file_results.loops,
                     file_results.loops + file_results.loop_count,
                     results->loops + loop_offset);
            loop_offset += file_results.loop_count;
        }
        
        // Copy structs
        if (file_results.structs && file_results.struct_count > 0) {
            std::copy(file_results.structs,
                     file_results.structs + file_results.struct_count,
                     results->structs + struct_offset);
            struct_offset += file_results.struct_count;
        }
        
        // Free the file results
        //ast_analyzer_free_results(const_cast<analysis_results_t*>(&file_results));
    }
    
    results->pattern_count = pattern_offset;
    results->loop_count = loop_offset;
    results->struct_count = struct_offset;
    
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
