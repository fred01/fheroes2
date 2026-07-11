/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2025                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

// This tool converts a map stored in one of the legacy formats readable by the engine
// (Heroes of Might and Magic II "The Succession Wars" .mp2 / .h2c and "The Price of Loyalty"
// .mx2 / .hxc) to the modern fheroes2 map format (.fh2m) that the in-game Editor uses.
//
// The legacy map is loaded by the engine itself, so all the object data the engine understands
// (castles, heroes, signs, events, riddles, rumors, captured mines and so on) is available and
// gets transferred into the produced map. The map is then reconstructed in the same way the
// Editor does it, which guarantees that the resulting file can be opened both in the game and
// in the Editor.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "army.h"
#include "army_troop.h"
#include "artifact.h"
#include "castle.h"
#include "color.h"
#include "heroes.h"
#include "map_format_helper.h"
#include "map_format_info.h"
#include "map_object_info.h"
#include "maps.h"
#include "maps_fileinfo.h"
#include "maps_objects.h"
#include "maps_tiles.h"
#include "maps_tiles_helper.h"
#include "mp2.h"
#include "mp2_helper.h"
#include "players.h"
#include "race.h"
#include "resource.h"
#include "serialize.h"
#include "settings.h"
#include "skill.h"
#include "system.h"
#include "world.h"

namespace
{
    // A legacy map stores an object as a set of individual parts scattered across the tiles that
    // the object occupies. Every part is a 6-bit ICN identifier plus an 8-bit image index. Both
    // values are combined into a single key that is used to look up the modern object this part
    // may belong to.
    uint32_t makePartKey( const MP2::ObjectIcnType icnType, const uint32_t icnIndex )
    {
        return ( static_cast<uint32_t>( icnType ) << 16 ) | ( icnIndex & 0xFFFF );
    }

    struct ObjectLocation
    {
        Maps::ObjectGroup group{ Maps::ObjectGroup::NONE };
        uint32_t index{ 0 };

        // The offset of the part this location was found by, relative to the anchor tile of the object.
        fheroes2::Point partOffset;
    };

    struct PlacedObject
    {
        uint32_t uid{ 0 };
        Maps::ObjectGroup group{ Maps::ObjectGroup::NONE };
        uint32_t index{ 0 };
        int32_t tileIndex{ 0 };
    };

    // An object part of a tile, in the order the engine renders it.
    struct OrderedPart
    {
        uint32_t uid{ 0 };
        Maps::ObjectLayerType layerType{ Maps::OBJECT_LAYER };
        bool isTopLayer{ false };
    };

    // A single object part as it is stored in the legacy map.
    struct LegacyPart
    {
        int32_t tileIndex{ 0 };
        MP2::ObjectIcnType icnType{ MP2::OBJ_ICN_TYPE_UNKNOWN };
        uint8_t icnIndex{ 0 };
        bool isTopLayer{ false };

        bool operator<( const LegacyPart & other ) const
        {
            if ( tileIndex != other.tileIndex ) {
                return tileIndex < other.tileIndex;
            }
            if ( icnType != other.icnType ) {
                return icnType < other.icnType;
            }
            if ( icnIndex != other.icnIndex ) {
                return icnIndex < other.icnIndex;
            }
            return isTopLayer < other.isTopLayer;
        }
    };

    // Maps every part of every object the engine knows to the object it belongs to. An object of a
    // legacy map is looked up by its parts, and any of them will do: an object hanging over the edge
    // of the map has some of its parts, possibly even the main one, cut off.
    std::map<uint32_t, std::vector<ObjectLocation>> buildObjectLookupTable()
    {
        std::map<uint32_t, std::vector<ObjectLocation>> lookup;

        for ( size_t groupId = static_cast<size_t>( Maps::ObjectGroup::ROADS ); groupId < static_cast<size_t>( Maps::ObjectGroup::GROUP_COUNT ); ++groupId ) {
            const auto group = static_cast<Maps::ObjectGroup>( groupId );
            const std::vector<Maps::ObjectInfo> & objects = Maps::getObjectsByGroup( group );

            for ( size_t index = 0; index < objects.size(); ++index ) {
                const auto addPart = [&lookup, group, index]( const Maps::ObjectPartInfo & part ) {
                    if ( part.icnType == MP2::OBJ_ICN_TYPE_UNKNOWN || part.icnIndex > 255 ) {
                        // Such a part can never be encoded in the legacy map format.
                        return;
                    }

                    lookup[makePartKey( part.icnType, part.icnIndex )].push_back( { group, static_cast<uint32_t>( index ), part.tileOffset } );
                };

                for ( const auto & part : objects[index].groundLevelParts ) {
                    addPart( part );
                }

                for ( const auto & part : objects[index].topLevelParts ) {
                    addPart( part );
                }
            }
        }

        return lookup;
    }

    // An object may hang over the edge of the map, and legacy maps do make use of this. Only an
    // action object has to fit into the map entirely, since a part of it that cannot be rendered
    // would still affect the passability. This repeats the check made by Maps::setObjectOnTile().
    bool doesObjectFitTheMap( const Maps::ObjectInfo & info, const fheroes2::Point & mainTilePos )
    {
        if ( !MP2::isOffGameActionObject( info.objectType ) ) {
            return true;
        }

        for ( const auto & part : info.groundLevelParts ) {
            if ( part.layerType == Maps::SHADOW_LAYER || part.layerType == Maps::TERRAIN_LAYER ) {
                // Shadows and terrain parts are allowed to be outside of the map.
                continue;
            }

            const fheroes2::Point pos = mainTilePos + part.tileOffset;
            if ( !Maps::isValidAbsPoint( pos.x, pos.y ) ) {
                return false;
            }
        }

        for ( const auto & part : info.topLevelParts ) {
            const fheroes2::Point pos = mainTilePos + part.tileOffset;
            if ( !Maps::isValidAbsPoint( pos.x, pos.y ) ) {
                return false;
            }
        }

        return true;
    }

    struct PartMatch
    {
        std::set<LegacyPart> parts;

        // The number of the parts whose layer matches the one of the legacy map as well. An object
        // and the shadow of another object may share the same image, and only the layer tells them
        // apart.
        size_t sameLayerCount{ 0 };

        // Whether the object is of the very type the legacy map gives to its anchor tile.
        bool isOfTheSameObjectType{ false };
    };

    // Returns the parts of the legacy object that a modern object placed on the given tile would
    // reproduce. The more parts match, the more certain we are that this is the object stored in the
    // legacy map, since different objects may well share the same main part.
    PartMatch getMatchingParts( const Maps::ObjectInfo & info, const fheroes2::Point & mainTilePos, const std::set<LegacyPart> & legacyParts,
                                const std::map<LegacyPart, Maps::ObjectLayerType> & partLayers, const int32_t mapWidth )
    {
        PartMatch match;

        const auto matchPart = [&]( const fheroes2::Point & offset, const MP2::ObjectIcnType icnType, const uint32_t icnIndex, const bool isTopLayer,
                                    const Maps::ObjectLayerType layerType ) {
            const fheroes2::Point pos = mainTilePos + offset;
            if ( !Maps::isValidAbsPoint( pos.x, pos.y ) || icnIndex > 255 ) {
                return;
            }

            const LegacyPart part{ pos.y * mapWidth + pos.x, icnType, static_cast<uint8_t>( icnIndex ), isTopLayer };
            if ( legacyParts.find( part ) == legacyParts.end() ) {
                return;
            }

            match.parts.insert( part );

            if ( const auto layerIter = partLayers.find( part ); layerIter != partLayers.end() && layerIter->second == layerType ) {
                ++match.sameLayerCount;
            }
        };

        for ( const auto & part : info.groundLevelParts ) {
            matchPart( part.tileOffset, part.icnType, part.icnIndex, false, part.layerType );
        }

        for ( const auto & part : info.topLevelParts ) {
            matchPart( part.tileOffset, part.icnType, part.icnIndex, true, Maps::OBJECT_LAYER );
        }

        return match;
    }

