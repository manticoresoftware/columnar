use std::os::raw::c_char;
use crate::model::text_model_wrapper::{TextModelResult, TextModelWrapper, FloatVecResult, StringItem};

type LoadModelFn = extern "C" fn(
	*const c_char,
	usize,
	*const c_char,
	usize,
	*const c_char,
	usize,
	bool,
) -> TextModelResult;

type FreeModelResultFn = extern "C" fn(TextModelResult);

type MakeVectEmbeddingsFn = extern "C" fn(
	&TextModelWrapper,
	*const StringItem,
	usize
) -> FloatVecResult;

type FreeVecResultFn = extern "C" fn(FloatVecResult);

type GetLenFn = extern "C" fn(&TextModelWrapper) -> usize;

#[repr(C)]
pub struct EmbedLib {
	version: usize,
	load_model: LoadModelFn,
	free_model_result: FreeModelResultFn,
	make_vect_embeddings: MakeVectEmbeddingsFn,
	free_vec_result: FreeVecResultFn,
	get_hidden_size: GetLenFn,
	get_max_input_size: GetLenFn,
}

const LIB: EmbedLib = EmbedLib {
	version: 1usize,
	load_model: TextModelWrapper::load_model,
	free_model_result: TextModelWrapper::free_model_result,
	make_vect_embeddings: TextModelWrapper::make_vect_embeddings,
	free_vec_result: TextModelWrapper::free_vec_result,
	get_hidden_size: TextModelWrapper::get_hidden_size,
	get_max_input_size: TextModelWrapper::get_max_input_len,
};

#[no_mangle]
pub extern "C" fn GetLibFuncs() -> *const EmbedLib {
	std::panic::set_hook(Box::new(|_| {}));
	&LIB
}
