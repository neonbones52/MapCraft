#include "core/labels.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

#include "core/map_data.h"

// Port of archeologyEU4/Labels/LabelPython (placement/*.py, rendering/font.py layout) and the reverse engineering it
// implements (LABEL.md). Function and variable names follow the Python; float32 steps are spelled out with F() where
// the original narrows, so the arithmetic matches it operation by operation.

namespace mc
{
    namespace
    {
        using F = float;

        struct PointI
        {
            std::int32_t x;
            std::int32_t y;
        };

        struct Vec2
        {
            F x;
            F y;
        };

        struct Vec2d
        {
            double x;
            double y;
        };

        Vec2d operator+(Vec2d a, Vec2d b) { return { a.x + b.x, a.y + b.y }; }
        Vec2d operator-(Vec2d a, Vec2d b) { return { a.x - b.x, a.y - b.y }; }
        Vec2d operator*(Vec2d a, double s) { return { a.x * s, a.y * s }; }
        double dot(Vec2d a, Vec2d b) { return a.x * b.x + a.y * b.y; }
        Vec2d normalize(Vec2d v) { const double l = std::sqrt(dot(v, v)); return { v.x / l, v.y / l }; }
        Vec2d ccw(Vec2d v) { return { -v.y, v.x }; }

        // ------------------------------------------------------------------------ BresenhamLine (0x141566D50)

        std::vector<PointI> bresenham(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                                      std::int32_t capacity)
        {
            const bool leading_x = std::abs(x1 - x0) >= std::abs(y1 - y0);
            std::int32_t m0 = leading_x ? x0 : y0, s0 = leading_x ? y0 : x0;
            std::int32_t m1 = leading_x ? x1 : y1, s1 = leading_x ? y1 : x1;
            if (m0 > m1) {
                std::swap(m0, m1);
                std::swap(s0, s1);
            }
            const std::int32_t delta = m1 - m0;
            const std::int32_t minor = std::abs(s1 - s0);
            const std::int32_t step = s1 > s0 ? 1 : -1;
            std::vector<PointI> result;
            result.reserve(static_cast<std::size_t>(std::min<std::int64_t>(std::int64_t(delta) + 1, capacity)));
            std::int32_t error = 0;
            std::int32_t s = s0;
            for (std::int32_t m = m0; m <= m1; ++m) {
                result.push_back(leading_x ? PointI{ m, s } : PointI{ s, m });
                if (static_cast<std::int32_t>(result.size()) == capacity) {
                    return result;
                }
                error += minor;
                if (2 * error >= delta) {
                    s += step;
                    error -= delta;
                }
            }
            return result;
        }

        // ---------------------------------------------------------------------------- ValidateLine (0x140FD1930)

        struct Group
        {
            const LabelRaster& raster;
            const std::vector<std::int32_t>& table;
            std::int32_t gid;
            bool water_between;
            bool water_ends;
        };

        bool anchor(PointI point, const Group& g)
        {
            const auto x = static_cast<std::int16_t>(point.x);
            const auto y = static_cast<std::int16_t>(point.y);
            if (x < 0 || x >= g.raster.width || y < 0 || y >= g.raster.height) {
                return false;
            }
            const std::uint16_t p = g.raster.pixels[std::size_t(y) * g.raster.width + x];
            return p != 0 && g.table[p] == g.gid && (g.raster.land[p] || g.water_ends);
        }

        struct Best
        {
            std::int32_t start = 0;
            std::int32_t count = 0;
            std::int32_t distance = std::numeric_limits<std::int32_t>::max();
        };

        void consider(const std::vector<PointI>& points, std::int32_t end, std::int32_t count, Best* best,
                      std::int32_t px, std::int32_t py)
        {
            if (count == 0) {
                return;
            }
            const std::int32_t start = end - count;
            if (px == -1) {
                if (count > best->count) {
                    *best = { start, count, best->distance };
                }
                return;
            }
            std::int32_t distance = std::numeric_limits<std::int32_t>::max();
            for (std::int32_t j = start; j < end; ++j) {
                const std::int32_t dx = points[j].x - px;
                const std::int32_t dy = points[j].y - py;
                distance = std::min(distance, dx * dx + dy * dy);
            }
            std::int32_t a = count;
            std::int32_t b = best->count;
            if (distance < best->distance) {
                a = static_cast<std::int32_t>(F(count) * F(1.4));
            } else if (best->distance < distance) {
                b = static_cast<std::int32_t>(F(best->count) * F(1.4));
            }
            if (a > b) {
                *best = { start, count, distance };
            }
        }

