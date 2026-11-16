#define VERSION "SorterHunterDSP_V1.0"

#include "htypes.h"
#include "hutils.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <random>
#include "ConfigParser.h"
#include <ctime>
#include "prefix_processor.h"

ConfigParser cp;

bool use_symmetry = true; ///< Treat sorting network as symmetric or not
bool force_valid_uphill_step = true; ///< "Uphill" step inserts duplicate CE if not in final layer.
u8 N=0;                   ///< Problem dimension, i.e. number of inputs to be sorted
u32 EscapeRate=0;         ///< Adds a random pair (and its symmetric complement for symmetric networks) every x iterations
u32 MaxMutations=1;       ///< Maximum allowed number of mutations in evolution step
u32 PrefixType=0;         ///< Type of prefix used (0=none, 1=fixed, 2=greedy)
Network_t FixedPrefix;    ///< Fixed prefix to use (if applicable)
Network_t InitialNetwork; ///< Initial starting point of network
u32 GreedyPrefixSize=0;   ///< Size of greedy prefix (if applicable)
OCH_t conv_hull;          ///< "Best performing" network list found so far
uint64_t RandomSeed;      ///< Random seed
uint64_t RestartRate;     ///< Return to initial conditions each ... iterations (0=never)
u32 Verbosity=1;          ///< Overall verbosity level: 0:minimal, 1:moderate, 2:high, >2:debug

// ===================== DSP domain-specific search options =====================
// SearchMode selects the class of networks being hunted (see README_DSP.md):
//   0 = classic full sorter (all 2^N input behaviour verified)
//   1 = quantile / selection network: only wires in QuantileIndices are verified
//       (jitter buffer / GCC percentile & trimmed mean: indices [k_low..k_high])
//   2 = sliding window insertion network: N-1 presorted wires + 1 free "new sample" wire,
//       only 2N input patterns to verify (audio median / click removal)
//   3 = asymmetric partial network: isolate K smallest (side 0) or K largest (side 1) values
//       (AEC / VAD / AGC envelope & noise floor tracking)
//   4 = multi-output shared-prefix network: multiple order statistics of one window
//       verified simultaneously + early-output (settling depth) reporting (video 3x3/5x5)
u32 SearchMode=0;
std::vector<u8> targetIndices; ///< Sorted output indices verified by the quantile check (modes 1/3/4, optionally 2)
bool useQuantileCheck=false;   ///< true: verify target wire values against expected masks instead of full sortedness
u32 IsolateSide=0;             ///< Mode 3: 0 = isolate K smallest on low wires, 1 = isolate K largest on high wires
u32 IsolateCount=0;            ///< Mode 3: K
u32 PresortedCount=0;          ///< Mode 2: number of presorted wires (monotone input class), wires 0..PresortedCount-1
u32 NewSampleWire=0;           ///< Mode 2: wire index carrying the free "new sample" value
u32 SimdAwareCost=0;           ///< 1: use SIMD lane-crossing weighted cost instead of size in the convex hull
u32 SimdWeightAligned=1;       ///< Cost weight of vectorization friendly CEs (stride in {1,2,4,8} or mirror symmetric)
u32 SimdWeightCrossing=3;      ///< Cost weight of lane crossing CEs
u32 MaxActiveWires=0;          ///< Register pressure limit: max distinct wires per layer (0 = unlimited)
u32 ReportEarlyOutput=0;       ///< 1: report per-target settling layer for quantile modes (shared-prefix depth)
BitParallelList_t qmasks;      ///< Expected-value masks: targets.size() words per group of PARWORDSIZE test vectors
// ==============================================================================

// Working set of pairs in the sorting network
Network_t pairs; ///< Current core network: evolving section between prefix and postfix. For symmetric networks, mirrored pair (if not coinciding) is omitted.
Network_t se; ///< Symmetrical expansion of current network
Network_t newpairs;
Network_t prefix; ///< Fixed, greedy, hybrid or empty prefix network
Network_t postfix; ///< Fixed or empty postfix network

// Set of all possible pairs, unique taking into account symmetric complements
Network_t alphabet;

#define NMUTATIONTYPES 6 ///< Number of different mutation types
u32 mutation_type_weights[NMUTATIONTYPES]; ///< Relative probabilities for each mutation type
std::vector<u8> mutationSelector; ///< Helper variable to quickly pick a mutation with the requested probability.

// Random generation
std::random_device rd;
RandGen_t mtRand(rd()); // Mersenne twister is a rather good PRNG. Seeding quality varies between systems, but OK ; this is no crypto application.


/**
 * Send a bit-parallel set of test patterns through a sorting network. Maximum PARWORDSIZE patterns are processed
 * together.
 * 'Data' contains N words. Each bit position corresponds to an independent data set {0,1}^N to be sorted
 * Bit level truth table:
 * In    Out
 * 00 ->  00
 * 01 ->  01
 * 10 ->  01 ("swap")
 * 11 ->  11
 * @param data Input/output vectors
 * @param nw Network to be tested
 */
void applyBitParallelSort(BPWord_t data[], const Network_t &nw)
{
	size_t l=nw.size();
	for(size_t n=0;n<l;n++)
	{
		u32 i=nw[n].lo;
		u32 j=nw[n].hi;
		BPWord_t iold=data[i];
		data[i]&=data[j];
		data[j]|=iold;
	}
}

/**
 * Test vectors filled with input data sets fed to parallel sorter tester
 */
BitParallelList_t parallelpatterns_from_prefix;

