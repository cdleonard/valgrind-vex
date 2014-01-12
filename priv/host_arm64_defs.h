
/*---------------------------------------------------------------*/
/*--- begin                                 host_arm64_defs.h ---*/
/*---------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2013-2013 OpenWorks
      info@open-works.net

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#ifndef __VEX_HOST_ARM64_DEFS_H
#define __VEX_HOST_ARM64_DEFS_H

#include "libvex_basictypes.h"
#include "libvex.h"                      // VexArch
#include "host_generic_regs.h"           // HReg

//ZZ extern UInt arm_hwcaps;


/* --------- Registers. --------- */

//ZZ /* The usual HReg abstraction.
//ZZ    There are 16 general purpose regs.
//ZZ */

extern void ppHRegARM64 ( HReg );

extern HReg hregARM64_X0  ( void );
extern HReg hregARM64_X1  ( void );
extern HReg hregARM64_X2  ( void );
extern HReg hregARM64_X3  ( void );
extern HReg hregARM64_X4  ( void );
extern HReg hregARM64_X5  ( void );
extern HReg hregARM64_X6  ( void );
extern HReg hregARM64_X7  ( void );
//ZZ extern HReg hregARM_R8  ( void );
extern HReg hregARM64_X9  ( void );
extern HReg hregARM64_X10 ( void );
extern HReg hregARM64_X11 ( void );
extern HReg hregARM64_X12 ( void );
extern HReg hregARM64_X13 ( void );
extern HReg hregARM64_X14 ( void );
extern HReg hregARM64_X15 ( void );
extern HReg hregARM64_X21 ( void );
extern HReg hregARM64_X22 ( void );
extern HReg hregARM64_X23 ( void );
extern HReg hregARM64_X24 ( void );
extern HReg hregARM64_X25 ( void );
extern HReg hregARM64_X26 ( void );
extern HReg hregARM64_X27 ( void );
extern HReg hregARM64_X28 ( void );
extern HReg hregARM64_D8  ( void );
extern HReg hregARM64_D9  ( void );
extern HReg hregARM64_D10 ( void );
extern HReg hregARM64_D11 ( void );
extern HReg hregARM64_D12 ( void );
extern HReg hregARM64_D13 ( void );
extern HReg hregARM64_Q16 ( void );
extern HReg hregARM64_Q17 ( void );
extern HReg hregARM64_Q18 ( void );

/* Number of registers used arg passing in function calls */
#define ARM64_N_ARGREGS 8   /* x0 .. x7 */


/* --------- Condition codes. --------- */

typedef
   enum {
      ARM64cc_EQ  = 0,  /* equal                         : Z=1 */
      ARM64cc_NE  = 1,  /* not equal                     : Z=0 */

      ARM64cc_CS  = 2,  /* >=u (higher or same)          : C=1 */
      ARM64cc_CC  = 3,  /* <u  (lower)                   : C=0 */

      ARM64cc_MI  = 4,  /* minus (negative)              : N=1 */
      ARM64cc_PL  = 5,  /* plus (zero or +ve)            : N=0 */

      ARM64cc_VS  = 6,  /* overflow                      : V=1 */
      ARM64cc_VC  = 7,  /* no overflow                   : V=0 */

      ARM64cc_HI  = 8,  /* >u   (higher)                 :   C=1 && Z=0 */
      ARM64cc_LS  = 9,  /* <=u  (lower or same)          : !(C=1 && Z=0) */

      ARM64cc_GE  = 10, /* >=s (signed greater or equal) :   N=V */
      ARM64cc_LT  = 11, /* <s  (signed less than)        : !(N=V) */

      ARM64cc_GT  = 12, /* >s  (signed greater)          :   Z=0 && N=V */
      ARM64cc_LE  = 13, /* <=s (signed less or equal)    : !(Z=0 && N=V) */

      ARM64cc_AL  = 14, /* always (unconditional) */
      ARM64cc_NV  = 15  /* in 64-bit mode also means "always" */
   }
   ARM64CondCode;


/* --------- Memory address expressions (amodes). --------- */

typedef
   enum {
      ARM64am_RI9=1,  /* reg + simm9 */
      ARM64am_RI12,   /* reg + uimm12 * szB (iow, scaled by access size) */
      ARM64am_RR      /* reg1 + reg2 */
   }
   ARM64AModeTag;

typedef
   struct {
      ARM64AModeTag tag;
      union {
         struct {
            HReg reg;
            Int  simm9; /* -256 .. +255 */
         } RI9;
         struct {
            HReg  reg;
            UInt  uimm12; /* 0 .. 4095 */
            UChar szB;    /* 1, 2, 4, 8 (16 ?) */
         } RI12;
         struct {
            HReg base;
            HReg index;
         } RR;
      } ARM64am;
   }
   ARM64AMode;

extern ARM64AMode* ARM64AMode_RI9  ( HReg reg, Int simm9 );
extern ARM64AMode* ARM64AMode_RI12 ( HReg reg, Int uimm12, UChar szB );
extern ARM64AMode* ARM64AMode_RR   ( HReg base, HReg index );


/* --------- Reg or uimm12 or (uimm12 << 12) operands --------- */

typedef
   enum {
      ARM64riA_I12=4, /* uimm12 << 0 or 12 only */
      ARM64riA_R      /* reg */
   }
   ARM64RIATag;

