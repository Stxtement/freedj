#pragma once

#include <cmath>

namespace pathboard
{

// Tempo sync: how far must a deck be pitched so that it runs at the other deck's tempo?
// Half- and double-time are considered: a 95.5 BPM track can be matched to a 191 BPM track at its own
// speed (every second beat lines up), which needs 0 % pitch instead of +100 %.
struct SyncPlan
{
    bool possible = false;      // both tempi known
    bool withinLimit = false;   // |percent| <= maxPercent
    double percent = 0.0;       // pitch change to apply (+ = faster)
    double targetFactor = 1.0;  // 1 = same tempo, 0.5 = target runs half-time, 2 = target runs double-time
};

inline SyncPlan planSync (double ownOriginalBpm, double targetPlaybackBpm, double maxPercent)
{
    SyncPlan plan;
    if (ownOriginalBpm <= 0.0 || targetPlaybackBpm <= 0.0)
        return plan;

    plan.possible = true;
    plan.percent = 1.0e9;
    for (double factor : { 1.0, 0.5, 2.0 })
    {
        const double percent = (targetPlaybackBpm * factor / ownOriginalBpm - 1.0) * 100.0;
        if (std::abs (percent) < std::abs (plan.percent) - 1.0e-9)   // prefer factor 1 on ties
        {
            plan.percent = percent;
            plan.targetFactor = factor;
        }
    }
    plan.withinLimit = std::abs (plan.percent) <= maxPercent;
    return plan;
}

} // namespace pathboard
