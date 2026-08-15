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

#include "OutputRouteRuntime.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

#include <obs.hpp>

const char *GetStreamOutputType(const obs_service_t *service);

namespace OBS::Output {
namespace {

std::string ContextName(const char *kind, const Route &route, const Destination *destination = nullptr)
{
	std::string name = "obs-pro-";
	name += kind;
	name += '-';
	name += route.id;
	if (destination) {
		name += '-';
		name += destination->id;
	}
	return name;
}

void ApplyJsonSettings(obs_data_t *settings, const std::string &serialized)
{
	if (!settings || serialized.empty()) {
		return;
	}

	OBSDataAutoRelease overrides = obs_data_create_from_json(serialized.c_str());
	if (overrides) {
		obs_data_apply(settings, overrides);
	}
}

OBSEncoderAutoRelease CreateVideoEncoder(const Route &route, obs_canvas_t *canvas, obs_encoder_t *reference,
					 std::string &error)
{
	if (!reference) {
		error = "the reference output has no video encoder";
		return nullptr;
	}

	video_t *video = obs_canvas_get_video(canvas);
	if (!video) {
		error = "the route canvas has no video output";
		return nullptr;
	}

	const char *referenceId = obs_encoder_get_id(reference);
	const char *encoderId = route.videoEncoderId.empty() ? referenceId : route.videoEncoderId.c_str();
	if (!encoderId || !*encoderId) {
		error = "the route has no usable video encoder";
		return nullptr;
	}

	const bool sameEncoder = referenceId && strcmp(referenceId, encoderId) == 0;
	const bool sameVideo = obs_encoder_video(reference) == video || obs_encoder_parent_video(reference) == video;
	if (route.primary && sameEncoder && sameVideo && route.videoEncoderSettingsJson.empty()) {
		return obs_encoder_get_ref(reference);
	}

	OBSDataAutoRelease settings = sameEncoder ? obs_encoder_get_settings(reference)
						  : obs_encoder_defaults(encoderId);
	ApplyJsonSettings(settings, route.videoEncoderSettingsJson);
	const std::string name = ContextName("video", route);
	OBSEncoderAutoRelease encoder = obs_video_encoder_create(encoderId, name.c_str(), settings, nullptr);
	if (!encoder) {
		error = "failed to create video encoder '" + std::string(encoderId) + "'";
		return nullptr;
	}

	obs_encoder_set_video(encoder, video);
	return encoder;
}

OBSEncoderAutoRelease CreateAudioEncoder(const Route &route, obs_encoder_t *reference, std::string &error)
{
	if (!reference) {
		error = "the reference output has no audio encoder";
		return nullptr;
	}

	const char *referenceId = obs_encoder_get_id(reference);
	const char *encoderId = route.audioEncoderId.empty() ? referenceId : route.audioEncoderId.c_str();
	if (!encoderId || !*encoderId) {
		error = "the route has no usable audio encoder";
		return nullptr;
	}

	const bool sameEncoder = referenceId && strcmp(referenceId, encoderId) == 0;
	if (route.primary && sameEncoder && obs_encoder_get_mixer_index(reference) == route.audioMix &&
	    route.audioEncoderSettingsJson.empty()) {
		return obs_encoder_get_ref(reference);
	}

	OBSDataAutoRelease settings = sameEncoder ? obs_encoder_get_settings(reference)
						  : obs_encoder_defaults(encoderId);
	ApplyJsonSettings(settings, route.audioEncoderSettingsJson);
	const std::string name = ContextName("audio", route);
	OBSEncoderAutoRelease encoder =
		obs_audio_encoder_create(encoderId, name.c_str(), settings, route.audioMix, nullptr);
	if (!encoder) {
		error = "failed to create audio encoder '" + std::string(encoderId) + "'";
		return nullptr;
	}

	obs_encoder_set_audio(encoder, obs_get_audio());
	return encoder;
}

OBSDataAutoRelease CreateServiceSettings(const Destination &destination)
{
	OBSDataAutoRelease settings = obs_data_create();
	ApplyJsonSettings(settings, destination.serviceSettingsJson);
	if (!destination.serviceName.empty()) {
		obs_data_set_default_string(settings, "service", destination.serviceName.c_str());
	}
	if (!destination.server.empty()) {
		obs_data_set_default_string(settings, "server", destination.server.c_str());
	}
	if (!destination.streamKey.empty()) {
		obs_data_set_default_string(settings, "key", destination.streamKey.c_str());
	}
	obs_data_set_default_bool(settings, "use_auth", destination.useAuthentication);
	if (destination.useAuthentication) {
		obs_data_set_default_string(settings, "username", destination.username.c_str());
		obs_data_set_default_string(settings, "password", destination.password.c_str());
	}
	return settings;
}

} // namespace

struct Runtime::Impl {
	struct DestinationRuntime;

