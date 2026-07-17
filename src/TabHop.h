#pragma once
#include <vector>
#include <cstdlib>

enum class HopKind { Next, Prev, JumpDigit, JumpLast };

struct Hop {
    HopKind kind;
    int     digit;
};

inline int RingDist(int a, int b, int k)
{
    const int d = std::abs(a - b);
    const int w = k - d;
    return d < w ? d : w;
}

// Optimal keystroke plan from active to target (1-indexed, 1..k) on the tab ring.
// Relative Next/Prev wrap; JumpDigit hits positions 1..8, JumpLast hits k. At most one
// jump, always first (an absolute jump erases prior position), so the whole strategy
// space is {no-jump walk} + {jump-to-anchor then walk} — a min over a fixed candidate set.
inline std::vector<Hop> PlanTabHops(int active, int target, int k)
{
    std::vector<Hop> best;
    if (k <= 0 || active == target) return best;

    auto walk = [&](int from, std::vector<Hop>& seq) {
        const int fwd = ((target - from) % k + k) % k;
        const int bwd = ((from - target) % k + k) % k;
        if (fwd <= bwd) for (int i = 0; i < fwd; ++i) seq.push_back({ HopKind::Next, 0 });
        else            for (int i = 0; i < bwd; ++i) seq.push_back({ HopKind::Prev, 0 });
    };

    walk(active, best);

    auto consider = [&](Hop jump, int land) {
        std::vector<Hop> c;
        c.push_back(jump);
        walk(land, c);
        if (c.size() < best.size()) best = std::move(c);
    };
    for (int d = 1; d <= 8 && d <= k; ++d) consider({ HopKind::JumpDigit, d }, d);
    consider({ HopKind::JumpLast, 0 }, k);

    return best;
}