/**
 * Shuffle a list of single patterns and convert it to the bit-parallel test vector list
 * (plus expected-value masks in case of quantile checking).
 * @param singles Input/output patterns to convert (shuffled in place)
 */
void buildBitParallelVectors(SinglePatternList_t &singles)
{
	std::shuffle(singles.begin(),singles.end(), mtRand); // Shuffle test vectors: improve probability of early rejection of non-sorters

	bool is_even = ((N%2)==0);
	convertToBitParallelDSP(N, singles, use_symmetry && is_even, targetIndices, parallelpatterns_from_prefix, qmasks);
}

/**
 * Initialise test vectors with patterns produced by the prefix.
 * Test vectors are stored in parallelpatterns_from_prefix
 * @param prefix Network prefix to use
 */
void prepareTestVectorsFromPrefix(const Network_t &prefix)
{
	SinglePatternList_t singles;
	computePrefixOutputs(N, prefix, singles);

	buildBitParallelVectors(singles);
}

/**
 * Initialise test vectors for the sliding window insertion mode (SearchMode=2):
 * the input class consists of all patterns that are monotone (presorted ascending) on
 * wires 0..PresortedCount-1 and carry a free "new sample" bit on wire NewSampleWire.
 * This yields exactly 2*(PresortedCount+1) raw patterns, reduced to about 2N after removal
 * of the two already sorted fixpoints - versus 2^N patterns for a generic sorter.
 * @param prefix Network prefix to use (may be empty; applied to the restricted class)
 */
void prepareTestVectorsSliding(const Network_t &prefix)
{
	SinglePatternList_t raw;

	for(u32 p=0;p<=PresortedCount;p++) // p = number of 1s on the presorted wires
	{
		for(u32 f=0;f<2;f++) // free bit on the new sample wire
		{
			SortWord_t w=0;
			for(u32 i=0;i<PresortedCount;i++)
			{
				if(i >= (PresortedCount-p))
					w |= 1ULL<<i; // ascending order: wires PresortedCount-p .. PresortedCount-1 are 1
			}
			if(f)
				w |= 1ULL<<NewSampleWire;
			raw.push_back(w);
		}
	}

	SinglePatternList_t singles;
	computeRestrictedOutputs(N, prefix, raw, singles);

	if(Verbosity > 1)
	{
		printf("Sliding window input class: %lu raw patterns (%u presorted wires, new sample wire %u) -> %lu unique prefix outputs.\n",
		       raw.size(),PresortedCount,NewSampleWire,singles.size());
	}

	buildBitParallelVectors(singles);
}

/**
 * Initialize "alphabet" of CEs to use
 */

void initalphabet()
{
	alphabet.clear();
	for(u32 i=0;i<(N-1u);i++)
		for(u32 j=i+1;j<N;j++)
		{
			u32 isym=N-1-j;
			u32 jsym=N-1-i;
			
			if(!use_symmetry || (isym>i) || ((isym==i) && (jsym>=j)))
			{
				Pair_t p={(u8)i,(u8)j};
				alphabet.push_back(p);
			}	
		}
}		

/**
 * Heuristic test vector reordering - attempt to speed up rejection of failing networks.
 * Core idea is to move the test vectors that most likely reject a non-sorter to the front of the list.
 * Withing the first group of PARWORDSIZE test vectors, the individual vectors are competing for the lowest bit position in a ladder tournament.
 * Within that group, each time the vector with the lowest failing index is moving one step closer towards bit 0 by swapping it with its neighbour.
 * Vectors within the 2nd group are competing with the highest bit position i.e. the "degradation candidate" of the 1st group. Vectors in higher
 * numbered groups (3rd group or later) are not individually rewarded, but the whole group is swapped with a group that is evaluated earlier in the ranking.
 * As the network evolves, so will the selection of "best" vectors for detecting failing mutant networks. The method described attempts to dynamically
 * optimize the order to the evolving situation. Note that to accept a network, still all test vectors need to pass, no shortcuts are taken.
 * The same bit permutation must be applied to ALL per-group word arrays (data words AND quantile
 * expected-value masks), otherwise masks and vector columns get out of alignment!
 * @param w Word list to permute (data words or qmask words)
 * @param S Number of words per test vector group (N for data, targets.size() for qmasks)
 * @param failvector Index of first failing vector
 */
static void bumpWordGroup(BitParallelList_t &w, size_t S, size_t failvector)
{
	size_t groupno = failvector/PARWORDSIZE;
	size_t idx=S*groupno;

	if(groupno > 1)
	{
		size_t delta = S*((groupno+7)/8);
		// Move up failing vector group about 1/8 the distance to the front
		for(size_t k=0;k<S;k++)
		{
			BPWord_t z=w[idx+k-delta];
			w[idx+k-delta] = w[idx+k];
			w[idx+k] = z;
		}
	}
	else if (groupno==1)
	{
		// Swap with last bit position of group 0
		BPWord_t m0 = 1ull << (PARWORDSIZE-1);
		BPWord_t m1 = 1ull << (failvector%PARWORDSIZE);
		int shift = (PARWORDSIZE-1)-(failvector%PARWORDSIZE);
		for(size_t k=0;k<S;k++)
		{
			BPWord_t old0=w[k];
			BPWord_t old1=w[k+S];
			w[k] = (old0&~m0) | ((old1&m1)<<shift);
			w[k+S] = (old1&~m1) | ((old0&m0)>>shift);
		}
	}
	else if (failvector>0) // groupno==0, bit position >0
	{
		// Swap with neighbouring bit position within group 0
		BPWord_t m0 = 1ull << (failvector-1);
		BPWord_t m1 = 1ull << failvector;
		for(size_t k=0;k<S;k++)
		{
			BPWord_t old=w[k];
			w[k] = (old&~m0&~m1) | ((old&m1)>>1) | ((old&m0)<<1);
		}
	}
}

