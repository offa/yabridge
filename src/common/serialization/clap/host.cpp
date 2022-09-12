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

#include "host.h"

#include <clap/ext/audio-ports.h>

namespace clap {
namespace host {

Host::Host(const clap_host_t& original)
    : clap_version(original.clap_version),
      name((assert(original.name), original.name)),
      vendor(original.vendor ? std::optional(original.vendor) : std::nullopt),
      url(original.url ? std::optional(original.url) : std::nullopt),
      version((assert(original.version), original.version)) {}

SupportedHostExtensions::SupportedHostExtensions(const clap_host& host)
    : supports_audio_ports(host.get_extension(&host, CLAP_EXT_AUDIO_PORTS)) {}

}  // namespace host
}  // namespace clap
