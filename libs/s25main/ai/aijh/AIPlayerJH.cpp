// Copyright (C) 2005 - 2021 Settlers Freaks (sf-team at siedler25.org)
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AIPlayerJH.h"
#include "AIConstruction.h"
#include "BuildingPlanner.h"
#include "FindWhConditions.h"
#include "GamePlayer.h"
#include "Jobs.h"
#include "RttrForeachPt.h"
#include "addons/const_addons.h"
#include "ai/AIEvents.h"
#include "boost/filesystem/fstream.hpp"
#include "buildings/noBuildingSite.h"
#include "buildings/nobHarborBuilding.h"
#include "buildings/nobMilitary.h"
#include "buildings/nobUsual.h"
#include "helpers/MaxEnumValue.h"
#include "helpers/containerUtils.h"
#include "network/GameMessages.h"
#include "notifications/BuildingNote.h"
#include "notifications/ExpeditionNote.h"
#include "notifications/NodeNote.h"
#include "notifications/ResourceNote.h"
#include "notifications/RoadNote.h"
#include "notifications/ShipNote.h"
#include "pathfinding/PathConditionRoad.h"
#include "nodeObjs/noAnimal.h"
#include "nodeObjs/noFlag.h"
#include "nodeObjs/noShip.h"
#include "nodeObjs/noTree.h"
#include "gameData/BuildingConsts.h"
#include "gameData/BuildingProperties.h"
#include "gameData/GameConsts.h"
#include "gameData/JobConsts.h"
#include "gameData/TerrainDesc.h"
#include "gameData/ToolConsts.h"
#include <algorithm>
#include <array>
#include <memory>
#include <random>
#include <stdexcept>
#include <type_traits>

namespace {
void HandleBuildingNote(AIEventManager& eventMgr, const BuildingNote& note)
{
    std::unique_ptr<AIEvent::Base> ev;
    using namespace AIEvent;
    switch(note.type)
    {
        case BuildingNote::Constructed:
            ev = std::make_unique<AIEvent::Building>(EventType::BuildingFinished, note.pos, note.bld);
            break;
        case BuildingNote::Destroyed:
            ev = std::make_unique<AIEvent::Building>(EventType::BuildingDestroyed, note.pos, note.bld);
            break;
        case BuildingNote::Captured:
            ev = std::make_unique<AIEvent::Building>(EventType::BuildingConquered, note.pos, note.bld);
            break;
        case BuildingNote::Lost:
            ev = std::make_unique<AIEvent::Building>(EventType::BuildingLost, note.pos, note.bld);
            break;
        case BuildingNote::LostLand:
            ev = std::make_unique<AIEvent::Building>(EventType::LostLand, note.pos, note.bld);
            break;
        case BuildingNote::NoRessources:
            ev = std::make_unique<AIEvent::Building>(EventType::NoMoreResourcesReachable, note.pos, note.bld);
            break;
        case BuildingNote::LuaOrder:
            ev = std::make_unique<AIEvent::Building>(EventType::LuaConstructionOrder, note.pos, note.bld);
            break;
        default: RTTR_Assert(false); return;
    }
    eventMgr.AddAIEvent(std::move(ev));
}
void HandleExpeditionNote(AIEventManager& eventMgr, const ExpeditionNote& note)
{
    switch(note.type)
    {
        case ExpeditionNote::Waiting:
            eventMgr.AddAIEvent(std::make_unique<AIEvent::Location>(AIEvent::EventType::ExpeditionWaiting, note.pos));
            break;
        case ExpeditionNote::ColonyFounded:
            eventMgr.AddAIEvent(std::make_unique<AIEvent::Location>(AIEvent::EventType::NewColonyFounded, note.pos));
            break;
    }
}
void HandleResourceNote(AIEventManager& eventMgr, const ResourceNote& note)
{
    eventMgr.AddAIEvent(std::make_unique<AIEvent::Resource>(AIEvent::EventType::ResourceFound, note.pos, note.res));
}
void HandleRoadNote(AIEventManager& eventMgr, const RoadNote& note)
{
    switch(note.type)
    {
        case RoadNote::Constructed:
            eventMgr.AddAIEvent(std::make_unique<AIEvent::Direction>(AIEvent::EventType::RoadConstructionComplete,
                                                                     note.pos, note.route.front()));
            break;
        case RoadNote::ConstructionFailed:
            eventMgr.AddAIEvent(std::make_unique<AIEvent::Direction>(AIEvent::EventType::RoadConstructionFailed,
                                                                     note.pos, note.route.front()));
            break;
    }
}
void HandleShipNote(AIEventManager& eventMgr, const ShipNote& note)
{
    if(note.type == ShipNote::Constructed)
        eventMgr.AddAIEvent(std::make_unique<AIEvent::Location>(AIEvent::EventType::ShipBuilt, note.pos));
}
} // namespace

namespace AIJH {

Subscription recordBQsToUpdate(const GameWorldBase& gw, std::vector<MapPoint>& bqsToUpdate)
{
    auto addToBqsToUpdate = [&bqsToUpdate](const MapPoint pt, unsigned) {
        bqsToUpdate.push_back(pt);
        return false;
    };
    return gw.GetNotifications().subscribe<NodeNote>([&gw, addToBqsToUpdate](const NodeNote& note) {
        if(note.type == NodeNote::BQ)
        {
            // Need to check surrounding nodes for possible/impossible flags (e.g. near border)
            gw.CheckPointsInRadius(note.pos, 1, addToBqsToUpdate, true);
        } else if(note.type == NodeNote::Owner)
        {
            // Owner changes border, which changes where buildings can be placed next to it
            // And as flags are need for buildings we need range 2 (e.g. range 1 is flag, range 2 building)
            gw.CheckPointsInRadius(note.pos, 2, addToBqsToUpdate, true);
        }
    });
}

static bool isUnlimitedResource(const AIResource res, const GlobalGameSettings& ggs)
{
    switch(res)
    {
        case AIResource::Gold:
        case AIResource::Ironore:
        case AIResource::Coal: return ggs.isEnabled(AddonId::INEXHAUSTIBLE_MINES);
        case AIResource::Granite:
            return ggs.isEnabled(AddonId::INEXHAUSTIBLE_MINES) || ggs.isEnabled(AddonId::INEXHAUSTIBLE_GRANITEMINES);
        case AIResource::Fish: return ggs.isEnabled(AddonId::INEXHAUSTIBLE_FISH);
        default: return false;
    }
}

// Needed because AIResourceMap is not default initializable
template<size_t... I>
static auto createResourceMaps(const AIInterface& aii, const AIMap& aiMap, std::index_sequence<I...>)
{
    return helpers::EnumArray<AIResourceMap, AIResource>{
      AIResourceMap(AIResource(I), isUnlimitedResource(AIResource(I), aii.gwb.GetGGS()), aii, aiMap)...};
}
static auto createResourceMaps(const AIInterface& aii, const AIMap& aiMap)
{
    return createResourceMaps(aii, aiMap, std::make_index_sequence<helpers::NumEnumValues_v<AIResource>>{});
}

AIPlayerJH::AIPlayerJH(const unsigned char playerId, const GameWorldBase& gwb, const AI::Level level)
    : AIPlayer(playerId, gwb, level), UpgradeBldPos(MapPoint::Invalid()), resourceMaps(createResourceMaps(aii, aiMap)),
      isInitGfCompleted(false), defeated(player.IsDefeated()), bldPlanner(std::make_unique<BuildingPlanner>(*this)),
      construction(std::make_unique<AIConstruction>(*this))
{
    InitNodes();
    InitResourceMaps();
#ifdef DEBUG_AI
    SaveResourceMapsToFile();
#endif

    switch(level)
    {
        case AI::Level::Easy:
            attack_interval = 2500;
            build_interval = 1000;
            break;
        case AI::Level::Medium:
            attack_interval = 750;
            build_interval = 400;
            break;
        case AI::Level::Hard:
            attack_interval = 100;
            build_interval = 200;
            break;
        default: throw std::invalid_argument("Invalid AI level!");
    }
    // TODO: Maybe remove the AIEvents where possible and call the handler functions directly
    NotificationManager& notifications = gwb.GetNotifications();
    subBuilding = notifications.subscribe<BuildingNote>([this, playerId](const BuildingNote& note) {
        if(note.player == playerId)
            HandleBuildingNote(eventManager, note);
    });
    subExpedition = notifications.subscribe<ExpeditionNote>([this, playerId](const ExpeditionNote& note) {
        if(note.player == playerId)
            HandleExpeditionNote(eventManager, note);
    });
    subResource = notifications.subscribe<ResourceNote>([this, playerId](const ResourceNote& note) {
        if(note.player == playerId)
            HandleResourceNote(eventManager, note);
    });
    subRoad = notifications.subscribe<RoadNote>([this, playerId](const RoadNote& note) {
        if(note.player == playerId)
            HandleRoadNote(eventManager, note);
    });
    subShip = notifications.subscribe<ShipNote>([this, playerId](const ShipNote& note) {
        if(note.player == playerId)
            HandleShipNote(eventManager, note);
    });
    subBQ = recordBQsToUpdate(this->gwb, this->nodesWithOutdatedBQ);
}

AIPlayerJH::~AIPlayerJH() = default;

/// Wird jeden GF aufgerufen und die KI kann hier entsprechende Handlungen vollziehen
void AIPlayerJH::RunGF(const unsigned gf, bool gfisnwf)
{
    if(defeated)
        return;

    if(TestDefeat())
        return;
    if(!isInitGfCompleted)
    {
        InitStoreAndMilitarylists();
        InitDistribution();
    }
    if(isInitGfCompleted < 10)
    {
        isInitGfCompleted++;
        return; //  1 init -> 2 test defeat -> 3 do other ai stuff -> goto 2
    }
    if(gf == 100)
    {
        if(aii.GetMilitaryBuildings().empty() && aii.GetStorehouses().size() < 2)
            aii.Chat(_("Hi, I'm an artifical player and I'm not very good yet!"));
    }

    if(!nodesWithOutdatedBQ.empty())
    {
        helpers::makeUnique(nodesWithOutdatedBQ, MapPointLess());
        for(const MapPoint pt : nodesWithOutdatedBQ)
            aiMap[pt].bq = aii.GetBuildingQuality(pt);
        nodesWithOutdatedBQ.clear();
    }

    bldPlanner->Update(gf, *this);

    if(gfisnwf) // nwf -> now the orders have been executed -> new constructions can be started
        construction->ConstructionsExecuted();

    // LOG.write(("ai doing stuff %i \n",playerId);
    if(gf % 100 == 0)
        bldPlanner->UpdateBuildingsWanted(*this);
    ExecuteAIJob();

    if((gf + playerId * 17) % attack_interval == 0)
    {
        // CheckExistingMilitaryBuildings();
        TryToAttack();
    }
    if(((gf + playerId * 17) % 73 == 0) && (level != AI::Level::Easy))
    {
        MilUpgradeOptim();
    }

    if((gf + 41 + playerId * 17) % attack_interval == 0)
    {
        if(ggs.isEnabled(AddonId::SEA_ATTACK))
            TrySeaAttack();
    }

    if((gf + playerId * 13) % 1500 == 0)
    {
        CheckExpeditions();
        CheckForester();
        CheckGranitMine();
    }

    if((gf + playerId * 11) % 150 == 0)
    {
        AdjustSettings();
        // check for useless sawmills
        const std::list<nobUsual*>& sawMills = aii.GetBuildings(BuildingType::Sawmill);
        if(sawMills.size() > 3)
        {
            int burns = 0;
            for(const nobUsual* sawmill : sawMills)
            {
                if(sawmill->GetProductivity() < 1 && sawmill->HasWorker() && sawmill->GetNumWares(0) < 1
                   && (sawMills.size() - burns) > 3 && !sawmill->AreThereAnyOrderedWares())
                {
                    aii.DestroyBuilding(sawmill);
                    RemoveUnusedRoad(*sawmill->GetFlag(), Direction::NorthWest, true);
                    burns++;
                }
            }
        }
    }

    if((gf + playerId * 7) % build_interval == 0) // plan new buildings
    {
        CheckForUnconnectedBuildingSites();
        PlanNewBuildings(gf);
    }
}

void AIPlayerJH::OnChatMessage(unsigned /*sendPlayerId*/, ChatDestination, const std::string& /*msg*/) {}

void AIPlayerJH::PlanNewBuildings(const unsigned gf)
{
    bldPlanner->UpdateBuildingsWanted(*this);

    // pick a random storehouse and try to build one of these buildings around it (checks if we actually want more of
    // the building type)
    std::array<BuildingType, 24> bldToTest = {
      {BuildingType::HarborBuilding, BuildingType::Shipyard,   BuildingType::Sawmill,
       BuildingType::Forester,       BuildingType::Farm,       BuildingType::Fishery,
       BuildingType::Woodcutter,     BuildingType::Quarry,     BuildingType::GoldMine,
       BuildingType::IronMine,       BuildingType::CoalMine,   BuildingType::GraniteMine,
       BuildingType::Hunter,         BuildingType::Charburner, BuildingType::Ironsmelter,
       BuildingType::Mint,           BuildingType::Armory,     BuildingType::Metalworks,
       BuildingType::Brewery,        BuildingType::Mill,       BuildingType::PigFarm,
       BuildingType::Slaughterhouse, BuildingType::Bakery,     BuildingType::DonkeyBreeder}};
    const unsigned numResGatherBlds = 14; /* The first n buildings in the above list, that gather resources */

    // LOG.write(("new buildorders %i whs and %i mil for player %i
    // \n",aii.GetStorehouses().size(),aii.GetMilitaryBuildings().size(),playerId);

    const std::list<nobBaseWarehouse*>& storehouses = aii.GetStorehouses();
    if(!storehouses.empty())
    {
        // collect swords,shields,helpers,privates and beer in first storehouse or whatever is closest to the
        // upgradebuilding if we have one!
        nobBaseWarehouse* wh = GetUpgradeBuildingWarehouse();
        SetGatheringForUpgradeWarehouse(wh);

        if(ggs.GetMaxMilitaryRank() > 0) // there is more than 1 rank available -> distribute
            DistributeMaxRankSoldiersByBlocking(5, wh);
        // 30 boards amd 50 stones for each warehouse - block after that - should speed up expansion and limit losses in
        // case a warehouse is destroyed unlimited when every warehouse has at least that amount
        DistributeGoodsByBlocking(GoodType::Boards, 30);
        DistributeGoodsByBlocking(GoodType::Stones, 50);
        // go to the picked random warehouse and try to build around it
        int randomStore = rand() % (storehouses.size());
        auto it = storehouses.begin();
        std::advance(it, randomStore);
        const MapPoint whPos = (*it)->GetPos();
        UpdateNodesAround(whPos, 15); // update the area we want to build in first
        for(const BuildingType i : bldToTest)
        {
            if(construction->Wanted(i))
            {
                AddBuildJobAroundEveryWarehouse(i); // add a buildorder for the picked buildingtype at every warehouse
            }
        }
        if(gf > 1500 || aii.GetInventory().goods[GoodType::Boards] > 11)
            AddMilitaryBuildJob(whPos);
    }
    // end of construction around & orders for warehouses

    // now pick a random military building and try to build around that as well
    const std::list<nobMilitary*>& militaryBuildings = aii.GetMilitaryBuildings();
    if(militaryBuildings.empty())
        return;
    int randomMiliBld = rand() % militaryBuildings.size();
    auto it2 = militaryBuildings.begin();
    std::advance(it2, randomMiliBld);
    MapPoint bldPos = (*it2)->GetPos();
    UpdateNodesAround(bldPos, 15);
    // resource gathering buildings only around military; processing only close to warehouses
    for(unsigned i = 0; i < numResGatherBlds; i++)
    {
        if(construction->Wanted(bldToTest[i]))
        {
            AddBuildJobAroundEveryMilBld(bldToTest[i]);
        }
    }
    AddMilitaryBuildJob(bldPos);
    if((*it2)->IsUseless() && (*it2)->IsDemolitionAllowed() && randomMiliBld != UpdateUpgradeBuilding())
    {
        aii.DestroyBuilding(bldPos);
    }
}

bool AIPlayerJH::TestDefeat()
{
    if(isInitGfCompleted >= 10 && aii.GetStorehouses().empty())
    {
        // LOG.write(("ai defeated player %i \n",playerId);
        defeated = true;
        aii.Surrender();
        aii.Chat(_("You win"));
        return true;
    }
    return false;
}

unsigned AIPlayerJH::GetNumJobs() const
{
    return eventManager.GetEventNum() + construction->GetBuildJobNum() + construction->GetConnectJobNum();
}

/// returns the warehouse closest to the upgradebuilding or if it cant find a way the first warehouse and if there is no
/// warehouse left null
nobBaseWarehouse* AIPlayerJH::GetUpgradeBuildingWarehouse()
{
    const std::list<nobBaseWarehouse*>& storehouses = aii.GetStorehouses();
    if(storehouses.empty())
        return nullptr;
    nobBaseWarehouse* wh = storehouses.front();
    int uub = UpdateUpgradeBuilding();

    if(uub >= 0
       && storehouses.size() > 1) // upgradebuilding exists and more than 1 warehouse -> find warehouse closest to the
                                  // upgradebuilding - gather stuff there and deactivate gathering in the previous one
    {
        auto upgradeBldIt = aii.GetMilitaryBuildings().begin();
        std::advance(upgradeBldIt, uub);
        // which warehouse is closest to the upgrade building? -> train troops there and block max ranks
        wh = aii.FindWarehouse(**upgradeBldIt, FW::NoCondition(), false, false);
        if(!wh)
            wh = storehouses.front();
    }
    return wh;
}

void AIPlayerJH::AddMilitaryBuildJob(MapPoint pt)
{
    const auto milBld = construction->ChooseMilitaryBuilding(pt);
    if(milBld)
        AddBuildJob(*milBld, pt);
}

void AIPlayerJH::AddBuildJob(BuildingType type, const MapPoint pt, bool front, bool searchPosition)
{
    construction->AddBuildJob(
      std::make_unique<BuildJob>(*this, type, pt, searchPosition ? SearchMode::Radius : SearchMode::None), front);
}

void AIPlayerJH::AddBuildJobAroundEveryWarehouse(BuildingType bt)
{
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        AddBuildJob(bt, wh->GetPos(), false);
    }
}

