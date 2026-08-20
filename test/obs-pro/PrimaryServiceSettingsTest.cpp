#include <obs-data.h>

#include <cstring>

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			return __LINE__; \
	} while (false)

int main()
{
	const char *youtubeSettings = "{\"service\":\"YouTube - RTMPS\",\"protocol\":\"RTMPS\","
				      "\"server\":\"rtmps://youtube.example/live\",\"key\":\"secret\"}";
	obs_data_t *saved = obs_data_create_from_json(youtubeSettings);
	CHECK(saved != nullptr);

	const char *serialized = obs_data_get_json(saved);
	CHECK(serialized != nullptr && std::strstr(serialized, "YouTube - RTMPS") != nullptr);
	obs_data_t *reloaded = obs_data_create_from_json(serialized);
	CHECK(reloaded != nullptr);
	CHECK(std::strcmp(obs_data_get_string(reloaded, "service"), "YouTube - RTMPS") == 0);
	CHECK(std::strcmp(obs_data_get_string(reloaded, "protocol"), "RTMPS") == 0);
	CHECK(std::strcmp(obs_data_get_string(reloaded, "server"), "rtmps://youtube.example/live") == 0);
	CHECK(std::strcmp(obs_data_get_string(reloaded, "key"), "secret") == 0);

	obs_data_release(reloaded);
	obs_data_release(saved);
	return 0;
}