typedef
   struct {
      ARM64RIATag tag;
      union {
         struct {
            UShort imm12;  /* 0 .. 4095 */
            UChar  shift;  /* 0 or 12 only */
         } I12;
         struct {
            HReg reg;
         } R;
      } ARM64riA;
   }
   ARM64RIA;

extern ARM64RIA* ARM64RIA_I12 ( UShort imm12, UChar shift );
extern ARM64RIA* ARM64RIA_R   ( HReg );


/* --------- Reg or "bitfield" (logic immediate) operands --------- */

typedef
   enum {
      ARM64riL_I13=6, /* wierd-o bitfield immediate, 13 bits in total */
      ARM64riL_R      /* reg */
   }
   ARM64RILTag;

typedef
   struct {
      ARM64RILTag tag;
      union {
         struct {
            UChar bitN; /* 0 .. 1 */
            UChar immR; /* 0 .. 63 */
            UChar immS; /* 0 .. 63 */
         } I13;
         struct {
            HReg reg;
         } R;
      } ARM64riL;
   }
   ARM64RIL;

extern ARM64RIL* ARM64RIL_I13 ( UChar bitN, UChar immR, UChar immS );
extern ARM64RIL* ARM64RIL_R   ( HReg );


/* --------------- Reg or uimm6 operands --------------- */

typedef
   enum {
      ARM64ri6_I6=8,  /* uimm6, 1 .. 63 only */
      ARM64ri6_R      /* reg */
   }
   ARM64RI6Tag;

typedef
   struct {
      ARM64RI6Tag tag;
      union {
         struct {
            UInt imm6;   /* 1 .. 63 */
         } I6;
         struct {
            HReg reg;
         } R;
      } ARM64ri6;
   }
   ARM64RI6;

extern ARM64RI6* ARM64RI6_I6 ( UInt imm6 );
extern ARM64RI6* ARM64RI6_R  ( HReg );


/* --------------------- Instructions --------------------- */

typedef
   enum {
      ARM64lo_AND=10,
      ARM64lo_OR,
      ARM64lo_XOR
   }
   ARM64LogicOp;

typedef
   enum {
      ARM64sh_SHL=13,
      ARM64sh_SHR,
      ARM64sh_SAR
   }
   ARM64ShiftOp;

typedef
   enum {
      ARM64un_NEG=16,
      ARM64un_NOT,
      ARM64un_CLZ,
   }
   ARM64UnaryOp;

typedef
   enum {
      ARM64mul_PLAIN=60, /* lo64(64 * 64)  */
      ARM64mul_ZX,       /* hi64(64 *u 64) */
      ARM64mul_SX        /* hi64(64 *s 64) */
   }
   ARM64MulOp;

typedef
   /* These characterise an integer-FP conversion, but don't imply any
      particular direction. */
   enum {
      ARM64cvt_F32_I32S=65,
      ARM64cvt_F64_I32S,
      ARM64cvt_F32_I64S,
      ARM64cvt_F64_I64S,
      ARM64cvt_F32_I32U,
      ARM64cvt_F64_I32U,
      ARM64cvt_F32_I64U,
      ARM64cvt_F64_I64U,
      ARM64cvt_INVALID
   }
   ARM64CvtOp;

typedef
   enum {
      ARM64fpb_ADD=75,
      ARM64fpb_SUB,
      ARM64fpb_MUL,
      ARM64fpb_DIV,
      ARM64fpb_INVALID
   }
   ARM64FpBinOp;

typedef
   enum {
      ARM64fpu_NEG=82,
      ARM64fpu_ABS,
      ARM64fpu_SQRT,
      ARM64fpu_RINT,
      ARM64fpu_INVALID
   }
   ARM64FpUnaryOp;

