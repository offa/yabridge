// yabridge: a Wine plugin bridge
// Copyright (C) 2020-2022 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <vector>

#include <pluginterfaces/vst/ivstphysicalui.h>

/**
 * Serialization wrapper around `PhysicalUIMapList` that allows loading such a
 * list and writing the changes made by the plugin back to the original list.
 * The host provides a list with the `physicalUITypeID` field set for each
 * mapping, and the plugin then sets the `noteExpressionTypeID` to one of its
 * note expression of it can handle it.
 */
class YaPhysicalUIMapList {
   public:
    YaPhysicalUIMapList() noexcept;

    /**
     * Copy the data from a `PhysicalUIMapList` so it can be serialized.
     */
    YaPhysicalUIMapList(const Steinberg::Vst::PhysicalUIMapList& list) noexcept;

    /**
     * Reconstruct the original `PhysicalUIMapList` object passed to the
     * constructor and return it. This is used to handle
     * `INoteExpressionPhysicalUIMapping::getPhysicalUIMapping()` on the Wine
     * plugin host side. The returned object is valid as long as this object is
     * alive.
     */
    Steinberg::Vst::PhysicalUIMapList get() noexcept;

    /**
     * Write the `noteExpressionTypeID` values stored in `maps` back to the
     * original physical UI mapping list we copied `maps` from.
     */
    void write_back(Steinberg::Vst::PhysicalUIMapList& list) const;

    template <typename S>
    void serialize(S& s) {
        s.container(maps_, 1 << 31);
    }

    std::vector<Steinberg::Vst::PhysicalUIMap> maps_;
};

namespace Steinberg {
namespace Vst {
template <typename S>
void serialize(S& s, PhysicalUIMap map) {
    s.value4b(map.physicalUITypeID);
    s.value4b(map.noteExpressionTypeID);
}
}  // namespace Vst
}  // namespace Steinberg
