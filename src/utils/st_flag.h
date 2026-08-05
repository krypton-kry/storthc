#ifndef ST_FLAG_H
#define ST_FLAG_H

#include "st_arena.h"
#include "st_helper.h"
#include "st_string.h"

// @note: This is the flag parser api.
// @usage: The way it is supposed to be used is as follows.

// #include "st_flag.h"
// int main(int argc, char **argv) {
//     ST_arena_t *arena = ST_arena_alloc();
//     ST_flag_parser_t *fp = ST_flag_init(arena);
//     char *name;
//     bool echo;
//     ST_flag_bool(fp, "echo", "This will echo hello", &echo);
//     ST_flag_string(fp, "name", "This will accept name from user", &name);
//
//     if (!ST_flag_parse(fp, argc, argv)) {
//         ST_flag_usage(fp);
//         return 1;
//     }
//     if (echo)
//         puts("Echo: Hello");
//     if (name)
//         printf("%s\n", name);
//     return 0;
// }

typedef struct ST_flag_parser_t ST_flag_parser_t;

typedef enum {
    ST_FLAG_BOOL,
    ST_FLAG_INT,
    ST_FLAG_STRING,
    ST_FLAG_STRINGS,
} ST_flag_kind_t;

ST_flag_parser_t *ST_flag_init(ST_arena_t *arena);

void ST_flag_bool(ST_flag_parser_t *fp, const char *name, const char *description, b8 *value);
void ST_flag_int(ST_flag_parser_t *fp, const char *name, const char *description, i32 *value);
void ST_flag_string(ST_flag_parser_t *fp, const char *name, const char *description, char **value);
void ST_flag_strings(ST_flag_parser_t *fp, const char *name, const char *description,
                     ST_strings_t value);

void ST_flag_positional(ST_flag_parser_t *fp, const char *name, const char *description,
                        char **value, b8 required);
void ST_flag_alias(ST_flag_parser_t *fp, const char *long_name, char short_name);

b8 ST_flag_parse(ST_flag_parser_t *fp, int argc, char **argv);
void ST_flag_usage(ST_flag_parser_t *fp);
void ST_flag_dump(ST_flag_parser_t *fp);

#endif // ST_FLAG_H