//ZZ extern const HChar* showARMVfpUnaryOp ( ARMVfpUnaryOp op );
//ZZ 
//ZZ typedef
//ZZ    enum {
//ZZ       ARMneon_VAND=90,
//ZZ       ARMneon_VORR,
//ZZ       ARMneon_VXOR,
//ZZ       ARMneon_VADD,
//ZZ       ARMneon_VADDFP,
//ZZ       ARMneon_VRHADDS,
//ZZ       ARMneon_VRHADDU,
//ZZ       ARMneon_VPADDFP,
//ZZ       ARMneon_VABDFP,
//ZZ       ARMneon_VSUB,
//ZZ       ARMneon_VSUBFP,
//ZZ       ARMneon_VMAXU,
//ZZ       ARMneon_VMAXS,
//ZZ       ARMneon_VMAXF,
//ZZ       ARMneon_VMINU,
//ZZ       ARMneon_VMINS,
//ZZ       ARMneon_VMINF,
//ZZ       ARMneon_VQADDU,
//ZZ       ARMneon_VQADDS,
//ZZ       ARMneon_VQSUBU,
//ZZ       ARMneon_VQSUBS,
//ZZ       ARMneon_VCGTU,
//ZZ       ARMneon_VCGTS,
//ZZ       ARMneon_VCGEU,
//ZZ       ARMneon_VCGES,
//ZZ       ARMneon_VCGTF,
//ZZ       ARMneon_VCGEF,
//ZZ       ARMneon_VCEQ,
//ZZ       ARMneon_VCEQF,
//ZZ       ARMneon_VEXT,
//ZZ       ARMneon_VMUL,
//ZZ       ARMneon_VMULFP,
//ZZ       ARMneon_VMULLU,
//ZZ       ARMneon_VMULLS,
//ZZ       ARMneon_VMULP,
//ZZ       ARMneon_VMULLP,
//ZZ       ARMneon_VQDMULH,
//ZZ       ARMneon_VQRDMULH,
//ZZ       ARMneon_VPADD,
//ZZ       ARMneon_VPMINU,
//ZZ       ARMneon_VPMINS,
//ZZ       ARMneon_VPMINF,
//ZZ       ARMneon_VPMAXU,
//ZZ       ARMneon_VPMAXS,
//ZZ       ARMneon_VPMAXF,
//ZZ       ARMneon_VTBL,
//ZZ       ARMneon_VQDMULL,
//ZZ       ARMneon_VRECPS,
//ZZ       ARMneon_VRSQRTS,
//ZZ       /* ... */
//ZZ    }
//ZZ    ARMNeonBinOp;
//ZZ 
//ZZ typedef
//ZZ    enum {
//ZZ       ARMneon_VSHL=150,
//ZZ       ARMneon_VSAL, /* Yah, not SAR but SAL */
//ZZ       ARMneon_VQSHL,
//ZZ       ARMneon_VQSAL
//ZZ    }
//ZZ    ARMNeonShiftOp;
//ZZ 
//ZZ typedef
//ZZ    enum {
//ZZ       ARMneon_COPY=160,
//ZZ       ARMneon_COPYLU,
//ZZ       ARMneon_COPYLS,
//ZZ       ARMneon_COPYN,
//ZZ       ARMneon_COPYQNSS,
//ZZ       ARMneon_COPYQNUS,
//ZZ       ARMneon_COPYQNUU,
//ZZ       ARMneon_NOT,
//ZZ       ARMneon_EQZ,
//ZZ       ARMneon_DUP,
//ZZ       ARMneon_PADDLS,
//ZZ       ARMneon_PADDLU,
//ZZ       ARMneon_CNT,
//ZZ       ARMneon_CLZ,
//ZZ       ARMneon_CLS,
//ZZ       ARMneon_VCVTxFPxINT,
//ZZ       ARMneon_VQSHLNSS,
//ZZ       ARMneon_VQSHLNUU,
//ZZ       ARMneon_VQSHLNUS,
//ZZ       ARMneon_VCVTFtoU,
//ZZ       ARMneon_VCVTFtoS,
//ZZ       ARMneon_VCVTUtoF,
//ZZ       ARMneon_VCVTStoF,
//ZZ       ARMneon_VCVTFtoFixedU,
//ZZ       ARMneon_VCVTFtoFixedS,
//ZZ       ARMneon_VCVTFixedUtoF,
//ZZ       ARMneon_VCVTFixedStoF,
//ZZ       ARMneon_VCVTF16toF32,
//ZZ       ARMneon_VCVTF32toF16,
//ZZ       ARMneon_REV16,
//ZZ       ARMneon_REV32,
//ZZ       ARMneon_REV64,
//ZZ       ARMneon_ABS,
//ZZ       ARMneon_VNEGF,
//ZZ       ARMneon_VRECIP,
//ZZ       ARMneon_VRECIPF,
//ZZ       ARMneon_VABSFP,
//ZZ       ARMneon_VRSQRTEFP,
//ZZ       ARMneon_VRSQRTE
//ZZ       /* ... */
//ZZ    }
//ZZ    ARMNeonUnOp;
//ZZ 
//ZZ typedef
//ZZ    enum {
//ZZ       ARMneon_SETELEM=200,
//ZZ       ARMneon_GETELEMU,
//ZZ       ARMneon_GETELEMS,
//ZZ       ARMneon_VDUP,
//ZZ    }
//ZZ    ARMNeonUnOpS;
//ZZ 
//ZZ typedef
//ZZ    enum {
//ZZ       ARMneon_TRN=210,
//ZZ       ARMneon_ZIP,
//ZZ       ARMneon_UZP
//ZZ       /* ... */
//ZZ    }
//ZZ    ARMNeonDualOp;
//ZZ 
//ZZ extern const HChar* showARMNeonBinOp ( ARMNeonBinOp op );
//ZZ extern const HChar* showARMNeonUnOp ( ARMNeonUnOp op );
//ZZ extern const HChar* showARMNeonUnOpS ( ARMNeonUnOpS op );
//ZZ extern const HChar* showARMNeonShiftOp ( ARMNeonShiftOp op );
//ZZ extern const HChar* showARMNeonDualOp ( ARMNeonDualOp op );
//ZZ extern const HChar* showARMNeonBinOpDataType ( ARMNeonBinOp op );
//ZZ extern const HChar* showARMNeonUnOpDataType ( ARMNeonUnOp op );
//ZZ extern const HChar* showARMNeonUnOpSDataType ( ARMNeonUnOpS op );
//ZZ extern const HChar* showARMNeonShiftOpDataType ( ARMNeonShiftOp op );
//ZZ extern const HChar* showARMNeonDualOpDataType ( ARMNeonDualOp op );

