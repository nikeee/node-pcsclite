use napi::bindgen_prelude::*;
use napi::threadsafe_function::{
    ErrorStrategy, ThreadSafeCallContext, ThreadsafeFunction, ThreadsafeFunctionCallMode,
};
use pcsc::{
    Card, Context, Disposition, Protocol, Protocols, ReaderState, Scope, ShareMode, State,
};
use std::ffi::CString;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::error::pcsc_error;

const INFINITE: Duration = Duration::from_millis(0xFFFFFFFF);
const MAX_ATR_SIZE: usize = 33;

struct ConnectState {
    ctx: Option<Arc<Context>>,
    card: Option<Card>,
    active_protocol: u32,
}

struct Inner {
    name: CString,
    name_str: String,
    connect: Mutex<ConnectState>,
    connected: AtomicBool,
    status_ctx: Mutex<Option<Arc<Context>>>,
    status_state: AtomicU8, // 0 running, 1 closed, 2 error
    thread: Mutex<Option<JoinHandle<()>>>,
}

#[napi(js_name = "CardReader")]
pub struct CardReader {
    inner: Arc<Inner>,
}

#[napi]
impl CardReader {
    #[napi(constructor)]
    pub fn new(name: String) -> Result<Self> {
        let cname = CString::new(name.clone())
            .map_err(|_| Error::from_reason("Reader name contains NUL byte"))?;

        Ok(Self {
            inner: Arc::new(Inner {
                name: cname,
                name_str: name,
                connect: Mutex::new(ConnectState {
                    ctx: None,
                    card: None,
                    active_protocol: 0,
                }),
                connected: AtomicBool::new(false),
                status_ctx: Mutex::new(None),
                status_state: AtomicU8::new(0),
                thread: Mutex::new(None),
            }),
        })
    }

    #[napi(getter)]
    pub fn name(&self) -> &str {
        &self.inner.name_str
    }

    #[napi(getter)]
    pub fn connected(&self) -> bool {
        self.inner.connected.load(Ordering::SeqCst)
    }

    #[napi(setter)]
    pub fn set_connected(&mut self, value: bool) {
        self.inner.connected.store(value, Ordering::SeqCst);
    }

    // --- Constants ---

    #[napi(getter, js_name = "SCARD_SHARE_SHARED")]
    pub fn scard_share_shared(&self) -> u32 { 2 }
    #[napi(getter, js_name = "SCARD_SHARE_EXCLUSIVE")]
    pub fn scard_share_exclusive(&self) -> u32 { 1 }
    #[napi(getter, js_name = "SCARD_SHARE_DIRECT")]
    pub fn scard_share_direct(&self) -> u32 { 3 }

    #[napi(getter, js_name = "IOCTL_CCID_ESCAPE")]
    pub fn ioctl_ccid_escape(&self) -> u32 {
        if cfg!(windows) { 0x42000000 + 3500 } else { 0x42000000 + 1 }
    }

    #[napi(getter, js_name = "SCARD_PROTOCOL_T0")]
    pub fn scard_protocol_t0(&self) -> u32 { 0x0001 }
    #[napi(getter, js_name = "SCARD_PROTOCOL_T1")]
    pub fn scard_protocol_t1(&self) -> u32 { 0x0002 }
    #[napi(getter, js_name = "SCARD_PROTOCOL_RAW")]
    pub fn scard_protocol_raw(&self) -> u32 { 0x0004 }

    #[napi(getter, js_name = "SCARD_STATE_UNAWARE")]
    pub fn scard_state_unaware(&self) -> u32 { 0x0000 }
    #[napi(getter, js_name = "SCARD_STATE_IGNORE")]
    pub fn scard_state_ignore(&self) -> u32 { 0x0001 }
    #[napi(getter, js_name = "SCARD_STATE_CHANGED")]
    pub fn scard_state_changed(&self) -> u32 { 0x0002 }
    #[napi(getter, js_name = "SCARD_STATE_UNKNOWN")]
    pub fn scard_state_unknown(&self) -> u32 { 0x0004 }
    #[napi(getter, js_name = "SCARD_STATE_UNAVAILABLE")]
    pub fn scard_state_unavailable(&self) -> u32 { 0x0008 }
    #[napi(getter, js_name = "SCARD_STATE_EMPTY")]
    pub fn scard_state_empty(&self) -> u32 { 0x0010 }
    #[napi(getter, js_name = "SCARD_STATE_PRESENT")]
    pub fn scard_state_present(&self) -> u32 { 0x0020 }
    #[napi(getter, js_name = "SCARD_STATE_ATRMATCH")]
    pub fn scard_state_atrmatch(&self) -> u32 { 0x0040 }
    #[napi(getter, js_name = "SCARD_STATE_EXCLUSIVE")]
    pub fn scard_state_exclusive(&self) -> u32 { 0x0080 }
    #[napi(getter, js_name = "SCARD_STATE_INUSE")]
    pub fn scard_state_inuse(&self) -> u32 { 0x0100 }
    #[napi(getter, js_name = "SCARD_STATE_MUTE")]
    pub fn scard_state_mute(&self) -> u32 { 0x0200 }

