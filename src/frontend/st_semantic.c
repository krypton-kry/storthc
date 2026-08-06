#include "st_semantic.h"
#include "st_lexer.h"

static const char *ST_builtin_fns[] = {"typeof"};
static ST_tys_t ST_builtin_rets;

#define ST_MAX_FN_INSTANCE 1024

static ST_ht_generic_t ST_name_key(ST_string_t name) {
    return (ST_ht_generic_t){.tag = name.data, .size = name.len};
}

static void ST_sym_insert(ST_sema_t *se, ST_ht_t *ht, ST_sym_t *sym) {
    ST_ht_generic_t *key = ST_arena_push(se->arena, sizeof(*key));
    key->tag = sym->name.data;
    key->size = sym->name.len;
    ST_ht_set(ht, key, (ST_ht_generic_t){.tag = sym, .size = sizeof(*sym)});
}

static ST_sym_t *ST_sym_find_in(ST_ht_t *ht, ST_string_t name) {
    return (ST_sym_t *)ST_ht_get(ht, ST_name_key(name)).tag;
}

static ST_sym_t *ST_sym_find(ST_sema_t *se, ST_string_t name) {
    for (ST_scope_t *s = se->scope; s; s = s->parent) {
        ST_sym_t *sym = ST_sym_find_in(&s->table, name);
        if (sym)
            return sym;
    }
    return ST_sym_find_in(&se->globals, name);
}

static ST_sym_t *ST_sym_new(ST_sema_t *se, ST_sym_kind_t kind, ST_string_t name, ST_decl_t *decl,
                            ST_ty_t *t, u32 line, u32 col) {
    ST_sym_t *sym = ST_arena_push_zeroed(se->arena, sizeof(*sym));
    sym->kind = kind;
    sym->name = name;
    sym->decl = decl;
    sym->t = t;
    sym->line = line;
    sym->col = col;
    return sym;
}

static void ST_scope_push(ST_sema_t *se) {
    ST_scope_t *s = ST_arena_push_zeroed(se->arena, sizeof(*s));
    ST_ht_init(se->arena, &s->table, 8);
    s->parent = se->scope;
    se->scope = s;
}

static void ST_scope_pop(ST_sema_t *se) {
    se->scope = se->scope->parent;
}

static const char *ST_sym_kind_str(ST_sym_kind_t kind) {
    switch (kind) {
        case ST_SYM_VAR:
            return "variable";
        case ST_SYM_FN:
            return "function";
        case ST_SYM_TYPE:
            return "type";
        case ST_SYM_CONST:
            return "constant";
        case ST_SYM_EXTERN_VAR:
            return "extern variable";
    }
    return "symbol";
}

static void ST_declare_local(ST_sema_t *se, ST_string_t name, ST_ty_t *t, u32 line, u32 col) {
    ST_sym_t *prev = ST_sym_find_in(&se->scope->table, name);
    if (prev) {
        ST_diag_error(&se->diag, line, col, "redeclaration of '" ST_sv_fmt "' in the same scope",
                      ST_sv_args(name));
        ST_diag_note(&se->diag, prev->line, prev->col, "previous declaration is here");
        return;
    }
    ST_sym_insert(se, &se->scope->table, ST_sym_new(se, ST_SYM_VAR, name, NULL, t, line, col));
}

// short-hand: a NULL type means "already reported".
static const char *ST_tstr(ST_sema_t *se, ST_ty_t *t) {
    return ST_ty_cstr(se->arena, t);
}

static ST_ty_t *ST_prim_by_name(ST_sema_t *se, ST_string_t name) {
    ST_forrange(
        0, ST_TYPE_COUNT) if (ST_string_eq_cstr(name, ST_type_names[i])) return se->tys.prim[i];
    return NULL;
}

static b8 ST_ty_is_bool(ST_ty_t *t) {
    return t && t->kind == ST_TY_BOOL;
}
static b8 ST_ty_is_ptr(ST_ty_t *t) {
    return t && t->kind == ST_TY_PTR;
}
static b8 ST_ty_is_layout(ST_ty_t *t) {
    return t && (t->kind == ST_TY_STRUCT || t->kind == ST_TY_TAG_UNION);
}

// untyped int -> i32, untyped float -> f32;
static ST_ty_t *ST_ty_defaulted(ST_sema_t *se, ST_ty_t *t) {
    if (!t)
        return NULL;
    if (t->kind == ST_TY_UNTYPED_INT)
        return se->tys.prim[ST_ti32];
    if (t->kind == ST_TY_UNTYPED_FLOAT)
        return se->tys.prim[ST_tf32];
    return t;
}

// NOTE(segfault): Can a value of type `from` be used where `to` is expected,
// without a cast? I am not to sure if this should be a real bug or not??
static b8 ST_ty_coerces(ST_sema_t *se, ST_ty_t *from, ST_ty_t *to) {
    ST_unused(se);
    if (!from || !to)
        return 1;
    if (from == to)
        return 1;
    if (to->kind == ST_TY_ANY)
        return 1;
    if (from->kind == ST_TY_UNTYPED_INT &&
        (to->kind == ST_TY_INT || to->kind == ST_TY_FLOAT || to->kind == ST_TY_UNTYPED_FLOAT))
        return 1;
    if (from->kind == ST_TY_UNTYPED_FLOAT && to->kind == ST_TY_FLOAT)
        return 1;
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR &&
        (from->inner->kind == ST_TY_VOID || to->inner->kind == ST_TY_VOID))
        return 1;
    if (from->kind == ST_TY_ARRAY && to->kind == ST_TY_DYN_ARRAY && from->inner == to->inner)
        return 1;
    if (from->kind == ST_TY_FN && to->kind == ST_TY_FN)
        return ST_ty_equal(from, to);
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR && from->inner->kind == ST_TY_FN &&
        to->inner->kind == ST_TY_FN)
        return ST_ty_equal(from->inner, to->inner);
    return 0;
}

// Common type of two numeric operands, or NULL if they don't mix.
static ST_ty_t *ST_ty_num_unify(ST_sema_t *se, ST_ty_t *a, ST_ty_t *b) {
    if (!ST_ty_is_numeric(a) || !ST_ty_is_numeric(b))
        return NULL;
    if (a == b)
        return a;
    if (a->kind == ST_TY_UNTYPED_INT)
        return b;
    if (b->kind == ST_TY_UNTYPED_INT)
        return a;
    if (a->kind == ST_TY_UNTYPED_FLOAT && b->kind == ST_TY_FLOAT)
        return b;
    if (b->kind == ST_TY_UNTYPED_FLOAT && a->kind == ST_TY_FLOAT)
        return a;
    ST_unused(se);
    return NULL;
}

static ST_ty_t *ST_type_expr(ST_sema_t *se, ST_expr_t *e);
static ST_ty_t *ST_resolve_tyexpr(ST_sema_t *se, ST_tyexpr_t *te);
static void ST_complete_ty(ST_sema_t *se, ST_ty_t *t);
static void ST_build_fn_ty(ST_sema_t *se, ST_sym_t *sym, ST_fn_sig_t *sig);

static b8 ST_const_eval(ST_sema_t *se, ST_expr_t *e, i64 *out);

static b8 ST_const_eval_bin(ST_sema_t *se, ST_expr_t *e, i64 *out) {
    i64 l, r;
    if (!ST_const_eval(se, e->bin.l, &l))
        return 0;
    if (!ST_const_eval(se, e->bin.r, &r))
        return 0;
    ST_string_t op = e->bin.op;
    if (ST_string_eq_cstr(op, "+")) {
        *out = l + r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "-")) {
        *out = l - r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "*")) {
        *out = l * r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "/")) {
        if (!r)
            return 0;
        *out = l / r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "%")) {
        if (!r)
            return 0;
        *out = l % r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "<<")) {
        *out = (i64)((u64)l << (u64)r);
        return 1;
    }
    if (ST_string_eq_cstr(op, ">>")) {
        *out = l >> r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "&")) {
        *out = l & r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "|")) {
        *out = l | r;
        return 1;
    }
    if (ST_string_eq_cstr(op, "^")) {
        *out = l ^ r;
        return 1;
    }
    return 0;
}

static u32 ST_const_depth = 0;

static b8 ST_const_eval(ST_sema_t *se, ST_expr_t *e, i64 *out) {
    if (!e || ST_const_depth > 128)
        return 0;
    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
            *out = e->ival;
            return 1;
        case ST_EX_IDENT: {
            ST_sym_t *sym = ST_sym_find(se, e->name);
            if (!sym || sym->kind != ST_SYM_CONST || !sym->decl)
                return 0;
            ST_const_depth++;
            b8 ok = ST_const_eval(se, sym->decl->const_.value, out);
            ST_const_depth--;
            return ok;
        }
        case ST_EX_UNARY: {
            i64 v;
            if (!ST_const_eval(se, e->unary.operand, &v))
                return 0;
            if (ST_string_eq_cstr(e->unary.op, "-")) {
                *out = -v;
                return 1;
            }
            if (ST_string_eq_cstr(e->unary.op, "~")) {
                *out = ~v;
                return 1;
            }
            if (ST_string_eq_cstr(e->unary.op, "!")) {
                *out = !v;
                return 1;
            }
            return 0;
        }
        case ST_EX_BINARY:
            return ST_const_eval_bin(se, e, out);
        case ST_EX_CAST:
            return ST_const_eval(se, e->cast.operand, out);
        case ST_EX_SIZEOF: {
            ST_ty_t *t = ST_resolve_tyexpr(se, e->tyop.te);
            if (!t)
                return 0;
            ST_complete_ty(se, t);
            *out = e->tyop.is_align ? (i64)t->align : (i64)t->size;
            return 1;
        }
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_NULL:
        case ST_EX_CALL:
        case ST_EX_FIELD:
        case ST_EX_INDEX:
        case ST_EX_STRUCT_LIT:
        case ST_EX_ARRAY_NEW:
        case ST_EX_TYPEOF:
        case ST_EX_TYPEINFO:
        case ST_EX_KIND:
        case ST_EX_CSTR:
            return 0;
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
    return 0;
}

typedef struct {
    ST_decl_t *tmpl;
    ST_tys_t args;
} ST_inst_info_t;

static ST_inst_info_t *ST_inst_info_find(ST_sema_t *se, ST_ty_t *t) {
    ST_ht_generic_t k = {
        .tag = &t, .size = sizeof(t)
    };
    return (ST_inst_info_t *)ST_ht_get(&se->inst_info, k).tag;
}

static void ST_inst_info_put(ST_sema_t *se, ST_ty_t *t, ST_decl_t *tmpl, ST_tys_t args) {
    ST_inst_info_t *info = ST_arena_push_zeroed(se->arena, sizeof(*info));
    info->tmpl = tmpl;
    info->args = args;
    ST_ty_t **pt = ST_arena_push_zeroed(se->arena, sizeof(*pt));
    *pt = t;
    ST_ht_generic_t *hk = ST_arena_push_zeroed(se->arena, sizeof(*hk));
    hk->tag = pt;
    hk->size = sizeof(*pt);
    ST_ht_set(&se->inst_info, hk, (ST_ht_generic_t){.tag = info, .size = 0});
}

