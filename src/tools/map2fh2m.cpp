/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2026                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "color.h"
#include "map_format_helper.h"
#include "map_format_info.h"
#include "map_object_info.h"
#include "maps_fileinfo.h"
#include "maps_tiles.h"
#include "mp2.h"
#include "system.h"
#include "world.h"

namespace
{
    struct Part
    {
        int32_t tile = 0;
        Maps::ObjectLayerType layer = Maps::OBJECT_LAYER;
        MP2::ObjectIcnType icn = MP2::OBJ_ICN_TYPE_UNKNOWN;
        uint32_t index = 0;
        bool top = false;
    };

    bool operator==( const Part & l, const Part & r )
    {
        return l.tile == r.tile && l.layer == r.layer && l.icn == r.icn && l.index == r.index && l.top == r.top;
    }

    bool partLess( const Part & l, const Part & r )
    {
        return std::tie( l.tile, l.layer, l.icn, l.index, l.top ) < std::tie( r.tile, r.layer, r.icn, r.index, r.top );
    }

    void printUsage( const char * argv0 )
    {
        const std::string toolName = System::GetFileName( argv0 );
        std::cerr << toolName << " converts Heroes II MP2/H2C/HXC/MX2 maps to fheroes2 FH2M maps.\n"
                  << "Syntax: " << toolName << " input_map.[mp2|h2c|hxc|mx2] [output_map.fh2m]\n";
    }

    void addConditionMetadata( Maps::Map_Format::MapFormat & map, const Maps::FileInfo & info )
    {
        switch ( info.victoryConditionType ) {
        case Maps::FileInfo::VICTORY_CAPTURE_TOWN:
        case Maps::FileInfo::VICTORY_KILL_HERO:
            map.victoryConditionMetadata = { info.victoryConditionParams[0], info.victoryConditionParams[1] };
            break;
        case Maps::FileInfo::VICTORY_OBTAIN_ARTIFACT:
            map.victoryConditionMetadata = { info.victoryConditionParams[0] };
            break;
        case Maps::FileInfo::VICTORY_DEFEAT_OTHER_SIDE:
            for ( const PlayerColorsSet side : info.unions ) {
                if ( side != 0 && std::find( map.alliances.begin(), map.alliances.end(), side ) == map.alliances.end() ) {
                    map.alliances.push_back( side );
                }
            }
            break;
        case Maps::FileInfo::VICTORY_COLLECT_ENOUGH_GOLD:
            map.victoryConditionMetadata = { info.getWinningGoldAccumulationValue() };
            break;
        default:
            break;
        }

        switch ( info.lossConditionType ) {
        case Maps::FileInfo::LOSS_TOWN:
        case Maps::FileInfo::LOSS_HERO:
            map.lossConditionMetadata = { info.lossConditionParams[0], info.lossConditionParams[1] };
            break;
        case Maps::FileInfo::LOSS_OUT_OF_TIME:
            map.lossConditionMetadata = { info.lossConditionParams[0] };
            break;
        default:
            break;
        }
    }

    bool sameObject( const std::vector<Part> & parts, const int32_t anchorTile, const Maps::ObjectInfo & object )
    {
        std::vector<Part> expected;
        expected.reserve( object.groundLevelParts.size() + object.topLevelParts.size() );
        const int32_t width = world.w();

        for ( const Maps::LayeredObjectPartInfo & part : object.groundLevelParts ) {
            expected.push_back( { anchorTile + part.tileOffset.y * width + part.tileOffset.x, part.layerType, part.icnType, part.icnIndex, false } );
        }
        for ( const Maps::ObjectPartInfo & part : object.topLevelParts ) {
            expected.push_back( { anchorTile + part.tileOffset.y * width + part.tileOffset.x, Maps::OBJECT_LAYER, part.icnType, part.icnIndex, true } );
        }

        std::sort( expected.begin(), expected.end(), partLess );
        return parts == expected;
    }