    #[napi(getter, js_name = "SCARD_LEAVE_CARD")]
    pub fn scard_leave_card(&self) -> u32 { 0 }
    #[napi(getter, js_name = "SCARD_RESET_CARD")]
    pub fn scard_reset_card(&self) -> u32 { 1 }
    #[napi(getter, js_name = "SCARD_UNPOWER_CARD")]
    pub fn scard_unpower_card(&self) -> u32 { 2 }
    #[napi(getter, js_name = "SCARD_EJECT_CARD")]
    pub fn scard_eject_card(&self) -> u32 { 3 }

    // --- Status monitoring ---

    #[napi]
    pub fn get_status(&mut self, status_cb: JsFunction, end_cb: JsFunction) -> Result<()> {
        {
            let g = self.inner.thread.lock().unwrap();
            if g.is_some() {
                return Err(Error::from_reason("get_status() already called"));
            }
        }

        let status_tsfn: ThreadsafeFunction<StatusEvent, ErrorStrategy::CalleeHandled> =
            status_cb.create_threadsafe_function(
                0,
                |ctx: ThreadSafeCallContext<StatusEvent>| {
                    let StatusEvent { state, atr } = ctx.value;
                    let state_js = ctx.env.create_uint32(state)?;
                    let atr_js = ctx.env.create_buffer_with_data(atr)?.into_raw();
                    Ok(vec![state_js.into_unknown(), atr_js.into_unknown()])
                },
            )?;

        let end_tsfn: ThreadsafeFunction<(), ErrorStrategy::Fatal> =
            end_cb.create_threadsafe_function(0, |_ctx: ThreadSafeCallContext<()>| {
                Ok(Vec::<napi::JsUnknown>::new())
            })?;

        let inner = Arc::clone(&self.inner);
        let handle = thread::spawn(move || {
            run_status_thread(inner, status_tsfn, end_tsfn);
        });

        *self.inner.thread.lock().unwrap() = Some(handle);
        Ok(())
    }

    // --- Connect / Disconnect / Transmit / Control ---

