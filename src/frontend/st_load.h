#ifndef ST_LOAD_H
#define ST_LOAD_H

#include "../utils/st_arena.h"
#include "../utils/st_ht.h"
#include "../utils/st_srcmap.h"
#include "st_lexer.h"

b8 ST_load_file(ST_arena_t *arena, ST_string_t entry_path, ST_srcmap_t *srcs, ST_tokens_t *out);

#endif
