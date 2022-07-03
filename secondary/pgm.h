// Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

#ifndef _sipgm_
#define _sipgm_

#include "common.h"
#include "reader.h"
#include "pgm/pgm_index.hpp"

namespace SI
{
	struct ApproxPos_t
	{
		size_t m_iPos; ///< The approximate position of the key.
		size_t m_iLo;  ///< The lower bound of the range.
		size_t m_iHi;  ///< The upper bound of the range.
	};

	class PGM_i
	{
	public:
		virtual size_t Save ( std::vector<uint8_t> & dData ) const = 0;
		virtual void Load ( util::FileReader_c & tRd ) = 0;
		virtual ApproxPos_t Search ( uint64_t uVal ) const = 0;
	};

	template <typename VALUE>
	struct PGM_T : public pgm::PGMIndex<VALUE>, public PGM_i 
	{
		PGM_T ()
		{
		}

		template<typename RandomIt>
		PGM_T ( RandomIt tBegin, RandomIt tEnd )
			: pgm::PGMIndex<VALUE> ( tBegin, tEnd )
		{
		}

		size_t Save ( std::vector<uint8_t> & dData ) const final
		{
			size_t uOff = dData.size();

			util::MemWriter_c tWr ( dData );

			tWr.Pack_uint32 ( (int)this->n );
			WriteTypedKey ( tWr, this->first_key );

			tWr.Pack_uint32 ( (int)this->segments.size() );
			for ( const auto & tSeg : this->segments )
			{
				WriteTypedKey ( tWr, tSeg.key );
				tWr.Pack_uint32 ( util::FloatToUint ( tSeg.slope ) );
				tWr.Pack_uint32 ( tSeg.intercept );
			}

			tWr.Pack_uint32 ( (int)this->levels_sizes.size() );
			for ( const size_t & tLvl : this->levels_sizes )
				tWr.Pack_uint64 ( tLvl );

			tWr.Pack_uint32 ( (int)this->levels_offsets.size() );
			for ( const size_t & tOff : this->levels_offsets )
				tWr.Pack_uint64 ( tOff );

			return uOff;
		}

		void Load ( util::FileReader_c & tRd ) final
		{
			this->n = tRd.Unpack_uint32();
			LoadTypedKey ( tRd, this->first_key );

			this->segments.resize ( tRd.Unpack_uint32() );
			for ( auto & tSeg : this->segments )
			{
				LoadTypedKey ( tRd, tSeg.key );
				tSeg.slope = util::UintToFloat ( tRd.Unpack_uint32() );
				tSeg.intercept = tRd.Unpack_uint32();
			}

			this->levels_sizes.resize ( tRd.Unpack_uint32() );
			for ( size_t & tLvl : this->levels_sizes )
				tLvl = tRd.Unpack_uint64();

			this->levels_offsets.resize ( tRd.Unpack_uint32() );
			for ( size_t & tOff : this->levels_offsets )
				tOff = tRd.Unpack_uint64();
		}

		ApproxPos_t Search ( uint64_t uVal ) const final;

		void WriteTypedKey ( util::MemWriter_c & tWr, VALUE tVal ) const
		{
			tWr.Pack_uint64 ( (uint64_t)tVal );
		}

		void LoadTypedKey ( util::FileReader_c & tRd, VALUE  & tVal ) const
		{
			tVal = tRd.Unpack_uint64();
		}
	};

	template<>
	inline void PGM_T<float>::WriteTypedKey ( util::MemWriter_c & tWr, float tVal ) const
	{
		tWr.Pack_uint32 ( util::FloatToUint ( tVal ) );
	}

	template<>
	inline void PGM_T<float>::LoadTypedKey ( util::FileReader_c & tRd, float & tVal ) const
	{
		tVal = util::UintToFloat ( tRd.Unpack_uint32() );
	}

	static ApproxPos_t GetPos ( pgm::ApproxPos tPos )
	{
		ApproxPos_t tIt;
		tIt.m_iPos = tPos.pos;
		tIt.m_iLo = tPos.lo;
		tIt.m_iHi = tPos.hi;
		return tIt;
	}

	template<>
	inline ApproxPos_t PGM_T<uint32_t>::Search ( uint64_t uVal ) const
	{
		return GetPos ( this->search ( (uint32_t)uVal ) );
	}

	template<>
	inline ApproxPos_t PGM_T<uint64_t>::Search ( uint64_t uVal ) const
	{
		return GetPos ( this->search ( uVal ) );
	}

	template<>
	inline ApproxPos_t PGM_T<int64_t>::Search ( uint64_t uVal ) const
	{
		return GetPos ( this->search ( (int64_t)uVal ) );
	}

	template<>
	inline ApproxPos_t PGM_T<float>::Search ( uint64_t uVal ) const
	{
		return GetPos ( this->search ( util::UintToFloat ( uVal ) ) );
	}

} // namespace SI


#endif // _sipgm_