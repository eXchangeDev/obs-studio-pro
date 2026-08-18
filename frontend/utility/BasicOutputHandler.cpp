#include "BasicOutputHandler.hpp"
#include "AdvancedOutput.hpp"
#include "SimpleOutput.hpp"

#include <utility/GoLiveAPI_Network.hpp>
#include <utility/MultitrackVideoError.hpp>
#include <utility/StartMultiTrackVideoStreamingGuard.hpp>
#include <utility/VCamConfig.hpp>
#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QThreadPool>

#include <algorithm>
#include <iterator>

using namespace std;

extern bool EncoderAvailable(const char *encoder);
extern std::string DeserializeConfigText(const char *text);

namespace {

bool IsPlatformSessionActive(const OBS::Output::PlatformSession &session)
{
	const auto state = session.State();
	const bool outputActive = session.Output() && obs_output_active(session.Output());
	return state == OBS::Output::SessionRuntimeState::Starting ||
	       ((state == OBS::Output::SessionRuntimeState::Active ||
		 state == OBS::Output::SessionRuntimeState::Stopping) &&
		outputActive) ||
	       outputActive;
}

} // namespace

volatile bool streaming_active = false;
volatile bool recording_active = false;
volatile bool recording_paused = false;
volatile bool replaybuf_active = false;
volatile bool virtualcam_active = false;

void OBSStreamStarting(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");
	if (auto *session = output->FindPlatformSessionForOutput(obj);
	    session && session != output->compatibilitySession) {
		return;
	}

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0) {
		return;
	}

	output->delayActive = true;
	QMetaObject::invokeMethod(output->main, "StreamDelayStarting", Q_ARG(int, sec));
}

void OBSStreamStopping(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	obs_output_t *obj = (obs_output_t *)calldata_ptr(params, "output");
	if (auto *session = output->FindPlatformSessionForOutput(obj);
	    session && session != output->compatibilitySession) {
		return;
	}

	int sec = (int)obs_output_get_active_delay(obj);
	if (sec == 0) {
		QMetaObject::invokeMethod(output->main, "StreamStopping");
	} else {
		QMetaObject::invokeMethod(output->main, "StreamDelayStopping", Q_ARG(int, sec));
	}
}

void OBSStartStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	auto *streamOutput = static_cast<obs_output_t *>(calldata_ptr(params, "output"));
	auto *session = output->FindPlatformSessionForOutput(streamOutput);
	if (!session) {
		session = output->compatibilitySession;
	}
	if (session) {
		session->ClearError();
		session->SetState(OBS::Output::SessionRuntimeState::Active);
	}
	output->UpdateAggregateStreamingState();
}

void OBSStopStreaming(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");
	auto *streamOutput = static_cast<obs_output_t *>(calldata_ptr(params, "output"));
	auto *session = output->FindPlatformSessionForOutput(streamOutput);
	if (!session) {
		session = output->compatibilitySession;
	}
	if (session) {
		if (code == OBS_OUTPUT_SUCCESS) {
			session->ClearError();
			session->SetState(OBS::Output::SessionRuntimeState::Idle);
		} else {
			session->SetError(last_error ? last_error : "stream output stopped");
		}
	}

	output->delayActive = false;
	if (session == output->multitrackSession) {
		output->multitrackVideoActive = false;
	}
	output->UpdateAggregateStreamingState(code, last_error ? last_error : "");
}

void OBSStartRecording(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->recordingActive = true;
	os_atomic_set_bool(&recording_active, true);
	QMetaObject::invokeMethod(output->main, "RecordingStart");
}

void OBSStopRecording(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");
	const char *last_error = calldata_string(params, "last_error");

	QString arg_last_error = QString::fromUtf8(last_error);

	output->recordingActive = false;
	os_atomic_set_bool(&recording_active, false);
	os_atomic_set_bool(&recording_paused, false);
	QMetaObject::invokeMethod(output->main, "RecordingStop", Q_ARG(int, code), Q_ARG(QString, arg_last_error));
}

void OBSRecordStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "RecordStopping");
}

void OBSRecordFileChanged(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	const char *next_file = calldata_string(params, "next_file");

	QString arg_last_file = QString::fromUtf8(output->lastRecordingPath.c_str());

	QMetaObject::invokeMethod(output->main, "RecordingFileChanged", Q_ARG(QString, arg_last_file));

	output->lastRecordingPath = next_file;
}

void OBSStartReplayBuffer(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->replayBufferActive = true;
	os_atomic_set_bool(&replaybuf_active, true);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStart");
}

void OBSStopReplayBuffer(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->replayBufferActive = false;
	os_atomic_set_bool(&replaybuf_active, false);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStop", Q_ARG(int, code));
}

void OBSReplayBufferStopping(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferStopping");
}

void OBSReplayBufferSaved(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	QMetaObject::invokeMethod(output->main, "ReplayBufferSaved", Qt::QueuedConnection);
}

static void OBSStartVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);

	output->virtualCamActive = true;
	os_atomic_set_bool(&virtualcam_active, true);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStart");
}

static void OBSStopVirtualCam(void *data, calldata_t *params)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	int code = (int)calldata_int(params, "code");

	output->virtualCamActive = false;
	os_atomic_set_bool(&virtualcam_active, false);
	QMetaObject::invokeMethod(output->main, "OnVirtualCamStop", Q_ARG(int, code));
}

static void OBSDeactivateVirtualCam(void *data, calldata_t * /* params */)
{
	BasicOutputHandler *output = static_cast<BasicOutputHandler *>(data);
	output->DestroyVirtualCamView();
}