	struct RouteRuntime {
		Route config;
		OBSCanvasAutoRelease canvas;
		OBSEncoderAutoRelease videoEncoder;
		OBSEncoderAutoRelease audioEncoder;
		std::vector<std::unique_ptr<DestinationRuntime>> destinations;
	};

	struct DestinationRuntime {
		Impl *owner = nullptr;
		RouteRuntime *route = nullptr;
		Destination config;
		OBSServiceAutoRelease service;
		OBSOutputAutoRelease output;
		OBSSignal startSignal;
		OBSSignal stopSignal;
		std::atomic<RuntimeState> state{RuntimeState::Idle};
		std::string lastError;
	};

	RuntimeOptions options;
	std::vector<std::unique_ptr<RouteRuntime>> routes;
	mutable std::mutex mutex;
	std::atomic_bool intentionalStop{false};

	static void OutputStarted(void *data, calldata_t *)
	{
		auto *destination = static_cast<DestinationRuntime *>(data);
		destination->state.store(RuntimeState::Active);
		std::lock_guard lock(destination->owner->mutex);
		destination->lastError.clear();
	}

	static void OutputStopped(void *data, calldata_t *params)
	{
		auto *destination = static_cast<DestinationRuntime *>(data);
		Impl *owner = destination->owner;
		const int code = static_cast<int>(calldata_int(params, "code"));
		const char *lastError = calldata_string(params, "last_error");
		const RuntimeState finalState = code == OBS_OUTPUT_SUCCESS ? RuntimeState::Idle : RuntimeState::Failed;

		// Keep the route alive until this callback has finished inspecting it.
		// Prepare() treats Stopping as active and therefore cannot clear the
		// callback's destination storage concurrently.
		destination->state.store(RuntimeState::Stopping);
		{
			std::lock_guard lock(owner->mutex);
			destination->lastError = lastError ? lastError : "";
		}

		if (owner->intentionalStop.load() ||
		    destination->route->config.failoverMode != FailoverMode::ClientSequential) {
			destination->state.store(finalState);
			return;
		}

		auto &destinations = destination->route->destinations;
		auto current = std::find_if(destinations.begin(), destinations.end(),
					    [destination](const auto &item) { return item.get() == destination; });
		if (current == destinations.end()) {
			destination->state.store(finalState);
			return;
		}

		for (++current; current != destinations.end(); ++current) {
			if (owner->StartDestination(**current)) {
				break;
			}
		}
		destination->state.store(finalState);
	}

