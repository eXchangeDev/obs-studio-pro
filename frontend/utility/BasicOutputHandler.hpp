#pragma once

#include <utility/MultitrackVideoOutput.hpp>
#include <utility/PlatformSession.hpp>
#include <utility/OutputRouteRuntime.hpp>
#include <utility/WHIPSimulcastEncoders.hpp>

#include <obs.hpp>
#include <util/dstr.hpp>

#include <functional>
#include <future>
#include <string_view>
#include <vector>

#define RTMP_PROTOCOL "rtmp"
#define SRT_PROTOCOL "srt"
#define RIST_PROTOCOL "rist"

class OBSBasic;

using SetupStreamingContinuation_t = std::function<void(bool)>;

enum class ReplayProgramBindingResult {
	Legacy,
	Configured,
	Failed,
};

struct BasicOutputHandler {
	OBSOutputAutoRelease fileOutput;
	OBSOutputAutoRelease streamOutput;
	OBSOutputAutoRelease replayBuffer;
	OBSOutputAutoRelease virtualCam;
	bool streamingActive = false;
	bool recordingActive = false;
	bool delayActive = false;
	bool replayBufferActive = false;
	bool virtualCamActive = false;
	OBSBasic *main;

	// Compatibility alias only. The actual MultitrackVideoOutput is owned by
	// multitrackSession so provider state and failures remain session-scoped.
	MultitrackVideoOutput *multitrackVideo = nullptr;
	OBS::Output::PlatformSession *multitrackSession = nullptr;
	OBS::Output::PlatformSession *compatibilitySession = nullptr;
	// Shared ownership keeps an asynchronous provider setup alive long enough
	// to reject its callback safely if a newer setup supersedes it.
	std::vector<std::shared_ptr<OBS::Output::PlatformSession>> platformSessionRuntimes;
	bool multitrackVideoActive = false;

	OBSOutputAutoRelease StreamingOutput() const
	{
		return (multitrackVideo && multitrackVideoActive)
			       ? multitrackVideo->StreamingOutput()
			       : OBSOutputAutoRelease{obs_output_get_ref(streamOutput)};
	}

	obs_view_t *virtualCamView = nullptr;
	video_t *virtualCamVideo = nullptr;
	obs_scene_t *vCamSourceScene = nullptr;
	obs_sceneitem_t *vCamSourceSceneItem = nullptr;

	std::unique_ptr<WHIPSimulcastEncoders> whipSimulcastEncoders;
	OBS::Output::Runtime outputRoutes;
	OBS::Output::SessionSet platformSessions;

	std::string outputType;
	std::string lastError;

	std::string lastRecordingPath;

	OBSSignal startRecording;
	OBSSignal stopRecording;
	OBSSignal startReplayBuffer;
	OBSSignal stopReplayBuffer;
	OBSSignal startStreaming;
	OBSSignal stopStreaming;
	OBSSignal startVirtualCam;
	OBSSignal stopVirtualCam;
	OBSSignal deactivateVirtualCam;
	OBSSignal streamDelayStarting;
	OBSSignal streamStopping;
	OBSSignal recordStopping;
	OBSSignal recordFileChanged;
	OBSSignal replayBufferStopping;
	OBSSignal replayBufferSaved;

	BasicOutputHandler(OBSBasic *main_);

	virtual ~BasicOutputHandler() {};

	virtual std::shared_future<void> SetupStreaming(obs_service_t *service,
							SetupStreamingContinuation_t continuation) = 0;
	virtual bool StartStreaming(obs_service_t *service) = 0;
	virtual bool StartRecording() = 0;
	virtual bool StartReplayBuffer() { return false; }
	virtual bool StartVirtualCam();
	virtual void StopStreaming(bool force = false) = 0;
	virtual void StopRecording(bool force = false) = 0;
	virtual void StopReplayBuffer(bool force = false) { (void)force; }
	virtual void StopVirtualCam();
	virtual bool StreamingActive() const = 0;
	virtual bool RecordingActive() const = 0;
	virtual bool ReplayBufferActive() const { return false; }
	virtual bool VirtualCamActive() const;

