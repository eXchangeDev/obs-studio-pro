#include <utility/OutputRoute.hpp>
#include <utility/PlatformSession.hpp>

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			return __LINE__; \
	} while (false)

int main()
{
	OBS::Output::Route twitch;
	twitch.id = "program-main";
	twitch.name = "Twitch Enhanced";
	twitch.canvas.uuid = "horizontal-canvas";
	twitch.primary = true;

	OBS::Output::Destination youtubeEndpoint;
	youtubeEndpoint.id = "youtube-endpoint";
	youtubeEndpoint.name = "YouTube";
	youtubeEndpoint.sessionId = "youtube-session";
	youtubeEndpoint.service = "rtmp_common";
	youtubeEndpoint.serviceName = "YouTube - RTMPS";
	youtubeEndpoint.server = "rtmps://example.invalid/live";
	youtubeEndpoint.dynamicBitrateEnabled = true;

	OBS::Output::Route youtube;
	youtube.id = "youtube-hq";
	youtube.name = "YouTube HQ";
	youtube.canvas.uuid = "horizontal-canvas";
	youtube.videoEncoderSettingsJson = R"({"bitrate":50000})";
	youtube.audioEncoderSettingsJson = R"({"bitrate":320})";
	youtube.destinations.emplace_back(youtubeEndpoint);

	OBS::Output::RouteSet routes;
	routes.routes = {twitch, youtube};
	auto sessions = OBS::Output::MigrateRouteSet(routes);
	CHECK(sessions.sessions.size() == 2);
	CHECK(sessions.sessions[0].id == "stream1");
	CHECK(sessions.sessions[0].compatibilityDefault);
	CHECK(sessions.sessions[1].id == "youtube-session");
	CHECK(sessions.sessions[1].dynamicBitrateEnabled);

	sessions.sessions[0].deliveryMode = OBS::Output::DeliveryMode::EnhancedMultitrack;
	sessions.sessions[0].capabilities.enhancedMultitrack = true;
	sessions.sessions[0].multitrackConfig.source = OBS::Output::MultitrackConfigSource::StaticJson;
	sessions.sessions[0].multitrackConfig.value = R"({"encoder_configurations":[]})";
	sessions.programs[1].videoFormat.width = 3840;
	sessions.programs[1].videoFormat.height = 2160;
	sessions.programs[1].recordingEligible = true;
	sessions.sessions[0].programBindings.push_back(
		{"youtube-hq", "vertical", OBS::Output::FailoverMode::None, true});
	sessions.replayBuffer.programId = "youtube-hq";

	auto reconciled = OBS::Output::ReconcileRouteSet(sessions, routes);
	CHECK(reconciled.replayBuffer.programId == "youtube-hq");
	CHECK(reconciled.programs[1].replayEligible);
	CHECK(reconciled.sessions[0].deliveryMode == OBS::Output::DeliveryMode::EnhancedMultitrack);
	CHECK(reconciled.sessions[0].multitrackConfig.IsJson());
	CHECK(reconciled.sessions[0].programBindings.size() == 2);
	CHECK(reconciled.sessions[0].programBindings[1].role == "vertical");
	CHECK(reconciled.programs[1].videoFormat.width == 3840);
	CHECK(reconciled.programs[1].videoFormat.height == 2160);
	CHECK(reconciled.programs[1].recordingEligible);

	std::string error;
	OBS::Output::SessionSet restored;
	CHECK(OBS::Output::Deserialize(OBS::Output::Serialize(reconciled), restored, error));
	CHECK(error.empty());
	CHECK(restored.replayBuffer.programId == "youtube-hq");
	CHECK(restored.sessions[1].dynamicBitrateEnabled);

	const auto projected = OBS::Output::ToRouteSet(restored);
	CHECK(projected.routes.size() == 2);
	CHECK(projected.routes[1].destinations.size() == 1);
	CHECK(projected.routes[1].destinations[0].sessionId == "youtube-session");
	CHECK(projected.routes[1].destinations[0].dynamicBitrateEnabled);

	restored.sessions[1].enabled = false;
	const auto disabledProjection = OBS::Output::ToRouteSet(restored);
	CHECK(disabledProjection.routes.size() == 2);
	CHECK(disabledProjection.routes[1].destinations.size() == 1);
	CHECK(!disabledProjection.routes[1].destinations[0].enabled);
	return 0;
}