static ST_ty_t *ST_instantiate_struct(ST_sema_t *se, ST_decl_t *tmpl, ST_tys_t args, u32 line,
                                      u32 col) {
    if (args.count != tmpl->struct_.generics.count) {
        ST_diag_error(&se->diag, line, col, "'" ST_sv_fmt "' expects %u type argument(s), got %u",
                      ST_sv_args(tmpl->name), tmpl->struct_.generics.count, args.count);
        return NULL;
    }
    ST_forrange(0, args.count) if (!args.items[i]) return NULL;
    ST_string_t mangled = ST_ty_mangle_instance_name(se->arena, tmpl->name, &args);
    ST_ht_generic_t key = {.tag = mangled.data, .size = mangled.len};
    ST_ht_generic_t existing = ST_ht_get(&se->instantiations, key);
    if (existing.tag)
        return (ST_ty_t *)existing.tag;

    ST_decl_t *id = ST_decl_new(se->arena, ST_DE_STRUCT, line, col);
    id->name = mangled;
    id->is_pub = tmpl->is_pub;
    id->struct_.packing = tmpl->struct_.packing;
    id->struct_.fields = tmpl->struct_.fields;

    ST_ty_t *t = ST_ty_for_decls(&se->tys, id);
    ST_ht_generic_t *hk = ST_arena_push(se->arena, sizeof(*hk));
    hk->tag = mangled.data;
    hk->size = mangled.len;
    ST_ht_set(&se->instantiations, hk, (ST_ht_generic_t){.tag = t, .size = 0});
    if (se->prog)
        ST_da_append_arena(se->arena, &se->prog->decls, id);

    ST_ht_t bindings;
    ST_ht_init(se->arena, &bindings, 8);
    ST_forrange(0, args.count) {
        ST_string_t *pname = tmpl->struct_.generics.items + i;
        ST_ht_generic_t *bk = ST_arena_push(se->arena, sizeof(*bk));
        bk->tag = pname->data;
        bk->size = pname->len;
        ST_ht_set(&bindings, bk, (ST_ht_generic_t){.tag = args.items[i], .size = 0});
    }

    ST_inst_info_put(se, t, tmpl, args);
    ST_ht_t *save = se->generic_bindings;
    b8 save_s = se->stamp_tyexprs;

    se->generic_bindings = &bindings;
    se->stamp_tyexprs = 0;

    ST_complete_ty(se, t);
    se->stamp_tyexprs = save_s;

    se->generic_bindings = save;

    return t;
}

static ST_ty_t *ST_resolve_tyexpr_raw(ST_sema_t *se, ST_tyexpr_t *te) {
    if (!te)
        return NULL;
    switch (te->kind) {
        case ST_TE_NAME: {
            if (te->is_generic_param) {
                if (se->generic_bindings) {
                    ST_ht_generic_t k = {.tag = te->name.data, .size = te->name.len};
                    ST_ht_generic_t found = ST_ht_get(se->generic_bindings, k);
                    if (found.tag)
                        return (ST_ty_t *)found.tag;
                    ST_diag_error(&se->diag, te->line, te->col,
                                  "generic parameter '$ " ST_sv_fmt
                                  "' used outside of instansiation",
                                  ST_sv_args(te->name));
                    return NULL;
                }
            }
            ST_ty_t *prim = ST_prim_by_name(se, te->name);
            if (prim)
                return prim;
            ST_sym_t *sym = ST_sym_find_in(&se->globals, te->name);
            if (!sym) {
                ST_diag_error(&se->diag, te->line, te->col, "unknown type '" ST_sv_fmt "'",
                              ST_sv_args(te->name));
                return NULL;
            }
            if (sym->kind != ST_SYM_TYPE) {
                ST_diag_error(&se->diag, te->line, te->col, "'" ST_sv_fmt "' is a %s, not a type",
                              ST_sv_args(te->name), ST_sym_kind_str(sym->kind));
                ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(te->name));
                return NULL;
            }
            return ST_ty_for_decls(&se->tys, sym->decl);
        }
        case ST_TE_PTR: {
            ST_ty_t *inner = ST_resolve_tyexpr(se, te->inner);
            if (!inner)
                return NULL;
            return ST_ty_ptr(&se->tys, inner);
        }
        case ST_TE_FN: {
            ST_ty_t *t = ST_ty_fn_new(&se->tys);
            t->is_variadic = te->fn_is_variadic;
            ST_forrange(0, te->fn_params.count) {
                ST_ty_t *pt = ST_resolve_tyexpr(se, te->fn_params.items[i]);
                ST_da_append_arena(se->arena, &t->params, pt);
            }
            ST_forrange(0, te->fn_rets.count) {
                ST_ty_t *rt = ST_resolve_tyexpr(se, te->fn_rets.items[i]);
                if (rt && rt->kind == ST_TY_VOID)
                    continue;
                ST_da_append_arena(se->arena, &t->rets, rt);
            }

            return t;
        }
        case ST_TE_TYPEOF:
            return ST_type_expr(se, te->typeof_operand);
        case ST_TE_GENERIC_INST: {
            ST_sym_t *tmpl = ST_sym_find_in(&se->templates, te->name);
            if (!tmpl) {
                if (ST_sym_find_in(&se->globals, te->name)) {
                    ST_diag_error(&se->diag, te->line, te->col,
                                  "'" ST_sv_fmt "' is not a generic type", ST_sv_args(te->name));
                } else
                    ST_diag_error(&se->diag, te->line, te->col, "unknown type '" ST_sv_fmt "' ",
                                  ST_sv_args(te->name));
                return NULL;
            }
            if (tmpl->decl->kind != ST_DE_STRUCT) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "generic %s instaniation is not supported yet",
                              ST_sym_kind_str(tmpl->kind));
                return NULL;
            }
            ST_tys_t args = {0};
            b8 ok = 1;
            ST_forrange(0, te->generic_args.count) {
                ST_ty_t *at = ST_resolve_tyexpr(se, te->generic_args.items[i]);
                if (!at)
                    ok = 0;
                ST_da_append_arena(se->arena, &args, at);
            }
            if (!ok)
                return NULL;
            return ST_instantiate_struct(se, tmpl->decl, args, te->line, te->col);
        }
        case ST_TE_ARRAY: {
            ST_ty_t *inner = ST_resolve_tyexpr(se, te->inner);
            if (!inner)
                return NULL;
            if (te->is_dynamic)
                return ST_ty_dyn_array(&se->tys, inner);
            if (!te->count_expr) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "array size can only be inferred on a declaration with "
                              "an array-literal initalizer ('x : []T = { ... }); "
                              "\ngive an explicit size '[N]T' here");
                return NULL;
            }
            ST_complete_ty(se, inner);
            i64 n = 0;
            if (!ST_const_eval(se, te->count_expr, &n)) {
                ST_diag_error(&se->diag, te->line, te->col,
                              "array size must be a constant integer expression");
                return NULL;
            }
            if (n < 0) {
                ST_diag_error(&se->diag, te->line, te->col, "array size cannot be negative (%ld)",
                              n);
                return NULL;
            }
            return ST_ty_array(&se->tys, inner, (u64)n);
        }
    }
    return NULL;
}

static ST_ty_t *ST_resolve_tyexpr(ST_sema_t *se, ST_tyexpr_t *te) {
    ST_ty_t *t = ST_resolve_tyexpr_raw(se, te);
    if (te && t && se->stamp_tyexprs)
        te->resolved = t;
    return t;
}

static ST_expr_t *ST_clone_expr(ST_arena_t *a, ST_expr_t *e);
static ST_stmt_t *ST_clone_stmt(ST_arena_t *a, ST_stmt_t *s);

static ST_tyexpr_t *ST_clone_tyexpr(ST_arena_t *a, ST_tyexpr_t *te) {
    if (!te)
        return NULL;
    ST_tyexpr_t *n = ST_tyexpr_new(a, te->kind, te->line, te->col);
    *n = *te;
    n->resolved = NULL;
    n->inner = ST_clone_tyexpr(a, te->inner);
    n->count_expr = ST_clone_expr(a, te->count_expr);
    n->typeof_operand = ST_clone_expr(a, te->typeof_operand);

    n->fn_params = (ST_tyexprs_t){0};
    ST_forrange(0, te->fn_params.count)
        ST_da_append_arena(a, &n->fn_params, ST_clone_tyexpr(a, te->fn_params.items[i]));

    n->fn_rets = (ST_tyexprs_t){0};
    ST_forrange(0, te->fn_rets.count)
        ST_da_append_arena(a, &n->fn_rets, ST_clone_tyexpr(a, te->fn_rets.items[i]));

    n->generic_args = (ST_tyexprs_t){0};
    ST_forrange(0, te->generic_args.count)
        ST_da_append_arena(a, &n->generic_args, ST_clone_tyexpr(a, te->generic_args.items[i]));

    return n;
}

static ST_expr_t *ST_clone_expr(ST_arena_t *a, ST_expr_t *e) {
    if (!e)
        return NULL;
    ST_expr_t *n = ST_expr_new(a, e->kind, e->line, e->col);
    *n = *e;
    n->ty = NULL;
    switch(e->kind) {
    case ST_EX_INT:
    case ST_EX_FLOAT:
    case ST_EX_STR:
    case ST_EX_CHAR:
    case ST_EX_BOOL:
    case ST_EX_NULL:
    case ST_EX_IDENT:
        break;

    case ST_EX_UNARY:
        n->unary.operand = ST_clone_expr(a, e->unary.operand);
        break;

    case ST_EX_BINARY:
        n->bin.l = ST_clone_expr(a, e->bin.l);
        n->bin.r = ST_clone_expr(a, e->bin.r);
        break;

    case ST_EX_CALL:
        n->call.callee = ST_clone_expr(a, e->call.callee);
        n->call.args = (ST_args_t){0};
        ST_forrange(0, e->call.args.count) {
            ST_arg_t arg = e->call.args.items[i];
            arg.value = ST_clone_expr(a, arg.value);
            ST_da_append_arena(a, &n->call.args, arg);
        }
        break;

    case ST_EX_FIELD:
        n->field.base = ST_clone_expr(a, e->field.base);
        break;

    case ST_EX_INDEX:
        n->index.base = ST_clone_expr(a, e->index.base);
        n->index.index = ST_clone_expr(a, e->index.index);
        break;

    case ST_EX_CAST:
        n->cast.operand = ST_clone_expr(a, e->cast.operand);
        n->cast.to = ST_clone_tyexpr(a, e->cast.to);
        break;

    case ST_EX_STRUCT_LIT:
        n->struct_lit.generic_args = (ST_tyexprs_t){0};
        ST_forrange(0, e->struct_lit.generic_args.count)
            ST_da_append_arena(a, &n->struct_lit.generic_args,
                               ST_clone_tyexpr(a, e->struct_lit.generic_args.items[i]));

        n->struct_lit.inits = (ST_field_inits_t){0};
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t fi = e->struct_lit.inits.items[i];
            fi.value = ST_clone_expr(a, fi.value);
            ST_da_append_arena(a, &n->struct_lit.inits, fi);
        }
        break;
    case ST_EX_ARRAY_NEW:
        n->array_new.te = ST_clone_tyexpr(a, e->array_new.te);
        break;

    case ST_EX_SIZEOF:
    case ST_EX_TYPEOF:
    case ST_EX_TYPEINFO:
    case ST_EX_KIND:
    case ST_EX_CSTR:
        n->tyop.te = ST_clone_tyexpr(a, e->tyop.te);
        n->tyop.operand = ST_clone_expr(a, e->tyop.operand);
        break;
    case ST_EX_COUNT:
        ST_assert(0);
        break;
    }

    return n;
}

static ST_stmts_t ST_clone_body(ST_arena_t *a, ST_stmts_t *body) {
    ST_stmts_t out = {0};
    ST_forrange(0, body->count)
        ST_da_append_arena(a, &out, ST_clone_stmt(a, body->items[i]));
    return out;
}

