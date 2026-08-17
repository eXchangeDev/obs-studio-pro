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

#include "OutputRoute.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <obs.hpp>

namespace OBS::Output {

enum class RuntimeState {
	Idle,
	Starting,
	Active,
	Stopping,
	Failed,
};

inline bool IsRuntimeActive(RuntimeState state, bool outputActive)
{
	return state == RuntimeState::Starting || state == RuntimeState::Active ||
	       (state == RuntimeState::Stopping && outputActive) || outputActive;
}

struct RuntimeOptions {
	std::string bindIp = "default";
	std::string ipFamily = "IPv4+IPv6";
	int reconnectRetryCount = 20;
	int reconnectRetrySeconds = 2;
	uint32_t delaySeconds = 0;
	uint32_t delayFlags = 0;
	// Programs consumed by local outputs such as replay must keep their
	// encoders prepared even when they currently have no transport endpoint.
	std::vector<std::string> retainedProgramIds;
	// Session transitions are reported after the destination state has been
	// committed. This keeps the OBS aggregate facade out of transport callbacks
	// while preserving independent session errors.
	std::function<void(std::string_view, RuntimeState, std::string_view)> stateChanged;
};

struct DestinationSnapshot {
	std::string routeId;
	std::string sessionId;
	std::string destinationId;
	std::string name;
	RuntimeState state = RuntimeState::Idle;
	std::string lastError;
	uint64_t totalBytes = 0;
	int droppedFrames = 0;
	int totalFrames = 0;
};

struct SessionSnapshot {
	std::string sessionId;
	RuntimeState state = RuntimeState::Idle;
	std::string lastError;
	size_t endpointCount = 0;
};

// Manages the additional destinations attached to OBS' regular stream output.
// Every route owns one video and one audio encoder. All destinations in that
// route attach to those same encoder instances, so identical frames are encoded
// once and then fanned out as encoded packets by libobs.
class Runtime {
public:
	Runtime();
	~Runtime();

	Runtime(const Runtime &) = delete;
	Runtime &operator=(const Runtime &) = delete;

	bool Prepare(const RouteSet &routes, obs_output_t *referenceOutput, const RuntimeOptions &options,
		     std::string &error);
	bool Prepare(const RouteSet &routes, obs_encoder_t *referenceVideo, obs_encoder_t *referenceAudio,
		     const RuntimeOptions &options, std::string &error);
	size_t Start();
	size_t StartSession(std::string_view sessionId);
	void Stop(bool force = false);
	void StopSession(std::string_view sessionId, bool force = false);
	void Clear();

	bool Active() const;
	size_t PreparedDestinationCount() const;
	std::vector<DestinationSnapshot> Snapshot() const;
	std::vector<SessionSnapshot> SessionSnapshots() const;
	OBSEncoderAutoRelease ProgramVideoEncoder(std::string_view programId) const;
	OBSEncoderAutoRelease ProgramAudioEncoder(std::string_view programId) const;
	OBSServiceAutoRelease SessionService(std::string_view sessionId) const;
	OBSOutputAutoRelease SessionOutput(std::string_view sessionId) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace OBS::Output
