#include <tinycthread.h>

#include <stdlib.h>
#include <process.h>
#include <sys/timeb.h>

// Standard, good-to-have defines
#ifndef NULL
#define NULL (void*)0
#endif
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

int
mtx_init(mtx_t* mtx, int type)
{
    mtx->mAlreadyLocked = FALSE;
    mtx->mRecursive     = type & mtx_recursive;
    mtx->mTimed         = type & mtx_timed;
    if (!mtx->mTimed) {
        InitializeCriticalSection(&(mtx->mHandle.cs));
    } else {
        mtx->mHandle.mut = CreateMutex(NULL, FALSE, NULL);
        if (mtx->mHandle.mut == NULL) {
            return thrd_error;
        }
    }
    return thrd_success;
}

void
mtx_destroy(mtx_t* mtx)
{
    if (!mtx->mTimed) {
        DeleteCriticalSection(&(mtx->mHandle.cs));
    } else {
        CloseHandle(mtx->mHandle.mut);
    }
}

int
mtx_lock(mtx_t* mtx)
{
    if (!mtx->mTimed) {
        EnterCriticalSection(&(mtx->mHandle.cs));
    } else {
        switch (WaitForSingleObject(mtx->mHandle.mut, INFINITE)) {
            case WAIT_OBJECT_0:
                break;
            case WAIT_ABANDONED:
            default:
                return thrd_error;
        }
    }

    if (!mtx->mRecursive) {
        while (mtx->mAlreadyLocked) {
            Sleep(1); // Simulate deadlock...
        }
        mtx->mAlreadyLocked = TRUE;
    }
    return thrd_success;
}

int
mtx_timedlock(mtx_t* mtx, const struct timespec* ts)
{
    struct timespec current_ts;
    DWORD timeoutMs;

    if (!mtx->mTimed) {
        return thrd_error;
    }

    timespec_get(&current_ts, TIME_UTC);

    if ((current_ts.tv_sec > ts->tv_sec) || ((current_ts.tv_sec == ts->tv_sec) && (current_ts.tv_nsec >= ts->tv_nsec))) {
        timeoutMs = 0;
    } else {
        timeoutMs = (DWORD)(ts->tv_sec - current_ts.tv_sec) * 1000;
        timeoutMs += (ts->tv_nsec - current_ts.tv_nsec) / 1000000;
        timeoutMs += 1;
    }

    // TODO: the timeout for WaitForSingleObject doesn't include time
    // while the computer is asleep.
    switch (WaitForSingleObject(mtx->mHandle.mut, timeoutMs)) {
        case WAIT_OBJECT_0:
            break;
        case WAIT_TIMEOUT:
            return thrd_timedout;
        case WAIT_ABANDONED:
        default:
            return thrd_error;
    }

    if (!mtx->mRecursive) {
        while (mtx->mAlreadyLocked) {
            Sleep(1); // Simulate deadlock...
        }
        mtx->mAlreadyLocked = TRUE;
    }

    return thrd_success;
}

int
mtx_trylock(mtx_t* mtx)
{
    int ret;

    if (!mtx->mTimed) {
        ret = TryEnterCriticalSection(&(mtx->mHandle.cs)) ? thrd_success : thrd_busy;
    } else {
        ret = (WaitForSingleObject(mtx->mHandle.mut, 0) == WAIT_OBJECT_0) ? thrd_success : thrd_busy;
    }

    if ((!mtx->mRecursive) && (ret == thrd_success)) {
        if (mtx->mAlreadyLocked) {
            LeaveCriticalSection(&(mtx->mHandle.cs));
            ret = thrd_busy;
        } else {
            mtx->mAlreadyLocked = TRUE;
        }
    }
    return ret;
}

int
mtx_unlock(mtx_t* mtx)
{
    mtx->mAlreadyLocked = FALSE;
    if (!mtx->mTimed) {
        LeaveCriticalSection(&(mtx->mHandle.cs));
    } else {
        if (!ReleaseMutex(mtx->mHandle.mut)) {
            return thrd_error;
        }
    }
    return thrd_success;
}

#define _CONDITION_EVENT_ONE 0
#define _CONDITION_EVENT_ALL 1

