#include "typecheck.h"
#include "../ast/ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ** Internal Helpers **

static void tc_error(TypeChecker *tc, Token t, const char *msg) {
  fprintf(stderr, "Type Error at %s:%d:%d: %s\n", g_current_filename, t.line,
          t.col, msg);
  tc->error_count++;
}

static void tc_enter_scope(TypeChecker *tc) {
  Scope *s = malloc(sizeof(Scope));
  s->symbols = NULL;
  s->parent = tc->current_scope;
  tc->current_scope = s;
}

static void tc_exit_scope(TypeChecker *tc) {
  if (!tc->current_scope) {
    return;
  }
  Scope *old = tc->current_scope;
  tc->current_scope = old->parent;

  Symbol *sym = old->symbols;
  while (sym) {
    Symbol *next = sym->next;
    free(sym);
    sym = next;
  }
  free(old);
}

static void tc_add_symbol(TypeChecker *tc, const char *name, Type *type,
                          Token t) {
  Symbol *s = malloc(sizeof(Symbol));
  memset(s, 0, sizeof(Symbol));
  s->name = strdup(name);
  s->type_info = type;
  s->decl_token = t;
  s->next = tc->current_scope->symbols;
  tc->current_scope->symbols = s;
}

static Symbol *tc_lookup(TypeChecker *tc, const char *name) {
  Scope *s = tc->current_scope;
  while (s) {
    Symbol *curr = s->symbols;
    while (curr) {
      if (0 == strcmp(curr->name, name)) {
        return curr;
      }
      curr = curr->next;
    }
    s = s->parent;
  }
  return NULL;
}

static int is_integer_type(Type *t) {
  if (!t)
    return 0;
  // Check all integer kinds defined in TypeKind
  return (t->kind >= TYPE_I8 && t->kind <= TYPE_U128) || t->kind == TYPE_INT ||
         t->kind == TYPE_UINT || t->kind == TYPE_USIZE || t->kind == TYPE_ISIZE;
}

static int is_signed_integer(Type *t) {
  if (!t)
    return 0;
  return (t->kind == TYPE_I8 || t->kind == TYPE_I16 || t->kind == TYPE_I32 ||
          t->kind == TYPE_I64 || t->kind == TYPE_I128 || t->kind == TYPE_INT ||
          t->kind == TYPE_ISIZE);
}

// Check if node is a literal integer
// This allows '0' to be used with unsigned types without casting.
static int is_safe_integer_literal(ASTNode *node) {
  if (!node || node->type != NODE_EXPR_LITERAL)
    return 0;

  // In Zen C, string/char literals have string_val set.
  if (node->literal.string_val != NULL)
    return 0;

  // In codegen.c, type_kind == 1 represents floats
  if (node->literal.type_kind == 1)
    return 0;

  // If it's not a string, char, or float, it's an integer literal.
  // Parsed integer literals (e.g. 123) are positive.
  // Negative numbers are handled as UnaryOp(-, Literal(123)), so this
  // safe check correctly returns false for negative literals.
  return 1;
}

// ** Node Checkers **

static void check_node(TypeChecker *tc, ASTNode *node);

static void check_block(TypeChecker *tc, ASTNode *block) {
  tc_enter_scope(tc);
  ASTNode *stmt = block->block.statements;
  while (stmt) {
    check_node(tc, stmt);
    stmt = stmt->next;
  }
  tc_exit_scope(tc);
}

static int check_type_compatibility(TypeChecker *tc, Type *target,
                                    ASTNode *value_expr, Token t) {
  if (!target || !value_expr)
    return 1;

  Type *value_type = value_expr->type_info;
  if (!value_type)
    return 1; // Can't check yet

  // 1. Exact Match
  if (type_eq(target, value_type))
    return 1;

  // 2. Void Pointer Generics (void* matches any pointer)
  if (target->kind == TYPE_POINTER && target->inner->kind == TYPE_VOID)
    return 1;
  if (value_type->kind == TYPE_POINTER && value_type->inner->kind == TYPE_VOID)
    return 1;

  // 3. Integer Promotion / Safety Checks
  if (is_integer_type(target) && is_integer_type(value_type)) {
    int target_signed = is_signed_integer(target);
    int value_signed = is_signed_integer(value_type);

    // A. Signed/Unsigned Mismatch
    if (target_signed != value_signed) {
      // ERGONOMIC FIX: Allow implicit conversion IF it is a safe positive
      // literal e.g. 'usize x = 0;' or 'if (len > 0)'
      if (is_safe_integer_literal(value_expr)) {
        return 1; // Safe!
      }

      // Otherwise, warn strict
      char *t_str = type_to_string(target);
      char *v_str = type_to_string(value_type);
      char msg[256];
      snprintf(msg, 255,
               "Sign mismatch: cannot implicitly convert '%s' to '%s' (use "
               "cast or unsigned literal)",
               v_str, t_str);
      tc_error(tc, t, msg);
      free(t_str);
      free(v_str);
      return 0;
    }

    // B. Size truncation could be checked here (optional)
    return 1;
  }

  // 4. Default Failure
  char *t_str = type_to_string(target);
  char *v_str = type_to_string(value_type);
  char msg[256];
  snprintf(msg, 255, "Type mismatch: expected '%s', got '%s'", t_str, v_str);
  tc_error(tc, t, msg);
  free(t_str);
  free(v_str);
  return 0;
}

