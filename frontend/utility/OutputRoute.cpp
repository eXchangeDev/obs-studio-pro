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
	if (value == "recording") {
		return Kind::Recording;
	}
	if (value == "virtual_camera") {
		return Kind::VirtualCamera;
	}
	return Kind::Stream;
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
	}

	return "none";
}

FailoverMode FailoverModeFromString(const std::string &value)
{
	if (value == "client_sequential") {
		return FailoverMode::ClientSequential;
	}
	if (value == "client_parallel") {
		return FailoverMode::ClientParallel;
	}
	return FailoverMode::None;
}

void AppendErrors(std::vector<std::string> &target, std::vector<std::string> source, const std::string &prefix)
{
	for (auto &error : source) {
		target.emplace_back(prefix + error);
	}
}

struct CanvasResolverContext {
	const CanvasReference &reference;
	obs_canvas_t *match = nullptr;
};

bool ResolveCanvasCallback(void *data, obs_canvas_t *canvas)
{
	auto *context = static_cast<CanvasResolverContext *>(data);
	if (!CanvasReferenceMatches(context->reference, canvas)) {
		return true;
	}

	context->match = obs_canvas_get_ref(canvas);
	return false;
}

} // namespace

CanvasReference CanvasReferenceFromCanvas(const obs_canvas_t *canvas)
{
	CanvasReference reference;
	if (!canvas) {
		return reference;
	}

	if (const char *uuid = obs_canvas_get_uuid(canvas)) {
		reference.uuid = uuid;
	}
	if (const char *name = obs_canvas_get_name(canvas)) {
		reference.name = name;
	}

	return reference;
}

bool CanvasReferenceMatches(const CanvasReference &reference, const obs_canvas_t *canvas)
{
	if (!canvas) {
		return false;
	}

	const char *uuid = obs_canvas_get_uuid(canvas);
	if (!reference.uuid.empty() && uuid) {
		return reference.uuid == uuid;
	}

	const char *name = obs_canvas_get_name(canvas);
	return !reference.name.empty() && name && reference.name == name;
}

obs_canvas_t *ResolveCanvas(const CanvasReference &reference)
{
	if (reference.uuid.empty() && reference.name.empty()) {
		return nullptr;
	}

	CanvasResolverContext context{reference};
	obs_enum_canvases(ResolveCanvasCallback, &context);
	return context.match;
}

std::vector<std::string> Validate(const Destination &destination)
{
	std::vector<std::string> errors;

	if (destination.id.empty()) {
		errors.emplace_back("destination id is empty");
	}
	if (destination.name.empty()) {
		errors.emplace_back("destination name is empty");
	}
	if (destination.service.empty()) {
		errors.emplace_back("destination service is empty");
	}
	if (destination.server.empty()) {
		errors.emplace_back("destination server is empty");
	}

	return errors;
}

std::vector<std::string> Validate(const Route &route)
{
	std::vector<std::string> errors;

	if (route.id.empty()) {
		errors.emplace_back("route id is empty");
	}
	if (route.name.empty()) {
		errors.emplace_back("route name is empty");
	}
	if (route.canvas.uuid.empty() && route.canvas.name.empty()) {
		errors.emplace_back("route has no canvas reference");
	}
	if (route.audioMix >= MAX_AUDIO_MIXES) {
		errors.emplace_back("route audio mix is out of range");
	}
	std::unordered_set<std::string> destinationIds;
	for (size_t index = 0; index < route.destinations.size(); ++index) {
		const auto &destination = route.destinations[index];
		if (!destination.id.empty() && !destinationIds.insert(destination.id).second) {
			errors.emplace_back("duplicate destination id: " + destination.id);
		}
		AppendErrors(errors, Validate(destination), "destination[" + std::to_string(index) + "]: ");
	}

	return errors;
}

