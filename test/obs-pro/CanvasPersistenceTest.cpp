#include <obs.h>

#include <cstring>

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			obs_shutdown(); \
			return __LINE__; \
		} \
	} while (false)

int main()
{
	if (!obs_startup("en-US", nullptr, nullptr)) {
		return 1;
	}

	obs_video_info info{};
	info.fps_num = 60000;
	info.fps_den = 1001;
	info.base_width = 1920;
	info.base_height = 1080;
	info.output_width = 1280;
	info.output_height = 720;
	info.output_format = VIDEO_FORMAT_NV12;
	info.adapter = 2;
	info.gpu_conversion = true;
	info.colorspace = VIDEO_CS_709;
	info.range = VIDEO_RANGE_FULL;
	info.scale_type = OBS_SCALE_LANCZOS;

	obs_canvas_t *canvas = obs_canvas_create("Canvas Persistence", &info, 0);
	CHECK(canvas != nullptr);
	obs_data_t *saved = obs_save_canvas(canvas);
	CHECK(saved != nullptr);
	obs_data_t *savedVideo = obs_data_get_obj(saved, "video_info");
	CHECK(savedVideo != nullptr);
	CHECK(obs_data_get_int(savedVideo, "fps_num") == 60000);
	CHECK(obs_data_get_int(savedVideo, "output_width") == 1280);
	CHECK(obs_data_get_int(savedVideo, "adapter") == 2);
	CHECK(obs_data_get_bool(savedVideo, "gpu_conversion"));
	CHECK(obs_data_get_int(savedVideo, "range") == VIDEO_RANGE_FULL);
	obs_data_release(savedVideo);

	obs_canvas_t *loaded = obs_load_canvas(saved);
	CHECK(loaded != nullptr);
	obs_data_t *reloaded = obs_save_canvas(loaded);
	CHECK(reloaded != nullptr);
	obs_data_t *reloadedVideo = obs_data_get_obj(reloaded, "video_info");
	CHECK(reloadedVideo != nullptr);
	CHECK(obs_data_get_int(reloadedVideo, "fps_den") == 1001);
	CHECK(obs_data_get_int(reloadedVideo, "base_height") == 1080);
	CHECK(obs_data_get_int(reloadedVideo, "scale_type") == OBS_SCALE_LANCZOS);
	obs_data_release(reloadedVideo);

	obs_data_t *legacy = obs_data_create_from_json(
		"{\"name\":\"Legacy Canvas\",\"uuid\":\"legacy-canvas\",\"private\":false,\"flags\":0}");
	CHECK(legacy != nullptr);
	obs_canvas_t *legacyCanvas = obs_load_canvas(legacy);
	CHECK(legacyCanvas != nullptr);
	obs_data_t *legacySaved = obs_save_canvas(legacyCanvas);
	CHECK(legacySaved != nullptr);
	obs_data_t *legacyVideo = obs_data_get_obj(legacySaved, "video_info");
	CHECK(legacyVideo == nullptr);

	obs_data_release(legacySaved);
	obs_canvas_release(legacyCanvas);
	obs_data_release(legacy);
	obs_data_release(reloaded);
	obs_canvas_release(loaded);
	obs_data_release(saved);
	obs_canvas_release(canvas);
	obs_shutdown();
	return 0;
}
