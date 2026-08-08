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
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <obs.h>

namespace OBS::Output {

constexpr uint32_t RouteSchemaVersion = 1;

enum class Kind {
	Stream,
	Recording,
	VirtualCamera,
};

enum class DestinationMode {
	Direct,
	ManagedRoute,
};

enum class FailoverMode {
	None,
	ClientSequential,
	ClientParallel,
	ServerManaged,
};

struct CanvasReference {
	std::string uuid;
	std::string name;
};

struct Destination {
	std::string id;
	std::string name;
	DestinationMode mode = DestinationMode::Direct;

	// Direct destinations are resolved through OBS' existing service layer.
	std::string service;
	std::string server;

	// Managed destinations are resolved by an external routing/control plane.
	// Credentials intentionally do not live in this model.
	std::string managedRouteId;

	uint32_t priority = 0;
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
	uint32_t audioMix = 0;

	FailoverMode failoverMode = FailoverMode::None;
	std::vector<Destination> destinations;
	bool enabled = true;
};

struct RouteSet {
	uint32_t schemaVersion = RouteSchemaVersion;
	std::vector<Route> routes;
};

CanvasReference CanvasReferenceFromCanvas(const obs_canvas_t *canvas);
bool CanvasReferenceMatches(const CanvasReference &reference, const obs_canvas_t *canvas);

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

} // namespace OBS::Output
