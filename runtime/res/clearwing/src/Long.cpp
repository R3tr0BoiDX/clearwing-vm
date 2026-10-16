#include "java/lang/Long.h"

#include <charconv>

extern "C" {

jobject SM_java_lang_Long_toString_long_int_R_java_lang_String(jcontext ctx, jlong value, jint radix) {
    if (radix < 2 || radix > 36) // Like the JDK
        radix = 10;
    char buffer[65]; // Sign and 64 digits in radix 2
    auto end = std::to_chars(buffer, buffer + sizeof(buffer), value, radix).ptr;
    return (jobject) stringFromNativeLength(ctx, buffer, (int) (end - buffer));
}

}