	bool StartDestination(DestinationRuntime &destination)
	{
		destination.state.store(RuntimeState::Starting);
		if (obs_output_start(destination.output)) {
			return true;
		}

		destination.state.store(RuntimeState::Failed);
		const char *lastError = obs_output_get_last_error(destination.output);
		{
			std::lock_guard lock(mutex);
			destination.lastError = lastError && *lastError ? lastError : "output failed to start";
		}
		return false;
	}
};

Runtime::Runtime() : impl(std::make_unique<Impl>()) {}

Runtime::~Runtime()
{
	Stop(true);
}

bool Runtime::Prepare(const RouteSet &routeSet, obs_output_t *referenceOutput, const RuntimeOptions &options,
		      std::string &error)
{
	if (Active()) {
		error = "additional outputs are still active";
		return false;
	}

	impl->routes.clear();
	impl->options = options;
	impl->intentionalStop.store(false);

	if (!referenceOutput) {
		error = "the reference stream output is missing";
		return false;
	}

	auto validationErrors = Validate(routeSet);
	if (!validationErrors.empty()) {
		error = validationErrors.front();
		return false;
	}

	obs_encoder_t *referenceVideo = obs_output_get_video_encoder(referenceOutput);
	obs_encoder_t *referenceAudio = obs_output_get_audio_encoder(referenceOutput, 0);

	for (const Route &route : routeSet.routes) {
		if (!route.enabled || route.kind != Kind::Stream) {
			continue;
		}

		const bool hasEnabledDestination =
			std::any_of(route.destinations.begin(), route.destinations.end(),
				    [](const Destination &destination) { return destination.enabled; });
		if (!hasEnabledDestination) {
			continue;
		}

		auto runtimeRoute = std::make_unique<Impl::RouteRuntime>();
		runtimeRoute->config = route;
		runtimeRoute->canvas = ResolveCanvas(route.canvas);
		if (!runtimeRoute->canvas) {
			error = "route '" + route.name + "' references a missing canvas";
			impl->routes.clear();
			return false;
		}

		runtimeRoute->videoEncoder = CreateVideoEncoder(route, runtimeRoute->canvas, referenceVideo, error);
		if (!runtimeRoute->videoEncoder) {
			error = "route '" + route.name + "': " + error;
			impl->routes.clear();
			return false;
		}

		runtimeRoute->audioEncoder = CreateAudioEncoder(route, referenceAudio, error);
		if (!runtimeRoute->audioEncoder) {
			error = "route '" + route.name + "': " + error;
			impl->routes.clear();
			return false;
		}

		for (const Destination &destination : route.destinations) {
			if (!destination.enabled) {
				continue;
			}

			auto runtimeDestination = std::make_unique<Impl::DestinationRuntime>();
			runtimeDestination->owner = impl.get();
			runtimeDestination->route = runtimeRoute.get();
			runtimeDestination->config = destination;

			OBSDataAutoRelease serviceSettings = CreateServiceSettings(destination);
			const std::string serviceName = ContextName("service", route, &destination);
			runtimeDestination->service = obs_service_create(destination.service.c_str(),
									 serviceName.c_str(), serviceSettings, nullptr);
			if (!runtimeDestination->service) {
				error = "destination '" + destination.name + "': failed to create service '" +
					destination.service + "'";
				impl->routes.clear();
				return false;
			}

			const char *outputType = GetStreamOutputType(runtimeDestination->service);
			if (!outputType) {
				error = "destination '" + destination.name + "': no compatible output is available";
				impl->routes.clear();
				return false;
			}

			const std::string outputName = ContextName("output", route, &destination);
			runtimeDestination->output =
				obs_output_create(outputType, outputName.c_str(), nullptr, nullptr);
			if (!runtimeDestination->output) {
				error = "destination '" + destination.name + "': failed to create output '" +
					outputType + "'";
				impl->routes.clear();
				return false;
			}

			obs_output_set_video_encoder(runtimeDestination->output, runtimeRoute->videoEncoder);
			obs_output_set_audio_encoder(runtimeDestination->output, runtimeRoute->audioEncoder, 0);
			obs_output_set_service(runtimeDestination->output, runtimeDestination->service);

			OBSDataAutoRelease outputSettings = obs_data_create();
			obs_data_set_string(outputSettings, "bind_ip", options.bindIp.c_str());
			obs_data_set_string(outputSettings, "ip_family", options.ipFamily.c_str());
			// Independent dynamic-bitrate controllers would fight over a shared
			// encoder, so additional fan-out outputs deliberately leave it off.
			obs_data_set_bool(outputSettings, "dyn_bitrate", false);
			obs_output_update(runtimeDestination->output, outputSettings);
			obs_output_set_delay(runtimeDestination->output, options.delaySeconds, options.delayFlags);
			obs_output_set_reconnect_settings(runtimeDestination->output, options.reconnectRetryCount,
							  options.reconnectRetrySeconds);

			signal_handler_t *signals = obs_output_get_signal_handler(runtimeDestination->output);
			runtimeDestination->startSignal.Connect(signals, "start", Impl::OutputStarted,
								runtimeDestination.get());
			runtimeDestination->stopSignal.Connect(signals, "stop", Impl::OutputStopped,
							       runtimeDestination.get());
			runtimeRoute->destinations.emplace_back(std::move(runtimeDestination));
		}

		std::stable_sort(runtimeRoute->destinations.begin(), runtimeRoute->destinations.end(),
				 [](const auto &left, const auto &right) {
					 return left->config.priority < right->config.priority;
				 });
		impl->routes.emplace_back(std::move(runtimeRoute));
	}

	error.clear();
	return true;
}

size_t Runtime::Start()
{
	if (Active()) {
		return 0;
	}
	impl->intentionalStop.store(false);
	size_t started = 0;

	for (auto &route : impl->routes) {
		if (route->config.failoverMode == FailoverMode::ClientSequential) {
			for (auto &destination : route->destinations) {
				if (impl->StartDestination(*destination)) {
					++started;
					break;
				}
			}
			continue;
		}

		for (auto &destination : route->destinations) {
			if (impl->StartDestination(*destination)) {
				++started;
			}
		}
	}

	return started;
}

void Runtime::Stop(bool force)
{
	impl->intentionalStop.store(true);
	std::vector<OBSOutputAutoRelease> outputs;

	for (auto &route : impl->routes) {
		for (auto &destination : route->destinations) {
			const RuntimeState state = destination->state.load();
			if (!obs_output_active(destination->output) && state != RuntimeState::Starting) {
				continue;
			}
			destination->state.store(RuntimeState::Stopping);
			outputs.emplace_back(obs_output_get_ref(destination->output));
		}
	}

	for (auto &output : outputs) {
		if (force) {
			obs_output_force_stop(output);
		} else {
			obs_output_stop(output);
		}
	}
}

void Runtime::Clear()
{
	if (Active()) {
		return;
	}
	impl->routes.clear();
}

bool Runtime::Active() const
{
	for (const auto &route : impl->routes) {
		for (const auto &destination : route->destinations) {
			const RuntimeState state = destination->state.load();
			if (state == RuntimeState::Starting || state == RuntimeState::Active ||
			    state == RuntimeState::Stopping || obs_output_active(destination->output)) {
				return true;
			}
		}
	}
	return false;
}

size_t Runtime::PreparedDestinationCount() const
{
	size_t count = 0;
	for (const auto &route : impl->routes) {
		count += route->destinations.size();
	}
	return count;
}

std::vector<DestinationSnapshot> Runtime::Snapshot() const
{
	std::vector<DestinationSnapshot> snapshots;
	std::lock_guard lock(impl->mutex);
	for (const auto &route : impl->routes) {
		for (const auto &destination : route->destinations) {
			DestinationSnapshot snapshot;
			snapshot.routeId = route->config.id;
			snapshot.destinationId = destination->config.id;
			snapshot.name = destination->config.name;
			snapshot.state = destination->state.load();
			snapshot.lastError = destination->lastError;
			snapshot.totalBytes = obs_output_get_total_bytes(destination->output);
			snapshot.droppedFrames = obs_output_get_frames_dropped(destination->output);
			snapshot.totalFrames = obs_output_get_total_frames(destination->output);
			snapshots.emplace_back(std::move(snapshot));
		}
	}
	return snapshots;
}

} // namespace OBS::Output
