// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

#include "builderstr.h"
#include "buildertraits.h"

#include "memory"

namespace columnar
{

static const uint64_t HASH_SEED = 0xCBF29CE484222325ULL;

class AttributeHeaderBuilder_String_c : public AttributeHeaderBuilder_c
{
	using BASE = AttributeHeaderBuilder_c;
	using BASE::BASE;

public:
	bool	Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );
	void	SetHashFlag ( bool bSet ) { m_bHaveHashes = bSet; }
	bool	HaveStringHashes() const { return m_bHaveHashes; }

protected:
	bool	m_bHaveHashes = false;
};


bool AttributeHeaderBuilder_String_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	if ( !BASE::Save ( tWriter, tBaseOffset, sError ) )
		return false;

	tWriter.Write_uint8 ( m_bHaveHashes ? 1 : 0 );

	return !tWriter.IsError();
}

// uses 2 types of string length packing:
// 1. string lengths are constant. store length once
// 2. string lengths are different. pack monotonic increasing offsets
class Packer_String_c : public PackerTraits_T<AttributeHeaderBuilder_String_c>
{
	using BASE = PackerTraits_T<AttributeHeaderBuilder_String_c>;

public:
							Packer_String_c ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc );

	void					AddDoc ( int64_t tAttr ) final;
	void					AddDoc ( const uint8_t * pData, int iLength ) final;
	void					AddDoc ( const int64_t * pData, int iLength ) final;

protected:
	std::vector<std::string> m_dCollected;
	std::vector<uint64_t>	m_dOffsets;
	StringHash_fn			m_fnHashCalc = nullptr;

	void					Flush() override;
	virtual StrPacking_e	ChoosePacking() const;
	virtual void			WriteToFile ( StrPacking_e ePacking );

	template <typename T1, typename T2>
	void					WritePacked_Delta ( T1 && fnWriteSubBlock, T2 && fnWriteOffsets );

private:
	int						m_iConstLength = -1;
	std::vector<uint8_t>	m_dTmpBuffer;
	std::vector<uint64_t>	m_dTmpLengths;

	void					AnalyzeCollected ( const uint8_t * pData, int iLength );
	void					WritePacked_Const();

	template <typename WRITER>
	void					WriteHashes ( int iBlockStart, int iBlockValues, WRITER & tWriter );
};


Packer_String_c::Packer_String_c ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc )
	: BASE ( tSettings, sName, AttrType_e::STRING )
	, m_fnHashCalc ( fnHashCalc )
{
	m_tHeader.SetHashFlag ( !!fnHashCalc );
}


void Packer_String_c::AddDoc ( int64_t tAttr )
{
	assert ( 0 && "INTERNAL ERROR: sending integers to string packer" );
}


void Packer_String_c::AddDoc ( const uint8_t * pData, int iLength )
{
	if ( m_dCollected.size()==DOCS_PER_BLOCK )
		Flush();

	AnalyzeCollected ( pData, iLength );
	m_dCollected.push_back ( std::string ( (const char*)pData, iLength ) );
}


void Packer_String_c::AddDoc ( const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending MVA to string packer" );
}


void Packer_String_c::Flush()
{
	if ( m_dCollected.empty() )
		return;

	m_tHeader.AddBlock ( m_tWriter.GetPos() );

	WriteToFile ( ChoosePacking() );

	m_dCollected.resize(0);
	m_iConstLength = -1;
}


void Packer_String_c::AnalyzeCollected ( const uint8_t * pData, int iLength )
{
	if ( m_dCollected.empty() )
		m_iConstLength = iLength;
	else if ( iLength!=m_iConstLength )
		m_iConstLength = -1;
}


StrPacking_e Packer_String_c::ChoosePacking() const
{
	if ( m_iConstLength!=-1 )
		return StrPacking_e::CONSTLEN;

	return StrPacking_e::DELTA;
}


void Packer_String_c::WriteToFile ( StrPacking_e ePacking )
{
	m_tWriter.Pack_uint32 ( to_underlying(ePacking) );

	switch ( ePacking )
	{
	case StrPacking_e::CONSTLEN:
		WritePacked_Const();
		break;

	case StrPacking_e::DELTA:
		assert ( 0 && "Packing should be implemented in descendants");
		break;

	default:
		assert ( 0 && "Unknown packing" );
		break;
	}
}


void Packer_String_c::WritePacked_Const()
{
	assert ( m_iConstLength>=0 );
	m_tWriter.Pack_uint32 ( m_iConstLength );

	WriteHashes ( 0, (int)m_dCollected.size(), m_tWriter );

	for ( const auto & i : m_dCollected )
		m_tWriter.Write ( (const uint8_t*)i.c_str(), i.length() );
}


template <typename WRITER>
void Packer_String_c::WriteHashes ( int iBlockStart, int iBlockValues, WRITER & tWriter )
{
	if ( !m_tHeader.HaveStringHashes() )
		return;

	assert(m_fnHashCalc);

	for ( int i = 0; i<iBlockValues; i++ )
	{
		const std::string & sCollected = m_dCollected[iBlockStart+i];
		int iLen = (int)sCollected.length();
		uint64_t uHash = 0;
		if ( iLen>0 )
			uHash = m_fnHashCalc ( (const uint8_t*)sCollected.c_str(), iLen, HASH_SEED );

		tWriter.Write_uint64(uHash);
	}
}


