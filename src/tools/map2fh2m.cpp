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
// The conversion reproduces the terrain and places every object that the engine knows about
// back onto the map. Fine grained object metadata that only exists in the legacy format
// (custom castle armies, hero equipment, event and sign texts, riddles, victory / loss
// conditions and so on) is not transferred - such information has to be re-entered in the
// Editor. The produced map is nonetheless a valid, playable and editable .fh2m file.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "serialize.h"
#include "system.h"

#include "castle.h"
#include "color.h"
#include "map_format_info.h"
#include "map_object_info.h"
#include "maps.h"
#include "maps_fileinfo.h"
#include "mp2.h"
#include "mp2_helper.h"
#include "race.h"

namespace
{
    // The legacy map format stores an object part as a 6-bit ICN identifier plus an 8-bit image
    // index. Both values are combined into a single key that is used to look up the modern object
    // this part belongs to.
    uint32_t makePartKey( const MP2::ObjectIcnType icnType, const uint32_t icnIndex )
    {
        return ( static_cast<uint32_t>( icnType ) << 16 ) | ( icnIndex & 0xFFFF );
    }

    struct ObjectLocation
    {
        Maps::ObjectGroup group{ Maps::ObjectGroup::NONE };
        uint32_t index{ 0 };
    };

    // For every object known to the engine the ICN source of its main (origin) part is mapped to
    // the object's group and index. A legacy map stores an object as a set of individual parts
    // scattered across the tiles it occupies; the origin part always sits on the object's anchor
    // tile. Finding this part on a tile therefore lets us place the whole modern object there and
    // let the engine recreate the remaining parts.
    std::map<uint32_t, ObjectLocation> buildObjectLookupTable()
    {
        std::map<uint32_t, ObjectLocation> lookup;

        for ( size_t groupId = static_cast<size_t>( Maps::ObjectGroup::ROADS ); groupId < static_cast<size_t>( Maps::ObjectGroup::GROUP_COUNT ); ++groupId ) {
            const auto group = static_cast<Maps::ObjectGroup>( groupId );
            const std::vector<Maps::ObjectInfo> & objects = Maps::getObjectsByGroup( group );

            for ( size_t index = 0; index < objects.size(); ++index ) {
                const auto & parts = objects[index].groundLevelParts;
                if ( parts.empty() ) {
                    continue;
                }

                // The main object part always comes first and is placed on the object's origin tile.
                const auto & mainPart = parts.front();
                if ( mainPart.icnType == MP2::OBJ_ICN_TYPE_UNKNOWN || mainPart.icnIndex > 255 ) {
                    // Such a part can never be encoded in the legacy map format.
                    continue;
                }

                // Keep the first object registered for a given key so that objects from the earlier
                // (landscape) groups take precedence over later ones in the rare case of a collision.
                lookup.try_emplace( makePartKey( mainPart.icnType, mainPart.icnIndex ), ObjectLocation{ group, static_cast<uint32_t>( index ) } );
            }
        }

        return lookup;
    }

    // Places an object into the map format and creates the mandatory metadata entry the Editor
    // expects for this object kind (mirrors Maps::addObjectToMap from the engine).
    void addObject( Maps::Map_Format::MapFormat & map, const int32_t tileId, const Maps::ObjectGroup group, const uint32_t index, const uint32_t uid )
    {
        auto & object = map.tiles[tileId].objects.emplace_back();
        object.id = uid;
        object.group = group;
        object.index = index;

        const Maps::ObjectInfo & info = Maps::getObjectInfo( group, static_cast<int32_t>( index ) );

        using Maps::ObjectGroup;

        if ( group == ObjectGroup::KINGDOM_HEROES ) {
            map.heroMetadata[uid].race = Race::IndexToRace( static_cast<int>( info.metadata[1] ) );
        }
        else if ( group == ObjectGroup::KINGDOM_TOWNS ) {
            map.castleMetadata[uid].builtBuildings.push_back( info.metadata[1] == 0 ? BUILD_TENT : BUILD_CASTLE );
        }
        else if ( group == ObjectGroup::ADVENTURE_MISCELLANEOUS && info.objectType == MP2::OBJ_JAIL ) {
            // A jailed hero has a random race by default.
            map.heroMetadata[uid].race = Race::RAND;
        }
        else if ( group == ObjectGroup::MONSTERS ) {
            map.standardMetadata[uid];
        }
        else if ( group == ObjectGroup::ADVENTURE_MISCELLANEOUS ) {
            switch ( info.objectType ) {
            case MP2::OBJ_EVENT:
                map.adventureMapEventMetadata[uid];
                break;
            case MP2::OBJ_SIGN:
                map.signMetadata[uid];
                break;
            case MP2::OBJ_SPHINX:
                map.sphinxMetadata[uid];
                break;
            default:
                break;
            }
        }
        else if ( group == ObjectGroup::ADVENTURE_WATER && info.objectType == MP2::OBJ_BOTTLE ) {
            map.signMetadata[uid];
        }
        else if ( group == ObjectGroup::ADVENTURE_ARTIFACTS ) {
            map.standardMetadata[uid];
        }
    }