static ST_stmt_t *ST_clone_stmt(ST_arena_t *a, ST_stmt_t *s) {
    if (!s)
        return NULL;
    ST_stmt_t *n = ST_stmt_new(a, s->kind, s->line, s->col);
    switch (s->kind) {

    case ST_ST_EXPR:
        n->expr = ST_clone_expr(a, s->expr);
        break;

    case ST_ST_DECL:
        n->decl.te = ST_clone_tyexpr(a, s->decl.te);
        n->decl.init = ST_clone_expr(a, s->decl.init);
        break;

    case ST_ST_ASSIGN:
        n->assign.lhs = ST_clone_expr(a, s->assign.lhs);
        n->assign.rhs = ST_clone_expr(a, s->assign.rhs);
        break;

    case ST_ST_MULTI_BIND:
        n->multi.values = (ST_exprs_t){0};
        ST_forrange(0, s->multi.values.count)
            ST_da_append_arena(a, &n->multi.values,
                               ST_clone_expr(a, s->multi.values.items[i]));
        break;

    case ST_ST_IF:
        n->if_.cond = ST_clone_expr(a, s->if_.cond);
        n->if_.then_body = ST_clone_body(a, &s->if_.then_body);
        n->if_.else_stmt = ST_clone_stmt(a, s->if_.else_stmt);
        break;

    case ST_ST_SWITCH:
        n->switch_.cond = ST_clone_expr(a, s->switch_.cond);
        n->switch_.cases = (ST_cases_t){0};
        ST_forrange(0, s->switch_.cases.count) {
            ST_case_t *c = &s->switch_.cases.items[i];
            ST_case_t nc = { .line = c->line, .col = c->col};
            for (u32 k = 0; k < c->values.count; k++)
            ST_da_append_arena(a, &nc.values,
                               ST_clone_expr(a, c->values.items[k]));
            nc.body = ST_clone_body(a, &c->body);
            ST_da_append_arena(a, &n->switch_.cases, nc);
        }
        break;

    case ST_ST_WHILE:
        n->while_.cond = ST_clone_expr(a, s->while_.cond);
        n->while_.body = ST_clone_body(a, &s->while_.body);
        break;

    case ST_ST_FOR_RANGE:
        n->for_range.lo = ST_clone_expr(a, s->for_range.lo);
        n->for_range.hi = ST_clone_expr(a, s->for_range.hi);
        n->for_range.iter_te = ST_clone_tyexpr(a, s->for_range.iter_te);
        n->for_range.body = ST_clone_body(a, &s->for_range.body);
        break;

    case ST_ST_FOR_ARRAY:
        n->for_array.target = ST_clone_expr(a, s->for_array.target);
        n->for_array.body = ST_clone_body(a, &s->for_array.body);
        break;

    case ST_ST_RETURN:
        n->ret.values = (ST_exprs_t){0};
        ST_forrange(0, s->ret.values.count)
            ST_da_append_arena(a, &n->ret.values,
                               ST_clone_expr(a, s->ret.values.items[i]));
        break;

    case ST_ST_BLOCK:
        n->block = ST_clone_body(a, &s->block);
        break;

    case ST_ST_DEFER:
        n->defer_stmt = ST_clone_stmt(a, s->defer_stmt);
        break;

    case ST_ST_BREAK:
    case ST_ST_CONTINUE:
    case ST_ST_LABEL:
    case ST_ST_GODOWN:
    case ST_ST_ASM:
        break;

    case ST_ST_COUNT:
        ST_assert(0);
        break;

    }

    return n;
}

static ST_fn_sig_t ST_clone_fn_sig(ST_arena_t *a, ST_fn_sig_t *s) {
    ST_fn_sig_t out = {0};
    out.has_ret_ann = s->has_ret_ann;
    out.is_variadic = s->is_variadic;
    ST_forrange(0, s->params.count) {
        ST_param_t p = s->params.items[i];
        p.te = ST_clone_tyexpr(a, p.te);
        p.def = ST_clone_expr(a, p.def);
        ST_da_append_arena(a, &out.params, p);
    }

    ST_forrange(0, s->rets.count)
        ST_da_append_arena(a, &out.rets, ST_clone_tyexpr(a, s->rets.items[i]));
    return out;
}

static b8 ST_unify_tyexpr(ST_sema_t *se, ST_tyexpr_t *pt, ST_ty_t *at,
                       ST_ht_t *bindings) {
    if (!pt || !at)
        return 1;

    switch(pt->kind) {
    case ST_TE_NAME: {
        if (!pt->is_generic_param)
            return 1;
        ST_ty_t *want = ST_ty_defaulted(se, at);
        ST_ht_generic_t k = { .tag = pt->name.data, .size = pt->name.len };
        ST_ty_t *bound = (ST_ty_t *)ST_ht_get(bindings, k).tag;
        if (bound)
            return bound == want || ST_ty_equal(bound, want);

        ST_ht_generic_t *bk = ST_arena_push(se->arena, sizeof(*bk));
        bk->tag = pt->name.data;
        bk->size = pt->name.len;
        ST_ht_set(bindings, bk, (ST_ht_generic_t){.tag = want, .size = 0});
        return 1;
    }

    case ST_TE_PTR: {
        if (at->kind != ST_TY_PTR)
            return 1;
        return ST_unify_tyexpr(se, pt->inner, at->inner, bindings);
    }

    case ST_TE_ARRAY: {
        if (pt->is_dynamic)
            return at->kind == ST_TY_DYN_ARRAY ?
                ST_unify_tyexpr(se, pt->inner, at->inner, bindings)
                : 1;
        return  at->kind == ST_TY_ARRAY ?
            ST_unify_tyexpr(se, pt->inner, at->inner, bindings)
            : 1;
    }

    case ST_TE_FN: {
        if (at->kind == ST_TY_PTR && at->inner && at->inner->kind == ST_TY_FN)
            at = at->inner;
        if (at->kind != ST_TY_FN)
            return 1;
        ST_forrange(0, pt->fn_params.count) {
            if (i >= at->params.count)
                break;
            if (!ST_unify_tyexpr(se, pt->fn_params.items[i],
                                 at->params.items[i], bindings))
                return 0;
        }
        return 1;
    }

    case ST_TE_GENERIC_INST: {
        if (at->kind != ST_TY_STRUCT)
            return 1;
        ST_inst_info_t *info = ST_inst_info_find(se, at);
        if (!info)
            return 1;

        ST_sym_t *tsym = ST_sym_find_in(&se->templates, pt->name);
        if (!tsym || tsym->decl != info->tmpl)
            return 1;

        if (pt->generic_args.count != info->args.count)
            return 1;

        ST_forrange(0, pt->generic_args.count)
            if (!ST_unify_tyexpr(se, pt->generic_args.items[i],
                                 info->args.items[i], bindings)) return 0;
        return 1;
    }

    case ST_TE_TYPEOF:
        return 1;
    }

    return 1;
}

static ST_sym_t *ST_instantiate_fn(ST_sema_t *se, ST_decl_t *tmpl, ST_ht_t *bindings,
                                   u32 line, u32 col) {
    ST_tys_t args = {0};
    ST_forrange(0, tmpl->fn.sig.generics.count) {
        ST_string_t g = tmpl->fn.sig.generics.items[i];
        ST_ht_generic_t k = { .tag = g.data, .size = g.len };
        ST_ty_t *bound = (ST_ty_t *)ST_ht_get(bindings, k).tag;
        if (!bound) {
            ST_diag_error(&se->diag, line, col,
                          "could not infer generic parameter '$"ST_sv_fmt
                          "' for call to '"ST_sv_fmt"'", ST_sv_args(g),
                          ST_sv_args(tmpl->name));
            ST_diag_note(&se->diag, tmpl->line, tmpl->col, "'" ST_sv_fmt"' is declared here",
                         ST_sv_args(tmpl->name));
            return NULL;
        }
        ST_da_append_arena(se->arena, &args, bound);
    }

    ST_string_t mangled = ST_ty_mangle_instance_name(se->arena, tmpl->name, &args);
    ST_ht_generic_t key = { .tag = mangled.data, .size = mangled.len };
    ST_sym_t *existing = ST_ht_get(&se->fn_instantiations, key).tag;
    if (existing)
        return existing;

    if (se->n_fn_instances >= ST_MAX_FN_INSTANCE) {
        ST_diag_error(&se->diag, line, col,
                      "too many generic function instantiations (limit %u); "
                      "likely runaway recursive instantiation of '"ST_sv_fmt"'",
                      ST_MAX_FN_INSTANCE, ST_sv_args(tmpl->name));
        return NULL;
    }
    se->n_fn_instances++;
    ST_decl_t *inst = ST_decl_new(se->arena, ST_DE_FN, tmpl->line, tmpl->col);
    inst->name = mangled;
    inst->is_pub = tmpl->is_pub;
    inst->fn.sig = ST_clone_fn_sig(se->arena, &tmpl->fn.sig);
    inst->fn.is_prototype = tmpl->fn.is_prototype;
    inst->fn.body = ST_clone_body(se->arena, &tmpl->fn.body);

    ST_ht_t *pb = ST_arena_push_zeroed(se->arena, sizeof(*pb));
    *pb = *bindings;

    ST_sym_t *sym = ST_sym_new(se, ST_SYM_FN, mangled, inst, NULL, tmpl->line, tmpl->col);
    sym->generic_bindings = pb;

    ST_ht_generic_t *hk = ST_arena_push(se->arena, sizeof(*hk));
    hk->tag = mangled.data;
    hk->size = mangled.len;

    ST_ht_set(&se->fn_instantiations, hk, (ST_ht_generic_t){ .tag = sym, .size = 0});
    ST_sym_insert(se, &se->globals, sym);

    ST_ht_t *save_b = se->generic_bindings;
    b8 save_s = se->stamp_tyexprs;
    se->generic_bindings = pb;
    se->stamp_tyexprs = 1;

    ST_build_fn_ty(se, sym, &inst->fn.sig);
    se->stamp_tyexprs = save_s;
    se->generic_bindings = save_b;

    if (se->prog)
        ST_da_append_arena(se->arena, &se->prog->decls, inst);

    return sym;
}

static u32 ST_align_up(u32 x, u32 a) {
    if (a < 2)
        return x;
    return (x + a - 1) & ~(a - 1);
}

static void ST_complete_struct(ST_sema_t *se, ST_ty_t *t) {
    ST_decl_t *d = t->decl;
    b8 packed = d->struct_.packing == ST_PACK_PACKED;
    u32 off = 0, align = 1;

    ST_forrange(0, d->struct_.fields.count) {
        ST_field_spec_t *f = &d->struct_.fields.items[i];
        ST_ty_t *ft = NULL;
        if (f->te)
            ft = ST_resolve_tyexpr(se, f->te);
        else if (f->anon)
            ft = ST_ty_for_decls(&se->tys, f->anon);
        if (!ft)
            continue;

        ST_complete_ty(se, ft); // no-op unless struct/tag_union by value
        u32 a = packed ? 1 : (ft->align ? ft->align : 1);
        off = ST_align_up(off, a);

        ST_ty_field_t field = {.name = f->name, .ty = ft, .offset = off};
        ST_da_append_arena(se->arena, &t->fields, field);

        off += ft->size;
        if (a > align)
            align = a;
    }
    t->align = align;
    t->size = ST_align_up(off, align);
}

static void ST_complete_tag_union(ST_sema_t *se, ST_ty_t *t) {
    ST_decl_t *d = t->decl;
    u32 max = 0;
    ST_forrange(0, d->tag_union.variants.count) {
        ST_variant_spec_t *v = &d->tag_union.variants.items[i];
        if (!v->payload)
            continue;
        ST_ty_t *pt = ST_resolve_tyexpr(se, v->payload);
        if (!pt)
            continue;
        ST_complete_ty(se, pt);
        if (pt->size > max)
            max = pt->size;
    }
    t->align = 8;
    t->size = 8 + ST_align_up(max, 8);
}

static void ST_complete_ty(ST_sema_t *se, ST_ty_t *t) {
    if (!ST_ty_is_layout(t))
        return;
    if (t->state == ST_TY_STATE_DONE)
        return;
    if (t->state == ST_TY_STATE_COMPUTING) {
        ST_diag_error(&se->diag, t->decl->line, t->decl->col,
                      "recursive type '" ST_sv_fmt "' has infinite size",
                      ST_sv_args(t->decl->name));
        ST_diag_note(&se->diag, t->decl->line, t->decl->col,
                     "break the cycle with a pointer, e.g. '*" ST_sv_fmt "'",
                     ST_sv_args(t->decl->name));
        t->state = ST_TY_STATE_DONE;
        return;
    }
    t->state = ST_TY_STATE_COMPUTING;
    if (t->kind == ST_TY_STRUCT)
        ST_complete_struct(se, t);
    else
        ST_complete_tag_union(se, t);
    if (t->state != ST_TY_STATE_DONE)
        t->state = ST_TY_STATE_DONE;
}

