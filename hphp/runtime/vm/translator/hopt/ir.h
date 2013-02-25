/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#ifndef incl_HPHP_VM_IR_H_
#define incl_HPHP_VM_IR_H_

#include <algorithm>
#include <cstdarg>
#include <iostream>
#include <vector>
#include <stack>
#include <list>
#include <forward_list>
#include <cassert>
#include <type_traits>
#include <boost/noncopyable.hpp>
#include <boost/checked_delete.hpp>
#include <boost/type_traits/has_trivial_destructor.hpp>
#include "util/asm-x64.h"
#include "runtime/ext/ext_continuation.h"
#include "runtime/vm/translator/physreg.h"
#include "runtime/vm/translator/abi-x64.h"
#include "runtime/vm/translator/types.h"
#include "runtime/vm/translator/runtime-type.h"
#include "runtime/vm/translator/translator-runtime.h"
#include "runtime/base/types.h"
#include "runtime/vm/func.h"
#include "runtime/vm/class.h"
#include "folly/Range.h"
#include "folly/Conv.h"

namespace HPHP {
// forward declaration
class StringData;
namespace VM {
namespace JIT {

using namespace HPHP::VM::Transl;

class FailedIRGen : public std::exception {
 public:
  const char* file;
  const int   line;
  const char* func;
  FailedIRGen(const char* _file, int _line, const char* _func) :
      file(_file), line(_line), func(_func) { }
};

// Flags to identify if a branch should go to a patchable jmp in astubs
// happens when instructions have been moved off the main trace to the exit path.
static const TCA kIRDirectJmpInactive = nullptr;
// Fixup Jcc;Jmp branches out of trace using REQ_BIND_JMPCC_FIRST/SECOND
static const TCA kIRDirectJccJmpActive = (TCA)0x01;
// Optimize Jcc exit from trace when fall through path stays in trace
static const TCA kIRDirectJccActive = (TCA)0x02;
// Optimize guard exit from beginning of trace
static const TCA kIRDirectGuardActive = (TCA)0x03;

#define SPUNT(instr) do {                           \
  throw FailedIRGen(__FILE__, __LINE__, instr);     \
} while(0)
#define PUNT(instr) SPUNT(#instr)

//////////////////////////////////////////////////////////////////////

/*
 * The instruction table below uses the following notation.  To use
 * it, you have to define these symbols to do what you want, and then
 * instantiate IR_OPCODES.
 *
 * dstinfo:
 *
 *   Contains a description of how to compute the type of the
 *   destination(s) of an instruction from its sources.
 *
 *     ND        instruction has no destination
 *     D(type)   single dst has a specific type
 *     DofS(N)   single dst has the type of src N
 *     DUnbox(N) single dst has unboxed type of src N
 *     DBox(N)   single dst has boxed type of src N
 *     DParam    single dst has type of the instruction's type parameter
 *     DLabel    multiple dests for a DefLabel
 *     DVector   single dst depends on semantics of the vector instruction
 *
 * srcinfo:
 *
 *   Contains a series of tests on the source parameters in order.
 *
 *     NA            instruction takes no sources
 *     SUnk          intructions sources are not yet documented/checked
 *     S(t1,...,tn)  source must be a subtype of {t1|..|tn}
 *     C(type)       source must be a constant, and subtype of type
 *     CStr          same as C(StaticStr)
 *     SNumInt       same as S(Int,Bool)
 *     SNum          same as S(Int,Bool,Dbl)
 *
 * flags:
 *
 *   See doc/ir.specification for the meaning of these flag various.
 *
 *   The flags in this opcode table supply default values for the
 *   querying functions in IRInstruction---those functions involve
 *   additional logic (based on operand types, etc) on a
 *   per-instruction basis.
 *
 *   The following abbreviations are used in this table:
 *
 *      NF    no flags
 *      C     canCSE
 *      E     isEssential
 *      N     callsNative
 *      PRc   producesRC
 *      CRc   consumesRC
 *      Refs  mayModifyRefs
 *      Rm    isRematerializable
 *      Er    mayRaiseError
 *      Mem   hasMemEffects
 *      T     isTerminal
 */
#define IR_OPCODES                                                            \
/*    name                      dstinfo srcinfo                      flags */ \
O(GuardType,                    DParam, S(Gen),                          C|E) \
O(GuardLoc,                         ND, S(StkPtr),                         E) \
O(GuardStk,                  D(StkPtr), S(StkPtr) C(Int),                  E) \
O(AssertStk,                 D(StkPtr), S(StkPtr) C(Int),                  E) \
O(GuardRefs,                        ND, SUnk,                              E) \
O(AssertLoc,                        ND, S(StkPtr),                         E) \
O(OpAdd,                        DParam, SNum SNum,                         C) \
O(OpSub,                        DParam, SNum SNum,                         C) \
O(OpAnd,                        D(Int), SNumInt SNumInt,                   C) \
O(OpOr,                         D(Int), SNum SNum,                         C) \
O(OpXor,                        D(Int), SNumInt SNumInt,                   C) \
O(OpMul,                        DParam, SNum SNum,                         C) \
O(Conv,                         DParam, S(Gen),                          C|N) \
O(ExtendsClass,                D(Bool), S(Cls) C(Cls),                     C) \
O(IsTypeMem,                   D(Bool), S(PtrToGen),                      NA) \
O(IsNTypeMem,                  D(Bool), S(PtrToGen),                      NA) \
                                                                              \
  /* TODO(#2058842): order currently matters for the 'query ops' here */      \
O(OpGt,                        D(Bool), S(Gen) S(Gen),                     C) \
O(OpGte,                       D(Bool), S(Gen) S(Gen),                     C) \
O(OpLt,                        D(Bool), S(Gen) S(Gen),                     C) \
O(OpLte,                       D(Bool), S(Gen) S(Gen),                     C) \
O(OpEq,                        D(Bool), S(Gen) S(Gen),                     C) \
O(OpNeq,                       D(Bool), S(Gen) S(Gen),                     C) \
O(OpSame,                      D(Bool), S(Gen) S(Gen),                   C|N) \
O(OpNSame,                     D(Bool), S(Gen) S(Gen),                   C|N) \
O(InstanceOf,                  D(Bool), S(Cls) S(Cls) C(Bool),           C|N) \
O(NInstanceOf,                 D(Bool), S(Cls) S(Cls) C(Bool),           C|N) \
O(InstanceOfBitmask,           D(Bool), S(Cls) CStr,                       C) \
O(NInstanceOfBitmask,          D(Bool), S(Cls) CStr,                       C) \
O(IsType,                      D(Bool), S(Cell),                           C) \
O(IsNType,                     D(Bool), S(Cell),                           C) \
  /* there is a conditional branch for each of the above query ops */         \
O(JmpGt,                       D(None), S(Gen) S(Gen),                     E) \
O(JmpGte,                      D(None), S(Gen) S(Gen),                     E) \
O(JmpLt,                       D(None), S(Gen) S(Gen),                     E) \
O(JmpLte,                      D(None), S(Gen) S(Gen),                     E) \
O(JmpEq,                       D(None), S(Gen) S(Gen),                     E) \
O(JmpNeq,                      D(None), S(Gen) S(Gen),                     E) \
O(JmpSame,                     D(None), S(Gen) S(Gen),                     E) \
O(JmpNSame,                    D(None), S(Gen) S(Gen),                     E) \
O(JmpInstanceOf,               D(None), S(Cls) S(Cls) C(Bool),           E|N) \
O(JmpNInstanceOf,              D(None), S(Cls) S(Cls) C(Bool),           E|N) \
O(JmpInstanceOfBitmask,        D(None), S(Cls) CStr,                       E) \
O(JmpNInstanceOfBitmask,       D(None), S(Cls) CStr,                       E) \
O(JmpIsType,                   D(None), SUnk,                              E) \
O(JmpIsNType,                  D(None), SUnk,                              E) \
  /* TODO(#2058842) keep preceeding conditional branches contiguous */        \
                                                                              \
/*    name                      dstinfo srcinfo                      flags */ \
O(JmpZero,                     D(None), SNum,                              E) \
O(JmpNZero,                    D(None), SNum,                              E) \
O(Jmp_,                        D(None), SUnk,                            T|E) \
O(JmpIndirect,                      ND, S(TCA),                          T|E) \
O(ExitWhenSurprised,                ND, NA,                                E) \
O(ExitOnVarEnv,                     ND, S(StkPtr),                         E) \
O(ReleaseVVOrExit,                  ND, S(StkPtr),                         E) \
O(CheckInit,                        ND, S(Gen),                           NF) \
O(Unbox,                     DUnbox(0), S(Gen),                          PRc) \
O(Box,                         DBox(0), S(Gen),              E|N|Mem|CRc|PRc) \
O(UnboxPtr,               D(PtrToCell), S(PtrToGen),                      NF) \
O(BoxPtr,            D(PtrToBoxedCell), S(PtrToGen),                   N|Mem) \
O(LdStack,                      DParam, S(StkPtr) C(Int),                 NF) \
O(LdLoc,                        DParam, S(StkPtr),                        NF) \
O(LdStackAddr,             D(PtrToGen), SUnk,                              C) \
O(LdLocAddr,                    DParam, S(StkPtr),                         C) \
O(LdMem,                        DParam, S(PtrToGen) C(Int),               NF) \
O(LdProp,                       DParam, S(Obj) C(Int),                    NF) \
O(LdRef,                        DParam, S(BoxedCell),                     NF) \
O(LdThis,                       D(Obj), S(StkPtr),                      C|Rm) \
O(LdCtx,                        D(Ctx), S(StkPtr),                      C|Rm) \
O(LdCtxCls,                     D(Cls), S(StkPtr),                      C|Rm) \
O(LdRetAddr,                D(RetAddr), S(StkPtr),                        NF) \
O(LdConst,                      DParam, NA,                             C|Rm) \
O(DefConst,                     DParam, NA,                                C) \
O(LdCls,                        D(Cls), S(Str) C(Cls),     C|E|N|Refs|Er|Mem) \
O(LdClsCached,                  D(Cls), CStr,              C|E|  Refs|Er|Mem) \
O(LdCachedClass,                D(Cls), CStr,                              C) \
O(LdClsCns,                     DParam, CStr CStr,                         C) \
O(LookupClsCns,                 DParam, CStr CStr,           E|Refs|Er|N|Mem) \
O(LdClsMethodCache,         D(FuncCls), SUnk,                           C|Er) \
O(LdClsMethodFCache,        D(FuncCtx), C(Cls) CStr S(Obj,Cls,Ctx),     C|Er) \
O(GetCtxFwdCall,                D(Ctx), S(Obj,Cls,Ctx) S(Func),            C) \
O(LdClsMethod,                 D(Func), S(Cls) C(Int),                     C) \
O(LdPropAddr,              D(PtrToGen), S(Obj) C(Int),                     C) \
O(LdClsPropAddr,           D(PtrToGen), S(Cls) S(Str) C(Cls),       C|E|N|Er) \
O(LdClsPropAddrCached,     D(PtrToGen), S(Cls) CStr CStr C(Cls),    C|E|  Er) \
O(LdObjMethod,                 D(Func), C(Int) CStr S(StkPtr), C|E|N|Refs|Er) \
O(LdObjClass,                   D(Cls), S(Obj),                            C) \
O(LdFunc,                      D(Func), S(Str),                   E|N|CRc|Er) \
O(LdFixedFunc,                 D(Func), CStr,                         C|E|Er) \
O(LdARFuncPtr,                 D(Func), S(StkPtr) C(Int),                  C) \
O(LdContLocalsPtr,        D(PtrToCell), S(Obj),                         C|Rm) \
O(LdSSwitchDestFast,            D(TCA), S(Gen),                         N|Rm) \
O(LdSSwitchDestSlow,            D(TCA), S(Gen),                 N|Rm|Refs|Er) \
O(NewObj,                    D(StkPtr), C(Int)                                \
                                          S(Str,Cls)                          \
                                          S(StkPtr)                           \
                                          S(StkPtr),             E|Mem|N|PRc) \
O(NewArray,                     D(Arr), C(Int),                  E|Mem|N|PRc) \
O(NewTuple,                     D(Arr), C(Int) S(StkPtr),    E|Mem|N|PRc|CRc) \
O(LdRaw,                        DParam, SUnk,                             NF) \
O(DefActRec,                 D(ActRec), S(StkPtr)                             \
                                          S(Func,FuncCls,FuncCtx,Null)        \
                                          S(Ctx,Cls,InitNull)                 \
                                          C(Int)                              \
                                          S(Str,Null),                   Mem) \
O(FreeActRec,                D(StkPtr), S(StkPtr),                       Mem) \
/*    name                      dstinfo srcinfo                      flags */ \
O(Call,                      D(StkPtr), SUnk,                 E|Mem|CRc|Refs) \
O(NativeImpl,                       ND, C(Func) S(StkPtr),      E|Mem|N|Refs) \
  /* XXX: why does RetCtrl sometimes get PtrToGen */                          \
O(RetCtrl,                          ND, S(StkPtr,PtrToGen)                    \
                                          S(StkPtr)                           \
                                          S(RetAddr),                T|E|Mem) \
O(RetVal,                           ND, S(StkPtr) S(Gen),          E|Mem|CRc) \
O(RetAdjustStack,            D(StkPtr), S(StkPtr),                         E) \
O(StMem,                            ND, S(PtrToCell)                          \
                                          C(Int) S(Gen),      E|Mem|CRc|Refs) \
O(StMemNT,                          ND, S(PtrToCell)                          \
                                          C(Int) S(Gen),           E|Mem|CRc) \
O(StProp,                           ND, S(Obj) S(Int) S(Gen), E|Mem|CRc|Refs) \
O(StPropNT,                         ND, S(Obj) S(Int) S(Gen),      E|Mem|CRc) \
O(StLoc,                            ND, S(StkPtr) S(Gen),          E|Mem|CRc) \
O(StLocNT,                          ND, S(StkPtr) S(Gen),          E|Mem|CRc) \
O(StRef,                       DBox(1), SUnk,                 E|Mem|CRc|Refs) \
O(StRefNT,                     DBox(1), SUnk,                      E|Mem|CRc) \
O(StRaw,                            ND, SUnk,                          E|Mem) \
O(SpillStack,                D(StkPtr), SUnk,                      E|Mem|CRc) \
O(ExitTrace,                        ND, SUnk,                            T|E) \
O(ExitTraceCc,                      ND, SUnk,                            T|E) \
O(ExitSlow,                         ND, SUnk,                            T|E) \
O(ExitSlowNoProgress,               ND, SUnk,                            T|E) \
O(ExitGuardFailure,                 ND, SUnk,                            T|E) \
O(SyncVMRegs,                       ND, S(StkPtr) S(StkPtr),               E) \
O(Mov,                         DofS(0), SUnk,                              C) \
O(LdAddr,                      DofS(0), SUnk,                              C) \
O(IncRef,                      DofS(0), S(Gen),                      Mem|PRc) \
O(DecRefLoc,                        ND, S(StkPtr),                E|Mem|Refs) \
O(DecRefStack,                      ND, S(StkPtr) C(Int),         E|Mem|Refs) \
O(DecRefThis,                       ND, SUnk,                     E|Mem|Refs) \
O(GenericRetDecRefs,         D(StkPtr), S(StkPtr)                             \
                                          S(Gen) C(Int),        E|N|Mem|Refs) \
O(DecRef,                           ND, S(Gen),               E|Mem|CRc|Refs) \
O(DecRefMem,                        ND, S(PtrToGen)                           \
                                          C(Int),             E|Mem|CRc|Refs) \
O(DecRefNZ,                         ND, S(Gen),                      Mem|CRc) \
O(DefLabel,                     DLabel, SUnk,                              E) \
O(Marker,                           ND, NA,                                E) \
O(DefFP,                     D(StkPtr), NA,                                E) \
O(DefSP,                     D(StkPtr), S(StkPtr) C(Int),                  E) \
O(RaiseUninitWarning,               ND, S(Str),              E|N|Mem|Refs|Er) \
O(PrintStr,                         ND, S(Str),                  E|N|Mem|CRc) \
O(PrintInt,                         ND, S(Int),                  E|N|Mem|CRc) \
O(PrintBool,                        ND, S(Bool),                 E|N|Mem|CRc) \
O(AddElemStrKey,                D(Arr), S(Arr)                                \
                                          S(Str)                              \
                                          S(Cell),        N|Mem|CRc|PRc|Refs) \
O(AddElemIntKey,                D(Arr), S(Arr)                                \
                                          S(Int)                              \
                                          S(Cell),        N|Mem|CRc|PRc|Refs) \
O(AddNewElem,                   D(Arr), SUnk,                  N|Mem|CRc|PRc) \
/*    name                      dstinfo srcinfo                      flags */ \
O(DefCns,                      D(Bool), SUnk,                      C|E|N|Mem) \
O(Concat,                       D(Str), S(Gen) S(Gen),    N|Mem|CRc|PRc|Refs) \
O(ArrayAdd,                     D(Arr), SUnk,                  N|Mem|CRc|PRc) \
O(DefCls,                           ND, SUnk,                          C|E|N) \
O(DefFunc,                          ND, SUnk,                          C|E|N) \
O(AKExists,                    D(Bool), S(Cell) S(Cell),                 C|N) \
O(InterpOne,                 D(StkPtr), SUnk,                E|N|Mem|Refs|Er) \
O(Spill,                       DofS(0), SUnk,                            Mem) \
O(Reload,                      DofS(0), SUnk,                            Mem) \
O(AllocSpill,                       ND, C(Int),                        E|Mem) \
O(FreeSpill,                        ND, C(Int),                        E|Mem) \
O(CreateCont,                   D(Obj), C(TCA)                                \
                                          S(StkPtr)                           \
                                          C(Bool)                             \
                                          C(Func)                             \
                                          C(Func),               E|N|Mem|PRc) \
O(FillContLocals,                   ND, S(StkPtr)                             \
                                          C(Func)                             \
                                          C(Func)                             \
                                          S(Obj),                    E|N|Mem) \
O(FillContThis,                     ND, S(StkPtr)                             \
                                          C(Func) C(Func) S(Obj),      E|Mem) \
O(ContEnter,                        ND, SUnk,                          E|Mem) \
O(UnlinkContVarEnv,                 ND, S(StkPtr),                   E|N|Mem) \
O(LinkContVarEnv,                   ND, S(StkPtr),                   E|N|Mem) \
O(ContRaiseCheck,                   ND, S(Obj),                            E) \
O(ContPreNext,                      ND, S(Obj),                        E|Mem) \
O(ContStartedCheck,                 ND, S(Obj),                            E) \
O(IterInit,                    D(Bool), S(Arr,Obj)                            \
                                          S(StkPtr)                           \
                                          C(Int)                              \
                                          C(Int),             N|Mem|Refs|CRc) \
O(IterInitK,                   D(Bool), S(Arr,Obj)                            \
                                          S(StkPtr)                           \
                                          C(Int)                              \
                                          C(Int)                              \
                                          C(Int),             N|Mem|Refs|CRc) \
O(IterNext,                    D(Bool), S(StkPtr) C(Int) C(Int),  N|Mem|Refs) \
O(IterNextK,                   D(Bool), S(StkPtr)                             \
                                          C(Int) C(Int) C(Int),   N|Mem|Refs) \
O(DefMIStateBase,         D(PtrToCell), NA,                               NF) \
O(PropX,                   D(PtrToGen), C(TCA)                                \
                                          C(Cls)                              \
                                          S(Obj,PtrToGen)                     \
                                          S(Gen)                              \
                                          S(PtrToCell),      E|N|Mem|Refs|Er) \
O(CGetProp,                    D(Cell), C(TCA)                                \
                                          C(Cls)                              \
                                          S(Obj,PtrToGen)                     \
                                          S(Gen)                              \
                                          S(PtrToCell),      E|N|Mem|Refs|Er) \
O(SetProp,                     DVector, C(TCA)                                \
                                          C(Cls)                              \
                                          S(Obj,PtrToGen)                     \
                                          S(Gen)                              \
                                          S(Cell),           E|N|Mem|Refs|Er) \
O(CGetElem,                    D(Cell), C(TCA)                                \
                                          S(PtrToGen)                         \
                                          S(Gen)                              \
                                          S(PtrToCell),      E|N|Mem|Refs|Er) \
O(SetElem,                     DVector, C(TCA)                                \
                                          S(PtrToGen)                         \
                                          S(Gen)                              \
                                          S(Cell),           E|N|Mem|Refs|Er) \
O(IncStat,                          ND, C(Int) C(Int) C(Bool),         E|Mem) \
O(DbgAssertRefCount,                ND, SUnk,                              E) \
O(Nop,                              ND, NA,                               NF) \
/* */

enum Opcode : uint16_t {
#define O(name, dsts, srcs, flags) name,
  IR_OPCODES
#undef O
  IR_NUM_OPCODES
};

//////////////////////////////////////////////////////////////////////

/*
 * Some IRInstructions with compile-time-only constants may carry
 * along extra data in the form of one of these structures.
 *
 * Note that this isn't really appropriate for compile-time constants
 * that are actually representing user values (we want them to be
 * visible to optimization passes, allocatable to registers, etc),
 * just compile-time metadata.
 *
 * These types must:
 *
 *   - Derive from IRExtraData (for overloading purposes)
 *   - Be arena-allocatable (no non-trivial destructors)
 *   - Either CopyConstructible, or implement a clone member
 *     function that takes an arena to clone to
 *
 * In addition, for extra data used with a cse-able instruction:
 *
 *   - Implement an equals() member that indicates equality for CSE
 *     purposes.
 *   - Implement a hash() method.
 *
 * Finally, optionally they may implement a show() method for use in
 * debug printouts.
 */

/*
 * Traits that returns the type of the extra C++ data structure for a
 * given instruction, if it has one, along with some other information
 * about the type.
 */
template<Opcode op> struct OpHasExtraData { enum { value = 0 }; };
template<Opcode op> struct IRExtraDataType;

//////////////////////////////////////////////////////////////////////

struct IRExtraData {};

struct LdSSwitchData : IRExtraData {
  struct Elm {
    const StringData* str;
    Offset            dest;
  };

