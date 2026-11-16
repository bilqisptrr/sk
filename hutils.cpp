#include "hutils.h"
#include <cstdio>

/**
 * Create orthogonal convex hull
 */
OCH_t::OCH_t()
{	
}

/**
 * Clear OCH data
 */
void OCH_t::clear()
{
	och.clear();
}

/**
 * Add a (size, depth) pair to the OCH computation
 * @param l length of network found
 * @param d depth of network found
 * @param true if the network is an "improvement" i.e. belongs to the updated set of OCH pairs that minimize both criteria.
 */
bool OCH_t::improved(u32 l, u32 d)
{
	bool matched=false;
	
	for(size_t k=0;k<och.size();k++)
	{
		if((l>=och[k].size)&&(d>=och[k].depth))
			matched=true;
	}
	
	if(matched)
		return false;
	
	std::vector<OCH_Entry> newch;
	OCH_Entry ce;
	ce.size=l;
	ce.depth=d;
	newch.push_back(ce);

	for(size_t k=0;k<och.size();k++)
	{
		if((och[k].size<l)||(och[k].depth<d))
		{
			newch.push_back(och[k]);
		}
	}
	
	och=newch;
	return true;
}
	

/**
 * Print best performing (length, depth) pairs found so far
 */
void OCH_t::print() const
{
	printf("Most performant: [");
	for(size_t k=0;k<och.size();k++)
	{
		printf("(%u,%u)",och[k].size,och[k].depth);
		printf("%c", (k<(och.size()-1)) ? ',':']');		
	}
	printf("\r\n");
	
}



u32 computeDepth(const Network_t &nw)
{
	// A comparator goes immediately after the latest layer touching either wire.
	// Layer numbers are one-based, leaving zero for an untouched wire.
	size_t wireDepth[NMAX] = {};
	size_t depth = 0;
	for(size_t k=0; k<nw.size(); ++k)
	{
		const Pair_t &p = nw[k];
		const size_t layer = 1 + (wireDepth[p.lo] > wireDepth[p.hi] ? wireDepth[p.lo] : wireDepth[p.hi]);
		wireDepth[p.lo] = wireDepth[p.hi] = layer;
		if(layer > depth)
			depth = layer;
	}
	return (u32)depth;
}

void decomposeLayers(const Network_t &nw, std::vector<Network_t> &layers)
{
	std::vector<SortWord_t> wiremasks;

	layers.clear();
	for(size_t k=0;k<nw.size();k++)
	{
		u32 i=nw[k].lo;
		u32 j=nw[k].hi;
		int matchidx=(int)wiremasks.size();
		int idx=matchidx-1;
		while(idx>=0)
		{
			if((wiremasks[idx] & ((1ULL<<i)|(1ULL<<j))) == 0)
			{
				matchidx=idx;
			}
			else
			{
				break;
			}
			idx--;
		}
		if(matchidx>=(int)wiremasks.size())
		{
			wiremasks.push_back(0);
			layers.push_back(Network_t());
		}
		wiremasks[matchidx]|= 1ULL<<i;
		wiremasks[matchidx]|= 1ULL<<j;
		layers[matchidx].push_back(nw[k]);
	}
}


u32 computeMaxLayerWires(const Network_t &nw)
{
	// Each layer contains disjoint comparators, so each contributes two wires
	// (or one for a degenerate self-pair). Keep common-sized networks on the stack and allocate
	// only for unusually deep networks; there can be at most nw.size() layers.
	const size_t stackLayers = 128;
	u8 stackCounts[stackLayers] = {};
	std::vector<u8> extraCounts;
	u8 *counts = stackCounts;
	if(nw.size() > stackLayers)
	{
		extraCounts.resize(nw.size(), 0);
		counts = extraCounts.data();
	}
	size_t wireDepth[NMAX] = {};
	u32 maxwires = 0;
	for(size_t k=0; k<nw.size(); ++k)
	{
		const Pair_t &p = nw[k];
		const size_t layer = 1 + (wireDepth[p.lo] > wireDepth[p.hi] ? wireDepth[p.lo] : wireDepth[p.hi]);
		wireDepth[p.lo] = wireDepth[p.hi] = layer;
		const u32 wires = (counts[layer-1] += (p.lo == p.hi ? 1 : 2));
		if(wires > maxwires)
			maxwires = wires;
	}
	return maxwires;
}

uint64_t computeSimdCost(const Network_t &nw, u8 ninputs, u32 w_aligned, u32 w_crossing)
{
	uint64_t cost=0;
	const SortWord_t alignedstrides = (1ULL<<1)|(1ULL<<2)|(1ULL<<4)|(1ULL<<8); // |i-j| in {1,2,4,8}

	for(size_t k=0;k<nw.size();k++)
	{
		u32 i=nw[k].lo;
		u32 j=nw[k].hi;
		u32 d=j-i;
		bool aligned = ((d<=8) && ((alignedstrides>>d)&1ULL)) || ((i+j)==(u32)(ninputs-1));
		cost += aligned ? w_aligned : w_crossing;
	}
	return cost;
}


void printnw(const Network_t &nw)
{
	printf("[");
	for(size_t k=0;k<nw.size();k++)
	{
		printf("(%u,%u)",nw[k].lo,nw[k].hi);
		printf("%c", ((k+1)<nw.size()) ? ',':']');
	}
	printf("}\r\n");
}

void symmetricExpansion(u8 ninputs, const Network_t &inpairs, Network_t &outpairs)
{
	outpairs.clear();
	for(size_t k=0;k<inpairs.size();k++)
	{
		outpairs.push_back(inpairs[k]);
		if((inpairs[k].lo+inpairs[k].hi)!=(ninputs-1)) // Don't duplicate pair that maps on itself
		{
			Pair_t sp={(u8)(ninputs-1-inpairs[k].hi), (u8)(ninputs-1-inpairs[k].lo)};
			outpairs.push_back(sp);
		}
	}
}


void concatNetwork(const Network_t &nw1, const Network_t &nw2, Network_t &result)
{
	result=nw1;
	result.insert(result.end(),nw2.begin(),nw2.end());
}


void appendNetwork(Network_t &dst, const Network_t &src)
{
	if(src.size()>0)
	{
		dst.insert(dst.end(),src.begin(),src.end());
	}
}

