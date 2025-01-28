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

class LoadedLib_c
{
public:
						LoadedLib_c ( const std::string sLibPath ) : m_sLibPath(sLibPath) {}
						~LoadedLib_c();

	bool				Initialize ( std::string & sError );

	void				AddModel ( const std::string & sKey, TextModelWrapper pModel ) { m_hModels.insert ( { sKey, pModel } ); }
	TextModelWrapper	GetModel ( const std::string & sKey ) const;
	const EmbedLib *	GetLibFuncs() const { return m_pLibFuncs; }

private:
	std::string			m_sLibPath;
	void *				m_pHandle = nullptr;
	std::unordered_map<std::string, TextModelWrapper> m_hModels;
	const EmbedLib *	m_pLibFuncs = nullptr;
};


bool LoadedLib_c::Initialize ( std::string & sError )
{
	m_pHandle = dlopen ( m_sLibPath.c_str(), RTLD_LAZY | RTLD_LOCAL );
	if ( !m_pHandle )
	{
		const char * szDlError = dlerror();
		sError = util::FormatStr ( "dlopen() failed: %s", szDlError ? szDlError : "(null)" );
		return false;
	}

	GetLibFuncs_fn fnGetLibFuncs;
	if ( !LoadFunc ( fnGetLibFuncs, m_pHandle, "GetLibFuncs", m_sLibPath.c_str(), sError ) )
		return false;

	m_pLibFuncs = fnGetLibFuncs();
	if ( !m_pLibFuncs )
	{
		sError = "Error initializing embeddings library";
		return false;
	}

	return true;
}

LoadedLib_c::~LoadedLib_c()
{
	if ( !m_pHandle )
		return;

	if ( m_pLibFuncs )
		for ( auto i : m_hModels )
		{
			TextModelResult	tResult = { i.second };
			m_pLibFuncs->free_model_result(tResult);
		}

	dlclose(m_pHandle);
}


TextModelWrapper LoadedLib_c::GetModel ( const std::string & sKey ) const
{
	const auto & tFound = m_hModels.find(sKey);
	return tFound==m_hModels.end() ? nullptr : tFound->second;
}

///////////////////////////////////////////////////////////////////////////////

class EmbeddingsLib_c : public EmbeddingsLib_i
{
public:
			EmbeddingsLib_c ( const std::string & sLibPath ) : m_sLibPath ( sLibPath ) {}

	bool	Load ( std::string & sError );

	TextToEmbeddings_i * CreateTextToEmbeddings ( const knn::ModelSettings_t & tSettings, std::string & sError ) const override;
	const std::string &	GetVersionStr() const override	{ return m_sVersion; }
	int		GetVersion() const override					{ return m_iVersion; }

private:
	std::shared_ptr<LoadedLib_c> m_pLib;
	std::string			m_sLibPath;
	int					m_iVersion = 0;
	std::string			m_sVersion;
};


bool EmbeddingsLib_c::Load ( std::string & sError )
{
	m_pLib = std::make_shared<LoadedLib_c>(m_sLibPath);
	if ( !m_pLib->Initialize(sError) )
		return false;

	auto * pFuncs = m_pLib->GetLibFuncs();
	assert(pFuncs);

	m_iVersion = int(pFuncs->version);
	m_sVersion = pFuncs->version_str;
	return true;
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

	bool	Initialize ( std::shared_ptr<LoadedLib_c> pLib, std::string & sError );
	bool	Convert ( const std::vector<std::string_view> & dTexts, std::vector<std::vector<float>> & dEmbeddings, std::string & sError ) const override;
	int		GetDims() const override;

private:
	ModelSettings_t		m_tSettings;
	std::shared_ptr<LoadedLib_c> m_pLib;
	TextModelWrapper	m_pModel = nullptr;
};


bool TextToEmbeddings_c::Initialize ( std::shared_ptr<LoadedLib_c> pLib, std::string & sError )
{
	assert ( !m_pModel && pLib );

	m_pLib = pLib;
	m_pModel = m_pLib->GetModel ( ToKey(m_tSettings) );
	if ( m_pModel )
		return true;

	auto * pFuncs = m_pLib->GetLibFuncs();
	assert(pFuncs);

	TextModelResult tResult = pFuncs->load_model ( m_tSettings.m_sModelName.c_str(), m_tSettings.m_sModelName.length(), m_tSettings.m_sCachePath.c_str(), m_tSettings.m_sCachePath.length(), m_tSettings.m_sAPIKey.c_str(), m_tSettings.m_sAPIKey.length(), m_tSettings.m_bUseGPU );
	if ( tResult.m_szError )
	{
		sError = tResult.m_szError;
		pFuncs->free_model_result(tResult);
		return false;
	}

	m_pModel = tResult.m_pModel;
	m_pLib->AddModel ( ToKey(m_tSettings), m_pModel );
	return true;
}


bool TextToEmbeddings_c::Convert ( const std::vector<std::string_view> & dTexts, std::vector<std::vector<float>> & dEmbeddings, std::string & sError ) const
{
	std::vector<StringItem> dStringItems;
	for ( const auto & i : dTexts )
		dStringItems.push_back ( { i.data(), i.length() } );

	auto * pFuncs = m_pLib->GetLibFuncs();
	assert(pFuncs);

	FloatVecResult tVecResult = pFuncs->make_vect_embeddings ( &m_pModel, dStringItems.data(), dStringItems.size() );
	if ( tVecResult.m_szError )
	{
		sError = tVecResult.m_szError;
		pFuncs->free_vec_result(tVecResult);
		return false;
	}

	dEmbeddings.resize ( tVecResult.len );
	for ( size_t i = 0; i < tVecResult.len; i++ )
	{
		const FloatVec & tVec = tVecResult.m_tEmbedding[i];
		dEmbeddings[i].resize ( tVec.len );
		memcpy ( dEmbeddings[i].data(), tVec.ptr, sizeof(float)*tVec.len );
	}

	pFuncs->free_vec_result(tVecResult);

	return true;
}


int	TextToEmbeddings_c::GetDims() const
{
	auto * pFuncs = m_pLib->GetLibFuncs();
	assert(pFuncs);
	return pFuncs->get_hidden_size ( &m_pModel );
}


TextToEmbeddings_i * EmbeddingsLib_c::CreateTextToEmbeddings ( const knn::ModelSettings_t & tSettings, std::string & sError ) const
{
	std::unique_ptr<TextToEmbeddings_c> pBuilder = std::make_unique<TextToEmbeddings_c>(tSettings);
	if ( !pBuilder->Initialize ( m_pLib, sError ) )
		pBuilder.reset();

	return pBuilder.release();
}

///////////////////////////////////////////////////////////////////////////////

knn::EmbeddingsLib_i * LoadEmbeddingsLib ( const std::string & sLibPath, std::string & sError )
{
	std::unique_ptr<EmbeddingsLib_c> pLib = std::make_unique<EmbeddingsLib_c>(sLibPath);
	if ( !pLib->Load(sError) )
		return nullptr;

	const int SUPPORTED_EMBEDDINGS_LIB_VER = 1;
	if ( pLib->GetVersion()!=SUPPORTED_EMBEDDINGS_LIB_VER )
	{
		sError = util::FormatStr ( "Unsupported embeddings library version %d (expected %d)", pLib->GetVersion(), SUPPORTED_EMBEDDINGS_LIB_VER );
		return nullptr;
	}

	return pLib.release();
}

} // namespace knn