    void fillCastleMetadata( const Castle & castle, Maps::Map_Format::CastleMetadata & metadata )
    {
        metadata.customName = castle.GetName();

        const Army & army = castle.GetArmy();
        for ( size_t i = 0; i < metadata.defenderMonsterType.size(); ++i ) {
            const Troop * troop = army.GetTroop( i );
            if ( troop != nullptr && troop->isValid() ) {
                metadata.defenderMonsterType[i] = troop->GetID();
                metadata.defenderMonsterCount[i] = static_cast<int32_t>( troop->GetCount() );
            }
        }

        // The legacy format stores the exact set of buildings of a castle, so it is transferred as is.
        static const std::array<uint32_t, 26> allBuildings{ BUILD_THIEVESGUILD,
                                                            BUILD_TAVERN,
                                                            BUILD_SHIPYARD,
                                                            BUILD_WELL,
                                                            BUILD_STATUE,
                                                            BUILD_LEFTTURRET,
                                                            BUILD_RIGHTTURRET,
                                                            BUILD_MARKETPLACE,
                                                            BUILD_WEL2,
                                                            BUILD_MOAT,
                                                            BUILD_SPEC,
                                                            BUILD_CASTLE,
                                                            BUILD_CAPTAIN,
                                                            BUILD_SHRINE,
                                                            BUILD_MAGEGUILD1,
                                                            BUILD_MAGEGUILD2,
                                                            BUILD_MAGEGUILD3,
                                                            BUILD_MAGEGUILD4,
                                                            BUILD_MAGEGUILD5,
                                                            BUILD_TENT,
                                                            DWELLING_MONSTER1,
                                                            DWELLING_MONSTER2,
                                                            DWELLING_MONSTER3,
                                                            DWELLING_MONSTER4,
                                                            DWELLING_MONSTER5,
                                                            DWELLING_MONSTER6 };

        static const std::array<uint32_t, 6> upgradedDwellings{ DWELLING_UPGRADE2, DWELLING_UPGRADE3, DWELLING_UPGRADE4,
                                                                DWELLING_UPGRADE5, DWELLING_UPGRADE6, DWELLING_UPGRADE7 };

        metadata.customBuildings = true;

        for ( const uint32_t building : allBuildings ) {
            if ( castle.isBuild( building ) ) {
                metadata.builtBuildings.push_back( building );
            }
        }

        for ( const uint32_t building : upgradedDwellings ) {
            if ( castle.isBuild( building ) ) {
                metadata.builtBuildings.push_back( building );
            }
        }
    }

    // The engine cross-checks the buildings of a castle against the object placed on the map, so a
    // castle must have its keep and a town must have its tent.
    void syncCastleAndTentBuildings( Maps::Map_Format::CastleMetadata & metadata, const bool isCastle )
    {
        auto & buildings = metadata.builtBuildings;

        buildings.erase( std::remove_if( buildings.begin(), buildings.end(), []( const uint32_t building ) { return building == BUILD_CASTLE || building == BUILD_TENT; } ),
                         buildings.end() );

        buildings.push_back( isCastle ? BUILD_CASTLE : BUILD_TENT );

        metadata.customBuildings = true;
    }

    void fillCapturableMetadata( Maps::Map_Format::MapFormat & map, const uint32_t uid, const int32_t tileIndex, const MP2::MapObjectType objectType )
    {
        if ( !Maps::isCapturableObject( objectType ) ) {
            return;
        }

        map.capturableObjectsMetadata[uid].ownerColor = world.ColorCapturedObject( tileIndex );
    }

    void fillEventMetadata( const MapEvent & event, Maps::Map_Format::AdventureMapEventMetadata & metadata )
    {
        metadata.message = event.message;
        metadata.humanPlayerColors = event.colors;
        metadata.computerPlayerColors = event.isComputerPlayerAllowed ? event.colors : 0;
        metadata.isRecurringEvent = !event.isSingleTimeEvent;

        metadata.artifact = event.artifact.GetID();
        if ( event.artifact.GetID() == Artifact::SPELL_SCROLL ) {
            metadata.artifactMetadata = event.artifact.getSpellId();
        }

        metadata.resources = event.resources;

        metadata.experience = event.experience;

        if ( event.secondarySkill.isValid() ) {
            metadata.secondarySkill = static_cast<uint8_t>( event.secondarySkill.Skill() );
            metadata.secondarySkillLevel = static_cast<uint8_t>( event.secondarySkill.Level() );
        }
    }

    void fillSphinxMetadata( const MapSphinx & sphinx, Maps::Map_Format::SphinxMetadata & metadata )
    {
        metadata.riddle = sphinx.riddle;
        metadata.answers.assign( sphinx.answers.begin(), sphinx.answers.end() );

        metadata.artifact = sphinx.artifact.GetID();
        if ( sphinx.artifact.GetID() == Artifact::SPELL_SCROLL ) {
            metadata.artifactMetadata = sphinx.artifact.getSpellId();
        }

        metadata.resources = sphinx.resources;
    }

