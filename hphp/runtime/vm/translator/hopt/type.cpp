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

#include "folly/Conv.h"
#include "folly/Format.h"
#include "folly/experimental/Gen.h"

#include "util/trace.h"
#include "runtime/vm/translator/hopt/ir.h"

namespace HPHP { namespace VM { namespace JIT {

TRACE_SET_MOD(hhir);

//////////////////////////////////////////////////////////////////////

namespace {

Type vectorReturn(const IRInstruction* inst) {
  assert(inst->getOpcode() == SetProp || inst->getOpcode() == SetElem);
  return VectorEffects(inst).valType;
}

}

Type outputType(const IRInstruction* inst) {

#define D(type)   return Type::type;
#define DofS(n)   return inst->getSrc(n)->getType();
#define DUnbox(n) return inst->getSrc(n)->getType().unbox();
#define DBox(n)   return inst->getSrc(n)->getType().box();
#define DParam    return inst->getTypeParam();
#define DLabel    return Type::None;
#define DVector   return vectorReturn(inst);
#define ND        assert(0 && "outputType requires HasDest or NaryDest");

#define O(name, dstinfo, srcinfo, flags) case name: dstinfo not_reached();

  switch (inst->getOpcode()) {
  IR_OPCODES
  default: not_reached();
  }

#undef O

#undef D
#undef DofS
#undef DUnbox
#undef DBox
#undef DParam
#undef DLabel
#undef DVector
#undef ND

}

//////////////////////////////////////////////////////////////////////

namespace {

// Returns a union type containing all the types in the
// variable-length argument list
Type buildUnion() {
  return Type::None;
}

template<class... Args>
Type buildUnion(Type t, Args... ts) {
  return t | buildUnion(ts...);
}

}

/*
 * Runtime typechecking for IRInstruction operands.
 *
 * This is generated using the table in ir.h.  We instantiate
 * IR_OPCODES after defining all the various source forms to do type
 * assertions according to their form (see ir.h for documentation on
 * the notation).  The checkers appear in argument order, so each one
 * increments curSrc, and at the end we can check that the argument
 * count was also correct.
 */
void assertOperandTypes(const IRInstruction* inst) {
  if (!debug) return;

  int curSrc = 0;

  auto bail = [&] (const std::string& msg) {
    FTRACE(1, "{}", msg);
    if (!::HPHP::Trace::moduleEnabled(::HPHP::Trace::hhir, 1)) {
      fprintf(stderr, "%s\n", msg.c_str());
    }
    always_assert(false && "instruction operand type check failure");
  };

  auto getSrc = [&]() -> SSATmp* {
    if (curSrc < inst->getNumSrcs()) {
      return inst->getSrc(curSrc);
    }

    bail(folly::format(
      "Error: instruction had too few operands\n"
      "   instruction: {}\n",
        inst->toString()
      ).str()
    );
    not_reached();
  };

  auto check = [&] (bool cond, const std::string& expected) {
    if (cond) return;

    bail(folly::format(
      "Error: failed type check on operand {}\n"
      "   instruction: {}\n"
      "   was expecting: {}\n"
      "   received: {}\n",
        curSrc,
        inst->toString(),
        expected,
        inst->getSrc(curSrc)->getType().toString()
      ).str()
    );
  };

  auto checkNoArgs = [&]{
    if (inst->getNumSrcs() == 0) return;
    bail(folly::format(
      "Error: instruction expected no operands\n"
      "   instruction: {}\n",
        inst->toString()
      ).str()
    );
  };

  auto countCheck = [&]{
    if (inst->getNumSrcs() == curSrc) return;
    bail(folly::format(
      "Error: instruction had too many operands\n"
      "   instruction: {}\n"
      "   expected {} arguments\n",
        inst->toString(),
        curSrc
      ).str()
    );
  };

  auto checkDst = [&] (bool cond, const std::string& errorMessage) {
    if (cond) return;

    bail(folly::format("Error: failed type check on dest operand\n"
                       "   instruction: {}\n"
                       "   message: {}\n",
                       inst->toString(),
                       errorMessage).str());
  };

#define IRT(name, ...) UNUSED static const Type name = Type::name;
  IR_TYPES
#undef IRT

#define NA       return checkNoArgs();
#define S(...)   {                                        \
                   Type t = buildUnion(__VA_ARGS__);      \
                   check(getSrc()->isA(t), t.toString()); \
                   ++curSrc;                              \
                 }
#define C(type)  check(getSrc()->isConst() &&       \
                       getSrc()->isA(type),         \
                       "constant " #type);          \
                  ++curSrc;
#define CStr     C(StaticStr)
#define SNumInt  S(Int, Bool)
#define SNum     S(Int, Bool, Dbl)
#define SUnk     return;
#define ND
#define DLabel
#define DVector
#define D(...)
#define DUnbox(src) checkDst(src < inst->getNumSrcs(),  \
                             "invalid src num");
#define DBox(src)   checkDst(src < inst->getNumSrcs(),  \
                             "invalid src num");
#define DofS(src)   checkDst(src < inst->getNumSrcs(),  \
                             "invalid src num");
#define DParam      checkDst(inst->getTypeParam() != Type::None,        \
                             "DParam with paramType None");

#define O(opcode, dstinfo, srcinfo, flags)      \
  case opcode: dstinfo srcinfo countCheck(); return;

  switch (inst->getOpcode()) {
    IR_OPCODES
  default: assert(0);
  }

#undef O

#undef NA
#undef S
#undef C
#undef CStr
#undef SNum
#undef SUnk

#undef ND
#undef D
#undef DUnbox
#undef DBox
#undef DofS
#undef DParam

}

//////////////////////////////////////////////////////////////////////

}}}

