#include "java/lang/Integer.h"

#include <charconv>

extern "C" {

jobject SM_java_lang_Integer_toString_int_int_R_java_lang_String(jcontext ctx, jint value, jint radix) {
    if (radix < 2 || radix > 36) // Like the JDK
        radix = 10;
    char buffer[33]; // Sign and 32 digits in radix 2
    auto end = std::to_chars(buffer, buffer + sizeof(buffer), value, radix).ptr;
    return (jobject) stringFromNativeLength(ctx, buffer, (int) (end - buffer));
}

}