int
cnd_init(cnd_t* cond)
{
    cond->mWaitersCount = 0;

    // Init critical section
    InitializeCriticalSection(&cond->mWaitersCountLock);

    // Init events
    cond->mEvents[_CONDITION_EVENT_ONE] = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (cond->mEvents[_CONDITION_EVENT_ONE] == NULL) {
        cond->mEvents[_CONDITION_EVENT_ALL] = NULL;
        return thrd_error;
    }
    cond->mEvents[_CONDITION_EVENT_ALL] = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (cond->mEvents[_CONDITION_EVENT_ALL] == NULL) {
        CloseHandle(cond->mEvents[_CONDITION_EVENT_ONE]);
        cond->mEvents[_CONDITION_EVENT_ONE] = NULL;
        return thrd_error;
    }

    return thrd_success;
}

void
cnd_destroy(cnd_t* cond)
{
    if (cond->mEvents[_CONDITION_EVENT_ONE] != NULL) {
        CloseHandle(cond->mEvents[_CONDITION_EVENT_ONE]);
    }
    if (cond->mEvents[_CONDITION_EVENT_ALL] != NULL) {
        CloseHandle(cond->mEvents[_CONDITION_EVENT_ALL]);
    }
    DeleteCriticalSection(&cond->mWaitersCountLock);
}

int
cnd_signal(cnd_t* cond)
{
    int haveWaiters;

    // Are there any waiters?
    EnterCriticalSection(&cond->mWaitersCountLock);
    haveWaiters = (cond->mWaitersCount > 0);
    LeaveCriticalSection(&cond->mWaitersCountLock);

    // If we have any waiting threads, send them a signal
    if (haveWaiters) {
        if (SetEvent(cond->mEvents[_CONDITION_EVENT_ONE]) == 0) {
            return thrd_error;
        }
    }

    return thrd_success;
}

int
cnd_broadcast(cnd_t* cond)
{
    int haveWaiters;

    // Are there any waiters?
    EnterCriticalSection(&cond->mWaitersCountLock);
    haveWaiters = (cond->mWaitersCount > 0);
    LeaveCriticalSection(&cond->mWaitersCountLock);

    // If we have any waiting threads, send them a signal
    if (haveWaiters) {
        if (SetEvent(cond->mEvents[_CONDITION_EVENT_ALL]) == 0) {
            return thrd_error;
        }
    }

    return thrd_success;
}

static int
_cnd_timedwait_win32(cnd_t* cond, mtx_t* mtx, DWORD timeout)
{
    DWORD result;
    int lastWaiter;

    // Increment number of waiters
    EnterCriticalSection(&cond->mWaitersCountLock);
    ++cond->mWaitersCount;
    LeaveCriticalSection(&cond->mWaitersCountLock);

    // Release the mutex while waiting for the condition (will decrease
    // the number of waiters when done)...
    mtx_unlock(mtx);

    // Wait for either event to become signaled due to cnd_signal() or
    // cnd_broadcast() being called
    result = WaitForMultipleObjects(2, cond->mEvents, FALSE, timeout);
    if (result == WAIT_TIMEOUT) {
        // The mutex is locked again before the function returns, even if an
        // error occurred
        mtx_lock(mtx);
        return thrd_timedout;
    } else if (result == WAIT_FAILED) {
        // The mutex is locked again before the function returns, even if an
        // error occurred
        mtx_lock(mtx);
        return thrd_error;
    }

    // Check if we are the last waiter
    EnterCriticalSection(&cond->mWaitersCountLock);
    --cond->mWaitersCount;
    lastWaiter = (result == (WAIT_OBJECT_0 + _CONDITION_EVENT_ALL)) && (cond->mWaitersCount == 0);
    LeaveCriticalSection(&cond->mWaitersCountLock);

    // If we are the last waiter to be notified to stop waiting, reset the event
    if (lastWaiter) {
        if (ResetEvent(cond->mEvents[_CONDITION_EVENT_ALL]) == 0) {
            // The mutex is locked again before the function returns, even if an
            // error occurred
            mtx_lock(mtx);
            return thrd_error;
        }
    }

    // Re-acquire the mutex
    mtx_lock(mtx);

    return thrd_success;
}

int
cnd_wait(cnd_t* cond, mtx_t* mtx)
{
    return _cnd_timedwait_win32(cond, mtx, INFINITE);
}

int
cnd_timedwait(cnd_t* cond, mtx_t* mtx, const struct timespec* ts)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
        unsigned long long nowInMilliseconds = now.tv_sec * 1000 + now.tv_nsec / 1000000;
        unsigned long long tsInMilliseconds  = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
        DWORD delta                          = (tsInMilliseconds > nowInMilliseconds) ? (DWORD)(tsInMilliseconds - nowInMilliseconds) : 0;
        return _cnd_timedwait_win32(cond, mtx, delta);
    } else {
        return thrd_error;
    }
}

