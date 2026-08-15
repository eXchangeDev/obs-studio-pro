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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <obs.h>

namespace OBS::Output {

constexpr uint32_t RouteSchemaVersion = 2;

enum class Kind {
	Stream,
	Recording,
	VirtualCamera,
};

enum class FailoverMode {
	None,
	ClientSequential,
	ClientParallel,
};

struct CanvasReference {
	std::string uuid;
	std::string name;
};

struct Destination {
	std::string id;
	std::string name;

	// Destinations are resolved through OBS' existing service/output layer. A
	// relay or external failover service is intentionally just another
	// destination from OBS' point of view. The default works for Twitch,
	// YouTube and relay endpoints when their RTMP URL and stream key are known.
	std::string service = "rtmp_custom";
	std::string serviceName;
	std::string server;
	std::string streamKey;
	std::string username;
	std::string password;

	// OBS' native service property view reads and writes the complete service
	// settings object.  Keeping that JSON intact lets additional destinations
	// use the same service definitions as the built-in Stream page instead of
	// maintaining a second, incomplete set of fields.  The legacy fields above
	// remain for schema-v1 migration and human-readable diagnostics.
	std::string serviceSettingsJson;

	uint32_t priority = 0;
	bool useAuthentication = false;
	bool enabled = true;
};

struct Route {
	std::string id;
	std::string name;
	Kind kind = Kind::Stream;
	CanvasReference canvas;

	// Empty encoder IDs mean "use the current profile/default encoder".
	std::string videoEncoderId;
	std::string audioEncoderId;
	std::string videoEncoderSettingsJson;
	std::string audioEncoderSettingsJson;
	uint32_t audioMix = 0;

	// This models failover that OBS itself performs. Any redundancy behind a
	// relay/server endpoint remains an implementation detail of that service.
	FailoverMode failoverMode = FailoverMode::None;
	std::vector<Destination> destinations;
	bool enabled = true;

	// The primary route is backed by OBS' existing Streaming tab.  Additional
	// destinations assigned to it share that encoder.  Every non-primary route
	// owns its own encoder settings tab.
	bool primary = false;
};

struct RouteSet {
	uint32_t schemaVersion = RouteSchemaVersion;
	std::vector<Route> routes;
};

CanvasReference CanvasReferenceFromCanvas(const obs_canvas_t *canvas);
bool CanvasReferenceMatches(const CanvasReference &reference, const obs_canvas_t *canvas);

// Returns a strong reference. Caller must release it with obs_canvas_release().
obs_canvas_t *ResolveCanvas(const CanvasReference &reference);

std::vector<std::string> Validate(const Destination &destination);
std::vector<std::string> Validate(const Route &route);
std::vector<std::string> Validate(const RouteSet &routes);

void to_json(nlohmann::json &json, const CanvasReference &reference);
void from_json(const nlohmann::json &json, CanvasReference &reference);
void to_json(nlohmann::json &json, const Destination &destination);
void from_json(const nlohmann::json &json, Destination &destination);
void to_json(nlohmann::json &json, const Route &route);
void from_json(const nlohmann::json &json, Route &route);
void to_json(nlohmann::json &json, const RouteSet &routes);
void from_json(const nlohmann::json &json, RouteSet &routes);

std::string Serialize(const RouteSet &routes);
bool Deserialize(std::string_view value, RouteSet &routes, std::string &error);

} // namespace OBS::Output
