#include "Array.hpp"
#include "java/lang/Object.h"
#include <map>
#include <vector>
#include <mutex>
#include <cstring>

extern "C" {

void clinit_array(jcontext ctx) {
}

void mark_array(jobject object, jint mark, jint depth) {
    if (!object || (depth > GC_DEPTH_ALWAYS && (object->gcMark < GC_MARK_START || object->gcMark == mark)))
        return;
    if (depth > MAX_GC_MARK_DEPTH) {
        markDeepObject(object);
        return;
    }
    if (object->gcMark >= GC_MARK_START)
        object->gcMark = mark;
    if (((jclass) ((jclass) object->clazz)->componentClass)->primitive)
        return;
    auto array = (jarray) object;
    for (int i = 0; i < array->length; i++) {
        auto child = ((jobject *) array->data)[i];
        if (child)
            ((gc_mark_ptr) ((jclass) child->clazz)->markFunction)(child, mark, depth + 1);
    }
}

jobject array_clone_R_java_lang_Object(jcontext ctx, jobject self) {
    auto array = (jarray) NULL_CHECK(self);
    auto clazz = (jclass) self->clazz;
    auto componentClass = (jclass) clazz->componentClass;
    auto copy = createArray(ctx, componentClass, array->length);
    memcpy(copy->data, array->data, componentClass->primitive ? array->length * componentClass->size : array->length * sizeof(jobject));
    return (jobject) copy;
}

void array_finalize(jcontext ctx, jobject self) {
    auto array = (jarray)self;
    if (array->data) {
        auto clazz = (jclass)((jclass) array->parent.clazz)->componentClass;
        adjustHeapUsage(clazz->primitive ? -array->length * clazz->size : -(int64_t)sizeof(jobject) * array->length);
        delete[] (char *) array->data;
        array->length = 0;
        array->data = nullptr;
    }
}

void *vtable_array[]{
        (void *) M_java_lang_Object_hashCode_R_int,
        (void *) M_java_lang_Object_equals_java_lang_Object_R_boolean,
        (void *) array_clone_R_java_lang_Object,
        (void *) M_java_lang_Object_getClass_R_java_lang_Class,
        (void *) M_java_lang_Object_toString_R_java_lang_String,
        (void *) array_finalize,
        (void *) M_java_lang_Object_notify,
        (void *) M_java_lang_Object_notifyAll,
        (void *) M_java_lang_Object_wait,
        (void *) M_java_lang_Object_wait_long,
        (void *) M_java_lang_Object_wait_long_int,
};

/// Gets an array class for the provided type. Does not throw exceptions.
jclass getArrayClass(jclass componentType, int dimensions) {
    static std::mutex mutex;
    static std::map<jclass, std::vector<jclass>> arrayClasses;

    // Find non-array component type
    while (componentType->arrayDimensions > 0) {
        componentType = (jclass) componentType->componentClass;
        dimensions++;
    }

    // A small per-thread cache in front of the lock and map, every array allocation comes here
    struct CacheEntry {
        jclass componentType;
        int dimensions;
        jclass arrayClass;
    };
    static thread_local CacheEntry cache[32];
    auto &cached = cache[(((uintptr_t) componentType >> 4) + dimensions) & 31];
    if (cached.componentType == componentType && cached.dimensions == dimensions)
        return cached.arrayClass;

    std::lock_guard guard(mutex);

    auto it = arrayClasses.find(componentType);
    if (it != arrayClasses.end() && (int)it->second.size() >= dimensions) {
        cached = {componentType, dimensions, it->second[dimensions - 1]};
        return cached.arrayClass;
    }

    auto &vector = arrayClasses[componentType];
    while ((int)vector.size() < dimensions) {
        auto component = vector.empty() ? componentType : vector.back();
        auto componentNameLength = strlen((const char *) component->nativeName);
        auto name = new char[componentNameLength + 2]; // Not zero initialized, memcpy handles null terminator
        name[0] = '[';
        memcpy(name + 1, (const char *) component->nativeName, componentNameLength + 1);
        vector.emplace_back(new Class{
                .nativeName = (jref) name,
                .parentClass = (jref) &class_java_lang_Object,
                .size = (jint) sizeof(Array),
                .classVtable = (jref) vtable_array,
                .staticInitializer = (jref) clinit_array,
                .markFunction = (jref) mark_array,
                .arrayDimensions = (jint) vector.size() + 1,
                .componentClass = (jref) component,
                .access = (componentType->access & 0x7) | 0x410, // Like the JVM: final abstract, visibility of the element type
                .interfaceCount = 0, // Todo: Cloneable
                .nativeInterfaces = (jref) nullptr,
                .fieldCount = 0,
                .nativeFields = (jref) nullptr,
                .methodCount = 0,
                .nativeMethods =  (jref) nullptr,
        });
        registerClass(vector.back());
    }

    cached = {componentType, dimensions, vector[dimensions - 1]};
    return cached.arrayClass;
}

/// Creates a new multi-dimensional array. Throws exceptions.
extern "C++" jarray createMultiArray(jcontext ctx, jclass type, const std::vector<int> &dimensions) {
    nullCheck(ctx, (jobject) type);
    auto array = (jarray) gcAllocProtected(ctx, getArrayClass(type, (int) dimensions.size())); // Safe because no methods are called on it, so no safepoint
    array->length = dimensions[0];
    if (array->length == 0) // Todo: Verify
        return array;
    if (type->primitive and dimensions.size() == 1) {
        array->data = new char[type->size * array->length]{};
        adjustHeapUsage(type->size * array->length);
    } else {
        array->data = new char[sizeof(jobject) * array->length]{};
        adjustHeapUsage((int64_t) sizeof(jobject) * array->length);
        if (dimensions.size() > 1) {
            auto itemDims = std::vector<int>(dimensions.begin() + 1, dimensions.end());
            for (int i = 0; i < dimensions[0]; i++)
                ((jobject *) array->data)[i] = (jobject) createMultiArray(ctx, type, itemDims);
        }
    }
    unprotectObject((jobject)array);
    return array;
}

/// Creates a new array. Throws exceptions.
jarray createArray(jcontext ctx, jclass type, int length) {
    // Without createMultiArray's vector and protecting: nothing here reaches a safepoint after the allocation
    nullCheck(ctx, (jobject) type);
    auto array = (jarray) gcAlloc(ctx, getArrayClass(type, 1));
    array->length = length;
    if (length == 0)
        return array;
    int64_t bytes = type->primitive ? (int64_t) type->size * length : (int64_t) sizeof(jobject) * length;
    array->data = new char[bytes]{};
    adjustHeapUsage(bytes);
    return array;
}

jarray createArrayProtected(jcontext ctx, jclass type, int length) {
    return (jarray)protectObject((jobject)createArray(ctx, type, length));
}

jarray createArrayEternal(jcontext ctx, jclass type, int length) {
    return (jarray)makeEternal((jobject)createArray(ctx, type, length));
}

}