    #[napi(js_name = "_connect")]
    pub fn connect_card(
        &mut self,
        share_mode: u32,
        pref_protocol: u32,
        cb: JsFunction,
    ) -> Result<()> {
        let tsfn: ThreadsafeFunction<u32, ErrorStrategy::CalleeHandled> = cb
            .create_threadsafe_function(0, |ctx: ThreadSafeCallContext<u32>| {
                Ok(vec![ctx.env.create_uint32(ctx.value)?])
            })?;

        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            let res = do_connect(&inner, share_mode, pref_protocol);
            let napi_res = res.map_err(|(method, e)| pcsc_error(method, e));
            tsfn.call(napi_res, ThreadsafeFunctionCallMode::NonBlocking);
        });
        Ok(())
    }

    #[napi(js_name = "_disconnect")]
    pub fn disconnect_card(&mut self, disposition: u32, cb: JsFunction) -> Result<()> {
        let tsfn: ThreadsafeFunction<(), ErrorStrategy::CalleeHandled> = cb
            .create_threadsafe_function(0, |_ctx: ThreadSafeCallContext<()>| {
                Ok(Vec::<napi::JsUnknown>::new())
            })?;

        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            let res = do_disconnect(&inner, disposition);
            let napi_res = res.map_err(|(method, e)| pcsc_error(method, e));
            tsfn.call(napi_res, ThreadsafeFunctionCallMode::NonBlocking);
        });
        Ok(())
    }

    #[napi(js_name = "_transmit")]
    pub fn transmit(
        &mut self,
        data: Buffer,
        res_len: u32,
        protocol: u32,
        cb: JsFunction,
    ) -> Result<()> {
        let tsfn: ThreadsafeFunction<Vec<u8>, ErrorStrategy::CalleeHandled> = cb
            .create_threadsafe_function(0, |ctx: ThreadSafeCallContext<Vec<u8>>| {
                Ok(vec![ctx.env.create_buffer_with_data(ctx.value)?.into_raw()])
            })?;

        let in_data: Vec<u8> = data.to_vec();
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            let res = do_transmit(&inner, in_data, res_len as usize, protocol);
            let napi_res = res.map_err(|(method, e)| pcsc_error(method, e));
            tsfn.call(napi_res, ThreadsafeFunctionCallMode::NonBlocking);
        });
        Ok(())
    }

    #[napi(js_name = "_control")]
    pub fn control(
        &mut self,
        data: Buffer,
        control_code: u32,
        res_len: u32,
        cb: JsFunction,
    ) -> Result<()> {
        let tsfn: ThreadsafeFunction<Vec<u8>, ErrorStrategy::CalleeHandled> = cb
            .create_threadsafe_function(0, |ctx: ThreadSafeCallContext<Vec<u8>>| {
                Ok(vec![ctx.env.create_buffer_with_data(ctx.value)?.into_raw()])
            })?;

        let in_data: Vec<u8> = data.to_vec();
        let inner = Arc::clone(&self.inner);
        thread::spawn(move || {
            let res = do_control(&inner, in_data, control_code, res_len as usize);
            let napi_res = res.map_err(|(method, e)| pcsc_error(method, e));
            tsfn.call(napi_res, ThreadsafeFunctionCallMode::NonBlocking);
        });
        Ok(())
    }

    #[napi]
    pub fn close(&mut self) -> i32 {
        stop_status_thread(&self.inner);
        0
    }
}

impl Drop for CardReader {
    fn drop(&mut self) {
        stop_status_thread(&self.inner);

        // Disconnect card if still connected
        let card = self.inner.connect.lock().unwrap().card.take();
        if let Some(c) = card {
            let _ = c.disconnect(Disposition::LeaveCard);
        }
    }
}

fn stop_status_thread(inner: &Inner) {
    if inner.status_state.load(Ordering::SeqCst) == 0 {
        inner.status_state.store(1, Ordering::SeqCst);
        if let Some(ctx) = inner.status_ctx.lock().unwrap().as_ref() {
            let _ = ctx.cancel();
        }
    }
    let handle = inner.thread.lock().unwrap().take();
    if let Some(h) = handle {
        let _ = h.join();
    }
}

// --- Operation implementations ---