void AIPlayerJH::AddBuildJobAroundEveryMilBld(BuildingType bt)
{
    for(const nobMilitary* milBld : aii.GetMilitaryBuildings())
    {
        AddBuildJob(bt, milBld->GetPos(), false);
    }
}

void AIPlayerJH::SetGatheringForUpgradeWarehouse(nobBaseWarehouse* upgradewarehouse)
{
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        // deactivate gathering for all warehouses that are NOT the one next to the upgradebuilding
        const MapPoint whPos = wh->GetPos();
        if(upgradewarehouse->GetPos() != whPos)
        {
            if(wh->IsInventorySetting(GoodType::Beer, EInventorySetting::Collect)) // collecting beer? -> stop it
                aii.SetInventorySetting(whPos, GoodType::Beer, InventorySetting());

            if(wh->IsInventorySetting(GoodType::Sword, EInventorySetting::Collect)) // collecting swords? -> stop it
                aii.SetInventorySetting(whPos, GoodType::Sword, InventorySetting());

            if(wh->IsInventorySetting(GoodType::ShieldRomans,
                                      EInventorySetting::Collect)) // collecting shields? -> stop it
                aii.SetInventorySetting(whPos, GoodType::ShieldRomans, InventorySetting());

            if(wh->IsInventorySetting(Job::Private, EInventorySetting::Collect)) // collecting privates? -> stop it
                aii.SetInventorySetting(whPos, Job::Private, InventorySetting());

            if(wh->IsInventorySetting(Job::Helper, EInventorySetting::Collect)) // collecting helpers? -> stop it
                aii.SetInventorySetting(whPos, Job::Helper, InventorySetting());
        } else // activate gathering in the closest warehouse
        {
            if(!wh->IsInventorySetting(GoodType::Beer, EInventorySetting::Collect)) // not collecting beer? -> start it
                aii.SetInventorySetting(whPos, GoodType::Beer, EInventorySetting::Collect);

            if(!wh->IsInventorySetting(GoodType::Sword,
                                       EInventorySetting::Collect)) // not collecting swords? -> start it
                aii.SetInventorySetting(whPos, GoodType::Sword, EInventorySetting::Collect);

            if(!wh->IsInventorySetting(GoodType::ShieldRomans,
                                       EInventorySetting::Collect)) // not collecting shields? -> start it
                aii.SetInventorySetting(whPos, GoodType::ShieldRomans, EInventorySetting::Collect);

            if(!wh->IsInventorySetting(Job::Private, EInventorySetting::Collect)
               && ggs.GetMaxMilitaryRank()
                    > 0) // not collecting privates AND we can actually upgrade soldiers? -> start it
                aii.SetInventorySetting(whPos, Job::Private, EInventorySetting::Collect);

            // less than 50 helpers - collect them: more than 50 stop collecting
            if(wh->GetInventory().people[Job::Helper] < 50)
            {
                if(!wh->IsInventorySetting(Job::Helper, EInventorySetting::Collect))
                    aii.SetInventorySetting(whPos, Job::Helper, EInventorySetting::Collect);
            } else
            {
                if(wh->IsInventorySetting(Job::Helper, EInventorySetting::Collect))
                    aii.SetInventorySetting(whPos, Job::Helper, InventorySetting());
            }
        }
    }
}

AINodeResource AIPlayerJH::CalcResource(MapPoint pt)
{
    const AISubSurfaceResource subRes = aii.GetSubsurfaceResource(pt);
    const AISurfaceResource surfRes = aii.GetSurfaceResource(pt);

    // no resources underground
    if(subRes == AISubSurfaceResource::Nothing)
    {
        // also no resource on the ground: plant space or unusable?
        if(surfRes == AISurfaceResource::Nothing)
        {
            // already road, really no resources here
            if(gwb.IsOnRoad(pt))
                return AINodeResource::Nothing;
            // check for vital plant space
            if(!gwb.IsOfTerrain(pt, [](const TerrainDesc& desc) { return desc.IsVital(); }))
                return AINodeResource::Nothing;
            return AINodeResource::Plantspace;
        } else
            return convertToNodeResource(surfRes);
    } else // resources in underground
    {
        switch(surfRes)
        {
            case AISurfaceResource::Stones:
            case AISurfaceResource::Wood: return AINodeResource::Multiple;
            case AISurfaceResource::Blocked: break;
            case AISurfaceResource::Nothing: return convertToNodeResource(subRes);
        }
        return AINodeResource::Nothing;
    }
}

void AIPlayerJH::InitReachableNodes()
{
    std::queue<MapPoint> toCheck;

    // Alle auf not reachable setzen
    RTTR_FOREACH_PT(MapPoint, aiMap.GetSize())
    {
        Node& node = aiMap[pt];
        node.reachable = false;
        node.failed_penalty = 0;
        const auto* myFlag = gwb.GetSpecObj<noFlag>(pt);
        if(myFlag && myFlag->GetPlayer() == playerId)
        {
            node.reachable = true;
            toCheck.push(pt);
        }
    }

    IterativeReachableNodeChecker(toCheck);
}

void AIPlayerJH::IterativeReachableNodeChecker(std::queue<MapPoint> toCheck)
{
    // TODO auch mal bootswege bauen können

    PathConditionRoad<GameWorldBase> roadPathChecker(gwb, false);
    while(!toCheck.empty())
    {
        // Reachable coordinate
        MapPoint curPt = toCheck.front();

        // Coordinates to test around this reachable coordinate
        for(const MapPoint curNeighbour : aiMap.GetNeighbours(curPt))
        {
            Node& node = aiMap[curNeighbour];

            // already reached, don't test again
            if(node.reachable)
                continue;

            // Test whether point is reachable; yes->add to check list
            if(roadPathChecker.IsNodeOk(curNeighbour))
            {
                if(node.failed_penalty == 0)
                {
                    node.reachable = true;
                    toCheck.push(curNeighbour);
                } else
                {
                    node.failed_penalty--;
                }
            }
        }
        toCheck.pop();
    }
}

void AIPlayerJH::UpdateReachableNodes(const std::vector<MapPoint>& pts)
{
    std::queue<MapPoint> toCheck;

    for(const MapPoint& curPt : pts)
    {
        const auto* flag = gwb.GetSpecObj<noFlag>(curPt);
        if(flag && flag->GetPlayer() == playerId)
        {
            aiMap[curPt].reachable = true;
            toCheck.push(curPt);
        } else
            aiMap[curPt].reachable = false;
    }
    IterativeReachableNodeChecker(toCheck);
}

