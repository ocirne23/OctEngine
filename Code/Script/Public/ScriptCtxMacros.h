#pragma once

// ---- parameter-pair expansion ----
// Each SCRIPT_CTX_FUNCS row below lists its parameters ONCE, as (type, name) pairs, instead of a typed
// declaration list and a same-named bare-argument list side by side. SCRIPT_CTX_PARAMS/SCRIPT_CTX_ARGS expand
// that one list into "type name, type name, ..." (the declaration -- SCRIPT_CTX_PTR/DECL/METHOD all need this)
// and "name, name, ..." (the cooked build's inline forwarder call -- SCRIPT_CTX_METHOD only, see below)
// respectively. Capped at 8 parameters (audioTrigger, the widest entry today, uses 7) -- a 9th parameter needs
// one more _PARAMS_9/_ARGS_9 pair below plus widening SCRIPT_CTX_PICK. No zero-parameter entries are supported
// here, matching log/logf, which already sit outside SCRIPT_CTX_FUNCS for their own irregular signatures --
// declare a hypothetical zero-arg function by hand alongside them instead.
#define SCRIPT_CTX_EXPAND(x) x // forces one extra rescan pass -- cheap insurance against preprocessor quirks
#define SCRIPT_CTX_PAIR_DECL(type, name) type name
#define SCRIPT_CTX_PAIR_ARG(type, name)  name
#define SCRIPT_CTX_APPLY(macro, pair) SCRIPT_CTX_EXPAND(macro pair) // "macro (type, name)" -> macro(type, name)

#define SCRIPT_CTX_PARAMS_1(p0)                     SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p0)
#define SCRIPT_CTX_PARAMS_2(p0,p1)                  SCRIPT_CTX_PARAMS_1(p0), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p1)
#define SCRIPT_CTX_PARAMS_3(p0,p1,p2)                SCRIPT_CTX_PARAMS_2(p0,p1), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p2)
#define SCRIPT_CTX_PARAMS_4(p0,p1,p2,p3)             SCRIPT_CTX_PARAMS_3(p0,p1,p2), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p3)
#define SCRIPT_CTX_PARAMS_5(p0,p1,p2,p3,p4)          SCRIPT_CTX_PARAMS_4(p0,p1,p2,p3), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p4)
#define SCRIPT_CTX_PARAMS_6(p0,p1,p2,p3,p4,p5)       SCRIPT_CTX_PARAMS_5(p0,p1,p2,p3,p4), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p5)
#define SCRIPT_CTX_PARAMS_7(p0,p1,p2,p3,p4,p5,p6)    SCRIPT_CTX_PARAMS_6(p0,p1,p2,p3,p4,p5), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p6)
#define SCRIPT_CTX_PARAMS_8(p0,p1,p2,p3,p4,p5,p6,p7) SCRIPT_CTX_PARAMS_7(p0,p1,p2,p3,p4,p5,p6), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_DECL, p7)

#define SCRIPT_CTX_ARGS_1(p0)                        SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p0)
#define SCRIPT_CTX_ARGS_2(p0,p1)                     SCRIPT_CTX_ARGS_1(p0), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p1)
#define SCRIPT_CTX_ARGS_3(p0,p1,p2)                  SCRIPT_CTX_ARGS_2(p0,p1), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p2)
#define SCRIPT_CTX_ARGS_4(p0,p1,p2,p3)               SCRIPT_CTX_ARGS_3(p0,p1,p2), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p3)
#define SCRIPT_CTX_ARGS_5(p0,p1,p2,p3,p4)            SCRIPT_CTX_ARGS_4(p0,p1,p2,p3), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p4)
#define SCRIPT_CTX_ARGS_6(p0,p1,p2,p3,p4,p5)         SCRIPT_CTX_ARGS_5(p0,p1,p2,p3,p4), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p5)
#define SCRIPT_CTX_ARGS_7(p0,p1,p2,p3,p4,p5,p6)      SCRIPT_CTX_ARGS_6(p0,p1,p2,p3,p4,p5), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p6)
#define SCRIPT_CTX_ARGS_8(p0,p1,p2,p3,p4,p5,p6,p7)   SCRIPT_CTX_ARGS_7(p0,p1,p2,p3,p4,p5,p6), SCRIPT_CTX_APPLY(SCRIPT_CTX_PAIR_ARG, p7)

// Picks the Nth trailing macro name by (ab)using argument-count-dependent positional alignment against a
// descending sentinel list -- the standard "overload a variadic macro by argument count" trick. N = number of
// (type, name) pairs passed to SCRIPT_CTX_PARAMS/SCRIPT_CTX_ARGS, 1..8.
#define SCRIPT_CTX_PICK(_1,_2,_3,_4,_5,_6,_7,_8,NAME,...) NAME
#define SCRIPT_CTX_PARAMS_(NAME, ...) NAME(__VA_ARGS__)
#define SCRIPT_CTX_PARAMS(...) SCRIPT_CTX_EXPAND(SCRIPT_CTX_PARAMS_(SCRIPT_CTX_PICK(__VA_ARGS__, \
    SCRIPT_CTX_PARAMS_8,SCRIPT_CTX_PARAMS_7,SCRIPT_CTX_PARAMS_6,SCRIPT_CTX_PARAMS_5, \
    SCRIPT_CTX_PARAMS_4,SCRIPT_CTX_PARAMS_3,SCRIPT_CTX_PARAMS_2,SCRIPT_CTX_PARAMS_1), __VA_ARGS__))
#define SCRIPT_CTX_ARGS_(NAME, ...) NAME(__VA_ARGS__)
#define SCRIPT_CTX_ARGS(...) SCRIPT_CTX_EXPAND(SCRIPT_CTX_ARGS_(SCRIPT_CTX_PICK(__VA_ARGS__, \
    SCRIPT_CTX_ARGS_8,SCRIPT_CTX_ARGS_7,SCRIPT_CTX_ARGS_6,SCRIPT_CTX_ARGS_5, \
    SCRIPT_CTX_ARGS_4,SCRIPT_CTX_ARGS_3,SCRIPT_CTX_ARGS_2,SCRIPT_CTX_ARGS_1), __VA_ARGS__))