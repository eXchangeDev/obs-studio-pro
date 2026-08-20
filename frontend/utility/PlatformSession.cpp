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

#include "PlatformSession.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

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

OutputEndpoint EndpointFromDestination(const Destination &destination)
{
	OutputEndpoint endpoint;
	endpoint.id = destination.id;
	endpoint.name = destination.name;
	endpoint.service = destination.service;
	endpoint.serviceName = destination.serviceName;
	endpoint.server = destination.server;
	endpoint.streamKey = destination.streamKey;
	endpoint.username = destination.username;
	endpoint.password = destination.password;
	endpoint.serviceSettingsJson = destination.serviceSettingsJson;
	endpoint.priority = destination.priority;
	endpoint.reconnectRetryCount = destination.reconnectRetryCount;
	endpoint.reconnectRetrySeconds = destination.reconnectRetrySeconds;
	endpoint.reconnectEnabled = destination.reconnectEnabled;
	endpoint.useAuthentication = destination.useAuthentication;
	endpoint.enabled = destination.enabled;
	return endpoint;
}

Destination DestinationFromEndpoint(const OutputEndpoint &endpoint, const std::string &sessionId)
{
	Destination destination;
	destination.id = endpoint.id;
	destination.name = endpoint.name;
	destination.sessionId = sessionId;
	destination.service = endpoint.service;
	destination.serviceName = endpoint.serviceName;
	destination.server = endpoint.server;
	destination.streamKey = endpoint.streamKey;
	destination.username = endpoint.username;
	destination.password = endpoint.password;
	destination.serviceSettingsJson = endpoint.serviceSettingsJson;
	destination.priority = endpoint.priority;
	destination.reconnectRetryCount = endpoint.reconnectRetryCount;
	destination.reconnectRetrySeconds = endpoint.reconnectRetrySeconds;
	destination.reconnectEnabled = endpoint.reconnectEnabled;
	destination.useAuthentication = endpoint.useAuthentication;
	destination.enabled = endpoint.enabled;
	return destination;
}

} // namespace

void PlatformSession::AttachService(obs_service_t *value)
{
	service = value ? obs_service_get_ref(value) : nullptr;
}

void PlatformSession::AttachOutput(obs_output_t *value)
{
	output = value ? obs_output_get_ref(value) : nullptr;
}

void PlatformSession::AttachCapabilityProvider(std::shared_ptr<const PlatformCapabilityProvider> provider)
{
	capabilityProvider = std::move(provider);
	if (capabilityProvider) {
		config.capabilities = capabilityProvider->GetCapabilities();
	}
}

void PlatformSession::SetError(std::string value)
{
	lastError = std::move(value);
	state = SessionRuntimeState::Failed;
}

void PlatformSession::ClearError()
{
	lastError.clear();
	if (state == SessionRuntimeState::Failed) {
		state = SessionRuntimeState::Idle;
	}
}

const char *ToString(DeliveryMode mode)
{
	switch (mode) {
	case DeliveryMode::Standard:
		return "standard";
	case DeliveryMode::EnhancedMultitrack:
		return "enhanced_multitrack";
	case DeliveryMode::PlatformDualStream:
		return "platform_dual_stream";
	}

	return "standard";
}

DeliveryMode DeliveryModeFromString(const std::string &value)
{
	if (value == "enhanced_multitrack") {
		return DeliveryMode::EnhancedMultitrack;
	}
	if (value == "platform_dual_stream") {
		return DeliveryMode::PlatformDualStream;
	}
	return DeliveryMode::Standard;
}

std::vector<std::string> Validate(const Program &program)
{
	std::vector<std::string> errors;
	if (program.id.empty()) {
		errors.emplace_back("program id is empty");
	}
	if (program.name.empty()) {
		errors.emplace_back("program name is empty");
	}
	if (program.canvas.uuid.empty() && program.canvas.name.empty()) {
		errors.emplace_back("program has no canvas reference");
	}
	if (program.audioMix >= MAX_AUDIO_MIXES) {
		errors.emplace_back("program audio mix is out of range");
	}
	return errors;
}