void AIPlayerJH::InitNodes()
{
    aiMap.Resize(gwb.GetSize());

    InitReachableNodes();

    RTTR_FOREACH_PT(MapPoint, aiMap.GetSize())
    {
        Node& node = aiMap[pt];

        node.bq = aii.GetBuildingQuality(pt);
        node.res = CalcResource(pt);
        node.owned = aii.IsOwnTerritory(pt);
        node.border = aii.IsBorder(pt);
        node.farmed = false;
    }
}

void AIPlayerJH::UpdateNodesAround(const MapPoint pt, unsigned radius)
{
    std::vector<MapPoint> pts = gwb.GetPointsInRadius(pt, radius);
    UpdateReachableNodes(pts);
    for(const MapPoint& pt : pts)
    {
        Node& node = aiMap[pt];
        // Change of ownership might change bq
        node.bq = aii.GetBuildingQuality(pt);
        node.owned = aii.IsOwnTerritory(pt);
        node.border = aii.IsBorder(pt);
    }
}

void AIPlayerJH::InitResourceMaps()
{
    for(auto& resMap : resourceMaps)
        resMap.init();
}

void AIPlayerJH::SetFarmedNodes(const MapPoint pt, bool set)
{
    // Radius in dem Bauplatz für Felder blockiert wird
    const unsigned radius = 3;

    aiMap[pt].farmed = set;
    std::vector<MapPoint> pts = gwb.GetPointsInRadius(pt, radius);
    for(const MapPoint& curPt : pts)
        aiMap[curPt].farmed = set;
}

MapPoint AIPlayerJH::FindBestPosition(const MapPoint& pt, AIResource res, BuildingQuality size, unsigned radius,
                                      int minimum)
{
    resourceMaps[res].updateAround(pt, radius);
    return resourceMaps[res].findBestPosition(pt, size, radius, minimum);
}

void AIPlayerJH::ExecuteAIJob()
{
    // Check whether current job is finished...
    /*if (currentJob)
    {
        if (currentJob->GetStatus() == JobState::Finished)
        {
            delete currentJob;
            currentJob = 0;
        }
    }

    // ... or it failed
    if (currentJob)
    {
        if (currentJob->GetStatus() == JobState::Failed)
        {
            // TODO fehlerbehandlung?
            //std::cout << "Job failed." << std::endl;
            delete currentJob;
            currentJob = 0;
        }
    }*/
    unsigned quota = 10; // limit the amount of events to handle
    while(eventManager.EventAvailable()
          && quota) // handle all new events - some will add new orders but they can all be handled instantly
    {
        quota--;
        currentJob = std::make_unique<EventJob>(*this, eventManager.GetEvent());
        currentJob->ExecuteJob();
    }
    // how many construction & connect jobs the ai will attempt every gf, the ai gets new orders from events and every
    // 200 gf
    quota = (aii.GetStorehouses().size() + aii.GetMilitaryBuildings().size()) * 1;
    if(quota > 40)
        quota = 40;

    construction->ExecuteJobs(quota); // try to execute up to quota connect & construction jobs
    /*
    // if no current job available, take next one! events first, then constructions
    if (!currentJob)
    {
        if (construction->BuildJobAvailable())
        {
            currentJob = construction->GetBuildJob();
        }
    }
    // Something to do? Do it!
    if (currentJob)
        currentJob->ExecuteJob();
        */
}

void AIPlayerJH::DistributeGoodsByBlocking(const GoodType good, unsigned limit)
{
    const std::list<nobBaseWarehouse*>& storehouses = aii.GetStorehouses();
    if(aii.GetHarbors().size() >= storehouses.size() / 2)
    {
        // dont distribute on maps that are mostly sea maps - harbors are too difficult to defend and have to handle
        // quite a lot of traffic already So unblock everywhere
        for(nobBaseWarehouse* wh : storehouses)
        {
            if(wh->IsInventorySetting(good, EInventorySetting::Stop)) // not unblocked then issue command to unblock
                aii.SetInventorySetting(wh->GetPos(), good,
                                        wh->GetInventorySetting(good).Toggle(EInventorySetting::Stop));
        }
        return;
    }

    RTTR_Assert(storehouses.size() >= 2); // Should be assured by condition above
    // We can only distribute between reachable warehouses, so divide them
    std::vector<std::vector<const nobBaseWarehouse*>> whsByReachability;
    for(const nobBaseWarehouse* wh : storehouses)
    {
        // See to which other whs this is connected
        bool foundConnectedWh = false;
        for(std::vector<const nobBaseWarehouse*>& whGroup : whsByReachability)
        {
            if(aii.FindPathOnRoads(*wh, *whGroup.front()))
            {
                whGroup.push_back(wh);
                foundConnectedWh = true;
                break;
            }
        }
        // Not connected to any other -> Add new group
        if(!foundConnectedWh)
            whsByReachability.push_back(std::vector<const nobBaseWarehouse*>(1, wh));
    }

    // Now check each group individually
    for(const std::vector<const nobBaseWarehouse*>& whGroup : whsByReachability)
    {
        // First check if all WHs have more than limit goods (or better: if one does not)
        bool allWHsHaveLimit = true;
        for(const nobBaseWarehouse* wh : whGroup)
        {
            if(wh->GetNumVisualWares(good) <= limit)
            {
                allWHsHaveLimit = false;
                break;
            }
        }
        if(allWHsHaveLimit)
        {
            // So unblock everywhere
            for(const nobBaseWarehouse* wh : whGroup)
            {
                if(wh->IsInventorySetting(good, EInventorySetting::Stop)) // not unblocked then issue command to unblock
                    aii.SetInventorySetting(wh->GetPos(), good,
                                            wh->GetInventorySetting(good).Toggle(EInventorySetting::Stop));
            }
        } else
        {
            // At least 1 WH needs wares
            for(const nobBaseWarehouse* wh : whGroup)
            {
                if(wh->GetNumVisualWares(good) <= limit) // not at limit - unblock it
                {
                    if(wh->IsInventorySetting(good,
                                              EInventorySetting::Stop)) // not unblocked then issue command to unblock
                        aii.SetInventorySetting(wh->GetPos(), good,
                                                wh->GetInventorySetting(good).Toggle(EInventorySetting::Stop));
                } else // at limit - block it
                {
                    if(!wh->IsInventorySetting(good,
                                               EInventorySetting::Stop)) // not blocked then issue command to block
                        aii.SetInventorySetting(wh->GetPos(), good,
                                                wh->GetInventorySetting(good).Toggle(EInventorySetting::Stop));
                }
            }
        }
    }
}

void AIPlayerJH::DistributeMaxRankSoldiersByBlocking(unsigned limit, nobBaseWarehouse* upwh)
{
    const std::list<nobBaseWarehouse*>& storehouses = aii.GetStorehouses();
    unsigned numCompleteWh = storehouses.size();

    if(numCompleteWh < 1) // no warehouses -> no job
        return;

    ::Job maxRankJob = SOLDIER_JOBS[ggs.GetMaxMilitaryRank()];

    if(numCompleteWh == 1) // only 1 warehouse? dont block max ranks here
    {
        nobBaseWarehouse& wh = *storehouses.front();
        if(wh.IsInventorySetting(maxRankJob, EInventorySetting::Stop))
            aii.SetInventorySetting(wh.GetPos(), maxRankJob,
                                    wh.GetInventorySetting(maxRankJob).Toggle(EInventorySetting::Stop));
        return;
    }
    // rest applies for at least 2 complete warehouses!
    std::list<const nobMilitary*> frontierMils; // make a list containing frontier military buildings
    for(const nobMilitary* wh : aii.GetMilitaryBuildings())
    {
        if(wh->GetFrontierDistance() != FrontierDistance::Far && !wh->IsNewBuilt())
            frontierMils.push_back(wh);
    }
    std::list<const nobBaseWarehouse*>
      frontierWhs; // make a list containing all warehouses near frontier military buildings
    for(const nobBaseWarehouse* wh : storehouses)
    {
        for(const nobMilitary* milBld : frontierMils)
        {
            if(gwb.CalcDistance(wh->GetPos(), milBld->GetPos()) < 12)
            {
                frontierWhs.push_back(wh);
                break;
            }
        }
    }
    // have frontier warehouses?
    if(!frontierWhs.empty())
    {
        // LOG.write(("distribute maxranks - got frontierwhs for player %i \n",playerId);
        bool hasUnderstaffedWh = false;
        // try to gather limit maxranks in each - if we have that many unblock for all frontier whs,
        // check if there is at least one with less than limit first
        for(const nobBaseWarehouse* wh : frontierWhs)
        {
            if(wh->GetInventory().people[maxRankJob] < limit)
            {
                hasUnderstaffedWh = true;
                break;
            }
        }
        // if understaffed was found block in all with >=limit else unblock in all
        for(const nobBaseWarehouse* wh : storehouses)
        {
            const bool shouldBlock = !helpers::contains(frontierWhs, wh) // Not a frontier wh or:
                                     || (hasUnderstaffedWh && wh->GetInventory().people[maxRankJob] >= limit);
            if(shouldBlock != wh->IsInventorySetting(maxRankJob, EInventorySetting::Stop))
                aii.SetInventorySetting(wh->GetPos(), maxRankJob,
                                        wh->GetInventorySetting(maxRankJob).Toggle(EInventorySetting::Stop));
        }
    } else // there are no frontier whs!
    {
        // LOG.write(("distribute maxranks - got NO frontierwhs for player %i \n",playerId);
        bool hasUnderstaffedWh = false;
        // try to gather limit maxranks in each - if we have that many unblock for all  whs,
        // check if there is at least one with less than limit first
        for(const nobBaseWarehouse* wh : storehouses)
        {
            if(wh->GetInventory().people[maxRankJob] < limit
               && wh->GetPos() != upwh->GetPos()) // warehouse next to upgradebuilding is special case
            {
                hasUnderstaffedWh = true;
                break;
            }
        }
        for(const nobBaseWarehouse* wh : storehouses)
        {
            bool shouldBlock;
            if(wh->GetPos()
               == upwh->GetPos()) // warehouse next to upgradebuilding should block when there is more than 1 wh
            {
                // LOG.write(("distribute maxranks - got NO frontierwhs for player %i , block at hq \n",playerId);
                shouldBlock = true;
            } else if(hasUnderstaffedWh)
            {
                shouldBlock = wh->GetInventory().people[maxRankJob] >= limit;
            } else // no understaffedwh
                shouldBlock = false;
            if(shouldBlock != wh->IsInventorySetting(maxRankJob, EInventorySetting::Stop))
                aii.SetInventorySetting(wh->GetPos(), maxRankJob,
                                        wh->GetInventorySetting(maxRankJob).Toggle(EInventorySetting::Stop));
        }
    }
}
MapPoint AIPlayerJH::SimpleFindPosition(const MapPoint& pt, BuildingQuality size, unsigned radius) const
{
    std::vector<MapPoint> pts = gwb.GetPointsInRadius(pt, radius);
    for(const MapPoint& curPt : pts)
    {
        if(!aiMap[curPt].reachable || aiMap[curPt].farmed || !aii.IsOwnTerritory(curPt))
            continue;
        if(aii.isHarborPosClose(curPt, 2, true))
        {
            if(size != BuildingQuality::Harbor)
                continue;
        }
        RTTR_Assert(aii.GetBuildingQuality(curPt) == GetAINode(curPt).bq);
        if(canUseBq(aii.GetBuildingQuality(curPt), size)) //(*nodes)[idx].bq; TODO: Update nodes BQ and use that
            return curPt;
    }

    return MapPoint::Invalid();
}

