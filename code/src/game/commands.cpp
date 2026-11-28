#include "game/commands.h"

#include <algorithm>

namespace mc
{
    void CommandQueue::post(std::uint32_t day, const Command& command)
    {
        day_.push_back(day);
        number_.push_back(next_number_++);
        kind_.push_back(command.kind);
        country_.push_back(command.country);
        army_slot_.push_back(command.army.slot);
        army_generation_.push_back(command.army.generation);
        province_.push_back(command.province);
        target_.push_back(command.target);
        due_.reserve(day_.size());   // here, outside the day, so running them never allocates
    }

    void CommandQueue::sort_due()
    {
        // A full order: the numbers are unique.
        std::sort(due_.begin(), due_.end(), [&](std::uint32_t a, std::uint32_t b) {
            return day_[a] != day_[b] ? day_[a] < day_[b] : number_[a] < number_[b];
        });
    }

    void CommandQueue::drop(std::uint32_t today)
    {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < day_.size(); ++i) {
            if (day_[i] > today) {
                day_[kept] = day_[i];
                number_[kept] = number_[i];
                kind_[kept] = kind_[i];
                country_[kept] = country_[i];
                army_slot_[kept] = army_slot_[i];
                army_generation_[kept] = army_generation_[i];
                province_[kept] = province_[i];
                target_[kept] = target_[i];
                ++kept;
            }
        }
        day_.resize(kept);
        number_.resize(kept);
        kind_.resize(kept);
        country_.resize(kept);
        army_slot_.resize(kept);
        army_generation_.resize(kept);
        province_.resize(kept);
        target_.resize(kept);
    }
}