std::vector<std::string> Validate(const PlatformSessionConfig &session, const SessionSet &set)
{
	std::vector<std::string> errors;
	if (session.id.empty()) {
		errors.emplace_back("session id is empty");
	}
	if (session.name.empty()) {
		errors.emplace_back("session name is empty");
	}
	if (session.platformId.empty()) {
		errors.emplace_back("session platform id is empty");
	}
	if (session.deliveryMode == DeliveryMode::EnhancedMultitrack && !session.capabilities.enhancedMultitrack) {
		errors.emplace_back("session enables enhanced multitrack without the capability");
	}
	if (session.deliveryMode == DeliveryMode::PlatformDualStream && !session.capabilities.platformDualStream) {
		errors.emplace_back("session enables platform dual stream without the capability");
	}

	std::unordered_set<std::string> endpointIds;
	for (size_t index = 0; index < session.endpoints.size(); ++index) {
		const auto &endpoint = session.endpoints[index];
		if (endpoint.id.empty()) {
			errors.emplace_back("endpoint[" + std::to_string(index) + "]: endpoint id is empty");
		}
		if (!endpointIds.insert(endpoint.id).second) {
			errors.emplace_back("duplicate endpoint id: " + endpoint.id);
		}
		if (session.enabled && endpoint.enabled && endpoint.service.empty()) {
			errors.emplace_back("endpoint[" + std::to_string(index) + "]: service is empty");
		}
		if (session.enabled && endpoint.enabled && endpoint.server.empty() &&
		    endpoint.serviceSettingsJson.empty()) {
			errors.emplace_back("endpoint[" + std::to_string(index) + "]: server is empty");
		}
	}

	for (size_t index = 0; index < session.programBindings.size(); ++index) {
		const auto &binding = session.programBindings[index];
		auto found = std::find_if(set.programs.begin(), set.programs.end(),
					  [&](const Program &program) { return program.id == binding.programId; });
		if (found == set.programs.end()) {
			errors.emplace_back("binding[" + std::to_string(index) + "] references missing program " +
					    binding.programId);
		}
	}

	return errors;
}

std::vector<std::string> Validate(const SessionSet &set)
{
	std::vector<std::string> errors;
	if (set.schemaVersion != PlatformSessionSchemaVersion) {
		errors.emplace_back("unsupported platform session schema version");
	}

	std::unordered_set<std::string> programIds;
	for (size_t index = 0; index < set.programs.size(); ++index) {
		const auto &program = set.programs[index];
		if (!programIds.insert(program.id).second) {
			errors.emplace_back("duplicate program id: " + program.id);
		}
		AppendErrors(errors, Validate(program), "program[" + std::to_string(index) + "]: ");
	}

	std::unordered_set<std::string> sessionIds;
	for (size_t index = 0; index < set.sessions.size(); ++index) {
		const auto &session = set.sessions[index];
		if (!sessionIds.insert(session.id).second) {
			errors.emplace_back("duplicate session id: " + session.id);
		}
		AppendErrors(errors, Validate(session, set), "session[" + std::to_string(index) + "]: ");
	}

	if (!set.replayBuffer.programId.empty()) {
		const auto program = std::find_if(set.programs.begin(), set.programs.end(), [&](const Program &item) {
			return item.id == set.replayBuffer.programId;
		});
		if (program == set.programs.end()) {
			errors.emplace_back("replay buffer references missing program " + set.replayBuffer.programId);
		} else if (!program->enabled) {
			errors.emplace_back("replay buffer references disabled program " + set.replayBuffer.programId);
		}
	}

	return errors;
}

