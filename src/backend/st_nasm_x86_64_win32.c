#include "st_nasm.h"

// https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
static const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
#define ST_N_ARG_REGS ((u32)ST_array_len(arg_regs))
static const char *xmm_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
#define ST_N_XMM_REGS ((u32)ST_array_len(xmm_regs))

typedef struct {
    ST_ir_fn_t *fn;
    u32 hidden_ret_off;
    u32 next_int_arg, next_float_arg;
} ST_gen_ctx_t;

static void ST_generate_strs(FILE *out, ST_ir_module_t *m) {
    if (!m->strs.count)
        return;
    // https://www.nasm.us/doc/nasm09.html
    fprintf(out, "\nsection .rdata\n");
    ST_forrange(0, m->strs.count) {
        ST_string_t s = m->strs.items[i];
        fprintf(out, "str_%u_data: db ", (u32)i);
        for (u32 j = 0; j < s.len; j++) {
            fprintf(out, "0x%02x, ", (u8)s.data[j]);
        }
        fprintf(out, "0\n");
        fprintf(out, "align 8\n");
        fprintf(out, "str_%u:\n", (u32)i);
        fprintf(out, "    dq str_%u_data\n", (u32)i);
        fprintf(out, "    dq %u\n", (u32)s.len);
    }
}

static void ST_generate_globals(FILE *out, ST_ir_module_t *m) {
    if (!m->globals.count)
        return;
    b8 any_init = 0, any_uninit = 0;
    ST_forrange(0, m->globals.count) {
        if (m->globals.items[i].has_init)
            any_init = 1;
        else
            any_uninit = 1;
    }
    if (any_init) {
        fprintf(out, "\nsection .data\n");
        ST_forrange(0, m->globals.count) {
            ST_ir_global_var_t *g = &m->globals.items[i];
            if (!g->has_init)
                continue;
            u32 size = g->ty && g->ty->size ? g->ty->size : 8;
            u32 align = g->ty && g->ty->align ? g->ty->align : 8;
            if (g->is_pub)
                fprintf(out, "global " ST_sv_fmt "\n", ST_sv_args(g->name));
            fprintf(out, "align %u\n", align);
            fprintf(out, ST_sv_fmt ":\n", ST_sv_args(g->name));
            if (g->init_is_float) {
                if (size == 4) {
                    u32 bits;
                    float f = (float)g->init_float;
                    memcpy(&bits, &f, sizeof(bits));
                    fprintf(out, "    dd 0x%08x\n", bits);
                } else {
                    u64 bits;
                    memcpy(&bits, &g->init_float, sizeof(bits));
                    fprintf(out, "    dq 0x%016llx\n", (unsigned long long)bits);
                }
            } else {
                switch (size) {
                case 1:
                    fprintf(out, "    db %lld\n", (long long)g->init_int);
                    break;
                case 2:
                    fprintf(out, "    dw %lld\n", (long long)g->init_int);
                    break;
                case 4:
                    fprintf(out, "    dd %lld\n", (long long)g->init_int);
                    break;
                default:
                    fprintf(out, "    dq %lld\n", (long long)g->init_int);
                    break;
                }
            }
        }
    }
    if (any_uninit) {
        fprintf(out, "\nsection .bss\n");
        ST_forrange(0, m->globals.count) {
            ST_ir_global_var_t *g = &m->globals.items[i];
            if (g->has_init)
                continue;
            u32 size = g->ty && g->ty->size ? g->ty->size : 8;
            u32 align = g->ty && g->ty->align ? g->ty->align : 8;
            if (g->is_pub)
                fprintf(out, "global " ST_sv_fmt "\n", ST_sv_args(g->name));
            fprintf(out, "align %u\n", align);
            fprintf(out, ST_sv_fmt ":\n", ST_sv_args(g->name));
            fprintf(out, "    resb %u\n", size);
        }
    }
}

static void ST_generate_inst(FILE *out, ST_gen_ctx_t *ctx, ST_ir_inst_t *in) {
    _Static_assert(ST_IR_COUNT == 51, "IR count exceeded");
    ST_todo("ST_generate_inst");
}

static void ST_generate_term(FILE *out, ST_gen_ctx_t *ctx, ST_ir_block_t *b) {
    ST_todo("ST_generate_term");
}

static u32 ST_call_ret_count(ST_ir_inst_t *call_inst) {
    if (call_inst->call.callee && call_inst->call.callee->ty)
        return call_inst->call.callee->ty->rets.count;
    return 1;
}

static u32 ST_layout_fn(ST_ir_fn_t *fn, ST_gen_ctx_t *ctx) {
    ST_todo("ST_layout_fn");
}

static void ST_generate_fn(FILE *out, ST_ir_fn_t *fn) {
    ST_todo("ST_generate_fn");
}

b8 ST_nasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src, ST_string_t file, b8 emit_entry) {
    ST_unused(src);
    ST_unused(file);
    if (out == NULL)
        out = stdout;
    fprintf(out, "BITS 64\n");
    fprintf(out, "default rel\n");
    fprintf(out, "extern ExitProcess\n");
    fprintf(out, "extern _CRT_INIT\n");
    ST_generate_strs(out, m);
    ST_generate_globals(out, m);

    fprintf(out, "section .text\n");
    ST_forrange(0, m->fns.count) {
        ST_ir_fn_t *fn = m->fns.items[i];
        if (fn->is_extern) {
            fprintf(out, "extern " ST_sv_fmt "\n", ST_sv_args(fn->name));
            continue;
        }
        if (fn->is_pub) {
            fprintf(out, "global " ST_sv_fmt "\n", ST_sv_args(fn->name));
        }
        ST_generate_fn(out, fn);
    }
    if (emit_entry) {
        fprintf(out, "\nglobal _start\n");
        fprintf(out, "\n_start:\n");
        fprintf(out, "      push rbp\n");
        fprintf(out, "      mov rbp, rsp\n");
        fprintf(out, "      sub rsp, 32\n");
        fprintf(out, "      call _CRT_INIT\n");
        fprintf(out, "      call main\n");
        fprintf(out, "      mov rcx, rax\n");
        fprintf(out, "      call ExitProcess\n");
    }

    return 1;
}