struct TinyCThreadTSSData
{
    void* value;
    tss_t key;
    struct TinyCThreadTSSData* next;
};

static tss_dtor_t _tinycthread_tss_dtors[1088] = {
    NULL,
};

static _Thread_local struct TinyCThreadTSSData* _tinycthread_tss_head = NULL;
static _Thread_local struct TinyCThreadTSSData* _tinycthread_tss_tail = NULL;

static void _tinycthread_tss_cleanup(void);

static void
_tinycthread_tss_cleanup(void)
{
    struct TinyCThreadTSSData* data;
    int iteration;
    unsigned int again = 1;
    void* value;

    for (iteration = 0; iteration < TSS_DTOR_ITERATIONS && again > 0; iteration++) {
        again = 0;
        for (data = _tinycthread_tss_head; data != NULL; data = data->next) {
            if (data->value != NULL) {
                value       = data->value;
                data->value = NULL;

                if (_tinycthread_tss_dtors[data->key] != NULL) {
                    again = 1;
                    _tinycthread_tss_dtors[data->key](value);
                }
            }
        }
    }

    while (_tinycthread_tss_head != NULL) {
        data = _tinycthread_tss_head->next;
        free(_tinycthread_tss_head);
        _tinycthread_tss_head = data;
    }
    _tinycthread_tss_head = NULL;
    _tinycthread_tss_tail = NULL;
}

static void NTAPI
_tinycthread_tss_callback(PVOID h, DWORD dwReason, PVOID pv)
{
    (void)h;
    (void)pv;

    if (_tinycthread_tss_head != NULL && (dwReason == DLL_THREAD_DETACH || dwReason == DLL_PROCESS_DETACH)) {
        _tinycthread_tss_cleanup();
    }
}

#if defined(_MSC_VER)
#ifdef _M_X64
#pragma const_seg(".CRT$XLB")
#else
#pragma data_seg(".CRT$XLB")
#endif
PIMAGE_TLS_CALLBACK p_thread_callback = _tinycthread_tss_callback;
#ifdef _M_X64
#pragma data_seg()
#else
#pragma const_seg()
#endif
#else
PIMAGE_TLS_CALLBACK p_thread_callback __attribute__((section(".CRT$XLB"))) = _tinycthread_tss_callback;
#endif

/// @brief Information to pass to the new thread (what to run).
typedef struct
{
    /// @brief Pointer to the function to be executed.
    thrd_start_t mFunction;
    /// @brief Function argument for the thread function.
    void* mArg;
} _thread_start_info;

/// @brief Thread wrapper function.
static DWORD WINAPI
_thrd_wrapper_function(LPVOID aArg)
{
    thrd_start_t fun;
    void* arg;
    int res;

    // Get thread startup information
    _thread_start_info* ti = (_thread_start_info*)aArg;
    fun                    = ti->mFunction;
    arg                    = ti->mArg;

    // The thread is responsible for freeing the startup information
    free((void*)ti);

    // Call the actual client thread function
    res = fun(arg);

    if (_tinycthread_tss_head != NULL) {
        _tinycthread_tss_cleanup();
    }

    return (DWORD)res;
}

int
thrd_create(thrd_t* thr, thrd_start_t func, void* arg)
{
    // Fill out the thread startup information (passed to the thread wrapper,
    // which will eventually free it)
    _thread_start_info* ti = (_thread_start_info*)malloc(sizeof(_thread_start_info));
    if (ti == NULL) {
        return thrd_nomem;
    }
    ti->mFunction = func;
    ti->mArg      = arg;

    // Create the thread
    *thr = CreateThread(NULL, 0, _thrd_wrapper_function, (LPVOID)ti, 0, NULL);

    // Did we fail to create the thread?
    if (!*thr) {
        free(ti);
        return thrd_error;
    }

    return thrd_success;
}

thrd_t
thrd_current(void)
{
    return GetCurrentThread();
}

int
thrd_detach(thrd_t thr)
{
    // https://stackoverflow.com/questions/12744324/how-to-detach-a-thread-on-windows-c#answer-12746081
    return CloseHandle(thr) != 0 ? thrd_success : thrd_error;
}

int
thrd_equal(thrd_t thr0, thrd_t thr1)
{
    return GetThreadId(thr0) == GetThreadId(thr1);
}

void
thrd_exit(int res)
{
    if (_tinycthread_tss_head != NULL) {
        _tinycthread_tss_cleanup();
    }

    ExitThread((DWORD)res);
}