SessionSet MigrateRouteSet(const RouteSet &routes)
{
	SessionSet result;
	result.schemaVersion = PlatformSessionSchemaVersion;

	for (const Route &route : routes.routes) {
		Program program;
		program.id = route.id;
		program.name = route.name;
		program.kind = route.kind;
		program.canvas = route.canvas;
		program.videoEncoderId = route.videoEncoderId;
		program.audioEncoderId = route.audioEncoderId;
		program.videoEncoderSettingsJson = route.videoEncoderSettingsJson;
		program.audioEncoderSettingsJson = route.audioEncoderSettingsJson;
		program.rescaleFilter = route.rescaleFilter;
		program.rescaleResolution = route.rescaleResolution;
		program.audioMix = route.audioMix;
		program.enabled = route.enabled;
		program.compatibilityDefault = route.primary;
		result.programs.emplace_back(std::move(program));

		for (const Destination &destination : route.destinations) {
			const std::string sessionId = destination.sessionId.empty() ? "session-" + destination.id
										    : destination.sessionId;
			auto session =
				std::find_if(result.sessions.begin(), result.sessions.end(),
					     [&](const PlatformSessionConfig &item) { return item.id == sessionId; });
			if (session == result.sessions.end()) {
				PlatformSessionConfig config;
				config.id = sessionId;
				config.name = destination.name.empty() ? sessionId : destination.name;
				config.platformId = destination.serviceName.empty() ? destination.service
										    : destination.serviceName;
				config.capabilities.standardStreaming = true;
				config.dynamicBitrateEnabled = destination.dynamicBitrateEnabled;
				config.programBindings.push_back(ProgramBinding{
					route.id, route.primary ? "primary" : "secondary", route.failoverMode, true});
				config.endpoints.push_back(EndpointFromDestination(destination));
				result.sessions.emplace_back(std::move(config));
			} else {
				const bool alreadyBound = std::any_of(
					session->programBindings.begin(), session->programBindings.end(),
					[&](const ProgramBinding &binding) { return binding.programId == route.id; });
				if (!alreadyBound) {
					session->programBindings.push_back(
						ProgramBinding{route.id, route.primary ? "primary" : "secondary",
							       route.failoverMode, true});
				}
				session->dynamicBitrateEnabled = session->dynamicBitrateEnabled ||
								 destination.dynamicBitrateEnabled;
				session->endpoints.push_back(EndpointFromDestination(destination));
			}
		}
	}

	const auto primary = std::find_if(routes.routes.begin(), routes.routes.end(),
					  [](const Route &route) { return route.primary; });
	if (primary != routes.routes.end()) {
		PlatformSessionConfig compatibility;
		compatibility.id = "stream1";
		compatibility.name = "Primary Stream";
		compatibility.platformId = "obs-service";
		compatibility.authenticationReference = "stream1";
		compatibility.compatibilityDefault = true;
		compatibility.programBindings.push_back({primary->id, "primary", FailoverMode::None, true});
		result.sessions.insert(result.sessions.begin(), std::move(compatibility));
	}

	return result;
}

SessionSet ReconcileRouteSet(const SessionSet &existing, const RouteSet &routes)
{
	SessionSet result = MigrateRouteSet(routes);
	result.replayBuffer = existing.replayBuffer;

	// RouteSet is a compatibility projection and intentionally does not expose
	// all Program-owned settings. Preserve those fields while applying edits
	// made through the existing route UI.
	for (Program &program : result.programs) {
		const auto previous = std::find_if(existing.programs.begin(), existing.programs.end(),
						   [&](const Program &item) { return item.id == program.id; });
		if (previous == existing.programs.end()) {
			continue;
		}

		program.videoFormat = previous->videoFormat;
		program.recordingEligible = previous->recordingEligible;
		program.replayEligible = previous->replayEligible;
	}

	for (PlatformSessionConfig &session : result.sessions) {
		const auto previous =
			std::find_if(existing.sessions.begin(), existing.sessions.end(),
				     [&](const PlatformSessionConfig &item) { return item.id == session.id; });
		if (previous == existing.sessions.end()) {
			continue;
		}

		session.authenticationReference = previous->authenticationReference;
		session.nativeDockId = previous->nativeDockId;
		session.deliveryMode = previous->deliveryMode;
		session.multitrackConfig = previous->multitrackConfig;
		session.capabilities = previous->capabilities;
		session.dynamicBitrateEnabled = previous->dynamicBitrateEnabled;
		session.enabled = previous->enabled;
		session.compatibilityDefault = previous->compatibilityDefault || session.id == "stream1";
		if (session.platformId.empty() || session.platformId == "obs-service") {
			session.platformId = previous->platformId;
		}
		if (session.endpoints.empty()) {
			session.name = previous->name;
		}

		for (ProgramBinding &binding : session.programBindings) {
			const auto previousBinding = std::find_if(
				previous->programBindings.begin(), previous->programBindings.end(),
				[&](const ProgramBinding &item) { return item.programId == binding.programId; });
			if (previousBinding != previous->programBindings.end()) {
				binding.role = previousBinding->role;
			}
		}

		// Native provider sessions have no compatibility endpoint, so RouteSet
		// cannot represent secondary Program bindings such as a vertical
		// Enhanced Broadcasting canvas. Keep those bindings explicitly.
		if (session.endpoints.empty()) {
			for (const ProgramBinding &binding : previous->programBindings) {
				const bool programExists = std::any_of(result.programs.begin(), result.programs.end(),
								       [&](const Program &program) {
									       return program.id == binding.programId;
								       });
				const bool alreadyBound =
					std::any_of(session.programBindings.begin(), session.programBindings.end(),
						    [&](const ProgramBinding &item) {
							    return item.programId == binding.programId;
						    });
				if (programExists && !alreadyBound) {
					session.programBindings.emplace_back(binding);
				}
			}
		}
	}

	// Preserve provider-native sessions which are not expressible as route
	// endpoints. Sessions with endpoints are intentionally omitted when their
	// last destination is removed in the route UI.
	for (const PlatformSessionConfig &previous : existing.sessions) {
		const bool exists =
			std::any_of(result.sessions.begin(), result.sessions.end(),
				    [&](const PlatformSessionConfig &session) { return session.id == previous.id; });
		if (exists || !previous.endpoints.empty()) {
			continue;
		}

		PlatformSessionConfig preserved = previous;
		preserved.programBindings.erase(
			std::remove_if(preserved.programBindings.begin(), preserved.programBindings.end(),
				       [&](const ProgramBinding &binding) {
					       return std::none_of(result.programs.begin(), result.programs.end(),
								   [&](const Program &program) {
									   return program.id == binding.programId;
								   });
				       }),
			preserved.programBindings.end());
		if (!preserved.programBindings.empty()) {
			result.sessions.emplace_back(std::move(preserved));
		}
	}

	const auto replayProgram = std::find_if(result.programs.begin(), result.programs.end(), [&](Program &program) {
		return program.id == result.replayBuffer.programId;
	});
	if (!result.replayBuffer.programId.empty() && replayProgram == result.programs.end()) {
		result.replayBuffer.programId.clear();
	} else if (replayProgram != result.programs.end()) {
		replayProgram->replayEligible = true;
	}

	return result;
}

