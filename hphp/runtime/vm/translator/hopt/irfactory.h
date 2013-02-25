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

#ifndef incl_HPHP_VM_IRFACTORY_H_
#define incl_HPHP_VM_IRFACTORY_H_

#include <type_traits>

#include "util/arena.h"
#include "runtime/vm/translator/hopt/ir.h"

#include <vector>

namespace HPHP { namespace VM { namespace JIT {

//////////////////////////////////////////////////////////////////////

/*
 * ReturnValue makeInstruction(Lambda, Args...) --
 *
 *   Create an IRInstruction on the stack using Args, and call Lambda
 *   with a pointer to it, returning the result.
 *
 *   Normally IRInstruction creation should go through either
 *   IRFactory::gen or TraceBuilder::gen.  This utility is used to
 *   implement those.
 */

namespace detail {

template<class Ret, class Func>
struct InstructionBuilder {
  explicit InstructionBuilder(const Func& func) : func(func) {}

  // Create an IRInstruction, and then recursively chew on the Args
  // list to populate its fields.
  template<class... Args>
  Ret go(Opcode op, Args... args) {
    IRInstruction inst(op);
    return go2(&inst, args...);
  }

private:
  // Main loop: call setter() on the head of the list and repeat,
  // until there's no overload for the Head type; then it will fall
  // through to the base case.
  template<class Head, class... Tail>
  Ret go2(IRInstruction* inst, Head x, Tail... xs) {
    setter(inst, x);
    return go2(inst, xs...);
  }

  // Base cases: either there are no SSATmps, or there's a variadic
  // list of SSATmp*'s, or there's an int followed by a SSATmp**.

  Ret go2(IRInstruction* inst) { return stop(inst); }

  template<class... SSAs>
  Ret go2(IRInstruction* inst, SSATmp* t1, SSAs... ts) {
    SSATmp* ssas[] = { t1, ts... };
    inst->initializeSrcs(1 + sizeof...(ts), ssas);
    return stop(inst);
  }

  template<class Int>
  Ret go2(IRInstruction* inst, Int num, SSATmp** ssas) {
    inst->initializeSrcs(num, ssas);
    return stop(inst);
  }

  // For each of the optional parameters, there's an overloaded
  // setter:

  void setter(IRInstruction* inst, LabelInstruction* l) {
    inst->setLabel(l);
  }

  void setter(IRInstruction* inst, Type t) {
    inst->setTypeParam(t);
  }

  void setter(IRInstruction* inst, IRExtraData* extra) {
    inst->setExtra(extra);
  }

  // Finally we end up here.
  Ret stop(IRInstruction* inst) {
    if (debug) assertOperandTypes(inst);
    return func(inst);
  }

private:
  const Func& func;
};

}

template<class Func, class... Args>
typename std::result_of<Func (IRInstruction*)>::type
makeInstruction(Func func, Args... args) {
  typedef typename std::result_of<Func (IRInstruction*)>::type Ret;
  return detail::InstructionBuilder<Ret,Func>(func).go(args...);
}

//////////////////////////////////////////////////////////////////////

/*
 * IRFactory is used for allocating and controlling the lifetime of IR
 * objects.
 */
class IRFactory {
public:
  IRFactory()
    : m_nextLabelId(0)
    , m_nextOpndId(0)
    , m_nextInstId(0)
  {}

  /*
   * Create an IRInstruction with lifetime equivalent to this IRFactory.
   *
   * Arguments are passed in the following format:
   *
   *   gen(Opcode, [IRExtraData*,] [type param,] [exit label,] [srcs ...]);
   *
   * All arguments are optional except the opcode.  `srcs' may be
   * specified either as a list of SSATmp*'s, or as a integer size and
   * a SSATmp**.
   */
  template<class... Args>
  IRInstruction* gen(Args... args) {
    return makeInstruction(
      [this] (IRInstruction* inst) { return inst->clone(this); },
      args...
    );
  }

  template<class T>
  T* cloneInstruction(const T* inst) {
    T* newInst = new (m_arena) T(m_arena, inst,
                                 IRInstruction::IId(m_nextInstId++));
    newSSATmp(newInst);
    return newInst;
  }

  LabelInstruction* defLabel(const Func*);
  LabelInstruction* defLabel(const Func*, unsigned numDst);

  /*
   * Creates move instrution that moves from src to dst. We can't use gen
   * to create such a move because gen assigns a newly allocated destination
   * SSATmp whereas we want to use the given dst SSATmp.
   */
  IRInstruction* mov(SSATmp* dst, SSATmp* src);

  Arena&   arena()               { return m_arena; }
  uint32_t numTmps() const       { return m_nextOpndId; }
  uint32_t numLabels() const     { return m_nextLabelId; }
  uint32_t numInsts() const      { return m_nextInstId; }

private:
  void newSSATmp(IRInstruction* inst) {
    if (!inst->hasDst()) return;
    SSATmp* tmp = new (m_arena) SSATmp(m_nextOpndId++, inst);
    inst->setDst(tmp);
  }

  uint32_t m_nextLabelId;
  uint32_t m_nextOpndId;
  uint32_t m_nextInstId;

  // SSATmp and IRInstruction objects are allocated here.
  Arena m_arena;
};

/*
 * Utility to keep a vector of state about each instruction, indexed by
 * instruction id.
 */
template<class T>
struct InstrState {
  InstrState(IRFactory* factory, T init)
    : m_info(factory->numInsts(), init)
    , m_factory(factory)
    , m_init(init) {
  }
  T& operator[](const IRInstruction* inst) {
    auto iid = inst->getIId();
    if (iid >= m_info.size()) grow();
    assert(iid < m_info.size());
    return m_info[iid];
  }
  const T& operator[](const IRInstruction* inst) const {
    assert(inst->getIId() < m_info.size());
    return m_info[inst->getIId()];
  }
  void reset() {
    for (size_t i = 0, n = m_info.size(); i < n; ++i) {
      m_info[i] = m_init;
    }
    grow();
  }

private:
  void grow() {
    for (size_t i = m_info.size(), n = m_factory->numInsts(); i < n; ++i) {
      m_info.push_back(m_init);
    }
  }

private:
  std::vector<T> m_info;
  IRFactory* m_factory;
  T m_init;
};

//////////////////////////////////////////////////////////////////////

}}}

#endif
