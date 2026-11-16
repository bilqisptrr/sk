#include "hutils.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>

// decomposeLayers is the independent reference implementation used for reporting.
static void check(const Network_t &nw)
{
    std::vector<Network_t> layers;
    decomposeLayers(nw, layers);
    u32 maxWires = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        SortWord_t mask = 0;
        for (size_t j = 0; j < layers[i].size(); ++j) {
            const Pair_t &p = layers[i][j];
            mask |= (1ULL << p.lo) | (1ULL << p.hi);
        }
        u32 wires = (u32)__builtin_popcountll(mask);
        if (wires > maxWires) maxWires = wires;
    }
    if (computeDepth(nw) != layers.size() || computeMaxLayerWires(nw) != maxWires) {
        std::fprintf(stderr, "Layer metrics mismatch for %zu comparators\n", nw.size());
        std::abort();
    }
}

int main()
{
    std::mt19937 random(1234);
    Network_t nw;
    check(nw);
    for (unsigned i = 0; i < 32; ++i) nw.push_back(Pair_t{(u8)(2*i), (u8)(2*i+1)});
    check(nw); // One full 64-wire layer.
    nw.clear();
    for (unsigned i = 0; i < 600; ++i) nw.push_back(Pair_t{0, 1});
    check(nw); // More layers than the stack buffer.
    nw.clear();
    nw.push_back(Pair_t{0, 0});
    nw.push_back(Pair_t{1, 1});
    check(nw); // Degenerate self-pairs still touch one wire each.
    for (unsigned trial = 0; trial < 5000; ++trial) {
        unsigned wires = 2 + random() % 63;
        unsigned length = random() % 400;
        nw.clear();
        for (unsigned i = 0; i < length; ++i) {
            unsigned a = random() % wires;
            unsigned b = random() % (wires - 1);
            if (b >= a) ++b;
            if (a > b) { unsigned tmp = a; a = b; b = tmp; }
            nw.push_back(Pair_t{(u8)a, (u8)b});
        }
        check(nw);
    }
    std::puts("Layer metrics: all tests passed");
}