// Constants keep their untyped type so `N :: 10` still coerces anywhere.
static ST_ty_t *ST_ty_of_const(ST_sema_t *se, ST_sym_t *sym) {
    if (sym->t)
        return sym->t;
    if (!sym->decl || sym->decl->kind != ST_DE_CONST)
        return NULL;
    if (ST_const_depth > 128) {
        ST_diag_error(&se->diag, sym->line, sym->col,
                      "constant '" ST_sv_fmt "' is defined in terms of itself",
                      ST_sv_args(sym->name));
        return NULL;
    }
    ST_const_depth++;
    sym->t = ST_type_expr(se, sym->decl->const_.value);
    ST_const_depth--;
    return sym->t;
}

static ST_ty_t *ST_type_ident(ST_sema_t *se, ST_expr_t *e) {
    ST_sym_t *sym = ST_sym_find(se, e->name);
    if (!sym) {
        ST_diag_error(&se->diag, e->line, e->col, "use of undeclared identifier '" ST_sv_fmt "'",
                      ST_sv_args(e->name));
        return NULL;
    }
    switch (sym->kind) {
        case ST_SYM_VAR:
        case ST_SYM_EXTERN_VAR:
        case ST_SYM_FN:
            return sym->t;
        case ST_SYM_CONST:
            return ST_ty_of_const(se, sym);
        case ST_SYM_TYPE:
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' is a type, it cannot be used as a value",
                          ST_sv_args(e->name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(e->name));
            return NULL;
    }
    return NULL;
}

static ST_ty_t *ST_type_unary(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *t = ST_type_expr(se, e->unary.operand);
    if (!t)
        return NULL;
    ST_string_t op = e->unary.op;

    if (ST_string_eq_cstr(op, "-")) {
        if (!ST_ty_is_numeric(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "unary '-' needs a numeric operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "!")) {
        if (!ST_ty_is_bool(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "'!' needs a 'bool' operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "~")) {
        if (!ST_ty_is_int(t)) {
            ST_diag_error(&se->diag, e->line, e->col, "'~' needs an integer operand, got '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t;
    }
    if (ST_string_eq_cstr(op, "*")) {
        if (!ST_ty_is_ptr(t) || t->inner->kind == ST_TY_VOID) {
            ST_diag_error(&se->diag, e->line, e->col, "cannot dereference a value of type '%s'",
                          ST_tstr(se, t));
            return NULL;
        }
        return t->inner;
    }
    if (ST_string_eq_cstr(op, "&"))
        return ST_ty_ptr(&se->tys, ST_ty_defaulted(se, t));
    return t;
}

static b8 ST_op_is(ST_string_t op, const char *a, const char *b) {
    return ST_string_eq_cstr(op, a) || (b && ST_string_eq_cstr(op, b));
}

static ST_ty_t *ST_type_binary(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *l = ST_type_expr(se, e->bin.l);
    ST_ty_t *r = ST_type_expr(se, e->bin.r);
    if (!l || !r)
        return NULL;
    ST_string_t op = e->bin.op;
    ST_ty_t *bool_ty = se->tys.prim[ST_tbool];

    // logical
    if (ST_op_is(op, "&&", "||")) {
        if (!ST_ty_is_bool(l) || !ST_ty_is_bool(r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' needs 'bool' operands, got '%s' and '%s'",
                          ST_sv_args(op), ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        return bool_ty;
    }

    // equality: anything that coerces one way or the other
    if (ST_op_is(op, "==", "!=")) {
        if (!ST_ty_coerces(se, l, r) && !ST_ty_coerces(se, r, l) && !ST_ty_num_unify(se, l, r)) {
            ST_diag_error(&se->diag, e->line, e->col, "cannot compare '%s' with '%s'",
                          ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        return bool_ty;
    }

    // ordering: numeric or char
    if (ST_op_is(op, "<", "<=") || ST_op_is(op, ">", ">=")) {
        if (l->kind == ST_TY_CHAR && r->kind == ST_TY_CHAR)
            return bool_ty;
        if (!ST_ty_num_unify(se, l, r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "cannot order '%s' and '%s' with '" ST_sv_fmt "'", ST_tstr(se, l),
                          ST_tstr(se, r), ST_sv_args(op));
            return NULL;
        }
        return bool_ty;
    }

    // bitwise and shifts: integers only
    if (ST_op_is(op, "&", "|") || ST_op_is(op, "^", NULL) || ST_op_is(op, "<<", ">>")) {
        if (!ST_ty_is_int(l) || !ST_ty_is_int(r)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' needs integer operands, got '%s' and '%s'",
                          ST_sv_args(op), ST_tstr(se, l), ST_tstr(se, r));
            return NULL;
        }
        if (ST_op_is(op, "<<", ">>"))
            return l->kind == ST_TY_UNTYPED_INT ? r : l;
        ST_ty_t *u = ST_ty_num_unify(se, l, r);
        if (!u) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "mismatched integer types '%s' and '%s' for '" ST_sv_fmt "'",
                          ST_tstr(se, l), ST_tstr(se, r), ST_sv_args(op));
            return NULL;
        }
        return u;
    }

    // arithmetic
    if (ST_string_eq_cstr(op, "%") && (!ST_ty_is_int(l) || !ST_ty_is_int(r))) {
        ST_diag_error(&se->diag, e->line, e->col, "'%%' needs integer operands, got '%s' and '%s'",
                      ST_tstr(se, l), ST_tstr(se, r));
        return NULL;
    }
    ST_ty_t *u = ST_ty_num_unify(se, l, r);
    if (!u) {
        ST_diag_error(&se->diag, e->line, e->col,
                      "invalid operands to '" ST_sv_fmt "': '%s' and '%s'", ST_sv_args(op),
                      ST_tstr(se, l), ST_tstr(se, r));
        return NULL;
    }
    return u;
}

static void ST_arg_extern_decay(ST_sema_t *se, ST_sym_t *sym, ST_ty_t *pt, ST_arg_t *arg) {
    if (!sym || !sym->decl || sym->decl->kind != ST_DE_EXTERN_FN)
        return;
    if (!arg->value->ty || arg->value->ty->kind != ST_TY_STRING)
        return;
    if (pt &&
        (pt->kind != ST_TY_PTR || (pt->inner->kind != ST_TY_CHAR && pt->inner->kind != ST_TY_VOID)))
        return;

    ST_expr_t *cs = ST_expr_new(se->arena, ST_EX_CSTR, arg->value->line, arg->value->col);
    cs->tyop.operand = arg->value;
    cs->ty = ST_ty_ptr(&se->tys, se->tys.prim[ST_tchar]);
    arg->value = cs;
}

static ST_tys_t *ST_type_call(ST_sema_t *se, ST_expr_t *e) {
    ST_expr_t *callee = e->call.callee;
    ST_sym_t *sym = NULL;
    ST_ty_t *fnty = NULL;

    ST_decl_t *fn_tmpl = NULL;

    if (callee && callee->kind == ST_EX_IDENT) {
        sym = ST_sym_find(se, callee->name);
        if (!sym) {
            ST_sym_t *tsym = ST_sym_find_in(&se->templates, callee->name);
            if (tsym && tsym->decl && tsym->decl->kind == ST_DE_FN)
                fn_tmpl = tsym->decl;
            else if (tsym)
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "'" ST_sv_fmt "' is a generic type, it cannot be called",
                              ST_sv_args(callee->name));
            else
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "call to undeclared function '" ST_sv_fmt"'",
                              ST_sv_args(callee->name));

        } else if (sym->kind != ST_SYM_FN && sym->kind != ST_SYM_VAR) {
            ST_diag_error(&se->diag, callee->line, callee->col,
                          "'" ST_sv_fmt "' is a %s, it cannot be called", ST_sv_args(callee->name),
                          ST_sym_kind_str(sym->kind));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(callee->name));
            sym = NULL;
        } else {
            fnty = sym->t;
            if (fnty && fnty->kind == ST_TY_PTR && fnty->inner && fnty->inner->kind == ST_TY_FN)
                fnty = fnty->inner;
            if (sym->kind == ST_SYM_VAR && fnty && fnty->kind != ST_TY_FN) {
                ST_diag_error(&se->diag, callee->line, callee->col,
                              "cannot call a value of type '%s'", ST_tstr(se, fnty));
                fnty = NULL;
                sym = NULL;
            }
        }
    } else if (callee) {
        fnty = ST_type_expr(se, callee);
        if (fnty && fnty->kind == ST_TY_PTR && fnty->inner && fnty->inner->kind == ST_TY_FN)
            fnty = fnty->inner;
        if (fnty && fnty->kind != ST_TY_FN) {
            ST_diag_error(&se->diag, callee->line, callee->col, "cannot call a value of type '%s'",
                          ST_tstr(se, fnty));
            fnty = NULL;
        }
    }

    // type all argument expressions
    ST_forrange(0, e->call.args.count) ST_type_expr(se, e->call.args.items[i].value);

    if (fn_tmpl) {
        ST_ht_t bindings;
        ST_ht_init(se->arena, &bindings, 8);
        ST_fn_sig_t *tsig = &fn_tmpl->fn.sig;
        u32 pos = 0;
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            u32 idx = tsig->params.count;
            if (arg->name.len) {
                for (u32 k = 0; k < tsig->params.count; k++)
                    if (ST_string_eq(tsig->params.items[k].name, arg->name)) {
                        idx = k;
                        break;
                    }
            } else
                idx = pos++;

            if (idx < tsig->params.count && tsig->params.items[idx].te && arg->value->ty)
                ST_unify_tyexpr(se, tsig->params.items[idx].te, arg->value->ty, &bindings);
        }

        sym = ST_instantiate_fn(se, fn_tmpl, &bindings, e->line, e->col);
        if (!sym)
            return NULL;

        callee->name = sym->name;
        fnty = sym->t;
    }

    // builtins: no signature yet, everything goes
    if (sym && sym->kind == ST_SYM_FN && !sym->decl)
        return &ST_builtin_rets;

    // declaration-based checks: defaults and named arguments
    ST_fn_sig_t *sig = NULL;
    if (sym && sym->kind == ST_SYM_FN && sym->decl)
        sig = sym->decl->kind == ST_DE_FN ? &sym->decl->fn.sig : &sym->decl->extern_fn.sig;

    if (!fnty)
        return NULL;

    b8 has_named = 0;
    ST_forrange(0, e->call.args.count) if (e->call.args.items[i].name.len) has_named = 1;

    if (sig && has_named) {
        u32 pos = 0;
        ST_forrange(0, e->call.args.count) {
            ST_arg_t *arg = &e->call.args.items[i];
            u32 idx = sig->params.count;
            if (arg->name.len) {
                for (u32 k = 0; k < sig->params.count; k++)
                    if (ST_string_eq(sig->params.items[k].name, arg->name)) {
                        idx = k;
                        break;
                    }
                if (idx == sig->params.count) {
                    ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                                  "'" ST_sv_fmt "' has no parameter named '" ST_sv_fmt "'",
                                  ST_sv_args(sym->name), ST_sv_args(arg->name));
                    continue;
                }
            } else
                idx = pos++;
            if (idx >= fnty->params.count)
                continue;
            ST_ty_t *pt = fnty->params.items[idx];
            ST_arg_extern_decay(se, sym, pt, arg);
            ST_ty_t *at = arg->value->ty;
            if (pt && at && !ST_ty_coerces(se, at, pt))
                ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                              "argument '" ST_sv_fmt "' expects '%s', got '%s'",
                              ST_sv_args(sig->params.items[idx].name), ST_tstr(se, pt),
                              ST_tstr(se, at));
        }
        return &fnty->rets;
    }

    // positional: count checks
    u32 n = e->call.args.count;
    u32 max_p = fnty->params.count;
    u32 min_args = max_p;
    if (sig) {
        min_args = 0;
        ST_forrange(0, sig->params.count) if (!sig->params.items[i].def) min_args++;
    }

    if (n < min_args || (!fnty->is_variadic && n > max_p)) {
        u32 line = sym ? sym->line : (callee ? callee->line : e->line);
        u32 col = sym ? sym->col : (callee ? callee->col : e->col);
        ST_string_t name = sym ? sym->name : ST_cstr_to_str("function");
        if (fnty->is_variadic)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects at least %u argument%s, got %u",
                          ST_sv_args(name), min_args, min_args == 1 ? "" : "s", n);
        else if (min_args == max_p)
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u argument%s, got %u", ST_sv_args(name), max_p,
                          max_p == 1 ? "" : "s", n);
        else
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' expects %u to %u arguments, got %u", ST_sv_args(name),
                          min_args, max_p, n);
        if (sym)
            ST_diag_note(&se->diag, line, col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(name));
    }

    // positional type checks
    ST_forrange(0, n) {
        if (i >= max_p)
            break;
        ST_arg_t *arg = &e->call.args.items[i];
        ST_ty_t *pt = fnty->params.items[i];
        ST_arg_extern_decay(se, sym, pt, arg);
        ST_ty_t *at = arg->value->ty;
        if (pt && at && !ST_ty_coerces(se, at, pt)) {
            ST_diag_error(&se->diag, arg->value->line, arg->value->col,
                          "argument %u expects '%s', got '%s'", i + 1, ST_tstr(se, pt),
                          ST_tstr(se, at));
            if (sym)
                ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(sym->name));
        }
    }
    if (fnty->is_variadic) {
        ST_forrange(max_p, n) {
            ST_arg_t *arg = &e->call.args.items[i];
            ST_arg_extern_decay(se, sym, NULL, arg);
            if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_INT)
                arg->value->ty = se->tys.prim[ST_ti32];
            else if (arg->value->ty && arg->value->ty->kind == ST_TY_UNTYPED_FLOAT)
                arg->value->ty = se->tys.prim[ST_tf32];
        }
    }
    return &fnty->rets;
}

