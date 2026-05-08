#include "cardreader.h"
#include "common.h"
#include <cassert>
#include <cstring>
#include <vector>

class ConnectWorker : public Napi::AsyncWorker {
public:
    ConnectWorker(Napi::Function& callback, CardReader* reader,
                  DWORD share_mode, DWORD pref_protocol)
        : Napi::AsyncWorker(callback, "PCSCLite:Connect"),
          m_reader(reader),
          m_share_mode(share_mode),
          m_pref_protocol(pref_protocol),
          m_result(SCARD_S_SUCCESS),
          m_card_protocol(0) {}

    void Execute() override {
        LONG result = SCARD_S_SUCCESS;
        uv_mutex_lock(&m_reader->m_mutex);
        if (!m_reader->m_card_context) {
            result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL,
                                           &m_reader->m_card_context);
        }
        if (result == SCARD_S_SUCCESS) {
            result = SCardConnect(m_reader->m_card_context,
                                  m_reader->m_name.c_str(),
                                  m_share_mode,
                                  m_pref_protocol,
                                  &m_reader->m_card_handle,
                                  &m_card_protocol);
        }
        uv_mutex_unlock(&m_reader->m_mutex);
        m_result = result;
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (m_result) {
            Callback().Call({
                Napi::Error::New(env, error_msg("SCardConnect", m_result)).Value()
            });
        } else {
            m_reader->Value().Set("connected", Napi::Boolean::New(env, true));
            Callback().Call({
                env.Null(),
                Napi::Number::New(env, m_card_protocol)
            });
        }
    }

private:
    CardReader* m_reader;
    DWORD m_share_mode;
    DWORD m_pref_protocol;
    LONG m_result;
    DWORD m_card_protocol;
};

class DisconnectWorker : public Napi::AsyncWorker {
public:
    DisconnectWorker(Napi::Function& callback, CardReader* reader, DWORD disposition)
        : Napi::AsyncWorker(callback, "PCSCLite:Disconnect"),
          m_reader(reader),
          m_disposition(disposition),
          m_result(SCARD_S_SUCCESS) {}

    void Execute() override {
        LONG result = SCARD_S_SUCCESS;
        uv_mutex_lock(&m_reader->m_mutex);
        if (m_reader->m_card_handle) {
            result = SCardDisconnect(m_reader->m_card_handle, m_disposition);
            if (result == SCARD_S_SUCCESS) {
                m_reader->m_card_handle = 0;
            }
        }
        uv_mutex_unlock(&m_reader->m_mutex);
        m_result = result;
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (m_result) {
            Callback().Call({
                Napi::Error::New(env, error_msg("SCardDisconnect", m_result)).Value()
            });
        } else {
            m_reader->Value().Set("connected", Napi::Boolean::New(env, false));
            Callback().Call({ env.Null() });
        }
    }

private:
    CardReader* m_reader;
    DWORD m_disposition;
    LONG m_result;
};

class TransmitWorker : public Napi::AsyncWorker {
public:
    TransmitWorker(Napi::Function& callback, CardReader* reader,
                   DWORD card_protocol,
                   std::vector<uint8_t> in_data,
                   DWORD out_len)
        : Napi::AsyncWorker(callback, "PCSCLite:Transmit"),
          m_reader(reader),
          m_card_protocol(card_protocol),
          m_in_data(std::move(in_data)),
          m_out_data(out_len),
          m_out_len(out_len),
          m_result(SCARD_E_INVALID_HANDLE) {}

    void Execute() override {
        LONG result = SCARD_E_INVALID_HANDLE;
        uv_mutex_lock(&m_reader->m_mutex);
        // Under windows, SCARD_IO_REQUEST param must be NULL.
        // Else error RPC_X_BAD_STUB_DATA / 0x06F7 on each call.
        if (m_reader->m_card_handle) {
            SCARD_IO_REQUEST send_pci = { m_card_protocol, sizeof(SCARD_IO_REQUEST) };
            result = SCardTransmit(m_reader->m_card_handle,
                                   &send_pci,
                                   m_in_data.data(),
                                   static_cast<DWORD>(m_in_data.size()),
                                   NULL,
                                   m_out_data.data(),
                                   &m_out_len);
        }
        uv_mutex_unlock(&m_reader->m_mutex);
        m_result = result;
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (m_result) {
            Callback().Call({
                Napi::Error::New(env, error_msg("SCardTransmit", m_result)).Value()
            });
        } else {
            Callback().Call({
                env.Null(),
                Napi::Buffer<uint8_t>::Copy(env, m_out_data.data(), m_out_len)
            });
        }
    }

private:
    CardReader* m_reader;
    DWORD m_card_protocol;
    std::vector<uint8_t> m_in_data;
    std::vector<uint8_t> m_out_data;
    DWORD m_out_len;
    LONG m_result;
};