MapPoint AIPlayerJH::FindPositionForBuildingAround(BuildingType type, const MapPoint& around)
{
    constexpr unsigned searchRadius = 11;
    MapPoint foundPos = MapPoint::Invalid();
    switch(type)
    {
        case BuildingType::Woodcutter:
        {
            foundPos = FindBestPosition(around, AIResource::Wood, BUILDING_SIZE[type], searchRadius, 20);
            break;
        }
        case BuildingType::Forester:
            // ensure some distance to other foresters and an minimal amount of plantspace
            if(!construction->OtherUsualBuildingInRadius(around, 12, BuildingType::Forester)
               && GetDensity(around, AIResource::Plantspace, 7) > 15)
                foundPos = FindBestPosition(around, AIResource::Wood, BUILDING_SIZE[type], searchRadius, 0);
            break;
        case BuildingType::Hunter:
        {
            // check if there are any animals in range
            if(HuntablesinRange(around, (2 << GetBldPlanner().GetNumBuildings(BuildingType::Hunter))))
                foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius);
            break;
        }
        case BuildingType::Quarry:
        {
            unsigned numQuarries = GetBldPlanner().GetNumBuildings(BuildingType::Quarry);
            foundPos = FindBestPosition(around, AIResource::Stones, BUILDING_SIZE[type], searchRadius,
                                        std::min(40u, 1 + numQuarries * 10));
            if(foundPos.isValid() && !ValidStoneinRange(foundPos))
            {
                resourceMaps[AIResource::Stones].avoidPosition(foundPos);
                foundPos = MapPoint::Invalid();
            }
            break;
        }
        case BuildingType::Barracks:
        case BuildingType::Guardhouse:
        case BuildingType::Watchtower:
        case BuildingType::Fortress:
            foundPos = FindBestPosition(around, AIResource::Borderland, BUILDING_SIZE[type], searchRadius);
            break;
        case BuildingType::GoldMine:
            foundPos = FindBestPosition(around, AIResource::Gold, BuildingQuality::Mine, searchRadius);
            break;
        case BuildingType::CoalMine:
            foundPos = FindBestPosition(around, AIResource::Coal, BuildingQuality::Mine, searchRadius);
            break;
        case BuildingType::IronMine:
            foundPos = FindBestPosition(around, AIResource::Ironore, BuildingQuality::Mine, searchRadius);
            break;
        case BuildingType::GraniteMine:
            if(!ggs.isEnabled(
                 AddonId::INEXHAUSTIBLE_GRANITEMINES)) // inexhaustible granite mines do not require granite
                foundPos = FindBestPosition(around, AIResource::Granite, BuildingQuality::Mine, searchRadius);
            else
                foundPos = SimpleFindPosition(around, BuildingQuality::Mine, searchRadius);
            break;

        case BuildingType::Fishery:
            foundPos = FindBestPosition(around, AIResource::Fish, BUILDING_SIZE[type], searchRadius);
            if(foundPos.isValid() && !ValidFishInRange(foundPos))
            {
                resourceMaps[AIResource::Fish].avoidPosition(foundPos);
                foundPos = MapPoint::Invalid();
            }
            break;
        case BuildingType::Storehouse:
            if(!construction->OtherStoreInRadius(around, 15))
                foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius);
            break;
        case BuildingType::HarborBuilding:
            foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius);
            if(foundPos.isValid()
               && !HarborPosRelevant(GetWorld().GetHarborPointID(foundPos))) // bad harborspot detected DO NOT USE
                foundPos = MapPoint::Invalid();
            break;
        case BuildingType::Shipyard:
            foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius);
            if(foundPos.isValid() && IsInvalidShipyardPosition(foundPos))
                foundPos = MapPoint::Invalid();
            break;
        case BuildingType::Farm:
            foundPos = FindBestPosition(around, AIResource::Plantspace, BUILDING_SIZE[type], searchRadius, 85);
            if(foundPos.isValid())
                foundPos = FindBestPosition(around, AIResource::Plantspace, BUILDING_SIZE[type], searchRadius, 85);
            break;
        case BuildingType::Catapult:
            foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius);
            if(foundPos.isValid() && aii.isBuildingNearby(BuildingType::Catapult, foundPos, 7))
                foundPos = MapPoint::Invalid();
            break;
        default: foundPos = SimpleFindPosition(around, BUILDING_SIZE[type], searchRadius); break;
    }
    return foundPos;
}

unsigned AIPlayerJH::GetDensity(MapPoint pt, AIResource res, int radius)
{
    RTTR_Assert(pt.x < aiMap.GetWidth() && pt.y < aiMap.GetHeight());

    std::vector<MapPoint> pts = gwb.GetPointsInRadius(pt, radius);
    const unsigned numAllPTs = pts.size();
    RTTR_Assert(numAllPTs > 0);

    const auto hasResource = [this, res](const MapPoint& curPt) {
        // TODO: Fix
        // return aiMap[curPt].res == res;
        return CalcResource(curPt) == res;
    };
    const unsigned numGoodPts = helpers::count_if(pts, hasResource);
    return (numGoodPts * 100) / numAllPTs;
}

void AIPlayerJH::HandleNewMilitaryBuildingOccupied(const MapPoint pt)
{
    // kill bad flags we find
    RemoveAllUnusedRoads(pt);
    bldPlanner->UpdateBuildingsWanted(*this);
    const auto* mil = gwb.GetSpecObj<nobMilitary>(pt);
    if(!mil)
        return;
    // if near border and gold disabled (by addon): enable it
    if(mil->GetFrontierDistance() != FrontierDistance::Far)
    {
        if(mil->IsGoldDisabled())
            aii.SetCoinsAllowed(pt, true);
    } else if((mil->GetBuildingType() == BuildingType::Barracks || mil->GetBuildingType() == BuildingType::Guardhouse)
              && mil->GetBuildingType() != construction->GetBiggestAllowedMilBuilding())
    {
        if(!mil->IsGoldDisabled())
            aii.SetCoinsAllowed(pt, false);
    }

    AddBuildJob(BuildingType::HarborBuilding, pt);
    if(!IsInvalidShipyardPosition(pt))
        AddBuildJob(BuildingType::Shipyard, pt);
    if(SoldierAvailable())
        AddMilitaryBuildJob(pt);

    // try to build one the following buildings around the new military building

    std::array<BuildingType, 11> bldToTest = {
      BuildingType::Storehouse, BuildingType::Woodcutter, BuildingType::Quarry,      BuildingType::GoldMine,
      BuildingType::CoalMine,   BuildingType::IronMine,   BuildingType::GraniteMine, BuildingType::Fishery,
      BuildingType::Farm,       BuildingType::Hunter,     BuildingType::Forester};
    unsigned bldToTestStartIdx = 0;
    // remove the storehouse from the building test list if we are close to another storehouse already
    for(const nobBaseWarehouse* bldSite : aii.GetStorehouses())
    {
        if(gwb.CalcDistance(bldSite->GetPos(), pt) < 20)
        {
            bldToTestStartIdx = 1;
            break;
        }
    }
    // same is true for warehouses which are still in production
    for(const noBuildingSite* bldSite : aii.GetBuildingSites())
    {
        if(BuildingProperties::IsWareHouse(bldSite->GetBuildingType()))
        {
            if(gwb.CalcDistance(bldSite->GetPos(), pt) < 20)
            {
                bldToTestStartIdx = 1;
                break;
            }
        }
    }

    for(unsigned i = bldToTestStartIdx; i < bldToTest.size(); ++i)
    {
        if(construction->Wanted(bldToTest[i]))
        {
            AddBuildJob(bldToTest[i], pt);
        }
    }
}

void AIPlayerJH::HandleBuilingDestroyed(MapPoint pt, BuildingType bld)
{
    switch(bld)
    {
        case BuildingType::Charburner:
        case BuildingType::Farm: SetFarmedNodes(pt, false); break;
        case BuildingType::HarborBuilding:
        {
            // destroy all other buildings around the harborspot in range 2 so we can rebuild the harbor ...
            for(const MapPoint curPt : gwb.GetPointsInRadius(pt, 2))
            {
                const auto* const bb = gwb.GetSpecObj<noBaseBuilding>(curPt);
                if(bb)
                    aii.DestroyBuilding(curPt);
                else
                {
                    const auto* const bs = gwb.GetSpecObj<noBuildingSite>(curPt);
                    if(bs)
                        aii.DestroyFlag(gwb.GetNeighbour(curPt, Direction::SouthEast));
                }
            }
            break;
        }
        default: break;
    }
}

void AIPlayerJH::HandleRoadConstructionComplete(MapPoint pt, Direction dir)
{
    // todo: detect "bad" roads and handle them
    const auto* flag = gwb.GetSpecObj<noFlag>(pt);
    // does the flag still exist?
    if(!flag)
        return;
    // does the roadsegment still exist?
    const RoadSegment* const roadSeg = flag->GetRoute(dir);
    if(!roadSeg || roadSeg->GetLength() < 4) // road too short to need flags
        return;
    // check if this road leads to a warehouseflag and if it does start setting flags from the warehouseflag else from
    // the new flag goal is to move roadsegments with a length of more than 2 away from the warehouse
    const noFlag& otherFlag = roadSeg->GetOtherFlag(*flag);
    MapPoint bldPos = gwb.GetNeighbour(otherFlag.GetPos(), Direction::NorthWest);
    if(aii.IsBuildingOnNode(bldPos, BuildingType::Storehouse)
       || aii.IsBuildingOnNode(bldPos, BuildingType::HarborBuilding)
       || aii.IsBuildingOnNode(bldPos, BuildingType::Headquarters))
        construction->SetFlagsAlongRoad(otherFlag, roadSeg->GetOtherFlagDir(*flag) + 3u);
    else
    {
        // set flags on our new road starting from the new flag
        construction->SetFlagsAlongRoad(*flag, dir);
    }
}

void AIPlayerJH::HandleRoadConstructionFailed(const MapPoint pt, Direction)
{
    const auto* flag = gwb.GetSpecObj<noFlag>(pt);
    // does the flag still exist?
    if(!flag)
        return;
    // is it our flag?
    if(flag->GetPlayer() != playerId)
        return;
    // if it isnt a useless flag AND it has no current road connection then retry to build a road.
    if(RemoveUnusedRoad(*flag, boost::none, true, false))
        construction->AddConnectFlagJob(flag);
}

void AIPlayerJH::HandleMilitaryBuilingLost(const MapPoint pt)
{
    // For now, this is the same as losing land.
    HandleLostLand(pt);
}

void AIPlayerJH::HandleBuildingFinished(const MapPoint pt, BuildingType bld)
{
    switch(bld)
    {
        case BuildingType::HarborBuilding:
            UpdateNodesAround(pt, 8); // todo: fix radius
            RemoveAllUnusedRoads(
              pt); // repair & reconnect road system - required when a colony gets a new harbor by expedition
            aii.ChangeReserve(pt, 0, 1); // order 1 defender to stay in the harborbuilding

            // if there are positions free start an expedition!
            if(HarborPosRelevant(gwb.GetHarborPointID(pt), true))
            {
                aii.StartStopExpedition(pt, true);
            }
            break;

        case BuildingType::Shipyard: aii.SetShipYardMode(pt, true); break;

        case BuildingType::Storehouse: break;
        case BuildingType::Woodcutter: AddBuildJob(BuildingType::Sawmill, pt); break;
        default: break;
    }
}

void AIPlayerJH::HandleNewColonyFounded(const MapPoint pt)
{
    construction->AddConnectFlagJob(gwb.GetSpecObj<noFlag>(gwb.GetNeighbour(pt, Direction::SouthEast)));
}

void AIPlayerJH::HandleExpedition(const noShip* ship)
{
    if(!ship->IsWaitingForExpeditionInstructions())
        return;
    if(ship->IsAbleToFoundColony())
        aii.FoundColony(ship);
    else
    {
        const unsigned offset = rand() % helpers::MaxEnumValue_v<ShipDirection>;
        for(auto dir : helpers::EnumRange<ShipDirection>{})
        {
            dir = ShipDirection((rttr::enum_cast(dir) + offset) % helpers::MaxEnumValue_v<ShipDirection>);
            if(aii.IsExplorationDirectionPossible(ship->GetPos(), ship->GetCurrentHarbor(), dir))
            {
                aii.TravelToNextSpot(dir, ship);
                return;
            }
        }
        // no direction possible, sad, stop it
        aii.CancelExpedition(ship);
    }
}

void AIPlayerJH::HandleExpedition(const MapPoint pt)
{
    const noShip* ship = nullptr;

    for(const noBase& obj : gwb.GetFigures(pt))
    {
        if(obj.GetGOT() == GO_Type::Ship)
        {
            const auto& curShip = static_cast<const noShip&>(obj);
            if(curShip.GetPlayerId() == playerId && curShip.IsWaitingForExpeditionInstructions())
            {
                ship = &curShip;
                break;
            }
        }
    }
    if(ship)
    {
        HandleExpedition(ship);
    }
}

void AIPlayerJH::HandleTreeChopped(const MapPoint pt)
{
    // std::cout << "Tree chopped." << std::endl;

    aiMap[pt].reachable = true;

    UpdateNodesAround(pt, 3);

    int random = rand();

    if(random % 2 == 0)
        AddMilitaryBuildJob(pt);
    else // if (random % 12 == 0)
        AddBuildJob(BuildingType::Woodcutter, pt);
}