    // Transfers the victory and loss conditions. The legacy format stores the position of the
    // object the condition is set for, while the modern one stores its tile index and color.
    void fillConditions( const Maps::FileInfo & fileInfo, Maps::Map_Format::MapFormat & map )
    {
        const auto getTileIndex = []( const uint16_t x, const uint16_t y, const int32_t width ) { return static_cast<uint32_t>( y * width + x ); };

        // The color of the owner of the town or the hero the condition is set for. A neutral object
        // belongs to nobody, which must not be confused with belonging to everybody: a town of the
        // player's own color would mean that the "capture the town" condition is met right away.
        const auto getObjectColor = [width = map.width]( const uint32_t tileIndex ) -> uint32_t {
            const fheroes2::Point tilePos{ static_cast<int32_t>( tileIndex ) % width, static_cast<int32_t>( tileIndex ) / width };

            if ( const Castle * castle = world.getCastleEntrance( tilePos ); castle != nullptr ) {
                return static_cast<uint32_t>( castle->GetColor() );
            }

            if ( const Heroes * hero = world.GetHeroes( tilePos ); hero != nullptr ) {
                return static_cast<uint32_t>( hero->GetColor() );
            }

            return static_cast<uint32_t>( world.ColorCapturedObject( static_cast<int32_t>( tileIndex ) ) );
        };

        map.victoryConditionType = fileInfo.victoryConditionType;
        map.isVictoryConditionApplicableForAI = fileInfo.compAlsoWins;
        map.allowNormalVictory = fileInfo.allowNormalVictory;

        switch ( fileInfo.victoryConditionType ) {
        case Maps::FileInfo::VICTORY_CAPTURE_TOWN:
        case Maps::FileInfo::VICTORY_KILL_HERO: {
            const uint32_t tileIndex = getTileIndex( fileInfo.victoryConditionParams[0], fileInfo.victoryConditionParams[1], map.width );
            map.victoryConditionMetadata = { tileIndex, getObjectColor( tileIndex ) };
            break;
        }
        case Maps::FileInfo::VICTORY_OBTAIN_ARTIFACT:
            map.victoryConditionMetadata = { static_cast<uint32_t>( fileInfo.WinsFindArtifactID() ) };
            break;
        case Maps::FileInfo::VICTORY_COLLECT_ENOUGH_GOLD:
            map.victoryConditionMetadata = { fileInfo.getWinningGoldAccumulationValue() };
            break;
        case Maps::FileInfo::VICTORY_DEFEAT_OTHER_SIDE: {
            // The modern format stores exactly two alliances.
            std::set<PlayerColorsSet> sides;
            for ( const PlayerColorsSet side : fileInfo.unions ) {
                if ( side != 0 ) {
                    sides.emplace( side );
                }
            }

            map.alliances.assign( sides.begin(), sides.end() );

            if ( map.alliances.size() != 2 ) {
                // The alliances are broken, so fall back to the normal victory condition.
                map.alliances.clear();
                map.victoryConditionType = Maps::FileInfo::VICTORY_DEFEAT_EVERYONE;
                map.isVictoryConditionApplicableForAI = true;
                map.allowNormalVictory = true;
            }
            break;
        }
        default:
            break;
        }

        if ( map.victoryConditionType == Maps::FileInfo::VICTORY_DEFEAT_EVERYONE ) {
            map.isVictoryConditionApplicableForAI = true;
            map.allowNormalVictory = true;
        }

        map.lossConditionType = fileInfo.lossConditionType;

        switch ( fileInfo.lossConditionType ) {
        case Maps::FileInfo::LOSS_TOWN:
        case Maps::FileInfo::LOSS_HERO: {
            const uint32_t tileIndex = getTileIndex( fileInfo.lossConditionParams[0], fileInfo.lossConditionParams[1], map.width );

            // The modern format only allows human players to be set for this condition.
            const uint32_t color = getObjectColor( tileIndex ) & static_cast<uint32_t>( map.humanPlayerColors );
            map.lossConditionMetadata = { tileIndex, color };
            break;
        }
        case Maps::FileInfo::LOSS_OUT_OF_TIME:
            map.lossConditionMetadata = { fileInfo.lossConditionParams[0] };
            break;
        default:
            break;
        }
    }

    void fillRumorsAndEvents( Maps::Map_Format::MapFormat & map )
    {
        for ( const std::string & rumor : world.getCustomRumors() ) {
            map.rumors.push_back( rumor );
        }

        for ( const EventDate & event : world.getAllEventsDate() ) {
            auto & dailyEvent = map.dailyEvents.emplace_back();

            dailyEvent.message = event.message;
            dailyEvent.humanPlayerColors = event.colors;
            dailyEvent.computerPlayerColors = event.isApplicableForAIPlayers ? event.colors : 0;
            dailyEvent.firstOccurrenceDay = event.firstOccurrenceDay;
            dailyEvent.repeatPeriodInDays = event.repeatPeriodInDays;

            dailyEvent.resources = event.resource;
        }
    }

    // Prints the rendering order of the object parts of every tile. Used to compare a converted map
    // against the legacy one it was made of.
    void dumpRenderOrder()
    {
        for ( int32_t t = 0; t < world.w() * world.h(); ++t ) {
            const Maps::Tile & tile = world.getTile( t );

            std::string line;

            const auto addPart = [&line]( const Maps::ObjectPart & part ) {
                if ( part.icnType == MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                    return;
                }

                // The objects created by the converter have no counterpart in the legacy map.
                if ( part.icnType == MP2::OBJ_ICN_TYPE_FLAG32 || part.icnType == MP2::OBJ_ICN_TYPE_MINIHERO ) {
                    return;
                }

                line += " " + std::to_string( static_cast<int>( part.icnType ) ) + ":" + std::to_string( static_cast<int>( part.icnIndex ) ) + "/L"
                        + std::to_string( static_cast<int>( part.layerType ) );
            };

            for ( const Maps::ObjectPart & part : tile.getGroundObjectParts() ) {
                addPart( part );
            }

            addPart( tile.getMainObjectPart() );

            for ( const Maps::ObjectPart & part : tile.getTopObjectParts() ) {
                addPart( part );
            }

            if ( !line.empty() ) {
                std::cout << "ORDER " << t << line << std::endl;
            }
        }
    }

    // Prints the passability of every tile. Used to compare a converted map against the legacy one.
    void dumpPassability()
    {
        world.updatePassabilities();

        for ( int32_t t = 0; t < world.w() * world.h(); ++t ) {
            std::cout << "PASS " << t << " " << world.getTile( t ).GetPassable() << " " << MP2::StringObject( world.getTile( t ).getMainObjectType() ) << std::endl;
        }
    }

    // The value a legacy map stores for every tile. The engine turns it into the metadata of the tile,
    // but it also uses zero as "not set": the amount of a treasure, the size of a troop or the spell
    // of a shrine are then chosen at random every time the map is loaded. Once the map is loaded this
    // value is already replaced by the chosen one, so it has to be read from the file itself in order
    // to tell what the map maker did set and what has to stay random.
    std::vector<uint32_t> readLegacyTileValues( const std::string & inputPath, const int32_t worldSize )
    {
        std::vector<uint32_t> values( worldSize, 0 );

        StreamFile stream;
        if ( !stream.open( inputPath, "rb" ) ) {
            return values;
        }

        stream.seek( MP2::MP2_MAP_INFO_SIZE );

        for ( int32_t i = 0; i < worldSize; ++i ) {
            MP2::MP2TileInfo mp2tile;
            MP2::loadTile( stream, mp2tile );

            // This is how Maps::Tile::Init() works out the metadata of a tile.
            values[i] = ( ( ( mp2tile.quantity2 << 8 ) + mp2tile.quantity1 ) >> 3 );
        }

        return values;
    }

    // Loads a map the same way the game does when a new scenario is started. This is a stricter
    // check than the one the Editor performs, but it fully re-initializes the world, which the
    // engine only supports once per run. Hence it is available as a separate mode of this tool.
    bool verifyMapInGame( const std::string & path )
    {
        Maps::Map_Format::BaseMapFormat baseMap;
        if ( !Maps::Map_Format::loadBaseMap( path, baseMap ) ) {
            return false;
        }

        Maps::FileInfo fileInfo;
        if ( !fileInfo.loadResurrectionMap( baseMap, path ) ) {
            return false;
        }

        Settings & conf = Settings::Get();
        conf.setCurrentMapInfo( fileInfo );
        conf.GetPlayers().SetStartGame();

        if ( !world.loadResurrectionMap( path ) ) {
            return false;
        }

        if ( getenv( "MAP2FH2M_DUMP_ORDER" ) != nullptr ) {
            dumpRenderOrder();
        }

        if ( getenv( "MAP2FH2M_DUMP_PASS" ) != nullptr ) {
            dumpPassability();
        }

        return true;
    }