typedef
   enum {
      /* baseline */
      ARM64in_Arith=1220,
      ARM64in_Cmp,
      ARM64in_Logic,
      ARM64in_Test,
      ARM64in_Shift,
      ARM64in_Unary,
      ARM64in_MovI,        /* int reg-reg move */
      ARM64in_Imm64,
      ARM64in_LdSt64,
      ARM64in_LdSt32,      /* w/ ZX loads */
      ARM64in_LdSt16,      /* w/ ZX loads */
      ARM64in_LdSt8,       /* w/ ZX loads */
      ARM64in_XDirect,     /* direct transfer to GA */
      ARM64in_XIndir,      /* indirect transfer to GA */
      ARM64in_XAssisted,   /* assisted transfer to GA */
      ARM64in_CSel,
      ARM64in_Call,
      ARM64in_AddToSP,     /* move SP by small, signed constant */
      ARM64in_FromSP,      /* move SP to integer register */
      ARM64in_Mul,
//ZZ       ARMin_LdrEX,
//ZZ       ARMin_StrEX,
      /* vector */
      ARM64in_VLdStS,   /* 32-bit FP load/store, with imm offset  */
      ARM64in_VLdStD,   /* 64-bit FP load/store, with imm offset  */
      ARM64in_VLdStQ,
      ARM64in_VCvtI2F,
      ARM64in_VCvtF2I,
      ARM64in_VCvtSD,
      ARM64in_VUnaryD,
      ARM64in_VUnaryS,
      ARM64in_VBinD,
      ARM64in_VBinS,
      ARM64in_VCmpD,
      ARM64in_VCmpS,
      ARM64in_FPCR,
//ZZ       ARMin_VAluS,
//ZZ       ARMin_VCMovD,
//ZZ       ARMin_VCMovS,
//ZZ       ARMin_VXferD,
//ZZ       ARMin_VXferS,
//ZZ       ARMin_VCvtID,
//ZZ       ARMin_MFence,
//ZZ       ARMin_CLREX,
//ZZ       /* Neon */
//ZZ       ARMin_NLdStD,
//ZZ       ARMin_NUnary,
//ZZ       ARMin_NUnaryS,
//ZZ       ARMin_NDual,
//ZZ       ARMin_NBinary,
//ZZ       ARMin_NBinaryS,
//ZZ       ARMin_NShift,
//ZZ       ARMin_NShl64, // special case 64-bit shift of Dreg by immediate
      ARM64in_VImmQ,
      ARM64in_VDfromX,    /* Move an Xreg to a Dreg */
      ARM64in_VQfromXX,   /* Move 2 Xregs to a Qreg */
      ARM64in_VXfromQ,    /* Move half a Qreg to an Xreg */
      ARM64in_VMov,       /* vector reg-reg move, 16, 8 or 4 bytes */
      /* infrastructure */
      ARM64in_EvCheck,     /* Event check */
//ZZ       ARMin_ProfInc      /* 64-bit profile counter increment */
   }
   ARM64InstrTag;

/* Destinations are on the LEFT (first operand) */

