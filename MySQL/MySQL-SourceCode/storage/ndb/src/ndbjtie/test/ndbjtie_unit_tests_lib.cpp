/*
 Copyright (c) 2012, 2025, Oracle and/or its affiliates.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License, version 2.0,
 as published by the Free Software Foundation.

 This program is designed to work with certain software (including
 but not limited to OpenSSL) that is licensed under separate terms,
 as designated in a particular file or component or in included license
 documentation.  The authors of MySQL hereby grant you an additional
 permission to link the program and your derivative works with the
 separately licensed software that they have either included with
 the program or referenced in the documentation.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License, version 2.0, for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
/*
 * ndbjtie_unit_tests_lib.cpp
 */

// libraries
#include "helpers.hpp"
#include "jtie.hpp"
#include "jtie_lib.hpp"

// the applied Java mapping of basic variable-width C++ types
#define JTIE_JNI_SHORT_T jshort
#define JTIE_JNI_INT_T jint
#define JTIE_JNI_LONG_T jint
#define JTIE_JNI_LONGLONG_T jlong
#define JTIE_JNI_LONGDOUBLE_T jdouble
#include "jtie_tconv_vwidth.hpp"

// global test ndbjtie_unit_tests library definitions
#include "ndbjtie_unit_tests_consts.hpp"

// ---------------------------------------------------------------------------
// JNI function stubs
// ---------------------------------------------------------------------------

// The API stub functions in this file have mangled names that adhere
// to the JVM specification.  It is not necessary to include the
// function prototypes generated by the javah tool from the Java source
// if they are declared to receive "C" linkage here.
extern "C" {

// #include "test_NdbJTieConstantsTest.h"

/*
 * Class:     test_NdbJTieConstantsTest
 * Method:    nativeConstValue
 * Signature: (Ljava/lang/String;)J
 */
JNIEXPORT jlong JNICALL Java_test_NdbJTieConstantsTest_nativeConstValue(
    JNIEnv *env, jclass cls, jstring p0) {
  TRACE(
      "jlong Java_test_NdbJTieConstantsTest_nativeConstValue(JNIEnv *, jclass, "
      "jstring)");
  return gcall_fr<ttrait_int64, ttrait_char_cp_jutf8null, &::nativeConstValue>(
      env, cls, p0);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Library Load and Unload Handlers
// ---------------------------------------------------------------------------

// Initializes the JTie resources; called when the native library is loaded;
// returns the JNI version needed by the native library or JNI_ERR.
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *jvm, void *reserved) {
  TRACE("jint JNI_OnLoad(JavaVM *, void *)");
  VERBOSE("loading the NDB JTie Unit Test library ...");

  const jint required_jni_version = JTie_OnLoad(jvm, reserved);
  if (required_jni_version == JNI_ERR) {
    PRINT_ERROR("JTie_OnLoad() returned: JNI_ERR");
    return JNI_ERR;
  }

  VERBOSE("... loaded the NDB JTie Unit Test library");
  return required_jni_version;
}

// Called when the class loader containing the native library is garbage
// collected; called in an unknown context (such as from a finalizer):
// be conservative, and refrain from arbitrary Java call-backs.
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *jvm, void *reserved) {
  TRACE("void JNI_OnUnload(JavaVM *, void *)");
  VERBOSE("unloading the NDB JTie Unit Test library...");

  JTie_OnUnload(jvm, reserved);

  VERBOSE("... unloaded the NDB JTie Unit Test library");
}

// ---------------------------------------------------------------------------
