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

#pragma once

#include <cstddef>
#include <string>

namespace fheroes2
{
    class Sprite;
}

// Custom heroes are the heroes defined by external portrait files (not to be confused with the Heroes::CUSTOM mode which
// means that a hero was customized by a map maker). Portraits are looked up in the `files/images/portraits/<faction>/`
// directories of every engine's root data directory, where <faction> is one of:
//   knight, barbarian, sorceress, warlock, wizard, necromancer.
//
// The file name without extension is the hero's default name (it is passed through the translation system, so it can be
// translated using a regular .po file) and the directory defines the hero's race. So renaming a file renames the hero and
// adding a file adds a new hero. Supported formats are PNG (requires ENABLE_IMAGE=ON) and BMP.
//
// Custom heroes are ordered by race (in the order above) and then by name. The hero with index N in this list has the
// Heroes::FIRST_CUSTOM_HERO + N hero ID. Therefore, adding, removing or renaming portrait files changes the IDs of other
// custom heroes, which affects existing save files and maps that refer to these heroes.
namespace CustomHeroes
{
    // The directories are scanned only once, during the first call of any of these functions.
    size_t getCount();

    // Returns the hero's default (untranslated) name.
    const std::string & getName( const size_t index );

    int getRace( const size_t index );

    // Returns a big portrait (the same size as the original ones). It is loaded on the first request.
    const fheroes2::Sprite & getPortrait( const size_t index );

    // Returns a mini-portrait used for small hero icons.
    const fheroes2::Sprite & getSmallPortrait( const size_t index );
}