bool return_first_id(void *data, const char *id)
{
	const char **output = (const char **)data;

	*output = id;
	return false;
}

const char *GetStreamOutputType(const obs_service_t *service)
{
	const char *protocol = obs_service_get_protocol(service);
	const char *output = nullptr;

	if (!protocol) {
		blog(LOG_WARNING, "The service '%s' has no protocol set", obs_service_get_id(service));
		return nullptr;
	}

	if (!obs_is_output_protocol_registered(protocol)) {
		blog(LOG_WARNING, "The protocol '%s' is not registered", protocol);
		return nullptr;
	}

	/* Check if the service has a preferred output type */
	output = obs_service_get_preferred_output_type(service);
	if (output) {
		if ((obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0) {
			return output;
		}

		blog(LOG_WARNING, "The output '%s' is not registered, fallback to another one", output);
	}

	/* Otherwise, prefer first-party output types */
	if (can_use_output(protocol, "rtmp_output", "RTMP", "RTMPS")) {
		return "rtmp_output";
	} else if (can_use_output(protocol, "ffmpeg_hls_muxer", "HLS")) {
		return "ffmpeg_hls_muxer";
	} else if (can_use_output(protocol, "ffmpeg_mpegts_muxer", "SRT", "RIST")) {
		return "ffmpeg_mpegts_muxer";
	}

	/* If third-party protocol, use the first enumerated type */
	obs_enum_output_types_with_protocol(protocol, &output, return_first_id);
	if (output) {
		return output;
	}

	blog(LOG_WARNING, "No output compatible with the service '%s' is registered", obs_service_get_id(service));

	return nullptr;
}

BasicOutputHandler::BasicOutputHandler(OBSBasic *main_) : main(main_)
{
	if (main->vcamEnabled) {
		virtualCam = obs_output_create(VIRTUAL_CAM_ID, "virtualcam_output", nullptr, nullptr);

		signal_handler_t *signal = obs_output_get_signal_handler(virtualCam);
		startVirtualCam.Connect(signal, "start", OBSStartVirtualCam, this);
		stopVirtualCam.Connect(signal, "stop", OBSStopVirtualCam, this);
		deactivateVirtualCam.Connect(signal, "deactivate", OBSDeactivateVirtualCam, this);
	}

	if (config_get_int(main->Config(), "Stream1", "WHIPSimulcastTotalLayers") > 1) {
		whipSimulcastEncoders = make_unique<WHIPSimulcastEncoders>();
	}
}

bool BasicOutputHandler::LoadPlatformSessions(OBS::Output::RouteSet &routes, std::string &error)
{
	platformSessions = {};
	const char *serializedSessions = config_get_string(main->Config(), "Stream1", "PlatformSessions");
	const char *serializedRoutes = config_get_string(main->Config(), "Stream1", "OutputRoutes");
	routes = {};

	if (serializedSessions && *serializedSessions) {
		if (!OBS::Output::Deserialize(serializedSessions, platformSessions, error)) {
			blog(LOG_WARNING, "OBS Studio Pro: failed to parse platform sessions: %s", error.c_str());
			platformSessions = {};
		} else {
			routes = OBS::Output::ToRouteSet(platformSessions);
		}
	}

	if (routes.routes.empty() && serializedRoutes && *serializedRoutes) {
		if (!OBS::Output::Deserialize(serializedRoutes, routes, error)) {
			blog(LOG_WARNING, "OBS Studio Pro: failed to parse additional output routes: %s",
			     error.c_str());
			return false;
		}
		// Keep the in-memory session model authoritative after a legacy profile
		// is loaded. Settings saving writes both projections until all callers
		// have moved to the session model.
		platformSessions = OBS::Output::MigrateRouteSet(routes);
		routes = OBS::Output::ToRouteSet(platformSessions);
	}

	error.clear();
	return true;
}

OBS::Output::PlatformSession *BasicOutputHandler::FindPlatformSession(std::string_view sessionId) const
{
	const auto found = std::find_if(platformSessionRuntimes.begin(), platformSessionRuntimes.end(),
					[&](const auto &session) { return session->Config().id == sessionId; });
	return found == platformSessionRuntimes.end() ? nullptr : found->get();
}

OBS::Output::PlatformSession *BasicOutputHandler::FindPlatformSessionForOutput(obs_output_t *output) const
{
	if (!output) {
		return nullptr;
	}
	const auto found = std::find_if(platformSessionRuntimes.begin(), platformSessionRuntimes.end(),
					[&](const auto &session) { return session->Output() == output; });
	return found == platformSessionRuntimes.end() ? nullptr : found->get();
}

void BasicOutputHandler::OutputRouteStateChanged(std::string_view sessionId, OBS::Output::RuntimeState state,
						 std::string_view error)
{
	if (auto *session = FindPlatformSession(sessionId)) {
		switch (state) {
		case OBS::Output::RuntimeState::Idle:
			session->ClearError();
			session->SetState(OBS::Output::SessionRuntimeState::Idle);
			break;
		case OBS::Output::RuntimeState::Starting:
			session->SetState(OBS::Output::SessionRuntimeState::Starting);
			break;
		case OBS::Output::RuntimeState::Active:
			session->ClearError();
			session->SetState(OBS::Output::SessionRuntimeState::Active);
			break;
		case OBS::Output::RuntimeState::Stopping:
			session->SetState(OBS::Output::SessionRuntimeState::Stopping);
			break;
		case OBS::Output::RuntimeState::Failed:
			session->SetError(error.empty() ? "session output failed" : std::string(error));
			break;
		}
	}

	if (state == OBS::Output::RuntimeState::Active || state == OBS::Output::RuntimeState::Idle ||
	    state == OBS::Output::RuntimeState::Failed) {
		UpdateAggregateStreamingState(
			state == OBS::Output::RuntimeState::Failed ? OBS_OUTPUT_ERROR : OBS_OUTPUT_SUCCESS, error);
	}
}

void BasicOutputHandler::UpdateAggregateStreamingState(int code, std::string_view error)
{
	bool anySessionActive = outputRoutes.Active();
	if (!anySessionActive) {
		anySessionActive = std::any_of(platformSessionRuntimes.begin(), platformSessionRuntimes.end(),
					       [](const auto &session) { return IsPlatformSessionActive(*session); });
	}

	if (anySessionActive) {
		if (!streamingActive) {
			streamingActive = true;
			os_atomic_set_bool(&streaming_active, true);
			QMetaObject::invokeMethod(main, "StreamingStart");
		}
		return;
	}

	if (!streamingActive) {
		return;
	}

	streamingActive = false;
	os_atomic_set_bool(&streaming_active, false);
	const QString lastError = error.empty() ? QString{}
						: QString::fromUtf8(error.data(), static_cast<int>(error.size()));
	QMetaObject::invokeMethod(main, "StreamingStop", Q_ARG(int, code), Q_ARG(QString, lastError));
}

OBS::Output::PlatformSession *BasicOutputHandler::PrepareCompatibilitySession(obs_service_t *service)
{
	if (!service) {
		return nullptr;
	}

	OBS::Output::RouteSet routes;
	std::string error;
	if (!LoadPlatformSessions(routes, error)) {
		blog(LOG_WARNING, "OBS Studio Pro: cannot prepare platform sessions: %s", error.c_str());
		return nullptr;
	}

	auto primaryProgram =
		std::find_if(platformSessions.programs.begin(), platformSessions.programs.end(),
			     [](const OBS::Output::Program &program) { return program.compatibilityDefault; });
	if (primaryProgram == platformSessions.programs.end()) {
		OBS::Output::Program program;
		program.id = "program-main";
		program.name = "Main Output";
		OBSCanvasAutoRelease mainCanvas = obs_get_main_canvas();
		program.canvas = OBS::Output::CanvasReferenceFromCanvas(mainCanvas);
		program.compatibilityDefault = true;
		platformSessions.programs.insert(platformSessions.programs.begin(), std::move(program));
		primaryProgram = platformSessions.programs.begin();
	}

	auto compatibility =
		std::find_if(platformSessions.sessions.begin(), platformSessions.sessions.end(),
			     [](const OBS::Output::PlatformSessionConfig &session) { return session.id == "stream1"; });
	if (compatibility == platformSessions.sessions.end()) {
		OBS::Output::PlatformSessionConfig config;
		config.id = "stream1";
		config.name = "Primary Stream";
		config.authenticationReference = "stream1";
		config.programBindings.push_back(
			{primaryProgram->id, "primary", OBS::Output::FailoverMode::None, true});
		platformSessions.sessions.insert(platformSessions.sessions.begin(), std::move(config));
		compatibility = platformSessions.sessions.begin();
	}

	for (auto &session : platformSessions.sessions) {
		session.compatibilityDefault = session.id == compatibility->id;
	}
	const bool hasPrimaryBinding =
		std::any_of(compatibility->programBindings.begin(), compatibility->programBindings.end(),
			    [&](const OBS::Output::ProgramBinding &binding) {
				    return binding.enabled && binding.programId == primaryProgram->id;
			    });
	if (!hasPrimaryBinding) {
		compatibility->programBindings.push_back(
			{primaryProgram->id, "primary", OBS::Output::FailoverMode::None, true});
	}

	const char *extraCanvasUuid = config_get_string(main->Config(), "Stream1", "MultitrackExtraCanvas");
	if (extraCanvasUuid && *extraCanvasUuid) {
		auto verticalProgram = std::find_if(platformSessions.programs.begin(), platformSessions.programs.end(),
						    [&](const OBS::Output::Program &program) {
							    return program.canvas.uuid == extraCanvasUuid;
						    });
		if (verticalProgram == platformSessions.programs.end()) {
			OBSCanvasAutoRelease canvas = obs_get_canvas_by_uuid(extraCanvasUuid);
			if (canvas) {
				OBS::Output::Program program;
				program.id = "program-canvas-" + std::string(extraCanvasUuid);
				program.name = obs_canvas_get_name(canvas);
				program.canvas = OBS::Output::CanvasReferenceFromCanvas(canvas);
				platformSessions.programs.emplace_back(std::move(program));
				verticalProgram = std::prev(platformSessions.programs.end());
			}
		}
		if (verticalProgram != platformSessions.programs.end()) {
			const bool alreadyBound = std::any_of(compatibility->programBindings.begin(),
							      compatibility->programBindings.end(),
							      [&](const OBS::Output::ProgramBinding &binding) {
								      return binding.enabled &&
									     binding.programId == verticalProgram->id;
							      });
			if (!alreadyBound) {
				compatibility->programBindings.push_back(
					{verticalProgram->id, "vertical", OBS::Output::FailoverMode::None, true});
			}
		}
	}

	OBSDataAutoRelease serviceSettings = obs_service_get_settings(service);
	const char *platformId = obs_data_get_string(serviceSettings, "service");
	if (!platformId || !*platformId) {
		platformId = obs_data_get_string(serviceSettings, "service_name");
	}
	if (!platformId || !*platformId) {
		platformId = obs_service_get_id(service);
	}
	compatibility->platformId = platformId && *platformId ? platformId : "obs-service";
	if (compatibility->name.empty() || compatibility->name == "Primary Stream") {
		compatibility->name = compatibility->platformId;
	}

	if (config_get_bool(main->Config(), "Stream1", "MultitrackVideoConfigOverrideEnabled")) {
		compatibility->multitrackConfig.source = OBS::Output::MultitrackConfigSource::CustomJson;
		compatibility->multitrackConfig.value = DeserializeConfigText(
			config_get_string(main->Config(), "Stream1", "MultitrackVideoConfigOverride"));
	} else if (compatibility->multitrackConfig.value.empty()) {
		const QString autoConfigUrl = MultitrackVideoAutoConfigURL(service);
		if (!autoConfigUrl.isEmpty()) {
			compatibility->multitrackConfig.source = OBS::Output::MultitrackConfigSource::RemoteProviderUrl;
			compatibility->multitrackConfig.value = autoConfigUrl.toStdString();
		}
	}

	compatibility->capabilities.standardStreaming = true;
	compatibility->capabilities.enhancedMultitrack = !compatibility->multitrackConfig.value.empty();
	const bool enhancedRequested = config_get_bool(main->Config(), "Stream1", "EnableMultitrackVideo") ||
				       compatibility->deliveryMode == OBS::Output::DeliveryMode::EnhancedMultitrack;
	compatibility->deliveryMode = enhancedRequested && compatibility->capabilities.enhancedMultitrack
					      ? OBS::Output::DeliveryMode::EnhancedMultitrack
					      : OBS::Output::DeliveryMode::Standard;

	platformSessionRuntimes.clear();
	multitrackSession = nullptr;
	compatibilitySession = nullptr;
	multitrackVideo = nullptr;
	multitrackVideoActive = false;
	OBS::Output::PlatformSession *compatibilityRuntime = nullptr;
	for (const auto &config : platformSessions.sessions) {
		auto runtime = std::make_shared<OBS::Output::PlatformSession>(config);
		if (config.id == compatibility->id) {
			runtime->AttachService(service);
			compatibilityRuntime = runtime.get();
		}
		platformSessionRuntimes.emplace_back(std::move(runtime));
	}
	compatibilitySession = compatibilityRuntime;

	if (compatibilityRuntime &&
	    compatibilityRuntime->Config().deliveryMode == OBS::Output::DeliveryMode::EnhancedMultitrack) {
		multitrackSession = compatibilityRuntime;
		multitrackVideo = &compatibilityRuntime->EnsureMultitrackOutput();
	}
	return compatibilityRuntime;
}

static OBS::Output::RuntimeOptions OutputRuntimeOptions(BasicOutputHandler *handler,
							const OBS::Output::SessionSet &sessions)
{
	OBSBasic *main = handler->main;
	OBS::Output::RuntimeOptions options;
	const char *bindIp = config_get_string(main->Config(), "Output", "BindIP");
	const char *ipFamily = config_get_string(main->Config(), "Output", "IPFamily");
	if (bindIp && *bindIp) {
		options.bindIp = bindIp;
	}
	if (ipFamily && *ipFamily) {
		options.ipFamily = ipFamily;
	}

	if (config_get_bool(main->Config(), "Output", "Reconnect")) {
		options.reconnectRetryCount = config_get_int(main->Config(), "Output", "MaxRetries");
		options.reconnectRetrySeconds = config_get_int(main->Config(), "Output", "RetryDelay");
	} else {
		options.reconnectRetryCount = 0;
	}

	if (config_get_bool(main->Config(), "Output", "DelayEnable")) {
		options.delaySeconds = config_get_int(main->Config(), "Output", "DelaySec");
		if (config_get_bool(main->Config(), "Output", "DelayPreserve")) {
			options.delayFlags = OBS_OUTPUT_DELAY_PRESERVE;
		}
	}

	if (!sessions.replayBuffer.programId.empty()) {
		options.retainedProgramIds.emplace_back(sessions.replayBuffer.programId);
	}
	options.stateChanged = [handler](std::string_view sessionId, OBS::Output::RuntimeState state,
					 std::string_view error) {
		handler->OutputRouteStateChanged(sessionId, state, error);
	};
	return options;
}

void BasicOutputHandler::AttachOutputRouteSessions()
{
	for (const auto &session : platformSessionRuntimes) {
		if (session.get() == compatibilitySession) {
			continue;
		}
		auto service = outputRoutes.SessionService(session->Config().id);
		auto output = outputRoutes.SessionOutput(session->Config().id);
		if (service) {
			session->AttachService(service);
		}
		if (output) {
			session->AttachOutput(output);
		}
	}
}

bool BasicOutputHandler::PrepareOutputRoutes(obs_output_t *referenceOutput)
{
	if (!referenceOutput) {
		blog(LOG_WARNING, "OBS Studio Pro: cannot prepare Programs without a reference output");
		return false;
	}
	return PrepareOutputRoutes(obs_output_get_video_encoder(referenceOutput),
				   obs_output_get_audio_encoder(referenceOutput, 0));
}

bool BasicOutputHandler::PrepareOutputRoutes(obs_encoder_t *referenceVideo, obs_encoder_t *referenceAudio)
{
	OBS::Output::RouteSet routes;
	std::string error;
	if (!LoadPlatformSessions(routes, error)) {
		return false;
	}
	if (replayBufferActive && !platformSessions.replayBuffer.programId.empty()) {
		auto replayVideo = outputRoutes.ProgramVideoEncoder(platformSessions.replayBuffer.programId);
		auto replayAudio = outputRoutes.ProgramAudioEncoder(platformSessions.replayBuffer.programId);
		if (replayVideo && replayAudio) {
			blog(LOG_INFO,
			     "OBS Studio Pro: retaining prepared Program encoders while the replay buffer is active");
			AttachOutputRouteSessions();
			return true;
		}
	}
	if (outputRoutes.Active()) {
		blog(LOG_INFO, "OBS Studio Pro: retaining active platform session outputs");
		AttachOutputRouteSessions();
		return true;
	}

	if (routes.routes.empty()) {
		outputRoutes.Clear();
		return true;
	}

	const auto options = OutputRuntimeOptions(this, platformSessions);
	if (!outputRoutes.Prepare(routes, referenceVideo, referenceAudio, options, error)) {
		blog(LOG_WARNING, "OBS Studio Pro: failed to prepare additional outputs: %s", error.c_str());
		return false;
	}

	blog(LOG_INFO, "OBS Studio Pro: prepared %zu additional output destination(s)",
	     outputRoutes.PreparedDestinationCount());
	AttachOutputRouteSessions();
	return true;
}

ReplayProgramBindingResult BasicOutputHandler::ConfigureReplayBufferProgram(obs_encoder_t *referenceVideo,
									    obs_encoder_t *referenceAudio,
									    std::string &error)
{
	OBS::Output::RouteSet routes;
	if (!LoadPlatformSessions(routes, error)) {
		return ReplayProgramBindingResult::Failed;
	}

	const std::string &programId = platformSessions.replayBuffer.programId;
	if (programId.empty()) {
		error.clear();
		return ReplayProgramBindingResult::Legacy;
	}

	auto video = outputRoutes.ProgramVideoEncoder(programId);
	auto audio = outputRoutes.ProgramAudioEncoder(programId);
	if (!video || !audio) {
		if (!referenceVideo || !referenceAudio) {
			error = "the reference stream encoders are missing";
			return ReplayProgramBindingResult::Failed;
		}
		const auto options = OutputRuntimeOptions(this, platformSessions);
		if (!outputRoutes.Prepare(routes, referenceVideo, referenceAudio, options, error)) {
			return ReplayProgramBindingResult::Failed;
		}
		AttachOutputRouteSessions();
		video = outputRoutes.ProgramVideoEncoder(programId);
		audio = outputRoutes.ProgramAudioEncoder(programId);
	}

	if (!video || !audio) {
		error = "the replay Program has no prepared encoder pair";
		return ReplayProgramBindingResult::Failed;
	}

	obs_output_set_video_encoder(replayBuffer, video);
	for (size_t index = 0; index < MAX_AUDIO_MIXES; ++index) {
		obs_output_set_audio_encoder(replayBuffer, nullptr, index);
	}
	obs_output_set_audio_encoder(replayBuffer, audio, 0);

	const auto program = std::find_if(platformSessions.programs.begin(), platformSessions.programs.end(),
					  [&](const OBS::Output::Program &item) { return item.id == programId; });
	blog(LOG_INFO, "OBS Studio Pro: replay buffer uses Program '%s'",
	     program != platformSessions.programs.end() ? program->name.c_str() : programId.c_str());
	error.clear();
	return ReplayProgramBindingResult::Configured;
}

size_t BasicOutputHandler::StartOutputRoutes()
{
	const size_t started = outputRoutes.Start();
	if (outputRoutes.PreparedDestinationCount() > 0) {
		blog(LOG_INFO, "OBS Studio Pro: started %zu of %zu additional output destination(s)", started,
		     outputRoutes.PreparedDestinationCount());
	}
	return started;
}

size_t BasicOutputHandler::StartOutputSession(std::string_view sessionId)
{
	auto *session = FindPlatformSession(sessionId);
	if (session && !session->Config().enabled) {
		return 0;
	}
	size_t started = outputRoutes.StartSession(sessionId);
	if (!session) {
		return started;
	}
	if (session == compatibilitySession && session->Config().deliveryMode == OBS::Output::DeliveryMode::Standard &&
	    streamOutput && !obs_output_active(streamOutput)) {
		session->SetState(OBS::Output::SessionRuntimeState::Starting);
		if (obs_output_start(streamOutput)) {
			++started;
		} else {
			const char *lastError = obs_output_get_last_error(streamOutput);
			session->SetError(lastError && *lastError ? lastError : "stream output failed to start");
		}
		return started;
	}
	if (session->Config().deliveryMode != OBS::Output::DeliveryMode::EnhancedMultitrack ||
	    !session->MultitrackOutput()) {
		return started;
	}

	auto output = session->MultitrackOutput()->StreamingOutput();
	if (!output || obs_output_active(output)) {
		return started;
	}
	session->SetState(OBS::Output::SessionRuntimeState::Starting);
	if (obs_output_start(output)) {
		session->MultitrackOutput()->StartedStreaming();
		++started;
	} else {
		const char *lastError = obs_output_get_last_error(output);
		session->SetError(lastError && *lastError ? lastError : "multitrack output failed to start");
	}
	return started;
}

void BasicOutputHandler::StopOutputRoutes(bool force)
{
	outputRoutes.Stop(force);
}

void BasicOutputHandler::StopOutputSession(std::string_view sessionId, bool force)
{
	outputRoutes.StopSession(sessionId, force);
	auto *session = FindPlatformSession(sessionId);
	if (!session) {
		return;
	}

	if (session->Config().deliveryMode == OBS::Output::DeliveryMode::EnhancedMultitrack &&
	    session->MultitrackOutput()) {
		session->SetState(OBS::Output::SessionRuntimeState::Stopping);
		if (force) {
			auto output = session->MultitrackOutput()->StreamingOutput();
			if (output) {
				obs_output_force_stop(output);
			}
		} else {
			session->MultitrackOutput()->StopStreaming();
		}
		return;
	}

	if (session == compatibilitySession && streamOutput && obs_output_active(streamOutput)) {
		session->SetState(OBS::Output::SessionRuntimeState::Stopping);
		if (force) {
			obs_output_force_stop(streamOutput);
		} else {
			obs_output_stop(streamOutput);
		}
	}
}

std::vector<OBS::Output::SessionSnapshot> BasicOutputHandler::OutputSessionSnapshots() const
{
	auto snapshots = outputRoutes.SessionSnapshots();
	for (const auto &runtime : platformSessionRuntimes) {
		const auto found = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto &snapshot) {
			return snapshot.sessionId == runtime->Config().id;
		});
		if (found != snapshots.end() && runtime.get() != compatibilitySession) {
			continue;
		}

		OBS::Output::SessionSnapshot snapshot;
		snapshot.sessionId = runtime->Config().id;
		snapshot.lastError = runtime->LastError();
		snapshot.endpointCount = std::max<size_t>(1, runtime->Config().endpoints.size());
		switch (runtime->State()) {
		case OBS::Output::SessionRuntimeState::Validating:
		case OBS::Output::SessionRuntimeState::Preparing:
		case OBS::Output::SessionRuntimeState::Starting:
			snapshot.state = OBS::Output::RuntimeState::Starting;
			break;
		case OBS::Output::SessionRuntimeState::Active:
			snapshot.state = OBS::Output::RuntimeState::Active;
			break;
		case OBS::Output::SessionRuntimeState::Stopping:
			snapshot.state = OBS::Output::RuntimeState::Stopping;
			break;
		case OBS::Output::SessionRuntimeState::Failed:
			snapshot.state = OBS::Output::RuntimeState::Failed;
			break;
		case OBS::Output::SessionRuntimeState::Idle:
			snapshot.state = OBS::Output::RuntimeState::Idle;
			break;
		}
		if (found == snapshots.end()) {
			snapshots.emplace_back(std::move(snapshot));
		} else {
			*found = std::move(snapshot);
		}
	}
	return snapshots;
}