void AIPlayerJH::HandleNoMoreResourcesReachable(const MapPoint pt, BuildingType bld)
{
    // Destroy old building (once)

    if(!aii.IsObjectTypeOnNode(pt, NodalObjectType::Building))
        return;
    // keep 2 woodcutters for each forester even if they sometimes run out of trees
    if(bld == BuildingType::Woodcutter)
    {
        for(const nobUsual* forester : aii.GetBuildings(BuildingType::Forester))
        {
            // is the forester somewhat close?
            if(gwb.CalcDistance(pt, forester->GetPos()) <= RES_RADIUS[AIResource::Wood])
            {
                // then find it's 2 woodcutters
                unsigned maxdist = gwb.CalcDistance(pt, forester->GetPos());
                int betterwoodcutters = 0;
                for(const nobUsual* woodcutter : aii.GetBuildings(BuildingType::Woodcutter))
                {
                    // dont count the woodcutter in question
                    if(pt == woodcutter->GetPos())
                        continue;
                    // TODO: We currently don't take the distance to the forester into account when placing a woodcutter
                    // This leads to points beeing equally good for placing but later it will be destroyed. Avoid that
                    // by checking only close woddcutters
                    if(gwb.CalcDistance(woodcutter->GetPos(), pt) > RES_RADIUS[AIResource::Wood])
                        continue;
                    // closer or equally close to forester than woodcutter in question?
                    if(gwb.CalcDistance(woodcutter->GetPos(), forester->GetPos()) <= maxdist)
                    {
                        betterwoodcutters++;
                        if(betterwoodcutters >= 2)
                            break;
                    }
                }
                // couldnt find 2 closer woodcutter -> keep it alive
                if(betterwoodcutters < 2)
                    return;
            }
        }
    }
    aii.DestroyBuilding(pt);
    // fishery cant find fish? set fish value at location to 0 so we dont have to calculate the value for this location
    // again
    if(bld == BuildingType::Fishery)
        resourceMaps[AIResource::Fish].avoidPosition(pt);

    UpdateNodesAround(pt, 11); // todo: fix radius
    RemoveUnusedRoad(*gwb.GetSpecObj<noFlag>(gwb.GetNeighbour(pt, Direction::SouthEast)), Direction::NorthWest, true);

    // try to expand, maybe res blocked a passage
    AddMilitaryBuildJob(pt);

    // and try to rebuild the same building
    if(bld != BuildingType::Hunter)
        AddBuildJob(bld, pt);

    // farm is always good!
    AddBuildJob(BuildingType::Farm, pt);
}

void AIPlayerJH::HandleShipBuilt(const MapPoint pt)
{
    // Stop building ships if reached a maximum (TODO: make variable)
    const std::list<nobUsual*>& shipyards = aii.GetBuildings(BuildingType::Shipyard);
    bool wantMoreShips;
    unsigned numRelevantSeas = GetNumAIRelevantSeaIds();
    if(numRelevantSeas == 0)
        wantMoreShips = false;
    else if(numRelevantSeas == 1)
        wantMoreShips = aii.GetNumShips() <= gwb.GetNumHarborPoints();
    else
    {
        unsigned wantedShipCt = std::min<unsigned>(7, 3 * shipyards.size());
        wantMoreShips = aii.GetNumShips() < wantedShipCt;
    }
    if(!wantMoreShips)
    {
        // Find shipyard from this ship by getting the closest one. Max distance of <12 nodes
        unsigned mindist = 12;
        const nobUsual* creatingShipyard = nullptr;
        for(const nobUsual* shipyard : shipyards)
        {
            unsigned distance = gwb.CalcDistance(shipyard->GetPos(), pt);
            if(distance < mindist)
            {
                mindist = distance;
                creatingShipyard = shipyard;
            }
        }
        if(creatingShipyard) // might have been destroyed by now
            aii.SetProductionEnabled(creatingShipyard->GetPos(), false);
    }
}

void AIPlayerJH::HandleBorderChanged(const MapPoint pt)
{
    UpdateNodesAround(pt, 11); // todo: fix radius

    const auto* mil = gwb.GetSpecObj<nobMilitary>(pt);
    if(mil)
    {
        if(mil->GetFrontierDistance() != FrontierDistance::Far)
        {
            if(mil->IsGoldDisabled())
                aii.SetCoinsAllowed(pt, true);

            // Fill up with soldiers
            for(unsigned rank = 0; rank < NUM_SOLDIER_RANKS; ++rank)
            {
                if(mil->GetTroopLimit(rank) != mil->GetMaxTroopsCt())
                    aii.SetTroopLimit(mil->GetPos(), rank, mil->GetMaxTroopsCt());
            }
        }
        if(mil->GetBuildingType() != construction->GetBiggestAllowedMilBuilding())
        {
            AddMilitaryBuildJob(pt);
        }
    }
}

void AIPlayerJH::HandleLostLand(const MapPoint pt)
{
    if(aii.GetStorehouses()
         .empty()) // check if we have a storehouse left - if we dont have one trying to find a path to one will crash
    {
        return;
    }
    RemoveAllUnusedRoads(pt);
}

void AIPlayerJH::MilUpgradeOptim()
{
    // do we have a upgrade building?
    int upb = UpdateUpgradeBuilding();
    int count = 0;
    const std::list<nobMilitary*>& militaryBuildings = aii.GetMilitaryBuildings();
    for(const nobMilitary* milBld : militaryBuildings)
    {
        if(count != upb) // not upgrade building
        {
            if(upb >= 0) // we do have an upgrade building
            {
                if(!milBld->IsGoldDisabled()) // deactivate gold for all other buildings
                {
                    aii.SetCoinsAllowed(milBld->GetPos(), false);
                }
                if(milBld->GetFrontierDistance() == FrontierDistance::Far
                   && (((unsigned)count + GetNumPlannedConnectedInlandMilitaryBlds())
                       < militaryBuildings.size())) // send out troops until 1 private is left, then cancel road
                {
                    if(milBld->GetNumTroops() > 1) // more than 1 soldier remaining? -> send out order
                    {
                        aii.SetTroopLimit(milBld->GetPos(), 0, 1);
                        for(unsigned rank = 1; rank < NUM_SOLDIER_RANKS; ++rank)
                            aii.SetTroopLimit(milBld->GetPos(), rank, 0);

                        // TODO: Currently the ai still manages soldiers by disconnecting roads, if in the future it
                        // uses only SetTroopLimit then this can be removed
                        for(unsigned rank = 0; rank < NUM_SOLDIER_RANKS; ++rank)
                            aii.SetTroopLimit(milBld->GetPos(), rank, milBld->GetMaxTroopsCt());
                    } else if(!milBld->IsNewBuilt()) // 0-1 soldier remains and the building has had at least 1 soldier
                                                     // at some point and the building is not new on the list-> cancel
                                                     // road (and fix roadsystem if necessary)
                    {
                        RemoveUnusedRoad(*milBld->GetFlag(), Direction::NorthWest, true, true, true);
                    }
                } else if(milBld->GetFrontierDistance()
                          != FrontierDistance::Far) // frontier building - connect to road system
                {
                    construction->AddConnectFlagJob(milBld->GetFlag());
                }
            } else // no upgrade building? -> activate gold for frontier buildings
            {
                if(milBld->IsGoldDisabled() && milBld->GetFrontierDistance() != FrontierDistance::Far)
                {
                    aii.SetCoinsAllowed(milBld->GetPos(), true);
                }
            }
        } else // upgrade building
        {
            if(!construction->IsConnectedToRoadSystem(milBld->GetFlag()))
            {
                construction->AddConnectFlagJob(milBld->GetFlag());
                continue;
            }
            if(milBld->IsGoldDisabled()) // activate gold
            {
                aii.SetCoinsAllowed(milBld->GetPos(), true);
            }
            // Keep 0 max rank soldiers, 1 of each other rank and fill the rest with privates
            aii.SetTroopLimit(milBld->GetPos(), 0, milBld->GetMaxTroopsCt());
            for(unsigned rank = 1; rank < ggs.GetMaxMilitaryRank(); ++rank)
                aii.SetTroopLimit(milBld->GetPos(), rank, 1);
            aii.SetTroopLimit(milBld->GetPos(), ggs.GetMaxMilitaryRank(), 0);
        }
        count++;
    }
}

bool AIPlayerJH::HasFrontierBuildings()
{
    for(const nobMilitary* milBld : aii.GetMilitaryBuildings())
    {
        if(milBld->GetFrontierDistance() != FrontierDistance::Far)
            return true;
    }
    return false;
}

void AIPlayerJH::CheckExpeditions()
{
    const std::list<nobHarborBuilding*>& harbors = aii.GetHarbors();
    for(const nobHarborBuilding* harbor : harbors)
    {
        bool isHarborRelevant = HarborPosRelevant(harbor->GetHarborPosID(), true);
        if(harbor->IsExpeditionActive() != isHarborRelevant) // harbor is collecting for expedition and shouldnt OR not
                                                             // collecting and should -> toggle expedition
        {
            aii.StartStopExpedition(harbor->GetPos(), isHarborRelevant);
        }
    }
    // find lost expedition ships - ai should get a notice and catch them all but just in case some fell through the
    // system
    const std::vector<noShip*>& ships = aii.GetShips();
    for(const noShip* harbor : ships)
    {
        if(harbor->IsWaitingForExpeditionInstructions())
            HandleExpedition(harbor);
    }
}

void AIPlayerJH::CheckForester()
{
    const std::list<nobUsual*>& foresters = aii.GetBuildings(BuildingType::Forester);
    if(!foresters.empty() && foresters.size() < 2 && aii.GetMilitaryBuildings().size() < 3
       && aii.GetBuildingSites().size() < 3)
    // stop the forester
    {
        if(!(*foresters.begin())->IsProductionDisabled())
            aii.SetProductionEnabled(foresters.front()->GetPos(), false);
    } else // activate the forester
    {
        if(!foresters.empty() && (*foresters.begin())->IsProductionDisabled())
            aii.SetProductionEnabled(foresters.front()->GetPos(), true);
    }
}

void AIPlayerJH::CheckGranitMine()
{
    // stop production in granite mines when the ai has many stones (100+ and at least 15 for each warehouse)
    bool enableProduction =
      AmountInStorage(GoodType::Stones) < 100 || AmountInStorage(GoodType::Stones) < 15 * aii.GetStorehouses().size();
    for(const nobUsual* mine : aii.GetBuildings(BuildingType::GraniteMine))
    {
        // !productionDisabled != enableProduction
        if(mine->IsProductionDisabled() == enableProduction)
            aii.SetProductionEnabled(mine->GetPos(), enableProduction);
    }
}

