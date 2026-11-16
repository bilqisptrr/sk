#ifndef _HTYPES_H_
#define _HTYPES_H_

#include <stdint.h>
#include <vector>

#define NMAX (64)
#define PARWORDSIZE (64)

using std::size_t;

typedef uint64_t SortWord_t; ///< Needs to contain at least NMAX bits
typedef uint64_t BPWord_t;   ///< Bit-parallel operation word, needs to contain at least PARWORDSIZE bits
typedef uint32_t u32;
typedef uint8_t u8;

/**
 * CE representation
 */
struct Pair_t{
	u8 lo,hi; ///< low and high line indices connected by the element
	bool operator==(const Pair_t &p) const { return (lo==p.lo)&&(hi==p.hi);}
	bool operator!=(const Pair_t &p) const { return (lo!=p.lo)||(hi!=p.hi);}
};

typedef std::vector<Pair_t> Network_t;

typedef std::vector<SortWord_t> SinglePatternList_t;

typedef std::vector<BPWord_t> BitParallelList_t;


#endif // _HTYPES_H_
