#ifndef CARDREADER_H
#define CARDREADER_H

#include <napi.h>
#include <uv.h>
#include <string>
#ifdef __APPLE__
#include <PCSC/winscard.h>
#include <PCSC/wintypes.h>
#else
#include <winscard.h>
#endif

#ifdef _WIN32
#define MAX_ATR_SIZE 33
#endif
#ifdef WIN32
#define IOCTL_CCID_ESCAPE (0x42000000 + 3500)
#else
#define IOCTL_CCID_ESCAPE (0x42000000 + 1)
#endif

class ConnectWorker;
class DisconnectWorker;
class TransmitWorker;
class ControlWorker;

class CardReader : public Napi::ObjectWrap<CardReader> {

    public:

        struct AsyncResult {
            LONG result;
            DWORD status;
            BYTE atr[MAX_ATR_SIZE];
            DWORD atrlen;
            bool do_exit;
        };

        static Napi::Object Init(Napi::Env env, Napi::Object exports);

        CardReader(const Napi::CallbackInfo& info);
        ~CardReader();

        const SCARDHANDLE& GetHandler() const { return m_card_handle; }

    private:

        friend class ConnectWorker;
        friend class DisconnectWorker;
        friend class TransmitWorker;
        friend class ControlWorker;

        Napi::Value GetStatus(const Napi::CallbackInfo& info);
        Napi::Value Connect(const Napi::CallbackInfo& info);
        Napi::Value Disconnect(const Napi::CallbackInfo& info);
        Napi::Value Transmit(const Napi::CallbackInfo& info);
        Napi::Value Control(const Napi::CallbackInfo& info);
        Napi::Value Close(const Napi::CallbackInfo& info);

        static void HandlerFunction(void* arg);
        void HandleAsyncResult(Napi::Env env, Napi::Function jsCallback, AsyncResult* ar);

    private:

        SCARDCONTEXT m_card_context;
        SCARDCONTEXT m_status_card_context;
        SCARDHANDLE m_card_handle;
        std::string m_name;
        uv_thread_t m_status_thread;
        uv_mutex_t m_mutex;
        uv_cond_t m_cond;
        int m_state;
        bool m_thread_running;

        Napi::ThreadSafeFunction m_tsfn;
};

#endif /* CARDREADER_H */