        std::vector<PointI> validate(std::vector<PointI> points, const Group& g, std::int32_t px = -1,
                                     std::int32_t py = -1)
        {
            std::int32_t n = static_cast<std::int32_t>(points.size());
            if (n == 0) {
                return points;
            }
            // Outer ends: the last, then the first point that may anchor the line (own group, land or water ends).
            for (std::int32_t i = n - 1; i >= 0; --i) {
                if (anchor(points[i], g)) {
                    n = i + 1;
                    break;
                }
            }
            for (std::int32_t i = 0; i < n; ++i) {
                if (anchor(points[i], g)) {
                    points.erase(points.begin(), points.begin() + i);
                    n -= i;
                    break;
                }
            }
            points.resize(n);
            // Runs of allowed points: own group, any lake, other water when water_between.
            Best best;
            std::int32_t run = 0;
            for (std::int32_t i = 0; i < n; ++i) {
                const auto x = static_cast<std::int16_t>(points[i].x);
                const auto y = static_cast<std::int16_t>(points[i].y);
                if (x < 0 || x >= g.raster.width || y < 0 || y >= g.raster.height) {
                    continue;   // deliberately neither increments nor ends the run (0x140FD1DF9)
                }
                const std::uint16_t p = g.raster.pixels[std::size_t(y) * g.raster.width + x];
                if (p != 0 && (g.table[p] == g.gid || g.raster.lake[p] || (!g.raster.land[p] && g.water_between))) {
                    ++run;
                } else {
                    consider(points, i, run, &best, px, py);
                    run = 0;
                }
            }
            consider(points, n, run, &best, px, py);
            return { points.begin() + best.start, points.begin() + best.start + best.count };
        }

        std::vector<PointI> validated(const std::array<Vec2, 2>& line, const Group& g, Vec2 preferred)
        {
            const std::int32_t w = g.raster.width;
            const std::int32_t h = g.raster.height;
            auto points = bresenham(static_cast<std::int32_t>(line[0].x), static_cast<std::int32_t>(line[0].y),
                                    static_cast<std::int32_t>(line[1].x), static_cast<std::int32_t>(line[1].y),
                                    w + h);
            return validate(std::move(points), g, static_cast<std::int32_t>(preferred.x),
                            static_cast<std::int32_t>(preferred.y));
        }

        // ------------------------------------------------------------------ AddNameArea baseline (0x140FD4380)

        struct Baseline
        {
            std::array<Vec2, 2> line;
            std::array<F, 4> box;
            std::uint64_t area = 0;
            F length = 0.0f;
        };

        bool baseline(const Group& g, Baseline* out)
        {
            const LabelRaster& raster = g.raster;
            std::array<std::uint64_t, 6> sums{};
            std::array<std::int32_t, 4> box{ raster.width, raster.height, 0, 0 };
            bool any = false;
            for (std::size_t p = 1; p < raster.count(); ++p) {       // ascending ids, not BFS order
                if (g.table[p] != g.gid) {
                    continue;
                }
                for (int i = 0; i < 6; ++i) {
                    sums[i] += raster.moments[p][i];
                }
                if (raster.moments[p][0] > 0) {
                    const auto& b = raster.bbox[p];
                    box = { std::min(box[0], b[0]), std::min(box[1], b[1]), std::max(box[2], b[2]),
                            std::max(box[3], b[3]) };
                    any = true;
                }
            }
            const auto [n, sx, sy, sxy, sxx, syy] = sums;
            const auto d = static_cast<std::int64_t>(n * sxy - sx * sy);
            const auto vx = static_cast<std::int64_t>(n * sxx - sx * sx);
            const auto vy = static_cast<std::int64_t>(n * syy - sy * sy);
            if (n == 0 || vx == 0 || vy == 0 || !any) {
                return false;
            }
            const F cx = F(double(sx) / double(n));
            const F cy = F(double(sy) / double(n));
            const F a = F(double(d) / double(vx));
            const F c = F(double(d) / double(vy));
            const F b = F(cy - F(a * cx));
            const F intercept = F(cx - F(c * cy));
            const F x0 = F(box[0]), y0 = F(box[1]), x1 = F(box[2]), y1 = F(box[3]);
            const std::array<std::array<Vec2, 2>, 2> original{ {
                { Vec2{ x0, F(F(a * x0) + b) }, Vec2{ x1, F(F(a * x1) + b) } },
                { Vec2{ F(F(c * y0) + intercept), y0 }, Vec2{ F(F(c * y1) + intercept), y1 } },
            } };
            auto best = original;
            std::array<std::int32_t, 2> counts{ 0, 0 };
            const std::int32_t w = raster.width;
            const std::int32_t h = raster.height;
            for (const std::int32_t k : { -1, 0, 1 }) {
                auto candidates = original;
                for (int axis = 0; axis < 2; ++axis) {
                    const F dx = F(best[axis][1].x - best[axis][0].x);
                    const F dy = F(best[axis][1].y - best[axis][0].y);
                    const F length = F(std::sqrt(F(F(dx * dx) + F(dy * dy))));
                    const F inv = F(F(1.0f) / length);
                    const Vec2 normal{ F(-dy * inv), F(dx * inv) };
                    const F shift = F(3 * k);
                    for (auto& p : candidates[axis]) {
                        p = { F(p.x + F(shift * normal.x)), F(p.y + F(shift * normal.y)) };
                    }
                }
                if (k != 0) {
                    bool outside = false;
                    for (const auto& line : candidates) {
                        for (const auto& p : line) {
                            outside |= p.x < 0 || p.x >= F(w) || p.y < 0 || p.y >= F(h);
                        }
                    }
                    if (outside) {
                        continue;   // the whole pair of this k is skipped
                    }
                }
                std::array<std::vector<PointI>, 2> runs;
                for (int axis = 0; axis < 2; ++axis) {
                    const auto& line = candidates[axis];
                    runs[axis] = validate(bresenham(static_cast<std::int32_t>(line[0].x),
                                                    static_cast<std::int32_t>(line[0].y),
                                                    static_cast<std::int32_t>(line[1].x),
                                                    static_cast<std::int32_t>(line[1].y), w + h), g);
                }
                for (int axis = 0; axis < 2; ++axis) {
                    const auto& run = runs[axis];
                    const auto count = static_cast<std::int32_t>(run.size());
                    if (count > counts[axis] && count > 1) {
                        counts[axis] = count;
                        best[axis] = { Vec2{ F(run.front().x), F(run.front().y) },
                                       Vec2{ F(run.back().x), F(run.back().y) } };
                    }
                }
            }
            std::array<F, 2> lengths2{};
            for (int axis = 0; axis < 2; ++axis) {
                auto& line = best[axis];
                if (line[0].x > line[1].x) {
                    std::swap(line[0], line[1]);
                }
                const F dx = F(line[1].x - line[0].x);
                const F dy = F(line[1].y - line[0].y);
                lengths2[axis] = F(F(dx * dx) + F(dy * dy));
            }
            const int axis = lengths2[1] >= F(F(1.2) * lengths2[0]) ? 1 : 0;
            out->line = best[axis];
            out->box = { x0, y0, x1, y1 };
            out->area = n;
            out->length = F(std::sqrt(lengths2[axis]));
            return std::isfinite(out->line[0].x) && std::isfinite(out->line[0].y) && std::isfinite(out->line[1].x) &&
                   std::isfinite(out->line[1].y) && out->length != 0.0f;
        }