std::vector<std::string> Validate(const RouteSet &routes)
{
	std::vector<std::string> errors;

	if (routes.schemaVersion != RouteSchemaVersion) {
		errors.emplace_back("unsupported route schema version");
	}

	std::unordered_set<std::string> routeIds;
	size_t primaryRoutes = 0;
	for (size_t index = 0; index < routes.routes.size(); ++index) {
		const auto &route = routes.routes[index];
		if (route.primary) {
			++primaryRoutes;
		}
		if (!route.id.empty() && !routeIds.insert(route.id).second) {
			errors.emplace_back("duplicate route id: " + route.id);
		}
		AppendErrors(errors, Validate(route), "route[" + std::to_string(index) + "]: ");
	}
	if (!routes.routes.empty() && primaryRoutes != 1) {
		errors.emplace_back("route set must contain exactly one primary route");
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
		     {"service", destination.service},
		     {"service_name", destination.serviceName},
		     {"server", destination.server},
		     {"stream_key", destination.streamKey},
		     {"username", destination.username},
		     {"password", destination.password},
		     {"service_settings", destination.serviceSettingsJson},
		     {"priority", destination.priority},
		     {"use_authentication", destination.useAuthentication},
		     {"enabled", destination.enabled}};
}

void from_json(const json &value, Destination &destination)
{
	destination.id = value.value("id", std::string{});
	destination.name = value.value("name", std::string{});
	destination.service = value.value("service", std::string{"rtmp_custom"});
	destination.serviceName = value.value("service_name", std::string{});
	destination.server = value.value("server", std::string{});
	destination.streamKey = value.value("stream_key", std::string{});
	destination.username = value.value("username", std::string{});
	destination.password = value.value("password", std::string{});
	destination.serviceSettingsJson = value.value("service_settings", std::string{});
	destination.priority = value.value("priority", 0U);
	destination.useAuthentication = value.value("use_authentication", false);
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
		     {"video_encoder_settings", route.videoEncoderSettingsJson},
		     {"audio_encoder_settings", route.audioEncoderSettingsJson},
		     {"audio_mix", route.audioMix},
		     {"failover_mode", ToString(route.failoverMode)},
		     {"destinations", route.destinations},
		     {"enabled", route.enabled},
		     {"primary", route.primary}};
}

void from_json(const json &value, Route &route)
{
	route.id = value.value("id", std::string{});
	route.name = value.value("name", std::string{});
	route.kind = KindFromString(value.value("kind", std::string{"stream"}));
	route.canvas = value.value("canvas", CanvasReference{});
	route.videoEncoderId = value.value("video_encoder_id", std::string{});
	route.audioEncoderId = value.value("audio_encoder_id", std::string{});
	route.videoEncoderSettingsJson = value.value("video_encoder_settings", std::string{});
	route.audioEncoderSettingsJson = value.value("audio_encoder_settings", std::string{});
	route.audioMix = value.value("audio_mix", 0U);
	route.failoverMode = FailoverModeFromString(value.value("failover_mode", std::string{"none"}));
	route.destinations = value.value("destinations", std::vector<Destination>{});
	route.enabled = value.value("enabled", true);
	route.primary = value.value("primary", false);
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

std::string Serialize(const RouteSet &routes)
{
	return json(routes).dump();
}

bool Deserialize(std::string_view value, RouteSet &routes, std::string &error)
{
	try {
		routes = json::parse(value).get<RouteSet>();
		if (routes.schemaVersion == 1) {
			// Schema v1 grouped destinations by canvas and had no explicit
			// primary route.  The integrated Settings controller reconciles
			// the route with the live main-canvas UUID; choosing the first
			// route here keeps deserialization independent of a running OBS
			// instance.
			if (!routes.routes.empty()) {
				routes.routes.front().primary = true;
			}
			routes.schemaVersion = RouteSchemaVersion;
		} else if (routes.schemaVersion != RouteSchemaVersion) {
			error = "unsupported route schema version " + std::to_string(routes.schemaVersion);
			return false;
		}
		error.clear();
		return true;
	} catch (const json::exception &exception) {
		error = exception.what();
		return false;
	}
}

} // namespace OBS::Output
