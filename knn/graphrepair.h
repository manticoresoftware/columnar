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

// This file is NOT a part of the common headers (API). It is internal to the knn library and is
// not reachable through knn.h, so changes here do not require a LIB_VERSION bump.

#pragma once

#include "hnswlib.h"

#include <cstdint>

namespace knn
{

struct RepairStats_t
{
	uint64_t	m_uNodes = 0;				// cur_element_count on entry
	uint64_t	m_uOrphansFound = 0;		// level-0 nodes not reachable from the entry point
	uint64_t	m_uAppends = 0;				// relinked into a free slot
	uint64_t	m_u2HopWidenings = 0;		// relinked via the bounded 2-hop search
	uint64_t	m_uEvictions = 0;			// relinked by replacing a non-tree edge
	uint64_t	m_uDistanceCalls = 0;		// distance evaluations performed by the pass
	uint64_t	m_uUnreachableAfter = 0;	// still unreachable after a full re-traversal
	uint64_t	m_uRuns = 0;				// how many times the pass ran (one per KNN attribute)
};

// Reconnects level-0 nodes that HNSW construction left with no incoming edge.
//
// hnswlib's mutuallyConnectNewElement can leave a node with zero in-edges: when a neighbour's link
// list is full it re-runs getNeighborsByHeuristic2 over the existing links plus the new node, and
// that heuristic may drop the new node outright or return fewer entries than before.
// The affected row stays in the index, but no knn() query can reach it.
//
// This pass is best effort: it never fails a build. A node it cannot relink is left exactly as it
// was, which is no worse than not running at all. 
//
// MUST be called before ScalarQuantizer_i::FinalizeEncoding(). uFullThreshold is maxM0_ in production;
// a test passes a smaller value to force the eviction branch. pStats may be null.
void					RepairGraphConnectivity ( hnswlib::HierarchicalNSW<float> & tAlg );

/// Test seam for a build driven through the public Builder_i API, where the caller cannot reach the
/// private HNSWIndexBuilder_c::m_pAlg. All thread-local; a threshold of 0 restores the default
/// (maxM0_).
///
/// The stats ACCUMULATE across runs and count them in m_uRuns, because one Save covers every KNN
/// attribute in the index and each attribute repairs its own graph. Reporting only the last run
/// would let a test pass while earlier attributes were skipped entirely. Call
/// Test_ResetRepairStats() before a build and check m_uRuns against the attribute count.
void					Test_SetRepairFullThreshold ( uint32_t uThreshold );
void					Test_ResetRepairStats();
const RepairStats_t &	Test_GetRepairStats();

} // namespace knn