typedef
   struct {
      ARM64InstrTag tag;
      union {
         /* --- INTEGER INSTRUCTIONS --- */
         /* 64 bit ADD/SUB reg, reg or uimm12<<{0,12} */
         struct {
            HReg      dst;
            HReg      argL;
            ARM64RIA* argR;
            Bool      isAdd;
         } Arith;
         /* 64 or 32 bit CMP reg, reg or aimm (SUB and set flags) */
         struct {
            HReg      argL;
            ARM64RIA* argR;
            Bool      is64;
         } Cmp;
         /* 64 bit AND/OR/XOR reg, reg or bitfield-immediate */
         struct {
            HReg         dst;
            HReg         argL;
            ARM64RIL*    argR;
            ARM64LogicOp op;
         } Logic;
         /* 64 bit TST reg, reg or bimm (AND and set flags) */
         struct {
            HReg      argL;
            ARM64RIL* argR;
         } Test;
         /* 64 bit SHL/SHR/SAR, 2nd arg is reg or imm */
         struct {
            HReg         dst;
            HReg         argL;
            ARM64RI6*    argR;
            ARM64ShiftOp op;
         } Shift;
         /* NOT/NEG/CLZ, 64 bit only */
         struct {
            HReg         dst;
            HReg         src;
            ARM64UnaryOp op;
         } Unary;
         /* MOV dst, src -- reg-reg move for integer registers */
         struct {
            HReg dst;
            HReg src;
         } MovI;
         /* Pseudo-insn; make a 64-bit immediate */
         struct {
            HReg  dst;
            ULong imm64;
         } Imm64;
         /* 64-bit load or store */
         struct {
            Bool        isLoad;
            HReg        rD;
            ARM64AMode* amode;
         } LdSt64;
         /* zx-32-to-64-bit load, or 32-bit store */
         struct {
            Bool        isLoad;
            HReg        rD;
            ARM64AMode* amode;
         } LdSt32;
         /* zx-16-to-64-bit load, or 16-bit store */
         struct {
            Bool        isLoad;
            HReg        rD;
            ARM64AMode* amode;
         } LdSt16;
         /* zx-8-to-64-bit load, or 8-bit store */
         struct {
            Bool        isLoad;
            HReg        rD;
            ARM64AMode* amode;
         } LdSt8;
         /* Update the guest PC value, then exit requesting to chain
            to it.  May be conditional.  Urr, use of Addr64 implicitly
            assumes that wordsize(guest) == wordsize(host). */
         struct {
            Addr64        dstGA;    /* next guest address */
            ARM64AMode*   amPC;     /* amode in guest state for PC */
            ARM64CondCode cond;     /* can be ARM64cc_AL */
            Bool          toFastEP; /* chain to the slow or fast point? */
         } XDirect;
         /* Boring transfer to a guest address not known at JIT time.
            Not chainable.  May be conditional. */
         struct {
            HReg          dstGA;
            ARM64AMode*   amPC;
            ARM64CondCode cond; /* can be ARM64cc_AL */
         } XIndir;
         /* Assisted transfer to a guest address, most general case.
            Not chainable.  May be conditional. */
         struct {
            HReg          dstGA;
            ARM64AMode*   amPC;
            ARM64CondCode cond; /* can be ARM64cc_AL */
            IRJumpKind    jk;
         } XAssisted;
         /* CSEL: dst = if cond then argL else argR.  cond may be anything. */
          struct {
            HReg          dst;
            HReg          argL;
            HReg          argR;
            ARM64CondCode cond;
         } CSel;
         /* Pseudo-insn.  Call target (an absolute address), on given
            condition (which could be ARM64cc_AL). */
         struct {
            RetLoc        rloc;     /* where the return value will be */
            HWord         target;
            ARM64CondCode cond;
            Int           nArgRegs; /* # regs carrying args: 0 .. 8 */
         } Call;
         /* move SP by small, signed constant */
         struct {
            Int simm; /* needs to be 0 % 16 and in the range -4095
                         .. 4095 inclusive */
         } AddToSP;
         /* move SP to integer register */
         struct {
            HReg dst;
         } FromSP;
         /* Integer multiply, with 3 variants:
              (PLAIN) lo64(64 *  64)
              (ZX)    hi64(64 *u 64)
              (SX)    hi64(64 *s 64)
         */
         struct {
            HReg       dst;
            HReg       argL;
            HReg       argR;
            ARM64MulOp op;
         } Mul;
//ZZ          /* LDREX{,H,B} r2, [r4]  and
//ZZ             LDREXD r2, r3, [r4]   (on LE hosts, transferred value is r3:r2)
//ZZ             Again, hardwired registers since this is not performance
//ZZ             critical, and there are possibly constraints on the
//ZZ             registers that we can't express in the register allocator.*/
//ZZ          struct {
//ZZ             Int  szB; /* 1, 2, 4 or 8 */
//ZZ          } LdrEX;
//ZZ          /* STREX{,H,B} r0, r2, [r4]  and  
//ZZ             STREXD r0, r2, r3, [r4]   (on LE hosts, transferred value is r3:r2)
//ZZ             r0 = SC( [r4] = r2 )      (8, 16, 32 bit transfers)
//ZZ             r0 = SC( [r4] = r3:r2)    (64 bit transfers)
//ZZ             Ditto comment re fixed registers. */
//ZZ          struct {
//ZZ             Int  szB; /* 1, 2, 4 or 8 */
//ZZ          } StrEX;
         /* --- INSTRUCTIONS INVOLVING VECTOR REGISTERS --- */
         /* 32-bit Fp load/store */
         struct {
            Bool isLoad;
            HReg sD;
            HReg rN;
            UInt uimm12;  /* 0 .. 16380 inclusive, 0 % 4 */
         } VLdStS;
         /* 64-bit Fp load/store */
         struct {
            Bool isLoad;
            HReg dD;
            HReg rN;
            UInt uimm12;  /* 0 .. 32760 inclusive, 0 % 8 */
         } VLdStD;
         /* 128-bit Vector load/store. */
         struct {
            Bool isLoad;
            HReg rQ; // data
            HReg rN; // address
         } VLdStQ;
         /* Scalar conversion of int to float. */
         struct {
            ARM64CvtOp how;
            HReg       rD; // dst, a D or S register
            HReg       rS; // src, a W or X register
         } VCvtI2F;
         /* Scalar conversion of float to int, w/ specified RM. */
         struct {
            ARM64CvtOp how;
            HReg       rD; // dst, a W or X register
            HReg       rS; // src, a D or S register
            UChar      armRM; // ARM encoded RM:
                              // 00=nearest, 01=+inf, 10=-inf, 11=zero
         } VCvtF2I;
         /* Convert between 32-bit and 64-bit FP values (both
            ways). (FCVT) */
         struct {
            Bool sToD; /* True: F32->F64.  False: F64->F32 */
            HReg dst;
            HReg src;
         } VCvtSD;
         /* 64-bit FP unary */
         struct {
            ARM64FpUnaryOp op;
            HReg           dst;
            HReg           src;
         } VUnaryD;
         /* 32-bit FP unary */
         struct {
            ARM64FpUnaryOp op;
            HReg           dst;
            HReg           src;
         } VUnaryS;
         /* 64-bit FP binary arithmetic */
         struct {
            ARM64FpBinOp op;
            HReg         dst;
            HReg         argL;
            HReg         argR;
         } VBinD;
         /* 32-bit FP binary arithmetic */
         struct {
            ARM64FpBinOp op;
            HReg         dst;
            HReg         argL;
            HReg         argR;
         } VBinS;
         /* 64-bit FP compare */
         struct {
            HReg argL;
            HReg argR;
         } VCmpD;
         /* 32-bit FP compare */
         struct {
            HReg argL;
            HReg argR;
         } VCmpS;
         /* Move a 32-bit value to/from the FPCR */
         struct {
            Bool toFPCR;
            HReg iReg;
         } FPCR;
//ZZ          /* 32-bit FP binary arithmetic */
//ZZ          struct {
//ZZ             ARMVfpOp op;
//ZZ             HReg     dst;
//ZZ             HReg     argL;
//ZZ             HReg     argR;
//ZZ          } VAluS;
//ZZ          /* 64-bit FP mov src to dst on the given condition, which may
//ZZ             not be ARMcc_AL. */
//ZZ          struct {
//ZZ             ARMCondCode cond;
//ZZ             HReg        dst;
//ZZ             HReg        src;
//ZZ          } VCMovD;
//ZZ          /* 32-bit FP mov src to dst on the given condition, which may
//ZZ             not be ARMcc_AL. */
//ZZ          struct {
//ZZ             ARMCondCode cond;
//ZZ             HReg        dst;
//ZZ             HReg        src;
//ZZ          } VCMovS;
//ZZ          /* Transfer a VFP D reg to/from two integer registers (VMOV) */
//ZZ          struct {
//ZZ             Bool toD;
//ZZ             HReg dD;
//ZZ             HReg rHi;
//ZZ             HReg rLo;
//ZZ          } VXferD;
//ZZ          /* Transfer a VFP S reg to/from an integer register (VMOV) */
//ZZ          struct {
//ZZ             Bool toS;
//ZZ             HReg fD;
//ZZ             HReg rLo;
//ZZ          } VXferS;
//ZZ          /* Convert between 32-bit ints and 64-bit FP values (both ways
//ZZ             and both signednesses). (FSITOD, FUITOD, FTOSID, FTOUID) */
//ZZ          struct {
//ZZ             Bool iToD; /* True: I32->F64.  False: F64->I32 */
//ZZ             Bool syned; /* True: I32 is signed.  False: I32 is unsigned */
//ZZ             HReg dst;
//ZZ             HReg src;
//ZZ          } VCvtID;
//ZZ          /* Mem fence.  An insn which fences all loads and stores as
//ZZ             much as possible before continuing.  On ARM we emit the
//ZZ             sequence
//ZZ                mcr 15,0,r0,c7,c10,4 (DSB)
//ZZ                mcr 15,0,r0,c7,c10,5 (DMB)
//ZZ                mcr 15,0,r0,c7,c5,4 (ISB)
//ZZ             which is probably total overkill, but better safe than
//ZZ             sorry.
//ZZ          */
//ZZ          struct {
//ZZ          } MFence;
//ZZ          /* A CLREX instruction. */
//ZZ          struct {
//ZZ          } CLREX;
//ZZ          /* Neon data processing instruction: 3 registers of the same
//ZZ             length */
//ZZ          struct {
//ZZ             ARMNeonBinOp op;
//ZZ             HReg dst;
//ZZ             HReg argL;
//ZZ             HReg argR;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NBinary;
//ZZ          struct {
//ZZ             ARMNeonBinOp op;
//ZZ             ARMNRS* dst;
//ZZ             ARMNRS* argL;
//ZZ             ARMNRS* argR;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NBinaryS;
//ZZ          struct {
//ZZ             ARMNeonShiftOp op;
//ZZ             HReg dst;
//ZZ             HReg argL;
//ZZ             HReg argR;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NShift;
//ZZ          struct {
//ZZ             HReg dst;
//ZZ             HReg src;
//ZZ             UInt amt; /* 1..63 only */
//ZZ          } NShl64;
//ZZ          struct {
//ZZ             Bool isLoad;
//ZZ             HReg dD;
//ZZ             ARMAModeN *amode;
//ZZ          } NLdStD
//ZZ          struct {
//ZZ             ARMNeonUnOpS op;
//ZZ             ARMNRS*  dst;
//ZZ             ARMNRS*  src;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NUnaryS;
//ZZ          struct {
//ZZ             ARMNeonUnOp op;
//ZZ             HReg  dst;
//ZZ             HReg  src;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NUnary;
//ZZ          /* Takes two arguments and modifies them both. */
//ZZ          struct {
//ZZ             ARMNeonDualOp op;
//ZZ             HReg  arg1;
//ZZ             HReg  arg2;
//ZZ             UInt size;
//ZZ             Bool Q;
//ZZ          } NDual;
         struct {
            HReg   rQ;
            UShort imm; /* Same 1-bit-per-byte encoding as IR */
         } VImmQ;
         struct {
            HReg rD;
            HReg rX;
         } VDfromX;
         struct {
            HReg rQ;
            HReg rXhi;
            HReg rXlo;
         } VQfromXX;
         struct {
            HReg rX;
            HReg rQ;
            UInt laneNo; /* either 0 or 1 */
         } VXfromQ;
         /* MOV dst, src -- reg-reg move for vector registers */
         struct {
            UInt szB; // 16=mov qD,qS;  8=mov dD,dS;  4=mov sD,sS
            HReg dst;
            HReg src;
         } VMov;
         struct {
            ARM64AMode* amCounter;
            ARM64AMode* amFailAddr;
         } EvCheck;
//ZZ          struct {
//ZZ             /* No fields.  The address of the counter to inc is
//ZZ                installed later, post-translation, by patching it in,
//ZZ                as it is not known at translation time. */
//ZZ          } ProfInc;
      } ARM64in;
   }
   ARM64Instr;