static ST_ty_t *ST_type_field(ST_sema_t *se, ST_expr_t *e) {
    ST_expr_t *base = e->field.base;

    // TODO(segfault): tag_union construction
    // Type.Member: enum variants
    if (base && base->kind == ST_EX_IDENT) {
        ST_sym_t *sym = ST_sym_find(se, base->name);
        if (sym && sym->kind == ST_SYM_TYPE && sym->decl) {
            ST_ty_t *t = ST_ty_for_decls(&se->tys, sym->decl);
            ST_variant_specs_t *vs = NULL;
            if (sym->decl->kind == ST_DE_ENUM)
                vs = &sym->decl->enum_.variants;
            else if (sym->decl->kind == ST_DE_TAG_UNION)
                vs = &sym->decl->tag_union.variants;
            if (!vs) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "'" ST_sv_fmt "' is a struct, use a struct literal "
                              "to build one",
                              ST_sv_args(base->name));
                return NULL;
            }
            ST_forrange(0, vs->count) if (ST_string_eq(vs->items[i].name, e->field.name)) return t;
            ST_diag_error(&se->diag, e->line, e->col,
                          "'" ST_sv_fmt "' has no variant '" ST_sv_fmt "'", ST_sv_args(base->name),
                          ST_sv_args(e->field.name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(base->name));
            return NULL;
        }
    }

    ST_ty_t *t = ST_type_expr(se, base);
    if (!t)
        return NULL;
    if (t->kind == ST_TY_PTR)
        t = t->inner; // one auto-deref, like a.b on *A

    if (t->kind == ST_TY_STRUCT) {
        ST_complete_ty(se, t);
        ST_forrange(0, t->fields.count) if (ST_string_eq(t->fields.items[i].name,
                                                         e->field.name)) return t->fields.items[i]
            .ty;
        ST_diag_error(&se->diag, e->line, e->col, "'%s' has no field '" ST_sv_fmt "'",
                      ST_tstr(se, t), ST_sv_args(e->field.name));
        if (t->decl)
            ST_diag_note(&se->diag, t->decl->line, t->decl->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(t->decl->name));
        return NULL;
    }

    // built-in members on arrays and strings
    if (t->kind == ST_TY_ARRAY || t->kind == ST_TY_DYN_ARRAY || t->kind == ST_TY_STRING) {
        if (ST_string_eq_cstr(e->field.name, "len"))
            return se->tys.prim[ST_ti32];
        if (ST_string_eq_cstr(e->field.name, "ptr"))
            return ST_ty_ptr(&se->tys, t->kind == ST_TY_STRING ? se->tys.prim[ST_tchar] : t->inner);
    }

    ST_diag_error(&se->diag, e->line, e->col, "'%s' has no field '" ST_sv_fmt "'", ST_tstr(se, t),
                  ST_sv_args(e->field.name));
    return NULL;
}

static ST_ty_t *ST_type_index(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *bt = ST_type_expr(se, e->index.base);
    ST_ty_t *it = ST_type_expr(se, e->index.index);
    if (it && !ST_ty_is_int(it))
        ST_diag_error(&se->diag, e->index.index->line, e->index.index->col,
                      "array index must be an integer, got '%s'", ST_tstr(se, it));
    if (!bt)
        return NULL;
    if (bt->kind == ST_TY_ARRAY) {
        i64 idx = 0;
        if (ST_const_eval(se, e->index.index, &idx) && ((idx < 0) || (u64)idx >= bt->count)) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "array index '%lld' is out of bound for array of length %llu",
                          (long long)idx, (unsigned long long)bt->count);
            return bt->inner;
        }
        return bt->inner;
    }
    if (bt->kind == ST_TY_STRING)
        return se->tys.prim[ST_tchar];
    if (bt->kind == ST_TY_DYN_ARRAY)
        return bt->inner;
    if (bt->kind == ST_TY_PTR && bt->inner->kind != ST_TY_VOID)
        return bt->inner;
    ST_diag_error(&se->diag, e->line, e->col, "cannot index a value of type '%s'", ST_tstr(se, bt));
    return NULL;
}

static ST_ty_t *ST_type_cast(ST_sema_t *se, ST_expr_t *e) {
    ST_ty_t *from = ST_type_expr(se, e->cast.operand);
    ST_ty_t *to = ST_resolve_tyexpr(se, e->cast.to);
    if (!from || !to)
        return to;
    if (ST_ty_coerces(se, from, to))
        return to;

    b8 from_num = ST_ty_is_numeric(from) || from->kind == ST_TY_CHAR || from->kind == ST_TY_ENUM ||
                  from->kind == ST_TY_BOOL;
    b8 to_num = ST_ty_is_numeric(to) || to->kind == ST_TY_CHAR || to->kind == ST_TY_ENUM;
    if (from_num && to_num)
        return to;
    if (from->kind == ST_TY_PTR && to->kind == ST_TY_PTR)
        return to;
    if (from->kind == ST_TY_PTR && ST_ty_is_int(to))
        return to;
    if (ST_ty_is_int(from) && to->kind == ST_TY_PTR)
        return to;
    if (from->kind == ST_TY_ANY)
        return to;

    ST_diag_error(&se->diag, e->line, e->col, "cannot cast '%s' to '%s'", ST_tstr(se, from),
                  ST_tstr(se, to));
    return to;
}

static ST_ty_t *ST_type_struct_lit(ST_sema_t *se, ST_expr_t *e, ST_ty_t *expect) {
    ST_ty_t *t = NULL;
    ST_sym_t *lit_tmpl = e->struct_lit.type_name.len
        ? ST_sym_find_in(&se->templates, e->struct_lit.type_name)
        : NULL;

    b8 is_struct_lit = lit_tmpl && lit_tmpl->decl && lit_tmpl->decl->kind == ST_DE_STRUCT;

    if (e->struct_lit.type_name.len && e->struct_lit.generic_args.count) {
        if (!is_struct_lit)
            ST_diag_error(&se->diag, e->line, e->col, "'" ST_sv_fmt " ' is not a generic type",
                          ST_sv_args(e->struct_lit.type_name));
        else {
            ST_tys_t args = {0};
            b8 ok = 1;
            ST_forrange(0, e->struct_lit.generic_args.count) {
                ST_ty_t *at = ST_resolve_tyexpr(se, e->struct_lit.generic_args.items[i]);
                if (!at)
                    ok = 0;
                ST_da_append_arena(se->arena, &args, at);
            }
            if (ok)
                t = ST_instantiate_struct(se, lit_tmpl->decl, args, e->line, e->col);
        }

    } else if (is_struct_lit) {
        ST_decl_t *td = lit_tmpl->decl;
        ST_ht_t bindings;
        ST_ht_init(se->arena, &bindings, 8);

        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t *fi = &e->struct_lit.inits.items[i];
            ST_ty_t *vt = ST_type_expr(se, fi->value);
            if (!vt)
                continue;
            ST_field_spec_t *fs = NULL;
            if (fi->name.len) {
                for (u32 k = 0; k < td->struct_.fields.count; ++k)
                    if (ST_string_eq(td->struct_.fields.items[i].name, fi->name)) {
                        fs = &td->struct_.fields.items[i];
                        break;
                    }
            } else if (i < td->struct_.fields.count)
                fs = &td->struct_.fields.items[i];
            if (fs && fs->te)
                ST_unify_tyexpr(se, fs->te, vt, &bindings);
        }

        ST_tys_t args = {0};
        b8 ok = 1;
        ST_forrange(0, td->struct_.generics.count) {
            ST_string_t g = td->struct_.generics.items[i];
            ST_ht_generic_t k = { .tag = g.data, .size = g.len };
            ST_ty_t *bound = (ST_ty_t *)ST_ht_get(&bindings, k).tag;
            if (!bound) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "could not infer generic parameter '$" ST_sv_fmt
                              "' for struct literal '" ST_sv_fmt "'; spell it out: "
                              ST_sv_fmt"(..){ .. }",
                              ST_sv_args(g), ST_sv_args(td->name), ST_sv_args(td->name));

                ST_diag_note(&se->diag, td->line, td->col, "'" ST_sv_fmt "' is declared here",
                             ST_sv_args(e->struct_lit.type_name));

                ok = 0;
                break;
            }
            ST_da_append_arena(se->arena, &args, bound);
        }
        if (ok)
            t = ST_instantiate_struct(se, td, args, e->line, e->col);
    }  else if (e->struct_lit.type_name.len) {
        ST_sym_t *sym = ST_sym_find_in(&se->globals, e->struct_lit.type_name);
        if (!sym)
            ST_diag_error(&se->diag, e->line, e->col,
                          "unknown type '" ST_sv_fmt "' in struct literal",
                          ST_sv_args(e->struct_lit.type_name));
        else if (sym->kind != ST_SYM_TYPE || sym->decl->kind != ST_DE_STRUCT) {
            ST_diag_error(&se->diag, e->line, e->col, "'" ST_sv_fmt "' is not a struct type",
                          ST_sv_args(e->struct_lit.type_name));
            ST_diag_note(&se->diag, sym->line, sym->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(e->struct_lit.type_name));
        } else
            t = ST_ty_for_decls(&se->tys, sym->decl);
    } else if (expect && (expect->kind == ST_TY_STRUCT || expect->kind == ST_TY_ARRAY))
        t = expect;
    else
        ST_diag_error(&se->diag, e->line, e->col,
                      "cannot infer the struct literal's type here, "
                      "name it: 'Type{ .. }'");

    if (t)
        ST_complete_ty(se, t);
    if (t && t->kind == ST_TY_ARRAY) {
        ST_ty_t *ety = t->inner;
        if (e->struct_lit.inits.count > t->count) {
            ST_diag_error(&se->diag, e->line, e->col,
                          "too many initalizers for an array of %llu elements%s",
                          (unsigned long long)t->count, t->count == 1 ? "" : "s");
        }
        ST_forrange(0, e->struct_lit.inits.count) {
            ST_field_init_t *fi = &e->struct_lit.inits.items[i];
            if (fi->name.len) {
                ST_diag_error(&se->diag, e->line, e->col,
                              "array literal do not take named initalizers.", "'(" ST_sv_fmt "')",
                              ST_sv_args(fi->name));
                continue;
            }
            ST_ty_t *vt = ST_type_expr(se, fi->value);
            if (i >= t->fields.count || !vt)
                continue;
            if (!ST_ty_coerces(se, vt, ety)) {
                ST_diag_error(&se->diag, e->line, e->col, "array element expects '%s', got '%s'.",
                              ST_tstr(se, ety), ST_tstr(se, vt));
            }
        }
        return t;
    }

    ST_forrange(0, e->struct_lit.inits.count) {
        ST_field_init_t *fi = &e->struct_lit.inits.items[i];
        ST_ty_t *vt = ST_type_expr(se, fi->value);
        if (!t)
            continue;

        ST_ty_t *ft = NULL;
        ST_string_t fname = fi->name;
        if (fi->name.len) {
            for (u32 k = 0; k < t->fields.count; k++)
                if (ST_string_eq(t->fields.items[k].name, fi->name)) {
                    ft = t->fields.items[k].ty;
                    break;
                }
            if (!ft) {
                ST_diag_error(&se->diag, fi->line, fi->col,
                              "struct '" ST_sv_fmt "' has no field '" ST_sv_fmt "'",
                              ST_sv_args(t->decl->name), ST_sv_args(fi->name));
                ST_diag_note(&se->diag, t->decl->line, t->decl->col,
                             "'" ST_sv_fmt "' is declared here", ST_sv_args(t->decl->name));
                continue;
            }
        } else if (i >= t->fields.count) {
            ST_diag_error(&se->diag, fi->line, fi->col,
                          "too many initializers for struct '" ST_sv_fmt "', it has %u field%s",
                          ST_sv_args(t->decl->name), t->fields.count,
                          t->fields.count == 1 ? "" : "s");
            ST_diag_note(&se->diag, t->decl->line, t->decl->col, "'" ST_sv_fmt "' is declared here",
                         ST_sv_args(t->decl->name));
            break;
        } else {
            ft = t->fields.items[i].ty;
            fname = t->fields.items[i].name;
        }

        if (ft && vt && !ST_ty_coerces(se, vt, ft))
            ST_diag_error(&se->diag, fi->line, fi->col,
                          "field '" ST_sv_fmt "' expects '%s', got '%s'", ST_sv_args(fname),
                          ST_tstr(se, ft), ST_tstr(se, vt));
    }
    return t;
}

