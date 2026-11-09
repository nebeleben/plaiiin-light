#pragma once
/* Permission bits and route table for AI share keys. Pure C, no ESP deps. */
#include <stdint.h>
#include <stdbool.h>

typedef enum { AI_M_GET, AI_M_POST, AI_M_PUT, AI_M_PATCH, AI_M_DELETE, AI_M_OTHER } ai_method_t;

enum {
    AI_PERM_CONTROL = 1u << 0,
    AI_PERM_EFFECTS = 1u << 1,
    AI_PERM_IMAGES  = 1u << 2,
    AI_PERM_SCENES  = 1u << 3,
    AI_PERM_STREAM  = 1u << 4,
    AI_PERM_OTA     = 1u << 5,
    AI_PERM_RENAME  = 1u << 6,
    AI_PERM_SWARM   = 1u << 7, /* read swarm membership/stats; never join/leave */
    AI_PERM_ALL     = 0xFFu,
};

#define AI_ROUTE_READ    0xFFFFFFFFu /* a plain read, allowed if the handler's min role is user */
#define AI_ROUTE_DENIED  0u          /* never reachable with an AI key */

const char *ai_perm_name(uint32_t single_bit);          /* NULL if not exactly one known bit */
uint32_t    ai_perm_from_name(const char *name);        /* 0 if unknown */
uint32_t    ai_route_perm(ai_method_t m, const char *uri); /* one AI_PERM_* bit, AI_ROUTE_READ or AI_ROUTE_DENIED */