//ZZ 
extern ARM64Instr* ARM64Instr_Arith   ( HReg, HReg, ARM64RIA*, Bool isAdd );
extern ARM64Instr* ARM64Instr_Cmp     ( HReg, ARM64RIA*, Bool is64 );
extern ARM64Instr* ARM64Instr_Logic   ( HReg, HReg, ARM64RIL*, ARM64LogicOp );
extern ARM64Instr* ARM64Instr_Test    ( HReg, ARM64RIL* );
extern ARM64Instr* ARM64Instr_Shift   ( HReg, HReg, ARM64RI6*, ARM64ShiftOp );
extern ARM64Instr* ARM64Instr_Unary   ( HReg, HReg, ARM64UnaryOp );
//ZZ extern ARMInstr* ARMInstr_CmpOrTst ( Bool isCmp, HReg, ARMRI84* );
extern ARM64Instr* ARM64Instr_MovI    ( HReg, HReg );
extern ARM64Instr* ARM64Instr_Imm64   ( HReg, ULong );
extern ARM64Instr* ARM64Instr_LdSt64  ( Bool isLoad, HReg, ARM64AMode* );
extern ARM64Instr* ARM64Instr_LdSt32  ( Bool isLoad, HReg, ARM64AMode* );
extern ARM64Instr* ARM64Instr_LdSt16  ( Bool isLoad, HReg, ARM64AMode* );
extern ARM64Instr* ARM64Instr_LdSt8   ( Bool isLoad, HReg, ARM64AMode* );
//ZZ extern ARMInstr* ARMInstr_Ld8S     ( ARMCondCode, HReg, ARMAMode2* );
extern ARM64Instr* ARM64Instr_XDirect ( Addr64 dstGA, ARM64AMode* amPC,
                                        ARM64CondCode cond, Bool toFastEP );