static ST_ty_t *ST_type_expr(ST_sema_t *se, ST_expr_t *e) {
    if (!e)
        return NULL;
    ST_ty_t *t = NULL;
    switch (e->kind) {
        case ST_EX_INT:
            t = se->tys.untyped_int;
            break;
        case ST_EX_FLOAT:
            t = se->tys.untyped_float;
            break;
        case ST_EX_STR:
            t = se->tys.prim[ST_tstring];
            break;
        case ST_EX_CHAR:
            t = se->tys.prim[ST_tchar];
            break;
        case ST_EX_BOOL:
            t = se->tys.prim[ST_tbool];
            break;
        case ST_EX_NULL:
            t = se->tys.null_ptr;
            break;
        case ST_EX_IDENT:
            t = ST_type_ident(se, e);
            break;
        case ST_EX_UNARY:
            t = ST_type_unary(se, e);
            break;
        case ST_EX_BINARY:
            t = ST_type_binary(se, e);
            break;
        case ST_EX_CALL: {
            ST_tys_t *rets = ST_type_call(se, e);
            if (!rets)
                break;
            if (rets->count == 0) {
                t = se->tys.prim[ST_tvoid];
                break;
            }
            if (rets->count > 1)
                ST_diag_error(&se->diag, e->line, e->col,
                              "call returns %u values, bind them all: 'a, b := f()'", rets->count);
            t = rets->items[0];
            break;
        }
        case ST_EX_FIELD:
            t = ST_type_field(se, e);
            break;
        case ST_EX_INDEX:
            t = ST_type_index(se, e);
            break;
        case ST_EX_CAST:
            t = ST_type_cast(se, e);
            break;
        case ST_EX_STRUCT_LIT:
            t = ST_type_struct_lit(se, e, NULL);
            break;
        case ST_EX_ARRAY_NEW:
            t = ST_resolve_tyexpr(se, e->array_new.te);
            break;
        case ST_EX_SIZEOF: {
            ST_ty_t *st = ST_resolve_tyexpr(se, e->tyop.te);
            if (st)
                ST_complete_ty(se, st);
            t = se->tys.untyped_int;
            break;
        }
        case ST_EX_TYPEOF:
            ST_type_expr(se, e->tyop.operand);
            t = se->tys.prim[ST_tu64]; // type id, until RTTI lands
            break;
        case ST_EX_TYPEINFO:
            if (e->tyop.te)
                ST_resolve_tyexpr(se, e->tyop.te);
            else
                ST_type_expr(se, e->tyop.operand);
            t = ST_ty_ptr(&se->tys, se->tys.prim[ST_tvoid]); // *Type_Info later
            break;
        case ST_EX_KIND:
            ST_type_expr(se, e->tyop.operand);
            t = se->tys.prim[ST_tu64];
            break;
        case ST_EX_CSTR: {
            ST_ty_t *ot = ST_type_expr(se, e->tyop.operand);
            if (ot && ot->kind != ST_TY_STRING)
                ST_diag_error(&se->diag, e->line, e->col, "'cstr' needs a 'string', got '%s'",
                              ST_tstr(se, ot));
            t = ST_ty_ptr(&se->tys, se->tys.prim[ST_tchar]);
            break;
        }
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
    e->ty = t;
    return t;
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s);

static void ST_check_body(ST_sema_t *se, ST_stmts_t *body) {
    ST_scope_push(se);
    ST_forrange(0, body->count) ST_check_stmt(se, body->items[i]);
    ST_scope_pop(se);
}

static void ST_check_cond(ST_sema_t *se, ST_expr_t *cond, const char *what) {
    ST_ty_t *t = ST_type_expr(se, cond);
    if (t && !ST_ty_is_bool(t))
        ST_diag_error(&se->diag, cond->line, cond->col, "%s condition must be 'bool', got '%s'",
                      what, ST_tstr(se, t));
}

static void ST_check_decl_stmt(ST_sema_t *se, ST_stmt_t *s) {
    b8 infer_count = s->decl.te && s->decl.te->kind == ST_TE_ARRAY && !s->decl.te->is_dynamic &&
                     !s->decl.te->count_expr;
    ST_ty_t *dt = NULL;
    if (s->decl.te && !infer_count)
        dt = ST_resolve_tyexpr(se, s->decl.te);
    if (dt)
        ST_complete_ty(se, dt);
    if (infer_count) {
        ST_ty_t *ety = ST_resolve_tyexpr(se, s->decl.te->inner);
        b8 has_lit = s->decl.init && s->decl.init->kind == ST_EX_STRUCT_LIT &&
                     !s->decl.init->struct_lit.type_name.len;
        if (ety && has_lit) {
            ST_complete_ty(se, ety);
            dt = ST_ty_array(&se->tys, ety, s->decl.init->struct_lit.inits.count);
        } else if (ety)
            ST_diag_error(&se->diag, s->line, s->col,
                          "cannot infer the size of '" ST_sv_fmt "' give it an"
                          "explicit count '[N]T' or initalize it with an array"
                          "iteral '{....}'.",
                          ST_sv_args(s->decl.name));
    }

    ST_ty_t *it = NULL;
    if (s->decl.init) {
        // unnamed struct literal takes the annotated type
        if (s->decl.init->kind == ST_EX_STRUCT_LIT && !s->decl.init->struct_lit.type_name.len &&
            dt) {
            it = ST_type_struct_lit(se, s->decl.init, dt);
            s->decl.init->ty = it;
        } else
            it = ST_type_expr(se, s->decl.init);
    }

    if (dt && it && !ST_ty_coerces(se, it, dt))
        ST_diag_error(&se->diag, s->line, s->col,
                      "cannot initialize '" ST_sv_fmt "' of type '%s' "
                      "with a value of type '%s'",
                      ST_sv_args(s->decl.name), ST_tstr(se, dt), ST_tstr(se, it));

    if (!dt && !s->decl.init)
        ST_diag_error(&se->diag, s->line, s->col, "'" ST_sv_fmt "' needs a type or an initializer",
                      ST_sv_args(s->decl.name));

    ST_ty_t *t = dt ? dt : ST_ty_defaulted(se, it);
    if (t && t->kind == ST_TY_VOID)
        ST_diag_error(&se->diag, s->line, s->col, "cannot declare '" ST_sv_fmt "' of type 'void'",
                      ST_sv_args(s->decl.name));
    ST_declare_local(se, s->decl.name, t, s->line, s->col);
}

static void ST_check_assign(ST_sema_t *se, ST_stmt_t *s) {
    ST_ty_t *lt = ST_type_expr(se, s->assign.lhs);
    ST_ty_t *rt = ST_type_expr(se, s->assign.rhs);
    if (!lt || !rt)
        return;
    ST_expr_t *lhs = s->assign.lhs;
    if (lhs->kind == ST_EX_INDEX && lhs->index.base->ty &&
        lhs->index.base->ty->kind == ST_TY_STRING) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "strings are immutable; cannot assign into a string");
        return;
    }
    if (lhs->kind == ST_EX_FIELD && lhs->field.base->ty &&
        lhs->field.base->ty->kind == ST_TY_STRING) {
        ST_diag_error(&se->diag, s->line, s->col,
                      "'" ST_sv_fmt "' of a string is read-only; "
                      "reassign the whole string instead",
                      ST_sv_args(lhs->field.name));
        return;
    }
    ST_string_t op = s->assign.op;

    if (ST_string_eq_cstr(op, "=")) {
        if (!ST_ty_coerces(se, rt, lt))
            ST_diag_error(&se->diag, s->line, s->col, "cannot assign '%s' to '%s'", ST_tstr(se, rt),
                          ST_tstr(se, lt));
        return;
    }

    // compound: `+=` `-=` `*=` `/=` need numeric, the rest need integers
    b8 arith = ST_op_is(op, "+=", "-=") || ST_op_is(op, "*=", "/=");
    if (arith) {
        if (!ST_ty_num_unify(se, lt, rt))
            ST_diag_error(&se->diag, s->line, s->col,
                          "invalid operands to '" ST_sv_fmt "': '%s' and '%s'", ST_sv_args(op),
                          ST_tstr(se, lt), ST_tstr(se, rt));
        return;
    }
    if (!ST_ty_is_int(lt) || !ST_ty_is_int(rt))
        ST_diag_error(&se->diag, s->line, s->col,
                      "'" ST_sv_fmt "' needs integer operands, got '%s' and '%s'", ST_sv_args(op),
                      ST_tstr(se, lt), ST_tstr(se, rt));
}

static void ST_check_multi(ST_sema_t *se, ST_stmt_t *s) {
    // a, b := f()  -> a single call producing every value
    b8 from_call = s->multi.n_names > 1 && s->multi.values.count == 1 &&
                   s->multi.values.items[0]->kind == ST_EX_CALL;

    ST_ty_t *tys[16] = {0};
    u32 n_tys = 0;

    if (from_call) {
        ST_expr_t *call = s->multi.values.items[0];
        ST_tys_t *rets = ST_type_call(se, call);
        if (rets) {
            if (rets->count != s->multi.n_names)
                ST_diag_error(&se->diag, s->line, s->col,
                              "call returns %u value%s, but %u name%s bound", rets->count,
                              rets->count == 1 ? "" : "s", s->multi.n_names,
                              s->multi.n_names == 1 ? " is" : "s are");
            ST_forrange(0, rets->count) {
                if (n_tys >= ST_array_len(tys))
                    break;
                tys[n_tys++] = rets->items[i];
            }
            call->ty = rets->count ? rets->items[0] : se->tys.prim[ST_tvoid];
        }
    } else {
        if (s->multi.values.count != s->multi.n_names)
            ST_diag_error(&se->diag, s->line, s->col, "expected %u value%s, got %u",
                          s->multi.n_names, s->multi.n_names == 1 ? "" : "s",
                          s->multi.values.count);
        ST_forrange(0, s->multi.values.count) {
            ST_ty_t *t = ST_type_expr(se, s->multi.values.items[i]);
            if (n_tys < ST_array_len(tys))
                tys[n_tys++] = t;
        }
    }

    ST_forrange(0, s->multi.n_names) {
        ST_ty_t *t = i < n_tys ? tys[i] : NULL;
        if (s->multi.declare)
            ST_declare_local(se, s->multi.names[i], ST_ty_defaulted(se, t), s->line, s->col);
        else {
            ST_sym_t *sym = ST_sym_find(se, s->multi.names[i]);
            if (!sym)
                ST_diag_error(&se->diag, s->line, s->col,
                              "use of undeclared identifier '" ST_sv_fmt "'",
                              ST_sv_args(s->multi.names[i]));
            else if (sym->t && t && !ST_ty_coerces(se, t, sym->t))
                ST_diag_error(&se->diag, s->line, s->col,
                              "cannot assign '%s' to '" ST_sv_fmt "' of type '%s'", ST_tstr(se, t),
                              ST_sv_args(s->multi.names[i]), ST_tstr(se, sym->t));
        }
    }
}