void bumpVectorPosition(BitParallelList_t &bpl, size_t failvector)
{
	// Data words: N words per group
	bumpWordGroup(bpl, N, failvector);

	// Quantile expected-value masks: one word per target index per group - must be
	// permuted identically to keep mask/vector-column alignment (see checkOutputGroup).
	if(useQuantileCheck && (targetIndices.size()>0))
	{
		bumpWordGroup(qmasks, targetIndices.size(), failvector);
	}
}


/**
 * Check the output of one group of PARWORDSIZE bit-parallel test vectors.
 * Classic mode (useQuantileCheck==false): scan for forbidden 1 -> 0 transitions (full sortedness).
 * Quantile mode (useQuantileCheck==true): compare only the target wires against the precomputed
 * expected-value masks (bit b of mask k = 1 iff test vector b has >= N-k ones). Wires outside the
 * target set are unconstrained. By the generalized zero-one principle for order statistics,
 * passing all 2^N zero-one vectors guarantees correct ranked selection for arbitrary inputs;
 * for restricted input classes (e.g. sliding window) correctness is guaranteed on that class.
 * @param data Output words of the network (N words, one per wire)
 * @param qgroup Group index of the test vector group (index into qmasks)
 * @param accum [OUT] Word with a bit set for each failing test vector of the group
 * @return true if all vectors of the group pass
 */
bool checkOutputGroup(const BPWord_t data[], size_t qgroup, BPWord_t &accum)
{
	accum=0;

	if(!useQuantileCheck)
	{
		for(size_t k=0;k<(N-1u);k++)
			accum |= data[k]&~data[k+1]; // Scan for forbidden 1 -> 0 transition
	}
	else
	{
		size_t T=targetIndices.size();
		size_t qbase=qgroup*T;
		for(size_t t=0;t<T;t++)
			accum |= data[targetIndices[t]] ^ qmasks[qbase+t];
	}

	return (accum==0ULL);
}

/**
 * Test a candidate network complementing the prefix.
 * This function is called during the regular evolution loop and attempts to
 * optimize the future order of test vectors in the background
 * @param pairs Candidated network
 * @param bpl List of test vectors matching the prefix
 * @return true if prefix+pairs form a valid network for the selected domain mode
 */
bool testpairsFromPrefixOutput(const Network_t &pairs, BitParallelList_t &bpl)
{
	size_t idx=0;
	size_t failvector=0;
	size_t qgroup=0;

	while(idx<bpl.size())
	{
		static BPWord_t data[NMAX];
		BPWord_t accum=0;

		for(size_t k=0;k<N;k++)
			data[k] = bpl[idx+k];

		applyBitParallelSort(data,pairs);

		if(!checkOutputGroup(data,qgroup,accum))
		{
			// A failing group has at least one set bit.
			failvector += (size_t)__builtin_ctzll(accum);

			bumpVectorPosition(bpl, failvector);

			return false;
		}
		idx+=N;
		qgroup++;
		failvector+=PARWORDSIZE;
	}
	return true;
}

/**
 * Test a candidate network complementing the prefix.
 * This function is called during the search for an initial solution
 * @param pairs Candidated network
 * @param bpl List of test vectors matching the prefix
 * @param failed_output_pattern First failing output pattern detected. Used to determine candidate elements to be appended.
 * @return true if prefix+pairs form a valid network for the selected domain mode
 */
bool testInitialPairsFromPrefixOutput(const Network_t &pairs, const BitParallelList_t &bpl, SortWord_t &failed_output_pattern)
{
	size_t idx=0;
	size_t qgroup=0;
	failed_output_pattern=0;

	while(idx<bpl.size())
	{
		static BPWord_t data[NMAX];
		BPWord_t accum=0;

		for(size_t k=0;k<N;k++)
			data[k] = bpl[idx+k];

		applyBitParallelSort(data,pairs);

		if(!checkOutputGroup(data,qgroup,accum))
		{
			const unsigned failingBit = (unsigned)__builtin_ctzll(accum);
			for(size_t k=0;k<N;k++)
				failed_output_pattern |= ((data[k] >> failingBit)&1) << k;

			return false;
		}
		idx+=N;
		qgroup++;
	}
	return true;
}



/**
 * Filter a network to obtain only the pairs that are in range 0..ninputs-1 and properly sorted
 * @param nw input network
 * @param ninputs Number of inputs
 * @return Filtered input network
 */
static const Network_t copyValidPairs(const Network_t &nw, u32 ninputs)
{
	static Network_t result;
	result.clear();
	for(Network_t::const_iterator it=nw.begin();it!=nw.end();it++)
	{
		if((it->hi < ninputs) && (it->lo < it->hi))
		{
			result.push_back( *it);
		}
	}
	return result;
}

/**
 * Create a prefix network using greedy algorithm A.
 * @param prefix [OUT] generated prefix
 * @param npairs Number of inputs to the network
 */
void fillprefixGreedyA(Network_t &prefix, u32 npairs )
{
	prefix.clear();
	SortWord_t sizetmp=createGreedyPrefix(N, npairs, use_symmetry, prefix, mtRand);
	if( Verbosity > 1)
	{
		printf("Greedy prefix size %lu, span %lu.\n",prefix.size(),(size_t)sizetmp);
	}
}

