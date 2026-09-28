/***************************************************************************
 *   fheroes2: https://github.com/ihhub/fheroes2                           *
 *   Copyright (C) 2026                                                    *
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

#include "custom_heroes.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "dir.h"
#include "heroes.h"
#include "image.h"
#include "image_tool.h"
#include "logging.h"
#include "race.h"
#include "settings.h"
#include "system.h"
#include "tools.h"

namespace
{
    // The size of the original big hero portraits (ICN::PORTxxxx).
    const int32_t portraitWidth{ 101 };
    const int32_t portraitHeight{ 93 };

    // The size of the original mini-portraits (ICN::MINIPORT).
    const int32_t smallPortraitWidth{ 30 };
    const int32_t smallPortraitHeight{ 22 };

    // Maps::Tile stores the ID of a hero standing on it as uint8_t, so the total number of heroes cannot exceed 256.
    const size_t maxCustomHeroesCount{ 256 - Heroes::FIRST_CUSTOM_HERO };

    const std::array<std::pair<int, const char *>, 6> raceDirectories{ { { Race::KNGT, "knight" },
                                                                         { Race::BARB, "barbarian" },
                                                                         { Race::SORC, "sorceress" },
                                                                         { Race::WRLK, "warlock" },
                                                                         { Race::WZRD, "wizard" },
                                                                         { Race::NECR, "necromancer" } } };

    struct CustomHero
    {
        std::string name;
        int race{ Race::NONE };
        std::string portraitPath;

        bool isPortraitLoaded{ false };
        fheroes2::Sprite portrait;
        fheroes2::Sprite smallPortrait;
    };

    std::vector<CustomHero> findCustomHeroes()
    {
        const std::string portraitsDirectory = System::concatPath( System::concatPath( "files", "images" ), "portraits" );

        std::vector<CustomHero> heroes;

        for ( const auto & [race, raceDirectory] : raceDirectories ) {
            // Heroes of the same race are sorted by name, case-insensitive. If the same name exists in several root directories,
            // the file from the directory with the highest priority is used, just like Settings::findFile() does.
            std::map<std::string, CustomHero> raceHeroes;

            for ( const char * extension : { ".png", ".bmp" } ) {
                for ( std::string & path : Settings::FindFiles( System::concatPath( portraitsDirectory, raceDirectory ), extension, false ) ) {
                    std::string name = System::GetStem( path );
                    if ( name.empty() ) {
                        continue;
                    }

                    std::string key = StringLower( name );

                    CustomHero hero;
                    hero.name = std::move( name );
                    hero.race = race;
                    hero.portraitPath = std::move( path );

                    raceHeroes.try_emplace( std::move( key ), std::move( hero ) );
                }
            }

            for ( auto & [key, hero] : raceHeroes ) {
                heroes.emplace_back( std::move( hero ) );
            }
        }

        if ( heroes.size() > maxCustomHeroesCount ) {
            ERROR_LOG( "Too many custom hero portraits: " << heroes.size() << ", only the first " << maxCustomHeroesCount << " will be used." )
            heroes.resize( maxCustomHeroesCount );
        }

        VERBOSE_LOG( "Found " << heroes.size() << " custom heroes in '" << portraitsDirectory << "' directories." )

        return heroes;
    }

    std::vector<CustomHero> & getCustomHeroes()
    {
        static std::vector<CustomHero> heroes = findCustomHeroes();
        return heroes;
    }

    CustomHero & getLoadedCustomHero( const size_t index )
    {
        std::vector<CustomHero> & heroes = getCustomHeroes();
        assert( index < heroes.size() );

        CustomHero & hero = heroes[index];
        if ( hero.isPortraitLoaded ) {
            return hero;
        }

        hero.isPortraitLoaded = true;

        fheroes2::Sprite image;
        if ( !fheroes2::Load( hero.portraitPath, image ) || image.empty() ) {
            ERROR_LOG( "Failed to load custom hero portrait: " << hero.portraitPath )
            return hero;
        }

        if ( image.width() == portraitWidth && image.height() == portraitHeight ) {
            hero.portrait = std::move( image );
        }
        else {
            // All interface elements are designed for the original portrait size.
            hero.portrait.resize( portraitWidth, portraitHeight );
            fheroes2::Resize( image, hero.portrait );
        }

        hero.smallPortrait.resize( smallPortraitWidth, smallPortraitHeight );
        fheroes2::Resize( hero.portrait, hero.smallPortrait );

        return hero;
    }
}

size_t CustomHeroes::getCount()
{
    return getCustomHeroes().size();
}

const std::string & CustomHeroes::getName( const size_t index )
{
    const std::vector<CustomHero> & heroes = getCustomHeroes();
    assert( index < heroes.size() );

    return heroes[index].name;
}

int CustomHeroes::getRace( const size_t index )
{
    const std::vector<CustomHero> & heroes = getCustomHeroes();
    assert( index < heroes.size() );

    return heroes[index].race;
}

const fheroes2::Sprite & CustomHeroes::getPortrait( const size_t index )
{
    return getLoadedCustomHero( index ).portrait;
}

const fheroes2::Sprite & CustomHeroes::getSmallPortrait( const size_t index )
{
    return getLoadedCustomHero( index ).smallPortrait;
}
