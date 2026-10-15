// Guest functions shared by fixture.cpp (which encodes and compiles them)
// and test.c (which runs them and replays each step with fast_cpu.h).
#pragma once
#include "cpu/cpu.h"

typedef enum {
  H1_END = 0,
  H1_FADDS,
  H1_FSUBS,
  H1_FMULS,
  H1_FDIVS,
  H1_FRSP,
  H1_FMADDS,
  H1_FMSUBS,
  H1_FNMADDS,
  H1_FNMSUBS,
  H1_PS_ADD,
  H1_PS_MUL,
  H1_PS_MADD,
} H1Kind;

typedef struct {
  H1Kind kind;
  u8 d, a, b, c, rc;
} H1Step;

#define H1_MAX_WORDS 8

typedef struct {
  const char *name;
  u32 address;
  H1Step steps[H1_MAX_WORDS];
} H1Function;

static inline u32 h1_encode(H1Step step) {
  u32 fields = ((u32)step.d << 21) | ((u32)step.a << 16) |
               ((u32)step.b << 11) | ((u32)step.c << 6) | step.rc;
  switch (step.kind) {
  case H1_FADDS: return 0xEC000000u | fields | (21u << 1);
  case H1_FSUBS: return 0xEC000000u | fields | (20u << 1);
  case H1_FMULS: return 0xEC000000u | fields | (25u << 1);
  case H1_FDIVS: return 0xEC000000u | fields | (18u << 1);
  case H1_FRSP: return 0xFC000000u | fields | (12u << 1);
  case H1_FMADDS: return 0xEC000000u | fields | (29u << 1);
  case H1_FMSUBS: return 0xEC000000u | fields | (28u << 1);
  case H1_FNMADDS: return 0xEC000000u | fields | (31u << 1);
  case H1_FNMSUBS: return 0xEC000000u | fields | (30u << 1);
  case H1_PS_ADD: return 0x10000000u | fields | (21u << 1);
  case H1_PS_MUL: return 0x10000000u | fields | (25u << 1);
  case H1_PS_MADD: return 0x10000000u | fields | (29u << 1);
  default: return 0;
  }
}

#define ADD(d, a, b) {H1_FADDS, d, a, b, 0, 0}
#define ADDRC(d, a, b) {H1_FADDS, d, a, b, 0, 1}
#define SUB(d, a, b) {H1_FSUBS, d, a, b, 0, 0}
#define MUL(d, a, c) {H1_FMULS, d, a, 0, c, 0}
#define DIV(d, a, b) {H1_FDIVS, d, a, b, 0, 0}
#define RSP(d, b) {H1_FRSP, d, 0, b, 0, 0}
#define MADD(d, a, c, b) {H1_FMADDS, d, a, b, c, 0}
#define MSUB(d, a, c, b) {H1_FMSUBS, d, a, b, c, 0}
#define NMADD(d, a, c, b) {H1_FNMADDS, d, a, b, c, 0}
#define NMSUB(d, a, c, b) {H1_FNMSUBS, d, a, b, c, 0}
#define PSADD(d, a, b) {H1_PS_ADD, d, a, b, 0, 0}
#define PSMUL(d, a, c) {H1_PS_MUL, d, a, 0, c, 0}
#define PSMADD(d, a, c, b) {H1_PS_MADD, d, a, b, c, 0}

static const H1Function h1_functions[] = {
    {"fadds", 0x80001000u, {ADD(1, 2, 3)}},
    {"fsubs", 0x80001100u, {SUB(4, 5, 6)}},
    {"fmuls", 0x80001200u, {MUL(7, 8, 9)}},
    {"fdivs", 0x80001300u, {DIV(10, 11, 12)}},
    {"frsp", 0x80001400u, {RSP(13, 14)}},
    {"fmadds", 0x80001500u, {MADD(17, 18, 19, 20)}},
    {"fmsubs", 0x80001600u, {MSUB(21, 22, 23, 24)}},
    {"fnmadds", 0x80001700u, {NMADD(25, 26, 27, 28)}},
    {"fnmsubs", 0x80001800u, {NMSUB(29, 30, 31, 1)}},
    {"fadds_rc", 0x80001900u, {ADDRC(1, 2, 3)}},
    {"fmadds_alias", 0x80001A00u, {MADD(1, 1, 2, 1)}},
    {"fmuls_alias", 0x80001B00u, {MUL(3, 3, 3)}},
    {"fdivs_alias", 0x80001C00u, {DIV(2, 2, 2)}},
    {"chain", 0x80001D00u,
     {ADD(1, 2, 3), MUL(4, 1, 5), MADD(6, 1, 4, 2), RSP(7, 6), SUB(8, 7, 6),
      DIV(9, 8, 1)}},
    {"fma_chain", 0x80001E00u,
     {MADD(1, 2, 3, 4), NMSUB(5, 1, 6, 7), MSUB(8, 5, 1, 2),
      NMADD(9, 8, 5, 1)}},
    {"mixed", 0x80001F00u,
     {ADD(1, 2, 3), PSADD(4, 1, 5), PSMUL(6, 4, 1), SUB(7, 6, 4),
      PSMADD(8, 7, 1, 6), MUL(9, 8, 7), RSP(10, 8)}},
    {"mixed2", 0x80002000u,
     {PSADD(1, 2, 3), ADD(4, 1, 2), PSMUL(5, 4, 1), MADD(6, 5, 4, 1)}},
    {"frsp_chain", 0x80002100u, {RSP(1, 2), ADD(3, 1, 2), RSP(4, 3)}},
    {"rc_chain", 0x80002200u, {ADDRC(1, 2, 3), ADDRC(4, 1, 1), MUL(5, 4, 1)}},
};

#define H1_FUNCTION_COUNT (sizeof(h1_functions) / sizeof(h1_functions[0]))