/**
 * Create a hybrid prefix network using first the fixed prefix, then append elements with greedy algorithm A.
 * @param prefix [OUT] generated prefix
 * @param npairs Number of inputs to the network
 */
void fillprefixFixedThenGreedyA(Network_t &prefix, u32 npairs )
{
	prefix=copyValidPairs(FixedPrefix, N);
	SortWord_t sizetmp=createGreedyPrefix(N, npairs+prefix.size(), use_symmetry, prefix, mtRand);
	if( Verbosity > 2)
	{
		printf("Hybrid prefix size %lu, span %lu.\n",prefix.size(),(size_t)sizetmp);
	}
}


/**
 * Attempt to apply a single mutation to the network. If the mutation is a priory rejected, 0 is returned and we will try again.
 * @param newpairs [IN/OUT] candidate network
 * @return Positive integer identifying type of mutation applied, or 0 if none.
 */
u32 attemptMutation(Network_t &newpairs)
{
	u32 applied=0; // Nothing
	u32 mtype=1+RANDELEM(mutationSelector);
	
	switch(mtype)
	{
		case 1:		
			if(newpairs.size()>0)   // Removal of random pair from list
			{
				u32 a=RANDIDX(newpairs);
				newpairs.erase(newpairs.begin() + a);
				applied=mtype;
			}
			break;
		case 2:
			if(newpairs.size()>1) // Swap two pairs at random positions in list
			{
				u32 a=RANDIDX(newpairs);
				u32 b=RANDIDX(newpairs);
				if(a>b){u32 z=a;a=b;b=z;}
				if(newpairs[a]!=newpairs[b])
				{
					bool dependent=false;
					u8 alo=newpairs[a].lo;
					u8 ahi=newpairs[a].hi;
					u8 blo=newpairs[b].lo;
					u8 bhi=newpairs[b].hi;
					
					// Pairs should either intersect, or another pair should exist between them that uses
					// one of the same 4 inputs. Otherwise, comparisons can be executed in parallel and
					// swapping them has no effect. 
					if((blo==alo)||(blo==ahi)||(bhi==alo)||(bhi==ahi))
						dependent=true;
					else
					{
						for(u32 k=a+1;k<b;k++)
						{
							u8 clo=newpairs[k].lo;
							u8 chi=newpairs[k].hi;
							if((clo==alo)||(clo==ahi)||(chi==alo)||(chi==ahi)||
							   (clo==blo)||(clo==bhi)||(chi==blo)||(chi==bhi))
							{
								dependent=true;
								break;
							}
						}
					}
					if(dependent)
					{			
						Pair_t z=newpairs[a];
						newpairs[a]=newpairs[b];
						newpairs[b]=z;
						applied=mtype;
					}
				}
			}
			break;
		case 3:		
			if(newpairs.size()>0)  // Replace a pair at a random position with another random pair
			{
				u32 a=RANDIDX(newpairs);
				Pair_t p=RANDELEM(alphabet);
				if(newpairs[a]!=p)
				{
					newpairs[a]=p;
					applied=mtype;
				}
			}
			break;
		case 4:
			if(newpairs.size()>1) // Cross two pairs at random positions in list
			{
				u32 a=RANDIDX(newpairs);
				u32 b=RANDIDX(newpairs);
				u8 alo=newpairs[a].lo;
				u8 ahi=newpairs[a].hi;
				u8 blo=newpairs[b].lo;
				u8 bhi=newpairs[b].hi;
				
				if ((alo!=blo)&&(alo!=bhi)&&(ahi!=blo)&&(ahi!=bhi))
				{
					u32 r2=mtRand()%2;
					u32 x = r2 ? bhi : blo;
					u32 y = r2 ? blo : bhi;
					newpairs[a].lo = min(alo, x);
					newpairs[a].hi = max(alo, x);
					newpairs[b].lo = min(ahi, y);
					newpairs[b].hi = max(ahi, y);
					applied=mtype;
				}
			}
			break;
		case 5:
			if(newpairs.size()>1) // Swap neighbouring intersecting pairs - special case of type r=2.
			{
				u32 a=RANDIDX(newpairs);
				u8 alo=newpairs[a].lo;
				u8 ahi=newpairs[a].hi;
				for(u32 b=a+1;b<newpairs.size();b++)
				{
					u8 blo=newpairs[b].lo;
					u8 bhi=newpairs[b].hi;
					if((blo==alo)||(blo==ahi)||(bhi==alo)||(bhi==ahi))
					{
						if(newpairs[a]!=newpairs[b])
						{
							Pair_t z=newpairs[a];
							newpairs[a]=newpairs[b];
							newpairs[b]=z;
							applied=mtype;
						}
						break;
					}
				}		
			}
			break;
		case 6:
			if(newpairs.size()>0) // Change one half of a pair - special case of type r=3.
			{
				u32 a=RANDIDX(newpairs);
				Pair_t p=newpairs[a];
				Pair_t q;
				do {
					q=RANDELEM(alphabet);
					}while((q.lo!=p.lo)&&(q.hi!=p.lo)&&(q.lo!=p.hi)&&(q.hi!=p.hi));
					
				if(q!=p)
				{
					newpairs[a]=q;
					applied=mtype;
				}
			}
			break;
		default:
			break;
	}
	
	return applied;
}