        // ------------------------------------------------ size, spacing and FillVertexBuffer (rendering/font.py)

        struct Glyph
        {
            std::int32_t index = 0;
            std::uint8_t code = 0;
            Vec2 q{};
            F width = 0.0f;
            F height = 0.0f;
            Vec2 position{};
            Vec2 tangent{};
        };

        struct Layout
        {
            std::string text;
            F scale = 0.0f;
            std::vector<Glyph> glyphs;
            Vec2 direction{};
        };

        char upper(unsigned char c)
        {
            // cp1252 byte-wise StrToUpper: ASCII, Latin-1 letters (not the division sign), š œ ž ÿ.
            if (c >= 'a' && c <= 'z') {
                return static_cast<char>(c - 32);
            }
            if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
                return static_cast<char>(c - 32);
            }
            switch (c) {
            case 0x9A: return static_cast<char>(0x8A);
            case 0x9C: return static_cast<char>(0x8C);
            case 0x9E: return static_cast<char>(0x8E);
            case 0xFF: return static_cast<char>(0x9F);
            default: return static_cast<char>(c);
            }
        }

        bool layout(const LabelFont& font, const std::string& name, const Baseline& base, Layout* out)
        {
            std::string original = name;
            std::transform(original.begin(), original.end(), original.begin(),
                           [](char c) { return upper(static_cast<unsigned char>(c)); });
            const std::int32_t width = font.measure(original);
            if (width == 0) {
                return false;
            }
            const F length = base.length;
            const F scale_length = F(F(F(0.7) * length) / F(width));
            const F scale_area = F(F(F(F(base.area) / length) * F(0.5)) / F(font.line_height));
            const F scale = std::min(scale_length, scale_area);
            std::string text = original;
            if (scale_area < scale_length) {
                // Letter spacing: the last string with 1..19 spaces between all bytes still shorter than the threshold.
                const F threshold = F(F(F(0.7) * length) / scale_area);
                for (int gap = 1; gap < 20; ++gap) {
                    std::string candidate;
                    for (std::size_t i = 0; i < original.size(); ++i) {
                        if (i > 0) {
                            candidate.append(static_cast<std::size_t>(gap), ' ');
                        }
                        candidate.push_back(original[i]);
                    }
                    if (F(font.measure(candidate)) >= threshold) {
                        break;
                    }
                    text = std::move(candidate);
                }
            }
            if (!std::isfinite(scale) || scale <= 0.0f) {
                return false;
            }
            Vec2 direction{ F(base.line[1].x - base.line[0].x), F(base.line[1].y - base.line[0].y) };
            const F inv = F(F(1.0f) / F(std::sqrt(F(F(direction.x * direction.x) + F(direction.y * direction.y)))));
            direction = { F(direction.x * inv), F(direction.y * inv) };
            const Vec2 normal{ -direction.y, direction.x };
            const F lift = F(F(F(font.line_height) * F(0.5)) * scale);
            const Vec2 origin{ F(F(F(base.line[0].x + base.line[1].x) * F(0.5)) - F(normal.x * lift)),
                               F(F(F(base.line[0].y + base.line[1].y) * F(0.5)) - F(normal.y * lift)) };
            std::int32_t cursor = -(font.measure(text) / 2);    // integer half width (FillVertexBuffer)
            const F interface_scale = F(F(0.05) * F(scale * F(20.0)));
            out->glyphs.clear();
            for (std::size_t j = 0; j < text.size(); ++j) {
                const auto c = static_cast<std::uint8_t>(text[j]);
                const LabelFont::Char& metric = font.chars[c];
                if (!metric.present) {
                    continue;
                }
                if (metric.height > 0) {
                    const F left = F(F(cursor + metric.xoffset) * interface_scale);
                    const F right = F(F(cursor + metric.xoffset + metric.width) * interface_scale);
                    const F bottom = F(F(font.line_height - metric.yoffset - metric.height) * interface_scale);
                    const F top = F(F(font.line_height - metric.yoffset) * interface_scale);
                    const Vec2 v0{ F(F(direction.x * left) + F(normal.x * bottom)),
                                   F(F(direction.y * left) + F(normal.y * bottom)) };
                    const Vec2 v3{ F(F(direction.x * right) + F(normal.x * top)),
                                   F(F(direction.y * right) + F(normal.y * top)) };
                    Glyph glyph;
                    glyph.index = static_cast<std::int32_t>(j);
                    glyph.code = c;
                    glyph.q = { F(F(F(v0.x + v3.x) * F(0.5)) + origin.x), F(F(F(v0.y + v3.y) * F(0.5)) + origin.y) };
                    glyph.width = F(right - left);
                    glyph.height = F(top - bottom);
                    out->glyphs.push_back(glyph);
                }
                cursor += metric.xadvance;
                if (metric.height > 0 && j + 1 < text.size()) {
                    cursor += font.kerning(c, static_cast<std::uint8_t>(text[j + 1]));
                }
            }
            out->text = std::move(text);
            out->scale = scale;
            out->direction = direction;
            return true;
        }

