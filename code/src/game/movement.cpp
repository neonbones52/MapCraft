#include "game/movement.h"

#include <algorithm>
#include <bit>

#include <emmintrin.h>

#include "core/map_data.h"
#include "game/armies.h"

namespace mc
{
    void Movement::build(const MapData& map, const Content& content, const WorldState& state)
    {
        const std::uint32_t n = map.count;
        auto land = [&](std::uint32_t id) { return id > 0 && id < n && map.kind[id] == ProvinceKind::land; };
        auto multiplier = [&](std::uint32_t id) {
            const std::uint8_t t = id < state.provinces.terrain.size() ? state.provinces.terrain[id] : 0;
            return t < content.terrain_movement_cost.size() ? content.terrain_movement_cost[t] : 1.0f;
        };
        offsets_.assign(padded(std::size_t{ n } + 1), 0);
        to_.clear();
        cost_.clear();
        back_.clear();
        for (std::uint32_t p = 0; p < n; ++p) {
            offsets_[p] = static_cast<std::uint32_t>(to_.size());
            if (!land(p)) {
                continue;
            }
            for (std::uint32_t k = map.adjacency_offsets[p]; k < map.adjacency_offsets[p + 1]; ++k) {
                const std::uint16_t q = map.adjacency_ids[k];
                // The march path's length (the curve over the two provinces' land, the same both ways): back_ is
                // exactly the other way's cost_. No path, no step.
                const float px = map.march_length[k];
                if (!land(q) || !(px > 0.0f)) {
                    continue;
                }
                to_.push_back(q);
                cost_.push_back(px * multiplier(q));
                back_.push_back(px * multiplier(p));
            }
        }
        offsets_[n] = static_cast<std::uint32_t>(to_.size());
        const std::size_t steps = to_.size();
        to_.resize(padded(steps), 0);
        cost_.resize(padded(steps), 0.0f);
        back_.resize(padded(steps), 0.0f);

        way_.assign(padded(n), 0.0f);
        toward_.assign(padded(n), 0);
        seen_.assign(padded(n), 0);
        settled_.assign(padded(n), 0);
        stamp_ = 0;
        heap_cost_.assign(padded(steps + 1), 0.0f);
        heap_province_.assign(padded(steps + 1), 0);
        heap_size_ = 0;
        arrived_.assign(kArmySlots / 64, 0);
        army_step_.assign(kArmySlots, 0.0f);
        army_after_.assign(kArmySlots, 0);
    }

    void Movement::rebuild_steps(const WorldState& state)
    {
        const Armies& armies = state.armies;
        std::fill(army_step_.begin(), army_step_.end(), 0.0f);
        std::fill(army_after_.begin(), army_after_.end(), std::uint16_t{ 0 });
        for_each_alive(armies.alive, [&](std::uint32_t a) {
            if (armies.next[a] != 0) {
                army_step_[a] = step_cost(armies.province[a], armies.next[a]);
                look_ahead(armies, static_cast<std::uint16_t>(a));
            }
        });
    }

    float Movement::step_cost(std::uint16_t from, std::uint16_t to) const
    {
        if (std::size_t{ from } + 1 >= offsets_.size()) {
            return 0.0f;
        }
        for (std::uint32_t k = offsets_[from]; k < offsets_[from + 1]; ++k) {
            if (to_[k] == to) {
                return cost_[k];
            }
        }
        return 0.0f;
    }

    void Movement::begin_step(Armies& armies, std::uint16_t slot, std::uint16_t from, std::uint16_t to)
    {
        armies.next[slot] = to;
        armies.progress[slot] = 0.0f;
        army_step_[slot] = step_cost(from, to);
    }

    void Movement::look_ahead(const Armies& armies, std::uint16_t slot)
    {
        army_after_[slot] = first_step(armies.next[slot], armies.destination[slot]);   // 0 if next is the destination
    }

    void Movement::stop(Armies& armies, std::uint16_t slot)
    {
        army_after_[slot] = 0;
        armies.next[slot] = 0;
        armies.destination[slot] = 0;
        armies.progress[slot] = 0.0f;
        army_step_[slot] = 0.0f;
    }

    bool Movement::order(WorldState& state, std::uint16_t slot, std::uint16_t destination)
    {
        Armies& armies = state.armies;
        const std::uint16_t here = armies.province[slot];
        const std::uint16_t next = armies.next[slot];
        if (next != 0 && armies.progress[slot] > kMoveLock * army_step_[slot]) {
            // Locked: the march goes on from the province the step leads to.
            if (destination != next && first_step(next, destination) == 0) {
                return false;
            }
            armies.destination[slot] = destination;
            look_ahead(armies, slot);
            return true;
        }
        // Standing, or free to turn: from the province it is in.
        if (destination == here) {
            stop(armies, slot);
            return true;
        }
        const std::uint16_t step = first_step(here, destination);
        if (step == 0) {
            return false;
        }
        if (step != next) {
            begin_step(armies, slot, here, step);
        }
        armies.destination[slot] = destination;
        look_ahead(armies, slot);
        return true;
    }

