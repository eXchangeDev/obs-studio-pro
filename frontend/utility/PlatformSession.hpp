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

#include "MultitrackConfigProvider.hpp"
#include "OutputRoute.hpp"

#include <obs.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

struct MultitrackVideoOutput;

namespace OBS::Output {

constexpr uint32_t PlatformSessionSchemaVersion = 1;

enum class DeliveryMode {
	Standard,
	EnhancedMultitrack,
	PlatformDualStream,
};

enum class SessionRuntimeState {
	Idle,
	Validating,
	Preparing,
	Starting,
	Active,
	Stopping,
	Failed,
};

struct ProgramVideoFormat {
	// Zero dimensions or FPS values mean "resolve from the referenced canvas"
	// for compatibility with the existing OBS profile settings.
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t fpsNumerator = 0;
	uint32_t fpsDenominator = 1;
	std::string scaleType;
	std::string colorSpace;
	std::string colorRange;
};

struct Program {
	std::string id;
	std::string name;
	Kind kind = Kind::Stream;
	CanvasReference canvas;
	ProgramVideoFormat videoFormat;
	std::string videoEncoderId;
	std::string audioEncoderId;
	std::string videoEncoderSettingsJson;
	std::string audioEncoderSettingsJson;
	uint32_t audioMix = 0;
	bool enabled = true;
	bool compatibilityDefault = false;
	bool recordingEligible = false;
	bool replayEligible = false;
};

struct ProgramBinding {
	std::string programId;
	std::string role = "primary";
	FailoverMode failoverMode = FailoverMode::None;
	bool enabled = true;
};

struct OutputEndpoint {
	std::string id;
	std::string name;
	std::string service = "rtmp_custom";
	std::string serviceName;
	std::string server;
	std::string streamKey;
	std::string username;
	std::string password;
	std::string serviceSettingsJson;
	uint32_t priority = 0;
	uint32_t reconnectRetryCount = 0;
	uint32_t reconnectRetrySeconds = 0;
	bool reconnectEnabled = true;
	bool useAuthentication = false;
	bool enabled = true;
};

struct PlatformCapabilities {
	bool standardStreaming = true;
	bool enhancedMultitrack = false;
	bool platformDualStream = false;
};

class PlatformCapabilityProvider {
public:
	virtual ~PlatformCapabilityProvider() = default;
	virtual PlatformCapabilities GetCapabilities() const = 0;
};

struct PlatformSessionConfig {
	std::string id;
	std::string name;
	std::string platformId;
	std::string authenticationReference;
	std::string nativeDockId;
	DeliveryMode deliveryMode = DeliveryMode::Standard;
	MultitrackConfigProvider multitrackConfig;
	PlatformCapabilities capabilities;
	std::vector<ProgramBinding> programBindings;
	std::vector<OutputEndpoint> endpoints;
	bool dynamicBitrateEnabled = false;
	bool enabled = true;
	bool compatibilityDefault = false;
};

struct ReplayBufferConfig {
	// Empty keeps OBS' existing recording/replay encoder selection. A value
	// binds the replay buffer to the local encoded Program before any platform
	// transport consumes it.
	std::string programId;
};

// Runtime ownership is deliberately separate from persisted configuration.
// The service is a libobs reference, while authentication and native UI are
// represented by stable references until their provider managers attach them.
class PlatformSession {
public:
	explicit PlatformSession(PlatformSessionConfig config = {});
	~PlatformSession();

	PlatformSession(const PlatformSession &) = delete;
	PlatformSession &operator=(const PlatformSession &) = delete;
	PlatformSession(PlatformSession &&) noexcept = default;
	PlatformSession &operator=(PlatformSession &&) noexcept = default;

	const PlatformSessionConfig &Config() const { return config; }
	PlatformSessionConfig &Config() { return config; }

	void AttachService(obs_service_t *service);
	obs_service_t *Service() const { return service; }
	void AttachOutput(obs_output_t *output);
	obs_output_t *Output() const { return output; }
	::MultitrackVideoOutput &EnsureMultitrackOutput();
	::MultitrackVideoOutput *MultitrackOutput() const { return multitrackOutput.get(); }
	void ResetMultitrackOutput();
	OBSSignal &StartingSignal() { return startingSignal; }
	OBSSignal &StoppingSignal() { return stoppingSignal; }
	OBSSignal &StartedSignal() { return startedSignal; }
	OBSSignal &StoppedSignal() { return stoppedSignal; }
	void DisconnectOutputSignals();
	void AttachCapabilityProvider(std::shared_ptr<const PlatformCapabilityProvider> provider);
	const PlatformCapabilityProvider *CapabilityProvider() const { return capabilityProvider.get(); }

	SessionRuntimeState State() const { return state; }
	void SetState(SessionRuntimeState value) { state = value; }
	const std::string &LastError() const { return lastError; }
	void SetError(std::string value);
	void ClearError();

private:
	PlatformSessionConfig config;
	OBSServiceAutoRelease service;
	OBSOutputAutoRelease output;
	std::unique_ptr<::MultitrackVideoOutput> multitrackOutput;
	OBSSignal startingSignal;
	OBSSignal stoppingSignal;
	OBSSignal startedSignal;
	OBSSignal stoppedSignal;
	std::shared_ptr<const PlatformCapabilityProvider> capabilityProvider;
	SessionRuntimeState state = SessionRuntimeState::Idle;
	std::string lastError;
};

struct SessionSet {
	uint32_t schemaVersion = PlatformSessionSchemaVersion;
	std::vector<Program> programs;
	std::vector<PlatformSessionConfig> sessions;
	ReplayBufferConfig replayBuffer;
};

const char *ToString(DeliveryMode mode);
DeliveryMode DeliveryModeFromString(const std::string &value);

std::vector<std::string> Validate(const Program &program);
std::vector<std::string> Validate(const PlatformSessionConfig &session, const SessionSet &set);
std::vector<std::string> Validate(const SessionSet &set);

// Convert the current fork's route model into the new persisted model. The
// conversion is deterministic and leaves the legacy RouteSet untouched.
SessionSet MigrateRouteSet(const RouteSet &routes);

// Applies changes made through the route compatibility UI while preserving
// session-owned provider/auth/capability state and replay selection.
SessionSet ReconcileRouteSet(const SessionSet &existing, const RouteSet &routes);

// Compatibility projection used by the existing settings and output paths.
RouteSet ToRouteSet(const SessionSet &set);

void to_json(nlohmann::json &json, const Program &program);
void from_json(const nlohmann::json &json, Program &program);
void to_json(nlohmann::json &json, const ProgramBinding &binding);
void from_json(const nlohmann::json &json, ProgramBinding &binding);
void to_json(nlohmann::json &json, const OutputEndpoint &endpoint);
void from_json(const nlohmann::json &json, OutputEndpoint &endpoint);
void to_json(nlohmann::json &json, const PlatformCapabilities &capabilities);
void from_json(const nlohmann::json &json, PlatformCapabilities &capabilities);
void to_json(nlohmann::json &json, const PlatformSessionConfig &session);
void from_json(const nlohmann::json &json, PlatformSessionConfig &session);
void to_json(nlohmann::json &json, const ReplayBufferConfig &replayBuffer);
void from_json(const nlohmann::json &json, ReplayBufferConfig &replayBuffer);
void to_json(nlohmann::json &json, const SessionSet &set);
void from_json(const nlohmann::json &json, SessionSet &set);

std::string Serialize(const SessionSet &set);
bool Deserialize(std::string_view value, SessionSet &set, std::string &error);

} // namespace OBS::Output
