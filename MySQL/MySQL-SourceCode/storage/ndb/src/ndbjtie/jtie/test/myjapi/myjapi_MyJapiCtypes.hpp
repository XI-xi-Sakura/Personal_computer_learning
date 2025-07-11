/*
 Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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
 * myjapi_MyJapiCtypes.hpp
 */

#ifndef myjapi_MyJapiCtypes_hpp
#define myjapi_MyJapiCtypes_hpp

#include <jni.h>

// API to implement against
#include "myapi.hpp"

// libraries
#include "helpers.hpp"
#include "jtie.hpp"

// the applied Java mapping of basic variable-width C++ types
#define JTIE_JNI_SHORT_T jshort
#define JTIE_JNI_INT_T jint
#define JTIE_JNI_LONG_T jint
#define JTIE_JNI_LONGLONG_T jlong
#define JTIE_JNI_LONGDOUBLE_T jdouble
#include "jtie_tconv_vwidth.hpp"

// The API stub functions in this file have mangled names that adhere
// to the JVM specification.  It is not necessary to include the
// function prototypes generated by the javah tool from the Java source,
//   #include "myjapi_MyJapiCtypes.h"
// if they are declared to receive "C" linkage here.
extern "C" {

// ---------------------------------------------------------------------------
// API JNI platform-dependent function stubs
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

JNIEXPORT jboolean JNICALL Java_myjapi_MyJapiCtypes_f31(JNIEnv *env, jclass cls,
                                                        jboolean p0) {
  TRACE("jboolean Java_myjapi_MyJapiCtypes_f31(JNIEnv *, jclass, jboolean)");
  return gcall_fr<ttrait_bool, ttrait_bool, f31>(env, cls, p0);
}

JNIEXPORT jbyte JNICALL Java_myjapi_MyJapiCtypes_f32(JNIEnv *env, jclass cls,
                                                     jbyte p0) {
  TRACE("jbyte Java_myjapi_MyJapiCtypes_f32(JNIEnv *, jclass, jbyte)");
  return gcall_fr<ttrait_char, ttrait_char, f32>(env, cls, p0);
}

JNIEXPORT jbyte JNICALL Java_myjapi_MyJapiCtypes_f33(JNIEnv *env, jclass cls,
                                                     jbyte p0) {
  TRACE("jbyte Java_myjapi_MyJapiCtypes_f33(JNIEnv *, jclass, jbyte)");
  return gcall_fr<ttrait_schar, ttrait_schar, f33>(env, cls, p0);
}

JNIEXPORT jbyte JNICALL Java_myjapi_MyJapiCtypes_f34(JNIEnv *env, jclass cls,
                                                     jbyte p0) {
  TRACE("jbyte Java_myjapi_MyJapiCtypes_f34(JNIEnv *, jclass, jbyte)");
  return gcall_fr<ttrait_uchar, ttrait_uchar, f34>(env, cls, p0);
}

JNIEXPORT jshort JNICALL Java_myjapi_MyJapiCtypes_f35(JNIEnv *env, jclass cls,
                                                      jshort p0) {
  TRACE("jshort Java_myjapi_MyJapiCtypes_f35(JNIEnv *, jclass, jshort)");
  return gcall_fr<ttrait_short, ttrait_short, f35>(env, cls, p0);
}

JNIEXPORT jshort JNICALL Java_myjapi_MyJapiCtypes_f36(JNIEnv *env, jclass cls,
                                                      jshort p0) {
  TRACE("jshort Java_myjapi_MyJapiCtypes_f36(JNIEnv *, jclass, jshort)");
  return gcall_fr<ttrait_ushort, ttrait_ushort, f36>(env, cls, p0);
}

JNIEXPORT jint JNICALL Java_myjapi_MyJapiCtypes_f37(JNIEnv *env, jclass cls,
                                                    jint p0) {
  TRACE("jint Java_myjapi_MyJapiCtypes_f37(JNIEnv *, jclass, jint)");
  return gcall_fr<ttrait_int, ttrait_int, f37>(env, cls, p0);
}

JNIEXPORT jint JNICALL Java_myjapi_MyJapiCtypes_f38(JNIEnv *env, jclass cls,
                                                    jint p0) {
  TRACE("jint Java_myjapi_MyJapiCtypes_f38(JNIEnv *, jclass, jint)");
  return gcall_fr<ttrait_uint, ttrait_uint, f38>(env, cls, p0);
}

JNIEXPORT jint JNICALL Java_myjapi_MyJapiCtypes_f39(JNIEnv *env, jclass cls,
                                                    jint p0) {
  TRACE("jint Java_myjapi_MyJapiCtypes_f39(JNIEnv *, jclass, jint)");
  return gcall_fr<ttrait_long, ttrait_long, f39>(env, cls, p0);
}

JNIEXPORT jint JNICALL Java_myjapi_MyJapiCtypes_f40(JNIEnv *env, jclass cls,
                                                    jint p0) {
  TRACE("jint Java_myjapi_MyJapiCtypes_f40(JNIEnv *, jclass, jint)");
  return gcall_fr<ttrait_ulong, ttrait_ulong, f40>(env, cls, p0);
}

JNIEXPORT jlong JNICALL Java_myjapi_MyJapiCtypes_f41(JNIEnv *env, jclass cls,
                                                     jlong p0) {
  TRACE("jlong Java_myjapi_MyJapiCtypes_f41(JNIEnv *, jclass, jlong)");
  return gcall_fr<ttrait_longlong, ttrait_longlong, f41>(env, cls, p0);
}

JNIEXPORT jlong JNICALL Java_myjapi_MyJapiCtypes_f42(JNIEnv *env, jclass cls,
                                                     jlong p0) {
  TRACE("jlong Java_myjapi_MyJapiCtypes_f42(JNIEnv *, jclass, jlong)");
  return gcall_fr<ttrait_ulonglong, ttrait_ulonglong, f42>(env, cls, p0);
}

JNIEXPORT jfloat JNICALL Java_myjapi_MyJapiCtypes_f43(JNIEnv *env, jclass cls,
                                                      jfloat p0) {
  TRACE("jfloat Java_myjapi_MyJapiCtypes_f43(JNIEnv *, jclass, jfloat)");
  return gcall_fr<ttrait_float, ttrait_float, f43>(env, cls, p0);
}

JNIEXPORT jdouble JNICALL Java_myjapi_MyJapiCtypes_f44(JNIEnv *env, jclass cls,
                                                       jdouble p0) {
  TRACE("jdouble Java_myjapi_MyJapiCtypes_f44(JNIEnv *, jclass, jdouble)");
  return gcall_fr<ttrait_double, ttrait_double, f44>(env, cls, p0);
}

JNIEXPORT jdouble JNICALL Java_myjapi_MyJapiCtypes_f45(JNIEnv *env, jclass cls,
                                                       jdouble p0) {
  TRACE(
      "jdouble Java_myjapi_MyJapiCtypes_f45(JNIEnv * env, jclass cls, "
      "jdouble)");
  return gcall_fr<ttrait_longdouble, ttrait_longdouble, f45>(env, cls, p0);
}

// ---------------------------------------------------------------------------

}  // extern "C"

#endif  // myjapi_MyJapiCtypes_hpp