    bool isSupportedMapSize( const uint32_t size )
    {
        return size == Maps::SMALL || size == Maps::MEDIUM || size == Maps::LARGE || size == Maps::XLARGE;
    }

    bool convertMap( const std::string & inputPath, const std::string & outputPath )
    {
        // Read the map header (name, description, difficulty, players ...) using the engine's
        // well tested reader. It also validates that the file is a supported legacy map.
        Maps::FileInfo fileInfo;
        if ( !fileInfo.readMP2Map( inputPath, true ) ) {
            std::cerr << "Error: " << inputPath << " is not a valid legacy map file." << std::endl;
            return false;
        }

        // Re-open the file to read the raw tile and add-on data.
        StreamFile stream;
        if ( !stream.open( inputPath, "rb" ) ) {
            std::cerr << "Error: cannot open " << inputPath << std::endl;
            return false;
        }

        if ( stream.getBE32() != 0x5C000000 ) {
            std::cerr << "Error: " << inputPath << " is not a valid legacy map file." << std::endl;
            return false;
        }

        const size_t totalFileSize = stream.size();
        if ( totalFileSize < MP2::MP2_MAP_INFO_SIZE ) {
            std::cerr << "Error: " << inputPath << " is corrupted." << std::endl;
            return false;
        }

        // The real map dimensions are stored at the very end of the map info section.
        stream.seek( MP2::MP2_MAP_INFO_SIZE - 2 * 4 );
        const uint32_t mapWidth = stream.getLE32();
        const uint32_t mapHeight = stream.getLE32();

        if ( mapWidth != mapHeight || !isSupportedMapSize( mapWidth ) ) {
            std::cerr << "Error: " << inputPath << " has unsupported map dimensions [" << mapWidth << " x " << mapHeight << "]." << std::endl;
            return false;
        }

        const int32_t width = static_cast<int32_t>( mapWidth );
        const int32_t worldSize = width * width;

        if ( totalFileSize < MP2::MP2_MAP_INFO_SIZE + static_cast<size_t>( worldSize ) * MP2::MP2_TILE_STRUCTURE_SIZE + MP2::MP2_ADDON_COUNT_SIZE ) {
            std::cerr << "Error: " << inputPath << " is corrupted." << std::endl;
            return false;
        }

        // The add-on structures follow the tile structures.
        stream.seek( MP2::MP2_MAP_INFO_SIZE + static_cast<size_t>( worldSize ) * MP2::MP2_TILE_STRUCTURE_SIZE );
        const size_t addonCount = stream.getLE32();

        std::vector<MP2::MP2AddonInfo> addons( addonCount );
        for ( auto & addon : addons ) {
            MP2::loadAddon( stream, addon );
        }

        // Rewind to the tile section.
        stream.seek( MP2::MP2_MAP_INFO_SIZE );

        const std::map<uint32_t, ObjectLocation> lookup = buildObjectLookupTable();

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

        // Note: victory / loss conditions are intentionally left at their defaults ("defeat all
        // enemies" / "lose all towns and heroes"). The legacy condition encoding cannot be mapped
        // onto the modern format one-to-one and has to be re-created in the Editor if required.

        map.tiles.resize( worldSize );

        // Every object is placed once - at the tile that holds its origin part. A legacy object
        // UID is reused as the modern UID (shifted by one, because the modern format requires
        // non-zero UIDs). Note that this deliberately preserves the legacy convention where an
        // object and its decorations share a single UID: a town, its basement and its flags are
        // separate objects that all carry the town's UID.
        std::set<uint64_t> placedObjects;
        size_t objectPartsFound = 0;
        size_t objectsPlaced = 0;

        const auto tryPlacePart = [&]( const int32_t tileId, const MP2::ObjectIcnType icnType, const uint32_t icnIndex, const uint32_t mp2Uid ) {
            if ( icnType == MP2::OBJ_ICN_TYPE_UNKNOWN || mp2Uid == UINT32_MAX ) {
                return;
            }

            ++objectPartsFound;

            const auto iter = lookup.find( makePartKey( icnType, icnIndex ) );
            if ( iter == lookup.end() ) {
                // Not an origin part of a known object - some other tile holds this object's origin.
                return;
            }

            // Guard against placing the very same object twice should its origin sprite happen to
            // appear on more than one of its tiles.
            const uint64_t placementKey = ( static_cast<uint64_t>( mp2Uid ) << 32 )
                                          | ( static_cast<uint64_t>( iter->second.group ) << 24 ) | iter->second.index;
            if ( !placedObjects.insert( placementKey ).second ) {
                return;
            }

            addObject( map, tileId, iter->second.group, iter->second.index, mp2Uid + 1 );
            ++objectsPlaced;
        };

        for ( int32_t i = 0; i < worldSize; ++i ) {
            MP2::MP2TileInfo mp2tile;
            MP2::loadTile( stream, mp2tile );

            Maps::Map_Format::TileInfo & tile = map.tiles[i];
            tile.terrainIndex = mp2tile.terrainImageIndex;
            tile.terrainFlags = mp2tile.terrainFlags & 0x03;

            // Object parts stored directly in the tile structure.
            tryPlacePart( i, static_cast<MP2::ObjectIcnType>( mp2tile.objectName1 >> 2 ), mp2tile.bottomIcnImageIndex, mp2tile.level1ObjectUID );
            tryPlacePart( i, static_cast<MP2::ObjectIcnType>( mp2tile.objectName2 >> 2 ), mp2tile.topIcnImageIndex, mp2tile.level2ObjectUID );

            // Object parts stored in the tile's linked list of add-ons.
            size_t addonIndex = mp2tile.nextAddonIndex;
            while ( addonIndex > 0 && addonIndex < addons.size() ) {
                const MP2::MP2AddonInfo & addon = addons[addonIndex];
                tryPlacePart( i, static_cast<MP2::ObjectIcnType>( addon.objectNameN1 >> 2 ), addon.bottomIcnImageIndex, addon.level1ObjectUID );
                tryPlacePart( i, static_cast<MP2::ObjectIcnType>( addon.objectNameN2 >> 2 ), addon.topIcnImageIndex, addon.level2ObjectUID );
                addonIndex = addon.nextAddonIndex;
            }
        }

        if ( !Maps::Map_Format::saveMap( outputPath, map ) ) {
            std::cerr << "Error: failed to write " << outputPath << std::endl;
            return false;
        }

        std::cout << "Converted '" << fileInfo.name << "'" << std::endl
                  << "  " << inputPath << " -> " << outputPath << std::endl
                  << "  Map size:      " << width << " x " << width << std::endl
                  << "  Objects found: " << objectPartsFound << " parts" << std::endl
                  << "  Objects placed: " << objectsPlaced << std::endl;

        return true;
    }
}

int main( int argc, char ** argv )
{
    if ( argc < 2 ) {
        const std::string toolName = System::GetFileName( argv[0] );

        std::cerr << toolName << " converts a legacy map (.mp2, .mx2, .h2c, .hxc) to the modern fheroes2 map format (.fh2m)." << std::endl
                  << "Syntax: " << toolName << " input_map [output_map.fh2m] ..." << std::endl
                  << "        When several input maps are given each one is converted next to the original file." << std::endl;
        return EXIT_FAILURE;
    }

    // A single input followed by an explicit output path.
    if ( argc == 3 ) {
        const std::filesystem::path outputPath( argv[2] );
        if ( outputPath.extension() == ".fh2m" ) {
            return convertMap( argv[1], outputPath.string() ) ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }

    bool everythingConverted = true;

    for ( int i = 1; i < argc; ++i ) {
        std::filesystem::path outputPath( argv[i] );
        outputPath.replace_extension( ".fh2m" );

        if ( !convertMap( argv[i], outputPath.string() ) ) {
            everythingConverted = false;
        }
    }

    return everythingConverted ? EXIT_SUCCESS : EXIT_FAILURE;
}
