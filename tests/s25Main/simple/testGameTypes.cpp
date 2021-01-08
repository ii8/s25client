// Copyright (c) 2017 - 2020 Settlers Freaks (sf-team at siedler25.org)
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

#include "helpers/EnumRange.h"
#include "gameTypes/GameTypesOutput.h"
#include "gameTypes/Resource.h"
#include "gameData/JobConsts.h"
#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(GameTypes)

BOOST_AUTO_TEST_CASE(ResourceValues)
{
    {
        Resource res;
        BOOST_TEST(res.getType() == ResourceType::Nothing);
        BOOST_TEST(res.getAmount() == 0u);
    }
    {
        Resource res(ResourceType::Nothing, 5);
        BOOST_TEST(res.getType() == ResourceType::Nothing);
        BOOST_TEST(res.getAmount() == 0u);
    }

    // Basic value
    Resource res(ResourceType::Gold, 10);
    BOOST_TEST(res.getType() == ResourceType::Gold);
    BOOST_TEST(res.getAmount() == 10u);
    // Change type
    res.setType(ResourceType::Iron);
    BOOST_TEST(res.getType() == ResourceType::Iron);
    BOOST_TEST(res.getAmount() == 10u);
    // Amount
    res.setAmount(5);
    BOOST_TEST(res.getType() == ResourceType::Iron);
    BOOST_TEST(res.getAmount() == 5u);
    // Copy value
    Resource res2(res.getValue());
    BOOST_TEST(res.getType() == ResourceType::Iron);
    BOOST_TEST(res.getAmount() == 5u);
    // Set 0
    res2.setAmount(0);
    BOOST_TEST(res2.getType() == ResourceType::Iron);
    BOOST_TEST(res2.getAmount() == 0u);
    // Has
    BOOST_TEST(res.has(ResourceType::Iron));
    BOOST_TEST(!res.has(ResourceType::Gold));
    BOOST_TEST(!res2.has(ResourceType::Iron));
    BOOST_TEST(!res.has(ResourceType::Nothing));
    BOOST_TEST(!res2.has(ResourceType::Nothing));
    // Nothing -> 0
    BOOST_TEST_REQUIRE(res.getAmount() != 0u);
    res.setType(ResourceType::Nothing);
    BOOST_TEST(res.getType() == ResourceType::Nothing);
    BOOST_TEST(res.getAmount() == 0u);
    // And stays 0
    res.setAmount(10);
    BOOST_TEST(res.getType() == ResourceType::Nothing);
    BOOST_TEST(res.getAmount() == 0u);
    BOOST_TEST(!res.has(ResourceType::Iron));
    BOOST_TEST(!res.has(ResourceType::Nothing));
    // Overflow check
    res2.setAmount(15);
    BOOST_TEST(res2.getType() == ResourceType::Iron);
    BOOST_TEST(res2.getAmount() == 15u);
    res2.setAmount(17);
    BOOST_TEST(res2.getType() == ResourceType::Iron);
    // Unspecified
    BOOST_TEST(res2.getAmount() < 17u);
}

BOOST_AUTO_TEST_CASE(ResourceConvertToFromUInt8)
{
    for(const auto type : helpers::enumRange<ResourceType>())
    {
        for(auto amount : {1u, 5u, 15u})
        {
            const Resource res(type, amount);
            const Resource res2(static_cast<uint8_t>(res));
            BOOST_TEST(res2.getType() == type);
            if(type == ResourceType::Nothing)
                amount = 0u;
            BOOST_TEST(res2.getAmount() == amount);
        }
    }
    // Out of bounds -> Validated
    const Resource res(0xFF);
    BOOST_TEST(res.getType() == ResourceType::Nothing);
    BOOST_TEST(res.getAmount() == 0u);
}

BOOST_AUTO_TEST_CASE(NationSpecificJobBobs)
{
    // Helper is not nation specific
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Helper].getBobId(Nation::Vikings)
               == JOB_SPRITE_CONSTS[Job::Helper].getBobId(Nation::Africans));
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Helper].getBobId(Nation::Vikings)
               == JOB_SPRITE_CONSTS[Job::Helper].getBobId(Nation::Babylonians));
    // Soldiers are
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Private].getBobId(Nation::Vikings)
               != JOB_SPRITE_CONSTS[Job::Private].getBobId(Nation::Africans));
    // Non native nations come after native ones
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Private].getBobId(Nation::Vikings)
               < JOB_SPRITE_CONSTS[Job::Private].getBobId(Nation::Babylonians));
    // Same for scouts
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Scout].getBobId(Nation::Vikings)
               != JOB_SPRITE_CONSTS[Job::Scout].getBobId(Nation::Africans));
    BOOST_TEST(JOB_SPRITE_CONSTS[Job::Scout].getBobId(Nation::Vikings)
               < JOB_SPRITE_CONSTS[Job::Scout].getBobId(Nation::Babylonians));
}

BOOST_AUTO_TEST_SUITE_END()
