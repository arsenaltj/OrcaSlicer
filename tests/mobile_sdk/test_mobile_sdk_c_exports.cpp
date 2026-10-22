#include "orca/mobile/device_sdk_c.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Mobile SDK C ABI exports stable C-callable entry points", "[MobileDeviceSDK][CABI]")
{
    using create_fn = orca_mobile_device_error_t (*)(const orca_mobile_device_options_t*, orca_mobile_device_t**);
    using destroy_fn = orca_mobile_device_error_t (*)(orca_mobile_device_t*);
    using poll_fn = orca_mobile_device_error_t (*)(orca_mobile_device_session_t*, uint32_t, orca_mobile_device_event_t*);
    using wake_fn = orca_mobile_device_error_t (*)(orca_mobile_device_session_t*);

    create_fn create = &orca_mobile_device_create;
    destroy_fn destroy = &orca_mobile_device_destroy;
    poll_fn poll = &orca_mobile_device_poll_event;
    wake_fn wake = &orca_mobile_device_wakeup;

    REQUIRE(create != nullptr);
    REQUIRE(destroy != nullptr);
    REQUIRE(poll != nullptr);
    REQUIRE(wake != nullptr);
    REQUIRE(orca_mobile_device_error_message(ORCA_MOBILE_DEVICE_E_CANCELED) != nullptr);
}

TEST_CASE("Mobile SDK C ABI keeps error values fixed width", "[MobileDeviceSDK][CABI]")
{
    static_assert(sizeof(orca_mobile_device_error_t) == 4, "C ABI errors must remain int32");
    REQUIRE(ORCA_MOBILE_DEVICE_OK == 0);
    REQUIRE(ORCA_MOBILE_DEVICE_E_INVALID_ARGUMENT == -1);
    REQUIRE(ORCA_MOBILE_DEVICE_E_INTERNAL == -12);
}
