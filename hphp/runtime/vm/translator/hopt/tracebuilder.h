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

#ifndef incl_HHVM_HHIR_TRACEBUILDER_H_
#define incl_HHVM_HHIR_TRACEBUILDER_H_

#include <boost/scoped_ptr.hpp>

#include "runtime/vm/translator/hopt/ir.h"
#include "runtime/vm/translator/hopt/irfactory.h"
#include "runtime/vm/translator/hopt/cse.h"
#include "runtime/vm/translator/hopt/simplifier.h"

#include "folly/ScopeGuard.h"

namespace HPHP {
namespace VM {
namespace JIT {

class TraceBuilder {
public:
  TraceBuilder(Offset initialBcOffset,
               uint32_t initialSpOffsetFromFp,
               IRFactory&,
               CSEHash& constants,
               const Func* func = nullptr);

  void setEnableCse(bool val)            { m_enableCse = val; }
  void setEnableSimplification(bool val) { m_enableSimplification = val; }

  Trace* makeExitTrace(uint32 bcOff) {
    return m_trace->addExitTrace(makeTrace(m_curFunc->getValFunc(),
                                           bcOff, false));
  }
  bool isThisAvailable() const {
    return m_thisIsAvailable;
  }
  void setThisAvailable() {
    m_thisIsAvailable = true;
  }

  void optimizeTrace();

  SSATmp* getFP() {
    return m_fpValue;
  }

  /*
   * Create an IRInstruction attached to this Trace, and allocate a
   * destination SSATmp for it.  Uses the same argument list format as
   * IRFactory::gen.
   */
  template<class... Args>
  SSATmp* gen(Args... args) {
    return makeInstruction(
      [this] (IRInstruction* inst) { return optimizeInst(inst); },
      args...
    );
  }

  SSATmp* genDefCns(const StringData* cnsName, SSATmp* val);
  SSATmp* genConcat(SSATmp* tl, SSATmp* tr);
  void    genDefCls(PreClass*, const HPHP::VM::Opcode* after);
  void    genDefFunc(Func*);

  SSATmp* genLdThis(Trace* trace);
  SSATmp* genLdCtx();
  SSATmp* genLdCtxCls();
  SSATmp* genLdRetAddr();
  SSATmp* genLdRaw(SSATmp* base, RawMemSlot::Kind kind, Type type);
  void    genStRaw(SSATmp* base, RawMemSlot::Kind kind, SSATmp* value,
                   int64 extraOff);

  SSATmp* genLdLoc(uint32 id);
  SSATmp* genLdLocAddr(uint32 id);
  void genRaiseUninitWarning(uint32 id);

  /*
   * Returns an SSATmp containing the (inner) value of the given local.
   * If the local is a boxed value, this returns its inner value.
   *
   * Note: For boxed values, this will generate a LdRef instruction which
   *       takes the given exit trace in case the inner type doesn't match
   *       the tracked type for this local.  This check may be optimized away
   *       if we can determine that the inner type must match the tracked type.
   */
  SSATmp* genLdLocAsCell(uint32 id, Trace* exitTrace);

  /*
   * Asserts that local 'id' has type 'type' and loads it into the
   * returned SSATmp.
   */
  SSATmp* genLdAssertedLoc(uint32 id, Type type);

  SSATmp* genStLoc(uint32 id,
                   SSATmp* src,
                   bool doRefCount,
                   bool genStoreType,
                   Trace* exit);
  SSATmp* genLdMem(SSATmp* addr, Type type, Trace* target);
  SSATmp* genLdMem(SSATmp* addr, int64_t offset,
                   Type type, Trace* target);
  void    genStMem(SSATmp* addr, SSATmp* src, bool genStoreType);
  void    genStMem(SSATmp* addr, int64 offset, SSATmp* src, bool stType);
  SSATmp* genLdProp(SSATmp* obj, SSATmp* prop, Type type, Trace* exit);
  void    genStProp(SSATmp* obj, SSATmp* prop, SSATmp* src, bool genStoreType);
  void    genSetPropCell(SSATmp* base, int64 offset, SSATmp* value);

