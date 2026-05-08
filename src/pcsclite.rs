use napi::bindgen_prelude::*;
use napi::threadsafe_function::{
    ErrorStrategy, ThreadSafeCallContext, ThreadsafeFunction, ThreadsafeFunctionCallMode,
};
use pcsc::{Context, ReaderState, Scope, State, PNP_NOTIFICATION};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::error::pcsc_error;

const INFINITE: Duration = Duration::from_millis(0xFFFFFFFF);

struct Inner {
    context: Option<Arc<Context>>,
    state: u8, // 0 = running, 1 = closed by user, 2 = error
    pnp: bool,
    thread: Option<JoinHandle<()>>,
}

#[napi(js_name = "PCSCLite")]
pub struct PcscLite {
    inner: Arc<Mutex<Inner>>,
}

#[napi]
impl PcscLite {
    #[napi(constructor)]
    pub fn new() -> Result<Self> {
        let context = Context::establish(Scope::System)
            .map_err(|e| pcsc_error("SCardEstablishContext", e))?;

        let mut states = [ReaderState::new(PNP_NOTIFICATION(), State::UNAWARE)];
        let _ = context.get_status_change(Duration::from_millis(0), &mut states);
        let pnp = !states[0].event_state().contains(State::UNKNOWN);

        Ok(Self {
            inner: Arc::new(Mutex::new(Inner {
                context: Some(Arc::new(context)),
                state: 0,
                pnp,
                thread: None,
            })),
        })
    }

    #[napi]
    pub fn start(&mut self, cb: JsFunction) -> Result<()> {
        {
            let g = self.inner.lock().unwrap();
            if g.thread.is_some() {
                return Err(Error::from_reason("start() already called"));
            }
        }

        let tsfn: ThreadsafeFunction<Vec<u8>, ErrorStrategy::CalleeHandled> = cb
            .create_threadsafe_function(0, |ctx: ThreadSafeCallContext<Vec<u8>>| {
                ctx.env
                    .create_buffer_with_data(ctx.value)
                    .map(|b| vec![b.into_raw()])
            })?;

        let inner = Arc::clone(&self.inner);
        let handle = thread::spawn(move || {
            run_handler(inner, tsfn);
        });

        let mut g = self.inner.lock().unwrap();
        g.thread = Some(handle);
        Ok(())
    }

    #[napi]
    pub fn close(&mut self) -> i32 {
        let handle = {
            let mut g = self.inner.lock().unwrap();
            if g.state == 0 {
                g.state = 1;
                if let Some(ctx) = &g.context {
                    let _ = ctx.cancel();
                }
            }
            g.thread.take()
        };

        if let Some(h) = handle {
            let _ = h.join();
        }
        0
    }
}

impl Drop for PcscLite {
    fn drop(&mut self) {
        let handle = {
            let mut g = self.inner.lock().unwrap();
            if g.state == 0 {
                g.state = 1;
                if let Some(ctx) = &g.context {
                    let _ = ctx.cancel();
                }
            }
            g.thread.take()
        };
        if let Some(h) = handle {
            let _ = h.join();
        }
    }
}

fn run_handler(
    inner: Arc<Mutex<Inner>>,
    tsfn: ThreadsafeFunction<Vec<u8>, ErrorStrategy::CalleeHandled>,
) {
    let mut current_state = State::UNAWARE;

    loop {
        let (ctx, pnp) = {
            let g = inner.lock().unwrap();
            if g.state != 0 {
                break;
            }
            match g.context.as_ref() {
                Some(c) => (Arc::clone(c), g.pnp),
                None => break,
            }
        };

        match list_readers_buffer(&ctx) {
            Ok(buf) => {
                tsfn.call(Ok(buf), ThreadsafeFunctionCallMode::NonBlocking);
            }
            Err(e) => {
                tsfn.call(
                    Err(pcsc_error("SCardListReaders", e)),
                    ThreadsafeFunctionCallMode::NonBlocking,
                );
                inner.lock().unwrap().state = 2;
                break;
            }
        }

        if pnp {
            let mut states = [ReaderState::new(PNP_NOTIFICATION(), current_state)];
            states[0].sync_current_state();
            match ctx.get_status_change(INFINITE, &mut states) {
                Ok(()) => {
                    current_state = states[0].event_state();
                }
                Err(pcsc::Error::Cancelled) => break,
                Err(e) => {
                    tsfn.call(
                        Err(pcsc_error("SCardGetStatusChange", e)),
                        ThreadsafeFunctionCallMode::NonBlocking,
                    );
                    inner.lock().unwrap().state = 2;
                    break;
                }
            }
        } else {
            thread::sleep(Duration::from_secs(1));
        }
    }

}

fn list_readers_buffer(ctx: &Context) -> std::result::Result<Vec<u8>, pcsc::Error> {
    match ctx.list_readers_owned() {
        Ok(names) => {
            if names.is_empty() {
                return Ok(Vec::new());
            }
            let mut buf = Vec::new();
            for n in &names {
                buf.extend_from_slice(n.as_bytes_with_nul());
            }
            buf.push(0);
            Ok(buf)
        }
        Err(pcsc::Error::NoReadersAvailable) => Ok(Vec::new()),
        Err(e) => Err(e),
    }
}
