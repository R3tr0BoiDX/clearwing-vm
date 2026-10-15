#include "java/lang/Throwable.h"
#include "java/lang/StackTraceElement.h"
#include "java/lang/String.h"

#include <string_view>

extern "C" {

// Only records the method names and line numbers: building the trace objects for every exception was slow, and
// libraries like Kryo throw and catch many. Throwable.getStackTrace and getStack build them when asked for.
void M_java_lang_Throwable_fillInStack(jcontext ctx, jobject self) {
    auto throwable = (java_lang_Throwable *) NULL_CHECK(self);
    int depth = ctx->stackDepth;
    auto frames = createArray(ctx, &class_long, depth * 2);
    auto data = (jlong *) frames->data;
    for (int i = 0; i < depth; i++) {
        auto &frame = ctx->frames[depth - 1 - i];
        data[i * 2] = (jlong) (frame.info ? frame.info->method : nullptr); // Static strings
        data[i * 2 + 1] = frame.lineNumber;
    }
    throwable->F_frames = (jref) frames;
    throwable->F_stackTrace = 0;
    throwable->F_stack = 0;
}

jobject SM_java_lang_Throwable_buildStackTrace_Array1_long_R_Array1_java_lang_StackTraceElement(jcontext ctx, jobject framesObj) {
    jtype frame[4];
    FrameInfo frameInfo { "java/lang/Throwable:buildStackTrace", 4 };
    auto frameRef = pushStackFrame(ctx, &frameInfo, frame);

    auto frames = (jarray) NULL_CHECK(framesObj);
    int depth = frames->length / 2;
    jarray trace = createArray(ctx, &class_java_lang_StackTraceElement, depth);
    frame[0].o = (jobject) trace;
    for (int i = 0; i < depth; i++) {
        auto data = (jlong *) frames->data;
        std::string_view method = data[i * 2] ? (const char *) data[i * 2] : "";
        auto line = (jint) data[i * 2 + 1];
        auto separator = method.find(':');
        frame[1].o = (jobject) stringFromNative(ctx, separator != std::string_view::npos ? method.substr(0, separator) : "Unknown");
        frame[2].o = (jobject) stringFromNative(ctx, separator != std::string_view::npos ? method.substr(separator + 1) : "unknown");
        frame[3].o = frame[1].o;
        ((jobject *) trace->data)[i] = constructObject<&class_java_lang_StackTraceElement, init_java_lang_StackTraceElement_java_lang_String_java_lang_String_java_lang_String_int>(ctx, frame[1].o, frame[2].o, frame[3].o, line);
    }

    popStackFrame(ctx);
    return frame[0].o;
}

}
