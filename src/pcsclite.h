#ifndef PCSCLITE_H
#define PCSCLITE_H

#include <napi.h>
#include <uv.h>
#include <string>
#ifdef __APPLE__
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#else
#include <winscard.h>
#endif

class PCSCLite : public Napi::ObjectWrap<PCSCLite> {

    public:

        struct AsyncResult {
            LONG result;
            LPSTR readers_name;
            DWORD readers_name_length;
            bool do_exit;
            std::string err_msg;
        };

        static Napi::Object Init(Napi::Env env, Napi::Object exports);

        PCSCLite(const Napi::CallbackInfo& info);
        ~PCSCLite();

    private:

        Napi::Value Start(const Napi::CallbackInfo& info);
        Napi::Value Close(const Napi::CallbackInfo& info);

        static void HandlerFunction(void* arg);
        void HandleAsyncResult(Napi::Env env, Napi::Function jsCallback, AsyncResult* ar);

        LONG get_card_readers(AsyncResult* async_result);

    private:

        SCARDCONTEXT m_card_context;
        SCARD_READERSTATE m_card_reader_state;
        uv_thread_t m_status_thread;
        uv_mutex_t m_mutex;
        uv_cond_t m_cond;
        bool m_pnp;
        int m_state;
        bool m_thread_running;

        Napi::ThreadSafeFunction m_tsfn;
        LONG m_pending_err_result;
        std::string m_pending_err_msg;
};

#endif /* PCSCLITE_H */