extern ARM64Instr* ARM64Instr_XIndir  ( HReg dstGA, ARM64AMode* amPC,
                                        ARM64CondCode cond );
extern ARM64Instr* ARM64Instr_XAssisted ( HReg dstGA, ARM64AMode* amPC,
                                          ARM64CondCode cond, IRJumpKind jk );
extern ARM64Instr* ARM64Instr_CSel    ( HReg dst, HReg argL, HReg argR,
                                        ARM64CondCode cond );
extern ARM64Instr* ARM64Instr_Call    ( ARM64CondCode, HWord, Int nArgRegs,
                                        RetLoc rloc );
extern ARM64Instr* ARM64Instr_AddToSP ( Int simm );
extern ARM64Instr* ARM64Instr_FromSP  ( HReg dst );
extern ARM64Instr* ARM64Instr_Mul     ( HReg dst, HReg argL, HReg argR,
                                        ARM64MulOp op );

//ZZ extern ARMInstr* ARMInstr_Mul      ( ARMMulOp op );
//ZZ extern ARMInstr* ARMInstr_LdrEX    ( Int szB );
//ZZ extern ARMInstr* ARMInstr_StrEX    ( Int szB );
extern ARM64Instr* ARM64Instr_VLdStS  ( Bool isLoad, HReg sD, HReg rN,
                                        UInt uimm12 /* 0 .. 16380, 0 % 4 */ );
extern ARM64Instr* ARM64Instr_VLdStD  ( Bool isLoad, HReg dD, HReg rN,
                                        UInt uimm12 /* 0 .. 32760, 0 % 8 */ );
extern ARM64Instr* ARM64Instr_VLdStQ  ( Bool isLoad, HReg rQ, HReg rN );
extern ARM64Instr* ARM64Instr_VCvtI2F ( ARM64CvtOp how, HReg rD, HReg rS );
extern ARM64Instr* ARM64Instr_VCvtF2I ( ARM64CvtOp how, HReg rD, HReg rS,
                                        UChar armRM );
