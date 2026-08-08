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

#include "OutputRoute.hpp"

#include <algorithm>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace OBS::Output {
namespace {

using json = nlohmann::json;

const char *ToString(Kind kind)
{
	switch (kind) {
	case Kind::Stream:
		return "stream";
	case Kind::Recording:
		return "recording";
	case Kind::VirtualCamera:
		return "virtual_camera";
	}

	return "stream";
}

Kind KindFromString(const std::string &value)
{
	if (value == "recording")
		return Kind::Recording;
	if (value == "virtual_camera")
		return Kind::VirtualCamera;
	return Kind::Stream;
}

const char *ToString(DestinationMode mode)
{
	switch (mode) {
	case DestinationMode::Direct:
		return "direct";
	case DestinationMode::ManagedRoute:
		return "managed_route";
	}

	return "direct";
}

DestinationMode DestinationModeFromString(const std::string &value)
{
	return value == "managed_route" ? DestinationMode::ManagedRoute : DestinationMode::Direct;
}

const char *ToString(FailoverMode mode)
{
	switch (mode) {
	case FailoverMode::None:
		return "none";
	case FailoverMode::ClientSequential:
		return "client_sequential";
	case FailoverMode::ClientParallel:
		return "client_parallel";
	case FailoverMode::ServerManaged:
		return "server_managed";
	}

	return "none";
}

FailoverMode FailoverModeFromString(const std::string &value)
{
	if (value == "client_sequential")
		return FailoverMode::ClientSequential;
	if (value == "client_parallel")
		return FailoverMode::ClientParallel;
	if (value == "server_managed")
		return FailoverMode::ServerManaged;
	return FailoverMode::None;
}

void AppendErrors(std::vector<std::string> &target, std::vector<std::string> source, const std::string &prefix)
{
	for (auto &error : source)
		target.emplace_back(prefix + error);
}

} // namespace

CanvasReference CanvasReferenceFromCanvas(const obs_canvas_t *canvas)
{
	CanvasReference reference;
	if (!canvas)
		return reference;

	if (const char *uuid = obs_canvas_get_uuid(canvas))
		reference.uuid = uuid;
	if (const char *name = obs_canvas_get_name(canvas))
		reference.name = name;

	return reference;
}

bool CanvasReferenceMatches(const CanvasReference &reference, const obs_canvas_t *canvas)
{
	if (!canvas)
		return false;

	const char *uuid = obs_canvas_get_uuid(canvas);
	if (!reference.uuid.empty() && uuid)
		return reference.uuid == uuid;

	const char *name = obs_canvas_get_name(canvas);
	return !reference.name.empty() && name && reference.name == name;
}

std::vector<std::string> Validate(const Destination &destination)
{
	std::vector<std::string> errors;

	if (destination.id.empty())
		errors.emplace_back("destination id is empty");
	if (destination.name.empty())
		errors.emplace_back("destination name is empty");

	if (destination.mode == DestinationMode::Direct) {
		if (destination.service.empty())
			errors.emplace_back("direct destination service is empty");
		if (destination.managedRouteId.size())
			errors.emplace_back("direct destination has a managed route id");
	} else {
		if (destination.managedRouteId.empty())
			errors.emplace_back("managed destination route id is empty");
	}

	return errors;
}

std::vector<std::string> Validate(const Route &route)
{
	std::vector<std::string> errors;

	if (route.id.empty())
		errors.emplace_back("route id is empty");
	if (route.name.empty())
		errors.emplace_back("route name is empty");
	if (route.canvas.uuid.empty() && route.canvas.name.empty())
		errors.emplace_back("route has no canvas reference");
	if (route.audioMix >= MAX_AUDIO_MIXES)
		errors.emplace_back("route audio mix is out of range");
	if (route.kind == Kind::Stream && route.destinations.empty())
		errors.emplace_back("stream route has no destinations");

	std::unordered_set<std::string> destinationIds;
	for (size_t index = 0; index < route.destinations.size(); ++index) {
		const auto &destination = route.destinations[index];
		if (!destination.id.empty() && !destinationIds.insert(destination.id).second)
			errors.emplace_back("duplicate destination id: " + destination.id);
		AppendErrors(errors, Validate(destination), "destination[" + std::to_string(index) + "]: ");
	}

	if (route.failoverMode == FailoverMode::ServerManaged) {
		const bool hasManagedDestination = std::any_of(route.destinations.begin(), route.destinations.end(),
							 [](const Destination &destination) {
								 return destination.mode == DestinationMode::ManagedRoute;
							 });
		if (!hasManagedDestination)
			errors.emplace_back("server-managed failover requires a managed destination");
	}

	return errors;
}

