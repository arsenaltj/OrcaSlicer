#include "orca/sdk_c.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Mobile SDK C ABI exports stable C-callable entry points", "[MobileSDK][CABI]")
{
    using create_fn = orca_sdk_error_t (*)(const orca_sdk_options_t*, orca_sdk_t**);
    using destroy_fn = orca_sdk_error_t (*)(orca_sdk_t*);
    using poll_fn = orca_sdk_error_t (*)(orca_sdk_session_t*, uint32_t, orca_sdk_event_t*);
    using wake_fn = orca_sdk_error_t (*)(orca_sdk_session_t*);

    create_fn create = &orca_sdk_create;
    destroy_fn destroy = &orca_sdk_destroy;
    poll_fn poll = &orca_sdk_poll_event;
    wake_fn wake = &orca_sdk_wakeup;

    REQUIRE(create != nullptr);
    REQUIRE(destroy != nullptr);
    REQUIRE(poll != nullptr);
    REQUIRE(wake != nullptr);
    REQUIRE(orca_sdk_error_message(ORCA_SDK_E_CANCELED) != nullptr);
}

TEST_CASE("Mobile SDK C ABI keeps error values fixed width", "[MobileSDK][CABI]")
{
    static_assert(sizeof(orca_sdk_error_t) == 4, "C ABI errors must remain int32");
    REQUIRE(ORCA_SDK_OK == 0);
    REQUIRE(ORCA_SDK_E_INVALID_ARGUMENT == -1);
    REQUIRE(ORCA_SDK_E_INTERNAL == -12);
}