  SSATmp* genBoxLoc(uint32 id);
  void    genBindLoc(uint32 id, SSATmp* ref, bool doRefCount = true);
  void    genInitLoc(uint32 id, SSATmp* t);

  void    genCheckClsCnsDefined(SSATmp* cns, Trace* exitTrace);
  SSATmp* genLdCurFuncPtr();
  SSATmp* genLdARFuncPtr(SSATmp* baseAddr, SSATmp* offset);
  SSATmp* genLdFuncCls(SSATmp* func);
  SSATmp* genNewObj(int32 numParams, const StringData* clsName);
  SSATmp* genNewObj(int32 numParams, SSATmp* cls);
  SSATmp* genNewArray(int32 capacity);
  SSATmp* genNewTuple(int32 numArgs, SSATmp* sp);
  SSATmp* genDefActRec(SSATmp* func, SSATmp* objOrClass, int32_t numArgs,
                       const StringData* invName);
  SSATmp* genFreeActRec();
  void    genGuardLoc(uint32 id, Type type, Trace* exitTrace);
  void    genGuardStk(uint32 id, Type type, Trace* exitTrace);
  void    genAssertStk(uint32_t id, Type type);
  SSATmp* genGuardType(SSATmp* src, Type type, Trace* nextTrace);
  void    genGuardRefs(SSATmp* funcPtr,
                       SSATmp* nParams,
                       SSATmp* bitsPtr,
                       SSATmp* firstBitNum,
                       SSATmp* mask64,
                       SSATmp* vals64,
                       Trace*  exitTrace);
  void    genAssertLoc(uint32 id, Type type);

  SSATmp* genUnbox(SSATmp* src, Trace* exit);
  SSATmp* genUnboxPtr(SSATmp* ptr);
  SSATmp* genLdRef(SSATmp* ref, Type type, Trace* exit);
  SSATmp* genAdd(SSATmp* src1, SSATmp* src2);
  SSATmp* genLdAddr(SSATmp* base, int64 offset);

  SSATmp* genSub(SSATmp* src1, SSATmp* src2);
  SSATmp* genMul(SSATmp* src1, SSATmp* src2);
  SSATmp* genAnd(SSATmp* src1, SSATmp* src2);
  SSATmp* genOr(SSATmp* src1, SSATmp* src2);
  SSATmp* genXor(SSATmp* src1, SSATmp* src2);
  SSATmp* genNot(SSATmp* src);

  SSATmp* genDefUninit();
  SSATmp* genDefInitNull();
  SSATmp* genDefNull();
  SSATmp* genJmp(Trace* target);
  SSATmp* genJmpCond(SSATmp* src, Trace* target, bool negate);
  void    genExitWhenSurprised(Trace* target);
  void    genExitOnVarEnv(Trace* target);
  void    genCheckInit(SSATmp* src, LabelInstruction* target);
  void    genCheckInit(SSATmp* src, Trace* target) {
    genCheckInit(src, getLabel(target));
  }
  SSATmp* genCmp(Opcode opc, SSATmp* src1, SSATmp* src2);
  SSATmp* genConvToBool(SSATmp* src);
  SSATmp* genConvToInt(SSATmp* src);
  SSATmp* genConvToDbl(SSATmp* src);
  SSATmp* genConvToStr(SSATmp* src);
  SSATmp* genConvToArr(SSATmp* src);
  SSATmp* genConvToObj(SSATmp* src);
  SSATmp* genLdPropAddr(SSATmp* obj, SSATmp* prop);
  SSATmp* genLdClsMethod(SSATmp* cls, uint32 methodSlot);
  SSATmp* genLdClsMethodCache(SSATmp* className,
                              SSATmp* methodName,
                              SSATmp* baseClass,
                              Trace* slowPathExit);
  SSATmp* genLdObjMethod(const StringData* methodName, SSATmp* obj);
  SSATmp* genCall(SSATmp* actRec,
                  uint32 returnBcOffset,
                  SSATmp* func,
                  uint32 numParams,
                  SSATmp** params);
  void    genReleaseVVOrExit(Trace* exit);
  SSATmp* genGenericRetDecRefs(SSATmp* retVal, int numLocals);
  void    genRetVal(SSATmp* val);
  SSATmp* genRetAdjustStack();
  void    genRetCtrl(SSATmp* sp, SSATmp* fp, SSATmp* retAddr);
  void    genDecRef(SSATmp* tmp);
  void    genDecRefMem(SSATmp* base, int64 offset, Type type);
  void    genDecRefStack(Type type, uint32 stackOff);
  void    genDecRefLoc(int id);
  void    genDecRefThis();
  void    genIncStat(int32 counter, int32 value, bool force = false);
  SSATmp* genIncRef(SSATmp* src);
  SSATmp* genSpillStack(uint32 stackAdjustment,
                        uint32 numOpnds,
                        SSATmp** opnds);
  SSATmp* genLdStack(int32 stackOff, Type type);
  SSATmp* genDefFP();
  SSATmp* genDefSP(int32 spOffset);
  SSATmp* genLdStackAddr(int64 offset);
  void    genVerifyParamType(SSATmp* objClass, SSATmp* className,
                             const Class* constraint, Trace* exitTrace);

