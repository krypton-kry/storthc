#ifndef ST_MODULE_H
#define ST_MODULE_H

#include "../utils/st_arena.h"
#include "../utils/st_diagnostic.h"
#include "../utils/st_srcmap.h"
#include "st_ast.h"

// @note: ST_modules_process resolves every '#import "name";' reachable from
// 'root_prog' (transitively, following imports of imports), compiles each
// imported module as its own file, and merges the result into 'root_prog':
//
//   - The root file's own top-level names are left exactly as written.
//   - Every decl pulled in from an imported module is renamed to
//     'modulename__decl' so it can't collide with anything else, and that
//     rename is also applied to every reference to it within the module's
//     own source (so the module's internal calls/uses keep working).
//   - 'alias.member' anywhere in any file's expressions is rewritten in
//     place to the target module's mangled name, after checking 'member'
//     was declared 'pub' in that module.
//   - 'ST_DE_IMPORT' decls are consumed by this pass and do not appear in
//     the final decl list; nothing downstream needs to know modules exist.
//
// Resolution order for '#import "name"' (relative to the *importing
// file's own directory*, not the process cwd):
//   1. <importing file's dir>/modules/<name>/module.st
//   2. $STORTHC_MODULE_PATH/<name>/module.st   (if that env var is set)
//   3. /usr/local/storthc/modules/<name>/module.st
//
// Known limitations (kept small on purpose):
//   - The rename of a module's own top-level names is lexical, not
//     scope-aware: a local variable or parameter that happens to share a
//     name with one of that module's own top-level decls will also get
//     renamed. Avoid naming locals the same as top-level decls in the same
//     file and this never comes up in practice.
//   - Cross-module *type* references ('p: io.Reader') aren't supported yet
//     -- only functions, consts, and globals are reachable through
//     'alias.member'. Structs/enums from a module are usable only within
//     that module's own file for now.
b8 ST_modules_process(ST_arena_t *arena, ST_program_t *root_prog, ST_string_t root_file,
                      ST_srcmap_t *srcs, ST_diag_t *diag);

#endif