RouteSet ToRouteSet(const SessionSet &set)
{
	RouteSet routes;
	routes.schemaVersion = RouteSchemaVersion;

	for (const Program &program : set.programs) {
		Route route;
		route.id = program.id;
		route.name = program.name;
		route.kind = program.kind;
		route.canvas = program.canvas;
		route.videoEncoderId = program.videoEncoderId;
		route.audioEncoderId = program.audioEncoderId;
		route.videoEncoderSettingsJson = program.videoEncoderSettingsJson;
		route.audioEncoderSettingsJson = program.audioEncoderSettingsJson;
		route.rescaleFilter = program.rescaleFilter;
		route.rescaleResolution = program.rescaleResolution;
		route.audioMix = program.audioMix;
		route.enabled = program.enabled;
		route.primary = program.compatibilityDefault;
		bool failoverModeSet = false;

		for (const PlatformSessionConfig &session : set.sessions) {
			const bool bound = std::any_of(session.programBindings.begin(), session.programBindings.end(),
						       [&](const ProgramBinding &binding) {
							       return binding.enabled &&
								      binding.programId == program.id;
						       });
			if (!bound) {
				continue;
			}
			const auto binding = std::find_if(session.programBindings.begin(),
							  session.programBindings.end(),
							  [&](const ProgramBinding &item) {
								  return item.enabled && item.programId == program.id;
							  });
			if (binding != session.programBindings.end() && !failoverModeSet) {
				route.failoverMode = binding->failoverMode;
				failoverModeSet = true;
			}
			for (const OutputEndpoint &endpoint : session.endpoints) {
				auto destination = DestinationFromEndpoint(endpoint, session.id);
				// Keep disabled sessions persisted while excluding their endpoints
				// from the compatibility runtime projection.
				destination.enabled = destination.enabled && session.enabled;
				destination.dynamicBitrateEnabled = session.dynamicBitrateEnabled;
				route.destinations.emplace_back(std::move(destination));
			}
		}

		routes.routes.emplace_back(std::move(route));
	}

	if (!routes.routes.empty() && std::none_of(routes.routes.begin(), routes.routes.end(),
						   [](const Route &route) { return route.primary; })) {
		auto defaultSession =
			std::find_if(set.sessions.begin(), set.sessions.end(),
				     [](const PlatformSessionConfig &session) { return session.compatibilityDefault; });
		if (defaultSession != set.sessions.end() && !defaultSession->programBindings.empty()) {
			const std::string &programId = defaultSession->programBindings.front().programId;
			for (Route &route : routes.routes) {
				if (route.id == programId) {
					route.primary = true;
					break;
				}
			}
		}
	}

	return routes;
}

