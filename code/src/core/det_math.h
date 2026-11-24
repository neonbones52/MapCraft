#pragma once

#include <xmmintrin.h>

// The simulation's (mc_sim's) maths: the one place for it (CLAUDE.md and docs/SIMULATION.md, "Детерминизм").
//
// Allowed everywhere in mc_sim: + - * /, and from <cmath> only what IEEE 754 rounds exactly the same on every CPU:
// sqrt, floor, ceil, trunc, round, fabs, isfinite (and the like: min, max, copysign). Not allowed: the CRT's exp,
// pow, log, sin and the other transcendental functions (MSVC may pick different code for different CPUs), fma.
// When the simulation needs one, it is written here on + - * / only (named det_<name>, so the linter's ban on the
// CRT names never hits it); none is needed yet.

namespace mc::det
{
    // MXCSR for the simulation: round to nearest, every exception masked, flush-to-zero and denormals-are-zero off
    // (the state Windows x64 starts a thread with, set explicitly so nothing outside can leave another one).
    inline constexpr unsigned int kSimMxcsr = 0x1F80;

    // Sets the floating-point environment of the simulation on the calling thread. World::advance_days calls it on
    // entry (the simulation runs on the main thread for now). When the simulation gets worker threads, each of them
    // calls it at its start as well.
    inline void set_sim_fp_env()
    {
        _mm_setcsr(kSimMxcsr);
    }
}