/**
 * Compute per-target "settling depth" of the currently accepted postfix part (global 'se'):
 * for each target index, the number of layers needed (counted from the prefix output, i.e. the
 * test vector input) before the target wire holds its final value for every test vector.
 * Because the check is performed on the complete set of zero-one test vectors, the resulting
 * early-output guarantee extends to all inputs (generalized zero-one principle).
 * This is the shareable prefix depth: all layers before the last settling layer are identical
 * work shared by every consumer of the multiple outputs (e.g. 3x3 video median+min+max).
 * @param perTarget [OUT] Settling layer per target index
 * @return Maximum settling layer over all targets
 */
static u32 computeSettlingDepths(std::vector<u32> &perTarget)
{
	size_t T=targetIndices.size();
	perTarget.assign(T,0);
	if(!useQuantileCheck || (T==0))
		return 0;

	std::vector<Network_t> layers;
	decomposeLayers(se,layers);

	size_t ngroups=qmasks.size()/T;
	static BPWord_t data[NMAX];
	std::vector<u32> lastmismatch(T,0);

	for(size_t g=0;g<ngroups;g++)
	{
		for(size_t k=0;k<N;k++)
			data[k]=parallelpatterns_from_prefix[g*N+k];

		for(size_t l=0;l<layers.size();l++)
		{
			applyBitParallelSort(data,layers[l]); // Layer pairs are wire-disjoint: sequential application is equivalent
			for(size_t t=0;t<T;t++)
			{
				if(data[targetIndices[t]] != qmasks[g*T+t])
				{
					// Keep the WORST (maximum) settling layer over all test vector groups
					if((u32)(l+1) > lastmismatch[t])
						lastmismatch[t]=(u32)(l+1);
				}
			}
		}
	}

	u32 maxsd=0;
	for(size_t t=0;t<T;t++)
	{
		perTarget[t]=lastmismatch[t];
		if(lastmismatch[t]>maxsd)
			maxsd=lastmismatch[t];
	}
	return maxsd;
}

/**
 * Report network if it is an improved (cost,depth) combination.
 * Cost equals the network size (classic) or the SIMD-aware weighted cost
 * (SimdAwareCost=1: vectorization friendly CEs weigh SimdWeightAligned, lane crossing
 * CEs weigh SimdWeightCrossing). The orthogonal convex hull tracks (cost,depth).
 * @param nw Valid network
 */
static void checkImproved(const Network_t &nw)
{
	u32 depth=computeDepth(nw);
	u32 cost = SimdAwareCost ? (u32)computeSimdCost(nw,N,SimdWeightAligned,SimdWeightCrossing) : (u32)nw.size();
	if(conv_hull.improved(cost,depth))
	{
		/* Print only if the network is an improved (cost,depth) combination */
		if((Verbosity > 1) || (nw.size() <= ((N*(N-1u))/2u))) // Reduce rubbish listing. Should at least compete with bubble sort before reporting
		{
			u32 mw = computeMaxLayerWires(se);
			printf(" {'N':%u,'L':%lu,'C':%u,'D':%u,'MW':%u,'sw':'%s','ESC':%u,'Prefix':%lu,'Postfix':%lu,'Mode':%u",N,nw.size(),cost,depth,mw,VERSION,EscapeRate,prefix.size(),postfix.size(),SearchMode);
			if(useQuantileCheck && ReportEarlyOutput)
			{
				std::vector<u32> sd;
				u32 maxsd=computeSettlingDepths(sd);
				printf(",'SD':%u,'SDt':[",maxsd);
				for(size_t t=0;t<sd.size();t++)
					printf("%u%c",sd[t],(t+1<sd.size())?',':']');
			}
			printf(",'nw':");
			printnw(nw);
			conv_hull.print();
		}
	}
}

/**
 * General help message
 */
static void usage()
{
	printf("Usage: SorterHunter <config_file_name>\n\n");
	printf("A sample config file containing help text is provided, named 'sample_config.txt'\n");
	printf("SorterHunter is a program that tries to find efficient sorting networks by applying\n");
	printf("an evolutionary approach. It is offered under MIT license\n");
	printf("The DSP fork (SorterHunterDSP) additionally hunts domain specific networks:\n");
	printf("quantile/selection networks, sliding window insertion networks, partial min/max-K\n");
	printf("isolation networks and multi-output networks - see README_DSP.md and the\n");
	printf("sample_config_dsp_*.txt files.\n");
	printf("Program version: %s\n",VERSION);
	exit(1);
}


uint64_t itercount=0;
uint64_t iter_next_report=1;
uint64_t iter_last_report=0;
time_t t0 = clock();
time_t t1 = t0;

/**
 * Check whether the target index set is mirror symmetric (k in targets <=> N-1-k in targets).
 * Only then may the mirrored test vector reduction (symmetric networks) be used in quantile mode.
 */
static bool targetsMirrorSymmetric()
{
	for(size_t t=0;t<targetIndices.size();t++)
	{
		u8 mk=N-1-targetIndices[t];
		bool found=false;
		for(size_t tt=0;tt<targetIndices.size();tt++)
		{
			if(targetIndices[tt]==mk)
			{
				found=true;
				break;
			}
		}
		if(!found)
			return false;
	}
	return true;
}

/**
 * Select and prepare the test vectors matching the current prefix and domain mode
 */
void prepareTestVectors(const Network_t &prefix)
{
	if(SearchMode==2)
		prepareTestVectorsSliding(prefix);
	else
		prepareTestVectorsFromPrefix(prefix);
}