void to_json(json &value, const Program &program)
{
	value = json{{"id", program.id},
		     {"name", program.name},
		     {"kind", ToString(program.kind)},
		     {"canvas", program.canvas},
		     {"video_format",
		      {{"width", program.videoFormat.width},
		       {"height", program.videoFormat.height},
		       {"fps_numerator", program.videoFormat.fpsNumerator},
		       {"fps_denominator", program.videoFormat.fpsDenominator},
		       {"scale_type", program.videoFormat.scaleType},
		       {"color_space", program.videoFormat.colorSpace},
		       {"color_range", program.videoFormat.colorRange}}},
		     {"video_encoder_id", program.videoEncoderId},
		     {"audio_encoder_id", program.audioEncoderId},
		     {"video_encoder_settings", program.videoEncoderSettingsJson},
		     {"audio_encoder_settings", program.audioEncoderSettingsJson},
		     {"rescale_filter", program.rescaleFilter},
		     {"rescale_resolution", program.rescaleResolution},
		     {"audio_mix", program.audioMix},
		     {"enabled", program.enabled},
		     {"compatibility_default", program.compatibilityDefault},
		     {"recording_eligible", program.recordingEligible},
		     {"replay_eligible", program.replayEligible}};
}

void from_json(const json &value, Program &program)
{
	program.id = value.value("id", std::string{});
	program.name = value.value("name", std::string{});
	program.kind = KindFromString(value.value("kind", std::string{"stream"}));
	program.canvas = value.value("canvas", CanvasReference{});
	const auto videoFormat = value.value("video_format", json::object());
	program.videoFormat.width = videoFormat.value("width", 0U);
	program.videoFormat.height = videoFormat.value("height", 0U);
	program.videoFormat.fpsNumerator = videoFormat.value("fps_numerator", 0U);
	program.videoFormat.fpsDenominator = videoFormat.value("fps_denominator", 1U);
	program.videoFormat.scaleType = videoFormat.value("scale_type", std::string{});
	program.videoFormat.colorSpace = videoFormat.value("color_space", std::string{});
	program.videoFormat.colorRange = videoFormat.value("color_range", std::string{});
	program.videoEncoderId = value.value("video_encoder_id", std::string{});
	program.audioEncoderId = value.value("audio_encoder_id", std::string{});
	program.videoEncoderSettingsJson = value.value("video_encoder_settings", std::string{});
	program.audioEncoderSettingsJson = value.value("audio_encoder_settings", std::string{});
	program.rescaleFilter = value.value("rescale_filter", OBS_SCALE_DISABLE);
	program.rescaleResolution = value.value("rescale_resolution", std::string{});
	program.audioMix = value.value("audio_mix", 0U);
	program.enabled = value.value("enabled", true);
	program.compatibilityDefault = value.value("compatibility_default", false);
	program.recordingEligible = value.value("recording_eligible", false);
	program.replayEligible = value.value("replay_eligible", false);
}

void to_json(json &value, const ProgramBinding &binding)
{
	value = json{{"program_id", binding.programId},
		     {"role", binding.role},
		     {"failover_mode", ToString(binding.failoverMode)},
		     {"enabled", binding.enabled}};
}

void from_json(const json &value, ProgramBinding &binding)
{
	binding.programId = value.value("program_id", std::string{});
	binding.role = value.value("role", std::string{"primary"});
	binding.failoverMode = FailoverModeFromString(value.value("failover_mode", std::string{"none"}));
	binding.enabled = value.value("enabled", true);
}

void to_json(json &value, const OutputEndpoint &endpoint)
{
	value = json{{"id", endpoint.id},
		     {"name", endpoint.name},
		     {"service", endpoint.service},
		     {"service_name", endpoint.serviceName},
		     {"server", endpoint.server},
		     {"service_settings", endpoint.serviceSettingsJson},
		     {"priority", endpoint.priority},
		     {"reconnect_retry_count", endpoint.reconnectRetryCount},
		     {"reconnect_retry_seconds", endpoint.reconnectRetrySeconds},
		     {"reconnect_enabled", endpoint.reconnectEnabled},
		     {"use_authentication", endpoint.useAuthentication},
		     {"enabled", endpoint.enabled}};
	if (endpoint.serviceSettingsJson.empty()) {
		value["stream_key"] = endpoint.streamKey;
		value["username"] = endpoint.username;
		value["password"] = endpoint.password;
	}
}

