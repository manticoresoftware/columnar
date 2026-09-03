// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
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

#include "graphrepair.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <vector>

namespace knn
{

namespace
{

using HNSWAlg_t = hnswlib::HierarchicalNSW<float>;

static const hnswlib::tableint INVALID_NODE = std::numeric_limits<hnswlib::tableint>::max();

// level-0 neighbour view of one node
struct LinkList_t
{
	hnswlib::linklistsizeint *	m_pHeader = nullptr;
	hnswlib::tableint *			m_pLinks = nullptr;
	uint32_t					m_uCount = 0;
};


inline LinkList_t GetLinks0 ( const HNSWAlg_t & tAlg, hnswlib::tableint tId )
{
	LinkList_t tRes;
	tRes.m_pHeader	= tAlg.get_linklist0(tId);
	tRes.m_pLinks	= (hnswlib::tableint*)( tRes.m_pHeader+1 );
	tRes.m_uCount	= tAlg.getListCount ( tRes.m_pHeader );
	return tRes;
}


// Distance with tFrom as the query and tTo as the document
inline float RepairDist ( const HNSWAlg_t & tAlg, hnswlib::tableint tFrom, hnswlib::tableint tTo, uint64_t & uDistCalls )
{
	uDistCalls++;
	return tAlg.calcDistanceConstruction<> ( tAlg.getDataByInternalId(tFrom), tTo, tAlg.getExternalLabel(tFrom) );
}


// Appends into a free slot
inline void AppendLink ( const HNSWAlg_t & tAlg, hnswlib::tableint tTarget, hnswlib::tableint tNew )
{
	LinkList_t tLL = GetLinks0 ( tAlg, tTarget );
	assert ( tLL.m_uCount < tAlg.maxM0_ );
	tLL.m_pLinks[tLL.m_uCount] = tNew;
	tAlg.setListCount ( tLL.m_pHeader, (unsigned short)( tLL.m_uCount+1 ) );
}


class NodeBitmap_c
{
public:
	void Resize ( uint64_t uSize )		{ m_dBits.assign ( (size_t)( ( uSize+63 ) >> 6 ), 0 ); }
	bool Get ( uint64_t uIdx ) const	{ return ( ( m_dBits [ (size_t)( uIdx>>6 ) ] >> ( uIdx & 63 ) ) & 1ULL )!=0; }
	void Set ( uint64_t uIdx )			{ m_dBits [ (size_t)( uIdx>>6 ) ] |= 1ULL << ( uIdx & 63 );	}

private:
	std::vector<uint64_t> m_dBits;
};


struct Candidate_t
{
	float				m_fDist = 0.0f;
	hnswlib::tableint	m_tId = INVALID_NODE;