void AIPlayerJH::TryToAttack()
{
    unsigned hq_or_harbor_without_soldiers = 0;
    std::vector<const nobBaseMilitary*> potentialTargets;

    // use own military buildings (except inland buildings) to search for enemy military buildings
    const std::list<nobMilitary*>& militaryBuildings = aii.GetMilitaryBuildings();
    const unsigned numMilBlds = militaryBuildings.size();
    // when the ai has many buildings the ai will not check the complete list every time
    constexpr unsigned limit = 40;
    for(const nobMilitary* milBld : militaryBuildings)
    {
        // We skip the current building with a probability of limit/numMilBlds
        // -> For twice the number of blds as the limit we will most likely skip every 2nd building
        // This way we check roughly (at most) limit buildings but avoid any preference for one building over an other
        if(rand() % numMilBlds > limit)
            continue;

        if(milBld->GetFrontierDistance() == FrontierDistance::Far) // inland building? -> skip it
            continue;

        // get nearby enemy buildings and store in set of potential attacking targets
        MapPoint src = milBld->GetPos();

        sortedMilitaryBlds buildings = gwb.LookForMilitaryBuildings(src, 2);
        for(const nobBaseMilitary* target : buildings)
        {
            if(helpers::contains(potentialTargets, target))
                continue;
            if(target->GetGOT() == GO_Type::NobMilitary && static_cast<const nobMilitary*>(target)->IsNewBuilt())
                continue;
            MapPoint dest = target->GetPos();
            if(gwb.CalcDistance(src, dest) < BASE_ATTACKING_DISTANCE && aii.IsPlayerAttackable(target->GetPlayer())
               && aii.IsVisible(dest))
            {
                if(target->GetGOT() != GO_Type::NobMilitary && !target->DefendersAvailable())
                {
                    // headquarter or harbor without any troops :)
                    hq_or_harbor_without_soldiers++;
                    potentialTargets.insert(potentialTargets.begin(), target);
                } else
                    potentialTargets.push_back(target);
            }
        }
    }

    // shuffle everything but headquarters and harbors without any troops in them
    std::shuffle(potentialTargets.begin() + hq_or_harbor_without_soldiers, potentialTargets.end(),
                 std::mt19937(std::random_device()()));

    // check for each potential attacking target the number of available attacking soldiers
    for(const nobBaseMilitary* target : potentialTargets)
    {
        const MapPoint dest = target->GetPos();

        unsigned attackersCount = 0;
        unsigned attackersStrength = 0;

        // ask each of nearby own military buildings for soldiers to contribute to the potential attack
        sortedMilitaryBlds myBuildings = gwb.LookForMilitaryBuildings(dest, 2);
        for(const nobBaseMilitary* otherMilBld : myBuildings)
        {
            if(otherMilBld->GetPlayer() == playerId)
            {
                const auto* myMil = dynamic_cast<const nobMilitary*>(otherMilBld);
                if(!myMil || myMil->IsUnderAttack())
                    continue;

                unsigned newAttackers;
                attackersStrength += myMil->GetSoldiersStrengthForAttack(dest, newAttackers);
                attackersCount += newAttackers;
            }
        }

        if(attackersCount == 0)
            continue;

        if((level == AI::Level::Hard) && (target->GetGOT() == GO_Type::NobMilitary))
        {
            const auto* enemyTarget = static_cast<const nobMilitary*>(target);
            if(attackersStrength <= enemyTarget->GetSoldiersStrength() || enemyTarget->GetNumTroops() == 0)
                continue;
        }

        aii.Attack(dest, attackersCount, true);
        return;
    }
}

void AIPlayerJH::TrySeaAttack()
{
    if(aii.GetNumShips() < 1)
        return;
    if(aii.GetHarbors().empty())
        return;
    std::vector<unsigned short> seaidswithattackers;
    std::vector<unsigned> attackersatseaid;
    std::vector<int> invalidseas;
    std::deque<const nobBaseMilitary*> potentialTargets;
    std::deque<const nobBaseMilitary*> undefendedTargets;
    std::vector<int> searcharoundharborspots;
    // all seaids with at least 1 ship count available attackers for later checks
    for(const noShip* ship : aii.GetShips())
    {
        // sea id not already listed as valid or invalid?
        if(!helpers::contains(seaidswithattackers, ship->GetSeaID())
           && !helpers::contains(invalidseas, ship->GetSeaID()))
        {
            unsigned attackercount = gwb.GetNumSoldiersForSeaAttackAtSea(playerId, ship->GetSeaID(), false);
            if(attackercount) // got attackers at this sea id? -> add to valid list
            {
                seaidswithattackers.push_back(ship->GetSeaID());
                attackersatseaid.push_back(attackercount);
            } else // not listed but no attackers? ->invalid
            {
                invalidseas.push_back(ship->GetSeaID());
            }
        }
    }
    if(seaidswithattackers.empty()) // no sea ids with attackers? skip the rest
        return;
    /*else
    {
        for(unsigned i=0;i<seaidswithattackers.size();i++)
            LOG.write(("attackers at sea ids for player %i, sea id %i, count %i \n",playerId, seaidswithattackers[i],
    attackersatseaid[i]);
    }*/
    // first check all harbors there might be some undefended ones - start at 1 to skip the harbor dummy
    for(unsigned i = 1; i < gwb.GetNumHarborPoints(); i++)
    {
        const nobHarborBuilding* hb;
        if((hb = gwb.GetSpecObj<nobHarborBuilding>(gwb.GetHarborPoint(i))))
        {
            if(aii.IsVisible(hb->GetPos()))
            {
                if(aii.IsPlayerAttackable(hb->GetPlayer()))
                {
                    // attackers for this building?
                    const std::vector<unsigned short> testseaidswithattackers =
                      gwb.GetFilteredSeaIDsForAttack(gwb.GetHarborPoint(i), seaidswithattackers, playerId);
                    if(!testseaidswithattackers.empty()) // harbor can be attacked?
                    {
                        if(!hb->DefendersAvailable()) // no defenders?
                            undefendedTargets.push_back(hb);
                        else // todo: maybe only attack this when there is a fair win chance for the attackers?
                            potentialTargets.push_back(hb);
                        // LOG.write(("found a defended harbor we can attack at %i,%i \n",hb->GetPos());
                    }
                } else // cant attack player owning the harbor -> add to list
                {
                    searcharoundharborspots.push_back(i);
                }
            }
            // else: not visible for player no need to look any further here
        } else // no harbor -> add to list
        {
            searcharoundharborspots.push_back(i);
            // LOG.write(("found an unused harborspot we have to look around of at %i,%i
            // \n",gwb.GetHarborPoint(i).x,gwb.GetHarborPoint(i).y);
        }
    }
    auto prng = std::mt19937(std::random_device()());
    // any undefendedTargets? -> pick one by random
    if(!undefendedTargets.empty())
    {
        std::shuffle(undefendedTargets.begin(), undefendedTargets.end(), prng);
        for(const nobBaseMilitary* targetMilBld : undefendedTargets)
        {
            std::vector<GameWorldBase::PotentialSeaAttacker> attackers =
              gwb.GetSoldiersForSeaAttack(playerId, targetMilBld->GetPos());
            if(!attackers.empty()) // try to attack it!
            {
                aii.SeaAttack(targetMilBld->GetPos(), 1, true);
                return;
            }
        }
    }
    // add all military buildings around still valid harborspots (unused or used by ally)
    unsigned limit = 15;
    unsigned skip = 0;
    if(searcharoundharborspots.size() > 15)
        skip = std::max<int>(rand() % (searcharoundharborspots.size() / 15 + 1) * 15, 1) - 1;
    for(unsigned i = skip; i < searcharoundharborspots.size() && limit > 0; i++)
    {
        limit--;
        // now add all military buildings around the harborspot to our list of potential targets
        sortedMilitaryBlds buildings = gwb.LookForMilitaryBuildings(gwb.GetHarborPoint(searcharoundharborspots[i]), 2);
        for(const nobBaseMilitary* milBld : buildings)
        {
            if(aii.IsPlayerAttackable(milBld->GetPlayer()) && aii.IsVisible(milBld->GetPos()))
            {
                const auto* enemyTarget = dynamic_cast<const nobMilitary*>((milBld));

                if(enemyTarget && enemyTarget->IsNewBuilt())
                    continue;
                if((milBld->GetGOT() != GO_Type::NobMilitary)
                   && (!milBld->DefendersAvailable())) // undefended headquarter(or unlikely as it is a harbor...) -
                                                       // priority list!
                {
                    const std::vector<unsigned short> testseaidswithattackers =
                      gwb.GetFilteredSeaIDsForAttack(milBld->GetPos(), seaidswithattackers, playerId);
                    if(!testseaidswithattackers.empty())
                    {
                        undefendedTargets.push_back(milBld);
                    }  // else - no attackers - do nothing
                } else // normal target - check is done after random shuffle so we dont have to check every possible
                       // target and instead only enough to get 1 good one
                {
                    potentialTargets.push_back(milBld);
                }
            } // not attackable or no vision of region - do nothing
        }
    }
    // now we have a deque full of available and maybe undefended targets that are available for attack -> shuffle and
    // attack the first one we can attack("should" be the first we check...)  any undefendedTargets? -> pick one by
    // random
    if(!undefendedTargets.empty())
    {
        std::shuffle(undefendedTargets.begin(), undefendedTargets.end(), prng);
        for(const nobBaseMilitary* targetMilBld : undefendedTargets)
        {
            std::vector<GameWorldBase::PotentialSeaAttacker> attackers =
              gwb.GetSoldiersForSeaAttack(playerId, targetMilBld->GetPos());
            if(!attackers.empty()) // try to attack it!
            {
                aii.SeaAttack(targetMilBld->GetPos(), 1, true);
                return;
            }
        }
    }
    std::shuffle(potentialTargets.begin(), potentialTargets.end(), prng);
    for(const nobBaseMilitary* ship : potentialTargets)
    {
        // TODO: decide if it is worth attacking the target and not just "possible"
        // test only if we should have attackers from one of our valid sea ids
        const std::vector<unsigned short> testseaidswithattackers =
          gwb.GetFilteredSeaIDsForAttack(ship->GetPos(), seaidswithattackers, playerId);
        if(!testseaidswithattackers.empty()) // only do the final check if it will probably be a good result
        {
            std::vector<GameWorldBase::PotentialSeaAttacker> attackers =
              gwb.GetSoldiersForSeaAttack(playerId, ship->GetPos()); // now get a final list of attackers and attack it
            if(!attackers.empty())
            {
                aii.SeaAttack(ship->GetPos(), attackers.size(), true);
                return;
            }
        }
    }
}

void AIPlayerJH::RecalcGround(const MapPoint buildingPos, std::vector<Direction>& route_road)
{
    // building itself
    if(aiMap[buildingPos].res == AIResource::Plantspace)
        aiMap[buildingPos].res = AINodeResource::Nothing;

    // flag of building
    const MapPoint flagPos = gwb.GetNeighbour(buildingPos, Direction::SouthEast);
    if(aiMap[flagPos].res == AIResource::Plantspace)
        aiMap[flagPos].res = AINodeResource::Nothing;

    // along the road
    MapPoint curPt = flagPos;
    for(auto i : route_road)
    {
        curPt = gwb.GetNeighbour(curPt, i);
        if(aiMap[curPt].res == AIResource::Plantspace)
            aiMap[curPt].res = AINodeResource::Nothing;
    }
}

void AIPlayerJH::SaveResourceMapsToFile()
{
    for(const auto res : helpers::enumRange<AIResource>())
    {
        bfs::ofstream file("resmap-" + std::to_string(static_cast<unsigned>(res)) + ".log");
        for(unsigned y = 0; y < aiMap.GetHeight(); ++y)
        {
            if(y % 2 == 1)
                file << "  ";
            for(unsigned x = 0; x < aiMap.GetWidth(); ++x)
                file << resourceMaps[res][MapPoint(x, y)] << "   ";
            file << "\n";
        }
    }
}

int AIPlayerJH::GetResMapValue(const MapPoint pt, AIResource res) const
{
    return GetResMap(res)[pt];
}

const AIResourceMap& AIPlayerJH::GetResMap(AIResource res) const
{
    return resourceMaps[res];
}

void AIPlayerJH::SendAIEvent(std::unique_ptr<AIEvent::Base> ev)
{
    eventManager.AddAIEvent(std::move(ev));
}

bool AIPlayerJH::IsFlagPartofCircle(const noFlag& startFlag, unsigned maxlen, const noFlag& curFlag,
                                    helpers::OptionalEnum<Direction> excludeDir, std::vector<const noFlag*> oldFlags)
{
    // If oldFlags is empty we just started
    if(!oldFlags.empty() && &startFlag == &curFlag)
        return true;
    if(maxlen < 1)
        return false;
    for(Direction testDir : helpers::EnumRange<Direction>{})
    {
        if(testDir == excludeDir)
            continue;
        if(testDir == Direction::NorthWest
           && (aii.IsObjectTypeOnNode(gwb.GetNeighbour(curFlag.GetPos(), Direction::NorthWest),
                                      NodalObjectType::Building)
               || aii.IsObjectTypeOnNode(gwb.GetNeighbour(curFlag.GetPos(), Direction::NorthWest),
                                         NodalObjectType::Buildingsite)))
        {
            continue;
        }
        const RoadSegment* route = curFlag.GetRoute(testDir);
        if(route)
        {
            const noFlag& flag = route->GetOtherFlag(curFlag);
            if(!helpers::contains(oldFlags, &flag))
            {
                oldFlags.push_back(&flag);
                Direction revDir = route->GetOtherFlagDir(curFlag) + 3u;
                if(IsFlagPartofCircle(startFlag, maxlen - 1, flag, revDir, oldFlags))
                    return true;
            }
        }
    }
    return false;
}

void AIPlayerJH::RemoveAllUnusedRoads(const MapPoint pt)
{
    std::vector<const noFlag*> flags = construction->FindFlags(pt, 25);
    // Jede Flagge testen...
    std::vector<const noFlag*> reconnectflags;
    for(const noFlag* flag : flags)
    {
        if(RemoveUnusedRoad(*flag, boost::none, true, false))
            reconnectflags.push_back(flag);
    }
    UpdateNodesAround(pt, 25);
    for(const noFlag* flag : reconnectflags)
        construction->AddConnectFlagJob(flag);
}

