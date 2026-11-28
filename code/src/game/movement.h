#pragma once

#include <cstdint>

#include "game/game_data.h"

namespace mc
{
    class MapData;

    // How far an army marches in a day, in the units of a step's cost (map px times the terrain's movement_cost):
    // 6.3 px of the plains. EU4's is 2.1 (a step costs the px between the two centres / 3, an army makes 0.7 of it a
    // day; MOVE.md of the EU4 research); our provinces are bigger in px, so the user's decision is three times that:
    // a step to a neighbour (67 px at the median) takes about 11 days over the plains.
    inline constexpr float kMarchPerDay = 6.3f;
    // An army past this share of its step can no longer turn back (EU4's MOVE_LOCK_PERCENTAGE).
    inline constexpr float kMoveLock = 0.5f;

    // The armies' march (docs/GAMEDATA.md, "Движение армий"): the rules of the command "go there" and of each day's
    // walking. Owned by World; its data is cache, rebuilt from the map, the content and the state after a load:
    //   the graph    the land provinces' steps (wasteland, seas and lakes are not crossed): to each land neighbour,
    //                the length of the march path (MapData::march_length: the curve from unit point to unit point
    //                over the land of the two provinces, tools/mapbake/marches.py) times the movement_cost
    //                (data/common/terrain.csv) of the province the step goes into, and whether the path crosses a
    //                river (MapData::march_river: the battle's river crossing, game/combat.h)
    //   the steps    per army slot, the cost of the step it is on (0 standing) and the step after it (the
    //                province the march goes on to from the next one; 0 where it ends there): found one step ahead,
    //                when a step begins, so the map's figure walks on through a province without a stop, and
    //                taken as the next step when the army arrives (the graph does not change, so it is exactly the
    //                step a search would find then)
    //   the search   the working columns of the path search, sized once and reused
    // An army on a step stays in its province until the step's progress reaches its cost; then it is in the next
    // province, the rest of the progress carried into the following step (EU4's ProgressMovementOneDay), or 0 where
    // the march ends. Up to half of the step (kMoveLock) a new order takes effect from the province it is in: the
    // same first step keeps its progress, another one starts from 0, an order to its own province stops it there.
    // Past half the step is locked: a new order takes effect from the province the step leads to.
    // The way is the cheapest by the steps' costs, ties to the lower province id; it is searched from the destination
    // backwards, so the step from any province on the way does not depend on where the march began.
    class Movement
    {
    public:
        // The graph, from the map's unit points and the provinces' terrain (state.provinces.terrain: data/setup, no
        // rule changes it yet), and the working columns. After every load.
        void build(const MapData& map, const Content& content, const WorldState& state);
        // The cost of each marching army's step, from the state. After every load and restart (after build).
        void rebuild_steps(const WorldState& state);

        // The command's effect: the army in `slot` marches to `destination`. False, and nothing changes, when there
        // is no way over land.
        bool order(WorldState& state, std::uint16_t slot, std::uint16_t destination);
        // One day of every march, in slot order.
        void day(WorldState& state);
        // The army in `slot` stops where it is, wherever it was on its step (it joins a battle: game/combat.h).
        void halt(WorldState& state, std::uint16_t slot) { stop(state.armies, slot); }
        // The army in `slot` ended a step today (the last day()): it came into the province it is in.
        [[nodiscard]] bool arrived_today(std::uint16_t slot) const { return (arrived_[slot / 64] >> (slot % 64)) & 1u; }
        // It arrived today over a river (MapData::march_river: its step's march path crosses one).
        [[nodiscard]] bool crossed_river_today(std::uint16_t slot) const
        {
            return (crossed_[slot / 64] >> (slot % 64)) & 1u;
        }

        // The cost of the way from `from` to every land province it reaches (a full search over the steps forward);
        // way_from(p) reads it until the next search or order, < 0 where there is no way.
        void search_from(std::uint16_t from);
        [[nodiscard]] float way_from(std::uint16_t province) const
        {
            return province < settled_.size() && settled_[province] == stamp_ ? way_[province] : -1.0f;
        }

        // The cost of the step from province `from` into its neighbour `to`; 0 if it is not a step.
        [[nodiscard]] float step_cost(std::uint16_t from, std::uint16_t to) const;
        // Whether the step from `from` into `to` crosses a river (false if it is not a step).
        [[nodiscard]] bool step_river(std::uint16_t from, std::uint16_t to) const;
        // The cost of the step the army in `slot` is on; 0 while it stands.
        [[nodiscard]] float army_step(std::uint16_t slot) const { return army_step_[slot]; }
        // The province the march of the army in `slot` goes on to after its next one; 0 if it ends there.
        [[nodiscard]] std::uint16_t army_after(std::uint16_t slot) const { return army_after_[slot]; }

    private:
        // The graph's index of the step from `from` into `to`, -1 if it is not a step.
        [[nodiscard]] int step_index(std::uint16_t from, std::uint16_t to) const;
        // The first step from `from` on the way to `to`: the neighbour, or 0 if there is no way (or from == to).
        [[nodiscard]] std::uint16_t first_step(std::uint16_t from, std::uint16_t to);
        void begin_step(Armies& armies, std::uint16_t slot, std::uint16_t from, std::uint16_t to);
        // The step after the next, for the army's destination as it is now.
        void look_ahead(const Armies& armies, std::uint16_t slot);
        void stop(Armies& armies, std::uint16_t slot);
        void push(float cost, std::uint16_t province);
        [[nodiscard]] std::uint16_t pop(float* cost);

        // The graph, CSR by province id: the steps out of province p are [offsets_[p], offsets_[p + 1]), each to
        // to_[k] at cost_[k]; back_[k] is the cost of the step the other way (into p), for the backward search.
        Column<std::uint32_t> offsets_;
        Column<std::uint16_t> to_;
        Column<float> cost_;
        Column<float> back_;
        Column<std::uint8_t> river_;         // 1: the step's march path crosses a river (MapData::march_river)
        Column<float> army_step_;            // per army slot
        Column<std::uint16_t> army_after_;   // per army slot

        // The search: per province, the cost of the way to the destination and the step toward it, valid where
        // seen_ is this search's stamp; settled_ marks the final ones. The heap holds (cost, province) pairs,
        // stale ones included (one push per improvement: at most one per step of the graph).
        Column<float> way_;
        Column<std::uint16_t> toward_;
        Column<std::uint32_t> seen_;
        Column<std::uint32_t> settled_;
        std::uint32_t stamp_ = 0;
        Column<float> heap_cost_;
        Column<std::uint16_t> heap_province_;
        std::uint32_t heap_size_ = 0;
        // The day's arrivals: bit s of word s / 64 = the army in slot s ends its step today.
        Column<std::uint64_t> arrived_;
        Column<std::uint64_t> crossed_;   // the same bits: of those, the ones whose step crossed a river
    };
}
