// Copyright (c) 2017 - 2017 Settlers Freaks (sf-team at siedler25.org)
//
// This file is part of Return To The Roots.
//
// Return To The Roots is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Return To The Roots is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Return To The Roots. If not, see <http://www.gnu.org/licenses/>.

#include "mapGenerator/RandomUtility.h"

#include <ctime>
#include <numeric>
#include <random>

namespace rttr { namespace mapGenerator {

    RandomUtility::RandomUtility()
    {
        auto seed = static_cast<uint64_t>(time(nullptr));
        rng_.seed(static_cast<UsedRNG::result_type>(seed));
    }

    RandomUtility::RandomUtility(uint64_t seed) { rng_.seed(static_cast<UsedRNG::result_type>(seed)); }

    bool RandomUtility::ByChance(unsigned percentage) { return static_cast<unsigned>(RandomInt(1, 100)) <= percentage; }

    unsigned RandomUtility::Index(const size_t& size) { return RandomInt(0, static_cast<unsigned>(size - 1)); }

    int RandomUtility::RandomInt(int min, int max)
    {
        std::uniform_int_distribution<int> distr(min, max);
        return distr(rng_);
    }

    MapPoint RandomUtility::RandomPoint(const MapExtent& size) { return MapPoint(RandomInt(0, size.x - 1), RandomInt(0, size.y - 1)); }

    double RandomUtility::RandomDouble(double min, double max)
    {
        std::uniform_real_distribution<double> distr(min, max);
        return distr(rng_);
    }

}} // namespace rttr::mapGenerator
