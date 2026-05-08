use napi::Error as NapiError;
use pcsc::Error as PcscError;

pub fn pcsc_error(method: &str, err: PcscError) -> NapiError {
    NapiError::from_reason(format!(
        "{} error: {}(0x{:08x})",
        method,
        err,
        err as u32
    ))
}