	bool operator < ( const Candidate_t & tOther ) const
	{
		if ( m_fDist!=tOther.m_fDist )
			return m_fDist < tOther.m_fDist;

		return m_tId < tOther.m_tId;
	}
};


// Marks everything reachable from tSeed over directed level-0 links and returns how many nodes it newly marked
// When pParent is set it also records, for every newly reached node, the node whose edge reached it, i.e. a spanning tree rooted at tSeed
uint64_t MarkReachable ( const HNSWAlg_t & tAlg, hnswlib::tableint tSeed, uint64_t uCount, NodeBitmap_c & tSeen, std::vector<hnswlib::tableint> & dStack, std::vector<hnswlib::tableint> * pParent )
{
	if ( tSeed>=uCount || tSeen.Get(tSeed) )
		return 0;

	tSeen.Set(tSeed);
	uint64_t uMarked = 1;

	dStack.resize(0);
	dStack.push_back(tSeed);

	while ( !dStack.empty() )
	{
		hnswlib::tableint tCur = dStack.back();
		dStack.pop_back();

		LinkList_t tLL = GetLinks0 ( tAlg, tCur );
		for ( uint32_t i=0; i<tLL.m_uCount; i++ )
		{
			hnswlib::tableint tNext = tLL.m_pLinks[i];
			if ( tNext>=uCount || tSeen.Get(tNext) )
				continue;

			tSeen.Set(tNext);
			if ( pParent )
				(*pParent)[tNext] = tCur;

			uMarked++;
			dStack.push_back(tNext);
		}
	}

	return uMarked;
}

// First candidate with a free slot; candidates are already ordered nearest-first
hnswlib::tableint FindFreeSlot ( const HNSWAlg_t & tAlg, const std::vector<Candidate_t> & dCand, uint32_t uFullThreshold )
{
	for ( const auto & tCand : dCand )
		if ( GetLinks0 ( tAlg, tCand.m_tId ).m_uCount < uFullThreshold )
			return tCand.m_tId;

	return INVALID_NODE;
}

// Ranks ids by distance from tOrphan and replaces dCand with the result
void RankCandidates ( const HNSWAlg_t & tAlg, hnswlib::tableint tOrphan, std::vector<hnswlib::tableint> & dIds, std::vector<Candidate_t> & dCand, uint64_t & uDistCalls )
{
	std::sort ( dIds.begin(), dIds.end() );
	dIds.erase ( std::unique ( dIds.begin(), dIds.end() ), dIds.end() );

	dCand.resize(0);
	dCand.reserve ( dIds.size() );
	for ( hnswlib::tableint tId : dIds )
		dCand.push_back ( { RepairDist ( tAlg, tOrphan, tId, uDistCalls ), tId } );

	std::sort ( dCand.begin(), dCand.end() );
}

static thread_local uint32_t g_uRepairFullThreshold = 0;
static thread_local RepairStats_t g_tRepairStats;

} // anonymous namespace


static void RepairGraphConnectivity ( HNSWAlg_t & tAlg, uint32_t uFullThreshold, RepairStats_t * pStats )
{
	RepairStats_t tStats;
	const uint64_t uCount = tAlg.cur_element_count;
	tStats.m_uNodes = uCount;

	// rows with an empty vector attribute never reach addPoint, so cur_element_count can be less than max_elements_
	if ( uCount<2 )
	{
		if ( pStats )
			*pStats = tStats;

		return;
	}

	const hnswlib::tableint tEntry = tAlg.enterpoint_node_;
	if ( tEntry>=uCount )	// -1 on an empty index
	{
		if ( pStats )
			*pStats = tStats;

		return;
	}

	const uint32_t uMaxM0 = (uint32_t)tAlg.maxM0_;
	if ( !uFullThreshold || uFullThreshold>uMaxM0 )
		uFullThreshold = uMaxM0;

	NodeBitmap_c tSeen;
	tSeen.Resize(uCount);

	std::vector<hnswlib::tableint> dStack;
	dStack.reserve ( (size_t)std::min<uint64_t> ( uCount, 1<<16 ) );

	uint64_t uMarked = MarkReachable ( tAlg, tEntry, uCount, tSeen, dStack, nullptr );
	if ( uMarked==uCount )		// healthy index
	{
		if ( pStats )
			*pStats = tStats;

		return;
	}

	std::vector<hnswlib::tableint> dOrphans;
	for ( uint64_t i=0; i<uCount; i++ )
		if ( !tSeen.Get(i) )
			dOrphans.push_back ( (hnswlib::tableint)i );

	tStats.m_uOrphansFound = dOrphans.size();

	// A spanning tree is what makes eviction safe, but it costs 4 bytes per node, so build it only if an eviction is actually needed
	std::vector<hnswlib::tableint> dParent;
	auto fnEnsureParents = [&]
	{
		if ( !dParent.empty() )
			return;

		dParent.assign ( (size_t)uCount, INVALID_NODE );
		NodeBitmap_c tTreeSeen;
		tTreeSeen.Resize(uCount);
		std::vector<hnswlib::tableint> dTreeStack;
		MarkReachable ( tAlg, tEntry, uCount, tTreeSeen, dTreeStack, &dParent );
	};

	std::vector<hnswlib::tableint> dIds;
	std::vector<Candidate_t> dCand;

	for ( hnswlib::tableint tOrphan : dOrphans )
	{
		if ( tSeen.Get(tOrphan) )	// already reconnected as part of an earlier orphan's component
			continue;

		// 1-hop: this orphan's own reachable out-links, nearest first
		LinkList_t tOrphanLinks = GetLinks0 ( tAlg, tOrphan );
		dIds.resize(0);
		for ( uint32_t i=0; i<tOrphanLinks.m_uCount; i++ )
		{
			hnswlib::tableint tN = tOrphanLinks.m_pLinks[i];
			if ( tN<uCount && tN!=tOrphan && tSeen.Get(tN) )
				dIds.push_back(tN);
		}

		RankCandidates ( tAlg, tOrphan, dIds, dCand, tStats.m_uDistanceCalls );
		hnswlib::tableint tTarget = FindFreeSlot ( tAlg, dCand, uFullThreshold );
		bool bWidened = false;

		if ( tTarget==INVALID_NODE )
		{
			// widen once to a bounded 2-hop neighbourhood (<= maxM0^2 nodes). Deliberately not a
			// scan of the whole reachable set, which would make the pass O(orphans * N).
			for ( uint32_t i=0; i<tOrphanLinks.m_uCount; i++ )
			{
				hnswlib::tableint tN = tOrphanLinks.m_pLinks[i];
				if ( tN>=uCount )
					continue;

				LinkList_t tHop = GetLinks0 ( tAlg, tN );
				for ( uint32_t j=0; j<tHop.m_uCount; j++ )
				{
					hnswlib::tableint tN2 = tHop.m_pLinks[j];
					if ( tN2<uCount && tN2!=tOrphan && tSeen.Get(tN2) )
						dIds.push_back(tN2);
				}
			}

			RankCandidates ( tAlg, tOrphan, dIds, dCand, tStats.m_uDistanceCalls );
			tTarget = FindFreeSlot ( tAlg, dCand, uFullThreshold );
			bWidened = tTarget!=INVALID_NODE;
		}

		if ( tTarget==INVALID_NODE && GetLinks0 ( tAlg, tEntry ).m_uCount<uFullThreshold )
			tTarget = tEntry;	// the entry point is reachable by definition

		if ( tTarget!=INVALID_NODE )
		{
			AppendLink ( tAlg, tTarget, tOrphan );
			tStats.m_uAppends++;
			if ( bWidened )
				tStats.m_u2HopWidenings++;
		}
		else
		{
			// Last resort: replace an edge. An in-degree test would NOT be safe here as a node
			// reachable only through the victim still counts toward its in-degree - so evict only
			// edges that are not part of the spanning tree. Removing a non-tree edge leaves the tree intact.
			fnEnsureParents();

			// the entry point is always a legal target, and it is the only one available when the
			// orphan's own neighbourhood yielded nothing reachable
			dIds.resize(0);
			for ( const auto & tCand : dCand )
				dIds.push_back ( tCand.m_tId );

			dIds.push_back(tEntry);

			bool bEvicted = false;
			for ( hnswlib::tableint tCandId : dIds )
			{
				LinkList_t tLL = GetLinks0 ( tAlg, tCandId );
				uint32_t uVictim = UINT32_MAX;
				float fWorst = -std::numeric_limits<float>::max();

				for ( uint32_t i=0; i<tLL.m_uCount; i++ )
				{
					hnswlib::tableint tY = tLL.m_pLinks[i];
					if ( tY>=uCount || tY==tEntry || tY==tOrphan || dParent[tY]==tCandId )
						continue;

					float fDist = RepairDist ( tAlg, tY, tCandId, tStats.m_uDistanceCalls );	// neighbour is the query
					if ( fDist>fWorst )
					{
						fWorst = fDist;
						uVictim = i;
					}
				}

				if ( uVictim==UINT32_MAX )
					continue;

				tLL.m_pLinks[uVictim] = tOrphan;	// count is unchanged, header untouched
				tTarget = tCandId;
				tStats.m_uEvictions++;
				bEvicted = true;
				break;
			}

			if ( !bEvicted )
				continue;	// best effort: leave this one alone, it is no worse than before
		}

		if ( !dParent.empty() )
			dParent[tOrphan] = tTarget;

		uMarked += MarkReachable ( tAlg, tOrphan, uCount, tSeen, dStack, dParent.empty() ? nullptr : &dParent );
	}

	// Re-traverse the mutated graph from scratch
	NodeBitmap_c tFinalSeen;
	tFinalSeen.Resize(uCount);
	std::vector<hnswlib::tableint> dFinalStack;
	uint64_t uFinalMarked = MarkReachable ( tAlg, tEntry, uCount, tFinalSeen, dFinalStack, nullptr );
	tStats.m_uUnreachableAfter = uCount - uFinalMarked;

	if ( pStats )
		*pStats = tStats;
}


void RepairGraphConnectivity ( HNSWAlg_t & tAlg )
{
	RepairStats_t tStats;
	RepairGraphConnectivity ( tAlg, g_uRepairFullThreshold, &tStats );

	g_tRepairStats.m_uNodes				+= tStats.m_uNodes;
	g_tRepairStats.m_uOrphansFound		+= tStats.m_uOrphansFound;
	g_tRepairStats.m_uAppends			+= tStats.m_uAppends;
	g_tRepairStats.m_u2HopWidenings		+= tStats.m_u2HopWidenings;
	g_tRepairStats.m_uEvictions			+= tStats.m_uEvictions;
	g_tRepairStats.m_uDistanceCalls		+= tStats.m_uDistanceCalls;
	g_tRepairStats.m_uUnreachableAfter	+= tStats.m_uUnreachableAfter;
	g_tRepairStats.m_uRuns++;
}


void Test_SetRepairFullThreshold ( uint32_t uThreshold )
{
	g_uRepairFullThreshold = uThreshold;
}


void Test_ResetRepairStats()
{
	g_tRepairStats = RepairStats_t();
}


const RepairStats_t & Test_GetRepairStats()
{
	return g_tRepairStats;
}

} // namespace knn