    void Movement::day(WorldState& state)
    {
        Armies& armies = state.armies;
        const std::size_t used = used_slots(armies.alive);
        // Eight slots at a time (SSE2; the columns are 64-byte aligned and the used slots whole mask words): every
        // marching army (next != 0) walks a day, standing and free slots add 0; then the ones whose progress reached
        // their step's cost are marked in arrived_.
        const __m128 speed = _mm_set1_ps(kMarchPerDay);
        const __m128i none = _mm_setzero_si128();
        std::uint64_t bits = 0;
        for (std::size_t s = 0; s < used; s += 8) {
            const __m128i next = _mm_load_si128(reinterpret_cast<const __m128i*>(&armies.next[s]));
            const __m128i standing = _mm_cmpeq_epi16(next, none);
            const __m128 standing_lo = _mm_castsi128_ps(_mm_unpacklo_epi16(standing, standing));
            const __m128 standing_hi = _mm_castsi128_ps(_mm_unpackhi_epi16(standing, standing));
            const __m128 lo = _mm_add_ps(_mm_load_ps(&armies.progress[s]), _mm_andnot_ps(standing_lo, speed));
            const __m128 hi = _mm_add_ps(_mm_load_ps(&armies.progress[s + 4]), _mm_andnot_ps(standing_hi, speed));
            _mm_store_ps(&armies.progress[s], lo);
            _mm_store_ps(&armies.progress[s + 4], hi);
            const __m128 done_lo = _mm_andnot_ps(standing_lo, _mm_cmpge_ps(lo, _mm_load_ps(&army_step_[s])));
            const __m128 done_hi = _mm_andnot_ps(standing_hi, _mm_cmpge_ps(hi, _mm_load_ps(&army_step_[s + 4])));
            const auto eight = static_cast<std::uint64_t>(_mm_movemask_ps(done_lo) | _mm_movemask_ps(done_hi) << 4);
            bits |= eight << (s % 64);
            if (s % 64 == 56) {
                arrived_[s / 64] = bits;
                bits = 0;
            }
        }
        // They arrive, in slot order: the march ends or the next step begins with what is left of the day.
        for (std::size_t w = 0; w < used / 64; ++w) {
            for (std::uint64_t b = arrived_[w]; b != 0; b &= b - 1) {
                const auto s = static_cast<std::uint16_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(b)));
                const std::uint16_t here = armies.next[s];
                const float done = army_step_[s];
                armies.province[s] = here;
                const std::uint16_t step = army_after_[s];   // found when this step began
                if (step == 0) {
                    stop(armies, s);
                } else {
                    armies.next[s] = step;
                    armies.progress[s] -= done;
                    army_step_[s] = step_cost(here, step);
                    look_ahead(armies, s);
                }
            }
        }
    }

    std::uint16_t Movement::first_step(std::uint16_t from, std::uint16_t to)
    {
        if (from == to || std::size_t{ from } >= way_.size() || std::size_t{ to } >= way_.size()) {
            return 0;
        }
        if (++stamp_ == 0) {
            std::fill(seen_.begin(), seen_.end(), 0u);
            std::fill(settled_.begin(), settled_.end(), 0u);
            stamp_ = 1;
        }
        // Dijkstra from the destination backwards: popping u, its neighbour v is reached by the step v -> u (back_);
        // a province's step toward the destination is the province it was reached from. Stops once `from` is
        // settled: what is settled does not depend on when the search stops.
        heap_size_ = 0;
        way_[to] = 0.0f;
        toward_[to] = 0;
        seen_[to] = stamp_;
        push(0.0f, to);
        while (heap_size_ > 0) {
            float cost = 0.0f;
            const std::uint16_t u = pop(&cost);
            if (settled_[u] == stamp_) {
                continue;   // a stale pair
            }
            settled_[u] = stamp_;
            if (u == from) {
                return toward_[u];
            }
            for (std::uint32_t k = offsets_[u]; k < offsets_[u + 1]; ++k) {
                const std::uint16_t v = to_[k];
                if (settled_[v] == stamp_) {
                    continue;
                }
                const float way = cost + back_[k];
                if (seen_[v] != stamp_ || way < way_[v]) {
                    seen_[v] = stamp_;
                    way_[v] = way;
                    toward_[v] = u;
                    push(way, v);
                }
            }
        }
        return 0;
    }

    // A binary min-heap of (cost, province), ordered by cost, then by province: a full order.
    void Movement::push(float cost, std::uint16_t province)
    {
        auto before = [&](float c, std::uint16_t p, std::uint32_t i) {
            return c < heap_cost_[i] || (c == heap_cost_[i] && p < heap_province_[i]);
        };
        std::uint32_t i = heap_size_++;
        while (i > 0) {
            const std::uint32_t parent = (i - 1) / 2;
            if (!before(cost, province, parent)) {
                break;
            }
            heap_cost_[i] = heap_cost_[parent];
            heap_province_[i] = heap_province_[parent];
            i = parent;
        }
        heap_cost_[i] = cost;
        heap_province_[i] = province;
    }

    std::uint16_t Movement::pop(float* cost)
    {
        *cost = heap_cost_[0];
        const std::uint16_t top = heap_province_[0];
        const std::uint32_t size = --heap_size_;
        const float c = heap_cost_[size];
        const std::uint16_t p = heap_province_[size];
        std::uint32_t i = 0;
        for (;;) {
            std::uint32_t child = 2 * i + 1;
            if (child >= size) {
                break;
            }
            if (child + 1 < size && (heap_cost_[child + 1] < heap_cost_[child] ||
                                     (heap_cost_[child + 1] == heap_cost_[child] &&
                                      heap_province_[child + 1] < heap_province_[child]))) {
                ++child;
            }
            if (!(heap_cost_[child] < c || (heap_cost_[child] == c && heap_province_[child] < p))) {
                break;
            }
            heap_cost_[i] = heap_cost_[child];
            heap_province_[i] = heap_province_[child];
            i = child;
        }
        heap_cost_[i] = c;
        heap_province_[i] = p;
        return top;
    }
}