static void check_var_decl(TypeChecker *tc, ASTNode *node) {
  if (node->var_decl.init_expr) {
    check_node(tc, node->var_decl.init_expr);

    Type *decl_type = node->type_info;

    if (decl_type) {
      // Pass the full expression node to allow literal checking
      check_type_compatibility(tc, decl_type, node->var_decl.init_expr,
                               node->token);
    }
  }

  Type *t = node->type_info;
  if (!t && node->var_decl.init_expr) {
    t = node->var_decl.init_expr->type_info;
  }

  tc_add_symbol(tc, node->var_decl.name, t, node->token);
  node->type_info = t;
}

static void check_function(TypeChecker *tc, ASTNode *node) {
  (void)tc_error;

  tc->current_func = node;
  tc_enter_scope(tc);

  for (int i = 0; i < node->func.arg_count; i++) {
    if (node->func.param_names && node->func.param_names[i]) {
      // FIXED: use arg_types from struct, not param_types
      Type *t = (node->func.arg_types) ? node->func.arg_types[i] : NULL;
      tc_add_symbol(tc, node->func.param_names[i], t, (Token){0});
    }
  }

  check_node(tc, node->func.body);

  tc_exit_scope(tc);
  tc->current_func = NULL;
}

static void check_expr_var(TypeChecker *tc, ASTNode *node) {
  Symbol *sym = tc_lookup(tc, node->var_ref.name);
  if (sym && sym->type_info) {
    node->type_info = sym->type_info;
  }
}

static void check_node(TypeChecker *tc, ASTNode *node) {
  if (!node) {
    return;
  }

  switch (node->type) {
  case NODE_ROOT:
    check_node(tc, node->root.children);
    break;
  case NODE_BLOCK:
    check_block(tc, node);
    break;
  case NODE_VAR_DECL:
    check_var_decl(tc, node);
    break;
  case NODE_FUNCTION:
    check_function(tc, node);
    break;
  case NODE_EXPR_VAR:
    check_expr_var(tc, node);
    break;
  case NODE_RETURN:
    if (node->ret.value) {
      check_node(tc, node->ret.value);
      // Check return type matches function signature
      if (tc->current_func && tc->current_func->type_info) {
        check_type_compatibility(tc, tc->current_func->type_info,
                                 node->ret.value, node->token);
      }
    }
    break;

  case NODE_IF:
    check_node(tc, node->if_stmt.condition);
    check_node(tc, node->if_stmt.then_body);
    check_node(tc, node->if_stmt.else_body);
    break;
  case NODE_WHILE:
    check_node(tc, node->while_stmt.condition);
    check_node(tc, node->while_stmt.body);
    break;
  case NODE_FOR:
    tc_enter_scope(tc);
    check_node(tc, node->for_stmt.init);
    check_node(tc, node->for_stmt.condition);
    check_node(tc, node->for_stmt.step);
    check_node(tc, node->for_stmt.body);
    tc_exit_scope(tc);
    break;

  case NODE_EXPR_BINARY:
    check_node(tc, node->binary.left);
    check_node(tc, node->binary.right);

    // Infer type from left operand
    if (node->binary.left->type_info) {
      node->type_info = node->binary.left->type_info;

      // Perform the comparison check using the robust compatibility logic
      // This will trigger is_safe_integer_literal for cases like (usize > 0)
      check_type_compatibility(tc, node->binary.left->type_info,
                               node->binary.right, node->token);
    }
    break;

  case NODE_EXPR_CALL:
    check_node(tc, node->call.callee);
    check_node(tc, node->call.args);
    // Propagate return type
    if (node->call.callee->type_info) {
      node->type_info = node->call.callee->type_info;
    }
    break;

  case NODE_EXPR_LITERAL:
    // Literals usually have their type set during parsing/inference phases
    break;

  default:
    break;
  }

  if (node->next) {
    check_node(tc, node->next);
  }
}

// ** Entry Point **

int check_program(ParserContext *ctx, ASTNode *root) {
  TypeChecker tc = {0};
  tc.pctx = ctx;

  printf("[TypeCheck] Starting semantic analysis...\n");
  check_node(&tc, root);

  if (tc.error_count > 0) {
    printf("[TypeCheck] Found %d errors.\n", tc.error_count);
    return 1;
  }
  printf("[TypeCheck] Passed.\n");
  return 0;
}
