/*
 * Copyright Redis Contributors.
 * All rights reserved.
 * SPDX-License-Identifier: BSD 3-Clause
 *
 * To make it easier to map each part of the format string with each argument,
 * this file provides a way to write
 *
 *     printf("a = %s, b = %s, c = %s\n",
 *            arg1, arg2, arg3);
 *
 * as
 *
 *     printf(FMTARGS("a = %s, ", arg1,
 *                    "b = %s, ", arg2,
 *                    "c = %s\n", arg3));
 *
 * FMTARGS is variadic macro which is implemented by passing on its arguments to
 * two other variadic macros of which one extracts the odd (the formats) and the
 * other extracts the even (the arguments). The definitions of these macros
 * include counting the number of macro arguments. Therefore, they don't accept
 * an unlimited number of arguments. Currently it is fixed to a maximum of 120
 * formats and arguments.
 */
#ifndef FMTARGS_H
#define FMTARGS_H

/* A macro to count the number of arguments. */
#define NARG(...) NARG_I(__VA_ARGS__,RSEQ_N())
#define NARG_I(...) ARG_N(__VA_ARGS__)

/* Define a macro which will call an arbitrary macro appended with a number indicating
 * the number of arguments it has. */
#define VFUNC_N_(name, n) name##n
#define VFUNC_N(name, n) VFUNC_N_(name, n)
#define VFUNC(func, ...) VFUNC_N(func, NARG(__VA_ARGS__)) (__VA_ARGS__)

/* Macros to extract the formats and the arguments from the fmt-arg pairs and
 * then combine them again with all formats first and the arguments last. */
#define COMPACT_FMT(...) VFUNC(COMPACT_FMT_, __VA_ARGS__)
#define COMPACT_VALUES(...) VFUNC(COMPACT_VALUES_, __VA_ARGS__)
#define FMTARGS(...) COMPACT_FMT(__VA_ARGS__), COMPACT_VALUES(__VA_ARGS__)

/* Everything below this line is automatically generated by
 * generate-fmtargs.py. Do not manually edit. */

#define ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56, _57, _58, _59, _60, _61, _62, _63, _64, _65, _66, _67, _68, _69, _70, _71, _72, _73, _74, _75, _76, _77, _78, _79, _80, _81, _82, _83, _84, _85, _86, _87, _88, _89, _90, _91, _92, _93, _94, _95, _96, _97, _98, _99, _100, _101, _102, _103, _104, _105, _106, _107, _108, _109, _110, _111, _112, _113, _114, _115, _116, _117, _118, _119, _120, _121, _122, _123, _124, _125, _126, _127, _128, _129, _130, _131, _132, _133, _134, _135, _136, _137, _138, _139, _140, _141, _142, _143, _144, _145, _146, _147, _148, _149, _150, _151, _152, _153, _154, _155, _156, _157, _158, _159, _160, N, ...) N

#define RSEQ_N() 160, 159, 158, 157, 156, 155, 154, 153, 152, 151, 150, 149, 148, 147, 146, 145, 144, 143, 142, 141, 140, 139, 138, 137, 136, 135, 134, 133, 132, 131, 130, 129, 128, 127, 126, 125, 124, 123, 122, 121, 120, 119, 118, 117, 116, 115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105, 104, 103, 102, 101, 100, 99, 98, 97, 96, 95, 94, 93, 92, 91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0