extern void log_vcam_changed(const VCamConfig &config, bool starting);

bool BasicOutputHandler::StartVirtualCam()
{
	if (!main->vcamEnabled) {
		return false;
	}

	bool typeIsProgram = main->vcamConfig.type == VCamOutputType::ProgramView;

	if (!virtualCamView && !typeIsProgram) {
		virtualCamView = obs_view_create();
	}

	UpdateVirtualCamOutputSource();

	if (!virtualCamVideo) {
		virtualCamVideo = typeIsProgram ? obs_get_video() : obs_view_add(virtualCamView);

		if (!virtualCamVideo) {
			return false;
		}
	}

	obs_output_set_media(virtualCam, virtualCamVideo, obs_get_audio());
	if (!Active()) {
		SetupOutputs();
	}

	bool success = obs_output_start(virtualCam);
	if (!success) {
		QString errorReason;

		const char *error = obs_output_get_last_error(virtualCam);
		if (error) {
			errorReason = QT_UTF8(error);
		} else {
			errorReason = QTStr("Output.StartFailedGeneric");
		}

		QMessageBox::critical(main, QTStr("Output.StartVirtualCamFailed"), errorReason);

		DestroyVirtualCamView();
	}

	log_vcam_changed(main->vcamConfig, true);

	return success;
}