  explicit LdSSwitchData() = default;
  LdSSwitchData(const LdSSwitchData&) = delete;
  LdSSwitchData& operator=(const LdSSwitchData&) = delete;

  LdSSwitchData* clone(Arena& arena) const {
    LdSSwitchData* target = new (arena) LdSSwitchData;
    target->func       = func;
    target->numCases   = numCases;
    target->defaultOff = defaultOff;
    target->cases      = new (arena) Elm[numCases];
    std::copy(cases, cases + numCases, const_cast<Elm*>(target->cases));
    return target;
  }

  const Func* func;
  int64_t     numCases;
  const Elm*  cases;
  Offset      defaultOff;
};

struct MarkerData : IRExtraData {
  uint32_t    bcOff;    // the bytecode offset in unit
  int32_t     stackOff; // stack off from start of trace
  const Func* func;     // which func are we in
};

struct LocalId : IRExtraData {
  explicit LocalId(uint32_t id)
    : locId(id)
  {}

  bool equals(LocalId o) const { return locId == o.locId; }
  size_t hash() const { return std::hash<uint32_t>()(locId); }
  std::string show() const { return folly::to<std::string>(locId); }

  uint32_t locId;
};

struct ConstData : IRExtraData {
  template<class T>
  explicit ConstData(T data)
    : m_dataBits(0)
  {
    static_assert(sizeof(T) <= sizeof m_dataBits,
                  "Constant data was larger than supported");
    static_assert(std::is_pod<T>::value,
                  "Constant data wasn't a pod?");
    std::memcpy(&m_dataBits, &data, sizeof data);
  }