    bool appendObject( Maps::Map_Format::MapFormat & map, const uint32_t uid, std::vector<Part> parts )
    {
        std::sort( parts.begin(), parts.end(), partLess );

        for ( uint8_t groupId = 0; groupId < static_cast<uint8_t>( Maps::ObjectGroup::GROUP_COUNT ); ++groupId ) {
            const auto group = static_cast<Maps::ObjectGroup>( groupId );
            const std::vector<Maps::ObjectInfo> & objects = Maps::getObjectsByGroup( group );
            for ( size_t i = 0; i < objects.size(); ++i ) {
                for ( const Part & part : parts ) {
                    if ( sameObject( parts, part.tile, objects[i] ) ) {
                        map.tiles[part.tile].objects.push_back( { uid, group, static_cast<uint32_t>( i ) } );
                        return true;
                    }
                }
            }
        }

        std::cerr << "Cannot convert object UID " << uid << " at tile " << parts.front().tile << std::endl;
        return false;
    }

    bool convertLoadedWorld( Maps::Map_Format::MapFormat & map )
    {
        map.width = world.w();
        map.tiles.resize( static_cast<size_t>( world.w() ) * world.h() );

        std::map<uint32_t, std::vector<Part>> objects;
        for ( int32_t tileId = 0; tileId < world.w() * world.h(); ++tileId ) {
            const Maps::Tile & tile = world.getTile( tileId );
            Maps::Map_Format::TileInfo & out = map.tiles[tileId];
            out.terrainIndex = tile.getTerrainImageIndex();
            out.terrainFlags = tile.getTerrainFlags();

            const auto addPart = [&]( const Maps::ObjectPart & part, const bool top ) {
                if ( part._uid != 0 && part.icnType != MP2::OBJ_ICN_TYPE_UNKNOWN ) {
                    objects[part._uid].push_back( { tileId, part.layerType, part.icnType, part.icnIndex, top } );
                }
            };
            for ( const Maps::ObjectPart & part : tile.getGroundObjectParts() ) {
                addPart( part, false );
            }
            addPart( tile.getMainObjectPart(), false );
            for ( const Maps::ObjectPart & part : tile.getTopObjectParts() ) {
                addPart( part, true );
            }

            if ( tile.getMainObjectPart()._uid != 0 && tile.metadata()[0] != 0 ) {
                std::transform( tile.metadata().begin(), tile.metadata().end(), map.standardMetadata[tile.getMainObjectPart()._uid].metadata.begin(),
                                []( const uint32_t value ) { return static_cast<int32_t>( value ); } );
            }
        }

        size_t skippedObjectCount = 0;
        for ( auto & [uid, parts] : objects ) {
            if ( !appendObject( map, uid, std::move( parts ) ) ) {
                ++skippedObjectCount;
            }
        }

        if ( skippedObjectCount > 0 ) {
            std::cerr << "Skipped " << skippedObjectCount << " object(s) which are not available in the FH2M object set." << std::endl;
        }

        return true;
    }
}

int main( const int argc, char ** argv )
{
    if ( argc != 2 && argc != 3 ) {
        printUsage( argv[0] );
        return EXIT_FAILURE;
    }

    const std::string input = argv[1];
    const std::string output = ( argc == 3 ) ? argv[2] : ( std::filesystem::path( input ).replace_extension( ".fh2m" ).string() );

    Maps::FileInfo info;
    if ( !info.readMP2Map( input, true ) ) {
        std::cerr << "Failed to read map information from " << input << std::endl;
        return EXIT_FAILURE;
    }

    if ( !world.LoadMapMP2( input, info.version == GameVersion::SUCCESSION_WARS ) ) {
        std::cerr << "Failed to load " << input << std::endl;
        return EXIT_FAILURE;
    }

    Maps::Map_Format::MapFormat map;
    map.difficulty = info.difficulty;
    map.availablePlayerColors = info.kingdomColors;
    map.humanPlayerColors = info.colorsAvailableForHumans;
    map.computerPlayerColors = info.colorsAvailableForComp;
    map.playerRace = info.races;
    map.victoryConditionType = info.victoryConditionType;
    map.isVictoryConditionApplicableForAI = info.compAlsoWins;
    map.allowNormalVictory = info.allowNormalVictory;
    map.lossConditionType = info.lossConditionType;
    map.mainLanguage = info.mainLanguage;
    map.name = info.name;
    map.description = info.description;
    addConditionMetadata( map, info );

    if ( !convertLoadedWorld( map ) || !Maps::updateMapPlayers( map ) ) {
        std::cerr << "Failed to convert " << input << std::endl;
        return EXIT_FAILURE;
    }

    if ( !Maps::Map_Format::saveMap( output, map ) ) {
        std::cerr << "Failed to save " << output << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Saved " << output << std::endl;
    return EXIT_SUCCESS;
}