void BasicOutputHandler::StopVirtualCam()
{
	if (main->vcamEnabled) {
		obs_output_stop(virtualCam);
	}
}

bool BasicOutputHandler::VirtualCamActive() const
{
	if (main->vcamEnabled) {
		return obs_output_active(virtualCam);
	}
	return false;
}

void BasicOutputHandler::UpdateVirtualCamOutputSource()
{
	if (!main->vcamEnabled || !virtualCamView) {
		return;
	}

	OBSSourceAutoRelease source;

	switch (main->vcamConfig.type) {
	case VCamOutputType::Invalid:
	case VCamOutputType::ProgramView:
		DestroyVirtualCameraScene();
		return;
	case VCamOutputType::PreviewOutput: {
		DestroyVirtualCameraScene();
		OBSSource s = main->GetCurrentSceneSource();
		obs_source_get_ref(s);
		source = s.Get();
		break;
	}
	case VCamOutputType::SceneOutput:
		DestroyVirtualCameraScene();
		source = obs_get_source_by_name(main->vcamConfig.scene.c_str());
		break;
	case VCamOutputType::SourceOutput:
		OBSSourceAutoRelease s = obs_get_source_by_name(main->vcamConfig.source.c_str());

		if (!vCamSourceScene) {
			vCamSourceScene = obs_scene_create_private("vcam_source");
		}
		source = obs_source_get_ref(obs_scene_get_source(vCamSourceScene));

		if (vCamSourceSceneItem && (obs_sceneitem_get_source(vCamSourceSceneItem) != s)) {
			obs_sceneitem_remove(vCamSourceSceneItem);
			vCamSourceSceneItem = nullptr;
		}

		if (!vCamSourceSceneItem) {
			vCamSourceSceneItem = obs_scene_add(vCamSourceScene, s);

			obs_sceneitem_set_bounds_type(vCamSourceSceneItem, OBS_BOUNDS_SCALE_INNER);
			obs_sceneitem_set_bounds_alignment(vCamSourceSceneItem, OBS_ALIGN_CENTER);

			const struct vec2 size = {
				(float)obs_source_get_width(source),
				(float)obs_source_get_height(source),
			};
			obs_sceneitem_set_bounds(vCamSourceSceneItem, &size);
		}
		break;
	}

	OBSSourceAutoRelease current = obs_view_get_source(virtualCamView, 0);
	if (source != current) {
		obs_view_set_source(virtualCamView, 0, source);
	}
}