	virtual void Update() = 0;
	virtual void SetupOutputs() = 0;

	bool PrepareOutputRoutes(obs_output_t *referenceOutput);
	bool PrepareOutputRoutes(obs_encoder_t *referenceVideo, obs_encoder_t *referenceAudio);
	size_t StartOutputRoutes();
	size_t StartOutputSession(std::string_view sessionId);
	void StopOutputRoutes(bool force = false);
	void StopOutputSession(std::string_view sessionId, bool force = false);
	std::vector<OBS::Output::SessionSnapshot> OutputSessionSnapshots() const;
	OBS::Output::PlatformSession *FindPlatformSessionForOutput(obs_output_t *output) const;
	void OutputRouteStateChanged(std::string_view sessionId, OBS::Output::RuntimeState state,
				     std::string_view error);
	void UpdateAggregateStreamingState(int code = OBS_OUTPUT_SUCCESS, std::string_view error = {});

	virtual void UpdateVirtualCamOutputSource();
	virtual void DestroyVirtualCamView();
	virtual void DestroyVirtualCameraScene();

	inline bool Active() const
	{
		return streamingActive || recordingActive || delayActive || replayBufferActive || virtualCamActive ||
		       multitrackVideoActive || outputRoutes.Active();
	}

protected:
	bool LoadPlatformSessions(OBS::Output::RouteSet &routes, std::string &error);
	OBS::Output::PlatformSession *PrepareCompatibilitySession(obs_service_t *service);
	OBS::Output::PlatformSession *FindPlatformSession(std::string_view sessionId) const;
	void AttachOutputRouteSessions();
	ReplayProgramBindingResult ConfigureReplayBufferProgram(obs_encoder_t *referenceVideo,
								obs_encoder_t *referenceAudio, std::string &error);
	void SetupAutoRemux(const char *&container);
	std::string GetRecordingFilename(const char *path, const char *container, bool noSpace, bool overwrite,
					 const char *format, bool ffmpeg);

	std::shared_future<void> SetupMultitrackVideo(OBS::Output::PlatformSession &session,
						      std::string audio_encoder_id, size_t main_audio_mixer,
						      std::optional<size_t> vod_track_mixer,
						      std::function<void(std::optional<bool>)> continuation);
	OBSDataAutoRelease GenerateMultitrackVideoStreamDumpConfig();
};

BasicOutputHandler *CreateSimpleOutputHandler(OBSBasic *main);
BasicOutputHandler *CreateAdvancedOutputHandler(OBSBasic *main);

void OBSStreamStarting(void *data, calldata_t *params);
void OBSStreamStopping(void *data, calldata_t *params);
void OBSStartStreaming(void *data, calldata_t *params);
void OBSStopStreaming(void *data, calldata_t *params);
void OBSStartRecording(void *data, calldata_t *params);
void OBSStopRecording(void *data, calldata_t *params);
void OBSRecordStopping(void *data, calldata_t *params);
void OBSRecordFileChanged(void *data, calldata_t *params);
void OBSStartReplayBuffer(void *data, calldata_t *params);
void OBSStopReplayBuffer(void *data, calldata_t *params);
void OBSReplayBufferStopping(void *data, calldata_t *params);
void OBSReplayBufferSaved(void *data, calldata_t *params);

inline bool can_use_output(const char *prot, const char *output, const char *prot_test1,
			   const char *prot_test2 = nullptr)
{
	return (strcmp(prot, prot_test1) == 0 || (prot_test2 && strcmp(prot, prot_test2) == 0)) &&
	       (obs_get_output_flags(output) & OBS_OUTPUT_SERVICE) != 0;
}

const char *GetStreamOutputType(const obs_service_t *service);

inline bool ServiceSupportsVodTrack(const char *service)
{
	static const char *vodTrackServices[] = {"Twitch"};

	for (const char *vodTrackService : vodTrackServices) {
		if (astrcmpi(vodTrackService, service) == 0) {
			return true;
		}
	}

	return false;
}

void clear_archive_encoder(obs_output_t *output, const char *expected_name);
