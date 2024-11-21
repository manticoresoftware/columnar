// Copyright (c) 2024, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "embeddings.h"

#include "knn.h"
#include "util/util_private.h"
#include "manticoresearch_text_embeddings.h"

#include <unordered_map>
#include <algorithm>

#ifdef WIN32
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>

	#define RTLD_LAZY		0
	#define RTLD_NOW		0
	#define RTLD_LOCAL		0
	#define RTLD_GLOBAL		0
#else
	#include <dlfcn.h>
#endif

namespace knn
{

#if _WIN32
void * dlsym ( void * lib, const char * name )	{ return (void*)GetProcAddress ( (HMODULE)lib, name ); }
void * dlopen ( const char * libname, int )		{ return LoadLibraryEx ( libname, NULL, 0 ); }
int dlclose ( void * lib )						{ return FreeLibrary ( (HMODULE)lib ) ? 0 : GetLastError(); }

const char * dlerror()
{
	static char szError[256];
	DWORD uError = GetLastError();
	FormatMessage ( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, uError, LANG_SYSTEM_DEFAULT, (LPTSTR)szError, sizeof(szError), NULL );
	return szError;
}

#endif // _WIN32

template <typename T>
bool LoadFunc ( T & pFunc, void * pHandle, const char * szFunc, const std::string & sLib, std::string & sError )
{
	pFunc = (T) dlsym ( pHandle, szFunc );
	if ( !pFunc )
	{
		sError = util::FormatStr ( "symbol '%s' not found in '%s'", szFunc, sLib.c_str() );
		dlclose(pHandle);
		return false;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////

using GetLibFuncs_fn = const EmbedLib * (*)();
const EmbedLib *			g_pLibFuncs = nullptr;

class LoadedLib_c
{
public:
						LoadedLib_c ( void * pHandle ) : m_pHandle(pHandle) {}
						~LoadedLib_c();

	void *				Handle() { return m_pHandle; }
	void				AddModel ( const std::string & sKey, TextModelWrapper pModel ) { m_hModels.insert ( { sKey, pModel } ); }
	TextModelWrapper	GetModel ( const std::string & sKey ) const;
	static std::weak_ptr<LoadedLib_c> Get();

private:
	void *	m_pHandle = nullptr;
	std::unordered_map<std::string, TextModelWrapper> m_hModels;
};


LoadedLib_c::~LoadedLib_c()
{
	if ( !m_pHandle )
		return;

	assert(g_pLibFuncs);
	for ( auto i : m_hModels )
	{
		TextModelResult	tResult = { i.second };
		g_pLibFuncs->free_model_result(tResult);
	}

	dlclose(m_pHandle);
}


TextModelWrapper LoadedLib_c::GetModel ( const std::string & sKey ) const
{
	const auto & tFound = m_hModels.find(sKey);
	return tFound==m_hModels.end() ? nullptr : tFound->second;
}


std::weak_ptr<LoadedLib_c> g_pLoadedLib;

std::shared_ptr<LoadedLib_c> LoadEmbeddingsLib ( const std::string & sLibPath, std::string & sError )
{
	std::shared_ptr<LoadedLib_c> pSharedLib = g_pLoadedLib.lock();
	if ( pSharedLib )
		return pSharedLib;

	pSharedLib = std::make_shared<LoadedLib_c> ( dlopen ( sLibPath.c_str(), RTLD_LAZY | RTLD_LOCAL ) );
	if ( !pSharedLib->Handle() )
	{
		const char * szDlError = dlerror();
		sError = util::FormatStr ( "dlopen() failed: %s", szDlError ? szDlError : "(null)" );
		return nullptr;		// if dlopen fails, don't report an error
	}

	GetLibFuncs_fn fnGetLibFuncs;
	if ( !LoadFunc ( fnGetLibFuncs, pSharedLib->Handle(), "GetLibFuncs", sLibPath.c_str(), sError ) )
		return nullptr;

	g_pLibFuncs = fnGetLibFuncs();
	if ( !g_pLibFuncs)
	{
		sError = "Error initializing embeddings library";
		return nullptr;
	}

	g_pLoadedLib = pSharedLib;
	return pSharedLib;
}

///////////////////////////////////////////////////////////////////////////////

std::string ToKey ( const ModelSettings_t & tSettings )
{
	return tSettings.m_sModelName + tSettings.m_sCachePath + tSettings.m_sAPIKey + std::to_string ( tSettings.m_bUseGPU );
}

///////////////////////////////////////////////////////////////////////////////

class TextToEmbeddings_c : public TextToEmbeddings_i
{
public:
			TextToEmbeddings_c ( const ModelSettings_t & tSettings ) : m_tSettings ( tSettings ) {}

	bool	Initialize ( const std::string & sLibPath, std::string & sError );
	bool	Convert ( std::string_view sText, std::vector<float> & dEmbedding, std::string & sError ) const override;
	int		GetDims() const override;

private:
	ModelSettings_t		m_tSettings;
	std::shared_ptr<LoadedLib_c> m_pLib;
	TextModelWrapper	m_pModel = nullptr;
};


bool TextToEmbeddings_c::Initialize ( const std::string & sLibPath, std::string & sError )
{
	assert(!m_pModel);

	m_pLib = LoadEmbeddingsLib ( sLibPath, sError );
	if ( !m_pLib )
		return false;

	m_pModel = m_pLib->GetModel ( ToKey(m_tSettings) );
	if ( m_pModel )
		return true;

	assert(g_pLibFuncs);
	TextModelResult tResult = g_pLibFuncs->load_model ( m_tSettings.m_sModelName.c_str(), m_tSettings.m_sModelName.length(), m_tSettings.m_sCachePath.c_str(), m_tSettings.m_sCachePath.length(), m_tSettings.m_sAPIKey.c_str(), m_tSettings.m_sAPIKey.length(), m_tSettings.m_bUseGPU );
	if ( tResult.m_szError )
	{
		sError = tResult.m_szError;
		g_pLibFuncs->free_model_result(tResult);
		return false;
	}

	m_pModel = tResult.m_pModel;
	m_pLib->AddModel ( ToKey(m_tSettings), m_pModel );
	return true;
}


bool TextToEmbeddings_c::Convert ( std::string_view sText, std::vector<float> & dEmbedding, std::string & sError ) const
{
	assert(g_pLibFuncs);

	FloatVecResult tVecResult = g_pLibFuncs->make_vect_embeddings ( &m_pModel, sText.data(), sText.length() );
	if ( tVecResult.m_szError )
	{
		sError = tVecResult.m_szError;
		g_pLibFuncs->free_vec_result(tVecResult);
		return false;
	}

	size_t tLen = tVecResult.m_tEmbedding.len;
	dEmbedding.resize(tLen);
	memcpy ( dEmbedding.data(), tVecResult.m_tEmbedding.ptr, sizeof(float)*tLen );
	g_pLibFuncs->free_vec_result(tVecResult);

	return true;
}


int	TextToEmbeddings_c::GetDims() const
{
	assert(g_pLibFuncs);
	return g_pLibFuncs->get_hidden_size ( &m_pModel );
}


knn::TextToEmbeddings_i * CreateTextToEmbeddings ( const std::string & sLibPath, const ModelSettings_t & tSettings, std::string & sError )
{
	std::unique_ptr<TextToEmbeddings_c> pBuilder = std::make_unique<TextToEmbeddings_c>(tSettings);
	if ( !pBuilder->Initialize ( sLibPath, sError ) )
		pBuilder.reset();

	return pBuilder.release();
}

} // namespace knn