void AIPlayerJH::CheckForUnconnectedBuildingSites()
{
    if(construction->GetConnectJobNum() > 0 || construction->GetBuildJobNum() > 0)
        return;
    for(noBuildingSite* bldSite : player.GetBuildingRegister().GetBuildingSites()) //-V807
    {
        noFlag* flag = bldSite->GetFlag();
        bool foundRoute = false;
        for(const auto dir : helpers::EnumRange<Direction>{})
        {
            if(dir == Direction::NorthWest)
                continue;
            if(flag->GetRoute(dir))
            {
                foundRoute = true;
                break;
            }
        }
        if(!foundRoute)
            construction->AddConnectFlagJob(flag);
    }
}

bool AIPlayerJH::RemoveUnusedRoad(const noFlag& startFlag, helpers::OptionalEnum<Direction> excludeDir,
                                  bool firstflag /*= true*/, bool allowcircle /*= true*/,
                                  bool keepstartflag /*= false*/)
{
    helpers::OptionalEnum<Direction> foundDir, foundDir2;
    unsigned char finds = 0;
    // Count roads from this flag...
    for(Direction dir : helpers::EnumRange<Direction>{})
    {
        if(dir == excludeDir)
            continue;
        if(dir == Direction::NorthWest
           && (aii.IsObjectTypeOnNode(gwb.GetNeighbour(startFlag.GetPos(), Direction::NorthWest),
                                      NodalObjectType::Building)
               || aii.IsObjectTypeOnNode(gwb.GetNeighbour(startFlag.GetPos(), Direction::NorthWest),
                                         NodalObjectType::Buildingsite)))
        {
            // the flag belongs to a building - update the pathing map around us and try to reconnect it (if we cant
            // reconnect it -> burn it(burning takes place at the pathfinding job))
            return true;
        }
        if(startFlag.GetRoute(dir))
        {
            finds++;
            if(finds == 1)
                foundDir = dir;
            else if(finds == 2)
                foundDir2 = dir;
        }
    }
    // if we found more than 1 road -> the flag is still in use.
    if(finds > 2)
        return false;
    else if(finds == 2)
    {
        if(allowcircle)
        {
            if(!IsFlagPartofCircle(startFlag, 10, startFlag, boost::none, {}))
                return false;
            if(!firstflag)
                return false;
        } else
            return false;
    }

    // kill the flag
    if(keepstartflag)
    {
        if(foundDir)
            aii.DestroyRoad(startFlag.GetPos(), *foundDir);
    } else
        aii.DestroyFlag(&startFlag);

    // nothing found?
    if(!foundDir)
        return false;
    // at least 1 road exists
    Direction revDir1 = startFlag.GetRoute(*foundDir)->GetOtherFlagDir(startFlag) + 3u;
    RemoveUnusedRoad(startFlag.GetRoute(*foundDir)->GetOtherFlag(startFlag), revDir1, false);
    // 2 roads exist
    if(foundDir2)
    {
        Direction revDir2 = startFlag.GetRoute(*foundDir2)->GetOtherFlagDir(startFlag) + 3u;
        RemoveUnusedRoad(startFlag.GetRoute(*foundDir2)->GetOtherFlag(startFlag), revDir2, false);
    }
    return false;
}

unsigned AIPlayerJH::SoldierAvailable(int rank)
{
    unsigned freeSoldiers = 0;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
    {
        const Inventory& inventory = wh->GetInventory();
        if(rank < 0)
        {
            for(const Job job : SOLDIER_JOBS)
                freeSoldiers += inventory[job];
        } else
            freeSoldiers += inventory[SOLDIER_JOBS[rank]];
    }
    return freeSoldiers;
}

bool AIPlayerJH::HuntablesinRange(const MapPoint pt, unsigned min)
{
    // check first if no other hunter(or hunter buildingsite) is nearby
    if(aii.isBuildingNearby(BuildingType::Hunter, pt, 14))
        return false;
    unsigned maxrange = 25;
    unsigned short fx, fy, lx, ly;
    const unsigned short SQUARE_SIZE = 19;
    unsigned huntablecount = 0;
    if(pt.x > SQUARE_SIZE)
        fx = pt.x - SQUARE_SIZE;
    else
        fx = 0;
    if(pt.y > SQUARE_SIZE)
        fy = pt.y - SQUARE_SIZE;
    else
        fy = 0;
    if(pt.x + SQUARE_SIZE < gwb.GetWidth())
        lx = pt.x + SQUARE_SIZE;
    else
        lx = gwb.GetWidth() - 1;
    if(pt.y + SQUARE_SIZE < gwb.GetHeight())
        ly = pt.y + SQUARE_SIZE;
    else
        ly = gwb.GetHeight() - 1;
    // Durchgehen und nach Tieren suchen
    for(MapPoint p2(0, fy); p2.y <= ly; ++p2.y)
    {
        for(p2.x = fx; p2.x <= lx; ++p2.x)
        {
            // Search for animals
            for(const noBase& fig : gwb.GetFigures(p2))
            {
                if(fig.GetType() == NodalObjectType::Animal)
                {
                    // Ist das Tier überhaupt zum Jagen geeignet?
                    if(!static_cast<const noAnimal&>(fig).CanHunted())
                        continue;
                    // Und komme ich hin?
                    if(gwb.FindHumanPath(pt, static_cast<const noAnimal&>(fig).GetPos(), maxrange))
                    // Dann nehmen wir es
                    {
                        if(++huntablecount >= min)
                            return true;
                    }
                }
            }
        }
    }
    return false;
}

void AIPlayerJH::InitStoreAndMilitarylists()
{
    for(const nobUsual* farm : aii.GetBuildings(BuildingType::Farm))
    {
        SetFarmedNodes(farm->GetPos(), true);
    }
    for(const nobUsual* charburner : aii.GetBuildings(BuildingType::Charburner))
    {
        SetFarmedNodes(charburner->GetPos(), true);
    }
    // find the upgradebuilding
    UpdateUpgradeBuilding();
}
int AIPlayerJH::UpdateUpgradeBuilding()
{
    std::vector<const nobMilitary*> backup;
    if(!aii.GetStorehouses().empty())
    {
        unsigned count = 0;
        for(const nobMilitary* milBld : aii.GetMilitaryBuildings())
        {
            // inland building, tower or fortress
            BuildingType bld = milBld->GetBuildingType();
            if((bld == BuildingType::Watchtower || bld == BuildingType::Fortress)
               && milBld->GetFrontierDistance() == FrontierDistance::Far)
            {
                if(construction->IsConnectedToRoadSystem(milBld->GetFlag()))
                {
                    // LOG.write(("UpdateUpgradeBuilding at %i,%i for player %i (listslot %i) \n",itObj->GetX(),
                    // itObj->GetY(), playerId, count);
                    UpgradeBldPos = milBld->GetPos();
                    return count;
                }
                backup.push_back(milBld);
            }
            count++;
        }
    }
    // no valid upgrade building yet - try to reconnect correctly flagged buildings
    for(const nobMilitary* milBld : backup)
    {
        construction->AddConnectFlagJob(milBld->GetFlag());
    }
    UpgradeBldPos = MapPoint::Invalid();
    return -1;
}
// set default start values for the ai for distribution & military settings
void AIPlayerJH::InitDistribution()
{
    // set good distribution settings
    Distributions goodSettings;
    goodSettings[0] = 10; // food granite
    goodSettings[1] = 10; // food coal
    goodSettings[2] = 10; // food iron
    goodSettings[3] = 10; // food gold

    goodSettings[4] = 10; // grain mill
    goodSettings[5] = 10; // grain pigfarm
    goodSettings[6] = 10; // grain donkeybreeder
    goodSettings[7] = 10; // grain brewery
    goodSettings[8] = 10; // grain charburner

    goodSettings[9] = 10;  // iron armory
    goodSettings[10] = 10; // iron metalworks

    goodSettings[11] = 10; // coal armory
    goodSettings[12] = 10; // coal ironsmelter
    goodSettings[13] = 10; // coal mint

    goodSettings[14] = 10; // wood sawmill
    goodSettings[15] = 10; // wood charburner

    goodSettings[16] = 10; // boards new buildings
    goodSettings[17] = 4;  // boards metalworks
    goodSettings[18] = 2;  // boards shipyard

    goodSettings[19] = 10; // water bakery
    goodSettings[20] = 10; // water brewery
    goodSettings[21] = 10; // water pigfarm
    goodSettings[22] = 10; // water donkeybreeder
    aii.ChangeDistribution(goodSettings);
}

bool AIPlayerJH::ValidTreeinRange(const MapPoint pt)
{
    unsigned max_radius = 6;
    for(MapCoord tx = gwb.GetXA(pt, Direction::West), r = 1; r <= max_radius;
        tx = gwb.GetXA(MapPoint(tx, pt.y), Direction::West), ++r)
    {
        MapPoint t2(tx, pt.y);
        for(unsigned i = 2; i < 8; ++i)
        {
            for(MapCoord r2 = 0; r2 < r; t2 = gwb.GetNeighbour(t2, convertToDirection(i)), ++r2)
            {
                // point has tree & path is available?
                if(gwb.GetNO(t2)->GetType() == NodalObjectType::Tree)
                {
                    // not already getting cut down or a freaking pineapple thingy?
                    if(!gwb.GetNode(t2).reserved && gwb.GetSpecObj<noTree>(t2)->ProducesWood())
                    {
                        if(gwb.FindHumanPath(pt, t2, 20))
                            return true;
                    }
                }
            }
        }
    }
    return false;
}

bool AIPlayerJH::ValidStoneinRange(const MapPoint pt)
{
    unsigned max_radius = 8;
    for(MapCoord tx = gwb.GetXA(pt, Direction::West), r = 1; r <= max_radius;
        tx = gwb.GetXA(MapPoint(tx, pt.y), Direction::West), ++r)
    {
        MapPoint t2(tx, pt.y);
        for(unsigned i = 2; i < 8; ++i)
        {
            for(MapCoord r2 = 0; r2 < r; t2 = gwb.GetNeighbour(t2, convertToDirection(i)), ++r2)
            {
                // point has tree & path is available?
                if(gwb.GetNO(t2)->GetType() == NodalObjectType::Granite)
                {
                    if(gwb.FindHumanPath(pt, t2, 20))
                        return true;
                }
            }
        }
    }
    return false;
}

void AIPlayerJH::ExecuteLuaConstructionOrder(const MapPoint pt, BuildingType bt, bool forced)
{
    if(!aii.CanBuildBuildingtype(bt)) // not allowed to build this buildingtype? -> do nothing!
        return;
    if(forced) // fixed location - just a direct gamecommand to build buildingtype at location (no checks if this is a
               // valid & good location from the ai)
    {
        aii.SetBuildingSite(pt, bt);
        auto j = std::make_unique<BuildJob>(*this, bt, pt);
        j->SetState(JobState::ExecutingRoad1);
        j->SetTarget(pt);
        construction->AddBuildJob(std::move(j), true); // connects the buildingsite to roadsystem
    } else
    {
        if(construction->Wanted(bt))
        {
            construction->AddBuildJob(std::make_unique<BuildJob>(*this, bt, pt),
                                      true); // add build job to the front of the list
        }
    }
}

