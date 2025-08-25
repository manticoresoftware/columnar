mod error;
mod ffi;
mod model;
mod utils;

#[cfg(test)]
mod utils_test;

#[cfg(test)]
mod error_test;

#[cfg(test)]
mod integration_test;

#[cfg(test)]
mod error_handling_test;

pub use error::LibError;
pub use ffi::{EmbedLib, GetLibFuncs};
pub use model::TextModel;