void BasicOutputHandler::DestroyVirtualCamView()
{
	if (main->vcamConfig.type == VCamOutputType::ProgramView) {
		virtualCamVideo = nullptr;
		return;
	}

	obs_view_remove(virtualCamView);
	obs_view_set_source(virtualCamView, 0, nullptr);
	virtualCamVideo = nullptr;

	obs_view_destroy(virtualCamView);
	virtualCamView = nullptr;

	DestroyVirtualCameraScene();
}

void BasicOutputHandler::DestroyVirtualCameraScene()
{
	if (!vCamSourceScene) {
		return;
	}

	obs_scene_release(vCamSourceScene);
	vCamSourceScene = nullptr;
	vCamSourceSceneItem = nullptr;
}

const char *FindAudioEncoderFromCodec(const char *type)
{
	const char *alt_enc_id = nullptr;
	size_t i = 0;

	while (obs_enum_encoder_types(i++, &alt_enc_id)) {
		const char *codec = obs_get_encoder_codec(alt_enc_id);
		if (strcmp(type, codec) == 0) {
			return alt_enc_id;
		}
	}

	return nullptr;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name)
{
	obs_encoder_t *last = obs_output_get_audio_encoder(output, 1);
	bool clear = false;

	/* ensures that we don't remove twitch's soundtrack encoder */
	if (last) {
		const char *name = obs_encoder_get_name(last);
		clear = name && strcmp(name, expected_name) == 0;
		obs_encoder_release(last);
	}

	if (clear) {
		obs_output_set_audio_encoder(output, nullptr, 1);
	}
}

