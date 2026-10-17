#include <cstring>
#include <ffi.h>
#include <iostream>
#include <ranges>
#include <java/lang/Class.h>
#include <java/lang/Thread.h>
#include <java/lang/Throwable.h>
#include <java/lang/reflect/Constructor.h>
#include <java/lang/reflect/Field.h>
#include <java/lang/reflect/Method.h>
#include <java/nio/Buffer.h>
#include <java/nio/ByteBuffer.h>

#include "Clearwing.h"

#include <string_view>
#include <java/lang/String.h>
#include <java/lang/VirtualMachineError.h>
#include <java/lang/ref/WeakReference.h>

#include <jni.h>

using namespace std::string_view_literals;

typedef JNIEnv *jnienv;

#define MAX_JNI_ARGS 255
#define MAX_FFI_ARGS (MAX_JNI_ARGS + 2)

template<typename B>
static int jniTry(jnienv env, const char *name, B block) {
    auto ctx = (jcontext)env;
    int result = JNI_OK;
    tryCatch(ctx, name, [&] {
        block(ctx);
    }, nullptr, [&](jobject e) {
        result = JNI_ERR;
        ctx->jniException = (jthrowable)e;
    });
    return result;
}

template<class T>
static T jniTryOr(jnienv env, const char *name, const std::function<T(jcontext ctx)> &block, T defaultVal = {}) {
    auto ctx = (jcontext)env;
    T result = defaultVal;
    tryCatch(ctx, name, [&] {
        result = block(ctx);
    }, nullptr, [&](jobject e) {
        ctx->jniException = (jthrowable)e;
    });
    return result;
}

static void addLocalRef(jnienv env, jobject obj) {
    auto ctx = (jcontext) env;
    auto &frame = ctx->frames[ctx->stackDepth - 1];
    frame.localRefs.back().push_back(obj);
}

static void deleteLocalRef(jnienv env, jobject obj) {
    auto ctx = (jcontext) env;
    auto &frame = ctx->frames[ctx->stackDepth - 1];
    for (auto &local : frame.localRefs | std::ranges::views::reverse) {
        if (const auto it = std::ranges::find(local, obj); it != local.end()) {
            local.erase(it);
            break;
        }
    }
}

static jobject addGlobalRef(jnienv env, jobject obj) {
    if (!obj)
        return nullptr;
    if ((jclass)obj->clazz == &class_java_lang_ref_WeakReference)
        obj = (jobject)((jweak) obj)->F_ptr;
    jcontext(env)->globalRefs.push_back(obj);
    return obj;
}

static void deleteGlobalRef(jnienv env, jobject obj) {
    auto ctx = (jcontext) env;
    if (const auto it = std::ranges::find(ctx->globalRefs, obj); it != ctx->globalRefs.end())
        ctx->globalRefs.erase(it);
}

template<typename T>
static jarray newArray(jnienv env, jclass clazz, int len, T defaultValue = {}) {
    return jniTryOr<jarray>(env, "NewArrayJNI", [&](jcontext ctx) {
        jarray array = createArray(ctx, clazz, len);
        if (defaultValue) {
            for (int i = 0; i < len; i++)
                ((T *)array->data)[i] = defaultValue;
        }
        return array;
    });
}

template<typename T>
static T getArrayElement(jnienv env, jarray array, int index) {
    return jniTryOr<T>(env, "GetArrayElementJNI", [&](jcontext ctx) {
        if (index < 0 or index >= array->length)
            throwIndexOutOfBounds(ctx);
        return ((T *)array->data)[index];
    });
}

template<typename T>
static void setArrayElement(jnienv env, jarray array, int index, T value) {
    jniTry(env, "SetArrayElementJNI", [&](jcontext ctx) {
        if (index < 0 or index >= array->length)
            throwIndexOutOfBounds(ctx);
        ((T *)array->data)[index] = value;
    });
}

template<typename T>
static T *getArrayElements(jnienv env, jarray array, jbool *isCopy) {
    if (isCopy)
        *isCopy = true;
    addGlobalRef(env, (jobject)array);
    return (T *)array->data;
}

static void releaseArrayElements(jnienv env, jarray array) {
    deleteGlobalRef(env, (jobject)array);
}

template<typename T>
static void getArrayRegion(jnienv env, jarray array, T *buffer, int start, int len) {
    jniTry(env, "GetArrayRegionJNI", [&](jcontext ctx) {
        if (start < 0 or len < 0 or start + len > array->length)
            throwIndexOutOfBounds(ctx);
        memcpy(buffer, (T *)array->data + start, len * sizeof(T));
    });
}

template<typename T>
static void setArrayRegion(jnienv env, jarray array, const T *buffer, int start, int len) {
    jniTry(env, "SetArrayRegionJNI", [&](jcontext ctx) {
        if (start < 0 or len < 0 or start + len > array->length)
            throwIndexOutOfBounds(ctx);
        memcpy((T *)array->data + start, buffer, len * sizeof(T));
    });
}

static jmethod findMethod_(jcontext ctx, jclass clazz, std::string_view name, std::string_view signature, bool isStatic) {
    M_java_lang_Class_ensureInitialized(ctx, (jobject)clazz);
    bool isConstructor = "<init>" == name;
    auto methods = (jarray)(isConstructor ? clazz->constructors : clazz->methods);
    for (int i = 0; i < methods->length; i++) {
        auto method = ((jmethod *)methods->data)[i];
        bool staticMethod = method->F_modifiers & 0x8;
        if (stringToNative(ctx, (jstring)method->F_name) == name and stringToNative(ctx, (jstring)method->F_desc) == signature and staticMethod == isStatic) {
            M_java_lang_reflect_Method_ensureSignatureInitialized(ctx, (jobject)method);
            return method;
        }
    }
    for (int i = 0; i < clazz->interfaceCount; i++) {
        if (auto method = findMethod_(ctx, ((jclass *)clazz->nativeInterfaces)[i], name, signature, isStatic))
            return method;
    }
    if (clazz->parentClass)
        return findMethod_(ctx, (jclass)clazz->parentClass, name, signature, isStatic);
    return nullptr;
}

static jmethod findMethod(jnienv env, jclass clazz, std::string_view name, std::string_view signature, bool isStatic) {
    return jniTryOr<jmethod>(env, "FindMethodJNI", [&](jcontext ctx) {
        if (jmethod method = findMethod_(ctx, clazz, name, signature, isStatic))
            return method;
        throwIllegalArgument(ctx); // Should be NoSuchMethodError
    });
}