void from_json(const json &value, OutputEndpoint &endpoint)
{
	endpoint.id = value.value("id", std::string{});
	endpoint.name = value.value("name", std::string{});
	endpoint.service = value.value("service", std::string{"rtmp_custom"});
	endpoint.serviceName = value.value("service_name", std::string{});
	endpoint.server = value.value("server", std::string{});
	endpoint.streamKey = value.value("stream_key", std::string{});
	endpoint.username = value.value("username", std::string{});
	endpoint.password = value.value("password", std::string{});
	endpoint.serviceSettingsJson = value.value("service_settings", std::string{});
	endpoint.priority = value.value("priority", 0U);
	endpoint.reconnectRetryCount = value.value("reconnect_retry_count", 0U);
	endpoint.reconnectRetrySeconds = value.value("reconnect_retry_seconds", 0U);
	endpoint.reconnectEnabled = value.value("reconnect_enabled", true);
	endpoint.useAuthentication = value.value("use_authentication", false);
	endpoint.enabled = value.value("enabled", true);
}

void to_json(json &value, const PlatformCapabilities &capabilities)
{
	value = json{{"standard_streaming", capabilities.standardStreaming},
		     {"enhanced_multitrack", capabilities.enhancedMultitrack},
		     {"platform_dual_stream", capabilities.platformDualStream}};
}

void from_json(const json &value, PlatformCapabilities &capabilities)
{
	capabilities.standardStreaming = value.value("standard_streaming", true);
	capabilities.enhancedMultitrack = value.value("enhanced_multitrack", false);
	capabilities.platformDualStream = value.value("platform_dual_stream", false);
}

void to_json(json &value, const PlatformSessionConfig &session)
{
	value = json{{"id", session.id},
		     {"name", session.name},
		     {"platform_id", session.platformId},
		     {"authentication_reference", session.authenticationReference},
		     {"native_dock_id", session.nativeDockId},
		     {"delivery_mode", ToString(session.deliveryMode)},
		     {"multitrack_config_source", ToString(session.multitrackConfig.source)},
		     {"multitrack_config", session.multitrackConfig.value},
		     {"capabilities", session.capabilities},
		     {"program_bindings", session.programBindings},
		     {"endpoints", session.endpoints},
		     {"dynamic_bitrate_enabled", session.dynamicBitrateEnabled},
		     {"enabled", session.enabled},
		     {"compatibility_default", session.compatibilityDefault}};
}

void from_json(const json &value, PlatformSessionConfig &session)
{
	session.id = value.value("id", std::string{});
	session.name = value.value("name", std::string{});
	session.platformId = value.value("platform_id", std::string{});
	session.authenticationReference = value.value("authentication_reference", std::string{});
	session.nativeDockId = value.value("native_dock_id", std::string{});
	session.deliveryMode = DeliveryModeFromString(value.value("delivery_mode", std::string{"standard"}));
	session.multitrackConfig.source = MultitrackConfigSourceFromString(
		value.value("multitrack_config_source", std::string{"remote_provider_url"}));
	session.multitrackConfig.value = value.value("multitrack_config", std::string{});
	session.capabilities = value.value("capabilities", PlatformCapabilities{});
	session.programBindings = value.value("program_bindings", std::vector<ProgramBinding>{});
	session.endpoints = value.value("endpoints", std::vector<OutputEndpoint>{});
	session.dynamicBitrateEnabled = value.value("dynamic_bitrate_enabled", false);
	session.enabled = value.value("enabled", true);
	session.compatibilityDefault = value.value("compatibility_default", false);
}

void to_json(json &value, const ReplayBufferConfig &replayBuffer)
{
	value = json{{"program_id", replayBuffer.programId}};
}

void from_json(const json &value, ReplayBufferConfig &replayBuffer)
{
	replayBuffer.programId = value.value("program_id", std::string{});
}

void to_json(json &value, const SessionSet &set)
{
	value = json{{"schema_version", set.schemaVersion},
		     {"programs", set.programs},
		     {"sessions", set.sessions},
		     {"replay_buffer", set.replayBuffer}};
}

void from_json(const json &value, SessionSet &set)
{
	set.schemaVersion = value.value("schema_version", PlatformSessionSchemaVersion);
	set.programs = value.value("programs", std::vector<Program>{});
	set.sessions = value.value("sessions", std::vector<PlatformSessionConfig>{});
	set.replayBuffer = value.value("replay_buffer", ReplayBufferConfig{});
}

std::string Serialize(const SessionSet &set)
{
	return json(set).dump();
}

bool Deserialize(std::string_view value, SessionSet &set, std::string &error)
{
	try {
		set = json::parse(value).get<SessionSet>();
		if (set.schemaVersion != PlatformSessionSchemaVersion) {
			error = "unsupported platform session schema version " + std::to_string(set.schemaVersion);
			return false;
		}
		auto errors = Validate(set);
		if (!errors.empty()) {
			error = errors.front();
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