void BasicOutputHandler::SetupAutoRemux(const char *&container)
{
	bool autoRemux = config_get_bool(main->Config(), "Video", "AutoRemux");
	if (autoRemux && strcmp(container, "mp4") == 0) {
		container = "mkv";
	}
}

std::string BasicOutputHandler::GetRecordingFilename(const char *path, const char *container, bool noSpace,
						     bool overwrite, const char *format, bool ffmpeg)
{
	if (!ffmpeg) {
		SetupAutoRemux(container);
	}

	string dst = GetOutputFilename(path, container, noSpace, overwrite, format);
	lastRecordingPath = dst;
	return dst;
}

std::shared_future<void> BasicOutputHandler::SetupMultitrackVideo(OBS::Output::PlatformSession &session,
								  std::string audio_encoder_id, size_t main_audio_mixer,
								  std::optional<size_t> vod_track_mixer,
								  std::function<void(std::optional<bool>)> continuation)
{
	auto start_streaming_guard = std::make_shared<StartMultitrackVideoStreamingGuard>();
	obs_service_t *service = session.Service();
	if (!service || !session.Config().enabled ||
	    session.Config().deliveryMode != OBS::Output::DeliveryMode::EnhancedMultitrack) {
		continuation(std::nullopt);
		return start_streaming_guard->GetFuture();
	}

	multitrackSession = &session;
	multitrackVideo = &session.EnsureMultitrackOutput();
	session.SetState(OBS::Output::SessionRuntimeState::Preparing);
	const auto sessionOwner = std::find_if(platformSessionRuntimes.begin(), platformSessionRuntimes.end(),
					       [&](const auto &runtime) { return runtime.get() == &session; });
	if (sessionOwner == platformSessionRuntimes.end()) {
		session.SetError("platform session runtime is no longer available");
		continuation(false);
		return start_streaming_guard->GetFuture();
	}
	const std::shared_ptr<OBS::Output::PlatformSession> retainedSession = *sessionOwner;

	multitrackVideoActive = false;

	streamDelayStarting.Disconnect();
	streamStopping.Disconnect();
	startStreaming.Disconnect();
	stopStreaming.Disconnect();

	OBS::Output::MultitrackConfigProvider config_provider = session.Config().multitrackConfig;
	if (config_provider.value.empty()) {
		session.SetError("enhanced multitrack has no configuration provider");
		session.SetState(OBS::Output::SessionRuntimeState::Idle);
		continuation(std::nullopt);
		return start_streaming_guard->GetFuture();
	}

	std::vector<std::string> canvasUuids;
	for (const auto &binding : session.Config().programBindings) {
		if (!binding.enabled) {
			continue;
		}
		const auto program =
			std::find_if(platformSessions.programs.begin(), platformSessions.programs.end(),
				     [&](const OBS::Output::Program &item) { return item.id == binding.programId; });
		if (program == platformSessions.programs.end()) {
			continue;
		}
		OBSCanvasAutoRelease canvas = OBS::Output::ResolveCanvas(program->canvas);
		if (!canvas) {
			session.SetError("Program '" + program->name + "' references a missing canvas");
			continuation(false);
			return start_streaming_guard->GetFuture();
		}
		const char *uuid = obs_canvas_get_uuid(canvas);
		if (!uuid || !*uuid || std::find(canvasUuids.begin(), canvasUuids.end(), uuid) != canvasUuids.end()) {
			continue;
		}
		canvasUuids.emplace_back(uuid);
	}

	OBSDataAutoRelease settings = obs_service_get_settings(service);
	QString key = obs_data_get_string(settings, "key");
	const std::string serviceName = session.Config().platformId.empty() ? session.Config().name
									    : session.Config().platformId;

	std::optional<std::string> custom_rtmp_url;
	std::optional<bool> use_rtmps;
	auto server = obs_data_get_string(settings, "server");
	if (strncmp(server, "auto", 4) != 0) {
		custom_rtmp_url = server;
	} else {
		QString server_ = server;
		use_rtmps = server_.contains("rtmps", Qt::CaseInsensitive);
	}

	auto service_custom_server = obs_data_get_bool(settings, "using_custom_server");
	if (custom_rtmp_url.has_value()) {
		blog(LOG_INFO, "Using %sserver URL from the PlatformSession service",
		     service_custom_server ? "custom " : "");
	}

	auto maximum_aggregate_bitrate =
		config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrateAuto")
			? std::nullopt
			: std::make_optional<uint32_t>(
				  config_get_int(main->Config(), "Stream1", "MultitrackVideoMaximumAggregateBitrate"));

	auto maximum_video_tracks = config_get_bool(main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracksAuto")
					    ? std::nullopt
					    : std::make_optional<uint32_t>(config_get_int(
						      main->Config(), "Stream1", "MultitrackVideoMaximumVideoTracks"));

	auto stream_dump_config = GenerateMultitrackVideoStreamDumpConfig();

	auto *sessionMultitrack = multitrackVideo;
	auto continue_on_main_thread = [this, retainedSession, sessionMultitrack, start_streaming_guard,
					service = OBSService{service}, continuation = std::move(continuation)](
					       std::optional<MultitrackVideoError> error) {
		auto *sessionRuntime = retainedSession.get();
		if (FindPlatformSession(sessionRuntime->Config().id) != sessionRuntime) {
			sessionRuntime->DisconnectOutputSignals();
			return continuation(false);
		}
		if (error) {
			OBSDataAutoRelease service_settings = obs_service_get_settings(service);
			auto multitrack_video_name = QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
			if (obs_data_has_user_value(service_settings, "multitrack_video_name")) {
				multitrack_video_name = obs_data_get_string(service_settings, "multitrack_video_name");
			}

			multitrackVideoActive = false;
			sessionRuntime->SetError(error->error.toStdString());
			if (!error->ShowDialog(main, multitrack_video_name)) {
				return continuation(false);
			}
			sessionRuntime->SetState(OBS::Output::SessionRuntimeState::Idle);
			return continuation(std::nullopt);
		}

		multitrackVideoActive = true;
		sessionRuntime->ClearError();
		sessionRuntime->SetState(OBS::Output::SessionRuntimeState::Starting);
		auto streamingOutput = sessionMultitrack->StreamingOutput();
		sessionRuntime->AttachOutput(streamingOutput);

		auto signal_handler = sessionMultitrack->StreamingSignalHandler();

		sessionRuntime->DisconnectOutputSignals();
		sessionRuntime->StartingSignal().Connect(signal_handler, "starting", OBSStreamStarting, this);
		sessionRuntime->StoppingSignal().Connect(signal_handler, "stopping", OBSStreamStopping, this);
		sessionRuntime->StartedSignal().Connect(signal_handler, "start", OBSStartStreaming, this);
		sessionRuntime->StoppedSignal().Connect(signal_handler, "stop", OBSStopStreaming, this);
		return continuation(true);
	};

	QThreadPool::globalInstance()->start([=, main = main, service = OBSService{service},
					      stream_dump_config = OBSData{stream_dump_config},
					      start_streaming_guard = start_streaming_guard]() mutable {
		std::optional<MultitrackVideoError> error;
		try {
			sessionMultitrack->PrepareStreaming(main, serviceName.c_str(), service, custom_rtmp_url, key,
							    audio_encoder_id.c_str(), maximum_aggregate_bitrate,
							    maximum_video_tracks, std::move(config_provider),
							    stream_dump_config, main_audio_mixer, vod_track_mixer,
							    use_rtmps, canvasUuids);
		} catch (const MultitrackVideoError &error_) {
			error.emplace(error_);
		}

		QMetaObject::invokeMethod(main, [=] { continue_on_main_thread(error); });
	});

	return start_streaming_guard->GetFuture();
}

OBSDataAutoRelease BasicOutputHandler::GenerateMultitrackVideoStreamDumpConfig()
{
	auto stream_dump_enabled = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpEnabled");

	if (!stream_dump_enabled) {
		return nullptr;
	}

	const char *path = config_get_string(main->Config(), "SimpleOutput", "FilePath");
	bool noSpace = config_get_bool(main->Config(), "SimpleOutput", "FileNameWithoutSpace");
	const char *filenameFormat = config_get_string(main->Config(), "Output", "FilenameFormatting");
	bool overwriteIfExists = config_get_bool(main->Config(), "Output", "OverwriteIfExists");
	bool useMP4 = config_get_bool(main->Config(), "Stream1", "MultitrackVideoStreamDumpAsMP4");

	string f;

	OBSDataAutoRelease settings = obs_data_create();
	f = GetFormatString(filenameFormat, nullptr, nullptr);
	string strPath = GetRecordingFilename(path, useMP4 ? "mp4" : "flv", noSpace, overwriteIfExists, f.c_str(),
					      // never remux stream dump
					      false);
	obs_data_set_string(settings, "path", strPath.c_str());

	if (useMP4) {
		obs_data_set_bool(settings, "use_mp4", true);
		obs_data_set_string(settings, "muxer_settings", "write_encoder_info=1");
	}

	return settings;
}

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main)
{
	return new SimpleOutput(main);
}

BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main)
{
	return new AdvancedOutput(main);
}