std::vector<std::string> Validate(const RouteSet &routes)
{
	std::vector<std::string> errors;

	if (routes.schemaVersion != RouteSchemaVersion)
		errors.emplace_back("unsupported route schema version");

	std::unordered_set<std::string> routeIds;
	for (size_t index = 0; index < routes.routes.size(); ++index) {
		const auto &route = routes.routes[index];
		if (!route.id.empty() && !routeIds.insert(route.id).second)
			errors.emplace_back("duplicate route id: " + route.id);
		AppendErrors(errors, Validate(route), "route[" + std::to_string(index) + "]: ");
	}

	return errors;
}

void to_json(json &value, const CanvasReference &reference)
{
	value = json{{"uuid", reference.uuid}, {"name", reference.name}};
}

void from_json(const json &value, CanvasReference &reference)
{
	reference.uuid = value.value("uuid", std::string{});
	reference.name = value.value("name", std::string{});
}

void to_json(json &value, const Destination &destination)
{
	value = json{{"id", destination.id},
		     {"name", destination.name},
		     {"mode", ToString(destination.mode)},
		     {"service", destination.service},
		     {"server", destination.server},
		     {"managed_route_id", destination.managedRouteId},
		     {"priority", destination.priority},
		     {"enabled", destination.enabled}};
}

void from_json(const json &value, Destination &destination)
{
	destination.id = value.value("id", std::string{});
	destination.name = value.value("name", std::string{});
	destination.mode = DestinationModeFromString(value.value("mode", std::string{"direct"}));
	destination.service = value.value("service", std::string{});
	destination.server = value.value("server", std::string{});
	destination.managedRouteId = value.value("managed_route_id", std::string{});
	destination.priority = value.value("priority", 0U);
	destination.enabled = value.value("enabled", true);
}

void to_json(json &value, const Route &route)
{
	value = json{{"id", route.id},
		     {"name", route.name},
		     {"kind", ToString(route.kind)},
		     {"canvas", route.canvas},
		     {"video_encoder_id", route.videoEncoderId},
		     {"audio_encoder_id", route.audioEncoderId},
		     {"audio_mix", route.audioMix},
		     {"failover_mode", ToString(route.failoverMode)},
		     {"destinations", route.destinations},
		     {"enabled", route.enabled}};
}

void from_json(const json &value, Route &route)
{
	route.id = value.value("id", std::string{});
	route.name = value.value("name", std::string{});
	route.kind = KindFromString(value.value("kind", std::string{"stream"}));
	route.canvas = value.value("canvas", CanvasReference{});
	route.videoEncoderId = value.value("video_encoder_id", std::string{});
	route.audioEncoderId = value.value("audio_encoder_id", std::string{});
	route.audioMix = value.value("audio_mix", 0U);
	route.failoverMode = FailoverModeFromString(value.value("failover_mode", std::string{"none"}));
	route.destinations = value.value("destinations", std::vector<Destination>{});
	route.enabled = value.value("enabled", true);
}

void to_json(json &value, const RouteSet &routes)
{
	value = json{{"schema_version", routes.schemaVersion}, {"routes", routes.routes}};
}

void from_json(const json &value, RouteSet &routes)
{
	routes.schemaVersion = value.value("schema_version", RouteSchemaVersion);
	routes.routes = value.value("routes", std::vector<Route>{});
}

} // namespace OBS::Output
