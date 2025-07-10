//
// Created by Spencer Martin on 7/2/25.
//

#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

//General preprocessor utilities
#define FIRST(a, ...) a
#define SECOND(a, b, ...) b

#define EMPTY()
#define DEFER(x) x EMPTY()
#define DEFER_2(x) x EMPTY() EMPTY()
#define EVAL_1(...) __VA_ARGS__
#define EVAL_2(...) EVAL_1(EVAL_1(__VA_ARGS__))
#define EVAL_3(...) EVAL_2(EVAL_2(__VA_ARGS__))
#define EVAL_4(...) EVAL_3(EVAL_3(__VA_ARGS__))
#define EVAL_5(...) EVAL_4(EVAL_4(__VA_ARGS__))
#define EVAL_6(...) EVAL_5(EVAL_5(__VA_ARGS__))
#define EVAL_7(...) EVAL_6(EVAL_6(__VA_ARGS__))
#define EVAL_8(...) EVAL_7(EVAL_7(__VA_ARGS__))
#define EVAL(...) EVAL_8(__VA_ARGS__)

#define _MAP_NEXT(MACRO, HEAD, ...) MACRO(HEAD) __VA_OPT__(, DEFER(_MAP_AGAIN)(MACRO, __VA_ARGS__))
#define _MAP_AGAIN(MACRO, HEAD, ...) MACRO(HEAD) __VA_OPT__(, DEFER(_MAP_NEXT)(MACRO, __VA_ARGS__))
#define MAP(MACRO, ...) __VA_OPT__(EVAL(_MAP_NEXT(MACRO, __VA_ARGS__)))

//Support macros for STRIP_BASE, which takes expressions like "public Foo" or "virtual protected Foo" and extracts
//the base class Foo
#define _crocos_internal_public ,
#define _crocos_internal_private ,
#define _crocos_internal_protected ,
#define _crocos_internal_final ,
#define _crocos_internal_virtual ,

#define _STRIP_BASE_PREPARE(x) (_crocos_internal_ ## x, x)
#define _STRIP_BASE(x) EVAL_1(DEFER(SECOND) _STRIP_BASE_PREPARE(x))
#define STRIP_BASE(x) _STRIP_BASE(_STRIP_BASE(_STRIP_BASE(_STRIP_BASE(_STRIP_BASE(x)))))

#define STRIP(...) MAP(STRIP_BASE, __VA_ARGS__)

#endif //PREPROCESSOR_H
