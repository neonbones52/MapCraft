#include "game/modifiers.h"

#include <algorithm>
#include <cstdint>

namespace mc
{
    void sum_country_modifiers(const ModifierBundles& bundles, const CountryStates& countries, CountryModifiers* out)
    {
        out->count = countries.count;
        const std::size_t n = padded(countries.count);
        const std::size_t words = countries.modifier_words;
        const std::size_t bundle_count = bundles.list.size();
        const std::uint64_t* sets = countries.modifiers.data();
        auto sum = [&](const std::vector<Fixed>& value, Column<Fixed>& column) {
            column.resize(n);   // the same size after the first time: no allocation
            std::fill(column.begin(), column.end(), bundles.base != 0 ? value[bundles.base] : 0);
            Fixed* s = column.data();
            for (std::size_t b = 1; b < bundle_count; ++b) {
                const Fixed v = value[b];
                if (b == bundles.base || v == 0) {
                    continue;
                }
                const std::size_t word = b / 64;
                const unsigned shift = static_cast<unsigned>(b % 64);
                for (std::size_t c = 0; c < n; ++c) {
                    // v where the country has the bundle, else 0: a mask, no branch.
                    const auto has = static_cast<Fixed>((sets[c * words + word] >> shift) & 1u);
                    s[c] += v & -has;
                }
            }
        };
#define MC_SUM(name) sum(bundles.name, out->name);
        MC_COUNTRY_MODIFIERS(MC_SUM)
#undef MC_SUM
    }
}