        // ---------------------------------------------------------------------------- CurveText (0x140FD2060)

        std::array<Vec2, 2> section(Vec2 q, const std::array<Vec2, 2>& line, const std::array<F, 4>& box)
        {
            F dx = F(line[1].x - line[0].x);
            const F dy = F(line[1].y - line[0].y);
            F x0 = box[0];
            const F y0 = box[1];
            F x1 = box[2];
            const F y1 = box[3];
            if (std::abs(dy) <= F(0.01)) {
                return { Vec2{ q.x, y0 }, Vec2{ q.x, y1 } };
            }
            if (std::abs(dx) <= F(0.01)) {
                const F adjusted = F(line[0].x + (dx <= 0.0f ? F(-0.01) : F(0.01)));
                dx = F(adjusted - line[0].x);
            }
            const F slope = F(-dx / dy);
            const F b = F(q.y - F(q.x * slope));
            F a = F(F(x0 * slope) + b);
            F z = F(F(x1 * slope) + b);
            const F clamped_a = std::clamp(a, y0, y1);
            const F clamped_z = std::clamp(z, y0, y1);
            if (a != clamped_a) {
                x0 = F(F(clamped_a - b) / slope);
                a = clamped_a;
            }
            if (z != clamped_z) {
                x1 = F(F(clamped_z - b) / slope);
                z = clamped_z;
            }
            return { Vec2{ x0, a }, Vec2{ x1, z } };
        }

        Vec2 catmull(const std::array<Vec2d, 4>& controls, F t)
        {
            // Controls are built in double, narrowed to float32 for CatmullRom<Eigen::Matrix<float,2,1>>.
            const F t2 = F(t * t);
            const F t3 = F(t2 * t);
            const F c0 = F(F(F(t2 + t2) - t3) - t);
            const F c1 = F(F(F(t3 * F(3)) - F(t2 * F(5))) + F(2));
            const F c2 = F(F(F(t2 * F(4)) - F(t3 * F(3))) + t);
            const F c3 = F(t3 - t2);
            Vec2 r{};
            for (int axis = 0; axis < 2; ++axis) {
                const F p0 = F(axis ? controls[0].y : controls[0].x);
                const F p1 = F(axis ? controls[1].y : controls[1].x);
                const F p2 = F(axis ? controls[2].y : controls[2].x);
                const F p3 = F(axis ? controls[3].y : controls[3].x);
                const F v = F(F(0.5) * F(F(F(F(p0 * c0) + F(p1 * c1)) + F(p2 * c2)) + F(p3 * c3)));
                (axis ? r.y : r.x) = v;
            }
            return r;
        }