class ControlWorker : public Napi::AsyncWorker {
public:
    ControlWorker(Napi::Function& callback, CardReader* reader,
                  DWORD control_code,
                  Napi::Buffer<uint8_t> in_buf,
                  Napi::Buffer<uint8_t> out_buf)
        : Napi::AsyncWorker(callback, "PCSCLite:Control"),
          m_reader(reader),
          m_control_code(control_code),
          m_in_data(in_buf.Data()),
          m_in_len(static_cast<DWORD>(in_buf.Length())),
          m_out_data(out_buf.Data()),
          m_out_len(static_cast<DWORD>(out_buf.Length())),
          m_actual_len(0),
          m_result(SCARD_E_INVALID_HANDLE),
          m_in_ref(Napi::Persistent(in_buf.As<Napi::Object>())),
          m_out_ref(Napi::Persistent(out_buf.As<Napi::Object>())) {}

    void Execute() override {
        LONG result = SCARD_E_INVALID_HANDLE;
        uv_mutex_lock(&m_reader->m_mutex);
        if (m_reader->m_card_handle) {
            result = SCardControl(m_reader->m_card_handle,
                                  m_control_code,
                                  m_in_data,
                                  m_in_len,
                                  m_out_data,
                                  m_out_len,
                                  &m_actual_len);
        }
        uv_mutex_unlock(&m_reader->m_mutex);
        m_result = result;
    }

    void OnOK() override {
        Napi::Env env = Env();
        if (m_result) {
            Callback().Call({
                Napi::Error::New(env, error_msg("SCardControl", m_result)).Value()
            });
        } else {
            Callback().Call({
                env.Null(),
                Napi::Number::New(env, m_actual_len)
            });
        }
    }

private:
    CardReader* m_reader;
    DWORD m_control_code;
    const uint8_t* m_in_data;
    DWORD m_in_len;
    uint8_t* m_out_data;
    DWORD m_out_len;
    DWORD m_actual_len;
    LONG m_result;
    Napi::ObjectReference m_in_ref;
    Napi::ObjectReference m_out_ref;
};

Napi::Object CardReader::Init(Napi::Env env, Napi::Object exports) {

    Napi::Function func = DefineClass(env, "CardReader", {
        InstanceMethod("get_status", &CardReader::GetStatus),
        InstanceMethod("_connect", &CardReader::Connect),
        InstanceMethod("_disconnect", &CardReader::Disconnect),
        InstanceMethod("_transmit", &CardReader::Transmit),
        InstanceMethod("_control", &CardReader::Control),
        InstanceMethod("close", &CardReader::Close),

        // PCSCLite constants
        // Share Mode
        InstanceValue("SCARD_SHARE_SHARED", Napi::Number::New(env, SCARD_SHARE_SHARED)),
        InstanceValue("SCARD_SHARE_EXCLUSIVE", Napi::Number::New(env, SCARD_SHARE_EXCLUSIVE)),
        InstanceValue("SCARD_SHARE_DIRECT", Napi::Number::New(env, SCARD_SHARE_DIRECT)),

        // Control Code
        InstanceValue("IOCTL_CCID_ESCAPE", Napi::Number::New(env, IOCTL_CCID_ESCAPE)),

        // Protocol
        InstanceValue("SCARD_PROTOCOL_T0", Napi::Number::New(env, SCARD_PROTOCOL_T0)),
        InstanceValue("SCARD_PROTOCOL_T1", Napi::Number::New(env, SCARD_PROTOCOL_T1)),
        InstanceValue("SCARD_PROTOCOL_RAW", Napi::Number::New(env, SCARD_PROTOCOL_RAW)),

        // State
        InstanceValue("SCARD_STATE_UNAWARE", Napi::Number::New(env, SCARD_STATE_UNAWARE)),
        InstanceValue("SCARD_STATE_IGNORE", Napi::Number::New(env, SCARD_STATE_IGNORE)),
        InstanceValue("SCARD_STATE_CHANGED", Napi::Number::New(env, SCARD_STATE_CHANGED)),
        InstanceValue("SCARD_STATE_UNKNOWN", Napi::Number::New(env, SCARD_STATE_UNKNOWN)),
        InstanceValue("SCARD_STATE_UNAVAILABLE", Napi::Number::New(env, SCARD_STATE_UNAVAILABLE)),
        InstanceValue("SCARD_STATE_EMPTY", Napi::Number::New(env, SCARD_STATE_EMPTY)),
        InstanceValue("SCARD_STATE_PRESENT", Napi::Number::New(env, SCARD_STATE_PRESENT)),
        InstanceValue("SCARD_STATE_ATRMATCH", Napi::Number::New(env, SCARD_STATE_ATRMATCH)),
        InstanceValue("SCARD_STATE_EXCLUSIVE", Napi::Number::New(env, SCARD_STATE_EXCLUSIVE)),
        InstanceValue("SCARD_STATE_INUSE", Napi::Number::New(env, SCARD_STATE_INUSE)),
        InstanceValue("SCARD_STATE_MUTE", Napi::Number::New(env, SCARD_STATE_MUTE)),

        // Disconnect disposition
        InstanceValue("SCARD_LEAVE_CARD", Napi::Number::New(env, SCARD_LEAVE_CARD)),
        InstanceValue("SCARD_RESET_CARD", Napi::Number::New(env, SCARD_RESET_CARD)),
        InstanceValue("SCARD_UNPOWER_CARD", Napi::Number::New(env, SCARD_UNPOWER_CARD)),
        InstanceValue("SCARD_EJECT_CARD", Napi::Number::New(env, SCARD_EJECT_CARD)),
    });

    exports.Set("CardReader", func);
    return exports;
}