type OpResult<T> = std::result::Result<T, (&'static str, pcsc::Error)>;

fn do_connect(inner: &Inner, share_mode: u32, pref_protocol: u32) -> OpResult<u32> {
    let mut state = inner.connect.lock().unwrap();
    if state.ctx.is_none() {
        let ctx = Context::establish(Scope::System)
            .map_err(|e| ("SCardEstablishContext", e))?;
        state.ctx = Some(Arc::new(ctx));
    }
    let ctx = Arc::clone(state.ctx.as_ref().unwrap());

    let share = match share_mode {
        1 => ShareMode::Exclusive,
        2 => ShareMode::Shared,
        3 => ShareMode::Direct,
        _ => ShareMode::Exclusive,
    };
    let protocols = Protocols::from_bits_truncate(pref_protocol as _);

    let card = ctx
        .connect(&inner.name, share, protocols)
        .map_err(|e| ("SCardConnect", e))?;
    let active = match card.status2_owned() {
        Ok(s) => protocol_to_u32(s.protocol()),
        Err(_) => 0,
    };
    state.card = Some(card);
    state.active_protocol = active;
    drop(state);

    inner.connected.store(true, Ordering::SeqCst);
    Ok(active)
}

fn protocol_to_u32(p: Protocol) -> u32 {
    match p {
        Protocol::T0 => 0x0001,
        Protocol::T1 => 0x0002,
        Protocol::RAW => 0x0004,
    }
}

fn do_disconnect(inner: &Inner, disposition: u32) -> OpResult<()> {
    let mut state = inner.connect.lock().unwrap();
    let Some(card) = state.card.take() else {
        return Ok(());
    };
    drop(state);

    let disp = match disposition {
        0 => Disposition::LeaveCard,
        1 => Disposition::ResetCard,
        2 => Disposition::UnpowerCard,
        3 => Disposition::EjectCard,
        _ => Disposition::UnpowerCard,
    };

    match card.disconnect(disp) {
        Ok(()) => {
            inner.connected.store(false, Ordering::SeqCst);
            Ok(())
        }
        Err((card, e)) => {
            // Restore card on failure
            inner.connect.lock().unwrap().card = Some(card);
            Err(("SCardDisconnect", e))
        }
    }
}

fn do_transmit(
    inner: &Inner,
    in_data: Vec<u8>,
    out_len: usize,
    _protocol: u32,
) -> OpResult<Vec<u8>> {
    let state = inner.connect.lock().unwrap();
    let card = state
        .card
        .as_ref()
        .ok_or(("SCardTransmit", pcsc::Error::InvalidHandle))?;

    let mut recv = vec![0u8; out_len];
    let response = card
        .transmit(&in_data, &mut recv)
        .map_err(|e| ("SCardTransmit", e))?;
    let resp_len = response.len();
    recv.truncate(resp_len);
    Ok(recv)
}

fn do_control(
    inner: &Inner,
    in_data: Vec<u8>,
    control_code: u32,
    out_len: usize,
) -> OpResult<Vec<u8>> {
    let state = inner.connect.lock().unwrap();
    let card = state
        .card
        .as_ref()
        .ok_or(("SCardControl", pcsc::Error::InvalidHandle))?;

    let mut recv = vec![0u8; out_len];
    let response = card
        .control(control_code as _, &in_data, &mut recv)
        .map_err(|e| ("SCardControl", e))?;
    let resp_len = response.len();
    recv.truncate(resp_len);
    Ok(recv)
}

// --- Status thread ---

struct StatusEvent {
    state: u32,
    atr: Vec<u8>,
}

fn run_status_thread(
    inner: Arc<Inner>,
    status_tsfn: ThreadsafeFunction<StatusEvent, ErrorStrategy::CalleeHandled>,
    end_tsfn: ThreadsafeFunction<(), ErrorStrategy::Fatal>,
) {
    let ctx = match Context::establish(Scope::System) {
        Ok(c) => Arc::new(c),
        Err(e) => {
            status_tsfn.call(
                Err(pcsc_error("SCardEstablishContext", e)),
                ThreadsafeFunctionCallMode::NonBlocking,
            );
            end_tsfn.call((), ThreadsafeFunctionCallMode::NonBlocking);
            return;
        }
    };

    *inner.status_ctx.lock().unwrap() = Some(Arc::clone(&ctx));

    let mut current_state = State::UNAWARE;

    while inner.status_state.load(Ordering::SeqCst) == 0 {
        let mut states = [ReaderState::new(inner.name.as_c_str(), current_state)];
        states[0].sync_current_state();

        let result = ctx.get_status_change(INFINITE, &mut states);

        match result {
            Ok(()) => {}
            Err(pcsc::Error::Cancelled) => {
                inner.status_state.compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst).ok();
                break;
            }
            Err(pcsc::Error::UnknownReader) | Err(pcsc::Error::NoReadersAvailable) => {
                // Reader unplugged — not an error, just exit
                break;
            }
            Err(e) => {
                status_tsfn.call(
                    Err(pcsc_error("SCardGetStatusChange", e)),
                    ThreadsafeFunctionCallMode::NonBlocking,
                );
                inner.status_state.store(2, Ordering::SeqCst);
                break;
            }
        }

        if inner.status_state.load(Ordering::SeqCst) != 0 {
            break;
        }

        let event_state = states[0].event_state();
        if event_state == current_state {
            current_state = event_state;
            continue;
        }

        let atr_bytes = states[0].atr();
        let mut atr_vec = Vec::with_capacity(atr_bytes.len().min(MAX_ATR_SIZE));
        atr_vec.extend_from_slice(&atr_bytes[..atr_bytes.len().min(MAX_ATR_SIZE)]);

        status_tsfn.call(
            Ok(StatusEvent {
                state: event_state.bits() as u32,
                atr: atr_vec,
            }),
            ThreadsafeFunctionCallMode::NonBlocking,
        );

        current_state = event_state;
    }

    end_tsfn.call((), ThreadsafeFunctionCallMode::NonBlocking);

    inner.status_ctx.lock().unwrap().take();
}
