#pragma once
#include <vector>

enum class HopKind { Next, Prev, JumpDigit, JumpLast };

struct Hop {
    HopKind kind;
    int     digit;
};

// Optimal keystroke plan from active to target (1-indexed, 1..k) on the tab ring.
// Relative Next/Prev wrap; JumpDigit hits positions 1..8, JumpLast hits k. At most one
// jump, always first (an absolute jump erases prior position), so the whole strategy
// space is {no-jump walk} + {jump-to-anchor then walk} — a min over a fixed candidate set.
// activeKnown=false when the cache has no confirmed active tab: the no-jump walk would run
// from a guessed origin and land wrong, so drop it — every anchored plan is origin-independent.
inline std::vector<Hop> PlanTabHops(int active, int target, int k, bool activeKnown = true)
{
    std::vector<Hop> best;
    if (k <= 0) return best;
    if (activeKnown && active == target) return best;

    auto walk = [&](int from, std::vector<Hop>& seq) {
        const int fwd = ((target - from) % k + k) % k;
        const int bwd = ((from - target) % k + k) % k;
        if (fwd <= bwd) for (int i = 0; i < fwd; ++i) seq.push_back({ HopKind::Next, 0 });
        else            for (int i = 0; i < bwd; ++i) seq.push_back({ HopKind::Prev, 0 });
    };

    bool have = false;
    if (activeKnown) { walk(active, best); have = true; }

    auto consider = [&](Hop jump, int land) {
        std::vector<Hop> c;
        c.push_back(jump);
        walk(land, c);
        if (!have || c.size() < best.size()) { best = std::move(c); have = true; }
    };
    for (int d = 1; d <= 8 && d <= k; ++d) consider({ HopKind::JumpDigit, d }, d);
    consider({ HopKind::JumpLast, 0 }, k);

    return best;
}