  void    genNativeImpl();

  void    genUnlinkContVarEnv();
  void    genLinkContVarEnv();
  void    genContEnter(SSATmp* contAR, SSATmp* addr, int64 returnBcOffset);
  Trace*  genContRaiseCheck(SSATmp* cont, Trace* target);
  Trace*  genContPreNext(SSATmp* cont, Trace* target);
  Trace*  genContStartedCheck(SSATmp* cont, Trace* target);

  SSATmp* genIterInit(SSATmp* src, uint32 iterId, uint32 valLocalId);
  SSATmp* genIterInitK(SSATmp* src,
                       uint32 iterId,
                       uint32 valLocalId,
                       uint32 keyLocalId);
  SSATmp* genIterNext(uint32 iterId, uint32 valLocalId);
  SSATmp* genIterNextK(uint32 iterId, uint32 valLocalId, uint32 keyLocalId);

  SSATmp* genInterpOne(uint32 pcOff, uint32 stackAdjustment,
                       Type resultType, Trace* target);
  Trace* getExitSlowTrace(uint32 bcOff,
                          int32 stackDeficit,
                          uint32 numOpnds,
                          SSATmp** opnds);

  /*
   * Generates a trace exit that can be the target of a conditional
   * or unconditional control flow instruction from the main trace.
   *
   * Lifetime of the returned pointer is managed by the trace this
   * TraceBuilder is generating.
   */
  Trace* genExitTrace(uint32 bcOff,
                      int32  stackDeficit,
                      uint32 numOpnds,
                      SSATmp** opnds,
                      TraceExitType::ExitType,
                      uint32 notTakenBcOff = 0);

  /*
   * Generates a target exit trace for GuardFailure exits.
   *
   * Lifetime of the returned pointer is managed by the trace this
   * TraceBuilder is generating.
   */
  Trace* genExitGuardFailure(uint32 off);

  // generates the ExitTrace instruction at the end of a trace
  void genTraceEnd(uint32 nextPc,
                   TraceExitType::ExitType exitType = TraceExitType::Normal);

  template<typename T>
  SSATmp* genDefConst(T val) {
    ConstData cdata(val);
    return gen(DefConst, typeForConst(val), &cdata);
  }

  SSATmp* genDefVoid() {
    ConstData cdata(0);
    return gen(DefConst, Type::None, &cdata);
  }

  template<typename T>
  SSATmp* genLdConst(T val) {
    ConstData cdata(val);
    return gen(LdConst, typeForConst(val), &cdata);
  }

  Trace* getTrace() const { return m_trace.get(); }
  IRFactory* getIrFactory() { return &m_irFactory; }
  int32 getSpOffset() { return m_spOffset; }
  SSATmp* getSp() const { return m_spValue; }
  SSATmp* getFp() const { return m_fpValue; }

  Type getLocalType(int id);

  LabelInstruction* getLabel(Trace* trace) {
    return trace ? trace->getLabel() : NULL;
  }