int
thrd_join(thrd_t thr, int* res)
{
    DWORD dwRes;

    if (WaitForSingleObject(thr, INFINITE) == WAIT_FAILED) {
        return thrd_error;
    }
    if (res != NULL) {
        if (GetExitCodeThread(thr, &dwRes) != 0) {
            *res = (int)dwRes;
        } else {
            return thrd_error;
        }
    }
    CloseHandle(thr);
    return thrd_success;
}

int
thrd_sleep(const struct timespec* duration, struct timespec* remaining)
{
    struct timespec start;
    DWORD t;

    timespec_get(&start, TIME_UTC);

    t = SleepEx((DWORD)(duration->tv_sec * 1000 + duration->tv_nsec / 1000000 + (((duration->tv_nsec % 1000000) == 0) ? 0 : 1)), TRUE);

    if (t == 0) {
        return 0;
    } else {
        if (remaining != NULL) {
            timespec_get(remaining, TIME_UTC);
            remaining->tv_sec -= start.tv_sec;
            remaining->tv_nsec -= start.tv_nsec;
            if (remaining->tv_nsec < 0) {
                remaining->tv_nsec += 1000000000;
                remaining->tv_sec -= 1;
            }
        }

        return (t == WAIT_IO_COMPLETION) ? -1 : -2;
    }
}

void
thrd_yield(void)
{
    Sleep(0);
}

int
tss_create(tss_t* key, tss_dtor_t dtor)
{
    *key = TlsAlloc();
    if (*key == TLS_OUT_OF_INDEXES) {
        return thrd_error;
    }
    _tinycthread_tss_dtors[*key] = dtor;
    return thrd_success;
}

void
tss_delete(tss_t key)
{
    struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*)TlsGetValue(key);
    struct TinyCThreadTSSData* prev = NULL;
    if (data != NULL) {
        if (data == _tinycthread_tss_head) {
            _tinycthread_tss_head = data->next;
        } else {
            prev = _tinycthread_tss_head;
            if (prev != NULL) {
                while (prev->next != data) {
                    prev = prev->next;
                }
            }
        }

        if (data == _tinycthread_tss_tail) {
            _tinycthread_tss_tail = prev;
        }

        free(data);
    }
    _tinycthread_tss_dtors[key] = NULL;
    TlsFree(key);
}

void*
tss_get(tss_t key)
{
    struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*)TlsGetValue(key);
    if (data == NULL) {
        return NULL;
    }
    return data->value;
}

int
tss_set(tss_t key, void* val)
{
    struct TinyCThreadTSSData* data = (struct TinyCThreadTSSData*)TlsGetValue(key);
    if (data == NULL) {
        data = (struct TinyCThreadTSSData*)malloc(sizeof(struct TinyCThreadTSSData));
        if (data == NULL) {
            return thrd_error;
        }

        data->value = NULL;
        data->key   = key;
        data->next  = NULL;

        if (_tinycthread_tss_tail != NULL) {
            _tinycthread_tss_tail->next = data;
        } else {
            _tinycthread_tss_tail = data;
        }

        if (_tinycthread_tss_head == NULL) {
            _tinycthread_tss_head = data;
        }

        if (!TlsSetValue(key, data)) {
            free(data);
            return thrd_error;
        }
    }
    data->value = val;
    return thrd_success;
}

#if defined(_TTHREAD_EMULATE_TIMESPEC_GET_)
int
_tthread_timespec_get(struct timespec* ts, int base)
{
    struct _timeb tb;

    if (base != TIME_UTC) {
        return 0;
    }

    _ftime_s(&tb);
    ts->tv_sec  = (time_t)tb.time;
    ts->tv_nsec = 1000000L * (long)tb.millitm;

    return base;
}
#endif // _TTHREAD_EMULATE_TIMESPEC_GET_

void
call_once(once_flag* flag, void (*func)(void))
{
    // The idea here is that we use a spin lock (via the
    // InterlockedCompareExchange function) to restrict access to the
    // critical section until we have initialized it, then we use the
    // critical section to block until the callback has completed
    // execution.
    while (flag->status < 3) {
        switch (flag->status) {
            case 0:
                if (InterlockedCompareExchange(&(flag->status), 1, 0) == 0) {
                    InitializeCriticalSection(&(flag->lock));
                    EnterCriticalSection(&(flag->lock));
                    flag->status = 2;
                    func();
                    flag->status = 3;
                    LeaveCriticalSection(&(flag->lock));
                    return;
                }
                break;
            case 1:
                break;
            case 2:
                EnterCriticalSection(&(flag->lock));
                LeaveCriticalSection(&(flag->lock));
                break;
        }
    }
}
