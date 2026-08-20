#pragma once

#include "MultitrackConfigProvider.hpp"

#include <obs.hpp>
#include <util/config-file.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class QString;
class QWidget;

void StreamStartHandler(void *arg, calldata_t *);
void StreamStopHandler(void *arg, calldata_t *data);

void RecordingStartHandler(void *arg, calldata_t *data);
void RecordingStopHandler(void *arg, calldata_t *);

struct MultitrackVideoOutput {
public:
	~MultitrackVideoOutput();

	void PrepareStreaming(QWidget *parent, const char *service_name, obs_service_t *service,
			      const std::optional<std::string> &rtmp_url, const QString &stream_key,
			      const char *audio_encoder_id, std::optional<uint32_t> maximum_aggregate_bitrate,
			      std::optional<uint32_t> maximum_video_tracks,
			      OBS::Output::MultitrackConfigProvider config_provider,
			      obs_data_t *dump_stream_to_file_config, size_t main_audio_mixer,
			      std::optional<size_t> vod_track_mixer, std::optional<bool> use_rtmps,
			      const std::vector<std::string> &canvas_uuids);
	signal_handler_t *StreamingSignalHandler();
	void StartedStreaming();
	void StopStreaming();

	OBSOutputAutoRelease StreamingOutput()
	{
		const std::lock_guard current_lock{current_mutex};
		return current ? obs_output_get_ref(current->output_) : nullptr;
	}

	bool RestartOnError() { return restart_on_error; }

private:
	struct OBSOutputObjects {
		OBSOutputAutoRelease output_;
		std::shared_ptr<obs_encoder_group_t> video_encoder_group_;
		std::vector<OBSEncoderAutoRelease> audio_encoders_;
		OBSServiceAutoRelease multitrack_video_service_;
		OBSSignal start_signal, stop_signal;
		std::vector<OBSCanvasAutoRelease> canvases;
	};

	std::optional<OBSOutputObjects> take_current();
	std::optional<OBSOutputObjects> take_current_stream_dump();

	/* Stop callbacks are dispatched to every listener in order. Defer taking
	 * the current objects until that dispatch has completed so destroying the
	 * internal stop signal cannot skip a frontend stop callback. */
	static void ReleaseOnMainThread(MultitrackVideoOutput *self, std::weak_ptr<int> lifetime_token,
					bool stream_dump);

	std::mutex current_mutex;
	std::optional<OBSOutputObjects> current;

	std::mutex current_stream_dump_mutex;
	std::optional<OBSOutputObjects> current_stream_dump;
	std::shared_ptr<int> lifetime_token = std::make_shared<int>(0);

	bool restart_on_error = false;
	uint8_t reconnect_attempts = 0;

	friend void StreamStartHandler(void *arg, calldata_t *data);
	friend void StreamStopHandler(void *arg, calldata_t *data);
	friend void RecordingStartHandler(void *arg, calldata_t *data);
	friend void RecordingStopHandler(void *arg, calldata_t *);
};
