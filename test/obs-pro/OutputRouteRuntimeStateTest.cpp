#include <utility/OutputRouteRuntime.hpp>

#define CHECK(condition) \
	do { \
		if (!(condition)) \
			return __LINE__; \
	} while (false)

int main()
{
	using OBS::Output::IsRuntimeActive;
	using OBS::Output::RuntimeState;

	CHECK(!IsRuntimeActive(RuntimeState::Idle, false));
	CHECK(IsRuntimeActive(RuntimeState::Starting, false));
	CHECK(!IsRuntimeActive(RuntimeState::Active, false));
	CHECK(IsRuntimeActive(RuntimeState::Active, true));
	CHECK(IsRuntimeActive(RuntimeState::Stopping, true));
	CHECK(!IsRuntimeActive(RuntimeState::Stopping, false));
	CHECK(IsRuntimeActive(RuntimeState::Failed, true));
	CHECK(!IsRuntimeActive(RuntimeState::Failed, false));
	return 0;
}
