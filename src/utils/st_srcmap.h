#ifndef ST_SRCMAP_H
#define ST_SRCMAP_H

#include "st_arena.h"
#include "st_ht.h"
#include "st_string.h"

typedef struct {
    ST_arena_t *arena;
    ST_ht_t table;
} ST_srcmap_t;

void ST_srcmap_init(ST_arena_t *arena, ST_srcmap_t *m);
void ST_srcmap_put(ST_srcmap_t *m, ST_string_t file, ST_string_t src);
ST_string_t ST_srcmap_get(ST_srcmap_t *m, ST_string_t file);

#endif // ST_SRCMAP_H