CardReader::CardReader(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<CardReader>(info),
      m_card_context(0),
      m_status_card_context(0),
      m_card_handle(0),
      m_status_thread(),
      m_state(0),
      m_thread_running(false) {

    Napi::Env env = info.Env();

    if (!info[0].IsString()) {
        Napi::TypeError::New(env, "First argument must be a string").ThrowAsJavaScriptException();
        return;
    }

    Napi::String reader_name = info[0].As<Napi::String>();
    m_name = reader_name.Utf8Value();

    assert(uv_mutex_init(&m_mutex) == 0);
    assert(uv_cond_init(&m_cond) == 0);

    Napi::Object self = this->Value();
    self.Set("name", reader_name);
    self.Set("connected", Napi::Boolean::New(env, false));
}

CardReader::~CardReader() {
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

Napi::Value CardReader::GetStatus(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();
    Napi::Function cb = info[0].As<Napi::Function>();

    m_tsfn = Napi::ThreadSafeFunction::New(
        env,
        cb,
        "PCSCLite_CardReader_Status",
        0,
        1
    );

    int ret = uv_thread_create(&m_status_thread, HandlerFunction, this);
    assert(ret == 0);
    m_thread_running = true;

    return env.Undefined();
}

Napi::Value CardReader::Connect(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();

    if (!info[0].IsNumber()) {
        Napi::Error::New(env, "First argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[1].IsNumber()) {
        Napi::Error::New(env, "Second argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[2].IsFunction()) {
        Napi::Error::New(env, "Third argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    DWORD share_mode = info[0].As<Napi::Number>().Uint32Value();
    DWORD pref_protocol = info[1].As<Napi::Number>().Uint32Value();
    Napi::Function cb = info[2].As<Napi::Function>();

    auto* worker = new ConnectWorker(cb, this, share_mode, pref_protocol);
    worker->Queue();

    return env.Undefined();
}

Napi::Value CardReader::Disconnect(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();

    if (!info[0].IsNumber()) {
        Napi::Error::New(env, "First argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[1].IsFunction()) {
        Napi::Error::New(env, "Second argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    DWORD disposition = info[0].As<Napi::Number>().Uint32Value();
    Napi::Function cb = info[1].As<Napi::Function>();

    auto* worker = new DisconnectWorker(cb, this, disposition);
    worker->Queue();

    return env.Undefined();
}

Napi::Value CardReader::Transmit(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();

    if (!info[0].IsBuffer()) {
        Napi::Error::New(env, "First argument must be a Buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[1].IsNumber()) {
        Napi::Error::New(env, "Second argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[2].IsNumber()) {
        Napi::Error::New(env, "Third argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[3].IsFunction()) {
        Napi::Error::New(env, "Fourth argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    uint32_t out_len = info[1].As<Napi::Number>().Uint32Value();
    uint32_t protocol = info[2].As<Napi::Number>().Uint32Value();
    Napi::Function cb = info[3].As<Napi::Function>();

    std::vector<uint8_t> in_copy(buf.Data(), buf.Data() + buf.Length());
    auto* worker = new TransmitWorker(cb, this, protocol, std::move(in_copy), out_len);
    worker->Queue();

    return env.Undefined();
}

Napi::Value CardReader::Control(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();

    if (!info[0].IsBuffer()) {
        Napi::Error::New(env, "First argument must be a Buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[1].IsNumber()) {
        Napi::Error::New(env, "Second argument must be an integer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[2].IsBuffer()) {
        Napi::Error::New(env, "Third argument must be a Buffer").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    if (!info[3].IsFunction()) {
        Napi::Error::New(env, "Fourth argument must be a callback function").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Buffer<uint8_t> in_buf = info[0].As<Napi::Buffer<uint8_t>>();
    DWORD control_code = info[1].As<Napi::Number>().Uint32Value();
    Napi::Buffer<uint8_t> out_buf = info[2].As<Napi::Buffer<uint8_t>>();
    Napi::Function cb = info[3].As<Napi::Function>();

    auto* worker = new ControlWorker(cb, this, control_code, in_buf, out_buf);
    worker->Queue();

    return env.Undefined();
}

Napi::Value CardReader::Close(const Napi::CallbackInfo& info) {

    Napi::Env env = info.Env();
    LONG result = SCARD_S_SUCCESS;

    if (m_thread_running) {
        uv_mutex_lock(&m_mutex);
        if (m_state == 0) {
            int ret;
            int times = 0;
            m_state = 1;
            do {
                result = SCardCancel(m_status_card_context);
                ret = uv_cond_timedwait(&m_cond, &m_mutex, 10000000);
            } while ((ret != 0) && (++times < 5));
        }

        uv_mutex_unlock(&m_mutex);
        assert(uv_thread_join(&m_status_thread) == 0);
        m_thread_running = false;
    }

    return Napi::Number::New(env, result);
}

void CardReader::HandlerFunction(void* arg) {

    CardReader* reader = static_cast<CardReader*>(arg);

    LONG result = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL,
                                        &reader->m_status_card_context);
    (void)result;

    SCARD_READERSTATE card_reader_state = SCARD_READERSTATE();
    card_reader_state.szReader = reader->m_name.c_str();
    card_reader_state.dwCurrentState = SCARD_STATE_UNAWARE;

    while (!reader->m_state) {

        result = SCardGetStatusChange(reader->m_status_card_context, INFINITE,
                                      &card_reader_state, 1);

        AsyncResult* ar = new AsyncResult();
        ar->result = result;
        ar->status = 0;
        ar->atrlen = 0;
        ar->do_exit = false;

        uv_mutex_lock(&reader->m_mutex);
        if (reader->m_state == 1) {
            // Exit requested by user. Notify close method about SCardStatusChange was interrupted.
            uv_cond_signal(&reader->m_cond);
        } else if (result != (LONG)SCARD_S_SUCCESS) {
            // Exit this loop due to errors
            reader->m_state = 2;
        }

        ar->do_exit = (reader->m_state != 0);
        if (card_reader_state.dwEventState == card_reader_state.dwCurrentState) {
            ar->status = 0;
        } else {
            ar->status = card_reader_state.dwEventState;
        }
        memcpy(ar->atr, card_reader_state.rgbAtr, card_reader_state.cbAtr);
        ar->atrlen = card_reader_state.cbAtr;

        uv_mutex_unlock(&reader->m_mutex);

        reader->m_tsfn.NonBlockingCall(ar,
            [reader](Napi::Env env, Napi::Function jsCallback, AsyncResult* data) {
                reader->HandleAsyncResult(env, jsCallback, data);
            });

        card_reader_state.dwCurrentState = card_reader_state.dwEventState;
    }

    reader->m_tsfn.Release();

    if (reader->m_status_card_context) {
        SCardReleaseContext(reader->m_status_card_context);
        reader->m_status_card_context = 0;
    }
}

void CardReader::HandleAsyncResult(Napi::Env env, Napi::Function jsCallback, AsyncResult* ar) {

    if (env != nullptr && !jsCallback.IsEmpty()) {

        if (m_thread_running) {
            uv_mutex_lock(&m_mutex);
        }

        if (m_state == 1) {
            // Swallow events: Listening thread was cancelled by user.
        } else if ((ar->result == SCARD_S_SUCCESS) ||
                   (ar->result == (LONG)SCARD_E_NO_READERS_AVAILABLE) ||
                   (ar->result == (LONG)SCARD_E_UNKNOWN_READER)) { // Card reader was unplugged, it's not an error
            if (ar->status != 0) {
                jsCallback.Call({
                    env.Undefined(),
                    Napi::Number::New(env, ar->status),
                    Napi::Buffer<uint8_t>::Copy(env, ar->atr, ar->atrlen)
                });
            }
        } else {
            jsCallback.Call({
                Napi::Error::New(env, error_msg("SCardGetStatusChange", ar->result)).Value()
            });
        }

        if (ar->do_exit) {
            /* Emit end event on the wrapper */
            Napi::Object self = this->Value();
            Napi::Value emit = self.Get("emit");
            if (emit.IsFunction()) {
                emit.As<Napi::Function>().Call(self, {
                    Napi::String::New(env, "_end")
                });
            }
        }

        if (m_thread_running) {
            uv_mutex_unlock(&m_mutex);
        }
    }

    delete ar;
}
