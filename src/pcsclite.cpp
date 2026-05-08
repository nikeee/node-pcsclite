#include "pcsclite.h"
#include "common.h"
#include <cassert>
#include <cstring>

Napi::Object PCSCLite::Init(Napi::Env env, Napi::Object exports) {

    Napi::Function func = DefineClass(env, "PCSCLite", {
        InstanceMethod("start", &PCSCLite::Start),
        InstanceMethod("close", &PCSCLite::Close),
    });

    exports.Set("PCSCLite", func);
    return exports;
}

PCSCLite::PCSCLite(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<PCSCLite>(info),
      m_card_context(0),
      m_card_reader_state(),
      m_status_thread(),
      m_pnp(true),
      m_state(0),
      m_thread_running(false),
      m_pending_err_result(SCARD_S_SUCCESS) {

    Napi::Env env = info.Env();

    assert(uv_mutex_init(&m_mutex) == 0);
    assert(uv_cond_init(&m_cond) == 0);

    // TODO: consider removing this Windows workaround that should not be needed anymore
#ifdef _WIN32
    HKEY hKey;
    DWORD startStatus, datacb = sizeof(DWORD);
    LONG _res;
    _res = RegOpenKeyEx(HKEY_LOCAL_MACHINE, "System\\CurrentControlSet\\Services\\SCardSvr", 0, KEY_READ, &hKey);
    if (_res != ERROR_SUCCESS) {
        printf("Reg Open Key exited with %d\n", _res);
        goto postServiceCheck;
    }
    _res = RegQueryValueEx(hKey, "Start", NULL, NULL, (LPBYTE)&startStatus, &datacb);
    if (_res != ERROR_SUCCESS) {
        printf("Reg Query Value exited with %d\n", _res);
        goto postServiceCheck;
    }
    if (startStatus != 2) {
        SHELLEXECUTEINFO seInfo = {0};
        seInfo.cbSize = sizeof(SHELLEXECUTEINFO);
        seInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
        seInfo.hwnd = NULL;
        seInfo.lpVerb = "runas";
        seInfo.lpFile = "sc.exe";
        seInfo.lpParameters = "config SCardSvr start=auto";
        seInfo.lpDirectory = NULL;
        seInfo.nShow = SW_SHOWNORMAL;
        seInfo.hInstApp = NULL;
        if (!ShellExecuteEx(&seInfo)) {
            printf("Shell Execute failed with %d\n", GetLastError());
            goto postServiceCheck;
        }
        WaitForSingleObject(seInfo.hProcess, INFINITE);
        CloseHandle(seInfo.hProcess);
    }
postServiceCheck:
#endif // _WIN32

    LONG result;
    // TODO: consider removing this do-while Windows workaround that should not be needed anymore
    do {
        // TODO: make dwScope (now hard-coded to SCARD_SCOPE_SYSTEM) customisable
        result = SCardEstablishContext(SCARD_SCOPE_SYSTEM,
                                            NULL,
                                            NULL,
                                            &m_card_context);
    } while(result == SCARD_E_NO_SERVICE || result == SCARD_E_SERVICE_STOPPED);
    if (result != SCARD_S_SUCCESS) {
        Napi::Error::New(env, error_msg("SCardEstablishContext", result)).ThrowAsJavaScriptException();
        return;
    }

    m_card_reader_state.szReader = "\\\\?PnP?\\Notification";
    m_card_reader_state.dwCurrentState = SCARD_STATE_UNAWARE;
    result = SCardGetStatusChange(m_card_context,
                                  0,
                                  &m_card_reader_state,
                                  1);

    if ((result != SCARD_S_SUCCESS) && (result != (LONG)SCARD_E_TIMEOUT)) {
        Napi::Error::New(env, error_msg("SCardGetStatusChange", result)).ThrowAsJavaScriptException();
        return;
    }

    m_pnp = !(m_card_reader_state.dwEventState & SCARD_STATE_UNKNOWN);
}

PCSCLite::~PCSCLite() {

    if (m_thread_running) {
        SCardCancel(m_card_context);
        assert(uv_thread_join(&m_status_thread) == 0);
        m_thread_running = false;
    }

    if (m_card_context) {
        SCardReleaseContext(m_card_context);
    }

    uv_cond_destroy(&m_cond);
    uv_mutex_destroy(&m_mutex);
}

Napi::Value PCSCLite::Start(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();
    Napi::Function cb = info[0].As<Napi::Function>();

    m_tsfn = Napi::ThreadSafeFunction::New(
        env,
        cb,
        "PCSCLite_Start",
        0,    // unlimited queue
        1     // initial thread count
    );

    int ret = uv_thread_create(&m_status_thread, HandlerFunction, this);
    assert(ret == 0);
    m_thread_running = true;

    return env.Undefined();
}

Napi::Value PCSCLite::Close(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();
    LONG result = SCARD_S_SUCCESS;

    if (m_pnp) {
        if (m_thread_running) {
            uv_mutex_lock(&m_mutex);
            if (m_state == 0) {
                int ret;
                int times = 0;
                m_state = 1;
                do {
                    result = SCardCancel(m_card_context);
                    ret = uv_cond_timedwait(&m_cond, &m_mutex, 10000000);
                } while ((ret != 0) && (++times < 5));
            }
            uv_mutex_unlock(&m_mutex);
        }
    } else {
        m_state = 1;
    }

    if (m_thread_running) {
        assert(uv_thread_join(&m_status_thread) == 0);
        m_thread_running = false;
    }

    return Napi::Number::New(env, result);
}

void PCSCLite::HandlerFunction(void* arg) {

    PCSCLite* p = static_cast<PCSCLite*>(arg);

    while (!p->m_state) {
        AsyncResult* ar = new AsyncResult();
        ar->readers_name = NULL;
        ar->readers_name_length = 0;
        ar->do_exit = false;

        /* Get card readers */
        LONG result = p->get_card_readers(ar);
        if (result == (LONG)SCARD_E_NO_READERS_AVAILABLE) {
            result = SCARD_S_SUCCESS;
        }

        ar->result = result;
        if (result != SCARD_S_SUCCESS) {
            ar->err_msg = error_msg("SCardListReaders", result);
        }

        /* Notify the nodejs thread */
        p->m_tsfn.NonBlockingCall(ar,
            [p](Napi::Env env, Napi::Function jsCallback, AsyncResult* data) {
                p->HandleAsyncResult(env, jsCallback, data);
            });

        if (result == SCARD_S_SUCCESS) {
            if (p->m_pnp) {
                /* Set current status */
                p->m_card_reader_state.dwCurrentState =
                    p->m_card_reader_state.dwEventState;
                /* Start checking for status change */
                result = SCardGetStatusChange(p->m_card_context,
                                              INFINITE,
                                              &p->m_card_reader_state,
                                              1);

                uv_mutex_lock(&p->m_mutex);
                if (p->m_state) {
                    uv_cond_signal(&p->m_cond);
                }

                if (result != SCARD_S_SUCCESS) {
                    p->m_state = 2;
                    p->m_pending_err_result = result;
                    p->m_pending_err_msg = error_msg("SCardGetStatusChange", result);
                }

                uv_mutex_unlock(&p->m_mutex);
            } else {
                /*  If PnP is not supported, just wait for 1 second */
                Sleep(1000);
            }
        } else {
            /* Error on last card access, stop monitoring */
            p->m_state = 2;
        }
    }

    /* Send final exit notification */
    AsyncResult* exit_ar = new AsyncResult();
    exit_ar->readers_name = NULL;
    exit_ar->readers_name_length = 0;
    exit_ar->do_exit = true;
    exit_ar->result = p->m_pending_err_result;
    exit_ar->err_msg = p->m_pending_err_msg;

    p->m_tsfn.NonBlockingCall(exit_ar,
        [p](Napi::Env env, Napi::Function jsCallback, AsyncResult* data) {
            p->HandleAsyncResult(env, jsCallback, data);
        });

    p->m_tsfn.Release();
}

void PCSCLite::HandleAsyncResult(Napi::Env env, Napi::Function jsCallback, AsyncResult* ar) {

    if (env != nullptr && !jsCallback.IsEmpty()) {
        if (m_state == 1) {
            // Swallow events: Listening thread was cancelled by user.
        } else if ((ar->result == SCARD_S_SUCCESS) ||
                   (ar->result == (LONG)SCARD_E_NO_READERS_AVAILABLE)) {
            const char* data = ar->readers_name ? ar->readers_name : "";
            jsCallback.Call({
                env.Undefined(),
                Napi::Buffer<char>::Copy(env, data, ar->readers_name_length)
            });
        } else {
            jsCallback.Call({
                Napi::Error::New(env, ar->err_msg).Value()
            });
        }
    }

#ifdef SCARD_AUTOALLOCATE
    if (ar->readers_name) {
        SCardFreeMemory(m_card_context, ar->readers_name);
    }
#else
    delete[] ar->readers_name;
#endif
    delete ar;
}

LONG PCSCLite::get_card_readers(AsyncResult* async_result) {

    DWORD readers_name_length;
    LPTSTR readers_name;

    LONG result = SCARD_S_SUCCESS;

    /* Reset the readers_name in the baton */
    async_result->readers_name = NULL;
    async_result->readers_name_length = 0;

#ifdef SCARD_AUTOALLOCATE
    readers_name_length = SCARD_AUTOALLOCATE;
    result = SCardListReaders(m_card_context,
                              NULL,
                              (LPTSTR)&readers_name,
                              &readers_name_length);
#else
    /* Find out ReaderNameLength */
    result = SCardListReaders(m_card_context,
                              NULL,
                              NULL,
                              &readers_name_length);
    if (result != SCARD_S_SUCCESS) {
        return result;
    }

    /*
     * Allocate Memory for ReaderName and retrieve all readers in the terminal
     */
    readers_name = new char[readers_name_length];
    result = SCardListReaders(m_card_context,
                              NULL,
                              readers_name,
                              &readers_name_length);
#endif

    if (result != SCARD_S_SUCCESS) {
#ifndef SCARD_AUTOALLOCATE
        delete[] readers_name;
#endif
        readers_name = NULL;
        readers_name_length = 0;
#ifndef SCARD_AUTOALLOCATE
        /* Retry in case of insufficient buffer error */
        if (result == (LONG)SCARD_E_INSUFFICIENT_BUFFER) {
            result = get_card_readers(async_result);
        }
#endif
        if (result == SCARD_E_NO_SERVICE || result == SCARD_E_SERVICE_STOPPED) {
            SCardReleaseContext(m_card_context);
            SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &m_card_context);
            result = get_card_readers(async_result);
        }
    } else {
        /* Store the readers_name in the baton */
        async_result->readers_name = readers_name;
        async_result->readers_name_length = readers_name_length;
    }

    return result;
}