static jfield findField(jnienv env, jclass clazz, std::string_view name, std::string_view signature, bool isStatic) {
    return jniTryOr<jfield>(env, "FindFieldJNI", [&](jcontext ctx) {
        M_java_lang_Class_ensureInitialized(ctx, (jobject)clazz);
        auto fields = (jarray) clazz->fields;
        for (int i = 0; i < fields->length; i++) {
            auto field = ((jfield *)fields->data)[i];
            bool staticField = field->F_modifiers & 0x8;
            if (stringToNative(ctx, (jstring)field->F_name) == name and stringToNative(ctx, (jstring)field->F_signature) == signature and staticField == isStatic)
                return field;
        }
        throwIllegalArgument(ctx); // Should be NoSuchFieldError
    });
}

template<typename T>
static constexpr ffi_type *getFfiType() {
    if constexpr (std::is_same_v<T, jbool>)
        return &ffi_type_uint8;
    else if constexpr (std::is_same_v<T, jbyte>)
        return &ffi_type_sint8;
    else if constexpr (std::is_same_v<T, jchar>)
        return &ffi_type_uint16;
    else if constexpr (std::is_same_v<T, jshort>)
        return &ffi_type_sint16;
    else if constexpr (std::is_same_v<T, jint>)
        return &ffi_type_sint32;
    else if constexpr (std::is_same_v<T, jlong>)
        return &ffi_type_sint64;
    else if constexpr (std::is_same_v<T, jfloat>)
        return &ffi_type_float;
    else if constexpr (std::is_same_v<T, jdouble>)
        return &ffi_type_double;
    else
        return &ffi_type_pointer;
}

static ffi_type *typeToFFI(jclass type) {
    if (type == &class_byte)
        return &ffi_type_sint8;
    if (type == &class_short)
        return &ffi_type_sint16;
    if (type == &class_char)
        return &ffi_type_uint16;
    if (type == &class_int)
        return &ffi_type_sint32;
    if (type == &class_long)
        return &ffi_type_sint64;
    if (type == &class_float)
        return &ffi_type_float;
    if (type == &class_double)
        return &ffi_type_double;
    if (type == &class_boolean)
        return &ffi_type_uint8;
    return &ffi_type_pointer;
}

template<typename T>
static T invokeMethod(jnienv env, jclass clazz, jobject self, jmethod method, const jvalue *args) {
    ffi_type *argTypes[MAX_FFI_ARGS];
    const void *argPtrs[MAX_FFI_ARGS];

    auto ctx = (jcontext) env;
    bool isStatic = !self;
    bool isSpecial = !isStatic and clazz;
    auto owner = (jclass) method->F_declaringClass;
    bool isInterface = owner->access & 0x200;
    int argOffset = !isStatic ? 2 : 1;
    auto paramTypes = (jarray) method->F_parameterTypes;

    if (isStatic)
        ((static_init_ptr)((jclass)method->F_declaringClass)->staticInitializer)(ctx);

    argTypes[0] = &ffi_type_pointer;
    argPtrs[0] = &ctx;
    if (!isStatic) {
        argTypes[1] = &ffi_type_pointer;
        argPtrs[1] = &self;
    }

    for (int i = 0; i < paramTypes->length; i++) {
        jclass type = ((jclass *)paramTypes->data)[i];
        argTypes[argOffset + i] = typeToFFI(type);
        argPtrs[argOffset + i] = &args[i];
    }

    void *func;
    if (isInterface)
        func = resolveInterfaceMethod(ctx, owner, (int) method->F_offset, self);
    else if (isStatic or isSpecial)
        func = (void *) method->F_address;
    else
        func = ((void **) self->vtable)[method->F_offset];

    bool initialSuspend = ctx->suspended;
    ctx->suspended = false;
    SAFEPOINT();

    jvalue returnValue{};
    jniTry(env, "InvokeJNI", [&](jcontext) {
        ffi_cif cif;
        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, paramTypes->length + argOffset, getFfiType<T>(), argTypes) != FFI_OK)
            throw std::runtime_error("FFI CIF prep failed");
        ffi_call(&cif, (void (*)()) func, &returnValue, (void **)argPtrs);
    });

    ctx->suspended = initialSuspend;
    if constexpr (std::is_same_v<T, jobject>) // Todo: Should really just have this return value on a stack
        protectObject(*(jobject *)&returnValue);
    SAFEPOINT();
    if constexpr (std::is_same_v<T, jobject>)
        unprotectObject(*(jobject *)&returnValue);

    if constexpr (!std::is_same_v<T, void>)
        return *(T *)&returnValue;
    else
        return;
}

template<typename T>
static T invokeMethod(jnienv env, jclass clazz, jobject self, jmethod method, va_list argList) {
    jvalue args[MAX_JNI_ARGS];
    auto paramTypes = (jarray) method->F_parameterTypes;
    for (int i = 0; i < paramTypes->length; i++) {
        jvalue &arg = args[i];
        jclass type = ((jclass *)paramTypes->data)[i];
        if (type == &class_boolean or type == &class_byte or type == &class_char or type == &class_short or type == &class_int)
            arg.i = va_arg(argList, int);
        else if (type == &class_long)
            arg.j = va_arg(argList, jlong);
        else if (type == &class_float)
            arg.f = (float)va_arg(argList, jdouble);
        else if (type == &class_double)
            arg.d = va_arg(argList, jdouble);
        else
            arg.l = va_arg(argList, jobject);
    }
    return invokeMethod<T>(env, clazz, self, method, args);
}

static jobject vmNewObject(jnienv env, jclass clazz, jmethod method, const jvalue *args) {
    auto obj = jniTryOr<jobject>(env, "NewObject", [&](jcontext ctx) {
        return gcAlloc(ctx, clazz);
    });
    if (!obj)
        return nullptr;
    invokeMethod<void>(env, clazz, obj, method, args);
    return obj;
}

static jobject vmNewObject(jnienv env, jclass clazz, jmethod method, va_list args) {
    auto obj = jniTryOr<jobject>(env, "NewObject", [&](jcontext ctx) {
        return gcAlloc(ctx, clazz);
    });
    if (!obj)
        return nullptr;
    invokeMethod<void>(env, clazz, obj, method, args);
    return obj;
}

static jobject vmNewObject(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = vmNewObject(env, clazz, method, args);
    va_end(args);
    return result;
}

static jobject vmCallObjectMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jobject>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jboolean vmCallBooleanMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jbool>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jbyte vmCallByteMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jbyte>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jchar vmCallCharMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jchar>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jshort vmCallShortMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jshort>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jint vmCallIntMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jint>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jlong vmCallLongMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jlong>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jfloat vmCallFloatMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jfloat>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static jdouble vmCallDoubleMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jdouble>(env, nullptr, obj, method, args);
    va_end(args);
    return result;
}

static void vmCallVoidMethod(jnienv env, jobject obj, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    invokeMethod<void>(env, nullptr, obj, method, args);
    va_end(args);
}

static jobject vmCallNonvirtualObjectMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jobject>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jboolean vmCallNonvirtualBooleanMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jboolean>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jbyte vmCallNonvirtualByteMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jbyte>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jchar vmCallNonvirtualCharMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jchar>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jshort vmCallNonvirtualShortMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jshort>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jint vmCallNonvirtualIntMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jint>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jlong vmCallNonvirtualLongMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jlong>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jfloat vmCallNonvirtualFloatMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jfloat>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static jdouble vmCallNonvirtualDoubleMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jdouble>(env, clazz, obj, method, args);
    va_end(args);
    return result;
}

static void vmCallNonvirtualVoidMethod(jnienv env, jobject obj, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    invokeMethod<jvoid>(env, clazz, obj, method, args);
    va_end(args);
}

static jobject vmCallStaticObjectMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jobject>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jboolean vmCallStaticBooleanMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jbool>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jbyte vmCallStaticByteMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jbyte>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jchar vmCallStaticCharMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jchar>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jshort vmCallStaticShortMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jshort>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jint vmCallStaticIntMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jint>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jlong vmCallStaticLongMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jlong>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jfloat vmCallStaticFloatMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jfloat>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static jdouble vmCallStaticDoubleMethod(jnienv env, jclass clazz, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    auto result = invokeMethod<jdouble>(env, clazz, nullptr, method, args);
    va_end(args);
    return result;
}

static void vmCallStaticVoidMethod(jnienv env, jclass cls, jmethod method, ...) {
    va_list args;
    va_start(args, method);
    invokeMethod<void>(env, cls, nullptr, method, args);
    va_end(args);
}