extern ARM64Instr* ARM64Instr_VCvtSD  ( Bool sToD, HReg dst, HReg src );
extern ARM64Instr* ARM64Instr_VUnaryD ( ARM64FpUnaryOp op, HReg dst, HReg src );
extern ARM64Instr* ARM64Instr_VUnaryS ( ARM64FpUnaryOp op, HReg dst, HReg src );
extern ARM64Instr* ARM64Instr_VBinD   ( ARM64FpBinOp op, HReg, HReg, HReg );
extern ARM64Instr* ARM64Instr_VBinS   ( ARM64FpBinOp op, HReg, HReg, HReg );
extern ARM64Instr* ARM64Instr_VCmpD   ( HReg argL, HReg argR );
extern ARM64Instr* ARM64Instr_VCmpS   ( HReg argL, HReg argR );
extern ARM64Instr* ARM64Instr_FPCR    ( Bool toFPCR, HReg iReg );
//ZZ extern ARMInstr* ARMInstr_VAluS    ( ARMVfpOp op, HReg, HReg, HReg );
//ZZ extern ARMInstr* ARMInstr_VCMovD   ( ARMCondCode, HReg dst, HReg src );
//ZZ extern ARMInstr* ARMInstr_VCMovS   ( ARMCondCode, HReg dst, HReg src );
//ZZ extern ARMInstr* ARMInstr_VXferD   ( Bool toD, HReg dD, HReg rHi, HReg rLo );
//ZZ extern ARMInstr* ARMInstr_VXferS   ( Bool toS, HReg fD, HReg rLo );
//ZZ extern ARMInstr* ARMInstr_VCvtID   ( Bool iToD, Bool syned,
//ZZ                                      HReg dst, HReg src );
//ZZ extern ARMInstr* ARMInstr_MFence   ( void );
//ZZ extern ARMInstr* ARMInstr_CLREX    ( void );
//ZZ extern ARMInstr* ARMInstr_NLdStD   ( Bool isLoad, HReg, ARMAModeN* );
//ZZ extern ARMInstr* ARMInstr_NUnary   ( ARMNeonUnOp, HReg, HReg, UInt, Bool );
//ZZ extern ARMInstr* ARMInstr_NUnaryS  ( ARMNeonUnOpS, ARMNRS*, ARMNRS*,
//ZZ                                      UInt, Bool );
//ZZ extern ARMInstr* ARMInstr_NDual    ( ARMNeonDualOp, HReg, HReg, UInt, Bool );
//ZZ extern ARMInstr* ARMInstr_NBinary  ( ARMNeonBinOp, HReg, HReg, HReg,
//ZZ                                      UInt, Bool );
//ZZ extern ARMInstr* ARMInstr_NShift   ( ARMNeonShiftOp, HReg, HReg, HReg,
//ZZ                                      UInt, Bool );
//ZZ extern ARMInstr* ARMInstr_NShl64   ( HReg, HReg, UInt );
extern ARM64Instr* ARM64Instr_VImmQ   ( HReg, UShort );
extern ARM64Instr* ARM64Instr_VDfromX ( HReg rD, HReg rX );
extern ARM64Instr* ARM64Instr_VQfromXX( HReg rQ, HReg rXhi, HReg rXlo );
extern ARM64Instr* ARM64Instr_VXfromQ ( HReg rX, HReg rQ, UInt laneNo );
extern ARM64Instr* ARM64Instr_VMov    ( UInt szB, HReg dst, HReg src );

extern ARM64Instr* ARM64Instr_EvCheck ( ARM64AMode* amCounter,
                                        ARM64AMode* amFailAddr );
//ZZ extern ARMInstr* ARMInstr_ProfInc  ( void );

extern void ppARM64Instr ( ARM64Instr* );


/* Some functions that insulate the register allocator from details
   of the underlying instruction set. */
extern void getRegUsage_ARM64Instr ( HRegUsage*, ARM64Instr*, Bool );
extern void mapRegs_ARM64Instr     ( HRegRemap*, ARM64Instr*, Bool );
extern Bool isMove_ARM64Instr      ( ARM64Instr*, HReg*, HReg* );
extern Int  emit_ARM64Instr        ( /*MB_MOD*/Bool* is_profInc,
                                     UChar* buf, Int nbuf, ARM64Instr* i,
                                     Bool mode64,
                                     void* disp_cp_chain_me_to_slowEP,
                                     void* disp_cp_chain_me_to_fastEP,
                                     void* disp_cp_xindir,
                                     void* disp_cp_xassisted );

extern void genSpill_ARM64  ( /*OUT*/HInstr** i1, /*OUT*/HInstr** i2,
                              HReg rreg, Int offset, Bool );
extern void genReload_ARM64 ( /*OUT*/HInstr** i1, /*OUT*/HInstr** i2,
                              HReg rreg, Int offset, Bool );

extern void getAllocableRegs_ARM64 ( Int*, HReg** );
extern HInstrArray* iselSB_ARM64 ( IRSB*, 
                                   VexArch,
                                   VexArchInfo*,
                                   VexAbiInfo*,
                                   Int offs_Host_EvC_Counter,
                                   Int offs_Host_EvC_FailAddr,
                                   Bool chainingAllowed,
                                   Bool addProfInc,
                                   Addr64 max_ga );

/* How big is an event check?  This is kind of a kludge because it
   depends on the offsets of host_EvC_FAILADDR and
   host_EvC_COUNTER. */
extern Int evCheckSzB_ARM64 ( void );

/* Perform a chaining and unchaining of an XDirect jump. */
extern VexInvalRange chainXDirect_ARM64 ( void* place_to_chain,
                                          void* disp_cp_chain_me_EXPECTED,
                                          void* place_to_jump_to );

//ZZ extern VexInvalRange unchainXDirect_ARM ( void* place_to_unchain,
//ZZ                                           void* place_to_jump_to_EXPECTED,
//ZZ                                           void* disp_cp_chain_me );
//ZZ 
//ZZ /* Patch the counter location into an existing ProfInc point. */
//ZZ extern VexInvalRange patchProfInc_ARM ( void*  place_to_patch,
//ZZ                                         ULong* location_of_counter );


#endif /* ndef __VEX_HOST_ARM64_DEFS_H */

/*---------------------------------------------------------------*/
/*--- end                                   host_arm64_defs.h ---*/
/*---------------------------------------------------------------*/