        bool curve_text(Layout* layout, const Baseline& base, const Group& g, const LabelFont& font)
        {
            auto& glyphs = layout->glyphs;
            if (glyphs.empty()) {
                return false;
            }
            std::vector<Vec2> mids;
            mids.reserve(glyphs.size());
            for (const Glyph& glyph : glyphs) {
                const auto line = section(glyph.q, base.line, base.box);
                const auto run = validated(line, g, glyph.q);
                mids.push_back(run.empty() ? glyph.q
                                           : Vec2{ F(F(F(run.front().x) + F(run.back().x)) * F(0.5)),
                                                   F(F(F(run.front().y) + F(run.back().y)) * F(0.5)) });
            }
            auto half = [](Vec2 q, Vec2 m) {
                return Vec2d{ double(F(F(q.x + m.x) * F(0.5))), double(F(F(q.y + m.y) * F(0.5))) };
            };
            Vec2d a = half(glyphs.front().q, mids.front());
            Vec2d b = glyphs.size() > 1 ? half(glyphs.back().q, mids.back()) : Vec2d{ 0.0, 0.0 };   // if / else if
            Vec2d c{ 0.0, 0.0 };
            for (const Vec2& m : mids) {
                c = c + Vec2d{ double(m.x), double(m.y) };
            }
            c = c * (1.0 / double(mids.size()));
            Vec2d v = a - c;
            Vec2d u = normalize(c - b);
            Vec2d w = normalize(v);
            Vec2d e0 = a + w * dot(v, u) + ccw(w) * dot(v, ccw(u));
            v = b - c;
            u = normalize(c - a);
            w = normalize(v);
            const Vec2d cw_w{ w.y, -w.x };
            const Vec2d cw_u{ u.y, -u.x };
            Vec2d e1 = b + w * dot(v, u) + cw_w * dot(v, cw_u);
            if (b.x < a.x) {
                std::swap(a, b);
                std::swap(e0, e1);
            }
            const std::array<Vec2d, 5> controls{ e0, a, c, b, e1 };
            for (const Vec2d& p : controls) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
                    return false;   // no invented fallback
                }
            }
            const auto m = static_cast<std::int32_t>(layout->text.size());
            for (Glyph& glyph : glyphs) {
                const F uu = m > 1 ? F(F(glyph.index) / F(m - 1)) : F(1);
                const bool first = uu <= F(0.5);
                const std::array<Vec2d, 4> pts = first ? std::array<Vec2d, 4>{ controls[0], controls[1], controls[2], controls[3] }
                                                       : std::array<Vec2d, 4>{ controls[1], controls[2], controls[3], controls[4] };
                const F t = first ? F(uu + uu) : F(F(uu - F(0.5)) * F(2));
                const Vec2 position = catmull(pts, t);
                Vec2 tangent{};
                if (t <= F(0.5)) {
                    const Vec2 next = catmull(pts, F(t + F(0.01)));
                    tangent = { F(next.x - position.x), F(next.y - position.y) };
                } else {
                    const Vec2 previous = catmull(pts, F(t - F(0.01)));
                    tangent = { F(position.x - previous.x), F(position.y - previous.y) };
                }
                if (m <= 2) {
                    tangent = layout->direction;
                }
                const F length = F(std::sqrt(F(F(tangent.x * tangent.x) + F(tangent.y * tangent.y))));
                tangent = { F(tangent.x / length), F(tangent.y / length) };
                if (!std::isfinite(tangent.x) || !std::isfinite(tangent.y)) {
                    return false;
                }
                glyph.position = position;
                glyph.tangent = tangent;
                if (glyph.code == '.' || glyph.code == '\'') {
                    const LabelFont::Char& metric = font.chars[glyph.code];
                    const F offset = F(F(layout->scale) * F(F(F(F(font.line_height) * F(0.5)) - F(metric.yoffset)) -
                                                            F(F(metric.height) * F(0.5))));
                    glyph.position = { F(position.x + F(-tangent.y * offset)), F(position.y + F(tangent.x * offset)) };
                }
            }
            return true;
        }

        // ------------------------------------------------------------------------------------- blobs

        // FloodFillBlob (0x1410AE870): FIFO BFS in stored neighbour order; a neighbour is visited before its key is
        // read and joins only with the seed's key.
        std::vector<std::uint16_t> flood(const LabelRaster& raster, std::span<const std::int32_t> key,
                                         std::uint16_t seed)
        {
            std::vector<std::uint16_t> blob;
            if (key[seed] == -1) {
                return blob;
            }
            std::vector<std::uint8_t> visited(raster.count(), 0);
            visited[seed] = 1;
            blob.push_back(seed);
            for (std::size_t head = 0; head < blob.size(); ++head) {
                for (const std::uint16_t q : raster.neighbours[blob[head]]) {
                    if (visited[q]) {
                        continue;
                    }
                    visited[q] = 1;
                    if (key[q] != -1 && key[q] == key[seed]) {
                        blob.push_back(q);
                    }
                }
            }
            return blob;
        }

        void emit(const LabelRaster& raster, const std::vector<std::uint16_t>& blob, std::int32_t area,
                  const std::string& name, std::vector<std::uint8_t>* emitted, std::vector<NameRecord>* records)
        {
            // The 45-byte strncpy of SProvinceData; terminated safely here.
            const std::string text = to_cp1252(name).substr(0, 45);
            for (const std::uint16_t p : blob) {
                (*emitted)[p] = 1;
                records->push_back({ p, area, text, raster.land[p] != 0, raster.land[p] == 0 });
            }
        }

        std::string replace_all(std::string s, const std::string& from, const std::string& to)
        {
            for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
                s.replace(at, from.size(), to);
            }
            return s;
        }
    }

    // ------------------------------------------------------------------------------------------ LabelRaster

    void LabelRaster::build(const MapData& map)
    {
        width = static_cast<std::int32_t>(map.width);
        height = static_cast<std::int32_t>(map.height);
        const std::size_t n = map.count;
        pixels.resize(std::size_t(width) * height);
        for (std::int32_t y = 0; y < height; ++y) {        // EU4 reads the BMP bottom row first
            std::memcpy(&pixels[std::size_t(y) * width], &map.pixel_province[std::size_t(height - 1 - y) * width],
                        std::size_t(width) * sizeof(std::uint16_t));
        }
        moments.assign(n, {});
        bbox.assign(n, { width, height, 0, 0 });
        neighbours.assign(n, {});
        std::vector<std::uint8_t> seen(n * n, 0);
        for (std::int32_t y = 0; y < height; ++y) {
            for (std::int32_t x = 0; x < width; ++x) {
                const std::uint16_t a = pixels[std::size_t(y) * width + x];
                if (a > 0 && a < n) {
                    auto& m = moments[a];
                    m[0] += 1;
                    m[1] += static_cast<std::uint64_t>(x);
                    m[2] += static_cast<std::uint64_t>(y);
                    m[3] += static_cast<std::uint64_t>(static_cast<std::int64_t>(x * y));   // int32 products
                    m[4] += static_cast<std::uint64_t>(static_cast<std::int64_t>(x * x));
                    m[5] += static_cast<std::uint64_t>(static_cast<std::int64_t>(y * y));
                    auto& b = bbox[a];
                    b = { std::min(b[0], x), std::min(b[1], y), std::max(b[2], x + 1), std::max(b[3], y + 1) };
                }
                // CalculateAdjacencies: the pixel above first, then the one to the right; reverse edge first. Our map
                // does not wrap round like EU4's globe, so there is no x = W-1 -> 0 seam.
                for (int axis = 0; axis < 2; ++axis) {
                    if (axis == 1 && x + 1 >= width) {
                        continue;
                    }
                    const std::uint16_t b = axis == 0 ? pixels[std::size_t(std::min(y + 1, height - 1)) * width + x]
                                                      : pixels[std::size_t(y) * width + x + 1];
                    if (a == b || a >= n || b >= n) {
                        continue;
                    }
                    if (a > 0 && !seen[std::size_t(b) * n + a]) {
                        neighbours[b].push_back(a);
                        seen[std::size_t(b) * n + a] = 1;
                    }
                    if (b > 0 && !seen[std::size_t(a) * n + b]) {
                        neighbours[a].push_back(b);
                        seen[std::size_t(a) * n + b] = 1;
                    }
                }
            }
        }
        land.assign(n, 0);
        lake.assign(n, 0);
        used.clear();
        for (std::size_t p = 0; p < n; ++p) {
            if (moments[p][0] == 0) {
                bbox[p] = { 0, 0, 0, 0 };
            }
            land[p] = p > 0 && map.kind[p] == ProvinceKind::land;
            lake[p] = map.kind[p] == ProvinceKind::lake;
            if (p > 0 && moments[p][0] > 0) {
                used.push_back(static_cast<std::uint16_t>(p));
            }
        }
    }

    // --------------------------------------------------------------------------------------------- LabelFont

    void LabelFont::build(const MapData& map)
    {
        line_height = map.font_line_height;
        padding = map.font_padding;
        chars = {};
        for (const MapData::FontChar& c : map.font_chars) {
            if (c.code < 0 || c.code > 255) {
                continue;
            }
            chars[c.code] = { true, c.xoffset, c.yoffset, c.width, c.height, c.xadvance, c.atlas_x, c.atlas_y,
                              c.cell_width, c.cell_height, c.quad_x, c.quad_y };
        }
        kerning_.assign(256 * 256, 0);
        for (const MapData::FontKerning& k : map.font_kerning) {
            if (k.first >= 0 && k.first < 256 && k.second >= 0 && k.second < 256) {
                kerning_[std::size_t(k.first) * 256 + k.second] = k.amount;
            }
        }
    }

    std::int32_t LabelFont::kerning(std::uint8_t a, std::uint8_t b) const
    {
        return kerning_.empty() ? 0 : kerning_[std::size_t(a) * 256 + b];
    }

    std::int32_t LabelFont::measure(const std::string& text) const
    {
        // Sum of advances and the kerning of every following pair (GetActualRequiredSize, one line).
        std::int32_t width = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<std::uint8_t>(text[i]);
            width += chars[c].present ? chars[c].xadvance : 0;
            if (i + 1 < text.size()) {
                width += kerning(c, static_cast<std::uint8_t>(text[i + 1]));
            }
        }
        return width;
    }

    // ------------------------------------------------------------------------------------ SetMapNameMode

    std::vector<NameRecord> realm_records(const LabelRaster& raster, const RealmNames& realms)
    {
        const std::size_t n = raster.count();
        auto valid = [&](std::int32_t tag) { return tag >= 0 && std::size_t(tag) < realms.name.size(); };
        auto owner = [&](std::size_t p) { return p < realms.owner.size() && valid(realms.owner[p]) ? realms.owner[p] : -1; };
        auto tribal = [&](std::size_t p) {
            return p < realms.tribal_owner.size() && valid(realms.tribal_owner[p]) ? realms.tribal_owner[p] : -1;
        };
        auto impassable = [&](std::size_t p) { return p < realms.impassable.size() && realms.impassable[p]; };

        // COwnerArea: connected components of the ordinary owner.
        std::vector<std::int32_t> component(n, -1);
        std::vector<std::int32_t> sizes;
        for (std::size_t seed = 1; seed < n; ++seed) {
            if (owner(seed) == -1 || component[seed] != -1) {
                continue;
            }
            const auto area = static_cast<std::int32_t>(sizes.size());
            std::deque<std::size_t> queue{ seed };
            component[seed] = area;
            std::int32_t size = 0;
            while (!queue.empty()) {
                const std::size_t p = queue.front();
                queue.pop_front();
                ++size;
                for (const std::uint16_t q : raster.neighbours[p]) {
                    if (component[q] == -1 && owner(q) == owner(seed)) {
                        component[q] = area;
                        queue.push_back(q);
                    }
                }
            }
            sizes.push_back(size);
        }

        // GetRealmIdentifier (0x1410A7680).
        std::vector<std::int32_t> realm(n, -1);
        for (std::size_t p = 0; p < n; ++p) {
            const std::int32_t tag = owner(p) != -1 ? owner(p) : tribal(p);
            if (tag != -1) {
                realm[p] = tag;
                continue;
            }
            const bool water = !raster.land[p];
            if (!water && !(impassable(p) && realms.color_wastelands)) {
                continue;
            }
            std::vector<std::pair<std::int32_t, std::int32_t>> votes;   // first met first
            std::vector<std::int32_t> qualifying;
            std::int32_t denominator = 0;
            for (const std::uint16_t q : raster.neighbours[p]) {
                if (!raster.land[q]) {
                    continue;
                }
                ++denominator;
                if (impassable(q) || owner(q) == -1) {
                    continue;
                }
                auto it = std::find_if(votes.begin(), votes.end(), [&](const auto& v) { return v.first == owner(q); });
                if (it == votes.end()) {
                    votes.push_back({ owner(q), 1 });
                } else {
                    ++it->second;
                }
                const std::int32_t a = component[q];
                if (a != -1 && sizes[a] >= realms.min_bridged_piece &&
                    std::find(qualifying.begin(), qualifying.end(), a) == qualifying.end()) {
                    qualifying.push_back(a);
                }
            }
            if (water && qualifying.size() < 2) {
                continue;
            }
            for (const auto& [tag_q, count] : votes) {
                if ((!water && count * 2 > denominator) || count == denominator) {
                    realm[p] = tag_q;
                    break;
                }
            }
        }

        std::vector<std::uint8_t> emitted(n, 0);
        std::vector<NameRecord> records;
        std::int32_t next_area = 1;
        auto at = [](const std::vector<std::int32_t>& v, std::size_t p) { return p < v.size() ? v[p] : -1; };
        for (const bool capital_phase : { true, false }) {
            for (const std::uint16_t seed : raster.used) {
                const std::int32_t tag = owner(seed);
                if (emitted[seed] || tag == -1) {
                    continue;
                }
                const std::uint16_t capital = std::size_t(tag) < realms.capital.size() ? realms.capital[tag] : 0;
                if (capital_phase && seed != capital) {
                    continue;
                }
                const auto blob = flood(raster, realm, seed);
                std::string name = realms.name[tag];
                if (!capital_phase) {
                    // The remote part's dominant geographical region, by province count, first maximum in BFS order.
                    std::vector<std::pair<std::int32_t, std::int32_t>> counts;
                    for (const std::uint16_t p : blob) {
                        const std::int32_t r = at(realms.region, p);
                        if (r == -1) {
                            continue;
                        }
                        auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& v) { return v.first == r; });
                        if (it == counts.end()) {
                            counts.push_back({ r, 1 });
                        } else {
                            ++it->second;
                        }
                    }
                    std::int32_t dominant = -1;
                    std::int32_t dominant_count = 0;
                    for (const auto& [r, count] : counts) {
                        if (count > dominant_count) {
                            dominant = r;
                            dominant_count = count;
                        }
                    }
                    const std::int32_t capital_region = capital > 0 && capital < n ? at(realms.region, capital) : -1;
                    const bool colonial = std::size_t(tag) < realms.colonial.size() && realms.colonial[tag];
                    if (realms.use_region_names && capital_region != -1 && !colonial && dominant != -1 &&
                        dominant != capital_region) {
                        if (static_cast<std::int32_t>(blob.size()) < realms.min_provinces_for_name) {
                            continue;   // not emitted: its next province seeds the same blob again
                        }
                        const auto first = *std::find_if(blob.begin(), blob.end(),
                                                         [&](std::uint16_t p) { return at(realms.region, p) == dominant; });
                        std::string geographic = std::size_t(dominant) < realms.region_names.size()
                                                     ? realms.region_names[dominant] : std::string();
                        if (static_cast<std::int32_t>(blob.size()) <= realms.min_provinces_for_region) {
                            const std::int32_t area = at(realms.area, first);
                            if (area != -1 && std::size_t(area) < realms.area_names.size()) {
                                geographic = realms.area_names[area];
                            }
                        } else if (dominant_count < static_cast<std::int32_t>(blob.size()) / 2) {
                            const std::int32_t super = at(realms.superregion, first);
                            if (super != -1 && std::size_t(super) < realms.superregion_names.size()) {
                                geographic = realms.superregion_names[super];
                            }
                        }
                        const std::string adjective = std::size_t(tag) < realms.adjective.size() ? realms.adjective[tag]
                                                                                                  : realms.name[tag];
                        name = replace_all(replace_all(realms.enclave_format, "$ADJECTIVE$", adjective), "$AREANAME$",
                                           geographic);
                    }
                }
                emit(raster, blob, next_area++, name, &emitted, &records);
            }
        }
        return records;
    }

    // ---------------------------------------------------------------------- CGenerateNamesWork::Update

    std::vector<MapLabel> generate_names(const LabelRaster& raster, const LabelFont& font,
                                         std::span<const NameRecord> records)
    {
        std::vector<std::int32_t> table(raster.count(), -1);
        for (const NameRecord& r : records) {
            table[r.province] = r.area;   // the last record of a province wins
        }
        std::vector<MapLabel> labels;
        for (const NameRecord& r : records) {
            const std::uint16_t p = r.province;
            if (p == 0 || raster.moments[p][0] == 0 || table[p] == -1 || r.name.empty()) {
                continue;
            }
            // The first eligible record decides the text and the water flags of the whole attempt.
            const Group group{ raster, table, r.area, r.water_between, r.water_ends };
            Baseline base;
            Layout text;
            if (baseline(group, &base) && layout(font, r.name, base, &text) && curve_text(&text, base, group, font)) {
                MapLabel label;
                label.text = text.text;
                label.scale = text.scale;
                const auto h = static_cast<F>(raster.height);
                for (const Glyph& g : text.glyphs) {
                    // EU4 raster coordinates (pixel index, y up) -> our map px (pixel centre at +0.5, y down).
                    label.glyphs.push_back({ g.position.x + 0.5f, h - 0.5f - g.position.y, g.tangent.x, -g.tangent.y,
                                             g.width, g.height, g.code });
                }
                labels.push_back(std::move(label));
            }
            for (auto& t : table) {
                if (t == r.area) {
                    t = -1;   // consumed, also after a failed attempt
                }
            }
        }
        return labels;
    }

    std::string to_cp1252(const std::string& utf8)
    {
        // UTF-8 decode; code points 0-255 map to themselves except the cp1252 extras below; others become '?'.
        static const std::pair<std::uint32_t, char> extras[] = {
            { 0x20AC, '\x80' }, { 0x201A, '\x82' }, { 0x0192, '\x83' }, { 0x201E, '\x84' }, { 0x2026, '\x85' },
            { 0x2020, '\x86' }, { 0x2021, '\x87' }, { 0x02C6, '\x88' }, { 0x2030, '\x89' }, { 0x0160, '\x8A' },
            { 0x2039, '\x8B' }, { 0x0152, '\x8C' }, { 0x017D, '\x8E' }, { 0x2018, '\x91' }, { 0x2019, '\x92' },
            { 0x201C, '\x93' }, { 0x201D, '\x94' }, { 0x2022, '\x95' }, { 0x2013, '\x96' }, { 0x2014, '\x97' },
            { 0x02DC, '\x98' }, { 0x2122, '\x99' }, { 0x0161, '\x9A' }, { 0x203A, '\x9B' }, { 0x0153, '\x9C' },
            { 0x017E, '\x9E' }, { 0x0178, '\x9F' },
        };
        std::string out;
        for (std::size_t i = 0; i < utf8.size();) {
            const auto c = static_cast<unsigned char>(utf8[i]);
            std::uint32_t code = c;
            std::size_t length = 1;
            if (c >= 0xF0) {
                length = 4;
                code = c & 0x07;
            } else if (c >= 0xE0) {
                length = 3;
                code = c & 0x0F;
            } else if (c >= 0xC0) {
                length = 2;
                code = c & 0x1F;
            }
            for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
                code = (code << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
            }
            i += length;
            if (code < 0x80 || (code >= 0xA0 && code <= 0xFF)) {
                out.push_back(static_cast<char>(code));
                continue;
            }
            char mapped = '?';
            for (const auto& [u, b] : extras) {
                if (u == code) {
                    mapped = b;
                }
            }
            out.push_back(mapped);
        }
        return out;
    }
}
