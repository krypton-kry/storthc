#include "st_nasm.h"

// https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
static const char *arg_regs[] = {"rcx", "rdx", "r8", "r9"};
#define ST_N_ARG_REGS ((u32)ST_array_len(arg_regs))
static const char *xmm_regs[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
#define ST_N_XMM_REGS ((u32)ST_array_len(xmm_regs))

static void ST_generate_strs(FILE *out, ST_ir_module_t *m) {
    ST_todo("ST_generate_strs");
}

static void ST_generate_fn(FILE *out, ST_ir_fn_t *fn) {
    ST_todo("ST_generate_fn");
}

b8 ST_nasm_generate(FILE *out, ST_ir_module_t *m, ST_string_t src, ST_string_t file, b8 emit_entry) {
        ST_todo("ST_nasm_generate");
}