/// returns the percentage*100 of possible normal+ building places
unsigned AIPlayerJH::BQsurroundcheck(const MapPoint pt, unsigned range, bool includeexisting, unsigned limit)
{
    unsigned maxvalue = 6 * (2 << (range - 1)) - 5; // 1,7,19,43,91,... = 6*2^range -5
    unsigned count = 0;
    RTTR_Assert(aii.GetBuildingQuality(pt) == GetAINode(pt).bq);
    if((aii.GetBuildingQuality(pt) >= BuildingQuality::Hut && aii.GetBuildingQuality(pt) <= BuildingQuality::Castle)
       || aii.GetBuildingQuality(pt) == BuildingQuality::Harbor)
    {
        count++;
    }
    NodalObjectType nob = gwb.GetNO(pt)->GetType();
    if(includeexisting)
    {
        if(nob == NodalObjectType::Building || nob == NodalObjectType::Buildingsite || nob == NodalObjectType::Extension
           || nob == NodalObjectType::Fire || nob == NodalObjectType::CharburnerPile)
            count++;
    }
    // first count all the possible building places
    for(MapCoord tx = gwb.GetXA(pt, Direction::West), r = 1; r <= range;
        tx = gwb.GetXA(MapPoint(tx, pt.y), Direction::West), ++r)
    {
        MapPoint t2(tx, pt.y);
        for(unsigned i = 2; i < 8; ++i)
        {
            for(MapCoord r2 = 0; r2 < r; t2 = gwb.GetNeighbour(t2, convertToDirection(i)), ++r2)
            {
                if(limit && ((count * 100) / maxvalue) > limit)
                    return ((count * 100) / maxvalue);
                // point can be used for a building
                if((aii.GetBuildingQualityAnyOwner(t2) >= BuildingQuality::Hut
                    && aii.GetBuildingQualityAnyOwner(t2) <= BuildingQuality::Castle)
                   || aii.GetBuildingQualityAnyOwner(t2) == BuildingQuality::Harbor)
                {
                    count++;
                    continue;
                }
                if(includeexisting)
                {
                    nob = gwb.GetNO(t2)->GetType();
                    if(nob == NodalObjectType::Building || nob == NodalObjectType::Buildingsite
                       || nob == NodalObjectType::Extension || nob == NodalObjectType::Fire
                       || nob == NodalObjectType::CharburnerPile)
                        count++;
                }
            }
        }
    }
    // LOG.write(("bqcheck at %i,%i r%u result: %u,%u \n",pt,range,count,maxvalue);
    return ((count * 100) / maxvalue);
}

bool AIPlayerJH::HarborPosRelevant(unsigned harborid, bool onlyempty) const
{
    if(harborid < 1 || harborid > gwb.GetNumHarborPoints()) // not a real harbor - shouldnt happen...
    {
        RTTR_Assert(false);
        return false;
    }
    if(!onlyempty)
        return helpers::contains(aii.getUsableHarbors(), harborid);

    for(const auto dir : helpers::EnumRange<Direction>{})
    {
        const unsigned short seaId = gwb.GetSeaId(harborid, dir);
        if(!seaId)
            continue;

        for(unsigned curHarborId = 1; curHarborId <= gwb.GetNumHarborPoints();
            curHarborId++) // start at 1 harbor dummy yadayada :>
        {
            if(curHarborId != harborid && gwb.IsHarborAtSea(curHarborId, seaId))
            {
                // check if the spot is actually free for colonization?
                if(gwb.IsHarborPointFree(curHarborId, playerId))
                    return true;
            }
        }
    }
    return false;
}

bool AIPlayerJH::NoEnemyHarbor()
{
    for(unsigned i = 1; i <= gwb.GetNumHarborPoints(); i++)
    {
        if(aii.IsBuildingOnNode(gwb.GetHarborPoint(i), BuildingType::HarborBuilding)
           && !aii.IsOwnTerritory(gwb.GetHarborPoint(i)))
        {
            // LOG.write(("found a harbor at spot %i ",i);
            return false;
        }
    }
    return true;
}

bool AIPlayerJH::IsInvalidShipyardPosition(const MapPoint pt)
{
    return aii.isBuildingNearby(BuildingType::Shipyard, pt, 19) || !aii.isHarborPosClose(pt, 7);
}

unsigned AIPlayerJH::AmountInStorage(GoodType good) const
{
    unsigned counter = 0;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        counter += wh->GetInventory().goods[good];
    return counter;
}

unsigned AIPlayerJH::AmountInStorage(::Job job) const
{
    unsigned counter = 0;
    for(const nobBaseWarehouse* wh : aii.GetStorehouses())
        counter += wh->GetInventory().people[job];
    return counter;
}

bool AIPlayerJH::ValidFishInRange(const MapPoint pt)
{
    unsigned max_radius = 5;
    return gwb.CheckPointsInRadius(
      pt, max_radius,
      [this, pt](const MapPoint curPt, unsigned) {
          if(gwb.GetNode(curPt).resources.has(ResourceType::Fish)) // fish on current spot?
          {
              // try to find a path to a neighboring node on the coast
              for(const MapPoint nb : gwb.GetNeighbours(curPt))
              {
                  if(gwb.FindHumanPath(pt, nb, 10))
                      return true;
              }
          }
          return false;
      },
      false);
}

unsigned AIPlayerJH::GetNumAIRelevantSeaIds() const
{
    std::vector<unsigned short> validseaids;
    std::list<unsigned short> onetimeuseseaids;
    for(unsigned i = 1; i <= gwb.GetNumHarborPoints(); i++)
    {
        for(const auto dir : helpers::EnumRange<Direction>{})
        {
            const unsigned short seaId = gwb.GetSeaId(i, dir);
            if(!seaId)
                continue;
            // there is a sea id? -> check if it is already a validid or a once found id
            if(!helpers::contains(validseaids, seaId)) // not yet in validseas?
            {
                if(!helpers::contains(onetimeuseseaids, seaId)) // not yet in onetimeuseseaids?
                    onetimeuseseaids.push_back(seaId);
                else
                {
                    // LOG.write(("found a second harbor at sea id %i \n",seaIds[r]);
                    onetimeuseseaids.remove(seaId);
                    validseaids.push_back(seaId);
                }
            }
        }
    }
    return validseaids.size();
}

void AIPlayerJH::AdjustSettings()
{
    const Inventory& inventory = aii.GetInventory();
    // update tool creation settings
    if(bldPlanner->GetNumBuildings(BuildingType::Metalworks) > 0u)
    {
        ToolSettings toolsettings{};
        const auto calcToolPriority = [&](const Tool tool) {
            const GoodType good = TOOL_TO_GOOD[tool];
            unsigned numToolsAvailable = inventory[good];
            // Find missing jobs for buildings
            for(const auto job : helpers::enumRange<Job>())
            {
                if(JOB_CONSTS[job].tool != good)
                    continue;
                unsigned numBuildingsRequiringWorker = 0;
                for(const auto bld : helpers::enumRange<BuildingType>())
                {
                    if(BLD_WORK_DESC[bld].job == job)
                        numBuildingsRequiringWorker += bldPlanner->GetNumBuildings(bld);
                }
                if(numBuildingsRequiringWorker > inventory[job])
                {
                    const unsigned requiredTools = numBuildingsRequiringWorker - inventory[job];
                    // When we are missing tools produce some.
                    // Slightly higher priority if we don't have any tool at all.
                    if(requiredTools > numToolsAvailable)
                        return (inventory[good] == 0) ? 4 : 2;
                    numToolsAvailable -= requiredTools;
                }
            }
            return 0;
        };
        // Basic tools to produce stone, boards and iron are very important to have, do those first
        for(const Tool tool : {Tool::Axe, Tool::Saw, Tool::PickAxe, Tool::Crucible})
            toolsettings[tool] = calcToolPriority(tool);
        // Set some minima
        if(inventory[GoodType::Saw] + inventory[Job::Carpenter] < 2)
            toolsettings[Tool::Saw] = 10;
        if(inventory[GoodType::Axe] + inventory[Job::Woodcutter] < 2)
            toolsettings[Tool::Axe] = 10;
        if(inventory[GoodType::PickAxe] + inventory[Job::Stonemason] < 2)
            toolsettings[Tool::PickAxe] = 7;
        // Only if we haven't ordered any basic tool, we may order other tools
        if(toolsettings[Tool::Axe] == 0 && toolsettings[Tool::PickAxe] == 0 && toolsettings[Tool::Saw] == 0
           && toolsettings[Tool::Crucible] == 0)
        {
            // Order those as required for existing and planned buildings
            for(const Tool tool : {Tool::Hammer, Tool::Scythe, Tool::Rollingpin, Tool::Shovel, Tool::Tongs,
                                   Tool::Cleaver, Tool::RodAndLine, Tool::Bow})
            {
                toolsettings[tool] = calcToolPriority(tool);
            }
            // Always have at least one of those in stock for other stuff
            for(const Tool tool : {Tool::Hammer, Tool::Shovel, Tool::Tongs})
            {
                if(inventory[TOOL_TO_GOOD[tool]] == 0)
                    toolsettings[tool] = std::max<unsigned>(toolsettings[tool], 1u);
            }
            // We want about 12 woodcutters, so if we don't have axes produce some
            if(inventory[GoodType::Axe] == 0 && inventory[Job::Woodcutter] < 12)
            {
                // Higher priority if we can't meet the building requirements as calculated above
                toolsettings[Tool::Axe] = (toolsettings[Tool::Axe] == 0) ? 4 : 7;
            }
        }

        for(const auto tool : helpers::enumRange<Tool>())
        {
            if(toolsettings[tool] != player.GetToolPriority(tool))
            {
                aii.ChangeTools(toolsettings);
                break;
            }
        }
    }

    // Set military settings to some currently required values
    MilitarySettings milSettings;
    milSettings[0] = 10;
    milSettings[1] = HasFrontierBuildings() ?
                       5 :
                       0; // if we have a front send strong soldiers first else weak first to make upgrading easier
    milSettings[2] = 4;
    milSettings[3] = 5;
    // interior 0bar full if we have an upgrade building and gold(or produce gold) else 1 soldier each
    milSettings[4] = UpdateUpgradeBuilding() >= 0
                         && (inventory[GoodType::Coins] > 0
                             || (inventory[GoodType::Gold] > 0 && inventory[GoodType::Coal] > 0
                                 && !aii.GetBuildings(BuildingType::Mint).empty())) ?
                       8 :
                       0;
    milSettings[6] =
      ggs.isEnabled(AddonId::SEA_ATTACK) ? 8 : 0; // harbor flag: no sea attacks?->no soldiers else 50% to 100%
    milSettings[5] = CalcMilSettings(); // inland 1bar min 50% max 100% depending on how many soldiers are available
    milSettings[7] = 8;                 // front: 100%
    if(player.GetMilitarySetting(5) != milSettings[5] || player.GetMilitarySetting(6) != milSettings[6]
       || player.GetMilitarySetting(4) != milSettings[4]
       || player.GetMilitarySetting(1) != milSettings[1]) // only send the command if we want to change something
        aii.ChangeMilitary(milSettings);
}

unsigned AIPlayerJH::CalcMilSettings()
{
    std::array<unsigned, 5> InlandTroops = {
      0, 0, 0, 0, 0}; // how many troops are required to fill inland buildings at settings 4,5,6,7,8
    /// first sum up all soldiers we have
    unsigned numSoldiers = 0;
    for(auto i : SOLDIER_JOBS)
        numSoldiers += aii.GetInventory().people[i];

    // now add up all counts of soldiers that are fixed in use and those that depend on whatever we have as a result
    const unsigned numShouldStayConnected = GetNumPlannedConnectedInlandMilitaryBlds();
    int count = 0;
    unsigned soldierInUseFixed = 0;
    const int uun = UpdateUpgradeBuilding();
    const std::list<nobMilitary*>& militaryBuildings = aii.GetMilitaryBuildings();
    for(const nobMilitary* milBld : militaryBuildings)
    {
        if(milBld->GetFrontierDistance() == FrontierDistance::Near
           || milBld->GetFrontierDistance() == FrontierDistance::Harbor
           || (milBld->GetFrontierDistance() == FrontierDistance::Far
               && (militaryBuildings.size() < (unsigned)count + numShouldStayConnected
                   || count == uun))) // front or connected interior
        {
            soldierInUseFixed += milBld->CalcRequiredNumTroops(FrontierDistance::Mid, 8);
        } else if(milBld->GetFrontierDistance() == FrontierDistance::Mid) // 1 bar (inland)
        {
            for(int i = 0; i < 5; i++)
                InlandTroops[i] += milBld->CalcRequiredNumTroops(FrontierDistance::Mid, 4 + i);
        } else // setting should be 0 so add 1 soldier
            soldierInUseFixed++;

        count++;
    }

    // now the current need total and for inland and harbor is ready for use
    unsigned returnValue = 8;
    while(returnValue > 4)
    {
        // have more than enough soldiers for this setting or just enough and this is the current setting? -> return it
        // else try the next lower setting down to 4 (50%)
        if(soldierInUseFixed + InlandTroops[returnValue - 4] < numSoldiers * 10 / 11
           || (player.GetMilitarySetting(5) >= returnValue
               && soldierInUseFixed + InlandTroops[returnValue - 4] < numSoldiers))
            break;
        returnValue--;
    }
    // LOG.write(("player %i inland milsetting %i \n",playerId,returnvalue);
    return returnValue;
}

} // namespace AIJH
