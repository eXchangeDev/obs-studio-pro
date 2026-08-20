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

#include "MultitrackConfigProvider.hpp"

namespace OBS::Output {

const char *ToString(MultitrackConfigSource source)
{
	switch (source) {
	case MultitrackConfigSource::RemoteProviderUrl:
		return "remote_provider_url";
	case MultitrackConfigSource::CustomJson:
		return "custom_json";
	case MultitrackConfigSource::StaticJson:
		return "static_json";
	}

	return "remote_provider_url";
}

MultitrackConfigSource MultitrackConfigSourceFromString(const std::string &value)
{
	if (value == "custom_json") {
		return MultitrackConfigSource::CustomJson;
	}
	if (value == "static_json") {
		return MultitrackConfigSource::StaticJson;
	}
	return MultitrackConfigSource::RemoteProviderUrl;
}

} // namespace OBS::Output