#define COMPACT_FMT_2(fmt, value) fmt
#define COMPACT_FMT_4(fmt, value, ...) fmt COMPACT_FMT_2(__VA_ARGS__)
#define COMPACT_FMT_6(fmt, value, ...) fmt COMPACT_FMT_4(__VA_ARGS__)
#define COMPACT_FMT_8(fmt, value, ...) fmt COMPACT_FMT_6(__VA_ARGS__)
#define COMPACT_FMT_10(fmt, value, ...) fmt COMPACT_FMT_8(__VA_ARGS__)
#define COMPACT_FMT_12(fmt, value, ...) fmt COMPACT_FMT_10(__VA_ARGS__)
#define COMPACT_FMT_14(fmt, value, ...) fmt COMPACT_FMT_12(__VA_ARGS__)
#define COMPACT_FMT_16(fmt, value, ...) fmt COMPACT_FMT_14(__VA_ARGS__)
#define COMPACT_FMT_18(fmt, value, ...) fmt COMPACT_FMT_16(__VA_ARGS__)
#define COMPACT_FMT_20(fmt, value, ...) fmt COMPACT_FMT_18(__VA_ARGS__)
#define COMPACT_FMT_22(fmt, value, ...) fmt COMPACT_FMT_20(__VA_ARGS__)
#define COMPACT_FMT_24(fmt, value, ...) fmt COMPACT_FMT_22(__VA_ARGS__)
#define COMPACT_FMT_26(fmt, value, ...) fmt COMPACT_FMT_24(__VA_ARGS__)
#define COMPACT_FMT_28(fmt, value, ...) fmt COMPACT_FMT_26(__VA_ARGS__)
#define COMPACT_FMT_30(fmt, value, ...) fmt COMPACT_FMT_28(__VA_ARGS__)
#define COMPACT_FMT_32(fmt, value, ...) fmt COMPACT_FMT_30(__VA_ARGS__)
#define COMPACT_FMT_34(fmt, value, ...) fmt COMPACT_FMT_32(__VA_ARGS__)
#define COMPACT_FMT_36(fmt, value, ...) fmt COMPACT_FMT_34(__VA_ARGS__)
#define COMPACT_FMT_38(fmt, value, ...) fmt COMPACT_FMT_36(__VA_ARGS__)
#define COMPACT_FMT_40(fmt, value, ...) fmt COMPACT_FMT_38(__VA_ARGS__)
#define COMPACT_FMT_42(fmt, value, ...) fmt COMPACT_FMT_40(__VA_ARGS__)
#define COMPACT_FMT_44(fmt, value, ...) fmt COMPACT_FMT_42(__VA_ARGS__)
#define COMPACT_FMT_46(fmt, value, ...) fmt COMPACT_FMT_44(__VA_ARGS__)
#define COMPACT_FMT_48(fmt, value, ...) fmt COMPACT_FMT_46(__VA_ARGS__)
#define COMPACT_FMT_50(fmt, value, ...) fmt COMPACT_FMT_48(__VA_ARGS__)
#define COMPACT_FMT_52(fmt, value, ...) fmt COMPACT_FMT_50(__VA_ARGS__)
#define COMPACT_FMT_54(fmt, value, ...) fmt COMPACT_FMT_52(__VA_ARGS__)
#define COMPACT_FMT_56(fmt, value, ...) fmt COMPACT_FMT_54(__VA_ARGS__)
#define COMPACT_FMT_58(fmt, value, ...) fmt COMPACT_FMT_56(__VA_ARGS__)
#define COMPACT_FMT_60(fmt, value, ...) fmt COMPACT_FMT_58(__VA_ARGS__)
#define COMPACT_FMT_62(fmt, value, ...) fmt COMPACT_FMT_60(__VA_ARGS__)
#define COMPACT_FMT_64(fmt, value, ...) fmt COMPACT_FMT_62(__VA_ARGS__)
#define COMPACT_FMT_66(fmt, value, ...) fmt COMPACT_FMT_64(__VA_ARGS__)
#define COMPACT_FMT_68(fmt, value, ...) fmt COMPACT_FMT_66(__VA_ARGS__)
#define COMPACT_FMT_70(fmt, value, ...) fmt COMPACT_FMT_68(__VA_ARGS__)
#define COMPACT_FMT_72(fmt, value, ...) fmt COMPACT_FMT_70(__VA_ARGS__)
#define COMPACT_FMT_74(fmt, value, ...) fmt COMPACT_FMT_72(__VA_ARGS__)
#define COMPACT_FMT_76(fmt, value, ...) fmt COMPACT_FMT_74(__VA_ARGS__)
#define COMPACT_FMT_78(fmt, value, ...) fmt COMPACT_FMT_76(__VA_ARGS__)
#define COMPACT_FMT_80(fmt, value, ...) fmt COMPACT_FMT_78(__VA_ARGS__)
#define COMPACT_FMT_82(fmt, value, ...) fmt COMPACT_FMT_80(__VA_ARGS__)
#define COMPACT_FMT_84(fmt, value, ...) fmt COMPACT_FMT_82(__VA_ARGS__)
#define COMPACT_FMT_86(fmt, value, ...) fmt COMPACT_FMT_84(__VA_ARGS__)
#define COMPACT_FMT_88(fmt, value, ...) fmt COMPACT_FMT_86(__VA_ARGS__)
#define COMPACT_FMT_90(fmt, value, ...) fmt COMPACT_FMT_88(__VA_ARGS__)
#define COMPACT_FMT_92(fmt, value, ...) fmt COMPACT_FMT_90(__VA_ARGS__)
#define COMPACT_FMT_94(fmt, value, ...) fmt COMPACT_FMT_92(__VA_ARGS__)
#define COMPACT_FMT_96(fmt, value, ...) fmt COMPACT_FMT_94(__VA_ARGS__)
#define COMPACT_FMT_98(fmt, value, ...) fmt COMPACT_FMT_96(__VA_ARGS__)
#define COMPACT_FMT_100(fmt, value, ...) fmt COMPACT_FMT_98(__VA_ARGS__)
#define COMPACT_FMT_102(fmt, value, ...) fmt COMPACT_FMT_100(__VA_ARGS__)
#define COMPACT_FMT_104(fmt, value, ...) fmt COMPACT_FMT_102(__VA_ARGS__)
#define COMPACT_FMT_106(fmt, value, ...) fmt COMPACT_FMT_104(__VA_ARGS__)
#define COMPACT_FMT_108(fmt, value, ...) fmt COMPACT_FMT_106(__VA_ARGS__)
#define COMPACT_FMT_110(fmt, value, ...) fmt COMPACT_FMT_108(__VA_ARGS__)
#define COMPACT_FMT_112(fmt, value, ...) fmt COMPACT_FMT_110(__VA_ARGS__)
#define COMPACT_FMT_114(fmt, value, ...) fmt COMPACT_FMT_112(__VA_ARGS__)
#define COMPACT_FMT_116(fmt, value, ...) fmt COMPACT_FMT_114(__VA_ARGS__)
#define COMPACT_FMT_118(fmt, value, ...) fmt COMPACT_FMT_116(__VA_ARGS__)
#define COMPACT_FMT_120(fmt, value, ...) fmt COMPACT_FMT_118(__VA_ARGS__)
#define COMPACT_FMT_122(fmt, value, ...) fmt COMPACT_FMT_120(__VA_ARGS__)
#define COMPACT_FMT_124(fmt, value, ...) fmt COMPACT_FMT_122(__VA_ARGS__)
#define COMPACT_FMT_126(fmt, value, ...) fmt COMPACT_FMT_124(__VA_ARGS__)
#define COMPACT_FMT_128(fmt, value, ...) fmt COMPACT_FMT_126(__VA_ARGS__)
#define COMPACT_FMT_130(fmt, value, ...) fmt COMPACT_FMT_128(__VA_ARGS__)
#define COMPACT_FMT_132(fmt, value, ...) fmt COMPACT_FMT_130(__VA_ARGS__)
#define COMPACT_FMT_134(fmt, value, ...) fmt COMPACT_FMT_132(__VA_ARGS__)
#define COMPACT_FMT_136(fmt, value, ...) fmt COMPACT_FMT_134(__VA_ARGS__)
#define COMPACT_FMT_138(fmt, value, ...) fmt COMPACT_FMT_136(__VA_ARGS__)
#define COMPACT_FMT_140(fmt, value, ...) fmt COMPACT_FMT_138(__VA_ARGS__)
#define COMPACT_FMT_142(fmt, value, ...) fmt COMPACT_FMT_140(__VA_ARGS__)
#define COMPACT_FMT_144(fmt, value, ...) fmt COMPACT_FMT_142(__VA_ARGS__)
#define COMPACT_FMT_146(fmt, value, ...) fmt COMPACT_FMT_144(__VA_ARGS__)
#define COMPACT_FMT_148(fmt, value, ...) fmt COMPACT_FMT_146(__VA_ARGS__)
#define COMPACT_FMT_150(fmt, value, ...) fmt COMPACT_FMT_148(__VA_ARGS__)
#define COMPACT_FMT_152(fmt, value, ...) fmt COMPACT_FMT_150(__VA_ARGS__)
#define COMPACT_FMT_154(fmt, value, ...) fmt COMPACT_FMT_152(__VA_ARGS__)
#define COMPACT_FMT_156(fmt, value, ...) fmt COMPACT_FMT_154(__VA_ARGS__)
#define COMPACT_FMT_158(fmt, value, ...) fmt COMPACT_FMT_156(__VA_ARGS__)
#define COMPACT_FMT_160(fmt, value, ...) fmt COMPACT_FMT_158(__VA_ARGS__)