  /*
   * ifelse() generates if-then-else blocks within a trace.  The caller
   * supplies lambdas to create the branch, next-body, and taken-body.
   * The next and taken lambdas must return one SSATmp* value; ifelse() returns
   * the SSATmp for the merged value.
   */
  template <class Branch, class Next, class Taken>
  SSATmp* ifelse(const Func* func, Branch branch, Next next, Taken taken) {
    LabelInstruction* taken_label = m_irFactory.defLabel(func);
    LabelInstruction* done_label = m_irFactory.defLabel(func, 1);
    bool oldEnableCse = m_enableCse;
    m_enableCse = false;
    SCOPE_EXIT { m_enableCse = oldEnableCse; };
    branch(taken_label);
    SSATmp* v1 = next();
    gen(Jmp_, done_label, v1);
    appendInstruction(taken_label);
    SSATmp* v2 = taken();
    gen(Jmp_, done_label, v2);
    appendInstruction(done_label);
    SSATmp* result = done_label->getDst(0);
    result->setType(Type::unionOf(v1->getType(), v2->getType()));
    return result;
  }

private:
  void      appendInstruction(IRInstruction* inst);
  enum      CloneInstMode { kCloneInst, kUseInst };
  SSATmp*   optimizeInst(IRInstruction* inst, CloneInstMode clone=kCloneInst);
  SSATmp*   cseLookup(IRInstruction* inst);
  void      cseInsert(IRInstruction* inst);
  CSEHash*  getCSEHashTable(IRInstruction* inst);
  void      killCse();
  void      killLocals();
  void      killLocalValue(int id);
  void      setLocalValue(int id, SSATmp* value);
  void      setLocalType(int id, Type type);
  SSATmp*   getLocalValue(int id);
  bool      isValueAvailable(SSATmp* tmp) const;
  bool      anyLocalHasValue(SSATmp* tmp) const;
  void      updateLocalRefValues(SSATmp* oldRef, SSATmp* newRef);
  void      updateTrackedState(IRInstruction* inst);
  void      clearTrackedState();

  Trace* makeTrace(const Func* func, uint32 bcOff, bool isMain) {
    return new Trace(m_irFactory.defLabel(func), bcOff, isMain);
  }
  void genStLocAux(uint32 id, SSATmp* t0, bool genStoreType);

  /*
   * Fields
   */
  IRFactory& m_irFactory;
  Simplifier m_simplifier;

  Offset const m_initialBcOff; // offset of initial bytecode in trace
  boost::scoped_ptr<Trace> const m_trace; // generated trace

  // Flags that enable optimizations
  bool       m_enableCse;
  bool       m_enableSimplification;

  /*
   * While building a trace one instruction at a time, a TraceBuilder
   * tracks various state for generating code and for optimization:
   *
   *   (1) m_fpValue & m_spValue track which SSATmp holds the current VM
   *       frame and stack pointer values.
   *
   *   (2) m_spOffset tracks the offset of the m_spValue from m_fpValue.
   *
   *   (3) m_curFunc tracks the current function containing the
   *       generated code; currently, this remains constant during
   *       tracebuilding but once we implement inlining it'll have to
   *       be updated to track the context of inlined functions.
   *
   *   (4) m_cseHash & m_constTable are hashtables for common
   *       sub-expression elimination. m_constTable holds only constants.
   *
   *   (5) m_thisIsAvailable tracks whether the current ActRec has a
   *       non-null this pointer.
   *
   *   (6) m_localValues & m_localTypes track the current values and
   *       types held in locals. These vectors are indexed by the
   *       local's id.
   *
   * The function updateTrackedState(IRInstruction* inst) updates this
   * state (called after an instruction is appended to the trace), and
   * the function clearTrackedState() clears it.
   */
  SSATmp*    m_spValue;      // current physical sp
  SSATmp*    m_fpValue;      // current physical fp
  int32      m_spOffset;     // offset of physical sp from physical fp
  SSATmp*    m_curFunc;      // current function context
  CSEHash&   m_constTable;
  CSEHash    m_cseHash;
  bool       m_thisIsAvailable; // true only if current ActRec has non-null this

  // vectors that track local values & types
  std::vector<SSATmp*>   m_localValues;
  std::vector<Type> m_localTypes;
};

template<>
inline SSATmp* TraceBuilder::genDefConst(Type t) {
  ConstData cdata(0);
  return gen(DefConst, t, &cdata);
}

}}}

#endif
