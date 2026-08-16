#include <utility/OutputRoute.hpp>
#include <utility/PlatformSession.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			return __LINE__; \
	} while (false)

namespace {

class ScenarioRuntime {
public:
	explicit ScenarioRuntime(const OBS::Output::SessionSet &sessionSet) : configuration(sessionSet)
	{
		for (const auto &session : configuration.sessions) {
			activeSessions.emplace(session.id, false);
		}
	}

	bool StartSession(std::string_view sessionId)
	{
		const auto session = std::find_if(configuration.sessions.begin(), configuration.sessions.end(),
						  [&](const auto &item) { return item.id == sessionId; });
		if (session == configuration.sessions.end() || !session->enabled) {
			return false;
		}
		if (session->deliveryMode == OBS::Output::DeliveryMode::EnhancedMultitrack &&
		    (!session->capabilities.enhancedMultitrack || session->multitrackConfig.value.empty())) {
			return false;
		}
		activeSessions[session->id] = true;
		return true;
	}

	void StopSession(std::string_view sessionId) { activeSessions[std::string(sessionId)] = false; }

	bool SessionActive(std::string_view sessionId) const
	{
		const auto found = activeSessions.find(std::string(sessionId));
		return found != activeSessions.end() && found->second;
	}

	bool StartReplay()
	{
		const auto program = ReplayProgram();
		if (!program || !program->enabled) {
			return false;
		}
		replayActive = true;
		return true;
	}

	bool ReplayActive() const { return replayActive; }

	const OBS::Output::Program *SaveReplay() const { return replayActive ? ReplayProgram() : nullptr; }

private:
	const OBS::Output::Program *ReplayProgram() const
	{
		const auto program =
			std::find_if(configuration.programs.begin(), configuration.programs.end(),
				     [&](const auto &item) { return item.id == configuration.replayBuffer.programId; });
		return program == configuration.programs.end() ? nullptr : &*program;
	}

	const OBS::Output::SessionSet &configuration;
	std::unordered_map<std::string, bool> activeSessions;
	bool replayActive = false;
};

} // namespace

int main()
{
	OBS::Output::Route twitchProgram;
	twitchProgram.id = "program-main";
	twitchProgram.name = "Twitch Enhanced";
	twitchProgram.canvas.uuid = "horizontal-canvas";
	twitchProgram.primary = true;

	OBS::Output::Destination youtubeEndpoint;
	youtubeEndpoint.id = "youtube-endpoint";
	youtubeEndpoint.name = "YouTube";
	youtubeEndpoint.sessionId = "youtube-session";
	youtubeEndpoint.service = "rtmp_common";
	youtubeEndpoint.serviceName = "YouTube - RTMPS";
	youtubeEndpoint.server = "rtmps://example.invalid/live";

	OBS::Output::Route youtubeProgram;
	youtubeProgram.id = "youtube-hq";
	youtubeProgram.name = "YouTube HQ";
	youtubeProgram.canvas.uuid = "horizontal-canvas";
	youtubeProgram.videoEncoderSettingsJson = R"({"bitrate":50000,"rate_control":"CBR"})";
	youtubeProgram.audioEncoderSettingsJson = R"({"bitrate":320})";
	youtubeProgram.destinations.emplace_back(youtubeEndpoint);

	OBS::Output::RouteSet routes;
	routes.routes = {twitchProgram, youtubeProgram};
	auto sessions = OBS::Output::MigrateRouteSet(routes);
	sessions.sessions[0].deliveryMode = OBS::Output::DeliveryMode::EnhancedMultitrack;
	sessions.sessions[0].capabilities.enhancedMultitrack = true;
	sessions.sessions[0].multitrackConfig.source = OBS::Output::MultitrackConfigSource::StaticJson;
	sessions.sessions[0].multitrackConfig.value = R"({"encoder_configurations":[{}]})";
	sessions.replayBuffer.programId = "youtube-hq";
	sessions = OBS::Output::ReconcileRouteSet(sessions, routes);

	std::string error;
	OBS::Output::SessionSet restored;
	CHECK(OBS::Output::Deserialize(OBS::Output::Serialize(sessions), restored, error));
	CHECK(OBS::Output::Validate(restored).empty());

	ScenarioRuntime runtime(restored);
	CHECK(runtime.StartSession("youtube-session"));
	CHECK(runtime.StartReplay());
	CHECK(runtime.StartSession("stream1"));
	CHECK(runtime.SessionActive("youtube-session"));
	CHECK(runtime.SessionActive("stream1"));
	CHECK(runtime.ReplayActive());

	runtime.StopSession("youtube-session");
	CHECK(!runtime.SessionActive("youtube-session"));
	CHECK(runtime.SessionActive("stream1"));
	CHECK(runtime.ReplayActive());

	CHECK(runtime.StartSession("youtube-session"));
	runtime.StopSession("stream1");
	CHECK(!runtime.SessionActive("stream1"));
	CHECK(runtime.SessionActive("youtube-session"));
	CHECK(runtime.ReplayActive());

	const auto *savedProgram = runtime.SaveReplay();
	CHECK(savedProgram != nullptr);
	CHECK(savedProgram->id == "youtube-hq");
	const auto encoderSettings = nlohmann::json::parse(savedProgram->videoEncoderSettingsJson);
	CHECK(encoderSettings.at("bitrate").get<int>() == 50000);
	CHECK(encoderSettings.at("rate_control").get<std::string>() == "CBR");
	return 0;
}