    bool convertMap( const std::string & inputPath, const std::string & outputPath )
    {
        // Read the map header. It also validates that the file is a supported legacy map.
        Maps::FileInfo fileInfo;
        if ( !fileInfo.readMP2Map( inputPath, true ) ) {
            std::cerr << "Error: " << inputPath << " is not a valid legacy map file." << std::endl;
            return false;
        }

        // The engine loads a map into the state the settings describe - the players of the map among
        // other things. The game does this before loading a map and so must this tool, or a map would
        // be loaded into the state left by the previous one.
        Settings & conf = Settings::Get();
        conf.setCurrentMapInfo( fileInfo );
        conf.GetPlayers().SetStartGame();

        // Let the engine load the map. This gives us every object it understands together with all
        // of its data instead of only the raw tile information.
        if ( !world.LoadMapMP2( inputPath, fileInfo.version == GameVersion::SUCCESSION_WARS ) ) {
            std::cerr << "Error: failed to load " << inputPath << std::endl;
            return false;
        }

        const int32_t width = world.w();
        const int32_t worldSize = width * world.h();

        if ( getenv( "MAP2FH2M_DUMP_ORDER" ) != nullptr ) {
            dumpRenderOrder();
        }

        if ( getenv( "MAP2FH2M_DUMP_PASS" ) != nullptr ) {
            dumpPassability();
        }

        Maps::Map_Format::MapFormat map;
        map.isCampaign = false;
        map.difficulty = fileInfo.difficulty;
        map.availablePlayerColors = fileInfo.kingdomColors;
        map.humanPlayerColors = fileInfo.colorsAvailableForHumans;
        map.computerPlayerColors = fileInfo.colorsAvailableForComp;
        map.name = fileInfo.name;
        map.description = fileInfo.description;
        map.mainLanguage = fileInfo.mainLanguage;
        map.width = width;

        for ( size_t i = 0; i < map.playerRace.size() && i < fileInfo.races.size(); ++i ) {
            map.playerRace[i] = fileInfo.races[i];
        }

        // What the map maker did set, as opposed to what the engine has chosen at random while loading.
        const std::vector<uint32_t> legacyTileValues = readLegacyTileValues( inputPath, worldSize );

        fillConditions( fileInfo, map );
        fillRumorsAndEvents( map );

        map.tiles.resize( worldSize );

        // Collect the terrain and all the object parts of the legacy map, grouping the parts by the
        // UID of the object they belong to.
        std::map<uint32_t, std::set<LegacyPart>> legacyObjects;

        // The layer every part of the legacy map sits on.
        std::map<LegacyPart, Maps::ObjectLayerType> partLayers;

        // The order in which the objects of a tile are rendered. Within a single layer the engine
        // renders the parts in the order they were added, so this order has to be reproduced.
        std::vector<std::vector<OrderedPart>> renderOrder( worldSize );

        // The object the legacy map shows as the main one of a tile. It defines what the tile is.
        std::vector<uint32_t> mainObjectOfTile( worldSize, 0 );

        for ( int32_t tileIndex = 0; tileIndex < worldSize; ++tileIndex ) {
            const Maps::Tile & worldTile = world.getTile( tileIndex );

            Maps::Map_Format::TileInfo & tile = map.tiles[tileIndex];
            tile.terrainIndex = worldTile.getTerrainImageIndex();
            tile.terrainFlags = worldTile.getTerrainFlags();

            const auto addPart = [&legacyObjects, &partLayers, tileIndex]( const Maps::ObjectPart & part, const bool isTopLayer ) {
                if ( part._uid == 0 || part.icnType == MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                    return;
                }

                const LegacyPart legacyPart{ tileIndex, part.icnType, part.icnIndex, isTopLayer };

                legacyObjects[part._uid].insert( legacyPart );
                partLayers.try_emplace( legacyPart, part.layerType );
            };

            addPart( worldTile.getMainObjectPart(), false );

            for ( const Maps::ObjectPart & part : worldTile.getGroundObjectParts() ) {
                addPart( part, false );
            }

            for ( const Maps::ObjectPart & part : worldTile.getTopObjectParts() ) {
                addPart( part, true );
            }

            // The parts of a tile are already sorted by the engine: the ground parts come in the
            // rendering order and the main part is the topmost one of them.
            std::vector<OrderedPart> & order = renderOrder[tileIndex];

            for ( const Maps::ObjectPart & part : worldTile.getGroundObjectParts() ) {
                if ( part._uid != 0 && part.icnType != MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                    order.push_back( { part._uid, part.layerType, false } );
                }
            }

            const Maps::ObjectPart & mainPart = worldTile.getMainObjectPart();
            if ( mainPart._uid != 0 && mainPart.icnType != MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                order.push_back( { mainPart._uid, mainPart.layerType, false } );

                mainObjectOfTile[tileIndex] = mainPart._uid;
            }

            for ( const Maps::ObjectPart & part : worldTile.getTopObjectParts() ) {
                if ( part._uid != 0 && part.icnType != MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                    order.push_back( { part._uid, Maps::OBJECT_LAYER, true } );
                }
            }
        }

        const std::map<uint32_t, std::vector<ObjectLocation>> lookup = buildObjectLookupTable();

        // Every object is placed once - on the tile that holds its main part.
        std::set<uint64_t> placedObjects;
        size_t objectsSkipped = 0;

        // The objects that have been placed. They are needed to attach the metadata afterwards.
        std::vector<PlacedObject> placedObjectList;

        for ( const auto & [legacyUid, parts] : legacyObjects ) {
            // A single legacy object may consist of several modern ones - a town, for instance, comes
            // with its basement and its flags. So the objects that reproduce the parts of the legacy
            // object are picked one by one, each time taking the one that reproduces most of the parts
            // still missing. This also keeps a part of a large object from being mistaken for a small
            // object that happens to have the very same image.
            std::set<LegacyPart> missingParts = parts;

            while ( !missingParts.empty() ) {
                const Maps::ObjectInfo * bestInfo = nullptr;
                ObjectLocation bestLocation;
                int32_t bestAnchorTileIndex = 0;
                PartMatch bestMatch;

                for ( const LegacyPart & part : parts ) {
                    const auto iter = lookup.find( makePartKey( part.icnType, part.icnIndex ) );
                    if ( iter == lookup.end() ) {
                        // No object of the engine has such a part.
                        continue;
                    }

                    const fheroes2::Point tilePos{ part.tileIndex % width, part.tileIndex / width };

                    for ( const ObjectLocation & location : iter->second ) {
                        const Maps::ObjectInfo & info = Maps::getObjectInfo( location.group, static_cast<int32_t>( location.index ) );
                        if ( info.groundLevelParts.empty() ) {
                            continue;
                        }

                        // The object is looked up by any of its parts, so its anchor tile is worked out
                        // from the offset of that very part.
                        const fheroes2::Point anchorPos = tilePos - location.partOffset;
                        if ( !Maps::isValidAbsPoint( anchorPos.x, anchorPos.y ) || !doesObjectFitTheMap( info, anchorPos ) ) {
                            // The anchor tile of the object is outside of the map, or an action object
                            // does not fit into it. The Editor cannot represent such an object.
                            continue;
                        }

                        const uint64_t placementKey
                            = ( static_cast<uint64_t>( legacyUid ) << 32 ) | ( static_cast<uint64_t>( location.group ) << 24 ) | location.index;
                        if ( placedObjects.count( placementKey ) > 0 ) {
                            continue;
                        }

                        PartMatch match = getMatchingParts( info, anchorPos, parts, partLayers, width );

                        size_t newPartCount = 0;
                        for ( const LegacyPart & matchedPart : match.parts ) {
                            if ( missingParts.count( matchedPart ) > 0 ) {
                                ++newPartCount;
                            }
                        }

                        if ( newPartCount == 0 ) {
                            continue;
                        }

                        // The legacy map tells what an object of a tile is, and an object that is what
                        // the legacy map says takes precedence. Otherwise a mine dug into a mountain
                        // could well be taken for a mountain, which not only looks the same but is also
                        // passable in a different way.
                        match.isOfTheSameObjectType = world.getTile( anchorPos.y * width + anchorPos.x ).getMainObjectType( false ) == info.objectType;

                        // Prefer the object the legacy map says the tile holds, then the one that
                        // reproduces the most of the parts that are still missing, then the one that
                        // puts them on the same layers, then the one that reproduces the most parts.
                        const auto rank = [&missingParts]( const PartMatch & partMatch ) {
                            size_t stillMissingCount = 0;
                            for ( const LegacyPart & matchedPart : partMatch.parts ) {
                                if ( missingParts.count( matchedPart ) > 0 ) {
                                    ++stillMissingCount;
                                }
                            }

                            return std::make_tuple( partMatch.isOfTheSameObjectType, stillMissingCount, partMatch.sameLayerCount, partMatch.parts.size() );
                        };

                        if ( bestInfo == nullptr || rank( match ) > rank( bestMatch ) ) {
                            bestInfo = &info;
                            bestLocation = location;
                            bestAnchorTileIndex = anchorPos.y * width + anchorPos.x;
                            bestMatch = std::move( match );
                        }
                    }
                }

                if ( bestInfo == nullptr ) {
                    // The remaining parts cannot be reproduced by any object known to the engine.
                    objectsSkipped += missingParts.size();
                    break;
                }

                const uint64_t placementKey
                    = ( static_cast<uint64_t>( legacyUid ) << 32 ) | ( static_cast<uint64_t>( bestLocation.group ) << 24 ) | bestLocation.index;
                placedObjects.insert( placementKey );

                placedObjectList.push_back( { legacyUid, bestLocation.group, bestLocation.index, bestAnchorTileIndex } );

                for ( const LegacyPart & matchedPart : bestMatch.parts ) {
                    missingParts.erase( matchedPart );
                }
            }
        }

        // An adventure map event is the only object that a legacy map stores without any image at
        // all: it has neither an image nor a UID, and its presence is only marked by the object type
        // of the tile it sits on. Such an object cannot be found by the image based search above, so
        // it is placed here. The modern format does give an event an (invisible) image.
        size_t eventsPlaced = 0;

        // The objects created by this tool need UIDs of their own, which must not clash with the
        // legacy ones. They are also the largest ones, which makes such an object the main object of
        // its tile - exactly what an event, a hero or a buried artifact has to be.
        uint32_t nextExtraUid = legacyObjects.empty() ? 0 : legacyObjects.rbegin()->first;

        {
            const std::vector<Maps::ObjectInfo> & miscObjects = Maps::getObjectsByGroup( Maps::ObjectGroup::ADVENTURE_MISCELLANEOUS );

            uint32_t eventObjectIndex = UINT32_MAX;
            for ( size_t i = 0; i < miscObjects.size(); ++i ) {
                if ( miscObjects[i].objectType == MP2::OBJ_EVENT ) {
                    eventObjectIndex = static_cast<uint32_t>( i );
                    break;
                }
            }

            if ( eventObjectIndex == UINT32_MAX ) {
                std::cerr << "Error: the engine does not know the adventure map event object." << std::endl;
                return false;
            }

            for ( int32_t tileIndex = 0; tileIndex < worldSize; ++tileIndex ) {
                if ( world.getTile( tileIndex ).getMainObjectType( false ) != MP2::OBJ_EVENT ) {
                    continue;
                }

                ++nextExtraUid;
                placedObjectList.push_back( { nextExtraUid, Maps::ObjectGroup::ADVENTURE_MISCELLANEOUS, eventObjectIndex, tileIndex } );
                ++eventsPlaced;
            }
        }

        // A legacy map does not store the flags of a town: the engine draws them itself using the
        // color kept in the town's data. The Editor, on the other hand, works the other way round -
        // it reads the town's color from the flags standing on both sides of the town entrance, and
        // these flags must carry the UID of the town. So the flags have to be created here.
        size_t flagsPlaced = 0;

        for ( const PlacedObject & town : std::vector<PlacedObject>( placedObjectList ) ) {
            if ( town.group != Maps::ObjectGroup::KINGDOM_TOWNS ) {
                continue;
            }

            const Castle * castle = world.getCastleEntrance( { town.tileIndex % width, town.tileIndex / width } );
            if ( castle == nullptr ) {
                continue;
            }

            const int colorIndex = Color::GetIndex( castle->GetColor() );
            if ( colorIndex < 0 ) {
                continue;
            }

            // The Editor keeps the left flag on the tile before the entrance and the right one after it.
            if ( town.tileIndex <= 0 || town.tileIndex >= worldSize - 1 ) {
                continue;
            }

            placedObjectList.push_back( { town.uid, Maps::ObjectGroup::LANDSCAPE_FLAGS, static_cast<uint32_t>( colorIndex * 2 ), town.tileIndex - 1 } );
            placedObjectList.push_back( { town.uid, Maps::ObjectGroup::LANDSCAPE_FLAGS, static_cast<uint32_t>( colorIndex * 2 + 1 ), town.tileIndex + 1 } );

            flagsPlaced += 2;
        }

        // A hero has no image in a loaded legacy map either: the engine removes the hero sprite from
        // the tile and keeps the hero as a separate object. So heroes are placed here as well.
        size_t heroesPlaced = 0;

        {
            const std::vector<Maps::ObjectInfo> & heroObjects = Maps::getObjectsByGroup( Maps::ObjectGroup::KINGDOM_HEROES );

            for ( int32_t tileIndex = 0; tileIndex < worldSize; ++tileIndex ) {
                const Heroes * hero = world.GetHeroes( { tileIndex % width, tileIndex / width } );
                if ( hero == nullptr || hero->Modes( Heroes::JAIL ) ) {
                    // A jailed hero is represented by the jail object which is a normal map object.
                    continue;
                }

                const int colorIndex = Color::GetIndex( hero->GetColor() );
                if ( colorIndex < 0 ) {
                    continue;
                }

                // Find the hero object of the needed color and race.
                uint32_t heroObjectIndex = UINT32_MAX;
                for ( size_t i = 0; i < heroObjects.size(); ++i ) {
                    const auto & objectMetadata = heroObjects[i].metadata;
                    if ( static_cast<int>( objectMetadata[0] ) == colorIndex && Race::IndexToRace( static_cast<int>( objectMetadata[1] ) ) == hero->GetRace() ) {
                        heroObjectIndex = static_cast<uint32_t>( i );
                        break;
                    }
                }

                if ( heroObjectIndex == UINT32_MAX ) {
                    continue;
                }

                ++nextExtraUid;
                placedObjectList.push_back( { nextExtraUid, Maps::ObjectGroup::KINGDOM_HEROES, heroObjectIndex, tileIndex } );
                ++heroesPlaced;
            }
        }

        // The engine removes the Ultimate Artifact from the map right after loading it, remembering
        // only the tile it is buried on. So it has to be put back as well.
        size_t ultimateArtifactsPlaced = 0;

        {
            const int32_t ultimateArtifactTile = world.GetUltimateArtifact().getPosition();

            if ( ultimateArtifactTile >= 0 && ultimateArtifactTile < worldSize ) {
                const std::vector<Maps::ObjectInfo> & artifactObjects = Maps::getObjectsByGroup( Maps::ObjectGroup::ADVENTURE_ARTIFACTS );

                uint32_t ultimateObjectIndex = UINT32_MAX;
                for ( size_t i = 0; i < artifactObjects.size(); ++i ) {
                    if ( artifactObjects[i].objectType == MP2::OBJ_RANDOM_ULTIMATE_ARTIFACT ) {
                        ultimateObjectIndex = static_cast<uint32_t>( i );
                        break;
                    }
                }

                if ( ultimateObjectIndex != UINT32_MAX
                     && doesObjectFitTheMap( Maps::getObjectInfo( Maps::ObjectGroup::ADVENTURE_ARTIFACTS, static_cast<int32_t>( ultimateObjectIndex ) ),
                                             { ultimateArtifactTile % width, ultimateArtifactTile / width } ) ) {
                    ++nextExtraUid;
                    placedObjectList.push_back( { nextExtraUid, Maps::ObjectGroup::ADVENTURE_ARTIFACTS, ultimateObjectIndex, ultimateArtifactTile } );

                    ultimateArtifactsPlaced = 1;
                }
            }
        }

        // The number of the objects whose rendering order the legacy map defines inconsistently.
        size_t contradictoryOrderings = 0;

        // The engine renders the objects of a tile that sit on the same layer in the order they were
        // added, and adds them in the ascending order of their UIDs. The rendering order of a legacy
        // map is therefore reproduced by giving the objects UIDs that follow the order of their parts
        // within the tiles. This also decides which object becomes the main one of a tile - the last
        // one added - so, for instance, a monster standing in front of a bush stays a monster.
        //
        // All the objects of a single legacy object keep sharing one UID: the Editor relies on a town
        // and its flags having the same UID to work out the town's color.
        {
            std::set<uint32_t> placedUids;
            for ( const PlacedObject & placedObject : placedObjectList ) {
                placedUids.insert( placedObject.uid );
            }

            // Build the ordering constraints out of the tiles: an object rendered before another one
            // on the same tile and layer must get a smaller UID.
            std::map<uint32_t, std::set<uint32_t>> successors;
            std::map<uint32_t, size_t> predecessorCount;
            for ( const uint32_t uid : placedUids ) {
                predecessorCount[uid] = 0;
            }

            const auto addConstraints = [&successors, &predecessorCount]( const std::vector<uint32_t> & sequence ) {
                for ( size_t i = 0; i + 1 < sequence.size(); ++i ) {
                    if ( sequence[i] == sequence[i + 1] ) {
                        continue;
                    }

                    if ( successors[sequence[i]].insert( sequence[i + 1] ).second ) {
                        ++predecessorCount[sequence[i + 1]];
                    }
                }
            };

            // The largest UID of the legacy map. Everything above it has been created by this tool.
            const uint32_t largestLegacyUid = legacyObjects.empty() ? 0 : legacyObjects.rbegin()->first;

            // The rendering position of an object on a tile of the legacy map.
            const auto legacyPosition = [&renderOrder, &mainObjectOfTile, largestLegacyUid]( const int32_t tileIndex, const uint32_t uid ) {
                if ( mainObjectOfTile[tileIndex] == uid ) {
                    // The object the legacy map shows as the main one of this tile has to stay the
                    // main one, whatever layers the modern definitions put the other objects on. This
                    // is what keeps a monster guarding a mine a monster.
                    return std::numeric_limits<size_t>::max() - 1;
                }

                const std::vector<OrderedPart> & tileParts = renderOrder[tileIndex];

                for ( size_t i = 0; i < tileParts.size(); ++i ) {
                    if ( tileParts[i].uid == uid ) {
                        // The parts of the legacy map are numbered from 1 here, see below.
                        return i + 1;
                    }
                }

                if ( uid > largestLegacyUid ) {
                    // An object created by this tool - an event, a hero or the buried artifact. It has
                    // no counterpart in the legacy map and must become the main object of its tile.
                    return std::numeric_limits<size_t>::max();
                }

                // A modern object may well have more parts than the legacy map gives it. Such an extra
                // part must not cover the objects that the legacy map does have on this tile - most
                // notably it must not take a monster's place as the main object of the tile.
                return static_cast<size_t>( 0 );
            };

            // The parts of an object end up on the layers prescribed by the modern object definition,
            // which do not have to match the layers of the legacy map. So the objects are grouped by
            // the tile and layer they will actually share, and within such a group they are ordered
            // the way the legacy map renders them.
            std::map<std::tuple<int32_t, bool, int>, std::vector<std::pair<size_t, uint32_t>>> groups;

            for ( const PlacedObject & placedObject : placedObjectList ) {
                const Maps::ObjectInfo & info = Maps::getObjectInfo( placedObject.group, static_cast<int32_t>( placedObject.index ) );
                const fheroes2::Point anchorPos{ placedObject.tileIndex % width, placedObject.tileIndex / width };

                const auto addToGroup = [&]( const fheroes2::Point & offset, const Maps::ObjectLayerType layerType, const bool isTopLayer ) {
                    const fheroes2::Point pos = anchorPos + offset;
                    if ( !Maps::isValidAbsPoint( pos.x, pos.y ) ) {
                        return;
                    }

                    const int32_t tileIndex = pos.y * width + pos.x;

                    groups[{ tileIndex, isTopLayer, static_cast<int>( layerType ) }].emplace_back( legacyPosition( tileIndex, placedObject.uid ), placedObject.uid );
                };

                for ( const auto & part : info.groundLevelParts ) {
                    if ( part.layerType == Maps::SHADOW_LAYER || part.layerType == Maps::TERRAIN_LAYER ) {
                        // The legacy map orders the shadows of the objects rather freely, which makes
                        // the constraints of the different tiles contradict each other. Since a shadow
                        // is drawn below everything anyway, its order is not worth such a conflict.
                        continue;
                    }

                    addToGroup( part.tileOffset, part.layerType, false );
                }

                for ( const auto & part : info.topLevelParts ) {
                    addToGroup( part.tileOffset, Maps::OBJECT_LAYER, true );
                }
            }

            for ( auto & [key, group] : groups ) {
                std::stable_sort( group.begin(), group.end() );

                std::vector<uint32_t> sequence;
                sequence.reserve( group.size() );
                for ( const auto & [position, uid] : group ) {
                    sequence.push_back( uid );
                }

                addConstraints( sequence );
            }

            // Objects that are not constrained keep the order of the legacy map. The objects created
            // by this tool (events, heroes, the buried artifact) have the largest legacy UIDs and thus
            // come last, which is what makes them the main objects of their tiles.
            std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> ready;
            for ( const auto & [uid, count] : predecessorCount ) {
                if ( count == 0 ) {
                    ready.push( uid );
                }
            }

            std::map<uint32_t, uint32_t> legacyToModernUid;
            uint32_t nextUid = 1;

            // A legacy map may well order the same pair of objects differently on different tiles,
            // which makes the constraints contradictory. In such a case the least constrained object
            // is taken next, which breaks the contradiction while keeping the rest of the order.
            size_t contradictionsResolved = 0;

            while ( legacyToModernUid.size() < placedUids.size() ) {
                if ( ready.empty() ) {
                    uint32_t bestUid = 0;
                    size_t bestCount = std::numeric_limits<size_t>::max();

                    for ( const uint32_t uid : placedUids ) {
                        if ( legacyToModernUid.count( uid ) > 0 ) {
                            continue;
                        }

                        if ( predecessorCount[uid] < bestCount ) {
                            bestCount = predecessorCount[uid];
                            bestUid = uid;
                        }
                    }

                    predecessorCount[bestUid] = 0;
                    ready.push( bestUid );

                    ++contradictionsResolved;
                }

                const uint32_t uid = ready.top();
                ready.pop();

                if ( !legacyToModernUid.emplace( uid, nextUid ).second ) {
                    continue;
                }

                ++nextUid;

                for ( const uint32_t successor : successors[uid] ) {
                    if ( predecessorCount[successor] > 0 ) {
                        --predecessorCount[successor];
                    }

                    if ( predecessorCount[successor] == 0 && legacyToModernUid.count( successor ) == 0 ) {
                        ready.push( successor );
                    }
                }
            }

            contradictoryOrderings = contradictionsResolved;

            for ( PlacedObject & placedObject : placedObjectList ) {
                placedObject.uid = legacyToModernUid[placedObject.uid];
            }
        }

        // Place the objects on the tiles in the ascending order of their UIDs. A town, its basement
        // and its flags share a single UID, and the engine keeps them in the order they are stored,
        // so the basement has to come before the town it belongs to.
        std::stable_sort( placedObjectList.begin(), placedObjectList.end(), []( const PlacedObject & left, const PlacedObject & right ) {
            if ( left.uid != right.uid ) {
                return left.uid < right.uid;
            }

            return left.group < right.group;
        } );

        for ( const PlacedObject & placedObject : placedObjectList ) {
            auto & object = map.tiles[placedObject.tileIndex].objects.emplace_back();
            object.id = placedObject.uid;
            object.group = placedObject.group;
            object.index = placedObject.index;
        }

        // Now that every object is placed, transfer the data it carries. Note that the engine
        // requires a metadata entry to exist for some of the object kinds, even an empty one.
        size_t metadataEntries = 0;

        for ( const PlacedObject & placedObject : placedObjectList ) {
            const uint32_t uid = placedObject.uid;
            const int32_t tileIndex = placedObject.tileIndex;
            const fheroes2::Point tilePos{ tileIndex % width, tileIndex / width };

            const Maps::ObjectInfo & info = Maps::getObjectInfo( placedObject.group, static_cast<int32_t>( placedObject.index ) );
            const std::array<uint32_t, 3> & tileData = world.getTile( tileIndex ).metadata();

            switch ( placedObject.group ) {
            case Maps::ObjectGroup::KINGDOM_TOWNS: {
                auto & metadata = map.castleMetadata[uid];

                if ( const Castle * castle = world.getCastleEntrance( tilePos ); castle != nullptr ) {
                    fillCastleMetadata( *castle, metadata );
                    ++metadataEntries;
                }

                // The engine expects the presence of a castle or a tent to match the object itself.
                syncCastleAndTentBuildings( metadata, info.metadata[1] != 0 );
                break;
            }
            case Maps::ObjectGroup::KINGDOM_HEROES: {
                auto & metadata = map.heroMetadata[uid];

                if ( const Heroes * hero = world.GetHeroes( tilePos ); hero != nullptr ) {
                    metadata = hero->getHeroMetadata();
                    ++metadataEntries;
                }

                // The engine expects the race to match the one of the object itself.
                metadata.race = Race::IndexToRace( static_cast<int>( info.metadata[1] ) );
                break;
            }
            case Maps::ObjectGroup::MONSTERS: {
                // The metadata of a monster holds the size of its troop. A troop whose size the map
                // maker did not set is a random one and has to stay random.
                auto & metadata = map.standardMetadata[uid];

                metadata.metadata[0] = ( legacyTileValues[tileIndex] == 0 ) ? 0 : static_cast<int32_t>( tileData[0] );
                metadata.metadata[1] = static_cast<int32_t>( tileData[1] );
                metadata.metadata[2] = static_cast<int32_t>( tileData[2] );

                ++metadataEntries;
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_ARTIFACTS: {
                // An artifact only carries metadata when it is a Spell Scroll: the spell it holds.
                // Everything else about an artifact is restored by the engine from the object itself.
                auto & metadata = map.standardMetadata[uid];

                if ( info.objectType == MP2::OBJ_ARTIFACT && info.metadata[0] == Artifact::SPELL_SCROLL ) {
                    metadata.metadata[0] = static_cast<int32_t>( tileData[1] );
                }

                ++metadataEntries;
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_TREASURES: {
                if ( info.objectType == MP2::OBJ_RESOURCE ) {
                    // The metadata holds the amount of the resource. An amount the map maker did not
                    // set is a random one and has to stay random.
                    map.standardMetadata[uid].metadata[0] = ( legacyTileValues[tileIndex] == 0 ) ? 0 : static_cast<int32_t>( tileData[1] );
                    ++metadataEntries;
                }
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_POWER_UPS: {
                switch ( info.objectType ) {
                case MP2::OBJ_SHRINE_FIRST_CIRCLE:
                case MP2::OBJ_SHRINE_SECOND_CIRCLE:
                case MP2::OBJ_SHRINE_THIRD_CIRCLE:
                    if ( legacyTileValues[tileIndex] != 0 ) {
                        // The map maker did choose the spell of this shrine, so make it the only option.
                        map.selectionObjectMetadata[uid].selectedItems = { static_cast<int32_t>( tileData[0] ) };
                        ++metadataEntries;
                    }

                    // Otherwise the spell is a random one and no metadata is stored, which is what
                    // makes the engine choose a spell anew every time the map is loaded.
                    break;
                default:
                    break;
                }
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_WATER: {
                if ( info.objectType == MP2::OBJ_BOTTLE ) {
                    if ( const auto * sign = dynamic_cast<const MapSign *>( world.GetMapObject( static_cast<uint32_t>( tileIndex ) ) ); sign != nullptr ) {
                        map.signMetadata[uid].message = sign->message.text;
                        ++metadataEntries;
                    }
                    else {
                        map.signMetadata[uid];
                    }
                }
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_MINES: {
                fillCapturableMetadata( map, uid, tileIndex, info.objectType );
                ++metadataEntries;
                break;
            }
            case Maps::ObjectGroup::ADVENTURE_MISCELLANEOUS: {
                // Signs, events and riddles are stored as separate objects whose UID is the index of
                // the tile they are placed on.
                MapBaseObject * mapObject = world.GetMapObject( static_cast<uint32_t>( tileIndex ) );

                switch ( info.objectType ) {
                case MP2::OBJ_SIGN:
                    if ( const auto * sign = dynamic_cast<const MapSign *>( mapObject ); sign != nullptr ) {
                        map.signMetadata[uid].message = sign->message.text;
                        ++metadataEntries;
                    }
                    else {
                        map.signMetadata[uid];
                    }
                    break;
                case MP2::OBJ_EVENT:
                    if ( const auto * event = dynamic_cast<const MapEvent *>( mapObject ); event != nullptr ) {
                        fillEventMetadata( *event, map.adventureMapEventMetadata[uid] );
                        ++metadataEntries;
                    }
                    else {
                        map.adventureMapEventMetadata[uid];
                    }
                    break;
                case MP2::OBJ_SPHINX:
                    if ( const auto * sphinx = dynamic_cast<const MapSphinx *>( mapObject ); sphinx != nullptr ) {
                        fillSphinxMetadata( *sphinx, map.sphinxMetadata[uid] );
                        ++metadataEntries;
                    }
                    else {
                        map.sphinxMetadata[uid];
                    }
                    break;
                case MP2::OBJ_JAIL: {
                    auto & metadata = map.heroMetadata[uid];

                    if ( const Heroes * hero = world.GetHeroes( tilePos ); hero != nullptr ) {
                        metadata = hero->getHeroMetadata();
                        ++metadataEntries;
                    }
                    else {
                        metadata.race = Race::RAND;
                    }
                    break;
                }
                case MP2::OBJ_WITCHS_HUT:
                case MP2::OBJ_PYRAMID:
                    if ( legacyTileValues[tileIndex] != 0 ) {
                        // The map maker did choose the skill or the spell, so make it the only option.
                        // Otherwise it stays random, which is what an empty metadata means.
                        map.selectionObjectMetadata[uid].selectedItems = { static_cast<int32_t>( tileData[0] ) };
                        ++metadataEntries;
                    }
                    break;
                default:
                    fillCapturableMetadata( map, uid, tileIndex, info.objectType );
                    ++metadataEntries;
                    break;
                }
                break;
            }
            default:
                break;
            }
        }

        std::cout << "Converted '" << fileInfo.name << "'" << std::endl
                  << "  " << inputPath << " -> " << outputPath << std::endl
                  << "  Map size:       " << width << " x " << width << std::endl
                  << "  Objects placed: " << placedObjectList.size() << std::endl
                  << "  Objects skipped (cut off by the map border): " << objectsSkipped << std::endl
                  << "  Events placed:  " << eventsPlaced << std::endl
                  << "  Heroes placed:  " << heroesPlaced << std::endl
                  << "  Town flags placed: " << flagsPlaced << std::endl
                  << "  Ultimate Artifact placed: " << ultimateArtifactsPlaced << std::endl
                  << "  Objects with a contradictory rendering order: " << contradictoryOrderings << std::endl
                  << "  Metadata entries: " << metadataEntries << std::endl;

        // The header of a legacy map does not have to list every player the map actually has: the
        // header of a campaign map only lists the players of the campaign scenario. The modern format
        // works the other way round - a hero of a color that is not a player of the map is not even
        // created. So the players are worked out from the objects of the map, the way the Editor does
        // it, and only who may be played by a human is kept as the legacy map defines it.
        const PlayerColorsSet legacyHumanColors = map.humanPlayerColors;

        if ( !Maps::updateMapPlayers( map ) ) {
            std::cerr << "Error: failed to work out the players of the map." << std::endl;
            return false;
        }

        if ( const PlayerColorsSet humanColors = legacyHumanColors & map.availablePlayerColors; humanColors != 0 ) {
            map.humanPlayerColors = humanColors;

            // Every player of the map may be played by the computer.
            map.computerPlayerColors = map.availablePlayerColors;
        }

        // The Editor refuses to open a map that it cannot fully reconstruct, so run the very same
        // procedure it uses to make sure that a broken map is never written out.
        if ( !Maps::readMapInEditor( map ) ) {
            std::cerr << "Error: the converted map cannot be opened in the Editor." << std::endl;
            return false;
        }

        if ( !Maps::Map_Format::saveMap( outputPath, map ) ) {
            std::cerr << "Error: failed to write " << outputPath << std::endl;
            return false;
        }

        return true;
    }
}

namespace
{
    bool isLegacyMapFile( const std::filesystem::path & path )
    {
        std::string extension = path.extension().string();
        std::transform( extension.begin(), extension.end(), extension.begin(), []( const unsigned char character ) { return static_cast<char>( tolower( character ) ); } );

        return extension == ".mp2" || extension == ".mx2" || extension == ".h2c" || extension == ".hxc";
    }

    // Converts a legacy map and makes sure that the result can be both played and edited. A map that
    // does not pass the check is not left behind.
    bool convertAndVerifyMap( const std::string & inputPath, const std::string & outputPath )
    {
        if ( !convertMap( inputPath, outputPath ) ) {
            return false;
        }

        if ( !verifyMapInGame( outputPath ) ) {
            std::cerr << "Error: '" << inputPath << "' has been converted into a map that the game cannot load. Skipping it." << std::endl;

            std::error_code errorCode;
            std::filesystem::remove( outputPath, errorCode );

            return false;
        }

        return true;
    }
}

int main( int argc, char ** argv )
{
    // The engine only treats "The Price of Loyalty" content as valid when the expansion resources
    // are present. This tool works with map data rather than resources, so the expansion content
    // must always be allowed, otherwise objects of the expansion maps would be lost.
    Settings::Get().EnablePriceOfLoyaltySupport( true );

    if ( argc < 2 ) {
        const std::string toolName = System::GetFileName( argv[0] );

        std::cerr << toolName << " converts legacy maps (.mp2, .mx2, .h2c, .hxc) to the modern fheroes2 map format (.fh2m)." << std::endl
                  << "Every converted map is checked to be loadable by the game, and is not written out if it is not." << std::endl
                  << std::endl
                  << "Syntax: " << toolName << " input_map [output_map.fh2m]" << std::endl
                  << "        " << toolName << " input_map ... | directory ..." << std::endl
                  << std::endl
                  << "A map is converted next to the original file unless an output file is given. A directory is" << std::endl
                  << "searched for the legacy maps it holds - use '.' for the current one." << std::endl;
        return EXIT_FAILURE;
    }

    // A single map followed by an explicit output path.
    if ( argc == 3 && std::filesystem::path( argv[2] ).extension() == ".fh2m" ) {
        return convertAndVerifyMap( argv[1], argv[2] ) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // Collect the maps to convert.
    std::vector<std::filesystem::path> maps;

    for ( int i = 1; i < argc; ++i ) {
        const std::filesystem::path path( argv[i] );

        std::error_code errorCode;
        if ( !std::filesystem::is_directory( path, errorCode ) ) {
            if ( !isLegacyMapFile( path ) ) {
                // A wildcard passed to this tool brings in everything a directory holds, including the
                // maps it has converted before.
                std::cerr << "Warning: '" << path.string() << "' is not a legacy map. Skipping it." << std::endl;
                continue;
            }

            maps.push_back( path );
            continue;
        }

        std::vector<std::filesystem::path> mapsInDirectory;

        for ( const auto & entry : std::filesystem::directory_iterator( path, errorCode ) ) {
            if ( entry.is_regular_file( errorCode ) && isLegacyMapFile( entry.path() ) ) {
                mapsInDirectory.push_back( entry.path() );
            }
        }

        if ( mapsInDirectory.empty() ) {
            std::cerr << "Warning: '" << path.string() << "' holds no legacy maps." << std::endl;
        }

        // The order the file system returns the entries in is arbitrary.
        std::sort( mapsInDirectory.begin(), mapsInDirectory.end() );

        maps.insert( maps.end(), mapsInDirectory.begin(), mapsInDirectory.end() );
    }

    size_t convertedCount = 0;
    size_t failedCount = 0;

    for ( const std::filesystem::path & inputPath : maps ) {
        std::filesystem::path outputPath( inputPath );
        outputPath.replace_extension( ".fh2m" );

        if ( convertAndVerifyMap( inputPath.string(), outputPath.string() ) ) {
            ++convertedCount;
        }
        else {
            ++failedCount;
        }
    }

    if ( maps.size() > 1 ) {
        std::cout << std::endl << "Converted " << convertedCount << " of " << maps.size() << " maps." << std::endl;
    }

    return failedCount == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
