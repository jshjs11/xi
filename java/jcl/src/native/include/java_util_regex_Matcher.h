/* DO NOT EDIT THIS FILE - it is machine generated */
#include <jni.h>
/* Header for class java_util_regex_Matcher */

#ifndef _Included_java_util_regex_Matcher
#define _Included_java_util_regex_Matcher
#ifdef __cplusplus
extern "C" {
#endif
/*
 * Class:     java_util_regex_Matcher
 * Method:    closeImpl
 * Signature: (I)V
 */
JNIEXPORT void JNICALL Java_java_util_regex_Matcher_closeImpl
  (JNIEnv *, jclass, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    findImpl
 * Signature: (ILjava/lang/String;I[I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_findImpl
  (JNIEnv *, jclass, jint, jstring, jint, jintArray);

/*
 * Class:     java_util_regex_Matcher
 * Method:    findNextImpl
 * Signature: (ILjava/lang/String;[I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_findNextImpl
  (JNIEnv *, jclass, jint, jstring, jintArray);

/*
 * Class:     java_util_regex_Matcher
 * Method:    groupCountImpl
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_java_util_regex_Matcher_groupCountImpl
  (JNIEnv *, jclass, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    hitEndImpl
 * Signature: (I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_hitEndImpl
  (JNIEnv *, jclass, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    lookingAtImpl
 * Signature: (ILjava/lang/String;[I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_lookingAtImpl
  (JNIEnv *, jclass, jint, jstring, jintArray);

/*
 * Class:     java_util_regex_Matcher
 * Method:    matchesImpl
 * Signature: (ILjava/lang/String;[I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_matchesImpl
  (JNIEnv *, jclass, jint, jstring, jintArray);

/*
 * Class:     java_util_regex_Matcher
 * Method:    openImpl
 * Signature: (I)I
 */
JNIEXPORT jint JNICALL Java_java_util_regex_Matcher_openImpl
  (JNIEnv *, jclass, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    requireEndImpl
 * Signature: (I)Z
 */
JNIEXPORT jboolean JNICALL Java_java_util_regex_Matcher_requireEndImpl
  (JNIEnv *, jclass, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    setInputImpl
 * Signature: (ILjava/lang/String;II)V
 */
JNIEXPORT void JNICALL Java_java_util_regex_Matcher_setInputImpl
  (JNIEnv *, jclass, jint, jstring, jint, jint);

/*
 * Class:     java_util_regex_Matcher
 * Method:    useAnchoringBoundsImpl
 * Signature: (IZ)V
 */
JNIEXPORT void JNICALL Java_java_util_regex_Matcher_useAnchoringBoundsImpl
  (JNIEnv *, jclass, jint, jboolean);

/*
 * Class:     java_util_regex_Matcher
 * Method:    useTransparentBoundsImpl
 * Signature: (IZ)V
 */
JNIEXPORT void JNICALL Java_java_util_regex_Matcher_useTransparentBoundsImpl
  (JNIEnv *, jclass, jint, jboolean);

#ifdef __cplusplus
}
#endif
#endif