template <typename T1, typename T2>
void Packer_String_c::WritePacked_Delta ( T1 && fnWriteSubBlock, T2 && fnWriteOffsets )
{
	// internal arrangement: [packed_length0]...[packed_lengthN][data0]...[dataN]

	int iSubblockSize = m_tHeader.GetSettings().m_iSubblockSize;
	int iBlocks = ( (int)m_dCollected.size() + iSubblockSize - 1 ) / iSubblockSize;

	m_dOffsets.resize(iBlocks);
	m_dTmpBuffer.resize(0);

	MemWriter_c tMemWriter ( m_dTmpBuffer );

	int iBlockStart = 0;
	for ( int iBlock=0; iBlock < (int)m_dOffsets.size(); iBlock++ )
	{
		int iBlockValues = GetSubblockSize ( iBlock, iBlocks, (int)m_dCollected.size(), iSubblockSize );
		m_dOffsets[iBlock] = tMemWriter.GetPos();

		WriteHashes ( iBlockStart, iBlockValues, tMemWriter );

		// write lengths
		m_dTmpLengths.resize(iBlockValues);
		m_dTmpLengths[0] = m_dCollected[iBlockStart].size();
		for ( int i = 1; i<iBlockValues; i++ )
			m_dTmpLengths[i] = m_dTmpLengths[i-1] + m_dCollected[iBlockStart+i].size();

		fnWriteSubBlock ( Span_T<uint64_t>(m_dTmpLengths), tMemWriter );

		// write bodies
		for ( int i = 0; i<iBlockValues; i++ )
		{
			const std::string & sCollected = m_dCollected[iBlockStart+i];
			tMemWriter.Write ( (const uint8_t*)sCollected.c_str(), sCollected.length() );
		}

		iBlockStart += iBlockValues;
	}

	assert ( !m_dOffsets[0] );
	fnWriteOffsets ( m_dOffsets );

	m_tWriter.Write ( m_dTmpBuffer.data(), m_dTmpBuffer.size()*sizeof ( m_dTmpBuffer[0] ) );
}

//////////////////////////////////////////////////////////////////////////

class Packer_String_PFOR_c : public Packer_String_c
{
	using BASE = Packer_String_c;

public:
					Packer_String_PFOR_c ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc );

protected:
	StrPacking_e	ChoosePacking() const override;
	void			WriteToFile ( StrPacking_e ePacking ) override;

private:
	std::vector<uint64_t>		m_dUncompressed;
	std::vector<uint32_t>		m_dCompressed;
	std::unique_ptr<IntCodec_i>	m_pCodec;
	std::vector<uint8_t>		m_dTmpBuffer2;

	void			WriteOffsets_PFOR ( std::vector<uint64_t> & dOffsets );
};


Packer_String_PFOR_c::Packer_String_PFOR_c ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc )
	: BASE ( tSettings, sName, fnHashCalc )
	, m_pCodec ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) )
{}


StrPacking_e Packer_String_PFOR_c::ChoosePacking() const
{
	StrPacking_e ePacking = BASE::ChoosePacking();
	if ( ePacking==StrPacking_e::DELTA )
		return StrPacking_e::DELTA_PFOR;

	return ePacking;
}


void Packer_String_PFOR_c::WriteToFile ( StrPacking_e ePacking )
{
	if ( ePacking==StrPacking_e::DELTA_PFOR )
	{
		m_tWriter.Pack_uint32 ( to_underlying(ePacking) );
		WritePacked_Delta (
			[this]( const Span_T<uint64_t> & dSubblockValues, MemWriter_c & tWriter ) { WriteValues_Delta_PFOR ( dSubblockValues, m_dUncompressed, m_dCompressed, tWriter, m_pCodec.get() ); },
			[this]( std::vector<uint64_t> & dOffsets ) { WriteOffsets_PFOR(dOffsets); }
		);
	}
	else
		BASE::WriteToFile(ePacking);
}


void Packer_String_PFOR_c::WriteOffsets_PFOR ( std::vector<uint64_t> & dOffsets )
{
	// write sub-block sizes to a in-memory buffer
	m_dTmpBuffer2.resize(0);
	MemWriter_c tMemWriterOffsets(m_dTmpBuffer2);

	WriteValues_Delta_PFOR ( Span_T<uint64_t>(dOffsets), m_dUncompressed, m_dCompressed, tMemWriterOffsets, m_pCodec.get() );

	// write compressed offsets
	m_tWriter.Write ( m_dTmpBuffer2.data(), m_dTmpBuffer2.size()*sizeof ( m_dTmpBuffer2[0] ) );
}

//////////////////////////////////////////////////////////////////////////

Packer_i * CreatePackerStr ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc )
{
	return new Packer_String_PFOR_c ( tSettings, sName, fnHashCalc );
}

} // namespace columnar