#define COMPACT_VALUES_2(fmt, value) value
#define COMPACT_VALUES_4(fmt, value, ...) value, COMPACT_VALUES_2(__VA_ARGS__)
#define COMPACT_VALUES_6(fmt, value, ...) value, COMPACT_VALUES_4(__VA_ARGS__)
#define COMPACT_VALUES_8(fmt, value, ...) value, COMPACT_VALUES_6(__VA_ARGS__)
#define COMPACT_VALUES_10(fmt, value, ...) value, COMPACT_VALUES_8(__VA_ARGS__)
#define COMPACT_VALUES_12(fmt, value, ...) value, COMPACT_VALUES_10(__VA_ARGS__)
#define COMPACT_VALUES_14(fmt, value, ...) value, COMPACT_VALUES_12(__VA_ARGS__)
#define COMPACT_VALUES_16(fmt, value, ...) value, COMPACT_VALUES_14(__VA_ARGS__)
#define COMPACT_VALUES_18(fmt, value, ...) value, COMPACT_VALUES_16(__VA_ARGS__)
#define COMPACT_VALUES_20(fmt, value, ...) value, COMPACT_VALUES_18(__VA_ARGS__)
#define COMPACT_VALUES_22(fmt, value, ...) value, COMPACT_VALUES_20(__VA_ARGS__)
#define COMPACT_VALUES_24(fmt, value, ...) value, COMPACT_VALUES_22(__VA_ARGS__)
#define COMPACT_VALUES_26(fmt, value, ...) value, COMPACT_VALUES_24(__VA_ARGS__)
#define COMPACT_VALUES_28(fmt, value, ...) value, COMPACT_VALUES_26(__VA_ARGS__)
#define COMPACT_VALUES_30(fmt, value, ...) value, COMPACT_VALUES_28(__VA_ARGS__)
#define COMPACT_VALUES_32(fmt, value, ...) value, COMPACT_VALUES_30(__VA_ARGS__)
#define COMPACT_VALUES_34(fmt, value, ...) value, COMPACT_VALUES_32(__VA_ARGS__)
#define COMPACT_VALUES_36(fmt, value, ...) value, COMPACT_VALUES_34(__VA_ARGS__)
#define COMPACT_VALUES_38(fmt, value, ...) value, COMPACT_VALUES_36(__VA_ARGS__)
#define COMPACT_VALUES_40(fmt, value, ...) value, COMPACT_VALUES_38(__VA_ARGS__)
#define COMPACT_VALUES_42(fmt, value, ...) value, COMPACT_VALUES_40(__VA_ARGS__)
#define COMPACT_VALUES_44(fmt, value, ...) value, COMPACT_VALUES_42(__VA_ARGS__)
#define COMPACT_VALUES_46(fmt, value, ...) value, COMPACT_VALUES_44(__VA_ARGS__)
#define COMPACT_VALUES_48(fmt, value, ...) value, COMPACT_VALUES_46(__VA_ARGS__)
#define COMPACT_VALUES_50(fmt, value, ...) value, COMPACT_VALUES_48(__VA_ARGS__)
#define COMPACT_VALUES_52(fmt, value, ...) value, COMPACT_VALUES_50(__VA_ARGS__)
#define COMPACT_VALUES_54(fmt, value, ...) value, COMPACT_VALUES_52(__VA_ARGS__)
#define COMPACT_VALUES_56(fmt, value, ...) value, COMPACT_VALUES_54(__VA_ARGS__)
#define COMPACT_VALUES_58(fmt, value, ...) value, COMPACT_VALUES_56(__VA_ARGS__)
#define COMPACT_VALUES_60(fmt, value, ...) value, COMPACT_VALUES_58(__VA_ARGS__)
#define COMPACT_VALUES_62(fmt, value, ...) value, COMPACT_VALUES_60(__VA_ARGS__)
#define COMPACT_VALUES_64(fmt, value, ...) value, COMPACT_VALUES_62(__VA_ARGS__)
#define COMPACT_VALUES_66(fmt, value, ...) value, COMPACT_VALUES_64(__VA_ARGS__)
#define COMPACT_VALUES_68(fmt, value, ...) value, COMPACT_VALUES_66(__VA_ARGS__)
#define COMPACT_VALUES_70(fmt, value, ...) value, COMPACT_VALUES_68(__VA_ARGS__)
#define COMPACT_VALUES_72(fmt, value, ...) value, COMPACT_VALUES_70(__VA_ARGS__)
#define COMPACT_VALUES_74(fmt, value, ...) value, COMPACT_VALUES_72(__VA_ARGS__)
#define COMPACT_VALUES_76(fmt, value, ...) value, COMPACT_VALUES_74(__VA_ARGS__)
#define COMPACT_VALUES_78(fmt, value, ...) value, COMPACT_VALUES_76(__VA_ARGS__)
#define COMPACT_VALUES_80(fmt, value, ...) value, COMPACT_VALUES_78(__VA_ARGS__)
#define COMPACT_VALUES_82(fmt, value, ...) value, COMPACT_VALUES_80(__VA_ARGS__)
#define COMPACT_VALUES_84(fmt, value, ...) value, COMPACT_VALUES_82(__VA_ARGS__)
#define COMPACT_VALUES_86(fmt, value, ...) value, COMPACT_VALUES_84(__VA_ARGS__)
#define COMPACT_VALUES_88(fmt, value, ...) value, COMPACT_VALUES_86(__VA_ARGS__)
#define COMPACT_VALUES_90(fmt, value, ...) value, COMPACT_VALUES_88(__VA_ARGS__)
#define COMPACT_VALUES_92(fmt, value, ...) value, COMPACT_VALUES_90(__VA_ARGS__)
#define COMPACT_VALUES_94(fmt, value, ...) value, COMPACT_VALUES_92(__VA_ARGS__)
#define COMPACT_VALUES_96(fmt, value, ...) value, COMPACT_VALUES_94(__VA_ARGS__)
#define COMPACT_VALUES_98(fmt, value, ...) value, COMPACT_VALUES_96(__VA_ARGS__)
#define COMPACT_VALUES_100(fmt, value, ...) value, COMPACT_VALUES_98(__VA_ARGS__)
#define COMPACT_VALUES_102(fmt, value, ...) value, COMPACT_VALUES_100(__VA_ARGS__)
#define COMPACT_VALUES_104(fmt, value, ...) value, COMPACT_VALUES_102(__VA_ARGS__)
#define COMPACT_VALUES_106(fmt, value, ...) value, COMPACT_VALUES_104(__VA_ARGS__)
#define COMPACT_VALUES_108(fmt, value, ...) value, COMPACT_VALUES_106(__VA_ARGS__)
#define COMPACT_VALUES_110(fmt, value, ...) value, COMPACT_VALUES_108(__VA_ARGS__)
#define COMPACT_VALUES_112(fmt, value, ...) value, COMPACT_VALUES_110(__VA_ARGS__)
#define COMPACT_VALUES_114(fmt, value, ...) value, COMPACT_VALUES_112(__VA_ARGS__)
#define COMPACT_VALUES_116(fmt, value, ...) value, COMPACT_VALUES_114(__VA_ARGS__)
#define COMPACT_VALUES_118(fmt, value, ...) value, COMPACT_VALUES_116(__VA_ARGS__)
#define COMPACT_VALUES_120(fmt, value, ...) value, COMPACT_VALUES_118(__VA_ARGS__)
#define COMPACT_VALUES_122(fmt, value, ...) value, COMPACT_VALUES_120(__VA_ARGS__)
#define COMPACT_VALUES_124(fmt, value, ...) value, COMPACT_VALUES_122(__VA_ARGS__)
#define COMPACT_VALUES_126(fmt, value, ...) value, COMPACT_VALUES_124(__VA_ARGS__)
#define COMPACT_VALUES_128(fmt, value, ...) value, COMPACT_VALUES_126(__VA_ARGS__)
#define COMPACT_VALUES_130(fmt, value, ...) value, COMPACT_VALUES_128(__VA_ARGS__)
#define COMPACT_VALUES_132(fmt, value, ...) value, COMPACT_VALUES_130(__VA_ARGS__)
#define COMPACT_VALUES_134(fmt, value, ...) value, COMPACT_VALUES_132(__VA_ARGS__)
#define COMPACT_VALUES_136(fmt, value, ...) value, COMPACT_VALUES_134(__VA_ARGS__)
#define COMPACT_VALUES_138(fmt, value, ...) value, COMPACT_VALUES_136(__VA_ARGS__)
#define COMPACT_VALUES_140(fmt, value, ...) value, COMPACT_VALUES_138(__VA_ARGS__)
#define COMPACT_VALUES_142(fmt, value, ...) value, COMPACT_VALUES_140(__VA_ARGS__)
#define COMPACT_VALUES_144(fmt, value, ...) value, COMPACT_VALUES_142(__VA_ARGS__)
#define COMPACT_VALUES_146(fmt, value, ...) value, COMPACT_VALUES_144(__VA_ARGS__)
#define COMPACT_VALUES_148(fmt, value, ...) value, COMPACT_VALUES_146(__VA_ARGS__)
#define COMPACT_VALUES_150(fmt, value, ...) value, COMPACT_VALUES_148(__VA_ARGS__)
#define COMPACT_VALUES_152(fmt, value, ...) value, COMPACT_VALUES_150(__VA_ARGS__)
#define COMPACT_VALUES_154(fmt, value, ...) value, COMPACT_VALUES_152(__VA_ARGS__)
#define COMPACT_VALUES_156(fmt, value, ...) value, COMPACT_VALUES_154(__VA_ARGS__)
#define COMPACT_VALUES_158(fmt, value, ...) value, COMPACT_VALUES_156(__VA_ARGS__)
#define COMPACT_VALUES_160(fmt, value, ...) value, COMPACT_VALUES_158(__VA_ARGS__)

#endif
