#pragma once

#include <algorithm>
#include <array>
#include <chrono>

#include "game/calendar.h"

namespace mc
{
    // How fast the game's days go by: the player's speed (1..5) and the pause. Pure timing, it knows nothing of the
    // world: advance() calls the tick it is given once per day due.
    //   speeds 1..4  a month lasts 60, 30, 15 and 7 seconds
    //   speed 5      as fast as the CPU allows (EU4's top speed): ticks back to back until the frame's budget is
    //                spent, so the UI keeps its frames; at least one tick a frame
    // A new game starts paused at speed 3.
    class Pace
    {
    public:
        static constexpr int kSlowest = 1;
        static constexpr int kFastest = 5;

        // Speed 5's share of a frame: well below a 60 Hz frame, so the map and the UI keep theirs.
        static constexpr std::chrono::milliseconds kFrameBudget{ 8 };
        // A longer frame (a dragged window, a hitch) counts as this long, or it would drop a burst of days.
        static constexpr float kMaxFrame = 0.25f;

        void set_speed(int speed)
        {
            speed = std::clamp(speed, kSlowest, kFastest);
            if (speed != speed_) {
                speed_ = speed;
                carry_ = 0.0f;
            }
        }
        void change_speed(int step) { set_speed(speed_ + step); }
        void toggle_pause() { paused_ = !paused_; }

        [[nodiscard]] int speed() const { return speed_; }
        [[nodiscard]] bool paused() const { return paused_; }

        // `dt` seconds of real time passed: runs `tick()` once for every game day it brings, and returns how many
        // ran. Nothing while paused.
        template <class Tick>
        int advance(float dt, Tick&& tick)
        {
            if (paused_) {
                return 0;
            }
            int days = 0;
            if (speed_ == kFastest) {
                const auto deadline = std::chrono::steady_clock::now() + kFrameBudget;
                do {
                    tick();
                    ++days;
                } while (std::chrono::steady_clock::now() < deadline);
                return days;
            }
            carry_ += std::min(dt, kMaxFrame);
            const float day = kSecondsPerMonth[static_cast<std::size_t>(speed_ - kSlowest)] /
                              static_cast<float>(kDaysPerMonth);
            while (carry_ >= day) {
                carry_ -= day;
                tick();
                ++days;
            }
            return days;
        }

    private:
        // Real seconds per game month at speeds 1..4.
        static constexpr std::array<float, 4> kSecondsPerMonth = { 60.0f, 30.0f, 15.0f, 7.0f };

        int speed_ = 3;
        bool paused_ = true;
        float carry_ = 0.0f;   // seconds of the current day already gone (speeds 1..4)
    };
}
