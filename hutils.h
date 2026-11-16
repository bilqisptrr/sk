#ifndef _HUTILS_H_
#define _HUTILS_H_

#include "htypes.h"
#include <random>

inline u32 min(u32 x,u32 y) { return (x<y)?x:y;} ///< Classic minimum
inline u32 max(u32 x,u32 y) { return (x>y)?x:y;} ///< Classic maximum

/**
 * Compute the number of layers in a sorting network
 * @param nw Input network
 * @return Number of parallel operation layers
 */
u32 computeDepth(const Network_t &nw);

/**
 * Decompose a network into parallel layers, using greedy earliest-layer assignment
 * (same layering semantics as computeDepth).
 * @param nw Input network
 * @param layers [OUT] Sequence of layers, each containing the CEs assigned to it
 */
void decomposeLayers(const Network_t &nw, std::vector<Network_t> &layers);

/**
 * Compute the maximum number of distinct wires touched by any single layer of a network.
 * This is a proxy for the register pressure of a stage-wise (SIMD) implementation:
 * every active wire in a stage needs to reside in a vector register.
 * @param nw Input network
 * @return Maximum number of distinct wires touched by a single layer (0 for empty network)
 */
u32 computeMaxLayerWires(const Network_t &nw);

/**
 * Compute the SIMD-aware cost of a network: sum of per-CE weights.
 * A CE (i,j) is considered "vectorization friendly" (low weight) if the symmetric wire
 * distance |i-j| is a power of two in the range {1,2,4,8} (single shuffle/vperm/tbl
 * style instruction) or if the pair is mirror-symmetric (i+j == ninputs-1).
 * All other ("lane crossing") pairs are penalized with a higher weight.
 * @param nw Input network
 * @param ninputs Number of network inputs
 * @param w_aligned Weight of vectorization friendly CEs
 * @param w_crossing Weight of lane crossing CEs
 * @return Total weighted cost
 */
uint64_t computeSimdCost(const Network_t &nw, u8 ninputs, u32 w_aligned, u32 w_crossing);

/**
 * Create "symmetric" sorting network by creating a mirror image of each pair if it doesn't coincide with the original.
 * Note: for networks with odd input sizes, the mirror image of a pair connected to the middle line will necessarilly belong
 * to a neighbouring layer.
 * @param ninputs Number of inputs
 * @param inpairs Input network
 * @param outpairs Symmetrical output network 
 */
void symmetricExpansion(u8 ninputs, const Network_t &inpairs, Network_t &outpairs);

/**
 * Print a sorting network as text
 * @param nw network to print
 */
void printnw(const Network_t &nw);

/**
 * Concatenate two (partial) sorting networks into a new one
 * @param nw1 First network
 * @param nw2 Second network
 * @param result [OUT] Concatenation of both networks
 */
void concatNetwork(const Network_t &nw1, const Network_t &nw2, Network_t &result);


/**
 * Append one network to another
 * @param dst Network to be modified
 * @param src Network to be appended
 */
void appendNetwork(Network_t &dst, const Network_t &src);


/** 
 * Orthogonal Convex Hull, to keep track of unmatched (size,depth) combinations of the networks we found
 */
 
class OCH_t {
public:
	OCH_t();
	void clear();
	bool improved(u32 size, u32 depth);
	void print() const;
private: 
	struct OCH_Entry{
		u32 size;
		u32 depth;
	};

	std::vector<OCH_Entry> och;
};

// Random generation defs

typedef std::mt19937_64 RandGen_t;

#define RANDIDX(v) (mtRand()%v.size())      ///< Random index from vector
#define RANDELEM(v) (v[RANDIDX(v)])         ///< Random element from vector

#endif