extern "C" {

jni createJni(jcontext ctx) {
    return new JNINativeInterface_{
        .reserved0 = ctx,

        .GetVersion = [](jnienv) -> jint {
            return JNI_VERSION_1_8;
        },

        .DefineClass = [](jnienv env, [[maybe_unused]] const char *name, [[maybe_unused]] jobject loader, [[maybe_unused]] const jbyte *buf, [[maybe_unused]] jsize len) -> jclass {
            jniTry(env, "DefineClass", [&](jcontext ctx) {
                auto ex = gcAlloc(ctx, &class_java_lang_VirtualMachineError);
                init_java_lang_VirtualMachineError_java_lang_String(ctx, ex, (jobject)createStringLiteral(ctx, u8"DefineClass not supported"_j));
                ctx->jniException = (jthrowable)ex;
            });
            return nullptr;
        },

        .FindClass = [](jnienv, const char *name) -> jclass {
            return classForName(name);
        },

        .FromReflectedMethod = [](jnienv, jobject method) -> jmethod {
            return (jmethod)method;
        },

        .FromReflectedField = [](jnienv, jobject field) -> jfield {
            return (jfield)field;
        },

        .ToReflectedMethod = [](jnienv, jclass, jmethod method, [[maybe_unused]] jboolean isStatic) -> jobject {
            return (jobject)method;
        },

        .GetSuperclass = [](jnienv, jclass sub) -> jclass {
            return (jclass)((jclass) sub->parent.clazz)->parentClass;
        },

        .IsAssignableFrom = [](jnienv env, jclass sub, jclass sup) -> jboolean {
            return isAssignableFrom((jcontext) env, sup, sub);
        },

        .ToReflectedField = [](jnienv, jclass, jfield field, [[maybe_unused]] jboolean isStatic) -> jobject {
            return (jobject)field;
        },

        .Throw = [](jnienv env, jthrowable obj) -> jint {
            jcontext(env)->jniException = obj;
            return JNI_OK;
        },

        .ThrowNew = [](jnienv env, jclass clazz, const char *msg) -> jint {
            auto ctx = (jcontext) env;
            jobject msgStr{};
            jobject obj{};
            tryCatch(ctx, "ThrowNew", [&] {
                auto constructors = (jarray)clazz->constructors;
                for (int i = 0; i < constructors->length; i++) {
                    auto constructor = ((java_lang_reflect_Constructor **)constructors->data)[i];
                    auto method = (java_lang_reflect_Method *) constructor->F_method;
                    if ("(Ljava/lang/String;)V"sv == stringToNative(ctx, (jstring)method->F_desc)) {
                        typedef void(* ThrowConstructor)(jcontext ctx, jobject self, jobject message);
                        msgStr = (jobject)stringFromNativeProtected(ctx, msg);
                        obj = gcAllocProtected(ctx, clazz);
                        ((ThrowConstructor)method->F_offset)(ctx, obj, msgStr);
                        ctx->jniException = (jthrowable)obj;
                        break;
                    }
                }
            }, nullptr, [&](jobject ex) {
                if (obj)
                    unprotectObject(obj);
                obj = nullptr;
                ctx->jniException = (jthrowable)ex;
            });
            if (msgStr)
                unprotectObject(msgStr);
            return obj ? JNI_OK : JNI_ERR;
        },

        .ExceptionOccurred = [](jnienv env) -> jthrowable {
            auto ctx = (jcontext)env;
            if (ctx->jniException) {
                // addLocalRef(env, (jobject)ctx->jniException); // Todo: Supposed to push local ref?
                return ctx->jniException;
            }
            return nullptr;
        },

        .ExceptionDescribe = [](jnienv env) -> void {
            jniTry(env, "ExceptionDescribe", [&](jcontext ctx) {
                if (jthrowable ex = ctx->jniException)
                    std::cerr << "JNI Exception: " << ex->F_message ? stringToNative(ctx, (jstring)ex->F_message) : (char *)((jclass) ex->parent.clazz)->nativeName;
            });
        },

        .ExceptionClear = [](jnienv env) -> void {
            jcontext(env)->jniException = nullptr;
        },

        .FatalError = [](jnienv, const char *msg) -> void {
            std::cerr << "Fatal Error: " << msg;
            exit(-1);
        },

        .PushLocalFrame = [](jnienv env, [[maybe_unused]] jint capacity) -> jint {
            auto ctx = (jcontext)env;
            auto &frame = ctx->frames[ctx->stackDepth - 1];
            frame.localRefs.emplace_back();
            return JNI_OK;
        },

        .PopLocalFrame = [](jnienv env, jobject result) -> jobject {
            auto ctx = (jcontext)env;
            auto &frame = ctx->frames[ctx->stackDepth - 1];
            frame.localRefs.pop_back();
            if (frame.localRefs.size() <= 1)
                throw std::runtime_error("No local reference frame to pop");
            if (result)
                frame.localRefs.back().push_back(result);
            return result;
        },

        .NewGlobalRef = addGlobalRef,

        .DeleteGlobalRef = deleteGlobalRef,

        .DeleteLocalRef = [](jnienv env, jobject obj) -> void {
            deleteLocalRef(env, obj);
        },

        .IsSameObject = [](jnienv, jobject obj1, jobject obj2) -> jboolean {
            if (obj1 == obj2)
                return true;
            if (!obj1 || !obj2)
                return false;
            jobject target1 = (jclass)obj1->clazz == &class_java_lang_ref_WeakReference ? (jobject)((jweak)obj1)->F_ptr : obj1;
            jobject target2 = (jclass)obj2->clazz == &class_java_lang_ref_WeakReference ? (jobject)((jweak)obj2)->F_ptr : obj2;
            return target1 == target2;
        },

        .NewLocalRef = [](jnienv env, jobject ref) -> jobject {
            if (!ref)
                return nullptr;
            addLocalRef(env, ref);
            return ref;
        },

        .EnsureLocalCapacity = [](jnienv, [[maybe_unused]] jint capacity) -> jint {
            return JNI_OK; // No-op
        },

        .AllocObject = [](jnienv env, jclass clazz) -> jobject {
            return jniTryOr<jobject>(env, "AllocObject", [&](jcontext ctx) {
                return gcAlloc(ctx, clazz);
            });
        },

        .NewObject = vmNewObject,

        .NewObjectV = vmNewObject,

        .NewObjectA = vmNewObject,

        .GetObjectClass = [](jnienv, jobject obj) -> jclass {
            return (jclass)obj->clazz;
        },

        .IsInstanceOf = [](jnienv env, jobject obj, jclass clazz) -> jboolean {
            return isInstance((jcontext) env, obj, clazz);
        },

        .GetMethodID = [](jnienv env, jclass clazz, const char *name, const char *sig) -> jmethod {
            return findMethod(env, clazz, name, sig, false);
        },

        .CallObjectMethod = vmCallObjectMethod,

        .CallObjectMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jobject {
            return invokeMethod<jobject>(env, nullptr, obj, method, args);
        },

        .CallObjectMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue * args) -> jobject {
            return invokeMethod<jobject>(env, nullptr, obj, method, args);
        },

        .CallBooleanMethod = vmCallBooleanMethod,

        .CallBooleanMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jboolean {
            return invokeMethod<jbool>(env, nullptr, obj, method, args);
        },

        .CallBooleanMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue * args) -> jboolean {
            return invokeMethod<jbool>(env, nullptr, obj, method, args);
        },

        .CallByteMethod = vmCallByteMethod,

        .CallByteMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jbyte {
            return invokeMethod<jbyte>(env, nullptr, obj, method, args);
        },

        .CallByteMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jbyte {
            return invokeMethod<jbyte>(env, nullptr, obj, method, args);
        },

        .CallCharMethod = vmCallCharMethod,

        .CallCharMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jchar {
            return invokeMethod<jchar>(env, nullptr, obj, method, args);
        },

        .CallCharMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jchar {
            return invokeMethod<jchar>(env, nullptr, obj, method, args);
        },

        .CallShortMethod = vmCallShortMethod,

        .CallShortMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jshort {
            return invokeMethod<jshort>(env, nullptr, obj, method, args);
        },

        .CallShortMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jshort {
            return invokeMethod<jshort>(env, nullptr, obj, method, args);
        },

        .CallIntMethod = vmCallIntMethod,

        .CallIntMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jint {
            return invokeMethod<jint>(env, nullptr, obj, method, args);
        },

        .CallIntMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jint {
            return invokeMethod<jint>(env, nullptr, obj, method, args);
        },

        .CallLongMethod = vmCallLongMethod,

        .CallLongMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jlong {
            return invokeMethod<jlong>(env, nullptr, obj, method, args);
        },

        .CallLongMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jlong {
            return invokeMethod<jlong>(env, nullptr, obj, method, args);
        },

        .CallFloatMethod = vmCallFloatMethod,

        .CallFloatMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jfloat {
            return invokeMethod<jfloat>(env, nullptr, obj, method, args);
        },

        .CallFloatMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jfloat {
            return invokeMethod<jfloat>(env, nullptr, obj, method, args);
        },

        .CallDoubleMethod = vmCallDoubleMethod,

        .CallDoubleMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> jdouble {
            return invokeMethod<jdouble>(env, nullptr, obj, method, args);
        },

        .CallDoubleMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue *args) -> jdouble {
            return invokeMethod<jdouble>(env, nullptr, obj, method, args);
        },

        .CallVoidMethod = vmCallVoidMethod,

        .CallVoidMethodV = [](jnienv env, jobject obj, jmethod method, va_list args) -> void {
            invokeMethod<void>(env, nullptr, obj, method, args);
        },

        .CallVoidMethodA = [](jnienv env, jobject obj, jmethod method, const jvalue * args) -> void {
            invokeMethod<void>(env, nullptr, obj, method, args);
        },

        .CallNonvirtualObjectMethod = vmCallNonvirtualObjectMethod,

        .CallNonvirtualObjectMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jobject {
            return invokeMethod<jobject>(env, clazz, obj, method, args);
        },

        .CallNonvirtualObjectMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue * args) -> jobject {
            return invokeMethod<jobject>(env, clazz, obj, method, args);
        },

        .CallNonvirtualBooleanMethod = vmCallNonvirtualBooleanMethod,

        .CallNonvirtualBooleanMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jboolean {
            return invokeMethod<jbool>(env, clazz, obj, method, args);
        },

        .CallNonvirtualBooleanMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue * args) -> jboolean {
            return invokeMethod<jbool>(env, clazz, obj, method, args);
        },

        .CallNonvirtualByteMethod = vmCallNonvirtualByteMethod,

        .CallNonvirtualByteMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jbyte {
            return invokeMethod<jbyte>(env, clazz, obj, method, args);
        },

        .CallNonvirtualByteMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jbyte {
            return invokeMethod<jbyte>(env, clazz, obj, method, args);
        },

        .CallNonvirtualCharMethod = vmCallNonvirtualCharMethod,

        .CallNonvirtualCharMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jchar {
            return invokeMethod<jchar>(env, clazz, obj, method, args);
        },

        .CallNonvirtualCharMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jchar {
            return invokeMethod<jchar>(env, clazz, obj, method, args);
        },

        .CallNonvirtualShortMethod = vmCallNonvirtualShortMethod,

        .CallNonvirtualShortMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jshort {
            return invokeMethod<jshort>(env, clazz, obj, method, args);
        },

        .CallNonvirtualShortMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jshort {
            return invokeMethod<jshort>(env, clazz, obj, method, args);
        },

        .CallNonvirtualIntMethod = vmCallNonvirtualIntMethod,

        .CallNonvirtualIntMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jint {
            return invokeMethod<jint>(env, clazz, obj, method, args);
        },

        .CallNonvirtualIntMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jint {
            return invokeMethod<jint>(env, clazz, obj, method, args);
        },

        .CallNonvirtualLongMethod = vmCallNonvirtualLongMethod,

        .CallNonvirtualLongMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jlong {
            return invokeMethod<jlong>(env, clazz, obj, method, args);
        },

        .CallNonvirtualLongMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jlong {
            return invokeMethod<jlong>(env, clazz, obj, method, args);
        },

        .CallNonvirtualFloatMethod = vmCallNonvirtualFloatMethod,

        .CallNonvirtualFloatMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jfloat {
            return invokeMethod<jfloat>(env, clazz, obj, method, args);
        },

        .CallNonvirtualFloatMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jfloat {
            return invokeMethod<jfloat>(env, clazz, obj, method, args);
        },

        .CallNonvirtualDoubleMethod = vmCallNonvirtualDoubleMethod,

        .CallNonvirtualDoubleMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> jdouble {
            return invokeMethod<jdouble>(env, clazz, obj, method, args);
        },

        .CallNonvirtualDoubleMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue *args) -> jdouble {
            return invokeMethod<jdouble>(env, clazz, obj, method, args);
        },

        .CallNonvirtualVoidMethod = vmCallNonvirtualVoidMethod,

        .CallNonvirtualVoidMethodV = [](jnienv env, jobject obj, jclass clazz, jmethod method, va_list args) -> void {
            invokeMethod<void>(env, clazz, obj, method, args);
        },

        .CallNonvirtualVoidMethodA = [](jnienv env, jobject obj, jclass clazz, jmethod method, const jvalue * args) -> void {
            invokeMethod<void>(env, clazz, obj, method, args);
        },

        .GetFieldID = [](jnienv env, jclass clazz, const char *name, const char *sig) -> jfield {
            return findField(env, clazz, name, sig, false);
        },

        .GetObjectField = [](jnienv, jobject obj, jfield field) -> jobject {
            return *(jobject *)((char *)obj + field->F_offset);
        },

        .GetBooleanField = [](jnienv, jobject obj, jfield field) -> jboolean {
            return *(jbool *)((char *)obj + field->F_offset);
        },

        .GetByteField = [](jnienv, jobject obj, jfield field) -> jbyte {
            return *(jbyte *)((char *)obj + field->F_offset);
        },

        .GetCharField = [](jnienv, jobject obj, jfield field) -> jchar {
            return *(jchar *)((char *)obj + field->F_offset);
        },

        .GetShortField = [](jnienv, jobject obj, jfield field) -> jshort {
            return *(jshort *)((char *)obj + field->F_offset);
        },

        .GetIntField = [](jnienv, jobject obj, jfield field) -> jint {
            return *(jint *)((char *)obj + field->F_offset);
        },

        .GetLongField = [](jnienv, jobject obj, jfield field) -> jlong {
            return *(jlong *)((char *)obj + field->F_offset);
        },

        .GetFloatField = [](jnienv, jobject obj, jfield field) -> jfloat {
            return *(jfloat *)((char *)obj + field->F_offset);
        },

        .GetDoubleField = [](jnienv, jobject obj, jfield field) -> jdouble {
            return *(jdouble *)((char *)obj + field->F_offset);
        },

        .SetObjectField = [](jnienv, jobject obj, jfield field, jobject val) -> void {
            *(jobject *)((char *)obj + field->F_offset) = val;
        },

        .SetBooleanField = [](jnienv, jobject obj, jfield field, jboolean val) -> void {
            *(jbool *)((char *)obj + field->F_offset) = val;
        },

        .SetByteField = [](jnienv, jobject obj, jfield field, jbyte val) -> void {
            *(jbyte *)((char *)obj + field->F_offset) = val;
        },

        .SetCharField = [](jnienv, jobject obj, jfield field, jchar val) -> void {
            *(jchar *)((char *)obj + field->F_offset) = val;
        },

        .SetShortField = [](jnienv, jobject obj, jfield field, jshort val) -> void {
            *(jshort *)((char *)obj + field->F_offset) = val;
        },

        .SetIntField = [](jnienv, jobject obj, jfield field, jint val) -> void {
            *(jint *)((char *)obj + field->F_offset) = val;
        },

        .SetLongField = [](jnienv, jobject obj, jfield field, jlong val) -> void {
            *(jlong *)((char *)obj + field->F_offset) = val;
        },

        .SetFloatField = [](jnienv, jobject obj, jfield field, jfloat val) -> void {
            *(jfloat *)((char *)obj + field->F_offset) = val;
        },

        .SetDoubleField = [](jnienv, jobject obj, jfield field, jdouble val) -> void {
            *(jdouble *)((char *)obj + field->F_offset) = val;
        },

        .GetStaticMethodID = [](jnienv env, jclass clazz, const char *name, const char *sig) -> jmethod {
            return findMethod(env, clazz, name, sig, true);
        },

        .CallStaticObjectMethod = vmCallStaticObjectMethod,

        .CallStaticObjectMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jobject {
            return invokeMethod<jobject>(env, clazz, nullptr, method, args);
        },

        .CallStaticObjectMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jobject {
            return invokeMethod<jobject>(env, clazz, nullptr, method, args);
        },

        .CallStaticBooleanMethod = vmCallStaticBooleanMethod,

        .CallStaticBooleanMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jboolean {
            return invokeMethod<jbool>(env, clazz, nullptr, method, args);
        },

        .CallStaticBooleanMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jboolean {
            return invokeMethod<jbool>(env, clazz, nullptr, method, args);
        },

        .CallStaticByteMethod = vmCallStaticByteMethod,

        .CallStaticByteMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jbyte {
            return invokeMethod<jbyte>(env, clazz, nullptr, method, args);
        },

        .CallStaticByteMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jbyte {
            return invokeMethod<jbyte>(env, clazz, nullptr, method, args);
        },

        .CallStaticCharMethod = vmCallStaticCharMethod,

        .CallStaticCharMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jchar {
            return invokeMethod<jchar>(env, clazz, nullptr, method, args);
        },

        .CallStaticCharMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jchar {
            return invokeMethod<jchar>(env, clazz, nullptr, method, args);
        },

        .CallStaticShortMethod = vmCallStaticShortMethod,

        .CallStaticShortMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jshort {
            return invokeMethod<jshort>(env, clazz, nullptr, method, args);
        },

        .CallStaticShortMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jshort {
            return invokeMethod<jshort>(env, clazz, nullptr, method, args);
        },

        .CallStaticIntMethod = vmCallStaticIntMethod,

        .CallStaticIntMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jint {
            return invokeMethod<jint>(env, clazz, nullptr, method, args);
        },

        .CallStaticIntMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jint {
            return invokeMethod<jint>(env, clazz, nullptr, method, args);
        },

        .CallStaticLongMethod = vmCallStaticLongMethod,

        .CallStaticLongMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jlong {
            return invokeMethod<jlong>(env, clazz, nullptr, method, args);
        },

        .CallStaticLongMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jlong {
            return invokeMethod<jlong>(env, clazz, nullptr, method, args);
        },

        .CallStaticFloatMethod = vmCallStaticFloatMethod,

        .CallStaticFloatMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jfloat {
            return invokeMethod<jfloat>(env, clazz, nullptr, method, args);
        },

        .CallStaticFloatMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jfloat {
            return invokeMethod<jfloat>(env, clazz, nullptr, method, args);
        },

        .CallStaticDoubleMethod = vmCallStaticDoubleMethod,

        .CallStaticDoubleMethodV = [](jnienv env, jclass clazz, jmethod method, va_list args) -> jdouble {
            return invokeMethod<jdouble>(env, clazz, nullptr, method, args);
        },

        .CallStaticDoubleMethodA = [](jnienv env, jclass clazz, jmethod method, const jvalue *args) -> jdouble {
            return invokeMethod<jdouble>(env, clazz, nullptr, method, args);
        },

        .CallStaticVoidMethod = vmCallStaticVoidMethod,

        .CallStaticVoidMethodV = [](jnienv env, jclass cls, jmethod method, va_list args) -> void {
            invokeMethod<void>(env, cls, nullptr, method, args);
        },

        .CallStaticVoidMethodA = [](jnienv env, jclass cls, jmethod method, const jvalue * args) -> void {
            invokeMethod<void>(env, cls, nullptr, method, args);
        },

        .GetStaticFieldID = [](jnienv env, jclass clazz, const char *name, const char *sig) -> jfield {
            return findField(env, clazz, name, sig, true);
        },

        .GetStaticObjectField = [](jnienv, jclass, jfield field) -> jobject {
            return *(jobject *)field->F_offset;
        },

        .GetStaticBooleanField = [](jnienv, jclass, jfield field) -> jboolean {
            return *(jboolean *)field->F_offset;
        },

        .GetStaticByteField = [](jnienv, jclass, jfield field) -> jbyte {
            return *(jbyte *)field->F_offset;
        },

        .GetStaticCharField = [](jnienv, jclass, jfield field) -> jchar {
            return *(jchar *)field->F_offset;
        },

        .GetStaticShortField = [](jnienv, jclass, jfield field) -> jshort {
            return *(jshort *)field->F_offset;
        },

        .GetStaticIntField = [](jnienv, jclass, jfield field) -> jint {
            return *(jint *)field->F_offset;
        },

        .GetStaticLongField = [](jnienv, jclass, jfield field) -> jlong {
            return *(jlong *)field->F_offset;
        },

        .GetStaticFloatField = [](jnienv, jclass, jfield field) -> jfloat {
            return *(jfloat *)field->F_offset;
        },

        .GetStaticDoubleField = [](jnienv, jclass, jfield field) -> jdouble {
            return *(jdouble *)field->F_offset;
        },

        .SetStaticObjectField = [](jnienv, jclass, jfield field, jobject value) -> void {
            *(jobject *)field->F_offset = value;
        },

        .SetStaticBooleanField = [](jnienv, jclass, jfield field, jboolean value) -> void {
            *(jbool *)field->F_offset = value;
        },

        .SetStaticByteField = [](jnienv, jclass, jfield field, jbyte value) -> void {
            *(jbyte *)field->F_offset = value;
        },

        .SetStaticCharField = [](jnienv, jclass, jfield field, jchar value) -> void {
            *(jchar *)field->F_offset = value;
        },

        .SetStaticShortField = [](jnienv, jclass, jfield field, jshort value) -> void {
            *(jshort *)field->F_offset = value;
        },

        .SetStaticIntField = [](jnienv, jclass, jfield field, jint value) -> void {
            *(jint *)field->F_offset = value;
        },

        .SetStaticLongField = [](jnienv, jclass, jfield field, jlong value) -> void {
            *(jlong *)field->F_offset = value;
        },

        .SetStaticFloatField = [](jnienv, jclass, jfield field, jfloat value) -> void {
            *(jfloat *)field->F_offset = value;
        },

        .SetStaticDoubleField = [](jnienv, jclass, jfield field, jdouble value) -> void {
            *(jdouble *)field->F_offset = value;
        },

        .NewString = [](jnienv env, const jchar *unicode, jsize len) -> jstring {
            return jniTryOr<jstring>(env, "NewString", [&](jcontext ctx) {
                jobject str = gcAllocProtected(ctx, &class_java_lang_String);
                jarray chars = createArrayProtected(ctx, &class_char, len);
                memcpy(chars->data, unicode, len * sizeof(jchar));
                init_java_lang_String_Array1_char(ctx, str, (jobject)chars);
                unprotectObject((jobject)chars);
                unprotectObject(str);
                return (jstring)str;
            });
        },

        .GetStringLength = [](jnienv, jstring str) -> jsize {
            return str->F_count;
        },

        .GetStringChars = [](jnienv env, jstring str, jboolean *isCopy) -> const jchar * {
            if (isCopy)
                *isCopy = false;
            addGlobalRef(env, (jobject)str);
            return (jchar *)((jarray)str->F_value)->data;
        },

        .ReleaseStringChars = [](jnienv env, jstring str, const jchar *chars) -> void {
            deleteGlobalRef(env, (jobject)str);
        },

        .NewStringUTF = [](jnienv env, const char *utf) -> jstring {
            return jniTryOr<jstring>(env, "NewStringUTF", [&](jcontext ctx) {
                return stringFromNative(ctx, utf);
            });
        },

        .GetStringUTFLength = [](jnienv env, jstring str) -> jsize {
            return jniTryOr<int>(env, "GetStringUTFLength", [&](jcontext ctx) {
                return ((jarray)M_java_lang_String_getBytes_R_Array1_byte(ctx, (jobject)str))->length; // This is not ideal, but easy
            });
        },

        .GetStringUTFChars = [](jnienv env, jstring str, jboolean *isCopy) -> const char* {
            if (isCopy)
                *isCopy = true;
            return jniTryOr<const char *>(env, "GetStringUTFChars", [&](jcontext ctx) {
                auto bytes = (jarray)M_java_lang_String_getBytes_R_Array1_byte(ctx, (jobject)str);
                auto chars = new char[bytes->length + 1]{};
                memcpy(chars, bytes->data, bytes->length);
                return chars;
            });
        },

        .ReleaseStringUTFChars = [](jnienv, jstring str, const char* chars) -> void {
            delete[] (char *)chars;
        },

        .GetArrayLength = [](jnienv, jarray array) -> jsize {
            return array->length;
        },

        .NewObjectArray = [](jnienv env, jsize len, jclass clazz, jobject init) -> jobjectArray {
            return newArray(env, clazz, len, init);
        },

        .GetObjectArrayElement = [](jnienv env, jobjectArray array, jsize index) -> jobject {
            return getArrayElement<jobject>(env, array, index);
        },

        .SetObjectArrayElement = [](jnienv env, jobjectArray array, jsize index, jobject val) -> void {
            setArrayElement(env, array, index, val);
        },

        .NewBooleanArray = [](jnienv env, jsize len) -> jbooleanArray {
            return newArray<jbool>(env, &class_boolean, len);
        },

        .NewByteArray = [](jnienv env, jsize len) -> jbyteArray {
            return newArray<jbyte>(env, &class_byte, len);
        },

        .NewCharArray = [](jnienv env, jsize len) -> jcharArray {
            return newArray<jchar>(env, &class_char, len);
        },

        .NewShortArray = [](jnienv env, jsize len) -> jshortArray {
            return newArray<jshort>(env, &class_short, len);
        },

        .NewIntArray = [](jnienv env, jsize len) -> jintArray {
            return newArray<jint>(env, &class_int, len);
        },

        .NewLongArray = [](jnienv env, jsize len) -> jlongArray {
            return newArray<jlong>(env, &class_long, len);
        },

        .NewFloatArray = [](jnienv env, jsize len) -> jfloatArray {
            return newArray<jfloat>(env, &class_float, len);
        },

        .NewDoubleArray = [](jnienv env, jsize len) -> jdoubleArray {
            return newArray<jdouble>(env, &class_double, len);
        },

        .GetBooleanArrayElements = [](jnienv env, jbooleanArray array, jboolean *isCopy) -> jboolean * {
            return getArrayElements<jbool>(env, array, isCopy);
        },

        .GetByteArrayElements = [](jnienv env, jbyteArray array, jboolean *isCopy) -> jbyte * {
            return getArrayElements<jbyte>(env, array, isCopy);
        },

        .GetCharArrayElements = [](jnienv env, jcharArray array, jboolean *isCopy) -> jchar * {
            return getArrayElements<jchar>(env, array, isCopy);
        },

        .GetShortArrayElements = [](jnienv env, jshortArray array, jboolean *isCopy) -> jshort * {
            return getArrayElements<jshort>(env, array, isCopy);
        },

        .GetIntArrayElements = [](jnienv env, jintArray array, jboolean *isCopy) -> jint * {
            return getArrayElements<jint>(env, array, isCopy);
        },

        .GetLongArrayElements = [](jnienv env, jlongArray array, jboolean *isCopy) -> jlong * {
            return getArrayElements<jlong>(env, array, isCopy);
        },

        .GetFloatArrayElements = [](jnienv env, jfloatArray array, jboolean *isCopy) -> jfloat * {
            return getArrayElements<jfloat>(env, array, isCopy);
        },

        .GetDoubleArrayElements = [](jnienv env, jdoubleArray array, jboolean *isCopy) -> jdouble * {
            return getArrayElements<jdouble>(env, array, isCopy);
        },

        .ReleaseBooleanArrayElements = [](jnienv env, jbooleanArray array, [[maybe_unused]] jboolean *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseByteArrayElements = [](jnienv env, jbyteArray array, [[maybe_unused]] jbyte *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseCharArrayElements = [](jnienv env, jcharArray array, [[maybe_unused]] jchar *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseShortArrayElements = [](jnienv env, jshortArray array, [[maybe_unused]] jshort *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseIntArrayElements = [](jnienv env, jintArray array, [[maybe_unused]] jint *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseLongArrayElements = [](jnienv env, jlongArray array, [[maybe_unused]] jlong *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseFloatArrayElements = [](jnienv env, jfloatArray array, [[maybe_unused]] jfloat *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .ReleaseDoubleArrayElements = [](jnienv env, jdoubleArray array, [[maybe_unused]] jdouble *elems, [[maybe_unused]] jint mode) -> void {
            releaseArrayElements(env, array);
        },

        .GetBooleanArrayRegion = [](jnienv env, jbooleanArray array, jsize start, jsize len, jboolean *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetByteArrayRegion = [](jnienv env, jbyteArray array, jsize start, jsize len, jbyte *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetCharArrayRegion = [](jnienv env, jcharArray array, jsize start, jsize len, jchar *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetShortArrayRegion = [](jnienv env, jshortArray array, jsize start, jsize len, jshort *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetIntArrayRegion = [](jnienv env, jintArray array, jsize start, jsize len, jint *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetLongArrayRegion = [](jnienv env, jlongArray array, jsize start, jsize len, jlong *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetFloatArrayRegion = [](jnienv env, jfloatArray array, jsize start, jsize len, jfloat *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .GetDoubleArrayRegion = [](jnienv env, jdoubleArray array, jsize start, jsize len, jdouble *buf) -> void {
            getArrayRegion(env, array, buf, start, len);
        },

        .SetBooleanArrayRegion = [](jnienv env, jbooleanArray array, jsize start, jsize len, const jboolean *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetByteArrayRegion = [](jnienv env, jbyteArray array, jsize start, jsize len, const jbyte *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetCharArrayRegion = [](jnienv env, jcharArray array, jsize start, jsize len, const jchar *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetShortArrayRegion = [](jnienv env, jshortArray array, jsize start, jsize len, const jshort *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetIntArrayRegion = [](jnienv env, jintArray array, jsize start, jsize len, const jint *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetLongArrayRegion = [](jnienv env, jlongArray array, jsize start, jsize len, const jlong *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetFloatArrayRegion = [](jnienv env, jfloatArray array, jsize start, jsize len, const jfloat *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .SetDoubleArrayRegion = [](jnienv env, jdoubleArray array, jsize start, jsize len, const jdouble *buf) -> void {
            setArrayRegion(env, array, buf, start, len);
        },

        .RegisterNatives = [](jnienv env, jclass clazz, const JNINativeMethod *methods, jint nMethods) -> jint {
            return jniTry(env, "RegisterNatives", [&](jcontext ctx) {
                for (int i = 0; i < nMethods; i++) {
                    const JNINativeMethod &nativeMethod = methods[i];
                    std::string_view name{nativeMethod.name};
                    std::string_view signature{nativeMethod.signature};
                    auto methodArray = (jarray)clazz->methods;
                    bool found = false;
                    for (int j = 0; j < methodArray->length; j++) {
                        auto method = ((jmethod *)methodArray->data)[j];
                        if (stringToNative(ctx, (jstring)method->F_name) == name and stringToNative(ctx, (jstring)method->F_desc) == signature) {
                            method->F_nativeFunc = (jlong)nativeMethod.fnPtr;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        throwIllegalArgument(ctx);
                }
            });
        },

        .UnregisterNatives = [](jnienv, jclass clazz) -> jint {
            auto methods = (jarray)clazz->methods;
            for (int i = 0; i < methods->length; i++)
                ((jmethod *)methods->data)[i]->F_nativeFunc = 0;
            return JNI_OK;
        },

        .MonitorEnter = [](jnienv env, jobject obj) -> jint {
            return jniTry(env, "MonitorEnter", [&](jcontext ctx) {
                monitorEnter(ctx, obj);
            });
        },

        .MonitorExit = [](jnienv env, jobject obj) -> jint {
            return jniTry(env, "MonitorEnter", [&](jcontext ctx) {
                monitorExit(ctx, obj);
            });
        },

        .GetJavaVM = [](jnienv, jvm *vm) -> jint {
            *vm = getJavaVM();
            return JNI_OK;
        },

        .GetStringRegion = [](jnienv env, jstring str, jsize start, jsize len, jchar *buf) -> void {
            jniTry(env, "GetStringRegion", [&](jcontext ctx) {
                if (start < 0 or len < 0 or start + len > str->F_count)
                    throwIndexOutOfBounds(ctx);
                memcpy(buf, (jchar *)((jarray)str->F_value)->data + start, len * sizeof(jchar));
            });
        },

        .GetStringUTFRegion = [](jnienv env, jstring str, jsize start, jsize len, char *buf) -> void {
            jniTry(env, "GetStringUTFRegion", [&](jcontext ctx) {
                auto bytes = (jarray)M_java_lang_String_getBytes_R_Array1_byte(ctx, (jobject)str);
                if (start < 0 or len < 0 or start + len > bytes->length)
                    throwIndexOutOfBounds(ctx);
                memcpy(buf, (jbyte *)bytes->data + start, len);
            });
        },

        .GetPrimitiveArrayCritical = [](jnienv env, jarray array, jboolean *isCopy) -> void * {
            if (isCopy)
                *isCopy = false;
            addGlobalRef(env, (jobject)array);
            return array->data;
        },

        .ReleasePrimitiveArrayCritical = [](jnienv env, jarray array, [[maybe_unused]] void *carray, [[maybe_unused]] jint mode) -> void {
            deleteGlobalRef(env, (jobject)array);
        },

        .GetStringCritical = [](jnienv env, jstring string, jboolean *isCopy) -> const jchar * {
            if (isCopy)
                *isCopy = false;
            addGlobalRef(env, (jobject)string);
            return (jchar *)((jarray)string->F_value)->data;
        },

        .ReleaseStringCritical = [](jnienv env, jstring string, [[maybe_unused]] const jchar *cstring) -> void {
            deleteGlobalRef(env, (jobject)string);
        },

        .NewWeakGlobalRef = [](jnienv env, jobject obj) -> jweak {
            if (!obj || (jclass)obj->clazz == &class_java_lang_ref_WeakReference)
                return nullptr;
            return jniTryOr<jweak>(env, "NewWeakGlobalRef", [&](jcontext ctx) {
                auto weak = (jweak)gcAllocProtected(ctx, &class_java_lang_ref_WeakReference);
                init_java_lang_ref_WeakReference_java_lang_Object(ctx, (jobject)weak, obj);
                unprotectObject((jobject)weak);
                addGlobalRef(env, (jobject)weak);
                return weak;
            });
        },

        .DeleteWeakGlobalRef = [](jnienv env, jweak ref) -> void {
            deleteGlobalRef(env, (jobject)ref);
        },

        .ExceptionCheck = [](jnienv env) -> jboolean {
            return jcontext(env)->jniException;
        },

        .NewDirectByteBuffer = [](jnienv env, void* address, jlong capacity) -> jobject {
            auto ctx = (jcontext)env->functions->reserved0;
            ctx->suspended = false;
            SAFEPOINT();
            jobject pixelBuffer{};
            tryCatchFinally(ctx, "NewDirectByteBuffer", [&] {
                pixelBuffer = gcAllocProtected(ctx, &class_java_nio_ByteBuffer);
                init_java_nio_ByteBuffer_long_int_boolean(ctx, pixelBuffer, (jlong) address, (jint)capacity, false);
            }, nullptr, [&](jobject e) {
                ctx->jniException = (jthrowable)e;
            }, [&] {
                if (pixelBuffer)
                    unprotectObject(pixelBuffer);
                ctx->suspended = true;
            });
            return pixelBuffer;
        },

        .GetDirectBufferAddress = [](jnienv, jobject buf) -> void * {
            return (void *)((java_nio_Buffer *)buf)->F_address;
        },

        .GetDirectBufferCapacity = [](jnienv, jobject buf) -> jlong {
            return ((java_nio_Buffer *)buf)->F_capacity;
        },

        /* New JNI 1.6 Features */

        .GetObjectRefType = [](jnienv, jobject) -> jobjectRefType {
            return JNIInvalidRefType; // Not storing as separate reference objects aside from jweak, so just can't fully differentiate
        },

        /* Module Features */

        .GetModule = [](jnienv, jclass) -> jobject {
            return nullptr;
        },

        /* Virtual threads */

        .IsVirtualThread = [](jnienv env, jobject obj) -> jboolean {
            return false;
        },
    };
}

static int vmAttachThread(jvm, void **penv, void *args) {
    if (jcontext ctx = getThreadContext()) {
        *penv = ctx;
        return JNI_OK;
    }
    jcontext ctx = createContext();
    jthread thread{};
    tryCatch(ctx, "AttachCurrentThread", [&] {
        thread = (jthread) gcAllocEternal(ctx, &class_java_lang_Thread);
        init_java_lang_Thread(ctx, (jobject)thread); // Todo: Support name arg
    }, nullptr, [&](jobject) {
        if (thread)
            makeEphemeral((jobject)thread);
        thread = nullptr;
    });
    if (!thread) {
        destroyContext(ctx);
        return JNI_ERR;
    }
    thread->F_alive = true;
    thread->F_started = true;
    thread->F_nativeContext = (jlong)ctx;
    ctx->thread = thread;
    attachThread(ctx);
    *penv = ctx;
    return JNI_OK;
}

JNIInvokeInterface_ invokeInterfaceInst{
    .DestroyJavaVM = [](jvm){
        shutdownVM(nullptr);
        return JNI_OK;
    },

    .AttachCurrentThread = vmAttachThread,

    .DetachCurrentThread = [](jvm) {
        jcontext ctx = getThreadContext();
        if (!ctx)
            return JNI_EDETACHED;
        makeEphemeral((jobject)ctx->thread);
        ctx->thread->F_alive = false;
        ctx->thread->F_finished = true;
        detachThread();
        destroyContext(ctx);
        return JNI_OK;
    },

    .GetEnv = [](jvm, void **penv, [[maybe_unused]] jint version) {
        jcontext ctx = getThreadContext();
        *penv = ctx;
        return ctx ? JNI_OK : JNI_EDETACHED;
    },

    .AttachCurrentThreadAsDaemon = vmAttachThread
};

JavaVM vmInst{ .functions = &invokeInterfaceInst };

jvm getJavaVM() {
    return &vmInst;
}

void destroyJni(jni env) {
    delete env;
}

}
