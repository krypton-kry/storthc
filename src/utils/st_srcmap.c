#include "st_srcmap.h"

void ST_srcmap_init(ST_arena_t *arena, ST_srcmap_t *m) {
    m->arena = arena;
    ST_ht_init(arena, &m->table, 16);
}

void ST_srcmap_put(ST_srcmap_t *m, ST_string_t file, ST_string_t src) {
    ST_ht_generic_t *hk = ST_arena_push(m->arena, sizeof(*hk));
    hk->tag = file.data;
    hk->size = file.len;

    ST_string_t *psrc = ST_arena_push(m->arena, sizeof(*psrc));
    *psrc = src;
    ST_ht_set(&m->table, hk, (ST_ht_generic_t){.tag = psrc, .size = 0});
}

ST_string_t ST_srcmap_get(ST_srcmap_t *m, ST_string_t file) {
    ST_ht_generic_t key = {.tag = file.data, .size = file.len};
    ST_string_t *found = (ST_string_t *)ST_ht_get(&m->table, key).tag;
    return found ? *found : (ST_string_t){0};
}
