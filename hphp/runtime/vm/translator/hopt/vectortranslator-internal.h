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

#ifndef incl_RUNTIME_VM_TRANSLATOR_HOPT_VECTOR_TRANSLATOR_HELPERS_H_
#define incl_RUNTIME_VM_TRANSLATOR_HOPT_VECTOR_TRANSLATOR_HELPERS_H_

#include "util/base.h"
#include "runtime/base/types.h"
#include "runtime/vm/stats.h"
#include "runtime/vm/translator/translator.h"

namespace HPHP { namespace VM { namespace JIT {

/* In hhir-translated tracelets, the MInstrState is stored right above
 * the reserved spill space so we add an extra offset.  */
#undef MISOFF
#define MISOFF(nm) (offsetof(Transl::MInstrState, nm) + kReservedRSPSpillSpace)

#define CTX() m_tb.genDefConst(arGetContextClass(curFrame()))

static const MInstrAttr Warn = MIA_warn;
static const MInstrAttr Unset = MIA_unset;
static const MInstrAttr Reffy = MIA_reffy;
static const MInstrAttr Define = MIA_define;
static const MInstrAttr None = MIA_none;
static const MInstrAttr WarnDefine = MInstrAttr(Warn | Define);
static const MInstrAttr DefineReffy = MInstrAttr(Define | Reffy);
static const MInstrAttr WarnDefineReffy = MInstrAttr(Warn | Define | Reffy);
#define WDU(attrs) attrs & Warn, attrs & Define, attrs & Unset
#define WDRU(attrs) attrs & Warn, attrs & Define, attrs & Reffy, attrs & Unset

/* The following bunch of macros and functions are used to build up tables of
 * helper function pointers and determine which helper should be called based
 * on a variable number of bool and enum arguments. */

template<typename T> constexpr unsigned bitWidth() {
  static_assert(IncDec_invalid == 4,
                "IncDecOp enum must fit in 2 bits");
  static_assert(SetOp_invalid == 11,
                "SetOpOp enum must fit in 4 bits");
  return std::is_same<T, bool>::value ? 1
    : std::is_same<T, KeyType>::value ? 2
    : std::is_same<T, MInstrAttr>::value ? 4
    : std::is_same<T, IncDecOp>::value ? 2
    : std::is_same<T, SetOpOp>::value ? 4
    : sizeof(T) * CHAR_BIT;
}

// Determines the width in bits of all of its arguments
template<typename... T> unsigned multiBitWidth();
template<typename T, typename... Args>
inline unsigned multiBitWidth(T t, Args... args) {
  return bitWidth<T>() + multiBitWidth<Args...>(args...);
}
template<>
inline unsigned multiBitWidth() {
  return 0;
}

// Given the same arguments as multiBitWidth, buildBitmask will determine which
// index in the table corresponds to the provided parameters.
template<unsigned bit>
inline unsigned buildBitmask() {
  static_assert(bit < (sizeof(unsigned) * CHAR_BIT - 1), "Too many bits");
  return 0;
}
template<unsigned bit = 0, typename T, typename... Args>
inline unsigned buildBitmask(T c, Args... args) {
  unsigned bits = (unsigned)c & ((1u << bitWidth<T>()) - 1);
  return buildBitmask<bit + bitWidth<T>()>(args...) | bits << bit;
}

// FILL_ROW and BUILD_OPTAB* build up the static table of function pointers
#define FILL_ROW(nm, ...) do {                          \
    OpFunc* dest = &optab[buildBitmask(__VA_ARGS__)];   \
    assert(*dest == nullptr);                           \
    *dest = nm;                                         \
  } while (false);
#define FILL_ROWH(nm, hot, ...) FILL_ROW(nm, __VA_ARGS__)

#define BUILD_OPTAB(...) BUILD_OPTAB_ARG(HELPER_TABLE(FILL_ROW), __VA_ARGS__)
#define BUILD_OPTABH(...) BUILD_OPTAB_ARG(HELPER_TABLE(FILL_ROWH), __VA_ARGS__)
#define BUILD_OPTAB_ARG(FILL_TABLE, ...)                                \
  static OpFunc* optab = nullptr;                                          \
  if (!optab) {                                                         \
    optab = (OpFunc*)calloc(1 << multiBitWidth(__VA_ARGS__), sizeof(OpFunc)); \
    FILL_TABLE                                                          \
  }                                                                     \
  unsigned idx = buildBitmask(__VA_ARGS__);                             \
  OpFunc opFunc = optab[idx];                                           \
  assert(opFunc);

// The getKeyType family of functions determine the KeyType to be used as a
// template argument to helper functions. S, IS, or I at the end of the
// function names signals that the caller supports non-literal strings, int, or
// both, respectively.
static KeyType getKeyType(const SSATmp* key, bool nonLitStr,
                          bool nonLitInt) {
  assert(key->getType().isStaticallyKnown());

  if (key->isBoxed()) {
    // Variants can change types at arbitrary times, so don't try to
    // pass them in registers.
    return AnyKey;
  }
  if ((key->isConst() || nonLitStr) && key->isString()) {
    return StrKey;
  } else if ((key->isConst() || nonLitInt) && key->isA(Type::Int)) {
    return IntKey;
  } else {
    return AnyKey;
  }
}
inline static KeyType getKeyType(const SSATmp* key) {
  return getKeyType(key, false, false);
}
inline static KeyType getKeyTypeS(const SSATmp* key) {
  return getKeyType(key, true, false);
}
inline static KeyType getKeyTypeIS(const SSATmp* key) {
  return getKeyType(key, true, true);
}


// The next few functions are helpers to assert commonly desired
// conditions about values. Most often used inline while passing
// arguments to a helper.
inline static SSATmp* ptr(SSATmp* t) {
  assert(t->getType().isPtr());
  return t;
}
inline static SSATmp* objOrPtr(SSATmp* t) {
  assert(t->isA(Type::Obj) || t->getType().isPtr());
  return t;
}
inline static SSATmp* noLitInt(SSATmp* t) {
  assert(!(t->isConst() && t->isA(Type::Int)));
  return t;
}

// keyPtr is used by helper function implementations to convert a
// TypedValue passed by value into a TypedValue* suitable for passing
// to helpers from member_operations.h
template<KeyType kt>
static inline TypedValue* keyPtr(TypedValue& key) {
  if (kt == AnyKey) {
    return &key;
  } else {
    return reinterpret_cast<TypedValue*>(key.m_data.num);
  }
}

template<KeyType kt, bool isRef>
static inline TypedValue* unbox(TypedValue* k) {
  if (isRef) {
    if (kt == AnyKey) {
      assert(k->m_type == KindOfRef);
      k = k->m_data.pref->tv();
      assert(k->m_type != KindOfRef);
    } else {
      assert(k->m_type == keyDataType(kt) ||
             (IS_STRING_TYPE(k->m_type) && IS_STRING_TYPE(keyDataType(kt))));
      return reinterpret_cast<TypedValue*>(k->m_data.num);
    }
  } else if (kt == AnyKey) {
    assert(k->m_type != KindOfRef);
  }
  return k;
}

} } }

#endif