/**
 * SorterHunter main routine
 */
int main(int argc, char *argv[])
{
	/* Handle validity of command line options - extremely simple */
	if(argc!=2)
	{
		usage();
		return -1;
	}
	
	/* Process configuration file */
	if(!cp.parseConfig(argv[1]))
	{
		printf("Error parsing config options.\n");
		return -1;
	}
	
	if(cp.getInt("RandomSeed")!=0u)
	{
		RandomSeed=cp.getInt("RandomSeed");
		mtRand.seed(RandomSeed);
	}
	
	N=cp.getInt("Ninputs",0);
	use_symmetry = (cp.getInt("Symmetric")>0u);
	force_valid_uphill_step = (cp.getInt("ForceValidUphillStep",1)>0);
	EscapeRate = cp.getInt("EscapeRate",0);
	MaxMutations= cp.getInt("MaxMutations",1);
	mutation_type_weights[0]=cp.getInt("WeigthRemovePair",1);
	mutation_type_weights[1]=cp.getInt("WeigthSwapPairs",1);
	mutation_type_weights[2]=cp.getInt("WeigthReplacePair",1);
	mutation_type_weights[3]=cp.getInt("WeightCrossPairs",1);
	mutation_type_weights[4]=cp.getInt("WeightSwapIntersectingPairs",1);
	mutation_type_weights[5]=cp.getInt("WeightReplaceHalfPair",1);
	for(u32 n=0;n<NMUTATIONTYPES;n++)
	{
		for(u32 k=0;k<mutation_type_weights[n];k++)
			mutationSelector.push_back(n);
	}
	if(mutationSelector.size()==0)
	{
		printf("No mutation types selected.\n");
		exit(1);
	}
	PrefixType=cp.getInt("PrefixType",0);
	FixedPrefix=cp.getNetwork("FixedPrefix");
	GreedyPrefixSize=cp.getInt("GreedyPrefixSize",0);
	RestartRate=cp.getInt("RestartRate",0);
	Verbosity=cp.getInt("Verbosity",1);
	postfix=cp.getNetwork("Postfix");

	/* ==================== DSP domain-specific options ==================== */
	SearchMode=cp.getInt("SearchMode",0);
	if(SearchMode>4)
	{
		printf("Error: SearchMode must be 0..4 (0=full sorter, 1=quantile, 2=sliding window, 3=partial isolation, 4=multi-output).\n");
		exit(1);
	}

	useQuantileCheck = (SearchMode==1) || (SearchMode==3) || (SearchMode==4);

	if((SearchMode==1) || (SearchMode==4))
	{
		const std::vector<uint64_t> &qidx=cp.getIntList("QuantileIndices");
		if(qidx.size()==0)
		{
			printf("Error: SearchMode=%u requires 'QuantileIndices=<comma separated output indices>'\n",SearchMode);
			exit(1);
		}
		for(size_t t=0;t<qidx.size();t++)
		{
			if(qidx[t]>=N)
			{
				printf("Error: QuantileIndices value %llu out of range for Ninputs=%u\n",(unsigned long long)qidx[t],N);
				exit(1);
			}
			targetIndices.push_back((u8)qidx[t]);
		}
		std::sort(targetIndices.begin(),targetIndices.end());
		targetIndices.erase(std::unique(targetIndices.begin(),targetIndices.end()),targetIndices.end());
	}
	else if(SearchMode==3)
	{
		IsolateSide=cp.getInt("IsolateSide",0);
		IsolateCount=cp.getInt("IsolateCount",1);
		if((IsolateSide>1) || (IsolateCount<1) || (IsolateCount>N))
		{
			printf("Error: IsolateSide must be 0 (K smallest) or 1 (K largest), IsolateCount in 1..Ninputs.\n");
			exit(1);
		}
		if(IsolateSide==0)
		{
			for(u32 k=0;k<IsolateCount;k++)
				targetIndices.push_back((u8)k);
		}
		else
		{
			for(u32 k=0;k<IsolateCount;k++)
				targetIndices.push_back((u8)(N-1-k));
		}
		std::sort(targetIndices.begin(),targetIndices.end());
	}
	else if(SearchMode==2)
	{
		PresortedCount=cp.getInt("PresortedCount",N-1);
		NewSampleWire=cp.getInt("NewSampleWire",PresortedCount);
		if((PresortedCount<1) || (PresortedCount>=(u32)N))
		{
			printf("Error: PresortedCount must be in 1..Ninputs-1.\n");
			exit(1);
		}
		if((NewSampleWire<PresortedCount) || (NewSampleWire>=(u32)N))
		{
			printf("Error: NewSampleWire must be outside the presorted wire block [0..%u] and below Ninputs.\n",PresortedCount-1u);
			exit(1);
		}
		if((PrefixType==2) || (PrefixType==3))
		{
			if(Verbosity > 0)
			{
				printf("Warning: greedy prefix (PrefixType=%u) minimizes the remaining pattern set over ALL 2^N inputs.\n",PrefixType);
				printf("         In sliding window mode (SearchMode=2) the input class is restricted, so the greedy prefix is disabled.\n");
			}
			PrefixType=0;
		}
		if(cp.getIntList("QuantileIndices").size()>0)
		{
			/* Optional: in sliding window mode, verify only the given order statistics
			   instead of the full sortedness of the window */
			const std::vector<uint64_t> &qidx=cp.getIntList("QuantileIndices");
			for(size_t t=0;t<qidx.size();t++)
			{
				if(qidx[t]>=N)
				{
					printf("Error: QuantileIndices value %llu out of range for Ninputs=%u\n",(unsigned long long)qidx[t],N);
					exit(1);
				}
				targetIndices.push_back((u8)qidx[t]);
			}
			std::sort(targetIndices.begin(),targetIndices.end());
			targetIndices.erase(std::unique(targetIndices.begin(),targetIndices.end()),targetIndices.end());
			useQuantileCheck=true;
		}
	}

	SimdAwareCost=cp.getInt("SimdAwareCost",0);
	SimdWeightAligned=cp.getInt("SimdWeightAligned",1);
	SimdWeightCrossing=cp.getInt("SimdWeightCrossing",3);
	if((SimdWeightAligned==0) || (SimdWeightCrossing==0))
	{
		printf("Error: SimdWeightAligned and SimdWeightCrossing must be >= 1.\n");
		exit(1);
	}
	MaxActiveWires=cp.getInt("MaxActiveWires",0);
	if(MaxActiveWires>(u32)N)
	{
		MaxActiveWires=N;
	}
	ReportEarlyOutput=cp.getInt("ReportEarlyOutput",(SearchMode==4)?1u:0u);

	if(MaxActiveWires>0)
	{
		u32 pw=computeMaxLayerWires(postfix);
		if(pw>MaxActiveWires)
		{
			printf("Error: fixed Postfix network touches %u wires in its widest layer, exceeding MaxActiveWires=%u.\n",pw,MaxActiveWires);
			printf("       Evolution could never satisfy the register pressure constraint (infinite loop). Fix the Postfix or raise the limit.\n");
			exit(1);
		}
	}

	/* Symmetry reductions are only valid if the verification target set is mirror symmetric
	   (k in targets <=> N-1-k in targets), and never for the sliding window input class. */
	if(use_symmetry && ((SearchMode==2) || (useQuantileCheck && !targetsMirrorSymmetric())))
	{
		if(Verbosity > 0)
		{
			printf("Warning: option 'Symmetric' disabled: %s does not map the mirrored input class onto the target set.\n",
			       (SearchMode==2) ? "the sliding window input class" : "the target index set is not mirror symmetric");
		}
		use_symmetry = false;
	}

	if((N%2) && use_symmetry)
	{
		if(Verbosity > 0)
		{
			printf("Warning: option 'Symmetric' ignored for odd number of inputs\n");
		}
		use_symmetry = false;
	}

	if(Verbosity > 0)
	{
		const char *modenames[]={"classic full sorter","quantile/selection","sliding window insertion","partial isolation (min/max-K)","multi-output (shared prefix)"};
		printf("DSP mode: %u (%s), N=%u\n",SearchMode,modenames[SearchMode],N);
		if(useQuantileCheck)
		{
			printf("Verified output indices:");
			for(size_t t=0;t<targetIndices.size();t++)
				printf(" %u",targetIndices[t]);
			printf("\n");
		}
		printf("Cost function: %s (aligned weight %u, crossing weight %u)\n",
		       SimdAwareCost ? "SIMD-aware weighted" : "network size",SimdWeightAligned,SimdWeightCrossing);
		if(MaxActiveWires>0)
			printf("Register pressure limit: max %u active wires per layer\n",MaxActiveWires);
	}
	/* ===================================================================== */

	if((N%2) && use_symmetry)
	{
		if(Verbosity > 0)
		{
			printf("Warning: option 'Symmetric' ignored for odd number of inputs\n");
		}
		use_symmetry = false;
	}

	/* Initialize set of CEs to pick from */
	initalphabet();

	/* Create initial prefix network */
	switch(PrefixType)
	{
		case 1: // Fixed prefix
			prefix=copyValidPairs(FixedPrefix, N);
			break;
		case 2: // Greedy algorithm A 
			fillprefixGreedyA(prefix, GreedyPrefixSize);
			break;
		case 3: // Hybrid prefix
			fillprefixFixedThenGreedyA(prefix, GreedyPrefixSize);
			break;
		default: // No prefix
			prefix.clear();
			break;
	}

	if(Verbosity > 0)
	{
		printf("Prefix size: %lu\n",prefix.size());
	}
	
	/* Prepare a set of test vectors matching the prefix and the selected domain mode */
	prepareTestVectors(prefix);


	for(;;) // Outer loop - restart from here if restart is triggered (only applies if RestartRate!=0)
	{
		pairs=copyValidPairs(cp.getNetwork("InitialNetwork"),N);

		// Produce initial solution, simply by adding random pairs until we found a valid network. In case no postfix is present, we demand that the added pair
		// fixes at least one of the output inversions in the first detected error output vector, so it does at least some useful work to help sorting the outputs.
		// In case there is a postfix network, this check is not implemented.
		for(;;)
		{
			if(use_symmetry)
				symmetricExpansion(N, pairs,se);
			else
				se=pairs;
				
			appendNetwork(se,postfix);

			SortWord_t failed_output_pattern;

			bool initial_regok = (MaxActiveWires==0) || (MaxActiveWires>=N) || (computeMaxLayerWires(se)<=MaxActiveWires);

			if(!initial_regok && postfix.empty())
			{
				// Appending comparators cannot shrink an existing layer without a postfix.
				printf("Error: initial network exceeds MaxActiveWires=%u; try a higher limit or a different random seed.\n", MaxActiveWires);
				return -1;
			}

			if(initial_regok && testInitialPairsFromPrefixOutput(se, parallelpatterns_from_prefix, failed_output_pattern))
				break;

			Pair_t p;
			
			if(postfix.size() == 0) // Empty postfix: find a pattern that fixes an arbitrary inversion in the first failed output
			{
				bool found_useful_ce=false;
				do {
					p = RANDELEM(alphabet);
					
					if ( (((failed_output_pattern>>p.lo)&1)==1) && (((failed_output_pattern>>p.hi)&1)==0) )
						found_useful_ce = true;
						
					if(use_symmetry)
					{
						if ( (((failed_output_pattern>>((N-1)-p.hi))&1)==1) && (((failed_output_pattern>>((N-1)-p.lo))&1)==0) )
							found_useful_ce = true;			
					}
					
				}while( !found_useful_ce );
			}
			else // In case of postfix: just append a random initial pair to the core network, cannot directly determine good candidate from failed output pattern.
			{
				p = RANDELEM(alphabet); 
			}
			
			pairs.push_back( p );
		}

		Network_t totalnw;
		concatNetwork(prefix,se,totalnw);

		if(Verbosity>1)
		{
			printf("Initial network size: %lu\n",totalnw.size());
		}

		checkImproved(totalnw);

		if(pairs.size()==0)
		{
			/* Degenerate but legitimate case: the prefix (+postfix) alone already satisfies the
			   verification target of the selected domain mode. Nothing left to evolve. */
			if(Verbosity>0)
			{
				printf("Network complete: prefix (+postfix) already satisfies the target. Nothing to evolve.\n");
			}
			return 0;
		}

		for(;;) // Program never ends, keep trying to improve, we may restart in the outer loop however.
		{
			if(Verbosity>2)
			{
				itercount++;
				if(itercount >= iter_next_report)
				{
					clock_t t2 = clock();
					
					if((t2>t1)&&(t2>t0))
					{
						double t=(t2-t0)/(double)CLOCKS_PER_SEC;
						double dt=(t2-t1)/(double)CLOCKS_PER_SEC;
						printf("Iteration %lu  t=%.3lf s     %.1lf it/s\n", itercount, t,  (iter_next_report-iter_last_report)/dt ); 
					}
					
					t1=t2;
					iter_last_report = iter_next_report;
					iter_next_report += (1 + iter_next_report/10); // Report about each 10% increase of iteration count, avoid all too frequent output
				}
			}
			/* Determine number of mutations to use in this iteration */
			u32 nmods=1;

			if(MaxMutations>1)
			{
				nmods += mtRand()%MaxMutations;
			}
			
			/* Create a copy of the accepted set of pairs */
			newpairs=pairs;
			
			/* Apply the mutations */
			u32 modcount=0;
			while(modcount<nmods)
			{
				u32 r=attemptMutation(newpairs);
				if(r!=0)
				{
					modcount++;
				}
			}
			
			/* Create a symmetric expansion of the modified pairs (or just a copy if non-symmetric network) */
			if(use_symmetry)
			{
				symmetricExpansion(N, newpairs,se);
			}
			else
			{
				se=newpairs;
			}

			appendNetwork(se,postfix);

			/* Register pressure gate: reject candidates whose widest layer exceeds the
			   vector register budget (MaxActiveWires). */
			bool regok = (MaxActiveWires==0) || (MaxActiveWires>=N) || (computeMaxLayerWires(se)<=MaxActiveWires);

			/* Test whether the new postfix network yields a valid network for the selected
			   domain mode when combined with the prefix */
			if(regok && (se.size()>0) && testpairsFromPrefixOutput(se,parallelpatterns_from_prefix))
			{
				concatNetwork(prefix,se,totalnw);

				/* Accept the new postfix */
				pairs=newpairs;

				checkImproved(totalnw);
			}

			/* With low probability, add another pair random pair at a random place. Attempt to escape from local optimum. */
			if((EscapeRate>0) && ((mtRand()%EscapeRate)==0))
			{
				int a=mtRand()%(pairs.size()+1); // Random insertion position
				Pair_t p = RANDELEM(alphabet);

				// Determine if the random pair p could be added in the last layer
				bool hit_successor = false;
				for(Network_t::const_iterator it=pairs.begin()+a; it!= pairs.end() ; it++)
				{
					if((it->lo==p.lo)||(it->hi==p.lo)||(it->lo==p.hi)||(it->hi==p.hi))
					{
						hit_successor = true;
						break;
					}
				}

				if(force_valid_uphill_step && hit_successor)
				{
					pairs.insert(pairs.begin()+a, pairs[a]); // Prepend duplicate of existing pair right in front of it => Sorter with redundant pair will remain valid
				}
				else
				{
					pairs.insert(pairs.begin()+a, p); // Add random pair at the end of the network
				}
			}
		
			if((RestartRate>0) && ((mtRand()%RestartRate)==0))
			{
				if( Verbosity > 1)
				{
					printf("Restart.\n");
				}
				switch(PrefixType) // Recompute prefix if not fixed
				{
					case 1: // Fixed prefix - no update: vectors remain the same after restart
						break;
					case 2: // Greedy algorithm A
						fillprefixGreedyA(prefix, GreedyPrefixSize);
						prepareTestVectors(prefix);
						break;
					case 3: // Hybrid prefix
						fillprefixFixedThenGreedyA(prefix, GreedyPrefixSize);
						prepareTestVectors(prefix);
						break;
					default: // No prefix - no update: vectors remain the same after restart
						break;
				}

				break; // Restart using outer loop
			}
		}
	}

	return 0;
}