  template<class T>
  T as() const {
    T ret;
    std::memcpy(&ret, &m_dataBits, sizeof ret);
    return ret;
  }

  bool equals(ConstData o) const { return m_dataBits == o.m_dataBits; }
  size_t hash() const { return std::hash<uintptr_t>()(m_dataBits); }

private:
  uintptr_t m_dataBits;
};

//////////////////////////////////////////////////////////////////////

#define X(op, data)                                                   \
  template<> struct IRExtraDataType<op> { typedef data type; };       \
  template<> struct OpHasExtraData<op> { enum { value = 1 }; };       \
  static_assert(boost::has_trivial_destructor<data>::value,           \
                "IR extra data type must be trivially destructible")

X(LdSSwitchDestFast,  LdSSwitchData);
X(LdSSwitchDestSlow,  LdSSwitchData);
X(Marker,             MarkerData);
X(RaiseUninitWarning, LocalId);
X(GuardLoc,           LocalId);
X(AssertLoc,          LocalId);
X(LdLocAddr,          LocalId);
X(DecRefLoc,          LocalId);
X(LdLoc,              LocalId);
X(StLoc,              LocalId);
X(StLocNT,            LocalId);
X(DefConst,           ConstData);
X(LdConst,            ConstData);

#undef X

//////////////////////////////////////////////////////////////////////

template<bool hasExtra, Opcode opc, class T> struct AssertExtraTypes {
  static void doassert() {
    assert(!"called getExtra on an opcode without extra data");
  }
};

template<Opcode opc, class T> struct AssertExtraTypes<true,opc,T> {
  static void doassert() {
    typedef typename IRExtraDataType<opc>::type ExtraType;
    if (!std::is_same<ExtraType,T>::value) {
      assert(!"getExtra<T> was called with an extra data "
              "type that doesn't match the opcode type");
    }
  }
};

// Asserts that Opcode opc has extradata and it is of type T.
template<class T> void assert_opcode_extra(Opcode opc) {
#define O(opcode, dstinfo, srcinfo, flags)      \
  case opcode:                                  \
    AssertExtraTypes<                           \
      OpHasExtraData<opcode>::value,opcode,T    \
    >::doassert();                              \
    break;
  switch (opc) { IR_OPCODES default: not_reached(); }
#undef O
}

//////////////////////////////////////////////////////////////////////

inline bool isCmpOp(Opcode opc) {
  return (opc >= OpGt && opc <= OpNSame);
}

// A "query op" is any instruction returning Type::Bool that is both
// branch-fusable and negateable.
inline bool isQueryOp(Opcode opc) {
  return (opc >= OpGt && opc <= IsNType);
}

inline Opcode queryToJmpOp(Opcode opc) {
  assert(isQueryOp(opc));
  return (Opcode)(JmpGt + (opc - OpGt));
}

inline bool isQueryJmpOp(Opcode opc) {
  return opc >= JmpGt && opc <= JmpIsNType;
}

inline Opcode queryJmpToQueryOp(Opcode opc) {
  assert(isQueryJmpOp(opc));
  return Opcode(OpGt + (opc - JmpGt));
}

/*
 * Right now branch fusion is too indiscriminate to handle fusing
 * with potentially expensive-to-repeat operations.  TODO(#2053369)
 */
inline bool disableBranchFusion(Opcode opc) {
  return opc == InstanceOf ||
         opc == NInstanceOf ||
         opc == InstanceOfBitmask ||
         opc == NInstanceOfBitmask;
}

/*
 * Return the opcode that corresponds to negation of opc.
 */
Opcode negateQueryOp(Opcode opc);

extern Opcode queryCommuteTable[];

inline Opcode commuteQueryOp(Opcode opc) {
  assert(opc >= OpGt && opc <= OpNSame);
  return queryCommuteTable[opc - OpGt];
}

namespace TraceExitType {
// Must update in sync with ExitTrace entries in OPC table above
enum ExitType {
  Normal,
  NormalCc,
  Slow,
  SlowNoProgress,
  GuardFailure
};
}

extern TraceExitType::ExitType getExitType(Opcode opc);
extern Opcode getExitOpcode(TraceExitType::ExitType);
inline bool isExitSlow(TraceExitType::ExitType t) {
  return t == TraceExitType::Slow || t == TraceExitType::SlowNoProgress;
}

const char* opcodeName(Opcode opcode);

#define IRT_BOXES(name, bit)                                            \
  IRT(name,             (bit))                                          \
  IRT(Boxed##name,      (bit) << kBoxShift)                             \
  IRT(PtrTo##name,      (bit) << kPtrShift)                             \
  IRT(PtrToBoxed##name, (bit) << kPtrBoxShift)

#define IRT_PHP(c)                                                      \
  c(Uninit,       1ULL << 0)                                            \
  c(InitNull,     1ULL << 1)                                            \
  c(Bool,         1ULL << 2)                                            \
  c(Int,          1ULL << 3)                                            \
  c(Dbl,          1ULL << 4)                                            \
  c(StaticStr,    1ULL << 5)                                            \
  c(CountedStr,   1ULL << 6)                                            \
  c(StaticArr,    1ULL << 7)                                            \
  c(CountedArr,   1ULL << 8)                                            \
  c(Obj,          1ULL << 9)
// Boxed*:       10-19
// PtrTo*:       20-29
// PtrToBoxed*:  30-39

// This list should be in non-decreasing order of specificity
#define IRT_PHP_UNIONS(c)                                               \
  c(Null,          kUninit|kInitNull)                                   \
  c(Str,           kStaticStr|kCountedStr)                              \
  c(Arr,           kStaticArr|kCountedArr)                              \
  c(UncountedInit, kInitNull|kBool|kInt|kDbl|kStaticStr|kStaticArr)     \
  c(Uncounted,     kUncountedInit|kUninit)                              \
  c(Cell,          kUncounted|kStr|kArr|kObj)

#define IRT_RUNTIME                                                     \
  IRT(Cls,         1ULL << 40)                                          \
  IRT(Func,        1ULL << 41)                                          \
  IRT(VarEnv,      1ULL << 42)                                          \
  IRT(NamedEntity, 1ULL << 43)                                          \
  IRT(FuncCls,     1ULL << 44) /* {Func*, Cctx} */                      \
  IRT(FuncObj,     1ULL << 45) /* {Func*, Obj} */                       \
  IRT(Cctx,        1ULL << 46) /* Class* with the lowest bit set,  */   \
                               /* as stored in ActRec.m_cls field  */   \
  IRT(RetAddr,     1ULL << 47) /* Return address */                     \
  IRT(StkPtr,      1ULL << 48) /* any pointer into VM stack: VmSP or VmFP*/ \
  IRT(TCA,         1ULL << 49)                                          \
  IRT(ActRec,      1ULL << 50)                                          \
  IRT(None,        1ULL << 51)

// The definitions for these are in ir.cpp
#define IRT_UNIONS                                                      \
  IRT(Ctx,         kObj|kCctx)                                          \
  IRT(FuncCtx,     kFuncCls|kFuncObj)

// Gen, Counted, PtrToGen, and PtrToCounted are here instead of
// IRT_PHP_UNIONS because boxing them (e.g., BoxedGen, PtrToBoxedGen)
// would be nonsense types.
#define IRT_SPECIAL                                                 \
  IRT(Bottom,       0)                                              \
  IRT(Counted,      kCountedStr|kCountedArr|kObj|kBoxedCell)        \
  IRT(PtrToCounted, kCounted << kPtrShift)                          \
  IRT(Gen,          kCell|kBoxedCell)                               \
  IRT(PtrToGen,     kGen << kPtrShift)

// All types (including union types) that represent program values,
// except Gen (which is special). Boxed*, PtrTo*, and PtrToBoxed* only
// exist for these types.
#define IRT_USERLAND(c) IRT_PHP(c) IRT_PHP_UNIONS(c)

// All types with just a single bit set
#define IRT_PRIMITIVE IRT_PHP(IRT_BOXES) IRT_RUNTIME

// All types
#define IR_TYPES IRT_USERLAND(IRT_BOXES) IRT_RUNTIME IRT_UNIONS IRT_SPECIAL

class Type {
  typedef uint64_t bits_t;

  static const size_t kBoxShift = 10;
  static const size_t kPtrShift = kBoxShift * 2;
  static const size_t kPtrBoxShift = kBoxShift + kPtrShift;

  enum TypeBits {
#define IRT(name, bits) k##name = (bits),
  IR_TYPES
#undef IRT
  };

  union {
    bits_t m_bits;
    TypeBits m_typedBits;
  };

public:
# define IRT(name, ...) static const Type name;
  IR_TYPES
# undef IRT

  explicit Type(bits_t bits = kNone)
    : m_bits(bits)
    {}

  size_t hash() const {
    return hash_int64(m_bits);
  }

  bool operator==(Type other) const {
    return m_bits == other.m_bits;
  }

  bool operator!=(Type other) const {
    return !operator==(other);
  }

  Type operator|(Type other) const {
    return Type(m_bits | other.m_bits);
  }

  Type operator&(Type other) const {
    return Type(m_bits & other.m_bits);
  }

  std::string toString() const;
  static std::string debugString(Type t);
  static Type fromString(const std::string& str);

  bool isBoxed() const {
    return subtypeOf(BoxedCell);
  }

  bool notBoxed() const {
    return subtypeOf(Cell);
  }

  bool maybeBoxed() const {
    return (*this & BoxedCell) != Bottom;
  }

  bool isPtr() const {
    return subtypeOf(PtrToGen);
  }

  bool isCounted() const {
    return subtypeOf(Counted);
  }

  bool maybeCounted() const {
    return (*this & Counted) != Bottom;
  }

  bool notCounted() const {
    return !maybeCounted();
  }

  bool isStaticallyKnown() const {
    // Str, Arr and Null are technically unions but are statically
    // known for all practical purposes. Same for a union that
    // consists of nothing but boxed types.
    if (isString() || isArray() || isNull() ||
        (isPtr() &&
         (deref().isString() || deref().isArray() || deref().isNull())) ||
        isBoxed()) {
      return true;
    }

    // This will return true iff no more than 1 bit is set in
    // m_bits. If 0 bits are set, then *this == Bottom, which is
    // statically known.
    return (m_bits & (m_bits - 1)) == 0;
  }

  bool isStaticallyKnownUnboxed() const {
    return isStaticallyKnown() && notBoxed();
  }

  bool needsStaticBitCheck() const {
    return (*this & (StaticStr | StaticArr)) != Bottom;
  }

  // returns true if definitely not uninitialized
  bool isInit() const {
    return !Uninit.subtypeOf(*this);
  }

  bool maybeUninit() const {
    return !isInit();
  }

  /*
   * Returns true if this is a strict subtype of t2.
   */
  bool strictSubtypeOf(Type t2) const {
    return *this != t2 && subtypeOf(t2);
  }

  /*
   * Returns true if this is a non-strict subtype of t2.
   */
  bool subtypeOf(Type t2) const {
    return (m_bits & t2.m_bits) == m_bits;
  }

  /*
   * Returns true if this is exactly equal to t2. Be careful: you
   * probably mean subtypeOf.
   */
  bool equals(Type t2) const {
    return m_bits == t2.m_bits;
  }

  /*
   * Returns the most refined of two types.
   *
   * Pre: the types must not be completely unrelated.
   */
  static Type mostRefined(Type t1, Type t2) {
    assert(t1.subtypeOf(t2) || t2.subtypeOf(t1));
    return t1.subtypeOf(t2) ? t1 : t2;
  }

  static Type binArithResultType(Type t1, Type t2) {
    if (t1.subtypeOf(Type::Dbl) || t2.subtypeOf(Type::Dbl)) {
      return Type::Dbl;
    }
    return Type::Int;
  }

  bool isArray() const {
    return subtypeOf(Arr);
  }

  bool isString() const {
    return subtypeOf(Str);
  }

  bool isNull() const {
    return subtypeOf(Null);
  }

  Type innerType() const {
    assert(isBoxed());
    return Type(m_bits >> kBoxShift);
  }

  /*
   * unionOf: return the least common predefined supertype of t1 and
   * t2, i.e.. the most refined type t3 such that t1 <: t3 and t2 <:
   * t3. Note that arbitrary union types are possible, but this
   * function always returns one of the predefined types.
   */
  static Type unionOf(Type t1, Type t2) {
    assert(t1.subtypeOf(Gen) == t2.subtypeOf(Gen));
    assert(t1.subtypeOf(Gen) || t1 == t2); // can only union TypeValue types
    if (t1 == t2) return t1;
    if (t1.subtypeOf(t2)) return t2;
    if (t2.subtypeOf(t1)) return t1;
    static const Type union_types[] = {
#   define IRT(name, ...) name, Boxed##name,
    IRT_PHP_UNIONS(IRT)
#   undef IRT
      Gen,
    };
    Type t12 = t1 | t2;
    for (auto u : union_types) {
      if (t12.subtypeOf(u)) return u;
    }
    not_reached();
  }

  Type box() const {
    // XXX: Get php-specific logic that isn't a natural property of a
    // sane type system out of this class (such as Uninit.box() ==
    // BoxedNull). #2087268
    assert(subtypeOf(Gen));
    assert(notBoxed());
    if (subtypeOf(Uninit)) {
      return BoxedNull;
    }
    return Type(m_bits << kBoxShift);
  }

  // This computes the type effects of the Unbox opcode.
  Type unbox() const {
    assert(subtypeOf(Gen));
    return (*this & Cell) | (*this & BoxedCell).innerType();
  }

  Type deref() const {
    assert(isPtr());
    return Type(m_bits >> kPtrShift);
  }

  Type derefIfPtr() const {
    assert(subtypeOf(Gen | PtrToGen));
    return isPtr() ? deref() : *this;
  }

  Type ptr() const {
    assert(!isPtr());
    assert(subtypeOf(Gen));
    return Type(m_bits << kPtrShift);
  }

  bool canRunDtor() const {
    return
      (*this & (Obj | CountedArr | BoxedObj | BoxedCountedArr))
      != Type::Bottom;
  }

  // translates a compiler Type to an HPHP::DataType
  DataType toDataType() const {
    assert(!isPtr());
    if (isBoxed()) {
      return KindOfRef;
    }

    // Order is important here: types must progress from more specific
    // to less specific to return the most specific DataType.
    if (subtypeOf(None))          return KindOfInvalid;
    if (subtypeOf(Uninit))        return KindOfUninit;
    if (subtypeOf(Null))          return KindOfNull;
    if (subtypeOf(Bool))          return KindOfBoolean;
    if (subtypeOf(Int))           return KindOfInt64;
    if (subtypeOf(Dbl))           return KindOfDouble;
    if (subtypeOf(StaticStr))     return KindOfStaticString;
    if (subtypeOf(Str))           return KindOfString;
    if (subtypeOf(Arr))           return KindOfArray;
    if (subtypeOf(Obj))           return KindOfObject;
    if (subtypeOf(Cls))           return KindOfClass;
    if (subtypeOf(UncountedInit)) return KindOfUncountedInit;
    if (subtypeOf(Uncounted))     return KindOfUncounted;
    if (subtypeOf(Gen))           return KindOfAny;
    not_reached();
  }

  static Type fromDataType(DataType outerType,
                           DataType innerType = KindOfInvalid) {
    assert(innerType != KindOfRef);

    switch (outerType) {
      case KindOfInvalid       : return None;
      case KindOfUninit        : return Uninit;
      case KindOfNull          : return InitNull;
      case KindOfBoolean       : return Bool;
      case KindOfInt64         : return Int;
      case KindOfDouble        : return Dbl;
      case KindOfStaticString  : return StaticStr;
      case KindOfString        : return Str;
      case KindOfArray         : return Arr;
      case KindOfObject        : return Obj;
      case KindOfClass         : return Cls;
      case KindOfUncountedInit : return UncountedInit;
      case KindOfUncounted     : return Uncounted;
      case KindOfAny           : return Gen;
      case KindOfRef: {
        if (innerType == KindOfInvalid) {
          return BoxedCell;
        } else {
          return fromDataType(innerType).box();
        }
      }
      default                  : not_reached();
    }
  }

  static Type fromRuntimeType(const Transl::RuntimeType& rtt) {
    return fromDataType(rtt.outerType(), rtt.innerType());
  }
}; // class Type

static_assert(sizeof(Type) <= sizeof(uint64_t),
              "JIT::Type should fit in a register");

/*
 * typeForConst(T)
 *
 *   returns the Type type for a C++ type that may be used with
 *   ConstData.
 */

// The only interesting case is int/bool disambiguation.
template<class T>
typename std::enable_if<
  std::is_integral<T>::value,
  Type
>::type typeForConst(T) {
  return std::is_same<T,bool>::value ? Type::Bool : Type::Int;
}

inline Type typeForConst(const StringData*)  { return Type::StaticStr; }
inline Type typeForConst(const NamedEntity*) { return Type::NamedEntity; }
inline Type typeForConst(const Func*)        { return Type::Func; }
inline Type typeForConst(const Class*)       { return Type::Cls; }
inline Type typeForConst(TCA)                { return Type::TCA; }
inline Type typeForConst(double)             { return Type::Dbl; }
inline Type typeForConst(const ArrayData* ad) {
  assert(ad->isStatic());
  return Type::StaticArr;
}

bool cmpOpTypesMayReenter(Opcode, Type t0, Type t1);

class RawMemSlot {
 public:

  enum Kind {
    ContLabel, ContDone, ContShouldThrow, ContRunning, ContARPtr,
    StrLen, FuncNumParams, FuncRefBitVec, FuncBody, MisBaseStrOff,
    MaxKind
  };

  static RawMemSlot& Get(Kind k) {
    switch (k) {
      case ContLabel:       return GetContLabel();
      case ContDone:        return GetContDone();
      case ContShouldThrow: return GetContShouldThrow();
      case ContRunning:     return GetContRunning();
      case ContARPtr:       return GetContARPtr();
      case StrLen:          return GetStrLen();
      case FuncNumParams:   return GetFuncNumParams();
      case FuncRefBitVec:   return GetFuncRefBitVec();
      case FuncBody:        return GetFuncBody();
      case MisBaseStrOff:   return GetMisBaseStrOff();
      default: not_reached();
    }
  }

  int64 getOffset()   const { return m_offset; }
  int32 getSize()     const { return m_size; }
  Type getType() const { return m_type; }
  bool allowExtra()   const { return m_allowExtra; }

 private:
  RawMemSlot(int64 offset, int32 size, Type type, bool allowExtra = false)
    : m_offset(offset), m_size(size), m_type(type), m_allowExtra(allowExtra) { }

  static RawMemSlot& GetContLabel() {
    static RawMemSlot m(CONTOFF(m_label), sz::qword, Type::Int);
    return m;
  }
  static RawMemSlot& GetContDone() {
    static RawMemSlot m(CONTOFF(m_done), sz::byte, Type::Bool);
    return m;
  }
  static RawMemSlot& GetContShouldThrow() {
    static RawMemSlot m(CONTOFF(m_should_throw), sz::byte, Type::Bool);
    return m;
  }
  static RawMemSlot& GetContRunning() {
    static RawMemSlot m(CONTOFF(m_running), sz::byte, Type::Bool);
    return m;
  }
  static RawMemSlot& GetContARPtr() {
    static RawMemSlot m(CONTOFF(m_arPtr), sz::qword, Type::StkPtr);
    return m;
  }
  static RawMemSlot& GetStrLen() {
    static RawMemSlot m(StringData::sizeOffset(), sz::dword, Type::Int);
    return m;
  }
  static RawMemSlot& GetFuncNumParams() {
    static RawMemSlot m(Func::numParamsOff(), sz::qword, Type::Int);
    return m;
  }
  static RawMemSlot& GetFuncRefBitVec() {
    static RawMemSlot m(Func::refBitVecOff(), sz::qword, Type::Int);
    return m;
  }
  static RawMemSlot& GetFuncBody() {
    static RawMemSlot m(Func::funcBodyOff(), sz::qword, Type::TCA);
    return m;
  }
  static RawMemSlot& GetMisBaseStrOff() {
    static RawMemSlot m(MISOFF(baseStrOff), sz::byte, Type::Bool, true);
    return m;
  }

  int64 m_offset;
  int32 m_size;
  Type m_type;
  bool m_allowExtra; // Used as a flag to ensure that extra offets are
                     // only used with RawMemSlots that support it
};

class SSATmp;
class Trace;
class CodeGenerator;
class IRFactory;
class Simplifier;

bool isRefCounted(SSATmp* opnd);

class LabelInstruction;

using folly::Range;

typedef Range<SSATmp**> SSARange;

/*
 * All IRInstruction subclasses must be arena-allocatable.
 * (Destructors are not called when they come from IRFactory.)
 */
struct IRInstruction {
  typedef std::list<IRInstruction*> List;
  typedef std::list<IRInstruction*>::iterator Iterator;
  typedef std::list<IRInstruction*>::reverse_iterator ReverseIterator;
  enum IId { kTransient = 0xffffffff };

  /*
   * Create an IRInstruction for the opcode `op'.
   *
   * IRInstruction creation is usually done through IRFactory or
   * TraceBuilder rather than directly.
   */
  explicit IRInstruction(Opcode op,
                         uint32_t numSrcs = 0,
                         SSATmp** srcs = nullptr)
    : m_op(op)
    , m_typeParam(Type::None)
    , m_numSrcs(numSrcs)
    , m_numDsts(0)
    , m_iid(kTransient)
    , m_id(0)
    , m_srcs(srcs)
    , m_dst(nullptr)
    , m_asmAddr(nullptr)
    , m_label(nullptr)
    , m_parent(nullptr)
    , m_tca(nullptr)
    , m_extra(nullptr)
  {}

  IRInstruction(const IRInstruction&) = delete;
  IRInstruction& operator=(const IRInstruction&) = delete;

  /*
   * Construct an IRInstruction as a deep copy of `inst', using
   * arena to allocate memory for its srcs/dests.
   */
  explicit IRInstruction(Arena& arena, const IRInstruction* inst,
                         IId iid);

  /*
   * Initialize the source list for this IRInstruction.  We must not
   * have already had our sources initialized before this function is
   * called.
   *
   * Memory for `srcs' is owned outside of this class and must outlive
   * it.
   */
  void initializeSrcs(uint32_t numSrcs, SSATmp** srcs) {
    assert(!m_srcs && !m_numSrcs);
    m_numSrcs = numSrcs;
    m_srcs = srcs;
  }

  /*
   * Return access to extra-data on this instruction, for the
   * specified opcode type.
   *
   * Pre: getOpcode() == opc
   */
  template<Opcode opc>
  const typename IRExtraDataType<opc>::type* getExtra() const {
    assert(opc == getOpcode() && "getExtra type error");
    assert(m_extra != nullptr);
    return static_cast<typename IRExtraDataType<opc>::type*>(m_extra);
  }

  /*
   * Return access to extra-data of type T.  Requires that
   * IRExtraDataType<opc>::type is T for this instruction's opcode.
   *
   * It's normally preferable to use the version of this function that
   * takes the opcode instead of this one.  This is for writing code
   * that is supposed to be able to handle multiple opcode types that
   * share the same kind of extra data.
   */
  template<class T> const T* getExtra() const {
    auto opcode = getOpcode();
    if (debug) assert_opcode_extra<T>(opcode);
    return static_cast<const T*>(m_extra);
  }

  /*
   * Returns whether or not this opcode has an associated extra data
   * struct.
   */
  bool hasExtra() const;

  /*
   * Set the extra-data for this IRInstruction to the given pointer.
   * Lifetime is must outlast this IRInstruction (and any of its
   * clones).
   */
  void setExtra(IRExtraData* data) { assert(!m_extra); m_extra = data; }

  /*
   * Replace an instruction in place with a Nop.  This sometimes may
   * be a result of a simplification pass.
   */
  void convertToNop();

  /*
   * Deep-copy an IRInstruction, using factory to allocate memory for
   * the IRInstruction itself, and its srcs/dests.
   */
  virtual IRInstruction* clone(IRFactory* factory) const;

  Opcode     getOpcode()   const       { return m_op; }
  void       setOpcode(Opcode newOpc)  { m_op = newOpc; }
  Type       getTypeParam() const      { return m_typeParam; }
  void       setTypeParam(Type t)      { m_typeParam = t; }
  uint32_t   getNumSrcs()  const       { return m_numSrcs; }
  void       setNumSrcs(uint32_t i)    {
    assert(i <= m_numSrcs);
    m_numSrcs = i;
  }
  SSATmp*    getSrc(uint32 i) const;
  void       setSrc(uint32 i, SSATmp* newSrc);
  void       appendSrc(Arena&, SSATmp*);
  SSARange   getSrcs() const {
    return SSARange(m_srcs, m_numSrcs);
  }
  unsigned   getNumDsts() const     { return m_numDsts; }
  SSATmp*    getDst() const         {
    assert(!naryDst());
    return m_dst;
  }
  void       setDst(SSATmp* newDst) {
    assert(hasDst());
    m_dst = newDst;
    m_numDsts = newDst ? 1 : 0;
  }
  SSATmp*    getDst(unsigned i) const {
    assert(naryDst() && i < m_numDsts);
    return m_dsts[i];
  }
  SSARange   getDsts();
  Range<SSATmp* const*> getDsts() const;
  void       setDsts(unsigned numDsts, SSATmp** newDsts) {
    assert(naryDst());
    m_numDsts = numDsts;
    m_dsts = newDsts;
  }

  TCA        getTCA()      const       { return m_tca; }
  void       setTCA(TCA    newTCA)     { m_tca = newTCA; }

  /*
   * An instruction's 'id' has different meanings depending on the
   * compilation phase.
   */
  uint32     getId()       const       { return m_id; }
  void       setId(uint32 newId)       { m_id = newId; }

  /*
   * Instruction id (iid) is stable and useful as an array index.
   */
  uint32     getIId()      const       {
    assert(m_iid != kTransient);
    return m_iid;
  }

  /*
   * Returns true if the instruction is in a transient state.  That
   * is, it's allocated on the stack and we haven't yet committed to
   * inserting it in any blocks.
   */
  bool       isTransient() const       { return m_iid == kTransient; }

  void       setAsmAddr(void* addr)    { m_asmAddr = addr; }
  void*      getAsmAddr() const        { return m_asmAddr; }
  RegSet     getLiveOutRegs() const    { return m_liveOutRegs; }
  void       setLiveOutRegs(RegSet s)  { m_liveOutRegs = s; }
  Trace*     getParent() const { return m_parent; }
  void       setParent(Trace* parentTrace) { m_parent = parentTrace; }

  void setLabel(LabelInstruction* l) { m_label = l; }
  LabelInstruction* getLabel() const { return m_label; }
  bool isControlFlowInstruction() const { return m_label != nullptr; }

  virtual bool equals(IRInstruction* inst) const;
  virtual size_t hash() const;
  virtual void print(std::ostream& ostream) const;
  void print() const;
  std::string toString() const;

  /*
   * Helper accessors for the OpcodeFlag bits for this instruction.
   *
   * Note that these wrappers have additional logic beyond just
   * checking the corresponding flags bit---you should generally use
   * these when you have an actual IRInstruction instead of just an
   * Opcode enum value.
   */
  bool canCSE() const;
  bool hasDst() const;
  bool naryDst() const;
  bool hasMemEffects() const;
  bool isRematerializable() const;
  bool isNative() const;
  bool consumesReferences() const;
  bool consumesReference(int srcNo) const;
  bool producesReference() const;
  bool mayModifyRefs() const;
  bool mayRaiseError() const;
  bool isEssential() const;
  bool isTerminal() const;

  void printDst(std::ostream& ostream) const;
  void printSrc(std::ostream& ostream, uint32 srcIndex) const;
  void printOpcode(std::ostream& ostream) const;
  void printSrcs(std::ostream& ostream) const;

private:
  bool mayReenterHelper() const;

private:
  Opcode            m_op;
  Type              m_typeParam;
  uint16            m_numSrcs;
  uint16            m_numDsts;
  const IId         m_iid;
  uint32            m_id;
  SSATmp**          m_srcs;
  RegSet            m_liveOutRegs;
  union {
    SSATmp*         m_dst;  // if HasDest
    SSATmp**        m_dsts; // if NaryDest
  };
  void*             m_asmAddr;
  LabelInstruction* m_label;
  Trace*            m_parent;
  TCA               m_tca;
  IRExtraData*      m_extra;
};

class LabelInstruction : public IRInstruction {
public:
  explicit LabelInstruction(uint32 id, const Func* func)
    : IRInstruction(DefLabel)
    , m_labelId(id)
    , m_patchAddr(0)
    , m_func(func)
  {}

  explicit LabelInstruction(Arena& arena, const LabelInstruction* inst, IId iid)
    : IRInstruction(arena, inst, iid)
    , m_labelId(inst->m_labelId)
    , m_patchAddr(0)
    , m_func(inst->m_func)
  {}

  uint32      getLabelId() const  { return m_labelId; }
  const Func* getFunc() const     { return m_func; }

  virtual void print(std::ostream& ostream) const;
  virtual bool equals(IRInstruction* inst) const;
  virtual size_t hash() const;
  virtual IRInstruction* clone(IRFactory* factory) const;

  void    printLabel(std::ostream& ostream) const;
  void    prependPatchAddr(TCA addr);
  void*   getPatchAddr();

private:
  const uint32 m_labelId;
  void*  m_patchAddr; // Support patching forward jumps
  const Func* m_func; // which func are we in
};

/*
 * Return the output type from a given IRInstruction.
 *
 * The destination type is always predictable from the types of the
 * inputs and any type parameters to the instruction.
 */
Type outputType(const IRInstruction*);

/*
 * Assert that an instruction has operands of allowed types.
 */
void assertOperandTypes(const IRInstruction*);

struct SpillInfo {
  enum Type { MMX, Memory };

  explicit SpillInfo(RegNumber r) : m_type(MMX), m_val(int(r)) {}
  explicit SpillInfo(uint32_t v)  : m_type(Memory), m_val(v) {}

  Type      type() const { return m_type; }
  RegNumber mmx()  const { return RegNumber(m_val); }
  uint32_t  mem()  const { return m_val; }

private:
  Type     m_type : 1;
  uint32_t m_val : 31;
};

inline std::ostream& operator<<(std::ostream& os, SpillInfo si) {
  switch (si.type()) {
  case SpillInfo::MMX:
    os << "mmx" << reg::regname(RegXMM(int(si.mmx())));
    break;
  case SpillInfo::Memory:
    os << "spill[" << si.mem() << "]";
    break;
  }
  return os;
}

class SSATmp {
public:
  uint32            getId() const { return m_id; }
  IRInstruction*    getInstruction() const { return m_inst; }
  void              setInstruction(IRInstruction* i) { m_inst = i; }
  Type              getType() const { return m_type; }
  void              setType(Type t) { m_type = t; }
  uint32            getLastUseId() { return m_lastUseId; }
  void              setLastUseId(uint32 newId) { m_lastUseId = newId; }
  uint32            getUseCount() { return m_useCount; }
  void              setUseCount(uint32 count) { m_useCount = count; }
  void              incUseCount() { m_useCount++; }
  uint32            decUseCount() { return --m_useCount; }
  bool              isBoxed() const { return getType().isBoxed(); }
  bool              isString() const { return isA(Type::Str); }
  bool              isArray() const { return isA(Type::Arr); }
  void              print(std::ostream& ostream,
                          bool printLastUse = false) const;
  void              print() const;

  // Used for Jcc to Jmp elimination
  void              setTCA(TCA tca);
  TCA               getTCA() const;

  // XXX: false for Null, etc.  Would rather it returns whether we
  // have a compile-time constant value.
  bool isConst() const {
    return m_inst->getOpcode() == DefConst ||
      m_inst->getOpcode() == LdConst;
  }

  /*
   * For SSATmps with a compile-time constant value, the following
   * functions allow accessing it.
   *
   * Pre: getInstruction() &&
   *   (getInstruction()->getOpcode() == DefConst ||
   *    getInstruction()->getOpcode() == LdConst)
   */
  bool               getValBool() const;
  int64              getValInt() const;
  int64              getValRawInt() const;
  double             getValDbl() const;
  const StringData*  getValStr() const;
  const ArrayData*   getValArr() const;
  const Func*        getValFunc() const;
  const Class*       getValClass() const;
  const NamedEntity* getValNamedEntity() const;
  uintptr_t          getValBits() const;
  TCA                getValTCA() const;

  /*
   * Returns: Type::subtypeOf(getType(), tag).
   *
   * This should be used for most checks on the types of IRInstruction
   * sources.
   */
  bool isA(Type tag) const {
    return getType().subtypeOf(tag);
  }

  /*
   * Returns whether or not a given register index is allocated to a
   * register, or returns false if it is spilled.
   *
   * Right now, we only spill both at the same time and only Spill and
   * Reload instructions need to deal with SSATmps that are spilled.
   */
  bool hasReg(uint32 i = 0) const {
    return !m_isSpilled && m_regs[i] != InvalidReg;
  }

  /*
   * The maximum number of registers this SSATmp may need allocated.
   * This is based on the type of the temporary (some types never have
   * regs, some have two, etc).
   */
  int               numNeededRegs() const;

  /*
   * The number of regs actually allocated to this SSATmp.  This might
   * end up fewer than numNeededRegs if the SSATmp isn't really
   * being used.
   */
  int               numAllocatedRegs() const;

  /*
   * Access to allocated registers.
   *
   * Returns InvalidReg for slots that aren't allocated.
   */
  PhysReg     getReg() const { assert(!m_isSpilled); return m_regs[0]; }
  PhysReg     getReg(uint32 i) const { assert(!m_isSpilled); return m_regs[i]; }
  void        setReg(PhysReg reg, uint32 i) { m_regs[i] = reg; }

  /*
   * Returns information about how to spill/fill a SSATmp.
   *
   * These functions are only valid if this SSATmp is being spilled or
   * filled.  In all normal instructions (i.e. other than Spill and
   * Reload), SSATmps are assigned registers instead of spill
   * locations.
   */
  void        setSpillInfo(int idx, SpillInfo si) { m_spillInfo[idx] = si;
                                                    m_isSpilled = true; }
  SpillInfo   getSpillInfo(int idx) const { assert(m_isSpilled);
                                            return m_spillInfo[idx]; }

  /*
   * During register allocation, this is used to track spill locations
   * that are assigned to specific SSATmps.  A value of -1 is used to
   * indicate no spill slot has been assigned.
   *
   * After register allocation, use getSpillInfo to access information
   * about where we've decided to spill/fill a given SSATmp from.
   * This value doesn't have any meaning outside of the linearscan
   * pass.
   */
  int32_t           getSpillSlot() const { return m_spillSlot; }
  void              setSpillSlot(int32_t val) { m_spillSlot = val; }

private:
  friend class IRFactory;
  friend class TraceBuilder;

  // May only be created via IRFactory.  Note that this class is never
  // destructed, so don't add complex members.
  SSATmp(uint32 opndId, IRInstruction* i)
    : m_inst(i)
    , m_id(opndId)
    , m_type(outputType(i))
    , m_lastUseId(0)
    , m_useCount(0)
    , m_isSpilled(false)
    , m_spillSlot(-1)
  {
    m_regs[0] = m_regs[1] = InvalidReg;
  }
  SSATmp(const SSATmp&);
  SSATmp& operator=(const SSATmp&);

  IRInstruction*  m_inst;
  const uint32    m_id;
  Type            m_type; // type when defined
  uint32          m_lastUseId;
  uint16          m_useCount;
  bool            m_isSpilled : 1;
  int32_t         m_spillSlot : 31;

  /*
   * m_regs[0] is always the value of this SSATmp.
   *
   * Cell or Gen types use two registers: m_regs[1] is the runtime
   * type.
   */
  static const int kMaxNumRegs = 2;
  union {
    PhysReg m_regs[kMaxNumRegs];
    SpillInfo m_spillInfo[kMaxNumRegs];
  };
};

int vectorBaseIdx(const IRInstruction* inst);
int vectorKeyIdx(const IRInstruction* inst);
int vectorValIdx(const IRInstruction* inst);

struct VectorEffects {
  VectorEffects(const IRInstruction* inst) {
    init(inst->getOpcode(),
         inst->getSrc(vectorBaseIdx(inst))->getType(),
         inst->getSrc(vectorKeyIdx(inst))->getType(),
         inst->getSrc(vectorValIdx(inst))->getType());
  }
  VectorEffects(Opcode op, Type base, Type key, Type val) {
    init(op, base, key, val);
  }
  Type baseType;
  Type valType;
  bool baseTypeChanged;
  bool baseValChanged;
  bool valTypeChanged;

private:
  void init(Opcode op, const Type base, const Type key, const Type val);
};

class Trace : boost::noncopyable {
public:
  explicit Trace(LabelInstruction* label, uint32 bcOff, bool isMain) {
    appendInstruction(label);
    m_bcOff = bcOff;
    m_lastAsmAddress = nullptr;
    m_firstAsmAddress = nullptr;
    m_firstAstubsAddress = nullptr;
    m_isMain = isMain;
  }

  ~Trace() {
    std::for_each(m_exitTraces.begin(), m_exitTraces.end(),
                  boost::checked_deleter<Trace>());
  }

  IRInstruction::List& getInstructionList() {
    return m_instructionList;
  }
  LabelInstruction* getLabel() const {
    IRInstruction* first = *m_instructionList.begin();
    assert(first->getOpcode() == DefLabel);
    return (LabelInstruction*) first;
  }
  IRInstruction* prependInstruction(IRInstruction* inst) {
    // jump over the trace's label
    std::list<IRInstruction*>::iterator it = m_instructionList.begin();
    ++it;
    m_instructionList.insert(it, inst);
    inst->setParent(this);
    return inst;
  }
  IRInstruction* appendInstruction(IRInstruction* inst) {
    m_instructionList.push_back(inst);
    inst->setParent(this);
    return inst;
  }

  uint32 getBcOff() { return m_bcOff; }
  void setLastAsmAddress(uint8* addr) { m_lastAsmAddress = addr; }
  void setFirstAsmAddress(uint8* addr) { m_firstAsmAddress = addr; }
  void setFirstAstubsAddress(uint8* addr) { m_firstAstubsAddress = addr; }
  uint8* getLastAsmAddress() { return m_lastAsmAddress; }
  uint8* getFirstAsmAddress() { return m_firstAsmAddress; }
  Trace* addExitTrace(Trace* exit) {
    m_exitTraces.push_back(exit);
    return exit;
  }
  bool isMain() const { return m_isMain; }

  typedef std::list<Trace*> List;
  typedef std::list<Trace*>::iterator Iterator;

  List& getExitTraces() { return m_exitTraces; }
  void print(std::ostream& ostream, bool printAsm,
             bool isExit = false) const;
  void print() const;  // default to std::cout and printAsm == true

private:
  // offset of the first bytecode in this trace; 0 if this trace doesn't
  // represent a bytecode boundary.
  uint32 m_bcOff;
  // instructions in main trace starting with a label
  std::list<IRInstruction*> m_instructionList;
  // traces to which this trace exits
  List m_exitTraces;
  uint8* m_firstAsmAddress;
  uint8* m_firstAstubsAddress;
  uint8* m_lastAsmAddress;
  bool m_isMain;
};

/**
 * A Block (basic block) refers to a single-entry, single-exit, non-empty
 * range of instructions within a trace.  The instruction range is denoted by
 * iterators within the owning trace's instruction list, to facilitate
 * mutating that list.
 */
class Block {
  typedef IRInstruction::Iterator Iter;

public:
  Block(unsigned id, Iter start, Iter end) : m_post_id(id), m_isJoin(0),
    m_range(start, end) {
    m_succ[0] = m_succ[1] = nullptr;
  }

  Block(const Block& other) : m_post_id(other.m_post_id), m_isJoin(0),
    m_range(other.m_range) {
    m_succ[0] = m_succ[1] = nullptr;
  }

  // postorder number of this block in [0..NumBlocks); higher means earlier
  unsigned postId() const { return m_post_id; }
  bool isJoin() const { return m_isJoin != 0; }

  // range-for interface
  Iter begin() const { return m_range.begin(); }
  Iter end() const { return m_range.end(); }

  IRInstruction* lastInst() const {
    Iter e = end();
    return *(--e);
  }

  Range<Block**> succ() {
    IRInstruction* last = lastInst();
    if (last->isControlFlowInstruction() && !last->isTerminal()) {
      return Range<Block**>(m_succ, m_succ + 2); // 2-way branch
    }
    if (last->isControlFlowInstruction()) {
      return Range<Block**>(m_succ+1, m_succ + 2); // jmp
    }
    if (!last->isTerminal()) {
      return Range<Block**>(m_succ, m_succ + 1); // ordinary instruction
    }
    return Range<Block**>(m_succ, m_succ); // exit, return, throw, etc
  }

private:
  friend std::list<Block> buildCfg(Trace*);
  unsigned const m_post_id;
  unsigned m_isJoin:1; // true if this block has 2+ predecessors
  Range<IRInstruction::Iterator> m_range;
  Block* m_succ[2];
};

typedef std::list<Block> BlockList;

/*
 * Some utility micro-passes used from other major passes.
 */

/*
 * Remove any instruction if live[iid] == false
 */
void removeDeadInstructions(Trace* trace, const boost::dynamic_bitset<>& live);

/*
 * Identify basic blocks in Trace and Trace's children, then produce a
 * reverse-postorder list of blocks, with connected successors.
 */
BlockList buildCfg(Trace*);

/*
 * Ensure valid SSA properties; each SSATmp must be defined exactly once,
 * only used in positions dominated by the definition.
 */
bool checkCfg(Trace*, const IRFactory&);

/**
 * Run all optimization passes on this trace
 */
void optimizeTrace(Trace*, IRFactory* irFactory);

/**
 * Assign ids to each instruction in reverse postorder (lowest id first),
 * Removes any exit traces that are not reachable, and returns the reached
 * instructions in numbered order.
 */
BlockList numberInstructions(Trace*);

/*
 * Counts the number of cells a SpillStack will logically push.  (Not
 * including the number it pops.)  That is, for each SSATmp in the
 * spill sources, this totals up whether it is an ActRec or a cell.
 */
int32_t spillValueCells(IRInstruction* spillStack);

/*
 * When SpillStack takes an ActRec, it has this many extra
 * dependencies in the spill vector for the values in the ActRec.
 */
constexpr int kSpillStackActRecExtraArgs = 4;

inline bool isConvIntOrPtrToBool(IRInstruction* instr) {
  if (!(instr->getOpcode() == Conv &&
        instr->getTypeParam() == Type::Bool)) {
    return false;
  }

  return instr->getSrc(0)->isA(
    Type::Int | Type::Func | Type::Cls | Type::FuncCls |
    Type::VarEnv | Type::TCA);
}

/*
 * Visit basic blocks in a preorder traversal over the dominator tree.
 * The state argument is passed by value (copied) as we move down the tree,
 * so each child in the tree gets the state after the parent was processed.
 * The body lambda should take State& (by reference) so it can modify it
 * as each block is processed.
 */
template <class State, class Body>
void forPreorderDoms(Block* block, std::forward_list<Block*> children[],
                     State state, Body body) {
  body(block, state);
  for (Block* child : children[block->postId()]) {
    forPreorderDoms(child, children, state, body);
  }
}

/*
 * Visit the main trace followed by exit traces.
 */
template <class Body>
void forEachTrace(Trace* main, Body body) {
  body(main);
  for (Trace* exit : main->getExitTraces()) {
    body(exit);
  }
}

/*
 * Visit each instruction in the main trace, then the exit traces
 */
template <class Body>
void forEachTraceInst(Trace* main, Body body) {
  forEachTrace(main, [=](Trace* t) {
    for (IRInstruction* inst : t->getInstructionList()) {
      body(inst);
    }
  });
}

}}}

namespace std {
  template<> struct hash<HPHP::VM::JIT::Opcode> {
    size_t operator()(HPHP::VM::JIT::Opcode op) const { return op; }
  };
  template<> struct hash<HPHP::VM::JIT::Type> {
    size_t operator()(HPHP::VM::JIT::Type t) const { return t.hash(); }
  };
}

#endif
