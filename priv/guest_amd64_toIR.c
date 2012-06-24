
/*--------------------------------------------------------------------*/
/*--- begin                                     guest_amd64_toIR.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2004-2011 OpenWorks LLP
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

   Neither the names of the U.S. Department of Energy nor the
   University of California nor the names of its contributors may be
   used to endorse or promote products derived from this software
   without prior written permission.
*/

/* Translates AMD64 code to IR. */

/* TODO:

   All Puts to CC_OP/CC_DEP1/CC_DEP2/CC_NDEP should really be checked
   to ensure a 64-bit value is being written.

   x87 FP Limitations:
 
   * all arithmetic done at 64 bits
 
   * no FP exceptions, except for handling stack over/underflow
 
   * FP rounding mode observed only for float->int conversions and
     int->float conversions which could lose accuracy, and for
     float-to-float rounding.  For all other operations,
     round-to-nearest is used, regardless.
 
   * FP sin/cos/tan/sincos: C2 flag is always cleared.  IOW the
     simulation claims the argument is in-range (-2^63 <= arg <= 2^63)
     even when it isn't.
 
   * some of the FCOM cases could do with testing -- not convinced
     that the args are the right way round.
 
   * FSAVE does not re-initialise the FPU; it should do
 
   * FINIT not only initialises the FPU environment, it also zeroes
     all the FP registers.  It should leave the registers unchanged.
 
    RDTSC returns zero, always.
 
    SAHF should cause eflags[1] == 1, and in fact it produces 0.  As
    per Intel docs this bit has no meaning anyway.  Since PUSHF is the
    only way to observe eflags[1], a proper fix would be to make that
    bit be set by PUSHF.
 
    This module uses global variables and so is not MT-safe (if that
    should ever become relevant).
*/

/* Notes re address size overrides (0x67).

   According to the AMD documentation (24594 Rev 3.09, Sept 2003,
   "AMD64 Architecture Programmer's Manual Volume 3: General-Purpose
   and System Instructions"), Section 1.2.3 ("Address-Size Override
   Prefix"):

   0x67 applies to all explicit memory references, causing the top
   32 bits of the effective address to become zero.

   0x67 has no effect on stack references (push/pop); these always
   use a 64-bit address.

   0x67 changes the interpretation of instructions which implicitly
   reference RCX/RSI/RDI, so that in fact ECX/ESI/EDI are used
   instead.  These are:

      cmp{s,sb,sw,sd,sq}
      in{s,sb,sw,sd}
      jcxz, jecxz, jrcxz
      lod{s,sb,sw,sd,sq}
      loop{,e,bz,be,z}
      mov{s,sb,sw,sd,sq}
      out{s,sb,sw,sd}
      rep{,e,ne,nz}
      sca{s,sb,sw,sd,sq}
      sto{s,sb,sw,sd,sq}
      xlat{,b} */

/* "Special" instructions.

   This instruction decoder can decode three special instructions
   which mean nothing natively (are no-ops as far as regs/mem are
   concerned) but have meaning for supporting Valgrind.  A special
   instruction is flagged by the 16-byte preamble 48C1C703 48C1C70D
   48C1C73D 48C1C733 (in the standard interpretation, that means: rolq
   $3, %rdi; rolq $13, %rdi; rolq $61, %rdi; rolq $51, %rdi).
   Following that, one of the following 3 are allowed (standard
   interpretation in parentheses):

      4887DB (xchgq %rbx,%rbx)   %RDX = client_request ( %RAX )
      4887C9 (xchgq %rcx,%rcx)   %RAX = guest_NRADDR
      4887D2 (xchgq %rdx,%rdx)   call-noredir *%RAX

   Any other bytes following the 16-byte preamble are illegal and
   constitute a failure in instruction decoding.  This all assumes
   that the preamble will never occur except in specific code
   fragments designed for Valgrind to catch.

   No prefixes may precede a "Special" instruction.
*/

/* casLE (implementation of lock-prefixed insns) and rep-prefixed
   insns: the side-exit back to the start of the insn is done with
   Ijk_Boring.  This is quite wrong, it should be done with
   Ijk_NoRedir, since otherwise the side exit, which is intended to
   restart the instruction for whatever reason, could go somewhere
   entirely else.  Doing it right (with Ijk_NoRedir jumps) would make
   no-redir jumps performance critical, at least for rep-prefixed
   instructions, since all iterations thereof would involve such a
   jump.  It's not such a big deal with casLE since the side exit is
   only taken if the CAS fails, that is, the location is contended,
   which is relatively unlikely.

   Note also, the test for CAS success vs failure is done using
   Iop_CasCmp{EQ,NE}{8,16,32,64} rather than the ordinary
   Iop_Cmp{EQ,NE} equivalents.  This is so as to tell Memcheck that it
   shouldn't definedness-check these comparisons.  See
   COMMENT_ON_CasCmpEQ in memcheck/mc_translate.c for
   background/rationale.
*/

/* LOCK prefixed instructions.  These are translated using IR-level
   CAS statements (IRCAS) and are believed to preserve atomicity, even
   from the point of view of some other process racing against a
   simulated one (presumably they communicate via a shared memory
   segment).

   Handlers which are aware of LOCK prefixes are:
      dis_op2_G_E      (add, or, adc, sbb, and, sub, xor)
      dis_cmpxchg_G_E  (cmpxchg)
      dis_Grp1         (add, or, adc, sbb, and, sub, xor)
      dis_Grp3         (not, neg)
      dis_Grp4         (inc, dec)
      dis_Grp5         (inc, dec)
      dis_Grp8_Imm     (bts, btc, btr)
      dis_bt_G_E       (bts, btc, btr)
      dis_xadd_G_E     (xadd)
*/


#include "libvex_basictypes.h"
#include "libvex_ir.h"
#include "libvex.h"
#include "libvex_guest_amd64.h"

#include "main_util.h"
#include "main_globals.h"
#include "guest_generic_bb_to_IR.h"
#include "guest_generic_x87.h"
#include "guest_amd64_defs.h"


/*------------------------------------------------------------*/
/*--- Globals                                              ---*/
/*------------------------------------------------------------*/

/* These are set at the start of the translation of an insn, right
   down in disInstr_AMD64, so that we don't have to pass them around
   endlessly.  They are all constant during the translation of any
   given insn. */

/* These are set at the start of the translation of a BB, so
   that we don't have to pass them around endlessly. */

/* We need to know this to do sub-register accesses correctly. */
static Bool host_is_bigendian;

/* Pointer to the guest code area (points to start of BB, not to the
   insn being processed). */
static UChar* guest_code;

/* The guest address corresponding to guest_code[0]. */
static Addr64 guest_RIP_bbstart;

/* The guest address for the instruction currently being
   translated. */
static Addr64 guest_RIP_curr_instr;

/* The IRSB* into which we're generating code. */
static IRSB* irsb;

/* For ensuring that %rip-relative addressing is done right.  A read
   of %rip generates the address of the next instruction.  It may be
   that we don't conveniently know that inside disAMode().  For sanity
   checking, if the next insn %rip is needed, we make a guess at what
   it is, record that guess here, and set the accompanying Bool to
   indicate that -- after this insn's decode is finished -- that guess
   needs to be checked.  */

/* At the start of each insn decode, is set to (0, False).
   After the decode, if _mustcheck is now True, _assumed is
   checked. */

static Addr64 guest_RIP_next_assumed;
static Bool   guest_RIP_next_mustcheck;


/*------------------------------------------------------------*/
/*--- Helpers for constructing IR.                         ---*/
/*------------------------------------------------------------*/
 
/* Generate a new temporary of the given type. */
static IRTemp newTemp ( IRType ty )
{
   vassert(isPlausibleIRType(ty));
   return newIRTemp( irsb->tyenv, ty );
}

/* Add a statement to the list held by "irsb". */
static void stmt ( IRStmt* st )
{
   addStmtToIRSB( irsb, st );
}

/* Generate a statement "dst := e". */ 
static void assign ( IRTemp dst, IRExpr* e )
{
   stmt( IRStmt_WrTmp(dst, e) );
}

static IRExpr* unop ( IROp op, IRExpr* a )
{
   return IRExpr_Unop(op, a);
}

static IRExpr* binop ( IROp op, IRExpr* a1, IRExpr* a2 )
{
   return IRExpr_Binop(op, a1, a2);
}

static IRExpr* triop ( IROp op, IRExpr* a1, IRExpr* a2, IRExpr* a3 )
{
   return IRExpr_Triop(op, a1, a2, a3);
}

static IRExpr* mkexpr ( IRTemp tmp )
{
   return IRExpr_RdTmp(tmp);
}

static IRExpr* mkU8 ( ULong i )
{
   vassert(i < 256);
   return IRExpr_Const(IRConst_U8( (UChar)i ));
}

static IRExpr* mkU16 ( ULong i )
{
   vassert(i < 0x10000ULL);
   return IRExpr_Const(IRConst_U16( (UShort)i ));
}

static IRExpr* mkU32 ( ULong i )
{
   vassert(i < 0x100000000ULL);
   return IRExpr_Const(IRConst_U32( (UInt)i ));
}

static IRExpr* mkU64 ( ULong i )
{
   return IRExpr_Const(IRConst_U64(i));
}

static IRExpr* mkU ( IRType ty, ULong i )
{
   switch (ty) {
      case Ity_I8:  return mkU8(i);
      case Ity_I16: return mkU16(i);
      case Ity_I32: return mkU32(i);
      case Ity_I64: return mkU64(i);
      default: vpanic("mkU(amd64)");
   }
}

static void storeLE ( IRExpr* addr, IRExpr* data )
{
   stmt( IRStmt_Store(Iend_LE, addr, data) );
}

static IRExpr* loadLE ( IRType ty, IRExpr* addr )
{
   return IRExpr_Load(Iend_LE, ty, addr);
}

static IROp mkSizedOp ( IRType ty, IROp op8 )
{
   vassert(op8 == Iop_Add8 || op8 == Iop_Sub8 
           || op8 == Iop_Mul8 
           || op8 == Iop_Or8 || op8 == Iop_And8 || op8 == Iop_Xor8
           || op8 == Iop_Shl8 || op8 == Iop_Shr8 || op8 == Iop_Sar8
           || op8 == Iop_CmpEQ8 || op8 == Iop_CmpNE8
           || op8 == Iop_CasCmpNE8
           || op8 == Iop_Not8 );
   switch (ty) {
      case Ity_I8:  return 0 +op8;
      case Ity_I16: return 1 +op8;
      case Ity_I32: return 2 +op8;
      case Ity_I64: return 3 +op8;
      default: vpanic("mkSizedOp(amd64)");
   }
}

static 
IRExpr* doScalarWidening ( Int szSmall, Int szBig, Bool signd, IRExpr* src )
{
   if (szSmall == 1 && szBig == 4) {
      return unop(signd ? Iop_8Sto32 : Iop_8Uto32, src);
   }
   if (szSmall == 1 && szBig == 2) {
      return unop(signd ? Iop_8Sto16 : Iop_8Uto16, src);
   }
   if (szSmall == 2 && szBig == 4) {
      return unop(signd ? Iop_16Sto32 : Iop_16Uto32, src);
   }
   if (szSmall == 1 && szBig == 8 && !signd) {
      return unop(Iop_8Uto64, src);
   }
   if (szSmall == 1 && szBig == 8 && signd) {
      return unop(Iop_8Sto64, src);
   }
   if (szSmall == 2 && szBig == 8 && !signd) {
      return unop(Iop_16Uto64, src);
   }
   if (szSmall == 2 && szBig == 8 && signd) {
      return unop(Iop_16Sto64, src);
   }
   vpanic("doScalarWidening(amd64)");
}



/*------------------------------------------------------------*/
/*--- Debugging output                                     ---*/
/*------------------------------------------------------------*/

/* Bomb out if we can't handle something. */
__attribute__ ((noreturn))
static void unimplemented ( HChar* str )
{
   vex_printf("amd64toIR: unimplemented feature\n");
   vpanic(str);
}

#define DIP(format, args...)           \
   if (vex_traceflags & VEX_TRACE_FE)  \
      vex_printf(format, ## args)

#define DIS(buf, format, args...)      \
   if (vex_traceflags & VEX_TRACE_FE)  \
      vex_sprintf(buf, format, ## args)


/*------------------------------------------------------------*/
/*--- Offsets of various parts of the amd64 guest state.   ---*/
/*------------------------------------------------------------*/

#define OFFB_RAX       offsetof(VexGuestAMD64State,guest_RAX)
#define OFFB_RBX       offsetof(VexGuestAMD64State,guest_RBX)
#define OFFB_RCX       offsetof(VexGuestAMD64State,guest_RCX)
#define OFFB_RDX       offsetof(VexGuestAMD64State,guest_RDX)
#define OFFB_RSP       offsetof(VexGuestAMD64State,guest_RSP)
#define OFFB_RBP       offsetof(VexGuestAMD64State,guest_RBP)
#define OFFB_RSI       offsetof(VexGuestAMD64State,guest_RSI)
#define OFFB_RDI       offsetof(VexGuestAMD64State,guest_RDI)
#define OFFB_R8        offsetof(VexGuestAMD64State,guest_R8)
#define OFFB_R9        offsetof(VexGuestAMD64State,guest_R9)
#define OFFB_R10       offsetof(VexGuestAMD64State,guest_R10)
#define OFFB_R11       offsetof(VexGuestAMD64State,guest_R11)
#define OFFB_R12       offsetof(VexGuestAMD64State,guest_R12)
#define OFFB_R13       offsetof(VexGuestAMD64State,guest_R13)
#define OFFB_R14       offsetof(VexGuestAMD64State,guest_R14)
#define OFFB_R15       offsetof(VexGuestAMD64State,guest_R15)

#define OFFB_RIP       offsetof(VexGuestAMD64State,guest_RIP)

#define OFFB_FS_ZERO   offsetof(VexGuestAMD64State,guest_FS_ZERO)
#define OFFB_GS_0x60   offsetof(VexGuestAMD64State,guest_GS_0x60)

#define OFFB_CC_OP     offsetof(VexGuestAMD64State,guest_CC_OP)
#define OFFB_CC_DEP1   offsetof(VexGuestAMD64State,guest_CC_DEP1)
#define OFFB_CC_DEP2   offsetof(VexGuestAMD64State,guest_CC_DEP2)
#define OFFB_CC_NDEP   offsetof(VexGuestAMD64State,guest_CC_NDEP)

#define OFFB_FPREGS    offsetof(VexGuestAMD64State,guest_FPREG[0])
#define OFFB_FPTAGS    offsetof(VexGuestAMD64State,guest_FPTAG[0])
#define OFFB_DFLAG     offsetof(VexGuestAMD64State,guest_DFLAG)
#define OFFB_ACFLAG    offsetof(VexGuestAMD64State,guest_ACFLAG)
#define OFFB_IDFLAG    offsetof(VexGuestAMD64State,guest_IDFLAG)
#define OFFB_FTOP      offsetof(VexGuestAMD64State,guest_FTOP)
#define OFFB_FC3210    offsetof(VexGuestAMD64State,guest_FC3210)
#define OFFB_FPROUND   offsetof(VexGuestAMD64State,guest_FPROUND)

#define OFFB_SSEROUND  offsetof(VexGuestAMD64State,guest_SSEROUND)
#define OFFB_YMM0      offsetof(VexGuestAMD64State,guest_YMM0)
#define OFFB_YMM1      offsetof(VexGuestAMD64State,guest_YMM1)
#define OFFB_YMM2      offsetof(VexGuestAMD64State,guest_YMM2)
#define OFFB_YMM3      offsetof(VexGuestAMD64State,guest_YMM3)
#define OFFB_YMM4      offsetof(VexGuestAMD64State,guest_YMM4)
#define OFFB_YMM5      offsetof(VexGuestAMD64State,guest_YMM5)
#define OFFB_YMM6      offsetof(VexGuestAMD64State,guest_YMM6)
#define OFFB_YMM7      offsetof(VexGuestAMD64State,guest_YMM7)
#define OFFB_YMM8      offsetof(VexGuestAMD64State,guest_YMM8)
#define OFFB_YMM9      offsetof(VexGuestAMD64State,guest_YMM9)
#define OFFB_YMM10     offsetof(VexGuestAMD64State,guest_YMM10)
#define OFFB_YMM11     offsetof(VexGuestAMD64State,guest_YMM11)
#define OFFB_YMM12     offsetof(VexGuestAMD64State,guest_YMM12)
#define OFFB_YMM13     offsetof(VexGuestAMD64State,guest_YMM13)
#define OFFB_YMM14     offsetof(VexGuestAMD64State,guest_YMM14)
#define OFFB_YMM15     offsetof(VexGuestAMD64State,guest_YMM15)
#define OFFB_YMM16     offsetof(VexGuestAMD64State,guest_YMM16)

#define OFFB_EMWARN    offsetof(VexGuestAMD64State,guest_EMWARN)
#define OFFB_TISTART   offsetof(VexGuestAMD64State,guest_TISTART)
#define OFFB_TILEN     offsetof(VexGuestAMD64State,guest_TILEN)

#define OFFB_NRADDR    offsetof(VexGuestAMD64State,guest_NRADDR)


/*------------------------------------------------------------*/
/*--- Helper bits and pieces for deconstructing the        ---*/
/*--- amd64 insn stream.                                   ---*/
/*------------------------------------------------------------*/

/* This is the AMD64 register encoding -- integer regs. */
#define R_RAX 0
#define R_RCX 1
#define R_RDX 2
#define R_RBX 3
#define R_RSP 4
#define R_RBP 5
#define R_RSI 6
#define R_RDI 7
#define R_R8  8
#define R_R9  9
#define R_R10 10
#define R_R11 11
#define R_R12 12
#define R_R13 13
#define R_R14 14
#define R_R15 15

/* This is the Intel register encoding -- segment regs. */
#define R_ES 0
#define R_CS 1
#define R_SS 2
#define R_DS 3
#define R_FS 4
#define R_GS 5


/* Various simple conversions */

static ULong extend_s_8to64 ( UChar x )
{
   return (ULong)((((Long)x) << 56) >> 56);
}

static ULong extend_s_16to64 ( UShort x )
{
   return (ULong)((((Long)x) << 48) >> 48);
}

static ULong extend_s_32to64 ( UInt x )
{
   return (ULong)((((Long)x) << 32) >> 32);
}

/* Figure out whether the mod and rm parts of a modRM byte refer to a
   register or memory.  If so, the byte will have the form 11XXXYYY,
   where YYY is the register number. */
inline
static Bool epartIsReg ( UChar mod_reg_rm )
{
   return toBool(0xC0 == (mod_reg_rm & 0xC0));
}

/* Extract the 'g' field from a modRM byte.  This only produces 3
   bits, which is not a complete register number.  You should avoid
   this function if at all possible. */
inline
static Int gregLO3ofRM ( UChar mod_reg_rm )
{
   return (Int)( (mod_reg_rm >> 3) & 7 );
}

/* Ditto the 'e' field of a modRM byte. */
inline
static Int eregLO3ofRM ( UChar mod_reg_rm )
{
   return (Int)(mod_reg_rm & 0x7);
}

/* Get a 8/16/32-bit unsigned value out of the insn stream. */

static inline UChar getUChar ( Long delta )
{
   UChar v = guest_code[delta+0];
   return v;
}

static UInt getUDisp16 ( Long delta )
{
   UInt v = guest_code[delta+1]; v <<= 8;
   v |= guest_code[delta+0];
   return v & 0xFFFF;
}

//.. static UInt getUDisp ( Int size, Long delta )
//.. {
//..    switch (size) {
//..       case 4: return getUDisp32(delta);
//..       case 2: return getUDisp16(delta);
//..       case 1: return getUChar(delta);
//..       default: vpanic("getUDisp(x86)");
//..    }
//..    return 0; /*notreached*/
//.. }


/* Get a byte value out of the insn stream and sign-extend to 64
   bits. */
static Long getSDisp8 ( Long delta )
{
   return extend_s_8to64( guest_code[delta] );
}

/* Get a 16-bit value out of the insn stream and sign-extend to 64
   bits. */
static Long getSDisp16 ( Long delta )
{
   UInt v = guest_code[delta+1]; v <<= 8;
   v |= guest_code[delta+0];
   return extend_s_16to64( (UShort)v );
}

/* Get a 32-bit value out of the insn stream and sign-extend to 64
   bits. */
static Long getSDisp32 ( Long delta )
{
   UInt v = guest_code[delta+3]; v <<= 8;
   v |= guest_code[delta+2]; v <<= 8;
   v |= guest_code[delta+1]; v <<= 8;
   v |= guest_code[delta+0];
   return extend_s_32to64( v );
}

/* Get a 64-bit value out of the insn stream. */
static Long getDisp64 ( Long delta )
{
   ULong v = 0;
   v |= guest_code[delta+7]; v <<= 8;
   v |= guest_code[delta+6]; v <<= 8;
   v |= guest_code[delta+5]; v <<= 8;
   v |= guest_code[delta+4]; v <<= 8;
   v |= guest_code[delta+3]; v <<= 8;
   v |= guest_code[delta+2]; v <<= 8;
   v |= guest_code[delta+1]; v <<= 8;
   v |= guest_code[delta+0];
   return v;
}

/* Note: because AMD64 doesn't allow 64-bit literals, it is an error
   if this is called with size==8.  Should not happen. */
static Long getSDisp ( Int size, Long delta )
{
   switch (size) {
      case 4: return getSDisp32(delta);
      case 2: return getSDisp16(delta);
      case 1: return getSDisp8(delta);
      default: vpanic("getSDisp(amd64)");
  }
}

static ULong mkSizeMask ( Int sz )
{
   switch (sz) {
      case 1: return 0x00000000000000FFULL;
      case 2: return 0x000000000000FFFFULL;
      case 4: return 0x00000000FFFFFFFFULL;
      case 8: return 0xFFFFFFFFFFFFFFFFULL;
      default: vpanic("mkSzMask(amd64)");
   }
}

static Int imin ( Int a, Int b )
{
   return (a < b) ? a : b;
}

static IRType szToITy ( Int n )
{
   switch (n) {
      case 1: return Ity_I8;
      case 2: return Ity_I16;
      case 4: return Ity_I32;
      case 8: return Ity_I64;
      default: vex_printf("\nszToITy(%d)\n", n);
               vpanic("szToITy(amd64)");
   }
}


/*------------------------------------------------------------*/
/*--- For dealing with prefixes.                           ---*/
/*------------------------------------------------------------*/

/* The idea is to pass around an int holding a bitmask summarising
   info from the prefixes seen on the current instruction, including
   info from the REX byte.  This info is used in various places, but
   most especially when making sense of register fields in
   instructions.

   The top 8 bits of the prefix are 0x55, just as a hacky way to
   ensure it really is a valid prefix.

   Things you can safely assume about a well-formed prefix:
   * at most one segment-override bit (CS,DS,ES,FS,GS,SS) is set.
   * if REX is not present then REXW,REXR,REXX,REXB will read
     as zero.
   * F2 and F3 will not both be 1.
*/

typedef UInt  Prefix;

#define PFX_ASO    (1<<0)    /* address-size override present (0x67) */
#define PFX_66     (1<<1)    /* operand-size override-to-16 present (0x66) */
#define PFX_REX    (1<<2)    /* REX byte present (0x40 to 0x4F) */
#define PFX_REXW   (1<<3)    /* REX W bit, if REX present, else 0 */
#define PFX_REXR   (1<<4)    /* REX R bit, if REX present, else 0 */
#define PFX_REXX   (1<<5)    /* REX X bit, if REX present, else 0 */
#define PFX_REXB   (1<<6)    /* REX B bit, if REX present, else 0 */
#define PFX_LOCK   (1<<7)    /* bus LOCK prefix present (0xF0) */
#define PFX_F2     (1<<8)    /* REP/REPE/REPZ prefix present (0xF2) */
#define PFX_F3     (1<<9)    /* REPNE/REPNZ prefix present (0xF3) */
#define PFX_CS     (1<<10)   /* CS segment prefix present (0x2E) */
#define PFX_DS     (1<<11)   /* DS segment prefix present (0x3E) */
#define PFX_ES     (1<<12)   /* ES segment prefix present (0x26) */
#define PFX_FS     (1<<13)   /* FS segment prefix present (0x64) */
#define PFX_GS     (1<<14)   /* GS segment prefix present (0x65) */
#define PFX_SS     (1<<15)   /* SS segment prefix present (0x36) */
#define PFX_VEX    (1<<16)   /* VEX prefix present (0xC4 or 0xC5) */
#define PFX_VEXL   (1<<17)   /* VEX L bit, if VEX present, else 0 */
/* The extra register field VEX.vvvv is encoded (after not-ing it) as
   PFX_VEXnV3 .. PFX_VEXnV0, so these must occupy adjacent bit
   positions. */
#define PFX_VEXnV0 (1<<18)   /* ~VEX vvvv[0], if VEX present, else 0 */
#define PFX_VEXnV1 (1<<19)   /* ~VEX vvvv[1], if VEX present, else 0 */
#define PFX_VEXnV2 (1<<20)   /* ~VEX vvvv[2], if VEX present, else 0 */
#define PFX_VEXnV3 (1<<21)   /* ~VEX vvvv[3], if VEX present, else 0 */


#define PFX_EMPTY 0x55000000

static Bool IS_VALID_PFX ( Prefix pfx ) {
   return toBool((pfx & 0xFF000000) == PFX_EMPTY);
}

static Bool haveREX ( Prefix pfx ) {
   return toBool(pfx & PFX_REX);
}

static Int getRexW ( Prefix pfx ) {
   return (pfx & PFX_REXW) ? 1 : 0;
}
static Int getRexR ( Prefix pfx ) {
   return (pfx & PFX_REXR) ? 1 : 0;
}
static Int getRexX ( Prefix pfx ) {
   return (pfx & PFX_REXX) ? 1 : 0;
}
static Int getRexB ( Prefix pfx ) {
   return (pfx & PFX_REXB) ? 1 : 0;
}

/* Check a prefix doesn't have F2 or F3 set in it, since usually that
   completely changes what instruction it really is. */
static Bool haveF2orF3 ( Prefix pfx ) {
   return toBool((pfx & (PFX_F2|PFX_F3)) > 0);
}
static Bool haveF2 ( Prefix pfx ) {
   return toBool((pfx & PFX_F2) > 0);
}
static Bool haveF3 ( Prefix pfx ) {
   return toBool((pfx & PFX_F3) > 0);
}

static Bool have66 ( Prefix pfx ) {
   return toBool((pfx & PFX_66) > 0);
}
static Bool haveASO ( Prefix pfx ) {
   return toBool((pfx & PFX_ASO) > 0);
}

/* Return True iff pfx has 66 set and F2 and F3 clear */
static Bool have66noF2noF3 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_66|PFX_F2|PFX_F3)) == PFX_66);
}

/* Return True iff pfx has F2 set and 66 and F3 clear */
static Bool haveF2no66noF3 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_66|PFX_F2|PFX_F3)) == PFX_F2);
}

/* Return True iff pfx has F3 set and 66 and F2 clear */
static Bool haveF3no66noF2 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_66|PFX_F2|PFX_F3)) == PFX_F3);
}

/* Return True iff pfx has F3 set and F2 clear */
static Bool haveF3noF2 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_F2|PFX_F3)) == PFX_F3);
}

/* Return True iff pfx has F2 set and F3 clear */
static Bool haveF2noF3 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_F2|PFX_F3)) == PFX_F2);
}

/* Return True iff pfx has 66, F2 and F3 clear */
static Bool haveNo66noF2noF3 ( Prefix pfx )
{
  return 
     toBool((pfx & (PFX_66|PFX_F2|PFX_F3)) == 0);
}

/* Return True iff pfx has any of 66, F2 and F3 set */
static Bool have66orF2orF3 ( Prefix pfx )
{
  return toBool( ! haveNo66noF2noF3(pfx) );
}

/* Return True iff pfx has 66 or F2 set */
static Bool have66orF2 ( Prefix pfx )
{
   return toBool((pfx & (PFX_66|PFX_F2)) > 0);
}

/* Clear all the segment-override bits in a prefix. */
static Prefix clearSegBits ( Prefix p )
{
   return 
      p & ~(PFX_CS | PFX_DS | PFX_ES | PFX_FS | PFX_GS | PFX_SS);
}

/* Get the (inverted, hence back to "normal") VEX.vvvv field. */
static UInt getVexNvvvv ( Prefix pfx ) {
   UInt r = (UInt)pfx;
   r /= (UInt)PFX_VEXnV0; /* pray this turns into a shift */
   return r & 0xF;
}

static Bool haveVEX ( Prefix pfx ) {
   return toBool(pfx & PFX_VEX);
}

static Int getVexL ( Prefix pfx ) {
   return (pfx & PFX_VEXL) ? 1 : 0;
}


/*------------------------------------------------------------*/
/*--- For dealing with escapes                             ---*/
/*------------------------------------------------------------*/


/* Escapes come after the prefixes, but before the primary opcode
   byte.  They escape the primary opcode byte into a bigger space.
   The 0xF0000000 isn't significant, except so as to make it not
   overlap valid Prefix values, for sanity checking.
*/

typedef
   enum { 
      ESC_NONE=0xF0000000, // none
      ESC_0F,              // 0F
      ESC_0F38,            // 0F 38
      ESC_0F3A             // 0F 3A
   }
   Escape;


/*------------------------------------------------------------*/
/*--- For dealing with integer registers                   ---*/
/*------------------------------------------------------------*/

/* This is somewhat complex.  The rules are:

   For 64, 32 and 16 bit register references, the e or g fields in the
   modrm bytes supply the low 3 bits of the register number.  The
   fourth (most-significant) bit of the register number is supplied by
   the REX byte, if it is present; else that bit is taken to be zero.

   The REX.R bit supplies the high bit corresponding to the g register
   field, and the REX.B bit supplies the high bit corresponding to the
   e register field (when the mod part of modrm indicates that modrm's
   e component refers to a register and not to memory).

   The REX.X bit supplies a high register bit for certain registers
   in SIB address modes, and is generally rarely used.

   For 8 bit register references, the presence of the REX byte itself
   has significance.  If there is no REX present, then the 3-bit
   number extracted from the modrm e or g field is treated as an index
   into the sequence %al %cl %dl %bl %ah %ch %dh %bh -- that is, the
   old x86 encoding scheme.

   But if there is a REX present, the register reference is
   interpreted in the same way as for 64/32/16-bit references: a high
   bit is extracted from REX, giving a 4-bit number, and the denoted
   register is the lowest 8 bits of the 16 integer registers denoted
   by the number.  In particular, values 3 through 7 of this sequence
   do not refer to %ah %ch %dh %bh but instead to the lowest 8 bits of
   %rsp %rbp %rsi %rdi.

   The REX.W bit has no bearing at all on register numbers.  Instead
   its presence indicates that the operand size is to be overridden
   from its default value (32 bits) to 64 bits instead.  This is in
   the same fashion that an 0x66 prefix indicates the operand size is
   to be overridden from 32 bits down to 16 bits.  When both REX.W and
   0x66 are present there is a conflict, and REX.W takes precedence.

   Rather than try to handle this complexity using a single huge
   function, several smaller ones are provided.  The aim is to make it
   as difficult as possible to screw up register decoding in a subtle
   and hard-to-track-down way.

   Because these routines fish around in the host's memory (that is,
   in the guest state area) for sub-parts of guest registers, their
   correctness depends on the host's endianness.  So far these
   routines only work for little-endian hosts.  Those for which
   endianness is important have assertions to ensure sanity.
*/


/* About the simplest question you can ask: where do the 64-bit
   integer registers live (in the guest state) ? */

static Int integerGuestReg64Offset ( UInt reg )
{
   switch (reg) {
      case R_RAX: return OFFB_RAX;
      case R_RCX: return OFFB_RCX;
      case R_RDX: return OFFB_RDX;
      case R_RBX: return OFFB_RBX;
      case R_RSP: return OFFB_RSP;
      case R_RBP: return OFFB_RBP;
      case R_RSI: return OFFB_RSI;
      case R_RDI: return OFFB_RDI;
      case R_R8:  return OFFB_R8;
      case R_R9:  return OFFB_R9;
      case R_R10: return OFFB_R10;
      case R_R11: return OFFB_R11;
      case R_R12: return OFFB_R12;
      case R_R13: return OFFB_R13;
      case R_R14: return OFFB_R14;
      case R_R15: return OFFB_R15;
      default: vpanic("integerGuestReg64Offset(amd64)");
   }
}


/* Produce the name of an integer register, for printing purposes.
   reg is a number in the range 0 .. 15 that has been generated from a
   3-bit reg-field number and a REX extension bit.  irregular denotes
   the case where sz==1 and no REX byte is present. */

static 
HChar* nameIReg ( Int sz, UInt reg, Bool irregular )
{
   static HChar* ireg64_names[16]
     = { "%rax", "%rcx", "%rdx", "%rbx", "%rsp", "%rbp", "%rsi", "%rdi",
         "%r8",  "%r9",  "%r10", "%r11", "%r12", "%r13", "%r14", "%r15" };
   static HChar* ireg32_names[16]
     = { "%eax", "%ecx", "%edx", "%ebx", "%esp", "%ebp", "%esi", "%edi",
         "%r8d", "%r9d", "%r10d","%r11d","%r12d","%r13d","%r14d","%r15d" };
   static HChar* ireg16_names[16]
     = { "%ax",  "%cx",  "%dx",  "%bx",  "%sp",  "%bp",  "%si",  "%di",
         "%r8w", "%r9w", "%r10w","%r11w","%r12w","%r13w","%r14w","%r15w" };
   static HChar* ireg8_names[16]
     = { "%al",  "%cl",  "%dl",  "%bl",  "%spl", "%bpl", "%sil", "%dil",
         "%r8b", "%r9b", "%r10b","%r11b","%r12b","%r13b","%r14b","%r15b" };
   static HChar* ireg8_irregular[8] 
     = { "%al", "%cl", "%dl", "%bl", "%ah", "%ch", "%dh", "%bh" };

   vassert(reg < 16);
   if (sz == 1) {
      if (irregular)
         vassert(reg < 8);
   } else {
      vassert(irregular == False);
   }

   switch (sz) {
      case 8: return ireg64_names[reg];
      case 4: return ireg32_names[reg];
      case 2: return ireg16_names[reg];
      case 1: if (irregular) {
                 return ireg8_irregular[reg];
              } else {
                 return ireg8_names[reg];
              }
      default: vpanic("nameIReg(amd64)");
   }
}

/* Using the same argument conventions as nameIReg, produce the
   guest state offset of an integer register. */

static 
Int offsetIReg ( Int sz, UInt reg, Bool irregular )
{
   vassert(reg < 16);
   if (sz == 1) {
      if (irregular)
         vassert(reg < 8);
   } else {
      vassert(irregular == False);
   }

   /* Deal with irregular case -- sz==1 and no REX present */
   if (sz == 1 && irregular) {
      switch (reg) {
         case R_RSP: return 1+ OFFB_RAX;
         case R_RBP: return 1+ OFFB_RCX;
         case R_RSI: return 1+ OFFB_RDX;
         case R_RDI: return 1+ OFFB_RBX;
         default:    break; /* use the normal case */
      }
   }

   /* Normal case */
   return integerGuestReg64Offset(reg);
}


/* Read the %CL register :: Ity_I8, for shift/rotate operations. */

static IRExpr* getIRegCL ( void )
{
   vassert(!host_is_bigendian);
   return IRExpr_Get( OFFB_RCX, Ity_I8 );
}


/* Write to the %AH register. */

static void putIRegAH ( IRExpr* e )
{
   vassert(!host_is_bigendian);
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I8);
   stmt( IRStmt_Put( OFFB_RAX+1, e ) );
}


/* Read/write various widths of %RAX, as it has various
   special-purpose uses. */

static HChar* nameIRegRAX ( Int sz )
{
   switch (sz) {
      case 1: return "%al";
      case 2: return "%ax";
      case 4: return "%eax";
      case 8: return "%rax";
      default: vpanic("nameIRegRAX(amd64)");
   }
}

static IRExpr* getIRegRAX ( Int sz )
{
   vassert(!host_is_bigendian);
   switch (sz) {
      case 1: return IRExpr_Get( OFFB_RAX, Ity_I8 );
      case 2: return IRExpr_Get( OFFB_RAX, Ity_I16 );
      case 4: return unop(Iop_64to32, IRExpr_Get( OFFB_RAX, Ity_I64 ));
      case 8: return IRExpr_Get( OFFB_RAX, Ity_I64 );
      default: vpanic("getIRegRAX(amd64)");
   }
}

static void putIRegRAX ( Int sz, IRExpr* e )
{
   IRType ty = typeOfIRExpr(irsb->tyenv, e);
   vassert(!host_is_bigendian);
   switch (sz) {
      case 8: vassert(ty == Ity_I64);
              stmt( IRStmt_Put( OFFB_RAX, e ));
              break;
      case 4: vassert(ty == Ity_I32);
              stmt( IRStmt_Put( OFFB_RAX, unop(Iop_32Uto64,e) ));
              break;
      case 2: vassert(ty == Ity_I16);
              stmt( IRStmt_Put( OFFB_RAX, e ));
              break;
      case 1: vassert(ty == Ity_I8);
              stmt( IRStmt_Put( OFFB_RAX, e ));
              break;
      default: vpanic("putIRegRAX(amd64)");
   }
}


/* Read/write various widths of %RDX, as it has various
   special-purpose uses. */

static HChar* nameIRegRDX ( Int sz )
{
   switch (sz) {
      case 1: return "%dl";
      case 2: return "%dx";
      case 4: return "%edx";
      case 8: return "%rdx";
      default: vpanic("nameIRegRDX(amd64)");
   }
}

static IRExpr* getIRegRDX ( Int sz )
{
   vassert(!host_is_bigendian);
   switch (sz) {
      case 1: return IRExpr_Get( OFFB_RDX, Ity_I8 );
      case 2: return IRExpr_Get( OFFB_RDX, Ity_I16 );
      case 4: return unop(Iop_64to32, IRExpr_Get( OFFB_RDX, Ity_I64 ));
      case 8: return IRExpr_Get( OFFB_RDX, Ity_I64 );
      default: vpanic("getIRegRDX(amd64)");
   }
}

static void putIRegRDX ( Int sz, IRExpr* e )
{
   vassert(!host_is_bigendian);
   vassert(typeOfIRExpr(irsb->tyenv, e) == szToITy(sz));
   switch (sz) {
      case 8: stmt( IRStmt_Put( OFFB_RDX, e ));
              break;
      case 4: stmt( IRStmt_Put( OFFB_RDX, unop(Iop_32Uto64,e) ));
              break;
      case 2: stmt( IRStmt_Put( OFFB_RDX, e ));
              break;
      case 1: stmt( IRStmt_Put( OFFB_RDX, e ));
              break;
      default: vpanic("putIRegRDX(amd64)");
   }
}


/* Simplistic functions to deal with the integer registers as a
   straightforward bank of 16 64-bit regs. */

static IRExpr* getIReg64 ( UInt regno )
{
   return IRExpr_Get( integerGuestReg64Offset(regno),
                      Ity_I64 );
}

static void putIReg64 ( UInt regno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I64);
   stmt( IRStmt_Put( integerGuestReg64Offset(regno), e ) );
}

static HChar* nameIReg64 ( UInt regno )
{
   return nameIReg( 8, regno, False );
}


/* Simplistic functions to deal with the lower halves of integer
   registers as a straightforward bank of 16 32-bit regs. */

static IRExpr* getIReg32 ( UInt regno )
{
   vassert(!host_is_bigendian);
   return unop(Iop_64to32,
               IRExpr_Get( integerGuestReg64Offset(regno),
                           Ity_I64 ));
}

static void putIReg32 ( UInt regno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I32);
   stmt( IRStmt_Put( integerGuestReg64Offset(regno), 
                     unop(Iop_32Uto64,e) ) );
}

static HChar* nameIReg32 ( UInt regno )
{
   return nameIReg( 4, regno, False );
}


/* Simplistic functions to deal with the lower quarters of integer
   registers as a straightforward bank of 16 16-bit regs. */

static IRExpr* getIReg16 ( UInt regno )
{
   vassert(!host_is_bigendian);
   return IRExpr_Get( integerGuestReg64Offset(regno),
                      Ity_I16 );
}

static void putIReg16 ( UInt regno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I16);
   stmt( IRStmt_Put( integerGuestReg64Offset(regno), 
                     unop(Iop_16Uto64,e) ) );
}

static HChar* nameIReg16 ( UInt regno )
{
   return nameIReg( 2, regno, False );
}


/* Sometimes what we know is a 3-bit register number, a REX byte, and
   which field of the REX byte is to be used to extend to a 4-bit
   number.  These functions cater for that situation.  
*/
static IRExpr* getIReg64rexX ( Prefix pfx, UInt lo3bits )
{
   vassert(lo3bits < 8);
   vassert(IS_VALID_PFX(pfx));
   return getIReg64( lo3bits | (getRexX(pfx) << 3) );
}

static HChar* nameIReg64rexX ( Prefix pfx, UInt lo3bits )
{
   vassert(lo3bits < 8);
   vassert(IS_VALID_PFX(pfx));
   return nameIReg( 8, lo3bits | (getRexX(pfx) << 3), False );
}

static HChar* nameIRegRexB ( Int sz, Prefix pfx, UInt lo3bits )
{
   vassert(lo3bits < 8);
   vassert(IS_VALID_PFX(pfx));
   vassert(sz == 8 || sz == 4 || sz == 2 || sz == 1);
   return nameIReg( sz, lo3bits | (getRexB(pfx) << 3), 
                        toBool(sz==1 && !haveREX(pfx)) );
}

static IRExpr* getIRegRexB ( Int sz, Prefix pfx, UInt lo3bits )
{
   vassert(lo3bits < 8);
   vassert(IS_VALID_PFX(pfx));
   vassert(sz == 8 || sz == 4 || sz == 2 || sz == 1);
   if (sz == 4) {
      sz = 8;
      return unop(Iop_64to32,
                  IRExpr_Get(
                     offsetIReg( sz, lo3bits | (getRexB(pfx) << 3), 
                                     toBool(sz==1 && !haveREX(pfx)) ),
                     szToITy(sz)
                 )
             );
   } else {
      return IRExpr_Get(
                offsetIReg( sz, lo3bits | (getRexB(pfx) << 3), 
                                toBool(sz==1 && !haveREX(pfx)) ),
                szToITy(sz)
             );
   }
}

static void putIRegRexB ( Int sz, Prefix pfx, UInt lo3bits, IRExpr* e )
{
   vassert(lo3bits < 8);
   vassert(IS_VALID_PFX(pfx));
   vassert(sz == 8 || sz == 4 || sz == 2 || sz == 1);
   vassert(typeOfIRExpr(irsb->tyenv, e) == szToITy(sz));
   stmt( IRStmt_Put( 
            offsetIReg( sz, lo3bits | (getRexB(pfx) << 3), 
                            toBool(sz==1 && !haveREX(pfx)) ),
            sz==4 ? unop(Iop_32Uto64,e) : e
   ));
}


/* Functions for getting register numbers from modrm bytes and REX
   when we don't have to consider the complexities of integer subreg
   accesses.
*/
/* Extract the g reg field from a modRM byte, and augment it using the
   REX.R bit from the supplied REX byte.  The R bit usually is
   associated with the g register field.
*/
static UInt gregOfRexRM ( Prefix pfx, UChar mod_reg_rm )
{
   Int reg = (Int)( (mod_reg_rm >> 3) & 7 );
   reg += (pfx & PFX_REXR) ? 8 : 0;
   return reg;
}

/* Extract the e reg field from a modRM byte, and augment it using the
   REX.B bit from the supplied REX byte.  The B bit usually is
   associated with the e register field (when modrm indicates e is a
   register, that is).
*/
static UInt eregOfRexRM ( Prefix pfx, UChar mod_reg_rm )
{
   Int rm;
   vassert(epartIsReg(mod_reg_rm));
   rm = (Int)(mod_reg_rm & 0x7);
   rm += (pfx & PFX_REXB) ? 8 : 0;
   return rm;
}


/* General functions for dealing with integer register access. */

/* Produce the guest state offset for a reference to the 'g' register
   field in a modrm byte, taking into account REX (or its absence),
   and the size of the access.
*/
static UInt offsetIRegG ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   UInt reg;
   vassert(!host_is_bigendian);
   vassert(IS_VALID_PFX(pfx));
   vassert(sz == 8 || sz == 4 || sz == 2 || sz == 1);
   reg = gregOfRexRM( pfx, mod_reg_rm );
   return offsetIReg( sz, reg, toBool(sz == 1 && !haveREX(pfx)) );
}

static 
IRExpr* getIRegG ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   if (sz == 4) {
      sz = 8;
      return unop(Iop_64to32,
                  IRExpr_Get( offsetIRegG( sz, pfx, mod_reg_rm ),
                              szToITy(sz) ));
   } else {
      return IRExpr_Get( offsetIRegG( sz, pfx, mod_reg_rm ),
                         szToITy(sz) );
   }
}

static 
void putIRegG ( Int sz, Prefix pfx, UChar mod_reg_rm, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == szToITy(sz));
   if (sz == 4) {
      e = unop(Iop_32Uto64,e);
   }
   stmt( IRStmt_Put( offsetIRegG( sz, pfx, mod_reg_rm ), e ) );
}

static
HChar* nameIRegG ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   return nameIReg( sz, gregOfRexRM(pfx,mod_reg_rm),
                        toBool(sz==1 && !haveREX(pfx)) );
}


/* Produce the guest state offset for a reference to the 'e' register
   field in a modrm byte, taking into account REX (or its absence),
   and the size of the access.  eregOfRexRM will assert if mod_reg_rm
   denotes a memory access rather than a register access.
*/
static UInt offsetIRegE ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   UInt reg;
   vassert(!host_is_bigendian);
   vassert(IS_VALID_PFX(pfx));
   vassert(sz == 8 || sz == 4 || sz == 2 || sz == 1);
   reg = eregOfRexRM( pfx, mod_reg_rm );
   return offsetIReg( sz, reg, toBool(sz == 1 && !haveREX(pfx)) );
}

static 
IRExpr* getIRegE ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   if (sz == 4) {
      sz = 8;
      return unop(Iop_64to32,
                  IRExpr_Get( offsetIRegE( sz, pfx, mod_reg_rm ),
                              szToITy(sz) ));
   } else {
      return IRExpr_Get( offsetIRegE( sz, pfx, mod_reg_rm ),
                         szToITy(sz) );
   }
}

static 
void putIRegE ( Int sz, Prefix pfx, UChar mod_reg_rm, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == szToITy(sz));
   if (sz == 4) {
      e = unop(Iop_32Uto64,e);
   }
   stmt( IRStmt_Put( offsetIRegE( sz, pfx, mod_reg_rm ), e ) );
}

static
HChar* nameIRegE ( Int sz, Prefix pfx, UChar mod_reg_rm )
{
   return nameIReg( sz, eregOfRexRM(pfx,mod_reg_rm),
                        toBool(sz==1 && !haveREX(pfx)) );
}


/*------------------------------------------------------------*/
/*--- For dealing with XMM registers                       ---*/
/*------------------------------------------------------------*/

static Int ymmGuestRegOffset ( UInt ymmreg )
{
   switch (ymmreg) {
      case 0:  return OFFB_YMM0;
      case 1:  return OFFB_YMM1;
      case 2:  return OFFB_YMM2;
      case 3:  return OFFB_YMM3;
      case 4:  return OFFB_YMM4;
      case 5:  return OFFB_YMM5;
      case 6:  return OFFB_YMM6;
      case 7:  return OFFB_YMM7;
      case 8:  return OFFB_YMM8;
      case 9:  return OFFB_YMM9;
      case 10: return OFFB_YMM10;
      case 11: return OFFB_YMM11;
      case 12: return OFFB_YMM12;
      case 13: return OFFB_YMM13;
      case 14: return OFFB_YMM14;
      case 15: return OFFB_YMM15;
      default: vpanic("ymmGuestRegOffset(amd64)");
   }
}

static Int xmmGuestRegOffset ( UInt xmmreg )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   return ymmGuestRegOffset( xmmreg );
}

/* Lanes of vector registers are always numbered from zero being the
   least significant lane (rightmost in the register).  */

static Int xmmGuestRegLane16offset ( UInt xmmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 8);
   return xmmGuestRegOffset( xmmreg ) + 2 * laneno;
}

static Int xmmGuestRegLane32offset ( UInt xmmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 4);
   return xmmGuestRegOffset( xmmreg ) + 4 * laneno;
}

static Int xmmGuestRegLane64offset ( UInt xmmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 2);
   return xmmGuestRegOffset( xmmreg ) + 8 * laneno;
}

static Int ymmGuestRegLane128offset ( UInt ymmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 2);
   return ymmGuestRegOffset( ymmreg ) + 16 * laneno;
}

static Int ymmGuestRegLane64offset ( UInt ymmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 4);
   return ymmGuestRegOffset( ymmreg ) + 8 * laneno;
}

static Int ymmGuestRegLane32offset ( UInt ymmreg, Int laneno )
{
   /* Correct for little-endian host only. */
   vassert(!host_is_bigendian);
   vassert(laneno >= 0 && laneno < 8);
   return ymmGuestRegOffset( ymmreg ) + 4 * laneno;
}

static IRExpr* getXMMReg ( UInt xmmreg )
{
   return IRExpr_Get( xmmGuestRegOffset(xmmreg), Ity_V128 );
}

static IRExpr* getXMMRegLane64 ( UInt xmmreg, Int laneno )
{
   return IRExpr_Get( xmmGuestRegLane64offset(xmmreg,laneno), Ity_I64 );
}

static IRExpr* getXMMRegLane64F ( UInt xmmreg, Int laneno )
{
   return IRExpr_Get( xmmGuestRegLane64offset(xmmreg,laneno), Ity_F64 );
}

static IRExpr* getXMMRegLane32 ( UInt xmmreg, Int laneno )
{
   return IRExpr_Get( xmmGuestRegLane32offset(xmmreg,laneno), Ity_I32 );
}

static IRExpr* getXMMRegLane32F ( UInt xmmreg, Int laneno )
{
   return IRExpr_Get( xmmGuestRegLane32offset(xmmreg,laneno), Ity_F32 );
}

static IRExpr* getXMMRegLane16 ( UInt xmmreg, Int laneno )
{
  return IRExpr_Get( xmmGuestRegLane16offset(xmmreg,laneno), Ity_I16 );
}

static void putXMMReg ( UInt xmmreg, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_V128);
   stmt( IRStmt_Put( xmmGuestRegOffset(xmmreg), e ) );
}

static void putXMMRegLane64 ( UInt xmmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I64);
   stmt( IRStmt_Put( xmmGuestRegLane64offset(xmmreg,laneno), e ) );
}

static void putXMMRegLane64F ( UInt xmmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_F64);
   stmt( IRStmt_Put( xmmGuestRegLane64offset(xmmreg,laneno), e ) );
}

static void putXMMRegLane32F ( UInt xmmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_F32);
   stmt( IRStmt_Put( xmmGuestRegLane32offset(xmmreg,laneno), e ) );
}

static void putXMMRegLane32 ( UInt xmmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I32);
   stmt( IRStmt_Put( xmmGuestRegLane32offset(xmmreg,laneno), e ) );
}

static IRExpr* getYMMReg ( UInt xmmreg )
{
   return IRExpr_Get( ymmGuestRegOffset(xmmreg), Ity_V256 );
}

static IRExpr* getYMMRegLane128 ( UInt ymmreg, Int laneno )
{
   return IRExpr_Get( ymmGuestRegLane128offset(ymmreg,laneno), Ity_V128 );
}

static IRExpr* getYMMRegLane64 ( UInt ymmreg, Int laneno )
{
   return IRExpr_Get( ymmGuestRegLane64offset(ymmreg,laneno), Ity_I64 );
}

static IRExpr* getYMMRegLane32 ( UInt ymmreg, Int laneno )
{
   return IRExpr_Get( ymmGuestRegLane32offset(ymmreg,laneno), Ity_I32 );
}

static void putYMMReg ( UInt ymmreg, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_V256);
   stmt( IRStmt_Put( ymmGuestRegOffset(ymmreg), e ) );
}

static void putYMMRegLane128 ( UInt ymmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_V128);
   stmt( IRStmt_Put( ymmGuestRegLane128offset(ymmreg,laneno), e ) );
}

static void putYMMRegLane64F ( UInt ymmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_F64);
   stmt( IRStmt_Put( ymmGuestRegLane64offset(ymmreg,laneno), e ) );
}

static void putYMMRegLane64 ( UInt ymmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I64);
   stmt( IRStmt_Put( ymmGuestRegLane64offset(ymmreg,laneno), e ) );
}

static void putYMMRegLane32F ( UInt ymmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_F32);
   stmt( IRStmt_Put( ymmGuestRegLane32offset(ymmreg,laneno), e ) );
}

static void putYMMRegLane32 ( UInt ymmreg, Int laneno, IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I32);
   stmt( IRStmt_Put( ymmGuestRegLane32offset(ymmreg,laneno), e ) );
}

static IRExpr* mkV128 ( UShort mask )
{
   return IRExpr_Const(IRConst_V128(mask));
}

/* Write the low half of a YMM reg and zero out the upper half. */
static void putYMMRegLoAndZU ( UInt ymmreg, IRExpr* e )
{
   putYMMRegLane128( ymmreg, 0, e );
   putYMMRegLane128( ymmreg, 1, mkV128(0) );
}

static IRExpr* mkAnd1 ( IRExpr* x, IRExpr* y )
{
   vassert(typeOfIRExpr(irsb->tyenv,x) == Ity_I1);
   vassert(typeOfIRExpr(irsb->tyenv,y) == Ity_I1);
   return unop(Iop_64to1, 
               binop(Iop_And64, 
                     unop(Iop_1Uto64,x), 
                     unop(Iop_1Uto64,y)));
}

/* Generate a compare-and-swap operation, operating on memory at
   'addr'.  The expected value is 'expVal' and the new value is
   'newVal'.  If the operation fails, then transfer control (with a
   no-redir jump (XXX no -- see comment at top of this file)) to
   'restart_point', which is presumably the address of the guest
   instruction again -- retrying, essentially. */
static void casLE ( IRExpr* addr, IRExpr* expVal, IRExpr* newVal,
                    Addr64 restart_point )
{
   IRCAS* cas;
   IRType tyE    = typeOfIRExpr(irsb->tyenv, expVal);
   IRType tyN    = typeOfIRExpr(irsb->tyenv, newVal);
   IRTemp oldTmp = newTemp(tyE);
   IRTemp expTmp = newTemp(tyE);
   vassert(tyE == tyN);
   vassert(tyE == Ity_I64 || tyE == Ity_I32
           || tyE == Ity_I16 || tyE == Ity_I8);
   assign(expTmp, expVal);
   cas = mkIRCAS( IRTemp_INVALID, oldTmp, Iend_LE, addr, 
                  NULL, mkexpr(expTmp), NULL, newVal );
   stmt( IRStmt_CAS(cas) );
   stmt( IRStmt_Exit(
            binop( mkSizedOp(tyE,Iop_CasCmpNE8),
                   mkexpr(oldTmp), mkexpr(expTmp) ),
            Ijk_Boring, /*Ijk_NoRedir*/
            IRConst_U64( restart_point ),
            OFFB_RIP
         ));
}


/*------------------------------------------------------------*/
/*--- Helpers for %rflags.                                 ---*/
/*------------------------------------------------------------*/

/* -------------- Evaluating the flags-thunk. -------------- */

/* Build IR to calculate all the eflags from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression ::
   Ity_I64. */
static IRExpr* mk_amd64g_calculate_rflags_all ( void )
{
   IRExpr** args
      = mkIRExprVec_4( IRExpr_Get(OFFB_CC_OP,   Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
                       IRExpr_Get(OFFB_CC_NDEP, Ity_I64) );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/, 
           "amd64g_calculate_rflags_all", &amd64g_calculate_rflags_all,
           args
        );
   /* Exclude OP and NDEP from definedness checking.  We're only
      interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
   return call;
}

/* Build IR to calculate some particular condition from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression ::
   Ity_Bit. */
static IRExpr* mk_amd64g_calculate_condition ( AMD64Condcode cond )
{
   IRExpr** args
      = mkIRExprVec_5( mkU64(cond),
                       IRExpr_Get(OFFB_CC_OP,   Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
                       IRExpr_Get(OFFB_CC_NDEP, Ity_I64) );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/, 
           "amd64g_calculate_condition", &amd64g_calculate_condition,
           args
        );
   /* Exclude the requested condition, OP and NDEP from definedness
      checking.  We're only interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<1) | (1<<4);
   return unop(Iop_64to1, call);
}

/* Build IR to calculate just the carry flag from stored
   CC_OP/CC_DEP1/CC_DEP2/CC_NDEP.  Returns an expression :: Ity_I64. */
static IRExpr* mk_amd64g_calculate_rflags_c ( void )
{
   IRExpr** args
      = mkIRExprVec_4( IRExpr_Get(OFFB_CC_OP,   Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP1, Ity_I64),
                       IRExpr_Get(OFFB_CC_DEP2, Ity_I64),
                       IRExpr_Get(OFFB_CC_NDEP, Ity_I64) );
   IRExpr* call
      = mkIRExprCCall(
           Ity_I64,
           0/*regparm*/, 
           "amd64g_calculate_rflags_c", &amd64g_calculate_rflags_c,
           args
        );
   /* Exclude OP and NDEP from definedness checking.  We're only
      interested in DEP1 and DEP2. */
   call->Iex.CCall.cee->mcx_mask = (1<<0) | (1<<3);
   return call;
}


/* -------------- Building the flags-thunk. -------------- */

/* The machinery in this section builds the flag-thunk following a
   flag-setting operation.  Hence the various setFlags_* functions.
*/

static Bool isAddSub ( IROp op8 )
{
   return toBool(op8 == Iop_Add8 || op8 == Iop_Sub8);
}

static Bool isLogic ( IROp op8 )
{
   return toBool(op8 == Iop_And8 || op8 == Iop_Or8 || op8 == Iop_Xor8);
}

/* U-widen 8/16/32/64 bit int expr to 64. */
static IRExpr* widenUto64 ( IRExpr* e )
{
   switch (typeOfIRExpr(irsb->tyenv,e)) {
      case Ity_I64: return e;
      case Ity_I32: return unop(Iop_32Uto64, e);
      case Ity_I16: return unop(Iop_16Uto64, e);
      case Ity_I8:  return unop(Iop_8Uto64, e);
      default: vpanic("widenUto64");
   }
}

/* S-widen 8/16/32/64 bit int expr to 32. */
static IRExpr* widenSto64 ( IRExpr* e )
{
   switch (typeOfIRExpr(irsb->tyenv,e)) {
      case Ity_I64: return e;
      case Ity_I32: return unop(Iop_32Sto64, e);
      case Ity_I16: return unop(Iop_16Sto64, e);
      case Ity_I8:  return unop(Iop_8Sto64, e);
      default: vpanic("widenSto64");
   }
}

/* Narrow 8/16/32/64 bit int expr to 8/16/32/64.  Clearly only some
   of these combinations make sense. */
static IRExpr* narrowTo ( IRType dst_ty, IRExpr* e )
{
   IRType src_ty = typeOfIRExpr(irsb->tyenv,e);
   if (src_ty == dst_ty)
      return e;
   if (src_ty == Ity_I32 && dst_ty == Ity_I16)
      return unop(Iop_32to16, e);
   if (src_ty == Ity_I32 && dst_ty == Ity_I8)
      return unop(Iop_32to8, e);
   if (src_ty == Ity_I64 && dst_ty == Ity_I32)
      return unop(Iop_64to32, e);
   if (src_ty == Ity_I64 && dst_ty == Ity_I16)
      return unop(Iop_64to16, e);
   if (src_ty == Ity_I64 && dst_ty == Ity_I8)
      return unop(Iop_64to8, e);

   vex_printf("\nsrc, dst tys are: ");
   ppIRType(src_ty);
   vex_printf(", ");
   ppIRType(dst_ty);
   vex_printf("\n");
   vpanic("narrowTo(amd64)");
}


/* Set the flags thunk OP, DEP1 and DEP2 fields.  The supplied op is
   auto-sized up to the real op. */

static 
void setFlags_DEP1_DEP2 ( IROp op8, IRTemp dep1, IRTemp dep2, IRType ty )
{
   Int ccOp = 0;
   switch (ty) {
      case Ity_I8:  ccOp = 0; break;
      case Ity_I16: ccOp = 1; break;
      case Ity_I32: ccOp = 2; break;
      case Ity_I64: ccOp = 3; break;
      default: vassert(0);
   }
   switch (op8) {
      case Iop_Add8: ccOp += AMD64G_CC_OP_ADDB;   break;
      case Iop_Sub8: ccOp += AMD64G_CC_OP_SUBB;   break;
      default:       ppIROp(op8);
                     vpanic("setFlags_DEP1_DEP2(amd64)");
   }
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(ccOp)) );
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(dep1))) );
   stmt( IRStmt_Put( OFFB_CC_DEP2, widenUto64(mkexpr(dep2))) );
}


/* Set the OP and DEP1 fields only, and write zero to DEP2. */

static 
void setFlags_DEP1 ( IROp op8, IRTemp dep1, IRType ty )
{
   Int ccOp = 0;
   switch (ty) {
      case Ity_I8:  ccOp = 0; break;
      case Ity_I16: ccOp = 1; break;
      case Ity_I32: ccOp = 2; break;
      case Ity_I64: ccOp = 3; break;
      default: vassert(0);
   }
   switch (op8) {
      case Iop_Or8:
      case Iop_And8:
      case Iop_Xor8: ccOp += AMD64G_CC_OP_LOGICB; break;
      default:       ppIROp(op8);
                     vpanic("setFlags_DEP1(amd64)");
   }
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(ccOp)) );
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(dep1))) );
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0)) );
}


/* For shift operations, we put in the result and the undershifted
   result.  Except if the shift amount is zero, the thunk is left
   unchanged. */

static void setFlags_DEP1_DEP2_shift ( IROp    op64,
                                       IRTemp  res,
                                       IRTemp  resUS,
                                       IRType  ty,
                                       IRTemp  guard )
{
   Int ccOp = 0;
   switch (ty) {
      case Ity_I8:  ccOp = 0; break;
      case Ity_I16: ccOp = 1; break;
      case Ity_I32: ccOp = 2; break;
      case Ity_I64: ccOp = 3; break;
      default: vassert(0);
   }

   vassert(guard);

   /* Both kinds of right shifts are handled by the same thunk
      operation. */
   switch (op64) {
      case Iop_Shr64:
      case Iop_Sar64: ccOp += AMD64G_CC_OP_SHRB; break;
      case Iop_Shl64: ccOp += AMD64G_CC_OP_SHLB; break;
      default:        ppIROp(op64);
                      vpanic("setFlags_DEP1_DEP2_shift(amd64)");
   }

   /* DEP1 contains the result, DEP2 contains the undershifted value. */
   stmt( IRStmt_Put( OFFB_CC_OP,
                     IRExpr_Mux0X( mkexpr(guard),
                                   IRExpr_Get(OFFB_CC_OP,Ity_I64),
                                   mkU64(ccOp))) );
   stmt( IRStmt_Put( OFFB_CC_DEP1,
                     IRExpr_Mux0X( mkexpr(guard),
                                   IRExpr_Get(OFFB_CC_DEP1,Ity_I64),
                                   widenUto64(mkexpr(res)))) );
   stmt( IRStmt_Put( OFFB_CC_DEP2, 
                     IRExpr_Mux0X( mkexpr(guard),
                                   IRExpr_Get(OFFB_CC_DEP2,Ity_I64),
                                   widenUto64(mkexpr(resUS)))) );
}


/* For the inc/dec case, we store in DEP1 the result value and in NDEP
   the former value of the carry flag, which unfortunately we have to
   compute. */

static void setFlags_INC_DEC ( Bool inc, IRTemp res, IRType ty )
{
   Int ccOp = inc ? AMD64G_CC_OP_INCB : AMD64G_CC_OP_DECB;

   switch (ty) {
      case Ity_I8:  ccOp += 0; break;
      case Ity_I16: ccOp += 1; break;
      case Ity_I32: ccOp += 2; break;
      case Ity_I64: ccOp += 3; break;
      default: vassert(0);
   }
   
   /* This has to come first, because calculating the C flag 
      may require reading all four thunk fields. */
   stmt( IRStmt_Put( OFFB_CC_NDEP, mk_amd64g_calculate_rflags_c()) );
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(ccOp)) );
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(res))) );
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0)) );
}


/* Multiplies are pretty much like add and sub: DEP1 and DEP2 hold the
   two arguments. */

static
void setFlags_MUL ( IRType ty, IRTemp arg1, IRTemp arg2, ULong base_op )
{
   switch (ty) {
      case Ity_I8:
         stmt( IRStmt_Put( OFFB_CC_OP, mkU64(base_op+0) ) );
         break;
      case Ity_I16:
         stmt( IRStmt_Put( OFFB_CC_OP, mkU64(base_op+1) ) );
         break;
      case Ity_I32:
         stmt( IRStmt_Put( OFFB_CC_OP, mkU64(base_op+2) ) );
         break;
      case Ity_I64:
         stmt( IRStmt_Put( OFFB_CC_OP, mkU64(base_op+3) ) );
         break;
      default:
         vpanic("setFlags_MUL(amd64)");
   }
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(arg1)) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, widenUto64(mkexpr(arg2)) ));
}


/* -------------- Condition codes. -------------- */

/* Condition codes, using the AMD encoding.  */

static HChar* name_AMD64Condcode ( AMD64Condcode cond )
{
   switch (cond) {
      case AMD64CondO:      return "o";
      case AMD64CondNO:     return "no";
      case AMD64CondB:      return "b";
      case AMD64CondNB:     return "ae"; /*"nb";*/
      case AMD64CondZ:      return "e"; /*"z";*/
      case AMD64CondNZ:     return "ne"; /*"nz";*/
      case AMD64CondBE:     return "be";
      case AMD64CondNBE:    return "a"; /*"nbe";*/
      case AMD64CondS:      return "s";
      case AMD64CondNS:     return "ns";
      case AMD64CondP:      return "p";
      case AMD64CondNP:     return "np";
      case AMD64CondL:      return "l";
      case AMD64CondNL:     return "ge"; /*"nl";*/
      case AMD64CondLE:     return "le";
      case AMD64CondNLE:    return "g"; /*"nle";*/
      case AMD64CondAlways: return "ALWAYS";
      default: vpanic("name_AMD64Condcode");
   }
}

static 
AMD64Condcode positiveIse_AMD64Condcode ( AMD64Condcode  cond,
                                          /*OUT*/Bool*   needInvert )
{
   vassert(cond >= AMD64CondO && cond <= AMD64CondNLE);
   if (cond & 1) {
      *needInvert = True;
      return cond-1;
   } else {
      *needInvert = False;
      return cond;
   }
}


/* -------------- Helpers for ADD/SUB with carry. -------------- */

/* Given ta1, ta2 and tres, compute tres = ADC(ta1,ta2) and set flags
   appropriately.

   Optionally, generate a store for the 'tres' value.  This can either
   be a normal store, or it can be a cas-with-possible-failure style
   store:

   if taddr is IRTemp_INVALID, then no store is generated.

   if taddr is not IRTemp_INVALID, then a store (using taddr as
   the address) is generated:

     if texpVal is IRTemp_INVALID then a normal store is
     generated, and restart_point must be zero (it is irrelevant).

     if texpVal is not IRTemp_INVALID then a cas-style store is
     generated.  texpVal is the expected value, restart_point
     is the restart point if the store fails, and texpVal must
     have the same type as tres.   

*/
static void helper_ADC ( Int sz,
                         IRTemp tres, IRTemp ta1, IRTemp ta2,
                         /* info about optional store: */
                         IRTemp taddr, IRTemp texpVal, Addr32 restart_point )
{
   UInt    thunkOp;
   IRType  ty    = szToITy(sz);
   IRTemp  oldc  = newTemp(Ity_I64);
   IRTemp  oldcn = newTemp(ty);
   IROp    plus  = mkSizedOp(ty, Iop_Add8);
   IROp    xor   = mkSizedOp(ty, Iop_Xor8);

   vassert(typeOfIRTemp(irsb->tyenv, tres) == ty);

   switch (sz) {
      case 8:  thunkOp = AMD64G_CC_OP_ADCQ; break;
      case 4:  thunkOp = AMD64G_CC_OP_ADCL; break;
      case 2:  thunkOp = AMD64G_CC_OP_ADCW; break;
      case 1:  thunkOp = AMD64G_CC_OP_ADCB; break;
      default: vassert(0);
   }

   /* oldc = old carry flag, 0 or 1 */
   assign( oldc,  binop(Iop_And64,
                        mk_amd64g_calculate_rflags_c(),
                        mkU64(1)) );

   assign( oldcn, narrowTo(ty, mkexpr(oldc)) );

   assign( tres, binop(plus,
                       binop(plus,mkexpr(ta1),mkexpr(ta2)),
                       mkexpr(oldcn)) );

   /* Possibly generate a store of 'tres' to 'taddr'.  See comment at
      start of this function. */
   if (taddr != IRTemp_INVALID) {
      if (texpVal == IRTemp_INVALID) {
         vassert(restart_point == 0);
         storeLE( mkexpr(taddr), mkexpr(tres) );
      } else {
         vassert(typeOfIRTemp(irsb->tyenv, texpVal) == ty);
         /* .. and hence 'texpVal' has the same type as 'tres'. */
         casLE( mkexpr(taddr),
                mkexpr(texpVal), mkexpr(tres), restart_point );
      }
   }

   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(thunkOp) ) );
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(ta1))  ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, widenUto64(binop(xor, mkexpr(ta2), 
                                                         mkexpr(oldcn)) )) );
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkexpr(oldc) ) );
}


/* Given ta1, ta2 and tres, compute tres = SBB(ta1,ta2) and set flags
   appropriately.  As with helper_ADC, possibly generate a store of
   the result -- see comments on helper_ADC for details.
*/
static void helper_SBB ( Int sz,
                         IRTemp tres, IRTemp ta1, IRTemp ta2,
                         /* info about optional store: */
                         IRTemp taddr, IRTemp texpVal, Addr32 restart_point )
{
   UInt    thunkOp;
   IRType  ty    = szToITy(sz);
   IRTemp  oldc  = newTemp(Ity_I64);
   IRTemp  oldcn = newTemp(ty);
   IROp    minus = mkSizedOp(ty, Iop_Sub8);
   IROp    xor   = mkSizedOp(ty, Iop_Xor8);

   vassert(typeOfIRTemp(irsb->tyenv, tres) == ty);

   switch (sz) {
      case 8:  thunkOp = AMD64G_CC_OP_SBBQ; break;
      case 4:  thunkOp = AMD64G_CC_OP_SBBL; break;
      case 2:  thunkOp = AMD64G_CC_OP_SBBW; break;
      case 1:  thunkOp = AMD64G_CC_OP_SBBB; break;
      default: vassert(0);
   }

   /* oldc = old carry flag, 0 or 1 */
   assign( oldc, binop(Iop_And64,
                       mk_amd64g_calculate_rflags_c(),
                       mkU64(1)) );

   assign( oldcn, narrowTo(ty, mkexpr(oldc)) );

   assign( tres, binop(minus,
                       binop(minus,mkexpr(ta1),mkexpr(ta2)),
                       mkexpr(oldcn)) );

   /* Possibly generate a store of 'tres' to 'taddr'.  See comment at
      start of this function. */
   if (taddr != IRTemp_INVALID) {
      if (texpVal == IRTemp_INVALID) {
         vassert(restart_point == 0);
         storeLE( mkexpr(taddr), mkexpr(tres) );
      } else {
         vassert(typeOfIRTemp(irsb->tyenv, texpVal) == ty);
         /* .. and hence 'texpVal' has the same type as 'tres'. */
         casLE( mkexpr(taddr),
                mkexpr(texpVal), mkexpr(tres), restart_point );
      }
   }

   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(thunkOp) ) );
   stmt( IRStmt_Put( OFFB_CC_DEP1, widenUto64(mkexpr(ta1) )) );
   stmt( IRStmt_Put( OFFB_CC_DEP2, widenUto64(binop(xor, mkexpr(ta2), 
                                                         mkexpr(oldcn)) )) );
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkexpr(oldc) ) );
}


/* -------------- Helpers for disassembly printing. -------------- */

static HChar* nameGrp1 ( Int opc_aux )
{
   static HChar* grp1_names[8] 
     = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
   if (opc_aux < 0 || opc_aux > 7) vpanic("nameGrp1(amd64)");
   return grp1_names[opc_aux];
}

static HChar* nameGrp2 ( Int opc_aux )
{
   static HChar* grp2_names[8] 
     = { "rol", "ror", "rcl", "rcr", "shl", "shr", "shl", "sar" };
   if (opc_aux < 0 || opc_aux > 7) vpanic("nameGrp2(amd64)");
   return grp2_names[opc_aux];
}

static HChar* nameGrp4 ( Int opc_aux )
{
   static HChar* grp4_names[8] 
     = { "inc", "dec", "???", "???", "???", "???", "???", "???" };
   if (opc_aux < 0 || opc_aux > 1) vpanic("nameGrp4(amd64)");
   return grp4_names[opc_aux];
}

static HChar* nameGrp5 ( Int opc_aux )
{
   static HChar* grp5_names[8] 
     = { "inc", "dec", "call*", "call*", "jmp*", "jmp*", "push", "???" };
   if (opc_aux < 0 || opc_aux > 6) vpanic("nameGrp5(amd64)");
   return grp5_names[opc_aux];
}

static HChar* nameGrp8 ( Int opc_aux )
{
   static HChar* grp8_names[8] 
      = { "???", "???", "???", "???", "bt", "bts", "btr", "btc" };
   if (opc_aux < 4 || opc_aux > 7) vpanic("nameGrp8(amd64)");
   return grp8_names[opc_aux];
}

//.. static HChar* nameSReg ( UInt sreg )
//.. {
//..    switch (sreg) {
//..       case R_ES: return "%es";
//..       case R_CS: return "%cs";
//..       case R_SS: return "%ss";
//..       case R_DS: return "%ds";
//..       case R_FS: return "%fs";
//..       case R_GS: return "%gs";
//..       default: vpanic("nameSReg(x86)");
//..    }
//.. }

static HChar* nameMMXReg ( Int mmxreg )
{
   static HChar* mmx_names[8] 
     = { "%mm0", "%mm1", "%mm2", "%mm3", "%mm4", "%mm5", "%mm6", "%mm7" };
   if (mmxreg < 0 || mmxreg > 7) vpanic("nameMMXReg(amd64,guest)");
   return mmx_names[mmxreg];
}

static HChar* nameXMMReg ( Int xmmreg )
{
   static HChar* xmm_names[16] 
     = { "%xmm0",  "%xmm1",  "%xmm2",  "%xmm3", 
         "%xmm4",  "%xmm5",  "%xmm6",  "%xmm7", 
         "%xmm8",  "%xmm9",  "%xmm10", "%xmm11", 
         "%xmm12", "%xmm13", "%xmm14", "%xmm15" };
   if (xmmreg < 0 || xmmreg > 15) vpanic("nameXMMReg(amd64)");
   return xmm_names[xmmreg];
}
 
static HChar* nameMMXGran ( Int gran )
{
   switch (gran) {
      case 0: return "b";
      case 1: return "w";
      case 2: return "d";
      case 3: return "q";
      default: vpanic("nameMMXGran(amd64,guest)");
   }
}

static HChar nameISize ( Int size )
{
   switch (size) {
      case 8: return 'q';
      case 4: return 'l';
      case 2: return 'w';
      case 1: return 'b';
      default: vpanic("nameISize(amd64)");
   }
}

static HChar* nameYMMReg ( Int ymmreg )
{
   static HChar* ymm_names[16] 
     = { "%ymm0",  "%ymm1",  "%ymm2",  "%ymm3", 
         "%ymm4",  "%ymm5",  "%ymm6",  "%ymm7", 
         "%ymm8",  "%ymm9",  "%ymm10", "%ymm11", 
         "%ymm12", "%ymm13", "%ymm14", "%ymm15" };
   if (ymmreg < 0 || ymmreg > 15) vpanic("nameYMMReg(amd64)");
   return ymm_names[ymmreg];
}


/*------------------------------------------------------------*/
/*--- JMP helpers                                          ---*/
/*------------------------------------------------------------*/

static void jmp_lit( /*MOD*/DisResult* dres,
                     IRJumpKind kind, Addr64 d64 )
{
   vassert(dres->whatNext    == Dis_Continue);
   vassert(dres->len         == 0);
   vassert(dres->continueAt  == 0);
   vassert(dres->jk_StopHere == Ijk_INVALID);
   dres->whatNext    = Dis_StopHere;
   dres->jk_StopHere = kind;
   stmt( IRStmt_Put( OFFB_RIP, mkU64(d64) ) );
}

static void jmp_treg( /*MOD*/DisResult* dres,
                      IRJumpKind kind, IRTemp t )
{
   vassert(dres->whatNext    == Dis_Continue);
   vassert(dres->len         == 0);
   vassert(dres->continueAt  == 0);
   vassert(dres->jk_StopHere == Ijk_INVALID);
   dres->whatNext    = Dis_StopHere;
   dres->jk_StopHere = kind;
   stmt( IRStmt_Put( OFFB_RIP, mkexpr(t) ) );
}

static 
void jcc_01 ( /*MOD*/DisResult* dres,
              AMD64Condcode cond, Addr64 d64_false, Addr64 d64_true )
{
   Bool          invert;
   AMD64Condcode condPos;
   vassert(dres->whatNext    == Dis_Continue);
   vassert(dres->len         == 0);
   vassert(dres->continueAt  == 0);
   vassert(dres->jk_StopHere == Ijk_INVALID);
   dres->whatNext    = Dis_StopHere;
   dres->jk_StopHere = Ijk_Boring;
   condPos = positiveIse_AMD64Condcode ( cond, &invert );
   if (invert) {
      stmt( IRStmt_Exit( mk_amd64g_calculate_condition(condPos),
                         Ijk_Boring,
                         IRConst_U64(d64_false),
                         OFFB_RIP ) );
      stmt( IRStmt_Put( OFFB_RIP, mkU64(d64_true) ) );
   } else {
      stmt( IRStmt_Exit( mk_amd64g_calculate_condition(condPos),
                         Ijk_Boring,
                         IRConst_U64(d64_true),
                         OFFB_RIP ) );
      stmt( IRStmt_Put( OFFB_RIP, mkU64(d64_false) ) );
   }
}

/* Let new_rsp be the %rsp value after a call/return.  Let nia be the
   guest address of the next instruction to be executed.

   This function generates an AbiHint to say that -128(%rsp)
   .. -1(%rsp) should now be regarded as uninitialised.
*/
static 
void make_redzone_AbiHint ( VexAbiInfo* vbi,
                            IRTemp new_rsp, IRTemp nia, HChar* who )
{
   Int szB = vbi->guest_stack_redzone_size;
   vassert(szB >= 0);

   /* A bit of a kludge.  Currently the only AbI we've guested AMD64
      for is ELF.  So just check it's the expected 128 value
      (paranoia). */
   vassert(szB == 128);

   if (0) vex_printf("AbiHint: %s\n", who);
   vassert(typeOfIRTemp(irsb->tyenv, new_rsp) == Ity_I64);
   vassert(typeOfIRTemp(irsb->tyenv, nia) == Ity_I64);
   if (szB > 0)
      stmt( IRStmt_AbiHint( 
               binop(Iop_Sub64, mkexpr(new_rsp), mkU64(szB)), 
               szB,
               mkexpr(nia)
            ));
}


/*------------------------------------------------------------*/
/*--- Disassembling addressing modes                       ---*/
/*------------------------------------------------------------*/

static 
HChar* segRegTxt ( Prefix pfx )
{
   if (pfx & PFX_CS) return "%cs:";
   if (pfx & PFX_DS) return "%ds:";
   if (pfx & PFX_ES) return "%es:";
   if (pfx & PFX_FS) return "%fs:";
   if (pfx & PFX_GS) return "%gs:";
   if (pfx & PFX_SS) return "%ss:";
   return ""; /* no override */
}


/* 'virtual' is an IRExpr* holding a virtual address.  Convert it to a
   linear address by adding any required segment override as indicated
   by sorb, and also dealing with any address size override
   present. */
static
IRExpr* handleAddrOverrides ( VexAbiInfo* vbi, 
                              Prefix pfx, IRExpr* virtual )
{
   /* --- segment overrides --- */
   if (pfx & PFX_FS) {
      if (vbi->guest_amd64_assume_fs_is_zero) {
         /* Note that this is a linux-kernel specific hack that relies
            on the assumption that %fs is always zero. */
         /* return virtual + guest_FS_ZERO. */
         virtual = binop(Iop_Add64, virtual,
                                    IRExpr_Get(OFFB_FS_ZERO, Ity_I64));
      } else {
         unimplemented("amd64 %fs segment override");
      }
   }

   if (pfx & PFX_GS) {
      if (vbi->guest_amd64_assume_gs_is_0x60) {
         /* Note that this is a darwin-kernel specific hack that relies
            on the assumption that %gs is always 0x60. */
         /* return virtual + guest_GS_0x60. */
         virtual = binop(Iop_Add64, virtual,
                                    IRExpr_Get(OFFB_GS_0x60, Ity_I64));
      } else {
         unimplemented("amd64 %gs segment override");
      }
   }

   /* cs, ds, es and ss are simply ignored in 64-bit mode. */

   /* --- address size override --- */
   if (haveASO(pfx))
      virtual = unop(Iop_32Uto64, unop(Iop_64to32, virtual));

   return virtual;
}

//.. {
//..    Int    sreg;
//..    IRType hWordTy;
//..    IRTemp ldt_ptr, gdt_ptr, seg_selector, r64;
//.. 
//..    if (sorb == 0)
//..       /* the common case - no override */
//..       return virtual;
//.. 
//..    switch (sorb) {
//..       case 0x3E: sreg = R_DS; break;
//..       case 0x26: sreg = R_ES; break;
//..       case 0x64: sreg = R_FS; break;
//..       case 0x65: sreg = R_GS; break;
//..       default: vpanic("handleAddrOverrides(x86,guest)");
//..    }
//.. 
//..    hWordTy = sizeof(HWord)==4 ? Ity_I32 : Ity_I64;
//.. 
//..    seg_selector = newTemp(Ity_I32);
//..    ldt_ptr      = newTemp(hWordTy);
//..    gdt_ptr      = newTemp(hWordTy);
//..    r64          = newTemp(Ity_I64);
//.. 
//..    assign( seg_selector, unop(Iop_16Uto32, getSReg(sreg)) );
//..    assign( ldt_ptr, IRExpr_Get( OFFB_LDT, hWordTy ));
//..    assign( gdt_ptr, IRExpr_Get( OFFB_GDT, hWordTy ));
//.. 
//..    /*
//..    Call this to do the translation and limit checks: 
//..    ULong x86g_use_seg_selector ( HWord ldt, HWord gdt,
//..                                  UInt seg_selector, UInt virtual_addr )
//..    */
//..    assign( 
//..       r64, 
//..       mkIRExprCCall( 
//..          Ity_I64, 
//..          0/*regparms*/, 
//..          "x86g_use_seg_selector", 
//..          &x86g_use_seg_selector, 
//..          mkIRExprVec_4( mkexpr(ldt_ptr), mkexpr(gdt_ptr), 
//..                         mkexpr(seg_selector), virtual)
//..       )
//..    );
//.. 
//..    /* If the high 32 of the result are non-zero, there was a 
//..       failure in address translation.  In which case, make a
//..       quick exit.
//..    */
//..    stmt( 
//..       IRStmt_Exit(
//..          binop(Iop_CmpNE32, unop(Iop_64HIto32, mkexpr(r64)), mkU32(0)),
//..          Ijk_MapFail,
//..          IRConst_U32( guest_eip_curr_instr )
//..       )
//..    );
//.. 
//..    /* otherwise, here's the translated result. */
//..    return unop(Iop_64to32, mkexpr(r64));
//.. }


/* Generate IR to calculate an address indicated by a ModRM and
   following SIB bytes.  The expression, and the number of bytes in
   the address mode, are returned (the latter in *len).  Note that
   this fn should not be called if the R/M part of the address denotes
   a register instead of memory.  If print_codegen is true, text of
   the addressing mode is placed in buf.

   The computed address is stored in a new tempreg, and the
   identity of the tempreg is returned.

   extra_bytes holds the number of bytes after the amode, as supplied
   by the caller.  This is needed to make sense of %rip-relative
   addresses.  Note that the value that *len is set to is only the
   length of the amode itself and does not include the value supplied
   in extra_bytes.
 */

static IRTemp disAMode_copy2tmp ( IRExpr* addr64 )
{
   IRTemp tmp = newTemp(Ity_I64);
   assign( tmp, addr64 );
   return tmp;
}

static 
IRTemp disAMode ( /*OUT*/Int* len,
                  VexAbiInfo* vbi, Prefix pfx, Long delta, 
                  /*OUT*/HChar* buf, Int extra_bytes )
{
   UChar mod_reg_rm = getUChar(delta);
   delta++;

   buf[0] = (UChar)0;
   vassert(extra_bytes >= 0 && extra_bytes < 10);

   /* squeeze out the reg field from mod_reg_rm, since a 256-entry
      jump table seems a bit excessive. 
   */
   mod_reg_rm &= 0xC7;                         /* is now XX000YYY */
   mod_reg_rm  = toUChar(mod_reg_rm | (mod_reg_rm >> 3));
                                               /* is now XX0XXYYY */
   mod_reg_rm &= 0x1F;                         /* is now 000XXYYY */
   switch (mod_reg_rm) {

      /* REX.B==0: (%rax) .. (%rdi), not including (%rsp) or (%rbp).
         REX.B==1: (%r8)  .. (%r15), not including (%r12) or (%r13).
      */
      case 0x00: case 0x01: case 0x02: case 0x03: 
      /* ! 04 */ /* ! 05 */ case 0x06: case 0x07:
         { UChar rm = toUChar(mod_reg_rm & 7);
           DIS(buf, "%s(%s)", segRegTxt(pfx), nameIRegRexB(8,pfx,rm));
           *len = 1;
           return disAMode_copy2tmp(
                  handleAddrOverrides(vbi, pfx, getIRegRexB(8,pfx,rm)));
         }

      /* REX.B==0: d8(%rax) ... d8(%rdi), not including d8(%rsp) 
         REX.B==1: d8(%r8)  ... d8(%r15), not including d8(%r12) 
      */
      case 0x08: case 0x09: case 0x0A: case 0x0B: 
      /* ! 0C */ case 0x0D: case 0x0E: case 0x0F:
         { UChar rm = toUChar(mod_reg_rm & 7);
           Long d   = getSDisp8(delta);
           if (d == 0) {
              DIS(buf, "%s(%s)", segRegTxt(pfx), nameIRegRexB(8,pfx,rm));
           } else {
              DIS(buf, "%s%lld(%s)", segRegTxt(pfx), d, nameIRegRexB(8,pfx,rm));
           }
           *len = 2;
           return disAMode_copy2tmp(
                  handleAddrOverrides(vbi, pfx,
                     binop(Iop_Add64,getIRegRexB(8,pfx,rm),mkU64(d))));
         }

      /* REX.B==0: d32(%rax) ... d32(%rdi), not including d32(%rsp)
         REX.B==1: d32(%r8)  ... d32(%r15), not including d32(%r12)
      */
      case 0x10: case 0x11: case 0x12: case 0x13: 
      /* ! 14 */ case 0x15: case 0x16: case 0x17:
         { UChar rm = toUChar(mod_reg_rm & 7);
           Long  d  = getSDisp32(delta);
           DIS(buf, "%s%lld(%s)", segRegTxt(pfx), d, nameIRegRexB(8,pfx,rm));
           *len = 5;
           return disAMode_copy2tmp(
                  handleAddrOverrides(vbi, pfx,
                     binop(Iop_Add64,getIRegRexB(8,pfx,rm),mkU64(d))));
         }

      /* REX.B==0: a register, %rax .. %rdi.  This shouldn't happen. */
      /* REX.B==1: a register, %r8  .. %r16.  This shouldn't happen. */
      case 0x18: case 0x19: case 0x1A: case 0x1B:
      case 0x1C: case 0x1D: case 0x1E: case 0x1F:
         vpanic("disAMode(amd64): not an addr!");

      /* RIP + disp32.  This assumes that guest_RIP_curr_instr is set
         correctly at the start of handling each instruction. */
      case 0x05: 
         { Long d = getSDisp32(delta);
           *len = 5;
           DIS(buf, "%s%lld(%%rip)", segRegTxt(pfx), d);
           /* We need to know the next instruction's start address.
              Try and figure out what it is, record the guess, and ask
              the top-level driver logic (bbToIR_AMD64) to check we
              guessed right, after the instruction is completely
              decoded. */
           guest_RIP_next_mustcheck = True;
           guest_RIP_next_assumed = guest_RIP_bbstart 
                                    + delta+4 + extra_bytes;
           return disAMode_copy2tmp( 
                     handleAddrOverrides(vbi, pfx, 
                        binop(Iop_Add64, mkU64(guest_RIP_next_assumed), 
                                         mkU64(d))));
         }

      case 0x04: {
         /* SIB, with no displacement.  Special cases:
            -- %rsp cannot act as an index value.  
               If index_r indicates %rsp, zero is used for the index.
            -- when mod is zero and base indicates RBP or R13, base is 
               instead a 32-bit sign-extended literal.
            It's all madness, I tell you.  Extract %index, %base and 
            scale from the SIB byte.  The value denoted is then:
               | %index == %RSP && (%base == %RBP || %base == %R13)
               = d32 following SIB byte
               | %index == %RSP && !(%base == %RBP || %base == %R13)
               = %base
               | %index != %RSP && (%base == %RBP || %base == %R13)
               = d32 following SIB byte + (%index << scale)
               | %index != %RSP && !(%base == %RBP || %base == %R13)
               = %base + (%index << scale)
         */
         UChar sib     = getUChar(delta);
         UChar scale   = toUChar((sib >> 6) & 3);
         UChar index_r = toUChar((sib >> 3) & 7);
         UChar base_r  = toUChar(sib & 7);
         /* correct since #(R13) == 8 + #(RBP) */
         Bool  base_is_BPor13 = toBool(base_r == R_RBP);
         Bool  index_is_SP    = toBool(index_r == R_RSP && 0==getRexX(pfx));
         delta++;

         if ((!index_is_SP) && (!base_is_BPor13)) {
            if (scale == 0) {
               DIS(buf, "%s(%s,%s)", segRegTxt(pfx), 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r));
            } else {
               DIS(buf, "%s(%s,%s,%d)", segRegTxt(pfx), 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r), 1<<scale);
            }
            *len = 2;
            return
               disAMode_copy2tmp( 
               handleAddrOverrides(vbi, pfx,
                  binop(Iop_Add64, 
                        getIRegRexB(8,pfx,base_r),
                        binop(Iop_Shl64, getIReg64rexX(pfx,index_r),
                              mkU8(scale)))));
         }

         if ((!index_is_SP) && base_is_BPor13) {
            Long d = getSDisp32(delta);
            DIS(buf, "%s%lld(,%s,%d)", segRegTxt(pfx), d, 
                      nameIReg64rexX(pfx,index_r), 1<<scale);
            *len = 6;
            return
               disAMode_copy2tmp(
               handleAddrOverrides(vbi, pfx, 
                  binop(Iop_Add64,
                        binop(Iop_Shl64, getIReg64rexX(pfx,index_r), 
                                         mkU8(scale)),
                        mkU64(d))));
         }

         if (index_is_SP && (!base_is_BPor13)) {
            DIS(buf, "%s(%s)", segRegTxt(pfx), nameIRegRexB(8,pfx,base_r));
            *len = 2;
            return disAMode_copy2tmp(
                   handleAddrOverrides(vbi, pfx, getIRegRexB(8,pfx,base_r)));
         }

         if (index_is_SP && base_is_BPor13) {
            Long d = getSDisp32(delta);
            DIS(buf, "%s%lld", segRegTxt(pfx), d);
            *len = 6;
            return disAMode_copy2tmp(
                   handleAddrOverrides(vbi, pfx, mkU64(d)));
         }

         vassert(0);
      }

      /* SIB, with 8-bit displacement.  Special cases:
         -- %esp cannot act as an index value.  
            If index_r indicates %esp, zero is used for the index.
         Denoted value is:
            | %index == %ESP
            = d8 + %base
            | %index != %ESP
            = d8 + %base + (%index << scale)
      */
      case 0x0C: {
         UChar sib     = getUChar(delta);
         UChar scale   = toUChar((sib >> 6) & 3);
         UChar index_r = toUChar((sib >> 3) & 7);
         UChar base_r  = toUChar(sib & 7);
         Long d        = getSDisp8(delta+1);

         if (index_r == R_RSP && 0==getRexX(pfx)) {
            DIS(buf, "%s%lld(%s)", segRegTxt(pfx), 
                                   d, nameIRegRexB(8,pfx,base_r));
            *len = 3;
            return disAMode_copy2tmp(
                   handleAddrOverrides(vbi, pfx, 
                      binop(Iop_Add64, getIRegRexB(8,pfx,base_r), mkU64(d)) ));
         } else {
            if (scale == 0) {
               DIS(buf, "%s%lld(%s,%s)", segRegTxt(pfx), d, 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r));
            } else {
               DIS(buf, "%s%lld(%s,%s,%d)", segRegTxt(pfx), d, 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r), 1<<scale);
            }
            *len = 3;
            return 
                disAMode_copy2tmp(
                handleAddrOverrides(vbi, pfx,
                  binop(Iop_Add64,
                        binop(Iop_Add64, 
                              getIRegRexB(8,pfx,base_r), 
                              binop(Iop_Shl64, 
                                    getIReg64rexX(pfx,index_r), mkU8(scale))),
                        mkU64(d))));
         }
         vassert(0); /*NOTREACHED*/
      }

      /* SIB, with 32-bit displacement.  Special cases:
         -- %rsp cannot act as an index value.  
            If index_r indicates %rsp, zero is used for the index.
         Denoted value is:
            | %index == %RSP
            = d32 + %base
            | %index != %RSP
            = d32 + %base + (%index << scale)
      */
      case 0x14: {
         UChar sib     = getUChar(delta);
         UChar scale   = toUChar((sib >> 6) & 3);
         UChar index_r = toUChar((sib >> 3) & 7);
         UChar base_r  = toUChar(sib & 7);
         Long d        = getSDisp32(delta+1);

         if (index_r == R_RSP && 0==getRexX(pfx)) {
            DIS(buf, "%s%lld(%s)", segRegTxt(pfx), 
                                   d, nameIRegRexB(8,pfx,base_r));
            *len = 6;
            return disAMode_copy2tmp(
                   handleAddrOverrides(vbi, pfx, 
                      binop(Iop_Add64, getIRegRexB(8,pfx,base_r), mkU64(d)) ));
         } else {
            if (scale == 0) {
               DIS(buf, "%s%lld(%s,%s)", segRegTxt(pfx), d, 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r));
            } else {
               DIS(buf, "%s%lld(%s,%s,%d)", segRegTxt(pfx), d, 
                         nameIRegRexB(8,pfx,base_r), 
                         nameIReg64rexX(pfx,index_r), 1<<scale);
            }
            *len = 6;
            return 
                disAMode_copy2tmp(
                handleAddrOverrides(vbi, pfx,
                  binop(Iop_Add64,
                        binop(Iop_Add64, 
                              getIRegRexB(8,pfx,base_r), 
                              binop(Iop_Shl64, 
                                    getIReg64rexX(pfx,index_r), mkU8(scale))),
                        mkU64(d))));
         }
         vassert(0); /*NOTREACHED*/
      }

      default:
         vpanic("disAMode(amd64)");
         return 0; /*notreached*/
   }
}


/* Figure out the number of (insn-stream) bytes constituting the amode
   beginning at delta.  Is useful for getting hold of literals beyond
   the end of the amode before it has been disassembled.  */

static UInt lengthAMode ( Prefix pfx, Long delta )
{
   UChar mod_reg_rm = getUChar(delta);
   delta++;

   /* squeeze out the reg field from mod_reg_rm, since a 256-entry
      jump table seems a bit excessive. 
   */
   mod_reg_rm &= 0xC7;                         /* is now XX000YYY */
   mod_reg_rm  = toUChar(mod_reg_rm | (mod_reg_rm >> 3));
                                               /* is now XX0XXYYY */
   mod_reg_rm &= 0x1F;                         /* is now 000XXYYY */
   switch (mod_reg_rm) {

      /* REX.B==0: (%rax) .. (%rdi), not including (%rsp) or (%rbp).
         REX.B==1: (%r8)  .. (%r15), not including (%r12) or (%r13).
      */
      case 0x00: case 0x01: case 0x02: case 0x03: 
      /* ! 04 */ /* ! 05 */ case 0x06: case 0x07:
         return 1;

      /* REX.B==0: d8(%rax) ... d8(%rdi), not including d8(%rsp) 
         REX.B==1: d8(%r8)  ... d8(%r15), not including d8(%r12) 
      */
      case 0x08: case 0x09: case 0x0A: case 0x0B: 
      /* ! 0C */ case 0x0D: case 0x0E: case 0x0F:
         return 2;

      /* REX.B==0: d32(%rax) ... d32(%rdi), not including d32(%rsp)
         REX.B==1: d32(%r8)  ... d32(%r15), not including d32(%r12)
      */
      case 0x10: case 0x11: case 0x12: case 0x13: 
      /* ! 14 */ case 0x15: case 0x16: case 0x17:
         return 5;

      /* REX.B==0: a register, %rax .. %rdi.  This shouldn't happen. */
      /* REX.B==1: a register, %r8  .. %r16.  This shouldn't happen. */
      /* Not an address, but still handled. */
      case 0x18: case 0x19: case 0x1A: case 0x1B:
      case 0x1C: case 0x1D: case 0x1E: case 0x1F:
         return 1;

      /* RIP + disp32. */
      case 0x05: 
         return 5;

      case 0x04: {
         /* SIB, with no displacement. */
         UChar sib     = getUChar(delta);
         UChar base_r  = toUChar(sib & 7);
         /* correct since #(R13) == 8 + #(RBP) */
         Bool  base_is_BPor13 = toBool(base_r == R_RBP);

         if (base_is_BPor13) {
            return 6;
         } else {
            return 2;
         }
      }

      /* SIB, with 8-bit displacement. */
      case 0x0C:
         return 3;

      /* SIB, with 32-bit displacement. */
      case 0x14:
         return 6;

      default:
         vpanic("lengthAMode(amd64)");
         return 0; /*notreached*/
   }
}


/*------------------------------------------------------------*/
/*--- Disassembling common idioms                          ---*/
/*------------------------------------------------------------*/

/* Handle binary integer instructions of the form
      op E, G  meaning
      op reg-or-mem, reg
   Is passed the a ptr to the modRM byte, the actual operation, and the
   data size.  Returns the address advanced completely over this
   instruction.

   E(src) is reg-or-mem
   G(dst) is reg.

   If E is reg, -->    GET %G,  tmp
                       OP %E,   tmp
                       PUT tmp, %G
 
   If E is mem and OP is not reversible, 
                -->    (getAddr E) -> tmpa
                       LD (tmpa), tmpa
                       GET %G, tmp2
                       OP tmpa, tmp2
                       PUT tmp2, %G

   If E is mem and OP is reversible
                -->    (getAddr E) -> tmpa
                       LD (tmpa), tmpa
                       OP %G, tmpa
                       PUT tmpa, %G
*/
static
ULong dis_op2_E_G ( VexAbiInfo* vbi,
                    Prefix      pfx,
                    Bool        addSubCarry,
                    IROp        op8, 
                    Bool        keep,
                    Int         size, 
                    Long        delta0,
                    HChar*      t_amd64opc )
{
   HChar   dis_buf[50];
   Int     len;
   IRType  ty   = szToITy(size);
   IRTemp  dst1 = newTemp(ty);
   IRTemp  src  = newTemp(ty);
   IRTemp  dst0 = newTemp(ty);
   UChar   rm   = getUChar(delta0);
   IRTemp  addr = IRTemp_INVALID;

   /* addSubCarry == True indicates the intended operation is
      add-with-carry or subtract-with-borrow. */
   if (addSubCarry) {
      vassert(op8 == Iop_Add8 || op8 == Iop_Sub8);
      vassert(keep);
   }

   if (epartIsReg(rm)) {
      /* Specially handle XOR reg,reg, because that doesn't really
         depend on reg, and doing the obvious thing potentially
         generates a spurious value check failure due to the bogus
         dependency. */
      if ((op8 == Iop_Xor8 || (op8 == Iop_Sub8 && addSubCarry))
          && offsetIRegG(size,pfx,rm) == offsetIRegE(size,pfx,rm)) {
         if (False && op8 == Iop_Sub8)
            vex_printf("vex amd64->IR: sbb %%r,%%r optimisation(1)\n");
	 putIRegG(size,pfx,rm, mkU(ty,0));
      }

      assign( dst0, getIRegG(size,pfx,rm) );
      assign( src,  getIRegE(size,pfx,rm) );

      if (addSubCarry && op8 == Iop_Add8) {
         helper_ADC( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegG(size, pfx, rm, mkexpr(dst1));
      } else
      if (addSubCarry && op8 == Iop_Sub8) {
         helper_SBB( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegG(size, pfx, rm, mkexpr(dst1));
      } else {
         assign( dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)) );
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
         if (keep)
            putIRegG(size, pfx, rm, mkexpr(dst1));
      }

      DIP("%s%c %s,%s\n", t_amd64opc, nameISize(size), 
                          nameIRegE(size,pfx,rm),
                          nameIRegG(size,pfx,rm));
      return 1+delta0;
   } else {
      /* E refers to memory */
      addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign( dst0, getIRegG(size,pfx,rm) );
      assign( src,  loadLE(szToITy(size), mkexpr(addr)) );

      if (addSubCarry && op8 == Iop_Add8) {
         helper_ADC( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegG(size, pfx, rm, mkexpr(dst1));
      } else
      if (addSubCarry && op8 == Iop_Sub8) {
         helper_SBB( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegG(size, pfx, rm, mkexpr(dst1));
      } else {
         assign( dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)) );
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
         if (keep)
            putIRegG(size, pfx, rm, mkexpr(dst1));
      }

      DIP("%s%c %s,%s\n", t_amd64opc, nameISize(size), 
                          dis_buf, nameIRegG(size, pfx, rm));
      return len+delta0;
   }
}



/* Handle binary integer instructions of the form
      op G, E  meaning
      op reg, reg-or-mem
   Is passed the a ptr to the modRM byte, the actual operation, and the
   data size.  Returns the address advanced completely over this
   instruction.

   G(src) is reg.
   E(dst) is reg-or-mem

   If E is reg, -->    GET %E,  tmp
                       OP %G,   tmp
                       PUT tmp, %E
 
   If E is mem, -->    (getAddr E) -> tmpa
                       LD (tmpa), tmpv
                       OP %G, tmpv
                       ST tmpv, (tmpa)
*/
static
ULong dis_op2_G_E ( VexAbiInfo* vbi,
                    Prefix      pfx,
                    Bool        addSubCarry,
                    IROp        op8, 
                    Bool        keep,
                    Int         size, 
                    Long        delta0,
                    HChar*      t_amd64opc )
{
   HChar   dis_buf[50];
   Int     len;
   IRType  ty   = szToITy(size);
   IRTemp  dst1 = newTemp(ty);
   IRTemp  src  = newTemp(ty);
   IRTemp  dst0 = newTemp(ty);
   UChar   rm   = getUChar(delta0);
   IRTemp  addr = IRTemp_INVALID;

   /* addSubCarry == True indicates the intended operation is
      add-with-carry or subtract-with-borrow. */
   if (addSubCarry) {
      vassert(op8 == Iop_Add8 || op8 == Iop_Sub8);
      vassert(keep);
   }

   if (epartIsReg(rm)) {
      /* Specially handle XOR reg,reg, because that doesn't really
         depend on reg, and doing the obvious thing potentially
         generates a spurious value check failure due to the bogus
         dependency.  Ditto SBB reg,reg. */
      if ((op8 == Iop_Xor8 || (op8 == Iop_Sub8 && addSubCarry))
          && offsetIRegG(size,pfx,rm) == offsetIRegE(size,pfx,rm)) {
         putIRegE(size,pfx,rm, mkU(ty,0));
      }

      assign(dst0, getIRegE(size,pfx,rm));
      assign(src,  getIRegG(size,pfx,rm));

      if (addSubCarry && op8 == Iop_Add8) {
         helper_ADC( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegE(size, pfx, rm, mkexpr(dst1));
      } else
      if (addSubCarry && op8 == Iop_Sub8) {
         helper_SBB( size, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
         putIRegE(size, pfx, rm, mkexpr(dst1));
      } else {
         assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)));
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
         if (keep)
            putIRegE(size, pfx, rm, mkexpr(dst1));
      }

      DIP("%s%c %s,%s\n", t_amd64opc, nameISize(size), 
                          nameIRegG(size,pfx,rm),
                          nameIRegE(size,pfx,rm));
      return 1+delta0;
   }

   /* E refers to memory */    
   {
      addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign(dst0, loadLE(ty,mkexpr(addr)));
      assign(src,  getIRegG(size,pfx,rm));

      if (addSubCarry && op8 == Iop_Add8) {
         if (pfx & PFX_LOCK) {
            /* cas-style store */
            helper_ADC( size, dst1, dst0, src,
                        /*store*/addr, dst0/*expVal*/, guest_RIP_curr_instr );
         } else {
            /* normal store */
            helper_ADC( size, dst1, dst0, src,
                        /*store*/addr, IRTemp_INVALID, 0 );
         }
      } else
      if (addSubCarry && op8 == Iop_Sub8) {
         if (pfx & PFX_LOCK) {
            /* cas-style store */
            helper_SBB( size, dst1, dst0, src,
                        /*store*/addr, dst0/*expVal*/, guest_RIP_curr_instr );
         } else {
            /* normal store */
            helper_SBB( size, dst1, dst0, src,
                        /*store*/addr, IRTemp_INVALID, 0 );
         }
      } else {
         assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)));
         if (keep) {
            if (pfx & PFX_LOCK) {
               if (0) vex_printf("locked case\n" );
               casLE( mkexpr(addr),
                      mkexpr(dst0)/*expval*/, 
                      mkexpr(dst1)/*newval*/, guest_RIP_curr_instr );
            } else {
               if (0) vex_printf("nonlocked case\n");
               storeLE(mkexpr(addr), mkexpr(dst1));
            }
         }
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
      }

      DIP("%s%c %s,%s\n", t_amd64opc, nameISize(size), 
                          nameIRegG(size,pfx,rm), dis_buf);
      return len+delta0;
   }
}


/* Handle move instructions of the form
      mov E, G  meaning
      mov reg-or-mem, reg
   Is passed the a ptr to the modRM byte, and the data size.  Returns
   the address advanced completely over this instruction.

   E(src) is reg-or-mem
   G(dst) is reg.

   If E is reg, -->    GET %E,  tmpv
                       PUT tmpv, %G
 
   If E is mem  -->    (getAddr E) -> tmpa
                       LD (tmpa), tmpb
                       PUT tmpb, %G
*/
static
ULong dis_mov_E_G ( VexAbiInfo* vbi,
                    Prefix      pfx,
                    Int         size, 
                    Long        delta0 )
{
   Int len;
   UChar rm = getUChar(delta0);
   HChar dis_buf[50];

   if (epartIsReg(rm)) {
      putIRegG(size, pfx, rm, getIRegE(size, pfx, rm));
      DIP("mov%c %s,%s\n", nameISize(size),
                           nameIRegE(size,pfx,rm),
                           nameIRegG(size,pfx,rm));
      return 1+delta0;
   }

   /* E refers to memory */    
   {
      IRTemp addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      putIRegG(size, pfx, rm, loadLE(szToITy(size), mkexpr(addr)));
      DIP("mov%c %s,%s\n", nameISize(size), 
                           dis_buf,
                           nameIRegG(size,pfx,rm));
      return delta0+len;
   }
}


/* Handle move instructions of the form
      mov G, E  meaning
      mov reg, reg-or-mem
   Is passed the a ptr to the modRM byte, and the data size.  Returns
   the address advanced completely over this instruction.

   G(src) is reg.
   E(dst) is reg-or-mem

   If E is reg, -->    GET %G,  tmp
                       PUT tmp, %E
 
   If E is mem, -->    (getAddr E) -> tmpa
                       GET %G, tmpv
                       ST tmpv, (tmpa) 
*/
static
ULong dis_mov_G_E ( VexAbiInfo* vbi,
                    Prefix      pfx,
                    Int         size, 
                    Long        delta0 )
{
   Int len;
   UChar rm = getUChar(delta0);
   HChar dis_buf[50];

   if (epartIsReg(rm)) {
      putIRegE(size, pfx, rm, getIRegG(size, pfx, rm));
      DIP("mov%c %s,%s\n", nameISize(size),
                           nameIRegG(size,pfx,rm),
                           nameIRegE(size,pfx,rm));
      return 1+delta0;
   }

   /* E refers to memory */    
   {
      IRTemp addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      storeLE( mkexpr(addr), getIRegG(size, pfx, rm) );
      DIP("mov%c %s,%s\n", nameISize(size), 
                           nameIRegG(size,pfx,rm), 
                           dis_buf);
      return len+delta0;
   }
}


/* op $immediate, AL/AX/EAX/RAX. */
static
ULong dis_op_imm_A ( Int    size,
                     Bool   carrying,
                     IROp   op8,
                     Bool   keep,
                     Long   delta,
                     HChar* t_amd64opc )
{
   Int    size4 = imin(size,4);
   IRType ty    = szToITy(size);
   IRTemp dst0  = newTemp(ty);
   IRTemp src   = newTemp(ty);
   IRTemp dst1  = newTemp(ty);
   Long  lit    = getSDisp(size4,delta);
   assign(dst0, getIRegRAX(size));
   assign(src,  mkU(ty,lit & mkSizeMask(size)));

   if (isAddSub(op8) && !carrying) {
      assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)) );
      setFlags_DEP1_DEP2(op8, dst0, src, ty);
   }
   else
   if (isLogic(op8)) {
      vassert(!carrying);
      assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)) );
      setFlags_DEP1(op8, dst1, ty);
   }
   else
   if (op8 == Iop_Add8 && carrying) {
      helper_ADC( size, dst1, dst0, src,
                  /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
   }
   else
   if (op8 == Iop_Sub8 && carrying) {
      helper_SBB( size, dst1, dst0, src,
                  /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
   }
   else
      vpanic("dis_op_imm_A(amd64,guest)");

   if (keep)
      putIRegRAX(size, mkexpr(dst1));

   DIP("%s%c $%lld, %s\n", t_amd64opc, nameISize(size), 
                           lit, nameIRegRAX(size));
   return delta+size4;
}


/* Sign- and Zero-extending moves. */
static
ULong dis_movx_E_G ( VexAbiInfo* vbi,
                     Prefix pfx,
                     Long delta, Int szs, Int szd, Bool sign_extend )
{
   UChar rm = getUChar(delta);
   if (epartIsReg(rm)) {
      putIRegG(szd, pfx, rm,
                    doScalarWidening(
                       szs,szd,sign_extend,
                       getIRegE(szs,pfx,rm)));
      DIP("mov%c%c%c %s,%s\n", sign_extend ? 's' : 'z',
                               nameISize(szs), 
                               nameISize(szd),
                               nameIRegE(szs,pfx,rm),
                               nameIRegG(szd,pfx,rm));
      return 1+delta;
   }

   /* E refers to memory */    
   {
      Int    len;
      HChar  dis_buf[50];
      IRTemp addr = disAMode ( &len, vbi, pfx, delta, dis_buf, 0 );
      putIRegG(szd, pfx, rm,
                    doScalarWidening(
                       szs,szd,sign_extend, 
                       loadLE(szToITy(szs),mkexpr(addr))));
      DIP("mov%c%c%c %s,%s\n", sign_extend ? 's' : 'z',
                               nameISize(szs), 
                               nameISize(szd),
                               dis_buf, 
                               nameIRegG(szd,pfx,rm));
      return len+delta;
   }
}


/* Generate code to divide ArchRegs RDX:RAX / EDX:EAX / DX:AX / AX by
   the 64 / 32 / 16 / 8 bit quantity in the given IRTemp.  */
static
void codegen_div ( Int sz, IRTemp t, Bool signed_divide )
{
   /* special-case the 64-bit case */
   if (sz == 8) {
      IROp   op     = signed_divide ? Iop_DivModS128to64 
                                    : Iop_DivModU128to64;
      IRTemp src128 = newTemp(Ity_I128);
      IRTemp dst128 = newTemp(Ity_I128);
      assign( src128, binop(Iop_64HLto128, 
                            getIReg64(R_RDX), 
                            getIReg64(R_RAX)) );
      assign( dst128, binop(op, mkexpr(src128), mkexpr(t)) );
      putIReg64( R_RAX, unop(Iop_128to64,mkexpr(dst128)) );
      putIReg64( R_RDX, unop(Iop_128HIto64,mkexpr(dst128)) );
   } else {
      IROp   op    = signed_divide ? Iop_DivModS64to32 
                                   : Iop_DivModU64to32;
      IRTemp src64 = newTemp(Ity_I64);
      IRTemp dst64 = newTemp(Ity_I64);
      switch (sz) {
      case 4:
         assign( src64, 
                 binop(Iop_32HLto64, getIRegRDX(4), getIRegRAX(4)) );
         assign( dst64, 
                 binop(op, mkexpr(src64), mkexpr(t)) );
         putIRegRAX( 4, unop(Iop_64to32,mkexpr(dst64)) );
         putIRegRDX( 4, unop(Iop_64HIto32,mkexpr(dst64)) );
         break;
      case 2: {
         IROp widen3264 = signed_divide ? Iop_32Sto64 : Iop_32Uto64;
         IROp widen1632 = signed_divide ? Iop_16Sto32 : Iop_16Uto32;
         assign( src64, unop(widen3264,
                             binop(Iop_16HLto32, 
                                   getIRegRDX(2), 
                                   getIRegRAX(2))) );
         assign( dst64, binop(op, mkexpr(src64), unop(widen1632,mkexpr(t))) );
         putIRegRAX( 2, unop(Iop_32to16,unop(Iop_64to32,mkexpr(dst64))) );
         putIRegRDX( 2, unop(Iop_32to16,unop(Iop_64HIto32,mkexpr(dst64))) );
         break;
      }
      case 1: {
         IROp widen3264 = signed_divide ? Iop_32Sto64 : Iop_32Uto64;
         IROp widen1632 = signed_divide ? Iop_16Sto32 : Iop_16Uto32;
         IROp widen816  = signed_divide ? Iop_8Sto16  : Iop_8Uto16;
         assign( src64, unop(widen3264, 
                        unop(widen1632, getIRegRAX(2))) );
         assign( dst64, 
                 binop(op, mkexpr(src64), 
                           unop(widen1632, unop(widen816, mkexpr(t)))) );
         putIRegRAX( 1, unop(Iop_16to8, 
                        unop(Iop_32to16,
                        unop(Iop_64to32,mkexpr(dst64)))) );
         putIRegAH( unop(Iop_16to8, 
                    unop(Iop_32to16,
                    unop(Iop_64HIto32,mkexpr(dst64)))) );
         break;
      }
      default: 
         vpanic("codegen_div(amd64)");
      }
   }
}

static 
ULong dis_Grp1 ( VexAbiInfo* vbi,
                 Prefix pfx,
                 Long delta, UChar modrm, 
                 Int am_sz, Int d_sz, Int sz, Long d64 )
{
   Int     len;
   HChar   dis_buf[50];
   IRType  ty   = szToITy(sz);
   IRTemp  dst1 = newTemp(ty);
   IRTemp  src  = newTemp(ty);
   IRTemp  dst0 = newTemp(ty);
   IRTemp  addr = IRTemp_INVALID;
   IROp    op8  = Iop_INVALID;
   ULong   mask = mkSizeMask(sz);

   switch (gregLO3ofRM(modrm)) {
      case 0: op8 = Iop_Add8; break;  case 1: op8 = Iop_Or8;  break;
      case 2: break;  // ADC
      case 3: break;  // SBB
      case 4: op8 = Iop_And8; break;  case 5: op8 = Iop_Sub8; break;
      case 6: op8 = Iop_Xor8; break;  case 7: op8 = Iop_Sub8; break;
      /*NOTREACHED*/
      default: vpanic("dis_Grp1(amd64): unhandled case");
   }

   if (epartIsReg(modrm)) {
      vassert(am_sz == 1);

      assign(dst0, getIRegE(sz,pfx,modrm));
      assign(src,  mkU(ty,d64 & mask));

      if (gregLO3ofRM(modrm) == 2 /* ADC */) {
         helper_ADC( sz, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
      } else 
      if (gregLO3ofRM(modrm) == 3 /* SBB */) {
         helper_SBB( sz, dst1, dst0, src,
                     /*no store*/IRTemp_INVALID, IRTemp_INVALID, 0 );
      } else {
         assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)));
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
      }

      if (gregLO3ofRM(modrm) < 7)
         putIRegE(sz, pfx, modrm, mkexpr(dst1));

      delta += (am_sz + d_sz);
      DIP("%s%c $%lld, %s\n", 
          nameGrp1(gregLO3ofRM(modrm)), nameISize(sz), d64, 
          nameIRegE(sz,pfx,modrm));
   } else {
      addr = disAMode ( &len, vbi, pfx, delta, dis_buf, /*xtra*/d_sz );

      assign(dst0, loadLE(ty,mkexpr(addr)));
      assign(src, mkU(ty,d64 & mask));

      if (gregLO3ofRM(modrm) == 2 /* ADC */) {
         if (pfx & PFX_LOCK) {
            /* cas-style store */
            helper_ADC( sz, dst1, dst0, src,
                       /*store*/addr, dst0/*expVal*/, guest_RIP_curr_instr );
         } else {
            /* normal store */
            helper_ADC( sz, dst1, dst0, src,
                        /*store*/addr, IRTemp_INVALID, 0 );
         }
      } else 
      if (gregLO3ofRM(modrm) == 3 /* SBB */) {
         if (pfx & PFX_LOCK) {
            /* cas-style store */
            helper_SBB( sz, dst1, dst0, src,
                       /*store*/addr, dst0/*expVal*/, guest_RIP_curr_instr );
         } else {
            /* normal store */
            helper_SBB( sz, dst1, dst0, src,
                        /*store*/addr, IRTemp_INVALID, 0 );
         }
      } else {
         assign(dst1, binop(mkSizedOp(ty,op8), mkexpr(dst0), mkexpr(src)));
         if (gregLO3ofRM(modrm) < 7) {
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr), mkexpr(dst0)/*expVal*/, 
                                    mkexpr(dst1)/*newVal*/,
                                    guest_RIP_curr_instr );
            } else {
               storeLE(mkexpr(addr), mkexpr(dst1));
            }
         }
         if (isAddSub(op8))
            setFlags_DEP1_DEP2(op8, dst0, src, ty);
         else
            setFlags_DEP1(op8, dst1, ty);
      }

      delta += (len+d_sz);
      DIP("%s%c $%lld, %s\n", 
          nameGrp1(gregLO3ofRM(modrm)), nameISize(sz),
          d64, dis_buf);
   }
   return delta;
}


/* Group 2 extended opcodes.  shift_expr must be an 8-bit typed
   expression. */

static
ULong dis_Grp2 ( VexAbiInfo* vbi,
                 Prefix pfx,
                 Long delta, UChar modrm,
                 Int am_sz, Int d_sz, Int sz, IRExpr* shift_expr,
                 HChar* shift_expr_txt, Bool* decode_OK )
{
   /* delta on entry points at the modrm byte. */
   HChar  dis_buf[50];
   Int    len;
   Bool   isShift, isRotate, isRotateC;
   IRType ty    = szToITy(sz);
   IRTemp dst0  = newTemp(ty);
   IRTemp dst1  = newTemp(ty);
   IRTemp addr  = IRTemp_INVALID;

   *decode_OK = True;

   vassert(sz == 1 || sz == 2 || sz == 4 || sz == 8);

   /* Put value to shift/rotate in dst0. */
   if (epartIsReg(modrm)) {
      assign(dst0, getIRegE(sz, pfx, modrm));
      delta += (am_sz + d_sz);
   } else {
      addr = disAMode ( &len, vbi, pfx, delta, dis_buf, /*xtra*/d_sz );
      assign(dst0, loadLE(ty,mkexpr(addr)));
      delta += len + d_sz;
   }

   isShift = False;
   switch (gregLO3ofRM(modrm)) { case 4: case 5: case 6: case 7: isShift = True; }

   isRotate = False;
   switch (gregLO3ofRM(modrm)) { case 0: case 1: isRotate = True; }

   isRotateC = False;
   switch (gregLO3ofRM(modrm)) { case 2: case 3: isRotateC = True; }

   if (!isShift && !isRotate && !isRotateC) {
      /*NOTREACHED*/
      vpanic("dis_Grp2(Reg): unhandled case(amd64)");
   }

   if (isRotateC) {
      /* Call a helper; this insn is so ridiculous it does not deserve
         better.  One problem is, the helper has to calculate both the
         new value and the new flags.  This is more than 64 bits, and
         there is no way to return more than 64 bits from the helper.
         Hence the crude and obvious solution is to call it twice,
         using the sign of the sz field to indicate whether it is the
         value or rflags result we want.
      */
      Bool     left = toBool(gregLO3ofRM(modrm) == 2);
      IRExpr** argsVALUE;
      IRExpr** argsRFLAGS;

      IRTemp new_value  = newTemp(Ity_I64);
      IRTemp new_rflags = newTemp(Ity_I64);
      IRTemp old_rflags = newTemp(Ity_I64);

      assign( old_rflags, widenUto64(mk_amd64g_calculate_rflags_all()) );

      argsVALUE
         = mkIRExprVec_4( widenUto64(mkexpr(dst0)), /* thing to rotate */
                          widenUto64(shift_expr),   /* rotate amount */
                          mkexpr(old_rflags),
                          mkU64(sz) );
      assign( new_value, 
                 mkIRExprCCall(
                    Ity_I64, 
                    0/*regparm*/, 
                    left ? "amd64g_calculate_RCL" : "amd64g_calculate_RCR",
                    left ? &amd64g_calculate_RCL  : &amd64g_calculate_RCR,
                    argsVALUE
                 )
            );
      
      argsRFLAGS
         = mkIRExprVec_4( widenUto64(mkexpr(dst0)), /* thing to rotate */
                          widenUto64(shift_expr),   /* rotate amount */
                          mkexpr(old_rflags),
                          mkU64(-sz) );
      assign( new_rflags, 
                 mkIRExprCCall(
                    Ity_I64, 
                    0/*regparm*/, 
                    left ? "amd64g_calculate_RCL" : "amd64g_calculate_RCR",
                    left ? &amd64g_calculate_RCL  : &amd64g_calculate_RCR,
                    argsRFLAGS
                 )
            );

      assign( dst1, narrowTo(ty, mkexpr(new_value)) );
      stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
      stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(new_rflags) ));
      stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
      stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
   }

   else
   if (isShift) {

      IRTemp pre64     = newTemp(Ity_I64);
      IRTemp res64     = newTemp(Ity_I64);
      IRTemp res64ss   = newTemp(Ity_I64);
      IRTemp shift_amt = newTemp(Ity_I8);
      UChar  mask      = toUChar(sz==8 ? 63 : 31);
      IROp   op64;

      switch (gregLO3ofRM(modrm)) { 
         case 4: op64 = Iop_Shl64; break;
         case 5: op64 = Iop_Shr64; break;
         case 6: op64 = Iop_Shl64; break;
         case 7: op64 = Iop_Sar64; break;
         /*NOTREACHED*/
         default: vpanic("dis_Grp2:shift"); break;
      }

      /* Widen the value to be shifted to 64 bits, do the shift, and
         narrow back down.  This seems surprisingly long-winded, but
         unfortunately the AMD semantics requires that 8/16/32-bit
         shifts give defined results for shift values all the way up
         to 32, and this seems the simplest way to do it.  It has the
         advantage that the only IR level shifts generated are of 64
         bit values, and the shift amount is guaranteed to be in the
         range 0 .. 63, thereby observing the IR semantics requiring
         all shift values to be in the range 0 .. 2^word_size-1. 

         Therefore the shift amount is masked with 63 for 64-bit shifts
         and 31 for all others.
      */
      /* shift_amt = shift_expr & MASK, regardless of operation size */
      assign( shift_amt, binop(Iop_And8, shift_expr, mkU8(mask)) );

      /* suitably widen the value to be shifted to 64 bits. */
      assign( pre64, op64==Iop_Sar64 ? widenSto64(mkexpr(dst0))
                                     : widenUto64(mkexpr(dst0)) );

      /* res64 = pre64 `shift` shift_amt */
      assign( res64, binop(op64, mkexpr(pre64), mkexpr(shift_amt)) );

      /* res64ss = pre64 `shift` ((shift_amt - 1) & MASK) */
      assign( res64ss,
              binop(op64,
                    mkexpr(pre64), 
                    binop(Iop_And8,
                          binop(Iop_Sub8,
                                mkexpr(shift_amt), mkU8(1)),
                          mkU8(mask))) );

      /* Build the flags thunk. */
      setFlags_DEP1_DEP2_shift(op64, res64, res64ss, ty, shift_amt);

      /* Narrow the result back down. */
      assign( dst1, narrowTo(ty, mkexpr(res64)) );

   } /* if (isShift) */

   else 
   if (isRotate) {
      Int    ccOp      = ty==Ity_I8 ? 0 : (ty==Ity_I16 ? 1 
                                        : (ty==Ity_I32 ? 2 : 3));
      Bool   left      = toBool(gregLO3ofRM(modrm) == 0);
      IRTemp rot_amt   = newTemp(Ity_I8);
      IRTemp rot_amt64 = newTemp(Ity_I8);
      IRTemp oldFlags  = newTemp(Ity_I64);
      UChar  mask      = toUChar(sz==8 ? 63 : 31);

      /* rot_amt = shift_expr & mask */
      /* By masking the rotate amount thusly, the IR-level Shl/Shr
         expressions never shift beyond the word size and thus remain
         well defined. */
      assign(rot_amt64, binop(Iop_And8, shift_expr, mkU8(mask)));

      if (ty == Ity_I64)
         assign(rot_amt, mkexpr(rot_amt64));
      else
         assign(rot_amt, binop(Iop_And8, mkexpr(rot_amt64), mkU8(8*sz-1)));

      if (left) {

         /* dst1 = (dst0 << rot_amt) | (dst0 >>u (wordsize-rot_amt)) */
         assign(dst1, 
            binop( mkSizedOp(ty,Iop_Or8),
                   binop( mkSizedOp(ty,Iop_Shl8), 
                          mkexpr(dst0),
                          mkexpr(rot_amt)
                   ),
                   binop( mkSizedOp(ty,Iop_Shr8), 
                          mkexpr(dst0), 
                          binop(Iop_Sub8,mkU8(8*sz), mkexpr(rot_amt))
                   )
            )
         );
         ccOp += AMD64G_CC_OP_ROLB;

      } else { /* right */

         /* dst1 = (dst0 >>u rot_amt) | (dst0 << (wordsize-rot_amt)) */
         assign(dst1, 
            binop( mkSizedOp(ty,Iop_Or8),
                   binop( mkSizedOp(ty,Iop_Shr8), 
                          mkexpr(dst0),
                          mkexpr(rot_amt)
                   ),
                   binop( mkSizedOp(ty,Iop_Shl8), 
                          mkexpr(dst0), 
                          binop(Iop_Sub8,mkU8(8*sz), mkexpr(rot_amt))
                   )
            )
         );
         ccOp += AMD64G_CC_OP_RORB;

      }

      /* dst1 now holds the rotated value.  Build flag thunk.  We
         need the resulting value for this, and the previous flags.
         Except don't set it if the rotate count is zero. */

      assign(oldFlags, mk_amd64g_calculate_rflags_all());

      /* CC_DEP1 is the rotated value.  CC_NDEP is flags before. */
      stmt( IRStmt_Put( OFFB_CC_OP,
                        IRExpr_Mux0X( mkexpr(rot_amt64),
                                      IRExpr_Get(OFFB_CC_OP,Ity_I64),
                                      mkU64(ccOp))) );
      stmt( IRStmt_Put( OFFB_CC_DEP1, 
                        IRExpr_Mux0X( mkexpr(rot_amt64),
                                      IRExpr_Get(OFFB_CC_DEP1,Ity_I64),
                                      widenUto64(mkexpr(dst1)))) );
      stmt( IRStmt_Put( OFFB_CC_DEP2, 
                        IRExpr_Mux0X( mkexpr(rot_amt64),
                                      IRExpr_Get(OFFB_CC_DEP2,Ity_I64),
                                      mkU64(0))) );
      stmt( IRStmt_Put( OFFB_CC_NDEP, 
                        IRExpr_Mux0X( mkexpr(rot_amt64),
                                      IRExpr_Get(OFFB_CC_NDEP,Ity_I64),
                                      mkexpr(oldFlags))) );
   } /* if (isRotate) */

   /* Save result, and finish up. */
   if (epartIsReg(modrm)) {
      putIRegE(sz, pfx, modrm, mkexpr(dst1));
      if (vex_traceflags & VEX_TRACE_FE) {
         vex_printf("%s%c ",
                    nameGrp2(gregLO3ofRM(modrm)), nameISize(sz) );
         if (shift_expr_txt)
            vex_printf("%s", shift_expr_txt);
         else
            ppIRExpr(shift_expr);
         vex_printf(", %s\n", nameIRegE(sz,pfx,modrm));
      }
   } else {
      storeLE(mkexpr(addr), mkexpr(dst1));
      if (vex_traceflags & VEX_TRACE_FE) {
         vex_printf("%s%c ",
                    nameGrp2(gregLO3ofRM(modrm)), nameISize(sz) );
         if (shift_expr_txt)
            vex_printf("%s", shift_expr_txt);
         else
            ppIRExpr(shift_expr);
         vex_printf(", %s\n", dis_buf);
      }
   }
   return delta;
}


/* Group 8 extended opcodes (but BT/BTS/BTC/BTR only). */
static
ULong dis_Grp8_Imm ( VexAbiInfo* vbi,
                     Prefix pfx,
                     Long delta, UChar modrm,
                     Int am_sz, Int sz, ULong src_val,
                     Bool* decode_OK )
{
   /* src_val denotes a d8.
      And delta on entry points at the modrm byte. */

   IRType ty     = szToITy(sz);
   IRTemp t2     = newTemp(Ity_I64);
   IRTemp t2m    = newTemp(Ity_I64);
   IRTemp t_addr = IRTemp_INVALID;
   HChar  dis_buf[50];
   ULong  mask;

   /* we're optimists :-) */
   *decode_OK = True;

   /* Limit src_val -- the bit offset -- to something within a word.
      The Intel docs say that literal offsets larger than a word are
      masked in this way. */
   switch (sz) {
      case 2:  src_val &= 15; break;
      case 4:  src_val &= 31; break;
      case 8:  src_val &= 63; break;
      default: *decode_OK = False; return delta;
   }

   /* Invent a mask suitable for the operation. */
   switch (gregLO3ofRM(modrm)) {
      case 4: /* BT */  mask = 0;                  break;
      case 5: /* BTS */ mask = 1ULL << src_val;    break;
      case 6: /* BTR */ mask = ~(1ULL << src_val); break;
      case 7: /* BTC */ mask = 1ULL << src_val;    break;
         /* If this needs to be extended, probably simplest to make a
            new function to handle the other cases (0 .. 3).  The
            Intel docs do however not indicate any use for 0 .. 3, so
            we don't expect this to happen. */
      default: *decode_OK = False; return delta;
   }

   /* Fetch the value to be tested and modified into t2, which is
      64-bits wide regardless of sz. */
   if (epartIsReg(modrm)) {
      vassert(am_sz == 1);
      assign( t2, widenUto64(getIRegE(sz, pfx, modrm)) );
      delta += (am_sz + 1);
      DIP("%s%c $0x%llx, %s\n", nameGrp8(gregLO3ofRM(modrm)), 
                                nameISize(sz),
                                src_val, nameIRegE(sz,pfx,modrm));
   } else {
      Int len;
      t_addr = disAMode ( &len, vbi, pfx, delta, dis_buf, 1 );
      delta  += (len+1);
      assign( t2, widenUto64(loadLE(ty, mkexpr(t_addr))) );
      DIP("%s%c $0x%llx, %s\n", nameGrp8(gregLO3ofRM(modrm)), 
                                nameISize(sz),
                                src_val, dis_buf);
   }

   /* Compute the new value into t2m, if non-BT. */
   switch (gregLO3ofRM(modrm)) {
      case 4: /* BT */
         break;
      case 5: /* BTS */
         assign( t2m, binop(Iop_Or64, mkU64(mask), mkexpr(t2)) );
         break;
      case 6: /* BTR */
         assign( t2m, binop(Iop_And64, mkU64(mask), mkexpr(t2)) );
         break;
      case 7: /* BTC */
         assign( t2m, binop(Iop_Xor64, mkU64(mask), mkexpr(t2)) );
         break;
     default: 
         /*NOTREACHED*/ /*the previous switch guards this*/
         vassert(0);
   }

   /* Write the result back, if non-BT. */
   if (gregLO3ofRM(modrm) != 4 /* BT */) {
      if (epartIsReg(modrm)) {
	putIRegE(sz, pfx, modrm, narrowTo(ty, mkexpr(t2m)));
      } else {
         if (pfx & PFX_LOCK) {
            casLE( mkexpr(t_addr),
                   narrowTo(ty, mkexpr(t2))/*expd*/,
                   narrowTo(ty, mkexpr(t2m))/*new*/,
                   guest_RIP_curr_instr );
         } else {
            storeLE(mkexpr(t_addr), narrowTo(ty, mkexpr(t2m)));
         }
      }
   }

   /* Copy relevant bit from t2 into the carry flag. */
   /* Flags: C=selected bit, O,S,Z,A,P undefined, so are set to zero. */
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            binop(Iop_And64,
                  binop(Iop_Shr64, mkexpr(t2), mkU8(src_val)),
                  mkU64(1))
       ));
   /* Set NDEP even though it isn't used.  This makes redundant-PUT
      elimination of previous stores to this field work better. */
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));

   return delta;
}


/* Signed/unsigned widening multiply.  Generate IR to multiply the
   value in RAX/EAX/AX/AL by the given IRTemp, and park the result in
   RDX:RAX/EDX:EAX/DX:AX/AX.
*/
static void codegen_mulL_A_D ( Int sz, Bool syned, 
                               IRTemp tmp, HChar* tmp_txt )
{
   IRType ty = szToITy(sz);
   IRTemp t1 = newTemp(ty);

   assign( t1, getIRegRAX(sz) );

   switch (ty) {
      case Ity_I64: {
         IRTemp res128  = newTemp(Ity_I128);
         IRTemp resHi   = newTemp(Ity_I64);
         IRTemp resLo   = newTemp(Ity_I64);
         IROp   mulOp   = syned ? Iop_MullS64 : Iop_MullU64;
         UInt   tBaseOp = syned ? AMD64G_CC_OP_SMULB : AMD64G_CC_OP_UMULB;
         setFlags_MUL ( Ity_I64, t1, tmp, tBaseOp );
         assign( res128, binop(mulOp, mkexpr(t1), mkexpr(tmp)) );
         assign( resHi, unop(Iop_128HIto64,mkexpr(res128)));
         assign( resLo, unop(Iop_128to64,mkexpr(res128)));
         putIReg64(R_RDX, mkexpr(resHi));
         putIReg64(R_RAX, mkexpr(resLo));
         break;
      }
      case Ity_I32: {
         IRTemp res64   = newTemp(Ity_I64);
         IRTemp resHi   = newTemp(Ity_I32);
         IRTemp resLo   = newTemp(Ity_I32);
         IROp   mulOp   = syned ? Iop_MullS32 : Iop_MullU32;
         UInt   tBaseOp = syned ? AMD64G_CC_OP_SMULB : AMD64G_CC_OP_UMULB;
         setFlags_MUL ( Ity_I32, t1, tmp, tBaseOp );
         assign( res64, binop(mulOp, mkexpr(t1), mkexpr(tmp)) );
         assign( resHi, unop(Iop_64HIto32,mkexpr(res64)));
         assign( resLo, unop(Iop_64to32,mkexpr(res64)));
         putIRegRDX(4, mkexpr(resHi));
         putIRegRAX(4, mkexpr(resLo));
         break;
      }
      case Ity_I16: {
         IRTemp res32   = newTemp(Ity_I32);
         IRTemp resHi   = newTemp(Ity_I16);
         IRTemp resLo   = newTemp(Ity_I16);
         IROp   mulOp   = syned ? Iop_MullS16 : Iop_MullU16;
         UInt   tBaseOp = syned ? AMD64G_CC_OP_SMULB : AMD64G_CC_OP_UMULB;
         setFlags_MUL ( Ity_I16, t1, tmp, tBaseOp );
         assign( res32, binop(mulOp, mkexpr(t1), mkexpr(tmp)) );
         assign( resHi, unop(Iop_32HIto16,mkexpr(res32)));
         assign( resLo, unop(Iop_32to16,mkexpr(res32)));
         putIRegRDX(2, mkexpr(resHi));
         putIRegRAX(2, mkexpr(resLo));
         break;
      }
      case Ity_I8: {
         IRTemp res16   = newTemp(Ity_I16);
         IRTemp resHi   = newTemp(Ity_I8);
         IRTemp resLo   = newTemp(Ity_I8);
         IROp   mulOp   = syned ? Iop_MullS8 : Iop_MullU8;
         UInt   tBaseOp = syned ? AMD64G_CC_OP_SMULB : AMD64G_CC_OP_UMULB;
         setFlags_MUL ( Ity_I8, t1, tmp, tBaseOp );
         assign( res16, binop(mulOp, mkexpr(t1), mkexpr(tmp)) );
         assign( resHi, unop(Iop_16HIto8,mkexpr(res16)));
         assign( resLo, unop(Iop_16to8,mkexpr(res16)));
         putIRegRAX(2, mkexpr(res16));
         break;
      }
      default:
         ppIRType(ty);
         vpanic("codegen_mulL_A_D(amd64)");
   }
   DIP("%s%c %s\n", syned ? "imul" : "mul", nameISize(sz), tmp_txt);
}


/* Group 3 extended opcodes. */
static 
ULong dis_Grp3 ( VexAbiInfo* vbi, 
                 Prefix pfx, Int sz, Long delta, Bool* decode_OK )
{
   Long    d64;
   UChar   modrm;
   HChar   dis_buf[50];
   Int     len;
   IRTemp  addr;
   IRType  ty = szToITy(sz);
   IRTemp  t1 = newTemp(ty);
   IRTemp dst1, src, dst0;
   *decode_OK = True;
   modrm = getUChar(delta);
   if (epartIsReg(modrm)) {
      switch (gregLO3ofRM(modrm)) {
         case 0: { /* TEST */
            delta++; 
            d64 = getSDisp(imin(4,sz), delta); 
            delta += imin(4,sz);
            dst1 = newTemp(ty);
            assign(dst1, binop(mkSizedOp(ty,Iop_And8),
                               getIRegE(sz,pfx,modrm),
                               mkU(ty, d64 & mkSizeMask(sz))));
            setFlags_DEP1( Iop_And8, dst1, ty );
            DIP("test%c $%lld, %s\n", 
                nameISize(sz), d64, 
                nameIRegE(sz, pfx, modrm));
            break;
         }
         case 1:
            *decode_OK = False;
            return delta;
         case 2: /* NOT */
            delta++;
            putIRegE(sz, pfx, modrm,
                              unop(mkSizedOp(ty,Iop_Not8),
                                   getIRegE(sz, pfx, modrm)));
            DIP("not%c %s\n", nameISize(sz), 
                              nameIRegE(sz, pfx, modrm));
            break;
         case 3: /* NEG */
            delta++;
            dst0 = newTemp(ty);
            src  = newTemp(ty);
            dst1 = newTemp(ty);
            assign(dst0, mkU(ty,0));
            assign(src,  getIRegE(sz, pfx, modrm));
            assign(dst1, binop(mkSizedOp(ty,Iop_Sub8), mkexpr(dst0),
                                                       mkexpr(src)));
            setFlags_DEP1_DEP2(Iop_Sub8, dst0, src, ty);
            putIRegE(sz, pfx, modrm, mkexpr(dst1));
            DIP("neg%c %s\n", nameISize(sz), nameIRegE(sz, pfx, modrm));
            break;
         case 4: /* MUL (unsigned widening) */
            delta++;
            src = newTemp(ty);
            assign(src, getIRegE(sz,pfx,modrm));
            codegen_mulL_A_D ( sz, False, src,
                               nameIRegE(sz,pfx,modrm) );
            break;
         case 5: /* IMUL (signed widening) */
            delta++;
            src = newTemp(ty);
            assign(src, getIRegE(sz,pfx,modrm));
            codegen_mulL_A_D ( sz, True, src,
                               nameIRegE(sz,pfx,modrm) );
            break;
         case 6: /* DIV */
            delta++;
            assign( t1, getIRegE(sz, pfx, modrm) );
            codegen_div ( sz, t1, False );
            DIP("div%c %s\n", nameISize(sz), 
                              nameIRegE(sz, pfx, modrm));
            break;
         case 7: /* IDIV */
            delta++;
            assign( t1, getIRegE(sz, pfx, modrm) );
            codegen_div ( sz, t1, True );
            DIP("idiv%c %s\n", nameISize(sz), 
                               nameIRegE(sz, pfx, modrm));
            break;
         default: 
            /*NOTREACHED*/
            vpanic("Grp3(amd64,R)");
      }
   } else {
      addr = disAMode ( &len, vbi, pfx, delta, dis_buf,
                        /* we have to inform disAMode of any immediate
			   bytes used */
                        gregLO3ofRM(modrm)==0/*TEST*/
                           ? imin(4,sz)
                           : 0
                      );
      t1   = newTemp(ty);
      delta += len;
      assign(t1, loadLE(ty,mkexpr(addr)));
      switch (gregLO3ofRM(modrm)) {
         case 0: { /* TEST */
            d64 = getSDisp(imin(4,sz), delta); 
            delta += imin(4,sz);
            dst1 = newTemp(ty);
            assign(dst1, binop(mkSizedOp(ty,Iop_And8),
                               mkexpr(t1), 
                               mkU(ty, d64 & mkSizeMask(sz))));
            setFlags_DEP1( Iop_And8, dst1, ty );
            DIP("test%c $%lld, %s\n", nameISize(sz), d64, dis_buf);
            break;
         }
         case 1:
            *decode_OK = False;
            return delta;
         case 2: /* NOT */
            dst1 = newTemp(ty);
            assign(dst1, unop(mkSizedOp(ty,Iop_Not8), mkexpr(t1)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr), mkexpr(t1)/*expd*/, mkexpr(dst1)/*new*/,
                                    guest_RIP_curr_instr );
            } else {
               storeLE( mkexpr(addr), mkexpr(dst1) );
            }
            DIP("not%c %s\n", nameISize(sz), dis_buf);
            break;
         case 3: /* NEG */
            dst0 = newTemp(ty);
            src  = newTemp(ty);
            dst1 = newTemp(ty);
            assign(dst0, mkU(ty,0));
            assign(src,  mkexpr(t1));
            assign(dst1, binop(mkSizedOp(ty,Iop_Sub8), mkexpr(dst0),
                                                       mkexpr(src)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr), mkexpr(t1)/*expd*/, mkexpr(dst1)/*new*/,
                                    guest_RIP_curr_instr );
            } else {
               storeLE( mkexpr(addr), mkexpr(dst1) );
            }
            setFlags_DEP1_DEP2(Iop_Sub8, dst0, src, ty);
            DIP("neg%c %s\n", nameISize(sz), dis_buf);
            break;
         case 4: /* MUL (unsigned widening) */
            codegen_mulL_A_D ( sz, False, t1, dis_buf );
            break;
         case 5: /* IMUL */
            codegen_mulL_A_D ( sz, True, t1, dis_buf );
            break;
         case 6: /* DIV */
            codegen_div ( sz, t1, False );
            DIP("div%c %s\n", nameISize(sz), dis_buf);
            break;
         case 7: /* IDIV */
            codegen_div ( sz, t1, True );
            DIP("idiv%c %s\n", nameISize(sz), dis_buf);
            break;
         default: 
            /*NOTREACHED*/
            vpanic("Grp3(amd64,M)");
      }
   }
   return delta;
}


/* Group 4 extended opcodes. */
static
ULong dis_Grp4 ( VexAbiInfo* vbi,
                 Prefix pfx, Long delta, Bool* decode_OK )
{
   Int   alen;
   UChar modrm;
   HChar dis_buf[50];
   IRType ty = Ity_I8;
   IRTemp t1 = newTemp(ty);
   IRTemp t2 = newTemp(ty);

   *decode_OK = True;

   modrm = getUChar(delta);
   if (epartIsReg(modrm)) {
      assign(t1, getIRegE(1, pfx, modrm));
      switch (gregLO3ofRM(modrm)) {
         case 0: /* INC */
            assign(t2, binop(Iop_Add8, mkexpr(t1), mkU8(1)));
            putIRegE(1, pfx, modrm, mkexpr(t2));
            setFlags_INC_DEC( True, t2, ty );
            break;
         case 1: /* DEC */
            assign(t2, binop(Iop_Sub8, mkexpr(t1), mkU8(1)));
            putIRegE(1, pfx, modrm, mkexpr(t2));
            setFlags_INC_DEC( False, t2, ty );
            break;
         default: 
            *decode_OK = False;
            return delta;
      }
      delta++;
      DIP("%sb %s\n", nameGrp4(gregLO3ofRM(modrm)),
                      nameIRegE(1, pfx, modrm));
   } else {
      IRTemp addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( t1, loadLE(ty, mkexpr(addr)) );
      switch (gregLO3ofRM(modrm)) {
         case 0: /* INC */
            assign(t2, binop(Iop_Add8, mkexpr(t1), mkU8(1)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr), mkexpr(t1)/*expd*/, mkexpr(t2)/*new*/, 
                      guest_RIP_curr_instr );
            } else {
               storeLE( mkexpr(addr), mkexpr(t2) );
            }
            setFlags_INC_DEC( True, t2, ty );
            break;
         case 1: /* DEC */
            assign(t2, binop(Iop_Sub8, mkexpr(t1), mkU8(1)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr), mkexpr(t1)/*expd*/, mkexpr(t2)/*new*/, 
                      guest_RIP_curr_instr );
            } else {
               storeLE( mkexpr(addr), mkexpr(t2) );
            }
            setFlags_INC_DEC( False, t2, ty );
            break;
         default: 
            *decode_OK = False;
            return delta;
      }
      delta += alen;
      DIP("%sb %s\n", nameGrp4(gregLO3ofRM(modrm)), dis_buf);
   }
   return delta;
}


/* Group 5 extended opcodes. */
static
ULong dis_Grp5 ( VexAbiInfo* vbi,
                 Prefix pfx, Int sz, Long delta,
                 /*MOD*/DisResult* dres, /*OUT*/Bool* decode_OK )
{
   Int     len;
   UChar   modrm;
   HChar   dis_buf[50];
   IRTemp  addr = IRTemp_INVALID;
   IRType  ty = szToITy(sz);
   IRTemp  t1 = newTemp(ty);
   IRTemp  t2 = IRTemp_INVALID;
   IRTemp  t3 = IRTemp_INVALID;
   Bool    showSz = True;

   *decode_OK = True;

   modrm = getUChar(delta);
   if (epartIsReg(modrm)) {
      assign(t1, getIRegE(sz,pfx,modrm));
      switch (gregLO3ofRM(modrm)) {
         case 0: /* INC */
            t2 = newTemp(ty);
            assign(t2, binop(mkSizedOp(ty,Iop_Add8),
                             mkexpr(t1), mkU(ty,1)));
            setFlags_INC_DEC( True, t2, ty );
            putIRegE(sz,pfx,modrm, mkexpr(t2));
            break;
         case 1: /* DEC */
            t2 = newTemp(ty);
            assign(t2, binop(mkSizedOp(ty,Iop_Sub8),
                             mkexpr(t1), mkU(ty,1)));
            setFlags_INC_DEC( False, t2, ty );
            putIRegE(sz,pfx,modrm, mkexpr(t2));
            break;
         case 2: /* call Ev */
            /* Ignore any sz value and operate as if sz==8. */
            if (!(sz == 4 || sz == 8)) goto unhandled;
            sz = 8;
            t3 = newTemp(Ity_I64);
            assign(t3, getIRegE(sz,pfx,modrm));
            t2 = newTemp(Ity_I64);
            assign(t2, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(8)));
            putIReg64(R_RSP, mkexpr(t2));
            storeLE( mkexpr(t2), mkU64(guest_RIP_bbstart+delta+1));
            make_redzone_AbiHint(vbi, t2, t3/*nia*/, "call-Ev(reg)");
            jmp_treg(dres, Ijk_Call, t3);
            vassert(dres->whatNext == Dis_StopHere);
            showSz = False;
            break;
         case 4: /* jmp Ev */
            /* Ignore any sz value and operate as if sz==8. */
            if (!(sz == 4 || sz == 8)) goto unhandled;
            sz = 8;
            t3 = newTemp(Ity_I64);
            assign(t3, getIRegE(sz,pfx,modrm));
            jmp_treg(dres, Ijk_Boring, t3);
            vassert(dres->whatNext == Dis_StopHere);
            showSz = False;
            break;
         default: 
            *decode_OK = False;
            return delta;
      }
      delta++;
      DIP("%s%c %s\n", nameGrp5(gregLO3ofRM(modrm)),
                       showSz ? nameISize(sz) : ' ', 
                       nameIRegE(sz, pfx, modrm));
   } else {
      addr = disAMode ( &len, vbi, pfx, delta, dis_buf, 0 );
      if (gregLO3ofRM(modrm) != 2 && gregLO3ofRM(modrm) != 4
                                  && gregLO3ofRM(modrm) != 6) {
         assign(t1, loadLE(ty,mkexpr(addr)));
      }
      switch (gregLO3ofRM(modrm)) {
         case 0: /* INC */ 
            t2 = newTemp(ty);
            assign(t2, binop(mkSizedOp(ty,Iop_Add8),
                             mkexpr(t1), mkU(ty,1)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr),
                      mkexpr(t1), mkexpr(t2), guest_RIP_curr_instr );
            } else {
               storeLE(mkexpr(addr),mkexpr(t2));
            }
            setFlags_INC_DEC( True, t2, ty );
            break;
         case 1: /* DEC */ 
            t2 = newTemp(ty);
            assign(t2, binop(mkSizedOp(ty,Iop_Sub8),
                             mkexpr(t1), mkU(ty,1)));
            if (pfx & PFX_LOCK) {
               casLE( mkexpr(addr),
                      mkexpr(t1), mkexpr(t2), guest_RIP_curr_instr );
            } else {
               storeLE(mkexpr(addr),mkexpr(t2));
            }
            setFlags_INC_DEC( False, t2, ty );
            break;
         case 2: /* call Ev */
            /* Ignore any sz value and operate as if sz==8. */
            if (!(sz == 4 || sz == 8)) goto unhandled;
            sz = 8;
            t3 = newTemp(Ity_I64);
            assign(t3, loadLE(Ity_I64,mkexpr(addr)));
            t2 = newTemp(Ity_I64);
            assign(t2, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(8)));
            putIReg64(R_RSP, mkexpr(t2));
            storeLE( mkexpr(t2), mkU64(guest_RIP_bbstart+delta+len));
            make_redzone_AbiHint(vbi, t2, t3/*nia*/, "call-Ev(mem)");
            jmp_treg(dres, Ijk_Call, t3);
            vassert(dres->whatNext == Dis_StopHere);
            showSz = False;
            break;
         case 4: /* JMP Ev */
            /* Ignore any sz value and operate as if sz==8. */
            if (!(sz == 4 || sz == 8)) goto unhandled;
            sz = 8;
            t3 = newTemp(Ity_I64);
            assign(t3, loadLE(Ity_I64,mkexpr(addr)));
            jmp_treg(dres, Ijk_Boring, t3);
            vassert(dres->whatNext == Dis_StopHere);
            showSz = False;
            break;
         case 6: /* PUSH Ev */
            /* There is no encoding for 32-bit operand size; hence ... */
            if (sz == 4) sz = 8;
            if (!(sz == 8 || sz == 2)) goto unhandled;
            if (sz == 8) {
               t3 = newTemp(Ity_I64);
               assign(t3, loadLE(Ity_I64,mkexpr(addr)));
               t2 = newTemp(Ity_I64);
               assign( t2, binop(Iop_Sub64,getIReg64(R_RSP),mkU64(sz)) );
               putIReg64(R_RSP, mkexpr(t2) );
               storeLE( mkexpr(t2), mkexpr(t3) );
               break;
	    } else {
               goto unhandled; /* awaiting test case */
	    }
         default: 
         unhandled:
            *decode_OK = False;
            return delta;
      }
      delta += len;
      DIP("%s%c %s\n", nameGrp5(gregLO3ofRM(modrm)),
                       showSz ? nameISize(sz) : ' ', 
                       dis_buf);
   }
   return delta;
}


/*------------------------------------------------------------*/
/*--- Disassembling string ops (including REP prefixes)    ---*/
/*------------------------------------------------------------*/

/* Code shared by all the string ops */
static
void dis_string_op_increment ( Int sz, IRTemp t_inc )
{
   UChar logSz;
   if (sz == 8 || sz == 4 || sz == 2) {
      logSz = 1;
      if (sz == 4) logSz = 2;
      if (sz == 8) logSz = 3;
      assign( t_inc, 
              binop(Iop_Shl64, IRExpr_Get( OFFB_DFLAG, Ity_I64 ),
                               mkU8(logSz) ) );
   } else {
      assign( t_inc, 
              IRExpr_Get( OFFB_DFLAG, Ity_I64 ) );
   }
}

static
void dis_string_op( void (*dis_OP)( Int, IRTemp, Prefix pfx ),
                    Int sz, HChar* name, Prefix pfx )
{
   IRTemp t_inc = newTemp(Ity_I64);
   /* Really we ought to inspect the override prefixes, but we don't.
      The following assertion catches any resulting sillyness. */
   vassert(pfx == clearSegBits(pfx));
   dis_string_op_increment(sz, t_inc);
   dis_OP( sz, t_inc, pfx );
   DIP("%s%c\n", name, nameISize(sz));
}

static 
void dis_MOVS ( Int sz, IRTemp t_inc, Prefix pfx )
{
   IRType ty = szToITy(sz);
   IRTemp td = newTemp(Ity_I64);   /* RDI */
   IRTemp ts = newTemp(Ity_I64);   /* RSI */
   IRExpr *incd, *incs;

   if (haveASO(pfx)) {
      assign( td, unop(Iop_32Uto64, getIReg32(R_RDI)) );
      assign( ts, unop(Iop_32Uto64, getIReg32(R_RSI)) );
   } else {
      assign( td, getIReg64(R_RDI) );
      assign( ts, getIReg64(R_RSI) );
   }

   storeLE( mkexpr(td), loadLE(ty,mkexpr(ts)) );

   incd = binop(Iop_Add64, mkexpr(td), mkexpr(t_inc));
   incs = binop(Iop_Add64, mkexpr(ts), mkexpr(t_inc));
   if (haveASO(pfx)) {
      incd = unop(Iop_32Uto64, unop(Iop_64to32, incd));
      incs = unop(Iop_32Uto64, unop(Iop_64to32, incs));
   }
   putIReg64( R_RDI, incd );
   putIReg64( R_RSI, incs );
}

static 
void dis_LODS ( Int sz, IRTemp t_inc, Prefix pfx )
{
   IRType ty = szToITy(sz);
   IRTemp ts = newTemp(Ity_I64);   /* RSI */
   IRExpr *incs;

   if (haveASO(pfx))
      assign( ts, unop(Iop_32Uto64, getIReg32(R_RSI)) );
   else
      assign( ts, getIReg64(R_RSI) );

   putIRegRAX ( sz, loadLE(ty, mkexpr(ts)) );

   incs = binop(Iop_Add64, mkexpr(ts), mkexpr(t_inc));
   if (haveASO(pfx))
      incs = unop(Iop_32Uto64, unop(Iop_64to32, incs));
   putIReg64( R_RSI, incs );
}

static 
void dis_STOS ( Int sz, IRTemp t_inc, Prefix pfx )
{
   IRType ty = szToITy(sz);
   IRTemp ta = newTemp(ty);        /* rAX */
   IRTemp td = newTemp(Ity_I64);   /* RDI */
   IRExpr *incd;

   assign( ta, getIRegRAX(sz) );

   if (haveASO(pfx))
      assign( td, unop(Iop_32Uto64, getIReg32(R_RDI)) );
   else
      assign( td, getIReg64(R_RDI) );

   storeLE( mkexpr(td), mkexpr(ta) );

   incd = binop(Iop_Add64, mkexpr(td), mkexpr(t_inc));
   if (haveASO(pfx))
      incd = unop(Iop_32Uto64, unop(Iop_64to32, incd));
   putIReg64( R_RDI, incd );
}

static 
void dis_CMPS ( Int sz, IRTemp t_inc, Prefix pfx )
{
   IRType ty  = szToITy(sz);
   IRTemp tdv = newTemp(ty);      /* (RDI) */
   IRTemp tsv = newTemp(ty);      /* (RSI) */
   IRTemp td  = newTemp(Ity_I64); /*  RDI  */
   IRTemp ts  = newTemp(Ity_I64); /*  RSI  */
   IRExpr *incd, *incs;

   if (haveASO(pfx)) {
      assign( td, unop(Iop_32Uto64, getIReg32(R_RDI)) );
      assign( ts, unop(Iop_32Uto64, getIReg32(R_RSI)) );
   } else {
      assign( td, getIReg64(R_RDI) );
      assign( ts, getIReg64(R_RSI) );
   }

   assign( tdv, loadLE(ty,mkexpr(td)) );

   assign( tsv, loadLE(ty,mkexpr(ts)) );

   setFlags_DEP1_DEP2 ( Iop_Sub8, tsv, tdv, ty );

   incd = binop(Iop_Add64, mkexpr(td), mkexpr(t_inc));
   incs = binop(Iop_Add64, mkexpr(ts), mkexpr(t_inc));
   if (haveASO(pfx)) {
      incd = unop(Iop_32Uto64, unop(Iop_64to32, incd));
      incs = unop(Iop_32Uto64, unop(Iop_64to32, incs));
   }
   putIReg64( R_RDI, incd );
   putIReg64( R_RSI, incs );
}

static 
void dis_SCAS ( Int sz, IRTemp t_inc, Prefix pfx )
{
   IRType ty  = szToITy(sz);
   IRTemp ta  = newTemp(ty);       /*  rAX  */
   IRTemp td  = newTemp(Ity_I64);  /*  RDI  */
   IRTemp tdv = newTemp(ty);       /* (RDI) */
   IRExpr *incd;

   assign( ta, getIRegRAX(sz) );

   if (haveASO(pfx))
      assign( td, unop(Iop_32Uto64, getIReg32(R_RDI)) );
   else
      assign( td, getIReg64(R_RDI) );

   assign( tdv, loadLE(ty,mkexpr(td)) );

   setFlags_DEP1_DEP2 ( Iop_Sub8, ta, tdv, ty );

   incd = binop(Iop_Add64, mkexpr(td), mkexpr(t_inc));
   if (haveASO(pfx))
      incd = unop(Iop_32Uto64, unop(Iop_64to32, incd));
   putIReg64( R_RDI, incd );
}


/* Wrap the appropriate string op inside a REP/REPE/REPNE.  We assume
   the insn is the last one in the basic block, and so emit a jump to
   the next insn, rather than just falling through. */
static 
void dis_REP_op ( /*MOD*/DisResult* dres,
                  AMD64Condcode cond,
                  void (*dis_OP)(Int, IRTemp, Prefix),
                  Int sz, Addr64 rip, Addr64 rip_next, HChar* name,
                  Prefix pfx )
{
   IRTemp t_inc = newTemp(Ity_I64);
   IRTemp tc;
   IRExpr* cmp;

   /* Really we ought to inspect the override prefixes, but we don't.
      The following assertion catches any resulting sillyness. */
   vassert(pfx == clearSegBits(pfx));

   if (haveASO(pfx)) {
      tc = newTemp(Ity_I32);  /*  ECX  */
      assign( tc, getIReg32(R_RCX) );
      cmp = binop(Iop_CmpEQ32, mkexpr(tc), mkU32(0));
   } else {
      tc = newTemp(Ity_I64);  /*  RCX  */
      assign( tc, getIReg64(R_RCX) );
      cmp = binop(Iop_CmpEQ64, mkexpr(tc), mkU64(0));
   }

   stmt( IRStmt_Exit( cmp, Ijk_Boring,
                      IRConst_U64(rip_next), OFFB_RIP ) );

   if (haveASO(pfx))
      putIReg32(R_RCX, binop(Iop_Sub32, mkexpr(tc), mkU32(1)) );
  else
      putIReg64(R_RCX, binop(Iop_Sub64, mkexpr(tc), mkU64(1)) );

   dis_string_op_increment(sz, t_inc);
   dis_OP (sz, t_inc, pfx);

   if (cond == AMD64CondAlways) {
      jmp_lit(dres, Ijk_Boring, rip);
      vassert(dres->whatNext == Dis_StopHere);
   } else {
      stmt( IRStmt_Exit( mk_amd64g_calculate_condition(cond),
                         Ijk_Boring,
                         IRConst_U64(rip),
                         OFFB_RIP ) );
      jmp_lit(dres, Ijk_Boring, rip_next);
      vassert(dres->whatNext == Dis_StopHere);
   }
   DIP("%s%c\n", name, nameISize(sz));
}


/*------------------------------------------------------------*/
/*--- Arithmetic, etc.                                     ---*/
/*------------------------------------------------------------*/

/* IMUL E, G.  Supplied eip points to the modR/M byte. */
static
ULong dis_mul_E_G ( VexAbiInfo* vbi,
                    Prefix      pfx,
                    Int         size, 
                    Long        delta0 )
{
   Int    alen;
   HChar  dis_buf[50];
   UChar  rm = getUChar(delta0);
   IRType ty = szToITy(size);
   IRTemp te = newTemp(ty);
   IRTemp tg = newTemp(ty);
   IRTemp resLo = newTemp(ty);

   assign( tg, getIRegG(size, pfx, rm) );
   if (epartIsReg(rm)) {
      assign( te, getIRegE(size, pfx, rm) );
   } else {
      IRTemp addr = disAMode( &alen, vbi, pfx, delta0, dis_buf, 0 );
      assign( te, loadLE(ty,mkexpr(addr)) );
   }

   setFlags_MUL ( ty, te, tg, AMD64G_CC_OP_SMULB );

   assign( resLo, binop( mkSizedOp(ty, Iop_Mul8), mkexpr(te), mkexpr(tg) ) );

   putIRegG(size, pfx, rm, mkexpr(resLo) );

   if (epartIsReg(rm)) {
      DIP("imul%c %s, %s\n", nameISize(size), 
                             nameIRegE(size,pfx,rm),
                             nameIRegG(size,pfx,rm));
      return 1+delta0;
   } else {
      DIP("imul%c %s, %s\n", nameISize(size), 
                             dis_buf, 
                             nameIRegG(size,pfx,rm));
      return alen+delta0;
   }
}


/* IMUL I * E -> G.  Supplied rip points to the modR/M byte. */
static
ULong dis_imul_I_E_G ( VexAbiInfo* vbi,
                       Prefix      pfx,
                       Int         size, 
                       Long        delta,
                       Int         litsize )
{
   Long   d64;
   Int    alen;
   HChar  dis_buf[50];
   UChar  rm = getUChar(delta);
   IRType ty = szToITy(size);
   IRTemp te = newTemp(ty);
   IRTemp tl = newTemp(ty);
   IRTemp resLo = newTemp(ty);

   vassert(/*size == 1 ||*/ size == 2 || size == 4 || size == 8);

   if (epartIsReg(rm)) {
      assign(te, getIRegE(size, pfx, rm));
      delta++;
   } else {
      IRTemp addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                                     imin(4,litsize) );
      assign(te, loadLE(ty, mkexpr(addr)));
      delta += alen;
   }
   d64 = getSDisp(imin(4,litsize),delta);
   delta += imin(4,litsize);

   d64 &= mkSizeMask(size);
   assign(tl, mkU(ty,d64));

   assign( resLo, binop( mkSizedOp(ty, Iop_Mul8), mkexpr(te), mkexpr(tl) ));

   setFlags_MUL ( ty, te, tl, AMD64G_CC_OP_SMULB );

   putIRegG(size, pfx, rm, mkexpr(resLo));

   DIP("imul%c $%lld, %s, %s\n", 
       nameISize(size), d64, 
       ( epartIsReg(rm) ? nameIRegE(size,pfx,rm) : dis_buf ),
       nameIRegG(size,pfx,rm) );
   return delta;
}


/* Generate an IR sequence to do a popcount operation on the supplied
   IRTemp, and return a new IRTemp holding the result.  'ty' may be
   Ity_I16, Ity_I32 or Ity_I64 only. */
static IRTemp gen_POPCOUNT ( IRType ty, IRTemp src )
{
   Int i;
   if (ty == Ity_I16) {
      IRTemp old = IRTemp_INVALID;
      IRTemp nyu = IRTemp_INVALID;
      IRTemp mask[4], shift[4];
      for (i = 0; i < 4; i++) {
         mask[i]  = newTemp(ty);
         shift[i] = 1 << i;
      }
      assign(mask[0], mkU16(0x5555));
      assign(mask[1], mkU16(0x3333));
      assign(mask[2], mkU16(0x0F0F));
      assign(mask[3], mkU16(0x00FF));
      old = src;
      for (i = 0; i < 4; i++) {
         nyu = newTemp(ty);
         assign(nyu,
                binop(Iop_Add16, 
                      binop(Iop_And16,
                            mkexpr(old),
                            mkexpr(mask[i])),
                      binop(Iop_And16,
                            binop(Iop_Shr16, mkexpr(old), mkU8(shift[i])),
                            mkexpr(mask[i]))));
         old = nyu;
      }
      return nyu;
   }
   if (ty == Ity_I32) {
      IRTemp old = IRTemp_INVALID;
      IRTemp nyu = IRTemp_INVALID;
      IRTemp mask[5], shift[5];
      for (i = 0; i < 5; i++) {
         mask[i]  = newTemp(ty);
         shift[i] = 1 << i;
      }
      assign(mask[0], mkU32(0x55555555));
      assign(mask[1], mkU32(0x33333333));
      assign(mask[2], mkU32(0x0F0F0F0F));
      assign(mask[3], mkU32(0x00FF00FF));
      assign(mask[4], mkU32(0x0000FFFF));
      old = src;
      for (i = 0; i < 5; i++) {
         nyu = newTemp(ty);
         assign(nyu,
                binop(Iop_Add32, 
                      binop(Iop_And32,
                            mkexpr(old),
                            mkexpr(mask[i])),
                      binop(Iop_And32,
                            binop(Iop_Shr32, mkexpr(old), mkU8(shift[i])),
                            mkexpr(mask[i]))));
         old = nyu;
      }
      return nyu;
   }
   if (ty == Ity_I64) {
      IRTemp old = IRTemp_INVALID;
      IRTemp nyu = IRTemp_INVALID;
      IRTemp mask[6], shift[6];
      for (i = 0; i < 6; i++) {
         mask[i]  = newTemp(ty);
         shift[i] = 1 << i;
      }
      assign(mask[0], mkU64(0x5555555555555555ULL));
      assign(mask[1], mkU64(0x3333333333333333ULL));
      assign(mask[2], mkU64(0x0F0F0F0F0F0F0F0FULL));
      assign(mask[3], mkU64(0x00FF00FF00FF00FFULL));
      assign(mask[4], mkU64(0x0000FFFF0000FFFFULL));
      assign(mask[5], mkU64(0x00000000FFFFFFFFULL));
      old = src;
      for (i = 0; i < 6; i++) {
         nyu = newTemp(ty);
         assign(nyu,
                binop(Iop_Add64, 
                      binop(Iop_And64,
                            mkexpr(old),
                            mkexpr(mask[i])),
                      binop(Iop_And64,
                            binop(Iop_Shr64, mkexpr(old), mkU8(shift[i])),
                            mkexpr(mask[i]))));
         old = nyu;
      }
      return nyu;
   }
   /*NOTREACHED*/
   vassert(0);
}


/* Generate an IR sequence to do a count-leading-zeroes operation on
   the supplied IRTemp, and return a new IRTemp holding the result.
   'ty' may be Ity_I16, Ity_I32 or Ity_I64 only.  In the case where
   the argument is zero, return the number of bits in the word (the
   natural semantics). */
static IRTemp gen_LZCNT ( IRType ty, IRTemp src )
{
   vassert(ty == Ity_I64 || ty == Ity_I32 || ty == Ity_I16);

   IRTemp src64 = newTemp(Ity_I64);
   assign(src64, widenUto64( mkexpr(src) ));

   IRTemp src64x = newTemp(Ity_I64);
   assign(src64x, 
          binop(Iop_Shl64, mkexpr(src64),
                           mkU8(64 - 8 * sizeofIRType(ty))));

   // Clz64 has undefined semantics when its input is zero, so
   // special-case around that.
   IRTemp res64 = newTemp(Ity_I64);
   assign(res64,
          IRExpr_Mux0X(
             unop(Iop_1Uto8,
                  binop(Iop_CmpEQ64, mkexpr(src64x), mkU64(0))),
             unop(Iop_Clz64, mkexpr(src64x)),
             mkU64(8 * sizeofIRType(ty))
   ));

   IRTemp res = newTemp(ty);
   assign(res, narrowTo(ty, mkexpr(res64)));
   return res;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- x87 FLOATING POINT INSTRUCTIONS                      ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

/* --- Helper functions for dealing with the register stack. --- */

/* --- Set the emulation-warning pseudo-register. --- */

static void put_emwarn ( IRExpr* e /* :: Ity_I32 */ )
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
   stmt( IRStmt_Put( OFFB_EMWARN, e ) );
}

/* --- Produce an IRExpr* denoting a 64-bit QNaN. --- */

static IRExpr* mkQNaN64 ( void )
{
  /* QNaN is 0 2047 1 0(51times) 
     == 0b 11111111111b 1 0(51times)
     == 0x7FF8 0000 0000 0000
   */
   return IRExpr_Const(IRConst_F64i(0x7FF8000000000000ULL));
}

/* --------- Get/put the top-of-stack pointer :: Ity_I32 --------- */

static IRExpr* get_ftop ( void )
{
   return IRExpr_Get( OFFB_FTOP, Ity_I32 );
}

static void put_ftop ( IRExpr* e )
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
   stmt( IRStmt_Put( OFFB_FTOP, e ) );
}

/* --------- Get/put the C3210 bits. --------- */

static IRExpr*  /* :: Ity_I64 */ get_C3210 ( void )
{
   return IRExpr_Get( OFFB_FC3210, Ity_I64 );
}

static void put_C3210 ( IRExpr* e  /* :: Ity_I64 */ )
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I64);
   stmt( IRStmt_Put( OFFB_FC3210, e ) );
}

/* --------- Get/put the FPU rounding mode. --------- */
static IRExpr* /* :: Ity_I32 */ get_fpround ( void )
{
   return unop(Iop_64to32, IRExpr_Get( OFFB_FPROUND, Ity_I64 ));
}

static void put_fpround ( IRExpr* /* :: Ity_I32 */ e )
{
   vassert(typeOfIRExpr(irsb->tyenv, e) == Ity_I32);
   stmt( IRStmt_Put( OFFB_FPROUND, unop(Iop_32Uto64,e) ) );
}


/* --------- Synthesise a 2-bit FPU rounding mode. --------- */
/* Produces a value in 0 .. 3, which is encoded as per the type
   IRRoundingMode.  Since the guest_FPROUND value is also encoded as
   per IRRoundingMode, we merely need to get it and mask it for
   safety.
*/
static IRExpr* /* :: Ity_I32 */ get_roundingmode ( void )
{
   return binop( Iop_And32, get_fpround(), mkU32(3) );
}

static IRExpr* /* :: Ity_I32 */ get_FAKE_roundingmode ( void )
{
   return mkU32(Irrm_NEAREST);
}


/* --------- Get/set FP register tag bytes. --------- */

/* Given i, and some expression e, generate 'ST_TAG(i) = e'. */

static void put_ST_TAG ( Int i, IRExpr* value )
{
   IRRegArray* descr;
   vassert(typeOfIRExpr(irsb->tyenv, value) == Ity_I8);
   descr = mkIRRegArray( OFFB_FPTAGS, Ity_I8, 8 );
   stmt( IRStmt_PutI( mkIRPutI(descr, get_ftop(), i, value) ) );
}

/* Given i, generate an expression yielding 'ST_TAG(i)'.  This will be
   zero to indicate "Empty" and nonzero to indicate "NonEmpty".  */

static IRExpr* get_ST_TAG ( Int i )
{
   IRRegArray* descr = mkIRRegArray( OFFB_FPTAGS, Ity_I8, 8 );
   return IRExpr_GetI( descr, get_ftop(), i );
}


/* --------- Get/set FP registers. --------- */

/* Given i, and some expression e, emit 'ST(i) = e' and set the
   register's tag to indicate the register is full.  The previous
   state of the register is not checked. */

static void put_ST_UNCHECKED ( Int i, IRExpr* value )
{
   IRRegArray* descr;
   vassert(typeOfIRExpr(irsb->tyenv, value) == Ity_F64);
   descr = mkIRRegArray( OFFB_FPREGS, Ity_F64, 8 );
   stmt( IRStmt_PutI( mkIRPutI(descr, get_ftop(), i, value) ) );
   /* Mark the register as in-use. */
   put_ST_TAG(i, mkU8(1));
}

/* Given i, and some expression e, emit
      ST(i) = is_full(i) ? NaN : e
   and set the tag accordingly.
*/

static void put_ST ( Int i, IRExpr* value )
{
   put_ST_UNCHECKED( i,
                     IRExpr_Mux0X( get_ST_TAG(i),
                                   /* 0 means empty */
                                   value,
                                   /* non-0 means full */
                                   mkQNaN64()
                   )
   );
}


/* Given i, generate an expression yielding 'ST(i)'. */

static IRExpr* get_ST_UNCHECKED ( Int i )
{
   IRRegArray* descr = mkIRRegArray( OFFB_FPREGS, Ity_F64, 8 );
   return IRExpr_GetI( descr, get_ftop(), i );
}


/* Given i, generate an expression yielding 
  is_full(i) ? ST(i) : NaN
*/

static IRExpr* get_ST ( Int i )
{
   return
      IRExpr_Mux0X( get_ST_TAG(i),
                    /* 0 means empty */
                    mkQNaN64(),
                    /* non-0 means full */
                    get_ST_UNCHECKED(i));
}


/* Adjust FTOP downwards by one register. */

static void fp_push ( void )
{
   put_ftop( binop(Iop_Sub32, get_ftop(), mkU32(1)) );
}

/* Adjust FTOP upwards by one register, and mark the vacated register
   as empty.  */

static void fp_pop ( void )
{
   put_ST_TAG(0, mkU8(0));
   put_ftop( binop(Iop_Add32, get_ftop(), mkU32(1)) );
}

/* Clear the C2 bit of the FPU status register, for
   sin/cos/tan/sincos. */

static void clear_C2 ( void )
{
   put_C3210( binop(Iop_And64, get_C3210(), mkU64(~AMD64G_FC_MASK_C2)) );
}

/* Invent a plausible-looking FPU status word value:
      ((ftop & 7) << 11) | (c3210 & 0x4700)
 */
static IRExpr* get_FPU_sw ( void )
{
   return
      unop(Iop_32to16,
           binop(Iop_Or32,
                 binop(Iop_Shl32, 
                       binop(Iop_And32, get_ftop(), mkU32(7)), 
                             mkU8(11)),
                       binop(Iop_And32, unop(Iop_64to32, get_C3210()), 
                                        mkU32(0x4700))
      ));
}


/* ------------------------------------------------------- */
/* Given all that stack-mangling junk, we can now go ahead
   and describe FP instructions. 
*/

/* ST(0) = ST(0) `op` mem64/32(addr)
   Need to check ST(0)'s tag on read, but not on write.
*/
static
void fp_do_op_mem_ST_0 ( IRTemp addr, HChar* op_txt, HChar* dis_buf, 
                         IROp op, Bool dbl )
{
   DIP("f%s%c %s\n", op_txt, dbl?'l':'s', dis_buf);
   if (dbl) {
      put_ST_UNCHECKED(0, 
         triop( op, 
                get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                get_ST(0), 
                loadLE(Ity_F64,mkexpr(addr))
         ));
   } else {
      put_ST_UNCHECKED(0, 
         triop( op, 
                get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                get_ST(0), 
                unop(Iop_F32toF64, loadLE(Ity_F32,mkexpr(addr)))
         ));
   }
}


/* ST(0) = mem64/32(addr) `op` ST(0)
   Need to check ST(0)'s tag on read, but not on write.
*/
static
void fp_do_oprev_mem_ST_0 ( IRTemp addr, HChar* op_txt, HChar* dis_buf, 
                            IROp op, Bool dbl )
{
   DIP("f%s%c %s\n", op_txt, dbl?'l':'s', dis_buf);
   if (dbl) {
      put_ST_UNCHECKED(0, 
         triop( op, 
                get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                loadLE(Ity_F64,mkexpr(addr)),
                get_ST(0)
         ));
   } else {
      put_ST_UNCHECKED(0, 
         triop( op, 
                get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                unop(Iop_F32toF64, loadLE(Ity_F32,mkexpr(addr))),
                get_ST(0)
         ));
   }
}


/* ST(dst) = ST(dst) `op` ST(src).
   Check dst and src tags when reading but not on write.
*/
static
void fp_do_op_ST_ST ( HChar* op_txt, IROp op, UInt st_src, UInt st_dst,
                      Bool pop_after )
{
   DIP("f%s%s st(%u), st(%u)\n", op_txt, pop_after?"p":"", st_src, st_dst );
   put_ST_UNCHECKED( 
      st_dst, 
      triop( op, 
             get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
             get_ST(st_dst), 
             get_ST(st_src) ) 
   );
   if (pop_after)
      fp_pop();
}

/* ST(dst) = ST(src) `op` ST(dst).
   Check dst and src tags when reading but not on write.
*/
static
void fp_do_oprev_ST_ST ( HChar* op_txt, IROp op, UInt st_src, UInt st_dst,
                         Bool pop_after )
{
   DIP("f%s%s st(%u), st(%u)\n", op_txt, pop_after?"p":"", st_src, st_dst );
   put_ST_UNCHECKED( 
      st_dst, 
      triop( op, 
             get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
             get_ST(st_src), 
             get_ST(st_dst) ) 
   );
   if (pop_after)
      fp_pop();
}

/* %rflags(Z,P,C) = UCOMI( st(0), st(i) ) */
static void fp_do_ucomi_ST0_STi ( UInt i, Bool pop_after )
{
   DIP("fucomi%s %%st(0),%%st(%u)\n", pop_after ? "p" : "", i);
   /* This is a bit of a hack (and isn't really right).  It sets
      Z,P,C,O correctly, but forces A and S to zero, whereas the Intel
      documentation implies A and S are unchanged. 
   */
   /* It's also fishy in that it is used both for COMIP and
      UCOMIP, and they aren't the same (although similar). */
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            binop( Iop_And64,
                   unop( Iop_32Uto64,
                         binop(Iop_CmpF64, get_ST(0), get_ST(i))),
                   mkU64(0x45)
        )));
   if (pop_after)
      fp_pop();
}


/* returns 
   32to16( if e32 <s -32768 || e32 >s 32767 then -32768 else e32 )
*/
static IRExpr* x87ishly_qnarrow_32_to_16 ( IRExpr* e32 )
{
   IRTemp t32 = newTemp(Ity_I32);
   assign( t32, e32 );
   return
      IRExpr_Mux0X( 
         unop(Iop_1Uto8, 
              binop(Iop_CmpLT64U, 
                    unop(Iop_32Uto64, 
                         binop(Iop_Add32, mkexpr(t32), mkU32(32768))), 
                    mkU64(65536))),
         mkU16( 0x8000 ),
         unop(Iop_32to16, mkexpr(t32)));
}


static
ULong dis_FPU ( /*OUT*/Bool* decode_ok, 
                VexAbiInfo* vbi, Prefix pfx, Long delta )
{
   Int    len;
   UInt   r_src, r_dst;
   HChar  dis_buf[50];
   IRTemp t1, t2;

   /* On entry, delta points at the second byte of the insn (the modrm
      byte).*/
   UChar first_opcode = getUChar(delta-1);
   UChar modrm        = getUChar(delta+0);

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xD8 opcodes +-+-+-+-+-+-+-+ */

   if (first_opcode == 0xD8) {
      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
           specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FADD single-real */
               fp_do_op_mem_ST_0 ( addr, "add", dis_buf, Iop_AddF64, False );
               break;

            case 1: /* FMUL single-real */
               fp_do_op_mem_ST_0 ( addr, "mul", dis_buf, Iop_MulF64, False );
               break;

            case 2: /* FCOM single-real */
               DIP("fcoms %s\n", dis_buf);
               /* This forces C1 to zero, which isn't right. */
               /* The AMD documentation suggests that forcing C1 to
                  zero is correct (Eliot Moss) */
               put_C3210( 
                   unop( Iop_32Uto64,
                       binop( Iop_And32,
                              binop(Iop_Shl32, 
                                    binop(Iop_CmpF64, 
                                          get_ST(0),
                                          unop(Iop_F32toF64, 
                                               loadLE(Ity_F32,mkexpr(addr)))),
                                    mkU8(8)),
                              mkU32(0x4500)
                   )));
               break;  

            case 3: /* FCOMP single-real */
               /* The AMD documentation suggests that forcing C1 to
                  zero is correct (Eliot Moss) */
               DIP("fcomps %s\n", dis_buf);
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop( Iop_32Uto64,
                       binop( Iop_And32,
                              binop(Iop_Shl32, 
                                    binop(Iop_CmpF64, 
                                          get_ST(0),
                                          unop(Iop_F32toF64, 
                                               loadLE(Ity_F32,mkexpr(addr)))),
                                    mkU8(8)),
                              mkU32(0x4500)
                   )));
               fp_pop();
               break;  

            case 4: /* FSUB single-real */
               fp_do_op_mem_ST_0 ( addr, "sub", dis_buf, Iop_SubF64, False );
               break;

            case 5: /* FSUBR single-real */
               fp_do_oprev_mem_ST_0 ( addr, "subr", dis_buf, Iop_SubF64, False );
               break;

            case 6: /* FDIV single-real */
               fp_do_op_mem_ST_0 ( addr, "div", dis_buf, Iop_DivF64, False );
               break;

            case 7: /* FDIVR single-real */
               fp_do_oprev_mem_ST_0 ( addr, "divr", dis_buf, Iop_DivF64, False );
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xD8\n");
               goto decode_fail;
         }
      } else {
         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FADD %st(?),%st(0) */
               fp_do_op_ST_ST ( "add", Iop_AddF64, modrm - 0xC0, 0, False );
               break;

            case 0xC8 ... 0xCF: /* FMUL %st(?),%st(0) */
               fp_do_op_ST_ST ( "mul", Iop_MulF64, modrm - 0xC8, 0, False );
               break;

            /* Dunno if this is right */
            case 0xD0 ... 0xD7: /* FCOM %st(?),%st(0) */
               r_dst = (UInt)modrm - 0xD0;
               DIP("fcom %%st(0),%%st(%d)\n", r_dst);
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64,
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(r_dst)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               break;

            /* Dunno if this is right */
            case 0xD8 ... 0xDF: /* FCOMP %st(?),%st(0) */
               r_dst = (UInt)modrm - 0xD8;
               DIP("fcomp %%st(0),%%st(%d)\n", r_dst);
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64,
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(r_dst)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               fp_pop();
               break;

            case 0xE0 ... 0xE7: /* FSUB %st(?),%st(0) */
               fp_do_op_ST_ST ( "sub", Iop_SubF64, modrm - 0xE0, 0, False );
               break;

            case 0xE8 ... 0xEF: /* FSUBR %st(?),%st(0) */
               fp_do_oprev_ST_ST ( "subr", Iop_SubF64, modrm - 0xE8, 0, False );
               break;

            case 0xF0 ... 0xF7: /* FDIV %st(?),%st(0) */
               fp_do_op_ST_ST ( "div", Iop_DivF64, modrm - 0xF0, 0, False );
               break;

            case 0xF8 ... 0xFF: /* FDIVR %st(?),%st(0) */
               fp_do_oprev_ST_ST ( "divr", Iop_DivF64, modrm - 0xF8, 0, False );
               break;

            default:
               goto decode_fail;
         }
      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xD9 opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xD9) {
      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FLD single-real */
               DIP("flds %s\n", dis_buf);
               fp_push();
               put_ST(0, unop(Iop_F32toF64,
                              loadLE(Ity_F32, mkexpr(addr))));
               break;

            case 2: /* FST single-real */
               DIP("fsts %s\n", dis_buf);
               storeLE(mkexpr(addr),
                       binop(Iop_F64toF32, get_roundingmode(), get_ST(0)));
               break;

            case 3: /* FSTP single-real */
               DIP("fstps %s\n", dis_buf);
               storeLE(mkexpr(addr), 
                       binop(Iop_F64toF32, get_roundingmode(), get_ST(0)));
               fp_pop();
               break;

            case 4: { /* FLDENV m28 */
               /* Uses dirty helper: 
                     VexEmWarn amd64g_do_FLDENV ( VexGuestX86State*, HWord ) */
               IRTemp    ew = newTemp(Ity_I32);
               IRTemp   w64 = newTemp(Ity_I64);
               IRDirty*   d = unsafeIRDirty_0_N ( 
                                 0/*regparms*/, 
                                 "amd64g_dirtyhelper_FLDENV", 
                                 &amd64g_dirtyhelper_FLDENV,
                                 mkIRExprVec_1( mkexpr(addr) )
                              );
               d->needsBBP = True;
               d->tmp      = w64;
               /* declare we're reading memory */
               d->mFx   = Ifx_Read;
               d->mAddr = mkexpr(addr);
               d->mSize = 28;

               /* declare we're writing guest state */
               d->nFxState = 4;
               vex_bzero(&d->fxState, sizeof(d->fxState));

               d->fxState[0].fx     = Ifx_Write;
               d->fxState[0].offset = OFFB_FTOP;
               d->fxState[0].size   = sizeof(UInt);

               d->fxState[1].fx     = Ifx_Write;
               d->fxState[1].offset = OFFB_FPTAGS;
               d->fxState[1].size   = 8 * sizeof(UChar);

               d->fxState[2].fx     = Ifx_Write;
               d->fxState[2].offset = OFFB_FPROUND;
               d->fxState[2].size   = sizeof(ULong);

               d->fxState[3].fx     = Ifx_Write;
               d->fxState[3].offset = OFFB_FC3210;
               d->fxState[3].size   = sizeof(ULong);

               stmt( IRStmt_Dirty(d) );

               /* ew contains any emulation warning we may need to
                  issue.  If needed, side-exit to the next insn,
                  reporting the warning, so that Valgrind's dispatcher
                  sees the warning. */
	       assign(ew, unop(Iop_64to32,mkexpr(w64)) );
               put_emwarn( mkexpr(ew) );
               stmt( 
                  IRStmt_Exit(
                     binop(Iop_CmpNE32, mkexpr(ew), mkU32(0)),
                     Ijk_EmWarn,
                     IRConst_U64( guest_RIP_bbstart+delta ),
                     OFFB_RIP
                  )
               );

               DIP("fldenv %s\n", dis_buf);
               break;
            }

            case 5: {/* FLDCW */
               /* The only thing we observe in the control word is the
                  rounding mode.  Therefore, pass the 16-bit value
                  (x87 native-format control word) to a clean helper,
                  getting back a 64-bit value, the lower half of which
                  is the FPROUND value to store, and the upper half of
                  which is the emulation-warning token which may be
                  generated.
               */
               /* ULong amd64h_check_fldcw ( ULong ); */
               IRTemp t64 = newTemp(Ity_I64);
               IRTemp ew = newTemp(Ity_I32);
               DIP("fldcw %s\n", dis_buf);
               assign( t64, mkIRExprCCall(
                               Ity_I64, 0/*regparms*/, 
                               "amd64g_check_fldcw",
                               &amd64g_check_fldcw, 
                               mkIRExprVec_1( 
                                  unop( Iop_16Uto64, 
                                        loadLE(Ity_I16, mkexpr(addr)))
                               )
                            )
                     );

               put_fpround( unop(Iop_64to32, mkexpr(t64)) );
               assign( ew, unop(Iop_64HIto32, mkexpr(t64) ) );
               put_emwarn( mkexpr(ew) );
               /* Finally, if an emulation warning was reported,
                  side-exit to the next insn, reporting the warning,
                  so that Valgrind's dispatcher sees the warning. */
               stmt( 
                  IRStmt_Exit(
                     binop(Iop_CmpNE32, mkexpr(ew), mkU32(0)),
                     Ijk_EmWarn,
                     IRConst_U64( guest_RIP_bbstart+delta ),
                     OFFB_RIP
                  )
               );
               break;
            }

            case 6: { /* FNSTENV m28 */
               /* Uses dirty helper: 
                     void amd64g_do_FSTENV ( VexGuestAMD64State*, HWord ) */
               IRDirty* d = unsafeIRDirty_0_N ( 
                               0/*regparms*/, 
                               "amd64g_dirtyhelper_FSTENV", 
                               &amd64g_dirtyhelper_FSTENV,
                               mkIRExprVec_1( mkexpr(addr) )
                            );
               d->needsBBP = True;
               /* declare we're writing memory */
               d->mFx   = Ifx_Write;
               d->mAddr = mkexpr(addr);
               d->mSize = 28;

               /* declare we're reading guest state */
               d->nFxState = 4;
               vex_bzero(&d->fxState, sizeof(d->fxState));

               d->fxState[0].fx     = Ifx_Read;
               d->fxState[0].offset = OFFB_FTOP;
               d->fxState[0].size   = sizeof(UInt);

               d->fxState[1].fx     = Ifx_Read;
               d->fxState[1].offset = OFFB_FPTAGS;
               d->fxState[1].size   = 8 * sizeof(UChar);

               d->fxState[2].fx     = Ifx_Read;
               d->fxState[2].offset = OFFB_FPROUND;
               d->fxState[2].size   = sizeof(ULong);

               d->fxState[3].fx     = Ifx_Read;
               d->fxState[3].offset = OFFB_FC3210;
               d->fxState[3].size   = sizeof(ULong);

               stmt( IRStmt_Dirty(d) );

               DIP("fnstenv %s\n", dis_buf);
               break;
            }

            case 7: /* FNSTCW */
               /* Fake up a native x87 FPU control word.  The only
                  thing it depends on is FPROUND[1:0], so call a clean
                  helper to cook it up. */
               /* ULong amd64g_create_fpucw ( ULong fpround ) */
               DIP("fnstcw %s\n", dis_buf);
               storeLE(
                  mkexpr(addr), 
                  unop( Iop_64to16, 
                        mkIRExprCCall(
                           Ity_I64, 0/*regp*/,
                           "amd64g_create_fpucw", &amd64g_create_fpucw, 
                           mkIRExprVec_1( unop(Iop_32Uto64, get_fpround()) ) 
                        ) 
                  ) 
               );
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xD9\n");
               goto decode_fail;
         }

      } else {
         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FLD %st(?) */
               r_src = (UInt)modrm - 0xC0;
               DIP("fld %%st(%u)\n", r_src);
               t1 = newTemp(Ity_F64);
               assign(t1, get_ST(r_src));
               fp_push();
               put_ST(0, mkexpr(t1));
               break;

            case 0xC8 ... 0xCF: /* FXCH %st(?) */
               r_src = (UInt)modrm - 0xC8;
               DIP("fxch %%st(%u)\n", r_src);
               t1 = newTemp(Ity_F64);
               t2 = newTemp(Ity_F64);
               assign(t1, get_ST(0));
               assign(t2, get_ST(r_src));
               put_ST_UNCHECKED(0, mkexpr(t2));
               put_ST_UNCHECKED(r_src, mkexpr(t1));
               break;

            case 0xE0: /* FCHS */
               DIP("fchs\n");
               put_ST_UNCHECKED(0, unop(Iop_NegF64, get_ST(0)));
               break;

            case 0xE1: /* FABS */
               DIP("fabs\n");
               put_ST_UNCHECKED(0, unop(Iop_AbsF64, get_ST(0)));
               break;

            case 0xE5: { /* FXAM */
               /* This is an interesting one.  It examines %st(0),
                  regardless of whether the tag says it's empty or not.
                  Here, just pass both the tag (in our format) and the
                  value (as a double, actually a ULong) to a helper
                  function. */
               IRExpr** args
                  = mkIRExprVec_2( unop(Iop_8Uto64, get_ST_TAG(0)),
                                   unop(Iop_ReinterpF64asI64, 
                                        get_ST_UNCHECKED(0)) );
               put_C3210(mkIRExprCCall(
                            Ity_I64, 
                            0/*regparm*/, 
                            "amd64g_calculate_FXAM", &amd64g_calculate_FXAM,
                            args
                        ));
               DIP("fxam\n");
               break;
            }

            case 0xE8: /* FLD1 */
               DIP("fld1\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(1.0))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x3ff0000000000000ULL)));
               break;

            case 0xE9: /* FLDL2T */
               DIP("fldl2t\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(3.32192809488736234781))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x400a934f0979a371ULL)));
               break;

            case 0xEA: /* FLDL2E */
               DIP("fldl2e\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(1.44269504088896340739))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x3ff71547652b82feULL)));
               break;

            case 0xEB: /* FLDPI */
               DIP("fldpi\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(3.14159265358979323851))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x400921fb54442d18ULL)));
               break;

            case 0xEC: /* FLDLG2 */
               DIP("fldlg2\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(0.301029995663981143))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x3fd34413509f79ffULL)));
               break;

            case 0xED: /* FLDLN2 */
               DIP("fldln2\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(0.69314718055994530942))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x3fe62e42fefa39efULL)));
               break;

            case 0xEE: /* FLDZ */
               DIP("fldz\n");
               fp_push();
               /* put_ST(0, IRExpr_Const(IRConst_F64(0.0))); */
               put_ST(0, IRExpr_Const(IRConst_F64i(0x0000000000000000ULL)));
               break;

            case 0xF0: /* F2XM1 */
               DIP("f2xm1\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_2xm1F64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0)));
               break;

            case 0xF1: /* FYL2X */
               DIP("fyl2x\n");
               put_ST_UNCHECKED(1, 
                  triop(Iop_Yl2xF64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(1), 
                        get_ST(0)));
               fp_pop();
               break;

            case 0xF2: /* FPTAN */
               DIP("ftan\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_TanF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0)));
               fp_push();
               put_ST(0, IRExpr_Const(IRConst_F64(1.0)));
               clear_C2(); /* HACK */
               break;

            case 0xF3: /* FPATAN */
               DIP("fpatan\n");
               put_ST_UNCHECKED(1, 
                  triop(Iop_AtanF64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(1), 
                        get_ST(0)));
               fp_pop();
               break;

            case 0xF4: { /* FXTRACT */
               IRTemp argF = newTemp(Ity_F64);
               IRTemp sigF = newTemp(Ity_F64);
               IRTemp expF = newTemp(Ity_F64);
               IRTemp argI = newTemp(Ity_I64);
               IRTemp sigI = newTemp(Ity_I64);
               IRTemp expI = newTemp(Ity_I64);
               DIP("fxtract\n");
               assign( argF, get_ST(0) );
               assign( argI, unop(Iop_ReinterpF64asI64, mkexpr(argF)));
               assign( sigI, 
                       mkIRExprCCall(
                          Ity_I64, 0/*regparms*/, 
                          "x86amd64g_calculate_FXTRACT", 
                          &x86amd64g_calculate_FXTRACT, 
                          mkIRExprVec_2( mkexpr(argI), 
                                         mkIRExpr_HWord(0)/*sig*/ )) 
               );
               assign( expI, 
                       mkIRExprCCall(
                          Ity_I64, 0/*regparms*/, 
                          "x86amd64g_calculate_FXTRACT", 
                          &x86amd64g_calculate_FXTRACT, 
                          mkIRExprVec_2( mkexpr(argI), 
                                         mkIRExpr_HWord(1)/*exp*/ )) 
               );
               assign( sigF, unop(Iop_ReinterpI64asF64, mkexpr(sigI)) );
               assign( expF, unop(Iop_ReinterpI64asF64, mkexpr(expI)) );
               /* exponent */
               put_ST_UNCHECKED(0, mkexpr(expF) );
               fp_push();
               /* significand */
               put_ST(0, mkexpr(sigF) );
               break;
            }

            case 0xF5: { /* FPREM1 -- IEEE compliant */
               IRTemp a1 = newTemp(Ity_F64);
               IRTemp a2 = newTemp(Ity_F64);
               DIP("fprem1\n");
               /* Do FPREM1 twice, once to get the remainder, and once
                  to get the C3210 flag values. */
               assign( a1, get_ST(0) );
               assign( a2, get_ST(1) );
               put_ST_UNCHECKED(0,
                  triop(Iop_PRem1F64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1),
                        mkexpr(a2)));
               put_C3210(
                  unop(Iop_32Uto64,
                  triop(Iop_PRem1C3210F64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1),
                        mkexpr(a2)) ));
               break;
            }

            case 0xF7: /* FINCSTP */
               DIP("fincstp\n");
               put_ftop( binop(Iop_Add32, get_ftop(), mkU32(1)) );
               break;

            case 0xF8: { /* FPREM -- not IEEE compliant */
               IRTemp a1 = newTemp(Ity_F64);
               IRTemp a2 = newTemp(Ity_F64);
               DIP("fprem\n");
               /* Do FPREM twice, once to get the remainder, and once
                  to get the C3210 flag values. */
               assign( a1, get_ST(0) );
               assign( a2, get_ST(1) );
               put_ST_UNCHECKED(0,
                  triop(Iop_PRemF64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1),
                        mkexpr(a2)));
               put_C3210(
                  unop(Iop_32Uto64,
                  triop(Iop_PRemC3210F64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1),
                        mkexpr(a2)) ));
               break;
            }

            case 0xF9: /* FYL2XP1 */
               DIP("fyl2xp1\n");
               put_ST_UNCHECKED(1, 
                  triop(Iop_Yl2xp1F64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(1), 
                        get_ST(0)));
               fp_pop();
               break;

            case 0xFA: /* FSQRT */
               DIP("fsqrt\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_SqrtF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0)));
               break;

            case 0xFB: { /* FSINCOS */
               IRTemp a1 = newTemp(Ity_F64);
               assign( a1, get_ST(0) );
               DIP("fsincos\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_SinF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1)));
               fp_push();
               put_ST(0, 
                  binop(Iop_CosF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        mkexpr(a1)));
               clear_C2(); /* HACK */
               break;
            }

            case 0xFC: /* FRNDINT */
               DIP("frndint\n");
               put_ST_UNCHECKED(0,
                  binop(Iop_RoundF64toInt, get_roundingmode(), get_ST(0)) );
               break;

            case 0xFD: /* FSCALE */
               DIP("fscale\n");
               put_ST_UNCHECKED(0, 
                  triop(Iop_ScaleF64,
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0), 
                        get_ST(1)));
               break;

            case 0xFE: /* FSIN */
               DIP("fsin\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_SinF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0)));
               clear_C2(); /* HACK */
               break;

            case 0xFF: /* FCOS */
               DIP("fcos\n");
               put_ST_UNCHECKED(0, 
                  binop(Iop_CosF64, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0)));
               clear_C2(); /* HACK */
               break;

            default:
               goto decode_fail;
         }
      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDA opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDA) {

      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IROp   fop;
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;
         switch (gregLO3ofRM(modrm)) {

            case 0: /* FIADD m32int */ /* ST(0) += m32int */
               DIP("fiaddl %s\n", dis_buf);
               fop = Iop_AddF64;
               goto do_fop_m32;

            case 1: /* FIMUL m32int */ /* ST(0) *= m32int */
               DIP("fimull %s\n", dis_buf);
               fop = Iop_MulF64;
               goto do_fop_m32;

            case 4: /* FISUB m32int */ /* ST(0) -= m32int */
               DIP("fisubl %s\n", dis_buf);
               fop = Iop_SubF64;
               goto do_fop_m32;

            case 5: /* FISUBR m32int */ /* ST(0) = m32int - ST(0) */
               DIP("fisubrl %s\n", dis_buf);
               fop = Iop_SubF64;
               goto do_foprev_m32;

            case 6: /* FIDIV m32int */ /* ST(0) /= m32int */
               DIP("fisubl %s\n", dis_buf);
               fop = Iop_DivF64;
               goto do_fop_m32;

            case 7: /* FIDIVR m32int */ /* ST(0) = m32int / ST(0) */
               DIP("fidivrl %s\n", dis_buf);
               fop = Iop_DivF64;
               goto do_foprev_m32;

            do_fop_m32:
               put_ST_UNCHECKED(0, 
                  triop(fop, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0),
                        unop(Iop_I32StoF64,
                             loadLE(Ity_I32, mkexpr(addr)))));
               break;

            do_foprev_m32:
               put_ST_UNCHECKED(0, 
                  triop(fop, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        unop(Iop_I32StoF64,
                             loadLE(Ity_I32, mkexpr(addr))),
                        get_ST(0)));
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDA\n");
               goto decode_fail;
         }

      } else {

         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FCMOVB ST(i), ST(0) */
               r_src = (UInt)modrm - 0xC0;
               DIP("fcmovb %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(0, 
                                IRExpr_Mux0X( 
                                    unop(Iop_1Uto8,
                                         mk_amd64g_calculate_condition(AMD64CondB)), 
                                    get_ST(0), get_ST(r_src)) );
               break;

            case 0xC8 ... 0xCF: /* FCMOVE(Z) ST(i), ST(0) */
               r_src = (UInt)modrm - 0xC8;
               DIP("fcmovz %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(0, 
                                IRExpr_Mux0X( 
                                    unop(Iop_1Uto8,
                                         mk_amd64g_calculate_condition(AMD64CondZ)), 
                                    get_ST(0), get_ST(r_src)) );
               break;

            case 0xD0 ... 0xD7: /* FCMOVBE ST(i), ST(0) */
               r_src = (UInt)modrm - 0xD0;
               DIP("fcmovbe %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(0, 
                                IRExpr_Mux0X( 
                                    unop(Iop_1Uto8,
                                         mk_amd64g_calculate_condition(AMD64CondBE)), 
                                    get_ST(0), get_ST(r_src)) );
               break;

            case 0xD8 ... 0xDF: /* FCMOVU ST(i), ST(0) */
               r_src = (UInt)modrm - 0xD8;
               DIP("fcmovu %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(0, 
                                IRExpr_Mux0X( 
                                    unop(Iop_1Uto8,
                                         mk_amd64g_calculate_condition(AMD64CondP)), 
                                    get_ST(0), get_ST(r_src)) );
               break;

            case 0xE9: /* FUCOMPP %st(0),%st(1) */
               DIP("fucompp %%st(0),%%st(1)\n");
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64,
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(1)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               fp_pop();
               fp_pop();
               break;

            default:
               goto decode_fail;
         }

      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDB opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDB) {
      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FILD m32int */
               DIP("fildl %s\n", dis_buf);
               fp_push();
               put_ST(0, unop(Iop_I32StoF64,
                              loadLE(Ity_I32, mkexpr(addr))));
               break;

            case 1: /* FISTTPL m32 (SSE3) */
               DIP("fisttpl %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        binop(Iop_F64toI32S, mkU32(Irrm_ZERO), get_ST(0)) );
               fp_pop();
               break;

            case 2: /* FIST m32 */
               DIP("fistl %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        binop(Iop_F64toI32S, get_roundingmode(), get_ST(0)) );
               break;

            case 3: /* FISTP m32 */
               DIP("fistpl %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        binop(Iop_F64toI32S, get_roundingmode(), get_ST(0)) );
               fp_pop();
               break;

            case 5: { /* FLD extended-real */
               /* Uses dirty helper: 
                     ULong amd64g_loadF80le ( ULong )
                  addr holds the address.  First, do a dirty call to
                  get hold of the data. */
               IRTemp   val  = newTemp(Ity_I64);
               IRExpr** args = mkIRExprVec_1 ( mkexpr(addr) );

               IRDirty* d = unsafeIRDirty_1_N ( 
                               val, 
                               0/*regparms*/, 
                               "amd64g_dirtyhelper_loadF80le", 
                               &amd64g_dirtyhelper_loadF80le, 
                               args 
                            );
               /* declare that we're reading memory */
               d->mFx   = Ifx_Read;
               d->mAddr = mkexpr(addr);
               d->mSize = 10;

               /* execute the dirty call, dumping the result in val. */
               stmt( IRStmt_Dirty(d) );
               fp_push();
               put_ST(0, unop(Iop_ReinterpI64asF64, mkexpr(val)));

               DIP("fldt %s\n", dis_buf);
               break;
            }

            case 7: { /* FSTP extended-real */
               /* Uses dirty helper: 
                     void amd64g_storeF80le ( ULong addr, ULong data ) 
               */
               IRExpr** args 
                  = mkIRExprVec_2( mkexpr(addr), 
                                   unop(Iop_ReinterpF64asI64, get_ST(0)) );

               IRDirty* d = unsafeIRDirty_0_N ( 
                               0/*regparms*/, 
                               "amd64g_dirtyhelper_storeF80le", 
                               &amd64g_dirtyhelper_storeF80le,
                               args 
                            );
               /* declare we're writing memory */
               d->mFx   = Ifx_Write;
               d->mAddr = mkexpr(addr);
               d->mSize = 10;

               /* execute the dirty call. */
               stmt( IRStmt_Dirty(d) );
               fp_pop();

               DIP("fstpt\n %s", dis_buf);
               break;
            }

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDB\n");
               goto decode_fail;
         }

      } else {

         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FCMOVNB ST(i), ST(0) */
               r_src = (UInt)modrm - 0xC0;
               DIP("fcmovnb %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(0, 
                                IRExpr_Mux0X( 
                                    unop(Iop_1Uto8,
                                         mk_amd64g_calculate_condition(AMD64CondNB)), 
                                    get_ST(0), get_ST(r_src)) );
               break;

            case 0xC8 ... 0xCF: /* FCMOVNE(NZ) ST(i), ST(0) */
               r_src = (UInt)modrm - 0xC8;
               DIP("fcmovnz %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(
                  0, 
                  IRExpr_Mux0X( 
                     unop(Iop_1Uto8,
                          mk_amd64g_calculate_condition(AMD64CondNZ)), 
                     get_ST(0), 
                     get_ST(r_src)
                  )
               );
               break;

            case 0xD0 ... 0xD7: /* FCMOVNBE ST(i), ST(0) */
               r_src = (UInt)modrm - 0xD0;
               DIP("fcmovnbe %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(
                  0, 
                  IRExpr_Mux0X( 
                     unop(Iop_1Uto8,
                          mk_amd64g_calculate_condition(AMD64CondNBE)), 
                     get_ST(0), 
                     get_ST(r_src)
                  ) 
               );
               break;

            case 0xD8 ... 0xDF: /* FCMOVNU ST(i), ST(0) */
               r_src = (UInt)modrm - 0xD8;
               DIP("fcmovnu %%st(%u), %%st(0)\n", r_src);
               put_ST_UNCHECKED(
                  0, 
                  IRExpr_Mux0X( 
                     unop(Iop_1Uto8,
                          mk_amd64g_calculate_condition(AMD64CondNP)), 
                     get_ST(0), 
                     get_ST(r_src)
                  )
               );
               break;

            case 0xE2:
               DIP("fnclex\n");
               break;

            case 0xE3: {
               /* Uses dirty helper: 
                     void amd64g_do_FINIT ( VexGuestAMD64State* ) */
               IRDirty* d  = unsafeIRDirty_0_N ( 
                                0/*regparms*/, 
                                "amd64g_dirtyhelper_FINIT", 
                                &amd64g_dirtyhelper_FINIT,
                                mkIRExprVec_0()
                             );
               d->needsBBP = True;

               /* declare we're writing guest state */
               d->nFxState = 5;
               vex_bzero(&d->fxState, sizeof(d->fxState));

               d->fxState[0].fx     = Ifx_Write;
               d->fxState[0].offset = OFFB_FTOP;
               d->fxState[0].size   = sizeof(UInt);

               d->fxState[1].fx     = Ifx_Write;
               d->fxState[1].offset = OFFB_FPREGS;
               d->fxState[1].size   = 8 * sizeof(ULong);

               d->fxState[2].fx     = Ifx_Write;
               d->fxState[2].offset = OFFB_FPTAGS;
               d->fxState[2].size   = 8 * sizeof(UChar);

               d->fxState[3].fx     = Ifx_Write;
               d->fxState[3].offset = OFFB_FPROUND;
               d->fxState[3].size   = sizeof(ULong);

               d->fxState[4].fx     = Ifx_Write;
               d->fxState[4].offset = OFFB_FC3210;
               d->fxState[4].size   = sizeof(ULong);

               stmt( IRStmt_Dirty(d) );

               DIP("fninit\n");
               break;
            }

            case 0xE8 ... 0xEF: /* FUCOMI %st(0),%st(?) */
               fp_do_ucomi_ST0_STi( (UInt)modrm - 0xE8, False );
               break;

            case 0xF0 ... 0xF7: /* FCOMI %st(0),%st(?) */
               fp_do_ucomi_ST0_STi( (UInt)modrm - 0xF0, False );
               break;

            default:
               goto decode_fail;
         }
      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDC opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDC) {
      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FADD double-real */
               fp_do_op_mem_ST_0 ( addr, "add", dis_buf, Iop_AddF64, True );
               break;

            case 1: /* FMUL double-real */
               fp_do_op_mem_ST_0 ( addr, "mul", dis_buf, Iop_MulF64, True );
               break;

//..             case 2: /* FCOM double-real */
//..                DIP("fcoml %s\n", dis_buf);
//..                /* This forces C1 to zero, which isn't right. */
//..                put_C3210( 
//..                    binop( Iop_And32,
//..                           binop(Iop_Shl32, 
//..                                 binop(Iop_CmpF64, 
//..                                       get_ST(0),
//..                                       loadLE(Ity_F64,mkexpr(addr))),
//..                                 mkU8(8)),
//..                           mkU32(0x4500)
//..                    ));
//..                break;  

            case 3: /* FCOMP double-real */
               DIP("fcompl %s\n", dis_buf);
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64,
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, 
                                      get_ST(0),
                                      loadLE(Ity_F64,mkexpr(addr))),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               fp_pop();
               break;  

            case 4: /* FSUB double-real */
               fp_do_op_mem_ST_0 ( addr, "sub", dis_buf, Iop_SubF64, True );
               break;

            case 5: /* FSUBR double-real */
               fp_do_oprev_mem_ST_0 ( addr, "subr", dis_buf, Iop_SubF64, True );
               break;

            case 6: /* FDIV double-real */
               fp_do_op_mem_ST_0 ( addr, "div", dis_buf, Iop_DivF64, True );
               break;

            case 7: /* FDIVR double-real */
               fp_do_oprev_mem_ST_0 ( addr, "divr", dis_buf, Iop_DivF64, True );
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDC\n");
               goto decode_fail;
         }

      } else {

         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FADD %st(0),%st(?) */
               fp_do_op_ST_ST ( "add", Iop_AddF64, 0, modrm - 0xC0, False );
               break;

            case 0xC8 ... 0xCF: /* FMUL %st(0),%st(?) */
               fp_do_op_ST_ST ( "mul", Iop_MulF64, 0, modrm - 0xC8, False );
               break;

            case 0xE0 ... 0xE7: /* FSUBR %st(0),%st(?) */
               fp_do_oprev_ST_ST ( "subr", Iop_SubF64, 0, modrm - 0xE0, False );
               break;

            case 0xE8 ... 0xEF: /* FSUB %st(0),%st(?) */
               fp_do_op_ST_ST ( "sub", Iop_SubF64, 0, modrm - 0xE8, False );
               break;

            case 0xF0 ... 0xF7: /* FDIVR %st(0),%st(?) */
               fp_do_oprev_ST_ST ( "divr", Iop_DivF64, 0, modrm - 0xF0, False );
               break;

            case 0xF8 ... 0xFF: /* FDIV %st(0),%st(?) */
               fp_do_op_ST_ST ( "div", Iop_DivF64, 0, modrm - 0xF8, False );
               break;

            default:
               goto decode_fail;
         }

      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDD opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDD) {

      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FLD double-real */
               DIP("fldl %s\n", dis_buf);
               fp_push();
               put_ST(0, loadLE(Ity_F64, mkexpr(addr)));
               break;

            case 1: /* FISTTPQ m64 (SSE3) */
               DIP("fistppll %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        binop(Iop_F64toI64S, mkU32(Irrm_ZERO), get_ST(0)) );
               fp_pop();
               break;

            case 2: /* FST double-real */
               DIP("fstl %s\n", dis_buf);
               storeLE(mkexpr(addr), get_ST(0));
               break;

            case 3: /* FSTP double-real */
               DIP("fstpl %s\n", dis_buf);
               storeLE(mkexpr(addr), get_ST(0));
               fp_pop();
               break;

            case 4: { /* FRSTOR m94/m108 */
               IRTemp   ew = newTemp(Ity_I32);
               IRTemp  w64 = newTemp(Ity_I64);
               IRDirty*  d;
               if ( have66(pfx) ) {
                  /* Uses dirty helper: 
                     VexEmWarn amd64g_dirtyhelper_FRSTORS
                                  ( VexGuestAMD64State*, HWord ) */
                  d = unsafeIRDirty_0_N ( 
                         0/*regparms*/, 
                         "amd64g_dirtyhelper_FRSTORS",
                         &amd64g_dirtyhelper_FRSTORS,
                         mkIRExprVec_1( mkexpr(addr) )
                      );
                  d->mSize = 94;
               } else {
                  /* Uses dirty helper: 
                     VexEmWarn amd64g_dirtyhelper_FRSTOR 
                                  ( VexGuestAMD64State*, HWord ) */
                  d = unsafeIRDirty_0_N ( 
                         0/*regparms*/, 
                         "amd64g_dirtyhelper_FRSTOR",
                         &amd64g_dirtyhelper_FRSTOR,
                         mkIRExprVec_1( mkexpr(addr) )
                      );
                  d->mSize = 108;
               }

               d->needsBBP = True;
               d->tmp      = w64;
               /* declare we're reading memory */
               d->mFx   = Ifx_Read;
               d->mAddr = mkexpr(addr);
               /* d->mSize set above */

               /* declare we're writing guest state */
               d->nFxState = 5;
               vex_bzero(&d->fxState, sizeof(d->fxState));

               d->fxState[0].fx     = Ifx_Write;
               d->fxState[0].offset = OFFB_FTOP;
               d->fxState[0].size   = sizeof(UInt);

               d->fxState[1].fx     = Ifx_Write;
               d->fxState[1].offset = OFFB_FPREGS;
               d->fxState[1].size   = 8 * sizeof(ULong);

               d->fxState[2].fx     = Ifx_Write;
               d->fxState[2].offset = OFFB_FPTAGS;
               d->fxState[2].size   = 8 * sizeof(UChar);

               d->fxState[3].fx     = Ifx_Write;
               d->fxState[3].offset = OFFB_FPROUND;
               d->fxState[3].size   = sizeof(ULong);

               d->fxState[4].fx     = Ifx_Write;
               d->fxState[4].offset = OFFB_FC3210;
               d->fxState[4].size   = sizeof(ULong);

               stmt( IRStmt_Dirty(d) );

               /* ew contains any emulation warning we may need to
                  issue.  If needed, side-exit to the next insn,
                  reporting the warning, so that Valgrind's dispatcher
                  sees the warning. */
               assign(ew, unop(Iop_64to32,mkexpr(w64)) );
               put_emwarn( mkexpr(ew) );
               stmt( 
                  IRStmt_Exit(
                     binop(Iop_CmpNE32, mkexpr(ew), mkU32(0)),
                     Ijk_EmWarn,
                     IRConst_U64( guest_RIP_bbstart+delta ),
                     OFFB_RIP
                  )
               );

               if ( have66(pfx) ) {
                  DIP("frstors %s\n", dis_buf);
               } else {
                  DIP("frstor %s\n", dis_buf);
               }
               break;
            }

            case 6: { /* FNSAVE m94/m108 */
               IRDirty *d;
               if ( have66(pfx) ) {
                 /* Uses dirty helper: 
                    void amd64g_dirtyhelper_FNSAVES ( VexGuestX86State*, HWord ) */
                  d = unsafeIRDirty_0_N ( 
                         0/*regparms*/, 
                         "amd64g_dirtyhelper_FNSAVES", 
                         &amd64g_dirtyhelper_FNSAVES,
                         mkIRExprVec_1( mkexpr(addr) )
                         );
                  d->mSize = 94;
               } else {
                 /* Uses dirty helper: 
                    void amd64g_dirtyhelper_FNSAVE ( VexGuestX86State*, HWord ) */
                  d = unsafeIRDirty_0_N ( 
                         0/*regparms*/, 
                         "amd64g_dirtyhelper_FNSAVE",
                         &amd64g_dirtyhelper_FNSAVE,
                         mkIRExprVec_1( mkexpr(addr) )
                         );
                  d->mSize = 108;
               }
               d->needsBBP = True;
               /* declare we're writing memory */
               d->mFx   = Ifx_Write;
               d->mAddr = mkexpr(addr);
               /* d->mSize set above */

               /* declare we're reading guest state */
               d->nFxState = 5;
               vex_bzero(&d->fxState, sizeof(d->fxState));

               d->fxState[0].fx     = Ifx_Read;
               d->fxState[0].offset = OFFB_FTOP;
               d->fxState[0].size   = sizeof(UInt);

               d->fxState[1].fx     = Ifx_Read;
               d->fxState[1].offset = OFFB_FPREGS;
               d->fxState[1].size   = 8 * sizeof(ULong);

               d->fxState[2].fx     = Ifx_Read;
               d->fxState[2].offset = OFFB_FPTAGS;
               d->fxState[2].size   = 8 * sizeof(UChar);

               d->fxState[3].fx     = Ifx_Read;
               d->fxState[3].offset = OFFB_FPROUND;
               d->fxState[3].size   = sizeof(ULong);

               d->fxState[4].fx     = Ifx_Read;
               d->fxState[4].offset = OFFB_FC3210;
               d->fxState[4].size   = sizeof(ULong);

               stmt( IRStmt_Dirty(d) );

               if ( have66(pfx) ) {
                 DIP("fnsaves %s\n", dis_buf);
               } else {
                 DIP("fnsave %s\n", dis_buf);
               }
               break;
            }

            case 7: { /* FNSTSW m16 */
               IRExpr* sw = get_FPU_sw();
               vassert(typeOfIRExpr(irsb->tyenv, sw) == Ity_I16);
               storeLE( mkexpr(addr), sw );
               DIP("fnstsw %s\n", dis_buf);
               break;
            }

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDD\n");
               goto decode_fail;
         }
      } else {
         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FFREE %st(?) */
               r_dst = (UInt)modrm - 0xC0;
               DIP("ffree %%st(%u)\n", r_dst);
               put_ST_TAG ( r_dst, mkU8(0) );
               break;

            case 0xD0 ... 0xD7: /* FST %st(0),%st(?) */
               r_dst = (UInt)modrm - 0xD0;
               DIP("fst %%st(0),%%st(%u)\n", r_dst);
               /* P4 manual says: "If the destination operand is a
                  non-empty register, the invalid-operation exception
                  is not generated.  Hence put_ST_UNCHECKED. */
               put_ST_UNCHECKED(r_dst, get_ST(0));
               break;

            case 0xD8 ... 0xDF: /* FSTP %st(0),%st(?) */
               r_dst = (UInt)modrm - 0xD8;
               DIP("fstp %%st(0),%%st(%u)\n", r_dst);
               /* P4 manual says: "If the destination operand is a
                  non-empty register, the invalid-operation exception
                  is not generated.  Hence put_ST_UNCHECKED. */
               put_ST_UNCHECKED(r_dst, get_ST(0));
               fp_pop();
               break;

            case 0xE0 ... 0xE7: /* FUCOM %st(0),%st(?) */
               r_dst = (UInt)modrm - 0xE0;
               DIP("fucom %%st(0),%%st(%u)\n", r_dst);
               /* This forces C1 to zero, which isn't right. */
               put_C3210(
                   unop(Iop_32Uto64, 
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(r_dst)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               break;

            case 0xE8 ... 0xEF: /* FUCOMP %st(0),%st(?) */
               r_dst = (UInt)modrm - 0xE8;
               DIP("fucomp %%st(0),%%st(%u)\n", r_dst);
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64, 
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(r_dst)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               fp_pop();
               break;

            default:
               goto decode_fail;
         }
      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDE opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDE) {

      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IROp   fop;
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FIADD m16int */ /* ST(0) += m16int */
               DIP("fiaddw %s\n", dis_buf);
               fop = Iop_AddF64;
               goto do_fop_m16;

            case 1: /* FIMUL m16int */ /* ST(0) *= m16int */
               DIP("fimulw %s\n", dis_buf);
               fop = Iop_MulF64;
               goto do_fop_m16;

            case 4: /* FISUB m16int */ /* ST(0) -= m16int */
               DIP("fisubw %s\n", dis_buf);
               fop = Iop_SubF64;
               goto do_fop_m16;

            case 5: /* FISUBR m16int */ /* ST(0) = m16int - ST(0) */
               DIP("fisubrw %s\n", dis_buf);
               fop = Iop_SubF64;
               goto do_foprev_m16;

            case 6: /* FIDIV m16int */ /* ST(0) /= m16int */
               DIP("fisubw %s\n", dis_buf);
               fop = Iop_DivF64;
               goto do_fop_m16;

            case 7: /* FIDIVR m16int */ /* ST(0) = m16int / ST(0) */
               DIP("fidivrw %s\n", dis_buf);
               fop = Iop_DivF64;
               goto do_foprev_m16;

            do_fop_m16:
               put_ST_UNCHECKED(0, 
                  triop(fop, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        get_ST(0),
                        unop(Iop_I32StoF64,
                             unop(Iop_16Sto32, 
                                  loadLE(Ity_I16, mkexpr(addr))))));
               break;

            do_foprev_m16:
               put_ST_UNCHECKED(0, 
                  triop(fop, 
                        get_FAKE_roundingmode(), /* XXXROUNDINGFIXME */
                        unop(Iop_I32StoF64,
                             unop(Iop_16Sto32, 
                                  loadLE(Ity_I16, mkexpr(addr)))),
                        get_ST(0)));
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDE\n");
               goto decode_fail;
         }

      } else {

         delta++;
         switch (modrm) {

            case 0xC0 ... 0xC7: /* FADDP %st(0),%st(?) */
               fp_do_op_ST_ST ( "add", Iop_AddF64, 0, modrm - 0xC0, True );
               break;

            case 0xC8 ... 0xCF: /* FMULP %st(0),%st(?) */
               fp_do_op_ST_ST ( "mul", Iop_MulF64, 0, modrm - 0xC8, True );
               break;

            case 0xD9: /* FCOMPP %st(0),%st(1) */
               DIP("fcompp %%st(0),%%st(1)\n");
               /* This forces C1 to zero, which isn't right. */
               put_C3210( 
                   unop(Iop_32Uto64,
                   binop( Iop_And32,
                          binop(Iop_Shl32, 
                                binop(Iop_CmpF64, get_ST(0), get_ST(1)),
                                mkU8(8)),
                          mkU32(0x4500)
                   )));
               fp_pop();
               fp_pop();
               break;

            case 0xE0 ... 0xE7: /* FSUBRP %st(0),%st(?) */
               fp_do_oprev_ST_ST ( "subr", Iop_SubF64, 0,  modrm - 0xE0, True );
               break;

            case 0xE8 ... 0xEF: /* FSUBP %st(0),%st(?) */
               fp_do_op_ST_ST ( "sub", Iop_SubF64, 0,  modrm - 0xE8, True );
               break;

            case 0xF0 ... 0xF7: /* FDIVRP %st(0),%st(?) */
               fp_do_oprev_ST_ST ( "divr", Iop_DivF64, 0, modrm - 0xF0, True );
               break;

            case 0xF8 ... 0xFF: /* FDIVP %st(0),%st(?) */
               fp_do_op_ST_ST ( "div", Iop_DivF64, 0, modrm - 0xF8, True );
               break;

            default: 
               goto decode_fail;
         }

      }
   }

   /* -+-+-+-+-+-+-+-+-+-+-+-+ 0xDF opcodes +-+-+-+-+-+-+-+ */
   else
   if (first_opcode == 0xDF) {

      if (modrm < 0xC0) {

         /* bits 5,4,3 are an opcode extension, and the modRM also
            specifies an address. */
         IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
         delta += len;

         switch (gregLO3ofRM(modrm)) {

            case 0: /* FILD m16int */
               DIP("fildw %s\n", dis_buf);
               fp_push();
               put_ST(0, unop(Iop_I32StoF64,
                              unop(Iop_16Sto32,
                                   loadLE(Ity_I16, mkexpr(addr)))));
               break;

            case 1: /* FISTTPS m16 (SSE3) */
               DIP("fisttps %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        x87ishly_qnarrow_32_to_16( 
                        binop(Iop_F64toI32S, mkU32(Irrm_ZERO), get_ST(0)) ));
               fp_pop();
               break;

            case 2: /* FIST m16 */
               DIP("fists %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        x87ishly_qnarrow_32_to_16(
                        binop(Iop_F64toI32S, get_roundingmode(), get_ST(0)) ));
               break;

            case 3: /* FISTP m16 */
               DIP("fistps %s\n", dis_buf);
               storeLE( mkexpr(addr),
                        x87ishly_qnarrow_32_to_16( 
                        binop(Iop_F64toI32S, get_roundingmode(), get_ST(0)) ));
               fp_pop();
               break;

            case 5: /* FILD m64 */
               DIP("fildll %s\n", dis_buf);
               fp_push();
               put_ST(0, binop(Iop_I64StoF64,
                               get_roundingmode(),
                               loadLE(Ity_I64, mkexpr(addr))));
               break;

            case 7: /* FISTP m64 */
               DIP("fistpll %s\n", dis_buf);
               storeLE( mkexpr(addr), 
                        binop(Iop_F64toI64S, get_roundingmode(), get_ST(0)) );
               fp_pop();
               break;

            default:
               vex_printf("unhandled opc_aux = 0x%2x\n", gregLO3ofRM(modrm));
               vex_printf("first_opcode == 0xDF\n");
               goto decode_fail;
         }

      } else {

         delta++;
         switch (modrm) {

            case 0xC0: /* FFREEP %st(0) */
               DIP("ffreep %%st(%d)\n", 0);
               put_ST_TAG ( 0, mkU8(0) );
               fp_pop();
               break;

            case 0xE0: /* FNSTSW %ax */
               DIP("fnstsw %%ax\n");
               /* Invent a plausible-looking FPU status word value and
                  dump it in %AX:
                     ((ftop & 7) << 11) | (c3210 & 0x4700)
               */
               putIRegRAX(
                  2,
                  unop(Iop_32to16,
                       binop(Iop_Or32,
                             binop(Iop_Shl32, 
                                   binop(Iop_And32, get_ftop(), mkU32(7)), 
                                   mkU8(11)),
                             binop(Iop_And32, 
                                   unop(Iop_64to32, get_C3210()), 
                                   mkU32(0x4700))
               )));
               break;

            case 0xE8 ... 0xEF: /* FUCOMIP %st(0),%st(?) */
               fp_do_ucomi_ST0_STi( (UInt)modrm - 0xE8, True );
               break;

            case 0xF0 ... 0xF7: /* FCOMIP %st(0),%st(?) */
               /* not really right since COMIP != UCOMIP */
               fp_do_ucomi_ST0_STi( (UInt)modrm - 0xF0, True );
               break;

            default: 
               goto decode_fail;
         }
      }

   }

   else
      goto decode_fail;

   *decode_ok = True;
   return delta;

  decode_fail:
   *decode_ok = False;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- MMX INSTRUCTIONS                                     ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

/* Effect of MMX insns on x87 FPU state (table 11-2 of 
   IA32 arch manual, volume 3):

   Read from, or write to MMX register (viz, any insn except EMMS):
   * All tags set to Valid (non-empty) -- FPTAGS[i] := nonzero
   * FP stack pointer set to zero

   EMMS:
   * All tags set to Invalid (empty) -- FPTAGS[i] := zero
   * FP stack pointer set to zero
*/

static void do_MMX_preamble ( void )
{
   Int         i;
   IRRegArray* descr = mkIRRegArray( OFFB_FPTAGS, Ity_I8, 8 );
   IRExpr*     zero  = mkU32(0);
   IRExpr*     tag1  = mkU8(1);
   put_ftop(zero);
   for (i = 0; i < 8; i++)
      stmt( IRStmt_PutI( mkIRPutI(descr, zero, i, tag1) ) );
}

static void do_EMMS_preamble ( void )
{
   Int         i;
   IRRegArray* descr = mkIRRegArray( OFFB_FPTAGS, Ity_I8, 8 );
   IRExpr*     zero  = mkU32(0);
   IRExpr*     tag0  = mkU8(0);
   put_ftop(zero);
   for (i = 0; i < 8; i++)
      stmt( IRStmt_PutI( mkIRPutI(descr, zero, i, tag0) ) );
}


static IRExpr* getMMXReg ( UInt archreg )
{
   vassert(archreg < 8);
   return IRExpr_Get( OFFB_FPREGS + 8 * archreg, Ity_I64 );
}


static void putMMXReg ( UInt archreg, IRExpr* e )
{
   vassert(archreg < 8);
   vassert(typeOfIRExpr(irsb->tyenv,e) == Ity_I64);
   stmt( IRStmt_Put( OFFB_FPREGS + 8 * archreg, e ) );
}


/* Helper for non-shift MMX insns.  Note this is incomplete in the
   sense that it does not first call do_MMX_preamble() -- that is the
   responsibility of its caller. */

static 
ULong dis_MMXop_regmem_to_reg ( VexAbiInfo* vbi,
                                Prefix      pfx,
                                Long        delta,
                                UChar       opc,
                                HChar*      name,
                                Bool        show_granularity )
{
   HChar   dis_buf[50];
   UChar   modrm = getUChar(delta);
   Bool    isReg = epartIsReg(modrm);
   IRExpr* argL  = NULL;
   IRExpr* argR  = NULL;
   IRExpr* argG  = NULL;
   IRExpr* argE  = NULL;
   IRTemp  res   = newTemp(Ity_I64);

   Bool    invG  = False;
   IROp    op    = Iop_INVALID;
   void*   hAddr = NULL;
   HChar*  hName = NULL;
   Bool    eLeft = False;

#  define XXX(_name) do { hAddr = &_name; hName = #_name; } while (0)

   switch (opc) {
      /* Original MMX ones */
      case 0xFC: op = Iop_Add8x8; break;
      case 0xFD: op = Iop_Add16x4; break;
      case 0xFE: op = Iop_Add32x2; break;

      case 0xEC: op = Iop_QAdd8Sx8; break;
      case 0xED: op = Iop_QAdd16Sx4; break;

      case 0xDC: op = Iop_QAdd8Ux8; break;
      case 0xDD: op = Iop_QAdd16Ux4; break;

      case 0xF8: op = Iop_Sub8x8;  break;
      case 0xF9: op = Iop_Sub16x4; break;
      case 0xFA: op = Iop_Sub32x2; break;

      case 0xE8: op = Iop_QSub8Sx8; break;
      case 0xE9: op = Iop_QSub16Sx4; break;

      case 0xD8: op = Iop_QSub8Ux8; break;
      case 0xD9: op = Iop_QSub16Ux4; break;

      case 0xE5: op = Iop_MulHi16Sx4; break;
      case 0xD5: op = Iop_Mul16x4; break;
      case 0xF5: XXX(amd64g_calculate_mmx_pmaddwd); break;

      case 0x74: op = Iop_CmpEQ8x8; break;
      case 0x75: op = Iop_CmpEQ16x4; break;
      case 0x76: op = Iop_CmpEQ32x2; break;

      case 0x64: op = Iop_CmpGT8Sx8; break;
      case 0x65: op = Iop_CmpGT16Sx4; break;
      case 0x66: op = Iop_CmpGT32Sx2; break;

      case 0x6B: op = Iop_QNarrowBin32Sto16Sx4; eLeft = True; break;
      case 0x63: op = Iop_QNarrowBin16Sto8Sx8;  eLeft = True; break;
      case 0x67: op = Iop_QNarrowBin16Sto8Ux8;  eLeft = True; break;

      case 0x68: op = Iop_InterleaveHI8x8;  eLeft = True; break;
      case 0x69: op = Iop_InterleaveHI16x4; eLeft = True; break;
      case 0x6A: op = Iop_InterleaveHI32x2; eLeft = True; break;

      case 0x60: op = Iop_InterleaveLO8x8;  eLeft = True; break;
      case 0x61: op = Iop_InterleaveLO16x4; eLeft = True; break;
      case 0x62: op = Iop_InterleaveLO32x2; eLeft = True; break;

      case 0xDB: op = Iop_And64; break;
      case 0xDF: op = Iop_And64; invG = True; break;
      case 0xEB: op = Iop_Or64; break;
      case 0xEF: /* Possibly do better here if argL and argR are the
                    same reg */
                 op = Iop_Xor64; break;

      /* Introduced in SSE1 */
      case 0xE0: op = Iop_Avg8Ux8;    break;
      case 0xE3: op = Iop_Avg16Ux4;   break;
      case 0xEE: op = Iop_Max16Sx4;   break;
      case 0xDE: op = Iop_Max8Ux8;    break;
      case 0xEA: op = Iop_Min16Sx4;   break;
      case 0xDA: op = Iop_Min8Ux8;    break;
      case 0xE4: op = Iop_MulHi16Ux4; break;
      case 0xF6: XXX(amd64g_calculate_mmx_psadbw); break;

      /* Introduced in SSE2 */
      case 0xD4: op = Iop_Add64; break;
      case 0xFB: op = Iop_Sub64; break;

      default: 
         vex_printf("\n0x%x\n", (Int)opc);
         vpanic("dis_MMXop_regmem_to_reg");
   }

#  undef XXX

   argG = getMMXReg(gregLO3ofRM(modrm));
   if (invG)
      argG = unop(Iop_Not64, argG);

   if (isReg) {
      delta++;
      argE = getMMXReg(eregLO3ofRM(modrm));
   } else {
      Int    len;
      IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
      delta += len;
      argE = loadLE(Ity_I64, mkexpr(addr));
   }

   if (eLeft) {
      argL = argE;
      argR = argG;
   } else {
      argL = argG;
      argR = argE;
   }

   if (op != Iop_INVALID) {
      vassert(hName == NULL);
      vassert(hAddr == NULL);
      assign(res, binop(op, argL, argR));
   } else {
      vassert(hName != NULL);
      vassert(hAddr != NULL);
      assign( res, 
              mkIRExprCCall(
                 Ity_I64, 
                 0/*regparms*/, hName, hAddr,
                 mkIRExprVec_2( argL, argR )
              ) 
            );
   }

   putMMXReg( gregLO3ofRM(modrm), mkexpr(res) );

   DIP("%s%s %s, %s\n", 
       name, show_granularity ? nameMMXGran(opc & 3) : "",
       ( isReg ? nameMMXReg(eregLO3ofRM(modrm)) : dis_buf ),
       nameMMXReg(gregLO3ofRM(modrm)) );

   return delta;
}


/* Vector by scalar shift of G by the amount specified at the bottom
   of E.  This is a straight copy of dis_SSE_shiftG_byE. */

static ULong dis_MMX_shiftG_byE ( VexAbiInfo* vbi,
                                  Prefix pfx, Long delta, 
                                  HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen, size;
   IRTemp  addr;
   Bool    shl, shr, sar;
   UChar   rm   = getUChar(delta);
   IRTemp  g0   = newTemp(Ity_I64);
   IRTemp  g1   = newTemp(Ity_I64);
   IRTemp  amt  = newTemp(Ity_I64);
   IRTemp  amt8 = newTemp(Ity_I8);

   if (epartIsReg(rm)) {
      assign( amt, getMMXReg(eregLO3ofRM(rm)) );
      DIP("%s %s,%s\n", opname,
                        nameMMXReg(eregLO3ofRM(rm)),
                        nameMMXReg(gregLO3ofRM(rm)) );
      delta++;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( amt, loadLE(Ity_I64, mkexpr(addr)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameMMXReg(gregLO3ofRM(rm)) );
      delta += alen;
   }
   assign( g0,   getMMXReg(gregLO3ofRM(rm)) );
   assign( amt8, unop(Iop_64to8, mkexpr(amt)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x4: shl = True; size = 32; break;
      case Iop_ShlN32x2: shl = True; size = 32; break;
      case Iop_Shl64:    shl = True; size = 64; break;
      case Iop_ShrN16x4: shr = True; size = 16; break;
      case Iop_ShrN32x2: shr = True; size = 32; break;
      case Iop_Shr64:    shr = True; size = 64; break;
      case Iop_SarN16x4: sar = True; size = 16; break;
      case Iop_SarN32x2: sar = True; size = 32; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,binop(Iop_CmpLT64U,mkexpr(amt),mkU64(size))),
           mkU64(0),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else 
   if (sar) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,binop(Iop_CmpLT64U,mkexpr(amt),mkU64(size))),
           binop(op, mkexpr(g0), mkU8(size-1)),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else {
      vassert(0);
   }

   putMMXReg( gregLO3ofRM(rm), mkexpr(g1) );
   return delta;
}


/* Vector by scalar shift of E by an immediate byte.  This is a
   straight copy of dis_SSE_shiftE_imm. */

static 
ULong dis_MMX_shiftE_imm ( Long delta, HChar* opname, IROp op )
{
   Bool    shl, shr, sar;
   UChar   rm   = getUChar(delta);
   IRTemp  e0   = newTemp(Ity_I64);
   IRTemp  e1   = newTemp(Ity_I64);
   UChar   amt, size;
   vassert(epartIsReg(rm));
   vassert(gregLO3ofRM(rm) == 2 
           || gregLO3ofRM(rm) == 4 || gregLO3ofRM(rm) == 6);
   amt = getUChar(delta+1);
   delta += 2;
   DIP("%s $%d,%s\n", opname,
                      (Int)amt,
                      nameMMXReg(eregLO3ofRM(rm)) );

   assign( e0, getMMXReg(eregLO3ofRM(rm)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x4: shl = True; size = 16; break;
      case Iop_ShlN32x2: shl = True; size = 32; break;
      case Iop_Shl64:    shl = True; size = 64; break;
      case Iop_SarN16x4: sar = True; size = 16; break;
      case Iop_SarN32x2: sar = True; size = 32; break;
      case Iop_ShrN16x4: shr = True; size = 16; break;
      case Iop_ShrN32x2: shr = True; size = 32; break;
      case Iop_Shr64:    shr = True; size = 64; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( e1, amt >= size 
                    ? mkU64(0)
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else 
   if (sar) {
     assign( e1, amt >= size 
                    ? binop(op, mkexpr(e0), mkU8(size-1))
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else {
      vassert(0);
   }

   putMMXReg( eregLO3ofRM(rm), mkexpr(e1) );
   return delta;
}


/* Completely handle all MMX instructions except emms. */

static
ULong dis_MMX ( Bool* decode_ok,
                VexAbiInfo* vbi, Prefix pfx, Int sz, Long delta )
{
   Int   len;
   UChar modrm;
   HChar dis_buf[50];
   UChar opc = getUChar(delta);
   delta++;

   /* dis_MMX handles all insns except emms. */
   do_MMX_preamble();

   switch (opc) {

      case 0x6E: 
         if (sz == 4) {
            /* MOVD (src)ireg32-or-mem32 (E), (dst)mmxreg (G)*/
            modrm = getUChar(delta);
            if (epartIsReg(modrm)) {
               delta++;
               putMMXReg(
                  gregLO3ofRM(modrm),
                  binop( Iop_32HLto64,
                         mkU32(0),
                         getIReg32(eregOfRexRM(pfx,modrm)) ) );
               DIP("movd %s, %s\n", 
                   nameIReg32(eregOfRexRM(pfx,modrm)), 
                   nameMMXReg(gregLO3ofRM(modrm)));
            } else {
               IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
               delta += len;
               putMMXReg(
                  gregLO3ofRM(modrm),
                  binop( Iop_32HLto64,
                         mkU32(0),
                         loadLE(Ity_I32, mkexpr(addr)) ) );
               DIP("movd %s, %s\n", dis_buf, nameMMXReg(gregLO3ofRM(modrm)));
            }
         } 
         else
         if (sz == 8) {
            /* MOVD (src)ireg64-or-mem64 (E), (dst)mmxreg (G)*/
            modrm = getUChar(delta);
            if (epartIsReg(modrm)) {
               delta++;
               putMMXReg( gregLO3ofRM(modrm),
                          getIReg64(eregOfRexRM(pfx,modrm)) );
               DIP("movd %s, %s\n", 
                   nameIReg64(eregOfRexRM(pfx,modrm)), 
                   nameMMXReg(gregLO3ofRM(modrm)));
            } else {
               IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
               delta += len;
               putMMXReg( gregLO3ofRM(modrm),
                          loadLE(Ity_I64, mkexpr(addr)) );
               DIP("movd{64} %s, %s\n", dis_buf, nameMMXReg(gregLO3ofRM(modrm)));
            }
         }
         else {
            goto mmx_decode_failure;
         }
         break;

      case 0x7E:
         if (sz == 4) {
            /* MOVD (src)mmxreg (G), (dst)ireg32-or-mem32 (E) */
            modrm = getUChar(delta);
            if (epartIsReg(modrm)) {
               delta++;
               putIReg32( eregOfRexRM(pfx,modrm),
                          unop(Iop_64to32, getMMXReg(gregLO3ofRM(modrm)) ) );
               DIP("movd %s, %s\n", 
                   nameMMXReg(gregLO3ofRM(modrm)), 
                   nameIReg32(eregOfRexRM(pfx,modrm)));
            } else {
               IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
               delta += len;
               storeLE( mkexpr(addr),
                        unop(Iop_64to32, getMMXReg(gregLO3ofRM(modrm)) ) );
               DIP("movd %s, %s\n", nameMMXReg(gregLO3ofRM(modrm)), dis_buf);
            }
         }
         else
         if (sz == 8) {
            /* MOVD (src)mmxreg (G), (dst)ireg64-or-mem64 (E) */
            modrm = getUChar(delta);
            if (epartIsReg(modrm)) {
               delta++;
               putIReg64( eregOfRexRM(pfx,modrm),
                          getMMXReg(gregLO3ofRM(modrm)) );
               DIP("movd %s, %s\n", 
                   nameMMXReg(gregLO3ofRM(modrm)), 
                   nameIReg64(eregOfRexRM(pfx,modrm)));
            } else {
               IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
               delta += len;
               storeLE( mkexpr(addr),
                       getMMXReg(gregLO3ofRM(modrm)) );
               DIP("movd{64} %s, %s\n", nameMMXReg(gregLO3ofRM(modrm)), dis_buf);
            }
         } else {
            goto mmx_decode_failure;
         }
         break;

      case 0x6F:
         /* MOVQ (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4
             && /*ignore redundant REX.W*/!(sz==8 && haveNo66noF2noF3(pfx))) 
            goto mmx_decode_failure;
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta++;
            putMMXReg( gregLO3ofRM(modrm), getMMXReg(eregLO3ofRM(modrm)) );
            DIP("movq %s, %s\n", 
                nameMMXReg(eregLO3ofRM(modrm)), 
                nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
            delta += len;
            putMMXReg( gregLO3ofRM(modrm), loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movq %s, %s\n", 
                dis_buf, nameMMXReg(gregLO3ofRM(modrm)));
         }
         break;

      case 0x7F:
         /* MOVQ (src)mmxreg, (dst)mmxreg-or-mem */
         if (sz != 4
             && /*ignore redundant REX.W*/!(sz==8 && haveNo66noF2noF3(pfx)))
            goto mmx_decode_failure;
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* Fall through.  The assembler doesn't appear to generate
               these. */
            goto mmx_decode_failure;
         } else {
            IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
            delta += len;
            storeLE( mkexpr(addr), getMMXReg(gregLO3ofRM(modrm)) );
            DIP("mov(nt)q %s, %s\n", 
                nameMMXReg(gregLO3ofRM(modrm)), dis_buf);
         }
         break;

      case 0xFC: 
      case 0xFD: 
      case 0xFE: /* PADDgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "padd", True );
         break;

      case 0xEC: 
      case 0xED: /* PADDSgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4
             && /*ignore redundant REX.W*/!(sz==8 && haveNo66noF2noF3(pfx)))
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "padds", True );
         break;

      case 0xDC: 
      case 0xDD: /* PADDUSgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "paddus", True );
         break;

      case 0xF8: 
      case 0xF9: 
      case 0xFA: /* PSUBgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "psub", True );
         break;

      case 0xE8: 
      case 0xE9: /* PSUBSgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "psubs", True );
         break;

      case 0xD8: 
      case 0xD9: /* PSUBUSgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "psubus", True );
         break;

      case 0xE5: /* PMULHW (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pmulhw", False );
         break;

      case 0xD5: /* PMULLW (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pmullw", False );
         break;

      case 0xF5: /* PMADDWD (src)mmxreg-or-mem, (dst)mmxreg */
         vassert(sz == 4);
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pmaddwd", False );
         break;

      case 0x74: 
      case 0x75: 
      case 0x76: /* PCMPEQgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pcmpeq", True );
         break;

      case 0x64: 
      case 0x65: 
      case 0x66: /* PCMPGTgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pcmpgt", True );
         break;

      case 0x6B: /* PACKSSDW (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "packssdw", False );
         break;

      case 0x63: /* PACKSSWB (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "packsswb", False );
         break;

      case 0x67: /* PACKUSWB (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "packuswb", False );
         break;

      case 0x68: 
      case 0x69: 
      case 0x6A: /* PUNPCKHgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4
             && /*ignore redundant REX.W*/!(sz==8 && haveNo66noF2noF3(pfx))) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "punpckh", True );
         break;

      case 0x60: 
      case 0x61: 
      case 0x62: /* PUNPCKLgg (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4
             && /*ignore redundant REX.W*/!(sz==8 && haveNo66noF2noF3(pfx))) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "punpckl", True );
         break;

      case 0xDB: /* PAND (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pand", False );
         break;

      case 0xDF: /* PANDN (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pandn", False );
         break;

      case 0xEB: /* POR (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "por", False );
         break;

      case 0xEF: /* PXOR (src)mmxreg-or-mem, (dst)mmxreg */
         if (sz != 4) 
            goto mmx_decode_failure;
         delta = dis_MMXop_regmem_to_reg ( vbi, pfx, delta, opc, "pxor", False );
         break; 

#     define SHIFT_BY_REG(_name,_op)                                     \
                delta = dis_MMX_shiftG_byE(vbi, pfx, delta, _name, _op); \
                break;

      /* PSLLgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xF1: SHIFT_BY_REG("psllw", Iop_ShlN16x4);
      case 0xF2: SHIFT_BY_REG("pslld", Iop_ShlN32x2);
      case 0xF3: SHIFT_BY_REG("psllq", Iop_Shl64);

      /* PSRLgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xD1: SHIFT_BY_REG("psrlw", Iop_ShrN16x4);
      case 0xD2: SHIFT_BY_REG("psrld", Iop_ShrN32x2);
      case 0xD3: SHIFT_BY_REG("psrlq", Iop_Shr64);

      /* PSRAgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xE1: SHIFT_BY_REG("psraw", Iop_SarN16x4);
      case 0xE2: SHIFT_BY_REG("psrad", Iop_SarN32x2);

#     undef SHIFT_BY_REG

      case 0x71: 
      case 0x72: 
      case 0x73: {
         /* (sz==4): PSLLgg/PSRAgg/PSRLgg mmxreg by imm8 */
         UChar byte2, subopc;
         if (sz != 4) 
            goto mmx_decode_failure;
         byte2  = getUChar(delta);      /* amode / sub-opcode */
         subopc = toUChar( (byte2 >> 3) & 7 );

#        define SHIFT_BY_IMM(_name,_op)                        \
            do { delta = dis_MMX_shiftE_imm(delta,_name,_op);  \
            } while (0)

              if (subopc == 2 /*SRL*/ && opc == 0x71) 
                  SHIFT_BY_IMM("psrlw", Iop_ShrN16x4);
         else if (subopc == 2 /*SRL*/ && opc == 0x72) 
                 SHIFT_BY_IMM("psrld", Iop_ShrN32x2);
         else if (subopc == 2 /*SRL*/ && opc == 0x73) 
                 SHIFT_BY_IMM("psrlq", Iop_Shr64);

         else if (subopc == 4 /*SAR*/ && opc == 0x71) 
                 SHIFT_BY_IMM("psraw", Iop_SarN16x4);
         else if (subopc == 4 /*SAR*/ && opc == 0x72) 
                 SHIFT_BY_IMM("psrad", Iop_SarN32x2);

         else if (subopc == 6 /*SHL*/ && opc == 0x71) 
                 SHIFT_BY_IMM("psllw", Iop_ShlN16x4);
         else if (subopc == 6 /*SHL*/ && opc == 0x72) 
                  SHIFT_BY_IMM("pslld", Iop_ShlN32x2);
         else if (subopc == 6 /*SHL*/ && opc == 0x73) 
                 SHIFT_BY_IMM("psllq", Iop_Shl64);

         else goto mmx_decode_failure;

#        undef SHIFT_BY_IMM
         break;
      }

      case 0xF7: {
         IRTemp addr    = newTemp(Ity_I64);
         IRTemp regD    = newTemp(Ity_I64);
         IRTemp regM    = newTemp(Ity_I64);
         IRTemp mask    = newTemp(Ity_I64);
         IRTemp olddata = newTemp(Ity_I64);
         IRTemp newdata = newTemp(Ity_I64);

         modrm = getUChar(delta);
         if (sz != 4 || (!epartIsReg(modrm)))
            goto mmx_decode_failure;
         delta++;

         assign( addr, handleAddrOverrides( vbi, pfx, getIReg64(R_RDI) ));
         assign( regM, getMMXReg( eregLO3ofRM(modrm) ));
         assign( regD, getMMXReg( gregLO3ofRM(modrm) ));
         assign( mask, binop(Iop_SarN8x8, mkexpr(regM), mkU8(7)) );
         assign( olddata, loadLE( Ity_I64, mkexpr(addr) ));
         assign( newdata, 
                 binop(Iop_Or64, 
                       binop(Iop_And64, 
                             mkexpr(regD), 
                             mkexpr(mask) ),
                       binop(Iop_And64, 
                             mkexpr(olddata),
                             unop(Iop_Not64, mkexpr(mask)))) );
         storeLE( mkexpr(addr), mkexpr(newdata) );
         DIP("maskmovq %s,%s\n", nameMMXReg( eregLO3ofRM(modrm) ),
                                 nameMMXReg( gregLO3ofRM(modrm) ) );
         break;
      }

      /* --- MMX decode failure --- */
      default:
      mmx_decode_failure:
         *decode_ok = False;
         return delta; /* ignored */

   }

   *decode_ok = True;
   return delta;
}


/*------------------------------------------------------------*/
/*--- More misc arithmetic and other obscure insns.        ---*/
/*------------------------------------------------------------*/

/* Generate base << amt with vacated places filled with stuff
   from xtra.  amt guaranteed in 0 .. 63. */
static 
IRExpr* shiftL64_with_extras ( IRTemp base, IRTemp xtra, IRTemp amt )
{
   /* if   amt == 0 
      then base
      else (base << amt) | (xtra >>u (64-amt))
   */
   return
      IRExpr_Mux0X( 
         mkexpr(amt), 
         mkexpr(base),
         binop(Iop_Or64, 
               binop(Iop_Shl64, mkexpr(base), mkexpr(amt)),
               binop(Iop_Shr64, mkexpr(xtra), 
                                binop(Iop_Sub8, mkU8(64), mkexpr(amt)))
         )
      );
}

/* Generate base >>u amt with vacated places filled with stuff
   from xtra.  amt guaranteed in 0 .. 63. */
static 
IRExpr* shiftR64_with_extras ( IRTemp xtra, IRTemp base, IRTemp amt )
{
   /* if   amt == 0 
      then base
      else (base >>u amt) | (xtra << (64-amt))
   */
   return
      IRExpr_Mux0X( 
         mkexpr(amt), 
         mkexpr(base),
         binop(Iop_Or64, 
               binop(Iop_Shr64, mkexpr(base), mkexpr(amt)),
               binop(Iop_Shl64, mkexpr(xtra), 
                                binop(Iop_Sub8, mkU8(64), mkexpr(amt)))
         )
      );
}

/* Double length left and right shifts.  Apparently only required in
   v-size (no b- variant). */
static
ULong dis_SHLRD_Gv_Ev ( VexAbiInfo* vbi,
                        Prefix pfx,
                        Long delta, UChar modrm,
                        Int sz,
                        IRExpr* shift_amt,
                        Bool amt_is_literal,
                        HChar* shift_amt_txt,
                        Bool left_shift )
{
   /* shift_amt :: Ity_I8 is the amount to shift.  shift_amt_txt is used
      for printing it.   And eip on entry points at the modrm byte. */
   Int len;
   HChar dis_buf[50];

   IRType ty     = szToITy(sz);
   IRTemp gsrc   = newTemp(ty);
   IRTemp esrc   = newTemp(ty);
   IRTemp addr   = IRTemp_INVALID;
   IRTemp tmpSH  = newTemp(Ity_I8);
   IRTemp tmpSS  = newTemp(Ity_I8);
   IRTemp tmp64  = IRTemp_INVALID;
   IRTemp res64  = IRTemp_INVALID;
   IRTemp rss64  = IRTemp_INVALID;
   IRTemp resTy  = IRTemp_INVALID;
   IRTemp rssTy  = IRTemp_INVALID;
   Int    mask   = sz==8 ? 63 : 31;

   vassert(sz == 2 || sz == 4 || sz == 8);

   /* The E-part is the destination; this is shifted.  The G-part
      supplies bits to be shifted into the E-part, but is not
      changed.  

      If shifting left, form a double-length word with E at the top
      and G at the bottom, and shift this left.  The result is then in
      the high part.

      If shifting right, form a double-length word with G at the top
      and E at the bottom, and shift this right.  The result is then
      at the bottom.  */

   /* Fetch the operands. */

   assign( gsrc, getIRegG(sz, pfx, modrm) );

   if (epartIsReg(modrm)) {
      delta++;
      assign( esrc, getIRegE(sz, pfx, modrm) );
      DIP("sh%cd%c %s, %s, %s\n",
          ( left_shift ? 'l' : 'r' ), nameISize(sz), 
          shift_amt_txt,
          nameIRegG(sz, pfx, modrm), nameIRegE(sz, pfx, modrm));
   } else {
      addr = disAMode ( &len, vbi, pfx, delta, dis_buf, 
                        /* # bytes following amode */
                        amt_is_literal ? 1 : 0 );
      delta += len;
      assign( esrc, loadLE(ty, mkexpr(addr)) );
      DIP("sh%cd%c %s, %s, %s\n", 
          ( left_shift ? 'l' : 'r' ), nameISize(sz), 
          shift_amt_txt,
          nameIRegG(sz, pfx, modrm), dis_buf);
   }

   /* Calculate the masked shift amount (tmpSH), the masked subshift
      amount (tmpSS), the shifted value (res64) and the subshifted
      value (rss64). */

   assign( tmpSH, binop(Iop_And8, shift_amt, mkU8(mask)) );
   assign( tmpSS, binop(Iop_And8, 
                        binop(Iop_Sub8, mkexpr(tmpSH), mkU8(1) ),
                        mkU8(mask)));

   tmp64 = newTemp(Ity_I64);
   res64 = newTemp(Ity_I64);
   rss64 = newTemp(Ity_I64);

   if (sz == 2 || sz == 4) {

      /* G is xtra; E is data */
      /* what a freaking nightmare: */
      if (sz == 4 && left_shift) {
         assign( tmp64, binop(Iop_32HLto64, mkexpr(esrc), mkexpr(gsrc)) );
         assign( res64, 
                 binop(Iop_Shr64, 
                       binop(Iop_Shl64, mkexpr(tmp64), mkexpr(tmpSH)),
                       mkU8(32)) );
         assign( rss64, 
                 binop(Iop_Shr64, 
                       binop(Iop_Shl64, mkexpr(tmp64), mkexpr(tmpSS)),
                       mkU8(32)) );
      }
      else
      if (sz == 4 && !left_shift) {
         assign( tmp64, binop(Iop_32HLto64, mkexpr(gsrc), mkexpr(esrc)) );
         assign( res64, binop(Iop_Shr64, mkexpr(tmp64), mkexpr(tmpSH)) );
         assign( rss64, binop(Iop_Shr64, mkexpr(tmp64), mkexpr(tmpSS)) );
      }
      else
      if (sz == 2 && left_shift) {
         assign( tmp64,
                 binop(Iop_32HLto64,
                       binop(Iop_16HLto32, mkexpr(esrc), mkexpr(gsrc)),
                       binop(Iop_16HLto32, mkexpr(gsrc), mkexpr(gsrc))
         ));
	 /* result formed by shifting [esrc'gsrc'gsrc'gsrc] */
         assign( res64, 
                 binop(Iop_Shr64, 
                       binop(Iop_Shl64, mkexpr(tmp64), mkexpr(tmpSH)),
                       mkU8(48)) );
         /* subshift formed by shifting [esrc'0000'0000'0000] */
         assign( rss64, 
                 binop(Iop_Shr64, 
                       binop(Iop_Shl64, 
                             binop(Iop_Shl64, unop(Iop_16Uto64, mkexpr(esrc)),
                                              mkU8(48)),
                             mkexpr(tmpSS)),
                       mkU8(48)) );
      }
      else
      if (sz == 2 && !left_shift) {
         assign( tmp64,
                 binop(Iop_32HLto64,
                       binop(Iop_16HLto32, mkexpr(gsrc), mkexpr(gsrc)),
                       binop(Iop_16HLto32, mkexpr(gsrc), mkexpr(esrc))
         ));
         /* result formed by shifting [gsrc'gsrc'gsrc'esrc] */
         assign( res64, binop(Iop_Shr64, mkexpr(tmp64), mkexpr(tmpSH)) );
         /* subshift formed by shifting [0000'0000'0000'esrc] */
         assign( rss64, binop(Iop_Shr64, 
                              unop(Iop_16Uto64, mkexpr(esrc)), 
                              mkexpr(tmpSS)) );
      }

   } else {

      vassert(sz == 8);
      if (left_shift) {
         assign( res64, shiftL64_with_extras( esrc, gsrc, tmpSH ));
         assign( rss64, shiftL64_with_extras( esrc, gsrc, tmpSS ));
      } else {
         assign( res64, shiftR64_with_extras( gsrc, esrc, tmpSH ));
         assign( rss64, shiftR64_with_extras( gsrc, esrc, tmpSS ));
      }

   }

   resTy = newTemp(ty);
   rssTy = newTemp(ty);
   assign( resTy, narrowTo(ty, mkexpr(res64)) );
   assign( rssTy, narrowTo(ty, mkexpr(rss64)) );

   /* Put result back and write the flags thunk. */
   setFlags_DEP1_DEP2_shift ( left_shift ? Iop_Shl64 : Iop_Sar64,
                              resTy, rssTy, ty, tmpSH );

   if (epartIsReg(modrm)) {
      putIRegE(sz, pfx, modrm, mkexpr(resTy));
   } else {
      storeLE( mkexpr(addr), mkexpr(resTy) );
   }

   if (amt_is_literal) delta++;
   return delta;
}


/* Handle BT/BTS/BTR/BTC Gv, Ev.  Apparently b-size is not
   required. */

typedef enum { BtOpNone, BtOpSet, BtOpReset, BtOpComp } BtOp;

static HChar* nameBtOp ( BtOp op )
{
   switch (op) {
      case BtOpNone:  return "";
      case BtOpSet:   return "s";
      case BtOpReset: return "r";
      case BtOpComp:  return "c";
      default: vpanic("nameBtOp(amd64)");
   }
}


static
ULong dis_bt_G_E ( VexAbiInfo* vbi,
                   Prefix pfx, Int sz, Long delta, BtOp op )
{
   HChar  dis_buf[50];
   UChar  modrm;
   Int    len;
   IRTemp t_fetched, t_bitno0, t_bitno1, t_bitno2, t_addr0, 
     t_addr1, t_rsp, t_mask, t_new;

   vassert(sz == 2 || sz == 4 || sz == 8);

   t_fetched = t_bitno0 = t_bitno1 = t_bitno2 
             = t_addr0 = t_addr1 = t_rsp
             = t_mask = t_new = IRTemp_INVALID;

   t_fetched = newTemp(Ity_I8);
   t_new     = newTemp(Ity_I8);
   t_bitno0  = newTemp(Ity_I64);
   t_bitno1  = newTemp(Ity_I64);
   t_bitno2  = newTemp(Ity_I8);
   t_addr1   = newTemp(Ity_I64);
   modrm     = getUChar(delta);

   assign( t_bitno0, widenSto64(getIRegG(sz, pfx, modrm)) );
   
   if (epartIsReg(modrm)) {
      delta++;
      /* Get it onto the client's stack.  Oh, this is a horrible
         kludge.  See https://bugs.kde.org/show_bug.cgi?id=245925.
         Because of the ELF ABI stack redzone, there may be live data
         up to 128 bytes below %RSP.  So we can't just push it on the
         stack, else we may wind up trashing live data, and causing
         impossible-to-find simulation errors.  (Yes, this did
         happen.)  So we need to drop RSP before at least 128 before
         pushing it.  That unfortunately means hitting Memcheck's
         fast-case painting code.  Ideally we should drop more than
         128, to reduce the chances of breaking buggy programs that
         have live data below -128(%RSP).  Memcheck fast-cases moves
         of 288 bytes due to the need to handle ppc64-linux quickly,
         so let's use 288.  Of course the real fix is to get rid of
         this kludge entirely.  */
      t_rsp = newTemp(Ity_I64);
      t_addr0 = newTemp(Ity_I64);

      vassert(vbi->guest_stack_redzone_size == 128);
      assign( t_rsp, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(288)) );
      putIReg64(R_RSP, mkexpr(t_rsp));

      storeLE( mkexpr(t_rsp), getIRegE(sz, pfx, modrm) );

      /* Make t_addr0 point at it. */
      assign( t_addr0, mkexpr(t_rsp) );

      /* Mask out upper bits of the shift amount, since we're doing a
         reg. */
      assign( t_bitno1, binop(Iop_And64, 
                              mkexpr(t_bitno0), 
                              mkU64(sz == 8 ? 63 : sz == 4 ? 31 : 15)) );

   } else {
      t_addr0 = disAMode ( &len, vbi, pfx, delta, dis_buf, 0 );
      delta += len;
      assign( t_bitno1, mkexpr(t_bitno0) );
   }
  
   /* At this point: t_addr0 is the address being operated on.  If it
      was a reg, we will have pushed it onto the client's stack.
      t_bitno1 is the bit number, suitably masked in the case of a
      reg.  */
  
   /* Now the main sequence. */
   assign( t_addr1, 
           binop(Iop_Add64, 
                 mkexpr(t_addr0), 
                 binop(Iop_Sar64, mkexpr(t_bitno1), mkU8(3))) );

   /* t_addr1 now holds effective address */

   assign( t_bitno2, 
           unop(Iop_64to8, 
                binop(Iop_And64, mkexpr(t_bitno1), mkU64(7))) );

   /* t_bitno2 contains offset of bit within byte */

   if (op != BtOpNone) {
      t_mask = newTemp(Ity_I8);
      assign( t_mask, binop(Iop_Shl8, mkU8(1), mkexpr(t_bitno2)) );
   }

   /* t_mask is now a suitable byte mask */

   assign( t_fetched, loadLE(Ity_I8, mkexpr(t_addr1)) );

   if (op != BtOpNone) {
      switch (op) {
         case BtOpSet:
            assign( t_new,
                    binop(Iop_Or8, mkexpr(t_fetched), mkexpr(t_mask)) );
            break;
         case BtOpComp:
            assign( t_new,
                    binop(Iop_Xor8, mkexpr(t_fetched), mkexpr(t_mask)) );
            break;
         case BtOpReset:
            assign( t_new,
                    binop(Iop_And8, mkexpr(t_fetched), 
                                    unop(Iop_Not8, mkexpr(t_mask))) );
            break;
         default: 
            vpanic("dis_bt_G_E(amd64)");
      }
      if ((pfx & PFX_LOCK) && !epartIsReg(modrm)) {
         casLE( mkexpr(t_addr1), mkexpr(t_fetched)/*expd*/,
                                 mkexpr(t_new)/*new*/,
                                 guest_RIP_curr_instr );
      } else {
         storeLE( mkexpr(t_addr1), mkexpr(t_new) );
      }
   }
  
   /* Side effect done; now get selected bit into Carry flag */
   /* Flags: C=selected bit, O,S,Z,A,P undefined, so are set to zero. */
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            binop(Iop_And64,
                  binop(Iop_Shr64, 
                        unop(Iop_8Uto64, mkexpr(t_fetched)),
                        mkexpr(t_bitno2)),
                  mkU64(1)))
       );
   /* Set NDEP even though it isn't used.  This makes redundant-PUT
      elimination of previous stores to this field work better. */
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));

   /* Move reg operand from stack back to reg */
   if (epartIsReg(modrm)) {
      /* t_rsp still points at it. */
      /* only write the reg if actually modifying it; doing otherwise
         zeroes the top half erroneously when doing btl due to
         standard zero-extend rule */
      if (op != BtOpNone)
         putIRegE(sz, pfx, modrm, loadLE(szToITy(sz), mkexpr(t_rsp)) );
      putIReg64(R_RSP, binop(Iop_Add64, mkexpr(t_rsp), mkU64(288)) );
   }

   DIP("bt%s%c %s, %s\n",
       nameBtOp(op), nameISize(sz), nameIRegG(sz, pfx, modrm), 
       ( epartIsReg(modrm) ? nameIRegE(sz, pfx, modrm) : dis_buf ) );
 
   return delta;
}



/* Handle BSF/BSR.  Only v-size seems necessary. */
static
ULong dis_bs_E_G ( VexAbiInfo* vbi,
                   Prefix pfx, Int sz, Long delta, Bool fwds )
{
   Bool   isReg;
   UChar  modrm;
   HChar  dis_buf[50];

   IRType ty    = szToITy(sz);
   IRTemp src   = newTemp(ty);
   IRTemp dst   = newTemp(ty);
   IRTemp src64 = newTemp(Ity_I64);
   IRTemp dst64 = newTemp(Ity_I64);
   IRTemp src8  = newTemp(Ity_I8);

   vassert(sz == 8 || sz == 4 || sz == 2);

   modrm = getUChar(delta);
   isReg = epartIsReg(modrm);
   if (isReg) {
      delta++;
      assign( src, getIRegE(sz, pfx, modrm) );
   } else {
      Int    len;
      IRTemp addr = disAMode( &len, vbi, pfx, delta, dis_buf, 0 );
      delta += len;
      assign( src, loadLE(ty, mkexpr(addr)) );
   }

   DIP("bs%c%c %s, %s\n",
       fwds ? 'f' : 'r', nameISize(sz), 
       ( isReg ? nameIRegE(sz, pfx, modrm) : dis_buf ), 
       nameIRegG(sz, pfx, modrm));

   /* First, widen src to 64 bits if it is not already. */
   assign( src64, widenUto64(mkexpr(src)) );

   /* Generate an 8-bit expression which is zero iff the 
      original is zero, and nonzero otherwise */
   assign( src8,
           unop(Iop_1Uto8, 
                binop(Iop_CmpNE64,
                      mkexpr(src64), mkU64(0))) );

   /* Flags: Z is 1 iff source value is zero.  All others 
      are undefined -- we force them to zero. */
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            IRExpr_Mux0X( mkexpr(src8),
                          /* src==0 */
                          mkU64(AMD64G_CC_MASK_Z),
                          /* src!=0 */
                          mkU64(0)
                        )
       ));
   /* Set NDEP even though it isn't used.  This makes redundant-PUT
      elimination of previous stores to this field work better. */
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));

   /* Result: iff source value is zero, we can't use
      Iop_Clz64/Iop_Ctz64 as they have no defined result in that case.
      But anyway, amd64 semantics say the result is undefined in
      such situations.  Hence handle the zero case specially. */

   /* Bleh.  What we compute:

          bsf64:  if src == 0 then {dst is unchanged} 
                              else Ctz64(src)

          bsr64:  if src == 0 then {dst is unchanged} 
                              else 63 - Clz64(src)

          bsf32:  if src == 0 then {dst is unchanged} 
                              else Ctz64(32Uto64(src))

          bsr32:  if src == 0 then {dst is unchanged}
                              else 63 - Clz64(32Uto64(src))

          bsf16:  if src == 0 then {dst is unchanged} 
                              else Ctz64(32Uto64(16Uto32(src)))

          bsr16:  if src == 0 then {dst is unchanged} 
                              else 63 - Clz64(32Uto64(16Uto32(src)))
   */

   /* The main computation, guarding against zero. */
   assign( dst64,
           IRExpr_Mux0X( 
              mkexpr(src8),
              /* src == 0 -- leave dst unchanged */
              widenUto64( getIRegG( sz, pfx, modrm ) ),
              /* src != 0 */
              fwds ? unop(Iop_Ctz64, mkexpr(src64))
                   : binop(Iop_Sub64, 
                           mkU64(63), 
                           unop(Iop_Clz64, mkexpr(src64)))
           )
         );

   if (sz == 2)
      assign( dst, unop(Iop_64to16, mkexpr(dst64)) );
   else
   if (sz == 4)
      assign( dst, unop(Iop_64to32, mkexpr(dst64)) );
   else
      assign( dst, mkexpr(dst64) );

   /* dump result back */
   putIRegG( sz, pfx, modrm, mkexpr(dst) );

   return delta;
}


/* swap rAX with the reg specified by reg and REX.B */
static 
void codegen_xchg_rAX_Reg ( Prefix pfx, Int sz, UInt regLo3 )
{
   IRType ty = szToITy(sz);
   IRTemp t1 = newTemp(ty);
   IRTemp t2 = newTemp(ty);
   vassert(sz == 2 || sz == 4 || sz == 8);
   vassert(regLo3 < 8);
   if (sz == 8) {
      assign( t1, getIReg64(R_RAX) );
      assign( t2, getIRegRexB(8, pfx, regLo3) );
      putIReg64( R_RAX, mkexpr(t2) );
      putIRegRexB(8, pfx, regLo3, mkexpr(t1) );
   } else if (sz == 4) {
      assign( t1, getIReg32(R_RAX) );
      assign( t2, getIRegRexB(4, pfx, regLo3) );
      putIReg32( R_RAX, mkexpr(t2) );
      putIRegRexB(4, pfx, regLo3, mkexpr(t1) );
   } else {
      assign( t1, getIReg16(R_RAX) );
      assign( t2, getIRegRexB(2, pfx, regLo3) );
      putIReg16( R_RAX, mkexpr(t2) );
      putIRegRexB(2, pfx, regLo3, mkexpr(t1) );
   }
   DIP("xchg%c %s, %s\n", 
       nameISize(sz), nameIRegRAX(sz), 
                      nameIRegRexB(sz,pfx, regLo3));
}


static 
void codegen_SAHF ( void )
{
   /* Set the flags to:
      (amd64g_calculate_flags_all() & AMD64G_CC_MASK_O) 
                                    -- retain the old O flag
      | (%AH & (AMD64G_CC_MASK_S|AMD64G_CC_MASK_Z|AMD64G_CC_MASK_A
                |AMD64G_CC_MASK_P|AMD64G_CC_MASK_C)
   */
   ULong  mask_SZACP = AMD64G_CC_MASK_S|AMD64G_CC_MASK_Z|AMD64G_CC_MASK_A
                       |AMD64G_CC_MASK_C|AMD64G_CC_MASK_P;
   IRTemp oldflags   = newTemp(Ity_I64);
   assign( oldflags, mk_amd64g_calculate_rflags_all() );
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( OFFB_CC_DEP1,
         binop(Iop_Or64,
               binop(Iop_And64, mkexpr(oldflags), mkU64(AMD64G_CC_MASK_O)),
               binop(Iop_And64, 
                     binop(Iop_Shr64, getIReg64(R_RAX), mkU8(8)),
                     mkU64(mask_SZACP))
              )
   ));
}


static 
void codegen_LAHF ( void  )
{
   /* AH <- EFLAGS(SF:ZF:0:AF:0:PF:1:CF) */
   IRExpr* rax_with_hole;
   IRExpr* new_byte;
   IRExpr* new_rax;
   ULong   mask_SZACP = AMD64G_CC_MASK_S|AMD64G_CC_MASK_Z|AMD64G_CC_MASK_A
                        |AMD64G_CC_MASK_C|AMD64G_CC_MASK_P;

   IRTemp  flags = newTemp(Ity_I64);
   assign( flags, mk_amd64g_calculate_rflags_all() );

   rax_with_hole 
      = binop(Iop_And64, getIReg64(R_RAX), mkU64(~0xFF00ULL));
   new_byte 
      = binop(Iop_Or64, binop(Iop_And64, mkexpr(flags), mkU64(mask_SZACP)),
                        mkU64(1<<1));
   new_rax 
      = binop(Iop_Or64, rax_with_hole,
                        binop(Iop_Shl64, new_byte, mkU8(8)));
   putIReg64(R_RAX, new_rax);
}


static
ULong dis_cmpxchg_G_E ( /*OUT*/Bool* ok,
                        VexAbiInfo*  vbi,
                        Prefix       pfx,
                        Int          size, 
                        Long         delta0 )
{
   HChar dis_buf[50];
   Int   len;

   IRType ty    = szToITy(size);
   IRTemp acc   = newTemp(ty);
   IRTemp src   = newTemp(ty);
   IRTemp dest  = newTemp(ty);
   IRTemp dest2 = newTemp(ty);
   IRTemp acc2  = newTemp(ty);
   IRTemp cond8 = newTemp(Ity_I8);
   IRTemp addr  = IRTemp_INVALID;
   UChar  rm    = getUChar(delta0);

   /* There are 3 cases to consider:

      reg-reg: ignore any lock prefix, generate sequence based
               on Mux0X

      reg-mem, not locked: ignore any lock prefix, generate sequence
                           based on Mux0X

      reg-mem, locked: use IRCAS
   */

   if (epartIsReg(rm)) {
      /* case 1 */
      assign( dest, getIRegE(size, pfx, rm) );
      delta0++;
      assign( src, getIRegG(size, pfx, rm) );
      assign( acc, getIRegRAX(size) );
      setFlags_DEP1_DEP2(Iop_Sub8, acc, dest, ty);
      assign( cond8, unop(Iop_1Uto8, mk_amd64g_calculate_condition(AMD64CondZ)) );
      assign( dest2, IRExpr_Mux0X(mkexpr(cond8), mkexpr(dest), mkexpr(src)) );
      assign( acc2,  IRExpr_Mux0X(mkexpr(cond8), mkexpr(dest), mkexpr(acc)) );
      putIRegRAX(size, mkexpr(acc2));
      putIRegE(size, pfx, rm, mkexpr(dest2));
      DIP("cmpxchg%c %s,%s\n", nameISize(size),
                               nameIRegG(size,pfx,rm),
                               nameIRegE(size,pfx,rm) );
   } 
   else if (!epartIsReg(rm) && !(pfx & PFX_LOCK)) {
      /* case 2 */
      addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign( dest, loadLE(ty, mkexpr(addr)) );
      delta0 += len;
      assign( src, getIRegG(size, pfx, rm) );
      assign( acc, getIRegRAX(size) );
      setFlags_DEP1_DEP2(Iop_Sub8, acc, dest, ty);
      assign( cond8, unop(Iop_1Uto8, mk_amd64g_calculate_condition(AMD64CondZ)) );
      assign( dest2, IRExpr_Mux0X(mkexpr(cond8), mkexpr(dest), mkexpr(src)) );
      assign( acc2,  IRExpr_Mux0X(mkexpr(cond8), mkexpr(dest), mkexpr(acc)) );
      putIRegRAX(size, mkexpr(acc2));
      storeLE( mkexpr(addr), mkexpr(dest2) );
      DIP("cmpxchg%c %s,%s\n", nameISize(size), 
                               nameIRegG(size,pfx,rm), dis_buf);
   }
   else if (!epartIsReg(rm) && (pfx & PFX_LOCK)) {
      /* case 3 */
      /* src is new value.  acc is expected value.  dest is old value.
         Compute success from the output of the IRCAS, and steer the
         new value for RAX accordingly: in case of success, RAX is
         unchanged. */
      addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      delta0 += len;
      assign( src, getIRegG(size, pfx, rm) );
      assign( acc, getIRegRAX(size) );
      stmt( IRStmt_CAS( 
         mkIRCAS( IRTemp_INVALID, dest, Iend_LE, mkexpr(addr), 
                  NULL, mkexpr(acc), NULL, mkexpr(src) )
      ));
      setFlags_DEP1_DEP2(Iop_Sub8, acc, dest, ty);
      assign( cond8, unop(Iop_1Uto8, mk_amd64g_calculate_condition(AMD64CondZ)) );
      assign( acc2,  IRExpr_Mux0X(mkexpr(cond8), mkexpr(dest), mkexpr(acc)) );
      putIRegRAX(size, mkexpr(acc2));
      DIP("cmpxchg%c %s,%s\n", nameISize(size), 
                               nameIRegG(size,pfx,rm), dis_buf);
   }
   else vassert(0);

   *ok = True;
   return delta0;
}


/* Handle conditional move instructions of the form
      cmovcc E(reg-or-mem), G(reg)

   E(src) is reg-or-mem
   G(dst) is reg.

   If E is reg, -->    GET %E, tmps
                       GET %G, tmpd
                       CMOVcc tmps, tmpd
                       PUT tmpd, %G
 
   If E is mem  -->    (getAddr E) -> tmpa
                       LD (tmpa), tmps
                       GET %G, tmpd
                       CMOVcc tmps, tmpd
                       PUT tmpd, %G
*/
static
ULong dis_cmov_E_G ( VexAbiInfo* vbi,
                     Prefix        pfx,
                     Int           sz, 
                     AMD64Condcode cond,
                     Long          delta0 )
{
   UChar rm  = getUChar(delta0);
   HChar dis_buf[50];
   Int   len;

   IRType ty   = szToITy(sz);
   IRTemp tmps = newTemp(ty);
   IRTemp tmpd = newTemp(ty);

   if (epartIsReg(rm)) {
      assign( tmps, getIRegE(sz, pfx, rm) );
      assign( tmpd, getIRegG(sz, pfx, rm) );

      putIRegG( sz, pfx, rm,
                IRExpr_Mux0X( unop(Iop_1Uto8,
                                   mk_amd64g_calculate_condition(cond)),
                              mkexpr(tmpd),
                              mkexpr(tmps) )
              );
      DIP("cmov%s %s,%s\n", name_AMD64Condcode(cond),
                            nameIRegE(sz,pfx,rm),
                            nameIRegG(sz,pfx,rm));
      return 1+delta0;
   }

   /* E refers to memory */    
   {
      IRTemp addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign( tmps, loadLE(ty, mkexpr(addr)) );
      assign( tmpd, getIRegG(sz, pfx, rm) );

      putIRegG( sz, pfx, rm,
                IRExpr_Mux0X( unop(Iop_1Uto8,
                                   mk_amd64g_calculate_condition(cond)),
                              mkexpr(tmpd),
                              mkexpr(tmps) )
              );

      DIP("cmov%s %s,%s\n", name_AMD64Condcode(cond),
                            dis_buf,
                            nameIRegG(sz,pfx,rm));
      return len+delta0;
   }
}


static
ULong dis_xadd_G_E ( /*OUT*/Bool* decode_ok,
                     VexAbiInfo* vbi,
                     Prefix pfx, Int sz, Long delta0 )
{
   Int   len;
   UChar rm = getUChar(delta0);
   HChar dis_buf[50];

   IRType ty    = szToITy(sz);
   IRTemp tmpd  = newTemp(ty);
   IRTemp tmpt0 = newTemp(ty);
   IRTemp tmpt1 = newTemp(ty);

   /* There are 3 cases to consider:

      reg-reg: ignore any lock prefix,
               generate 'naive' (non-atomic) sequence

      reg-mem, not locked: ignore any lock prefix, generate 'naive'
                           (non-atomic) sequence

      reg-mem, locked: use IRCAS
   */

   if (epartIsReg(rm)) {
      /* case 1 */
      assign( tmpd, getIRegE(sz, pfx, rm) );
      assign( tmpt0, getIRegG(sz, pfx, rm) );
      assign( tmpt1, binop(mkSizedOp(ty,Iop_Add8),
                           mkexpr(tmpd), mkexpr(tmpt0)) );
      setFlags_DEP1_DEP2( Iop_Add8, tmpd, tmpt0, ty );
      putIRegG(sz, pfx, rm, mkexpr(tmpd));
      putIRegE(sz, pfx, rm, mkexpr(tmpt1));
      DIP("xadd%c %s, %s\n",
          nameISize(sz), nameIRegG(sz,pfx,rm),
          				 nameIRegE(sz,pfx,rm));
      *decode_ok = True;
      return 1+delta0;
   }
   else if (!epartIsReg(rm) && !(pfx & PFX_LOCK)) {
      /* case 2 */
      IRTemp addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign( tmpd,  loadLE(ty, mkexpr(addr)) );
      assign( tmpt0, getIRegG(sz, pfx, rm) );
      assign( tmpt1, binop(mkSizedOp(ty,Iop_Add8),
                           mkexpr(tmpd), mkexpr(tmpt0)) );
      setFlags_DEP1_DEP2( Iop_Add8, tmpd, tmpt0, ty );
      storeLE( mkexpr(addr), mkexpr(tmpt1) );
      putIRegG(sz, pfx, rm, mkexpr(tmpd));
      DIP("xadd%c %s, %s\n",
          nameISize(sz), nameIRegG(sz,pfx,rm), dis_buf);
      *decode_ok = True;
      return len+delta0;
   }
   else if (!epartIsReg(rm) && (pfx & PFX_LOCK)) {
      /* case 3 */
      IRTemp addr = disAMode ( &len, vbi, pfx, delta0, dis_buf, 0 );
      assign( tmpd,  loadLE(ty, mkexpr(addr)) );
      assign( tmpt0, getIRegG(sz, pfx, rm) );
      assign( tmpt1, binop(mkSizedOp(ty,Iop_Add8), 
                           mkexpr(tmpd), mkexpr(tmpt0)) );
      casLE( mkexpr(addr), mkexpr(tmpd)/*expVal*/,
                           mkexpr(tmpt1)/*newVal*/, guest_RIP_curr_instr );
      setFlags_DEP1_DEP2( Iop_Add8, tmpd, tmpt0, ty );
      putIRegG(sz, pfx, rm, mkexpr(tmpd));
      DIP("xadd%c %s, %s\n",
          nameISize(sz), nameIRegG(sz,pfx,rm), dis_buf);
      *decode_ok = True;
      return len+delta0;
   }
   /*UNREACHED*/
   vassert(0);
}

//.. /* Move 16 bits from Ew (ireg or mem) to G (a segment register). */
//.. 
//.. static
//.. UInt dis_mov_Ew_Sw ( UChar sorb, Long delta0 )
//.. {
//..    Int    len;
//..    IRTemp addr;
//..    UChar  rm  = getUChar(delta0);
//..    HChar  dis_buf[50];
//.. 
//..    if (epartIsReg(rm)) {
//..       putSReg( gregOfRM(rm), getIReg(2, eregOfRM(rm)) );
//..       DIP("movw %s,%s\n", nameIReg(2,eregOfRM(rm)), nameSReg(gregOfRM(rm)));
//..       return 1+delta0;
//..    } else {
//..       addr = disAMode ( &len, sorb, delta0, dis_buf );
//..       putSReg( gregOfRM(rm), loadLE(Ity_I16, mkexpr(addr)) );
//..       DIP("movw %s,%s\n", dis_buf, nameSReg(gregOfRM(rm)));
//..       return len+delta0;
//..    }
//.. }
//.. 
//.. /* Move 16 bits from G (a segment register) to Ew (ireg or mem).  If
//..    dst is ireg and sz==4, zero out top half of it.  */
//.. 
//.. static
//.. UInt dis_mov_Sw_Ew ( UChar sorb,
//..                      Int   sz,
//..                      UInt  delta0 )
//.. {
//..    Int    len;
//..    IRTemp addr;
//..    UChar  rm  = getUChar(delta0);
//..    HChar  dis_buf[50];
//.. 
//..    vassert(sz == 2 || sz == 4);
//.. 
//..    if (epartIsReg(rm)) {
//..       if (sz == 4)
//..          putIReg(4, eregOfRM(rm), unop(Iop_16Uto32, getSReg(gregOfRM(rm))));
//..       else
//..          putIReg(2, eregOfRM(rm), getSReg(gregOfRM(rm)));
//.. 
//..       DIP("mov %s,%s\n", nameSReg(gregOfRM(rm)), nameIReg(sz,eregOfRM(rm)));
//..       return 1+delta0;
//..    } else {
//..       addr = disAMode ( &len, sorb, delta0, dis_buf );
//..       storeLE( mkexpr(addr), getSReg(gregOfRM(rm)) );
//..       DIP("mov %s,%s\n", nameSReg(gregOfRM(rm)), dis_buf);
//..       return len+delta0;
//..    }
//.. }
//.. 
//.. 
//.. static 
//.. void dis_push_segreg ( UInt sreg, Int sz )
//.. {
//..     IRTemp t1 = newTemp(Ity_I16);
//..     IRTemp ta = newTemp(Ity_I32);
//..     vassert(sz == 2 || sz == 4);
//.. 
//..     assign( t1, getSReg(sreg) );
//..     assign( ta, binop(Iop_Sub32, getIReg(4, R_ESP), mkU32(sz)) );
//..     putIReg(4, R_ESP, mkexpr(ta));
//..     storeLE( mkexpr(ta), mkexpr(t1) );
//.. 
//..     DIP("pushw %s\n", nameSReg(sreg));
//.. }
//.. 
//.. static
//.. void dis_pop_segreg ( UInt sreg, Int sz )
//.. {
//..     IRTemp t1 = newTemp(Ity_I16);
//..     IRTemp ta = newTemp(Ity_I32);
//..     vassert(sz == 2 || sz == 4);
//.. 
//..     assign( ta, getIReg(4, R_ESP) );
//..     assign( t1, loadLE(Ity_I16, mkexpr(ta)) );
//.. 
//..     putIReg(4, R_ESP, binop(Iop_Add32, mkexpr(ta), mkU32(sz)) );
//..     putSReg( sreg, mkexpr(t1) );
//..     DIP("pop %s\n", nameSReg(sreg));
//.. }

static
void dis_ret ( /*MOD*/DisResult* dres, VexAbiInfo* vbi, ULong d64 )
{
   IRTemp t1 = newTemp(Ity_I64); 
   IRTemp t2 = newTemp(Ity_I64);
   IRTemp t3 = newTemp(Ity_I64);
   assign(t1, getIReg64(R_RSP));
   assign(t2, loadLE(Ity_I64,mkexpr(t1)));
   assign(t3, binop(Iop_Add64, mkexpr(t1), mkU64(8+d64)));
   putIReg64(R_RSP, mkexpr(t3));
   make_redzone_AbiHint(vbi, t3, t2/*nia*/, "ret");
   jmp_treg(dres, Ijk_Ret, t2);
   vassert(dres->whatNext == Dis_StopHere);
}


/*------------------------------------------------------------*/
/*--- SSE/SSE2/SSE3 helpers                                ---*/
/*------------------------------------------------------------*/

/* Worker function; do not call directly. 
   Handles full width G = G `op` E   and   G = (not G) `op` E.
*/

static ULong dis_SSE_E_to_G_all_wrk ( 
                VexAbiInfo* vbi,
                Prefix pfx, Long delta, 
                HChar* opname, IROp op,
                Bool   invertG
             )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRExpr* gpart
      = invertG ? unop(Iop_NotV128, getXMMReg(gregOfRexRM(pfx,rm)))
                : getXMMReg(gregOfRexRM(pfx,rm));
   if (epartIsReg(rm)) {
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart,
                           getXMMReg(eregOfRexRM(pfx,rm))) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart,
                           loadLE(Ity_V128, mkexpr(addr))) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* All lanes SSE binary operation, G = G `op` E. */

static
ULong dis_SSE_E_to_G_all ( VexAbiInfo* vbi,
                           Prefix pfx, Long delta, 
                           HChar* opname, IROp op )
{
   return dis_SSE_E_to_G_all_wrk( vbi, pfx, delta, opname, op, False );
}

/* All lanes SSE binary operation, G = (not G) `op` E. */

static
ULong dis_SSE_E_to_G_all_invG ( VexAbiInfo* vbi,
                                Prefix pfx, Long delta, 
                                HChar* opname, IROp op )
{
   return dis_SSE_E_to_G_all_wrk( vbi, pfx, delta, opname, op, True );
}


/* Lowest 32-bit lane only SSE binary operation, G = G `op` E. */

static ULong dis_SSE_E_to_G_lo32 ( VexAbiInfo* vbi,
                                   Prefix pfx, Long delta, 
                                   HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRExpr* gpart = getXMMReg(gregOfRexRM(pfx,rm));
   if (epartIsReg(rm)) {
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart,
                           getXMMReg(eregOfRexRM(pfx,rm))) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      /* We can only do a 32-bit memory read, so the upper 3/4 of the
         E operand needs to be made simply of zeroes. */
      IRTemp epart = newTemp(Ity_V128);
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( epart, unop( Iop_32UtoV128,
                           loadLE(Ity_I32, mkexpr(addr))) );
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart, mkexpr(epart)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* Lower 64-bit lane only SSE binary operation, G = G `op` E. */

static ULong dis_SSE_E_to_G_lo64 ( VexAbiInfo* vbi,
                                   Prefix pfx, Long delta, 
                                   HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRExpr* gpart = getXMMReg(gregOfRexRM(pfx,rm));
   if (epartIsReg(rm)) {
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart,
                           getXMMReg(eregOfRexRM(pfx,rm))) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      /* We can only do a 64-bit memory read, so the upper half of the
         E operand needs to be made simply of zeroes. */
      IRTemp epart = newTemp(Ity_V128);
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( epart, unop( Iop_64UtoV128,
                           loadLE(Ity_I64, mkexpr(addr))) );
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(op, gpart, mkexpr(epart)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* All lanes unary SSE operation, G = op(E). */

static ULong dis_SSE_E_to_G_unary_all ( 
                VexAbiInfo* vbi,
                Prefix pfx, Long delta, 
                HChar* opname, IROp op
             )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   if (epartIsReg(rm)) {
      putXMMReg( gregOfRexRM(pfx,rm), 
                 unop(op, getXMMReg(eregOfRexRM(pfx,rm))) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      putXMMReg( gregOfRexRM(pfx,rm), 
                 unop(op, loadLE(Ity_V128, mkexpr(addr))) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* Lowest 32-bit lane only unary SSE operation, G = op(E). */

static ULong dis_SSE_E_to_G_unary_lo32 ( 
                VexAbiInfo* vbi,
                Prefix pfx, Long delta, 
                HChar* opname, IROp op
             )
{
   /* First we need to get the old G value and patch the low 32 bits
      of the E operand into it.  Then apply op and write back to G. */
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRTemp  oldG0 = newTemp(Ity_V128);
   IRTemp  oldG1 = newTemp(Ity_V128);

   assign( oldG0, getXMMReg(gregOfRexRM(pfx,rm)) );

   if (epartIsReg(rm)) {
      assign( oldG1, 
              binop( Iop_SetV128lo32,
                     mkexpr(oldG0),
                     getXMMRegLane32(eregOfRexRM(pfx,rm), 0)) );
      putXMMReg( gregOfRexRM(pfx,rm), unop(op, mkexpr(oldG1)) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( oldG1, 
              binop( Iop_SetV128lo32,
                     mkexpr(oldG0),
                     loadLE(Ity_I32, mkexpr(addr)) ));
      putXMMReg( gregOfRexRM(pfx,rm), unop(op, mkexpr(oldG1)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* Lowest 64-bit lane only unary SSE operation, G = op(E). */

static ULong dis_SSE_E_to_G_unary_lo64 ( 
                VexAbiInfo* vbi,
                Prefix pfx, Long delta, 
                HChar* opname, IROp op
             )
{
   /* First we need to get the old G value and patch the low 64 bits
      of the E operand into it.  Then apply op and write back to G. */
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRTemp  oldG0 = newTemp(Ity_V128);
   IRTemp  oldG1 = newTemp(Ity_V128);

   assign( oldG0, getXMMReg(gregOfRexRM(pfx,rm)) );

   if (epartIsReg(rm)) {
      assign( oldG1, 
              binop( Iop_SetV128lo64,
                     mkexpr(oldG0),
                     getXMMRegLane64(eregOfRexRM(pfx,rm), 0)) );
      putXMMReg( gregOfRexRM(pfx,rm), unop(op, mkexpr(oldG1)) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( oldG1, 
              binop( Iop_SetV128lo64,
                     mkexpr(oldG0),
                     loadLE(Ity_I64, mkexpr(addr)) ));
      putXMMReg( gregOfRexRM(pfx,rm), unop(op, mkexpr(oldG1)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      return delta+alen;
   }
}


/* SSE integer binary operation:
      G = G `op` E   (eLeft == False)
      G = E `op` G   (eLeft == True)
*/
static ULong dis_SSEint_E_to_G( 
                VexAbiInfo* vbi,
                Prefix pfx, Long delta, 
                HChar* opname, IROp op,
                Bool   eLeft
             )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm = getUChar(delta);
   IRExpr* gpart = getXMMReg(gregOfRexRM(pfx,rm));
   IRExpr* epart = NULL;
   if (epartIsReg(rm)) {
      epart = getXMMReg(eregOfRexRM(pfx,rm));
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      delta += 1;
   } else {
      addr  = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      epart = loadLE(Ity_V128, mkexpr(addr));
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      delta += alen;
   }
   putXMMReg( gregOfRexRM(pfx,rm), 
              eLeft ? binop(op, epart, gpart)
	            : binop(op, gpart, epart) );
   return delta;
}


/* Helper for doing SSE FP comparisons.  False return ==> unhandled.
   This is all a bit of a kludge in that it ignores the subtleties of
   ordered-vs-unordered and signalling-vs-nonsignalling in the Intel
   spec. */
static Bool findSSECmpOp ( /*OUT*/Bool* preSwapP,
                           /*OUT*/IROp* opP,
                           /*OUT*/Bool* postNotP,
                           UInt imm8, Bool all_lanes, Int sz )
{
   if (imm8 >= 32) return False;

   /* First, compute a (preSwap, op, postNot) triple from
      the supplied imm8. */
   Bool pre = False;
   IROp op  = Iop_INVALID;
   Bool not = False;

#  define XXX(_pre, _op, _not) { pre = _pre; op = _op; not = _not; }
   switch (imm8) {
      case 0x0: XXX(False, Iop_CmpEQ32Fx4, False); break; // EQ
      case 0x1: XXX(False, Iop_CmpLT32Fx4, False); break; // LT
      case 0x2: XXX(False, Iop_CmpLE32Fx4, False); break; // LE
      case 0x3: XXX(False, Iop_CmpUN32Fx4, False); break; // UNORD
      case 0x4: XXX(False, Iop_CmpEQ32Fx4, True);  break; // NE
      case 0x5: XXX(False, Iop_CmpLT32Fx4, True);  break; // NLT
      case 0x6: XXX(False, Iop_CmpLE32Fx4, True);  break; // NLE
      case 0x7: XXX(False, Iop_CmpUN32Fx4, True);  break; // ORD
      /* "Enhanced Comparison Predicate[s] for VEX-Encoded [insns] */
      case 0xA: XXX(True,  Iop_CmpLT32Fx4, True);  break; // NGT_US
      case 0xC: XXX(False, Iop_CmpEQ32Fx4, True);  break; // NEQ_OQ
      case 0xD: XXX(True,  Iop_CmpLE32Fx4, False); break; // GE_OS
      case 0xE: XXX(True,  Iop_CmpLT32Fx4, False); break; // GT_OS
      /* Don't forget to add test cases to VCMPSS_128_<imm8> in
         avx-1.c if new cases turn up. */
      default: break;
   }
#  undef XXX
   if (op == Iop_INVALID) return False;

   /* Now convert the op into one with the same arithmetic but that is
      correct for the width and laneage requirements. */

   /**/ if (sz == 4 && all_lanes) {
      switch (op) {
         case Iop_CmpEQ32Fx4: op = Iop_CmpEQ32Fx4; break;
         case Iop_CmpLT32Fx4: op = Iop_CmpLT32Fx4; break;
         case Iop_CmpLE32Fx4: op = Iop_CmpLE32Fx4; break;
         case Iop_CmpUN32Fx4: op = Iop_CmpUN32Fx4; break;
         default: vassert(0);
      }
   }
   else if (sz == 4 && !all_lanes) {
      switch (op) {
         case Iop_CmpEQ32Fx4: op = Iop_CmpEQ32F0x4; break;
         case Iop_CmpLT32Fx4: op = Iop_CmpLT32F0x4; break;
         case Iop_CmpLE32Fx4: op = Iop_CmpLE32F0x4; break;
         case Iop_CmpUN32Fx4: op = Iop_CmpUN32F0x4; break;
         default: vassert(0);
      }
   }
   else if (sz == 8 && all_lanes) {
      switch (op) {
         case Iop_CmpEQ32Fx4: op = Iop_CmpEQ64Fx2; break;
         case Iop_CmpLT32Fx4: op = Iop_CmpLT64Fx2; break;
         case Iop_CmpLE32Fx4: op = Iop_CmpLE64Fx2; break;
         case Iop_CmpUN32Fx4: op = Iop_CmpUN64Fx2; break;
         default: vassert(0);
      }
   }
   else if (sz == 8 && !all_lanes) {
      switch (op) {
         case Iop_CmpEQ32Fx4: op = Iop_CmpEQ64F0x2; break;
         case Iop_CmpLT32Fx4: op = Iop_CmpLT64F0x2; break;
         case Iop_CmpLE32Fx4: op = Iop_CmpLE64F0x2; break;
         case Iop_CmpUN32Fx4: op = Iop_CmpUN64F0x2; break;
         default: vassert(0);
      }
   }
   else {
      vpanic("findSSECmpOp(amd64,guest)");
   }

   *preSwapP = pre; *opP = op; *postNotP = not;
   return True;
}


/* Handles SSE 32F/64F comparisons.  It can fail, in which case it
   returns the original delta to indicate failure. */

static Long dis_SSE_cmp_E_to_G ( VexAbiInfo* vbi,
                                 Prefix pfx, Long delta, 
                                 HChar* opname, Bool all_lanes, Int sz )
{
   Long    delta0 = delta;
   HChar   dis_buf[50];
   Int     alen;
   UInt    imm8;
   IRTemp  addr;
   Bool    preSwap = False;
   IROp    op      = Iop_INVALID;
   Bool    postNot = False;
   IRTemp  plain   = newTemp(Ity_V128);
   UChar   rm      = getUChar(delta);
   UShort  mask    = 0;
   vassert(sz == 4 || sz == 8);
   if (epartIsReg(rm)) {
      imm8 = getUChar(delta+1);
      if (imm8 >= 8) return delta0; /* FAIL */
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8, all_lanes, sz);
      if (!ok) return delta0; /* FAIL */
      vassert(!preSwap); /* never needed for imm8 < 8 */
      assign( plain, binop(op, getXMMReg(gregOfRexRM(pfx,rm)), 
                               getXMMReg(eregOfRexRM(pfx,rm))) );
      delta += 2;
      DIP("%s $%d,%s,%s\n", opname,
                            (Int)imm8,
                            nameXMMReg(eregOfRexRM(pfx,rm)),
                            nameXMMReg(gregOfRexRM(pfx,rm)) );
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8 = getUChar(delta+alen);
      if (imm8 >= 8) return delta0; /* FAIL */
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8, all_lanes, sz);
      if (!ok) return delta0; /* FAIL */
      vassert(!preSwap); /* never needed for imm8 < 8 */
      assign( plain, 
              binop(
                 op,
                 getXMMReg(gregOfRexRM(pfx,rm)), 
                   all_lanes 
                      ? loadLE(Ity_V128, mkexpr(addr))
                   : sz == 8
                      ? unop( Iop_64UtoV128, loadLE(Ity_I64, mkexpr(addr)))
                   : /*sz==4*/
                      unop( Iop_32UtoV128, loadLE(Ity_I32, mkexpr(addr)))
	      ) 
      );
      delta += alen+1;
      DIP("%s $%d,%s,%s\n", opname,
                            (Int)imm8,
                            dis_buf,
                            nameXMMReg(gregOfRexRM(pfx,rm)) );
   }

   if (postNot && all_lanes) {
      putXMMReg( gregOfRexRM(pfx,rm), 
                 unop(Iop_NotV128, mkexpr(plain)) );
   }
   else
   if (postNot && !all_lanes) {
      mask = toUShort(sz==4 ? 0x000F : 0x00FF);
      putXMMReg( gregOfRexRM(pfx,rm), 
                 binop(Iop_XorV128, mkexpr(plain), mkV128(mask)) );
   }
   else {
      putXMMReg( gregOfRexRM(pfx,rm), mkexpr(plain) );
   }

   return delta;
}


/* Vector by scalar shift of G by the amount specified at the bottom
   of E. */

static ULong dis_SSE_shiftG_byE ( VexAbiInfo* vbi,
                                  Prefix pfx, Long delta, 
                                  HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen, size;
   IRTemp  addr;
   Bool    shl, shr, sar;
   UChar   rm   = getUChar(delta);
   IRTemp  g0   = newTemp(Ity_V128);
   IRTemp  g1   = newTemp(Ity_V128);
   IRTemp  amt  = newTemp(Ity_I64);
   IRTemp  amt8 = newTemp(Ity_I8);
   if (epartIsReg(rm)) {
      assign( amt, getXMMRegLane64(eregOfRexRM(pfx,rm), 0) );
      DIP("%s %s,%s\n", opname,
                        nameXMMReg(eregOfRexRM(pfx,rm)),
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      delta++;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( amt, loadLE(Ity_I64, mkexpr(addr)) );
      DIP("%s %s,%s\n", opname,
                        dis_buf,
                        nameXMMReg(gregOfRexRM(pfx,rm)) );
      delta += alen;
   }
   assign( g0,   getXMMReg(gregOfRexRM(pfx,rm)) );
   assign( amt8, unop(Iop_64to8, mkexpr(amt)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x8: shl = True; size = 32; break;
      case Iop_ShlN32x4: shl = True; size = 32; break;
      case Iop_ShlN64x2: shl = True; size = 64; break;
      case Iop_SarN16x8: sar = True; size = 16; break;
      case Iop_SarN32x4: sar = True; size = 32; break;
      case Iop_ShrN16x8: shr = True; size = 16; break;
      case Iop_ShrN32x4: shr = True; size = 32; break;
      case Iop_ShrN64x2: shr = True; size = 64; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,
                binop(Iop_CmpLT64U, mkexpr(amt), mkU64(size))),
           mkV128(0x0000),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else 
   if (sar) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,
                binop(Iop_CmpLT64U, mkexpr(amt), mkU64(size))),
           binop(op, mkexpr(g0), mkU8(size-1)),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else {
      vassert(0);
   }

   putXMMReg( gregOfRexRM(pfx,rm), mkexpr(g1) );
   return delta;
}


/* Vector by scalar shift of E by an immediate byte. */

static 
ULong dis_SSE_shiftE_imm ( Prefix pfx, 
                           Long delta, HChar* opname, IROp op )
{
   Bool    shl, shr, sar;
   UChar   rm   = getUChar(delta);
   IRTemp  e0   = newTemp(Ity_V128);
   IRTemp  e1   = newTemp(Ity_V128);
   UChar   amt, size;
   vassert(epartIsReg(rm));
   vassert(gregLO3ofRM(rm) == 2 
           || gregLO3ofRM(rm) == 4 || gregLO3ofRM(rm) == 6);
   amt = getUChar(delta+1);
   delta += 2;
   DIP("%s $%d,%s\n", opname,
                      (Int)amt,
                      nameXMMReg(eregOfRexRM(pfx,rm)) );
   assign( e0, getXMMReg(eregOfRexRM(pfx,rm)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x8: shl = True; size = 16; break;
      case Iop_ShlN32x4: shl = True; size = 32; break;
      case Iop_ShlN64x2: shl = True; size = 64; break;
      case Iop_SarN16x8: sar = True; size = 16; break;
      case Iop_SarN32x4: sar = True; size = 32; break;
      case Iop_ShrN16x8: shr = True; size = 16; break;
      case Iop_ShrN32x4: shr = True; size = 32; break;
      case Iop_ShrN64x2: shr = True; size = 64; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( e1, amt >= size 
                    ? mkV128(0x0000)
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else 
   if (sar) {
     assign( e1, amt >= size 
                    ? binop(op, mkexpr(e0), mkU8(size-1))
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else {
      vassert(0);
   }

   putXMMReg( eregOfRexRM(pfx,rm), mkexpr(e1) );
   return delta;
}


/* Get the current SSE rounding mode. */

static IRExpr* /* :: Ity_I32 */ get_sse_roundingmode ( void )
{
   return 
      unop( Iop_64to32, 
            binop( Iop_And64, 
                   IRExpr_Get( OFFB_SSEROUND, Ity_I64 ), 
                   mkU64(3) ));
}

static void put_sse_roundingmode ( IRExpr* sseround )
{
   vassert(typeOfIRExpr(irsb->tyenv, sseround) == Ity_I32);
   stmt( IRStmt_Put( OFFB_SSEROUND, 
                     unop(Iop_32Uto64,sseround) ) );
}

/* Break a V128-bit value up into four 32-bit ints. */

static void breakupV128to32s ( IRTemp t128,
                               /*OUTs*/
                               IRTemp* t3, IRTemp* t2,
                               IRTemp* t1, IRTemp* t0 )
{
   IRTemp hi64 = newTemp(Ity_I64);
   IRTemp lo64 = newTemp(Ity_I64);
   assign( hi64, unop(Iop_V128HIto64, mkexpr(t128)) );
   assign( lo64, unop(Iop_V128to64,   mkexpr(t128)) );

   vassert(t0 && *t0 == IRTemp_INVALID);
   vassert(t1 && *t1 == IRTemp_INVALID);
   vassert(t2 && *t2 == IRTemp_INVALID);
   vassert(t3 && *t3 == IRTemp_INVALID);

   *t0 = newTemp(Ity_I32);
   *t1 = newTemp(Ity_I32);
   *t2 = newTemp(Ity_I32);
   *t3 = newTemp(Ity_I32);
   assign( *t0, unop(Iop_64to32,   mkexpr(lo64)) );
   assign( *t1, unop(Iop_64HIto32, mkexpr(lo64)) );
   assign( *t2, unop(Iop_64to32,   mkexpr(hi64)) );
   assign( *t3, unop(Iop_64HIto32, mkexpr(hi64)) );
}

/* Construct a V128-bit value from four 32-bit ints. */

static IRExpr* mkV128from32s ( IRTemp t3, IRTemp t2,
                               IRTemp t1, IRTemp t0 )
{
   return
      binop( Iop_64HLtoV128,
             binop(Iop_32HLto64, mkexpr(t3), mkexpr(t2)),
             binop(Iop_32HLto64, mkexpr(t1), mkexpr(t0))
   );
}

/* Break a 64-bit value up into four 16-bit ints. */

static void breakup64to16s ( IRTemp t64,
                             /*OUTs*/
                             IRTemp* t3, IRTemp* t2,
                             IRTemp* t1, IRTemp* t0 )
{
   IRTemp hi32 = newTemp(Ity_I32);
   IRTemp lo32 = newTemp(Ity_I32);
   assign( hi32, unop(Iop_64HIto32, mkexpr(t64)) );
   assign( lo32, unop(Iop_64to32,   mkexpr(t64)) );

   vassert(t0 && *t0 == IRTemp_INVALID);
   vassert(t1 && *t1 == IRTemp_INVALID);
   vassert(t2 && *t2 == IRTemp_INVALID);
   vassert(t3 && *t3 == IRTemp_INVALID);

   *t0 = newTemp(Ity_I16);
   *t1 = newTemp(Ity_I16);
   *t2 = newTemp(Ity_I16);
   *t3 = newTemp(Ity_I16);
   assign( *t0, unop(Iop_32to16,   mkexpr(lo32)) );
   assign( *t1, unop(Iop_32HIto16, mkexpr(lo32)) );
   assign( *t2, unop(Iop_32to16,   mkexpr(hi32)) );
   assign( *t3, unop(Iop_32HIto16, mkexpr(hi32)) );
}

/* Construct a 64-bit value from four 16-bit ints. */

static IRExpr* mk64from16s ( IRTemp t3, IRTemp t2,
                             IRTemp t1, IRTemp t0 )
{
   return
      binop( Iop_32HLto64,
             binop(Iop_16HLto32, mkexpr(t3), mkexpr(t2)),
             binop(Iop_16HLto32, mkexpr(t1), mkexpr(t0))
   );
}

/* Break a V256-bit value up into four 64-bit ints. */

static void breakupV256to64s ( IRTemp t256,
                               /*OUTs*/
                               IRTemp* t3, IRTemp* t2,
                               IRTemp* t1, IRTemp* t0 )
{ 
   vassert(t0 && *t0 == IRTemp_INVALID);
   vassert(t1 && *t1 == IRTemp_INVALID);
   vassert(t2 && *t2 == IRTemp_INVALID);
   vassert(t3 && *t3 == IRTemp_INVALID);
   *t0 = newTemp(Ity_I64);
   *t1 = newTemp(Ity_I64);
   *t2 = newTemp(Ity_I64);
   *t3 = newTemp(Ity_I64);
   assign( *t0, unop(Iop_V256to64_0, mkexpr(t256)) );
   assign( *t1, unop(Iop_V256to64_1, mkexpr(t256)) );
   assign( *t2, unop(Iop_V256to64_2, mkexpr(t256)) );
   assign( *t3, unop(Iop_V256to64_3, mkexpr(t256)) );
}

/* Break a V256-bit value up into two V128s. */

static void breakupV256toV128s ( IRTemp t256,
                                 /*OUTs*/
                                 IRTemp* t1, IRTemp* t0 )
{ 
   vassert(t0 && *t0 == IRTemp_INVALID);
   vassert(t1 && *t1 == IRTemp_INVALID);
   *t0 = newTemp(Ity_V128);
   *t1 = newTemp(Ity_V128);
   assign(*t1, unop(Iop_V256toV128_1, mkexpr(t256)));
   assign(*t0, unop(Iop_V256toV128_0, mkexpr(t256)));
}

/* Break a V256-bit value up into eight 32-bit ints.  */

static void breakupV256to32s ( IRTemp t256,
                               /*OUTs*/
                               IRTemp* t7, IRTemp* t6,
                               IRTemp* t5, IRTemp* t4,
                               IRTemp* t3, IRTemp* t2,
                               IRTemp* t1, IRTemp* t0 )
{
   IRTemp t128_1 = IRTemp_INVALID;
   IRTemp t128_0 = IRTemp_INVALID;
   breakupV256toV128s( t256, &t128_1, &t128_0 );
   breakupV128to32s( t128_1, t7, t6, t5, t4 );
   breakupV128to32s( t128_0, t3, t2, t1, t0 );
}

/* Break a V128-bit value up into two 64-bit ints. */

static void breakupV128to64s ( IRTemp t128,
                               /*OUTs*/
                               IRTemp* t1, IRTemp* t0 )
{
   vassert(t0 && *t0 == IRTemp_INVALID);
   vassert(t1 && *t1 == IRTemp_INVALID);
   *t0 = newTemp(Ity_I64);
   *t1 = newTemp(Ity_I64);
   assign( *t0, unop(Iop_V128to64,   mkexpr(t128)) );
   assign( *t1, unop(Iop_V128HIto64, mkexpr(t128)) );
}

/* Construct a V256-bit value from eight 32-bit ints. */

static IRExpr* mkV256from32s ( IRTemp t7, IRTemp t6,
                               IRTemp t5, IRTemp t4,
                               IRTemp t3, IRTemp t2,
                               IRTemp t1, IRTemp t0 )
{
   return
      binop( Iop_V128HLtoV256,
             binop( Iop_64HLtoV128,
                    binop(Iop_32HLto64, mkexpr(t7), mkexpr(t6)),
                    binop(Iop_32HLto64, mkexpr(t5), mkexpr(t4)) ),
             binop( Iop_64HLtoV128,
                    binop(Iop_32HLto64, mkexpr(t3), mkexpr(t2)),
                    binop(Iop_32HLto64, mkexpr(t1), mkexpr(t0)) )
   );
}

/* Construct a V256-bit value from four 64-bit ints. */

static IRExpr* mkV256from64s ( IRTemp t3, IRTemp t2,
                               IRTemp t1, IRTemp t0 )
{
   return
      binop( Iop_V128HLtoV256,
             binop(Iop_64HLtoV128, mkexpr(t3), mkexpr(t2)),
             binop(Iop_64HLtoV128, mkexpr(t1), mkexpr(t0))
   );
}

/* Helper for the SSSE3 (not SSE3) PMULHRSW insns.  Given two 64-bit
   values (aa,bb), computes, for each of the 4 16-bit lanes:

   (((aa_lane *s32 bb_lane) >>u 14) + 1) >>u 1
*/
static IRExpr* dis_PMULHRSW_helper ( IRExpr* aax, IRExpr* bbx )
{
   IRTemp aa      = newTemp(Ity_I64);
   IRTemp bb      = newTemp(Ity_I64);
   IRTemp aahi32s = newTemp(Ity_I64);
   IRTemp aalo32s = newTemp(Ity_I64);
   IRTemp bbhi32s = newTemp(Ity_I64);
   IRTemp bblo32s = newTemp(Ity_I64);
   IRTemp rHi     = newTemp(Ity_I64);
   IRTemp rLo     = newTemp(Ity_I64);
   IRTemp one32x2 = newTemp(Ity_I64);
   assign(aa, aax);
   assign(bb, bbx);
   assign( aahi32s,
           binop(Iop_SarN32x2,
                 binop(Iop_InterleaveHI16x4, mkexpr(aa), mkexpr(aa)),
                 mkU8(16) ));
   assign( aalo32s,
           binop(Iop_SarN32x2,
                 binop(Iop_InterleaveLO16x4, mkexpr(aa), mkexpr(aa)),
                 mkU8(16) ));
   assign( bbhi32s,
           binop(Iop_SarN32x2,
                 binop(Iop_InterleaveHI16x4, mkexpr(bb), mkexpr(bb)),
                 mkU8(16) ));
   assign( bblo32s,
           binop(Iop_SarN32x2,
                 binop(Iop_InterleaveLO16x4, mkexpr(bb), mkexpr(bb)),
                 mkU8(16) ));
   assign(one32x2, mkU64( (1ULL << 32) + 1 ));
   assign(
      rHi,
      binop(
         Iop_ShrN32x2,
         binop(
            Iop_Add32x2, 
            binop(
               Iop_ShrN32x2,
               binop(Iop_Mul32x2, mkexpr(aahi32s), mkexpr(bbhi32s)),
               mkU8(14)
            ),
            mkexpr(one32x2)
         ),
         mkU8(1)
      )
   );
   assign(
      rLo,
      binop(
         Iop_ShrN32x2,
         binop(
            Iop_Add32x2, 
            binop(
               Iop_ShrN32x2,
               binop(Iop_Mul32x2, mkexpr(aalo32s), mkexpr(bblo32s)),
               mkU8(14)
            ),
            mkexpr(one32x2)
         ),
         mkU8(1)
      )
   );
   return
      binop(Iop_CatEvenLanes16x4, mkexpr(rHi), mkexpr(rLo));
}

/* Helper for the SSSE3 (not SSE3) PSIGN{B,W,D} insns.  Given two 64-bit
   values (aa,bb), computes, for each lane:

          if aa_lane < 0 then - bb_lane
     else if aa_lane > 0 then bb_lane
     else 0
*/
static IRExpr* dis_PSIGN_helper ( IRExpr* aax, IRExpr* bbx, Int laneszB )
{
   IRTemp aa       = newTemp(Ity_I64);
   IRTemp bb       = newTemp(Ity_I64);
   IRTemp zero     = newTemp(Ity_I64);
   IRTemp bbNeg    = newTemp(Ity_I64);
   IRTemp negMask  = newTemp(Ity_I64);
   IRTemp posMask  = newTemp(Ity_I64);
   IROp   opSub    = Iop_INVALID;
   IROp   opCmpGTS = Iop_INVALID;

   switch (laneszB) {
      case 1: opSub = Iop_Sub8x8;  opCmpGTS = Iop_CmpGT8Sx8;  break;
      case 2: opSub = Iop_Sub16x4; opCmpGTS = Iop_CmpGT16Sx4; break;
      case 4: opSub = Iop_Sub32x2; opCmpGTS = Iop_CmpGT32Sx2; break;
      default: vassert(0);
   }

   assign( aa,      aax );
   assign( bb,      bbx );
   assign( zero,    mkU64(0) );
   assign( bbNeg,   binop(opSub,    mkexpr(zero), mkexpr(bb)) );
   assign( negMask, binop(opCmpGTS, mkexpr(zero), mkexpr(aa)) );
   assign( posMask, binop(opCmpGTS, mkexpr(aa),   mkexpr(zero)) );

   return
      binop(Iop_Or64,
            binop(Iop_And64, mkexpr(bb),    mkexpr(posMask)),
            binop(Iop_And64, mkexpr(bbNeg), mkexpr(negMask)) );

}


/* Helper for the SSSE3 (not SSE3) PABS{B,W,D} insns.  Given a 64-bit
   value aa, computes, for each lane

   if aa < 0 then -aa else aa

   Note that the result is interpreted as unsigned, so that the
   absolute value of the most negative signed input can be
   represented.
*/
static IRTemp math_PABS_MMX ( IRTemp aa, Int laneszB )
{
   IRTemp res     = newTemp(Ity_I64);
   IRTemp zero    = newTemp(Ity_I64);
   IRTemp aaNeg   = newTemp(Ity_I64);
   IRTemp negMask = newTemp(Ity_I64);
   IRTemp posMask = newTemp(Ity_I64);
   IROp   opSub   = Iop_INVALID;
   IROp   opSarN  = Iop_INVALID;

   switch (laneszB) {
      case 1: opSub = Iop_Sub8x8;  opSarN = Iop_SarN8x8;  break;
      case 2: opSub = Iop_Sub16x4; opSarN = Iop_SarN16x4; break;
      case 4: opSub = Iop_Sub32x2; opSarN = Iop_SarN32x2; break;
      default: vassert(0);
   }

   assign( negMask, binop(opSarN, mkexpr(aa), mkU8(8*laneszB-1)) );
   assign( posMask, unop(Iop_Not64, mkexpr(negMask)) );
   assign( zero,    mkU64(0) );
   assign( aaNeg,   binop(opSub, mkexpr(zero), mkexpr(aa)) );
   assign( res,
           binop(Iop_Or64,
                 binop(Iop_And64, mkexpr(aa),    mkexpr(posMask)),
                 binop(Iop_And64, mkexpr(aaNeg), mkexpr(negMask)) ));
   return res;
}

/* XMM version of math_PABS_MMX. */
static IRTemp math_PABS_XMM ( IRTemp aa, Int laneszB )
{
   IRTemp res  = newTemp(Ity_V128);
   IRTemp aaHi = newTemp(Ity_I64);
   IRTemp aaLo = newTemp(Ity_I64);
   assign(aaHi, unop(Iop_V128HIto64, mkexpr(aa)));
   assign(aaLo, unop(Iop_V128to64, mkexpr(aa)));
   assign(res, binop(Iop_64HLtoV128,
                     mkexpr(math_PABS_MMX(aaHi, laneszB)),
                     mkexpr(math_PABS_MMX(aaLo, laneszB))));
   return res;
}

/* Specialisations of math_PABS_XMM, since there's no easy way to do
   partial applications in C :-( */
static IRTemp math_PABS_XMM_pap4 ( IRTemp aa ) {
   return math_PABS_XMM(aa, 4);
}

static IRTemp math_PABS_XMM_pap2 ( IRTemp aa ) {
   return math_PABS_XMM(aa, 2);
}

static IRTemp math_PABS_XMM_pap1 ( IRTemp aa ) {
   return math_PABS_XMM(aa, 1);
}

static IRExpr* dis_PALIGNR_XMM_helper ( IRTemp hi64,
                                        IRTemp lo64, Long byteShift )
{
   vassert(byteShift >= 1 && byteShift <= 7);
   return
      binop(Iop_Or64,
            binop(Iop_Shl64, mkexpr(hi64), mkU8(8*(8-byteShift))),
            binop(Iop_Shr64, mkexpr(lo64), mkU8(8*byteShift))
      );
}

static IRTemp math_PALIGNR_XMM ( IRTemp sV, IRTemp dV, UInt imm8 ) 
{
   IRTemp res = newTemp(Ity_V128);
   IRTemp sHi = newTemp(Ity_I64);
   IRTemp sLo = newTemp(Ity_I64);
   IRTemp dHi = newTemp(Ity_I64);
   IRTemp dLo = newTemp(Ity_I64);
   IRTemp rHi = newTemp(Ity_I64);
   IRTemp rLo = newTemp(Ity_I64);

   assign( dHi, unop(Iop_V128HIto64, mkexpr(dV)) );
   assign( dLo, unop(Iop_V128to64,   mkexpr(dV)) );
   assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );

   if (imm8 == 0) {
      assign( rHi, mkexpr(sHi) );
      assign( rLo, mkexpr(sLo) );
   }
   else if (imm8 >= 1 && imm8 <= 7) {
      assign( rHi, dis_PALIGNR_XMM_helper(dLo, sHi, imm8) );
      assign( rLo, dis_PALIGNR_XMM_helper(sHi, sLo, imm8) );
   }
   else if (imm8 == 8) {
      assign( rHi, mkexpr(dLo) );
      assign( rLo, mkexpr(sHi) );
   }
   else if (imm8 >= 9 && imm8 <= 15) {
      assign( rHi, dis_PALIGNR_XMM_helper(dHi, dLo, imm8-8) );
      assign( rLo, dis_PALIGNR_XMM_helper(dLo, sHi, imm8-8) );
   }
   else if (imm8 == 16) {
      assign( rHi, mkexpr(dHi) );
      assign( rLo, mkexpr(dLo) );
   }
   else if (imm8 >= 17 && imm8 <= 23) {
      assign( rHi, binop(Iop_Shr64, mkexpr(dHi), mkU8(8*(imm8-16))) );
      assign( rLo, dis_PALIGNR_XMM_helper(dHi, dLo, imm8-16) );
   }
   else if (imm8 == 24) {
      assign( rHi, mkU64(0) );
      assign( rLo, mkexpr(dHi) );
   }
   else if (imm8 >= 25 && imm8 <= 31) {
      assign( rHi, mkU64(0) );
      assign( rLo, binop(Iop_Shr64, mkexpr(dHi), mkU8(8*(imm8-24))) );
   }
   else if (imm8 >= 32 && imm8 <= 255) {
      assign( rHi, mkU64(0) );
      assign( rLo, mkU64(0) );
   }
   else
      vassert(0);

   assign( res, binop(Iop_64HLtoV128, mkexpr(rHi), mkexpr(rLo)));
   return res;
}


/* Generate a SIGSEGV followed by a restart of the current instruction
   if effective_addr is not 16-aligned.  This is required behaviour
   for some SSE3 instructions and all 128-bit SSSE3 instructions.
   This assumes that guest_RIP_curr_instr is set correctly! */
static
void gen_SEGV_if_not_XX_aligned ( IRTemp effective_addr, ULong mask )
{
   stmt(
      IRStmt_Exit(
         binop(Iop_CmpNE64,
               binop(Iop_And64,mkexpr(effective_addr),mkU64(mask)),
               mkU64(0)),
         Ijk_SigSEGV,
         IRConst_U64(guest_RIP_curr_instr),
         OFFB_RIP
      )
   );
}

static void gen_SEGV_if_not_16_aligned ( IRTemp effective_addr ) {
   gen_SEGV_if_not_XX_aligned(effective_addr, 16-1);
}

static void gen_SEGV_if_not_32_aligned ( IRTemp effective_addr ) {
   gen_SEGV_if_not_XX_aligned(effective_addr, 32-1);
}

/* Helper for deciding whether a given insn (starting at the opcode
   byte) may validly be used with a LOCK prefix.  The following insns
   may be used with LOCK when their destination operand is in memory.
   AFAICS this is exactly the same for both 32-bit and 64-bit mode.

   ADD        80 /0,  81 /0,  82 /0,  83 /0,  00,  01
   OR         80 /1,  81 /1,  82 /x,  83 /1,  08,  09
   ADC        80 /2,  81 /2,  82 /2,  83 /2,  10,  11
   SBB        81 /3,  81 /3,  82 /x,  83 /3,  18,  19
   AND        80 /4,  81 /4,  82 /x,  83 /4,  20,  21
   SUB        80 /5,  81 /5,  82 /x,  83 /5,  28,  29
   XOR        80 /6,  81 /6,  82 /x,  83 /6,  30,  31

   DEC        FE /1,  FF /1
   INC        FE /0,  FF /0

   NEG        F6 /3,  F7 /3
   NOT        F6 /2,  F7 /2

   XCHG       86, 87

   BTC        0F BB,  0F BA /7
   BTR        0F B3,  0F BA /6
   BTS        0F AB,  0F BA /5

   CMPXCHG    0F B0,  0F B1
   CMPXCHG8B  0F C7 /1

   XADD       0F C0,  0F C1

   ------------------------------

   80 /0  =  addb $imm8,  rm8
   81 /0  =  addl $imm32, rm32  and  addw $imm16, rm16
   82 /0  =  addb $imm8,  rm8
   83 /0  =  addl $simm8, rm32  and  addw $simm8, rm16

   00     =  addb r8,  rm8
   01     =  addl r32, rm32  and  addw r16, rm16

   Same for ADD OR ADC SBB AND SUB XOR

   FE /1  = dec rm8
   FF /1  = dec rm32  and  dec rm16

   FE /0  = inc rm8
   FF /0  = inc rm32  and  inc rm16

   F6 /3  = neg rm8
   F7 /3  = neg rm32  and  neg rm16

   F6 /2  = not rm8
   F7 /2  = not rm32  and  not rm16

   0F BB     = btcw r16, rm16    and  btcl r32, rm32
   OF BA /7  = btcw $imm8, rm16  and  btcw $imm8, rm32

   Same for BTS, BTR
*/
static Bool can_be_used_with_LOCK_prefix ( UChar* opc )
{
   switch (opc[0]) {
      case 0x00: case 0x01: case 0x08: case 0x09:
      case 0x10: case 0x11: case 0x18: case 0x19:
      case 0x20: case 0x21: case 0x28: case 0x29:
      case 0x30: case 0x31:
         if (!epartIsReg(opc[1]))
            return True;
         break;

      case 0x80: case 0x81: case 0x82: case 0x83:
         if (gregLO3ofRM(opc[1]) >= 0 && gregLO3ofRM(opc[1]) <= 6
             && !epartIsReg(opc[1]))
            return True;
         break;

      case 0xFE: case 0xFF:
         if (gregLO3ofRM(opc[1]) >= 0 && gregLO3ofRM(opc[1]) <= 1
             && !epartIsReg(opc[1]))
            return True;
         break;

      case 0xF6: case 0xF7:
         if (gregLO3ofRM(opc[1]) >= 2 && gregLO3ofRM(opc[1]) <= 3
             && !epartIsReg(opc[1]))
            return True;
         break;

      case 0x86: case 0x87:
         if (!epartIsReg(opc[1]))
            return True;
         break;

      case 0x0F: {
         switch (opc[1]) {
            case 0xBB: case 0xB3: case 0xAB:
               if (!epartIsReg(opc[2]))
                  return True;
               break;
            case 0xBA: 
               if (gregLO3ofRM(opc[2]) >= 5 && gregLO3ofRM(opc[2]) <= 7
                   && !epartIsReg(opc[2]))
                  return True;
               break;
            case 0xB0: case 0xB1:
               if (!epartIsReg(opc[2]))
                  return True;
               break;
            case 0xC7: 
               if (gregLO3ofRM(opc[2]) == 1 && !epartIsReg(opc[2]) )
                  return True;
               break;
            case 0xC0: case 0xC1:
               if (!epartIsReg(opc[2]))
                  return True;
               break;
            default:
               break;
         } /* switch (opc[1]) */
         break;
      }

      default:
         break;
   } /* switch (opc[0]) */

   return False;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSE/SSE2: dis_ESC_0F__SSE2                 ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static Long dis_COMISD ( VexAbiInfo* vbi, Prefix pfx,
                         Long delta, Bool isAvx, UChar opc )
{
   vassert(opc == 0x2F/*COMISD*/ || opc == 0x2E/*UCOMISD*/);
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp argL  = newTemp(Ity_F64);
   IRTemp argR  = newTemp(Ity_F64);
   UChar  modrm = getUChar(delta);
   IRTemp addr  = IRTemp_INVALID;
   if (epartIsReg(modrm)) {
      assign( argR, getXMMRegLane64F( eregOfRexRM(pfx,modrm), 
                                      0/*lowest lane*/ ) );
      delta += 1;
      DIP("%s%scomisd %s,%s\n", isAvx ? "v" : "",
                                opc==0x2E ? "u" : "",
                                nameXMMReg(eregOfRexRM(pfx,modrm)),
                                nameXMMReg(gregOfRexRM(pfx,modrm)) );
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argR, loadLE(Ity_F64, mkexpr(addr)) );
      delta += alen;
      DIP("%s%scomisd %s,%s\n", isAvx ? "v" : "",
                                opc==0x2E ? "u" : "",
                                dis_buf,
                                nameXMMReg(gregOfRexRM(pfx,modrm)) );
   }
   assign( argL, getXMMRegLane64F( gregOfRexRM(pfx,modrm), 
                                   0/*lowest lane*/ ) );

   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            binop( Iop_And64,
                   unop( Iop_32Uto64, 
                         binop(Iop_CmpF64, mkexpr(argL), mkexpr(argR)) ),
                   mkU64(0x45)
       )));
   return delta;
}


static Long dis_COMISS ( VexAbiInfo* vbi, Prefix pfx,
                         Long delta, Bool isAvx, UChar opc )
{
   vassert(opc == 0x2F/*COMISS*/ || opc == 0x2E/*UCOMISS*/);
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp argL  = newTemp(Ity_F32);
   IRTemp argR  = newTemp(Ity_F32);
   UChar  modrm = getUChar(delta);
   IRTemp addr  = IRTemp_INVALID;
   if (epartIsReg(modrm)) {
      assign( argR, getXMMRegLane32F( eregOfRexRM(pfx,modrm), 
                                      0/*lowest lane*/ ) );
      delta += 1;
      DIP("%s%scomiss %s,%s\n", isAvx ? "v" : "",
                                opc==0x2E ? "u" : "",
                                nameXMMReg(eregOfRexRM(pfx,modrm)),
                                nameXMMReg(gregOfRexRM(pfx,modrm)) );
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argR, loadLE(Ity_F32, mkexpr(addr)) );
      delta += alen;
      DIP("%s%scomiss %s,%s\n", isAvx ? "v" : "",
                                opc==0x2E ? "u" : "",
                                dis_buf,
                                nameXMMReg(gregOfRexRM(pfx,modrm)) );
   }
   assign( argL, getXMMRegLane32F( gregOfRexRM(pfx,modrm), 
                                   0/*lowest lane*/ ) );

   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( 
            OFFB_CC_DEP1,
            binop( Iop_And64,
                   unop( Iop_32Uto64, 
                         binop(Iop_CmpF64, 
                               unop(Iop_F32toF64,mkexpr(argL)),
                               unop(Iop_F32toF64,mkexpr(argR)))),
                   mkU64(0x45)
       )));
   return delta;
}


static Long dis_PSHUFD_32x4 ( VexAbiInfo* vbi, Prefix pfx,
                              Long delta, Bool writesYmm )
{
   Int    order;
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp sV    = newTemp(Ity_V128);
   UChar  modrm = getUChar(delta);
   HChar* strV  = writesYmm ? "v" : "";
   IRTemp addr  = IRTemp_INVALID;
   if (epartIsReg(modrm)) {
      assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
      order = (Int)getUChar(delta+1);
      delta += 1+1;
      DIP("%spshufd $%d,%s,%s\n", strV, order, 
                                  nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 
                        1/*byte after the amode*/ );
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      order = (Int)getUChar(delta+alen);
      delta += alen+1;
      DIP("%spshufd $%d,%s,%s\n", strV, order, 
                                 dis_buf,
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
   }

   IRTemp s3, s2, s1, s0;
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );

#  define SEL(n)  ((n)==0 ? s0 : ((n)==1 ? s1 : ((n)==2 ? s2 : s3)))
   IRTemp dV = newTemp(Ity_V128);
   assign(dV,
          mkV128from32s( SEL((order>>6)&3), SEL((order>>4)&3),
                         SEL((order>>2)&3), SEL((order>>0)&3) )
   );
#  undef SEL

   (writesYmm ? putYMMRegLoAndZU : putXMMReg)
      (gregOfRexRM(pfx,modrm), mkexpr(dV));
   return delta;
}


static IRTemp math_PSRLDQ ( IRTemp sV, Int imm )
{
   IRTemp dV    = newTemp(Ity_V128);
   IRTemp hi64  = newTemp(Ity_I64);
   IRTemp lo64  = newTemp(Ity_I64);
   IRTemp hi64r = newTemp(Ity_I64);
   IRTemp lo64r = newTemp(Ity_I64);

   vassert(imm >= 0 && imm <= 255);
   if (imm >= 16) {
      assign(dV, mkV128(0x0000));
      return dV;
   }

   assign( hi64, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( lo64, unop(Iop_V128to64, mkexpr(sV)) );

   if (imm == 0) {
      assign( lo64r, mkexpr(lo64) );
      assign( hi64r, mkexpr(hi64) );
   }
   else
   if (imm == 8) {
      assign( hi64r, mkU64(0) );
      assign( lo64r, mkexpr(hi64) );
   }
   else 
   if (imm > 8) {
      assign( hi64r, mkU64(0) );
      assign( lo64r, binop( Iop_Shr64, mkexpr(hi64), mkU8( 8*(imm-8) ) ));
   } else {
      assign( hi64r, binop( Iop_Shr64, mkexpr(hi64), mkU8(8 * imm) ));
      assign( lo64r, 
              binop( Iop_Or64,
                     binop(Iop_Shr64, mkexpr(lo64), 
                           mkU8(8 * imm)),
                     binop(Iop_Shl64, mkexpr(hi64),
                           mkU8(8 * (8 - imm)) )
                     )
              );
   }
   
   assign( dV, binop(Iop_64HLtoV128, mkexpr(hi64r), mkexpr(lo64r)) );
   return dV;
}


static IRTemp math_PSLLDQ ( IRTemp sV, Int imm )
{
   IRTemp       dV    = newTemp(Ity_V128);
   IRTemp       hi64  = newTemp(Ity_I64);
   IRTemp       lo64  = newTemp(Ity_I64);
   IRTemp       hi64r = newTemp(Ity_I64);
   IRTemp       lo64r = newTemp(Ity_I64);

   vassert(imm >= 0 && imm <= 255);
   if (imm >= 16) {
      assign(dV, mkV128(0x0000));
      return dV;
   }

   assign( hi64, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( lo64, unop(Iop_V128to64, mkexpr(sV)) );
   
   if (imm == 0) {
      assign( lo64r, mkexpr(lo64) );
      assign( hi64r, mkexpr(hi64) );
   }
   else
   if (imm == 8) {
      assign( lo64r, mkU64(0) );
      assign( hi64r, mkexpr(lo64) );
   }
   else
   if (imm > 8) {
      assign( lo64r, mkU64(0) );
      assign( hi64r, binop( Iop_Shl64, mkexpr(lo64), mkU8( 8*(imm-8) ) ));
   } else {
      assign( lo64r, binop( Iop_Shl64, mkexpr(lo64), mkU8(8 * imm) ));
      assign( hi64r, 
              binop( Iop_Or64,
                     binop(Iop_Shl64, mkexpr(hi64), 
                           mkU8(8 * imm)),
                     binop(Iop_Shr64, mkexpr(lo64),
                           mkU8(8 * (8 - imm)) )
                     )
              );
   }

   assign( dV, binop(Iop_64HLtoV128, mkexpr(hi64r), mkexpr(lo64r)) );
   return dV;
}


static Long dis_CVTxSD2SI ( VexAbiInfo* vbi, Prefix pfx,
                            Long delta, Bool isAvx, UChar opc, Int sz )
{
   vassert(opc == 0x2D/*CVTSD2SI*/ || opc == 0x2C/*CVTTSD2SI*/);
   HChar  dis_buf[50];
   Int    alen   = 0;
   UChar  modrm  = getUChar(delta);
   IRTemp addr   = IRTemp_INVALID;
   IRTemp rmode  = newTemp(Ity_I32);
   IRTemp f64lo  = newTemp(Ity_F64);
   Bool   r2zero = toBool(opc == 0x2C);

   if (epartIsReg(modrm)) {
      delta += 1;
      assign(f64lo, getXMMRegLane64F(eregOfRexRM(pfx,modrm), 0));
      DIP("%scvt%ssd2si %s,%s\n", isAvx ? "v" : "", r2zero ? "t" : "",
                                  nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameIReg(sz, gregOfRexRM(pfx,modrm),
                                           False));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(f64lo, loadLE(Ity_F64, mkexpr(addr)));
      delta += alen;
      DIP("%scvt%ssd2si %s,%s\n", isAvx ? "v" : "", r2zero ? "t" : "",
                                  dis_buf,
                                  nameIReg(sz, gregOfRexRM(pfx,modrm),
                                           False));
   }

   if (r2zero) {
      assign( rmode, mkU32((UInt)Irrm_ZERO) );
   } else {
      assign( rmode, get_sse_roundingmode() );
   }

   if (sz == 4) {
      putIReg32( gregOfRexRM(pfx,modrm),
                 binop( Iop_F64toI32S, mkexpr(rmode), mkexpr(f64lo)) );
   } else {
      vassert(sz == 8);
      putIReg64( gregOfRexRM(pfx,modrm),
                 binop( Iop_F64toI64S, mkexpr(rmode), mkexpr(f64lo)) );
   }

   return delta;
}


static Long dis_CVTxSS2SI ( VexAbiInfo* vbi, Prefix pfx,
                            Long delta, Bool isAvx, UChar opc, Int sz )
{
   vassert(opc == 0x2D/*CVTSS2SI*/ || opc == 0x2C/*CVTTSS2SI*/);
   HChar  dis_buf[50];
   Int    alen   = 0;
   UChar  modrm  = getUChar(delta);
   IRTemp addr   = IRTemp_INVALID;
   IRTemp rmode  = newTemp(Ity_I32);
   IRTemp f32lo  = newTemp(Ity_F32);
   Bool   r2zero = toBool(opc == 0x2C);

   if (epartIsReg(modrm)) {
      delta += 1;
      assign(f32lo, getXMMRegLane32F(eregOfRexRM(pfx,modrm), 0));
      DIP("%scvt%sss2si %s,%s\n", isAvx ? "v" : "", r2zero ? "t" : "",
                                  nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameIReg(sz, gregOfRexRM(pfx,modrm), 
                                           False));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(f32lo, loadLE(Ity_F32, mkexpr(addr)));
      delta += alen;
      DIP("%scvt%sss2si %s,%s\n", isAvx ? "v" : "", r2zero ? "t" : "",
                                  dis_buf,
                                  nameIReg(sz, gregOfRexRM(pfx,modrm),
                                           False));
   }

   if (r2zero) {
      assign( rmode, mkU32((UInt)Irrm_ZERO) );
   } else {
      assign( rmode, get_sse_roundingmode() );
   }

   if (sz == 4) {
      putIReg32( gregOfRexRM(pfx,modrm),
                 binop( Iop_F64toI32S, 
                        mkexpr(rmode), 
                        unop(Iop_F32toF64, mkexpr(f32lo))) );
   } else {
      vassert(sz == 8);
      putIReg64( gregOfRexRM(pfx,modrm),
                 binop( Iop_F64toI64S, 
                        mkexpr(rmode), 
                        unop(Iop_F32toF64, mkexpr(f32lo))) );
   }
   
   return delta;
}


static Long dis_CVTPS2PD_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp f32lo = newTemp(Ity_F32);
   IRTemp f32hi = newTemp(Ity_F32);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( f32lo, getXMMRegLane32F(rE, 0) );
      assign( f32hi, getXMMRegLane32F(rE, 1) );
      delta += 1;
      DIP("%scvtps2pd %s,%s\n",
          isAvx ? "v" : "", nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( f32lo, loadLE(Ity_F32, mkexpr(addr)) );
      assign( f32hi, loadLE(Ity_F32, 
                            binop(Iop_Add64,mkexpr(addr),mkU64(4))) );
      delta += alen;
      DIP("%scvtps2pd %s,%s\n",
          isAvx ? "v" : "", dis_buf, nameXMMReg(rG));
   }

   putXMMRegLane64F( rG, 1, unop(Iop_F32toF64, mkexpr(f32hi)) );
   putXMMRegLane64F( rG, 0, unop(Iop_F32toF64, mkexpr(f32lo)) );
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0));
   return delta;
}


static Long dis_CVTPS2PD_256 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp f32_0 = newTemp(Ity_F32);
   IRTemp f32_1 = newTemp(Ity_F32);
   IRTemp f32_2 = newTemp(Ity_F32);
   IRTemp f32_3 = newTemp(Ity_F32);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( f32_0, getXMMRegLane32F(rE, 0) );
      assign( f32_1, getXMMRegLane32F(rE, 1) );
      assign( f32_2, getXMMRegLane32F(rE, 2) );
      assign( f32_3, getXMMRegLane32F(rE, 3) );
      delta += 1;
      DIP("vcvtps2pd %s,%s\n", nameXMMReg(rE), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( f32_0, loadLE(Ity_F32, mkexpr(addr)) );
      assign( f32_1, loadLE(Ity_F32, 
                            binop(Iop_Add64,mkexpr(addr),mkU64(4))) );
      assign( f32_2, loadLE(Ity_F32, 
                            binop(Iop_Add64,mkexpr(addr),mkU64(8))) );
      assign( f32_3, loadLE(Ity_F32, 
                            binop(Iop_Add64,mkexpr(addr),mkU64(12))) );
      delta += alen;
      DIP("vcvtps2pd %s,%s\n", dis_buf, nameYMMReg(rG));
   }

   putYMMRegLane64F( rG, 3, unop(Iop_F32toF64, mkexpr(f32_3)) );
   putYMMRegLane64F( rG, 2, unop(Iop_F32toF64, mkexpr(f32_2)) );
   putYMMRegLane64F( rG, 1, unop(Iop_F32toF64, mkexpr(f32_1)) );
   putYMMRegLane64F( rG, 0, unop(Iop_F32toF64, mkexpr(f32_0)) );
   return delta;
}


static Long dis_CVTPD2PS_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp argV  = newTemp(Ity_V128);
   IRTemp rmode = newTemp(Ity_I32);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getXMMReg(rE) );
      delta += 1;
      DIP("%scvtpd2ps %s,%s\n", isAvx ? "v" : "",
          nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("%scvtpd2ps %s,%s\n", isAvx ? "v" : "",
          dis_buf, nameXMMReg(rG) );
   }
         
   assign( rmode, get_sse_roundingmode() );
   IRTemp t0 = newTemp(Ity_F64);
   IRTemp t1 = newTemp(Ity_F64);
   assign( t0, unop(Iop_ReinterpI64asF64, 
                    unop(Iop_V128to64, mkexpr(argV))) );
   assign( t1, unop(Iop_ReinterpI64asF64, 
                    unop(Iop_V128HIto64, mkexpr(argV))) );
      
#  define CVT(_t)  binop( Iop_F64toF32, mkexpr(rmode), mkexpr(_t) )
   putXMMRegLane32(  rG, 3, mkU32(0) );
   putXMMRegLane32(  rG, 2, mkU32(0) );
   putXMMRegLane32F( rG, 1, CVT(t1) );
   putXMMRegLane32F( rG, 0, CVT(t0) );
#  undef CVT
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0) );

   return delta;
}


static Long dis_CVTxPS2DQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                                Long delta, Bool isAvx, Bool r2zero )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp argV  = newTemp(Ity_V128);
   IRTemp rmode = newTemp(Ity_I32);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1, t2, t3;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getXMMReg(rE) );
      delta += 1;
      DIP("%scvt%sps2dq %s,%s\n",
          isAvx ? "v" : "", r2zero ? "t" : "", nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("%scvt%sps2dq %s,%s\n",
          isAvx ? "v" : "", r2zero ? "t" : "", dis_buf, nameXMMReg(rG) );
   }

   assign( rmode, r2zero ? mkU32((UInt)Irrm_ZERO)
                         : get_sse_roundingmode() );
   t0 = t1 = t2 = t3 = IRTemp_INVALID;
   breakupV128to32s( argV, &t3, &t2, &t1, &t0 );
   /* This is less than ideal.  If it turns out to be a performance
      bottleneck it can be improved. */
#  define CVT(_t)                             \
      binop( Iop_F64toI32S,                   \
             mkexpr(rmode),                   \
             unop( Iop_F32toF64,              \
                   unop( Iop_ReinterpI32asF32, mkexpr(_t))) )
      
   putXMMRegLane32( rG, 3, CVT(t3) );
   putXMMRegLane32( rG, 2, CVT(t2) );
   putXMMRegLane32( rG, 1, CVT(t1) );
   putXMMRegLane32( rG, 0, CVT(t0) );
#  undef CVT
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0) );

   return delta;
}


static Long dis_CVTxPS2DQ_256 ( VexAbiInfo* vbi, Prefix pfx,
                                Long delta, Bool r2zero )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp argV  = newTemp(Ity_V256);
   IRTemp rmode = newTemp(Ity_I32);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1, t2, t3, t4, t5, t6, t7;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getYMMReg(rE) );
      delta += 1;
      DIP("vcvt%sps2dq %s,%s\n",
          r2zero ? "t" : "", nameYMMReg(rE), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V256, mkexpr(addr)) );
      delta += alen;
      DIP("vcvt%sps2dq %s,%s\n",
          r2zero ? "t" : "", dis_buf, nameYMMReg(rG) );
   }

   assign( rmode, r2zero ? mkU32((UInt)Irrm_ZERO)
                         : get_sse_roundingmode() );
   t0 = t1 = t2 = t3 = t4 = t5 = t6 = t7 = IRTemp_INVALID;
   breakupV256to32s( argV, &t7, &t6, &t5, &t4, &t3, &t2, &t1, &t0 );
   /* This is less than ideal.  If it turns out to be a performance
      bottleneck it can be improved. */
#  define CVT(_t)                             \
      binop( Iop_F64toI32S,                   \
             mkexpr(rmode),                   \
             unop( Iop_F32toF64,              \
                   unop( Iop_ReinterpI32asF32, mkexpr(_t))) )
      
   putYMMRegLane32( rG, 7, CVT(t7) );
   putYMMRegLane32( rG, 6, CVT(t6) );
   putYMMRegLane32( rG, 5, CVT(t5) );
   putYMMRegLane32( rG, 4, CVT(t4) );
   putYMMRegLane32( rG, 3, CVT(t3) );
   putYMMRegLane32( rG, 2, CVT(t2) );
   putYMMRegLane32( rG, 1, CVT(t1) );
   putYMMRegLane32( rG, 0, CVT(t0) );
#  undef CVT

   return delta;
}


static Long dis_CVTxPD2DQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                                Long delta, Bool isAvx, Bool r2zero )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp argV  = newTemp(Ity_V128);
   IRTemp rmode = newTemp(Ity_I32);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getXMMReg(rE) );
      delta += 1;
      DIP("%scvt%spd2dq %s,%s\n",
          isAvx ? "v" : "", r2zero ? "t" : "", nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("%scvt%spd2dqx %s,%s\n",
          isAvx ? "v" : "", r2zero ? "t" : "", dis_buf, nameXMMReg(rG) );
   }

   if (r2zero) {
      assign(rmode, mkU32((UInt)Irrm_ZERO) );
   } else {
      assign( rmode, get_sse_roundingmode() );
   }

   t0 = newTemp(Ity_F64);
   t1 = newTemp(Ity_F64);
   assign( t0, unop(Iop_ReinterpI64asF64, 
                    unop(Iop_V128to64, mkexpr(argV))) );
   assign( t1, unop(Iop_ReinterpI64asF64, 
                    unop(Iop_V128HIto64, mkexpr(argV))) );

#  define CVT(_t)  binop( Iop_F64toI32S,                   \
                          mkexpr(rmode),                   \
                          mkexpr(_t) )

   putXMMRegLane32( rG, 3, mkU32(0) );
   putXMMRegLane32( rG, 2, mkU32(0) );
   putXMMRegLane32( rG, 1, CVT(t1) );
   putXMMRegLane32( rG, 0, CVT(t0) );
#  undef CVT
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0) );

   return delta;
}


static Long dis_CVTxPD2DQ_256 ( VexAbiInfo* vbi, Prefix pfx,
                                Long delta, Bool r2zero )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp argV  = newTemp(Ity_V256);
   IRTemp rmode = newTemp(Ity_I32);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1, t2, t3;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getYMMReg(rE) );
      delta += 1;
      DIP("vcvt%spd2dq %s,%s\n",
          r2zero ? "t" : "", nameYMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V256, mkexpr(addr)) );
      delta += alen;
      DIP("vcvt%spd2dqy %s,%s\n",
          r2zero ? "t" : "", dis_buf, nameXMMReg(rG) );
   }

   if (r2zero) {
      assign(rmode, mkU32((UInt)Irrm_ZERO) );
   } else {
      assign( rmode, get_sse_roundingmode() );
   }

   t0 = IRTemp_INVALID;
   t1 = IRTemp_INVALID;
   t2 = IRTemp_INVALID;
   t3 = IRTemp_INVALID;
   breakupV256to64s( argV, &t3, &t2, &t1, &t0 );

#  define CVT(_t)  binop( Iop_F64toI32S,                   \
                          mkexpr(rmode),                   \
                          unop( Iop_ReinterpI64asF64,      \
                                mkexpr(_t) ) )

   putXMMRegLane32( rG, 3, CVT(t3) );
   putXMMRegLane32( rG, 2, CVT(t2) );
   putXMMRegLane32( rG, 1, CVT(t1) );
   putXMMRegLane32( rG, 0, CVT(t0) );
#  undef CVT
   putYMMRegLane128( rG, 1, mkV128(0) );

   return delta;
}


static Long dis_CVTDQ2PS_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp argV  = newTemp(Ity_V128);
   IRTemp rmode = newTemp(Ity_I32);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1, t2, t3;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getXMMReg(rE) );
      delta += 1;
      DIP("%scvtdq2ps %s,%s\n",
          isAvx ? "v" : "", nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("%scvtdq2ps %s,%s\n",
          isAvx ? "v" : "", dis_buf, nameXMMReg(rG) );
   }

   assign( rmode, get_sse_roundingmode() );
   t0 = IRTemp_INVALID;
   t1 = IRTemp_INVALID;
   t2 = IRTemp_INVALID;
   t3 = IRTemp_INVALID;
   breakupV128to32s( argV, &t3, &t2, &t1, &t0 );

#  define CVT(_t)  binop( Iop_F64toF32,                    \
                          mkexpr(rmode),                   \
                          unop(Iop_I32StoF64,mkexpr(_t)))
      
   putXMMRegLane32F( rG, 3, CVT(t3) );
   putXMMRegLane32F( rG, 2, CVT(t2) );
   putXMMRegLane32F( rG, 1, CVT(t1) );
   putXMMRegLane32F( rG, 0, CVT(t0) );
#  undef CVT
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0) );

   return delta;
}

static Long dis_CVTDQ2PS_256 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   IRTemp argV   = newTemp(Ity_V256);
   IRTemp rmode  = newTemp(Ity_I32);
   UInt   rG     = gregOfRexRM(pfx,modrm);
   IRTemp t0, t1, t2, t3, t4, t5, t6, t7;

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getYMMReg(rE) );
      delta += 1;
      DIP("vcvtdq2ps %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V256, mkexpr(addr)) );
      delta += alen;
      DIP("vcvtdq2ps %s,%s\n", dis_buf, nameYMMReg(rG) );
   }

   assign( rmode, get_sse_roundingmode() );
   t0 = IRTemp_INVALID;
   t1 = IRTemp_INVALID;
   t2 = IRTemp_INVALID;
   t3 = IRTemp_INVALID;
   t4 = IRTemp_INVALID;
   t5 = IRTemp_INVALID;
   t6 = IRTemp_INVALID;
   t7 = IRTemp_INVALID;
   breakupV256to32s( argV, &t7, &t6, &t5, &t4, &t3, &t2, &t1, &t0 );

#  define CVT(_t)  binop( Iop_F64toF32,                    \
                          mkexpr(rmode),                   \
                          unop(Iop_I32StoF64,mkexpr(_t)))
      
   putYMMRegLane32F( rG, 7, CVT(t7) );
   putYMMRegLane32F( rG, 6, CVT(t6) );
   putYMMRegLane32F( rG, 5, CVT(t5) );
   putYMMRegLane32F( rG, 4, CVT(t4) );
   putYMMRegLane32F( rG, 3, CVT(t3) );
   putYMMRegLane32F( rG, 2, CVT(t2) );
   putYMMRegLane32F( rG, 1, CVT(t1) );
   putYMMRegLane32F( rG, 0, CVT(t0) );
#  undef CVT

   return delta;
}


static Long dis_PMOVMSKB_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   /* UInt x86g_calculate_sse_pmovmskb ( ULong w64hi, ULong w64lo ); */
   UChar modrm = getUChar(delta);
   vassert(epartIsReg(modrm)); /* ensured by caller */
   UInt   rE = eregOfRexRM(pfx,modrm);
   UInt   rG = gregOfRexRM(pfx,modrm);
   IRTemp t0 = newTemp(Ity_I64);
   IRTemp t1 = newTemp(Ity_I64);
   IRTemp t5 = newTemp(Ity_I64);
   assign(t0, getXMMRegLane64(rE, 0));
   assign(t1, getXMMRegLane64(rE, 1));
   assign(t5, mkIRExprCCall( Ity_I64, 0/*regparms*/, 
                             "amd64g_calculate_sse_pmovmskb",
                             &amd64g_calculate_sse_pmovmskb,
                             mkIRExprVec_2( mkexpr(t1), mkexpr(t0) )));
   putIReg32(rG, unop(Iop_64to32,mkexpr(t5)));
   DIP("%spmovmskb %s,%s\n", isAvx ? "v" : "", nameXMMReg(rE),
       nameIReg32(rG));
   delta += 1;
   return delta;
}


/* FIXME: why not just use InterleaveLO / InterleaveHI?  I think the
   relevant ops are "xIsH ? InterleaveHI32x4 : InterleaveLO32x4". */
/* Does the maths for 128 bit versions of UNPCKLPS and UNPCKHPS */
static IRTemp math_UNPCKxPS_128 ( IRTemp sV, IRTemp dV, Bool xIsH )
{
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;
   breakupV128to32s( dV, &d3, &d2, &d1, &d0 );
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   IRTemp res = newTemp(Ity_V128);
   assign(res,  xIsH ? mkV128from32s( s3, d3, s2, d2 )
                     : mkV128from32s( s1, d1, s0, d0 ));
   return res;
}


/* FIXME: why not just use InterleaveLO / InterleaveHI ?? */
/* Does the maths for 128 bit versions of UNPCKLPD and UNPCKHPD */
static IRTemp math_UNPCKxPD_128 ( IRTemp sV, IRTemp dV, Bool xIsH )
{
   IRTemp s1 = newTemp(Ity_I64);
   IRTemp s0 = newTemp(Ity_I64);
   IRTemp d1 = newTemp(Ity_I64);
   IRTemp d0 = newTemp(Ity_I64);
   assign( d1, unop(Iop_V128HIto64, mkexpr(dV)) );
   assign( d0, unop(Iop_V128to64,   mkexpr(dV)) );
   assign( s1, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( s0, unop(Iop_V128to64,   mkexpr(sV)) );
   IRTemp res = newTemp(Ity_V128);
   assign(res, xIsH ? binop(Iop_64HLtoV128, mkexpr(s1), mkexpr(d1))
                    : binop(Iop_64HLtoV128, mkexpr(s0), mkexpr(d0)));
   return res;
}


/* Does the maths for 256 bit versions of UNPCKLPD and UNPCKHPD.
   Doesn't seem like this fits in either of the Iop_Interleave{LO,HI}
   or the Iop_Cat{Odd,Even}Lanes idioms, hence just do it the stupid
   way. */
static IRTemp math_UNPCKxPD_256 ( IRTemp sV, IRTemp dV, Bool xIsH )
{
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;
   breakupV256to64s( dV, &d3, &d2, &d1, &d0 );
   breakupV256to64s( sV, &s3, &s2, &s1, &s0 );
   IRTemp res = newTemp(Ity_V256);
   assign(res, xIsH
               ? IRExpr_Qop(Iop_64x4toV256, mkexpr(s3), mkexpr(d3),
                                            mkexpr(s1), mkexpr(d1))
               : IRExpr_Qop(Iop_64x4toV256, mkexpr(s2), mkexpr(d2),
                                            mkexpr(s0), mkexpr(d0)));
   return res;
}


/* FIXME: this is really bad.  Surely can do something better here?
   One observation is that the steering in the upper and lower 128 bit
   halves is the same as with math_UNPCKxPS_128, so we simply split
   into two halves, and use that.  Consequently any improvement in
   math_UNPCKxPS_128 (probably, to use interleave-style primops)
   benefits this too. */
static IRTemp math_UNPCKxPS_256 ( IRTemp sV, IRTemp dV, Bool xIsH )
{
   IRTemp sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
   IRTemp dVhi = IRTemp_INVALID, dVlo = IRTemp_INVALID;
   breakupV256toV128s( sV, &sVhi, &sVlo );
   breakupV256toV128s( dV, &dVhi, &dVlo );
   IRTemp rVhi = math_UNPCKxPS_128(sVhi, dVhi, xIsH);
   IRTemp rVlo = math_UNPCKxPS_128(sVlo, dVlo, xIsH);
   IRTemp rV   = newTemp(Ity_V256);
   assign(rV, binop(Iop_V128HLtoV256, mkexpr(rVhi), mkexpr(rVlo)));
   return rV;
}


static IRTemp math_SHUFPS_128 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;
   vassert(imm8 < 256);

   breakupV128to32s( dV, &d3, &d2, &d1, &d0 );
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );

#  define SELD(n) ((n)==0 ? d0 : ((n)==1 ? d1 : ((n)==2 ? d2 : d3)))
#  define SELS(n) ((n)==0 ? s0 : ((n)==1 ? s1 : ((n)==2 ? s2 : s3)))
   IRTemp res = newTemp(Ity_V128);
   assign(res, 
          mkV128from32s( SELS((imm8>>6)&3), SELS((imm8>>4)&3), 
                         SELD((imm8>>2)&3), SELD((imm8>>0)&3) ) );
#  undef SELD
#  undef SELS
   return res;
}


/* 256-bit SHUFPS appears to steer each of the 128-bit halves
   identically.  Hence do the clueless thing and use math_SHUFPS_128
   twice. */
static IRTemp math_SHUFPS_256 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
   IRTemp dVhi = IRTemp_INVALID, dVlo = IRTemp_INVALID;
   breakupV256toV128s( sV, &sVhi, &sVlo );
   breakupV256toV128s( dV, &dVhi, &dVlo );
   IRTemp rVhi = math_SHUFPS_128(sVhi, dVhi, imm8);
   IRTemp rVlo = math_SHUFPS_128(sVlo, dVlo, imm8);
   IRTemp rV   = newTemp(Ity_V256);
   assign(rV, binop(Iop_V128HLtoV256, mkexpr(rVhi), mkexpr(rVlo)));
   return rV;
}


static IRTemp math_SHUFPD_128 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp s1 = newTemp(Ity_I64);
   IRTemp s0 = newTemp(Ity_I64);
   IRTemp d1 = newTemp(Ity_I64);
   IRTemp d0 = newTemp(Ity_I64);

   assign( d1, unop(Iop_V128HIto64, mkexpr(dV)) );
   assign( d0, unop(Iop_V128to64,   mkexpr(dV)) );
   assign( s1, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( s0, unop(Iop_V128to64,   mkexpr(sV)) );

#  define SELD(n) mkexpr((n)==0 ? d0 : d1)
#  define SELS(n) mkexpr((n)==0 ? s0 : s1)

   IRTemp res = newTemp(Ity_V128);
   assign(res, binop( Iop_64HLtoV128,
                      SELS((imm8>>1)&1), SELD((imm8>>0)&1) ) );

#  undef SELD
#  undef SELS
   return res;
}


static IRTemp math_SHUFPD_256 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
   IRTemp dVhi = IRTemp_INVALID, dVlo = IRTemp_INVALID;
   breakupV256toV128s( sV, &sVhi, &sVlo );
   breakupV256toV128s( dV, &dVhi, &dVlo );
   IRTemp rVhi = math_SHUFPD_128(sVhi, dVhi, (imm8 >> 2) & 3);
   IRTemp rVlo = math_SHUFPD_128(sVlo, dVlo, imm8 & 3);
   IRTemp rV   = newTemp(Ity_V256);
   assign(rV, binop(Iop_V128HLtoV256, mkexpr(rVhi), mkexpr(rVlo)));
   return rV;
}


static IRTemp math_BLENDPD_128 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   UShort imm8_mask_16;
   IRTemp imm8_mask = newTemp(Ity_V128);

   switch( imm8 & 3 ) {
      case 0:  imm8_mask_16 = 0x0000; break;
      case 1:  imm8_mask_16 = 0x00FF; break;
      case 2:  imm8_mask_16 = 0xFF00; break;
      case 3:  imm8_mask_16 = 0xFFFF; break;
      default: vassert(0);            break;
   }
   assign( imm8_mask, mkV128( imm8_mask_16 ) );

   IRTemp res = newTemp(Ity_V128);
   assign ( res, binop( Iop_OrV128, 
                        binop( Iop_AndV128, mkexpr(sV),
                                            mkexpr(imm8_mask) ), 
                        binop( Iop_AndV128, mkexpr(dV), 
                               unop( Iop_NotV128, mkexpr(imm8_mask) ) ) ) );
   return res;
}


static IRTemp math_BLENDPD_256 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
   IRTemp dVhi = IRTemp_INVALID, dVlo = IRTemp_INVALID;
   breakupV256toV128s( sV, &sVhi, &sVlo );
   breakupV256toV128s( dV, &dVhi, &dVlo );
   IRTemp rVhi = math_BLENDPD_128(sVhi, dVhi, (imm8 >> 2) & 3);
   IRTemp rVlo = math_BLENDPD_128(sVlo, dVlo, imm8 & 3);
   IRTemp rV   = newTemp(Ity_V256);
   assign(rV, binop(Iop_V128HLtoV256, mkexpr(rVhi), mkexpr(rVlo)));
   return rV;
}


static IRTemp math_BLENDPS_128 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   UShort imm8_perms[16] = { 0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00,
                             0x0F0F, 0x0FF0, 0x0FFF, 0xF000, 0xF00F,
                             0xF0F0, 0xF0FF, 0xFF00, 0xFF0F, 0xFFF0,
                             0xFFFF };
   IRTemp imm8_mask = newTemp(Ity_V128);
   assign( imm8_mask, mkV128( imm8_perms[ (imm8 & 15) ] ) );

   IRTemp res = newTemp(Ity_V128);
   assign ( res, binop( Iop_OrV128,
                        binop( Iop_AndV128, mkexpr(sV), 
                                            mkexpr(imm8_mask) ),
                        binop( Iop_AndV128, mkexpr(dV),
                               unop( Iop_NotV128, mkexpr(imm8_mask) ) ) ) );
   return res;
}


static IRTemp math_BLENDPS_256 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   IRTemp sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
   IRTemp dVhi = IRTemp_INVALID, dVlo = IRTemp_INVALID;
   breakupV256toV128s( sV, &sVhi, &sVlo );
   breakupV256toV128s( dV, &dVhi, &dVlo );
   IRTemp rVhi = math_BLENDPS_128(sVhi, dVhi, (imm8 >> 4) & 15);
   IRTemp rVlo = math_BLENDPS_128(sVlo, dVlo, imm8 & 15);
   IRTemp rV   = newTemp(Ity_V256);
   assign(rV, binop(Iop_V128HLtoV256, mkexpr(rVhi), mkexpr(rVlo)));
   return rV;
}


static IRTemp math_PBLENDW_128 ( IRTemp sV, IRTemp dV, UInt imm8 )
{
   /* Make w be a 16-bit version of imm8, formed by duplicating each
      bit in imm8. */
   Int i;
   UShort imm16 = 0;
   for (i = 0; i < 8; i++) {
      if (imm8 & (1 << i))
         imm16 |= (3 << (2*i));
   }
   IRTemp imm16_mask = newTemp(Ity_V128);
   assign( imm16_mask, mkV128( imm16 ));

   IRTemp res = newTemp(Ity_V128);
   assign ( res, binop( Iop_OrV128,
                        binop( Iop_AndV128, mkexpr(sV), 
                                            mkexpr(imm16_mask) ),
                        binop( Iop_AndV128, mkexpr(dV),
                               unop( Iop_NotV128, mkexpr(imm16_mask) ) ) ) );
   return res;
}


static IRTemp math_PMULUDQ_128 ( IRTemp sV, IRTemp dV )
{
   /* This is a really poor translation -- could be improved if
      performance critical */
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;
   breakupV128to32s( dV, &d3, &d2, &d1, &d0 );
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   IRTemp res = newTemp(Ity_V128);
   assign(res, binop(Iop_64HLtoV128,
                     binop( Iop_MullU32, mkexpr(d2), mkexpr(s2)),
                     binop( Iop_MullU32, mkexpr(d0), mkexpr(s0)) ));
   return res;
}


static IRTemp math_PMULDQ_128 ( IRTemp dV, IRTemp sV )
{
   /* This is a really poor translation -- could be improved if
      performance critical */
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;
   breakupV128to32s( dV, &d3, &d2, &d1, &d0 );
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   IRTemp res = newTemp(Ity_V128);
   assign(res, binop(Iop_64HLtoV128,
                     binop( Iop_MullS32, mkexpr(d2), mkexpr(s2)),
                     binop( Iop_MullS32, mkexpr(d0), mkexpr(s0)) ));
   return res;
}


static IRTemp math_PMADDWD_128 ( IRTemp dV, IRTemp sV )
{
   IRTemp sVhi, sVlo, dVhi, dVlo;
   IRTemp resHi = newTemp(Ity_I64);
   IRTemp resLo = newTemp(Ity_I64);
   sVhi = sVlo = dVhi = dVlo = IRTemp_INVALID;
   breakupV128to64s( sV, &sVhi, &sVlo );
   breakupV128to64s( dV, &dVhi, &dVlo );
   assign( resHi, mkIRExprCCall(Ity_I64, 0/*regparms*/,
                                "amd64g_calculate_mmx_pmaddwd", 
                                &amd64g_calculate_mmx_pmaddwd,
                                mkIRExprVec_2( mkexpr(sVhi), mkexpr(dVhi))));
   assign( resLo, mkIRExprCCall(Ity_I64, 0/*regparms*/,
                                "amd64g_calculate_mmx_pmaddwd", 
                                &amd64g_calculate_mmx_pmaddwd,
                                mkIRExprVec_2( mkexpr(sVlo), mkexpr(dVlo))));
   IRTemp res = newTemp(Ity_V128);
   assign( res, binop(Iop_64HLtoV128, mkexpr(resHi), mkexpr(resLo))) ;
   return res;
}


static IRTemp math_ADDSUBPD_128 ( IRTemp dV, IRTemp sV )
{
   IRTemp addV = newTemp(Ity_V128);
   IRTemp subV = newTemp(Ity_V128);
   IRTemp a1   = newTemp(Ity_I64);
   IRTemp s0   = newTemp(Ity_I64);

   assign( addV, binop(Iop_Add64Fx2, mkexpr(dV), mkexpr(sV)) );
   assign( subV, binop(Iop_Sub64Fx2, mkexpr(dV), mkexpr(sV)) );

   assign( a1, unop(Iop_V128HIto64, mkexpr(addV) ));
   assign( s0, unop(Iop_V128to64,   mkexpr(subV) ));

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop(Iop_64HLtoV128, mkexpr(a1), mkexpr(s0)) );
   return res;
}


static IRTemp math_ADDSUBPD_256 ( IRTemp dV, IRTemp sV )
{
   IRTemp a3, a2, a1, a0, s3, s2, s1, s0;
   IRTemp addV = newTemp(Ity_V256);
   IRTemp subV = newTemp(Ity_V256);
   a3 = a2 = a1 = a0 = s3 = s2 = s1 = s0 = IRTemp_INVALID;

   assign( addV, binop(Iop_Add64Fx4, mkexpr(dV), mkexpr(sV)) );
   assign( subV, binop(Iop_Sub64Fx4, mkexpr(dV), mkexpr(sV)) );

   breakupV256to64s( addV, &a3, &a2, &a1, &a0 );
   breakupV256to64s( subV, &s3, &s2, &s1, &s0 );

   IRTemp res = newTemp(Ity_V256);
   assign( res, mkV256from64s( a3, s2, a1, s0 ) );
   return res;
}


static IRTemp math_ADDSUBPS_128 ( IRTemp dV, IRTemp sV )
{
   IRTemp a3, a2, a1, a0, s3, s2, s1, s0;
   IRTemp addV = newTemp(Ity_V128);
   IRTemp subV = newTemp(Ity_V128);
   a3 = a2 = a1 = a0 = s3 = s2 = s1 = s0 = IRTemp_INVALID;

   assign( addV, binop(Iop_Add32Fx4, mkexpr(dV), mkexpr(sV)) );
   assign( subV, binop(Iop_Sub32Fx4, mkexpr(dV), mkexpr(sV)) );

   breakupV128to32s( addV, &a3, &a2, &a1, &a0 );
   breakupV128to32s( subV, &s3, &s2, &s1, &s0 );

   IRTemp res = newTemp(Ity_V128);
   assign( res, mkV128from32s( a3, s2, a1, s0 ) );
   return res;
}


static IRTemp math_ADDSUBPS_256 ( IRTemp dV, IRTemp sV )
{
   IRTemp a7, a6, a5, a4, a3, a2, a1, a0;
   IRTemp s7, s6, s5, s4, s3, s2, s1, s0;
   IRTemp addV = newTemp(Ity_V256);
   IRTemp subV = newTemp(Ity_V256);
   a7 = a6 = a5 = a4 = a3 = a2 = a1 = a0 = IRTemp_INVALID;
   s7 = s6 = s5 = s4 = s3 = s2 = s1 = s0 = IRTemp_INVALID;

   assign( addV, binop(Iop_Add32Fx8, mkexpr(dV), mkexpr(sV)) );
   assign( subV, binop(Iop_Sub32Fx8, mkexpr(dV), mkexpr(sV)) );

   breakupV256to32s( addV, &a7, &a6, &a5, &a4, &a3, &a2, &a1, &a0 );
   breakupV256to32s( subV, &s7, &s6, &s5, &s4, &s3, &s2, &s1, &s0 );

   IRTemp res = newTemp(Ity_V256);
   assign( res, mkV256from32s( a7, s6, a5, s4, a3, s2, a1, s0 ) );
   return res;
}


/* Handle 128 bit PSHUFLW and PSHUFHW. */
static Long dis_PSHUFxW_128 ( VexAbiInfo* vbi, Prefix pfx,
                              Long delta, Bool isAvx, Bool xIsH )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   UInt   rG = gregOfRexRM(pfx,modrm);
   UInt   imm8;
   IRTemp sVmut, dVmut, sVcon, sV, dV, s3, s2, s1, s0;
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   sV    = newTemp(Ity_V128);
   dV    = newTemp(Ity_V128);
   sVmut = newTemp(Ity_I64);
   dVmut = newTemp(Ity_I64);
   sVcon = newTemp(Ity_I64);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      imm8 = (UInt)getUChar(delta+1);
      delta += 1+1;
      DIP("%spshuf%cw $%u,%s,%s\n",
          isAvx ? "v" : "", xIsH ? 'h' : 'l',
          imm8, nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      imm8 = (UInt)getUChar(delta+alen);
      delta += alen+1;
      DIP("%spshuf%cw $%u,%s,%s\n",
          isAvx ? "v" : "", xIsH ? 'h' : 'l',
          imm8, dis_buf, nameXMMReg(rG));
   }

   /* Get the to-be-changed (mut) and unchanging (con) bits of the
      source. */
   assign( sVmut, unop(xIsH ? Iop_V128HIto64 : Iop_V128to64,   mkexpr(sV)) );
   assign( sVcon, unop(xIsH ? Iop_V128to64   : Iop_V128HIto64, mkexpr(sV)) );

   breakup64to16s( sVmut, &s3, &s2, &s1, &s0 );
#  define SEL(n) \
             ((n)==0 ? s0 : ((n)==1 ? s1 : ((n)==2 ? s2 : s3)))
   assign(dVmut, mk64from16s( SEL((imm8>>6)&3), SEL((imm8>>4)&3),
                              SEL((imm8>>2)&3), SEL((imm8>>0)&3) ));
#  undef SEL

   assign(dV, xIsH ? binop(Iop_64HLtoV128, mkexpr(dVmut), mkexpr(sVcon))
                   : binop(Iop_64HLtoV128, mkexpr(sVcon), mkexpr(dVmut)) );

   (isAvx ? putYMMRegLoAndZU : putXMMReg)(rG, mkexpr(dV));
   return delta;
}


static Long dis_PEXTRW_128_EregOnly_toG ( VexAbiInfo* vbi, Prefix pfx,
                                          Long delta, Bool isAvx )
{
   Long   deltaIN = delta;
   UChar  modrm   = getUChar(delta);
   UInt   rG      = gregOfRexRM(pfx,modrm);
   IRTemp sV      = newTemp(Ity_V128);
   IRTemp d16     = newTemp(Ity_I16);
   UInt   imm8;
   IRTemp s0, s1, s2, s3;
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign(sV, getXMMReg(rE));
      imm8 = getUChar(delta+1) & 7;
      delta += 1+1;
      DIP("%spextrw $%d,%s,%s\n", isAvx ? "v" : "",
          (Int)imm8, nameXMMReg(rE), nameIReg32(rG));
   } else {
      /* The memory case is disallowed, apparently. */
      return deltaIN; /* FAIL */
   }
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   switch (imm8) {
      case 0:  assign(d16, unop(Iop_32to16,   mkexpr(s0))); break;
      case 1:  assign(d16, unop(Iop_32HIto16, mkexpr(s0))); break;
      case 2:  assign(d16, unop(Iop_32to16,   mkexpr(s1))); break;
      case 3:  assign(d16, unop(Iop_32HIto16, mkexpr(s1))); break;
      case 4:  assign(d16, unop(Iop_32to16,   mkexpr(s2))); break;
      case 5:  assign(d16, unop(Iop_32HIto16, mkexpr(s2))); break;
      case 6:  assign(d16, unop(Iop_32to16,   mkexpr(s3))); break;
      case 7:  assign(d16, unop(Iop_32HIto16, mkexpr(s3))); break;
      default: vassert(0);
   }
   putIReg32(rG, unop(Iop_16Uto32, mkexpr(d16)));
   return delta;
}
 

static Long dis_CVTDQ2PD_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp arg64 = newTemp(Ity_I64);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   UChar* mbV   = isAvx ? "v" : "";
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( arg64, getXMMRegLane64(rE, 0) );
      delta += 1;
      DIP("%scvtdq2pd %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
      delta += alen;
      DIP("%scvtdq2pd %s,%s\n", mbV, dis_buf, nameXMMReg(rG) );
   }
   putXMMRegLane64F( 
      rG, 0,
      unop(Iop_I32StoF64, unop(Iop_64to32, mkexpr(arg64)))
   );
   putXMMRegLane64F(
      rG, 1, 
      unop(Iop_I32StoF64, unop(Iop_64HIto32, mkexpr(arg64)))
   );
   if (isAvx)
      putYMMRegLane128(rG, 1, mkV128(0));
   return delta;
}


static Long dis_STMXCSR ( VexAbiInfo* vbi, Prefix pfx,
                          Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   vassert(!epartIsReg(modrm)); /* ensured by caller */
   vassert(gregOfRexRM(pfx,modrm) == 3); /* ditto */

   addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
   delta += alen;

   /* Fake up a native SSE mxcsr word.  The only thing it depends on
      is SSEROUND[1:0], so call a clean helper to cook it up.
   */
   /* ULong amd64h_create_mxcsr ( ULong sseround ) */
   DIP("%sstmxcsr %s\n",  isAvx ? "v" : "", dis_buf);
   storeLE( 
      mkexpr(addr), 
      unop(Iop_64to32,      
           mkIRExprCCall(
              Ity_I64, 0/*regp*/,
              "amd64g_create_mxcsr", &amd64g_create_mxcsr, 
              mkIRExprVec_1( unop(Iop_32Uto64,get_sse_roundingmode()) ) 
           ) 
      )
   );
   return delta;
}


static Long dis_LDMXCSR ( VexAbiInfo* vbi, Prefix pfx,
                          Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   vassert(!epartIsReg(modrm)); /* ensured by caller */
   vassert(gregOfRexRM(pfx,modrm) == 2); /* ditto */

   IRTemp t64 = newTemp(Ity_I64);
   IRTemp ew  = newTemp(Ity_I32);

   addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
   delta += alen;
   DIP("%sldmxcsr %s\n",  isAvx ? "v" : "", dis_buf);

   /* The only thing we observe in %mxcsr is the rounding mode.
      Therefore, pass the 32-bit value (SSE native-format control
      word) to a clean helper, getting back a 64-bit value, the
      lower half of which is the SSEROUND value to store, and the
      upper half of which is the emulation-warning token which may
      be generated.  
   */
   /* ULong amd64h_check_ldmxcsr ( ULong ); */
   assign( t64, mkIRExprCCall(
                   Ity_I64, 0/*regparms*/, 
                   "amd64g_check_ldmxcsr",
                   &amd64g_check_ldmxcsr, 
                   mkIRExprVec_1( 
                      unop(Iop_32Uto64,
                           loadLE(Ity_I32, mkexpr(addr))
                      )
                   )
                )
         );

   put_sse_roundingmode( unop(Iop_64to32, mkexpr(t64)) );
   assign( ew, unop(Iop_64HIto32, mkexpr(t64) ) );
   put_emwarn( mkexpr(ew) );
   /* Finally, if an emulation warning was reported, side-exit to
      the next insn, reporting the warning, so that Valgrind's
      dispatcher sees the warning. */
   stmt( 
      IRStmt_Exit(
         binop(Iop_CmpNE64, unop(Iop_32Uto64,mkexpr(ew)), mkU64(0)),
         Ijk_EmWarn,
         IRConst_U64(guest_RIP_bbstart+delta),
         OFFB_RIP
      )
   );
   return delta;
}


static IRTemp math_PINSRW_128 ( IRTemp v128, IRTemp u16, UInt imm8 )
{
   vassert(imm8 >= 0 && imm8 <= 7);

   // Create a V128 value which has the selected word in the
   // specified lane, and zeroes everywhere else.
   IRTemp tmp128    = newTemp(Ity_V128);
   IRTemp halfshift = newTemp(Ity_I64);
   assign(halfshift, binop(Iop_Shl64,
                           unop(Iop_16Uto64, mkexpr(u16)),
                           mkU8(16 * (imm8 & 3))));
   if (imm8 < 4) {
      assign(tmp128, binop(Iop_64HLtoV128, mkU64(0), mkexpr(halfshift)));
   } else {
      assign(tmp128, binop(Iop_64HLtoV128, mkexpr(halfshift), mkU64(0)));
   }

   UShort mask = ~(3 << (imm8 * 2));
   IRTemp res  = newTemp(Ity_V128);
   assign( res, binop(Iop_OrV128,
                      mkexpr(tmp128),
                      binop(Iop_AndV128, mkexpr(v128), mkV128(mask))) );
   return res;
}


static IRTemp math_PSADBW_128 ( IRTemp dV, IRTemp sV )
{
   IRTemp s1, s0, d1, d0;
   s1 = s0 = d1 = d0 = IRTemp_INVALID;

   breakupV128to64s( sV, &s1, &s0 );
   breakupV128to64s( dV, &d1, &d0 );
   
   IRTemp res = newTemp(Ity_V128);
   assign( res,
           binop(Iop_64HLtoV128,
                 mkIRExprCCall(Ity_I64, 0/*regparms*/,
                               "amd64g_calculate_mmx_psadbw", 
                               &amd64g_calculate_mmx_psadbw,
                               mkIRExprVec_2( mkexpr(s1), mkexpr(d1))),
                 mkIRExprCCall(Ity_I64, 0/*regparms*/,
                               "amd64g_calculate_mmx_psadbw", 
                               &amd64g_calculate_mmx_psadbw,
                               mkIRExprVec_2( mkexpr(s0), mkexpr(d0)))) );
   return res;
}


static Long dis_MASKMOVDQU ( VexAbiInfo* vbi, Prefix pfx,
                             Long delta, Bool isAvx )
{
   IRTemp regD    = newTemp(Ity_V128);
   IRTemp mask    = newTemp(Ity_V128);
   IRTemp olddata = newTemp(Ity_V128);
   IRTemp newdata = newTemp(Ity_V128);
   IRTemp addr    = newTemp(Ity_I64);
   UChar  modrm   = getUChar(delta);
   UInt   rG      = gregOfRexRM(pfx,modrm);
   UInt   rE      = eregOfRexRM(pfx,modrm);

   assign( addr, handleAddrOverrides( vbi, pfx, getIReg64(R_RDI) ));
   assign( regD, getXMMReg( rG ));

   /* Unfortunately can't do the obvious thing with SarN8x16
      here since that can't be re-emitted as SSE2 code - no such
      insn. */
   assign( mask, 
           binop(Iop_64HLtoV128,
                 binop(Iop_SarN8x8, 
                       getXMMRegLane64( eregOfRexRM(pfx,modrm), 1 ), 
                       mkU8(7) ),
                 binop(Iop_SarN8x8, 
                       getXMMRegLane64( eregOfRexRM(pfx,modrm), 0 ), 
                       mkU8(7) ) ));
   assign( olddata, loadLE( Ity_V128, mkexpr(addr) ));
   assign( newdata, binop(Iop_OrV128, 
                          binop(Iop_AndV128, 
                                mkexpr(regD), 
                                mkexpr(mask) ),
                          binop(Iop_AndV128, 
                                mkexpr(olddata),
                                unop(Iop_NotV128, mkexpr(mask)))) );
   storeLE( mkexpr(addr), mkexpr(newdata) );

   delta += 1;
   DIP("%smaskmovdqu %s,%s\n", isAvx ? "v" : "",
       nameXMMReg(rE), nameXMMReg(rG) );
   return delta;
}


static Long dis_MOVMSKPS_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   UChar modrm = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx,modrm);
   UInt   rE   = eregOfRexRM(pfx,modrm);
   IRTemp t0   = newTemp(Ity_I32);
   IRTemp t1   = newTemp(Ity_I32);
   IRTemp t2   = newTemp(Ity_I32);
   IRTemp t3   = newTemp(Ity_I32);
   delta += 1;
   assign( t0, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,0), mkU8(31)),
                      mkU32(1) ));
   assign( t1, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,1), mkU8(30)),
                      mkU32(2) ));
   assign( t2, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,2), mkU8(29)),
                      mkU32(4) ));
   assign( t3, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,3), mkU8(28)),
                      mkU32(8) ));
   putIReg32( rG, binop(Iop_Or32,
                        binop(Iop_Or32, mkexpr(t0), mkexpr(t1)),
                        binop(Iop_Or32, mkexpr(t2), mkexpr(t3)) ) );
   DIP("%smovmskps %s,%s\n", isAvx ? "v" : "",
       nameXMMReg(rE), nameIReg32(rG));
   return delta;
}


static Long dis_MOVMSKPS_256 ( VexAbiInfo* vbi, Prefix pfx, Long delta )
{
   UChar modrm = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx,modrm);
   UInt   rE   = eregOfRexRM(pfx,modrm);
   IRTemp t0   = newTemp(Ity_I32);
   IRTemp t1   = newTemp(Ity_I32);
   IRTemp t2   = newTemp(Ity_I32);
   IRTemp t3   = newTemp(Ity_I32);
   IRTemp t4   = newTemp(Ity_I32);
   IRTemp t5   = newTemp(Ity_I32);
   IRTemp t6   = newTemp(Ity_I32);
   IRTemp t7   = newTemp(Ity_I32);
   delta += 1;
   assign( t0, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,0), mkU8(31)),
                      mkU32(1) ));
   assign( t1, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,1), mkU8(30)),
                      mkU32(2) ));
   assign( t2, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,2), mkU8(29)),
                      mkU32(4) ));
   assign( t3, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,3), mkU8(28)),
                      mkU32(8) ));
   assign( t4, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,4), mkU8(27)),
                      mkU32(16) ));
   assign( t5, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,5), mkU8(26)),
                      mkU32(32) ));
   assign( t6, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,6), mkU8(25)),
                      mkU32(64) ));
   assign( t7, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,7), mkU8(24)),
                      mkU32(128) ));
   putIReg32( rG, binop(Iop_Or32,
                        binop(Iop_Or32,
                              binop(Iop_Or32, mkexpr(t0), mkexpr(t1)),
                              binop(Iop_Or32, mkexpr(t2), mkexpr(t3)) ),
                        binop(Iop_Or32,
                              binop(Iop_Or32, mkexpr(t4), mkexpr(t5)),
                              binop(Iop_Or32, mkexpr(t6), mkexpr(t7)) ) ) );
   DIP("vmovmskps %s,%s\n", nameYMMReg(rE), nameIReg32(rG));
   return delta;
}


static Long dis_MOVMSKPD_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   UChar modrm = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx,modrm);
   UInt   rE   = eregOfRexRM(pfx,modrm);
   IRTemp t0   = newTemp(Ity_I32);
   IRTemp t1   = newTemp(Ity_I32);
   delta += 1;
   assign( t0, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,1), mkU8(31)),
                      mkU32(1) ));
   assign( t1, binop( Iop_And32,
                      binop(Iop_Shr32, getXMMRegLane32(rE,3), mkU8(30)),
                      mkU32(2) ));
   putIReg32( rG, binop(Iop_Or32, mkexpr(t0), mkexpr(t1) ) );
   DIP("%smovmskpd %s,%s\n", isAvx ? "v" : "",
       nameXMMReg(rE), nameIReg32(rG));
   return delta;
}


static Long dis_MOVMSKPD_256 ( VexAbiInfo* vbi, Prefix pfx, Long delta )
{
   UChar modrm = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx,modrm);
   UInt   rE   = eregOfRexRM(pfx,modrm);
   IRTemp t0   = newTemp(Ity_I32);
   IRTemp t1   = newTemp(Ity_I32);
   IRTemp t2   = newTemp(Ity_I32);
   IRTemp t3   = newTemp(Ity_I32);
   delta += 1;
   assign( t0, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,1), mkU8(31)),
                      mkU32(1) ));
   assign( t1, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,3), mkU8(30)),
                      mkU32(2) ));
   assign( t2, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,5), mkU8(29)),
                      mkU32(4) ));
   assign( t3, binop( Iop_And32,
                      binop(Iop_Shr32, getYMMRegLane32(rE,7), mkU8(28)),
                      mkU32(8) ));
   putIReg32( rG, binop(Iop_Or32,
                        binop(Iop_Or32, mkexpr(t0), mkexpr(t1)),
                        binop(Iop_Or32, mkexpr(t2), mkexpr(t3)) ) );
   DIP("vmovmskps %s,%s\n", nameYMMReg(rE), nameIReg32(rG));
   return delta;
}


/* Note, this also handles SSE(1) insns. */
__attribute__((noinline))
static
Long dis_ESC_0F__SSE2 ( Bool* decode_OK,
                        VexAbiInfo* vbi,
                        Prefix pfx, Int sz, Long deltaIN,
                        DisResult* dres )
{
   IRTemp addr  = IRTemp_INVALID;
   IRTemp t0    = IRTemp_INVALID;
   IRTemp t1    = IRTemp_INVALID;
   IRTemp t2    = IRTemp_INVALID;
   IRTemp t3    = IRTemp_INVALID;
   IRTemp t4    = IRTemp_INVALID;
   IRTemp t5    = IRTemp_INVALID;
   IRTemp t6    = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x10:
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         /* 66 0F 10 = MOVUPD -- move from E (mem or xmm) to G (xmm). */
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movupd %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movupd %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      /* F2 0F 10 = MOVSD -- move 64 bits from E (mem or lo half xmm) to
         G (lo half xmm).  If E is mem, upper half of G is zeroed out.
         If E is reg, upper half of G is unchanged. */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8) ) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 0,
                             getXMMRegLane64( eregOfRexRM(pfx,modrm), 0 ));
            DIP("movsd %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), mkV128(0) );
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 0,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movsd %s,%s\n", dis_buf,
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      /* F3 0F 10 = MOVSS -- move 32 bits from E (mem or lo 1/4 xmm) to G
         (lo 1/4 xmm).  If E is mem, upper 3/4 of G is zeroed out. */
      if (haveF3no66noF2(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMRegLane32( gregOfRexRM(pfx,modrm), 0,
                             getXMMRegLane32( eregOfRexRM(pfx,modrm), 0 ));
            DIP("movss %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), mkV128(0) );
            putXMMRegLane32( gregOfRexRM(pfx,modrm), 0,
                             loadLE(Ity_I32, mkexpr(addr)) );
            DIP("movss %s,%s\n", dis_buf,
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      /* 0F 10 = MOVUPS -- move from E (mem or xmm) to G (xmm). */
      if (haveNo66noF2noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movups %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movups %s,%s\n", dis_buf,
                                     nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x11:
      /* F2 0F 11 = MOVSD -- move 64 bits from G (lo half xmm) to E (mem
         or lo half xmm). */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMRegLane64( eregOfRexRM(pfx,modrm), 0,
                             getXMMRegLane64( gregOfRexRM(pfx,modrm), 0 ));
            DIP("movsd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                 nameXMMReg(eregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr),
                     getXMMRegLane64(gregOfRexRM(pfx,modrm), 0) );
            DIP("movsd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                 dis_buf);
            delta += alen;
         }
         goto decode_success;
      }
      /* F3 0F 11 = MOVSS -- move 32 bits from G (lo 1/4 xmm) to E (mem
         or lo 1/4 xmm). */
      if (haveF3no66noF2(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through, we don't yet have a test case */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr),
                     getXMMRegLane32(gregOfRexRM(pfx,modrm), 0) );
            DIP("movss %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                 dis_buf);
            delta += alen;
            goto decode_success;
         }
      }
      /* 66 0F 11 = MOVUPD -- move from G (xmm) to E (mem or xmm). */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( eregOfRexRM(pfx,modrm),
   		    getXMMReg( gregOfRexRM(pfx,modrm) ) );
            DIP("movupd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
   	                       nameXMMReg(eregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movupd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                  dis_buf );
            delta += alen;
         }
         goto decode_success;
      }
      /* 0F 11 = MOVUPS -- move from G (xmm) to E (mem or xmm). */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through; awaiting test case */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movups %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                  dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      break;

   case 0x12:
      /* 66 0F 12 = MOVLPD -- move from mem to low half of XMM. */
      /* Identical to MOVLPS ? */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through; apparently reg-reg is not possible */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putXMMRegLane64( gregOfRexRM(pfx,modrm),
                             0/*lower lane*/,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movlpd %s, %s\n", 
                dis_buf, nameXMMReg( gregOfRexRM(pfx,modrm) ));
            goto decode_success;
         }
      }
      /* 0F 12 = MOVLPS -- move from mem to low half of XMM. */
      /* OF 12 = MOVHLPS -- from from hi half to lo half of XMM. */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            putXMMRegLane64( gregOfRexRM(pfx,modrm),  
                             0/*lower lane*/,
                             getXMMRegLane64( eregOfRexRM(pfx,modrm), 1 ));
            DIP("movhlps %s, %s\n", nameXMMReg(eregOfRexRM(pfx,modrm)), 
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putXMMRegLane64( gregOfRexRM(pfx,modrm),  0/*lower lane*/,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movlps %s, %s\n", 
                dis_buf, nameXMMReg( gregOfRexRM(pfx,modrm) ));
         }
         goto decode_success;
      }
      break;

   case 0x13:
      /* 0F 13 = MOVLPS -- move from low half of XMM to mem. */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr), 
                     getXMMRegLane64( gregOfRexRM(pfx,modrm), 
                                      0/*lower lane*/ ) );
            DIP("movlps %s, %s\n", nameXMMReg( gregOfRexRM(pfx,modrm) ),
                                   dis_buf);
            goto decode_success;
         }
         /* else fall through */
      }
      /* 66 0F 13 = MOVLPD -- move from low half of XMM to mem. */
      /* Identical to MOVLPS ? */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr), 
                     getXMMRegLane64( gregOfRexRM(pfx,modrm), 
                                      0/*lower lane*/ ) );
            DIP("movlpd %s, %s\n", nameXMMReg( gregOfRexRM(pfx,modrm) ),
                                   dis_buf);
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x14:
   case 0x15:
      /* 0F 14 = UNPCKLPS -- unpack and interleave low part F32s */
      /* 0F 15 = UNPCKHPS -- unpack and interleave high part F32s */
      /* These just appear to be special cases of SHUFPS */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         Bool   hi = toBool(opc == 0x15);
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt   rG = gregOfRexRM(pfx,modrm);
         assign( dV, getXMMReg(rG) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("unpck%sps %s,%s\n", hi ? "h" : "l",
                nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("unpck%sps %s,%s\n", hi ? "h" : "l",
                dis_buf, nameXMMReg(rG));
         }
         IRTemp res = math_UNPCKxPS_128( sV, dV, hi );
         putXMMReg( rG, mkexpr(res) );
         goto decode_success;
      }
      /* 66 0F 15 = UNPCKHPD -- unpack and interleave high part F64s */
      /* 66 0F 14 = UNPCKLPD -- unpack and interleave low part F64s */
      /* These just appear to be special cases of SHUFPS */
      if (have66noF2noF3(pfx) 
          && sz == 2 /* could be 8 if rex also present */) {
         Bool   hi = toBool(opc == 0x15);
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt   rG = gregOfRexRM(pfx,modrm);
         assign( dV, getXMMReg(rG) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("unpck%sps %s,%s\n", hi ? "h" : "l",
                nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("unpck%sps %s,%s\n", hi ? "h" : "l",
                dis_buf, nameXMMReg(rG));
         }
         IRTemp res = math_UNPCKxPD_128( sV, dV, hi );
         putXMMReg( rG, mkexpr(res) );
         goto decode_success;
      }
      break;

   case 0x16:
      /* 66 0F 16 = MOVHPD -- move from mem to high half of XMM. */
      /* These seems identical to MOVHPS.  This instruction encoding is
         completely crazy. */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through; apparently reg-reg is not possible */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 1/*upper lane*/,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movhpd %s,%s\n", dis_buf, 
                                  nameXMMReg( gregOfRexRM(pfx,modrm) ));
            goto decode_success;
         }
      }
      /* 0F 16 = MOVHPS -- move from mem to high half of XMM. */
      /* 0F 16 = MOVLHPS -- move from lo half to hi half of XMM. */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 1/*upper lane*/,
                             getXMMRegLane64( eregOfRexRM(pfx,modrm), 0 ) );
            DIP("movhps %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)), 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 1/*upper lane*/,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movhps %s,%s\n", dis_buf, 
                                  nameXMMReg( gregOfRexRM(pfx,modrm) ));
         }
         goto decode_success;
      }
      break;

   case 0x17:
      /* 0F 17 = MOVHPS -- move from high half of XMM to mem. */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr), 
                     getXMMRegLane64( gregOfRexRM(pfx,modrm),
                                      1/*upper lane*/ ) );
            DIP("movhps %s,%s\n", nameXMMReg( gregOfRexRM(pfx,modrm) ),
                                  dis_buf);
            goto decode_success;
         }
         /* else fall through */
      }
      /* 66 0F 17 = MOVHPD -- move from high half of XMM to mem. */
      /* Again, this seems identical to MOVHPS. */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr), 
                     getXMMRegLane64( gregOfRexRM(pfx,modrm),
                                      1/*upper lane*/ ) );
            DIP("movhpd %s,%s\n", nameXMMReg( gregOfRexRM(pfx,modrm) ),
                                  dis_buf);
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x18:
      /* 0F 18 /0 = PREFETCHNTA -- prefetch into caches, */
      /* 0F 18 /1 = PREFETCH0   -- with various different hints */
      /* 0F 18 /2 = PREFETCH1 */
      /* 0F 18 /3 = PREFETCH2 */
      if (haveNo66noF2noF3(pfx)
          && !epartIsReg(getUChar(delta)) 
          && gregLO3ofRM(getUChar(delta)) >= 0
          && gregLO3ofRM(getUChar(delta)) <= 3) {
         HChar* hintstr = "??";

         modrm = getUChar(delta);
         vassert(!epartIsReg(modrm));

         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;

         switch (gregLO3ofRM(modrm)) {
            case 0: hintstr = "nta"; break;
            case 1: hintstr = "t0"; break;
            case 2: hintstr = "t1"; break;
            case 3: hintstr = "t2"; break;
            default: vassert(0);
         }

         DIP("prefetch%s %s\n", hintstr, dis_buf);
         goto decode_success;
      }
      break;

   case 0x28:
      /* 66 0F 28 = MOVAPD -- move from E (mem or xmm) to G (xmm). */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movapd %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movapd %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      /* 0F 28 = MOVAPS -- move from E (mem or xmm) to G (xmm). */
      if (haveNo66noF2noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movaps %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movaps %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x29:
      /* 0F 29 = MOVAPS -- move from G (xmm) to E (mem or xmm). */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through; awaiting test case */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movaps %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                  dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      /* 66 0F 29 = MOVAPD -- move from G (xmm) to E (mem or xmm). */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( eregOfRexRM(pfx,modrm),
   		    getXMMReg( gregOfRexRM(pfx,modrm) ) );
            DIP("movapd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
   	                       nameXMMReg(eregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movapd %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)),
                                  dis_buf );
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x2A:
      /* 0F 2A = CVTPI2PS -- convert 2 x I32 in mem/mmx to 2 x F32 in low
         half xmm */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp arg64 = newTemp(Ity_I64);
         IRTemp rmode = newTemp(Ity_I32);

         modrm = getUChar(delta);
         do_MMX_preamble();
         if (epartIsReg(modrm)) {
            assign( arg64, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("cvtpi2ps %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("cvtpi2ps %s,%s\n", dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)) );
         }

         assign( rmode, get_sse_roundingmode() );

         putXMMRegLane32F( 
            gregOfRexRM(pfx,modrm), 0,
            binop(Iop_F64toF32, 
                  mkexpr(rmode),
                  unop(Iop_I32StoF64, 
                       unop(Iop_64to32, mkexpr(arg64)) )) );

         putXMMRegLane32F(
            gregOfRexRM(pfx,modrm), 1, 
            binop(Iop_F64toF32, 
                  mkexpr(rmode),
                  unop(Iop_I32StoF64,
                       unop(Iop_64HIto32, mkexpr(arg64)) )) );

         goto decode_success;
      }
      /* F3 0F 2A = CVTSI2SS 
         -- sz==4: convert I32 in mem/ireg to F32 in low quarter xmm
         -- sz==8: convert I64 in mem/ireg to F32 in low quarter xmm */
      if (haveF3no66noF2(pfx) && (sz == 4 || sz == 8)) {
         IRTemp rmode = newTemp(Ity_I32);
         assign( rmode, get_sse_roundingmode() );
         modrm = getUChar(delta);
         if (sz == 4) {
            IRTemp arg32 = newTemp(Ity_I32);
            if (epartIsReg(modrm)) {
               assign( arg32, getIReg32(eregOfRexRM(pfx,modrm)) );
               delta += 1;
               DIP("cvtsi2ss %s,%s\n", nameIReg32(eregOfRexRM(pfx,modrm)),
                                       nameXMMReg(gregOfRexRM(pfx,modrm)));
            } else {
               addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
               assign( arg32, loadLE(Ity_I32, mkexpr(addr)) );
               delta += alen;
               DIP("cvtsi2ss %s,%s\n", dis_buf,
                                       nameXMMReg(gregOfRexRM(pfx,modrm)) );
            }
            putXMMRegLane32F( 
               gregOfRexRM(pfx,modrm), 0,
               binop(Iop_F64toF32,
                     mkexpr(rmode),
                     unop(Iop_I32StoF64, mkexpr(arg32)) ) );
         } else {
            /* sz == 8 */
            IRTemp arg64 = newTemp(Ity_I64);
            if (epartIsReg(modrm)) {
               assign( arg64, getIReg64(eregOfRexRM(pfx,modrm)) );
               delta += 1;
               DIP("cvtsi2ssq %s,%s\n", nameIReg64(eregOfRexRM(pfx,modrm)),
                                        nameXMMReg(gregOfRexRM(pfx,modrm)));
            } else {
               addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
               assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
               delta += alen;
               DIP("cvtsi2ssq %s,%s\n", dis_buf,
                                        nameXMMReg(gregOfRexRM(pfx,modrm)) );
            }
            putXMMRegLane32F( 
               gregOfRexRM(pfx,modrm), 0,
               binop(Iop_F64toF32,
                     mkexpr(rmode),
                     binop(Iop_I64StoF64, mkexpr(rmode), mkexpr(arg64)) ) );
         }
         goto decode_success;
      }
      /* F2 0F 2A = CVTSI2SD 
         when sz==4 -- convert I32 in mem/ireg to F64 in low half xmm
         when sz==8 -- convert I64 in mem/ireg to F64 in low half xmm
      */
      if (haveF2no66noF3(pfx) && (sz == 4 || sz == 8)) {
         modrm = getUChar(delta);
         if (sz == 4) {
            IRTemp arg32 = newTemp(Ity_I32);
            if (epartIsReg(modrm)) {
               assign( arg32, getIReg32(eregOfRexRM(pfx,modrm)) );
               delta += 1;
               DIP("cvtsi2sdl %s,%s\n", nameIReg32(eregOfRexRM(pfx,modrm)),
                                        nameXMMReg(gregOfRexRM(pfx,modrm)));
            } else {
               addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
               assign( arg32, loadLE(Ity_I32, mkexpr(addr)) );
               delta += alen;
               DIP("cvtsi2sdl %s,%s\n", dis_buf,
                                        nameXMMReg(gregOfRexRM(pfx,modrm)) );
            }
            putXMMRegLane64F( gregOfRexRM(pfx,modrm), 0,
                              unop(Iop_I32StoF64, mkexpr(arg32)) 
            );
         } else {
            /* sz == 8 */
            IRTemp arg64 = newTemp(Ity_I64);
            if (epartIsReg(modrm)) {
               assign( arg64, getIReg64(eregOfRexRM(pfx,modrm)) );
               delta += 1;
               DIP("cvtsi2sdq %s,%s\n", nameIReg64(eregOfRexRM(pfx,modrm)),
                                        nameXMMReg(gregOfRexRM(pfx,modrm)));
            } else {
               addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
               assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
               delta += alen;
               DIP("cvtsi2sdq %s,%s\n", dis_buf,
                                        nameXMMReg(gregOfRexRM(pfx,modrm)) );
            }
            putXMMRegLane64F( 
               gregOfRexRM(pfx,modrm), 
               0,
               binop( Iop_I64StoF64,
                      get_sse_roundingmode(),
                      mkexpr(arg64)
               ) 
            );
         }
         goto decode_success;
      }
      /* 66 0F 2A = CVTPI2PD -- convert 2 x I32 in mem/mmx to 2 x F64 in
         xmm(G) */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp arg64 = newTemp(Ity_I64);

         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* Only switch to MMX mode if the source is a MMX register.
               This is inconsistent with all other instructions which
               convert between XMM and (M64 or MMX), which always switch
               to MMX mode even if 64-bit operand is M64 and not MMX.  At
               least, that's what the Intel docs seem to me to say.
               Fixes #210264. */
            do_MMX_preamble();
            assign( arg64, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("cvtpi2pd %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("cvtpi2pd %s,%s\n", dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)) );
         }

         putXMMRegLane64F( 
            gregOfRexRM(pfx,modrm), 0,
            unop(Iop_I32StoF64, unop(Iop_64to32, mkexpr(arg64)) )
         );

         putXMMRegLane64F( 
            gregOfRexRM(pfx,modrm), 1,
            unop(Iop_I32StoF64, unop(Iop_64HIto32, mkexpr(arg64)) )
         );

         goto decode_success;
      }
      break;

   case 0x2B:
      /* 66 0F 2B = MOVNTPD -- for us, just a plain SSE store. */
      /* 0F 2B = MOVNTPS -- for us, just a plain SSE store. */
      if ( (haveNo66noF2noF3(pfx) && sz == 4)
           || (have66noF2noF3(pfx) && sz == 2) ) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movntp%s %s,%s\n", sz==2 ? "d" : "s",
                                    dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x2C:
   case 0x2D:
      /* 0F 2D = CVTPS2PI -- convert 2 x F32 in mem/low half xmm to 2 x
         I32 in mmx, according to prevailing SSE rounding mode */
      /* 0F 2C = CVTTPS2PI -- convert 2 x F32 in mem/low half xmm to 2 x
         I32 in mmx, rounding towards zero */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp dst64  = newTemp(Ity_I64);
         IRTemp rmode  = newTemp(Ity_I32);
         IRTemp f32lo  = newTemp(Ity_F32);
         IRTemp f32hi  = newTemp(Ity_F32);
         Bool   r2zero = toBool(opc == 0x2C);

         do_MMX_preamble();
         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            delta += 1;
            assign(f32lo, getXMMRegLane32F(eregOfRexRM(pfx,modrm), 0));
            assign(f32hi, getXMMRegLane32F(eregOfRexRM(pfx,modrm), 1));
            DIP("cvt%sps2pi %s,%s\n", r2zero ? "t" : "",
                                      nameXMMReg(eregOfRexRM(pfx,modrm)),
                                      nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f32lo, loadLE(Ity_F32, mkexpr(addr)));
            assign(f32hi, loadLE(Ity_F32, binop( Iop_Add64, 
                                                 mkexpr(addr), 
                                                 mkU64(4) )));
            delta += alen;
            DIP("cvt%sps2pi %s,%s\n", r2zero ? "t" : "",
                                      dis_buf,
                                      nameMMXReg(gregLO3ofRM(modrm)));
         }

         if (r2zero) {
            assign(rmode, mkU32((UInt)Irrm_ZERO) );
         } else {
            assign( rmode, get_sse_roundingmode() );
         }

         assign( 
            dst64,
            binop( Iop_32HLto64,
                   binop( Iop_F64toI32S, 
                          mkexpr(rmode), 
                          unop( Iop_F32toF64, mkexpr(f32hi) ) ),
                   binop( Iop_F64toI32S, 
                          mkexpr(rmode), 
                          unop( Iop_F32toF64, mkexpr(f32lo) ) )
                 )
         );

         putMMXReg(gregLO3ofRM(modrm), mkexpr(dst64));
         goto decode_success;
      }
      /* F3 0F 2D = CVTSS2SI 
         when sz==4 -- convert F32 in mem/low quarter xmm to I32 in ireg, 
                       according to prevailing SSE rounding mode
         when sz==8 -- convert F32 in mem/low quarter xmm to I64 in ireg, 
                       according to prevailing SSE rounding mode
      */
      /* F3 0F 2C = CVTTSS2SI 
         when sz==4 -- convert F32 in mem/low quarter xmm to I32 in ireg, 
                       truncating towards zero
         when sz==8 -- convert F32 in mem/low quarter xmm to I64 in ireg, 
                       truncating towards zero 
      */
      if (haveF3no66noF2(pfx) && (sz == 4 || sz == 8)) {
         delta = dis_CVTxSS2SI( vbi, pfx, delta, False/*!isAvx*/, opc, sz);
         goto decode_success;
      }
      /* F2 0F 2D = CVTSD2SI 
         when sz==4 -- convert F64 in mem/low half xmm to I32 in ireg, 
                       according to prevailing SSE rounding mode
         when sz==8 -- convert F64 in mem/low half xmm to I64 in ireg, 
                       according to prevailing SSE rounding mode
      */
      /* F2 0F 2C = CVTTSD2SI 
         when sz==4 -- convert F64 in mem/low half xmm to I32 in ireg, 
                       truncating towards zero
         when sz==8 -- convert F64 in mem/low half xmm to I64 in ireg, 
                       truncating towards zero 
      */
      if (haveF2no66noF3(pfx) && (sz == 4 || sz == 8)) {
         delta = dis_CVTxSD2SI( vbi, pfx, delta, False/*!isAvx*/, opc, sz);
         goto decode_success;
      }
      /* 66 0F 2D = CVTPD2PI -- convert 2 x F64 in mem/xmm to 2 x
         I32 in mmx, according to prevailing SSE rounding mode */
      /* 66 0F 2C = CVTTPD2PI -- convert 2 x F64 in mem/xmm to 2 x
         I32 in mmx, rounding towards zero */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp dst64  = newTemp(Ity_I64);
         IRTemp rmode  = newTemp(Ity_I32);
         IRTemp f64lo  = newTemp(Ity_F64);
         IRTemp f64hi  = newTemp(Ity_F64);
         Bool   r2zero = toBool(opc == 0x2C);

         do_MMX_preamble();
         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            delta += 1;
            assign(f64lo, getXMMRegLane64F(eregOfRexRM(pfx,modrm), 0));
            assign(f64hi, getXMMRegLane64F(eregOfRexRM(pfx,modrm), 1));
            DIP("cvt%spd2pi %s,%s\n", r2zero ? "t" : "",
                                      nameXMMReg(eregOfRexRM(pfx,modrm)),
                                      nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f64lo, loadLE(Ity_F64, mkexpr(addr)));
            assign(f64hi, loadLE(Ity_F64, binop( Iop_Add64, 
                                                 mkexpr(addr), 
                                                 mkU64(8) )));
            delta += alen;
            DIP("cvt%spf2pi %s,%s\n", r2zero ? "t" : "",
                                      dis_buf,
                                      nameMMXReg(gregLO3ofRM(modrm)));
         }

         if (r2zero) {
            assign(rmode, mkU32((UInt)Irrm_ZERO) );
         } else {
            assign( rmode, get_sse_roundingmode() );
         }

         assign( 
            dst64,
            binop( Iop_32HLto64,
                   binop( Iop_F64toI32S, mkexpr(rmode), mkexpr(f64hi) ),
                   binop( Iop_F64toI32S, mkexpr(rmode), mkexpr(f64lo) )
                 )
         );

         putMMXReg(gregLO3ofRM(modrm), mkexpr(dst64));
         goto decode_success;
      }
      break;

   case 0x2E:
   case 0x2F:
      /* 66 0F 2E = UCOMISD -- 64F0x2 comparison G,E, and set ZCP */
      /* 66 0F 2F = COMISD  -- 64F0x2 comparison G,E, and set ZCP */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_COMISD( vbi, pfx, delta, False/*!isAvx*/, opc );
         goto decode_success;
      }
      /* 0F 2E = UCOMISS -- 32F0x4 comparison G,E, and set ZCP */
      /* 0F 2F = COMISS  -- 32F0x4 comparison G,E, and set ZCP */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_COMISS( vbi, pfx, delta, False/*!isAvx*/, opc );
         goto decode_success;
      }
      break;

   case 0x50:
      /* 0F 50 = MOVMSKPS - move 4 sign bits from 4 x F32 in xmm(E)
         to 4 lowest bits of ireg(G) */
      if (haveNo66noF2noF3(pfx) && (sz == 4 || sz == 8)
          && epartIsReg(getUChar(delta))) {
         /* sz == 8 is a kludge to handle insns with REX.W redundantly
            set to 1, which has been known to happen:

            4c 0f 50 d9             rex64X movmskps %xmm1,%r11d

            20071106: Intel docs say that REX.W isn't redundant: when
            present, a 64-bit register is written; when not present, only
            the 32-bit half is written.  However, testing on a Core2
            machine suggests the entire 64 bit register is written
            irrespective of the status of REX.W.  That could be because
            of the default rule that says "if the lower half of a 32-bit
            register is written, the upper half is zeroed".  By using
            putIReg32 here we inadvertantly produce the same behaviour as
            the Core2, for the same reason -- putIReg32 implements said
            rule.

            AMD docs give no indication that REX.W is even valid for this
            insn. */
         delta = dis_MOVMSKPS_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      /* 66 0F 50 = MOVMSKPD - move 2 sign bits from 2 x F64 in xmm(E) to
         2 lowest bits of ireg(G) */
      if (have66noF2noF3(pfx) && (sz == 2 || sz == 8)) {
         /* sz == 8 is a kludge to handle insns with REX.W redundantly
            set to 1, which has been known to happen:
            66 4c 0f 50 d9          rex64X movmskpd %xmm1,%r11d
            20071106: see further comments on MOVMSKPS implementation above.
         */
         delta = dis_MOVMSKPD_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x51:
      /* F3 0F 51 = SQRTSS -- approx sqrt 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_lo32( vbi, pfx, delta, 
                                            "sqrtss", Iop_Sqrt32F0x4 );
         goto decode_success;
      }
      /* 0F 51 = SQRTPS -- approx sqrt 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_all( vbi, pfx, delta, 
                                           "sqrtps", Iop_Sqrt32Fx4 );
         goto decode_success;
      }
      /* F2 0F 51 = SQRTSD -- approx sqrt 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_lo64( vbi, pfx, delta, 
                                            "sqrtsd", Iop_Sqrt64F0x2 );
         goto decode_success;
      }
      /* 66 0F 51 = SQRTPD -- approx sqrt 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_unary_all( vbi, pfx, delta, 
                                           "sqrtpd", Iop_Sqrt64Fx2 );
         goto decode_success;
      }
      break;

   case 0x52:
      /* F3 0F 52 = RSQRTSS -- approx reciprocal sqrt 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_lo32( vbi, pfx, delta, 
                                            "rsqrtss", Iop_RSqrt32F0x4 );
         goto decode_success;
      }
      /* 0F 52 = RSQRTPS -- approx reciprocal sqrt 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_all( vbi, pfx, delta, 
                                           "rsqrtps", Iop_RSqrt32Fx4 );
         goto decode_success;
      }
      break;

   case 0x53:
      /* F3 0F 53 = RCPSS -- approx reciprocal 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_lo32( vbi, pfx, delta,
                                            "rcpss", Iop_Recip32F0x4 );
         goto decode_success;
      }
      /* 0F 53 = RCPPS -- approx reciprocal 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_unary_all( vbi, pfx, delta,
                                           "rcpps", Iop_Recip32Fx4 );
         goto decode_success;
      }
      break;

   case 0x54:
      /* 0F 54 = ANDPS -- G = G and E */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "andps", Iop_AndV128 );
         goto decode_success;
      }
      /* 66 0F 54 = ANDPD -- G = G and E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "andpd", Iop_AndV128 );
         goto decode_success;
      }
      break;

   case 0x55:
      /* 0F 55 = ANDNPS -- G = (not G) and E */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all_invG( vbi, pfx, delta, "andnps",
                                                           Iop_AndV128 );
         goto decode_success;
      }
      /* 66 0F 55 = ANDNPD -- G = (not G) and E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all_invG( vbi, pfx, delta, "andnpd",
                                                           Iop_AndV128 );
         goto decode_success;
      }
      break;

   case 0x56:
      /* 0F 56 = ORPS -- G = G and E */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "orps", Iop_OrV128 );
         goto decode_success;
      }
      /* 66 0F 56 = ORPD -- G = G and E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "orpd", Iop_OrV128 );
         goto decode_success;
      }
      break;

   case 0x57:
      /* 66 0F 57 = XORPD -- G = G xor E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "xorpd", Iop_XorV128 );
         goto decode_success;
      }
      /* 0F 57 = XORPS -- G = G xor E */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "xorps", Iop_XorV128 );
         goto decode_success;
      }
      break;

   case 0x58:
      /* 0F 58 = ADDPS -- add 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "addps", Iop_Add32Fx4 );
         goto decode_success;
      }
      /* F3 0F 58 = ADDSS -- add 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "addss", Iop_Add32F0x4 );
         goto decode_success;
      }
      /* F2 0F 58 = ADDSD -- add 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "addsd", Iop_Add64F0x2 );
         goto decode_success;
      }
      /* 66 0F 58 = ADDPD -- add 32Fx4 from R/M to R */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "addpd", Iop_Add64Fx2 );
         goto decode_success;
      }
      break;

   case 0x59:
      /* F2 0F 59 = MULSD -- mul 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "mulsd", Iop_Mul64F0x2 );
         goto decode_success;
      }
      /* F3 0F 59 = MULSS -- mul 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "mulss", Iop_Mul32F0x4 );
         goto decode_success;
      }
      /* 0F 59 = MULPS -- mul 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "mulps", Iop_Mul32Fx4 );
         goto decode_success;
      }
      /* 66 0F 59 = MULPD -- mul 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "mulpd", Iop_Mul64Fx2 );
         goto decode_success;
      }
      break;

   case 0x5A:
      /* 0F 5A = CVTPS2PD -- convert 2 x F32 in low half mem/xmm to 2 x
         F64 in xmm(G). */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_CVTPS2PD_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      /* F3 0F 5A = CVTSS2SD -- convert F32 in mem/low 1/4 xmm to F64 in
         low half xmm(G) */
      if (haveF3no66noF2(pfx) && sz == 4) {
         IRTemp f32lo = newTemp(Ity_F32);

         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            assign(f32lo, getXMMRegLane32F(eregOfRexRM(pfx,modrm), 0));
            DIP("cvtss2sd %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f32lo, loadLE(Ity_F32, mkexpr(addr)));
            delta += alen;
            DIP("cvtss2sd %s,%s\n", dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         putXMMRegLane64F( gregOfRexRM(pfx,modrm), 0, 
                           unop( Iop_F32toF64, mkexpr(f32lo) ) );

         goto decode_success;
      }
      /* F2 0F 5A = CVTSD2SS -- convert F64 in mem/low half xmm to F32 in
         low 1/4 xmm(G), according to prevailing SSE rounding mode */
      if (haveF2no66noF3(pfx) && sz == 4) {
         IRTemp rmode = newTemp(Ity_I32);
         IRTemp f64lo = newTemp(Ity_F64);

         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            assign(f64lo, getXMMRegLane64F(eregOfRexRM(pfx,modrm), 0));
            DIP("cvtsd2ss %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f64lo, loadLE(Ity_F64, mkexpr(addr)));
            delta += alen;
            DIP("cvtsd2ss %s,%s\n", dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         assign( rmode, get_sse_roundingmode() );
         putXMMRegLane32F( 
            gregOfRexRM(pfx,modrm), 0, 
            binop( Iop_F64toF32, mkexpr(rmode), mkexpr(f64lo) )
         );

         goto decode_success;
      }
      /* 66 0F 5A = CVTPD2PS -- convert 2 x F64 in mem/xmm to 2 x F32 in
         lo half xmm(G), rounding according to prevailing SSE rounding
         mode, and zero upper half */
      /* Note, this is practically identical to CVTPD2DQ.  It would have
         be nice to merge them together. */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_CVTPD2PS_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x5B:
      /* F3 0F 5B = CVTTPS2DQ -- convert 4 x F32 in mem/xmm to 4 x I32 in
         xmm(G), rounding towards zero */
      /* 66 0F 5B = CVTPS2DQ -- convert 4 x F32 in mem/xmm to 4 x I32 in
         xmm(G), as per the prevailing rounding mode */
      if ( (have66noF2noF3(pfx) && sz == 2)
           || (haveF3no66noF2(pfx) && sz == 4) ) {
         Bool r2zero = toBool(sz == 4); // FIXME -- unreliable (???)
         delta = dis_CVTxPS2DQ_128( vbi, pfx, delta, False/*!isAvx*/, r2zero );
         goto decode_success;
      }
      /* 0F 5B = CVTDQ2PS -- convert 4 x I32 in mem/xmm to 4 x F32 in
         xmm(G) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_CVTDQ2PS_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x5C:
      /* F3 0F 5C = SUBSS -- sub 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "subss", Iop_Sub32F0x4 );
         goto decode_success;
      }
      /* F2 0F 5C = SUBSD -- sub 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "subsd", Iop_Sub64F0x2 );
         goto decode_success;
      }
      /* 0F 5C = SUBPS -- sub 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "subps", Iop_Sub32Fx4 );
         goto decode_success;
      }
      /* 66 0F 5C = SUBPD -- sub 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "subpd", Iop_Sub64Fx2 );
         goto decode_success;
      }
      break;

   case 0x5D:
      /* 0F 5D = MINPS -- min 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "minps", Iop_Min32Fx4 );
         goto decode_success;
      }
      /* F3 0F 5D = MINSS -- min 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "minss", Iop_Min32F0x4 );
         goto decode_success;
      }
      /* F2 0F 5D = MINSD -- min 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "minsd", Iop_Min64F0x2 );
         goto decode_success;
      }
      /* 66 0F 5D = MINPD -- min 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "minpd", Iop_Min64Fx2 );
         goto decode_success;
      }
      break;

   case 0x5E:
      /* F2 0F 5E = DIVSD -- div 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "divsd", Iop_Div64F0x2 );
         goto decode_success;
      }
      /* 0F 5E = DIVPS -- div 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "divps", Iop_Div32Fx4 );
         goto decode_success;
      }
      /* F3 0F 5E = DIVSS -- div 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "divss", Iop_Div32F0x4 );
         goto decode_success;
      }
      /* 66 0F 5E = DIVPD -- div 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "divpd", Iop_Div64Fx2 );
         goto decode_success;
      }
      break;

   case 0x5F:
      /* 0F 5F = MAXPS -- max 32Fx4 from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "maxps", Iop_Max32Fx4 );
         goto decode_success;
      }
      /* F3 0F 5F = MAXSS -- max 32F0x4 from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo32( vbi, pfx, delta, "maxss", Iop_Max32F0x4 );
         goto decode_success;
      }
      /* F2 0F 5F = MAXSD -- max 64F0x2 from R/M to R */
      if (haveF2no66noF3(pfx) && sz == 4) {
         delta = dis_SSE_E_to_G_lo64( vbi, pfx, delta, "maxsd", Iop_Max64F0x2 );
         goto decode_success;
      }
      /* 66 0F 5F = MAXPD -- max 64Fx2 from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "maxpd", Iop_Max64Fx2 );
         goto decode_success;
      }
      break;

   case 0x60:
      /* 66 0F 60 = PUNPCKLBW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpcklbw",
                                    Iop_InterleaveLO8x16, True );
         goto decode_success;
      }
      break;

   case 0x61:
      /* 66 0F 61 = PUNPCKLWD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpcklwd",
                                    Iop_InterleaveLO16x8, True );
         goto decode_success;
      }
      break;

   case 0x62:
      /* 66 0F 62 = PUNPCKLDQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpckldq",
                                    Iop_InterleaveLO32x4, True );
         goto decode_success;
      }
      break;

   case 0x63:
      /* 66 0F 63 = PACKSSWB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "packsswb",
                                    Iop_QNarrowBin16Sto8Sx16, True );
         goto decode_success;
      }
      break;

   case 0x64:
      /* 66 0F 64 = PCMPGTB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta, 
                                    "pcmpgtb", Iop_CmpGT8Sx16, False );
         goto decode_success;
      }
      break;

   case 0x65:
      /* 66 0F 65 = PCMPGTW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpgtw", Iop_CmpGT16Sx8, False );
         goto decode_success;
      }
      break;

   case 0x66:
      /* 66 0F 66 = PCMPGTD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpgtd", Iop_CmpGT32Sx4, False );
         goto decode_success;
      }
      break;

   case 0x67:
      /* 66 0F 67 = PACKUSWB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "packuswb",
                                    Iop_QNarrowBin16Sto8Ux16, True );
         goto decode_success;
      }
      break;

   case 0x68:
      /* 66 0F 68 = PUNPCKHBW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpckhbw",
                                    Iop_InterleaveHI8x16, True );
         goto decode_success;
      }
      break;

   case 0x69:
      /* 66 0F 69 = PUNPCKHWD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpckhwd",
                                    Iop_InterleaveHI16x8, True );
         goto decode_success;
      }
      break;

   case 0x6A:
      /* 66 0F 6A = PUNPCKHDQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta, 
                                    "punpckhdq",
                                    Iop_InterleaveHI32x4, True );
         goto decode_success;
      }
      break;

   case 0x6B:
      /* 66 0F 6B = PACKSSDW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "packssdw",
                                    Iop_QNarrowBin32Sto16Sx8, True );
         goto decode_success;
      }
      break;

   case 0x6C:
      /* 66 0F 6C = PUNPCKLQDQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpcklqdq",
                                    Iop_InterleaveLO64x2, True );
         goto decode_success;
      }
      break;

   case 0x6D:
      /* 66 0F 6D = PUNPCKHQDQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "punpckhqdq",
                                    Iop_InterleaveHI64x2, True );
         goto decode_success;
      }
      break;

   case 0x6E:
      /* 66 0F 6E = MOVD from ireg32/m32 to xmm lo 1/4,
                    zeroing high 3/4 of xmm. */
      /*              or from ireg64/m64 to xmm lo 1/2,
                    zeroing high 1/2 of xmm. */
      if (have66noF2noF3(pfx)) {
         vassert(sz == 2 || sz == 8);
         if (sz == 2) sz = 4;
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            if (sz == 4) {
               putXMMReg(
                  gregOfRexRM(pfx,modrm),
                  unop( Iop_32UtoV128, getIReg32(eregOfRexRM(pfx,modrm)) ) 
               );
               DIP("movd %s, %s\n", nameIReg32(eregOfRexRM(pfx,modrm)), 
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
            } else {
               putXMMReg(
                  gregOfRexRM(pfx,modrm),
                  unop( Iop_64UtoV128, getIReg64(eregOfRexRM(pfx,modrm)) ) 
               );
               DIP("movq %s, %s\n", nameIReg64(eregOfRexRM(pfx,modrm)), 
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
            }
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putXMMReg(
               gregOfRexRM(pfx,modrm),
               sz == 4 
                  ?  unop( Iop_32UtoV128,loadLE(Ity_I32, mkexpr(addr)) ) 
                  :  unop( Iop_64UtoV128,loadLE(Ity_I64, mkexpr(addr)) )
            );
            DIP("mov%c %s, %s\n", sz == 4 ? 'd' : 'q', dis_buf, 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         }
         goto decode_success;
      }
      break;

   case 0x6F:
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         /* 66 0F 6F = MOVDQA -- move from E (mem or xmm) to G (xmm). */
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movdqa %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movdqa %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      if (haveF3no66noF2(pfx) && sz == 4) {
         /* F3 0F 6F = MOVDQU -- move from E (mem or xmm) to G (xmm). */
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       getXMMReg( eregOfRexRM(pfx,modrm) ));
            DIP("movdqu %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("movdqu %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x70:
      /* 66 0F 70 = PSHUFD -- rearrange 4x32 from E(xmm or mem) to G(xmm) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PSHUFD_32x4( vbi, pfx, delta, False/*!writesYmm*/);
         goto decode_success;
      }
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F 70 = PSHUFW -- rearrange 4x16 from E(mmx or mem) to G(mmx) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         Int order;
         IRTemp sV, dV, s3, s2, s1, s0;
         s3 = s2 = s1 = s0 = IRTemp_INVALID;
         sV = newTemp(Ity_I64);
         dV = newTemp(Ity_I64);
         do_MMX_preamble();
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            order = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("pshufw $%d,%s,%s\n", order, 
                                      nameMMXReg(eregLO3ofRM(modrm)),
                                      nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf,
                              1/*extra byte after amode*/ );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            order = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("pshufw $%d,%s,%s\n", order, 
                                      dis_buf,
                                      nameMMXReg(gregLO3ofRM(modrm)));
         }
         breakup64to16s( sV, &s3, &s2, &s1, &s0 );
#        define SEL(n) \
                   ((n)==0 ? s0 : ((n)==1 ? s1 : ((n)==2 ? s2 : s3)))
         assign(dV,
   	     mk64from16s( SEL((order>>6)&3), SEL((order>>4)&3),
                             SEL((order>>2)&3), SEL((order>>0)&3) )
         );
         putMMXReg(gregLO3ofRM(modrm), mkexpr(dV));
#        undef SEL
         goto decode_success;
      }
      /* F2 0F 70 = PSHUFLW -- rearrange lower half 4x16 from E(xmm or
         mem) to G(xmm), and copy upper half */
      if (haveF2no66noF3(pfx) && sz == 4) {
         delta = dis_PSHUFxW_128( vbi, pfx, delta,
                                  False/*!isAvx*/, False/*!xIsH*/ );
         goto decode_success;
      }
      /* F3 0F 70 = PSHUFHW -- rearrange upper half 4x16 from E(xmm or
         mem) to G(xmm), and copy lower half */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_PSHUFxW_128( vbi, pfx, delta,
                                  False/*!isAvx*/, True/*xIsH*/ );
         goto decode_success;
      }
      break;

   case 0x71:
      /* 66 0F 71 /2 ib = PSRLW by immediate */
      if (have66noF2noF3(pfx) && sz == 2
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 2) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psrlw", Iop_ShrN16x8 );
         goto decode_success;
      }
      /* 66 0F 71 /4 ib = PSRAW by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 4) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psraw", Iop_SarN16x8 );
         goto decode_success;
      }
      /* 66 0F 71 /6 ib = PSLLW by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 6) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psllw", Iop_ShlN16x8 );
         goto decode_success;
      }
      break;

   case 0x72:
      /* 66 0F 72 /2 ib = PSRLD by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 2) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psrld", Iop_ShrN32x4 );
         goto decode_success;
      }
      /* 66 0F 72 /4 ib = PSRAD by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 4) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psrad", Iop_SarN32x4 );
         goto decode_success;
      }
      /* 66 0F 72 /6 ib = PSLLD by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 6) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "pslld", Iop_ShlN32x4 );
         goto decode_success;
      }
      break;

   case 0x73:
      /* 66 0F 73 /3 ib = PSRLDQ by immediate */
      /* note, if mem case ever filled in, 1 byte after amode */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 3) {
         Int imm = (Int)getUChar(delta+1);
         Int reg = eregOfRexRM(pfx,getUChar(delta));
         DIP("psrldq $%d,%s\n", imm, nameXMMReg(reg));
         delta += 2;
         IRTemp sV = newTemp(Ity_V128);
         assign( sV, getXMMReg(reg) );
         putXMMReg(reg, mkexpr(math_PSRLDQ( sV, imm )));
         goto decode_success;
      }
      /* 66 0F 73 /7 ib = PSLLDQ by immediate */
      /* note, if mem case ever filled in, 1 byte after amode */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 7) {
         Int imm = (Int)getUChar(delta+1);
         Int reg = eregOfRexRM(pfx,getUChar(delta));
         DIP("pslldq $%d,%s\n", imm, nameXMMReg(reg));
         vassert(imm >= 0 && imm <= 255);
         delta += 2;
         IRTemp sV = newTemp(Ity_V128);
         assign( sV, getXMMReg(reg) );
         putXMMReg(reg, mkexpr(math_PSLLDQ( sV, imm )));
         goto decode_success;
      }
      /* 66 0F 73 /2 ib = PSRLQ by immediate */
      if (have66noF2noF3(pfx) && sz == 2
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 2) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psrlq", Iop_ShrN64x2 );
         goto decode_success;
      }
      /* 66 0F 73 /6 ib = PSLLQ by immediate */
      if (have66noF2noF3(pfx) && sz == 2 
          && epartIsReg(getUChar(delta))
          && gregLO3ofRM(getUChar(delta)) == 6) {
         delta = dis_SSE_shiftE_imm( pfx, delta, "psllq", Iop_ShlN64x2 );
         goto decode_success;
      }
      break;

   case 0x74:
      /* 66 0F 74 = PCMPEQB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpeqb", Iop_CmpEQ8x16, False );
         goto decode_success;
      }
      break;

   case 0x75:
      /* 66 0F 75 = PCMPEQW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpeqw", Iop_CmpEQ16x8, False );
         goto decode_success;
      }
      break;

   case 0x76:
      /* 66 0F 76 = PCMPEQD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpeqd", Iop_CmpEQ32x4, False );
         goto decode_success;
      }
      break;

   case 0x7E:
      /* F3 0F 7E = MOVQ -- move 64 bits from E (mem or lo half xmm) to
         G (lo half xmm).  Upper half of G is zeroed out. */
      if (haveF3no66noF2(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 0,
                             getXMMRegLane64( eregOfRexRM(pfx,modrm), 0 ));
               /* zero bits 127:64 */
               putXMMRegLane64( gregOfRexRM(pfx,modrm), 1, mkU64(0) );
            DIP("movsd %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), mkV128(0) );
            putXMMRegLane64( gregOfRexRM(pfx,modrm), 0,
                             loadLE(Ity_I64, mkexpr(addr)) );
            DIP("movsd %s,%s\n", dis_buf,
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      /* 66 0F 7E = MOVD from xmm low 1/4 to ireg32 or m32. */
      /*              or from xmm low 1/2 to ireg64 or m64. */
         if (have66noF2noF3(pfx) && (sz == 2 || sz == 8)) {
         if (sz == 2) sz = 4;
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            if (sz == 4) {
               putIReg32( eregOfRexRM(pfx,modrm),
                          getXMMRegLane32(gregOfRexRM(pfx,modrm), 0) );
               DIP("movd %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), 
                                    nameIReg32(eregOfRexRM(pfx,modrm)));
   	 } else {
               putIReg64( eregOfRexRM(pfx,modrm),
                          getXMMRegLane64(gregOfRexRM(pfx,modrm), 0) );
               DIP("movq %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), 
                                    nameIReg64(eregOfRexRM(pfx,modrm)));
   	 }
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr),
                     sz == 4
                        ? getXMMRegLane32(gregOfRexRM(pfx,modrm),0)
                        : getXMMRegLane64(gregOfRexRM(pfx,modrm),0) );
            DIP("mov%c %s, %s\n", sz == 4 ? 'd' : 'q',
                                  nameXMMReg(gregOfRexRM(pfx,modrm)), dis_buf);
         }
         goto decode_success;
      }
      break;

   case 0x7F:
      /* F3 0F 7F = MOVDQU -- move from G (xmm) to E (mem or xmm). */
      if (haveF3no66noF2(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            goto decode_failure; /* awaiting test case */
            delta += 1;
            putXMMReg( eregOfRexRM(pfx,modrm),
                       getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movdqu %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), 
                                   nameXMMReg(eregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movdqu %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), dis_buf);
         }
         goto decode_success;
      }
      /* 66 0F 7F = MOVDQA -- move from G (xmm) to E (mem or xmm). */
      if (have66noF2noF3(pfx) && sz == 2) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            putXMMReg( eregOfRexRM(pfx,modrm),
                       getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movdqa %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), 
                                   nameXMMReg(eregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            delta += alen;
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movdqa %s, %s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), dis_buf);
         }
         goto decode_success;
      }
      break;

   case 0xAE:
      /* 0F AE /7 = SFENCE -- flush pending operations to memory */
      if (haveNo66noF2noF3(pfx) 
          && epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 7
          && sz == 4) {
         delta += 1;
         /* Insert a memory fence.  It's sometimes important that these
            are carried through to the generated code. */
         stmt( IRStmt_MBE(Imbe_Fence) );
         DIP("sfence\n");
         goto decode_success;
      }
      /* mindless duplication follows .. */
      /* 0F AE /5 = LFENCE -- flush pending operations to memory */
      /* 0F AE /6 = MFENCE -- flush pending operations to memory */
      if (haveNo66noF2noF3(pfx)
          && epartIsReg(getUChar(delta))
          && (gregLO3ofRM(getUChar(delta)) == 5
              || gregLO3ofRM(getUChar(delta)) == 6)
          && sz == 4) {
         delta += 1;
         /* Insert a memory fence.  It's sometimes important that these
            are carried through to the generated code. */
         stmt( IRStmt_MBE(Imbe_Fence) );
         DIP("%sfence\n", gregLO3ofRM(getUChar(delta-1))==5 ? "l" : "m");
         goto decode_success;
      }

      /* 0F AE /7 = CLFLUSH -- flush cache line */
      if (haveNo66noF2noF3(pfx)
          && !epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 7
          && sz == 4) {

         /* This is something of a hack.  We need to know the size of
            the cache line containing addr.  Since we don't (easily),
            assume 256 on the basis that no real cache would have a
            line that big.  It's safe to invalidate more stuff than we
            need, just inefficient. */
         ULong lineszB = 256ULL;

         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;

         /* Round addr down to the start of the containing block. */
         stmt( IRStmt_Put(
                  OFFB_TISTART,
                  binop( Iop_And64, 
                         mkexpr(addr), 
                         mkU64( ~(lineszB-1) ))) );

         stmt( IRStmt_Put(OFFB_TILEN, mkU64(lineszB) ) );

         jmp_lit(dres, Ijk_TInval, (Addr64)(guest_RIP_bbstart+delta));

         DIP("clflush %s\n", dis_buf);
         goto decode_success;
      }

      /* 0F AE /3 = STMXCSR m32 -- store %mxcsr */
      if (haveNo66noF2noF3(pfx)
          && !epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 3
          && sz == 4) {
         delta = dis_STMXCSR(vbi, pfx, delta, False/*!isAvx*/);
         goto decode_success;
      }
      /* 0F AE /2 = LDMXCSR m32 -- load %mxcsr */
      if (haveNo66noF2noF3(pfx)
          && !epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 2
          && sz == 4) {
         delta = dis_LDMXCSR(vbi, pfx, delta, False/*!isAvx*/);
         goto decode_success;
      }
      /* 0F AE /0 = FXSAVE m512 -- write x87 and SSE state to memory.
         Note that the presence or absence of REX.W slightly affects the
         written format: whether the saved FPU IP and DP pointers are 64
         or 32 bits.  But the helper function we call simply writes zero
         bits in the relevant fields (which are 64 bits regardless of
         what REX.W is) and so it's good enough (iow, equally broken) in
         both cases. */
      if (haveNo66noF2noF3(pfx) && (sz == 4 || sz == 8)
          && !epartIsReg(getUChar(delta))
          && gregOfRexRM(pfx,getUChar(delta)) == 0) {
          IRDirty* d;
         modrm = getUChar(delta);
         vassert(!epartIsReg(modrm));

         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         gen_SEGV_if_not_16_aligned(addr);

         DIP("%sfxsave %s\n", sz==8 ? "rex64/" : "", dis_buf);

         /* Uses dirty helper: 
               void amd64g_do_FXSAVE ( VexGuestAMD64State*, ULong ) */
         d = unsafeIRDirty_0_N ( 
                0/*regparms*/, 
                "amd64g_dirtyhelper_FXSAVE", 
                &amd64g_dirtyhelper_FXSAVE,
                mkIRExprVec_1( mkexpr(addr) )
             );
         d->needsBBP = True;

         /* declare we're writing memory */
         d->mFx   = Ifx_Write;
         d->mAddr = mkexpr(addr);
         d->mSize = 464; /* according to recent Intel docs */

         /* declare we're reading guest state */
         d->nFxState = 7;
         vex_bzero(&d->fxState, sizeof(d->fxState));

         d->fxState[0].fx     = Ifx_Read;
         d->fxState[0].offset = OFFB_FTOP;
         d->fxState[0].size   = sizeof(UInt);

         d->fxState[1].fx     = Ifx_Read;
         d->fxState[1].offset = OFFB_FPREGS;
         d->fxState[1].size   = 8 * sizeof(ULong);

         d->fxState[2].fx     = Ifx_Read;
         d->fxState[2].offset = OFFB_FPTAGS;
         d->fxState[2].size   = 8 * sizeof(UChar);

         d->fxState[3].fx     = Ifx_Read;
         d->fxState[3].offset = OFFB_FPROUND;
         d->fxState[3].size   = sizeof(ULong);

         d->fxState[4].fx     = Ifx_Read;
         d->fxState[4].offset = OFFB_FC3210;
         d->fxState[4].size   = sizeof(ULong);

         d->fxState[5].fx     = Ifx_Read;
         d->fxState[5].offset = OFFB_YMM0;
         d->fxState[5].size   = sizeof(U128);
         /* plus 15 more of the above, spaced out in YMM sized steps */
         d->fxState[5].nRepeats  = 15; 
         d->fxState[5].repeatLen = sizeof(U256);

         d->fxState[6].fx     = Ifx_Read;
         d->fxState[6].offset = OFFB_SSEROUND;
         d->fxState[6].size   = sizeof(ULong);

         /* Be paranoid ... this assertion tries to ensure the 16 %ymm
            images are packed back-to-back.  If not, the settings for
            d->fxState[5] are wrong. */
         vassert(32 == sizeof(U256));
         vassert(OFFB_YMM15 == (OFFB_YMM0 + 15 * 32));

         stmt( IRStmt_Dirty(d) );

         goto decode_success;
      }
      /* 0F AE /1 = FXRSTOR m512 -- read x87 and SSE state from memory.
         As with FXSAVE above we ignore the value of REX.W since we're
         not bothering with the FPU DP and IP fields. */
      if (haveNo66noF2noF3(pfx) && (sz == 4 || sz == 8)
          && !epartIsReg(getUChar(delta))
          && gregOfRexRM(pfx,getUChar(delta)) == 1) {
         IRDirty* d;
         modrm = getUChar(delta);
         vassert(!epartIsReg(modrm));

         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         gen_SEGV_if_not_16_aligned(addr);

         DIP("%sfxrstor %s\n", sz==8 ? "rex64/" : "", dis_buf);

         /* Uses dirty helper: 
               VexEmWarn amd64g_do_FXRSTOR ( VexGuestAMD64State*, ULong )
            NOTE:
               the VexEmWarn value is simply ignored
         */
         d = unsafeIRDirty_0_N ( 
                0/*regparms*/, 
                "amd64g_dirtyhelper_FXRSTOR", 
                &amd64g_dirtyhelper_FXRSTOR,
                mkIRExprVec_1( mkexpr(addr) )
             );
         d->needsBBP = True;

         /* declare we're reading memory */
         d->mFx   = Ifx_Read;
         d->mAddr = mkexpr(addr);
         d->mSize = 464; /* according to recent Intel docs */

         /* declare we're writing guest state */
         d->nFxState = 7;
         vex_bzero(&d->fxState, sizeof(d->fxState));

         d->fxState[0].fx     = Ifx_Write;
         d->fxState[0].offset = OFFB_FTOP;
         d->fxState[0].size   = sizeof(UInt);

         d->fxState[1].fx     = Ifx_Write;
         d->fxState[1].offset = OFFB_FPREGS;
         d->fxState[1].size   = 8 * sizeof(ULong);

         d->fxState[2].fx     = Ifx_Write;
         d->fxState[2].offset = OFFB_FPTAGS;
         d->fxState[2].size   = 8 * sizeof(UChar);

         d->fxState[3].fx     = Ifx_Write;
         d->fxState[3].offset = OFFB_FPROUND;
         d->fxState[3].size   = sizeof(ULong);

         d->fxState[4].fx     = Ifx_Write;
         d->fxState[4].offset = OFFB_FC3210;
         d->fxState[4].size   = sizeof(ULong);

         d->fxState[5].fx     = Ifx_Write;
         d->fxState[5].offset = OFFB_YMM0;
         d->fxState[5].size   = sizeof(U128);
         /* plus 15 more of the above, spaced out in YMM sized steps */
         d->fxState[5].nRepeats  = 15; 
         d->fxState[5].repeatLen = sizeof(U256);

         d->fxState[6].fx     = Ifx_Write;
         d->fxState[6].offset = OFFB_SSEROUND;
         d->fxState[6].size   = sizeof(ULong);

         /* Be paranoid ... this assertion tries to ensure the 16 %ymm
            images are packed back-to-back.  If not, the settings for
            d->fxState[5] are wrong. */
         vassert(32 == sizeof(U256));
         vassert(OFFB_YMM15 == (OFFB_YMM0 + 15 * 32));

         stmt( IRStmt_Dirty(d) );

         goto decode_success;
      }
      break;

   case 0xC2:
      /* 0F C2 = CMPPS -- 32Fx4 comparison from R/M to R */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         Long delta0 = delta;
         delta = dis_SSE_cmp_E_to_G( vbi, pfx, delta, "cmpps", True, 4 );
         if (delta > delta0) goto decode_success;
      }
      /* F3 0F C2 = CMPSS -- 32F0x4 comparison from R/M to R */
      if (haveF3no66noF2(pfx) && sz == 4) {
         Long delta0 = delta;
         delta = dis_SSE_cmp_E_to_G( vbi, pfx, delta, "cmpss", False, 4 );
         if (delta > delta0) goto decode_success;
      }
      /* F2 0F C2 = CMPSD -- 64F0x2 comparison from R/M to R */
      if (haveF2no66noF3(pfx) && sz == 4) {
         Long delta0 = delta;
         delta = dis_SSE_cmp_E_to_G( vbi, pfx, delta, "cmpsd", False, 8 );
         if (delta > delta0) goto decode_success;
      }
      /* 66 0F C2 = CMPPD -- 64Fx2 comparison from R/M to R */
      if (have66noF2noF3(pfx) && sz == 2) {
         Long delta0 = delta;
         delta = dis_SSE_cmp_E_to_G( vbi, pfx, delta, "cmppd", True, 8 );
         if (delta > delta0) goto decode_success;
      }
      break;

   case 0xC3:
      /* 0F C3 = MOVNTI -- for us, just a plain ireg store. */
      if (haveNo66noF2noF3(pfx) && (sz == 4 || sz == 8)) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getIRegG(sz, pfx, modrm) );
            DIP("movnti %s,%s\n", dis_buf,
                                  nameIRegG(sz, pfx, modrm));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0xC4:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F C4 = PINSRW -- get 16 bits from E(mem or low half ireg) and
         put it into the specified lane of mmx(G). */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         /* Use t0 .. t3 to hold the 4 original 16-bit lanes of the
            mmx reg.  t4 is the new lane value.  t5 is the original
            mmx value. t6 is the new mmx value. */
         Int lane;
         t4 = newTemp(Ity_I16);
         t5 = newTemp(Ity_I64);
         t6 = newTemp(Ity_I64);
         modrm = getUChar(delta);
         do_MMX_preamble();

         assign(t5, getMMXReg(gregLO3ofRM(modrm)));
         breakup64to16s( t5, &t3, &t2, &t1, &t0 );

         if (epartIsReg(modrm)) {
            assign(t4, getIReg16(eregOfRexRM(pfx,modrm)));
            delta += 1+1;
            lane = getUChar(delta-1);
            DIP("pinsrw $%d,%s,%s\n", (Int)lane, 
                                      nameIReg16(eregOfRexRM(pfx,modrm)),
                                      nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += 1+alen;
            lane = getUChar(delta-1);
            assign(t4, loadLE(Ity_I16, mkexpr(addr)));
            DIP("pinsrw $%d,%s,%s\n", (Int)lane,
                                      dis_buf,
                                      nameMMXReg(gregLO3ofRM(modrm)));
         }

         switch (lane & 3) {
            case 0:  assign(t6, mk64from16s(t3,t2,t1,t4)); break;
            case 1:  assign(t6, mk64from16s(t3,t2,t4,t0)); break;
            case 2:  assign(t6, mk64from16s(t3,t4,t1,t0)); break;
            case 3:  assign(t6, mk64from16s(t4,t2,t1,t0)); break;
            default: vassert(0);
         }
         putMMXReg(gregLO3ofRM(modrm), mkexpr(t6));
         goto decode_success;
      }
      /* 66 0F C4 = PINSRW -- get 16 bits from E(mem or low half ireg) and
         put it into the specified lane of xmm(G). */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         Int lane;
         t4 = newTemp(Ity_I16);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign(t4, getIReg16(rE));
            delta += 1+1;
            lane = getUChar(delta-1);
            DIP("pinsrw $%d,%s,%s\n",
                (Int)lane, nameIReg16(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 
                              1/*byte after the amode*/ );
            delta += 1+alen;
            lane = getUChar(delta-1);
            assign(t4, loadLE(Ity_I16, mkexpr(addr)));
            DIP("pinsrw $%d,%s,%s\n",
                (Int)lane, dis_buf, nameXMMReg(rG));
         }
         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg(rG));
         IRTemp res_vec = math_PINSRW_128( src_vec, t4, lane & 7);
         putXMMReg(rG, mkexpr(res_vec));
         goto decode_success;
      }
      break;

   case 0xC5:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F C5 = PEXTRW -- extract 16-bit field from mmx(E) and put 
         zero-extend of it in ireg(G). */
      if (haveNo66noF2noF3(pfx) && (sz == 4 || sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            IRTemp sV = newTemp(Ity_I64);
            t5 = newTemp(Ity_I16);
            do_MMX_preamble();
            assign(sV, getMMXReg(eregLO3ofRM(modrm)));
            breakup64to16s( sV, &t3, &t2, &t1, &t0 );
            switch (getUChar(delta+1) & 3) {
               case 0:  assign(t5, mkexpr(t0)); break;
               case 1:  assign(t5, mkexpr(t1)); break;
               case 2:  assign(t5, mkexpr(t2)); break;
               case 3:  assign(t5, mkexpr(t3)); break;
               default: vassert(0);
            }
            if (sz == 8)
               putIReg64(gregOfRexRM(pfx,modrm), unop(Iop_16Uto64, mkexpr(t5)));
            else
               putIReg32(gregOfRexRM(pfx,modrm), unop(Iop_16Uto32, mkexpr(t5)));
            DIP("pextrw $%d,%s,%s\n",
                (Int)getUChar(delta+1),
                nameMMXReg(eregLO3ofRM(modrm)),
                sz==8 ? nameIReg64(gregOfRexRM(pfx,modrm))
                      : nameIReg32(gregOfRexRM(pfx,modrm))
            );
            delta += 2;
            goto decode_success;
         } 
         /* else fall through */
         /* note, for anyone filling in the mem case: this insn has one
            byte after the amode and therefore you must pass 1 as the
            last arg to disAMode */
      }
      /* 66 0F C5 = PEXTRW -- extract 16-bit field from xmm(E) and put 
         zero-extend of it in ireg(G). */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         Long delta0 = delta;
         delta = dis_PEXTRW_128_EregOnly_toG( vbi, pfx, delta,
                                              False/*!isAvx*/ );
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      break;

   case 0xC6:
      /* 0F C6 /r ib = SHUFPS -- shuffle packed F32s */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         Int    imm8 = 0;
         IRTemp sV   = newTemp(Ity_V128);
         IRTemp dV   = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx,modrm);
         assign( dV, getXMMReg(rG) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            imm8 = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("shufps $%d,%s,%s\n", imm8, nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            imm8 = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("shufps $%d,%s,%s\n", imm8, dis_buf, nameXMMReg(rG));
         }
         IRTemp res = math_SHUFPS_128( sV, dV, imm8 );
         putXMMReg( gregOfRexRM(pfx,modrm), mkexpr(res) );
         goto decode_success;
      }
      /* 66 0F C6 /r ib = SHUFPD -- shuffle packed F64s */
      if (have66noF2noF3(pfx) && sz == 2) {
         Int    select;
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);

         modrm = getUChar(delta);
         assign( dV, getXMMReg(gregOfRexRM(pfx,modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            select = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("shufpd $%d,%s,%s\n", select, 
                                      nameXMMReg(eregOfRexRM(pfx,modrm)),
                                      nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            select = getUChar(delta+alen);
            delta += 1+alen;
            DIP("shufpd $%d,%s,%s\n", select, 
                                      dis_buf,
                                      nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         IRTemp res = math_SHUFPD_128( sV, dV, select );
         putXMMReg( gregOfRexRM(pfx,modrm), mkexpr(res) );
         goto decode_success;
      }
      break;

   case 0xD1:
      /* 66 0F D1 = PSRLW by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psrlw", Iop_ShrN16x8 );
         goto decode_success;
      }
      break;

   case 0xD2:
      /* 66 0F D2 = PSRLD by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psrld", Iop_ShrN32x4 );
         goto decode_success;
      }
      break;

   case 0xD3:
      /* 66 0F D3 = PSRLQ by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psrlq", Iop_ShrN64x2 );
         goto decode_success;
      }
      break;

   case 0xD4:
      /* 66 0F D4 = PADDQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddq", Iop_Add64x2, False );
         goto decode_success;
      }
      /* ***--- this is an MMX class insn introduced in SSE2 ---*** */
      /* 0F D4 = PADDQ -- add 64x1 */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                   vbi, pfx, delta, opc, "paddq", False );
         goto decode_success;
      }
      break;

   case 0xD5:
      /* 66 0F D5 = PMULLW -- 16x8 multiply */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta, 
                                    "pmullw", Iop_Mul16x8, False );
         goto decode_success;
      }
      break;

   case 0xD6:
      /* F3 0F D6 = MOVQ2DQ -- move from E (mmx) to G (lo half xmm, zero
         hi half). */
      if (haveF3no66noF2(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            do_MMX_preamble();
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       unop(Iop_64UtoV128, getMMXReg( eregLO3ofRM(modrm) )) );
            DIP("movq2dq %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                   nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += 1;
            goto decode_success;
         }
         /* apparently no mem case for this insn */
      }
      /* 66 0F D6 = MOVQ -- move 64 bits from G (lo half xmm) to E (mem
         or lo half xmm).  */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            /* fall through, awaiting test case */
            /* dst: lo half copied, hi half zeroed */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), 
                     getXMMRegLane64( gregOfRexRM(pfx,modrm), 0 ));
            DIP("movq %s,%s\n", nameXMMReg(gregOfRexRM(pfx,modrm)), dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      /* F2 0F D6 = MOVDQ2Q -- move from E (lo half xmm, not mem) to G (mmx). */
      if (haveF2no66noF3(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            do_MMX_preamble();
            putMMXReg( gregLO3ofRM(modrm), 
                       getXMMRegLane64( eregOfRexRM(pfx,modrm), 0 ));
            DIP("movdq2q %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                   nameMMXReg(gregLO3ofRM(modrm)));
            delta += 1;
            goto decode_success;
         }
         /* apparently no mem case for this insn */
      }
      break;

   case 0xD7:
      /* 66 0F D7 = PMOVMSKB -- extract sign bits from each of 16
         lanes in xmm(E), turn them into a byte, and put
         zero-extend of it in ireg(G).  Doing this directly is just
         too cumbersome; give up therefore and call a helper. */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)
          && epartIsReg(getUChar(delta))) { /* no memory case, it seems */
         delta = dis_PMOVMSKB_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F D7 = PMOVMSKB -- extract sign bits from each of 8 lanes in
         mmx(G), turn them into a byte, and put zero-extend of it in
         ireg(G). */
      if (haveNo66noF2noF3(pfx)
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            do_MMX_preamble();
            t0 = newTemp(Ity_I64);
            t1 = newTemp(Ity_I64);
            assign(t0, getMMXReg(eregLO3ofRM(modrm)));
            assign(t1, mkIRExprCCall(
                          Ity_I64, 0/*regparms*/, 
                          "amd64g_calculate_mmx_pmovmskb",
                          &amd64g_calculate_mmx_pmovmskb,
                          mkIRExprVec_1(mkexpr(t0))));
            putIReg32(gregOfRexRM(pfx,modrm), unop(Iop_64to32,mkexpr(t1)));
            DIP("pmovmskb %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                    nameIReg32(gregOfRexRM(pfx,modrm)));
            delta += 1;
            goto decode_success;
         } 
         /* else fall through */
      }
      break;

   case 0xD8:
      /* 66 0F D8 = PSUBUSB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubusb", Iop_QSub8Ux16, False );
         goto decode_success;
      }
      break;

   case 0xD9:
      /* 66 0F D9 = PSUBUSW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubusw", Iop_QSub16Ux8, False );
         goto decode_success;
      }
      break;

   case 0xDA:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F DA = PMINUB -- 8x8 unsigned min */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pminub", False );
         goto decode_success;
      }
      /* 66 0F DA = PMINUB -- 8x16 unsigned min */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pminub", Iop_Min8Ux16, False );
         goto decode_success;
      }
      break;

   case 0xDB:
      /* 66 0F DB = PAND */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "pand", Iop_AndV128 );
         goto decode_success;
      }
      break;

   case 0xDC:
      /* 66 0F DC = PADDUSB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddusb", Iop_QAdd8Ux16, False );
         goto decode_success;
      }
      break;

   case 0xDD:
      /* 66 0F DD = PADDUSW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddusw", Iop_QAdd16Ux8, False );
         goto decode_success;
      }
      break;

   case 0xDE:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F DE = PMAXUB -- 8x8 unsigned max */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pmaxub", False );
         goto decode_success;
      }
      /* 66 0F DE = PMAXUB -- 8x16 unsigned max */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pmaxub", Iop_Max8Ux16, False );
         goto decode_success;
      }
      break;

   case 0xDF:
      /* 66 0F DF = PANDN */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all_invG( vbi, pfx, delta, "pandn", Iop_AndV128 );
         goto decode_success;
      }
      break;

   case 0xE0:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F E0 = PAVGB -- 8x8 unsigned Packed Average, with rounding */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pavgb", False );
         goto decode_success;
      }
      /* 66 0F E0 = PAVGB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pavgb", Iop_Avg8Ux16, False );
         goto decode_success;
      }
      break;

   case 0xE1:
      /* 66 0F E1 = PSRAW by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psraw", Iop_SarN16x8 );
         goto decode_success;
      }
      break;

   case 0xE2:
      /* 66 0F E2 = PSRAD by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psrad", Iop_SarN32x4 );
         goto decode_success;
      }
      break;

   case 0xE3:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F E3 = PAVGW -- 16x4 unsigned Packed Average, with rounding */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pavgw", False );
         goto decode_success;
      }
      /* 66 0F E3 = PAVGW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pavgw", Iop_Avg16Ux8, False );
         goto decode_success;
      }
      break;

   case 0xE4:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F E4 = PMULUH -- 16x4 hi-half of unsigned widening multiply */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pmuluh", False );
         goto decode_success;
      }
      /* 66 0F E4 = PMULHUW -- 16x8 hi-half of unsigned widening multiply */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pmulhuw", Iop_MulHi16Ux8, False );
         goto decode_success;
      }
      break;

   case 0xE5:
      /* 66 0F E5 = PMULHW -- 16x8 hi-half of signed widening multiply */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pmulhw", Iop_MulHi16Sx8, False );
         goto decode_success;
      }
      break;

   case 0xE6:
      /* 66 0F E6 = CVTTPD2DQ -- convert 2 x F64 in mem/xmm to 2 x I32 in
         lo half xmm(G), and zero upper half, rounding towards zero */
      /* F2 0F E6 = CVTPD2DQ -- convert 2 x F64 in mem/xmm to 2 x I32 in
         lo half xmm(G), according to prevailing rounding mode, and zero
         upper half */
      if ( (haveF2no66noF3(pfx) && sz == 4)
           || (have66noF2noF3(pfx) && sz == 2) ) {
         delta = dis_CVTxPD2DQ_128( vbi, pfx, delta, False/*!isAvx*/,
                                    toBool(sz == 2)/*r2zero*/);
         goto decode_success;
      }
      /* F3 0F E6 = CVTDQ2PD -- convert 2 x I32 in mem/lo half xmm to 2 x
         F64 in xmm(G) */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_CVTDQ2PD_128(vbi, pfx, delta, False/*!isAvx*/);
         goto decode_success;
      }
      break;

   case 0xE7:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F E7 = MOVNTQ -- for us, just a plain MMX store.  Note, the
         Intel manual does not say anything about the usual business of
         the FP reg tags getting trashed whenever an MMX insn happens.
         So we just leave them alone. 
      */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            /* do_MMX_preamble(); Intel docs don't specify this */
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getMMXReg(gregLO3ofRM(modrm)) );
            DIP("movntq %s,%s\n", dis_buf,
                                  nameMMXReg(gregLO3ofRM(modrm)));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      /* 66 0F E7 = MOVNTDQ -- for us, just a plain SSE store. */
      if (have66noF2noF3(pfx) && sz == 2) {
         modrm = getUChar(delta);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(gregOfRexRM(pfx,modrm)) );
            DIP("movntdq %s,%s\n", dis_buf,
                                   nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0xE8:
      /* 66 0F E8 = PSUBSB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubsb", Iop_QSub8Sx16, False );
         goto decode_success;
      }
      break;

   case 0xE9:
      /* 66 0F E9 = PSUBSW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubsw", Iop_QSub16Sx8, False );
         goto decode_success;
      }
      break;

   case 0xEA:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F EA = PMINSW -- 16x4 signed min */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pminsw", False );
         goto decode_success;
      }
      /* 66 0F EA = PMINSW -- 16x8 signed min */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pminsw", Iop_Min16Sx8, False );
         goto decode_success;
      }
      break;

   case 0xEB:
      /* 66 0F EB = POR */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "por", Iop_OrV128 );
         goto decode_success;
      }
      break;

   case 0xEC:
      /* 66 0F EC = PADDSB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddsb", Iop_QAdd8Sx16, False );
         goto decode_success;
      }
      break;

   case 0xED:
      /* 66 0F ED = PADDSW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddsw", Iop_QAdd16Sx8, False );
         goto decode_success;
      }
      break;

   case 0xEE:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F EE = PMAXSW -- 16x4 signed max */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "pmaxsw", False );
         goto decode_success;
      }
      /* 66 0F EE = PMAXSW -- 16x8 signed max */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pmaxsw", Iop_Max16Sx8, False );
         goto decode_success;
      }
      break;

   case 0xEF:
      /* 66 0F EF = PXOR */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_E_to_G_all( vbi, pfx, delta, "pxor", Iop_XorV128 );
         goto decode_success;
      }
      break;

   case 0xF1:
      /* 66 0F F1 = PSLLW by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psllw", Iop_ShlN16x8 );
         goto decode_success;
      }
      break;

   case 0xF2:
      /* 66 0F F2 = PSLLD by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "pslld", Iop_ShlN32x4 );
         goto decode_success;
      }
      break;

   case 0xF3:
      /* 66 0F F3 = PSLLQ by E */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSE_shiftG_byE( vbi, pfx, delta, "psllq", Iop_ShlN64x2 );
         goto decode_success;
      }
      break;

   case 0xF4:
      /* 66 0F F4 = PMULUDQ -- unsigned widening multiply of 32-lanes 0 x
         0 to form lower 64-bit half and lanes 2 x 2 to form upper 64-bit
         half */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx,modrm);
         assign( dV, getXMMReg(rG) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("pmuludq %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pmuludq %s,%s\n", dis_buf, nameXMMReg(rG));
         }
         putXMMReg( rG, mkexpr(math_PMULUDQ_128( sV, dV )) );
         goto decode_success;
      }
      /* ***--- this is an MMX class insn introduced in SSE2 ---*** */
      /* 0F F4 = PMULUDQ -- unsigned widening multiply of 32-lanes 0 x
         0 to form 64-bit result */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV = newTemp(Ity_I64);
         IRTemp dV = newTemp(Ity_I64);
         t1 = newTemp(Ity_I32);
         t0 = newTemp(Ity_I32);
         modrm = getUChar(delta);

         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("pmuludq %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                   nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("pmuludq %s,%s\n", dis_buf,
                                   nameMMXReg(gregLO3ofRM(modrm)));
         }

         assign( t0, unop(Iop_64to32, mkexpr(dV)) );
         assign( t1, unop(Iop_64to32, mkexpr(sV)) );
         putMMXReg( gregLO3ofRM(modrm),
                    binop( Iop_MullU32, mkexpr(t0), mkexpr(t1) ) );
         goto decode_success;
      }
      break;

   case 0xF5:
      /* 66 0F F5 = PMADDWD -- Multiply and add packed integers from
         E(xmm or mem) to G(xmm) */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm     = getUChar(delta);
         UInt   rG = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("pmaddwd %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pmaddwd %s,%s\n", dis_buf, nameXMMReg(rG));
         }
         assign( dV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr(math_PMADDWD_128(dV, sV)) );
         goto decode_success;
      }
      break;

   case 0xF6:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F F6 = PSADBW -- sum of 8Ux8 absolute differences */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                    vbi, pfx, delta, opc, "psadbw", False );
         goto decode_success;
      }
      /* 66 0F F6 = PSADBW -- 2 x (8x8 -> 48 zeroes ++ u16) Sum Abs Diffs
         from E(xmm or mem) to G(xmm) */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp sV  = newTemp(Ity_V128);
         IRTemp dV  = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt   rG   = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("psadbw %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("psadbw %s,%s\n", dis_buf, nameXMMReg(rG));
         }
         assign( dV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr( math_PSADBW_128 ( dV, sV ) ) );

         goto decode_success;
      }
      break;

   case 0xF7:
      /* ***--- this is an MMX class insn introduced in SSE1 ---*** */
      /* 0F F7 = MASKMOVQ -- 8x8 masked store */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         Bool ok = False;
         delta = dis_MMX( &ok, vbi, pfx, sz, delta-1 );
         if (ok) goto decode_success;
      }
      /* 66 0F F7 = MASKMOVDQU -- store selected bytes of double quadword */
      if (have66noF2noF3(pfx) && sz == 2 && epartIsReg(getUChar(delta))) {
         delta = dis_MASKMOVDQU( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0xF8:
      /* 66 0F F8 = PSUBB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta, 
                                    "psubb", Iop_Sub8x16, False );
         goto decode_success;
      }
      break;

   case 0xF9:
      /* 66 0F F9 = PSUBW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubw", Iop_Sub16x8, False );
         goto decode_success;
      }
      break;

   case 0xFA:
      /* 66 0F FA = PSUBD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubd", Iop_Sub32x4, False );
         goto decode_success;
      }
      break;

   case 0xFB:
      /* 66 0F FB = PSUBQ */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "psubq", Iop_Sub64x2, False );
         goto decode_success;
      }
      /* ***--- this is an MMX class insn introduced in SSE2 ---*** */
      /* 0F FB = PSUBQ -- sub 64x1 */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         do_MMX_preamble();
         delta = dis_MMXop_regmem_to_reg ( 
                   vbi, pfx, delta, opc, "psubq", False );
         goto decode_success;
      }
      break;

   case 0xFC:
      /* 66 0F FC = PADDB */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddb", Iop_Add8x16, False );
         goto decode_success;
      }
      break;

   case 0xFD:
      /* 66 0F FD = PADDW */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddw", Iop_Add16x8, False );
         goto decode_success;
      }
      break;

   case 0xFE:
      /* 66 0F FE = PADDD */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "paddd", Iop_Add32x4, False );
         goto decode_success;
      }
      break;

   default:
      goto decode_failure;

   }

  decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSE3 (not SupSSE3): dis_ESC_0F__SSE3       ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static Long dis_MOVDDUP_128 ( VexAbiInfo* vbi, Prefix pfx,
                              Long delta, Bool isAvx )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp sV    = newTemp(Ity_V128);
   IRTemp d0    = newTemp(Ity_I64);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      DIP("%smovddup %s,%s\n",
          isAvx ? "v" : "", nameXMMReg(rE), nameXMMReg(rG));
      delta += 1;
      assign ( d0, unop(Iop_V128to64, mkexpr(sV)) );
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( d0, loadLE(Ity_I64, mkexpr(addr)) );
      DIP("%smovddup %s,%s\n",
          isAvx ? "v" : "", dis_buf, nameXMMReg(rG));
      delta += alen;
   }
   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, binop(Iop_64HLtoV128,mkexpr(d0),mkexpr(d0)) );
   return delta;
}


static Long dis_MOVDDUP_256 ( VexAbiInfo* vbi, Prefix pfx,
                              Long delta )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp d0    = newTemp(Ity_I64);
   IRTemp d1    = newTemp(Ity_I64);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      DIP("vmovddup %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
      delta += 1;
      assign ( d0, getYMMRegLane64(rE, 0) );
      assign ( d1, getYMMRegLane64(rE, 2) );
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( d0, loadLE(Ity_I64, mkexpr(addr)) );
      assign( d1, loadLE(Ity_I64, binop(Iop_Add64,
                                        mkexpr(addr), mkU64(16))) );
      DIP("vmovddup %s,%s\n", dis_buf, nameYMMReg(rG));
      delta += alen;
   }
   putYMMRegLane64( rG, 0, mkexpr(d0) );
   putYMMRegLane64( rG, 1, mkexpr(d0) );
   putYMMRegLane64( rG, 2, mkexpr(d1) );
   putYMMRegLane64( rG, 3, mkexpr(d1) );
   return delta;
}


static Long dis_MOVSxDUP_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx, Bool isL )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp sV    = newTemp(Ity_V128);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp s3, s2, s1, s0;
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      DIP("%smovs%cdup %s,%s\n",
          isAvx ? "v" : "", isL ? 'l' : 'h', nameXMMReg(rE), nameXMMReg(rG));
      delta += 1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      if (!isAvx)
         gen_SEGV_if_not_16_aligned( addr );
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      DIP("%smovs%cdup %s,%s\n",
          isAvx ? "v" : "", isL ? 'l' : 'h', dis_buf, nameXMMReg(rG));
      delta += alen;
   }
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, isL ? mkV128from32s( s2, s2, s0, s0 )
                : mkV128from32s( s3, s3, s1, s1 ) );
   return delta;
}


static Long dis_MOVSxDUP_256 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isL )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   IRTemp sV    = newTemp(Ity_V256);
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp s7, s6, s5, s4, s3, s2, s1, s0;
   s7 = s6 = s5 = s4 = s3 = s2 = s1 = s0 = IRTemp_INVALID;
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getYMMReg(rE) );
      DIP("vmovs%cdup %s,%s\n",
          isL ? 'l' : 'h', nameYMMReg(rE), nameYMMReg(rG));
      delta += 1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( sV, loadLE(Ity_V256, mkexpr(addr)) );
      DIP("vmovs%cdup %s,%s\n",
          isL ? 'l' : 'h', dis_buf, nameYMMReg(rG));
      delta += alen;
   }
   breakupV256to32s( sV, &s7, &s6, &s5, &s4, &s3, &s2, &s1, &s0 );
   putYMMRegLane128( rG, 1, isL ? mkV128from32s( s6, s6, s4, s4 )
                                : mkV128from32s( s7, s7, s5, s5 ) );
   putYMMRegLane128( rG, 0, isL ? mkV128from32s( s2, s2, s0, s0 )
                                : mkV128from32s( s3, s3, s1, s1 ) );
   return delta;
}


static IRTemp math_HADDPS_128 ( IRTemp dV, IRTemp sV, Bool isAdd )
{
   IRTemp s3, s2, s1, s0, d3, d2, d1, d0;
   IRTemp leftV  = newTemp(Ity_V128);
   IRTemp rightV = newTemp(Ity_V128);
   s3 = s2 = s1 = s0 = d3 = d2 = d1 = d0 = IRTemp_INVALID;

   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   breakupV128to32s( dV, &d3, &d2, &d1, &d0 );

   assign( leftV,  mkV128from32s( s2, s0, d2, d0 ) );
   assign( rightV, mkV128from32s( s3, s1, d3, d1 ) );

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop(isAdd ? Iop_Add32Fx4 : Iop_Sub32Fx4, 
                              mkexpr(leftV), mkexpr(rightV) ) );
   return res;
}


static IRTemp math_HADDPD_128 ( IRTemp dV, IRTemp sV, Bool isAdd )
{
   IRTemp s1, s0, d1, d0;
   IRTemp leftV  = newTemp(Ity_V128);
   IRTemp rightV = newTemp(Ity_V128);
   s1 = s0 = d1 = d0 = IRTemp_INVALID;

   breakupV128to64s( sV, &s1, &s0 );
   breakupV128to64s( dV, &d1, &d0 );
   
   assign( leftV,  binop(Iop_64HLtoV128, mkexpr(s0), mkexpr(d0)) );
   assign( rightV, binop(Iop_64HLtoV128, mkexpr(s1), mkexpr(d1)) );

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop(isAdd ? Iop_Add64Fx2 : Iop_Sub64Fx2,
                              mkexpr(leftV), mkexpr(rightV) ) );
   return res;
}


__attribute__((noinline))
static
Long dis_ESC_0F__SSE3 ( Bool* decode_OK,
                        VexAbiInfo* vbi,
                        Prefix pfx, Int sz, Long deltaIN )
{
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x12:
      /* F3 0F 12 = MOVSLDUP -- move from E (mem or xmm) to G (xmm),
         duplicating some lanes (2:2:0:0). */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_MOVSxDUP_128( vbi, pfx, delta, False/*!isAvx*/,
                                   True/*isL*/ );
         goto decode_success;
      }
      /* F2 0F 12 = MOVDDUP -- move from E (mem or xmm) to G (xmm),
         duplicating some lanes (0:1:0:1). */
      if (haveF2no66noF3(pfx) 
          && (sz == 4 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_MOVDDUP_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x16:
      /* F3 0F 16 = MOVSHDUP -- move from E (mem or xmm) to G (xmm),
         duplicating some lanes (3:3:1:1). */
      if (haveF3no66noF2(pfx) && sz == 4) {
         delta = dis_MOVSxDUP_128( vbi, pfx, delta, False/*!isAvx*/,
                                   False/*!isL*/ );
         goto decode_success;
      }
      break;

   case 0x7C:
   case 0x7D:
      /* F2 0F 7C = HADDPS -- 32x4 add across from E (mem or xmm) to G (xmm). */
      /* F2 0F 7D = HSUBPS -- 32x4 sub across from E (mem or xmm) to G (xmm). */
      if (haveF2no66noF3(pfx) && sz == 4) {
         IRTemp eV     = newTemp(Ity_V128);
         IRTemp gV     = newTemp(Ity_V128);
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         modrm         = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            DIP("h%sps %s,%s\n", str, nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("h%sps %s,%s\n", str, dis_buf, nameXMMReg(rG));
            delta += alen;
         }

         assign( gV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr( math_HADDPS_128 ( gV, eV, isAdd ) ) );
         goto decode_success;
      }
      /* 66 0F 7C = HADDPD -- 64x2 add across from E (mem or xmm) to G (xmm). */
      /* 66 0F 7D = HSUBPD -- 64x2 sub across from E (mem or xmm) to G (xmm). */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp eV     = newTemp(Ity_V128);
         IRTemp gV     = newTemp(Ity_V128);
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         modrm         = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            DIP("h%spd %s,%s\n", str, nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("h%spd %s,%s\n", str, dis_buf, nameXMMReg(rG));
            delta += alen;
         }

         assign( gV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr( math_HADDPD_128 ( gV, eV, isAdd ) ) );
         goto decode_success;
      }
      break;

   case 0xD0:
      /* 66 0F D0 = ADDSUBPD -- 64x4 +/- from E (mem or xmm) to G (xmm). */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp eV   = newTemp(Ity_V128);
         IRTemp gV   = newTemp(Ity_V128);
         modrm       = getUChar(delta);
         UInt   rG   = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            DIP("addsubpd %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("addsubpd %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }

         assign( gV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr( math_ADDSUBPD_128 ( gV, eV ) ) );
         goto decode_success;
      }
      /* F2 0F D0 = ADDSUBPS -- 32x4 +/-/+/- from E (mem or xmm) to G (xmm). */
      if (haveF2no66noF3(pfx) && sz == 4) {
         IRTemp eV   = newTemp(Ity_V128);
         IRTemp gV   = newTemp(Ity_V128);
         modrm       = getUChar(delta);
         UInt   rG   = gregOfRexRM(pfx,modrm);

         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            DIP("addsubps %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("addsubps %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }

         assign( gV, getXMMReg(rG) );
         putXMMReg( rG, mkexpr( math_ADDSUBPS_128 ( gV, eV ) ) );
         goto decode_success;
      }
      break;

   case 0xF0:
      /* F2 0F F0 = LDDQU -- move from E (mem or xmm) to G (xmm). */
      if (haveF2no66noF3(pfx) && sz == 4) {
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            goto decode_failure;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMReg( gregOfRexRM(pfx,modrm), 
                       loadLE(Ity_V128, mkexpr(addr)) );
            DIP("lddqu %s,%s\n", dis_buf,
                                 nameXMMReg(gregOfRexRM(pfx,modrm)));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   default:
      goto decode_failure;

   }

  decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSSE3: dis_ESC_0F38__SupSSE3               ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static
IRTemp math_PSHUFB_XMM ( IRTemp dV/*data to perm*/, IRTemp sV/*perm*/ )
{
   IRTemp sHi        = newTemp(Ity_I64);
   IRTemp sLo        = newTemp(Ity_I64);
   IRTemp dHi        = newTemp(Ity_I64);
   IRTemp dLo        = newTemp(Ity_I64);
   IRTemp rHi        = newTemp(Ity_I64);
   IRTemp rLo        = newTemp(Ity_I64);
   IRTemp sevens     = newTemp(Ity_I64);
   IRTemp mask0x80hi = newTemp(Ity_I64);
   IRTemp mask0x80lo = newTemp(Ity_I64);
   IRTemp maskBit3hi = newTemp(Ity_I64);
   IRTemp maskBit3lo = newTemp(Ity_I64);
   IRTemp sAnd7hi    = newTemp(Ity_I64);
   IRTemp sAnd7lo    = newTemp(Ity_I64);
   IRTemp permdHi    = newTemp(Ity_I64);
   IRTemp permdLo    = newTemp(Ity_I64);
   IRTemp res        = newTemp(Ity_V128);

   assign( dHi, unop(Iop_V128HIto64, mkexpr(dV)) );
   assign( dLo, unop(Iop_V128to64,   mkexpr(dV)) );
   assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );

   assign( sevens, mkU64(0x0707070707070707ULL) );

   /* mask0x80hi = Not(SarN8x8(sHi,7))
      maskBit3hi = SarN8x8(ShlN8x8(sHi,4),7)
      sAnd7hi    = And(sHi,sevens)
      permdHi    = Or( And(Perm8x8(dHi,sAnd7hi),maskBit3hi),
      And(Perm8x8(dLo,sAnd7hi),Not(maskBit3hi)) )
      rHi        = And(permdHi,mask0x80hi)
   */
   assign(
      mask0x80hi,
      unop(Iop_Not64, binop(Iop_SarN8x8,mkexpr(sHi),mkU8(7))));

   assign(
      maskBit3hi,
      binop(Iop_SarN8x8,
            binop(Iop_ShlN8x8,mkexpr(sHi),mkU8(4)),
            mkU8(7)));

   assign(sAnd7hi, binop(Iop_And64,mkexpr(sHi),mkexpr(sevens)));

   assign(
      permdHi,
      binop(
         Iop_Or64,
         binop(Iop_And64,
               binop(Iop_Perm8x8,mkexpr(dHi),mkexpr(sAnd7hi)),
               mkexpr(maskBit3hi)),
         binop(Iop_And64,
               binop(Iop_Perm8x8,mkexpr(dLo),mkexpr(sAnd7hi)),
               unop(Iop_Not64,mkexpr(maskBit3hi))) ));

   assign(rHi, binop(Iop_And64,mkexpr(permdHi),mkexpr(mask0x80hi)) );

   /* And the same for the lower half of the result.  What fun. */

   assign(
      mask0x80lo,
      unop(Iop_Not64, binop(Iop_SarN8x8,mkexpr(sLo),mkU8(7))));

   assign(
      maskBit3lo,
      binop(Iop_SarN8x8,
            binop(Iop_ShlN8x8,mkexpr(sLo),mkU8(4)),
            mkU8(7)));

   assign(sAnd7lo, binop(Iop_And64,mkexpr(sLo),mkexpr(sevens)));

   assign(
      permdLo,
      binop(
         Iop_Or64,
         binop(Iop_And64,
               binop(Iop_Perm8x8,mkexpr(dHi),mkexpr(sAnd7lo)),
               mkexpr(maskBit3lo)),
         binop(Iop_And64,
               binop(Iop_Perm8x8,mkexpr(dLo),mkexpr(sAnd7lo)),
               unop(Iop_Not64,mkexpr(maskBit3lo))) ));

   assign(rLo, binop(Iop_And64,mkexpr(permdLo),mkexpr(mask0x80lo)) );

   assign(res, binop(Iop_64HLtoV128, mkexpr(rHi), mkexpr(rLo)));
   return res;
}


static Long dis_PHADD_128 ( VexAbiInfo* vbi, Prefix pfx, Long delta,
                            Bool isAvx, UChar opc )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   HChar* str    = "???";
   IROp   opV64  = Iop_INVALID;
   IROp   opCatO = Iop_CatOddLanes16x4;
   IROp   opCatE = Iop_CatEvenLanes16x4;
   IRTemp sV     = newTemp(Ity_V128);
   IRTemp dV     = newTemp(Ity_V128);
   IRTemp sHi    = newTemp(Ity_I64);
   IRTemp sLo    = newTemp(Ity_I64);
   IRTemp dHi    = newTemp(Ity_I64);
   IRTemp dLo    = newTemp(Ity_I64);
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx,modrm);
   UInt   rV     = isAvx ? getVexNvvvv(pfx) : rG;

   switch (opc) {
      case 0x01: opV64 = Iop_Add16x4;   str = "addw";  break;
      case 0x02: opV64 = Iop_Add32x2;   str = "addd";  break;
      case 0x03: opV64 = Iop_QAdd16Sx4; str = "addsw"; break;
      case 0x05: opV64 = Iop_Sub16x4;   str = "subw";  break;
      case 0x06: opV64 = Iop_Sub32x2;   str = "subd";  break;
      case 0x07: opV64 = Iop_QSub16Sx4; str = "subsw"; break;
      default: vassert(0);
   }
   if (opc == 0x02 || opc == 0x06) {
      opCatO = Iop_InterleaveHI32x2;
      opCatE = Iop_InterleaveLO32x2;
   }

   assign( dV, getXMMReg(rV) );

   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      DIP("ph%s %s,%s\n", str, nameXMMReg(rE), nameXMMReg(rG));
      delta += 1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      if (!isAvx)
         gen_SEGV_if_not_16_aligned( addr );
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      DIP("ph%s %s,%s\n", str, dis_buf, nameXMMReg(rG));
      delta += alen;
   }

   assign( dHi, unop(Iop_V128HIto64, mkexpr(dV)) );
   assign( dLo, unop(Iop_V128to64,   mkexpr(dV)) );
   assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );

   /* This isn't a particularly efficient way to compute the
      result, but at least it avoids a proliferation of IROps,
      hence avoids complication all the backends. */
   
   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, 
        binop(Iop_64HLtoV128,
              binop(opV64,
                    binop(opCatE,mkexpr(sHi),mkexpr(sLo)),
                    binop(opCatO,mkexpr(sHi),mkexpr(sLo)) ),
              binop(opV64,
                    binop(opCatE,mkexpr(dHi),mkexpr(dLo)),
                    binop(opCatO,mkexpr(dHi),mkexpr(dLo)) ) ) );
   return delta;
}


static IRTemp math_PMADDUBSW_128 ( IRTemp dV, IRTemp sV )
{
   IRTemp sVoddsSX  = newTemp(Ity_V128);
   IRTemp sVevensSX = newTemp(Ity_V128);
   IRTemp dVoddsZX  = newTemp(Ity_V128);
   IRTemp dVevensZX = newTemp(Ity_V128);
   /* compute dV unsigned x sV signed */
   assign( sVoddsSX, binop(Iop_SarN16x8, mkexpr(sV), mkU8(8)) );
   assign( sVevensSX, binop(Iop_SarN16x8, 
                            binop(Iop_ShlN16x8, mkexpr(sV), mkU8(8)),
                            mkU8(8)) );
   assign( dVoddsZX, binop(Iop_ShrN16x8, mkexpr(dV), mkU8(8)) );
   assign( dVevensZX, binop(Iop_ShrN16x8,
                            binop(Iop_ShlN16x8, mkexpr(dV), mkU8(8)),
                            mkU8(8)) );

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop(Iop_QAdd16Sx8,
                      binop(Iop_Mul16x8, mkexpr(sVoddsSX), mkexpr(dVoddsZX)),
                      binop(Iop_Mul16x8, mkexpr(sVevensSX), mkexpr(dVevensZX))
                     )
         );
   return res;
}


__attribute__((noinline))
static
Long dis_ESC_0F38__SupSSE3 ( Bool* decode_OK,
                             VexAbiInfo* vbi,
                             Prefix pfx, Int sz, Long deltaIN )
{
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x00:
      /* 66 0F 38 00 = PSHUFB -- Packed Shuffle Bytes 8x16 (XMM) */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);

         modrm = getUChar(delta);
         assign( dV, getXMMReg(gregOfRexRM(pfx,modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            delta += 1;
            DIP("pshufb %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pshufb %s,%s\n", dis_buf,
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         IRTemp res = math_PSHUFB_XMM( dV, sV );
         putXMMReg(gregOfRexRM(pfx,modrm), mkexpr(res));
         goto decode_success;
      }
      /* 0F 38 00 = PSHUFB -- Packed Shuffle Bytes 8x8 (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV      = newTemp(Ity_I64);
         IRTemp dV      = newTemp(Ity_I64);

         modrm = getUChar(delta);
         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("pshufb %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                  nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("pshufb %s,%s\n", dis_buf,
                                  nameMMXReg(gregLO3ofRM(modrm)));
         }

         putMMXReg(
            gregLO3ofRM(modrm),
            binop(
               Iop_And64,
               /* permute the lanes */
               binop(
                  Iop_Perm8x8,
                  mkexpr(dV),
                  binop(Iop_And64, mkexpr(sV), mkU64(0x0707070707070707ULL))
               ),
               /* mask off lanes which have (index & 0x80) == 0x80 */
               unop(Iop_Not64, binop(Iop_SarN8x8, mkexpr(sV), mkU8(7)))
            )
         );
         goto decode_success;
      }
      break;

   case 0x01:
   case 0x02:
   case 0x03:
   case 0x05:
   case 0x06:
   case 0x07:
      /* 66 0F 38 01 = PHADDW -- 16x8 add across from E (mem or xmm) and
         G to G (xmm). */
      /* 66 0F 38 02 = PHADDD -- 32x4 add across from E (mem or xmm) and
         G to G (xmm). */
      /* 66 0F 38 03 = PHADDSW -- 16x8 signed qadd across from E (mem or
         xmm) and G to G (xmm). */
      /* 66 0F 38 05 = PHSUBW -- 16x8 sub across from E (mem or xmm) and
         G to G (xmm). */
      /* 66 0F 38 06 = PHSUBD -- 32x4 sub across from E (mem or xmm) and
         G to G (xmm). */
      /* 66 0F 38 07 = PHSUBSW -- 16x8 signed qsub across from E (mem or
         xmm) and G to G (xmm). */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         delta = dis_PHADD_128( vbi, pfx, delta, False/*isAvx*/, opc );
         goto decode_success;
      }
      /* ***--- these are MMX class insns introduced in SSSE3 ---*** */
      /* 0F 38 01 = PHADDW -- 16x4 add across from E (mem or mmx) and G
         to G (mmx). */
      /* 0F 38 02 = PHADDD -- 32x2 add across from E (mem or mmx) and G
         to G (mmx). */
      /* 0F 38 03 = PHADDSW -- 16x4 signed qadd across from E (mem or
         mmx) and G to G (mmx). */
      /* 0F 38 05 = PHSUBW -- 16x4 sub across from E (mem or mmx) and G
         to G (mmx). */
      /* 0F 38 06 = PHSUBD -- 32x2 sub across from E (mem or mmx) and G
         to G (mmx). */
      /* 0F 38 07 = PHSUBSW -- 16x4 signed qsub across from E (mem or
         mmx) and G to G (mmx). */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         HChar* str    = "???";
         IROp   opV64  = Iop_INVALID;
         IROp   opCatO = Iop_CatOddLanes16x4;
         IROp   opCatE = Iop_CatEvenLanes16x4;
         IRTemp sV     = newTemp(Ity_I64);
         IRTemp dV     = newTemp(Ity_I64);

         modrm = getUChar(delta);

         switch (opc) {
            case 0x01: opV64 = Iop_Add16x4;   str = "addw";  break;
            case 0x02: opV64 = Iop_Add32x2;   str = "addd";  break;
            case 0x03: opV64 = Iop_QAdd16Sx4; str = "addsw"; break;
            case 0x05: opV64 = Iop_Sub16x4;   str = "subw";  break;
            case 0x06: opV64 = Iop_Sub32x2;   str = "subd";  break;
            case 0x07: opV64 = Iop_QSub16Sx4; str = "subsw"; break;
            default: vassert(0);
         }
         if (opc == 0x02 || opc == 0x06) {
            opCatO = Iop_InterleaveHI32x2;
            opCatE = Iop_InterleaveLO32x2;
         }

         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("ph%s %s,%s\n", str, nameMMXReg(eregLO3ofRM(modrm)),
                                     nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("ph%s %s,%s\n", str, dis_buf,
                                     nameMMXReg(gregLO3ofRM(modrm)));
         }

         putMMXReg(
            gregLO3ofRM(modrm),
            binop(opV64,
                  binop(opCatE,mkexpr(sV),mkexpr(dV)),
                  binop(opCatO,mkexpr(sV),mkexpr(dV))
            )
         );
         goto decode_success;
      }
      break;

   case 0x04:
      /* 66 0F 38 04 = PMADDUBSW -- Multiply and Add Packed Signed and
         Unsigned Bytes (XMM) */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm     = getUChar(delta);
         UInt   rG = gregOfRexRM(pfx,modrm);

         assign( dV, getXMMReg(rG) );

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("pmaddubsw %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pmaddubsw %s,%s\n", dis_buf, nameXMMReg(rG));
         }

         putXMMReg( rG, mkexpr( math_PMADDUBSW_128( dV, sV ) ) );
         goto decode_success;
      }
      /* 0F 38 04 = PMADDUBSW -- Multiply and Add Packed Signed and
         Unsigned Bytes (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV        = newTemp(Ity_I64);
         IRTemp dV        = newTemp(Ity_I64);
         IRTemp sVoddsSX  = newTemp(Ity_I64);
         IRTemp sVevensSX = newTemp(Ity_I64);
         IRTemp dVoddsZX  = newTemp(Ity_I64);
         IRTemp dVevensZX = newTemp(Ity_I64);

         modrm = getUChar(delta);
         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("pmaddubsw %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                     nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("pmaddubsw %s,%s\n", dis_buf,
                                     nameMMXReg(gregLO3ofRM(modrm)));
         }

         /* compute dV unsigned x sV signed */
         assign( sVoddsSX,
                 binop(Iop_SarN16x4, mkexpr(sV), mkU8(8)) );
         assign( sVevensSX,
                 binop(Iop_SarN16x4, 
                       binop(Iop_ShlN16x4, mkexpr(sV), mkU8(8)), 
                       mkU8(8)) );
         assign( dVoddsZX,
                 binop(Iop_ShrN16x4, mkexpr(dV), mkU8(8)) );
         assign( dVevensZX,
                 binop(Iop_ShrN16x4,
                       binop(Iop_ShlN16x4, mkexpr(dV), mkU8(8)),
                       mkU8(8)) );

         putMMXReg(
            gregLO3ofRM(modrm),
            binop(Iop_QAdd16Sx4,
                  binop(Iop_Mul16x4, mkexpr(sVoddsSX), mkexpr(dVoddsZX)),
                  binop(Iop_Mul16x4, mkexpr(sVevensSX), mkexpr(dVevensZX))
            )
         );
         goto decode_success;
      }
      break;

   case 0x08:
   case 0x09:
   case 0x0A:
      /* 66 0F 38 08 = PSIGNB -- Packed Sign 8x16 (XMM) */
      /* 66 0F 38 09 = PSIGNW -- Packed Sign 16x8 (XMM) */
      /* 66 0F 38 0A = PSIGND -- Packed Sign 32x4 (XMM) */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV      = newTemp(Ity_V128);
         IRTemp dV      = newTemp(Ity_V128);
         IRTemp sHi     = newTemp(Ity_I64);
         IRTemp sLo     = newTemp(Ity_I64);
         IRTemp dHi     = newTemp(Ity_I64);
         IRTemp dLo     = newTemp(Ity_I64);
         HChar* str     = "???";
         Int    laneszB = 0;

         switch (opc) {
            case 0x08: laneszB = 1; str = "b"; break;
            case 0x09: laneszB = 2; str = "w"; break;
            case 0x0A: laneszB = 4; str = "d"; break;
            default: vassert(0);
         }

         modrm = getUChar(delta);
         assign( dV, getXMMReg(gregOfRexRM(pfx,modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            delta += 1;
            DIP("psign%s %s,%s\n", str, nameXMMReg(eregOfRexRM(pfx,modrm)),
                                        nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("psign%s %s,%s\n", str, dis_buf,
                                        nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         assign( dHi, unop(Iop_V128HIto64, mkexpr(dV)) );
         assign( dLo, unop(Iop_V128to64,   mkexpr(dV)) );
         assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
         assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );

         putXMMReg(
            gregOfRexRM(pfx,modrm),
            binop(Iop_64HLtoV128,
                  dis_PSIGN_helper( mkexpr(sHi), mkexpr(dHi), laneszB ),
                  dis_PSIGN_helper( mkexpr(sLo), mkexpr(dLo), laneszB )
            )
         );
         goto decode_success;
      }
      /* 0F 38 08 = PSIGNB -- Packed Sign 8x8  (MMX) */
      /* 0F 38 09 = PSIGNW -- Packed Sign 16x4 (MMX) */
      /* 0F 38 0A = PSIGND -- Packed Sign 32x2 (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV      = newTemp(Ity_I64);
         IRTemp dV      = newTemp(Ity_I64);
         HChar* str     = "???";
         Int    laneszB = 0;

         switch (opc) {
            case 0x08: laneszB = 1; str = "b"; break;
            case 0x09: laneszB = 2; str = "w"; break;
            case 0x0A: laneszB = 4; str = "d"; break;
            default: vassert(0);
         }

         modrm = getUChar(delta);
         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("psign%s %s,%s\n", str, nameMMXReg(eregLO3ofRM(modrm)),
                                        nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("psign%s %s,%s\n", str, dis_buf,
                                        nameMMXReg(gregLO3ofRM(modrm)));
         }

         putMMXReg(
            gregLO3ofRM(modrm),
            dis_PSIGN_helper( mkexpr(sV), mkexpr(dV), laneszB )
         );
         goto decode_success;
      }
      break;

   case 0x0B:
      /* 66 0F 38 0B = PMULHRSW -- Packed Multiply High with Round and
         Scale (XMM) */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV  = newTemp(Ity_V128);
         IRTemp dV  = newTemp(Ity_V128);
         IRTemp sHi = newTemp(Ity_I64);
         IRTemp sLo = newTemp(Ity_I64);
         IRTemp dHi = newTemp(Ity_I64);
         IRTemp dLo = newTemp(Ity_I64);

         modrm = getUChar(delta);
         assign( dV, getXMMReg(gregOfRexRM(pfx,modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            delta += 1;
            DIP("pmulhrsw %s,%s\n", nameXMMReg(eregOfRexRM(pfx,modrm)),
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pmulhrsw %s,%s\n", dis_buf,
                                    nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         assign( dHi, unop(Iop_V128HIto64, mkexpr(dV)) );
         assign( dLo, unop(Iop_V128to64,   mkexpr(dV)) );
         assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
         assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );

         putXMMReg(
            gregOfRexRM(pfx,modrm),
            binop(Iop_64HLtoV128,
                  dis_PMULHRSW_helper( mkexpr(sHi), mkexpr(dHi) ),
                  dis_PMULHRSW_helper( mkexpr(sLo), mkexpr(dLo) )
            )
         );
         goto decode_success;
      }
      /* 0F 38 0B = PMULHRSW -- Packed Multiply High with Round and Scale
         (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV = newTemp(Ity_I64);
         IRTemp dV = newTemp(Ity_I64);

         modrm = getUChar(delta);
         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("pmulhrsw %s,%s\n", nameMMXReg(eregLO3ofRM(modrm)),
                                    nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("pmulhrsw %s,%s\n", dis_buf,
                                    nameMMXReg(gregLO3ofRM(modrm)));
         }

         putMMXReg(
            gregLO3ofRM(modrm),
            dis_PMULHRSW_helper( mkexpr(sV), mkexpr(dV) )
         );
         goto decode_success;
      }
      break;

   case 0x1C:
   case 0x1D:
   case 0x1E:
      /* 66 0F 38 1C = PABSB -- Packed Absolute Value 8x16 (XMM) */
      /* 66 0F 38 1D = PABSW -- Packed Absolute Value 16x8 (XMM) */
      /* 66 0F 38 1E = PABSD -- Packed Absolute Value 32x4 (XMM) */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV  = newTemp(Ity_V128);
         HChar* str = "???";
         Int    laneszB = 0;

         switch (opc) {
            case 0x1C: laneszB = 1; str = "b"; break;
            case 0x1D: laneszB = 2; str = "w"; break;
            case 0x1E: laneszB = 4; str = "d"; break;
            default: vassert(0);
         }

         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            delta += 1;
            DIP("pabs%s %s,%s\n", str, nameXMMReg(eregOfRexRM(pfx,modrm)),
                                       nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pabs%s %s,%s\n", str, dis_buf,
                                       nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         putXMMReg( gregOfRexRM(pfx,modrm),
                    mkexpr(math_PABS_XMM(sV, laneszB)) );
         goto decode_success;
      }
      /* 0F 38 1C = PABSB -- Packed Absolute Value 8x8  (MMX) */
      /* 0F 38 1D = PABSW -- Packed Absolute Value 16x4 (MMX) */
      /* 0F 38 1E = PABSD -- Packed Absolute Value 32x2 (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV      = newTemp(Ity_I64);
         HChar* str     = "???";
         Int    laneszB = 0;

         switch (opc) {
            case 0x1C: laneszB = 1; str = "b"; break;
            case 0x1D: laneszB = 2; str = "w"; break;
            case 0x1E: laneszB = 4; str = "d"; break;
            default: vassert(0);
         }

         modrm = getUChar(delta);
         do_MMX_preamble();

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            delta += 1;
            DIP("pabs%s %s,%s\n", str, nameMMXReg(eregLO3ofRM(modrm)),
                                       nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("pabs%s %s,%s\n", str, dis_buf,
                                       nameMMXReg(gregLO3ofRM(modrm)));
         }

         putMMXReg( gregLO3ofRM(modrm),
                    mkexpr(math_PABS_MMX( sV, laneszB )) );
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSSE3: dis_ESC_0F3A__SupSSE3               ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_0F3A__SupSSE3 ( Bool* decode_OK,
                             VexAbiInfo* vbi,
                             Prefix pfx, Int sz, Long deltaIN )
{
   Long   d64   = 0;
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x0F:
      /* 66 0F 3A 0F = PALIGNR -- Packed Align Right (XMM) */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /*redundant REX.W*/ sz == 8)) {
         IRTemp sV  = newTemp(Ity_V128);
         IRTemp dV  = newTemp(Ity_V128);

         modrm = getUChar(delta);
         assign( dV, getXMMReg(gregOfRexRM(pfx,modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getXMMReg(eregOfRexRM(pfx,modrm)) );
            d64 = (Long)getUChar(delta+1);
            delta += 1+1;
            DIP("palignr $%d,%s,%s\n", (Int)d64,
                                       nameXMMReg(eregOfRexRM(pfx,modrm)),
                                       nameXMMReg(gregOfRexRM(pfx,modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            d64 = (Long)getUChar(delta+alen);
            delta += alen+1;
            DIP("palignr $%d,%s,%s\n", (Int)d64,
                                       dis_buf,
                                       nameXMMReg(gregOfRexRM(pfx,modrm)));
         }

         IRTemp res = math_PALIGNR_XMM( sV, dV, d64 );
         putXMMReg( gregOfRexRM(pfx,modrm), mkexpr(res) );
         goto decode_success;
      }
      /* 0F 3A 0F = PALIGNR -- Packed Align Right (MMX) */
      if (haveNo66noF2noF3(pfx) && sz == 4) {
         IRTemp sV  = newTemp(Ity_I64);
         IRTemp dV  = newTemp(Ity_I64);
         IRTemp res = newTemp(Ity_I64);

         modrm = getUChar(delta);
         do_MMX_preamble();
         assign( dV, getMMXReg(gregLO3ofRM(modrm)) );

         if (epartIsReg(modrm)) {
            assign( sV, getMMXReg(eregLO3ofRM(modrm)) );
            d64 = (Long)getUChar(delta+1);
            delta += 1+1;
            DIP("palignr $%d,%s,%s\n",  (Int)d64, 
                                        nameMMXReg(eregLO3ofRM(modrm)),
                                        nameMMXReg(gregLO3ofRM(modrm)));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( sV, loadLE(Ity_I64, mkexpr(addr)) );
            d64 = (Long)getUChar(delta+alen);
            delta += alen+1;
            DIP("palignr $%d%s,%s\n", (Int)d64,
                                      dis_buf,
                                      nameMMXReg(gregLO3ofRM(modrm)));
         }

         if (d64 == 0) {
            assign( res, mkexpr(sV) );
         }
         else if (d64 >= 1 && d64 <= 7) {
            assign(res, 
                   binop(Iop_Or64,
                         binop(Iop_Shr64, mkexpr(sV), mkU8(8*d64)),
                         binop(Iop_Shl64, mkexpr(dV), mkU8(8*(8-d64))
                        )));
         }
         else if (d64 == 8) {
           assign( res, mkexpr(dV) );
         }
         else if (d64 >= 9 && d64 <= 15) {
            assign( res, binop(Iop_Shr64, mkexpr(dV), mkU8(8*(d64-8))) );
         }
         else if (d64 >= 16 && d64 <= 255) {
            assign( res, mkU64(0) );
         }
         else
            vassert(0);

         putMMXReg( gregLO3ofRM(modrm), mkexpr(res) );
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSE4: dis_ESC_0F__SSE4                     ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_0F__SSE4 ( Bool* decode_OK,
                        VexArchInfo* archinfo,
                        VexAbiInfo* vbi,
                        Prefix pfx, Int sz, Long deltaIN )
{
   IRTemp addr  = IRTemp_INVALID;
   IRType ty    = Ity_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0xB8:
      /* F3 0F B8  = POPCNT{W,L,Q}
         Count the number of 1 bits in a register
      */
      if (haveF3noF2(pfx) /* so both 66 and REX.W are possibilities */
          && (sz == 2 || sz == 4 || sz == 8)) {
         /*IRType*/ ty  = szToITy(sz);
         IRTemp     src = newTemp(ty);
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            assign(src, getIRegE(sz, pfx, modrm));
            delta += 1;
            DIP("popcnt%c %s, %s\n", nameISize(sz), nameIRegE(sz, pfx, modrm),
                nameIRegG(sz, pfx, modrm));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0);
            assign(src, loadLE(ty, mkexpr(addr)));
            delta += alen;
            DIP("popcnt%c %s, %s\n", nameISize(sz), dis_buf,
                nameIRegG(sz, pfx, modrm));
         }

         IRTemp result = gen_POPCOUNT(ty, src);
         putIRegG(sz, pfx, modrm, mkexpr(result));

         // Update flags.  This is pretty lame .. perhaps can do better
         // if this turns out to be performance critical.
         // O S A C P are cleared.  Z is set if SRC == 0.
         stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
         stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
         stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
         stmt( IRStmt_Put( OFFB_CC_DEP1,
               binop(Iop_Shl64,
                     unop(Iop_1Uto64,
                          binop(Iop_CmpEQ64,
                                widenUto64(mkexpr(src)),
                                mkU64(0))),
                     mkU8(AMD64G_CC_SHIFT_Z))));

         goto decode_success;
      }
      break;

   case 0xBD:
      /* F3 0F BD -- LZCNT (count leading zeroes.  An AMD extension,
         which we can only decode if we're sure this is an AMD cpu
         that supports LZCNT, since otherwise it's BSR, which behaves
         differently.  Bizarrely, my Sandy Bridge also accepts these
         instructions but produces different results. */
      if (haveF3noF2(pfx) /* so both 66 and 48 are possibilities */
          && (sz == 2 || sz == 4 || sz == 8) 
          && 0 != (archinfo->hwcaps & VEX_HWCAPS_AMD64_LZCNT)) {
         /*IRType*/ ty  = szToITy(sz);
         IRTemp     src = newTemp(ty);
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            assign(src, getIRegE(sz, pfx, modrm));
            delta += 1;
            DIP("lzcnt%c %s, %s\n", nameISize(sz), nameIRegE(sz, pfx, modrm),
                nameIRegG(sz, pfx, modrm));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0);
            assign(src, loadLE(ty, mkexpr(addr)));
            delta += alen;
            DIP("lzcnt%c %s, %s\n", nameISize(sz), dis_buf,
                nameIRegG(sz, pfx, modrm));
         }

         IRTemp res = gen_LZCNT(ty, src);
         putIRegG(sz, pfx, modrm, mkexpr(res));

         // Update flags.  This is pretty lame .. perhaps can do better
         // if this turns out to be performance critical.
         // O S A P are cleared.  Z is set if RESULT == 0.
         // C is set if SRC is zero.
         IRTemp src64 = newTemp(Ity_I64);
         IRTemp res64 = newTemp(Ity_I64);
         assign(src64, widenUto64(mkexpr(src)));
         assign(res64, widenUto64(mkexpr(res)));

         IRTemp oszacp = newTemp(Ity_I64);
         assign(
            oszacp,
            binop(Iop_Or64,
                  binop(Iop_Shl64,
                        unop(Iop_1Uto64,
                             binop(Iop_CmpEQ64, mkexpr(res64), mkU64(0))),
                        mkU8(AMD64G_CC_SHIFT_Z)),
                  binop(Iop_Shl64,
                        unop(Iop_1Uto64,
                             binop(Iop_CmpEQ64, mkexpr(src64), mkU64(0))),
                        mkU8(AMD64G_CC_SHIFT_C))
            )
         );

         stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
         stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
         stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
         stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(oszacp) ));

         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSE4: dis_ESC_0F38__SSE4                   ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static IRTemp math_PBLENDVB_128 ( IRTemp vecE, IRTemp vecG,
                                  IRTemp vec0/*controlling mask*/,
                                  UInt gran, IROp opSAR )
{
   /* The tricky bit is to convert vec0 into a suitable mask, by
      copying the most significant bit of each lane into all positions
      in the lane. */
   IRTemp sh = newTemp(Ity_I8);
   assign(sh, mkU8(8 * gran - 1));

   IRTemp mask = newTemp(Ity_V128);
   assign(mask, binop(opSAR, mkexpr(vec0), mkexpr(sh)));

   IRTemp notmask = newTemp(Ity_V128);
   assign(notmask, unop(Iop_NotV128, mkexpr(mask)));

   IRTemp res = newTemp(Ity_V128);
   assign(res,  binop(Iop_OrV128,
                      binop(Iop_AndV128, mkexpr(vecE), mkexpr(mask)),
                      binop(Iop_AndV128, mkexpr(vecG), mkexpr(notmask))));
   return res;
}

static IRTemp math_PBLENDVB_256 ( IRTemp vecE, IRTemp vecG,
                                  IRTemp vec0/*controlling mask*/,
                                  UInt gran, IROp opSAR128 )
{
   /* The tricky bit is to convert vec0 into a suitable mask, by
      copying the most significant bit of each lane into all positions
      in the lane. */
   IRTemp sh = newTemp(Ity_I8);
   assign(sh, mkU8(8 * gran - 1));

   IRTemp vec0Hi = IRTemp_INVALID;
   IRTemp vec0Lo = IRTemp_INVALID;
   breakupV256toV128s( vec0, &vec0Hi, &vec0Lo );

   IRTemp mask = newTemp(Ity_V256);
   assign(mask, binop(Iop_V128HLtoV256,
                      binop(opSAR128, mkexpr(vec0Hi), mkexpr(sh)),
                      binop(opSAR128, mkexpr(vec0Lo), mkexpr(sh))));

   IRTemp notmask = newTemp(Ity_V256);
   assign(notmask, unop(Iop_NotV256, mkexpr(mask)));

   IRTemp res = newTemp(Ity_V256);
   assign(res,  binop(Iop_OrV256,
                      binop(Iop_AndV256, mkexpr(vecE), mkexpr(mask)),
                      binop(Iop_AndV256, mkexpr(vecG), mkexpr(notmask))));
   return res;
}

static Long dis_VBLENDV_128 ( VexAbiInfo* vbi, Prefix pfx, Long delta,
                              const HChar *name, UInt gran, IROp opSAR )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx, modrm);
   UInt   rV     = getVexNvvvv(pfx);
   UInt   rIS4   = 0xFF; /* invalid */
   IRTemp vecE   = newTemp(Ity_V128);
   IRTemp vecV   = newTemp(Ity_V128);
   IRTemp vecIS4 = newTemp(Ity_V128);
   if (epartIsReg(modrm)) {
      delta++;
      UInt rE = eregOfRexRM(pfx, modrm);
      assign(vecE, getXMMReg(rE));
      UChar ib = getUChar(delta);
      rIS4 = (ib >> 4) & 0xF;
      DIP("%s %s,%s,%s,%s\n",
          name, nameXMMReg(rIS4), nameXMMReg(rE),
          nameXMMReg(rV), nameXMMReg(rG));
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      delta += alen;
      assign(vecE, loadLE(Ity_V128, mkexpr(addr)));
      UChar ib = getUChar(delta);
      rIS4 = (ib >> 4) & 0xF;
      DIP("%s %s,%s,%s,%s\n",
          name, nameXMMReg(rIS4), dis_buf, nameXMMReg(rV), nameXMMReg(rG));
   }
   delta++;
   assign(vecV,   getXMMReg(rV));
   assign(vecIS4, getXMMReg(rIS4));
   IRTemp res = math_PBLENDVB_128( vecE, vecV, vecIS4, gran, opSAR );
   putYMMRegLoAndZU( rG, mkexpr(res) );
   return delta;
}

static Long dis_VBLENDV_256 ( VexAbiInfo* vbi, Prefix pfx, Long delta,
                              const HChar *name, UInt gran, IROp opSAR128 )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx, modrm);
   UInt   rV     = getVexNvvvv(pfx);
   UInt   rIS4   = 0xFF; /* invalid */
   IRTemp vecE   = newTemp(Ity_V256);
   IRTemp vecV   = newTemp(Ity_V256);
   IRTemp vecIS4 = newTemp(Ity_V256);
   if (epartIsReg(modrm)) {
      delta++;
      UInt rE = eregOfRexRM(pfx, modrm);
      assign(vecE, getYMMReg(rE));
      UChar ib = getUChar(delta);
      rIS4 = (ib >> 4) & 0xF;
      DIP("%s %s,%s,%s,%s\n",
          name, nameYMMReg(rIS4), nameYMMReg(rE),
          nameYMMReg(rV), nameYMMReg(rG));
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      delta += alen;
      assign(vecE, loadLE(Ity_V256, mkexpr(addr)));
      UChar ib = getUChar(delta);
      rIS4 = (ib >> 4) & 0xF;
      DIP("%s %s,%s,%s,%s\n",
          name, nameYMMReg(rIS4), dis_buf, nameYMMReg(rV), nameYMMReg(rG));
   }
   delta++;
   assign(vecV,   getYMMReg(rV));
   assign(vecIS4, getYMMReg(rIS4));
   IRTemp res = math_PBLENDVB_256( vecE, vecV, vecIS4, gran, opSAR128 );
   putYMMReg( rG, mkexpr(res) );
   return delta;
}

static void finish_xTESTy ( IRTemp andV, IRTemp andnV, Int sign )
{
   /* Set Z=1 iff (vecE & vecG) == 0
      Set C=1 iff (vecE & not vecG) == 0
   */

   /* andV, andnV:  vecE & vecG,  vecE and not(vecG) */

   /* andV resp. andnV, reduced to 64-bit values, by or-ing the top
      and bottom 64-bits together.  It relies on this trick:

      InterleaveLO64x2([a,b],[c,d]) == [b,d]    hence

      InterleaveLO64x2([a,b],[a,b]) == [b,b]    and similarly
      InterleaveHI64x2([a,b],[a,b]) == [a,a] 

      and so the OR of the above 2 exprs produces
      [a OR b, a OR b], from which we simply take the lower half.
   */
   IRTemp and64  = newTemp(Ity_I64);
   IRTemp andn64 = newTemp(Ity_I64);

   assign(and64,
          unop(Iop_V128to64,
               binop(Iop_OrV128,
                     binop(Iop_InterleaveLO64x2,
                           mkexpr(andV), mkexpr(andV)),
                     binop(Iop_InterleaveHI64x2,
                           mkexpr(andV), mkexpr(andV)))));

   assign(andn64,
          unop(Iop_V128to64,
               binop(Iop_OrV128,
                     binop(Iop_InterleaveLO64x2,
                           mkexpr(andnV), mkexpr(andnV)),
                     binop(Iop_InterleaveHI64x2,
                           mkexpr(andnV), mkexpr(andnV)))));

   IRTemp z64 = newTemp(Ity_I64);
   IRTemp c64 = newTemp(Ity_I64);
   if (sign == 64) {
      /* When only interested in the most significant bit, just shift
         arithmetically right and negate.  */
      assign(z64,
             unop(Iop_Not64,
                  binop(Iop_Sar64, mkexpr(and64), mkU8(63))));

      assign(c64,
             unop(Iop_Not64,
                  binop(Iop_Sar64, mkexpr(andn64), mkU8(63))));
   } else {
      if (sign == 32) {
         /* When interested in bit 31 and bit 63, mask those bits and
            fallthrough into the PTEST handling.  */
         IRTemp t0 = newTemp(Ity_I64);
         IRTemp t1 = newTemp(Ity_I64);
         IRTemp t2 = newTemp(Ity_I64);
         assign(t0, mkU64(0x8000000080000000ULL));
         assign(t1, binop(Iop_And64, mkexpr(and64), mkexpr(t0)));
         assign(t2, binop(Iop_And64, mkexpr(andn64), mkexpr(t0)));
         and64 = t1;
         andn64 = t2;
      }
      /* Now convert and64, andn64 to all-zeroes or all-1s, so we can
         slice out the Z and C bits conveniently.  We use the standard
         trick all-zeroes -> all-zeroes, anything-else -> all-ones
         done by "(x | -x) >>s (word-size - 1)".
      */
      assign(z64,
             unop(Iop_Not64,
                  binop(Iop_Sar64,
                        binop(Iop_Or64,
                              binop(Iop_Sub64, mkU64(0), mkexpr(and64)),
                                    mkexpr(and64)), mkU8(63))));

      assign(c64,
             unop(Iop_Not64,
                  binop(Iop_Sar64,
                        binop(Iop_Or64,
                              binop(Iop_Sub64, mkU64(0), mkexpr(andn64)),
                                    mkexpr(andn64)), mkU8(63))));
   }

   /* And finally, slice out the Z and C flags and set the flags
      thunk to COPY for them.  OSAP are set to zero. */
   IRTemp newOSZACP = newTemp(Ity_I64);
   assign(newOSZACP, 
          binop(Iop_Or64,
                binop(Iop_And64, mkexpr(z64), mkU64(AMD64G_CC_MASK_Z)),
                binop(Iop_And64, mkexpr(c64), mkU64(AMD64G_CC_MASK_C))));

   stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(newOSZACP)));
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
}


/* Handles 128 bit versions of PTEST, VTESTPS or VTESTPD.
   sign is 0 for PTEST insn, 32 for VTESTPS and 64 for VTESTPD. */
static Long dis_xTESTy_128 ( VexAbiInfo* vbi, Prefix pfx,
                             Long delta, Bool isAvx, Int sign )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx, modrm);
   IRTemp vecE = newTemp(Ity_V128);
   IRTemp vecG = newTemp(Ity_V128);

   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign(vecE, getXMMReg(rE));
      delta += 1;
      DIP( "%s%stest%s %s,%s\n",
           isAvx ? "v" : "", sign == 0 ? "p" : "",
           sign == 0 ? "" : sign == 32 ? "ps" : "pd",
           nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      if (!isAvx)
         gen_SEGV_if_not_16_aligned( addr );
      assign(vecE, loadLE( Ity_V128, mkexpr(addr) ));
      delta += alen;
      DIP( "%s%stest%s %s,%s\n",
           isAvx ? "v" : "", sign == 0 ? "p" : "",
           sign == 0 ? "" : sign == 32 ? "ps" : "pd",
           dis_buf, nameXMMReg(rG) );
   }

   assign(vecG, getXMMReg(rG));

   /* Set Z=1 iff (vecE & vecG) == 0
      Set C=1 iff (vecE & not vecG) == 0
   */

   /* andV, andnV:  vecE & vecG,  vecE and not(vecG) */
   IRTemp andV  = newTemp(Ity_V128);
   IRTemp andnV = newTemp(Ity_V128);
   assign(andV,  binop(Iop_AndV128, mkexpr(vecE), mkexpr(vecG)));
   assign(andnV, binop(Iop_AndV128,
                       mkexpr(vecE),
                       binop(Iop_XorV128, mkexpr(vecG),
                                          mkV128(0xFFFF))));

   finish_xTESTy ( andV, andnV, sign );
   return delta;
}


/* Handles 256 bit versions of PTEST, VTESTPS or VTESTPD.
   sign is 0 for PTEST insn, 32 for VTESTPS and 64 for VTESTPD. */
static Long dis_xTESTy_256 ( VexAbiInfo* vbi, Prefix pfx,
                             Long delta, Int sign )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx, modrm);
   IRTemp vecE   = newTemp(Ity_V256);
   IRTemp vecG   = newTemp(Ity_V256);

   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign(vecE, getYMMReg(rE));
      delta += 1;
      DIP( "v%stest%s %s,%s\n", sign == 0 ? "p" : "",
           sign == 0 ? "" : sign == 32 ? "ps" : "pd",
           nameYMMReg(rE), nameYMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(vecE, loadLE( Ity_V256, mkexpr(addr) ));
      delta += alen;
      DIP( "v%stest%s %s,%s\n", sign == 0 ? "p" : "",
           sign == 0 ? "" : sign == 32 ? "ps" : "pd",
           dis_buf, nameYMMReg(rG) );
   }

   assign(vecG, getYMMReg(rG));

   /* Set Z=1 iff (vecE & vecG) == 0
      Set C=1 iff (vecE & not vecG) == 0
   */

   /* andV, andnV:  vecE & vecG,  vecE and not(vecG) */
   IRTemp andV  = newTemp(Ity_V256);
   IRTemp andnV = newTemp(Ity_V256);
   assign(andV,  binop(Iop_AndV256, mkexpr(vecE), mkexpr(vecG)));
   assign(andnV, binop(Iop_AndV256,
                       mkexpr(vecE), unop(Iop_NotV256, mkexpr(vecG))));

   IRTemp andVhi  = IRTemp_INVALID;
   IRTemp andVlo  = IRTemp_INVALID;
   IRTemp andnVhi = IRTemp_INVALID;
   IRTemp andnVlo = IRTemp_INVALID;
   breakupV256toV128s( andV, &andVhi, &andVlo );
   breakupV256toV128s( andnV, &andnVhi, &andnVlo );

   IRTemp andV128  = newTemp(Ity_V128);
   IRTemp andnV128 = newTemp(Ity_V128);
   assign( andV128, binop( Iop_OrV128, mkexpr(andVhi), mkexpr(andVlo) ) );
   assign( andnV128, binop( Iop_OrV128, mkexpr(andnVhi), mkexpr(andnVlo) ) );

   finish_xTESTy ( andV128, andnV128, sign );
   return delta;
}


/* Handles 128 bit versions of PMOVZXBW and PMOVSXBW. */
static Long dis_PMOVxXBW_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx, Bool xIsZ )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp srcVec = newTemp(Ity_V128);
   UChar  modrm  = getUChar(delta);
   UChar* mbV    = isAvx ? "v" : "";
   UChar  how    = xIsZ ? 'z' : 's';
   UInt   rG     = gregOfRexRM(pfx, modrm);
   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      delta += 1;
      DIP( "%spmov%cxbw %s,%s\n", mbV, how, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcVec, 
              unop( Iop_64UtoV128, loadLE( Ity_I64, mkexpr(addr) ) ) );
      delta += alen;
      DIP( "%spmov%cxbw %s,%s\n", mbV, how, dis_buf, nameXMMReg(rG) );
   }

   IRExpr* res 
      = xIsZ /* do math for either zero or sign extend */
        ? binop( Iop_InterleaveLO8x16, 
                 IRExpr_Const( IRConst_V128(0) ), mkexpr(srcVec) )
        : binop( Iop_SarN16x8, 
                 binop( Iop_ShlN16x8, 
                        binop( Iop_InterleaveLO8x16,
                               IRExpr_Const( IRConst_V128(0) ),
                               mkexpr(srcVec) ),
                        mkU8(8) ),
                 mkU8(8) );

   (isAvx ? putYMMRegLoAndZU : putXMMReg) ( rG, res );

   return delta;
}


static Long dis_PMOVxXWD_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx, Bool xIsZ )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp srcVec = newTemp(Ity_V128);
   UChar  modrm  = getUChar(delta);
   UChar* mbV    = isAvx ? "v" : "";
   UChar  how    = xIsZ ? 'z' : 's';
   UInt   rG     = gregOfRexRM(pfx, modrm);

   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      delta += 1;
      DIP( "%spmov%cxwd %s,%s\n", mbV, how, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcVec, 
              unop( Iop_64UtoV128, loadLE( Ity_I64, mkexpr(addr) ) ) );
      delta += alen;
      DIP( "%spmov%cxwd %s,%s\n", mbV, how, dis_buf, nameXMMReg(rG) );
   }

   IRExpr* res
      = binop( Iop_InterleaveLO16x8,  
               IRExpr_Const( IRConst_V128(0) ), mkexpr(srcVec) );
   if (!xIsZ)
      res = binop(Iop_SarN32x4, 
                  binop(Iop_ShlN32x4, res, mkU8(16)), mkU8(16));

   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( gregOfRexRM(pfx, modrm), res );

   return delta;
}


static Long dis_PMOVSXWQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr     = IRTemp_INVALID;
   Int    alen     = 0;
   HChar  dis_buf[50];
   IRTemp srcBytes = newTemp(Ity_I32);
   UChar  modrm    = getUChar(delta);
   UChar* mbV      = isAvx ? "v" : "";
   UInt   rG       = gregOfRexRM(pfx, modrm);

   if ( epartIsReg( modrm ) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcBytes, getXMMRegLane32( rE, 0 ) );
      delta += 1;
      DIP( "%spmovsxwq %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcBytes, loadLE( Ity_I32, mkexpr(addr) ) );
      delta += alen;
      DIP( "%spmovsxwq %s,%s\n", mbV, dis_buf, nameXMMReg(rG) );
   }

   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, binop( Iop_64HLtoV128, 
                   unop( Iop_16Sto64,
                         unop( Iop_32HIto16, mkexpr(srcBytes) ) ),
                   unop( Iop_16Sto64, 
                         unop( Iop_32to16, mkexpr(srcBytes) ) ) ) );
   return delta;
}


static Long dis_PMOVZXWQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr     = IRTemp_INVALID;
   Int    alen     = 0;
   HChar  dis_buf[50];
   IRTemp srcVec = newTemp(Ity_V128);
   UChar  modrm    = getUChar(delta);
   UChar* mbV      = isAvx ? "v" : "";
   UInt   rG       = gregOfRexRM(pfx, modrm);

   if ( epartIsReg( modrm ) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      delta += 1;
      DIP( "%spmovzxwq %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcVec, 
              unop( Iop_32UtoV128, loadLE( Ity_I32, mkexpr(addr) ) ) );
      delta += alen;
      DIP( "%spmovzxwq %s,%s\n", mbV, dis_buf, nameXMMReg(rG) );
   }

   IRTemp zeroVec = newTemp( Ity_V128 );
   assign( zeroVec, IRExpr_Const( IRConst_V128(0) ) );

   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, binop( Iop_InterleaveLO16x8, 
                   mkexpr(zeroVec), 
                   binop( Iop_InterleaveLO16x8, 
                          mkexpr(zeroVec), mkexpr(srcVec) ) ) );
   return delta;
}


/* Handles 128 bit versions of PMOVZXDQ and PMOVSXDQ. */
static Long dis_PMOVxXDQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx, Bool xIsZ )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp srcI64 = newTemp(Ity_I64);
   IRTemp srcVec = newTemp(Ity_V128);
   UChar  modrm  = getUChar(delta);
   UChar* mbV    = isAvx ? "v" : "";
   UChar  how    = xIsZ ? 'z' : 's';
   UInt   rG     = gregOfRexRM(pfx, modrm);
   /* Compute both srcI64 -- the value to expand -- and srcVec -- same
      thing in a V128, with arbitrary junk in the top 64 bits.  Use
      one or both of them and let iropt clean up afterwards (as
      usual). */
   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      assign( srcI64, unop(Iop_V128to64, mkexpr(srcVec)) );
      delta += 1;
      DIP( "%spmov%cxdq %s,%s\n", mbV, how, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcI64, loadLE(Ity_I64, mkexpr(addr)) );
      assign( srcVec, unop( Iop_64UtoV128, mkexpr(srcI64)) );
      delta += alen;
      DIP( "%spmov%cxdq %s,%s\n", mbV, how, dis_buf, nameXMMReg(rG) );
   }

   IRExpr* res 
      = xIsZ /* do math for either zero or sign extend */
        ? binop( Iop_InterleaveLO32x4, 
                 IRExpr_Const( IRConst_V128(0) ), mkexpr(srcVec) )
        : binop( Iop_64HLtoV128, 
                 unop( Iop_32Sto64, 
                       unop( Iop_64HIto32, mkexpr(srcI64) ) ), 
                 unop( Iop_32Sto64, 
                       unop( Iop_64to32, mkexpr(srcI64) ) ) );

   (isAvx ? putYMMRegLoAndZU : putXMMReg) ( rG, res );

   return delta;
}


/* Handles 128 bit versions of PMOVZXBD and PMOVSXBD. */
static Long dis_PMOVxXBD_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx, Bool xIsZ )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   IRTemp srcVec = newTemp(Ity_V128);
   UChar  modrm  = getUChar(delta);
   UChar* mbV    = isAvx ? "v" : "";
   UChar  how    = xIsZ ? 'z' : 's';
   UInt   rG     = gregOfRexRM(pfx, modrm);
   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      delta += 1;
      DIP( "%spmov%cxbd %s,%s\n", mbV, how, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcVec, 
              unop( Iop_32UtoV128, loadLE( Ity_I32, mkexpr(addr) ) ) );
      delta += alen;
      DIP( "%spmov%cxbd %s,%s\n", mbV, how, dis_buf, nameXMMReg(rG) );
   }

   IRTemp zeroVec = newTemp(Ity_V128);
   assign( zeroVec, IRExpr_Const( IRConst_V128(0) ) );

   IRExpr* res
      = binop(Iop_InterleaveLO8x16,
              mkexpr(zeroVec),
              binop(Iop_InterleaveLO8x16, 
                    mkexpr(zeroVec), mkexpr(srcVec)));
   if (!xIsZ)
      res = binop(Iop_SarN32x4, 
                  binop(Iop_ShlN32x4, res, mkU8(24)), mkU8(24));

   (isAvx ? putYMMRegLoAndZU : putXMMReg) ( rG, res );

   return delta;
}


/* Handles 128 bit versions of PMOVSXBQ. */
static Long dis_PMOVSXBQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr     = IRTemp_INVALID;
   Int    alen     = 0;
   HChar  dis_buf[50];
   IRTemp srcBytes = newTemp(Ity_I16);
   UChar  modrm    = getUChar(delta);
   UChar* mbV      = isAvx ? "v" : "";
   UInt   rG       = gregOfRexRM(pfx, modrm);
   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcBytes, getXMMRegLane16( rE, 0 ) );
      delta += 1;
      DIP( "%spmovsxbq %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcBytes, loadLE( Ity_I16, mkexpr(addr) ) );
      delta += alen;
      DIP( "%spmovsxbq %s,%s\n", mbV, dis_buf, nameXMMReg(rG) );
   }

   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, binop( Iop_64HLtoV128,
                   unop( Iop_8Sto64,
                         unop( Iop_16HIto8, mkexpr(srcBytes) ) ),
                   unop( Iop_8Sto64,
                         unop( Iop_16to8, mkexpr(srcBytes) ) ) ) );
   return delta;
}


/* Handles 128 bit versions of PMOVZXBQ. */
static Long dis_PMOVZXBQ_128 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta, Bool isAvx )
{
   IRTemp addr     = IRTemp_INVALID;
   Int    alen     = 0;
   HChar  dis_buf[50];
   IRTemp srcVec   = newTemp(Ity_V128);
   UChar  modrm    = getUChar(delta);
   UChar* mbV      = isAvx ? "v" : "";
   UInt   rG       = gregOfRexRM(pfx, modrm);
   if ( epartIsReg(modrm) ) {
      UInt rE = eregOfRexRM(pfx, modrm);
      assign( srcVec, getXMMReg(rE) );
      delta += 1;
      DIP( "%spmovzxbq %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG) );
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( srcVec, 
              unop( Iop_32UtoV128, 
                    unop( Iop_16Uto32, loadLE( Ity_I16, mkexpr(addr) ))));
      delta += alen;
      DIP( "%spmovzxbq %s,%s\n", mbV, dis_buf, nameXMMReg(rG) );
   }

   IRTemp zeroVec = newTemp(Ity_V128);
   assign( zeroVec, IRExpr_Const( IRConst_V128(0) ) );

   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      ( rG, binop( Iop_InterleaveLO8x16, 
                   mkexpr(zeroVec), 
                   binop( Iop_InterleaveLO8x16, 
                          mkexpr(zeroVec), 
                          binop( Iop_InterleaveLO8x16, 
                                 mkexpr(zeroVec), mkexpr(srcVec) ) ) ) );
   return delta;
}


static Long dis_PHMINPOSUW_128 ( VexAbiInfo* vbi, Prefix pfx,
                                 Long delta, Bool isAvx )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UChar* mbV    = isAvx ? "v" : "";
   IRTemp sV     = newTemp(Ity_V128);
   IRTemp sHi    = newTemp(Ity_I64);
   IRTemp sLo    = newTemp(Ity_I64);
   IRTemp dLo    = newTemp(Ity_I64);
   UInt   rG     = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      delta += 1;
      DIP("%sphminposuw %s,%s\n", mbV, nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      gen_SEGV_if_not_16_aligned(addr);
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("%sphminposuw %s,%s\n", mbV, dis_buf, nameXMMReg(rG));
   }
   assign( sHi, unop(Iop_V128HIto64, mkexpr(sV)) );
   assign( sLo, unop(Iop_V128to64,   mkexpr(sV)) );
   assign( dLo, mkIRExprCCall(
                   Ity_I64, 0/*regparms*/,
                   "amd64g_calculate_sse_phminposuw", 
                   &amd64g_calculate_sse_phminposuw,
                   mkIRExprVec_2( mkexpr(sLo), mkexpr(sHi) )
         ));
   (isAvx ? putYMMRegLoAndZU : putXMMReg)
      (rG, unop(Iop_64UtoV128, mkexpr(dLo)));
   return delta;
}


static Long dis_AESx ( VexAbiInfo* vbi, Prefix pfx,
                       Long delta, Bool isAvx, UChar opc )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   rG     = gregOfRexRM(pfx, modrm);
   UInt   regNoL = 0;
   UInt   regNoR = (isAvx && opc != 0xDB) ? getVexNvvvv(pfx) : rG;

   /* This is a nasty kludge.  We need to pass 2 x V128 to the
      helper.  Since we can't do that, use a dirty
      helper to compute the results directly from the XMM regs in
      the guest state.  That means for the memory case, we need to
      move the left operand into a pseudo-register (XMM16, let's
      call it). */
   if (epartIsReg(modrm)) {
      regNoL = eregOfRexRM(pfx, modrm);
      delta += 1;
   } else {
      regNoL = 16; /* use XMM16 as an intermediary */
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      /* alignment check needed ???? */
      stmt( IRStmt_Put( OFFB_YMM16, loadLE(Ity_V128, mkexpr(addr)) ));
      delta += alen;
   }

   void*  fn = &amd64g_dirtyhelper_AES;
   HChar* nm = "amd64g_dirtyhelper_AES";

   /* Round up the arguments.  Note that this is a kludge -- the
      use of mkU64 rather than mkIRExpr_HWord implies the
      assumption that the host's word size is 64-bit. */
   UInt gstOffD = ymmGuestRegOffset(rG);
   UInt gstOffL = regNoL == 16 ? OFFB_YMM16 : ymmGuestRegOffset(regNoL);
   UInt gstOffR = ymmGuestRegOffset(regNoR);
   IRExpr*  opc4         = mkU64(opc);
   IRExpr*  gstOffDe     = mkU64(gstOffD);
   IRExpr*  gstOffLe     = mkU64(gstOffL);
   IRExpr*  gstOffRe     = mkU64(gstOffR);
   IRExpr** args
      = mkIRExprVec_4( opc4, gstOffDe, gstOffLe, gstOffRe );

   IRDirty* d    = unsafeIRDirty_0_N( 0/*regparms*/, nm, fn, args );
   /* It's not really a dirty call, but we can't use the clean
      helper mechanism here for the very lame reason that we can't
      pass 2 x V128s by value to a helper, nor get one back.  Hence
      this roundabout scheme. */
   d->needsBBP = True;
   d->nFxState = 2;
   vex_bzero(&d->fxState, sizeof(d->fxState));
   /* AES{ENC,ENCLAST,DEC,DECLAST} read both registers, and writes
      the second for !isAvx or the third for isAvx.
      AESIMC (0xDB) reads the first register, and writes the second. */
   d->fxState[0].fx     = Ifx_Read;
   d->fxState[0].offset = gstOffL;
   d->fxState[0].size   = sizeof(U128);
   d->fxState[1].offset = gstOffR;
   d->fxState[1].size   = sizeof(U128);
   if (opc == 0xDB)
      d->fxState[1].fx   = Ifx_Write;
   else if (!isAvx || rG == regNoR)
      d->fxState[1].fx   = Ifx_Modify;
   else {
      d->fxState[1].fx     = Ifx_Read;
      d->nFxState++;
      d->fxState[2].fx     = Ifx_Write;
      d->fxState[2].offset = gstOffD; 
      d->fxState[2].size   = sizeof(U128);
   }

   stmt( IRStmt_Dirty(d) );
   {
      HChar* opsuf;
      switch (opc) {
         case 0xDC: opsuf = "enc"; break;
         case 0XDD: opsuf = "enclast"; break;
         case 0xDE: opsuf = "dec"; break;
         case 0xDF: opsuf = "declast"; break;
         case 0xDB: opsuf = "imc"; break;
         default: vassert(0);
      }
      DIP("%saes%s %s,%s%s%s\n", isAvx ? "v" : "", opsuf, 
          (regNoL == 16 ? dis_buf : nameXMMReg(regNoL)),
          nameXMMReg(regNoR),
          (isAvx && opc != 0xDB) ? "," : "",
          (isAvx && opc != 0xDB) ? nameXMMReg(rG) : "");
   }
   if (isAvx)
      putYMMRegLane128( rG, 1, mkV128(0) );
   return delta;
}

static Long dis_AESKEYGENASSIST ( VexAbiInfo* vbi, Prefix pfx,
                                  Long delta, Bool isAvx )
{
   IRTemp addr   = IRTemp_INVALID;
   Int    alen   = 0;
   HChar  dis_buf[50];
   UChar  modrm  = getUChar(delta);
   UInt   regNoL = 0;
   UInt   regNoR = gregOfRexRM(pfx, modrm);
   UChar  imm    = 0;

   /* This is a nasty kludge.  See AESENC et al. instructions. */
   modrm = getUChar(delta);
   if (epartIsReg(modrm)) {
      regNoL = eregOfRexRM(pfx, modrm);
      imm = getUChar(delta+1);
      delta += 1+1;
   } else {
      regNoL = 16; /* use XMM16 as an intermediary */
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      /* alignment check ???? . */
      stmt( IRStmt_Put( OFFB_YMM16, loadLE(Ity_V128, mkexpr(addr)) ));
      imm = getUChar(delta+alen);
      delta += alen+1;
   }

   /* Who ya gonna call?  Presumably not Ghostbusters. */
   void*  fn = &amd64g_dirtyhelper_AESKEYGENASSIST;
   HChar* nm = "amd64g_dirtyhelper_AESKEYGENASSIST";

   /* Round up the arguments.  Note that this is a kludge -- the
      use of mkU64 rather than mkIRExpr_HWord implies the
      assumption that the host's word size is 64-bit. */
   UInt gstOffL = regNoL == 16 ? OFFB_YMM16 : ymmGuestRegOffset(regNoL);
   UInt gstOffR = ymmGuestRegOffset(regNoR);

   IRExpr*  imme          = mkU64(imm & 0xFF);
   IRExpr*  gstOffLe     = mkU64(gstOffL);
   IRExpr*  gstOffRe     = mkU64(gstOffR);
   IRExpr** args
      = mkIRExprVec_3( imme, gstOffLe, gstOffRe );

   IRDirty* d    = unsafeIRDirty_0_N( 0/*regparms*/, nm, fn, args );
   /* It's not really a dirty call, but we can't use the clean
      helper mechanism here for the very lame reason that we can't
      pass 2 x V128s by value to a helper, nor get one back.  Hence
      this roundabout scheme. */
   d->needsBBP = True;
   d->nFxState = 2;
   vex_bzero(&d->fxState, sizeof(d->fxState));
   d->fxState[0].fx     = Ifx_Read;
   d->fxState[0].offset = gstOffL;
   d->fxState[0].size   = sizeof(U128);
   d->fxState[1].fx     = Ifx_Write;
   d->fxState[1].offset = gstOffR;
   d->fxState[1].size   = sizeof(U128);
   stmt( IRStmt_Dirty(d) );

   DIP("%saeskeygenassist $%x,%s,%s\n", isAvx ? "v" : "", (UInt)imm,
       (regNoL == 16 ? dis_buf : nameXMMReg(regNoL)),
       nameXMMReg(regNoR));
   if (isAvx)
      putYMMRegLane128( regNoR, 1, mkV128(0) );
   return delta;
}


__attribute__((noinline))
static
Long dis_ESC_0F38__SSE4 ( Bool* decode_OK,
                          VexAbiInfo* vbi,
                          Prefix pfx, Int sz, Long deltaIN )
{
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x10:
   case 0x14:
   case 0x15:
      /* 66 0F 38 10 /r = PBLENDVB xmm1, xmm2/m128  (byte gran)
         66 0F 38 14 /r = BLENDVPS xmm1, xmm2/m128  (float gran)
         66 0F 38 15 /r = BLENDVPD xmm1, xmm2/m128  (double gran)
         Blend at various granularities, with XMM0 (implicit operand)
         providing the controlling mask.
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         modrm = getUChar(delta);

         HChar* nm    = NULL;
         UInt   gran  = 0;
         IROp   opSAR = Iop_INVALID;
         switch (opc) {
            case 0x10:
               nm = "pblendvb"; gran = 1; opSAR = Iop_SarN8x16;
               break;
            case 0x14:
               nm = "blendvps"; gran = 4; opSAR = Iop_SarN32x4;
               break;
            case 0x15:
               nm = "blendvpd"; gran = 8; opSAR = Iop_SarN64x2;
               break;
         }
         vassert(nm);

         IRTemp vecE = newTemp(Ity_V128);
         IRTemp vecG = newTemp(Ity_V128);
         IRTemp vec0 = newTemp(Ity_V128);

         if ( epartIsReg(modrm) ) {
            assign(vecE, getXMMReg(eregOfRexRM(pfx, modrm)));
            delta += 1;
            DIP( "%s %s,%s\n", nm,
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign(vecE, loadLE( Ity_V128, mkexpr(addr) ));
            delta += alen;
            DIP( "%s %s,%s\n", nm,
                 dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         assign(vecG, getXMMReg(gregOfRexRM(pfx, modrm)));
         assign(vec0, getXMMReg(0));

         IRTemp res = math_PBLENDVB_128( vecE, vecG, vec0, gran, opSAR );
         putXMMReg(gregOfRexRM(pfx, modrm), mkexpr(res));

         goto decode_success;
      }
      break;

   case 0x17:
      /* 66 0F 38 17 /r = PTEST xmm1, xmm2/m128
         Logical compare (set ZF and CF from AND/ANDN of the operands) */
      if (have66noF2noF3(pfx)
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_xTESTy_128( vbi, pfx, delta, False/*!isAvx*/, 0 );
         goto decode_success;
      }
      break;

   case 0x20:
      /* 66 0F 38 20 /r = PMOVSXBW xmm1, xmm2/m64 
         Packed Move with Sign Extend from Byte to Word (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXBW_128( vbi, pfx, delta,
                                   False/*!isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x21:
      /* 66 0F 38 21 /r = PMOVSXBD xmm1, xmm2/m32 
         Packed Move with Sign Extend from Byte to DWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXBD_128( vbi, pfx, delta,
                                   False/*!isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x22:
      /* 66 0F 38 22 /r = PMOVSXBQ xmm1, xmm2/m16
         Packed Move with Sign Extend from Byte to QWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVSXBQ_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x23:
      /* 66 0F 38 23 /r = PMOVSXWD xmm1, xmm2/m64 
         Packed Move with Sign Extend from Word to DWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXWD_128(vbi, pfx, delta,
                                  False/*!isAvx*/, False/*!xIsZ*/);
         goto decode_success;
      }
      break;

   case 0x24:
      /* 66 0F 38 24 /r = PMOVSXWQ xmm1, xmm2/m32
         Packed Move with Sign Extend from Word to QWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVSXWQ_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x25:
      /* 66 0F 38 25 /r = PMOVSXDQ xmm1, xmm2/m64
         Packed Move with Sign Extend from Double Word to Quad Word (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXDQ_128( vbi, pfx, delta,
                                   False/*!isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x28:
      /* 66 0F 38 28 = PMULDQ -- signed widening multiply of 32-lanes
         0 x 0 to form lower 64-bit half and lanes 2 x 2 to form upper
         64-bit half */
      /* This is a really poor translation -- could be improved if
         performance critical.  It's a copy-paste of PMULUDQ, too. */
      if (have66noF2noF3(pfx) && sz == 2) {
         IRTemp sV = newTemp(Ity_V128);
         IRTemp dV = newTemp(Ity_V128);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx,modrm);
         assign( dV, getXMMReg(rG) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("pmuldq %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("pmuldq %s,%s\n", dis_buf, nameXMMReg(rG));
         }

         putXMMReg( rG, mkexpr(math_PMULDQ_128( dV, sV )) );
         goto decode_success;
      }
      break;

   case 0x29:
      /* 66 0F 38 29 = PCMPEQQ
         64x2 equality comparison */
      if (have66noF2noF3(pfx) && sz == 2) { 
         /* FIXME: this needs an alignment check */
         delta = dis_SSEint_E_to_G( vbi, pfx, delta, 
                                    "pcmpeqq", Iop_CmpEQ64x2, False );
         goto decode_success;
      }
      break;

   case 0x2B:
      /* 66 0f 38 2B /r = PACKUSDW xmm1, xmm2/m128
         2x 32x4 S->U saturating narrow from xmm2/m128 to xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {
  
         modrm = getUChar(delta);

         IRTemp argL = newTemp(Ity_V128);
         IRTemp argR = newTemp(Ity_V128);

         if ( epartIsReg(modrm) ) {
            assign( argL, getXMMReg( eregOfRexRM(pfx, modrm) ) );
            delta += 1;
            DIP( "packusdw %s,%s\n",
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( argL, loadLE( Ity_V128, mkexpr(addr) ));
            delta += alen;
            DIP( "packusdw %s,%s\n",
                 dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         assign(argR, getXMMReg( gregOfRexRM(pfx, modrm) ));

         putXMMReg( gregOfRexRM(pfx, modrm), 
                    binop( Iop_QNarrowBin32Sto16Ux8,
                           mkexpr(argL), mkexpr(argR)) );

         goto decode_success;
      }
      break;

   case 0x30:
      /* 66 0F 38 30 /r = PMOVZXBW xmm1, xmm2/m64 
         Packed Move with Zero Extend from Byte to Word (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXBW_128( vbi, pfx, delta,
                                   False/*!isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x31:
      /* 66 0F 38 31 /r = PMOVZXBD xmm1, xmm2/m32 
         Packed Move with Zero Extend from Byte to DWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXBD_128( vbi, pfx, delta,
                                   False/*!isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x32:
      /* 66 0F 38 32 /r = PMOVZXBQ xmm1, xmm2/m16
         Packed Move with Zero Extend from Byte to QWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVZXBQ_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x33:
      /* 66 0F 38 33 /r = PMOVZXWD xmm1, xmm2/m64 
         Packed Move with Zero Extend from Word to DWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXWD_128( vbi, pfx, delta,
                                   False/*!isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x34:
      /* 66 0F 38 34 /r = PMOVZXWQ xmm1, xmm2/m32
         Packed Move with Zero Extend from Word to QWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVZXWQ_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x35:
      /* 66 0F 38 35 /r = PMOVZXDQ xmm1, xmm2/m64
         Packed Move with Zero Extend from DWord to QWord (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PMOVxXDQ_128( vbi, pfx, delta,
                                   False/*!isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x37:
      /* 66 0F 38 37 = PCMPGTQ
         64x2 comparison (signed, presumably; the Intel docs don't say :-)
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         /* FIXME: this needs an alignment check */
         delta = dis_SSEint_E_to_G( vbi, pfx, delta,
                                    "pcmpgtq", Iop_CmpGT64Sx2, False );
         goto decode_success;
      }
      break;

   case 0x38:
   case 0x3C:
      /* 66 0F 38 38 /r = PMINSB xmm1, xmm2/m128    8Sx16 (signed) min
         66 0F 38 3C /r = PMAXSB xmm1, xmm2/m128    8Sx16 (signed) max
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         /* FIXME: this needs an alignment check */
         Bool isMAX = opc == 0x3C;
         delta = dis_SSEint_E_to_G(
                    vbi, pfx, delta, 
                    isMAX ? "pmaxsb" : "pminsb",
                    isMAX ? Iop_Max8Sx16 : Iop_Min8Sx16,
                    False
                 );
         goto decode_success;
      }
      break;

   case 0x39:
   case 0x3D:
      /* 66 0F 38 39 /r = PMINSD xmm1, xmm2/m128
         Minimum of Packed Signed Double Word Integers (XMM)
         66 0F 38 3D /r = PMAXSD xmm1, xmm2/m128
         Maximum of Packed Signed Double Word Integers (XMM) 
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         /* FIXME: this needs an alignment check */
         Bool isMAX = opc == 0x3D;
         delta = dis_SSEint_E_to_G(
                    vbi, pfx, delta, 
                    isMAX ? "pmaxsd" : "pminsd",
                    isMAX ? Iop_Max32Sx4 : Iop_Min32Sx4,
                    False
                 );
         goto decode_success;
      }
      break;

   case 0x3A:
   case 0x3E:
      /* 66 0F 38 3A /r = PMINUW xmm1, xmm2/m128
         Minimum of Packed Unsigned Word Integers (XMM)
         66 0F 38 3E /r = PMAXUW xmm1, xmm2/m128
         Maximum of Packed Unsigned Word Integers (XMM)
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         /* FIXME: this needs an alignment check */
         Bool isMAX = opc == 0x3E;
         delta = dis_SSEint_E_to_G(
                    vbi, pfx, delta, 
                    isMAX ? "pmaxuw" : "pminuw",
                    isMAX ? Iop_Max16Ux8 : Iop_Min16Ux8,
                    False
                 );
         goto decode_success;
      }
      break;

   case 0x3B:
   case 0x3F:
      /* 66 0F 38 3B /r = PMINUD xmm1, xmm2/m128
         Minimum of Packed Unsigned Doubleword Integers (XMM)
         66 0F 38 3F /r = PMAXUD xmm1, xmm2/m128
         Maximum of Packed Unsigned Doubleword Integers (XMM)
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         /* FIXME: this needs an alignment check */
         Bool isMAX = opc == 0x3F;
         delta = dis_SSEint_E_to_G(
                    vbi, pfx, delta, 
                    isMAX ? "pmaxud" : "pminud",
                    isMAX ? Iop_Max32Ux4 : Iop_Min32Ux4,
                    False
                 );
         goto decode_success;
      }
      break;

   case 0x40:
      /* 66 0F 38 40 /r = PMULLD xmm1, xmm2/m128
         32x4 integer multiply from xmm2/m128 to xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {
  
         modrm = getUChar(delta);

         IRTemp argL = newTemp(Ity_V128);
         IRTemp argR = newTemp(Ity_V128);

         if ( epartIsReg(modrm) ) {
            assign( argL, getXMMReg( eregOfRexRM(pfx, modrm) ) );
            delta += 1;
            DIP( "pmulld %s,%s\n",
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( argL, loadLE( Ity_V128, mkexpr(addr) ));
            delta += alen;
            DIP( "pmulld %s,%s\n",
                 dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         assign(argR, getXMMReg( gregOfRexRM(pfx, modrm) ));

         putXMMReg( gregOfRexRM(pfx, modrm), 
                    binop( Iop_Mul32x4, mkexpr(argL), mkexpr(argR)) );

         goto decode_success;
      }
      break;

   case 0x41:
      /* 66 0F 38 41 /r = PHMINPOSUW xmm1, xmm2/m128
         Packed Horizontal Word Minimum from xmm2/m128 to xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PHMINPOSUW_128( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      } 
      break;

   case 0xDC:
   case 0xDD:
   case 0xDE:
   case 0xDF:
   case 0xDB:
      /* 66 0F 38 DC /r = AESENC xmm1, xmm2/m128
                  DD /r = AESENCLAST xmm1, xmm2/m128
                  DE /r = AESDEC xmm1, xmm2/m128
                  DF /r = AESDECLAST xmm1, xmm2/m128

                  DB /r = AESIMC xmm1, xmm2/m128 */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_AESx( vbi, pfx, delta, False/*!isAvx*/, opc );
         goto decode_success;
      }
      break;

   case 0xF0:
   case 0xF1:
      /* F2 0F 38 F0 /r = CRC32 r/m8, r32 (REX.W ok, 66 not ok)
         F2 0F 38 F1 /r = CRC32 r/m{16,32,64}, r32
         The decoding on this is a bit unusual.
      */
      if (haveF2noF3(pfx)
          && (opc == 0xF1 || (opc == 0xF0 && !have66(pfx)))) {
         modrm = getUChar(delta);

         if (opc == 0xF0) 
            sz = 1;
         else
            vassert(sz == 2 || sz == 4 || sz == 8);

         IRType tyE = szToITy(sz);
         IRTemp valE = newTemp(tyE);

         if (epartIsReg(modrm)) {
            assign(valE, getIRegE(sz, pfx, modrm));
            delta += 1;
            DIP("crc32b %s,%s\n", nameIRegE(sz, pfx, modrm),
                nameIRegG(1==getRexW(pfx) ? 8 : 4, pfx, modrm));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(valE, loadLE(tyE, mkexpr(addr)));
            delta += alen;
            DIP("crc32b %s,%s\n", dis_buf,
                nameIRegG(1==getRexW(pfx) ? 8 : 4, pfx, modrm));
         }

         /* Somewhat funny getting/putting of the crc32 value, in order
            to ensure that it turns into 64-bit gets and puts.  However,
            mask off the upper 32 bits so as to not get memcheck false
            +ves around the helper call. */
         IRTemp valG0 = newTemp(Ity_I64);
         assign(valG0, binop(Iop_And64, getIRegG(8, pfx, modrm),
                             mkU64(0xFFFFFFFF)));

         HChar* nm = NULL;
         void*  fn = NULL;
         switch (sz) {
            case 1: nm = "amd64g_calc_crc32b";
                    fn = &amd64g_calc_crc32b; break;
            case 2: nm = "amd64g_calc_crc32w";
                    fn = &amd64g_calc_crc32w; break;
            case 4: nm = "amd64g_calc_crc32l";
                    fn = &amd64g_calc_crc32l; break;
            case 8: nm = "amd64g_calc_crc32q";
                    fn = &amd64g_calc_crc32q; break;
         }
         vassert(nm && fn);
         IRTemp valG1 = newTemp(Ity_I64);
         assign(valG1,
                mkIRExprCCall(Ity_I64, 0/*regparm*/, nm, fn, 
                              mkIRExprVec_2(mkexpr(valG0),
                                            widenUto64(mkexpr(valE)))));

         putIRegG(4, pfx, modrm, unop(Iop_64to32, mkexpr(valG1)));
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level SSE4: dis_ESC_0F3A__SSE4                   ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static Long dis_PEXTRW ( VexAbiInfo* vbi, Prefix pfx,
                         Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   IRTemp t0    = IRTemp_INVALID;
   IRTemp t1    = IRTemp_INVALID;
   IRTemp t2    = IRTemp_INVALID;
   IRTemp t3    = IRTemp_INVALID;
   UChar  modrm = getUChar(delta);
   Int    alen  = 0;
   HChar  dis_buf[50];
   UInt   rG    = gregOfRexRM(pfx,modrm);
   Int    imm8_20;
   IRTemp xmm_vec = newTemp(Ity_V128);
   IRTemp d16   = newTemp(Ity_I16);
   HChar* mbV   = isAvx ? "v" : "";

   vassert(0==getRexW(pfx)); /* ensured by caller */
   assign( xmm_vec, getXMMReg(rG) );
   breakupV128to32s( xmm_vec, &t3, &t2, &t1, &t0 );

   if ( epartIsReg( modrm ) ) {
      imm8_20 = (Int)(getUChar(delta+1) & 7);
   } else { 
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8_20 = (Int)(getUChar(delta+alen) & 7);
   }

   switch (imm8_20) {
      case 0:  assign(d16, unop(Iop_32to16,   mkexpr(t0))); break;
      case 1:  assign(d16, unop(Iop_32HIto16, mkexpr(t0))); break;
      case 2:  assign(d16, unop(Iop_32to16,   mkexpr(t1))); break;
      case 3:  assign(d16, unop(Iop_32HIto16, mkexpr(t1))); break;
      case 4:  assign(d16, unop(Iop_32to16,   mkexpr(t2))); break;
      case 5:  assign(d16, unop(Iop_32HIto16, mkexpr(t2))); break;
      case 6:  assign(d16, unop(Iop_32to16,   mkexpr(t3))); break;
      case 7:  assign(d16, unop(Iop_32HIto16, mkexpr(t3))); break;
      default: vassert(0);
   }

   if ( epartIsReg( modrm ) ) {
      UInt rE = eregOfRexRM(pfx,modrm);
      putIReg32( rE, unop(Iop_16Uto32, mkexpr(d16)) );
      delta += 1+1;
      DIP( "%spextrw $%d, %s,%s\n", mbV, imm8_20,
           nameXMMReg( rG ), nameIReg32( rE ) );
   } else {
      storeLE( mkexpr(addr), mkexpr(d16) );
      delta += alen+1;
      DIP( "%spextrw $%d, %s,%s\n", mbV, imm8_20, nameXMMReg( rG ), dis_buf );
   }
   return delta;
}


static Long dis_PEXTRD ( VexAbiInfo* vbi, Prefix pfx,
                         Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   IRTemp t0    = IRTemp_INVALID;
   IRTemp t1    = IRTemp_INVALID;
   IRTemp t2    = IRTemp_INVALID;
   IRTemp t3    = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   Int    imm8_10;
   IRTemp xmm_vec   = newTemp(Ity_V128);
   IRTemp src_dword = newTemp(Ity_I32);
   HChar* mbV = isAvx ? "v" : "";

   vassert(0==getRexW(pfx)); /* ensured by caller */
   modrm = getUChar(delta);
   assign( xmm_vec, getXMMReg( gregOfRexRM(pfx,modrm) ) );
   breakupV128to32s( xmm_vec, &t3, &t2, &t1, &t0 );

   if ( epartIsReg( modrm ) ) {
      imm8_10 = (Int)(getUChar(delta+1) & 3);
   } else { 
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8_10 = (Int)(getUChar(delta+alen) & 3);
   }

   switch ( imm8_10 ) {
      case 0:  assign( src_dword, mkexpr(t0) ); break;
      case 1:  assign( src_dword, mkexpr(t1) ); break;
      case 2:  assign( src_dword, mkexpr(t2) ); break;
      case 3:  assign( src_dword, mkexpr(t3) ); break;
      default: vassert(0);
   }

   if ( epartIsReg( modrm ) ) {
      putIReg32( eregOfRexRM(pfx,modrm), mkexpr(src_dword) );
      delta += 1+1;
      DIP( "%spextrd $%d, %s,%s\n", mbV, imm8_10,
           nameXMMReg( gregOfRexRM(pfx, modrm) ),
           nameIReg32( eregOfRexRM(pfx, modrm) ) );
   } else {
      storeLE( mkexpr(addr), mkexpr(src_dword) );
      delta += alen+1;
      DIP( "%spextrd $%d, %s,%s\n", mbV,
           imm8_10, nameXMMReg( gregOfRexRM(pfx, modrm) ), dis_buf );
   }
   return delta;
}


static Long dis_PEXTRQ ( VexAbiInfo* vbi, Prefix pfx,
                         Long delta, Bool isAvx )
{
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   Int imm8_0;
   IRTemp xmm_vec   = newTemp(Ity_V128);
   IRTemp src_qword = newTemp(Ity_I64);
   HChar* mbV = isAvx ? "v" : "";

   vassert(1==getRexW(pfx)); /* ensured by caller */
   modrm = getUChar(delta);
   assign( xmm_vec, getXMMReg( gregOfRexRM(pfx,modrm) ) );

   if ( epartIsReg( modrm ) ) {
      imm8_0 = (Int)(getUChar(delta+1) & 1);
   } else {
      addr   = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8_0 = (Int)(getUChar(delta+alen) & 1);
   }

   switch ( imm8_0 ) {
      case 0:  assign( src_qword, unop(Iop_V128to64,   mkexpr(xmm_vec)) );
               break;
      case 1:  assign( src_qword, unop(Iop_V128HIto64, mkexpr(xmm_vec)) );
               break;
      default: vassert(0);
   }

   if ( epartIsReg( modrm ) ) {
      putIReg64( eregOfRexRM(pfx,modrm), mkexpr(src_qword) );
      delta += 1+1;
      DIP( "%spextrq $%d, %s,%s\n", mbV, imm8_0,
           nameXMMReg( gregOfRexRM(pfx, modrm) ),
           nameIReg64( eregOfRexRM(pfx, modrm) ) );
   } else {
      storeLE( mkexpr(addr), mkexpr(src_qword) );
      delta += alen+1;
      DIP( "%spextrq $%d, %s,%s\n", mbV,
           imm8_0, nameXMMReg( gregOfRexRM(pfx, modrm) ), dis_buf );
   }
   return delta;
}


/* This can fail, in which case it returns the original (unchanged)
   delta. */
static Long dis_PCMPxSTRx ( VexAbiInfo* vbi, Prefix pfx,
                            Long delta, Bool isAvx, UChar opc )
{
   Long   delta0  = delta;
   UInt   isISTRx = opc & 2;
   UInt   isxSTRM = (opc & 1) ^ 1;
   UInt   regNoL  = 0;
   UInt   regNoR  = 0;
   UChar  imm     = 0;
   IRTemp addr    = IRTemp_INVALID;
   Int    alen    = 0;
   HChar  dis_buf[50];

   /* This is a nasty kludge.  We need to pass 2 x V128 to the helper
      (which is clean).  Since we can't do that, use a dirty helper to
      compute the results directly from the XMM regs in the guest
      state.  That means for the memory case, we need to move the left
      operand into a pseudo-register (XMM16, let's call it). */
   UChar modrm = getUChar(delta);
   if (epartIsReg(modrm)) {
      regNoL = eregOfRexRM(pfx, modrm);
      regNoR = gregOfRexRM(pfx, modrm);
      imm = getUChar(delta+1);
      delta += 1+1;
   } else {
      regNoL = 16; /* use XMM16 as an intermediary */
      regNoR = gregOfRexRM(pfx, modrm);
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      /* No alignment check; I guess that makes sense, given that
         these insns are for dealing with C style strings. */
      stmt( IRStmt_Put( OFFB_YMM16, loadLE(Ity_V128, mkexpr(addr)) ));
      imm = getUChar(delta+alen);
      delta += alen+1;
   }

   /* Now we know the XMM reg numbers for the operands, and the
      immediate byte.  Is it one we can actually handle? Throw out any
      cases for which the helper function has not been verified. */
   switch (imm) {
      case 0x00:
      case 0x02: case 0x08: case 0x0A: case 0x0C: case 0x12:
      case 0x1A: case 0x38: case 0x3A: case 0x44: case 0x4A:
         break;
      case 0x01: // the 16-bit character versions of the above
      case 0x03: case 0x09: case 0x0B: case 0x0D: case 0x13:
      case 0x1B: case 0x39: case 0x3B: case 0x45: case 0x4B:
         break;
      default:
         return delta0; /*FAIL*/
   }

   /* Who ya gonna call?  Presumably not Ghostbusters. */
   void*  fn = &amd64g_dirtyhelper_PCMPxSTRx;
   HChar* nm = "amd64g_dirtyhelper_PCMPxSTRx";

   /* Round up the arguments.  Note that this is a kludge -- the use
      of mkU64 rather than mkIRExpr_HWord implies the assumption that
      the host's word size is 64-bit. */
   UInt gstOffL = regNoL == 16 ? OFFB_YMM16 : ymmGuestRegOffset(regNoL);
   UInt gstOffR = ymmGuestRegOffset(regNoR);

   IRExpr*  opc4_and_imm = mkU64((opc << 8) | (imm & 0xFF));
   IRExpr*  gstOffLe     = mkU64(gstOffL);
   IRExpr*  gstOffRe     = mkU64(gstOffR);
   IRExpr*  edxIN        = isISTRx ? mkU64(0) : getIRegRDX(8);
   IRExpr*  eaxIN        = isISTRx ? mkU64(0) : getIRegRAX(8);
   IRExpr** args
      = mkIRExprVec_5( opc4_and_imm, gstOffLe, gstOffRe, edxIN, eaxIN );

   IRTemp   resT = newTemp(Ity_I64);
   IRDirty* d    = unsafeIRDirty_1_N( resT, 0/*regparms*/, nm, fn, args );
   /* It's not really a dirty call, but we can't use the clean helper
      mechanism here for the very lame reason that we can't pass 2 x
      V128s by value to a helper, nor get one back.  Hence this
      roundabout scheme. */
   d->needsBBP = True;
   d->nFxState = 2;
   vex_bzero(&d->fxState, sizeof(d->fxState));
   d->fxState[0].fx     = Ifx_Read;
   d->fxState[0].offset = gstOffL;
   d->fxState[0].size   = sizeof(U128);
   d->fxState[1].fx     = Ifx_Read;
   d->fxState[1].offset = gstOffR;
   d->fxState[1].size   = sizeof(U128);
   if (isxSTRM) {
      /* Declare that the helper writes XMM0. */
      d->nFxState = 3;
      d->fxState[2].fx     = Ifx_Write;
      d->fxState[2].offset = ymmGuestRegOffset(0);
      d->fxState[2].size   = sizeof(U128);
   }

   stmt( IRStmt_Dirty(d) );

   /* Now resT[15:0] holds the new OSZACP values, so the condition
      codes must be updated. And for a xSTRI case, resT[31:16] holds
      the new ECX value, so stash that too. */
   if (!isxSTRM) {
      putIReg64(R_RCX, binop(Iop_And64,
                             binop(Iop_Shr64, mkexpr(resT), mkU8(16)),
                             mkU64(0xFFFF)));
   }

   /* Zap the upper half of the dest reg as per AVX conventions. */
   if (isxSTRM && isAvx)
      putYMMRegLane128(/*YMM*/0, 1, mkV128(0));

   stmt( IRStmt_Put(
            OFFB_CC_DEP1,
            binop(Iop_And64, mkexpr(resT), mkU64(0xFFFF))
   ));
   stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
   stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
   stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));

   if (regNoL == 16) {
      DIP("%spcmp%cstr%c $%x,%s,%s\n",
          isAvx ? "v" : "", isISTRx ? 'i' : 'e', isxSTRM ? 'm' : 'i',
          (UInt)imm, dis_buf, nameXMMReg(regNoR));
   } else {
      DIP("%spcmp%cstr%c $%x,%s,%s\n",
          isAvx ? "v" : "", isISTRx ? 'i' : 'e', isxSTRM ? 'm' : 'i',
          (UInt)imm, nameXMMReg(regNoL), nameXMMReg(regNoR));
   }

   return delta;
}


static IRTemp math_PINSRB_128 ( IRTemp v128, IRTemp u8, UInt imm8 )
{
   vassert(imm8 >= 0 && imm8 <= 15);

   // Create a V128 value which has the selected byte in the
   // specified lane, and zeroes everywhere else.
   IRTemp tmp128    = newTemp(Ity_V128);
   IRTemp halfshift = newTemp(Ity_I64);
   assign(halfshift, binop(Iop_Shl64,
                           unop(Iop_8Uto64, mkexpr(u8)),
                           mkU8(8 * (imm8 & 7))));
   if (imm8 < 8) {
      assign(tmp128, binop(Iop_64HLtoV128, mkU64(0), mkexpr(halfshift)));
   } else {
      assign(tmp128, binop(Iop_64HLtoV128, mkexpr(halfshift), mkU64(0)));
   }

   UShort mask = ~(1 << imm8);
   IRTemp res  = newTemp(Ity_V128);
   assign( res, binop(Iop_OrV128,
                      mkexpr(tmp128),
                      binop(Iop_AndV128, mkexpr(v128), mkV128(mask))) );
   return res;
}


static IRTemp math_PINSRD_128 ( IRTemp v128, IRTemp u32, UInt imm8 )
{
   IRTemp z32 = newTemp(Ity_I32);
   assign(z32, mkU32(0));

   /* Surround u32 with zeroes as per imm, giving us something we can
      OR into a suitably masked-out v128.*/
   IRTemp withZs = newTemp(Ity_V128);
   UShort mask = 0;
   switch (imm8) {
      case 3:  mask = 0x0FFF;
               assign(withZs, mkV128from32s(u32, z32, z32, z32));
               break;
      case 2:  mask = 0xF0FF;
               assign(withZs, mkV128from32s(z32, u32, z32, z32));
               break;
      case 1:  mask = 0xFF0F;
               assign(withZs, mkV128from32s(z32, z32, u32, z32));
               break;
      case 0:  mask = 0xFFF0;
               assign(withZs, mkV128from32s(z32, z32, z32, u32));
               break;
      default: vassert(0);
   }

   IRTemp res = newTemp(Ity_V128);
   assign(res, binop( Iop_OrV128,
                      mkexpr(withZs),
                      binop( Iop_AndV128, mkexpr(v128), mkV128(mask) ) ) );
   return res;
}


static IRTemp math_PINSRQ_128 ( IRTemp v128, IRTemp u64, UInt imm8 )
{
   /* Surround u64 with zeroes as per imm, giving us something we can
      OR into a suitably masked-out v128.*/
   IRTemp withZs = newTemp(Ity_V128);
   UShort mask = 0;
   if (imm8 == 0) { 
      mask = 0xFF00; 
      assign(withZs, binop(Iop_64HLtoV128, mkU64(0), mkexpr(u64)));
   } else {
      vassert(imm8 == 1);
      mask = 0x00FF;
      assign( withZs, binop(Iop_64HLtoV128, mkexpr(u64), mkU64(0)));
   }

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop( Iop_OrV128,
                       mkexpr(withZs),
                       binop( Iop_AndV128, mkexpr(v128), mkV128(mask) ) ) );
   return res;
}


static IRTemp math_INSERTPS ( IRTemp dstV, IRTemp toInsertD, UInt imm8 )
{
   const IRTemp inval = IRTemp_INVALID;
   IRTemp dstDs[4] = { inval, inval, inval, inval };
   breakupV128to32s( dstV, &dstDs[3], &dstDs[2], &dstDs[1], &dstDs[0] );

   vassert(imm8 <= 255);
   dstDs[(imm8 >> 4) & 3] = toInsertD; /* "imm8_count_d" */

   UInt imm8_zmask = (imm8 & 15);
   IRTemp zero_32 = newTemp(Ity_I32);
   assign( zero_32, mkU32(0) );
   IRTemp resV = newTemp(Ity_V128);
   assign( resV, mkV128from32s( 
                    ((imm8_zmask & 8) == 8) ? zero_32 : dstDs[3], 
                    ((imm8_zmask & 4) == 4) ? zero_32 : dstDs[2], 
                    ((imm8_zmask & 2) == 2) ? zero_32 : dstDs[1], 
                    ((imm8_zmask & 1) == 1) ? zero_32 : dstDs[0]) );
   return resV;
}


static Long dis_PEXTRB_128_GtoE ( VexAbiInfo* vbi, Prefix pfx,
                                  Long delta, Bool isAvx )
{
   IRTemp addr     = IRTemp_INVALID;
   Int    alen     = 0;
   HChar  dis_buf[50];
   IRTemp xmm_vec  = newTemp(Ity_V128);
   IRTemp sel_lane = newTemp(Ity_I32);
   IRTemp shr_lane = newTemp(Ity_I32);
   UChar* mbV      = isAvx ? "v" : "";
   UChar  modrm    = getUChar(delta);
   IRTemp t3, t2, t1, t0;
   Int    imm8;
   assign( xmm_vec, getXMMReg( gregOfRexRM(pfx,modrm) ) );
   t3 = t2 = t1 = t0 = IRTemp_INVALID;
   breakupV128to32s( xmm_vec, &t3, &t2, &t1, &t0 );

   if ( epartIsReg( modrm ) ) {
      imm8 = (Int)getUChar(delta+1);
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8 = (Int)getUChar(delta+alen);
   }
   switch ( (imm8 >> 2) & 3 ) {
      case 0:  assign( sel_lane, mkexpr(t0) ); break;
      case 1:  assign( sel_lane, mkexpr(t1) ); break;
      case 2:  assign( sel_lane, mkexpr(t2) ); break;
      case 3:  assign( sel_lane, mkexpr(t3) ); break;
      default: vassert(0);
   }
   assign( shr_lane, 
           binop( Iop_Shr32, mkexpr(sel_lane), mkU8(((imm8 & 3)*8)) ) );

   if ( epartIsReg( modrm ) ) {
      putIReg64( eregOfRexRM(pfx,modrm), 
                 unop( Iop_32Uto64, 
                       binop(Iop_And32, mkexpr(shr_lane), mkU32(255)) ) );
      delta += 1+1;
      DIP( "%spextrb $%d, %s,%s\n", mbV, imm8, 
           nameXMMReg( gregOfRexRM(pfx, modrm) ), 
           nameIReg64( eregOfRexRM(pfx, modrm) ) );
   } else {
      storeLE( mkexpr(addr), unop(Iop_32to8, mkexpr(shr_lane) ) );
      delta += alen+1;
      DIP( "%spextrb $%d,%s,%s\n", mbV,
           imm8, nameXMMReg( gregOfRexRM(pfx, modrm) ), dis_buf );
   }
   
   return delta;
}


static IRTemp math_DPPD_128 ( IRTemp src_vec, IRTemp dst_vec, UInt imm8 )
{
   vassert(imm8 < 256);
   UShort imm8_perms[4] = { 0x0000, 0x00FF, 0xFF00, 0xFFFF };
   IRTemp and_vec = newTemp(Ity_V128);
   IRTemp sum_vec = newTemp(Ity_V128);
   assign( and_vec, binop( Iop_AndV128,
                           binop( Iop_Mul64Fx2,
                                  mkexpr(dst_vec), mkexpr(src_vec) ),
                           mkV128( imm8_perms[ ((imm8 >> 4) & 3) ] ) ) );

   assign( sum_vec, binop( Iop_Add64F0x2,
                           binop( Iop_InterleaveHI64x2,
                                  mkexpr(and_vec), mkexpr(and_vec) ),
                           binop( Iop_InterleaveLO64x2,
                                  mkexpr(and_vec), mkexpr(and_vec) ) ) );
   IRTemp res = newTemp(Ity_V128);
   assign(res, binop( Iop_AndV128,
                      binop( Iop_InterleaveLO64x2,
                             mkexpr(sum_vec), mkexpr(sum_vec) ),
                      mkV128( imm8_perms[ (imm8 & 3) ] ) ) );
   return res;
}


static IRTemp math_DPPS_128 ( IRTemp src_vec, IRTemp dst_vec, UInt imm8 )
{
   vassert(imm8 < 256);
   IRTemp tmp_prod_vec = newTemp(Ity_V128);
   IRTemp prod_vec     = newTemp(Ity_V128);
   IRTemp sum_vec      = newTemp(Ity_V128);
   IRTemp v3, v2, v1, v0;
   v3 = v2 = v1 = v0   = IRTemp_INVALID;
   UShort imm8_perms[16] = { 0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00, 
                             0x0F0F, 0x0FF0, 0x0FFF, 0xF000, 0xF00F,
                             0xF0F0, 0xF0FF, 0xFF00, 0xFF0F, 0xFFF0,
                             0xFFFF };

   assign( tmp_prod_vec, 
           binop( Iop_AndV128, 
                  binop( Iop_Mul32Fx4, mkexpr(dst_vec),
                                       mkexpr(src_vec) ), 
                  mkV128( imm8_perms[((imm8 >> 4)& 15)] ) ) );
   breakupV128to32s( tmp_prod_vec, &v3, &v2, &v1, &v0 );
   assign( prod_vec, mkV128from32s( v3, v1, v2, v0 ) );

   assign( sum_vec, binop( Iop_Add32Fx4,
                           binop( Iop_InterleaveHI32x4, 
                                  mkexpr(prod_vec), mkexpr(prod_vec) ), 
                           binop( Iop_InterleaveLO32x4, 
                                  mkexpr(prod_vec), mkexpr(prod_vec) ) ) );

   IRTemp res = newTemp(Ity_V128);
   assign( res, binop( Iop_AndV128, 
                       binop( Iop_Add32Fx4,
                              binop( Iop_InterleaveHI32x4,
                                     mkexpr(sum_vec), mkexpr(sum_vec) ), 
                              binop( Iop_InterleaveLO32x4,
                                     mkexpr(sum_vec), mkexpr(sum_vec) ) ), 
                       mkV128( imm8_perms[ (imm8 & 15) ] ) ) );
   return res;
}


static IRTemp math_MPSADBW_128 ( IRTemp dst_vec, IRTemp src_vec, UInt imm8 )
{
   /* Mask out bits of the operands we don't need.  This isn't
      strictly necessary, but it does ensure Memcheck doesn't
      give us any false uninitialised value errors as a
      result. */
   UShort src_mask[4] = { 0x000F, 0x00F0, 0x0F00, 0xF000 };
   UShort dst_mask[2] = { 0x07FF, 0x7FF0 };

   IRTemp src_maskV = newTemp(Ity_V128);
   IRTemp dst_maskV = newTemp(Ity_V128);
   assign(src_maskV, mkV128( src_mask[ imm8 & 3 ] ));
   assign(dst_maskV, mkV128( dst_mask[ (imm8 >> 2) & 1 ] ));

   IRTemp src_masked = newTemp(Ity_V128);
   IRTemp dst_masked = newTemp(Ity_V128);
   assign(src_masked, binop(Iop_AndV128, mkexpr(src_vec), mkexpr(src_maskV)));
   assign(dst_masked, binop(Iop_AndV128, mkexpr(dst_vec), mkexpr(dst_maskV)));

   /* Generate 4 64 bit values that we can hand to a clean helper */
   IRTemp sHi = newTemp(Ity_I64);
   IRTemp sLo = newTemp(Ity_I64);
   assign( sHi, unop(Iop_V128HIto64, mkexpr(src_masked)) );
   assign( sLo, unop(Iop_V128to64,   mkexpr(src_masked)) );

   IRTemp dHi = newTemp(Ity_I64);
   IRTemp dLo = newTemp(Ity_I64);
   assign( dHi, unop(Iop_V128HIto64, mkexpr(dst_masked)) );
   assign( dLo, unop(Iop_V128to64,   mkexpr(dst_masked)) );

   /* Compute halves of the result separately */
   IRTemp resHi = newTemp(Ity_I64);
   IRTemp resLo = newTemp(Ity_I64);

   IRExpr** argsHi
      = mkIRExprVec_5( mkexpr(sHi), mkexpr(sLo), mkexpr(dHi), mkexpr(dLo),
                       mkU64( 0x80 | (imm8 & 7) ));
   IRExpr** argsLo
      = mkIRExprVec_5( mkexpr(sHi), mkexpr(sLo), mkexpr(dHi), mkexpr(dLo),
                       mkU64( 0x00 | (imm8 & 7) ));

   assign(resHi, mkIRExprCCall( Ity_I64, 0/*regparm*/,
                                "amd64g_calc_mpsadbw",
                                &amd64g_calc_mpsadbw, argsHi ));
   assign(resLo, mkIRExprCCall( Ity_I64, 0/*regparm*/,
                                "amd64g_calc_mpsadbw",
                                &amd64g_calc_mpsadbw, argsLo ));

   IRTemp res = newTemp(Ity_V128);
   assign(res, binop(Iop_64HLtoV128, mkexpr(resHi), mkexpr(resLo)));
   return res;
}

static Long dis_EXTRACTPS ( VexAbiInfo* vbi, Prefix pfx,
                            Long delta, Bool isAvx )
{
   IRTemp addr       = IRTemp_INVALID;
   Int    alen       = 0;
   HChar  dis_buf[50];
   UChar  modrm      = getUChar(delta);
   Int imm8_10;
   IRTemp xmm_vec    = newTemp(Ity_V128);
   IRTemp src_dword  = newTemp(Ity_I32);
   UInt   rG         = gregOfRexRM(pfx,modrm);
   IRTemp t3, t2, t1, t0;
   t3 = t2 = t1 = t0 = IRTemp_INVALID;

   assign( xmm_vec, getXMMReg( rG ) );
   breakupV128to32s( xmm_vec, &t3, &t2, &t1, &t0 );

   if ( epartIsReg( modrm ) ) {
      imm8_10 = (Int)(getUChar(delta+1) & 3);
   } else { 
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8_10 = (Int)(getUChar(delta+alen) & 3);
   }

   switch ( imm8_10 ) {
      case 0:  assign( src_dword, mkexpr(t0) ); break;
      case 1:  assign( src_dword, mkexpr(t1) ); break;
      case 2:  assign( src_dword, mkexpr(t2) ); break;
      case 3:  assign( src_dword, mkexpr(t3) ); break;
      default: vassert(0);
   }

   if ( epartIsReg( modrm ) ) {
      UInt rE = eregOfRexRM(pfx,modrm);
      putIReg32( rE, mkexpr(src_dword) );
      delta += 1+1;
      DIP( "%sextractps $%d, %s,%s\n", isAvx ? "v" : "", imm8_10,
           nameXMMReg( rG ), nameIReg32( rE ) );
   } else {
      storeLE( mkexpr(addr), mkexpr(src_dword) );
      delta += alen+1;
      DIP( "%sextractps $%d, %s,%s\n", isAvx ? "v" : "", imm8_10,
           nameXMMReg( rG ), dis_buf );
   }

   return delta;
}


static IRTemp math_PCLMULQDQ( IRTemp dV, IRTemp sV, UInt imm8 )
{
   IRTemp t0 = newTemp(Ity_I64);
   IRTemp t1 = newTemp(Ity_I64);
   assign(t0, unop((imm8&1)? Iop_V128HIto64 : Iop_V128to64, 
              mkexpr(dV)));
   assign(t1, unop((imm8&16) ? Iop_V128HIto64 : Iop_V128to64,
              mkexpr(sV)));

   IRTemp t2 = newTemp(Ity_I64);
   IRTemp t3 = newTemp(Ity_I64);

   IRExpr** args;

   args = mkIRExprVec_3(mkexpr(t0), mkexpr(t1), mkU64(0));
   assign(t2, mkIRExprCCall(Ity_I64,0, "amd64g_calculate_pclmul",
                            &amd64g_calculate_pclmul, args));
   args = mkIRExprVec_3(mkexpr(t0), mkexpr(t1), mkU64(1));
   assign(t3, mkIRExprCCall(Ity_I64,0, "amd64g_calculate_pclmul",
                            &amd64g_calculate_pclmul, args));

   IRTemp res     = newTemp(Ity_V128);
   assign(res, binop(Iop_64HLtoV128, mkexpr(t3), mkexpr(t2)));
   return res;
}


__attribute__((noinline))
static
Long dis_ESC_0F3A__SSE4 ( Bool* decode_OK,
                          VexAbiInfo* vbi,
                          Prefix pfx, Int sz, Long deltaIN )
{
   IRTemp addr  = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   *decode_OK = False;

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x08:
      /* 66 0F 3A 08 /r ib = ROUNDPS imm8, xmm2/m128, xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {

         IRTemp src0 = newTemp(Ity_F32);
         IRTemp src1 = newTemp(Ity_F32);
         IRTemp src2 = newTemp(Ity_F32);
         IRTemp src3 = newTemp(Ity_F32);
         IRTemp res0 = newTemp(Ity_F32);
         IRTemp res1 = newTemp(Ity_F32);
         IRTemp res2 = newTemp(Ity_F32);
         IRTemp res3 = newTemp(Ity_F32);
         IRTemp rm   = newTemp(Ity_I32);
         Int    imm  = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            assign( src0, 
                    getXMMRegLane32F( eregOfRexRM(pfx, modrm), 0 ) );
            assign( src1, 
                    getXMMRegLane32F( eregOfRexRM(pfx, modrm), 1 ) );
            assign( src2, 
                    getXMMRegLane32F( eregOfRexRM(pfx, modrm), 2 ) );
            assign( src3, 
                    getXMMRegLane32F( eregOfRexRM(pfx, modrm), 3 ) );
            imm = getUChar(delta+1);
            if (imm & ~15) goto decode_failure;
            delta += 1+1;
            DIP( "roundps $%d,%s,%s\n",
                 imm, nameXMMReg( eregOfRexRM(pfx, modrm) ),
                      nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            gen_SEGV_if_not_16_aligned(addr);
            assign( src0, loadLE(Ity_F32,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(0) )));
            assign( src1, loadLE(Ity_F32,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(4) )));
            assign( src2, loadLE(Ity_F32,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(8) )));
            assign( src3, loadLE(Ity_F32,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(12) )));
            imm = getUChar(delta+alen);
            if (imm & ~15) goto decode_failure;
            delta += alen+1;
            DIP( "roundps $%d,%s,%s\n",
                 imm, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         assign(res0, binop(Iop_RoundF32toInt, mkexpr(rm), mkexpr(src0)) );
         assign(res1, binop(Iop_RoundF32toInt, mkexpr(rm), mkexpr(src1)) );
         assign(res2, binop(Iop_RoundF32toInt, mkexpr(rm), mkexpr(src2)) );
         assign(res3, binop(Iop_RoundF32toInt, mkexpr(rm), mkexpr(src3)) );

         putXMMRegLane32F( gregOfRexRM(pfx, modrm), 0, mkexpr(res0) );
         putXMMRegLane32F( gregOfRexRM(pfx, modrm), 1, mkexpr(res1) );
         putXMMRegLane32F( gregOfRexRM(pfx, modrm), 2, mkexpr(res2) );
         putXMMRegLane32F( gregOfRexRM(pfx, modrm), 3, mkexpr(res3) );

         goto decode_success;
      }
      break;

   case 0x09:
      /* 66 0F 3A 09 /r ib = ROUNDPD imm8, xmm2/m128, xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {

         IRTemp src0 = newTemp(Ity_F64);
         IRTemp src1 = newTemp(Ity_F64);
         IRTemp res0 = newTemp(Ity_F64);
         IRTemp res1 = newTemp(Ity_F64);
         IRTemp rm   = newTemp(Ity_I32);
         Int    imm  = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            assign( src0, 
                    getXMMRegLane64F( eregOfRexRM(pfx, modrm), 0 ) );
            assign( src1, 
                    getXMMRegLane64F( eregOfRexRM(pfx, modrm), 1 ) );
            imm = getUChar(delta+1);
            if (imm & ~15) goto decode_failure;
            delta += 1+1;
            DIP( "roundpd $%d,%s,%s\n",
                 imm, nameXMMReg( eregOfRexRM(pfx, modrm) ),
                      nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            gen_SEGV_if_not_16_aligned(addr);
            assign( src0, loadLE(Ity_F64,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(0) )));
            assign( src1, loadLE(Ity_F64,
                                 binop(Iop_Add64, mkexpr(addr), mkU64(8) )));
            imm = getUChar(delta+alen);
            if (imm & ~15) goto decode_failure;
            delta += alen+1;
            DIP( "roundpd $%d,%s,%s\n",
                 imm, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         assign(res0, binop(Iop_RoundF64toInt, mkexpr(rm), mkexpr(src0)) );
         assign(res1, binop(Iop_RoundF64toInt, mkexpr(rm), mkexpr(src1)) );

         putXMMRegLane64F( gregOfRexRM(pfx, modrm), 0, mkexpr(res0) );
         putXMMRegLane64F( gregOfRexRM(pfx, modrm), 1, mkexpr(res1) );

         goto decode_success;
      }
      break;

   case 0x0A:
   case 0x0B:
      /* 66 0F 3A 0A /r ib = ROUNDSS imm8, xmm2/m32, xmm1
         66 0F 3A 0B /r ib = ROUNDSD imm8, xmm2/m64, xmm1
      */
      if (have66noF2noF3(pfx) && sz == 2) {

         Bool   isD = opc == 0x0B;
         IRTemp src = newTemp(isD ? Ity_F64 : Ity_F32);
         IRTemp res = newTemp(isD ? Ity_F64 : Ity_F32);
         Int    imm = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            assign( src, 
                    isD ? getXMMRegLane64F( eregOfRexRM(pfx, modrm), 0 )
                        : getXMMRegLane32F( eregOfRexRM(pfx, modrm), 0 ) );
            imm = getUChar(delta+1);
            if (imm & ~15) goto decode_failure;
            delta += 1+1;
            DIP( "rounds%c $%d,%s,%s\n",
                 isD ? 'd' : 's',
                 imm, nameXMMReg( eregOfRexRM(pfx, modrm) ),
                      nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE( isD ? Ity_F64 : Ity_F32, mkexpr(addr) ));
            imm = getUChar(delta+alen);
            if (imm & ~15) goto decode_failure;
            delta += alen+1;
            DIP( "rounds%c $%d,%s,%s\n",
                 isD ? 'd' : 's',
                 imm, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(res, binop(isD ? Iop_RoundF64toInt : Iop_RoundF32toInt,
                           (imm & 4) ? get_sse_roundingmode() 
                                     : mkU32(imm & 3),
                           mkexpr(src)) );

         if (isD)
            putXMMRegLane64F( gregOfRexRM(pfx, modrm), 0, mkexpr(res) );
         else
            putXMMRegLane32F( gregOfRexRM(pfx, modrm), 0, mkexpr(res) );

         goto decode_success;
      }
      break;

   case 0x0C:
      /* 66 0F 3A 0C /r ib = BLENDPS xmm1, xmm2/m128, imm8
         Blend Packed Single Precision Floating-Point Values (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {

         Int imm8;
         IRTemp dst_vec = newTemp(Ity_V128);
         IRTemp src_vec = newTemp(Ity_V128);

         modrm = getUChar(delta);

         assign( dst_vec, getXMMReg( gregOfRexRM(pfx, modrm) ) );

         if ( epartIsReg( modrm ) ) {
            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg( eregOfRexRM(pfx, modrm) ) );
            delta += 1+1;
            DIP( "blendps $%d, %s,%s\n", imm8,
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "blendpd $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         putXMMReg( gregOfRexRM(pfx, modrm), 
                    mkexpr( math_BLENDPS_128( src_vec, dst_vec, imm8) ) );
         goto decode_success;
      }
      break;

   case 0x0D:
      /* 66 0F 3A 0D /r ib = BLENDPD xmm1, xmm2/m128, imm8
         Blend Packed Double Precision Floating-Point Values (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {

         Int imm8;
         IRTemp dst_vec = newTemp(Ity_V128);
         IRTemp src_vec = newTemp(Ity_V128);

         modrm = getUChar(delta);
         assign( dst_vec, getXMMReg( gregOfRexRM(pfx, modrm) ) );

         if ( epartIsReg( modrm ) ) {
            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg( eregOfRexRM(pfx, modrm) ) );
            delta += 1+1;
            DIP( "blendpd $%d, %s,%s\n", imm8,
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "blendpd $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         putXMMReg( gregOfRexRM(pfx, modrm), 
                    mkexpr( math_BLENDPD_128( src_vec, dst_vec, imm8) ) );
         goto decode_success;
      }
      break;

   case 0x0E:
      /* 66 0F 3A 0E /r ib = PBLENDW xmm1, xmm2/m128, imm8
         Blend Packed Words (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {

         Int imm8;
         IRTemp dst_vec = newTemp(Ity_V128);
         IRTemp src_vec = newTemp(Ity_V128);

         modrm = getUChar(delta);

         assign( dst_vec, getXMMReg( gregOfRexRM(pfx, modrm) ) );

         if ( epartIsReg( modrm ) ) {
            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg( eregOfRexRM(pfx, modrm) ) );
            delta += 1+1;
            DIP( "pblendw $%d, %s,%s\n", imm8,
                 nameXMMReg( eregOfRexRM(pfx, modrm) ),
                 nameXMMReg( gregOfRexRM(pfx, modrm) ) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "pblendw $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg( gregOfRexRM(pfx, modrm) ) );
         }

         putXMMReg( gregOfRexRM(pfx, modrm), 
                    mkexpr( math_PBLENDW_128( src_vec, dst_vec, imm8) ) );
         goto decode_success;
      }
      break;

   case 0x14:
      /* 66 0F 3A 14 /r ib = PEXTRB r/m16, xmm, imm8
         Extract Byte from xmm, store in mem or zero-extend + store in gen.reg.
         (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PEXTRB_128_GtoE( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x15:
      /* 66 0F 3A 15 /r ib = PEXTRW r/m16, xmm, imm8
         Extract Word from xmm, store in mem or zero-extend + store in gen.reg.
         (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_PEXTRW( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x16:
      /* 66 no-REX.W 0F 3A 16 /r ib = PEXTRD reg/mem32, xmm2, imm8
         Extract Doubleword int from xmm reg and store in gen.reg or mem. (XMM) 
         Note that this insn has the same opcodes as PEXTRQ, but 
         here the REX.W bit is _not_ present */
      if (have66noF2noF3(pfx) 
          && sz == 2 /* REX.W is _not_ present */) {
         delta = dis_PEXTRD( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      /* 66 REX.W 0F 3A 16 /r ib = PEXTRQ reg/mem64, xmm2, imm8
         Extract Quadword int from xmm reg and store in gen.reg or mem. (XMM) 
         Note that this insn has the same opcodes as PEXTRD, but 
         here the REX.W bit is present */
      if (have66noF2noF3(pfx) 
          && sz == 8 /* REX.W is present */) {
         delta = dis_PEXTRQ( vbi, pfx, delta, False/*!isAvx*/);
         goto decode_success;
      }
      break;

   case 0x17:
      /* 66 0F 3A 17 /r ib = EXTRACTPS reg/mem32, xmm2, imm8 Extract
         float from xmm reg and store in gen.reg or mem.  This is
         identical to PEXTRD, except that REX.W appears to be ignored.
      */
      if (have66noF2noF3(pfx) 
          && (sz == 2 || /* ignore redundant REX.W */ sz == 8)) {
         delta = dis_EXTRACTPS( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x20:
      /* 66 0F 3A 20 /r ib = PINSRB xmm1, r32/m8, imm8
         Extract byte from r32/m8 and insert into xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {
         Int    imm8;
         IRTemp new8 = newTemp(Ity_I8);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx, modrm);
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8 = (Int)(getUChar(delta+1) & 0xF);
            assign( new8, unop(Iop_32to8, getIReg32(rE)) );
            delta += 1+1;
            DIP( "pinsrb $%d,%s,%s\n", imm8,
                 nameIReg32(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)(getUChar(delta+alen) & 0xF);
            assign( new8, loadLE( Ity_I8, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "pinsrb $%d,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }
         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( gregOfRexRM(pfx, modrm) ));
         IRTemp res = math_PINSRB_128( src_vec, new8, imm8 );
         putXMMReg( rG, mkexpr(res) );
         goto decode_success;
      }
      break;

   case 0x21:
      /* 66 0F 3A 21 /r ib = INSERTPS imm8, xmm2/m32, xmm1
         Insert Packed Single Precision Floating-Point Value (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         UInt   imm8;
         IRTemp d2ins = newTemp(Ity_I32); /* comes from the E part */
         const IRTemp inval = IRTemp_INVALID;

         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx, modrm);

         if ( epartIsReg( modrm ) ) {
            UInt   rE = eregOfRexRM(pfx, modrm);
            IRTemp vE = newTemp(Ity_V128);
            assign( vE, getXMMReg(rE) );
            IRTemp dsE[4] = { inval, inval, inval, inval };
            breakupV128to32s( vE, &dsE[3], &dsE[2], &dsE[1], &dsE[0] );
            imm8 = getUChar(delta+1);
            d2ins = dsE[(imm8 >> 6) & 3]; /* "imm8_count_s" */
            delta += 1+1;
            DIP( "insertps $%u, %s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( d2ins, loadLE( Ity_I32, mkexpr(addr) ) );
            imm8 = getUChar(delta+alen);
            delta += alen+1;
            DIP( "insertps $%u, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }

         IRTemp vG = newTemp(Ity_V128);
         assign( vG, getXMMReg(rG) );

         putXMMReg( rG, mkexpr(math_INSERTPS( vG, d2ins, imm8 )) );
         goto decode_success;
      }
      break;

   case 0x22:
      /* 66 no-REX.W 0F 3A 22 /r ib = PINSRD xmm1, r/m32, imm8
         Extract Doubleword int from gen.reg/mem32 and insert into xmm1 */
      if (have66noF2noF3(pfx) 
          && sz == 2 /* REX.W is NOT present */) {
         Int    imm8_10;
         IRTemp src_u32 = newTemp(Ity_I32);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx, modrm);

         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8_10 = (Int)(getUChar(delta+1) & 3);
            assign( src_u32, getIReg32( rE ) );
            delta += 1+1;
            DIP( "pinsrd $%d, %s,%s\n",
                 imm8_10, nameIReg32(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8_10 = (Int)(getUChar(delta+alen) & 3);
            assign( src_u32, loadLE( Ity_I32, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "pinsrd $%d, %s,%s\n", 
                 imm8_10, dis_buf, nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rG ));
         IRTemp res_vec = math_PINSRD_128( src_vec, src_u32, imm8_10 );
         putXMMReg( rG, mkexpr(res_vec) );
         goto decode_success;
      }
      /* 66 REX.W 0F 3A 22 /r ib = PINSRQ xmm1, r/m64, imm8
         Extract Quadword int from gen.reg/mem64 and insert into xmm1 */
      if (have66noF2noF3(pfx) 
          && sz == 8 /* REX.W is present */) {
         Int imm8_0;
         IRTemp src_u64 = newTemp(Ity_I64);
         modrm = getUChar(delta);
         UInt rG = gregOfRexRM(pfx, modrm);

         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8_0 = (Int)(getUChar(delta+1) & 1);
            assign( src_u64, getIReg64( rE ) );
            delta += 1+1;
            DIP( "pinsrq $%d, %s,%s\n",
                 imm8_0, nameIReg64(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8_0 = (Int)(getUChar(delta+alen) & 1);
            assign( src_u64, loadLE( Ity_I64, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "pinsrq $%d, %s,%s\n", 
                 imm8_0, dis_buf, nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rG ));
         IRTemp res_vec = math_PINSRQ_128( src_vec, src_u64, imm8_0 );
         putXMMReg( rG, mkexpr(res_vec) );
         goto decode_success;
      }
      break;

   case 0x40:
      /* 66 0F 3A 40 /r ib = DPPS xmm1, xmm2/m128, imm8
         Dot Product of Packed Single Precision Floating-Point Values (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         modrm = getUChar(delta);
         Int    imm8;
         IRTemp src_vec = newTemp(Ity_V128);
         IRTemp dst_vec = newTemp(Ity_V128);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         assign( dst_vec, getXMMReg( rG ) );
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg(rE) );
            delta += 1+1;
            DIP( "dpps $%d, %s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rG) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "dpps $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }
         IRTemp res = math_DPPS_128( src_vec, dst_vec, imm8 );
         putXMMReg( rG, mkexpr(res) );
         goto decode_success;
      }
      break;

   case 0x41:
      /* 66 0F 3A 41 /r ib = DPPD xmm1, xmm2/m128, imm8
         Dot Product of Packed Double Precision Floating-Point Values (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         modrm = getUChar(delta);
         Int    imm8;
         IRTemp src_vec = newTemp(Ity_V128);
         IRTemp dst_vec = newTemp(Ity_V128);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         assign( dst_vec, getXMMReg( rG ) );
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg(rE) );
            delta += 1+1;
            DIP( "dppd $%d, %s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rG) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "dppd $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }
         IRTemp res = math_DPPD_128( src_vec, dst_vec, imm8 );
         putXMMReg( rG, mkexpr(res) );
         goto decode_success;
      }
      break;

   case 0x42:
      /* 66 0F 3A 42 /r ib = MPSADBW xmm1, xmm2/m128, imm8
         Multiple Packed Sums of Absolule Difference (XMM) */
      if (have66noF2noF3(pfx) && sz == 2) {
         Int    imm8;
         IRTemp src_vec = newTemp(Ity_V128);
         IRTemp dst_vec = newTemp(Ity_V128);
         modrm          = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx, modrm);

         assign( dst_vec, getXMMReg(rG) );
  
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);

            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg(rE) );
            delta += 1+1;
            DIP( "mpsadbw $%d, %s,%s\n", imm8,
                 nameXMMReg(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "mpsadbw $%d, %s,%s\n", imm8, dis_buf, nameXMMReg(rG) );
         }

         putXMMReg( rG, mkexpr( math_MPSADBW_128(dst_vec, src_vec, imm8) ) );
         goto decode_success;
      }
      break;

   case 0x44:
      /* 66 0F 3A 44 /r ib = PCLMULQDQ xmm1, xmm2/m128, imm8
       * Carry-less multiplication of selected XMM quadwords into XMM
       * registers (a.k.a multiplication of polynomials over GF(2))
       */
      if (have66noF2noF3(pfx) && sz == 2) {
  
         Int imm8;
         IRTemp svec = newTemp(Ity_V128);
         IRTemp dvec = newTemp(Ity_V128);
         modrm       = getUChar(delta);
         UInt   rG   = gregOfRexRM(pfx, modrm);

         assign( dvec, getXMMReg(rG) );
  
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( svec, getXMMReg(rE) );
            delta += 1+1;
            DIP( "pclmulqdq $%d, %s,%s\n", imm8,
                 nameXMMReg(rE), nameXMMReg(rG) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( svec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "pclmulqdq $%d, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }

         putXMMReg( rG, mkexpr( math_PCLMULQDQ(dvec, svec, imm8) ) );
         goto decode_success;
      }
      break;

   case 0x60:
   case 0x61:
   case 0x62:
   case 0x63:
      /* 66 0F 3A 63 /r ib = PCMPISTRI imm8, xmm2/m128, xmm1
         66 0F 3A 62 /r ib = PCMPISTRM imm8, xmm2/m128, xmm1
         66 0F 3A 61 /r ib = PCMPESTRI imm8, xmm2/m128, xmm1
         66 0F 3A 60 /r ib = PCMPESTRM imm8, xmm2/m128, xmm1
         (selected special cases that actually occur in glibc,
          not by any means a complete implementation.)
      */
      if (have66noF2noF3(pfx) && sz == 2) {
         Long delta0 = delta;
         delta = dis_PCMPxSTRx( vbi, pfx, delta, False/*!isAvx*/, opc );
         if (delta > delta0) goto decode_success;
         /* else fall though; dis_PCMPxSTRx failed to decode it */
      }
      break;

   case 0xDF:
      /* 66 0F 3A DF /r ib = AESKEYGENASSIST imm8, xmm2/m128, xmm1 */
      if (have66noF2noF3(pfx) && sz == 2) {
         delta = dis_AESKEYGENASSIST( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   default:
      break;

   }

  decode_failure:
   *decode_OK = False;
   return deltaIN;

  decode_success:
   *decode_OK = True;
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_NONE         ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_NONE (
        /*MB_OUT*/DisResult* dres,
        /*MB_OUT*/Bool*      expect_CAS,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   Long   d64   = 0;
   UChar  abyte = 0;
   IRTemp addr  = IRTemp_INVALID;
   IRTemp t1    = IRTemp_INVALID;
   IRTemp t2    = IRTemp_INVALID;
   IRTemp t3    = IRTemp_INVALID;
   IRTemp t4    = IRTemp_INVALID;
   IRTemp t5    = IRTemp_INVALID;
   IRType ty    = Ity_INVALID;
   UChar  modrm = 0;
   Int    am_sz = 0;
   Int    d_sz  = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   case 0x00: /* ADD Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Add8, True, 1, delta, "add" );
      return delta;
   case 0x01: /* ADD Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Add8, True, sz, delta, "add" );
      return delta;

   case 0x02: /* ADD Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Add8, True, 1, delta, "add" );
      return delta;
   case 0x03: /* ADD Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Add8, True, sz, delta, "add" );
      return delta;

   case 0x04: /* ADD Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_Add8, True, delta, "add" );
      return delta;
   case 0x05: /* ADD Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A(sz, False, Iop_Add8, True, delta, "add" );
      return delta;

   case 0x08: /* OR Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Or8, True, 1, delta, "or" );
      return delta;
   case 0x09: /* OR Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Or8, True, sz, delta, "or" );
      return delta;

   case 0x0A: /* OR Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Or8, True, 1, delta, "or" );
      return delta;
   case 0x0B: /* OR Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Or8, True, sz, delta, "or" );
      return delta;

   case 0x0C: /* OR Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_Or8, True, delta, "or" );
      return delta;
   case 0x0D: /* OR Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_Or8, True, delta, "or" );
      return delta;

   case 0x10: /* ADC Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, True, Iop_Add8, True, 1, delta, "adc" );
      return delta;
   case 0x11: /* ADC Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, True, Iop_Add8, True, sz, delta, "adc" );
      return delta;

   case 0x12: /* ADC Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, True, Iop_Add8, True, 1, delta, "adc" );
      return delta;
   case 0x13: /* ADC Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, True, Iop_Add8, True, sz, delta, "adc" );
      return delta;

   case 0x14: /* ADC Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, True, Iop_Add8, True, delta, "adc" );
      return delta;
   case 0x15: /* ADC Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, True, Iop_Add8, True, delta, "adc" );
      return delta;

   case 0x18: /* SBB Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, True, Iop_Sub8, True, 1, delta, "sbb" );
      return delta;
   case 0x19: /* SBB Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, True, Iop_Sub8, True, sz, delta, "sbb" );
      return delta;

   case 0x1A: /* SBB Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, True, Iop_Sub8, True, 1, delta, "sbb" );
      return delta;
   case 0x1B: /* SBB Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, True, Iop_Sub8, True, sz, delta, "sbb" );
      return delta;

   case 0x1C: /* SBB Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, True, Iop_Sub8, True, delta, "sbb" );
      return delta;
   case 0x1D: /* SBB Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, True, Iop_Sub8, True, delta, "sbb" );
      return delta;

   case 0x20: /* AND Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_And8, True, 1, delta, "and" );
      return delta;
   case 0x21: /* AND Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_And8, True, sz, delta, "and" );
      return delta;

   case 0x22: /* AND Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_And8, True, 1, delta, "and" );
      return delta;
   case 0x23: /* AND Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_And8, True, sz, delta, "and" );
      return delta;

   case 0x24: /* AND Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_And8, True, delta, "and" );
      return delta;
   case 0x25: /* AND Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_And8, True, delta, "and" );
      return delta;

   case 0x28: /* SUB Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Sub8, True, 1, delta, "sub" );
      return delta;
   case 0x29: /* SUB Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Sub8, True, sz, delta, "sub" );
      return delta;

   case 0x2A: /* SUB Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Sub8, True, 1, delta, "sub" );
      return delta;
   case 0x2B: /* SUB Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Sub8, True, sz, delta, "sub" );
      return delta;

   case 0x2C: /* SUB Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A(1, False, Iop_Sub8, True, delta, "sub" );
      return delta;

   case 0x2D: /* SUB Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_Sub8, True, delta, "sub" );
      return delta;

   case 0x30: /* XOR Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Xor8, True, 1, delta, "xor" );
      return delta;
   case 0x31: /* XOR Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Xor8, True, sz, delta, "xor" );
      return delta;

   case 0x32: /* XOR Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Xor8, True, 1, delta, "xor" );
      return delta;
   case 0x33: /* XOR Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Xor8, True, sz, delta, "xor" );
      return delta;

   case 0x34: /* XOR Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_Xor8, True, delta, "xor" );
      return delta;
   case 0x35: /* XOR Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_Xor8, True, delta, "xor" );
      return delta;

   case 0x38: /* CMP Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Sub8, False, 1, delta, "cmp" );
      return delta;
   case 0x39: /* CMP Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_G_E ( vbi, pfx, False, Iop_Sub8, False, sz, delta, "cmp" );
      return delta;

   case 0x3A: /* CMP Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Sub8, False, 1, delta, "cmp" );
      return delta;
   case 0x3B: /* CMP Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_Sub8, False, sz, delta, "cmp" );
      return delta;

   case 0x3C: /* CMP Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_Sub8, False, delta, "cmp" );
      return delta;
   case 0x3D: /* CMP Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_Sub8, False, delta, "cmp" );
      return delta;

   case 0x50: /* PUSH eAX */
   case 0x51: /* PUSH eCX */
   case 0x52: /* PUSH eDX */
   case 0x53: /* PUSH eBX */
   case 0x55: /* PUSH eBP */
   case 0x56: /* PUSH eSI */
   case 0x57: /* PUSH eDI */
   case 0x54: /* PUSH eSP */
      /* This is the Right Way, in that the value to be pushed is
         established before %rsp is changed, so that pushq %rsp
         correctly pushes the old value. */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4 || sz == 8);
      if (sz == 4)
         sz = 8; /* there is no encoding for 32-bit push in 64-bit mode */
      ty = sz==2 ? Ity_I16 : Ity_I64;
      t1 = newTemp(ty); 
      t2 = newTemp(Ity_I64);
      assign(t1, getIRegRexB(sz, pfx, opc-0x50));
      assign(t2, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(sz)));
      putIReg64(R_RSP, mkexpr(t2) );
      storeLE(mkexpr(t2),mkexpr(t1));
      DIP("push%c %s\n", nameISize(sz), nameIRegRexB(sz,pfx,opc-0x50));
      return delta;

   case 0x58: /* POP eAX */
   case 0x59: /* POP eCX */
   case 0x5A: /* POP eDX */
   case 0x5B: /* POP eBX */
   case 0x5D: /* POP eBP */
   case 0x5E: /* POP eSI */
   case 0x5F: /* POP eDI */
   case 0x5C: /* POP eSP */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4 || sz == 8);
      if (sz == 4)
         sz = 8; /* there is no encoding for 32-bit pop in 64-bit mode */
      t1 = newTemp(szToITy(sz)); 
      t2 = newTemp(Ity_I64);
      assign(t2, getIReg64(R_RSP));
      assign(t1, loadLE(szToITy(sz),mkexpr(t2)));
      putIReg64(R_RSP, binop(Iop_Add64, mkexpr(t2), mkU64(sz)));
      putIRegRexB(sz, pfx, opc-0x58, mkexpr(t1));
      DIP("pop%c %s\n", nameISize(sz), nameIRegRexB(sz,pfx,opc-0x58));
      return delta;

   case 0x63: /* MOVSX */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (haveREX(pfx) && 1==getRexW(pfx)) {
         vassert(sz == 8);
         /* movsx r/m32 to r64 */
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta++;
            putIRegG(8, pfx, modrm, 
                             unop(Iop_32Sto64, 
                                  getIRegE(4, pfx, modrm)));
            DIP("movslq %s,%s\n",
                nameIRegE(4, pfx, modrm),
                nameIRegG(8, pfx, modrm));
            return delta;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putIRegG(8, pfx, modrm, 
                             unop(Iop_32Sto64, 
                                  loadLE(Ity_I32, mkexpr(addr))));
            DIP("movslq %s,%s\n", dis_buf, 
                nameIRegG(8, pfx, modrm));
            return delta;
         }
      } else {
         goto decode_failure;
      }

   case 0x68: /* PUSH Iv */
      if (haveF2orF3(pfx)) goto decode_failure;
      /* Note, sz==4 is not possible in 64-bit mode.  Hence ... */
      if (sz == 4) sz = 8;
      d64 = getSDisp(imin(4,sz),delta); 
      delta += imin(4,sz);
      goto do_push_I;

   case 0x69: /* IMUL Iv, Ev, Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_imul_I_E_G ( vbi, pfx, sz, delta, sz );
      return delta;

   case 0x6A: /* PUSH Ib, sign-extended to sz */
      if (haveF2orF3(pfx)) goto decode_failure;
      /* Note, sz==4 is not possible in 64-bit mode.  Hence ... */
      if (sz == 4) sz = 8;
      d64 = getSDisp8(delta); delta += 1;
      goto do_push_I;
   do_push_I:
      ty = szToITy(sz);
      t1 = newTemp(Ity_I64);
      t2 = newTemp(ty);
      assign( t1, binop(Iop_Sub64,getIReg64(R_RSP),mkU64(sz)) );
      putIReg64(R_RSP, mkexpr(t1) );
      /* stop mkU16 asserting if d32 is a negative 16-bit number
         (bug #132813) */
      if (ty == Ity_I16)
         d64 &= 0xFFFF;
      storeLE( mkexpr(t1), mkU(ty,d64) );
      DIP("push%c $%lld\n", nameISize(sz), (Long)d64);
      return delta;

   case 0x6B: /* IMUL Ib, Ev, Gv */
      delta = dis_imul_I_E_G ( vbi, pfx, sz, delta, 1 );
      return delta;

   case 0x70:
   case 0x71:
   case 0x72:   /* JBb/JNAEb (jump below) */
   case 0x73:   /* JNBb/JAEb (jump not below) */
   case 0x74:   /* JZb/JEb (jump zero) */
   case 0x75:   /* JNZb/JNEb (jump not zero) */
   case 0x76:   /* JBEb/JNAb (jump below or equal) */
   case 0x77:   /* JNBEb/JAb (jump not below or equal) */
   case 0x78:   /* JSb (jump negative) */
   case 0x79:   /* JSb (jump not negative) */
   case 0x7A:   /* JP (jump parity even) */
   case 0x7B:   /* JNP/JPO (jump parity odd) */
   case 0x7C:   /* JLb/JNGEb (jump less) */
   case 0x7D:   /* JGEb/JNLb (jump greater or equal) */
   case 0x7E:   /* JLEb/JNGb (jump less or equal) */
   case 0x7F: { /* JGb/JNLEb (jump greater) */
      Long   jmpDelta;
      HChar* comment  = "";
      if (haveF2orF3(pfx)) goto decode_failure;
      jmpDelta = getSDisp8(delta);
      vassert(-128 <= jmpDelta && jmpDelta < 128);
      d64 = (guest_RIP_bbstart+delta+1) + jmpDelta;
      delta++;
      if (resteerCisOk
          && vex_control.guest_chase_cond
          && (Addr64)d64 != (Addr64)guest_RIP_bbstart
          && jmpDelta < 0
          && resteerOkFn( callback_opaque, d64) ) {
         /* Speculation: assume this backward branch is taken.  So we
            need to emit a side-exit to the insn following this one,
            on the negation of the condition, and continue at the
            branch target address (d64).  If we wind up back at the
            first instruction of the trace, just stop; it's better to
            let the IR loop unroller handle that case. */
         stmt( IRStmt_Exit( 
                  mk_amd64g_calculate_condition(
                     (AMD64Condcode)(1 ^ (opc - 0x70))),
                  Ijk_Boring,
                  IRConst_U64(guest_RIP_bbstart+delta),
                  OFFB_RIP ) );
         dres->whatNext   = Dis_ResteerC;
         dres->continueAt = d64;
         comment = "(assumed taken)";
      }
      else
      if (resteerCisOk
          && vex_control.guest_chase_cond
          && (Addr64)d64 != (Addr64)guest_RIP_bbstart
          && jmpDelta >= 0
          && resteerOkFn( callback_opaque, guest_RIP_bbstart+delta ) ) {
         /* Speculation: assume this forward branch is not taken.  So
            we need to emit a side-exit to d64 (the dest) and continue
            disassembling at the insn immediately following this
            one. */
         stmt( IRStmt_Exit( 
                  mk_amd64g_calculate_condition((AMD64Condcode)(opc - 0x70)),
                  Ijk_Boring,
                  IRConst_U64(d64),
                  OFFB_RIP ) );
         dres->whatNext   = Dis_ResteerC;
         dres->continueAt = guest_RIP_bbstart+delta;
         comment = "(assumed not taken)";
      }
      else {
         /* Conservative default translation - end the block at this
            point. */
         jcc_01( dres, (AMD64Condcode)(opc - 0x70),
                 guest_RIP_bbstart+delta, d64 );
         vassert(dres->whatNext == Dis_StopHere);
      }
      DIP("j%s-8 0x%llx %s\n", name_AMD64Condcode(opc - 0x70), d64, comment);
      return delta;
   }

   case 0x80: /* Grp1 Ib,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      sz    = 1;
      d_sz  = 1;
      d64   = getSDisp8(delta + am_sz);
      delta = dis_Grp1 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, d64 );
      return delta;

   case 0x81: /* Grp1 Iv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = imin(sz,4);
      d64   = getSDisp(d_sz, delta + am_sz);
      delta = dis_Grp1 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, d64 );
      return delta;

   case 0x83: /* Grp1 Ib,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 1;
      d64   = getSDisp8(delta + am_sz);
      delta = dis_Grp1 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, d64 );
      return delta;

   case 0x84: /* TEST Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_And8, False, 1, delta, "test" );
      return delta;

   case 0x85: /* TEST Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op2_E_G ( vbi, pfx, False, Iop_And8, False, sz, delta, "test" );
      return delta;

   /* XCHG reg,mem automatically asserts LOCK# even without a LOCK
      prefix.  Therefore, surround it with a IRStmt_MBE(Imbe_BusLock)
      and IRStmt_MBE(Imbe_BusUnlock) pair.  But be careful; if it is
      used with an explicit LOCK prefix, we don't want to end up with
      two IRStmt_MBE(Imbe_BusLock)s -- one made here and one made by
      the generic LOCK logic at the top of disInstr. */
   case 0x86: /* XCHG Gb,Eb */
      sz = 1;
      /* Fall through ... */
   case 0x87: /* XCHG Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      ty = szToITy(sz);
      t1 = newTemp(ty); t2 = newTemp(ty);
      if (epartIsReg(modrm)) {
         assign(t1, getIRegE(sz, pfx, modrm));
         assign(t2, getIRegG(sz, pfx, modrm));
         putIRegG(sz, pfx, modrm, mkexpr(t1));
         putIRegE(sz, pfx, modrm, mkexpr(t2));
         delta++;
         DIP("xchg%c %s, %s\n", 
             nameISize(sz), nameIRegG(sz, pfx, modrm), 
                            nameIRegE(sz, pfx, modrm));
      } else {
         *expect_CAS = True;
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         assign( t1, loadLE(ty, mkexpr(addr)) );
         assign( t2, getIRegG(sz, pfx, modrm) );
         casLE( mkexpr(addr),
                mkexpr(t1), mkexpr(t2), guest_RIP_curr_instr );
         putIRegG( sz, pfx, modrm, mkexpr(t1) );
         delta += alen;
         DIP("xchg%c %s, %s\n", nameISize(sz), 
                                nameIRegG(sz, pfx, modrm), dis_buf);
      }
      return delta;

   case 0x88: /* MOV Gb,Eb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_mov_G_E(vbi, pfx, 1, delta);
      return delta;

   case 0x89: /* MOV Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_mov_G_E(vbi, pfx, sz, delta);
      return delta;

   case 0x8A: /* MOV Eb,Gb */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_mov_E_G(vbi, pfx, 1, delta);
      return delta;

   case 0x8B: /* MOV Ev,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_mov_E_G(vbi, pfx, sz, delta);
      return delta;

   case 0x8D: /* LEA M,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 4 && sz != 8)
         goto decode_failure;
      modrm = getUChar(delta);
      if (epartIsReg(modrm)) 
         goto decode_failure;
      /* NOTE!  this is the one place where a segment override prefix
         has no effect on the address calculation.  Therefore we clear
         any segment override bits in pfx. */
      addr = disAMode ( &alen, vbi, clearSegBits(pfx), delta, dis_buf, 0 );
      delta += alen;
      /* This is a hack.  But it isn't clear that really doing the
         calculation at 32 bits is really worth it.  Hence for leal,
         do the full 64-bit calculation and then truncate it. */
      putIRegG( sz, pfx, modrm, 
                         sz == 4
                            ? unop(Iop_64to32, mkexpr(addr))
                            : mkexpr(addr)
              );
      DIP("lea%c %s, %s\n", nameISize(sz), dis_buf, 
                            nameIRegG(sz,pfx,modrm));
      return delta;

   case 0x8F: { /* POPQ m64 / POPW m16 */
      Int   len;
      UChar rm;
      /* There is no encoding for 32-bit pop in 64-bit mode.
         So sz==4 actually means sz==8. */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4
              || /* tolerate redundant REX.W, see #210481 */ sz == 8);
      if (sz == 4) sz = 8;
      if (sz != 8) goto decode_failure; // until we know a sz==2 test case exists

      rm = getUChar(delta);

      /* make sure this instruction is correct POP */
      if (epartIsReg(rm) || gregLO3ofRM(rm) != 0)
         goto decode_failure;
      /* and has correct size */
      vassert(sz == 8);      
       
      t1 = newTemp(Ity_I64);
      t3 = newTemp(Ity_I64);
      assign( t1, getIReg64(R_RSP) );
      assign( t3, loadLE(Ity_I64, mkexpr(t1)) );
       
      /* Increase RSP; must be done before the STORE.  Intel manual
         says: If the RSP register is used as a base register for
         addressing a destination operand in memory, the POP
         instruction computes the effective address of the operand
         after it increments the RSP register.  */
      putIReg64(R_RSP, binop(Iop_Add64, mkexpr(t1), mkU64(sz)) );

      addr = disAMode ( &len, vbi, pfx, delta, dis_buf, 0 );
      storeLE( mkexpr(addr), mkexpr(t3) );

      DIP("popl %s\n", dis_buf);

      delta += len;
      return delta;
   }

   case 0x90: /* XCHG eAX,eAX */
      /* detect and handle F3 90 (rep nop) specially */
      if (!have66(pfx) && !haveF2(pfx) && haveF3(pfx)) {
         DIP("rep nop (P4 pause)\n");
         /* "observe" the hint.  The Vex client needs to be careful not
            to cause very long delays as a result, though. */
         jmp_lit(dres, Ijk_Yield, guest_RIP_bbstart+delta);
         vassert(dres->whatNext == Dis_StopHere);
         return delta;
      }
      /* detect and handle NOPs specially */
      if (/* F2/F3 probably change meaning completely */
          !haveF2orF3(pfx)
          /* If REX.B is 1, we're not exchanging rAX with itself */
          && getRexB(pfx)==0 ) {
         DIP("nop\n");
         return delta;
      }
      /* else fall through to normal case. */
   case 0x91: /* XCHG rAX,rCX */
   case 0x92: /* XCHG rAX,rDX */
   case 0x93: /* XCHG rAX,rBX */
   case 0x94: /* XCHG rAX,rSP */
   case 0x95: /* XCHG rAX,rBP */
   case 0x96: /* XCHG rAX,rSI */
   case 0x97: /* XCHG rAX,rDI */
      /* guard against mutancy */
      if (haveF2orF3(pfx)) goto decode_failure;
      codegen_xchg_rAX_Reg ( pfx, sz, opc - 0x90 );
      return delta;

   case 0x98: /* CBW */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz == 8) {
         putIRegRAX( 8, unop(Iop_32Sto64, getIRegRAX(4)) );
         DIP(/*"cdqe\n"*/"cltq");
         return delta;
      }
      if (sz == 4) {
         putIRegRAX( 4, unop(Iop_16Sto32, getIRegRAX(2)) );
         DIP("cwtl\n");
         return delta;
      }
      if (sz == 2) {
         putIRegRAX( 2, unop(Iop_8Sto16, getIRegRAX(1)) );
         DIP("cbw\n");
         return delta;
      }
      goto decode_failure;

   case 0x99: /* CWD/CDQ/CQO */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4 || sz == 8);
      ty = szToITy(sz);
      putIRegRDX( sz, 
                  binop(mkSizedOp(ty,Iop_Sar8), 
                        getIRegRAX(sz),
                        mkU8(sz == 2 ? 15 : (sz == 4 ? 31 : 63))) );
      DIP(sz == 2 ? "cwd\n" 
                  : (sz == 4 ? /*"cdq\n"*/ "cltd\n" 
                             : "cqo\n"));
      return delta;

   case 0x9B: /* FWAIT (X87 insn) */
      /* ignore? */
      DIP("fwait\n");
      return delta;

   case 0x9C: /* PUSHF */ {
      /* Note.  There is no encoding for a 32-bit pushf in 64-bit
         mode.  So sz==4 actually means sz==8. */
      /* 24 July 06: has also been seen with a redundant REX prefix,
         so must also allow sz==8. */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4 || sz == 8);
      if (sz == 4) sz = 8;
      if (sz != 8) goto decode_failure; // until we know a sz==2 test case exists

      t1 = newTemp(Ity_I64);
      assign( t1, binop(Iop_Sub64,getIReg64(R_RSP),mkU64(sz)) );
      putIReg64(R_RSP, mkexpr(t1) );

      t2 = newTemp(Ity_I64);
      assign( t2, mk_amd64g_calculate_rflags_all() );

      /* Patch in the D flag.  This can simply be a copy of bit 10 of
         baseBlock[OFFB_DFLAG]. */
      t3 = newTemp(Ity_I64);
      assign( t3, binop(Iop_Or64,
                        mkexpr(t2),
                        binop(Iop_And64,
                              IRExpr_Get(OFFB_DFLAG,Ity_I64),
                              mkU64(1<<10))) 
            );

      /* And patch in the ID flag. */
      t4 = newTemp(Ity_I64);
      assign( t4, binop(Iop_Or64,
                        mkexpr(t3),
                        binop(Iop_And64,
                              binop(Iop_Shl64, IRExpr_Get(OFFB_IDFLAG,Ity_I64), 
                                               mkU8(21)),
                              mkU64(1<<21)))
            );

      /* And patch in the AC flag too. */
      t5 = newTemp(Ity_I64);
      assign( t5, binop(Iop_Or64,
                        mkexpr(t4),
                        binop(Iop_And64,
                              binop(Iop_Shl64, IRExpr_Get(OFFB_ACFLAG,Ity_I64), 
                                               mkU8(18)),
                              mkU64(1<<18)))
            );

      /* if sz==2, the stored value needs to be narrowed. */
      if (sz == 2)
        storeLE( mkexpr(t1), unop(Iop_32to16,
                             unop(Iop_64to32,mkexpr(t5))) );
      else 
        storeLE( mkexpr(t1), mkexpr(t5) );

      DIP("pushf%c\n", nameISize(sz));
      return delta;
   }

   case 0x9D: /* POPF */
      /* Note.  There is no encoding for a 32-bit popf in 64-bit mode.
         So sz==4 actually means sz==8. */
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 2 || sz == 4);
      if (sz == 4) sz = 8;
      if (sz != 8) goto decode_failure; // until we know a sz==2 test case exists
      t1 = newTemp(Ity_I64); t2 = newTemp(Ity_I64);
      assign(t2, getIReg64(R_RSP));
      assign(t1, widenUto64(loadLE(szToITy(sz),mkexpr(t2))));
      putIReg64(R_RSP, binop(Iop_Add64, mkexpr(t2), mkU64(sz)));
      /* t1 is the flag word.  Mask out everything except OSZACP and 
         set the flags thunk to AMD64G_CC_OP_COPY. */
      stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
      stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
      stmt( IRStmt_Put( OFFB_CC_DEP1, 
                        binop(Iop_And64,
                              mkexpr(t1), 
                              mkU64( AMD64G_CC_MASK_C | AMD64G_CC_MASK_P 
                                     | AMD64G_CC_MASK_A | AMD64G_CC_MASK_Z 
                                     | AMD64G_CC_MASK_S| AMD64G_CC_MASK_O )
                             )
                       )
          );

      /* Also need to set the D flag, which is held in bit 10 of t1.
         If zero, put 1 in OFFB_DFLAG, else -1 in OFFB_DFLAG. */
      stmt( IRStmt_Put( 
               OFFB_DFLAG,
               IRExpr_Mux0X( 
                  unop(Iop_32to8,
                  unop(Iop_64to32,
                       binop(Iop_And64, 
                             binop(Iop_Shr64, mkexpr(t1), mkU8(10)), 
                             mkU64(1)))),
                  mkU64(1), 
                  mkU64(0xFFFFFFFFFFFFFFFFULL))) 
          );

      /* And set the ID flag */
      stmt( IRStmt_Put( 
               OFFB_IDFLAG,
               IRExpr_Mux0X( 
                  unop(Iop_32to8,
                  unop(Iop_64to32,
                       binop(Iop_And64, 
                             binop(Iop_Shr64, mkexpr(t1), mkU8(21)), 
                             mkU64(1)))),
                  mkU64(0), 
                  mkU64(1))) 
          );

      /* And set the AC flag too */
      stmt( IRStmt_Put( 
               OFFB_ACFLAG,
               IRExpr_Mux0X( 
                  unop(Iop_32to8,
                  unop(Iop_64to32,
                       binop(Iop_And64, 
                             binop(Iop_Shr64, mkexpr(t1), mkU8(18)), 
                             mkU64(1)))),
                  mkU64(0), 
                  mkU64(1))) 
          );

      DIP("popf%c\n", nameISize(sz));
      return delta;

   case 0x9E: /* SAHF */
      codegen_SAHF();
      DIP("sahf\n");
      return delta;

   case 0x9F: /* LAHF */
      codegen_LAHF();
      DIP("lahf\n");
      return delta;

   case 0xA0: /* MOV Ob,AL */
      if (have66orF2orF3(pfx)) goto decode_failure;
      sz = 1;
      /* Fall through ... */
   case 0xA1: /* MOV Ov,eAX */
      if (sz != 8 && sz != 4 && sz != 2 && sz != 1) 
         goto decode_failure;
      d64 = getDisp64(delta); 
      delta += 8;
      ty = szToITy(sz);
      addr = newTemp(Ity_I64);
      assign( addr, handleAddrOverrides(vbi, pfx, mkU64(d64)) );
      putIRegRAX(sz, loadLE( ty, mkexpr(addr) ));
      DIP("mov%c %s0x%llx, %s\n", nameISize(sz), 
                                  segRegTxt(pfx), d64,
                                  nameIRegRAX(sz));
      return delta;

   case 0xA2: /* MOV AL,Ob */
      if (have66orF2orF3(pfx)) goto decode_failure;
      sz = 1;
      /* Fall through ... */
   case 0xA3: /* MOV eAX,Ov */
      if (sz != 8 && sz != 4 && sz != 2 && sz != 1) 
         goto decode_failure;
      d64 = getDisp64(delta); 
      delta += 8;
      ty = szToITy(sz);
      addr = newTemp(Ity_I64);
      assign( addr, handleAddrOverrides(vbi, pfx, mkU64(d64)) );
      storeLE( mkexpr(addr), getIRegRAX(sz) );
      DIP("mov%c %s, %s0x%llx\n", nameISize(sz), nameIRegRAX(sz),
                                  segRegTxt(pfx), d64);
      return delta;

   case 0xA4:
   case 0xA5:
      /* F3 A4: rep movsb */
      if (haveF3(pfx) && !haveF2(pfx)) {
         if (opc == 0xA4)
            sz = 1;
         dis_REP_op ( dres, AMD64CondAlways, dis_MOVS, sz,
                      guest_RIP_curr_instr,
                      guest_RIP_bbstart+delta, "rep movs", pfx );
        dres->whatNext = Dis_StopHere;
        return delta;
      }
      /* A4: movsb */
      if (!haveF3(pfx) && !haveF2(pfx)) {
         if (opc == 0xA4)
            sz = 1;
         dis_string_op( dis_MOVS, sz, "movs", pfx );
         return delta;
      }
      goto decode_failure;

   case 0xA6:
   case 0xA7:
      /* F3 A6/A7: repe cmps/rep cmps{w,l,q} */
      if (haveF3(pfx) && !haveF2(pfx)) {
         if (opc == 0xA6)
            sz = 1;
         dis_REP_op ( dres, AMD64CondZ, dis_CMPS, sz, 
                      guest_RIP_curr_instr,
                      guest_RIP_bbstart+delta, "repe cmps", pfx );
         dres->whatNext = Dis_StopHere;
         return delta;
      }
      goto decode_failure;

   case 0xAA:
   case 0xAB:
      /* F3 AA/AB: rep stosb/rep stos{w,l,q} */
      if (haveF3(pfx) && !haveF2(pfx)) {
         if (opc == 0xAA)
            sz = 1;
         dis_REP_op ( dres, AMD64CondAlways, dis_STOS, sz,
                      guest_RIP_curr_instr,
                      guest_RIP_bbstart+delta, "rep stos", pfx );
         vassert(dres->whatNext == Dis_StopHere);
         return delta;
      }
      /* AA/AB: stosb/stos{w,l,q} */
      if (!haveF3(pfx) && !haveF2(pfx)) {
         if (opc == 0xAA)
            sz = 1;
         dis_string_op( dis_STOS, sz, "stos", pfx );
         return delta;
      }
      goto decode_failure;

   case 0xA8: /* TEST Ib, AL */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( 1, False, Iop_And8, False, delta, "test" );
      return delta;
   case 0xA9: /* TEST Iv, eAX */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_op_imm_A( sz, False, Iop_And8, False, delta, "test" );
      return delta;

   case 0xAC: /* LODS, no REP prefix */
   case 0xAD:
      dis_string_op( dis_LODS, ( opc == 0xAC ? 1 : sz ), "lods", pfx );
      return delta;

   case 0xAE:
   case 0xAF:
      /* F2 AE/AF: repne scasb/repne scas{w,l,q} */
      if (haveF2(pfx) && !haveF3(pfx)) {
         if (opc == 0xAE)
            sz = 1;
         dis_REP_op ( dres, AMD64CondNZ, dis_SCAS, sz, 
                      guest_RIP_curr_instr,
                      guest_RIP_bbstart+delta, "repne scas", pfx );
         vassert(dres->whatNext == Dis_StopHere);
         return delta;
      }
      /* F3 AE/AF: repe scasb/repe scas{w,l,q} */
      if (!haveF2(pfx) && haveF3(pfx)) {
         if (opc == 0xAE)
            sz = 1;
         dis_REP_op ( dres, AMD64CondZ, dis_SCAS, sz, 
                      guest_RIP_curr_instr,
                      guest_RIP_bbstart+delta, "repe scas", pfx );
         vassert(dres->whatNext == Dis_StopHere);
         return delta;
      }
      /* AE/AF: scasb/scas{w,l,q} */
      if (!haveF2(pfx) && !haveF3(pfx)) {
         if (opc == 0xAE)
            sz = 1;
         dis_string_op( dis_SCAS, sz, "scas", pfx );
         return delta;
      }
      goto decode_failure;

   /* XXXX be careful here with moves to AH/BH/CH/DH */
   case 0xB0: /* MOV imm,AL */
   case 0xB1: /* MOV imm,CL */
   case 0xB2: /* MOV imm,DL */
   case 0xB3: /* MOV imm,BL */
   case 0xB4: /* MOV imm,AH */
   case 0xB5: /* MOV imm,CH */
   case 0xB6: /* MOV imm,DH */
   case 0xB7: /* MOV imm,BH */
      if (haveF2orF3(pfx)) goto decode_failure;
      d64 = getUChar(delta); 
      delta += 1;
      putIRegRexB(1, pfx, opc-0xB0, mkU8(d64));
      DIP("movb $%lld,%s\n", d64, nameIRegRexB(1,pfx,opc-0xB0));
      return delta;

   case 0xB8: /* MOV imm,eAX */
   case 0xB9: /* MOV imm,eCX */
   case 0xBA: /* MOV imm,eDX */
   case 0xBB: /* MOV imm,eBX */
   case 0xBC: /* MOV imm,eSP */
   case 0xBD: /* MOV imm,eBP */
   case 0xBE: /* MOV imm,eSI */
   case 0xBF: /* MOV imm,eDI */
      /* This is the one-and-only place where 64-bit literals are
         allowed in the instruction stream. */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz == 8) {
         d64 = getDisp64(delta);
         delta += 8;
         putIRegRexB(8, pfx, opc-0xB8, mkU64(d64));
         DIP("movabsq $%lld,%s\n", (Long)d64, 
                                   nameIRegRexB(8,pfx,opc-0xB8));
      } else {
         d64 = getSDisp(imin(4,sz),delta);
         delta += imin(4,sz);
         putIRegRexB(sz, pfx, opc-0xB8, 
                         mkU(szToITy(sz), d64 & mkSizeMask(sz)));
         DIP("mov%c $%lld,%s\n", nameISize(sz), 
                                 (Long)d64, 
                                 nameIRegRexB(sz,pfx,opc-0xB8));
      }
      return delta;

   case 0xC0: { /* Grp2 Ib,Eb */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 1;
      d64   = getUChar(delta + am_sz);
      sz    = 1;
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         mkU8(d64 & 0xFF), NULL, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xC1: { /* Grp2 Ib,Ev */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 1;
      d64   = getUChar(delta + am_sz);
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         mkU8(d64 & 0xFF), NULL, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xC2: /* RET imm16 */
      if (have66orF2orF3(pfx)) goto decode_failure;
      d64 = getUDisp16(delta); 
      delta += 2;
      dis_ret(dres, vbi, d64);
      DIP("ret $%lld\n", d64);
      return delta;

   case 0xC3: /* RET */
      if (have66orF2(pfx)) goto decode_failure;
      /* F3 is acceptable on AMD. */
      dis_ret(dres, vbi, 0);
      DIP(haveF3(pfx) ? "rep ; ret\n" : "ret\n");
      return delta;

   case 0xC6: /* MOV Ib,Eb */
      sz = 1;
      goto do_Mov_I_E;
   case 0xC7: /* MOV Iv,Ev */
      goto do_Mov_I_E;
   do_Mov_I_E:
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      if (epartIsReg(modrm)) {
         delta++; /* mod/rm byte */
         d64 = getSDisp(imin(4,sz),delta); 
         delta += imin(4,sz);
         putIRegE(sz, pfx, modrm, 
                      mkU(szToITy(sz), d64 & mkSizeMask(sz)));
         DIP("mov%c $%lld, %s\n", nameISize(sz), 
                                  (Long)d64, 
                                  nameIRegE(sz,pfx,modrm));
      } else {
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 
                           /*xtra*/imin(4,sz) );
         delta += alen;
         d64 = getSDisp(imin(4,sz),delta);
         delta += imin(4,sz);
         storeLE(mkexpr(addr), 
                 mkU(szToITy(sz), d64 & mkSizeMask(sz)));
         DIP("mov%c $%lld, %s\n", nameISize(sz), (Long)d64, dis_buf);
      }
      return delta;

   case 0xC8: /* ENTER */
      /* Same comments re operand size as for LEAVE below apply.
         Also, only handles the case "enter $imm16, $0"; other cases
         for the second operand (nesting depth) are not handled. */
      if (sz != 4)
         goto decode_failure;
      d64 = getUDisp16(delta);
      delta += 2;
      vassert(d64 >= 0 && d64 <= 0xFFFF);
      if (getUChar(delta) != 0)
         goto decode_failure;
      delta++;
      /* Intel docs seem to suggest:
           push rbp
           temp = rsp
           rbp = temp
           rsp = rsp - imm16
      */
      t1 = newTemp(Ity_I64);
      assign(t1, getIReg64(R_RBP));
      t2 = newTemp(Ity_I64);
      assign(t2, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(8)));
      putIReg64(R_RSP, mkexpr(t2));
      storeLE(mkexpr(t2), mkexpr(t1));
      putIReg64(R_RBP, mkexpr(t2));
      if (d64 > 0) {
         putIReg64(R_RSP, binop(Iop_Sub64, mkexpr(t2), mkU64(d64)));
      }
      DIP("enter $%u, $0\n", (UInt)d64);
      return delta;

   case 0xC9: /* LEAVE */
      /* In 64-bit mode this defaults to a 64-bit operand size.  There
         is no way to encode a 32-bit variant.  Hence sz==4 but we do
         it as if sz=8. */
      if (sz != 4) 
         goto decode_failure;
      t1 = newTemp(Ity_I64); 
      t2 = newTemp(Ity_I64);
      assign(t1, getIReg64(R_RBP));
      /* First PUT RSP looks redundant, but need it because RSP must
         always be up-to-date for Memcheck to work... */
      putIReg64(R_RSP, mkexpr(t1));
      assign(t2, loadLE(Ity_I64,mkexpr(t1)));
      putIReg64(R_RBP, mkexpr(t2));
      putIReg64(R_RSP, binop(Iop_Add64, mkexpr(t1), mkU64(8)) );
      DIP("leave\n");
      return delta;

   case 0xCC: /* INT 3 */
      jmp_lit(dres, Ijk_SigTRAP, guest_RIP_bbstart + delta);
      vassert(dres->whatNext == Dis_StopHere);
      DIP("int $0x3\n");
      return delta;

   case 0xD0: { /* Grp2 1,Eb */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 0;
      d64   = 1;
      sz    = 1;
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         mkU8(d64), NULL, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xD1: { /* Grp2 1,Ev */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 0;
      d64   = 1;
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         mkU8(d64), NULL, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xD2: { /* Grp2 CL,Eb */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 0;
      sz    = 1;
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         getIRegCL(), "%cl", &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xD3: { /* Grp2 CL,Ev */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d_sz  = 0;
      delta = dis_Grp2 ( vbi, pfx, delta, modrm, am_sz, d_sz, sz, 
                         getIRegCL(), "%cl", &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xD8: /* X87 instructions */
   case 0xD9:
   case 0xDA:
   case 0xDB:
   case 0xDC:
   case 0xDD:
   case 0xDE:
   case 0xDF: {
      Bool redundantREXWok = False;

      if (haveF2orF3(pfx)) 
         goto decode_failure;

      /* kludge to tolerate redundant rex.w prefixes (should do this
         properly one day) */
      /* mono 1.1.18.1 produces 48 D9 FA, which is rex.w fsqrt */
      if ( (opc == 0xD9 && getUChar(delta+0) == 0xFA)/*fsqrt*/ )
         redundantREXWok = True;

      Bool size_OK = False;
      if ( sz == 4 )
         size_OK = True;
      else if ( sz == 8 )
         size_OK = redundantREXWok;
      else if ( sz == 2 ) {
         int mod_rm = getUChar(delta+0);
         int reg = gregLO3ofRM(mod_rm);
         /* The HotSpot JVM uses these */
         if ( (opc == 0xDD) && (reg == 0 /* FLDL   */ ||
                                reg == 4 /* FNSAVE */ ||
                                reg == 6 /* FRSTOR */ ) )
            size_OK = True;
      }
      /* AMD manual says 0x66 size override is ignored, except where
         it is meaningful */
      if (!size_OK)
         goto decode_failure;

      Bool decode_OK = False;
      delta = dis_FPU ( &decode_OK, vbi, pfx, delta );
      if (!decode_OK)
         goto decode_failure;

      return delta;
   }

   case 0xE0: /* LOOPNE disp8: decrement count, jump if count != 0 && ZF==0 */
   case 0xE1: /* LOOPE  disp8: decrement count, jump if count != 0 && ZF==1 */
   case 0xE2: /* LOOP   disp8: decrement count, jump if count != 0 */
    { /* The docs say this uses rCX as a count depending on the
         address size override, not the operand one. */
      IRExpr* zbit  = NULL;
      IRExpr* count = NULL;
      IRExpr* cond  = NULL;
      HChar*  xtra  = NULL;

      if (have66orF2orF3(pfx) || 1==getRexW(pfx)) goto decode_failure;
      /* So at this point we've rejected any variants which appear to
         be governed by the usual operand-size modifiers.  Hence only
         the address size prefix can have an effect.  It changes the
         size from 64 (default) to 32. */
      d64 = guest_RIP_bbstart+delta+1 + getSDisp8(delta);
      delta++;
      if (haveASO(pfx)) {
         /* 64to32 of 64-bit get is merely a get-put improvement
            trick. */
         putIReg32(R_RCX, binop(Iop_Sub32,
                                unop(Iop_64to32, getIReg64(R_RCX)), 
                                mkU32(1)));
      } else {
         putIReg64(R_RCX, binop(Iop_Sub64, getIReg64(R_RCX), mkU64(1)));
      }

      /* This is correct, both for 32- and 64-bit versions.  If we're
         doing a 32-bit dec and the result is zero then the default
         zero extension rule will cause the upper 32 bits to be zero
         too.  Hence a 64-bit check against zero is OK. */
      count = getIReg64(R_RCX);
      cond = binop(Iop_CmpNE64, count, mkU64(0));
      switch (opc) {
         case 0xE2: 
            xtra = ""; 
            break;
         case 0xE1: 
            xtra = "e"; 
            zbit = mk_amd64g_calculate_condition( AMD64CondZ );
            cond = mkAnd1(cond, zbit);
            break;
         case 0xE0: 
            xtra = "ne";
            zbit = mk_amd64g_calculate_condition( AMD64CondNZ );
            cond = mkAnd1(cond, zbit);
            break;
         default:
	    vassert(0);
      }
      stmt( IRStmt_Exit(cond, Ijk_Boring, IRConst_U64(d64), OFFB_RIP) );

      DIP("loop%s%s 0x%llx\n", xtra, haveASO(pfx) ? "l" : "", d64);
      return delta;
    }

   case 0xE3: 
      /* JRCXZ or JECXZ, depending address size override. */
      if (have66orF2orF3(pfx)) goto decode_failure;
      d64 = (guest_RIP_bbstart+delta+1) + getSDisp8(delta); 
      delta++;
      if (haveASO(pfx)) {
         /* 32-bit */
         stmt( IRStmt_Exit( binop(Iop_CmpEQ64, 
                                  unop(Iop_32Uto64, getIReg32(R_RCX)), 
                                  mkU64(0)),
                            Ijk_Boring,
                            IRConst_U64(d64),
                            OFFB_RIP
             ));
         DIP("jecxz 0x%llx\n", d64);
      } else {
         /* 64-bit */
         stmt( IRStmt_Exit( binop(Iop_CmpEQ64, 
                                  getIReg64(R_RCX), 
                                  mkU64(0)),
                            Ijk_Boring,
                            IRConst_U64(d64),
                            OFFB_RIP
               ));
         DIP("jrcxz 0x%llx\n", d64);
      }
      return delta;

   case 0xE4: /* IN imm8, AL */
      sz = 1; 
      t1 = newTemp(Ity_I64);
      abyte = getUChar(delta); delta++;
      assign(t1, mkU64( abyte & 0xFF ));
      DIP("in%c $%d,%s\n", nameISize(sz), (Int)abyte, nameIRegRAX(sz));
      goto do_IN;
   case 0xE5: /* IN imm8, eAX */
      if (!(sz == 2 || sz == 4)) goto decode_failure;
      t1 = newTemp(Ity_I64);
      abyte = getUChar(delta); delta++;
      assign(t1, mkU64( abyte & 0xFF ));
      DIP("in%c $%d,%s\n", nameISize(sz), (Int)abyte, nameIRegRAX(sz));
      goto do_IN;
   case 0xEC: /* IN %DX, AL */
      sz = 1; 
      t1 = newTemp(Ity_I64);
      assign(t1, unop(Iop_16Uto64, getIRegRDX(2)));
      DIP("in%c %s,%s\n", nameISize(sz), nameIRegRDX(2), 
                                         nameIRegRAX(sz));
      goto do_IN;
   case 0xED: /* IN %DX, eAX */
      if (!(sz == 2 || sz == 4)) goto decode_failure;
      t1 = newTemp(Ity_I64);
      assign(t1, unop(Iop_16Uto64, getIRegRDX(2)));
      DIP("in%c %s,%s\n", nameISize(sz), nameIRegRDX(2), 
                                         nameIRegRAX(sz));
      goto do_IN;
   do_IN: {
      /* At this point, sz indicates the width, and t1 is a 64-bit
         value giving port number. */
      IRDirty* d;
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 1 || sz == 2 || sz == 4);
      ty = szToITy(sz);
      t2 = newTemp(Ity_I64);
      d = unsafeIRDirty_1_N( 
             t2,
             0/*regparms*/, 
             "amd64g_dirtyhelper_IN", 
             &amd64g_dirtyhelper_IN,
             mkIRExprVec_2( mkexpr(t1), mkU64(sz) )
          );
      /* do the call, dumping the result in t2. */
      stmt( IRStmt_Dirty(d) );
      putIRegRAX(sz, narrowTo( ty, mkexpr(t2) ) );
      return delta;
   }

   case 0xE6: /* OUT AL, imm8 */
      sz = 1;
      t1 = newTemp(Ity_I64);
      abyte = getUChar(delta); delta++;
      assign( t1, mkU64( abyte & 0xFF ) );
      DIP("out%c %s,$%d\n", nameISize(sz), nameIRegRAX(sz), (Int)abyte);
      goto do_OUT;
   case 0xE7: /* OUT eAX, imm8 */
      if (!(sz == 2 || sz == 4)) goto decode_failure;
      t1 = newTemp(Ity_I64);
      abyte = getUChar(delta); delta++;
      assign( t1, mkU64( abyte & 0xFF ) );
      DIP("out%c %s,$%d\n", nameISize(sz), nameIRegRAX(sz), (Int)abyte);
      goto do_OUT;
   case 0xEE: /* OUT AL, %DX */
      sz = 1;
      t1 = newTemp(Ity_I64);
      assign( t1, unop(Iop_16Uto64, getIRegRDX(2)) );
      DIP("out%c %s,%s\n", nameISize(sz), nameIRegRAX(sz),
                                          nameIRegRDX(2));
      goto do_OUT;
   case 0xEF: /* OUT eAX, %DX */
      if (!(sz == 2 || sz == 4)) goto decode_failure;
      t1 = newTemp(Ity_I64);
      assign( t1, unop(Iop_16Uto64, getIRegRDX(2)) );
      DIP("out%c %s,%s\n", nameISize(sz), nameIRegRAX(sz),
                                          nameIRegRDX(2));
      goto do_OUT;
   do_OUT: {
      /* At this point, sz indicates the width, and t1 is a 64-bit
         value giving port number. */
      IRDirty* d;
      if (haveF2orF3(pfx)) goto decode_failure;
      vassert(sz == 1 || sz == 2 || sz == 4);
      ty = szToITy(sz);
      d = unsafeIRDirty_0_N( 
             0/*regparms*/, 
             "amd64g_dirtyhelper_OUT", 
             &amd64g_dirtyhelper_OUT,
             mkIRExprVec_3( mkexpr(t1),
                            widenUto64( getIRegRAX(sz) ), 
                            mkU64(sz) )
          );
      stmt( IRStmt_Dirty(d) );
      return delta;
   }

   case 0xE8: /* CALL J4 */
      if (haveF2orF3(pfx)) goto decode_failure;
      d64 = getSDisp32(delta); delta += 4;
      d64 += (guest_RIP_bbstart+delta); 
      /* (guest_RIP_bbstart+delta) == return-to addr, d64 == call-to addr */
      t1 = newTemp(Ity_I64); 
      assign(t1, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(8)));
      putIReg64(R_RSP, mkexpr(t1));
      storeLE( mkexpr(t1), mkU64(guest_RIP_bbstart+delta));
      t2 = newTemp(Ity_I64);
      assign(t2, mkU64((Addr64)d64));
      make_redzone_AbiHint(vbi, t1, t2/*nia*/, "call-d32");
      if (resteerOkFn( callback_opaque, (Addr64)d64) ) {
         /* follow into the call target. */
         dres->whatNext   = Dis_ResteerU;
         dres->continueAt = d64;
      } else {
         jmp_lit(dres, Ijk_Call, d64);
         vassert(dres->whatNext == Dis_StopHere);
      }
      DIP("call 0x%llx\n",d64);
      return delta;

   case 0xE9: /* Jv (jump, 16/32 offset) */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 4) 
         goto decode_failure; /* JRS added 2004 July 11 */
      d64 = (guest_RIP_bbstart+delta+sz) + getSDisp(sz,delta); 
      delta += sz;
      if (resteerOkFn(callback_opaque,d64)) {
         dres->whatNext   = Dis_ResteerU;
         dres->continueAt = d64;
      } else {
         jmp_lit(dres, Ijk_Boring, d64);
         vassert(dres->whatNext == Dis_StopHere);
      }
      DIP("jmp 0x%llx\n", d64);
      return delta;

   case 0xEB: /* Jb (jump, byte offset) */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 4) 
         goto decode_failure; /* JRS added 2004 July 11 */
      d64 = (guest_RIP_bbstart+delta+1) + getSDisp8(delta); 
      delta++;
      if (resteerOkFn(callback_opaque,d64)) {
         dres->whatNext   = Dis_ResteerU;
         dres->continueAt = d64;
      } else {
         jmp_lit(dres, Ijk_Boring, d64);
         vassert(dres->whatNext == Dis_StopHere);
      }
      DIP("jmp-8 0x%llx\n", d64);
      return delta;

   case 0xF5: /* CMC */
   case 0xF8: /* CLC */
   case 0xF9: /* STC */
      t1 = newTemp(Ity_I64);
      t2 = newTemp(Ity_I64);
      assign( t1, mk_amd64g_calculate_rflags_all() );
      switch (opc) {
         case 0xF5: 
            assign( t2, binop(Iop_Xor64, mkexpr(t1), 
                                         mkU64(AMD64G_CC_MASK_C)));
            DIP("cmc\n");
            break;
         case 0xF8: 
            assign( t2, binop(Iop_And64, mkexpr(t1), 
                                         mkU64(~AMD64G_CC_MASK_C)));
            DIP("clc\n");
            break;
         case 0xF9: 
            assign( t2, binop(Iop_Or64, mkexpr(t1), 
                                        mkU64(AMD64G_CC_MASK_C)));
            DIP("stc\n");
            break;
         default: 
            vpanic("disInstr(x64)(cmc/clc/stc)");
      }
      stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
      stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
      stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(t2) ));
      /* Set NDEP even though it isn't used.  This makes redundant-PUT
         elimination of previous stores to this field work better. */
      stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));
      return delta;

   case 0xF6: { /* Grp3 Eb */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_Grp3 ( vbi, pfx, 1, delta, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xF7: { /* Grp3 Ev */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_Grp3 ( vbi, pfx, sz, delta, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xFC: /* CLD */
      if (haveF2orF3(pfx)) goto decode_failure;
      stmt( IRStmt_Put( OFFB_DFLAG, mkU64(1)) );
      DIP("cld\n");
      return delta;

   case 0xFD: /* STD */
      if (haveF2orF3(pfx)) goto decode_failure;
      stmt( IRStmt_Put( OFFB_DFLAG, mkU64(-1ULL)) );
      DIP("std\n");
      return delta;

   case 0xFE: { /* Grp4 Eb */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_Grp4 ( vbi, pfx, delta, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   case 0xFF: { /* Grp5 Ev */
      Bool decode_OK = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_Grp5 ( vbi, pfx, sz, delta, dres, &decode_OK );
      if (!decode_OK) goto decode_failure;
      return delta;
   }

   default:
      break;

   }

  decode_failure:
   return deltaIN; /* fail */
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F           ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_0F (
        /*MB_OUT*/DisResult* dres,
        /*MB_OUT*/Bool*      expect_CAS,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   Long   d64   = 0;
   IRTemp addr  = IRTemp_INVALID;
   IRTemp t1    = IRTemp_INVALID;
   IRTemp t2    = IRTemp_INVALID;
   UChar  modrm = 0;
   Int    am_sz = 0;
   Int    alen  = 0;
   HChar  dis_buf[50];

   /* In the first switch, look for ordinary integer insns. */
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) { /* first switch */

   case 0x01:
   {
      modrm = getUChar(delta);
      /* 0F 01 /0 -- SGDT */
      /* 0F 01 /1 -- SIDT */
      if (!epartIsReg(modrm)
          && (gregLO3ofRM(modrm) == 0 || gregLO3ofRM(modrm) == 1)) {
         /* This is really revolting, but ... since each processor
            (core) only has one IDT and one GDT, just let the guest
            see it (pass-through semantics).  I can't see any way to
            construct a faked-up value, so don't bother to try. */
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         switch (gregLO3ofRM(modrm)) {
            case 0: DIP("sgdt %s\n", dis_buf); break;
            case 1: DIP("sidt %s\n", dis_buf); break;
            default: vassert(0); /*NOTREACHED*/
         }
         IRDirty* d = unsafeIRDirty_0_N (
                          0/*regparms*/,
                          "amd64g_dirtyhelper_SxDT",
                          &amd64g_dirtyhelper_SxDT,
                          mkIRExprVec_2( mkexpr(addr),
                                         mkU64(gregLO3ofRM(modrm)) )
                      );
         /* declare we're writing memory */
         d->mFx   = Ifx_Write;
         d->mAddr = mkexpr(addr);
         d->mSize = 6;
         stmt( IRStmt_Dirty(d) );
         return delta;
      }
      /* 0F 01 D0 = XGETBV */
      if (modrm == 0xD0 && (archinfo->hwcaps & VEX_HWCAPS_AMD64_AVX)) {
         delta += 1;
         DIP("xgetbv\n");
         /* Fault (SEGV) if ECX isn't zero.  Intel docs say #GP and I
            am not sure if that translates in to SEGV or to something
            else, in user space. */
         t1 = newTemp(Ity_I32);
         assign( t1, getIReg32(R_RCX) );
         stmt( IRStmt_Exit(binop(Iop_CmpNE32, mkexpr(t1), mkU32(0)),
                           Ijk_SigSEGV,
                           IRConst_U64(guest_RIP_curr_instr),
                           OFFB_RIP
         ));
         putIRegRAX(4, mkU32(7));
         putIRegRDX(4, mkU32(0));
         return delta;
      }
      /* else decode failed */
      break;
   }

   case 0x05: /* SYSCALL */
      guest_RIP_next_mustcheck = True;
      guest_RIP_next_assumed = guest_RIP_bbstart + delta;
      putIReg64( R_RCX, mkU64(guest_RIP_next_assumed) );
      /* It's important that all guest state is up-to-date
         at this point.  So we declare an end-of-block here, which
         forces any cached guest state to be flushed. */
      jmp_lit(dres, Ijk_Sys_syscall, guest_RIP_next_assumed);
      vassert(dres->whatNext == Dis_StopHere);
      DIP("syscall\n");
      return delta;

   case 0x1F:
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      if (epartIsReg(modrm)) goto decode_failure;
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      delta += alen;
      DIP("nop%c %s\n", nameISize(sz), dis_buf);
      return delta;

   case 0x31: { /* RDTSC */
      IRTemp   val  = newTemp(Ity_I64);
      IRExpr** args = mkIRExprVec_0();
      IRDirty* d    = unsafeIRDirty_1_N ( 
                         val, 
                         0/*regparms*/, 
                         "amd64g_dirtyhelper_RDTSC", 
                         &amd64g_dirtyhelper_RDTSC, 
                         args 
                      );
      if (have66orF2orF3(pfx)) goto decode_failure;
      /* execute the dirty call, dumping the result in val. */
      stmt( IRStmt_Dirty(d) );
      putIRegRDX(4, unop(Iop_64HIto32, mkexpr(val)));
      putIRegRAX(4, unop(Iop_64to32, mkexpr(val)));
      DIP("rdtsc\n");
      return delta;
   }

   case 0x40:
   case 0x41:
   case 0x42: /* CMOVBb/CMOVNAEb (cmov below) */
   case 0x43: /* CMOVNBb/CMOVAEb (cmov not below) */
   case 0x44: /* CMOVZb/CMOVEb (cmov zero) */
   case 0x45: /* CMOVNZb/CMOVNEb (cmov not zero) */
   case 0x46: /* CMOVBEb/CMOVNAb (cmov below or equal) */
   case 0x47: /* CMOVNBEb/CMOVAb (cmov not below or equal) */
   case 0x48: /* CMOVSb (cmov negative) */
   case 0x49: /* CMOVSb (cmov not negative) */
   case 0x4A: /* CMOVP (cmov parity even) */
   case 0x4B: /* CMOVNP (cmov parity odd) */
   case 0x4C: /* CMOVLb/CMOVNGEb (cmov less) */
   case 0x4D: /* CMOVGEb/CMOVNLb (cmov greater or equal) */
   case 0x4E: /* CMOVLEb/CMOVNGb (cmov less or equal) */
   case 0x4F: /* CMOVGb/CMOVNLEb (cmov greater) */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_cmov_E_G(vbi, pfx, sz, (AMD64Condcode)(opc - 0x40), delta);
      return delta;

   case 0x80:
   case 0x81:
   case 0x82:   /* JBb/JNAEb (jump below) */
   case 0x83:   /* JNBb/JAEb (jump not below) */
   case 0x84:   /* JZb/JEb (jump zero) */
   case 0x85:   /* JNZb/JNEb (jump not zero) */
   case 0x86:   /* JBEb/JNAb (jump below or equal) */
   case 0x87:   /* JNBEb/JAb (jump not below or equal) */
   case 0x88:   /* JSb (jump negative) */
   case 0x89:   /* JSb (jump not negative) */
   case 0x8A:   /* JP (jump parity even) */
   case 0x8B:   /* JNP/JPO (jump parity odd) */
   case 0x8C:   /* JLb/JNGEb (jump less) */
   case 0x8D:   /* JGEb/JNLb (jump greater or equal) */
   case 0x8E:   /* JLEb/JNGb (jump less or equal) */
   case 0x8F: { /* JGb/JNLEb (jump greater) */
      Long   jmpDelta;
      HChar* comment  = "";
      if (haveF2orF3(pfx)) goto decode_failure;
      jmpDelta = getSDisp32(delta);
      d64 = (guest_RIP_bbstart+delta+4) + jmpDelta;
      delta += 4;
      if (resteerCisOk
          && vex_control.guest_chase_cond
          && (Addr64)d64 != (Addr64)guest_RIP_bbstart
          && jmpDelta < 0
          && resteerOkFn( callback_opaque, d64) ) {
         /* Speculation: assume this backward branch is taken.  So
            we need to emit a side-exit to the insn following this
            one, on the negation of the condition, and continue at
            the branch target address (d64).  If we wind up back at
            the first instruction of the trace, just stop; it's
            better to let the IR loop unroller handle that case. */
         stmt( IRStmt_Exit( 
                  mk_amd64g_calculate_condition(
                     (AMD64Condcode)(1 ^ (opc - 0x80))),
                  Ijk_Boring,
                  IRConst_U64(guest_RIP_bbstart+delta),
                  OFFB_RIP
             ));
         dres->whatNext   = Dis_ResteerC;
         dres->continueAt = d64;
         comment = "(assumed taken)";
      }
      else
      if (resteerCisOk
          && vex_control.guest_chase_cond
          && (Addr64)d64 != (Addr64)guest_RIP_bbstart
          && jmpDelta >= 0
          && resteerOkFn( callback_opaque, guest_RIP_bbstart+delta ) ) {
         /* Speculation: assume this forward branch is not taken.
            So we need to emit a side-exit to d64 (the dest) and
            continue disassembling at the insn immediately
            following this one. */
         stmt( IRStmt_Exit( 
                  mk_amd64g_calculate_condition((AMD64Condcode)
                                                (opc - 0x80)),
                  Ijk_Boring,
                  IRConst_U64(d64),
                  OFFB_RIP
             ));
         dres->whatNext   = Dis_ResteerC;
         dres->continueAt = guest_RIP_bbstart+delta;
         comment = "(assumed not taken)";
      }
      else {
         /* Conservative default translation - end the block at
            this point. */
         jcc_01( dres, (AMD64Condcode)(opc - 0x80),
                 guest_RIP_bbstart+delta, d64 );
         vassert(dres->whatNext == Dis_StopHere);
      }
      DIP("j%s-32 0x%llx %s\n", name_AMD64Condcode(opc - 0x80), d64, comment);
      return delta;
   }

   case 0x90:
   case 0x91:
   case 0x92: /* set-Bb/set-NAEb (set if below) */
   case 0x93: /* set-NBb/set-AEb (set if not below) */
   case 0x94: /* set-Zb/set-Eb (set if zero) */
   case 0x95: /* set-NZb/set-NEb (set if not zero) */
   case 0x96: /* set-BEb/set-NAb (set if below or equal) */
   case 0x97: /* set-NBEb/set-Ab (set if not below or equal) */
   case 0x98: /* set-Sb (set if negative) */
   case 0x99: /* set-Sb (set if not negative) */
   case 0x9A: /* set-P (set if parity even) */
   case 0x9B: /* set-NP (set if parity odd) */
   case 0x9C: /* set-Lb/set-NGEb (set if less) */
   case 0x9D: /* set-GEb/set-NLb (set if greater or equal) */
   case 0x9E: /* set-LEb/set-NGb (set if less or equal) */
   case 0x9F: /* set-Gb/set-NLEb (set if greater) */
      if (haveF2orF3(pfx)) goto decode_failure;
      t1 = newTemp(Ity_I8);
      assign( t1, unop(Iop_1Uto8,mk_amd64g_calculate_condition(opc-0x90)) );
      modrm = getUChar(delta);
      if (epartIsReg(modrm)) {
         delta++;
         putIRegE(1, pfx, modrm, mkexpr(t1));
         DIP("set%s %s\n", name_AMD64Condcode(opc-0x90), 
                           nameIRegE(1,pfx,modrm));
      } else {
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         storeLE( mkexpr(addr), mkexpr(t1) );
         DIP("set%s %s\n", name_AMD64Condcode(opc-0x90), dis_buf);
      }
      return delta;

   case 0xA2: { /* CPUID */
      /* Uses dirty helper: 
            void amd64g_dirtyhelper_CPUID ( VexGuestAMD64State* )
         declared to mod rax, wr rbx, rcx, rdx
      */
      IRDirty* d     = NULL;
      HChar*   fName = NULL;
      void*    fAddr = NULL;
      if (haveF2orF3(pfx)) goto decode_failure;
      if (0 && /* Can't enable this until AVX support is complete. */
          archinfo->hwcaps == (VEX_HWCAPS_AMD64_SSE3
                               |VEX_HWCAPS_AMD64_CX16 
                               |VEX_HWCAPS_AMD64_AVX)) {
         fName = "amd64g_dirtyhelper_CPUID_avx_and_cx16";
         fAddr = &amd64g_dirtyhelper_CPUID_avx_and_cx16;
         /* This is a Core-i5-2300-like machine */
      }
      else if (archinfo->hwcaps == (VEX_HWCAPS_AMD64_SSE3
                                    |VEX_HWCAPS_AMD64_CX16)) {
         fName = "amd64g_dirtyhelper_CPUID_sse42_and_cx16";
         fAddr = &amd64g_dirtyhelper_CPUID_sse42_and_cx16;
         /* This is a Core-i5-670-like machine */
      }
      else {
         /* Give a CPUID for at least a baseline machine, SSE2
            only, and no CX16 */
         fName = "amd64g_dirtyhelper_CPUID_baseline";
         fAddr = &amd64g_dirtyhelper_CPUID_baseline;
      }

      vassert(fName); vassert(fAddr);
      d = unsafeIRDirty_0_N ( 0/*regparms*/, 
                              fName, fAddr, mkIRExprVec_0() );
      /* declare guest state effects */
      d->needsBBP = True;
      d->nFxState = 4;
      vex_bzero(&d->fxState, sizeof(d->fxState));
      d->fxState[0].fx     = Ifx_Modify;
      d->fxState[0].offset = OFFB_RAX;
      d->fxState[0].size   = 8;
      d->fxState[1].fx     = Ifx_Write;
      d->fxState[1].offset = OFFB_RBX;
      d->fxState[1].size   = 8;
      d->fxState[2].fx     = Ifx_Modify;
      d->fxState[2].offset = OFFB_RCX;
      d->fxState[2].size   = 8;
      d->fxState[3].fx     = Ifx_Write;
      d->fxState[3].offset = OFFB_RDX;
      d->fxState[3].size   = 8;
      /* execute the dirty call, side-effecting guest state */
      stmt( IRStmt_Dirty(d) );
      /* CPUID is a serialising insn.  So, just in case someone is
         using it as a memory fence ... */
      stmt( IRStmt_MBE(Imbe_Fence) );
      DIP("cpuid\n");
      return delta;
   }

   case 0xA3: /* BT Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 8 && sz != 4 && sz != 2) goto decode_failure;
      delta = dis_bt_G_E ( vbi, pfx, sz, delta, BtOpNone );
      return delta;

   case 0xA4: /* SHLDv imm8,Gv,Ev */
      modrm = getUChar(delta);
      d64   = delta + lengthAMode(pfx, delta);
      vex_sprintf(dis_buf, "$%d", (Int)getUChar(d64));
      delta = dis_SHLRD_Gv_Ev ( 
                 vbi, pfx, delta, modrm, sz, 
                 mkU8(getUChar(d64)), True, /* literal */
                 dis_buf, True /* left */ );
      return delta;

   case 0xA5: /* SHLDv %cl,Gv,Ev */
      modrm = getUChar(delta);
      delta = dis_SHLRD_Gv_Ev ( 
                 vbi, pfx, delta, modrm, sz,
                 getIRegCL(), False, /* not literal */
                 "%cl", True /* left */ );
      return delta;

   case 0xAB: /* BTS Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 8 && sz != 4 && sz != 2) goto decode_failure;
      delta = dis_bt_G_E ( vbi, pfx, sz, delta, BtOpSet );
      return delta;

   case 0xAC: /* SHRDv imm8,Gv,Ev */
      modrm = getUChar(delta);
      d64   = delta + lengthAMode(pfx, delta);
      vex_sprintf(dis_buf, "$%d", (Int)getUChar(d64));
      delta = dis_SHLRD_Gv_Ev ( 
                 vbi, pfx, delta, modrm, sz, 
                 mkU8(getUChar(d64)), True, /* literal */
                 dis_buf, False /* right */ );
      return delta;

   case 0xAD: /* SHRDv %cl,Gv,Ev */
      modrm = getUChar(delta);
      delta = dis_SHLRD_Gv_Ev ( 
                 vbi, pfx, delta, modrm, sz, 
                 getIRegCL(), False, /* not literal */
                 "%cl", False /* right */);
      return delta;

   case 0xAF: /* IMUL Ev, Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_mul_E_G ( vbi, pfx, sz, delta );
      return delta;

   case 0xB1: { /* CMPXCHG Gv,Ev (allowed in 16,32,64 bit) */
      Bool ok = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 2 && sz != 4 && sz != 8) goto decode_failure;
      delta = dis_cmpxchg_G_E ( &ok, vbi, pfx, sz, delta );
      if (!ok) goto decode_failure;
      return delta;
   }

   case 0xB0: { /* CMPXCHG Gb,Eb */
      Bool ok = True;
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_cmpxchg_G_E ( &ok, vbi, pfx, 1, delta );
      if (!ok) goto decode_failure;
      return delta;
   }

   case 0xB3: /* BTR Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 8 && sz != 4 && sz != 2) goto decode_failure;
      delta = dis_bt_G_E ( vbi, pfx, sz, delta, BtOpReset );
      return delta;

   case 0xB6: /* MOVZXb Eb,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 2 && sz != 4 && sz != 8)
         goto decode_failure;
      delta = dis_movx_E_G ( vbi, pfx, delta, 1, sz, False );
      return delta;

   case 0xB7: /* MOVZXw Ew,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 4 && sz != 8)
         goto decode_failure;
      delta = dis_movx_E_G ( vbi, pfx, delta, 2, sz, False );
      return delta;

   case 0xBA: { /* Grp8 Ib,Ev */
      Bool decode_OK = False;
      if (haveF2orF3(pfx)) goto decode_failure;
      modrm = getUChar(delta);
      am_sz = lengthAMode(pfx,delta);
      d64   = getSDisp8(delta + am_sz);
      delta = dis_Grp8_Imm ( vbi, pfx, delta, modrm, am_sz, sz, d64,
                             &decode_OK );
      if (!decode_OK)
         goto decode_failure;
      return delta;
   }

   case 0xBB: /* BTC Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 8 && sz != 4 && sz != 2) goto decode_failure;
      delta = dis_bt_G_E ( vbi, pfx, sz, delta, BtOpComp );
      return delta;

   case 0xBC: /* BSF Gv,Ev */
      if (haveF2orF3(pfx)) goto decode_failure;
      delta = dis_bs_E_G ( vbi, pfx, sz, delta, True );
      return delta;

   case 0xBD: /* BSR Gv,Ev */
      if (!haveF2orF3(pfx)) {
         /* no-F2 no-F3 0F BD = BSR */
         delta = dis_bs_E_G ( vbi, pfx, sz, delta, False );
         return delta;
      }
      /* Fall through, since F3 0F BD is LZCNT, and needs to
         be handled by dis_ESC_0F__SSE4. */
      break;

   case 0xBE: /* MOVSXb Eb,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 2 && sz != 4 && sz != 8)
         goto decode_failure;
      delta = dis_movx_E_G ( vbi, pfx, delta, 1, sz, True );
      return delta;

   case 0xBF: /* MOVSXw Ew,Gv */
      if (haveF2orF3(pfx)) goto decode_failure;
      if (sz != 4 && sz != 8)
         goto decode_failure;
      delta = dis_movx_E_G ( vbi, pfx, delta, 2, sz, True );
      return delta;

   case 0xC1: { /* XADD Gv,Ev */ 
      Bool decode_OK = False;
      delta = dis_xadd_G_E ( &decode_OK, vbi, pfx, sz, delta );
      if (!decode_OK)
         goto decode_failure;
      return delta;
   }

   case 0xC7: { /* CMPXCHG8B Ev, CMPXCHG16B Ev */
      IRType  elemTy     = sz==4 ? Ity_I32 : Ity_I64;
      IRTemp  expdHi     = newTemp(elemTy);
      IRTemp  expdLo     = newTemp(elemTy);
      IRTemp  dataHi     = newTemp(elemTy);
      IRTemp  dataLo     = newTemp(elemTy);
      IRTemp  oldHi      = newTemp(elemTy);
      IRTemp  oldLo      = newTemp(elemTy);
      IRTemp  flags_old  = newTemp(Ity_I64);
      IRTemp  flags_new  = newTemp(Ity_I64);
      IRTemp  success    = newTemp(Ity_I1);
      IROp    opOR       = sz==4 ? Iop_Or32    : Iop_Or64;
      IROp    opXOR      = sz==4 ? Iop_Xor32   : Iop_Xor64;
      IROp    opCasCmpEQ = sz==4 ? Iop_CasCmpEQ32 : Iop_CasCmpEQ64;
      IRExpr* zero       = sz==4 ? mkU32(0)    : mkU64(0);
      IRTemp expdHi64    = newTemp(Ity_I64);
      IRTemp expdLo64    = newTemp(Ity_I64);

      /* Translate this using a DCAS, even if there is no LOCK
         prefix.  Life is too short to bother with generating two
         different translations for the with/without-LOCK-prefix
         cases. */
      *expect_CAS = True;

      /* Decode, and generate address. */
      if (have66orF2orF3(pfx)) goto decode_failure;
      if (sz != 4 && sz != 8) goto decode_failure;
      if (sz == 8 && !(archinfo->hwcaps & VEX_HWCAPS_AMD64_CX16))
         goto decode_failure;
      modrm = getUChar(delta);
      if (epartIsReg(modrm)) goto decode_failure;
      if (gregLO3ofRM(modrm) != 1) goto decode_failure;
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      delta += alen;

      /* cmpxchg16b requires an alignment check. */
      if (sz == 8)
         gen_SEGV_if_not_16_aligned( addr );

      /* Get the expected and new values. */
      assign( expdHi64, getIReg64(R_RDX) );
      assign( expdLo64, getIReg64(R_RAX) );

      /* These are the correctly-sized expected and new values.
         However, we also get expdHi64/expdLo64 above as 64-bits
         regardless, because we will need them later in the 32-bit
         case (paradoxically). */
      assign( expdHi, sz==4 ? unop(Iop_64to32, mkexpr(expdHi64))
                            : mkexpr(expdHi64) );
      assign( expdLo, sz==4 ? unop(Iop_64to32, mkexpr(expdLo64))
                            : mkexpr(expdLo64) );
      assign( dataHi, sz==4 ? getIReg32(R_RCX) : getIReg64(R_RCX) );
      assign( dataLo, sz==4 ? getIReg32(R_RBX) : getIReg64(R_RBX) );

      /* Do the DCAS */
      stmt( IRStmt_CAS(
               mkIRCAS( oldHi, oldLo, 
                        Iend_LE, mkexpr(addr), 
                        mkexpr(expdHi), mkexpr(expdLo),
                        mkexpr(dataHi), mkexpr(dataLo)
            )));

      /* success when oldHi:oldLo == expdHi:expdLo */
      assign( success,
              binop(opCasCmpEQ,
                    binop(opOR,
                          binop(opXOR, mkexpr(oldHi), mkexpr(expdHi)),
                          binop(opXOR, mkexpr(oldLo), mkexpr(expdLo))
                    ),
                    zero
              ));

      /* If the DCAS is successful, that is to say oldHi:oldLo ==
         expdHi:expdLo, then put expdHi:expdLo back in RDX:RAX,
         which is where they came from originally.  Both the actual
         contents of these two regs, and any shadow values, are
         unchanged.  If the DCAS fails then we're putting into
         RDX:RAX the value seen in memory. */
      /* Now of course there's a complication in the 32-bit case
         (bah!): if the DCAS succeeds, we need to leave RDX:RAX
         unchanged; but if we use the same scheme as in the 64-bit
         case, we get hit by the standard rule that a write to the
         bottom 32 bits of an integer register zeros the upper 32
         bits.  And so the upper halves of RDX and RAX mysteriously
         become zero.  So we have to stuff back in the original
         64-bit values which we previously stashed in
         expdHi64:expdLo64, even if we're doing a cmpxchg8b. */
      /* It's just _so_ much fun ... */
      putIRegRDX( 8,
                  IRExpr_Mux0X( unop(Iop_1Uto8, mkexpr(success)),
                                sz == 4 ? unop(Iop_32Uto64, mkexpr(oldHi))
                                        : mkexpr(oldHi),
                                mkexpr(expdHi64)
                ));
      putIRegRAX( 8,
                  IRExpr_Mux0X( unop(Iop_1Uto8, mkexpr(success)),
                                sz == 4 ? unop(Iop_32Uto64, mkexpr(oldLo))
                                        : mkexpr(oldLo),
                                mkexpr(expdLo64)
                ));

      /* Copy the success bit into the Z flag and leave the others
         unchanged */
      assign( flags_old, widenUto64(mk_amd64g_calculate_rflags_all()));
      assign( 
         flags_new,
         binop(Iop_Or64,
               binop(Iop_And64, mkexpr(flags_old), 
                                mkU64(~AMD64G_CC_MASK_Z)),
               binop(Iop_Shl64,
                     binop(Iop_And64,
                           unop(Iop_1Uto64, mkexpr(success)), mkU64(1)), 
                     mkU8(AMD64G_CC_SHIFT_Z)) ));

      stmt( IRStmt_Put( OFFB_CC_OP,   mkU64(AMD64G_CC_OP_COPY) ));
      stmt( IRStmt_Put( OFFB_CC_DEP1, mkexpr(flags_new) ));
      stmt( IRStmt_Put( OFFB_CC_DEP2, mkU64(0) ));
      /* Set NDEP even though it isn't used.  This makes
         redundant-PUT elimination of previous stores to this field
         work better. */
      stmt( IRStmt_Put( OFFB_CC_NDEP, mkU64(0) ));

      /* Sheesh.  Aren't you glad it was me and not you that had to
         write and validate all this grunge? */

      DIP("cmpxchg8b %s\n", dis_buf);
      return delta;
   }

   case 0xC8: /* BSWAP %eax */
   case 0xC9:
   case 0xCA:
   case 0xCB:
   case 0xCC:
   case 0xCD:
   case 0xCE:
   case 0xCF: /* BSWAP %edi */
      if (haveF2orF3(pfx)) goto decode_failure;
      /* According to the AMD64 docs, this insn can have size 4 or
         8. */
      if (sz == 4) {
         t1 = newTemp(Ity_I32);
         t2 = newTemp(Ity_I32);
         assign( t1, getIRegRexB(4, pfx, opc-0xC8) );
         assign( t2,
            binop(Iop_Or32,
               binop(Iop_Shl32, mkexpr(t1), mkU8(24)),
            binop(Iop_Or32,
               binop(Iop_And32, binop(Iop_Shl32, mkexpr(t1), mkU8(8)),
                                mkU32(0x00FF0000)),
            binop(Iop_Or32,
               binop(Iop_And32, binop(Iop_Shr32, mkexpr(t1), mkU8(8)),
                                mkU32(0x0000FF00)),
               binop(Iop_And32, binop(Iop_Shr32, mkexpr(t1), mkU8(24)),
                                mkU32(0x000000FF) )
            )))
         );
         putIRegRexB(4, pfx, opc-0xC8, mkexpr(t2));
         DIP("bswapl %s\n", nameIRegRexB(4, pfx, opc-0xC8));
         return delta;
      }
      if (sz == 8) {
         IRTemp m8  = newTemp(Ity_I64);
         IRTemp s8  = newTemp(Ity_I64);
         IRTemp m16 = newTemp(Ity_I64);
         IRTemp s16 = newTemp(Ity_I64);
         IRTemp m32 = newTemp(Ity_I64);
         t1 = newTemp(Ity_I64);
         t2 = newTemp(Ity_I64);
         assign( t1, getIRegRexB(8, pfx, opc-0xC8) );

         assign( m8, mkU64(0xFF00FF00FF00FF00ULL) );
         assign( s8,
                 binop(Iop_Or64,
                       binop(Iop_Shr64,
                             binop(Iop_And64,mkexpr(t1),mkexpr(m8)),
                             mkU8(8)),
                       binop(Iop_And64,
                             binop(Iop_Shl64,mkexpr(t1),mkU8(8)),
                             mkexpr(m8))
                      ) 
               );

         assign( m16, mkU64(0xFFFF0000FFFF0000ULL) );
         assign( s16,
                 binop(Iop_Or64,
                       binop(Iop_Shr64,
                             binop(Iop_And64,mkexpr(s8),mkexpr(m16)),
                             mkU8(16)),
                       binop(Iop_And64,
                             binop(Iop_Shl64,mkexpr(s8),mkU8(16)),
                             mkexpr(m16))
                      ) 
               );

         assign( m32, mkU64(0xFFFFFFFF00000000ULL) );
         assign( t2,
                 binop(Iop_Or64,
                       binop(Iop_Shr64,
                             binop(Iop_And64,mkexpr(s16),mkexpr(m32)),
                             mkU8(32)),
                       binop(Iop_And64,
                             binop(Iop_Shl64,mkexpr(s16),mkU8(32)),
                             mkexpr(m32))
                      ) 
               );

         putIRegRexB(8, pfx, opc-0xC8, mkexpr(t2));
         DIP("bswapq %s\n", nameIRegRexB(8, pfx, opc-0xC8));
         return delta;
      }
      goto decode_failure;

   default:
      break;

   } /* first switch */


   /* =-=-=-=-=-=-=-=-= MMXery =-=-=-=-=-=-=-=-= */
   /* In the second switch, pick off MMX insns. */

   if (!have66orF2orF3(pfx)) {
      /* So there's no SIMD prefix. */

      vassert(sz == 4 || sz == 8);

      switch (opc) { /* second switch */

      case 0x71: 
      case 0x72: 
      case 0x73: /* PSLLgg/PSRAgg/PSRLgg mmxreg by imm8 */

      case 0x6E: /* MOVD (src)ireg-or-mem, (dst)mmxreg */
      case 0x7E: /* MOVD (src)mmxreg, (dst)ireg-or-mem */
      case 0x7F: /* MOVQ (src)mmxreg, (dst)mmxreg-or-mem */
      case 0x6F: /* MOVQ (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xFC: 
      case 0xFD: 
      case 0xFE: /* PADDgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xEC: 
      case 0xED: /* PADDSgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xDC:
      case 0xDD: /* PADDUSgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xF8: 
      case 0xF9: 
      case 0xFA: /* PSUBgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xE8: 
      case 0xE9: /* PSUBSgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xD8: 
      case 0xD9: /* PSUBUSgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xE5: /* PMULHW (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xD5: /* PMULLW (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xF5: /* PMADDWD (src)mmxreg-or-mem, (dst)mmxreg */

      case 0x74: 
      case 0x75: 
      case 0x76: /* PCMPEQgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0x64: 
      case 0x65: 
      case 0x66: /* PCMPGTgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0x6B: /* PACKSSDW (src)mmxreg-or-mem, (dst)mmxreg */
      case 0x63: /* PACKSSWB (src)mmxreg-or-mem, (dst)mmxreg */
      case 0x67: /* PACKUSWB (src)mmxreg-or-mem, (dst)mmxreg */

      case 0x68: 
      case 0x69: 
      case 0x6A: /* PUNPCKHgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0x60: 
      case 0x61: 
      case 0x62: /* PUNPCKLgg (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xDB: /* PAND (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xDF: /* PANDN (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xEB: /* POR (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xEF: /* PXOR (src)mmxreg-or-mem, (dst)mmxreg */

      case 0xF1: /* PSLLgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xF2: 
      case 0xF3: 

      case 0xD1: /* PSRLgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xD2: 
      case 0xD3: 

      case 0xE1: /* PSRAgg (src)mmxreg-or-mem, (dst)mmxreg */
      case 0xE2: { 
         Bool decode_OK = False;
         delta = dis_MMX ( &decode_OK, vbi, pfx, sz, deltaIN );
         if (decode_OK)
            return delta;
         goto decode_failure;
      }

      default:
         break;
      } /* second switch */

   }

   /* A couple of MMX corner cases */
   if (opc == 0x0E/* FEMMS */ || opc == 0x77/* EMMS */) {
      if (sz != 4)
         goto decode_failure;
      do_EMMS_preamble();
      DIP("{f}emms\n");
      return delta;
   }

   /* =-=-=-=-=-=-=-=-= SSE2ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's an SSE or SSE2 instruction.  We can try this
      without checking the guest hwcaps because SSE2 is a baseline
      facility in 64 bit mode. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F__SSE2 ( &decode_OK, vbi, pfx, sz, deltaIN, dres );
      if (decode_OK)
         return delta;
   }

   /* =-=-=-=-=-=-=-=-= SSE3ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's a SSE3 instruction.  FIXME: check guest hwcaps
      first. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F__SSE3 ( &decode_OK, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

   /* =-=-=-=-=-=-=-=-= SSE4ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's a SSE4 instruction.  FIXME: check guest hwcaps
      first. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F__SSE4 ( &decode_OK,
                                 archinfo, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

  decode_failure:
   return deltaIN; /* fail */
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F38         ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_0F38 (
        /*MB_OUT*/DisResult* dres,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   default:
      break;

   }

   /* =-=-=-=-=-=-=-=-= SSSE3ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's an SSSE3 instruction.  FIXME: consult guest hwcaps
      rather than proceeding indiscriminately. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F38__SupSSE3 ( &decode_OK, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

   /* =-=-=-=-=-=-=-=-= SSE4ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's an SSE4 instruction.  FIXME: consult guest hwcaps
      rather than proceeding indiscriminately. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F38__SSE4 ( &decode_OK, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

  //decode_failure:
   return deltaIN; /* fail */
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F3A         ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

__attribute__((noinline))
static
Long dis_ESC_0F3A (
        /*MB_OUT*/DisResult* dres,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   switch (opc) {

   default:
      break;

   }

   /* =-=-=-=-=-=-=-=-= SSSE3ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's an SSSE3 instruction.  FIXME: consult guest hwcaps
      rather than proceeding indiscriminately. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F3A__SupSSE3 ( &decode_OK, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

   /* =-=-=-=-=-=-=-=-= SSE4ery =-=-=-=-=-=-=-=-= */
   /* Perhaps it's an SSE4 instruction.  FIXME: consult guest hwcaps
      rather than proceeding indiscriminately. */
   {
      Bool decode_OK = False;
      delta = dis_ESC_0F3A__SSE4 ( &decode_OK, vbi, pfx, sz, deltaIN );
      if (decode_OK)
         return delta;
   }

   return deltaIN; /* fail */
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F__VEX      ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

/* FIXME: common up with the _256_ version below? */
static
Long dis_VEX_NDS_128_AnySimdPfx_0F_WIG (
        /*OUT*/Bool* uses_vvvv, VexAbiInfo* vbi,
        Prefix pfx, Long delta, HChar* name,
        /* The actual operation.  Use either 'op' or 'opfn',
           but not both. */
        IROp op, IRTemp(*opFn)(IRTemp,IRTemp),
        Bool invertLeftArg,
        Bool swapArgs
     )
{
   UChar  modrm = getUChar(delta);
   UInt   rD    = gregOfRexRM(pfx, modrm);
   UInt   rSL   = getVexNvvvv(pfx);
   IRTemp tSL   = newTemp(Ity_V128);
   IRTemp tSR   = newTemp(Ity_V128);
   IRTemp addr  = IRTemp_INVALID;
   HChar  dis_buf[50];
   Int    alen  = 0;
   vassert(0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*WIG?*/);

   assign(tSL, invertLeftArg ? unop(Iop_NotV128, getXMMReg(rSL))
                             : getXMMReg(rSL));

   if (epartIsReg(modrm)) {
      UInt rSR = eregOfRexRM(pfx, modrm);
      delta += 1;
      assign(tSR, getXMMReg(rSR));
      DIP("%s %s,%s,%s\n",
          name, nameXMMReg(rSR), nameXMMReg(rSL), nameXMMReg(rD));
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      delta += alen;
      assign(tSR, loadLE(Ity_V128, mkexpr(addr)));
      DIP("%s %s,%s,%s\n",
          name, dis_buf, nameXMMReg(rSL), nameXMMReg(rD));
   }

   IRTemp res = IRTemp_INVALID;
   if (op != Iop_INVALID) {
      vassert(opFn == NULL);
      res = newTemp(Ity_V128);
      assign(res, swapArgs ? binop(op, mkexpr(tSR), mkexpr(tSL))
                           : binop(op, mkexpr(tSL), mkexpr(tSR)));
   } else {
      vassert(opFn != NULL);
      res = swapArgs ? opFn(tSR, tSL) : opFn(tSL, tSR);
   }

   putYMMRegLoAndZU(rD, mkexpr(res));

   *uses_vvvv = True;
   return delta;
}


/* Handle a VEX_NDS_128_66_0F_WIG (3-addr) insn, with a simple IROp
   for the operation, no inversion of the left arg, and no swapping of
   args. */
static
Long dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple (
        /*OUT*/Bool* uses_vvvv, VexAbiInfo* vbi,
        Prefix pfx, Long delta, HChar* name,
        IROp op
     )
{
   return dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
             uses_vvvv, vbi, pfx, delta, name, op, NULL, False, False);
}


/* Handle a VEX_NDS_128_66_0F_WIG (3-addr) insn, using the given IR
   generator to compute the result, no inversion of the left
   arg, and no swapping of args. */
static
Long dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex (
        /*OUT*/Bool* uses_vvvv, VexAbiInfo* vbi,
        Prefix pfx, Long delta, HChar* name,
        IRTemp(*opFn)(IRTemp,IRTemp)
     )
{
   return dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
             uses_vvvv, vbi, pfx, delta, name,
             Iop_INVALID, opFn, False, False );
}


/* Vector by scalar shift of V by the amount specified at the bottom
   of E. */
static ULong dis_AVX128_shiftV_byE ( VexAbiInfo* vbi,
                                     Prefix pfx, Long delta, 
                                     HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen, size;
   IRTemp  addr;
   Bool    shl, shr, sar;
   UChar   modrm = getUChar(delta);
   UInt    rG    = gregOfRexRM(pfx,modrm);
   UInt    rV    = getVexNvvvv(pfx);;
   IRTemp  g0    = newTemp(Ity_V128);
   IRTemp  g1    = newTemp(Ity_V128);
   IRTemp  amt   = newTemp(Ity_I64);
   IRTemp  amt8  = newTemp(Ity_I8);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( amt, getXMMRegLane64(rE, 0) );
      DIP("%s %s,%s,%s\n", opname, nameXMMReg(rE),
          nameXMMReg(rV), nameXMMReg(rG) );
      delta++;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( amt, loadLE(Ity_I64, mkexpr(addr)) );
      DIP("%s %s,%s,%s\n", opname, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
      delta += alen;
   }
   assign( g0, getXMMReg(rV) );
   assign( amt8, unop(Iop_64to8, mkexpr(amt)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x8: shl = True; size = 32; break;
      case Iop_ShlN32x4: shl = True; size = 32; break;
      case Iop_ShlN64x2: shl = True; size = 64; break;
      case Iop_SarN16x8: sar = True; size = 16; break;
      case Iop_SarN32x4: sar = True; size = 32; break;
      case Iop_ShrN16x8: shr = True; size = 16; break;
      case Iop_ShrN32x4: shr = True; size = 32; break;
      case Iop_ShrN64x2: shr = True; size = 64; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,
                binop(Iop_CmpLT64U, mkexpr(amt), mkU64(size))),
           mkV128(0x0000),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else 
   if (sar) {
     assign( 
        g1,
        IRExpr_Mux0X(
           unop(Iop_1Uto8,
                binop(Iop_CmpLT64U, mkexpr(amt), mkU64(size))),
           binop(op, mkexpr(g0), mkU8(size-1)),
           binop(op, mkexpr(g0), mkexpr(amt8))
        )
     );
   } else {
      vassert(0);
   }

   putYMMRegLoAndZU( rG, mkexpr(g1) );
   return delta;
}


/* Vector by scalar shift of E into V, by an immediate byte.  Modified
   version of dis_SSE_shiftE_imm. */
static
Long dis_AVX128_shiftE_to_V_imm( Prefix pfx, 
                                 Long delta, HChar* opname, IROp op )
{
   Bool    shl, shr, sar;
   UChar   rm   = getUChar(delta);
   IRTemp  e0   = newTemp(Ity_V128);
   IRTemp  e1   = newTemp(Ity_V128);
   UInt    rD   = getVexNvvvv(pfx);
   UChar   amt, size;
   vassert(epartIsReg(rm));
   vassert(gregLO3ofRM(rm) == 2 
           || gregLO3ofRM(rm) == 4 || gregLO3ofRM(rm) == 6);
   amt = getUChar(delta+1);
   delta += 2;
   DIP("%s $%d,%s,%s\n", opname,
                         (Int)amt,
                         nameXMMReg(eregOfRexRM(pfx,rm)),
                         nameXMMReg(rD));
   assign( e0, getXMMReg(eregOfRexRM(pfx,rm)) );

   shl = shr = sar = False;
   size = 0;
   switch (op) {
      case Iop_ShlN16x8: shl = True; size = 16; break;
      case Iop_ShlN32x4: shl = True; size = 32; break;
      case Iop_ShlN64x2: shl = True; size = 64; break;
      case Iop_SarN16x8: sar = True; size = 16; break;
      case Iop_SarN32x4: sar = True; size = 32; break;
      case Iop_ShrN16x8: shr = True; size = 16; break;
      case Iop_ShrN32x4: shr = True; size = 32; break;
      case Iop_ShrN64x2: shr = True; size = 64; break;
      default: vassert(0);
   }

   if (shl || shr) {
     assign( e1, amt >= size 
                    ? mkV128(0x0000)
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else 
   if (sar) {
     assign( e1, amt >= size 
                    ? binop(op, mkexpr(e0), mkU8(size-1))
                    : binop(op, mkexpr(e0), mkU8(amt))
     );
   } else {
      vassert(0);
   }

   putYMMRegLoAndZU( rD, mkexpr(e1) );
   return delta;
}


/* Lower 64-bit lane only AVX128 binary operation:
   G[63:0]    = V[63:0] `op` E[63:0]
   G[127:64]  = V[127:64]
   G[255:128] = 0.
   The specified op must be of the 64F0x2 kind, so that it
   copies the upper half of the left operand to the result.
*/
static Long dis_AVX128_E_V_to_G_lo64 ( /*OUT*/Bool* uses_vvvv,
                                       VexAbiInfo* vbi,
                                       Prefix pfx, Long delta, 
                                       HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm    = getUChar(delta);
   UInt    rG    = gregOfRexRM(pfx,rm);
   UInt    rV    = getVexNvvvv(pfx);
   IRExpr* vpart = getXMMReg(rV);
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      putXMMReg( rG, binop(op, vpart, getXMMReg(rE)) );
      DIP("%s %s,%s,%s\n", opname,
          nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
      delta = delta+1;
   } else {
      /* We can only do a 64-bit memory read, so the upper half of the
         E operand needs to be made simply of zeroes. */
      IRTemp epart = newTemp(Ity_V128);
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( epart, unop( Iop_64UtoV128,
                           loadLE(Ity_I64, mkexpr(addr))) );
      putXMMReg( rG, binop(op, vpart, mkexpr(epart)) );
      DIP("%s %s,%s,%s\n", opname,
          dis_buf, nameXMMReg(rV), nameXMMReg(rG));
      delta = delta+alen;
   }
   putYMMRegLane128( rG, 1, mkV128(0) );
   *uses_vvvv = True;
   return delta;
}


/* Lower 64-bit lane only AVX128 unary operation:
   G[63:0]    = op(E[63:0])
   G[127:64]  = V[127:64]
   G[255:128] = 0
   The specified op must be of the 64F0x2 kind, so that it
   copies the upper half of the operand to the result.
*/
static Long dis_AVX128_E_V_to_G_lo64_unary ( /*OUT*/Bool* uses_vvvv,
                                             VexAbiInfo* vbi,
                                             Prefix pfx, Long delta, 
                                             HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm  = getUChar(delta);
   UInt    rG  = gregOfRexRM(pfx,rm);
   UInt    rV  = getVexNvvvv(pfx);
   IRTemp  e64 = newTemp(Ity_I64);

   /* Fetch E[63:0] */
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      assign(e64, getXMMRegLane64(rE, 0));
      DIP("%s %s,%s,%s\n", opname,
          nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
      delta += 1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(e64, loadLE(Ity_I64, mkexpr(addr)));
      DIP("%s %s,%s,%s\n", opname,
          dis_buf, nameXMMReg(rV), nameXMMReg(rG));
      delta += alen;
   }

   /* Create a value 'arg' as V[127:64]++E[63:0] */
   IRTemp arg = newTemp(Ity_V128);
   assign(arg,
          binop(Iop_SetV128lo64,
                getXMMReg(rV), mkexpr(e64)));
   /* and apply op to it */
   putYMMRegLoAndZU( rG, unop(op, mkexpr(arg)) );
   *uses_vvvv = True;
   return delta;
}


/* Lower 32-bit lane only AVX128 unary operation:
   G[31:0]    = op(E[31:0])
   G[127:32]  = V[127:32]
   G[255:128] = 0
   The specified op must be of the 32F0x4 kind, so that it
   copies the upper 3/4 of the operand to the result.
*/
static Long dis_AVX128_E_V_to_G_lo32_unary ( /*OUT*/Bool* uses_vvvv,
                                             VexAbiInfo* vbi,
                                             Prefix pfx, Long delta, 
                                             HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm  = getUChar(delta);
   UInt    rG  = gregOfRexRM(pfx,rm);
   UInt    rV  = getVexNvvvv(pfx);
   IRTemp  e32 = newTemp(Ity_I32);

   /* Fetch E[31:0] */
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      assign(e32, getXMMRegLane32(rE, 0));
      DIP("%s %s,%s,%s\n", opname,
          nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
      delta += 1;
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(e32, loadLE(Ity_I32, mkexpr(addr)));
      DIP("%s %s,%s,%s\n", opname,
          dis_buf, nameXMMReg(rV), nameXMMReg(rG));
      delta += alen;
   }

   /* Create a value 'arg' as V[127:32]++E[31:0] */
   IRTemp arg = newTemp(Ity_V128);
   assign(arg,
          binop(Iop_SetV128lo32,
                getXMMReg(rV), mkexpr(e32)));
   /* and apply op to it */
   putYMMRegLoAndZU( rG, unop(op, mkexpr(arg)) );
   *uses_vvvv = True;
   return delta;
}


/* Lower 32-bit lane only AVX128 binary operation:
   G[31:0]    = V[31:0] `op` E[31:0]
   G[127:32]  = V[127:32]
   G[255:128] = 0.
   The specified op must be of the 32F0x4 kind, so that it
   copies the upper 3/4 of the left operand to the result.
*/
static Long dis_AVX128_E_V_to_G_lo32 ( /*OUT*/Bool* uses_vvvv,
                                       VexAbiInfo* vbi,
                                       Prefix pfx, Long delta, 
                                       HChar* opname, IROp op )
{
   HChar   dis_buf[50];
   Int     alen;
   IRTemp  addr;
   UChar   rm    = getUChar(delta);
   UInt    rG    = gregOfRexRM(pfx,rm);
   UInt    rV    = getVexNvvvv(pfx);
   IRExpr* vpart = getXMMReg(rV);
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      putXMMReg( rG, binop(op, vpart, getXMMReg(rE)) );
      DIP("%s %s,%s,%s\n", opname,
          nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
      delta = delta+1;
   } else {
      /* We can only do a 32-bit memory read, so the upper 3/4 of the
         E operand needs to be made simply of zeroes. */
      IRTemp epart = newTemp(Ity_V128);
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( epart, unop( Iop_32UtoV128,
                           loadLE(Ity_I32, mkexpr(addr))) );
      putXMMReg( rG, binop(op, vpart, mkexpr(epart)) );
      DIP("%s %s,%s,%s\n", opname,
          dis_buf, nameXMMReg(rV), nameXMMReg(rG));
      delta = delta+alen;
   }
   putYMMRegLane128( rG, 1, mkV128(0) );
   *uses_vvvv = True;
   return delta;
}


/* All-lanes AVX128 binary operation:
   G[127:0]   = V[127:0] `op` E[127:0]
   G[255:128] = 0.
*/
static Long dis_AVX128_E_V_to_G ( /*OUT*/Bool* uses_vvvv,
                                  VexAbiInfo* vbi,
                                  Prefix pfx, Long delta, 
                                  HChar* opname, IROp op )
{
   return dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
             uses_vvvv, vbi, pfx, delta, opname, op,
             NULL, False/*!invertLeftArg*/, False/*!swapArgs*/
   );
}


/* Handles AVX128 32F/64F comparisons.  A derivative of
   dis_SSEcmp_E_to_G.  It can fail, in which case it returns the
   original delta to indicate failure. */
static
Long dis_AVX128_cmp_V_E_to_G ( /*OUT*/Bool* uses_vvvv,
                               VexAbiInfo* vbi,
                               Prefix pfx, Long delta, 
                               HChar* opname, Bool all_lanes, Int sz )
{
   vassert(sz == 4 || sz == 8);
   Long    deltaIN = delta;
   HChar   dis_buf[50];
   Int     alen;
   UInt    imm8;
   IRTemp  addr;
   Bool    preSwap = False;
   IROp    op      = Iop_INVALID;
   Bool    postNot = False;
   IRTemp  plain   = newTemp(Ity_V128);
   UChar   rm      = getUChar(delta);
   UInt    rG      = gregOfRexRM(pfx, rm);
   UInt    rV      = getVexNvvvv(pfx);
   IRTemp argL     = newTemp(Ity_V128);
   IRTemp argR     = newTemp(Ity_V128);

   assign(argL, getXMMReg(rV));
   if (epartIsReg(rm)) {
      imm8 = getUChar(delta+1);
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8, all_lanes, sz);
      if (!ok) return deltaIN; /* FAIL */
      UInt rE = eregOfRexRM(pfx,rm);
      assign(argR, getXMMReg(rE));
      delta += 1+1;
      DIP("%s $%d,%s,%s,%s\n",
          opname, (Int)imm8,
          nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8 = getUChar(delta+alen);
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8, all_lanes, sz);
      if (!ok) return deltaIN; /* FAIL */
      assign(argR, 
             all_lanes   ? loadLE(Ity_V128, mkexpr(addr))
             : sz == 8   ? unop( Iop_64UtoV128, loadLE(Ity_I64, mkexpr(addr)))
             : /*sz==4*/   unop( Iop_32UtoV128, loadLE(Ity_I32, mkexpr(addr))));
      delta += alen+1;
      DIP("%s $%d,%s,%s,%s\n",
          opname, (Int)imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG));
   }

   assign(plain, preSwap ? binop(op, mkexpr(argR), mkexpr(argL))
                         : binop(op, mkexpr(argL), mkexpr(argR)));

   if (all_lanes) {
      /* This is simple: just invert the result, if necessary, and
         have done. */
      if (postNot) {
         putYMMRegLoAndZU( rG, unop(Iop_NotV128, mkexpr(plain)) );
      } else {
         putYMMRegLoAndZU( rG, mkexpr(plain) );
      }
   }
   else
   if (!preSwap) {
      /* More complex.  It's a one-lane-only, hence need to possibly
         invert only that one lane.  But at least the other lanes are
         correctly "in" the result, having been copied from the left
         operand (argL). */
      if (postNot) {
         IRExpr* mask = mkV128(sz==4 ? 0x000F : 0x00FF);
         putYMMRegLoAndZU( rG, binop(Iop_XorV128, mkexpr(plain),
                                                  mask) );
      } else {
         putYMMRegLoAndZU( rG, mkexpr(plain) );
      }
   }
   else {
      /* This is the most complex case.  One-lane-only, but the args
         were swapped.  So we have to possibly invert the bottom lane,
         and (definitely) we have to copy the upper lane(s) from argL
         since, due to the swapping, what's currently there is from
         argR, which is not correct. */
      IRTemp res     = newTemp(Ity_V128);
      IRTemp mask    = newTemp(Ity_V128);
      IRTemp notMask = newTemp(Ity_V128);
      assign(mask,    mkV128(sz==4 ? 0x000F : 0x00FF));
      assign(notMask, mkV128(sz==4 ? 0xFFF0 : 0xFF00));
      if (postNot) {
         assign(res,
                binop(Iop_OrV128,
                      binop(Iop_AndV128,
                            unop(Iop_NotV128, mkexpr(plain)),
                            mkexpr(mask)),
                      binop(Iop_AndV128, mkexpr(argL), mkexpr(notMask))));
      } else {
         assign(res,
                binop(Iop_OrV128,
                      binop(Iop_AndV128,
                            mkexpr(plain),
                            mkexpr(mask)),
                      binop(Iop_AndV128, mkexpr(argL), mkexpr(notMask))));
      }
      putYMMRegLoAndZU( rG, mkexpr(res) );
   }

   *uses_vvvv = True;
   return delta;
}


/* Handles AVX256 32F/64F comparisons.  A derivative of
   dis_SSEcmp_E_to_G.  It can fail, in which case it returns the
   original delta to indicate failure. */
static
Long dis_AVX256_cmp_V_E_to_G ( /*OUT*/Bool* uses_vvvv,
                               VexAbiInfo* vbi,
                               Prefix pfx, Long delta, 
                               HChar* opname, Int sz )
{
   vassert(sz == 4 || sz == 8);
   Long    deltaIN = delta;
   HChar   dis_buf[50];
   Int     alen;
   UInt    imm8;
   IRTemp  addr;
   Bool    preSwap = False;
   IROp    op      = Iop_INVALID;
   Bool    postNot = False;
   IRTemp  plain   = newTemp(Ity_V256);
   UChar   rm      = getUChar(delta);
   UInt    rG      = gregOfRexRM(pfx, rm);
   UInt    rV      = getVexNvvvv(pfx);
   IRTemp argL     = newTemp(Ity_V256);
   IRTemp argR     = newTemp(Ity_V256);
   IRTemp argLhi   = IRTemp_INVALID;
   IRTemp argLlo   = IRTemp_INVALID;
   IRTemp argRhi   = IRTemp_INVALID;
   IRTemp argRlo   = IRTemp_INVALID;

   assign(argL, getYMMReg(rV));
   if (epartIsReg(rm)) {
      imm8 = getUChar(delta+1);
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8,
                             True/*all_lanes*/, sz);
      if (!ok) return deltaIN; /* FAIL */
      UInt rE = eregOfRexRM(pfx,rm);
      assign(argR, getYMMReg(rE));
      delta += 1+1;
      DIP("%s $%d,%s,%s,%s\n",
          opname, (Int)imm8,
          nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
      imm8 = getUChar(delta+alen);
      Bool ok = findSSECmpOp(&preSwap, &op, &postNot, imm8,
                             True/*all_lanes*/, sz);
      if (!ok) return deltaIN; /* FAIL */
      assign(argR, loadLE(Ity_V256, mkexpr(addr)) );
      delta += alen+1;
      DIP("%s $%d,%s,%s,%s\n",
          opname, (Int)imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
   }

   breakupV256toV128s( preSwap ? argR : argL, &argLhi, &argLlo );
   breakupV256toV128s( preSwap ? argL : argR, &argRhi, &argRlo );
   assign(plain, binop( Iop_V128HLtoV256,
                        binop(op, mkexpr(argLhi), mkexpr(argRhi)),
                        binop(op, mkexpr(argLlo), mkexpr(argRlo)) ) );

   /* This is simple: just invert the result, if necessary, and
      have done. */
   if (postNot) {
      putYMMReg( rG, unop(Iop_NotV256, mkexpr(plain)) );
   } else {
      putYMMReg( rG, mkexpr(plain) );
   }

   *uses_vvvv = True;
   return delta;
}


/* Handles AVX128 unary E-to-G all-lanes operations. */
static
Long dis_AVX128_E_to_G_unary ( /*OUT*/Bool* uses_vvvv,
                               VexAbiInfo* vbi,
                               Prefix pfx, Long delta, 
                               HChar* opname,
                               IRTemp (*opFn)(IRTemp) )
{
   HChar  dis_buf[50];
   Int    alen;
   IRTemp addr;
   IRTemp res  = newTemp(Ity_V128);
   IRTemp arg  = newTemp(Ity_V128);
   UChar  rm   = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx, rm);
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      assign(arg, getXMMReg(rE));
      delta += 1;
      DIP("%s %s,%s\n", opname, nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(arg, loadLE(Ity_V128, mkexpr(addr)));
      delta += alen;
      DIP("%s %s,%s\n", opname, dis_buf, nameXMMReg(rG));
   }
   res = opFn(arg);
   putYMMRegLoAndZU( rG, mkexpr(res) );
   *uses_vvvv = False;
   return delta;
}


/* Handles AVX128 unary E-to-G all-lanes operations. */
static
Long dis_AVX128_E_to_G_unary_all ( /*OUT*/Bool* uses_vvvv,
                                   VexAbiInfo* vbi,
                                   Prefix pfx, Long delta, 
                                   HChar* opname, IROp op )
{
   HChar  dis_buf[50];
   Int    alen;
   IRTemp addr;
   IRTemp arg  = newTemp(Ity_V128);
   UChar  rm   = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx, rm);
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      assign(arg, getXMMReg(rE));
      delta += 1;
      DIP("%s %s,%s\n", opname, nameXMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(arg, loadLE(Ity_V128, mkexpr(addr)));
      delta += alen;
      DIP("%s %s,%s\n", opname, dis_buf, nameXMMReg(rG));
   }
   putYMMRegLoAndZU( rG, unop(op, mkexpr(arg)) );
   *uses_vvvv = False;
   return delta;
}


/* FIXME: common up with the _128_ version above? */
static
Long dis_VEX_NDS_256_AnySimdPfx_0F_WIG (
        /*OUT*/Bool* uses_vvvv, VexAbiInfo* vbi,
        Prefix pfx, Long delta, HChar* name,
        /* The actual operation.  Use either 'op' or 'opfn',
           but not both. */
        IROp op, IRTemp(*opFn)(IRTemp,IRTemp),
        Bool invertLeftArg,
        Bool swapArgs
     )
{
   UChar  modrm = getUChar(delta);
   UInt   rD    = gregOfRexRM(pfx, modrm);
   UInt   rSL   = getVexNvvvv(pfx);
   IRTemp tSL   = newTemp(Ity_V256);
   IRTemp tSR   = newTemp(Ity_V256);
   IRTemp addr  = IRTemp_INVALID;
   HChar  dis_buf[50];
   Int    alen  = 0;
   vassert(1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*WIG?*/);

   assign(tSL, invertLeftArg ? unop(Iop_NotV256, getYMMReg(rSL))
                             : getYMMReg(rSL));

   if (epartIsReg(modrm)) {
      UInt rSR = eregOfRexRM(pfx, modrm);
      delta += 1;
      assign(tSR, getYMMReg(rSR));
      DIP("%s %s,%s,%s\n",
          name, nameYMMReg(rSR), nameYMMReg(rSL), nameYMMReg(rD));
   } else {
      addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
      delta += alen;
      assign(tSR, loadLE(Ity_V256, mkexpr(addr)));
      DIP("%s %s,%s,%s\n",
          name, dis_buf, nameYMMReg(rSL), nameYMMReg(rD));
   }

   IRTemp res = IRTemp_INVALID;
   if (op != Iop_INVALID) {
      vassert(opFn == NULL);
      res = newTemp(Ity_V256);
      assign(res, swapArgs ? binop(op, mkexpr(tSR), mkexpr(tSL))
                           : binop(op, mkexpr(tSL), mkexpr(tSR)));
   } else {
      vassert(opFn != NULL);
      res = swapArgs ? opFn(tSR, tSL) : opFn(tSL, tSR);
   }

   putYMMReg(rD, mkexpr(res));

   *uses_vvvv = True;
   return delta;
}


/* All-lanes AVX256 binary operation:
   G[255:0] = V[255:0] `op` E[255:0]
*/
static Long dis_AVX256_E_V_to_G ( /*OUT*/Bool* uses_vvvv,
                                  VexAbiInfo* vbi,
                                  Prefix pfx, Long delta, 
                                  HChar* opname, IROp op )
{
   return dis_VEX_NDS_256_AnySimdPfx_0F_WIG(
             uses_vvvv, vbi, pfx, delta, opname, op,
             NULL, False/*!invertLeftArg*/, False/*!swapArgs*/
   );
}


/* Handle a VEX_NDS_256_66_0F_WIG (3-addr) insn, using the given IR
   generator to compute the result, no inversion of the left
   arg, and no swapping of args. */
static
Long dis_VEX_NDS_256_AnySimdPfx_0F_WIG_complex (
        /*OUT*/Bool* uses_vvvv, VexAbiInfo* vbi,
        Prefix pfx, Long delta, HChar* name,
        IRTemp(*opFn)(IRTemp,IRTemp)
     )
{
   return dis_VEX_NDS_256_AnySimdPfx_0F_WIG(
             uses_vvvv, vbi, pfx, delta, name,
             Iop_INVALID, opFn, False, False );
}


/* Handles AVX256 unary E-to-G all-lanes operations. */
static
Long dis_AVX256_E_to_G_unary_all ( /*OUT*/Bool* uses_vvvv,
                                   VexAbiInfo* vbi,
                                   Prefix pfx, Long delta, 
                                   HChar* opname, IROp op )
{
   HChar  dis_buf[50];
   Int    alen;
   IRTemp addr;
   IRTemp arg  = newTemp(Ity_V256);
   UChar  rm   = getUChar(delta);
   UInt   rG   = gregOfRexRM(pfx, rm);
   if (epartIsReg(rm)) {
      UInt rE = eregOfRexRM(pfx,rm);
      assign(arg, getYMMReg(rE));
      delta += 1;
      DIP("%s %s,%s\n", opname, nameYMMReg(rE), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign(arg, loadLE(Ity_V256, mkexpr(addr)));
      delta += alen;
      DIP("%s %s,%s\n", opname, dis_buf, nameYMMReg(rG));
   }
   putYMMReg( rG, unop(op, mkexpr(arg)) );
   *uses_vvvv = False;
   return delta;
}


/* The use of ReinterpF64asI64 is ugly.  Surely could do better if we
   had a variant of Iop_64x4toV256 that took F64s as args instead. */
static Long dis_CVTDQ2PD_256 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   IRTemp sV    = newTemp(Ity_V128);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( sV, getXMMReg(rE) );
      delta += 1;
      DIP("vcvtdq2pd %s,%s\n", nameXMMReg(rE), nameYMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
      delta += alen;
      DIP("vcvtdq2pd %s,%s\n", dis_buf, nameYMMReg(rG) );
   }
   IRTemp s3, s2, s1, s0;
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
   IRExpr* res 
      = IRExpr_Qop(
           Iop_64x4toV256,
           unop(Iop_ReinterpF64asI64, unop(Iop_I32StoF64, mkexpr(s3))),
           unop(Iop_ReinterpF64asI64, unop(Iop_I32StoF64, mkexpr(s2))),
           unop(Iop_ReinterpF64asI64, unop(Iop_I32StoF64, mkexpr(s1))),
           unop(Iop_ReinterpF64asI64, unop(Iop_I32StoF64, mkexpr(s0)))
        );
   putYMMReg(rG, res);
   return delta;
}


static Long dis_CVTPD2PS_256 ( VexAbiInfo* vbi, Prefix pfx,
                               Long delta )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   UChar  modrm = getUChar(delta);
   UInt   rG    = gregOfRexRM(pfx,modrm);
   IRTemp argV  = newTemp(Ity_V256);
   IRTemp rmode = newTemp(Ity_I32);
   if (epartIsReg(modrm)) {
      UInt rE = eregOfRexRM(pfx,modrm);
      assign( argV, getYMMReg(rE) );
      delta += 1;
      DIP("vcvtpd2psy %s,%s\n", nameYMMReg(rE), nameXMMReg(rG));
   } else {
      addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
      assign( argV, loadLE(Ity_V256, mkexpr(addr)) );
      delta += alen;
      DIP("vcvtpd2psy %s,%s\n", dis_buf, nameXMMReg(rG) );
   }
         
   assign( rmode, get_sse_roundingmode() );
   IRTemp t3, t2, t1, t0;
   t3 = t2 = t1 = t0 = IRTemp_INVALID;
   breakupV256to64s( argV, &t3, &t2, &t1, &t0 );
#  define CVT(_t)  binop( Iop_F64toF32, mkexpr(rmode), \
                          unop(Iop_ReinterpI64asF64, mkexpr(_t)) )
   putXMMRegLane32F( rG, 3, CVT(t3) );
   putXMMRegLane32F( rG, 2, CVT(t2) );
   putXMMRegLane32F( rG, 1, CVT(t1) );
   putXMMRegLane32F( rG, 0, CVT(t0) );
#  undef CVT
   putYMMRegLane128( rG, 1, mkV128(0) );
   return delta;
}


__attribute__((noinline))
static
Long dis_ESC_0F__VEX (
        /*MB_OUT*/DisResult* dres,
        /*OUT*/   Bool*      uses_vvvv,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   *uses_vvvv = False;

   switch (opc) {

   case 0x10:
      /* VMOVSD m64, xmm1 = VEX.LIG.F2.0F.WIG 10 /r */
      /* Move 64 bits from E (mem only) to G (lo half xmm).
         Bits 255-64 of the dest are zeroed out. */
      if (haveF2no66noF3(pfx) && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         UInt   rG   = gregOfRexRM(pfx,modrm);
         IRTemp z128 = newTemp(Ity_V128);
         assign(z128, mkV128(0));
         putXMMReg( rG, mkexpr(z128) );
         /* FIXME: ALIGNMENT CHECK? */
         putXMMRegLane64( rG, 0, loadLE(Ity_I64, mkexpr(addr)) );
         putYMMRegLane128( rG, 1, mkexpr(z128) );
         DIP("vmovsd %s,%s\n", dis_buf, nameXMMReg(rG));
         delta += alen;
         goto decode_success;
      }
      /* VMOVSD xmm3, xmm2, xmm1 = VEX.LIG.F2.0F.WIG 10 /r */
      /* Reg form. */
      if (haveF2no66noF3(pfx) && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovsd %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           getXMMRegLane64(rV, 1),
                           getXMMRegLane64(rE, 0)));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVSS m32, xmm1 = VEX.LIG.F3.0F.WIG 10 /r */
      /* Move 32 bits from E (mem only) to G (lo half xmm).
         Bits 255-32 of the dest are zeroed out. */
      if (haveF3no66noF2(pfx) && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         UInt   rG   = gregOfRexRM(pfx,modrm);
         IRTemp z128 = newTemp(Ity_V128);
         assign(z128, mkV128(0));
         putXMMReg( rG, mkexpr(z128) );
         /* FIXME: ALIGNMENT CHECK? */
         putXMMRegLane32( rG, 0, loadLE(Ity_I32, mkexpr(addr)) );
         putYMMRegLane128( rG, 1, mkexpr(z128) );
         DIP("vmovss %s,%s\n", dis_buf, nameXMMReg(rG));
         delta += alen;
         goto decode_success;
      }
      /* VMOVSS xmm3, xmm2, xmm1 = VEX.LIG.F3.0F.WIG 10 /r */
      /* Reg form. */
      if (haveF3no66noF2(pfx) && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovss %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign( res, binop( Iop_64HLtoV128,
                             getXMMRegLane64(rV, 1),
                             binop(Iop_32HLto64,
                                   getXMMRegLane32(rV, 1),
                                   getXMMRegLane32(rE, 0)) ) );
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVUPD xmm2/m128, xmm1 = VEX.128.66.0F.WIG 10 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rG, getXMMReg( rE ));
            DIP("vmovupd %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putYMMRegLoAndZU( rG, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vmovupd %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPD ymm2/m256, ymm1 = VEX.256.66.0F.WIG 10 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rG, getYMMReg( rE ));
            DIP("vmovupd %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putYMMReg( rG, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vmovupd %s,%s\n", dis_buf, nameYMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPS xmm2/m128, xmm1 = VEX.128.0F.WIG 10 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rG, getXMMReg( rE ));
            DIP("vmovups %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putYMMRegLoAndZU( rG, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vmovups %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPS ymm2/m256, ymm1 = VEX.256.0F.WIG 10 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rG, getYMMReg( rE ));
            DIP("vmovups %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putYMMReg( rG, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vmovups %s,%s\n", dis_buf, nameYMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x11:
      /* VMOVSD xmm1, m64 = VEX.LIG.F2.0F.WIG 11 /r */
      /* Move 64 bits from G (low half xmm) to mem only. */
      if (haveF2no66noF3(pfx) && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         UInt   rG   = gregOfRexRM(pfx,modrm);
         /* FIXME: ALIGNMENT CHECK? */
         storeLE( mkexpr(addr), getXMMRegLane64(rG, 0));
         DIP("vmovsd %s,%s\n", nameXMMReg(rG), dis_buf);
         delta += alen;
         goto decode_success;
      }
      /* VMOVSD xmm3, xmm2, xmm1 = VEX.LIG.F2.0F.WIG 11 /r */
      /* Reg form. */
      if (haveF2no66noF3(pfx) && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovsd %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           getXMMRegLane64(rV, 1),
                           getXMMRegLane64(rE, 0)));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVSS xmm1, m64 = VEX.LIG.F3.0F.WIG 11 /r */
      /* Move 32 bits from G (low 1/4 xmm) to mem only. */
      if (haveF3no66noF2(pfx) && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         UInt   rG   = gregOfRexRM(pfx,modrm);
         /* FIXME: ALIGNMENT CHECK? */
         storeLE( mkexpr(addr), getXMMRegLane32(rG, 0));
         DIP("vmovss %s,%s\n", nameXMMReg(rG), dis_buf);
         delta += alen;
         goto decode_success;
      }
      /* VMOVSS xmm3, xmm2, xmm1 = VEX.LIG.F3.0F.WIG 11 /r */
      /* Reg form. */
      if (haveF3no66noF2(pfx) && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovss %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign( res, binop( Iop_64HLtoV128,
                             getXMMRegLane64(rV, 1),
                             binop(Iop_32HLto64,
                                   getXMMRegLane32(rV, 1),
                                   getXMMRegLane32(rE, 0)) ) );
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVUPD xmm1, xmm2/m128 = VEX.128.66.0F.WIG 11 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rE, getXMMReg(rG) );
            DIP("vmovupd %s,%s\n", nameXMMReg(rG), nameXMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMReg(rG) );
            DIP("vmovupd %s,%s\n", nameXMMReg(rG), dis_buf);
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPD ymm1, ymm2/m256 = VEX.256.66.0F.WIG 11 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rE, getYMMReg(rG) );
            DIP("vmovupd %s,%s\n", nameYMMReg(rG), nameYMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getYMMReg(rG) );
            DIP("vmovupd %s,%s\n", nameYMMReg(rG), dis_buf);
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPS xmm1, xmm2/m128 = VEX.128.0F.WIG 11 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rE, getXMMReg(rG) );
            DIP("vmovups %s,%s\n", nameXMMReg(rG), nameXMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMReg(rG) );
            DIP("vmovups %s,%s\n", nameXMMReg(rG), dis_buf);
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVUPS ymm1, ymm2/m256 = VEX.256.0F.WIG 11 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rE, getYMMReg(rG) );
            DIP("vmovups %s,%s\n", nameYMMReg(rG), nameYMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getYMMReg(rG) );
            DIP("vmovups %s,%s\n", nameYMMReg(rG), dis_buf);
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x12:
      /* VMOVDDUP xmm2/m64, xmm1 = VEX.128.F2.0F.WIG /12 r */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_MOVDDUP_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VMOVDDUP ymm2/m256, ymm1 = VEX.256.F2.0F.WIG /12 r */
      if (haveF2no66noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_MOVDDUP_256( vbi, pfx, delta );
         goto decode_success;
      }
      /* VMOVHLPS xmm3, xmm2, xmm1 = VEX.NDS.128.0F.WIG 12 /r */
      /* Insn only exists in reg form */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovhlps %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           getXMMRegLane64(rV, 1),
                           getXMMRegLane64(rE, 1)));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVLPS m64, xmm1, xmm2 = VEX.NDS.128.0F.WIG 12 /r */
      /* Insn exists only in mem form, it appears. */
      /* VMOVLPD m64, xmm1, xmm2 = VEX.NDS.128.66.0F.WIG 12 /r */
      /* Insn exists only in mem form, it appears. */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 0==getVexL(pfx)/*128*/ && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vmovlpd %s,%s,%s\n",
             dis_buf, nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           getXMMRegLane64(rV, 1),
                           loadLE(Ity_I64, mkexpr(addr))));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVSLDUP xmm2/m128, xmm1 = VEX.NDS.128.F3.0F.WIG 12 /r */
      if (haveF3no66noF2(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_MOVSxDUP_128( vbi, pfx, delta, True/*isAvx*/,
                                   True/*isL*/ );
         goto decode_success;
      }
      /* VMOVSLDUP ymm2/m256, ymm1 = VEX.NDS.256.F3.0F.WIG 12 /r */
      if (haveF3no66noF2(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_MOVSxDUP_256( vbi, pfx, delta, True/*isL*/ );
         goto decode_success;
      }
      break;

   case 0x13:
      /* VMOVLPS xmm1, m64 = VEX.128.0F.WIG 13 /r */
      /* Insn exists only in mem form, it appears. */
      /* VMOVLPD xmm1, m64 = VEX.128.66.0F.WIG 13 /r */
      /* Insn exists only in mem form, it appears. */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 0==getVexL(pfx)/*128*/ && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         storeLE( mkexpr(addr), getXMMRegLane64( rG, 0));
         DIP("vmovlpd %s,%s\n", nameXMMReg(rG), dis_buf);
         goto decode_success;
      }
      break;

   case 0x14:
   case 0x15:
      /* VUNPCKLPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 14 /r */
      /* VUNPCKHPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 15 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Bool   hi    = opc == 0x15;
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx,modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp eV    = newTemp(Ity_V128);
         IRTemp vV    = newTemp(Ity_V128);
         assign( vV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            delta += 1;
            DIP("vunpck%sps %s,%s\n", hi ? "h" : "l",
                nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("vunpck%sps %s,%s\n", hi ? "h" : "l",
                dis_buf, nameXMMReg(rG));
         }
         IRTemp res = math_UNPCKxPS_128( eV, vV, hi );
         putYMMRegLoAndZU( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VUNPCKLPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 14 /r */
      /* VUNPCKHPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 15 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Bool   hi    = opc == 0x15;
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx,modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp eV    = newTemp(Ity_V256);
         IRTemp vV    = newTemp(Ity_V256);
         assign( vV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getYMMReg(rE) );
            delta += 1;
            DIP("vunpck%sps %s,%s\n", hi ? "h" : "l",
                nameYMMReg(rE), nameYMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V256, mkexpr(addr)) );
            delta += alen;
            DIP("vunpck%sps %s,%s\n", hi ? "h" : "l",
                dis_buf, nameYMMReg(rG));
         }
         IRTemp res = math_UNPCKxPS_256( eV, vV, hi );
         putYMMReg( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VUNPCKLPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 14 /r */
      /* VUNPCKHPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 15 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Bool   hi    = opc == 0x15;
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx,modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp eV    = newTemp(Ity_V128);
         IRTemp vV    = newTemp(Ity_V128);
         assign( vV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            delta += 1;
            DIP("vunpck%spd %s,%s\n", hi ? "h" : "l",
                nameXMMReg(rE), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("vunpck%spd %s,%s\n", hi ? "h" : "l",
                dis_buf, nameXMMReg(rG));
         }
         IRTemp res = math_UNPCKxPD_128( eV, vV, hi );
         putYMMRegLoAndZU( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VUNPCKLPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 14 /r */
      /* VUNPCKHPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 15 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Bool   hi    = opc == 0x15;
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx,modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp eV    = newTemp(Ity_V256);
         IRTemp vV    = newTemp(Ity_V256);
         assign( vV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getYMMReg(rE) );
            delta += 1;
            DIP("vunpck%spd %s,%s\n", hi ? "h" : "l",
                nameYMMReg(rE), nameYMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( eV, loadLE(Ity_V256, mkexpr(addr)) );
            delta += alen;
            DIP("vunpck%spd %s,%s\n", hi ? "h" : "l",
                dis_buf, nameYMMReg(rG));
         }
         IRTemp res = math_UNPCKxPD_256( eV, vV, hi );
         putYMMReg( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x16:
      /* VMOVLHPS xmm3, xmm2, xmm1 = VEX.NDS.128.0F.WIG 16 /r */
      /* Insn only exists in reg form */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rE    = eregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         delta++;
         DIP("vmovlhps %s,%s,%s\n",
             nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           getXMMRegLane64(rE, 0),
                           getXMMRegLane64(rV, 0)));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVHPS m64, xmm1, xmm2 = VEX.NDS.128.0F.WIG 16 /r */
      /* Insn exists only in mem form, it appears. */
      /* VMOVHPD m64, xmm1, xmm2 = VEX.NDS.128.66.0F.WIG 16 /r */
      /* Insn exists only in mem form, it appears. */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 0==getVexL(pfx)/*128*/ && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         UInt  rV    = getVexNvvvv(pfx);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vmovhp%c %s,%s,%s\n", have66(pfx) ? 'd' : 's',
             dis_buf, nameXMMReg(rV), nameXMMReg(rG));
         IRTemp res = newTemp(Ity_V128);
         assign(res, binop(Iop_64HLtoV128,
                           loadLE(Ity_I64, mkexpr(addr)),
                           getXMMRegLane64(rV, 0)));
         putYMMRegLoAndZU(rG, mkexpr(res));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VMOVSHDUP xmm2/m128, xmm1 = VEX.NDS.128.F3.0F.WIG 16 /r */
      if (haveF3no66noF2(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_MOVSxDUP_128( vbi, pfx, delta, True/*isAvx*/,
                                   False/*!isL*/ );
         goto decode_success;
      }
      /* VMOVSHDUP ymm2/m256, ymm1 = VEX.NDS.256.F3.0F.WIG 16 /r */
      if (haveF3no66noF2(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_MOVSxDUP_256( vbi, pfx, delta, False/*!isL*/ );
         goto decode_success;
      }
      break;

   case 0x17:
      /* VMOVHPS xmm1, m64 = VEX.128.0F.WIG 17 /r */
      /* Insn exists only in mem form, it appears. */
      /* VMOVHPD xmm1, m64 = VEX.128.66.0F.WIG 17 /r */
      /* Insn exists only in mem form, it appears. */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 0==getVexL(pfx)/*128*/ && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         storeLE( mkexpr(addr), getXMMRegLane64( rG, 1));
         DIP("vmovhp%c %s,%s\n", have66(pfx) ? 'd' : 's',
             nameXMMReg(rG), dis_buf);
         goto decode_success;
      }
      break;

   case 0x28:
      /* VMOVAPD xmm2/m128, xmm1 = VEX.128.66.0F.WIG 28 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rG, getXMMReg( rE ));
            DIP("vmovapd %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            putYMMRegLoAndZU( rG, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vmovapd %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVAPD ymm2/m256, ymm1 = VEX.256.66.0F.WIG 28 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rG, getYMMReg( rE ));
            DIP("vmovapd %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_32_aligned( addr );
            putYMMReg( rG, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vmovapd %s,%s\n", dis_buf, nameYMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVAPS xmm2/m128, xmm1 = VEX.128.0F.WIG 28 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rG, getXMMReg( rE ));
            DIP("vmovaps %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            putYMMRegLoAndZU( rG, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vmovaps %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVAPS ymm2/m256, ymm1 = VEX.256.0F.WIG 28 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rG, getYMMReg( rE ));
            DIP("vmovaps %s,%s\n", nameYMMReg(rE), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_32_aligned( addr );
            putYMMReg( rG, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vmovaps %s,%s\n", dis_buf, nameYMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x29:
      /* VMOVAPD xmm1, xmm2/m128 = VEX.128.66.0F.WIG 29 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rE, getXMMReg(rG) );
            DIP("vmovapd %s,%s\n", nameXMMReg(rG), nameXMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(rG) );
            DIP("vmovapd %s,%s\n", nameXMMReg(rG), dis_buf );
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVAPD ymm1, ymm2/m256 = VEX.256.66.0F.WIG 29 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rE, getYMMReg(rG) );
            DIP("vmovapd %s,%s\n", nameYMMReg(rG), nameYMMReg(rE));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_32_aligned( addr );
            storeLE( mkexpr(addr), getYMMReg(rG) );
            DIP("vmovapd %s,%s\n", nameYMMReg(rG), dis_buf );
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVAPS xmm1, xmm2/m128 = VEX.128.0F.WIG 29 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMRegLoAndZU( rE, getXMMReg(rG) );
            DIP("vmovaps %s,%s\n", nameXMMReg(rG), nameXMMReg(rE));
            delta += 1;
            goto decode_success;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(rG) );
            DIP("vmovaps %s,%s\n", nameXMMReg(rG), dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      /* VMOVAPS ymm1, ymm2/m256 = VEX.256.0F.WIG 29 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putYMMReg( rE, getYMMReg(rG) );
            DIP("vmovaps %s,%s\n", nameYMMReg(rG), nameYMMReg(rE));
            delta += 1;
            goto decode_success;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_32_aligned( addr );
            storeLE( mkexpr(addr), getYMMReg(rG) );
            DIP("vmovaps %s,%s\n", nameYMMReg(rG), dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      break;

   case 0x2A: {
      IRTemp rmode = newTemp(Ity_I32);
      assign( rmode, get_sse_roundingmode() );
      /* VCVTSI2SD r/m32, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.W0 2A /r */
      if (haveF2no66noF3(pfx) && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp arg32 = newTemp(Ity_I32);
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign( arg32, getIReg32(rS) );
            delta += 1;
            DIP("vcvtsi2sdl %s,%s,%s\n",
                nameIReg32(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg32, loadLE(Ity_I32, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtsi2sdl %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane64F( rD, 0,
                           unop(Iop_I32StoF64, mkexpr(arg32)));
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VCVTSI2SD r/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.W1 2A /r */
      if (haveF2no66noF3(pfx) && 1==getRexW(pfx)/*W1*/) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp arg64 = newTemp(Ity_I64);
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign( arg64, getIReg64(rS) );
            delta += 1;
            DIP("vcvtsi2sdq %s,%s,%s\n",
                nameIReg64(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtsi2sdq %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane64F( rD, 0,
                           binop( Iop_I64StoF64,
                                  get_sse_roundingmode(),
                                  mkexpr(arg64)) );
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VCVTSI2SS r/m64, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.W1 2A /r */
      if (haveF3no66noF2(pfx) && 1==getRexW(pfx)/*W1*/) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp arg64 = newTemp(Ity_I64);
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign( arg64, getIReg64(rS) );
            delta += 1;
            DIP("vcvtsi2ssq %s,%s,%s\n",
                nameIReg64(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg64, loadLE(Ity_I64, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtsi2ssq %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane32F( rD, 0,
                           binop(Iop_F64toF32,
                                 mkexpr(rmode),
                                 binop(Iop_I64StoF64, mkexpr(rmode),
                                                      mkexpr(arg64)) ) );
         putXMMRegLane32( rD, 1, getXMMRegLane32( rV, 1 ));
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VCVTSI2SS r/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.W0 2A /r */
      if (haveF3no66noF2(pfx) && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp arg32 = newTemp(Ity_I32);
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign( arg32, getIReg32(rS) );
            delta += 1;
            DIP("vcvtsi2ssl %s,%s,%s\n",
                nameIReg32(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( arg32, loadLE(Ity_I32, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtsi2ssl %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane32F( rD, 0,
                           binop(Iop_F64toF32,
                                 mkexpr(rmode),
                                 unop(Iop_I32StoF64, mkexpr(arg32)) ) );
         putXMMRegLane32( rD, 1, getXMMRegLane32( rV, 1 ));
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;
   }

   case 0x2B:
      /* VMOVNTPD xmm1, m128 = VEX.128.66.0F.WIG 2B /r */
      /* VMOVNTPS xmm1, m128 = VEX.128.0F.WIG 2B /r */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 0==getVexL(pfx)/*128*/ && !epartIsReg(getUChar(delta))) {
         UChar  modrm = getUChar(delta);
         UInt   rS    = gregOfRexRM(pfx, modrm);
         IRTemp tS    = newTemp(Ity_V128);
         assign(tS, getXMMReg(rS));
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         gen_SEGV_if_not_16_aligned(addr);
         storeLE(mkexpr(addr), mkexpr(tS));
         DIP("vmovntp%c %s,%s\n", have66(pfx) ? 'd' : 's',
             nameXMMReg(rS), dis_buf);
         goto decode_success;
      }
      /* VMOVNTPD ymm1, m256 = VEX.256.66.0F.WIG 2B /r */
      /* VMOVNTPS ymm1, m256 = VEX.256.0F.WIG 2B /r */
      if ((have66noF2noF3(pfx) || haveNo66noF2noF3(pfx))
          && 1==getVexL(pfx)/*256*/ && !epartIsReg(getUChar(delta))) {
         UChar  modrm = getUChar(delta);
         UInt   rS    = gregOfRexRM(pfx, modrm);
         IRTemp tS    = newTemp(Ity_V256);
         assign(tS, getYMMReg(rS));
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         gen_SEGV_if_not_32_aligned(addr);
         storeLE(mkexpr(addr), mkexpr(tS));
         DIP("vmovntp%c %s,%s\n", have66(pfx) ? 'd' : 's',
             nameYMMReg(rS), dis_buf);
         goto decode_success;
      }
      break;

   case 0x2C:
      /* VCVTTSD2SI xmm1/m32, r32 = VEX.LIG.F2.0F.W0 2C /r */
      if (haveF2no66noF3(pfx) && 0==getRexW(pfx)/*W0*/) {
         delta = dis_CVTxSD2SI( vbi, pfx, delta, True/*isAvx*/, opc, 4);
         goto decode_success;
      }
      /* VCVTTSD2SI xmm1/m64, r64 = VEX.LIG.F2.0F.W1 2C /r */
      if (haveF2no66noF3(pfx) && 1==getRexW(pfx)/*W1*/) {
         delta = dis_CVTxSD2SI( vbi, pfx, delta, True/*isAvx*/, opc, 8);
         goto decode_success;
      }
      /* VCVTTSS2SI xmm1/m32, r32 = VEX.LIG.F3.0F.W0 2C /r */
      if (haveF3no66noF2(pfx) && 0==getRexW(pfx)/*W0*/) {
         delta = dis_CVTxSS2SI( vbi, pfx, delta, True/*isAvx*/, opc, 4);
         goto decode_success;
      }
      /* VCVTTSS2SI xmm1/m64, r64 = VEX.LIG.F3.0F.W1 2C /r */
      if (haveF3no66noF2(pfx) && 1==getRexW(pfx)/*W1*/) {
         delta = dis_CVTxSS2SI( vbi, pfx, delta, True/*isAvx*/, opc, 8);
         goto decode_success;
      }
      break;

   case 0x2D:
      /* VCVTSD2SI xmm1/m32, r32 = VEX.LIG.F2.0F.W0 2D /r */
      if (haveF2no66noF3(pfx) && 0==getRexW(pfx)/*W0*/) {
         delta = dis_CVTxSD2SI( vbi, pfx, delta, True/*isAvx*/, opc, 4);
         goto decode_success;
      }
      /* VCVTSD2SI xmm1/m64, r64 = VEX.LIG.F2.0F.W1 2D /r */
      if (haveF2no66noF3(pfx) && 1==getRexW(pfx)/*W1*/) {
         delta = dis_CVTxSD2SI( vbi, pfx, delta, True/*isAvx*/, opc, 8);
         goto decode_success;
      }
      /* VCVTSS2SI xmm1/m32, r32 = VEX.LIG.F3.0F.W0 2D /r */
      if (haveF3no66noF2(pfx) && 0==getRexW(pfx)/*W0*/) {
         delta = dis_CVTxSS2SI( vbi, pfx, delta, True/*isAvx*/, opc, 4);
         goto decode_success;
      }
      /* VCVTSS2SI xmm1/m64, r64 = VEX.LIG.F3.0F.W1 2D /r */
      if (haveF3no66noF2(pfx) && 1==getRexW(pfx)/*W1*/) {
         delta = dis_CVTxSS2SI( vbi, pfx, delta, True/*isAvx*/, opc, 8);
         goto decode_success;
      }
      break;

   case 0x2E:
   case 0x2F:
      /* VUCOMISD xmm2/m64, xmm1 = VEX.LIG.66.0F.WIG 2E /r */
      /* VCOMISD  xmm2/m64, xmm1 = VEX.LIG.66.0F.WIG 2F /r */
      if (have66noF2noF3(pfx)) {
         delta = dis_COMISD( vbi, pfx, delta, True/*isAvx*/, opc );
         goto decode_success;
      }
      /* VUCOMISS xmm2/m32, xmm1 = VEX.LIG.0F.WIG 2E /r */
      /* VCOMISS xmm2/m32, xmm1  = VEX.LIG.0F.WIG 2F /r */
      if (haveNo66noF2noF3(pfx)) {
         delta = dis_COMISS( vbi, pfx, delta, True/*isAvx*/, opc );
         goto decode_success;
      }
      break;

   case 0x50:
      /* VMOVMSKPD xmm2, r32 = VEX.128.66.0F.WIG 50 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_MOVMSKPD_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VMOVMSKPD ymm2, r32 = VEX.256.66.0F.WIG 50 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_MOVMSKPD_256( vbi, pfx, delta );
         goto decode_success;
      }
      /* VMOVMSKPS xmm2, r32 = VEX.128.0F.WIG 50 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_MOVMSKPS_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VMOVMSKPS ymm2, r32 = VEX.256.0F.WIG 50 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_MOVMSKPS_256( vbi, pfx, delta );
         goto decode_success;
      }
      break;

   case 0x51:
      /* VSQRTSS xmm3/m64(E), xmm2(V), xmm1(G) = VEX.NDS.LIG.F3.0F.WIG 51 /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32_unary(
                    uses_vvvv, vbi, pfx, delta, "vsqrtss", Iop_Sqrt32F0x4 );
         goto decode_success;
      }
      /* VSQRTPS xmm2/m128(E), xmm1(G) = VEX.NDS.128.0F.WIG 51 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vsqrtps", Iop_Sqrt32Fx4 );
         goto decode_success;
      }
      /* VSQRTPS ymm2/m256(E), ymm1(G) = VEX.NDS.256.0F.WIG 51 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vsqrtps", Iop_Sqrt32Fx8 );
         goto decode_success;
      }
      /* VSQRTSD xmm3/m64(E), xmm2(V), xmm1(G) = VEX.NDS.LIG.F2.0F.WIG 51 /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64_unary(
                    uses_vvvv, vbi, pfx, delta, "vsqrtsd", Iop_Sqrt64F0x2 );
         goto decode_success;
      }
      /* VSQRTPD xmm2/m128(E), xmm1(G) = VEX.NDS.128.66.0F.WIG 51 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vsqrtpd", Iop_Sqrt64Fx2 );
         goto decode_success;
      }
      /* VSQRTPD ymm2/m256(E), ymm1(G) = VEX.NDS.256.66.0F.WIG 51 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vsqrtpd", Iop_Sqrt64Fx4 );
         goto decode_success;
      }
      break;

   case 0x52:
      /* VRSQRTSS xmm3/m64(E), xmm2(V), xmm1(G) = VEX.NDS.LIG.F3.0F.WIG 52 /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32_unary(
                    uses_vvvv, vbi, pfx, delta, "vrsqrtss", Iop_RSqrt32F0x4 );
         goto decode_success;
      }
      /* VRSQRTPS xmm2/m128(E), xmm1(G) = VEX.NDS.128.0F.WIG 52 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vrsqrtps", Iop_RSqrt32Fx4 );
         goto decode_success;
      }
      /* VRSQRTPS ymm2/m256(E), ymm1(G) = VEX.NDS.256.0F.WIG 52 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vrsqrtps", Iop_RSqrt32Fx8 );
         goto decode_success;
      }
      break;

   case 0x53:
      /* VRCPSS xmm3/m64(E), xmm2(V), xmm1(G) = VEX.NDS.LIG.F3.0F.WIG 53 /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32_unary(
                    uses_vvvv, vbi, pfx, delta, "vrcpss", Iop_Recip32F0x4 );
         goto decode_success;
      }
      /* VRCPPS xmm2/m128(E), xmm1(G) = VEX.NDS.128.0F.WIG 53 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vrcpps", Iop_Recip32Fx4 );
         goto decode_success;
      }
      /* VRCPPS ymm2/m256(E), ymm1(G) = VEX.NDS.256.0F.WIG 53 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_to_G_unary_all(
                    uses_vvvv, vbi, pfx, delta, "vrcpps", Iop_Recip32Fx8 );
         goto decode_success;
      }
      break;

   case 0x54:
      /* VANDPD r/m, rV, r ::: r = rV & r/m */
      /* VANDPD = VEX.NDS.128.66.0F.WIG 54 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vandpd", Iop_AndV128 );
         goto decode_success;
      }
      /* VANDPD r/m, rV, r ::: r = rV & r/m */
      /* VANDPD = VEX.NDS.256.66.0F.WIG 54 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vandpd", Iop_AndV256 );
         goto decode_success;
      }
      /* VANDPS = VEX.NDS.128.0F.WIG 54 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vandps", Iop_AndV128 );
         goto decode_success;
      }
      /* VANDPS = VEX.NDS.256.0F.WIG 54 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vandps", Iop_AndV256 );
         goto decode_success;
      }
      break;

   case 0x55:
      /* VANDNPD r/m, rV, r ::: r = (not rV) & r/m */
      /* VANDNPD = VEX.NDS.128.66.0F.WIG 55 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vandpd", Iop_AndV128,
                    NULL, True/*invertLeftArg*/, False/*swapArgs*/ );
         goto decode_success;
      }
      /* VANDNPD = VEX.NDS.256.66.0F.WIG 55 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VEX_NDS_256_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vandpd", Iop_AndV256,
                    NULL, True/*invertLeftArg*/, False/*swapArgs*/ );
         goto decode_success;
      }
      /* VANDNPS = VEX.NDS.128.0F.WIG 55 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vandps", Iop_AndV128,
                    NULL, True/*invertLeftArg*/, False/*swapArgs*/ );
         goto decode_success;
      }
      /* VANDNPS = VEX.NDS.256.0F.WIG 55 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VEX_NDS_256_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vandps", Iop_AndV256,
                    NULL, True/*invertLeftArg*/, False/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x56:
      /* VORPD r/m, rV, r ::: r = rV | r/m */
      /* VORPD = VEX.NDS.128.66.0F.WIG 56 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vorpd", Iop_OrV128 );
         goto decode_success;
      }
      /* VORPD r/m, rV, r ::: r = rV | r/m */
      /* VORPD = VEX.NDS.256.66.0F.WIG 56 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vorpd", Iop_OrV256 );
         goto decode_success;
      }
      /* VORPS r/m, rV, r ::: r = rV | r/m */
      /* VORPS = VEX.NDS.128.0F.WIG 56 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vorps", Iop_OrV128 );
         goto decode_success;
      }
      /* VORPS r/m, rV, r ::: r = rV | r/m */
      /* VORPS = VEX.NDS.256.0F.WIG 56 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vorps", Iop_OrV256 );
         goto decode_success;
      }
      break;

   case 0x57:
      /* VXORPD r/m, rV, r ::: r = rV ^ r/m */
      /* VXORPD = VEX.NDS.128.66.0F.WIG 57 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vxorpd", Iop_XorV128 );
         goto decode_success;
      }
      /* VXORPD r/m, rV, r ::: r = rV ^ r/m */
      /* VXORPD = VEX.NDS.256.66.0F.WIG 57 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vxorpd", Iop_XorV256 );
         goto decode_success;
      }
      /* VXORPS r/m, rV, r ::: r = rV ^ r/m */
      /* VXORPS = VEX.NDS.128.0F.WIG 57 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vxorps", Iop_XorV128 );
         goto decode_success;
      }
      /* VXORPS r/m, rV, r ::: r = rV ^ r/m */
      /* VXORPS = VEX.NDS.256.0F.WIG 57 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vxorps", Iop_XorV256 );
         goto decode_success;
      }
      break;

   case 0x58:
      /* VADDSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 58 /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vaddsd", Iop_Add64F0x2 );
         goto decode_success;
      }
      /* VADDSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 58 /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vaddss", Iop_Add32F0x4 );
         goto decode_success;
      }
      /* VADDPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 58 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vaddps", Iop_Add32Fx4 );
         goto decode_success;
      }
      /* VADDPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 58 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vaddps", Iop_Add32Fx8 );
         goto decode_success;
      }
      /* VADDPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 58 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vaddpd", Iop_Add64Fx2 );
         goto decode_success;
      }
      /* VADDPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 58 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vaddpd", Iop_Add64Fx4 );
         goto decode_success;
      }
      break;

   case 0x59:
      /* VMULSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 59 /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vmulsd", Iop_Mul64F0x2 );
         goto decode_success;
      }
      /* VMULSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 59 /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vmulss", Iop_Mul32F0x4 );
         goto decode_success;
      }
      /* VMULPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 59 /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmulps", Iop_Mul32Fx4 );
         goto decode_success;
      }
      /* VMULPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 59 /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmulps", Iop_Mul32Fx8 );
         goto decode_success;
      }
      /* VMULPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 59 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmulpd", Iop_Mul64Fx2 );
         goto decode_success;
      }
      /* VMULPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 59 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmulpd", Iop_Mul64Fx4 );
         goto decode_success;
      }
      break;

   case 0x5A:
      /* VCVTPS2PD xmm2/m64, xmm1 = VEX.128.0F.WIG 5A /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTPS2PD_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VCVTPS2PD xmm2/m128, ymm1 = VEX.256.0F.WIG 5A /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTPS2PD_256( vbi, pfx, delta );
         goto decode_success;
      }
      /* VCVTPD2PS xmm2/m128, xmm1 = VEX.128.66.0F.WIG 5A /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTPD2PS_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VCVTPD2PS ymm2/m256, xmm1 = VEX.256.66.0F.WIG 5A /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTPD2PS_256( vbi, pfx, delta );
         goto decode_success;
      }
      /* VCVTSD2SS xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 5A /r */
      if (haveF2no66noF3(pfx)) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp f64lo = newTemp(Ity_F64);
         IRTemp rmode = newTemp(Ity_I32);
         assign( rmode, get_sse_roundingmode() );
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign(f64lo, getXMMRegLane64F(rS, 0));
            delta += 1;
            DIP("vcvtsd2ss %s,%s,%s\n",
                nameXMMReg(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f64lo, loadLE(Ity_F64, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtsd2ss %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane32F( rD, 0,
                           binop( Iop_F64toF32, mkexpr(rmode),
                                                mkexpr(f64lo)) );
         putXMMRegLane32( rD, 1, getXMMRegLane32( rV, 1 ));
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VCVTSS2SD xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 5A /r */
      if (haveF3no66noF2(pfx)) {
         UChar  modrm = getUChar(delta);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp f32lo = newTemp(Ity_F32);
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx,modrm);
            assign(f32lo, getXMMRegLane32F(rS, 0));
            delta += 1;
            DIP("vcvtss2sd %s,%s,%s\n",
                nameXMMReg(rS), nameXMMReg(rV), nameXMMReg(rD));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign(f32lo, loadLE(Ity_F32, mkexpr(addr)) );
            delta += alen;
            DIP("vcvtss2sd %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rD));
         }
         putXMMRegLane64F( rD, 0,
                           unop( Iop_F32toF64, mkexpr(f32lo)) );
         putXMMRegLane64( rD, 1, getXMMRegLane64( rV, 1 ));
         putYMMRegLane128( rD, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x5B:
      /* VCVTPS2DQ xmm2/m128, xmm1 = VEX.128.66.0F.WIG 5B /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTxPS2DQ_128( vbi, pfx, delta,
                                    True/*isAvx*/, False/*!r2zero*/ );
         goto decode_success;
      }
      /* VCVTPS2DQ ymm2/m256, ymm1 = VEX.256.66.0F.WIG 5B /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTxPS2DQ_256( vbi, pfx, delta,
                                    False/*!r2zero*/ );
         goto decode_success;
      }
      /* VCVTTPS2DQ xmm2/m128, xmm1 = VEX.128.F3.0F.WIG 5B /r */
      if (haveF3no66noF2(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTxPS2DQ_128( vbi, pfx, delta,
                                    True/*isAvx*/, True/*r2zero*/ );
         goto decode_success;
      }
      /* VCVTTPS2DQ ymm2/m256, ymm1 = VEX.256.F3.0F.WIG 5B /r */
      if (haveF3no66noF2(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTxPS2DQ_256( vbi, pfx, delta,
                                    True/*r2zero*/ );
         goto decode_success;
      }
      /* VCVTDQ2PS xmm2/m128, xmm1 = VEX.128.0F.WIG 5B /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTDQ2PS_128 ( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VCVTDQ2PS ymm2/m256, ymm1 = VEX.256.0F.WIG 5B /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTDQ2PS_256 ( vbi, pfx, delta );
         goto decode_success;
      }
      break;

   case 0x5C:
      /* VSUBSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 5C /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vsubsd", Iop_Sub64F0x2 );
         goto decode_success;
      }
      /* VSUBSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 5C /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vsubss", Iop_Sub32F0x4 );
         goto decode_success;
      }
      /* VSUBPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 5C /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vsubps", Iop_Sub32Fx4 );
         goto decode_success;
      }
      /* VSUBPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 5C /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vsubps", Iop_Sub32Fx8 );
         goto decode_success;
      }
      /* VSUBPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 5C /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vsubpd", Iop_Sub64Fx2 );
         goto decode_success;
      }
      /* VSUBPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 5C /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vsubpd", Iop_Sub64Fx4 );
         goto decode_success;
      }
      break;

   case 0x5D:
      /* VMINSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 5D /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vminsd", Iop_Min64F0x2 );
         goto decode_success;
      }
      /* VMINSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 5D /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vminss", Iop_Min32F0x4 );
         goto decode_success;
      }
      /* VMINPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 5D /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vminps", Iop_Min32Fx4 );
         goto decode_success;
      }
      /* VMINPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 5D /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vminps", Iop_Min32Fx8 );
         goto decode_success;
      }
      /* VMINPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 5D /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vminpd", Iop_Min64Fx2 );
         goto decode_success;
      }
      /* VMINPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 5D /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vminpd", Iop_Min64Fx4 );
         goto decode_success;
      }
      break;

   case 0x5E:
      /* VDIVSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 5E /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vdivsd", Iop_Div64F0x2 );
         goto decode_success;
      }
      /* VDIVSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 5E /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vdivss", Iop_Div32F0x4 );
         goto decode_success;
      }
      /* VDIVPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 5E /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vdivps", Iop_Div32Fx4 );
         goto decode_success;
      }
      /* VDIVPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 5E /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vdivps", Iop_Div32Fx8 );
         goto decode_success;
      }
      /* VDIVPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 5E /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vdivpd", Iop_Div64Fx2 );
         goto decode_success;
      }
      /* VDIVPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 5E /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vdivpd", Iop_Div64Fx4 );
         goto decode_success;
      }
      break;

   case 0x5F:
      /* VMAXSD xmm3/m64, xmm2, xmm1 = VEX.NDS.LIG.F2.0F.WIG 5F /r */
      if (haveF2no66noF3(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo64(
                    uses_vvvv, vbi, pfx, delta, "vmaxsd", Iop_Max64F0x2 );
         goto decode_success;
      }
      /* VMAXSS xmm3/m32, xmm2, xmm1 = VEX.NDS.LIG.F3.0F.WIG 5F /r */
      if (haveF3no66noF2(pfx)) {
         delta = dis_AVX128_E_V_to_G_lo32(
                    uses_vvvv, vbi, pfx, delta, "vmaxss", Iop_Max32F0x4 );
         goto decode_success;
      }
      /* VMAXPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.0F.WIG 5F /r */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmaxps", Iop_Max32Fx4 );
         goto decode_success;
      }
      /* VMAXPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.0F.WIG 5F /r */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmaxps", Iop_Max32Fx8 );
         goto decode_success;
      }
      /* VMAXPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 5F /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmaxpd", Iop_Max64Fx2 );
         goto decode_success;
      }
      /* VMAXPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 5F /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_AVX256_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vmaxpd", Iop_Max64Fx4 );
         goto decode_success;
      }
      break;

   case 0x60:
      /* VPUNPCKLBW r/m, rV, r ::: r = interleave-lo-bytes(rV, r/m) */
      /* VPUNPCKLBW = VEX.NDS.128.66.0F.WIG 60 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpcklbw",
                    Iop_InterleaveLO8x16, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x61:
      /* VPUNPCKLWD r/m, rV, r ::: r = interleave-lo-words(rV, r/m) */
      /* VPUNPCKLWD = VEX.NDS.128.66.0F.WIG 61 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpcklwd",
                    Iop_InterleaveLO16x8, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x62:
      /* VPUNPCKLDQ r/m, rV, r ::: r = interleave-lo-dwords(rV, r/m) */
      /* VPUNPCKLDQ = VEX.NDS.128.66.0F.WIG 62 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpckldq",
                    Iop_InterleaveLO32x4, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x63:
      /* VPACKSSWB r/m, rV, r ::: r = QNarrowBin16Sto8Sx16(rV, r/m) */
      /* VPACKSSWB = VEX.NDS.128.66.0F.WIG 63 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpacksswb",
                    Iop_QNarrowBin16Sto8Sx16, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x64:
      /* VPCMPGTB r/m, rV, r ::: r = rV `>s-by-8s` r/m */
      /* VPCMPGTB = VEX.NDS.128.66.0F.WIG 64 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpgtb", Iop_CmpGT8Sx16 );
         goto decode_success;
      }
      break;

   case 0x65:
      /* VPCMPGTW r/m, rV, r ::: r = rV `>s-by-16s` r/m */
      /* VPCMPGTW = VEX.NDS.128.66.0F.WIG 65 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpgtw", Iop_CmpGT16Sx8 );
         goto decode_success;
      }
      break;

   case 0x66:
      /* VPCMPGTD r/m, rV, r ::: r = rV `>s-by-32s` r/m */
      /* VPCMPGTD = VEX.NDS.128.66.0F.WIG 66 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpgtd", Iop_CmpGT32Sx4 );
         goto decode_success;
      }
      break;

   case 0x67:
      /* VPACKUSWB r/m, rV, r ::: r = QNarrowBin16Sto8Ux16(rV, r/m) */
      /* VPACKUSWB = VEX.NDS.128.66.0F.WIG 67 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpackuswb",
                    Iop_QNarrowBin16Sto8Ux16, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x68:
      /* VPUNPCKHBW r/m, rV, r ::: r = interleave-hi-bytes(rV, r/m) */
      /* VPUNPCKHBW = VEX.NDS.128.0F.WIG 68 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpckhbw",
                    Iop_InterleaveHI8x16, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x69:
      /* VPUNPCKHWD r/m, rV, r ::: r = interleave-hi-words(rV, r/m) */
      /* VPUNPCKHWD = VEX.NDS.128.0F.WIG 69 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpckhwd",
                    Iop_InterleaveHI16x8, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x6A:
      /* VPUNPCKHDQ r/m, rV, r ::: r = interleave-hi-dwords(rV, r/m) */
      /* VPUNPCKHDQ = VEX.NDS.128.66.0F.WIG 6A /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpckhdq",
                    Iop_InterleaveHI32x4, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x6B:
      /* VPACKSSDW r/m, rV, r ::: r = QNarrowBin32Sto16Sx8(rV, r/m) */
      /* VPACKSSDW = VEX.NDS.128.66.0F.WIG 6B /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpackssdw",
                    Iop_QNarrowBin32Sto16Sx8, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x6C:
      /* VPUNPCKLQDQ r/m, rV, r ::: r = interleave-lo-64bitses(rV, r/m) */
      /* VPUNPCKLQDQ = VEX.NDS.128.0F.WIG 6C /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpcklqdq",
                    Iop_InterleaveLO64x2, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x6D:
      /* VPUNPCKHQDQ r/m, rV, r ::: r = interleave-hi-64bitses(rV, r/m) */
      /* VPUNPCKHQDQ = VEX.NDS.128.0F.WIG 6D /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpunpckhqdq",
                    Iop_InterleaveHI64x2, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x6E:
      /* VMOVD r32/m32, xmm1 = VEX.128.66.0F.W0 6E */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         vassert(sz == 2); /* even tho we are transferring 4, not 2. */
         UChar modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            putYMMRegLoAndZU(
               gregOfRexRM(pfx,modrm),
               unop( Iop_32UtoV128, getIReg32(eregOfRexRM(pfx,modrm)) ) 
            );
            DIP("vmovd %s, %s\n", nameIReg32(eregOfRexRM(pfx,modrm)), 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
        } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putYMMRegLoAndZU(
               gregOfRexRM(pfx,modrm),
               unop( Iop_32UtoV128,loadLE(Ity_I32, mkexpr(addr)))
                             );
            DIP("vmovd %s, %s\n", dis_buf, 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         }
         goto decode_success;
      }
      /* VMOVQ r64/m64, xmm1 = VEX.128.66.0F.W1 6E */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 1==getRexW(pfx)/*W1*/) {
         vassert(sz == 2); /* even tho we are transferring 8, not 2. */
         UChar modrm = getUChar(delta);
         if (epartIsReg(modrm)) {
            delta += 1;
            putYMMRegLoAndZU(
               gregOfRexRM(pfx,modrm),
               unop( Iop_64UtoV128, getIReg64(eregOfRexRM(pfx,modrm)) ) 
            );
            DIP("vmovq %s, %s\n", nameIReg64(eregOfRexRM(pfx,modrm)), 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
        } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            putYMMRegLoAndZU(
               gregOfRexRM(pfx,modrm),
               unop( Iop_64UtoV128,loadLE(Ity_I64, mkexpr(addr)))
                             );
            DIP("vmovq %s, %s\n", dis_buf, 
                                  nameXMMReg(gregOfRexRM(pfx,modrm)));
         }
         goto decode_success;
      }
      break;

   case 0x6F:
      /* VMOVDQA ymm2/m256, ymm1 = VEX.256.66.0F.WIG 6F */
      /* VMOVDQU ymm2/m256, ymm1 = VEX.256.F3.0F.WIG 6F */
      if ((have66noF2noF3(pfx) || haveF3no66noF2(pfx))
          && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp tD    = newTemp(Ity_V256);
         Bool   isA   = have66noF2noF3(pfx);
         UChar  ch    = isA ? 'a' : 'u';
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx, modrm);
            delta += 1;
            assign(tD, getYMMReg(rS));
            DIP("vmovdq%c %s,%s\n", ch, nameYMMReg(rS), nameYMMReg(rD));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            if (isA)
               gen_SEGV_if_not_32_aligned(addr);
            assign(tD, loadLE(Ity_V256, mkexpr(addr)));
            DIP("vmovdq%c %s,%s\n", ch, dis_buf, nameYMMReg(rD));
         }
         putYMMReg(rD, mkexpr(tD));
         goto decode_success;
      }
      /* VMOVDQA xmm2/m128, xmm1 = VEX.128.66.0F.WIG 6F */
      /* VMOVDQU xmm2/m128, xmm1 = VEX.128.F3.0F.WIG 6F */
      if ((have66noF2noF3(pfx) || haveF3no66noF2(pfx))
          && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp tD    = newTemp(Ity_V128);
         Bool   isA   = have66noF2noF3(pfx);
         UChar  ch    = isA ? 'a' : 'u';
         if (epartIsReg(modrm)) {
            UInt rS = eregOfRexRM(pfx, modrm);
            delta += 1;
            assign(tD, getXMMReg(rS));
            DIP("vmovdq%c %s,%s\n", ch, nameXMMReg(rS), nameXMMReg(rD));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            if (isA)
               gen_SEGV_if_not_16_aligned(addr);
            assign(tD, loadLE(Ity_V128, mkexpr(addr)));
            DIP("vmovdq%c %s,%s\n", ch, dis_buf, nameXMMReg(rD));
         }
         putYMMRegLoAndZU(rD, mkexpr(tD));
         goto decode_success;
      }
      break;

   case 0x70:
      /* VPSHUFD imm8, xmm2/m128, xmm1 = VEX.128.66.0F.WIG 70 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PSHUFD_32x4( vbi, pfx, delta, True/*writesYmm*/);
         goto decode_success;
      }
      /* VPSHUFLW imm8, xmm2/m128, xmm1 = VEX.128.F2.0F.WIG 70 /r ib */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PSHUFxW_128( vbi, pfx, delta,
                                  True/*isAvx*/, False/*!xIsH*/ );
         goto decode_success;
      }
      /* VPSHUFHW imm8, xmm2/m128, xmm1 = VEX.128.F3.0F.WIG 70 /r ib */
      if (haveF3no66noF2(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PSHUFxW_128( vbi, pfx, delta,
                                  True/*isAvx*/, True/*xIsH*/ );
         goto decode_success;
      }
      break;

   case 0x71:
      /* VPSRLW imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 71 /2 ib */
      /* VPSRAW imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 71 /4 ib */
      /* VPSLLW imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 71 /6 ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         if (gregLO3ofRM(getUChar(delta)) == 2/*SRL*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsrlw", Iop_ShrN16x8 );
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 4/*SRA*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsraw", Iop_SarN16x8 );
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 6/*SLL*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsllw", Iop_ShlN16x8 );
            *uses_vvvv = True;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x72:
      /* VPSRLD imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 72 /2 ib */
      /* VPSRAD imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 72 /4 ib */
      /* VPSLLD imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 72 /6 ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         if (gregLO3ofRM(getUChar(delta)) == 2/*SRL*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsrld", Iop_ShrN32x4 );
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 4/*SRA*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsrad", Iop_SarN32x4 );
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 6/*SLL*/) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpslld", Iop_ShlN32x4 );
            *uses_vvvv = True;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x73:
      /* VPSRLDQ imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 73 /3 ib */
      /* VPSLLDQ imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 73 /7 ib */
      /* VPSRLQ  imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 73 /2 ib */
      /* VPSLLQ  imm8, xmm2, xmm1 = VEX.NDD.128.66.0F.WIG 73 /6 ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         Int    rS   = eregOfRexRM(pfx,getUChar(delta));
         Int    rD   = getVexNvvvv(pfx);
         IRTemp vecS = newTemp(Ity_V128);
         if (gregLO3ofRM(getUChar(delta)) == 3) {
            Int imm = (Int)getUChar(delta+1);
            DIP("vpsrldq $%d,%s,%s\n", imm, nameXMMReg(rS), nameXMMReg(rD));
            delta += 2;
            assign( vecS, getXMMReg(rS) );
            putYMMRegLoAndZU(rD, mkexpr(math_PSRLDQ( vecS, imm )));
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 7) {
            Int imm = (Int)getUChar(delta+1);
            DIP("vpslldq $%d,%s,%s\n", imm, nameXMMReg(rS), nameXMMReg(rD));
            delta += 2;
            assign( vecS, getXMMReg(rS) );
            putYMMRegLoAndZU(rD, mkexpr(math_PSLLDQ( vecS, imm )));
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 2) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsrlq", Iop_ShrN64x2 );
            *uses_vvvv = True;
            goto decode_success;
         }
         if (gregLO3ofRM(getUChar(delta)) == 6) {
            delta = dis_AVX128_shiftE_to_V_imm( pfx, delta,
                                                "vpsllq", Iop_ShlN64x2 );
            *uses_vvvv = True;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0x74:
      /* VPCMPEQB r/m, rV, r ::: r = rV `eq-by-8s` r/m */
      /* VPCMPEQB = VEX.NDS.128.66.0F.WIG 74 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpeqb", Iop_CmpEQ8x16 );
         goto decode_success;
      }
      break;

   case 0x75:
      /* VPCMPEQW r/m, rV, r ::: r = rV `eq-by-16s` r/m */
      /* VPCMPEQW = VEX.NDS.128.66.0F.WIG 75 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpeqw", Iop_CmpEQ16x8 );
         goto decode_success;
      }
      break;

   case 0x76:
      /* VPCMPEQD r/m, rV, r ::: r = rV `eq-by-32s` r/m */
      /* VPCMPEQD = VEX.NDS.128.66.0F.WIG 76 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpeqd", Iop_CmpEQ32x4 );
         goto decode_success;
      }
      break;

   case 0x77:
      /* VZEROUPPER = VEX.128.0F.WIG 77 */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Int i;
         IRTemp zero128 = newTemp(Ity_V128);
         assign(zero128, mkV128(0));
         for (i = 0; i < 16; i++) {
            putYMMRegLane128(i, 1, mkexpr(zero128));
         }
         DIP("vzeroupper\n");
         goto decode_success;
      }
      /* VZEROALL = VEX.256.0F.WIG 77 */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Int i;
         IRTemp zero128 = newTemp(Ity_V128);
         assign(zero128, mkV128(0));
         for (i = 0; i < 16; i++) {
            putYMMRegLoAndZU(i, mkexpr(zero128));
         }
         DIP("vzeroall\n");
         goto decode_success;
      }
      break;

   case 0x7C:
   case 0x7D:
      /* VHADDPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.F2.0F.WIG 7C /r */
      /* VHSUBPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.F2.0F.WIG 7D /r */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         IRTemp sV     = newTemp(Ity_V128);
         IRTemp dV     = newTemp(Ity_V128);
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         UChar modrm   = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         UInt   rV     = getVexNvvvv(pfx);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            DIP("vh%spd %s,%s,%s\n", str, nameXMMReg(rE),
                nameXMMReg(rV), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vh%spd %s,%s,%s\n", str, dis_buf,
                nameXMMReg(rV), nameXMMReg(rG));
            delta += alen;
         }
         assign( dV, getXMMReg(rV) );
         putYMMRegLoAndZU( rG, mkexpr( math_HADDPS_128 ( dV, sV, isAdd ) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VHADDPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.F2.0F.WIG 7C /r */
      /* VHSUBPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.F2.0F.WIG 7D /r */
      if (haveF2no66noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         IRTemp sV     = newTemp(Ity_V256);
         IRTemp dV     = newTemp(Ity_V256);
         IRTemp s1, s0, d1, d0;
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         UChar modrm   = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         UInt   rV     = getVexNvvvv(pfx);
         s1 = s0 = d1 = d0 = IRTemp_INVALID;
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getYMMReg(rE) );
            DIP("vh%spd %s,%s,%s\n", str, nameYMMReg(rE),
                nameYMMReg(rV), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vh%spd %s,%s,%s\n", str, dis_buf,
                nameYMMReg(rV), nameYMMReg(rG));
            delta += alen;
         }
         assign( dV, getYMMReg(rV) );
         breakupV256toV128s( dV, &d1, &d0 );
         breakupV256toV128s( sV, &s1, &s0 );
         putYMMReg( rG, binop(Iop_V128HLtoV256,
                              mkexpr( math_HADDPS_128 ( d1, s1, isAdd ) ),
                              mkexpr( math_HADDPS_128 ( d0, s0, isAdd ) ) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VHADDPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 7C /r */
      /* VHSUBPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG 7D /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         IRTemp sV     = newTemp(Ity_V128);
         IRTemp dV     = newTemp(Ity_V128);
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         UChar modrm   = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         UInt   rV     = getVexNvvvv(pfx);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            DIP("vh%spd %s,%s,%s\n", str, nameXMMReg(rE),
                nameXMMReg(rV), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            DIP("vh%spd %s,%s,%s\n", str, dis_buf,
                nameXMMReg(rV), nameXMMReg(rG));
            delta += alen;
         }
         assign( dV, getXMMReg(rV) );
         putYMMRegLoAndZU( rG, mkexpr( math_HADDPD_128 ( dV, sV, isAdd ) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VHADDPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 7C /r */
      /* VHSUBPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG 7D /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         IRTemp sV     = newTemp(Ity_V256);
         IRTemp dV     = newTemp(Ity_V256);
         IRTemp s1, s0, d1, d0;
         Bool   isAdd  = opc == 0x7C;
         HChar* str    = isAdd ? "add" : "sub";
         UChar modrm   = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx,modrm);
         UInt   rV     = getVexNvvvv(pfx);
         s1 = s0 = d1 = d0 = IRTemp_INVALID;
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getYMMReg(rE) );
            DIP("vh%spd %s,%s,%s\n", str, nameYMMReg(rE),
                nameYMMReg(rV), nameYMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V256, mkexpr(addr)) );
            DIP("vh%spd %s,%s,%s\n", str, dis_buf,
                nameYMMReg(rV), nameYMMReg(rG));
            delta += alen;
         }
         assign( dV, getYMMReg(rV) );
         breakupV256toV128s( dV, &d1, &d0 );
         breakupV256toV128s( sV, &s1, &s0 );
         putYMMReg( rG, binop(Iop_V128HLtoV256,
                              mkexpr( math_HADDPD_128 ( d1, s1, isAdd ) ),
                              mkexpr( math_HADDPD_128 ( d0, s0, isAdd ) ) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x7E:
      /* Note the Intel docs don't make sense for this.  I think they
         are wrong.  They seem to imply it is a store when in fact I
         think it is a load.  Also it's unclear whether this is W0, W1
         or WIG. */
      /* VMOVQ xmm2/m64, xmm1 = VEX.128.F3.0F.W0 7E /r */
      if (haveF3no66noF2(pfx) 
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         vassert(sz == 4); /* even tho we are transferring 8, not 4. */
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            putXMMRegLane64( rG, 0, getXMMRegLane64( rE, 0 ));
            DIP("vmovq %s,%s\n", nameXMMReg(rE), nameXMMReg(rG));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            putXMMRegLane64( rG, 0, loadLE(Ity_I64, mkexpr(addr)) );
            DIP("vmovq %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         /* zero bits 255:64 */
         putXMMRegLane64( rG, 1, mkU64(0) );
         putYMMRegLane128( rG, 1, mkV128(0) );
         goto decode_success;
      }
      /* VMOVQ xmm1, r64 = VEX.128.66.0F.W1 7E /r (reg case only) */
      /* Moves from G to E, so is a store-form insn */
      /* Intel docs list this in the VMOVD entry for some reason. */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 1==getRexW(pfx)/*W1*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            DIP("vmovq %s,%s\n", nameXMMReg(rG), nameIReg64(rE));
            putIReg64(rE, getXMMRegLane64(rG, 0));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMRegLane64(rG, 0) );
            DIP("vmovq %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      /* VMOVD xmm1, m32/r32 = VEX.128.66.0F.W0 7E /r (reg case only) */
      /* Moves from G to E, so is a store-form insn */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            DIP("vmovd %s,%s\n", nameXMMReg(rG), nameIReg32(rE));
            putIReg32(rE, getXMMRegLane32(rG, 0));
            delta += 1;
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMRegLane32(rG, 0) );
            DIP("vmovd %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
         }
         goto decode_success;
      }
      break;

   case 0x7F:
      /* VMOVDQA ymm1, ymm2/m256 = VEX.256.66.0F.WIG 7F */
      /* VMOVDQU ymm1, ymm2/m256 = VEX.256.F3.0F.WIG 7F */
      if ((have66noF2noF3(pfx) || haveF3no66noF2(pfx))
          && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   rS    = gregOfRexRM(pfx, modrm);
         IRTemp tS    = newTemp(Ity_V256);
         Bool   isA   = have66noF2noF3(pfx);
         UChar  ch    = isA ? 'a' : 'u';
         assign(tS, getYMMReg(rS));
         if (epartIsReg(modrm)) {
            UInt rD = eregOfRexRM(pfx, modrm);
            delta += 1;
            putYMMReg(rD, mkexpr(tS));
            DIP("vmovdq%c %s,%s\n", ch, nameYMMReg(rS), nameYMMReg(rD));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            if (isA)
               gen_SEGV_if_not_32_aligned(addr);
            storeLE(mkexpr(addr), mkexpr(tS));
            DIP("vmovdq%c %s,%s\n", ch, nameYMMReg(rS), dis_buf);
         }
         goto decode_success;
      }
      /* VMOVDQA xmm1, xmm2/m128 = VEX.128.66.0F.WIG 7F */
      /* VMOVDQU xmm1, xmm2/m128 = VEX.128.F3.0F.WIG 7F */
      if ((have66noF2noF3(pfx) || haveF3no66noF2(pfx))
          && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rS    = gregOfRexRM(pfx, modrm);
         IRTemp tS    = newTemp(Ity_V128);
         Bool   isA   = have66noF2noF3(pfx);
         UChar  ch    = isA ? 'a' : 'u';
         assign(tS, getXMMReg(rS));
         if (epartIsReg(modrm)) {
            UInt rD = eregOfRexRM(pfx, modrm);
            delta += 1;
            putYMMRegLoAndZU(rD, mkexpr(tS));
            DIP("vmovdq%c %s,%s\n", ch, nameXMMReg(rS), nameXMMReg(rD));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            if (isA)
               gen_SEGV_if_not_16_aligned(addr);
            storeLE(mkexpr(addr), mkexpr(tS));
            DIP("vmovdq%c %s,%s\n", ch, nameXMMReg(rS), dis_buf);
         }
         goto decode_success;
      }
      break;

   case 0xAE:
      /* VSTMXCSR m32 = VEX.LZ.0F.WIG AE /3 */
      if (haveNo66noF2noF3(pfx)
          && 0==getVexL(pfx)/*LZ*/
          && 0==getRexW(pfx) /* be paranoid -- Intel docs don't require this */
          && !epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 3
          && sz == 4) {
         delta = dis_STMXCSR(vbi, pfx, delta, True/*isAvx*/);
         goto decode_success;
      }
      /* VLDMXCSR m32 = VEX.LZ.0F.WIG AE /2 */
      if (haveNo66noF2noF3(pfx)
          && 0==getVexL(pfx)/*LZ*/
          && 0==getRexW(pfx) /* be paranoid -- Intel docs don't require this */
          && !epartIsReg(getUChar(delta)) && gregLO3ofRM(getUChar(delta)) == 2
          && sz == 4) {
         delta = dis_LDMXCSR(vbi, pfx, delta, True/*isAvx*/);
         goto decode_success;
      }
      break;

   case 0xC2:
      /* VCMPSD xmm3/m64(E=argL), xmm2(V=argR), xmm1(G) */
      /* = VEX.NDS.LIG.F2.0F.WIG C2 /r ib */
      if (haveF2no66noF3(pfx)) {
         Long delta0 = delta;
         delta = dis_AVX128_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmpsd", False/*!all_lanes*/,
                                          8/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      /* VCMPSS xmm3/m32(E=argL), xmm2(V=argR), xmm1(G) */
      /* = VEX.NDS.LIG.F3.0F.WIG C2 /r ib */
      if (haveF3no66noF2(pfx)) {
         Long delta0 = delta;
         delta = dis_AVX128_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmpss", False/*!all_lanes*/,
                                          4/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      /* VCMPPD xmm3/m128(E=argL), xmm2(V=argR), xmm1(G) */
      /* = VEX.NDS.128.66.0F.WIG C2 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Long delta0 = delta;
         delta = dis_AVX128_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmppd", True/*all_lanes*/,
                                          8/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      /* VCMPPD ymm3/m256(E=argL), ymm2(V=argR), ymm1(G) */
      /* = VEX.NDS.256.66.0F.WIG C2 /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Long delta0 = delta;
         delta = dis_AVX256_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmppd", 8/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      /* VCMPPS xmm3/m128(E=argL), xmm2(V=argR), xmm1(G) */
      /* = VEX.NDS.128.0F.WIG C2 /r ib */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Long delta0 = delta;
         delta = dis_AVX128_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmpps", True/*all_lanes*/,
                                          4/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      /* VCMPPS ymm3/m256(E=argL), ymm2(V=argR), ymm1(G) */
      /* = VEX.NDS.256.0F.WIG C2 /r ib */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Long delta0 = delta;
         delta = dis_AVX256_cmp_V_E_to_G( uses_vvvv, vbi, pfx, delta,
                                          "vcmpps", 4/*sz*/);
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      break;

   case 0xC4:
      /* VPINSRW r32/m16, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG C4 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         Int    imm8;
         IRTemp new16 = newTemp(Ity_I16);

         if ( epartIsReg( modrm ) ) {
            imm8 = (Int)(getUChar(delta+1) & 7);
            assign( new16, unop(Iop_32to16,
                                getIReg32(eregOfRexRM(pfx,modrm))) );
            delta += 1+1;
            DIP( "vpinsrw $%d,%s,%s\n", imm8,
                 nameIReg32( eregOfRexRM(pfx, modrm) ), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)(getUChar(delta+alen) & 7);
            assign( new16, loadLE( Ity_I16, mkexpr(addr) ));
            delta += alen+1;
            DIP( "vpinsrw $%d,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_PINSRW_128( src_vec, new16, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xC5:
      /* VPEXTRW imm8, xmm1, reg32 = VEX.128.66.0F.W0 C5 /r ib */
      if (have66noF2noF3(pfx)
         && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         Long delta0 = delta;
         delta = dis_PEXTRW_128_EregOnly_toG( vbi, pfx, delta,
                                              True/*isAvx*/ );
         if (delta > delta0) goto decode_success;
         /* else fall through -- decoding has failed */
      }
      break; 

   case 0xC6:
      /* VSHUFPS imm8, xmm3/m128, xmm2, xmm1, xmm2 */
      /* = VEX.NDS.128.0F.WIG C6 /r ib */
      if (haveNo66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Int    imm8 = 0;
         IRTemp eV   = newTemp(Ity_V128);
         IRTemp vV   = newTemp(Ity_V128);
         UInt  modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         UInt  rV    = getVexNvvvv(pfx);
         assign( vV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            imm8 = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("vshufps $%d,%s,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            imm8 = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("vshufps $%d,%s,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG));
         }
         IRTemp res = math_SHUFPS_128( eV, vV, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VSHUFPS imm8, ymm3/m256, ymm2, ymm1, ymm2 */
      /* = VEX.NDS.256.0F.WIG C6 /r ib */
      if (haveNo66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Int    imm8 = 0;
         IRTemp eV   = newTemp(Ity_V256);
         IRTemp vV   = newTemp(Ity_V256);
         UInt  modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         UInt  rV    = getVexNvvvv(pfx);
         assign( vV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getYMMReg(rE) );
            imm8 = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("vshufps $%d,%s,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( eV, loadLE(Ity_V256, mkexpr(addr)) );
            imm8 = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("vshufps $%d,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
         }
         IRTemp res = math_SHUFPS_256( eV, vV, imm8 );
         putYMMReg( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VSHUFPD imm8, xmm3/m128, xmm2, xmm1, xmm2 */
      /* = VEX.NDS.128.66.0F.WIG C6 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Int    imm8 = 0;
         IRTemp eV   = newTemp(Ity_V128);
         IRTemp vV   = newTemp(Ity_V128);
         UInt  modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         UInt  rV    = getVexNvvvv(pfx);
         assign( vV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getXMMReg(rE) );
            imm8 = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("vshufpd $%d,%s,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( eV, loadLE(Ity_V128, mkexpr(addr)) );
            imm8 = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("vshufpd $%d,%s,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG));
         }
         IRTemp res = math_SHUFPD_128( eV, vV, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VSHUFPD imm8, ymm3/m256, ymm2, ymm1, ymm2 */
      /* = VEX.NDS.256.66.0F.WIG C6 /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         Int    imm8 = 0;
         IRTemp eV   = newTemp(Ity_V256);
         IRTemp vV   = newTemp(Ity_V256);
         UInt  modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         UInt  rV    = getVexNvvvv(pfx);
         assign( vV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( eV, getYMMReg(rE) );
            imm8 = (Int)getUChar(delta+1);
            delta += 1+1;
            DIP("vshufpd $%d,%s,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( eV, loadLE(Ity_V256, mkexpr(addr)) );
            imm8 = (Int)getUChar(delta+alen);
            delta += 1+alen;
            DIP("vshufpd $%d,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
         }
         IRTemp res = math_SHUFPD_256( eV, vV, imm8 );
         putYMMReg( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xD0:
      /* VADDSUBPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D0 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vaddsubpd", math_ADDSUBPD_128 );
         goto decode_success;
      }
      /* VADDSUBPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F.WIG D0 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VEX_NDS_256_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vaddsubpd", math_ADDSUBPD_256 );
         goto decode_success;
      }
      /* VADDSUBPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.F2.0F.WIG D0 /r */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vaddsubps", math_ADDSUBPS_128 );
         goto decode_success;
      }
      /* VADDSUBPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.F2.0F.WIG D0 /r */
      if (haveF2no66noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VEX_NDS_256_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vaddsubps", math_ADDSUBPS_256 );
         goto decode_success;
      }
      break;

   case 0xD1:
      /* VPSRLW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D1 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsrlw", Iop_ShrN16x8 );
         *uses_vvvv = True;
         goto decode_success;
                        
      }
      break;

   case 0xD2:
      /* VPSRLD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D2 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsrld", Iop_ShrN32x4 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xD3:
      /* VPSRLQ xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D3 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsrlq", Iop_ShrN64x2 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xD4:
      /* VPADDQ r/m, rV, r ::: r = rV + r/m */
      /* VPADDQ = VEX.NDS.128.66.0F.WIG D4 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpaddq", Iop_Add64x2 );
         goto decode_success;
      }
      break;

   case 0xD5:
      /* VPMULLW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D5 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpmullw", Iop_Mul16x8 );
         goto decode_success;
      }
      break;

   case 0xD6:
      /* I can't even find any Intel docs for this one. */
      /* Basically: 66 0F D6 = MOVQ -- move 64 bits from G (lo half
         xmm) to E (mem or lo half xmm).  Looks like L==0(128), W==0
         (WIG, maybe?) */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && 0==getRexW(pfx)/*this might be redundant, dunno*/) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx,modrm);
         if (epartIsReg(modrm)) {
            /* fall through, awaiting test case */
            /* dst: lo half copied, hi half zeroed */
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            storeLE( mkexpr(addr), getXMMRegLane64( rG, 0 ));
            DIP("vmovq %s,%s\n", nameXMMReg(rG), dis_buf );
            delta += alen;
            goto decode_success;
         }
      }
      break;

   case 0xD7:
      /* VEX.128.66.0F.WIG D7 /r = VPMOVMSKB xmm1, r32 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVMSKB_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0xD8:
      /* VPSUBUSB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D8 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpsubusb", Iop_QSub8Ux16 );
         goto decode_success;
      }
     break;

   case 0xD9:
      /* VPSUBUSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG D9 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpsubusw", Iop_QSub16Ux8 );
         goto decode_success;
      }
      break;

   case 0xDA:
      /* VPMINUB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG DA /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpminub", Iop_Min8Ux16 );
         goto decode_success;
      }
      break;

   case 0xDB:
      /* VPAND r/m, rV, r ::: r = rV & r/m */
      /* VEX.NDS.128.66.0F.WIG DB /r = VPAND xmm3/m128, xmm2, xmm1 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpand", Iop_AndV128 );
         goto decode_success;
      }
      break;

   case 0xDC:
      /* VPADDUSB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG DC /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpaddusb", Iop_QAdd8Ux16 );
         goto decode_success;
      }
      break;

   case 0xDD:
      /* VPADDUSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG DD /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpaddusw", Iop_QAdd16Ux8 );
         goto decode_success;
      }
      break;

   case 0xDE:
      /* VPMAXUB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG DE /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpmaxub", Iop_Max8Ux16 );
         goto decode_success;
      }
      break;

   case 0xDF:
      /* VPANDN r/m, rV, r ::: r = rV & ~r/m (is that correct, re the ~ ?) */
      /* VEX.NDS.128.66.0F.WIG DF /r = VPANDN xmm3/m128, xmm2, xmm1 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpandn", Iop_AndV128,
                    NULL, True/*invertLeftArg*/, False/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0xE0:
      /* VPAVGB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E0 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpavgb", Iop_Avg8Ux16 );
         goto decode_success;
      }
      break;

   case 0xE1:
      /* VPSRAW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E1 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsraw", Iop_SarN16x8 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xE2:
      /* VPSRAD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E2 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsrad", Iop_SarN32x4 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xE3:
      /* VPAVGW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E3 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpavgw", Iop_Avg16Ux8 );
         goto decode_success;
      }
      break;

   case 0xE4:
      /* VPMULHUW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E4 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpmulhuw", Iop_MulHi16Ux8 );
         goto decode_success;
      }
      break;

   case 0xE5:
      /* VPMULHW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E5 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpmulhw", Iop_MulHi16Sx8 );
         goto decode_success;
      }
      break;

   case 0xE6:
      /* VCVTDQ2PD xmm2/m64, xmm1 = VEX.128.F3.0F.WIG E6 /r */
      if (haveF3no66noF2(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTDQ2PD_128(vbi, pfx, delta, True/*isAvx*/);
         goto decode_success;
      }
      /* VCVTDQ2PD xmm2/m128, ymm1 = VEX.256.F3.0F.WIG E6 /r */
      if (haveF3no66noF2(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTDQ2PD_256(vbi, pfx, delta);
         goto decode_success;
      }
      /* VCVTTPD2DQ xmm2/m128, xmm1 = VEX.128.66.0F.WIG E6 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTxPD2DQ_128(vbi, pfx, delta, True/*isAvx*/,
                                   True/*r2zero*/);
         goto decode_success;
      }
      /* VCVTTPD2DQ ymm2/m256, xmm1 = VEX.256.66.0F.WIG E6 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTxPD2DQ_256(vbi, pfx, delta, True/*r2zero*/);
         goto decode_success;
      }
      /* VCVTPD2DQ xmm2/m128, xmm1 = VEX.128.F2.0F.WIG E6 /r */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_CVTxPD2DQ_128(vbi, pfx, delta, True/*isAvx*/,
                                   False/*!r2zero*/);
         goto decode_success;
      }
      /* VCVTPD2DQ ymm2/m256, xmm1 = VEX.256.F2.0F.WIG E6 /r */
      if (haveF2no66noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_CVTxPD2DQ_256(vbi, pfx, delta, False/*!r2zero*/);
         goto decode_success;
      }
      break;

   case 0xE7:
      /* VMOVNTDQ xmm1, m128 = VEX.128.66.0F.WIG E7 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar modrm = getUChar(delta);
         UInt rG     = gregOfRexRM(pfx,modrm);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_16_aligned( addr );
            storeLE( mkexpr(addr), getXMMReg(rG) );
            DIP("vmovntdq %s,%s\n", dis_buf, nameXMMReg(rG));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      /* VMOVNTDQ ymm1, m256 = VEX.256.66.0F.WIG E7 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar modrm = getUChar(delta);
         UInt rG     = gregOfRexRM(pfx,modrm);
         if (!epartIsReg(modrm)) {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            gen_SEGV_if_not_32_aligned( addr );
            storeLE( mkexpr(addr), getYMMReg(rG) );
            DIP("vmovntdq %s,%s\n", dis_buf, nameYMMReg(rG));
            delta += alen;
            goto decode_success;
         }
         /* else fall through */
      }
      break;

   case 0xE8:
      /* VPSUBSB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E8 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpsubsb", Iop_QSub8Sx16 );
         goto decode_success;
      }
      break;

   case 0xE9:
      /* VPSUBSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG E9 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpsubsw", Iop_QSub16Sx8 );
         goto decode_success;
      }
      break;

   case 0xEA:
      /* VPMINSW r/m, rV, r ::: r = min-signed16s(rV, r/m) */
      /* VPMINSW = VEX.NDS.128.66.0F.WIG EA /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpminsw", Iop_Min16Sx8 );
         goto decode_success;
      }
      break;

   case 0xEB:
      /* VPOR r/m, rV, r ::: r = rV | r/m */
      /* VPOR = VEX.NDS.128.66.0F.WIG EB /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpor", Iop_OrV128 );
         goto decode_success;
      }
      break;

   case 0xEC:
      /* VPADDSB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG EC /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpaddsb", Iop_QAdd8Sx16 );
         goto decode_success;
      }
      break;

   case 0xED:
      /* VPADDSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG ED /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_V_to_G(
                    uses_vvvv, vbi, pfx, delta, "vpaddsw", Iop_QAdd16Sx8 );
         goto decode_success;
      }
      break;

   case 0xEE:
      /* VPMAXSW r/m, rV, r ::: r = max-signed16s(rV, r/m) */
      /* VPMAXSW = VEX.NDS.128.66.0F.WIG EE /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmaxsw", Iop_Max16Sx8 );
         goto decode_success;
      }
      break;

   case 0xEF:
      /* VPXOR r/m, rV, r ::: r = rV ^ r/m */
      /* VPXOR = VEX.NDS.128.66.0F.WIG EF /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpxor", Iop_XorV128 );
         goto decode_success;
      }
      break;

   case 0xF0:
      /* VLDDQU m256, ymm1 = VEX.256.F2.0F.WIG F0 /r */
      if (haveF2no66noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp tD    = newTemp(Ity_V256);
         if (epartIsReg(modrm)) break;
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         assign(tD, loadLE(Ity_V256, mkexpr(addr)));
         DIP("vlddqu %s,%s\n", dis_buf, nameYMMReg(rD));
         putYMMReg(rD, mkexpr(tD));
         goto decode_success;
      }
      /* VLDDQU m128, xmm1 = VEX.128.F2.0F.WIG F0 /r */
      if (haveF2no66noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp tD    = newTemp(Ity_V128);
         if (epartIsReg(modrm)) break;
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         assign(tD, loadLE(Ity_V128, mkexpr(addr)));
         DIP("vlddqu %s,%s\n", dis_buf, nameXMMReg(rD));
         putYMMRegLoAndZU(rD, mkexpr(tD));
         goto decode_success;
      }
      break;

   case 0xF1:
      /* VPSLLW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F1 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsllw", Iop_ShlN16x8 );
         *uses_vvvv = True;
         goto decode_success;
                        
      }
      break;

   case 0xF2:
      /* VPSLLD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F2 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpslld", Iop_ShlN32x4 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xF3:
      /* VPSLLQ xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F3 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_shiftV_byE( vbi, pfx, delta,
                                        "vpsllq", Iop_ShlN64x2 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0xF4:
      /* VPMULUDQ xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F4 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vpmuludq", math_PMULUDQ_128 );
         goto decode_success;
      }
      break;

   case 0xF5:
      /* VPMADDWD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F5 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vpmaddwd", math_PMADDWD_128 );
         goto decode_success;
      }
      break;

   case 0xF6:
      /* VPSADBW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F.WIG F6 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vpsadbw", math_PSADBW_128 );
         goto decode_success;
      }
      break;

   case 0xF7:
      /* VMASKMOVDQU xmm2, xmm1 = VEX.128.66.0F.WIG F7 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && epartIsReg(getUChar(delta))) {
         delta = dis_MASKMOVDQU( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0xF8:
      /* VPSUBB r/m, rV, r ::: r = rV - r/m */
      /* VPSUBB = VEX.NDS.128.66.0F.WIG F8 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpsubb", Iop_Sub8x16 );
         goto decode_success;
      }
      break;

   case 0xF9:
      /* VPSUBW r/m, rV, r ::: r = rV - r/m */
      /* VPSUBW = VEX.NDS.128.66.0F.WIG F9 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpsubw", Iop_Sub16x8 );
         goto decode_success;
      }
      break;

   case 0xFA:
      /* VPSUBD r/m, rV, r ::: r = rV - r/m */
      /* VPSUBD = VEX.NDS.128.66.0F.WIG FA /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpsubd", Iop_Sub32x4 );
         goto decode_success;
      }
      break;

   case 0xFB:
      /* VPSUBQ r/m, rV, r ::: r = rV - r/m */
      /* VPSUBQ = VEX.NDS.128.66.0F.WIG FB /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpsubq", Iop_Sub64x2 );
         goto decode_success;
      }
      break;

   case 0xFC:
      /* VPADDB r/m, rV, r ::: r = rV + r/m */
      /* VPADDB = VEX.NDS.128.66.0F.WIG FC /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpaddb", Iop_Add8x16 );
         goto decode_success;
      }
      break;

   case 0xFD:
      /* VPADDW r/m, rV, r ::: r = rV + r/m */
      /* VPADDW = VEX.NDS.128.66.0F.WIG FD /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpaddw", Iop_Add16x8 );
         goto decode_success;
      }
      break;

   case 0xFE:
      /* VPADDD r/m, rV, r ::: r = rV + r/m */
      /* VPADDD = VEX.NDS.128.66.0F.WIG FE /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpaddd", Iop_Add32x4 );
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   return deltaIN;

  decode_success:
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F38__VEX    ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static IRTemp math_PERMILPS_VAR_128 ( IRTemp dataV, IRTemp ctrlV )
{
   /* In the control vector, zero out all but the bottom two bits of
      each 32-bit lane. */
   IRExpr* cv1 = binop(Iop_ShrN32x4,
                       binop(Iop_ShlN32x4, mkexpr(ctrlV), mkU8(30)),
                       mkU8(30));
   /* And use the resulting cleaned-up control vector as steering
      in a Perm operation. */
   IRTemp res = newTemp(Ity_V128);
   assign(res, binop(Iop_Perm32x4, mkexpr(dataV), cv1));
   return res;
}

static IRTemp math_PERMILPS_VAR_256 ( IRTemp dataV, IRTemp ctrlV )
{
   IRTemp dHi, dLo, cHi, cLo;
   dHi = dLo = cHi = cLo = IRTemp_INVALID;
   breakupV256toV128s( dataV, &dHi, &dLo );
   breakupV256toV128s( ctrlV, &cHi, &cLo );
   IRTemp rHi = math_PERMILPS_VAR_128( dHi, cHi );
   IRTemp rLo = math_PERMILPS_VAR_128( dLo, cLo );
   IRTemp res = newTemp(Ity_V256);
   assign(res, binop(Iop_V128HLtoV256, mkexpr(rHi), mkexpr(rLo)));
   return res;
}

static IRTemp math_PERMILPD_VAR_128 ( IRTemp dataV, IRTemp ctrlV )
{
   /* No cleverness here .. */
   IRTemp dHi, dLo, cHi, cLo;
   dHi = dLo = cHi = cLo = IRTemp_INVALID;
   breakupV128to64s( dataV, &dHi, &dLo );
   breakupV128to64s( ctrlV, &cHi, &cLo );
   IRExpr* rHi
      = IRExpr_Mux0X( unop(Iop_64to8,
                           binop(Iop_And64, mkexpr(cHi), mkU64(2))),
                      mkexpr(dLo), mkexpr(dHi) );
   IRExpr* rLo
      = IRExpr_Mux0X( unop(Iop_64to8,
                           binop(Iop_And64, mkexpr(cLo), mkU64(2))),
                      mkexpr(dLo), mkexpr(dHi) );
   IRTemp res = newTemp(Ity_V128);
   assign(res, binop(Iop_64HLtoV128, rHi, rLo));
   return res;
}

static IRTemp math_PERMILPD_VAR_256 ( IRTemp dataV, IRTemp ctrlV )
{
   IRTemp dHi, dLo, cHi, cLo;
   dHi = dLo = cHi = cLo = IRTemp_INVALID;
   breakupV256toV128s( dataV, &dHi, &dLo );
   breakupV256toV128s( ctrlV, &cHi, &cLo );
   IRTemp rHi = math_PERMILPD_VAR_128( dHi, cHi );
   IRTemp rLo = math_PERMILPD_VAR_128( dLo, cLo );
   IRTemp res = newTemp(Ity_V256);
   assign(res, binop(Iop_V128HLtoV256, mkexpr(rHi), mkexpr(rLo)));
   return res;
}

__attribute__((noinline))
static
Long dis_ESC_0F38__VEX (
        /*MB_OUT*/DisResult* dres,
        /*OUT*/   Bool*      uses_vvvv,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   *uses_vvvv = False;

   switch (opc) {

   case 0x00:
      /* VPSHUFB r/m, rV, r ::: r = shuf(rV, r/m) */
      /* VPSHUFB = VEX.NDS.128.66.0F38.WIG 00 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta, "vpshufb", math_PSHUFB_XMM );
         goto decode_success;
      }
      break;

   case 0x01:
   case 0x02:
   case 0x03:
      /* VPHADDW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 01 /r */
      /* VPHADDD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 02 /r */
      /* VPHADDSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 03 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PHADD_128( vbi, pfx, delta, True/*isAvx*/, opc );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x04:
      /* VPMADDUBSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 04 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta, "vpmaddubsw",
                    math_PMADDUBSW_128 );
         goto decode_success;
      }
      break;
      
   case 0x05:
   case 0x06:
   case 0x07:
      /* VPHSUBW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 05 /r */
      /* VPHSUBD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 06 /r */
      /* VPHSUBSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 07 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PHADD_128( vbi, pfx, delta, True/*isAvx*/, opc );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x08:
   case 0x09:
   case 0x0A:
      /* VPSIGNB xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 08 /r */
      /* VPSIGNW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 09 /r */
      /* VPSIGND xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 0A /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         IRTemp sV      = newTemp(Ity_V128);
         IRTemp dV      = newTemp(Ity_V128);
         IRTemp sHi, sLo, dHi, dLo;
         sHi = sLo = dHi = dLo = IRTemp_INVALID;
         UChar  ch      = '?';
         Int    laneszB = 0;
         UChar  modrm   = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx,modrm);
         UInt   rV      = getVexNvvvv(pfx);

         switch (opc) {
            case 0x08: laneszB = 1; ch = 'b'; break;
            case 0x09: laneszB = 2; ch = 'w'; break;
            case 0x0A: laneszB = 4; ch = 'd'; break;
            default: vassert(0);
         }

         assign( dV, getXMMReg(rV) );

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("vpsign%c %s,%s,%s\n", ch, nameXMMReg(rE),
                nameXMMReg(rV), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("vpsign%c %s,%s,%s\n", ch, dis_buf,
                nameXMMReg(rV), nameXMMReg(rG));
         }

         breakupV128to64s( dV, &dHi, &dLo );
         breakupV128to64s( sV, &sHi, &sLo );

         putYMMRegLoAndZU(
            rG,
            binop(Iop_64HLtoV128,
                  dis_PSIGN_helper( mkexpr(sHi), mkexpr(dHi), laneszB ),
                  dis_PSIGN_helper( mkexpr(sLo), mkexpr(dLo), laneszB )
            )
         );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0B:
      /* VPMULHRSW xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 0B /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         IRTemp sV      = newTemp(Ity_V128);
         IRTemp dV      = newTemp(Ity_V128);
         IRTemp sHi, sLo, dHi, dLo;
         sHi = sLo = dHi = dLo = IRTemp_INVALID;
         UChar  modrm   = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx,modrm);
         UInt   rV      = getVexNvvvv(pfx);

         assign( dV, getXMMReg(rV) );

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx,modrm);
            assign( sV, getXMMReg(rE) );
            delta += 1;
            DIP("vpmulhrsw %s,%s,%s\n", nameXMMReg(rE),
                nameXMMReg(rV), nameXMMReg(rG));
         } else {
            addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            delta += alen;
            DIP("vpmulhrsw %s,%s,%s\n", dis_buf,
                nameXMMReg(rV), nameXMMReg(rG));
         }

         breakupV128to64s( dV, &dHi, &dLo );
         breakupV128to64s( sV, &sHi, &sLo );

         putYMMRegLoAndZU(
            rG,
            binop(Iop_64HLtoV128,
                  dis_PMULHRSW_helper( mkexpr(sHi), mkexpr(dHi) ),
                  dis_PMULHRSW_helper( mkexpr(sLo), mkexpr(dLo) )
            )
         );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0C:
      /* VPERMILPS xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.W0 0C /r */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp ctrlV = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            DIP("vpermilps %s,%s,%s\n",
                nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
            assign(ctrlV, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            DIP("vpermilps %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rG));
            assign(ctrlV, loadLE(Ity_V128, mkexpr(addr)));
         }
         IRTemp dataV = newTemp(Ity_V128);
         assign(dataV, getXMMReg(rV));
         IRTemp resV = math_PERMILPS_VAR_128(dataV, ctrlV);
         putYMMRegLoAndZU(rG, mkexpr(resV));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VPERMILPS ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F38.W0 0C /r */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp ctrlV = newTemp(Ity_V256);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            DIP("vpermilps %s,%s,%s\n",
                nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
            assign(ctrlV, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            DIP("vpermilps %s,%s,%s\n",
                dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(ctrlV, loadLE(Ity_V256, mkexpr(addr)));
         }
         IRTemp dataV = newTemp(Ity_V256);
         assign(dataV, getYMMReg(rV));
         IRTemp resV = math_PERMILPS_VAR_256(dataV, ctrlV);
         putYMMReg(rG, mkexpr(resV));
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0D:
      /* VPERMILPD xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.W0 0D /r */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp ctrlV = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            DIP("vpermilpd %s,%s,%s\n",
                nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
            assign(ctrlV, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            DIP("vpermilpd %s,%s,%s\n",
                dis_buf, nameXMMReg(rV), nameXMMReg(rG));
            assign(ctrlV, loadLE(Ity_V128, mkexpr(addr)));
         }
         IRTemp dataV = newTemp(Ity_V128);
         assign(dataV, getXMMReg(rV));
         IRTemp resV = math_PERMILPD_VAR_128(dataV, ctrlV);
         putYMMRegLoAndZU(rG, mkexpr(resV));
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VPERMILPD ymm3/m256, ymm2, ymm1 = VEX.NDS.256.66.0F38.W0 0D /r */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp ctrlV = newTemp(Ity_V256);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            DIP("vpermilpd %s,%s,%s\n",
                nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
            assign(ctrlV, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
            delta += alen;
            DIP("vpermilpd %s,%s,%s\n",
                dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(ctrlV, loadLE(Ity_V256, mkexpr(addr)));
         }
         IRTemp dataV = newTemp(Ity_V256);
         assign(dataV, getYMMReg(rV));
         IRTemp resV = math_PERMILPD_VAR_256(dataV, ctrlV);
         putYMMReg(rG, mkexpr(resV));
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0E:
      /* VTESTPS xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 0E /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_xTESTy_128( vbi, pfx, delta, True/*isAvx*/, 32 );
         goto decode_success;
      }
      /* VTESTPS ymm2/m256, ymm1 = VEX.256.66.0F38.WIG 0E /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_xTESTy_256( vbi, pfx, delta, 32 );
         goto decode_success;
      }
      break;

   case 0x0F:
      /* VTESTPD xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 0F /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_xTESTy_128( vbi, pfx, delta, True/*isAvx*/, 64 );
         goto decode_success;
      }
      /* VTESTPD ymm2/m256, ymm1 = VEX.256.66.0F38.WIG 0F /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_xTESTy_256( vbi, pfx, delta, 64 );
         goto decode_success;
      }
      break;

   case 0x17:
      /* VPTEST xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 17 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_xTESTy_128( vbi, pfx, delta, True/*isAvx*/, 0 );
         goto decode_success;
      }
      /* VPTEST ymm2/m256, ymm1 = VEX.256.66.0F38.WIG 17 /r */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_xTESTy_256( vbi, pfx, delta, 0 );
         goto decode_success;
      }
      break;

   case 0x18:
      /* VBROADCASTSS m32, xmm1 = VEX.128.66.0F38.WIG 18 /r */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/
          && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vbroadcastss %s,%s\n", dis_buf, nameXMMReg(rG));
         IRTemp t32 = newTemp(Ity_I32);
         assign(t32, loadLE(Ity_I32, mkexpr(addr)));
         IRTemp t64 = newTemp(Ity_I64);
         assign(t64, binop(Iop_32HLto64, mkexpr(t32), mkexpr(t32)));
         IRExpr* res = binop(Iop_64HLtoV128, mkexpr(t64), mkexpr(t64));
         putYMMRegLoAndZU(rG, res);
         goto decode_success;
      }
      /* VBROADCASTSS m32, ymm1 = VEX.256.66.0F38.WIG 18 /r */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/
          && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vbroadcastss %s,%s\n", dis_buf, nameYMMReg(rG));
         IRTemp t32 = newTemp(Ity_I32);
         assign(t32, loadLE(Ity_I32, mkexpr(addr)));
         IRTemp t64 = newTemp(Ity_I64);
         assign(t64, binop(Iop_32HLto64, mkexpr(t32), mkexpr(t32)));
         IRExpr* res = IRExpr_Qop(Iop_64x4toV256, mkexpr(t64), mkexpr(t64),
                                                  mkexpr(t64), mkexpr(t64));
         putYMMReg(rG, res);
         goto decode_success;
      }
      break;

   case 0x19:
      /* VBROADCASTSD m64, ymm1 = VEX.256.66.0F38.WIG 19 /r */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/
          && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vbroadcastsd %s,%s\n", dis_buf, nameYMMReg(rG));
         IRTemp t64 = newTemp(Ity_I64);
         assign(t64, loadLE(Ity_I64, mkexpr(addr)));
         IRExpr* res = IRExpr_Qop(Iop_64x4toV256, mkexpr(t64), mkexpr(t64),
                                                  mkexpr(t64), mkexpr(t64));
         putYMMReg(rG, res);
         goto decode_success;
      }
      break;

   case 0x1A:
      /* VBROADCASTF128 m128, ymm1 = VEX.256.66.0F38.WIG 1A /r */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/
          && !epartIsReg(getUChar(delta))) {
         UChar modrm = getUChar(delta);
         UInt  rG    = gregOfRexRM(pfx, modrm);
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         DIP("vbroadcastf128 %s,%s\n", dis_buf, nameYMMReg(rG));
         IRTemp t128 = newTemp(Ity_V128);
         assign(t128, loadLE(Ity_V128, mkexpr(addr)));
         putYMMReg( rG, binop(Iop_V128HLtoV256, mkexpr(t128), mkexpr(t128)) );
         goto decode_success;
      }
      break;

   case 0x1C:
      /* VPABSB xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 1C /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary(
                    uses_vvvv, vbi, pfx, delta,
                    "vpabsb", math_PABS_XMM_pap1 );
         goto decode_success;
      }
      break;

   case 0x1D:
      /* VPABSW xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 1D /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary(
                    uses_vvvv, vbi, pfx, delta,
                    "vpabsw", math_PABS_XMM_pap2 );
         goto decode_success;
      }
      break;

   case 0x1E:
      /* VPABSD xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 1E /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AVX128_E_to_G_unary(
                    uses_vvvv, vbi, pfx, delta,
                    "vpabsd", math_PABS_XMM_pap4 );
         goto decode_success;
      }
      break;

   case 0x20:
      /* VPMOVSXBW xmm2/m64, xmm1 */
      /* VPMOVSXBW = VEX.128.66.0F38.WIG 20 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXBW_128( vbi, pfx, delta,
                                   True/*isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x21:
      /* VPMOVSXBD xmm2/m32, xmm1 */
      /* VPMOVSXBD = VEX.128.66.0F38.WIG 21 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXBD_128( vbi, pfx, delta,
                                   True/*isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x22:
      /* VPMOVSXBQ xmm2/m16, xmm1 */
      /* VPMOVSXBQ = VEX.128.66.0F38.WIG 22 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVSXBQ_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x23:
      /* VPMOVSXWD xmm2/m64, xmm1 = VEX.128.66.0F38.WIG 23 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXWD_128( vbi, pfx, delta,
                                   True/*isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x24:
      /* VPMOVSXWQ xmm2/m32, xmm1 = VEX.128.66.0F38.WIG 24 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVSXWQ_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x25:
      /* VPMOVSXDQ xmm2/m64, xmm1 = VEX.128.66.0F38.WIG 25 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXDQ_128( vbi, pfx, delta,
                                   True/*isAvx*/, False/*!xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x28:
      /* VPMULDQ xmm3/m128, xmm2, xmm1 = VEX.NDS.128.66.0F38.WIG 28 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_complex(
                    uses_vvvv, vbi, pfx, delta,
                    "vpmuldq", math_PMULDQ_128 );
         goto decode_success;
      }
      break;

   case 0x29:
      /* VPCMPEQQ r/m, rV, r ::: r = rV `eq-by-64s` r/m */
      /* VPCMPEQQ = VEX.NDS.128.66.0F38.WIG 29 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpeqq", Iop_CmpEQ64x2 );
         goto decode_success;
      }
      break;

   case 0x2A:
      /* VMOVNTDQA m128, xmm1 = VEX.128.66.0F38.WIG 2A /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/
          && !epartIsReg(getUChar(delta))) {
         UChar  modrm = getUChar(delta);
         UInt   rD    = gregOfRexRM(pfx, modrm);
         IRTemp tD    = newTemp(Ity_V128);
         addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;
         gen_SEGV_if_not_16_aligned(addr);
         assign(tD, loadLE(Ity_V128, mkexpr(addr)));
         DIP("vmovntdqa %s,%s\n", dis_buf, nameXMMReg(rD));
         putYMMRegLoAndZU(rD, mkexpr(tD));
         goto decode_success;
      }
      break;

   case 0x2B:
      /* VPACKUSDW r/m, rV, r ::: r = QNarrowBin32Sto16Ux8(rV, r/m) */
      /* VPACKUSDW = VEX.NDS.128.66.0F38.WIG 2B /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG(
                    uses_vvvv, vbi, pfx, delta, "vpackusdw",
                    Iop_QNarrowBin32Sto16Ux8, NULL,
                    False/*!invertLeftArg*/, True/*swapArgs*/ );
         goto decode_success;
      }
      break;

   case 0x30:
      /* VPMOVZXBW xmm2/m64, xmm1 */
      /* VPMOVZXBW = VEX.128.66.0F38.WIG 30 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXBW_128( vbi, pfx, delta,
                                   True/*isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x31:
      /* VPMOVZXBD xmm2/m32, xmm1 */
      /* VPMOVZXBD = VEX.128.66.0F38.WIG 31 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXBD_128( vbi, pfx, delta,
                                   True/*isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x32:
      /* VPMOVZXBQ xmm2/m16, xmm1 */
      /* VPMOVZXBQ = VEX.128.66.0F38.WIG 32 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVZXBQ_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x33:
      /* VPMOVZXWD xmm2/m64, xmm1 */
      /* VPMOVZXWD = VEX.128.66.0F38.WIG 33 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXWD_128( vbi, pfx, delta,
                                   True/*isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x34:
      /* VPMOVZXWQ xmm2/m32, xmm1 = VEX.128.66.0F38.WIG 34 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVZXWQ_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x35:
      /* VPMOVZXDQ xmm2/m64, xmm1 = VEX.128.66.0F38.WIG 35 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PMOVxXDQ_128( vbi, pfx, delta,
                                   True/*isAvx*/, True/*xIsZ*/ );
         goto decode_success;
      }
      break;

   case 0x37:
      /* VPCMPGTQ r/m, rV, r ::: r = rV `>s-by-64s` r/m */
      /* VPCMPGTQ = VEX.NDS.128.66.0F38.WIG 37 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpcmpgtq", Iop_CmpGT64Sx2 );
         goto decode_success;
      }
      break;

   case 0x38:
      /* VPMINSB r/m, rV, r ::: r = min-signed-8s(rV, r/m) */
      /* VPMINSB = VEX.NDS.128.66.0F38.WIG 38 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpminsb", Iop_Min8Sx16 );
         goto decode_success;
      }
      break;

   case 0x39:
      /* VPMINSD r/m, rV, r ::: r = min-signed-32s(rV, r/m) */
      /* VPMINSD = VEX.NDS.128.66.0F38.WIG 39 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpminsd", Iop_Min32Sx4 );
         goto decode_success;
      }
      break;

   case 0x3A:
      /* VPMINUW r/m, rV, r ::: r = min-unsigned-16s(rV, r/m) */
      /* VPMINUW = VEX.NDS.128.66.0F38.WIG 3A /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpminuw", Iop_Min16Ux8 );
         goto decode_success;
      }
      break;

   case 0x3B:
      /* VPMINUD r/m, rV, r ::: r = min-unsigned-32s(rV, r/m) */
      /* VPMINUD = VEX.NDS.128.66.0F38.WIG 3B /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpminud", Iop_Min32Ux4 );
         goto decode_success;
      }
      break;

   case 0x3C:
      /* VPMAXSB r/m, rV, r ::: r = max-signed-8s(rV, r/m) */
      /* VPMAXSB = VEX.NDS.128.66.0F38.WIG 3C /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmaxsb", Iop_Max8Sx16 );
         goto decode_success;
      }
      break;

   case 0x3D:
      /* VPMAXSD r/m, rV, r ::: r = max-signed-32s(rV, r/m) */
      /* VPMAXSD = VEX.NDS.128.66.0F38.WIG 3D /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmaxsd", Iop_Max32Sx4 );
         goto decode_success;
      }
      break;

   case 0x3E:
      /* VPMAXUW r/m, rV, r ::: r = max-unsigned-16s(rV, r/m) */
      /* VPMAXUW = VEX.NDS.128.66.0F38.WIG 3E /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmaxuw", Iop_Max16Ux8 );
         goto decode_success;
      }
      break;

   case 0x3F:
      /* VPMAXUD r/m, rV, r ::: r = max-unsigned-32s(rV, r/m) */
      /* VPMAXUD = VEX.NDS.128.66.0F38.WIG 3F /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmaxud", Iop_Max32Ux4 );
         goto decode_success;
      }
      break;

   case 0x40:
      /* VPMULLD r/m, rV, r ::: r = mul-32s(rV, r/m) */
      /* VPMULLD = VEX.NDS.128.66.0F38.WIG 40 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VEX_NDS_128_AnySimdPfx_0F_WIG_simple(
                    uses_vvvv, vbi, pfx, delta, "vpmulld", Iop_Mul32x4 );
         goto decode_success;
      }
      break;

   case 0x41:
      /* VPHMINPOSUW xmm2/m128, xmm1 = VEX.128.66.0F38.WIG 41 /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_PHMINPOSUW_128( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      } 
      break;

   case 0xDB:
   case 0xDC:
   case 0xDD:
   case 0xDE:
   case 0xDF:
      /* VAESIMC xmm2/m128, xmm1 = VEX.128.66.0F38.WIG DB /r */
      /* VAESENC xmm3/m128, xmm2, xmm1 = VEX.128.66.0F38.WIG DC /r */
      /* VAESENCLAST xmm3/m128, xmm2, xmm1 = VEX.128.66.0F38.WIG DD /r */
      /* VAESDEC xmm3/m128, xmm2, xmm1 = VEX.128.66.0F38.WIG DE /r */
      /* VAESDECLAST xmm3/m128, xmm2, xmm1 = VEX.128.66.0F38.WIG DF /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AESx( vbi, pfx, delta, True/*!isAvx*/, opc );
         if (opc != 0xDB) *uses_vvvv = True;
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   return deltaIN;

  decode_success:
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Top-level post-escape decoders: dis_ESC_0F3A__VEX    ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

static IRTemp math_VPERMILPS_128 ( IRTemp sV, UInt imm8 )
{
   vassert(imm8 < 256);
   IRTemp s3, s2, s1, s0;
   s3 = s2 = s1 = s0 = IRTemp_INVALID;
   breakupV128to32s( sV, &s3, &s2, &s1, &s0 );
#  define SEL(_nn) (((_nn)==0) ? s0 : ((_nn)==1) ? s1 \
                                    : ((_nn)==2) ? s2 : s3)
   IRTemp res = newTemp(Ity_V128);
   assign(res, mkV128from32s( SEL((imm8 >> 6) & 3),
                              SEL((imm8 >> 4) & 3),
                              SEL((imm8 >> 2) & 3),
                              SEL((imm8 >> 0) & 3) ));
#  undef SEL
   return res;
}

__attribute__((noinline))
static
Long dis_ESC_0F3A__VEX (
        /*MB_OUT*/DisResult* dres,
        /*OUT*/   Bool*      uses_vvvv,
        Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
        Bool         resteerCisOk,
        void*        callback_opaque,
        VexArchInfo* archinfo,
        VexAbiInfo*  vbi,
        Prefix pfx, Int sz, Long deltaIN 
     )
{
   IRTemp addr  = IRTemp_INVALID;
   Int    alen  = 0;
   HChar  dis_buf[50];
   Long   delta = deltaIN;
   UChar  opc   = getUChar(delta);
   delta++;
   *uses_vvvv = False;

   switch (opc) {

   case 0x04:
      /* VPERMILPS imm8, ymm2/m256, ymm1 = VEX.256.66.0F3A.WIG 04 /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8  = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp sV    = newTemp(Ity_V256);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vpermilps $%u,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rG));
            assign(sV, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vpermilps $%u,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rG));
            assign(sV, loadLE(Ity_V256, mkexpr(addr)));
         }
         delta++;
         IRTemp  sVhi = IRTemp_INVALID, sVlo = IRTemp_INVALID;
         breakupV256toV128s( sV, &sVhi, &sVlo );
         IRTemp  dVhi = math_VPERMILPS_128( sVhi, imm8 );
         IRTemp  dVlo = math_VPERMILPS_128( sVlo, imm8 );
         IRExpr* res  = binop(Iop_V128HLtoV256, mkexpr(dVhi), mkexpr(dVlo));
         putYMMReg(rG, res);
         goto decode_success;
      }
      /* VPERMILPS imm8, xmm2/m128, xmm1 = VEX.128.66.0F3A.WIG 04 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8  = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp sV    = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vpermilps $%u,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rG));
            assign(sV, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vpermilps $%u,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rG));
            assign(sV, loadLE(Ity_V128, mkexpr(addr)));
         }
         delta++;
         putYMMRegLoAndZU(rG, mkexpr ( math_VPERMILPS_128 ( sV, imm8 ) ) );
         goto decode_success;
      }
      break;

   case 0x05:
      /* VPERMILPD imm8, xmm2/m128, xmm1 = VEX.128.66.0F3A.WIG 05 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8  = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp sV    = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vpermilpd $%u,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rG));
            assign(sV, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vpermilpd $%u,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rG));
            assign(sV, loadLE(Ity_V128, mkexpr(addr)));
         }
         delta++;
         IRTemp s1 = newTemp(Ity_I64);
         IRTemp s0 = newTemp(Ity_I64);
         assign(s1, unop(Iop_V128HIto64, mkexpr(sV)));
         assign(s0, unop(Iop_V128to64,   mkexpr(sV)));
         IRTemp dV = newTemp(Ity_V128);
         assign(dV, binop(Iop_64HLtoV128,
                               mkexpr((imm8 & (1<<1)) ? s1 : s0),
                               mkexpr((imm8 & (1<<0)) ? s1 : s0)));
         putYMMRegLoAndZU(rG, mkexpr(dV));
         goto decode_success;
      }
      /* VPERMILPD imm8, ymm2/m256, ymm1 = VEX.256.66.0F3A.WIG 05 /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8  = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp sV    = newTemp(Ity_V256);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vpermilpd $%u,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rG));
            assign(sV, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vpermilpd $%u,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rG));
            assign(sV, loadLE(Ity_V256, mkexpr(addr)));
         }
         delta++;
         IRTemp s3, s2, s1, s0;
         s3 = s2 = s1 = s0 = IRTemp_INVALID;
         breakupV256to64s(sV, &s3, &s2, &s1, &s0);
         IRTemp dV = newTemp(Ity_V256);
         assign(dV, IRExpr_Qop(Iop_64x4toV256,
                               mkexpr((imm8 & (1<<3)) ? s3 : s2),
                               mkexpr((imm8 & (1<<2)) ? s3 : s2),
                               mkexpr((imm8 & (1<<1)) ? s1 : s0),
                               mkexpr((imm8 & (1<<0)) ? s1 : s0)));
         putYMMReg(rG, mkexpr(dV));
         goto decode_success;
      }
      break;

   case 0x06:
      /* VPERM2F128 imm8, ymm3/m256, ymm2, ymm1 = VEX.NDS.66.0F3A.W0 06 /r ib */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8  = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp s00   = newTemp(Ity_V128);
         IRTemp s01   = newTemp(Ity_V128);
         IRTemp s10   = newTemp(Ity_V128);
         IRTemp s11   = newTemp(Ity_V128);
         assign(s00, getYMMRegLane128(rV, 0));
         assign(s01, getYMMRegLane128(rV, 1));
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vperm2f128 $%u,%s,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
            assign(s10, getYMMRegLane128(rE, 0));
            assign(s11, getYMMRegLane128(rE, 1));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vperm2f128 $%u,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(s10, loadLE(Ity_V128, binop(Iop_Add64,
                                               mkexpr(addr), mkU64(0))));
            assign(s11, loadLE(Ity_V128, binop(Iop_Add64,
                                               mkexpr(addr), mkU64(16))));
         }
         delta++;
#        define SEL(_nn) (((_nn)==0) ? s00 : ((_nn)==1) ? s01 \
                                           : ((_nn)==2) ? s10 : s11)
         putYMMRegLane128(rG, 0, mkexpr(SEL((imm8 >> 0) & 3)));
         putYMMRegLane128(rG, 1, mkexpr(SEL((imm8 >> 4) & 3)));
#        undef SEL
         if (imm8 & (1<<3)) putYMMRegLane128(rG, 0, mkV128(0));
         if (imm8 & (1<<7)) putYMMRegLane128(rG, 1, mkV128(0));
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x08:
      /* VROUNDPS imm8, xmm2/m128, xmm1 */
      /* VROUNDPS = VEX.NDS.128.66.0F3A.WIG 08 ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp src   = newTemp(Ity_V128);
         IRTemp s0    = IRTemp_INVALID;
         IRTemp s1    = IRTemp_INVALID;
         IRTemp s2    = IRTemp_INVALID;
         IRTemp s3    = IRTemp_INVALID;
         IRTemp rm    = newTemp(Ity_I32);
         Int    imm   = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            assign( src, getXMMReg( rE ) );
            imm = getUChar(delta+1);
            if (imm & ~15) break;
            delta += 1+1;
            DIP( "vroundps $%d,%s,%s\n", imm, nameXMMReg(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE(Ity_V128, mkexpr(addr) ) );
            imm = getUChar(delta+alen);
            if (imm & ~15) break;
            delta += alen+1;
            DIP( "vroundps $%d,%s,%s\n", imm, dis_buf, nameXMMReg(rG) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         breakupV128to32s( src, &s3, &s2, &s1, &s0 );
         putYMMRegLane128( rG, 1, mkV128(0) );
#        define CVT(s) binop(Iop_RoundF32toInt, mkexpr(rm), \
                             unop(Iop_ReinterpI32asF32, mkexpr(s)))
         putYMMRegLane32F( rG, 3, CVT(s3) );
         putYMMRegLane32F( rG, 2, CVT(s2) );
         putYMMRegLane32F( rG, 1, CVT(s1) );
         putYMMRegLane32F( rG, 0, CVT(s0) );
#        undef CVT
         goto decode_success;
      }
      /* VROUNDPS imm8, ymm2/m256, ymm1 */
      /* VROUNDPS = VEX.NDS.256.66.0F3A.WIG 08 ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp src   = newTemp(Ity_V256);
         IRTemp s0    = IRTemp_INVALID;
         IRTemp s1    = IRTemp_INVALID;
         IRTemp s2    = IRTemp_INVALID;
         IRTemp s3    = IRTemp_INVALID;
         IRTemp s4    = IRTemp_INVALID;
         IRTemp s5    = IRTemp_INVALID;
         IRTemp s6    = IRTemp_INVALID;
         IRTemp s7    = IRTemp_INVALID;
         IRTemp rm    = newTemp(Ity_I32);
         Int    imm   = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            assign( src, getYMMReg( rE ) );
            imm = getUChar(delta+1);
            if (imm & ~15) break;
            delta += 1+1;
            DIP( "vroundps $%d,%s,%s\n", imm, nameYMMReg(rE), nameYMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE(Ity_V256, mkexpr(addr) ) );
            imm = getUChar(delta+alen);
            if (imm & ~15) break;
            delta += alen+1;
            DIP( "vroundps $%d,%s,%s\n", imm, dis_buf, nameYMMReg(rG) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         breakupV256to32s( src, &s7, &s6, &s5, &s4, &s3, &s2, &s1, &s0 );
#        define CVT(s) binop(Iop_RoundF32toInt, mkexpr(rm), \
                             unop(Iop_ReinterpI32asF32, mkexpr(s)))
         putYMMRegLane32F( rG, 7, CVT(s7) );
         putYMMRegLane32F( rG, 6, CVT(s6) );
         putYMMRegLane32F( rG, 5, CVT(s5) );
         putYMMRegLane32F( rG, 4, CVT(s4) );
         putYMMRegLane32F( rG, 3, CVT(s3) );
         putYMMRegLane32F( rG, 2, CVT(s2) );
         putYMMRegLane32F( rG, 1, CVT(s1) );
         putYMMRegLane32F( rG, 0, CVT(s0) );
#        undef CVT
         goto decode_success;
      }

   case 0x09:
      /* VROUNDPD imm8, xmm2/m128, xmm1 */
      /* VROUNDPD = VEX.NDS.128.66.0F3A.WIG 09 ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp src   = newTemp(Ity_V128);
         IRTemp s0    = IRTemp_INVALID;
         IRTemp s1    = IRTemp_INVALID;
         IRTemp rm    = newTemp(Ity_I32);
         Int    imm   = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            assign( src, getXMMReg( rE ) );
            imm = getUChar(delta+1);
            if (imm & ~15) break;
            delta += 1+1;
            DIP( "vroundpd $%d,%s,%s\n", imm, nameXMMReg(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE(Ity_V128, mkexpr(addr) ) );
            imm = getUChar(delta+alen);
            if (imm & ~15) break;
            delta += alen+1;
            DIP( "vroundpd $%d,%s,%s\n", imm, dis_buf, nameXMMReg(rG) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         breakupV128to64s( src, &s1, &s0 );
         putYMMRegLane128( rG, 1, mkV128(0) );
#        define CVT(s) binop(Iop_RoundF64toInt, mkexpr(rm), \
                             unop(Iop_ReinterpI64asF64, mkexpr(s)))
         putYMMRegLane64F( rG, 1, CVT(s1) );
         putYMMRegLane64F( rG, 0, CVT(s0) );
#        undef CVT
         goto decode_success;
      }
      /* VROUNDPD imm8, ymm2/m256, ymm1 */
      /* VROUNDPD = VEX.NDS.256.66.0F3A.WIG 09 ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         IRTemp src   = newTemp(Ity_V256);
         IRTemp s0    = IRTemp_INVALID;
         IRTemp s1    = IRTemp_INVALID;
         IRTemp s2    = IRTemp_INVALID;
         IRTemp s3    = IRTemp_INVALID;
         IRTemp rm    = newTemp(Ity_I32);
         Int    imm   = 0;

         modrm = getUChar(delta);

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            assign( src, getYMMReg( rE ) );
            imm = getUChar(delta+1);
            if (imm & ~15) break;
            delta += 1+1;
            DIP( "vroundpd $%d,%s,%s\n", imm, nameYMMReg(rE), nameYMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE(Ity_V256, mkexpr(addr) ) );
            imm = getUChar(delta+alen);
            if (imm & ~15) break;
            delta += alen+1;
            DIP( "vroundps $%d,%s,%s\n", imm, dis_buf, nameYMMReg(rG) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(rm, (imm & 4) ? get_sse_roundingmode() : mkU32(imm & 3));

         breakupV256to64s( src, &s3, &s2, &s1, &s0 );
#        define CVT(s) binop(Iop_RoundF64toInt, mkexpr(rm), \
                             unop(Iop_ReinterpI64asF64, mkexpr(s)))
         putYMMRegLane64F( rG, 3, CVT(s3) );
         putYMMRegLane64F( rG, 2, CVT(s2) );
         putYMMRegLane64F( rG, 1, CVT(s1) );
         putYMMRegLane64F( rG, 0, CVT(s0) );
#        undef CVT
         goto decode_success;
      }

   case 0x0A:
   case 0x0B:
      /* VROUNDSS imm8, xmm3/m32, xmm2, xmm1 */
      /* VROUNDSS = VEX.NDS.128.66.0F3A.WIG 0A ib */
      /* VROUNDSD imm8, xmm3/m64, xmm2, xmm1 */
      /* VROUNDSD = VEX.NDS.128.66.0F3A.WIG 0B ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         Bool   isD   = opc == 0x0B;
         IRTemp src   = newTemp(isD ? Ity_F64 : Ity_F32);
         IRTemp res   = newTemp(isD ? Ity_F64 : Ity_F32);
         Int    imm   = 0;

         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            assign( src, 
                    isD ? getXMMRegLane64F(rE, 0) : getXMMRegLane32F(rE, 0) );
            imm = getUChar(delta+1);
            if (imm & ~15) break;
            delta += 1+1;
            DIP( "vrounds%c $%d,%s,%s,%s\n",
                 isD ? 'd' : 's',
                 imm, nameXMMReg( rE ), nameXMMReg( rV ), nameXMMReg( rG ) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( src, loadLE( isD ? Ity_F64 : Ity_F32, mkexpr(addr) ));
            imm = getUChar(delta+alen);
            if (imm & ~15) break;
            delta += alen+1;
            DIP( "vrounds%c $%d,%s,%s,%s\n",
                 isD ? 'd' : 's',
                 imm, dis_buf, nameXMMReg( rV ), nameXMMReg( rG ) );
         }

         /* (imm & 3) contains an Intel-encoded rounding mode.  Because
            that encoding is the same as the encoding for IRRoundingMode,
            we can use that value directly in the IR as a rounding
            mode. */
         assign(res, binop(isD ? Iop_RoundF64toInt : Iop_RoundF32toInt,
                           (imm & 4) ? get_sse_roundingmode() 
                                     : mkU32(imm & 3),
                           mkexpr(src)) );

         if (isD)
            putXMMRegLane64F( rG, 0, mkexpr(res) );
         else {
            putXMMRegLane32F( rG, 0, mkexpr(res) );
            putXMMRegLane32F( rG, 1, getXMMRegLane32F( rV, 1 ) );
         }
         putXMMRegLane64F( rG, 1, getXMMRegLane64F( rV, 1 ) );
         putYMMRegLane128( rG, 1, mkV128(0) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0C:
      /* VBLENDPS imm8, ymm3/m256, ymm2, ymm1 */
      /* VBLENDPS = VEX.NDS.256.66.0F3A.WIG 0C /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V256);
         IRTemp sE    = newTemp(Ity_V256);
         assign ( sV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vblendps $%u,%s,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
            assign(sE, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vblendps $%u,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(sE, loadLE(Ity_V256, mkexpr(addr)));
         }
         delta++;
         putYMMReg( rG, 
                    mkexpr( math_BLENDPS_256( sE, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VBLENDPS imm8, xmm3/m128, xmm2, xmm1 */
      /* VBLENDPS = VEX.NDS.128.66.0F3A.WIG 0C /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V128);
         IRTemp sE    = newTemp(Ity_V128);
         assign ( sV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vblendps $%u,%s,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
            assign(sE, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vblendps $%u,%s,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG));
            assign(sE, loadLE(Ity_V128, mkexpr(addr)));
         }
         delta++;
         putYMMRegLoAndZU( rG, 
                           mkexpr( math_BLENDPS_128( sE, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0D:
      /* VBLENDPD imm8, ymm3/m256, ymm2, ymm1 */
      /* VBLENDPD = VEX.NDS.256.66.0F3A.WIG 0D /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V256);
         IRTemp sE    = newTemp(Ity_V256);
         assign ( sV, getYMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vblendpd $%u,%s,%s,%s\n",
                imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
            assign(sE, getYMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vblendpd $%u,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(sE, loadLE(Ity_V256, mkexpr(addr)));
         }
         delta++;
         putYMMReg( rG, 
                    mkexpr( math_BLENDPD_256( sE, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VBLENDPD imm8, xmm3/m128, xmm2, xmm1 */
      /* VBLENDPD = VEX.NDS.128.66.0F3A.WIG 0D /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V128);
         IRTemp sE    = newTemp(Ity_V128);
         assign ( sV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vblendpd $%u,%s,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
            assign(sE, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vblendpd $%u,%s,%s,%s\n",
                imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG));
            assign(sE, loadLE(Ity_V128, mkexpr(addr)));
         }
         delta++;
         putYMMRegLoAndZU( rG, 
                           mkexpr( math_BLENDPD_128( sE, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0E:
      /* VPBLENDW imm8, xmm3/m128, xmm2, xmm1 */
      /* VPBLENDW = VEX.NDS.128.66.0F3A.WIG 0E /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   imm8;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V128);
         IRTemp sE    = newTemp(Ity_V128);
         assign ( sV, getXMMReg(rV) );
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            imm8 = getUChar(delta);
            DIP("vpblendw $%u,%s,%s,%s\n",
                imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG));
            assign(sE, getXMMReg(rE));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            imm8 = getUChar(delta);
            DIP("vpblendw $%u,%s,%s,%s\n",
                imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
            assign(sE, loadLE(Ity_V128, mkexpr(addr)));
         }
         delta++;
         putYMMRegLoAndZU( rG, 
                           mkexpr( math_PBLENDW_128( sE, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x0F:
      /* VPALIGNR imm8, xmm3/m128, xmm2, xmm1 */
      /* VPALIGNR = VEX.NDS.128.66.0F3A.WIG 0F /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp sV    = newTemp(Ity_V128);
         IRTemp dV    = newTemp(Ity_V128);
         UInt   imm8;

         assign( dV, getXMMReg(rV) );

         if ( epartIsReg( modrm ) ) {
            UInt   rE = eregOfRexRM(pfx, modrm);
            assign( sV, getXMMReg(rE) );
            imm8 = getUChar(delta+1);
            delta += 1+1;
            DIP("vpalignr $%d,%s,%s,%s\n", imm8, nameXMMReg(rE),
                                           nameXMMReg(rV), nameXMMReg(rG));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            gen_SEGV_if_not_16_aligned( addr );
            assign( sV, loadLE(Ity_V128, mkexpr(addr)) );
            imm8 = getUChar(delta+alen);
            delta += alen+1;
            DIP("vpalignr $%d,%s,%s,%s\n", imm8, dis_buf,
                                           nameXMMReg(rV), nameXMMReg(rG));
         }

         IRTemp res = math_PALIGNR_XMM( sV, dV, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x14:
      /* VPEXTRB imm8, xmm2, reg/m8 = VEX.128.66.0F3A.W0 14 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         delta = dis_PEXTRB_128_GtoE( vbi, pfx, delta, False/*!isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x15:
      /* VPEXTRW imm8, reg/m16, xmm2 */
      /* VPEXTRW = VEX.128.66.0F3A.W0 15 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         delta = dis_PEXTRW( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x16:
      /* VPEXTRD imm8, r32/m32, xmm2 */
      /* VPEXTRD = VEX.128.66.0F3A.W0 16 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         delta = dis_PEXTRD( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      /* VPEXTRQ = VEX.128.66.0F3A.W1 16 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 1==getRexW(pfx)/*W1*/) {
         delta = dis_PEXTRQ( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x17:
      /* VEXTRACTPS imm8, xmm1, r32/m32 = VEX.128.66.0F3A.WIG 17 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_EXTRACTPS( vbi, pfx, delta, True/*isAvx*/ );
         goto decode_success;
      }
      break;

   case 0x18:
      /* VINSERTF128 r/m, rV, rD
         ::: rD = insertinto(a lane in rV, 128 bits from r/m) */
      /* VINSERTF128 = VEX.NDS.256.66.0F3A.W0 18 /r ib */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   ib    = 0;
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         IRTemp t128  = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rE = eregOfRexRM(pfx, modrm);
            delta += 1;
            assign(t128, getXMMReg(rE));
            ib = getUChar(delta);
            DIP("vinsertf128 $%u,%s,%s,%s\n",
                ib, nameXMMReg(rE), nameYMMReg(rV), nameYMMReg(rG));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign(t128, loadLE(Ity_V128, mkexpr(addr)));
            delta += alen;
            ib = getUChar(delta);
            DIP("vinsertf128 $%u,%s,%s,%s\n",
                ib, dis_buf, nameYMMReg(rV), nameYMMReg(rG));
         }
         delta++;
         putYMMRegLane128(rG, 0,   getYMMRegLane128(rV, 0));
         putYMMRegLane128(rG, 1,   getYMMRegLane128(rV, 1));
         putYMMRegLane128(rG, ib & 1, mkexpr(t128));
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x19:
     /* VEXTRACTF128 $lane_no, rS, r/m
        ::: r/m:V128 = a lane of rS:V256 (RM format) */
     /* VEXTRACTF128 = VEX.256.66.0F3A.W0 19 /r ib */
      if (have66noF2noF3(pfx)
          && 1==getVexL(pfx)/*256*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   ib    = 0;
         UInt   rS    = gregOfRexRM(pfx, modrm);
         IRTemp t128  = newTemp(Ity_V128);
         if (epartIsReg(modrm)) {
            UInt rD = eregOfRexRM(pfx, modrm);
            delta += 1;
            ib = getUChar(delta);
            assign(t128, getYMMRegLane128(rS, ib & 1));
            putYMMRegLoAndZU(rD, mkexpr(t128));
            DIP("vextractf128 $%u,%s,%s\n",
                ib, nameXMMReg(rS), nameYMMReg(rD));
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            delta += alen;
            ib = getUChar(delta);
            assign(t128, getYMMRegLane128(rS, ib & 1));
            storeLE(mkexpr(addr), mkexpr(t128));
            DIP("vextractf128 $%u,%s,%s\n",
                ib, nameYMMReg(rS), dis_buf);
         }
         delta++;
         /* doesn't use vvvv */
         goto decode_success;
      }
      break;

   case 0x20:
      /* VPINSRB r32/m8, xmm2, xmm1 = VEX.NDS.128.66.0F3A.W0 20 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm  = getUChar(delta);
         UInt   rG     = gregOfRexRM(pfx, modrm);
         UInt   rV     = getVexNvvvv(pfx);
         Int    imm8;
         IRTemp src_u8 = newTemp(Ity_I8);

         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8 = (Int)(getUChar(delta+1) & 15);
            assign( src_u8, unop(Iop_32to8, getIReg32( rE )) );
            delta += 1+1;
            DIP( "vpinsrb $%d,%s,%s,%s\n",
                 imm8, nameIReg32(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)(getUChar(delta+alen) & 15);
            assign( src_u8, loadLE( Ity_I8, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vpinsrb $%d,%s,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_PINSRB_128( src_vec, src_u8, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x21:
      /* VINSERTPS imm8, xmm3/m32, xmm2, xmm1
         = VEX.NDS.128.66.0F3A.WIG 21 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         UInt   imm8;
         IRTemp d2ins = newTemp(Ity_I32); /* comes from the E part */
         const IRTemp inval = IRTemp_INVALID;

         if ( epartIsReg( modrm ) ) {
            UInt   rE = eregOfRexRM(pfx, modrm);
            IRTemp vE = newTemp(Ity_V128);
            assign( vE, getXMMReg(rE) );
            IRTemp dsE[4] = { inval, inval, inval, inval };
            breakupV128to32s( vE, &dsE[3], &dsE[2], &dsE[1], &dsE[0] );
            imm8 = getUChar(delta+1);
            d2ins = dsE[(imm8 >> 6) & 3]; /* "imm8_count_s" */
            delta += 1+1;
            DIP( "insertps $%u, %s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            assign( d2ins, loadLE( Ity_I32, mkexpr(addr) ) );
            imm8 = getUChar(delta+alen);
            delta += alen+1;
            DIP( "insertps $%u, %s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rG) );
         }

         IRTemp vV = newTemp(Ity_V128);
         assign( vV, getXMMReg(rV) );

         putYMMRegLoAndZU( rG, mkexpr(math_INSERTPS( vV, d2ins, imm8 )) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x22:
      /* VPINSRD r32/m32, xmm2, xmm1 = VEX.NDS.128.66.0F3A.W0 22 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 0==getRexW(pfx)/*W0*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         Int    imm8_10;
         IRTemp src_u32 = newTemp(Ity_I32);

         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8_10 = (Int)(getUChar(delta+1) & 3);
            assign( src_u32, getIReg32( rE ) );
            delta += 1+1;
            DIP( "vpinsrd $%d,%s,%s,%s\n",
                 imm8_10, nameIReg32(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8_10 = (Int)(getUChar(delta+alen) & 3);
            assign( src_u32, loadLE( Ity_I32, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vpinsrd $%d,%s,%s,%s\n", 
                 imm8_10, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_PINSRD_128( src_vec, src_u32, imm8_10 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VPINSRQ r64/m64, xmm2, xmm1 = VEX.NDS.128.66.0F3A.W1 22 /r ib */
      if (have66noF2noF3(pfx)
          && 0==getVexL(pfx)/*128*/ && 1==getRexW(pfx)/*W1*/) {
         UChar  modrm = getUChar(delta);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);
         Int    imm8_0;
         IRTemp src_u64 = newTemp(Ity_I64);

         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8_0 = (Int)(getUChar(delta+1) & 1);
            assign( src_u64, getIReg64( rE ) );
            delta += 1+1;
            DIP( "vpinsrq $%d,%s,%s,%s\n",
                 imm8_0, nameIReg64(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8_0 = (Int)(getUChar(delta+alen) & 1);
            assign( src_u64, loadLE( Ity_I64, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vpinsrd $%d,%s,%s,%s\n", 
                 imm8_0, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_PINSRQ_128( src_vec, src_u64, imm8_0 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x40:
      /* VDPPS imm8, xmm3/m128,xmm2,xmm1 = VEX.NDS.128.66.0F3A.WIG 40 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm   = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         UInt   rV      = getVexNvvvv(pfx);
         IRTemp dst_vec = newTemp(Ity_V128);
         Int    imm8;
         if (epartIsReg( modrm )) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( dst_vec, getXMMReg( rE ) );
            delta += 1+1;
            DIP( "vdpps $%d,%s,%s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)getUChar(delta+alen);
            assign( dst_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vdpps $%d,%s,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_DPPS_128( src_vec, dst_vec, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VDPPS imm8, ymm3/m128,ymm2,ymm1 = VEX.NDS.256.66.0F3A.WIG 40 /r ib */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         UChar  modrm   = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         UInt   rV      = getVexNvvvv(pfx);
         IRTemp dst_vec = newTemp(Ity_V256);
         Int    imm8;
         if (epartIsReg( modrm )) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( dst_vec, getYMMReg( rE ) );
            delta += 1+1;
            DIP( "vdpps $%d,%s,%s,%s\n",
                 imm8, nameYMMReg(rE), nameYMMReg(rV), nameYMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)getUChar(delta+alen);
            assign( dst_vec, loadLE( Ity_V256, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vdpps $%d,%s,%s,%s\n", 
                 imm8, dis_buf, nameYMMReg(rV), nameYMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V256);
         assign(src_vec, getYMMReg( rV ));
         IRTemp s0, s1, d0, d1;
         s0 = s1 = d0 = d1 = IRTemp_INVALID;
         breakupV256toV128s( dst_vec, &d1, &d0 );
         breakupV256toV128s( src_vec, &s1, &s0 );
         putYMMReg( rG, binop( Iop_V128HLtoV256,
                               mkexpr( math_DPPS_128(s1, d1, imm8) ),
                               mkexpr( math_DPPS_128(s0, d0, imm8) ) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x41:
      /* VDPPD imm8, xmm3/m128,xmm2,xmm1 = VEX.NDS.128.66.0F3A.WIG 41 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm   = getUChar(delta);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         UInt   rV      = getVexNvvvv(pfx);
         IRTemp dst_vec = newTemp(Ity_V128);
         Int    imm8;
         if (epartIsReg( modrm )) {
            UInt rE = eregOfRexRM(pfx,modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( dst_vec, getXMMReg( rE ) );
            delta += 1+1;
            DIP( "vdppd $%d,%s,%s,%s\n",
                 imm8, nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 1 );
            imm8 = (Int)getUChar(delta+alen);
            assign( dst_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            delta += alen+1;
            DIP( "vdppd $%d,%s,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         IRTemp src_vec = newTemp(Ity_V128);
         assign(src_vec, getXMMReg( rV ));
         IRTemp res_vec = math_DPPD_128( src_vec, dst_vec, imm8 );
         putYMMRegLoAndZU( rG, mkexpr(res_vec) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x42:
      /* VMPSADBW imm8, xmm3/m128,xmm2,xmm1 */
      /* VMPSADBW = VEX.NDS.128.66.0F3A.WIG 42 /r ib */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm   = getUChar(delta);
         Int    imm8;
         IRTemp src_vec = newTemp(Ity_V128);
         IRTemp dst_vec = newTemp(Ity_V128);
         UInt   rG      = gregOfRexRM(pfx, modrm);
         UInt   rV      = getVexNvvvv(pfx);

         assign( dst_vec, getXMMReg(rV) );
  
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);

            imm8 = (Int)getUChar(delta+1);
            assign( src_vec, getXMMReg(rE) );
            delta += 1+1;
            DIP( "vmpsadbw $%d, %s,%s,%s\n", imm8,
                 nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG) );
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            gen_SEGV_if_not_16_aligned( addr );
            assign( src_vec, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "vmpsadbw $%d, %s,%s,%s\n", imm8,
                 dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         putYMMRegLoAndZU( rG, mkexpr( math_MPSADBW_128(dst_vec,
                                                        src_vec, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x44:
      /* VPCLMULQDQ imm8, xmm3/m128,xmm2,xmm1 */
      /* VPCLMULQDQ = VEX.NDS.128.66.0F3A.WIG 44 /r ib */
      /* 66 0F 3A 44 /r ib = PCLMULQDQ xmm1, xmm2/m128, imm8
       * Carry-less multiplication of selected XMM quadwords into XMM
       * registers (a.k.a multiplication of polynomials over GF(2))
       */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         UChar  modrm = getUChar(delta);
         Int imm8;
         IRTemp sV    = newTemp(Ity_V128);
         IRTemp dV    = newTemp(Ity_V128);
         UInt   rG    = gregOfRexRM(pfx, modrm);
         UInt   rV    = getVexNvvvv(pfx);

         assign( dV, getXMMReg(rV) );
  
         if ( epartIsReg( modrm ) ) {
            UInt rE = eregOfRexRM(pfx, modrm);
            imm8 = (Int)getUChar(delta+1);
            assign( sV, getXMMReg(rE) );
            delta += 1+1;
            DIP( "vpclmulqdq $%d, %s,%s,%s\n", imm8,
                 nameXMMReg(rE), nameXMMReg(rV), nameXMMReg(rG) );    
         } else {
            addr = disAMode( &alen, vbi, pfx, delta, dis_buf, 
                             1/* imm8 is 1 byte after the amode */ );
            assign( sV, loadLE( Ity_V128, mkexpr(addr) ) );
            imm8 = (Int)getUChar(delta+alen);
            delta += alen+1;
            DIP( "vpclmulqdq $%d, %s,%s,%s\n", 
                 imm8, dis_buf, nameXMMReg(rV), nameXMMReg(rG) );
         }

         putYMMRegLoAndZU( rG, mkexpr( math_PCLMULQDQ(dV, sV, imm8) ) );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x4A:
      /* VBLENDVPS xmmG, xmmE/memE, xmmV, xmmIS4
         ::: xmmG:V128 = PBLEND(xmmE, xmmV, xmmIS4) (RMVR) */
      /* VBLENDVPS = VEX.NDS.128.66.0F3A.WIG 4A /r /is4 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VBLENDV_128 ( vbi, pfx, delta,
                                   "vblendvps", 4, Iop_SarN32x4 );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VBLENDVPS ymmG, ymmE/memE, ymmV, ymmIS4
         ::: ymmG:V256 = PBLEND(ymmE, ymmV, ymmIS4) (RMVR) */
      /* VBLENDVPS = VEX.NDS.256.66.0F3A.WIG 4A /r /is4 */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VBLENDV_256 ( vbi, pfx, delta,
                                   "vblendvps", 4, Iop_SarN32x4 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x4B:
      /* VBLENDVPD xmmG, xmmE/memE, xmmV, xmmIS4
         ::: xmmG:V128 = PBLEND(xmmE, xmmV, xmmIS4) (RMVR) */
      /* VBLENDVPD = VEX.NDS.128.66.0F3A.WIG 4B /r /is4 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VBLENDV_128 ( vbi, pfx, delta,
                                   "vblendvpd", 8, Iop_SarN64x2 );
         *uses_vvvv = True;
         goto decode_success;
      }
      /* VBLENDVPD ymmG, ymmE/memE, ymmV, ymmIS4
         ::: ymmG:V256 = PBLEND(ymmE, ymmV, ymmIS4) (RMVR) */
      /* VBLENDVPD = VEX.NDS.256.66.0F3A.WIG 4B /r /is4 */
      if (have66noF2noF3(pfx) && 1==getVexL(pfx)/*256*/) {
         delta = dis_VBLENDV_256 ( vbi, pfx, delta,
                                   "vblendvpd", 8, Iop_SarN64x2 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x4C:
      /* VPBLENDVB xmmG, xmmE/memE, xmmV, xmmIS4
         ::: xmmG:V128 = PBLEND(xmmE, xmmV, xmmIS4) (RMVR) */
      /* VPBLENDVB = VEX.NDS.128.66.0F3A.WIG 4C /r /is4 */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_VBLENDV_128 ( vbi, pfx, delta,
                                   "vpblendvb", 1, Iop_SarN8x16 );
         *uses_vvvv = True;
         goto decode_success;
      }
      break;

   case 0x60:
   case 0x61:
   case 0x62:
   case 0x63:
      /* VEX.128.66.0F3A.WIG 63 /r ib = VPCMPISTRI imm8, xmm2/m128, xmm1
         VEX.128.66.0F3A.WIG 62 /r ib = VPCMPISTRM imm8, xmm2/m128, xmm1
         VEX.128.66.0F3A.WIG 61 /r ib = VPCMPESTRI imm8, xmm2/m128, xmm1
         VEX.128.66.0F3A.WIG 60 /r ib = VPCMPESTRM imm8, xmm2/m128, xmm1
         (selected special cases that actually occur in glibc,
          not by any means a complete implementation.)
      */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         Long delta0 = delta;
         delta = dis_PCMPxSTRx( vbi, pfx, delta, True/*isAvx*/, opc );
         if (delta > delta0) goto decode_success;
         /* else fall though; dis_PCMPxSTRx failed to decode it */
      }
      break;

   case 0xDF:
      /* VAESKEYGENASSIST imm8, xmm2/m128, xmm1 = VEX.128.66.0F3A.WIG DF /r */
      if (have66noF2noF3(pfx) && 0==getVexL(pfx)/*128*/) {
         delta = dis_AESKEYGENASSIST( vbi, pfx, delta, True/*!isAvx*/ );
         goto decode_success;
      }
      break;

   default:
      break;

   }

  //decode_failure:
   return deltaIN;

  decode_success:
   return delta;
}


/*------------------------------------------------------------*/
/*---                                                      ---*/
/*--- Disassemble a single instruction                     ---*/
/*---                                                      ---*/
/*------------------------------------------------------------*/

/* Disassemble a single instruction into IR.  The instruction is
   located in host memory at &guest_code[delta]. */
   
static
DisResult disInstr_AMD64_WRK ( 
             /*OUT*/Bool* expect_CAS,
             Bool         (*resteerOkFn) ( /*opaque*/void*, Addr64 ),
             Bool         resteerCisOk,
             void*        callback_opaque,
             Long         delta64,
             VexArchInfo* archinfo,
             VexAbiInfo*  vbi
          )
{
   IRTemp    t1, t2, t3, t4, t5, t6;
   UChar     pre;
   Int       n, n_prefixes;
   DisResult dres;

   /* The running delta */
   Long delta = delta64;

   /* Holds eip at the start of the insn, so that we can print
      consistent error messages for unimplemented insns. */
   Long delta_start = delta;

   /* sz denotes the nominal data-op size of the insn; we change it to
      2 if an 0x66 prefix is seen and 8 if REX.W is 1.  In case of
      conflict REX.W takes precedence. */
   Int sz = 4;

   /* pfx holds the summary of prefixes. */
   Prefix pfx = PFX_EMPTY;

   /* Holds the computed opcode-escape indication. */
   Escape esc = ESC_NONE;

   /* Set result defaults. */
   dres.whatNext    = Dis_Continue;
   dres.len         = 0;
   dres.continueAt  = 0;
   dres.jk_StopHere = Ijk_INVALID;
   *expect_CAS = False;

   vassert(guest_RIP_next_assumed == 0);
   vassert(guest_RIP_next_mustcheck == False);

   t1 = t2 = t3 = t4 = t5 = t6 = IRTemp_INVALID; 

   DIP("\t0x%llx:  ", guest_RIP_bbstart+delta);

   /* Spot "Special" instructions (see comment at top of file). */
   {
      UChar* code = (UChar*)(guest_code + delta);
      /* Spot the 16-byte preamble:
         48C1C703   rolq $3,  %rdi
         48C1C70D   rolq $13, %rdi
         48C1C73D   rolq $61, %rdi
         48C1C733   rolq $51, %rdi
      */
      if (code[ 0] == 0x48 && code[ 1] == 0xC1 && code[ 2] == 0xC7 
                                               && code[ 3] == 0x03 &&
          code[ 4] == 0x48 && code[ 5] == 0xC1 && code[ 6] == 0xC7 
                                               && code[ 7] == 0x0D &&
          code[ 8] == 0x48 && code[ 9] == 0xC1 && code[10] == 0xC7 
                                               && code[11] == 0x3D &&
          code[12] == 0x48 && code[13] == 0xC1 && code[14] == 0xC7 
                                               && code[15] == 0x33) {
         /* Got a "Special" instruction preamble.  Which one is it? */
         if (code[16] == 0x48 && code[17] == 0x87 
                              && code[18] == 0xDB /* xchgq %rbx,%rbx */) {
            /* %RDX = client_request ( %RAX ) */
            DIP("%%rdx = client_request ( %%rax )\n");
            delta += 19;
            jmp_lit(&dres, Ijk_ClientReq, guest_RIP_bbstart+delta);
            vassert(dres.whatNext == Dis_StopHere);
            goto decode_success;
         }
         else
         if (code[16] == 0x48 && code[17] == 0x87 
                              && code[18] == 0xC9 /* xchgq %rcx,%rcx */) {
            /* %RAX = guest_NRADDR */
            DIP("%%rax = guest_NRADDR\n");
            delta += 19;
            putIRegRAX(8, IRExpr_Get( OFFB_NRADDR, Ity_I64 ));
            goto decode_success;
         }
         else
         if (code[16] == 0x48 && code[17] == 0x87 
                              && code[18] == 0xD2 /* xchgq %rdx,%rdx */) {
            /* call-noredir *%RAX */
            DIP("call-noredir *%%rax\n");
            delta += 19;
            t1 = newTemp(Ity_I64);
            assign(t1, getIRegRAX(8));
            t2 = newTemp(Ity_I64);
            assign(t2, binop(Iop_Sub64, getIReg64(R_RSP), mkU64(8)));
            putIReg64(R_RSP, mkexpr(t2));
            storeLE( mkexpr(t2), mkU64(guest_RIP_bbstart+delta));
            jmp_treg(&dres, Ijk_NoRedir, t1);
            vassert(dres.whatNext == Dis_StopHere);
            goto decode_success;
         }
         /* We don't know what it is. */
         goto decode_failure;
         /*NOTREACHED*/
      }
   }

   /* Eat prefixes, summarising the result in pfx and sz, and rejecting
      as many invalid combinations as possible. */
   n_prefixes = 0;
   while (True) {
      if (n_prefixes > 7) goto decode_failure;
      pre = getUChar(delta);
      switch (pre) {
         case 0x66: pfx |= PFX_66; break;
         case 0x67: pfx |= PFX_ASO; break;
         case 0xF2: pfx |= PFX_F2; break;
         case 0xF3: pfx |= PFX_F3; break;
         case 0xF0: pfx |= PFX_LOCK; *expect_CAS = True; break;
         case 0x2E: pfx |= PFX_CS; break;
         case 0x3E: pfx |= PFX_DS; break;
         case 0x26: pfx |= PFX_ES; break;
         case 0x64: pfx |= PFX_FS; break;
         case 0x65: pfx |= PFX_GS; break;
         case 0x36: pfx |= PFX_SS; break;
         case 0x40 ... 0x4F:
            pfx |= PFX_REX;
            if (pre & (1<<3)) pfx |= PFX_REXW;
            if (pre & (1<<2)) pfx |= PFX_REXR;
            if (pre & (1<<1)) pfx |= PFX_REXX;
            if (pre & (1<<0)) pfx |= PFX_REXB;
            break;
         default: 
            goto not_a_legacy_prefix;
      }
      n_prefixes++;
      delta++;
   }

   not_a_legacy_prefix:
   /* We've used up all the non-VEX prefixes.  Parse and validate a
      VEX prefix if that's appropriate. */
   if (archinfo->hwcaps & VEX_HWCAPS_AMD64_AVX) {
      /* Used temporarily for holding VEX prefixes. */
      UChar vex0 = getUChar(delta);
      if (vex0 == 0xC4) {
         /* 3-byte VEX */
         UChar vex1 = getUChar(delta+1);
         UChar vex2 = getUChar(delta+2);
         delta += 3;
         pfx |= PFX_VEX;
         /* Snarf contents of byte 1 */
         /* R */ pfx |= (vex1 & (1<<7)) ? 0 : PFX_REXR;
         /* X */ pfx |= (vex1 & (1<<6)) ? 0 : PFX_REXX;
         /* B */ pfx |= (vex1 & (1<<5)) ? 0 : PFX_REXB;
         /* m-mmmm */
         switch (vex1 & 0x1F) {
            case 1: esc = ESC_0F;   break;
            case 2: esc = ESC_0F38; break;
            case 3: esc = ESC_0F3A; break;
            /* Any other m-mmmm field will #UD */
            default: goto decode_failure;
         }
         /* Snarf contents of byte 2 */
         /* W */    pfx |= (vex2 & (1<<7)) ? PFX_REXW : 0;
         /* ~v3 */  pfx |= (vex2 & (1<<6)) ? 0 : PFX_VEXnV3;
         /* ~v2 */  pfx |= (vex2 & (1<<5)) ? 0 : PFX_VEXnV2;
         /* ~v1 */  pfx |= (vex2 & (1<<4)) ? 0 : PFX_VEXnV1;
         /* ~v0 */  pfx |= (vex2 & (1<<3)) ? 0 : PFX_VEXnV0;
         /* L */    pfx |= (vex2 & (1<<2)) ? PFX_VEXL : 0;
         /* pp */
         switch (vex2 & 3) {
            case 0: break;
            case 1: pfx |= PFX_66; break;
            case 2: pfx |= PFX_F3; break;
            case 3: pfx |= PFX_F2; break;
            default: vassert(0);
         }
      }
      else if (vex0 == 0xC5) {
         /* 2-byte VEX */
         UChar vex1 = getUChar(delta+1);
         delta += 2;
         pfx |= PFX_VEX;
         /* Snarf contents of byte 1 */
         /* R */    pfx |= (vex1 & (1<<7)) ? 0 : PFX_REXR;
         /* ~v3 */  pfx |= (vex1 & (1<<6)) ? 0 : PFX_VEXnV3;
         /* ~v2 */  pfx |= (vex1 & (1<<5)) ? 0 : PFX_VEXnV2;
         /* ~v1 */  pfx |= (vex1 & (1<<4)) ? 0 : PFX_VEXnV1;
         /* ~v0 */  pfx |= (vex1 & (1<<3)) ? 0 : PFX_VEXnV0;
         /* L */    pfx |= (vex1 & (1<<2)) ? PFX_VEXL : 0;
         /* pp */
         switch (vex1 & 3) {
            case 0: break;
            case 1: pfx |= PFX_66; break;
            case 2: pfx |= PFX_F3; break;
            case 3: pfx |= PFX_F2; break;
            default: vassert(0);
         }
         /* implied: */
         esc = ESC_0F;
      }
      /* Can't have both VEX and REX */
      if ((pfx & PFX_VEX) && (pfx & PFX_REX))
         goto decode_failure; /* can't have both */
   }

   /* Dump invalid combinations */
   n = 0;
   if (pfx & PFX_F2) n++;
   if (pfx & PFX_F3) n++;
   if (n > 1) 
      goto decode_failure; /* can't have both */

   n = 0;
   if (pfx & PFX_CS) n++;
   if (pfx & PFX_DS) n++;
   if (pfx & PFX_ES) n++;
   if (pfx & PFX_FS) n++;
   if (pfx & PFX_GS) n++;
   if (pfx & PFX_SS) n++;
   if (n > 1) 
      goto decode_failure; /* multiple seg overrides == illegal */

   /* We have a %fs prefix.  Reject it if there's no evidence in 'vbi'
      that we should accept it. */
   if ((pfx & PFX_FS) && !vbi->guest_amd64_assume_fs_is_zero)
      goto decode_failure;

   /* Ditto for %gs prefixes. */
   if ((pfx & PFX_GS) && !vbi->guest_amd64_assume_gs_is_0x60)
      goto decode_failure;

   /* Set up sz. */
   sz = 4;
   if (pfx & PFX_66) sz = 2;
   if ((pfx & PFX_REX) && (pfx & PFX_REXW)) sz = 8;

   /* Now we should be looking at the primary opcode byte or the
      leading escapes.  Check that any LOCK prefix is actually
      allowed. */
   if (pfx & PFX_LOCK) {
      if (can_be_used_with_LOCK_prefix( (UChar*)&guest_code[delta] )) {
         DIP("lock ");
      } else {
         *expect_CAS = False;
         goto decode_failure;
      }
   }

   /* Eat up opcode escape bytes, until we're really looking at the
      primary opcode byte.  But only if there's no VEX present. */
   if (!(pfx & PFX_VEX)) {
      vassert(esc == ESC_NONE);
      pre = getUChar(delta);
      if (pre == 0x0F) {
         delta++;
         pre = getUChar(delta);
         switch (pre) {
            case 0x38: esc = ESC_0F38; delta++; break;
            case 0x3A: esc = ESC_0F3A; delta++; break;
            default:   esc = ESC_0F; break;
         }
      }
   }

   /* So now we're really really looking at the primary opcode
      byte. */
   Long delta_at_primary_opcode = delta;

   if (!(pfx & PFX_VEX)) {
      /* Handle non-VEX prefixed instructions.  "Legacy" (non-VEX) SSE
         instructions preserve the upper 128 bits of YMM registers;
         iow we can simply ignore the presence of the upper halves of
         these registers. */
      switch (esc) {
         case ESC_NONE:
            delta = dis_ESC_NONE( &dres, expect_CAS,
                                  resteerOkFn, resteerCisOk, callback_opaque,
                                  archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_0F:
            delta = dis_ESC_0F  ( &dres, expect_CAS,
                                  resteerOkFn, resteerCisOk, callback_opaque,
                                  archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_0F38:
            delta = dis_ESC_0F38( &dres,
                                  resteerOkFn, resteerCisOk, callback_opaque,
                                  archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_0F3A:
            delta = dis_ESC_0F3A( &dres,
                                  resteerOkFn, resteerCisOk, callback_opaque,
                                  archinfo, vbi, pfx, sz, delta );
            break;
         default:
            vassert(0);
      }
   } else {
      /* VEX prefixed instruction */
      /* Sloppy Intel wording: "An instruction encoded with a VEX.128
         prefix that loads a YMM register operand ..." zeroes out bits
         128 and above of the register. */
      Bool uses_vvvv = False;
      switch (esc) {
         case ESC_0F:
            delta = dis_ESC_0F__VEX ( &dres, &uses_vvvv,
                                      resteerOkFn, resteerCisOk,
                                      callback_opaque,
                                      archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_0F38:
            delta = dis_ESC_0F38__VEX ( &dres, &uses_vvvv,
                                        resteerOkFn, resteerCisOk,
                                        callback_opaque,
                                        archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_0F3A:
            delta = dis_ESC_0F3A__VEX ( &dres, &uses_vvvv,
                                        resteerOkFn, resteerCisOk,
                                        callback_opaque,
                                        archinfo, vbi, pfx, sz, delta );
            break;
         case ESC_NONE:
            /* The presence of a VEX prefix, by Intel definition,
               always implies at least an 0F escape. */
            goto decode_failure;
         default:
            vassert(0);
      }
      /* If the insn doesn't use VEX.vvvv then it must be all ones.
         Check this. */
      if (!uses_vvvv) {
         if (getVexNvvvv(pfx) != 0)
            goto decode_failure;
      }
   }

   vassert(delta - delta_at_primary_opcode >= 0);
   vassert(delta - delta_at_primary_opcode < 16/*let's say*/);

   /* Use delta == delta_at_primary_opcode to denote decode failure.
      This implies that any successful decode must use at least one
      byte up. */
   if (delta == delta_at_primary_opcode)
      goto decode_failure;
   else
      goto decode_success; /* \o/ */

#if 0 /* XYZZY */

   /* ---------------------------------------------------- */
   /* --- The SSE/SSE2 decoder.                        --- */
   /* ---------------------------------------------------- */

   /* What did I do to deserve SSE ?  Perhaps I was really bad in a
      previous life? */

   /* Note, this doesn't handle SSE3 right now.  All amd64s support
      SSE2 as a minimum so there is no point distinguishing SSE1 vs
      SSE2. */

   insn = (UChar*)&guest_code[delta];

   /* FXSAVE is spuriously at the start here only because it is
      thusly placed in guest-x86/toIR.c. */

   /* ------ SSE decoder main ------ */

   /* ---------------------------------------------------- */
   /* --- end of the SSE decoder.                      --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- start of the SSE2 decoder.                   --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- end of the SSE/SSE2 decoder.                 --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- start of the SSE3 decoder.                   --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- end of the SSE3 decoder.                     --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- start of the SSSE3 decoder.                  --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- end of the SSSE3 decoder.                    --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- start of the SSE4 decoder                    --- */
   /* ---------------------------------------------------- */

   /* ---------------------------------------------------- */
   /* --- end of the SSE4 decoder                      --- */
   /* ---------------------------------------------------- */

   /*after_sse_decoders:*/

   /* Get the primary opcode. */
   opc = getUChar(delta); delta++;

   /* We get here if the current insn isn't SSE, or this CPU doesn't
      support SSE. */

   switch (opc) {

   /* ------------------------ Control flow --------------- */

   /* ------------------------ CWD/CDQ -------------------- */

   /* ------------------------ FPU ops -------------------- */

   /* ------------------------ INT ------------------------ */

   case 0xCD: { /* INT imm8 */
      IRJumpKind jk = Ijk_Boring;
      if (have66orF2orF3(pfx)) goto decode_failure;
      d64 = getUChar(delta); delta++;
      switch (d64) {
         case 32: jk = Ijk_Sys_int32; break;
         default: goto decode_failure;
      }
      guest_RIP_next_mustcheck = True;
      guest_RIP_next_assumed = guest_RIP_bbstart + delta;
      jmp_lit(jk, guest_RIP_next_assumed);
      /* It's important that all ArchRegs carry their up-to-date value
         at this point.  So we declare an end-of-block here, which
         forces any TempRegs caching ArchRegs to be flushed. */
      vassert(dres.whatNext == Dis_StopHere);
      DIP("int $0x%02x\n", (UInt)d64);
      break;
   }

   /* ------------------------ Jcond, byte offset --------- */

   /* ------------------------ IMUL ----------------------- */

   /* ------------------------ MOV ------------------------ */

   /* ------------------------ MOVx ------------------------ */

   /* ------------------------ opl imm, A ----------------- */

   /* ------------------------ opl Ev, Gv ----------------- */

   /* ------------------------ opl Gv, Ev ----------------- */

   /* ------------------------ POP ------------------------ */

   /* ------------------------ PUSH ----------------------- */

   /* ------ AE: SCAS variants ------ */

   /* ------ A6, A7: CMPS variants ------ */

   /* ------ AA, AB: STOS variants ------ */

   /* ------ A4, A5: MOVS variants ------ */

   /* ------------------------ XCHG ----------------------- */

   /* ------------------------ IN / OUT ----------------------- */
 
   /* ------------------------ (Grp1 extensions) ---------- */

   /* ------------------------ (Grp2 extensions) ---------- */

   /* ------------------------ (Grp3 extensions) ---------- */

   /* ------------------------ (Grp4 extensions) ---------- */

   /* ------------------------ (Grp5 extensions) ---------- */

   /* ------------------------ Escapes to 2-byte opcodes -- */

   case 0x0F: {
      opc = getUChar(delta); delta++;
      switch (opc) {

      /* =-=-=-=-=-=-=-=-=- Grp8 =-=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- BSF/BSR -=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- BSWAP -=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- BT/BTS/BTR/BTC =-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- CMOV =-=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- CMPXCHG -=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- CPUID -=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- MOVZX, MOVSX =-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- MUL/IMUL =-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- NOPs =-=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- Jcond d32 -=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- PREFETCH =-=-=-=-=-=-=-=-=-= */
      case 0x0D: /* 0F 0D /0 -- prefetch mem8 */
                 /* 0F 0D /1 -- prefetchw mem8 */
         if (have66orF2orF3(pfx)) goto decode_failure;
         modrm = getUChar(delta);
         if (epartIsReg(modrm)) goto decode_failure;
         if (gregLO3ofRM(modrm) != 0 && gregLO3ofRM(modrm) != 1)
            goto decode_failure;

         addr = disAMode ( &alen, vbi, pfx, delta, dis_buf, 0 );
         delta += alen;

         switch (gregLO3ofRM(modrm)) {
            case 0: DIP("prefetch %s\n", dis_buf); break;
            case 1: DIP("prefetchw %s\n", dis_buf); break;
            default: vassert(0); /*NOTREACHED*/
         }
         break;

      /* =-=-=-=-=-=-=-=-=- RDTSC -=-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- SETcc Eb =-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- SHLD/SHRD -=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- SYSCALL -=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- XADD -=-=-=-=-=-=-=-=-=-= */

      case 0xC0: { /* XADD Gb,Eb */ 
         Bool decode_OK = False;
         delta = dis_xadd_G_E ( &decode_OK, vbi, pfx, 1, delta );
         if (!decode_OK)
            goto decode_failure;
         break;
      }

      /* =-=-=-=-=-=-=-=-=- SGDT and SIDT =-=-=-=-=-=-=-=-=-=-= */

      /* =-=-=-=-=-=-=-=-=- unimp2 =-=-=-=-=-=-=-=-=-=-= */

      default:
         goto decode_failure;
   } /* switch (opc) for the 2-byte opcodes */
   goto decode_success;
   } /* case 0x0F: of primary opcode */

   /* ------------------------ ??? ------------------------ */
#endif /* XYZZY */
  
     //default:
  decode_failure:
   /* All decode failures end up here. */
   vex_printf("vex amd64->IR: unhandled instruction bytes: "
              "0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x\n",
              (Int)getUChar(delta_start+0),
              (Int)getUChar(delta_start+1),
              (Int)getUChar(delta_start+2),
              (Int)getUChar(delta_start+3),
              (Int)getUChar(delta_start+4),
              (Int)getUChar(delta_start+5),
              (Int)getUChar(delta_start+6),
              (Int)getUChar(delta_start+7) );
   vex_printf("vex amd64->IR:   REX=%d REX.W=%d REX.R=%d REX.X=%d REX.B=%d\n",
              haveREX(pfx) ? 1 : 0, getRexW(pfx), getRexR(pfx),
              getRexX(pfx), getRexB(pfx));
   vex_printf("vex amd64->IR:   VEX=%d VEX.L=%d VEX.nVVVV=0x%x ESC=%s\n",
              haveVEX(pfx) ? 1 : 0, getVexL(pfx),
              getVexNvvvv(pfx),
              esc==ESC_NONE ? "NONE" :
                esc==ESC_0F ? "0F" :
                esc==ESC_0F38 ? "0F38" :
                esc==ESC_0F3A ? "0F3A" : "???");
   vex_printf("vex amd64->IR:   PFX.66=%d PFX.F2=%d PFX.F3=%d\n",
              have66(pfx) ? 1 : 0, haveF2(pfx) ? 1 : 0,
              haveF3(pfx) ? 1 : 0);

   /* Tell the dispatcher that this insn cannot be decoded, and so has
      not been executed, and (is currently) the next to be executed.
      RIP should be up-to-date since it made so at the start of each
      insn, but nevertheless be paranoid and update it again right
      now. */
   stmt( IRStmt_Put( OFFB_RIP, mkU64(guest_RIP_curr_instr) ) );
   jmp_lit(&dres, Ijk_NoDecode, guest_RIP_curr_instr);
   vassert(dres.whatNext == Dis_StopHere);
   dres.len = 0;
   /* We also need to say that a CAS is not expected now, regardless
      of what it might have been set to at the start of the function,
      since the IR that we've emitted just above (to synthesis a
      SIGILL) does not involve any CAS, and presumably no other IR has
      been emitted for this (non-decoded) insn. */
   *expect_CAS = False;
   return dres;

   //   } /* switch (opc) for the main (primary) opcode switch. */

  decode_success:
   /* All decode successes end up here. */
   switch (dres.whatNext) {
      case Dis_Continue:
         stmt( IRStmt_Put( OFFB_RIP, mkU64(guest_RIP_bbstart + delta) ) );
         break;
      case Dis_ResteerU:
      case Dis_ResteerC:
         stmt( IRStmt_Put( OFFB_RIP, mkU64(dres.continueAt) ) );
         break;
      case Dis_StopHere:
         break;
      default:
         vassert(0);
   }

   DIP("\n");
   dres.len = (Int)toUInt(delta - delta_start);
   return dres;
}

#undef DIP
#undef DIS


/*------------------------------------------------------------*/
/*--- Top-level fn                                         ---*/
/*------------------------------------------------------------*/

/* Disassemble a single instruction into IR.  The instruction
   is located in host memory at &guest_code[delta]. */

DisResult disInstr_AMD64 ( IRSB*        irsb_IN,
                           Bool         (*resteerOkFn) ( void*, Addr64 ),
                           Bool         resteerCisOk,
                           void*        callback_opaque,
                           UChar*       guest_code_IN,
                           Long         delta,
                           Addr64       guest_IP,
                           VexArch      guest_arch,
                           VexArchInfo* archinfo,
                           VexAbiInfo*  abiinfo,
                           Bool         host_bigendian_IN )
{
   Int       i, x1, x2;
   Bool      expect_CAS, has_CAS;
   DisResult dres;

   /* Set globals (see top of this file) */
   vassert(guest_arch == VexArchAMD64);
   guest_code           = guest_code_IN;
   irsb                 = irsb_IN;
   host_is_bigendian    = host_bigendian_IN;
   guest_RIP_curr_instr = guest_IP;
   guest_RIP_bbstart    = guest_IP - delta;

   /* We'll consult these after doing disInstr_AMD64_WRK. */
   guest_RIP_next_assumed   = 0;
   guest_RIP_next_mustcheck = False;

   x1 = irsb_IN->stmts_used;
   expect_CAS = False;
   dres = disInstr_AMD64_WRK ( &expect_CAS, resteerOkFn,
                               resteerCisOk,
                               callback_opaque,
                               delta, archinfo, abiinfo );
   x2 = irsb_IN->stmts_used;
   vassert(x2 >= x1);

   /* If disInstr_AMD64_WRK tried to figure out the next rip, check it
      got it right.  Failure of this assertion is serious and denotes
      a bug in disInstr. */
   if (guest_RIP_next_mustcheck 
       && guest_RIP_next_assumed != guest_RIP_curr_instr + dres.len) {
      vex_printf("\n");
      vex_printf("assumed next %%rip = 0x%llx\n", 
                 guest_RIP_next_assumed );
      vex_printf(" actual next %%rip = 0x%llx\n", 
                 guest_RIP_curr_instr + dres.len );
      vpanic("disInstr_AMD64: disInstr miscalculated next %rip");
   }

   /* See comment at the top of disInstr_AMD64_WRK for meaning of
      expect_CAS.  Here, we (sanity-)check for the presence/absence of
      IRCAS as directed by the returned expect_CAS value. */
   has_CAS = False;
   for (i = x1; i < x2; i++) {
      if (irsb_IN->stmts[i]->tag == Ist_CAS)
         has_CAS = True;
   }

   if (expect_CAS != has_CAS) {
      /* inconsistency detected.  re-disassemble the instruction so as
         to generate a useful error message; then assert. */
      vex_traceflags |= VEX_TRACE_FE;
      dres = disInstr_AMD64_WRK ( &expect_CAS, resteerOkFn,
                                  resteerCisOk,
                                  callback_opaque,
                                  delta, archinfo, abiinfo );
      for (i = x1; i < x2; i++) {
         vex_printf("\t\t");
         ppIRStmt(irsb_IN->stmts[i]);
         vex_printf("\n");
      }
      /* Failure of this assertion is serious and denotes a bug in
         disInstr. */
      vpanic("disInstr_AMD64: inconsistency in LOCK prefix handling");
   }

   return dres;
}


/*------------------------------------------------------------*/
/*--- Unused stuff                                         ---*/
/*------------------------------------------------------------*/

// A potentially more Memcheck-friendly version of gen_LZCNT, if
// this should ever be needed.
//
//static IRTemp gen_LZCNT ( IRType ty, IRTemp src )
//{
//   /* Scheme is simple: propagate the most significant 1-bit into all
//      lower positions in the word.  This gives a word of the form
//      0---01---1.  Now invert it, giving a word of the form
//      1---10---0, then do a population-count idiom (to count the 1s,
//      which is the number of leading zeroes, or the word size if the
//      original word was 0.
//   */
//   Int i;
//   IRTemp t[7];
//   for (i = 0; i < 7; i++) {
//      t[i] = newTemp(ty);
//   }
//   if (ty == Ity_I64) {
//      assign(t[0], binop(Iop_Or64, mkexpr(src),
//                                   binop(Iop_Shr64, mkexpr(src),  mkU8(1))));
//      assign(t[1], binop(Iop_Or64, mkexpr(t[0]),
//                                   binop(Iop_Shr64, mkexpr(t[0]), mkU8(2))));
//      assign(t[2], binop(Iop_Or64, mkexpr(t[1]),
//                                   binop(Iop_Shr64, mkexpr(t[1]), mkU8(4))));
//      assign(t[3], binop(Iop_Or64, mkexpr(t[2]),
//                                   binop(Iop_Shr64, mkexpr(t[2]), mkU8(8))));
//      assign(t[4], binop(Iop_Or64, mkexpr(t[3]),
//                                   binop(Iop_Shr64, mkexpr(t[3]), mkU8(16))));
//      assign(t[5], binop(Iop_Or64, mkexpr(t[4]),
//                                   binop(Iop_Shr64, mkexpr(t[4]), mkU8(32))));
//      assign(t[6], unop(Iop_Not64, mkexpr(t[5])));
//      return gen_POPCOUNT(ty, t[6]);
//   }
//   if (ty == Ity_I32) {
//      assign(t[0], binop(Iop_Or32, mkexpr(src),
//                                   binop(Iop_Shr32, mkexpr(src),  mkU8(1))));
//      assign(t[1], binop(Iop_Or32, mkexpr(t[0]),
//                                   binop(Iop_Shr32, mkexpr(t[0]), mkU8(2))));
//      assign(t[2], binop(Iop_Or32, mkexpr(t[1]),
//                                   binop(Iop_Shr32, mkexpr(t[1]), mkU8(4))));
//      assign(t[3], binop(Iop_Or32, mkexpr(t[2]),
//                                   binop(Iop_Shr32, mkexpr(t[2]), mkU8(8))));
//      assign(t[4], binop(Iop_Or32, mkexpr(t[3]),
//                                   binop(Iop_Shr32, mkexpr(t[3]), mkU8(16))));
//      assign(t[5], unop(Iop_Not32, mkexpr(t[4])));
//      return gen_POPCOUNT(ty, t[5]);
//   }
//   if (ty == Ity_I16) {
//      assign(t[0], binop(Iop_Or16, mkexpr(src),
//                                   binop(Iop_Shr16, mkexpr(src),  mkU8(1))));
//      assign(t[1], binop(Iop_Or16, mkexpr(t[0]),
//                                   binop(Iop_Shr16, mkexpr(t[0]), mkU8(2))));
//      assign(t[2], binop(Iop_Or16, mkexpr(t[1]),
//                                   binop(Iop_Shr16, mkexpr(t[1]), mkU8(4))));
//      assign(t[3], binop(Iop_Or16, mkexpr(t[2]),
//                                   binop(Iop_Shr16, mkexpr(t[2]), mkU8(8))));
//      assign(t[4], unop(Iop_Not16, mkexpr(t[3])));
//      return gen_POPCOUNT(ty, t[4]);
//   }
//   vassert(0);
//}


/*--------------------------------------------------------------------*/
/*--- end                                       guest_amd64_toIR.c ---*/
/*--------------------------------------------------------------------*/
