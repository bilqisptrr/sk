#ifndef _PREFIX_PROCESSOR_H_
#define _PREFIX_PROCESSOR_H_

#include "htypes.h"
#include <vector>

/**
 * Given a prefix containing of 0 or more network pairs, computes the possible outputs of the (partially ordered) output set.
 * For an empty prefix, the result will contain 2**N patterns.
 * If the prefix is in itself a valid sorter, the result will contain N+1 patterns.
 * @param ninputs Number of inputs to the partially ordered network
 * @param prefix Prefix to process
 * @param patterns [OUT] List of output patterns
 */
void computePrefixOutputs(u8 ninputs, const Network_t &prefix, SinglePatternList_t &patterns);

/**
 * Applies a network to a RESTRICTED set of input patterns (e.g. patterns of a sliding window
 * with a presorted prefix and a single free "new sample" wire) and collects the unique output patterns.
 * This is the restricted-class counterpart of computePrefixOutputs: instead of enumerating all
 * possible (partially ordered) outputs for all 2**N inputs, only the outputs of the given input
 * patterns are computed. CE networks preserve the number of set bits, hence the restricted
 * input class maps onto a restricted output class.
 * @param ninputs Number of inputs to the network
 * @param nw Network to apply
 * @param inputs List of input patterns (bit i = value of wire i, wire 0 = smallest)
 * @param outputs [OUT] Unique output patterns
 */
void computeRestrictedOutputs(u8 ninputs, const Network_t &nw, const SinglePatternList_t &inputs, SinglePatternList_t &outputs);

/**
 * Converts a set of prefix output patterns to a bit parallel data structure to speed up testing of the "postfix" network.
 * The word size for packing is given by PARWORDSIZE
 * @param ninputs Number of inputs to the partially ordered network
 * @param singles Prefix output patterns to convert
 * @param use_symmetry Optimize using symmetry
 * @param parallels [OUT] Bit parallel representations of the patterns
 */
void convertToBitParallel(u8 ninputs, const SinglePatternList_t &singles, bool use_symmetry, BitParallelList_t &parallels);

/**
 * DSP domain-specific version of convertToBitParallel. Additionally builds, for each group of
 * PARWORDSIZE packed test vectors, one expected-value mask per target output index k:
 * bit b of mask k is 1 iff the b-th test vector of the group has at least (ninputs-k) set bits,
 * i.e. iff wire k of a correctly "ranked" output must be 1. These masks allow O(1) verification
 * of order statistics (quantile/median/min-K) instead of a full sortedness check.
 * Patterns that are already sorted are skipped (they are fixpoints of any CE network and always
 * satisfy the checks). If targets is empty, qmasks will be empty as well (classic full sorter testing).
 * @param ninputs Number of inputs to the partially ordered network
 * @param singles Prefix output patterns to convert
 * @param use_symmetry Optimize using symmetry (only valid if the target index set is mirror symmetric!)
 * @param targets Sorted list of output indices to be verified (empty for classic full sort)
 * @param parallels [OUT] Bit parallel representations of the patterns
 * @param qmasks [OUT] Expected-value masks: targets.size() words per group of PARWORDSIZE vectors
 */
void convertToBitParallelDSP(u8 ninputs, const SinglePatternList_t &singles, bool use_symmetry, const std::vector<u8> &targets,
                             BitParallelList_t &parallels, BitParallelList_t &qmasks);

/**
 * Tries to create a partially ordered network that (approximately) minimizes the number of possible outputs.
 * Function is called with the list of fixed pairs (optional, empty list if none).
 * Caller should take care of symmetry of fixed pairs.
 * @param ninputs Number of inputs to the partially ordered network
 * @param maxpairs Maximum number of pairs in the prefix
 * @param use_symmetry Set to true of the computed prefix needs to be symmetrical
 * @param prefix Contains fixed pairs as input (if any) and best prefix as output
 * @param rndgen Random number generator for shuffling
 * @return Number of outputs from partially ordered network (ninputs+1 if fully sorted, 2**ninputs worst case)
 */
SortWord_t createGreedyPrefix(u8 ninputs, u32 maxpairs, bool use_symmetry, Network_t &prefix, RandGen_t &rndgen);

#endif // _PREFIX_PROCESSOR_H_