static void ST_check_return(ST_sema_t *se, ST_stmt_t *s) {
    u32 want = se->cur_rets ? se->cur_rets->count : 0;
    u32 got = s->ret.values.count;
    if (want != got) {
        if (want == 0)
            ST_diag_error(&se->diag, s->line, s->col, "this function does not return a value");
        else
            ST_diag_error(&se->diag, s->line, s->col, "this function returns %u value%s, got %u",
                          want, want == 1 ? "" : "s", got);
    }
    ST_forrange(0, got) {
        ST_ty_t *t = ST_type_expr(se, s->ret.values.items[i]);
        if (i >= want || !se->cur_rets)
            continue;
        ST_ty_t *rt = se->cur_rets->items[i];
        if (t && rt && !ST_ty_coerces(se, t, rt))
            ST_diag_error(&se->diag, s->ret.values.items[i]->line, s->ret.values.items[i]->col,
                          "return value %u expects '%s', got '%s'", i + 1, ST_tstr(se, rt),
                          ST_tstr(se, t));
    }
}

static void ST_check_stmt(ST_sema_t *se, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            if (s->expr && s->expr->kind == ST_EX_CALL)
                ST_type_call(se, s->expr); // rets may be ignored as a statement
            else
                ST_type_expr(se, s->expr);
            break;
        case ST_ST_DECL:
            ST_check_decl_stmt(se, s);
            break;
        case ST_ST_ASSIGN:
            ST_check_assign(se, s);
            break;
        case ST_ST_MULTI_BIND:
            ST_check_multi(se, s);
            break;
        case ST_ST_IF:
            ST_check_cond(se, s->if_.cond, "'if'");
            ST_check_body(se, &s->if_.then_body);
            ST_check_stmt(se, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH: {
            ST_ty_t *ct = ST_type_expr(se, s->switch_.cond);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                for (u32 k = 0; k < c->values.count; k++) {
                    ST_ty_t *vt = ST_type_expr(se, c->values.items[k]);
                    if (ct && vt && !ST_ty_coerces(se, vt, ct) && !ST_ty_coerces(se, ct, vt) &&
                        !ST_ty_num_unify(se, ct, vt))
                        ST_diag_error(&se->diag, c->values.items[k]->line, c->values.items[k]->col,
                                      "case of type '%s' cannot match a '%s' switch",
                                      ST_tstr(se, vt), ST_tstr(se, ct));
                }
                ST_check_body(se, &c->body);
            }
            break;
        }
        case ST_ST_WHILE:
            ST_check_cond(se, s->while_.cond, "'while'");
            ST_check_body(se, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE: {
            ST_ty_t *lo = ST_type_expr(se, s->for_range.lo);
            ST_ty_t *hi = ST_type_expr(se, s->for_range.hi);
            if (lo && !ST_ty_is_int(lo))
                ST_diag_error(&se->diag, s->for_range.lo->line, s->for_range.lo->col,
                              "range bound must be an integer, got '%s'", ST_tstr(se, lo));
            if (hi && !ST_ty_is_int(hi))
                ST_diag_error(&se->diag, s->for_range.hi->line, s->for_range.hi->col,
                              "range bound must be an integer, got '%s'", ST_tstr(se, hi));
            ST_ty_t *iter = lo && hi ? ST_ty_num_unify(se, lo, hi) : NULL;
            ST_ty_t *decl_ty = iter ? iter : lo;
            if (s->for_range.iter_te) {
                ST_ty_t *annotated = ST_resolve_tyexpr(se, s->for_range.iter_te);
                if (annotated && !ST_ty_is_int(annotated))
                    ST_diag_error(&se->diag, s->for_range.iter_te->line, s->for_range.iter_te->col,
                                  "range iterator must be an integer, got '%s'",
                                  ST_tstr(se, annotated));
                else if (annotated) {
                    if (lo && !ST_ty_coerces(se, lo, annotated))
                        ST_diag_error(&se->diag, s->for_range.iter_te->line,
                                      s->for_range.iter_te->col,
                                      "range start of type '%s' does not fit iterator type '%s'",
                                      ST_tstr(se, lo), ST_tstr(se, annotated));
                    if (hi && !ST_ty_coerces(se, hi, annotated))
                        ST_diag_error(&se->diag, s->for_range.iter_te->line,
                                      s->for_range.iter_te->col,
                                      "range end of type '%s' does not fit iterator type '%s'",
                                      ST_tstr(se, hi), ST_tstr(se, annotated));
                    decl_ty = annotated;
                }
            }
            ST_scope_push(se);
            ST_declare_local(se, s->for_range.iter, ST_ty_defaulted(se, decl_ty), s->line, s->col);
            ST_forrange(0, s->for_range.body.count) ST_check_stmt(se, s->for_range.body.items[i]);
            ST_scope_pop(se);
            break;
        }
        case ST_ST_FOR_ARRAY: {
            ST_ty_t *tt = ST_type_expr(se, s->for_array.target);
            ST_ty_t *iter = NULL;
            if (tt) {
                if (tt->kind == ST_TY_ARRAY || tt->kind == ST_TY_DYN_ARRAY)
                    iter = tt->inner;
                else if (tt->kind == ST_TY_STRING)
                    iter = se->tys.prim[ST_tchar];
                else
                    ST_diag_error(&se->diag, s->for_array.target->line, s->for_array.target->col,
                                  "cannot iterate a value of type '%s'", ST_tstr(se, tt));
            }
            ST_scope_push(se);
            ST_declare_local(se, s->for_array.iter, iter, s->line, s->col);
            ST_forrange(0, s->for_array.body.count) ST_check_stmt(se, s->for_array.body.items[i]);
            ST_scope_pop(se);
            break;
        }
        case ST_ST_RETURN:
            ST_check_return(se, s);
            break;
        case ST_ST_BLOCK:
            ST_check_body(se, &s->block);
            break;
        case ST_ST_DEFER:
            ST_check_stmt(se, s->defer_stmt);
            break;
        case ST_ST_BREAK:
        case ST_ST_CONTINUE:
            break;
        case ST_ST_LABEL:
            break;
        case ST_ST_GODOWN:
            if (!ST_sym_find_in(se->labels, s->label))
                ST_diag_error(&se->diag, s->line, s->col, "goto to unknown label '" ST_sv_fmt "'",
                              ST_sv_args(s->label));
            break;
        case ST_ST_ASM:
            break;
        case ST_ST_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_collect_labels(ST_sema_t *se, ST_ht_t *labels, ST_stmts_t *body) {
    ST_forrange(0, body->count) {
        ST_stmt_t *s = body->items[i];
        if (!s)
            continue;
        switch (s->kind) {
            case ST_ST_LABEL: {
                ST_sym_t *prev = ST_sym_find_in(labels, s->label);
                if (prev) {
                    ST_diag_error(&se->diag, s->line, s->col, "duplicate label '" ST_sv_fmt "'",
                                  ST_sv_args(s->label));
                    ST_diag_note(&se->diag, prev->line, prev->col, "previous label is here");
                    break;
                }
                ST_sym_insert(se, labels,
                              ST_sym_new(se, ST_SYM_VAR, s->label, NULL, NULL, s->line, s->col));
                break;
            }
            case ST_ST_IF:
                ST_collect_labels(se, labels, &s->if_.then_body);
                if (s->if_.else_stmt) {
                    ST_stmts_t one = {.items = &s->if_.else_stmt, .count = 1};
                    ST_collect_labels(se, labels, &one);
                }
                break;
            case ST_ST_SWITCH:
                for (u32 k = 0; k < s->switch_.cases.count; k++)
                    ST_collect_labels(se, labels, &s->switch_.cases.items[k].body);
                break;
            case ST_ST_WHILE:
                ST_collect_labels(se, labels, &s->while_.body);
                break;
            case ST_ST_FOR_RANGE:
                ST_collect_labels(se, labels, &s->for_range.body);
                break;
            case ST_ST_FOR_ARRAY:
                ST_collect_labels(se, labels, &s->for_array.body);
                break;
            case ST_ST_BLOCK:
                ST_collect_labels(se, labels, &s->block);
                break;
            case ST_ST_DEFER: {
                ST_stmts_t one = {.items = &s->defer_stmt, .count = 1};
                ST_collect_labels(se, labels, &one);
                break;
            }
            case ST_ST_EXPR:
            case ST_ST_DECL:
            case ST_ST_ASSIGN:
            case ST_ST_MULTI_BIND:
            case ST_ST_RETURN:
            case ST_ST_BREAK:
            case ST_ST_CONTINUE:
            case ST_ST_GODOWN:
            case ST_ST_ASM:
                break;
            case ST_ST_COUNT:
                ST_assert(0);
                break;
        }
    }
}

static ST_sym_kind_t ST_decl_sym_kind(ST_decl_t *d) {
    switch (d->kind) {
        case ST_DE_STRUCT:
        case ST_DE_ENUM:
        case ST_DE_TAG_UNION:
            return ST_SYM_TYPE;
        case ST_DE_FN:
        case ST_DE_EXTERN_FN:
            return ST_SYM_FN;
        case ST_DE_CONST:
            return ST_SYM_CONST;
        case ST_DE_EXTERN_VAR:
            return ST_SYM_EXTERN_VAR;
        case ST_DE_COUNT:
            ST_assert(0);
            break;
    }
    return ST_SYM_VAR;
}

// Pass 1: register every top-level name.
static void ST_sema_collect(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        b8 is_generic_struct = d->kind == ST_DE_STRUCT && d->struct_.generics.count;
        b8 is_generic_fn = d->kind == ST_DE_FN && d->fn.sig.generics.count;

        if (is_generic_struct || is_generic_fn) {
            ST_sym_t *prev = ST_sym_find_in(&se->templates, d->name);
            if (prev) {
                ST_diag_error(&se->diag, d->line, d->col, "redefinition of '" ST_sv_fmt "'",
                              ST_sv_args(d->name));
                ST_diag_note(&se->diag, prev->line, prev->col, "previous definition is here");
                continue;
            }
            ST_sym_insert(se, &se->templates,
                          ST_sym_new(se, is_generic_fn ? ST_SYM_FN : ST_SYM_TYPE,
                                     d->name, d, NULL, d->line, d->col));

            if (is_generic_fn)
                continue;
        }
        ST_sym_t *prev = ST_sym_find_in(&se->globals, d->name);
        if (prev) {
            b8 prev_proto =
                prev->decl && prev->decl->kind == ST_DE_FN && prev->decl->fn.is_prototype;
            b8 cur_proto = d->kind == ST_DE_FN && d->fn.is_prototype;
            b8 both_fn = prev->decl && prev->decl->kind == ST_DE_FN && d->kind == ST_DE_FN;
            if (both_fn && (prev_proto ^ cur_proto)) {
                if (prev_proto) {
                    prev->decl = d;
                    prev->line = d->line;
                    prev->col = d->col;
                }
                continue;
            }
            ST_diag_error(&se->diag, d->line, d->col, "redefinition of '" ST_sv_fmt "'",
                          ST_sv_args(d->name));
            ST_diag_note(&se->diag, prev->line, prev->col, "previous definition is here");
            continue;
        }
        ST_sym_insert(se, &se->globals,
                      ST_sym_new(se, ST_decl_sym_kind(d), d->name, d, NULL, d->line, d->col));
    }
}

static void ST_check_dup_fields(ST_sema_t *se, ST_decl_t *d) {
    ST_forrange(0, d->struct_.fields.count) {
        ST_field_spec_t *f = &d->struct_.fields.items[i];
        for (u32 k = 0; k < i; k++)
            if (ST_string_eq(d->struct_.fields.items[k].name, f->name) && f->name.len) {
                ST_diag_error(&se->diag, f->line, f->col,
                              "duplicate field '" ST_sv_fmt "' in struct '" ST_sv_fmt "'",
                              ST_sv_args(f->name), ST_sv_args(d->name));
                ST_diag_note(&se->diag, d->struct_.fields.items[k].line,
                             d->struct_.fields.items[k].col, "previous field is here");
                break;
            }
    }
}

// Builds the ST_ty_t for a function signature and attaches it to the symbol.
static void ST_build_fn_ty(ST_sema_t *se, ST_sym_t *sym, ST_fn_sig_t *sig) {
    ST_ty_t *t = ST_ty_fn_new(&se->tys);
    t->is_variadic = sig->is_variadic;
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_ty_t *pt = p->te ? ST_resolve_tyexpr(se, p->te) : NULL;
        if (!pt && p->def)
            pt = ST_ty_defaulted(se, ST_type_expr(se, p->def));
        else if (p->def) {
            ST_ty_t *dt = ST_type_expr(se, p->def);
            if (pt && dt && !ST_ty_coerces(se, dt, pt))
                ST_diag_error(&se->diag, p->line, p->col,
                              "default for '" ST_sv_fmt "' expects '%s', got '%s'",
                              ST_sv_args(p->name), ST_tstr(se, pt), ST_tstr(se, dt));
        }
        ST_da_append_arena(se->arena, &t->params, pt);
    }
    ST_forrange(0, sig->rets.count) {
        ST_ty_t *rt = ST_resolve_tyexpr(se, sig->rets.items[i]);
        if (rt && rt->kind == ST_TY_VOID)
            continue;
        ST_da_append_arena(se->arena, &t->rets, rt);
    }
    sym->t = t;
}

