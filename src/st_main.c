#include "backend/st_nasm.h"
#include "frontend/st_lexer.h"
#include "frontend/st_module.h"
#include "frontend/st_parser.h"
#include "frontend/st_semantic.h"
#include "middle/st_lower.h"
#include "utils/st_arena.h"
#include "utils/st_flag.h"
#include "utils/st_helper.h"
#include "utils/st_process.h"
#include "frontend/st_load.h"
#include "utils/st_string.h"

// NOTE(krypton): Temporary till cli branch is merged
#if defined(__linux__)
    #define ST_OBJ_EXT ".o"
    #define ST_EXE_EXT ""
    #define ST_NASM_FORMAT "elf64"
#elif defined(_WIN32)
    #define ST_OBJ_EXT ".obj"
    #define ST_EXE_EXT ".exe"
    #define ST_NASM_FORMAT "win64"
#endif

void ST_os_link(ST_arena_t *arena, ST_procs_t *procs, const char *obj_path, const char *output) {
#if defined(__linux__)
    ST_append_process(procs, "ld", "-o", output, obj_path, "--dynamic-linker=/usr/lib64/ld-linux-x86-64.so.2", "-lc",);
#elif defined(_WIN32)
    ST_sb_t sb = {0};
    ST_arena_append_to_builder(arena, &sb, "/OUT:");
    ST_arena_append_to_builder(arena, &sb, output);
    ST_da_append_arena(arena, &sb, '\0');
    ST_append_process(procs, "link", "/nologo", obj_path, "/ENTRY:_start", "/SUBSYSTEM:CONSOLE", sb.items);
#endif
}

int main(int argc, char **argv) {
    b8 dump_tokens = 0, dump_ast = 0, dump_ir = 0, emit_asm = 0, build_exe = 0;
    b8 emit_obj = 0, run_exe = 0;
    char *path = NULL;

    ST_arena_t *arena = ST_arena_alloc();
    ST_flag_parser_t *fp = ST_flag_init(arena);
    ST_procs_t procs = {0};
    b8 rc = 1;

    ST_flag_bool(fp, "dump-tokens",
                 "Dump the tokens of storth"
                 " source code",
                 &dump_tokens);
    ST_flag_bool(fp, "dump-ast", "Dump the ast of storth source code", &dump_ast);
    ST_flag_bool(fp, "emit-ir",
                 "Print out the intermediate representation "
                 "of storth source code.",
                 &dump_ir);
    ST_flag_bool(fp, "emit-asm",
                 "Emit the assembly instruction that "
                 "is generated from the intermediate representation",
                 &emit_asm);
    ST_flag_bool(fp, "build", "Generate the final executable from the assembly.", &build_exe);
    ST_flag_bool(fp, "run",
                 "Generate the executable and run the "
                 "generated executable.",
                 &run_exe);
    ST_flag_bool(fp, "emit-obj",
                 "Emit the object files "
                 "generated from the assembly",
                 &emit_obj);

    ST_flag_alias(fp, "emit-asm", 's');
    ST_flag_alias(fp, "emit-obj", 'c');
    ST_flag_alias(fp, "build", 'b');
    ST_flag_alias(fp, "run", 'r');

    ST_flag_positional(fp, "input",
                       "Path to the storth source file "
                       "to compile",
                       &path, 1);

    const char *asm_path = "test.asm";
    const char *obj_path = "test" ST_OBJ_EXT;

    const char *exe_path = "test" ST_EXE_EXT;
    ST_string_t exe_path_s = ST_abs_path(arena, exe_path);
    char *exe_path_cstr = ST_arena_push(arena, exe_path_s.len + 1);
    memcpy(exe_path_cstr, exe_path_s.data, exe_path_s.len);
    exe_path_cstr[exe_path_s.len] = 0;
    exe_path = exe_path_cstr;

    if (!ST_flag_parse(fp, argc, argv)) {
        ST_flag_usage(fp);
        goto done;
    }
    
    // TODO(krypton): mkdir .build
    if (build_exe || run_exe) {
        asm_path = ".build/test.asm";
        obj_path = ".build/test" ST_OBJ_EXT;
    }

    FILE *f = fopen(asm_path, "wb");
    ST_string_t file = ST_abs_path(arena, path);

    ST_srcmap_t srcs;
    ST_srcmap_init(arena, &srcs);

    ST_tokens_t tokens;
    if (!ST_load_file(arena, ST_cstr_to_str(path), &srcs, &tokens))
        goto close;
    if (dump_tokens)
        ST_dump_token(tokens);

    ST_string_t src = ST_srcmap_get(&srcs, file);

    ST_program_t prog = {0};
    if (!ST_parse(arena, tokens, src, file, &srcs, &prog))
        goto close;

    ST_diag_t mod_diag = {.src = src, .file = file, .max_errors = ST_SEMA_MAX_ERRORS};
    if (!ST_modules_process(arena, &prog, file, &srcs, &mod_diag))
        goto close;

    if (dump_ast)
        ST_dump_program(stdout, &prog);

    ST_sema_t sema = {0};
    if (!ST_sema_run(arena, &prog, src, file, &sema))
        goto close;

    ST_ir_module_t mod = {0};
    rc = ST_lower_program(arena, &prog, &sema, src, file, &mod) ? 0 : 1;
    if (dump_ir)
        ST_ir_dump_module(stdout, &mod);
    if (rc != 0)
        goto close;

    if (emit_asm)
        if (!ST_nasm_generate(f, &mod, src, file, 1))
            goto close;
    
    if (emit_obj || build_exe || run_exe) {
        if (!ST_nasm_generate(f, &mod, src, file, 1))
            goto close;
        ST_append_process(&procs, "nasm", "-f", ST_NASM_FORMAT, asm_path);
        if (!ST_run_processes(&procs))
            goto close;
    }

    if (build_exe || run_exe) {
      ST_os_link(arena, &procs, obj_path, "test" ST_EXE_EXT);
      
        if (!ST_run_processes(&procs))
            goto close;
        if (run_exe) {
            ST_append_process(&procs, exe_path);
            if (!ST_run_processes(&procs))
                goto close;
        }
    }

close:
    fclose(f);
done:
    ST_free_process(&procs);
    ST_arena_free(arena);
    return rc;
}
