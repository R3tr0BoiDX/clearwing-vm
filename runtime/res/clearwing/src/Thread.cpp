#include "Clearwing.h"

#include "java/lang/Runnable.h"
#include "java/lang/Thread.h"
#include "java/lang/StackTraceElement.h"
#include "java/lang/String.h"
#include "java/lang/Object.h"
#include "java/lang/Throwable.h"

#include <thread>

extern "C" {

void threadEntrypoint(jcontext ctx, jthread thread) {
    attachThread(ctx);

    try {
        jtype frame[1];
        FrameInfo frameInfo { "java/lang/Thread:threadEntrypoint", 1 };
        auto frameRef = pushStackFrame(ctx, &frameInfo, frame);

        thread->F_started = true; // For the main thread, Thread.start sets these for the others
        thread->F_alive = true;

        tryCatch(frameRef, [&]{
            if (thread->F_entrypoint) {
                frame[0].o = (jobject) createArray(ctx, &class_java_lang_String, 0);
                ((main_ptr) thread->F_entrypoint)(ctx, frame[0].o);
            }
            else
                invokeVirtual<func_java_lang_Thread_run, VTABLE_java_lang_Thread_run>(ctx, (jobject)thread);
        }, &class_java_lang_Throwable, [&](jobject ex){
            // Todo: Default handlers
            tryCatch(frameRef, [&] {
                auto throwable = (java_lang_Throwable *)ex;
                if (throwable->F_message)
                    printf("Uncaught Exception: %s\n", stringToNative(ctx, (jstring)throwable->F_message));
                INVOKE_VIRTUAL(java_lang_Throwable_printStackTrace, ex);
            }, &class_java_lang_Throwable, [&](jobject) {});
        });

        popStackFrame(ctx);
    } catch (ExitException &) { }

    // Wake threads in join(), under the monitor so one that just saw this thread alive can't miss it
    ctx->suspended = true;
    auto monitor = getMonitor((jobject) thread);
    monitor->lock.lock();
    thread->F_alive = false;
    {
        std::lock_guard<std::mutex> lock(monitor->conditionMutex);
        monitor->condition.notify_all();
    }
    monitor->lock.unlock();
    ctx->dead = true;

    unprotectObject((jobject)thread);
}

jobject SM_java_lang_Thread_currentThread_R_java_lang_Thread(jcontext ctx) {
    return (jobject) ctx->thread;
}

void SM_java_lang_Thread_sleepImpl_long_int(jcontext ctx, jlong millis, jint nanos) {
    auto end = std::chrono::system_clock::now() + std::chrono::milliseconds(millis) + std::chrono::nanoseconds(nanos);
    interruptedCheck(ctx);
    // Waits on the thread's own condition without taking its monitor, so an interrupt can't leave it locked
    // (interrupt and VM shutdown notify the condition of blockedBy)
    auto thread = (jobject) ctx->thread;
    auto monitor = getMonitor(thread);
    while (std::chrono::system_clock::now() < end) {
        ctx->lock.lock();
        ctx->blockedBy = thread;
        ctx->lock.unlock();
        ctx->suspended = true;
        {
            std::unique_lock<std::mutex> lock(monitor->conditionMutex);
            if (!ctx->thread->F_interrupted)
                monitor->condition.wait_until(lock, end);
        }
        ctx->suspended = false;
        ctx->lock.lock();
        ctx->blockedBy = nullptr;
        ctx->lock.unlock();
        SAFEPOINT();
        interruptedCheck(ctx);
    }
}

void M_java_lang_Thread_start(jcontext ctx, jobject self) {
    auto newContext = createContext();
    auto thread = (jthread) self;
    protectObject((jobject)thread);
    thread->F_nativeContext = (intptr_t) newContext;
    newContext->thread = thread;
    // Alive from start() on, like the JDK, or a join() right after start() could return before the thread ran
    thread->F_started = true;
    thread->F_alive = true;
    newContext->nativeThread = new std::thread;
    try {
        *newContext->nativeThread = std::thread(threadEntrypoint, newContext, thread);
    } catch (std::system_error &) {
        thread->F_alive = false;
        throwRuntimeException(ctx, "Unable to create a thread");
    }
}

void M_java_lang_Thread_interrupt(jcontext ctx, jobject self) {
    auto threadCtx = (jcontext) ((jthread) NULL_CHECK(self))->F_nativeContext;
    threadCtx->lock.lock();
    threadCtx->thread->F_interrupted = true;
    if (threadCtx->blockedBy) {
        auto monitor = getMonitor((jobject) threadCtx->blockedBy);
        std::lock_guard<std::mutex> lock(monitor->conditionMutex); // See Object.wait
        monitor->condition.notify_all();
    }
    threadCtx->lock.unlock();
}

void M_java_lang_Thread_finalize(jcontext ctx, jobject selfObj) {
    auto self = (jthread) selfObj;
    auto context = (jcontext) self->F_nativeContext;
    context->nativeThread->join();
    delete context->nativeThread;
    destroyContext(context);
}

jobject M_java_lang_Thread_getStackTrace_R_Array1_java_lang_StackTraceElement(jcontext ctx, jobject self) {
    jtype frame[4];
    FrameInfo frameInfo { "java/lang/Thread:getStackTrace", 1 };
    auto frameRef = pushStackFrame(ctx, &frameInfo, frame);

    // Another thread's frames can't be read safely while it runs (frame infos may be stack allocated and
    // already gone), so like a VM that omits frames, other threads get an empty trace
    auto target = (jcontext) ((jthread) NULL_CHECK(self))->F_nativeContext;
    int depth = target == ctx ? target->stackDepth : 0;
    jarray trace = createArray(ctx, &class_java_lang_StackTraceElement, depth);
    frame[0].o = (jobject)trace;

    for (int i = 0; i < depth; i++) {
        auto stackFrame = &target->frames[i];
        std::string_view method = stackFrame->info && stackFrame->info->method ? stackFrame->info->method : "";
        auto separator = method.find(':');
        frame[1].o = (jobject)stringFromNative(ctx, separator != std::string_view::npos ? method.substr(0, separator) : "Unknown");
        frame[2].o = (jobject)stringFromNative(ctx, separator != std::string_view::npos ? method.substr(separator + 1) : "unknown");
        frame[3].o = frame[1].o;
        ((jobject *)trace->data)[depth - 1 - i] = constructObject<&class_java_lang_StackTraceElement, init_java_lang_StackTraceElement_java_lang_String_java_lang_String_java_lang_String_int>(ctx, frame[1].o, frame[2].o, frame[3].o, stackFrame->lineNumber);
    }

    popStackFrame(ctx);
    return frame[0].o;
}

// LockSupport: Thread.parkPermit is the permit, parking waits on the thread's own condition like sleep
void SM_java_util_concurrent_locks_LockSupport_park0_boolean_long(jcontext ctx, jbool absolute, jlong time) {
    auto thread = ctx->thread;
    auto monitor = getMonitor((jobject) thread);
    ctx->lock.lock();
    ctx->blockedBy = (jobject) thread;
    ctx->lock.unlock();
    ctx->suspended = true;
    {
        std::unique_lock<std::mutex> lock(monitor->conditionMutex);
        if (!thread->F_parkPermit && !thread->F_interrupted) { // Thread.interrupt notifies under the same mutex
            if (absolute)
                monitor->condition.wait_until(lock, std::chrono::system_clock::time_point(std::chrono::milliseconds(time)));
            else if (time > 0)
                monitor->condition.wait_for(lock, std::chrono::nanoseconds(time));
            else
                monitor->condition.wait(lock);
        }
        thread->F_parkPermit = false;
    }
    ctx->suspended = false;
    ctx->lock.lock();
    ctx->blockedBy = nullptr;
    ctx->lock.unlock();
    SAFEPOINT();
}

void SM_java_util_concurrent_locks_LockSupport_unpark0_java_lang_Thread(jcontext ctx, jobject threadObj) {
    auto monitor = getMonitor(threadObj);
    std::lock_guard<std::mutex> lock(monitor->conditionMutex);
    ((jthread) threadObj)->F_parkPermit = true;
    monitor->condition.notify_all();
}

}
