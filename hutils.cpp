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
	std::vector<SortWord_t> layers;
	int nlayers=0;

	for(size_t k=0;k<nw.size();k++)
	{
		u32 i=nw[k].lo;
		u32 j=nw[k].hi;
		int matchidx=nlayers;
		int idx=nlayers-1;
		while(idx>=0)
		{
			if((layers[idx] & ((1ULL<<i)|(1ULL<<j))) == 0)
			{
				matchidx=idx;
			}
			else
			{
				break;
			}
			idx--;
		}
		if(matchidx>=nlayers)
		{
			layers.push_back(0);
			nlayers++;
		}
		layers[matchidx]|= 1ULL<<i;
		layers[matchidx]|= 1ULL<<j;
	}

	return nlayers;
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
	std::vector<Network_t> layers;
	decomposeLayers(nw,layers);

	u32 maxwires=0;
	for(size_t l=0;l<layers.size();l++)
	{
		SortWord_t mask=0;
		for(size_t k=0;k<layers[l].size();k++)
		{
			mask |= (1ULL<<layers[l][k].lo) | (1ULL<<layers[l][k].hi);
		}
		u32 w=(u32)__builtin_popcountll(mask);
		if(w>maxwires)
		{
			maxwires=w;
		}
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

