/******************************************************************************
    Copyright (C) 2026 by OBS Studio Pro contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#include <string>

namespace OBS::Output {

// The transport implementation consumes this provider-neutral description.
// Provider adapters decide which source is appropriate for a session.
enum class MultitrackConfigSource {
	RemoteProviderUrl,
	CustomJson,
	StaticJson,
};

struct MultitrackConfigProvider {
	MultitrackConfigSource source = MultitrackConfigSource::RemoteProviderUrl;
	std::string value;

	bool IsRemote() const { return source == MultitrackConfigSource::RemoteProviderUrl; }
	bool IsJson() const
	{
		return source == MultitrackConfigSource::CustomJson || source == MultitrackConfigSource::StaticJson;
	}
};

const char *ToString(MultitrackConfigSource source);
MultitrackConfigSource MultitrackConfigSourceFromString(const std::string &value);

} // namespace OBS::Output
