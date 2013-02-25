/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   | Copyright (c) 1997-2010 The PHP Group                                |
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
namespace HPHP {

/*
void HPHP::f_asio_enter_context()
_ZN4HPHP20f_asio_enter_contextEv

*/

void fh_asio_enter_context() asm("_ZN4HPHP20f_asio_enter_contextEv");

/*
void HPHP::f_asio_exit_context()
_ZN4HPHP19f_asio_exit_contextEv

*/

void fh_asio_exit_context() asm("_ZN4HPHP19f_asio_exit_contextEv");

/*
HPHP::Object HPHP::f_asio_get_current()
_ZN4HPHP18f_asio_get_currentEv

(return value) => rax
_rv => rdi
*/

Value* fh_asio_get_current(Value* _rv) asm("_ZN4HPHP18f_asio_get_currentEv");

/*
void HPHP::f_asio_set_on_failed_callback(HPHP::Object const&)
_ZN4HPHP29f_asio_set_on_failed_callbackERKNS_6ObjectE

on_failed_cb => rdi
*/

void fh_asio_set_on_failed_callback(Value* on_failed_cb) asm("_ZN4HPHP29f_asio_set_on_failed_callbackERKNS_6ObjectE");


} // !HPHP