// Pass 2: make types for type declarations, lay them out, build signatures,
// and type constants and extern variables.
static void ST_sema_types(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue; // redefinition, already reported
        switch (d->kind) {
            case ST_DE_STRUCT:
                ST_check_dup_fields(se, d);
                sym->t = ST_ty_for_decls(&se->tys, d);
                break;
            case ST_DE_ENUM:
            case ST_DE_TAG_UNION:
                sym->t = ST_ty_for_decls(&se->tys, d);
                break;
            case ST_DE_CONST:
            case ST_DE_EXTERN_FN:
            case ST_DE_EXTERN_VAR:
            case ST_DE_FN:
                break;
            case ST_DE_COUNT:
                ST_assert(0);
                break;
        }
    }

    // layout after every type name is known, so structs can reference each
    // other
    for (u32 i = 0; i < prog->decls.count; i++) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue;
        switch (d->kind) {
            case ST_DE_STRUCT:
                if (d->struct_.generics.count)
                    break;
                ST_complete_ty(se, sym->t);
                break;
            case ST_DE_TAG_UNION:
                ST_complete_ty(se, sym->t);
                break;
            case ST_DE_ENUM:
                break; // fixed 8-byte layout, values are checked with their uses
            case ST_DE_CONST:
                ST_ty_of_const(se, sym);
                break;
            case ST_DE_EXTERN_VAR:
                sym->t = ST_resolve_tyexpr(se, d->extern_var.te);
                break;
            case ST_DE_EXTERN_FN:
                ST_build_fn_ty(se, sym, &d->extern_fn.sig);
                break;
            case ST_DE_FN:
                ST_build_fn_ty(se, sym, &d->fn.sig);
                break;
            case ST_DE_COUNT:
                ST_assert(0);
                break;
        }
    }
}

static void ST_check_fn_body(ST_sema_t *se, ST_sym_t *sym, ST_decl_t *d) {
    ST_fn_sig_t *sig = &d->fn.sig;
    ST_ty_t *fnty = sym->t;

    ST_ht_t *save_bindings = se->generic_bindings;
    b8 save_stamp = se->stamp_tyexprs;
    if (sym->generic_bindings) {
        se->generic_bindings = sym->generic_bindings;
        se->stamp_tyexprs = 1;
    }

    ST_ht_t labels;
    ST_ht_init(se->arena, &labels, 8);
    se->labels = &labels;
    if (!d->fn.is_prototype)
        ST_collect_labels(se, &labels, &d->fn.body);

    ST_scope_push(se);
    ST_forrange(0, sig->params.count) {
        ST_param_t *p = &sig->params.items[i];
        ST_ty_t *pt = fnty && i < fnty->params.count ? fnty->params.items[i] : NULL;
        ST_declare_local(se, p->name, pt, p->line, p->col);
    }

    se->cur_rets = fnty ? &fnty->rets : NULL;
    if (!d->fn.is_prototype)
        ST_forrange(0, d->fn.body.count) ST_check_stmt(se, d->fn.body.items[i]);
    se->cur_rets = NULL;

    ST_scope_pop(se);
    se->labels = NULL;

    se->stamp_tyexprs = save_stamp;
    se->generic_bindings = save_bindings;
}

// Pass 3: walk every function body with the full typed environment.
static void ST_sema_check(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        ST_sym_t *sym = ST_sym_find_in(&se->globals, d->name);
        if (!sym || sym->decl != d)
            continue;
        if (d->kind == ST_DE_FN)
            ST_check_fn_body(se, sym, d);
    }
}

// Pass 4 Typechecking

static void ST_default_expr(ST_sema_t *se, ST_expr_t *e);
static void ST_default_exprs(ST_sema_t *se, ST_exprs_t *es) {
    ST_forrange(0, es->count) ST_default_expr(se, es->items[i]);
}

static void ST_default_body(ST_sema_t *se, ST_stmts_t *body);
static void ST_default_stmt(ST_sema_t *se, ST_stmt_t *s) {
    if (!s)
        return;
    switch (s->kind) {
        case ST_ST_EXPR:
            ST_default_expr(se, s->expr);
            break;
        case ST_ST_DECL:
            ST_default_expr(se, s->decl.init);
            break;
        case ST_ST_ASSIGN:
            ST_default_expr(se, s->assign.lhs);
            ST_default_expr(se, s->assign.rhs);
            break;
        case ST_ST_MULTI_BIND:
            ST_default_exprs(se, &s->multi.values);
            break;
        case ST_ST_IF:
            ST_default_expr(se, s->if_.cond);
            ST_default_body(se, &s->if_.then_body);
            ST_default_stmt(se, s->if_.else_stmt);
            break;
        case ST_ST_SWITCH:
            ST_default_expr(se, s->switch_.cond);
            ST_forrange(0, s->switch_.cases.count) {
                ST_case_t *c = &s->switch_.cases.items[i];
                ST_default_exprs(se, &c->values);
                ST_default_body(se, &c->body);
            }
            break;
        case ST_ST_WHILE:
            ST_default_expr(se, s->while_.cond);
            ST_default_body(se, &s->while_.body);
            break;
        case ST_ST_FOR_RANGE:
            ST_default_expr(se, s->for_range.lo);
            ST_default_expr(se, s->for_range.hi);
            ST_default_body(se, &s->for_range.body);
            break;
        case ST_ST_FOR_ARRAY:
            ST_default_expr(se, s->for_array.target);
            ST_default_body(se, &s->for_array.body);
            break;
        case ST_ST_RETURN:
            ST_default_exprs(se, &s->ret.values);
            break;
        case ST_ST_BLOCK:
            ST_default_body(se, &s->block);
            break;
        case ST_ST_DEFER:
            ST_default_stmt(se, s->defer_stmt);
            break;
        case ST_ST_BREAK:
        case ST_ST_CONTINUE:
        case ST_ST_LABEL:
        case ST_ST_GODOWN:
        case ST_ST_ASM:
            break;
        case ST_ST_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_default_body(ST_sema_t *se, ST_stmts_t *body) {
    ST_forrange(0, body->count) ST_default_stmt(se, body->items[i]);
}

static void ST_default_expr(ST_sema_t *se, ST_expr_t *e) {
    if (!e)
        return;
    e->ty = ST_ty_defaulted(se, e->ty);
    switch (e->kind) {
        case ST_EX_INT:
        case ST_EX_FLOAT:
        case ST_EX_STR:
        case ST_EX_CHAR:
        case ST_EX_BOOL:
        case ST_EX_NULL:
        case ST_EX_IDENT:
        case ST_EX_ARRAY_NEW:
        case ST_EX_SIZEOF:
            break;
        case ST_EX_UNARY:
            ST_default_expr(se, e->unary.operand);
            break;
        case ST_EX_BINARY:
            ST_default_expr(se, e->bin.l);
            ST_default_expr(se, e->bin.r);
            break;
        case ST_EX_CALL:
            ST_default_expr(se, e->call.callee);
            ST_forrange(0, e->call.args.count) ST_default_expr(se, e->call.args.items[i].value);
            break;
        case ST_EX_FIELD:
            ST_default_expr(se, e->field.base);
            break;
        case ST_EX_INDEX:
            ST_default_expr(se, e->index.base);
            ST_default_expr(se, e->index.index);
            break;
        case ST_EX_CAST:
            ST_default_expr(se, e->cast.operand);
            break;
        case ST_EX_STRUCT_LIT:
            ST_forrange(0, e->struct_lit.inits.count)
                ST_default_expr(se, e->struct_lit.inits.items[i].value);
            break;
        case ST_EX_TYPEOF:
        case ST_EX_KIND:
        case ST_EX_CSTR:
            ST_default_expr(se, e->tyop.operand);
            break;
        case ST_EX_TYPEINFO:
            if (!e->tyop.te)
                ST_default_expr(se, e->tyop.operand);
            break;
        case ST_EX_COUNT:
            ST_assert(0);
            break;
    }
}

static void ST_sema_default_types(ST_sema_t *se, ST_program_t *prog) {
    ST_forrange(0, prog->decls.count) {
        ST_decl_t *d = prog->decls.items[i];
        if (!d)
            continue;
        if (d->kind == ST_DE_FN && d->fn.sig.generics.count)
                continue;

        switch (d->kind) {
            case ST_DE_CONST:
                ST_default_expr(se, d->const_.value);
                break;
            case ST_DE_FN:
                ST_forrange(0, d->fn.sig.params.count)
                    ST_default_expr(se, d->fn.sig.params.items[i].def);
                if (!d->fn.is_prototype)
                    ST_default_body(se, &d->fn.body);
                break;
            case ST_DE_EXTERN_FN:
                ST_forrange(0, d->extern_fn.sig.params.count)
                    ST_default_expr(se, d->extern_fn.sig.params.items[i].def);
            case ST_DE_STRUCT:
            case ST_DE_ENUM:
            case ST_DE_TAG_UNION:
            case ST_DE_EXTERN_VAR:
            case ST_DE_COUNT:
                break;
        }
    }
}

b8 ST_sema_run(ST_arena_t *arena, ST_program_t *prog, ST_string_t src, ST_string_t file,
               ST_sema_t *out) {
    ST_sema_t *se = out;
    *se = (ST_sema_t){0};
    se->arena = arena;
    se->diag.src = src;
    se->diag.file = file;
    se->diag.max_errors = ST_SEMA_MAX_ERRORS;
    ST_ht_init(arena, &se->globals, 64);
    ST_ht_init(arena, &se->templates, 16);
    ST_ht_init(arena, &se->instantiations, 16);
    ST_ht_init(arena, &se->fn_instantiations, 16);
    ST_ht_init(arena, &se->inst_info, 16);
    se->prog = prog;
    ST_ty_ctx_init(&se->tys, arena);
    ST_forrange(0, ST_array_len(ST_builtin_fns)) {
        ST_string_t name = ST_cstr_to_str((char *)ST_builtin_fns[i]);
        ST_sym_insert(se, &se->globals, ST_sym_new(se, ST_SYM_FN, name, NULL, NULL, 0, 0));
    }
    ST_sema_collect(se, prog);
    ST_sema_types(se, prog);
    ST_sema_check(se, prog);
    ST_sema_default_types(se, prog);
    return se->diag.n_errors == 0;
}
