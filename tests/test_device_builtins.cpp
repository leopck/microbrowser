#include "device_test_fixture.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <limits>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; ++failures; } } while (false)
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error(std::string(#condition) + " at " + std::to_string(__LINE__)); } while (false)

using device_test::FunctionSpec;
using device_test::PackageSpec;
using device_test::gpio_package;

struct Harness {
    explicit Harness(const PackageSpec& package)
        : browser(fixture.platform, {64, 64}, "http://127.0.0.1:8765", &fixture.registry) {
        browser.load(package.encode());
    }

    device_test::GpioFixture fixture;
    mb::Browser browser;
};

void complete(Harness& harness) {
    harness.fixture.model->complete_next();
    harness.browser.tick();
}

float exported_number(Harness& harness, std::string_view exporter, std::string_view key) {
    harness.browser.dispatch(exporter);
    const auto found = harness.fixture.platform.values.find(std::string(key));
    REQUIRE(found != harness.fixture.platform.values.end());
    REQUIRE(found->second.type == mb::Value::Type::number);
    return found->second.number;
}

bool exported_bool(Harness& harness, std::string_view exporter, std::string_view key) {
    harness.browser.dispatch(exporter);
    const auto found = harness.fixture.platform.values.find(std::string(key));
    REQUIRE(found != harness.fixture.platform.values.end());
    REQUIRE(found->second.type == mb::Value::Type::boolean);
    return found->second.boolean;
}

void add_exports(PackageSpec& package) {
    add_export_int(package, "export_count", "callback_count", "count");
    add_export_int(package, "export_status", "status", "status");
    add_export_int(package, "export_observed", "observed_int", "observed");
    add_export_bool(package, "export_flag", "observed_bool", "flag");
    add_export_bool(package, "export_native", "native_bool", "native");
}

void add_open(PackageSpec& package, std::string function_name = "open_gpio",
              std::string endpoint = "gpio", std::int64_t minimum_minor = 0) {
    if (std::find_if(package.functions.begin(), package.functions.end(), [](const FunctionSpec& function) {
            return function.name == "on_open";
        }) == package.functions.end()) {
        add_callback(package, "on_open", "device");
    }
    FunctionSpec& open = package.function(std::move(function_name));
    open.push(mb::Value::from_integer(static_cast<std::int64_t>(mb::Capability::gpio)))
        .push(mb::Value::from_string(std::move(endpoint)))
        .push(mb::Value::from_integer(minimum_minor))
        .push(mb::Value::from_string("on_open"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::open), 4)
        .pop()
        .finish();
}

void add_capability_probe(PackageSpec& package, std::string function_name,
                          mb::Capability capability, std::string endpoint) {
    FunctionSpec& check = package.function(std::move(function_name));
    check.push(mb::Value::from_integer(static_cast<std::int64_t>(capability)))
        .push(mb::Value::from_string(std::move(endpoint)))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::capability_supported), 2)
        .store_state(package.state("native_bool"))
        .finish();
}

void add_configure(PackageSpec& package, bool output) {
    if (std::find_if(package.functions.begin(), package.functions.end(), [](const FunctionSpec& function) {
            return function.name == "on_empty";
        }) == package.functions.end()) {
        add_callback(package, "on_empty");
    }
    FunctionSpec& configure = package.function(output ? "configure_output" : "configure_input");
    configure.load_state(package.state("device"))
        .push(mb::Value::from_integer(output ? 1 : 0))
        .push(mb::Value::from_integer(0))
        .push_bool(false)
        .push(mb::Value::from_string("on_empty"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::configure), 5)
        .pop()
        .finish();
}

void add_write(PackageSpec& package) {
    add_callback(package, "on_empty");
    FunctionSpec& write = package.function("write_high");
    write.load_state(package.state("device"))
        .push_bool(true)
        .push(mb::Value::from_string("on_empty"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::write), 3)
        .pop()
        .finish();
}

void add_read(PackageSpec& package, bool metadata = true) {
    FunctionSpec& callback = package.function("on_read", 3, 3);
    callback.load_local(1).store_state(package.state("status"));
    callback.load_local(0).store_state(package.state("operation"));
    callback.load_local(2).store_state(package.state("observed_bool"));
    increment_callbacks(package, callback);
    if (metadata) {
        callback.load_local(0)
            .push(mb::Value::from_integer(
                static_cast<std::int64_t>(mb::DeviceMetadataField::timestamp_ns)))
            .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::metadata), 2)
            .store_state(package.state("observed_int"));
    }
    callback.finish();
    FunctionSpec& read = package.function("read_gpio");
    read.load_state(package.state("device"))
        .push(mb::Value::from_string("on_read"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::read), 2)
        .pop()
        .finish();
}

void add_subscription(PackageSpec& package) {
    add_callback(package, "on_ready", "subscription");
    add_callback(package, "on_notification", "observed_bool");
    add_callback(package, "on_empty");
    FunctionSpec& subscribe = package.function("subscribe_gpio");
    subscribe.load_state(package.state("device"))
        .push(mb::Value::from_integer(static_cast<std::int64_t>(mb::GpioEdge::both)))
        .push(mb::Value::from_string("on_ready"))
        .push(mb::Value::from_string("on_notification"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::subscribe), 4)
        .pop()
        .finish();
    FunctionSpec& unsubscribe = package.function("unsubscribe_gpio");
    unsubscribe.load_state(package.state("device"))
        .load_state(package.state("subscription"))
        .push(mb::Value::from_string("on_empty"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::unsubscribe), 3)
        .pop()
        .finish();
}

void test_permissions_and_versions() {
    PackageSpec package = gpio_package();
    add_open(package);
    add_open(package, "open_denied", "other_gpio");
    add_open(package, "open_old_minor", "gpio", 1);
    for (const auto [name, endpoint] : {std::pair{"check_allowed", "gpio"},
                                        std::pair{"check_denied", "other_gpio"}}) {
        add_capability_probe(package, name, mb::Capability::gpio, endpoint);
    }
    add_exports(package);
    Harness harness(package);
    harness.browser.dispatch("check_allowed");
    CHECK(exported_bool(harness, "export_native", "native"));
    harness.browser.dispatch("check_denied");
    CHECK(!exported_bool(harness, "export_native", "native"));
    CHECK(harness.fixture.model->pending_count() == 0);
    harness.browser.dispatch("open_denied");
    CHECK(std::string(harness.browser.last_error()).find("IoStatus 2") != std::string::npos);
    CHECK(harness.fixture.model->pending_count() == 0);
    harness.browser.dispatch("open_old_minor");
    CHECK(std::string(harness.browser.last_error()).find("IoStatus 3") != std::string::npos);
    CHECK(harness.fixture.model->pending_count() == 0);
    harness.browser.dispatch("open_gpio");
    REQUIRE(harness.fixture.model->pending_count() == 1);
    const auto& request = std::get<mb::OpenRequest>(harness.fixture.model->pending_operation().request);
    CHECK(request.minimum_version.capability == mb::Capability::gpio);
    CHECK(request.minimum_version.major == mb::peripheral_abi_major);
    CHECK(request.minimum_version.minor == 0);
    complete(harness);
    CHECK(exported_number(harness, "export_count", "count") == 1.0F);
}

void expect_required_rejection(mb::Browser& browser, const PackageSpec& package) {
    bool rejected = false;
    try {
        browser.load(package.encode());
    } catch (const mb::PackageError& error) {
        rejected = std::string_view(error.what()).find("required") != std::string_view::npos;
    }
    CHECK(rejected);
    CHECK(browser.metrics().scripts == 0);
}

void test_required_capability_preflight_rejects_before_on_load() {
    PackageSpec below_minimum = gpio_package();
    below_minimum.capabilities[0].version.minor = 1;
    add_open(below_minimum);
    below_minimum.first_page_on_load = "open_gpio";
    device_test::GpioFixture fixture;
    mb::Browser browser(fixture.platform, {64, 64}, "http://127.0.0.1:8765", &fixture.registry);
    expect_required_rejection(browser, below_minimum);
    CHECK(fixture.model->pending_count() == 0);
    CHECK(fixture.model->release_count() == 0);

    below_minimum.capabilities[0].version.minor = 0;
    below_minimum.first_page_on_load.clear();
    browser.load(below_minimum.encode());
    browser.dispatch("open_gpio");
    CHECK(fixture.model->pending_count() == 1);

    PackageSpec no_registry_package = gpio_package();
    add_open(no_registry_package);
    no_registry_package.first_page_on_load = "open_gpio";
    mb::Browser no_registry(fixture.platform, {64, 64}, "http://127.0.0.1:8765", nullptr);
    expect_required_rejection(no_registry, no_registry_package);
    CHECK(fixture.model->pending_count() == 1);

    PackageSpec owner_only_package = gpio_package();
    owner_only_package.permissions = {"owner_gpio"};
    add_open(owner_only_package);
    owner_only_package.first_page_on_load = "open_gpio";
    device_test::GpioFixture owner_fixture;
    mb::Browser owner_only(owner_fixture.platform, {64, 64}, "http://127.0.0.1:8765",
                           &owner_fixture.registry);
    expect_required_rejection(owner_only, owner_only_package);
    CHECK(owner_fixture.model->pending_count() == 0);
    CHECK(owner_fixture.model->release_count() == 0);
}

void test_optional_capability_probes() {
    PackageSpec optional = gpio_package();
    optional.capabilities.clear();
    optional.capabilities.push_back({{mb::Capability::adc, mb::peripheral_abi_major, 0}, false});
    optional.capabilities.push_back(
        {{mb::Capability::usb_device, mb::peripheral_abi_major, 0}, false});
    optional.permissions = {"adc", "usb_device"};
    add_capability_probe(optional, "check_unavailable", mb::Capability::adc, "adc");
    add_capability_probe(optional, "check_available", mb::Capability::usb_device, "usb_device");
    add_exports(optional);
    Harness harness(optional);
    harness.browser.dispatch("check_unavailable");
    CHECK(!exported_bool(harness, "export_native", "native"));
    harness.browser.dispatch("check_available");
    CHECK(exported_bool(harness, "export_native", "native"));
    CHECK(harness.fixture.model->pending_count() == 0);

    PackageSpec below_minimum = gpio_package();
    below_minimum.capabilities[0].required = false;
    below_minimum.capabilities[0].version.minor = 1;
    below_minimum.permissions.push_back("usb_device");
    add_capability_probe(below_minimum, "check_old_minor", mb::Capability::gpio, "gpio");
    add_capability_probe(below_minimum, "check_undeclared", mb::Capability::usb_device, "usb_device");
    add_exports(below_minimum);
    Harness old(below_minimum);
    old.browser.dispatch("check_old_minor");
    CHECK(!exported_bool(old, "export_native", "native"));
    old.browser.dispatch("check_undeclared");
    CHECK(!exported_bool(old, "export_native", "native"));
    CHECK(old.fixture.model->pending_count() == 0);
}

void test_legacy_v2_without_registry() {
    PackageSpec legacy;
    legacy.format_major = 2;
    FunctionSpec& start = legacy.function("start");
    start.push(mb::Value::from_string("ready"))
        .push(mb::Value::from_number(1.0F))
        .builtin(13, 2)
        .pop()
        .finish();
    legacy.first_page_on_load = "start";
    device_test::GpioFixture fixture;
    mb::Browser browser(fixture.platform, {64, 64}, "http://127.0.0.1:8765", nullptr);
    browser.load(legacy.encode());
    const auto ready = fixture.platform.values.find("ready");
    REQUIRE(ready != fixture.platform.values.end());
    REQUIRE(ready->second.type == mb::Value::Type::number);
    CHECK(ready->second.number == 1.0F);
    CHECK(browser.metrics().scripts == 1);
    (void)browser.tick();
    CHECK(browser.last_error().empty());
    CHECK(fixture.model->pending_count() == 0);
}

void test_gpio_output_and_input_callbacks() {
    PackageSpec output = gpio_package();
    add_open(output);
    add_configure(output, true);
    add_write(output);
    add_exports(output);
    Harness out(output);
    out.browser.dispatch("open_gpio");
    complete(out);
    out.browser.dispatch("configure_output");
    REQUIRE(out.fixture.model->pending_count() == 1);
    const auto& configuration =
        std::get<mb::GpioConfigureRequest>(out.fixture.model->pending_operation().request);
    CHECK(configuration.direction == mb::GpioDirection::output);
    CHECK(configuration.pull == mb::GpioPull::none);
    CHECK(!configuration.initial_high);
    complete(out);
    out.browser.dispatch("write_high");
    CHECK(std::get<mb::GpioWriteRequest>(out.fixture.model->pending_operation().request).high);
    CHECK(!out.fixture.model->high);
    complete(out);
    CHECK(out.fixture.model->high);
    CHECK(exported_number(out, "export_count", "count") == 3.0F);
    CHECK(exported_number(out, "export_status", "status") == 0.0F);

    PackageSpec input = gpio_package();
    add_open(input);
    add_configure(input, false);
    add_read(input);
    add_exports(input);
    Harness in(input);
    in.browser.dispatch("open_gpio");
    complete(in);
    in.browser.dispatch("configure_input");
    complete(in);
    in.fixture.model->high = true;
    in.browser.dispatch("read_gpio");
    complete(in);
    CHECK(exported_bool(in, "export_flag", "flag"));
    CHECK(exported_number(in, "export_observed", "observed") == 100.0F);
    CHECK(exported_number(in, "export_status", "status") == 0.0F);
    CHECK(exported_number(in, "export_count", "count") == 3.0F);
}

void test_callback_type_and_completeness_failures() {
    PackageSpec mismatch = gpio_package();
    add_open(mismatch);
    FunctionSpec& bad = mismatch.function("on_bad_read", 3, 3);
    bad.load_local(1).store_state(mismatch.state("status"));
    increment_callbacks(mismatch, bad);
    bad.load_local(2).store_state(mismatch.state("observed_int")).finish();
    FunctionSpec& read_bad = mismatch.function("read_bad");
    read_bad.load_state(mismatch.state("device"))
        .push(mb::Value::from_string("on_bad_read"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::read), 2)
        .pop()
        .finish();
    add_exports(mismatch);
    Harness wrong_value(mismatch);
    wrong_value.browser.dispatch("open_gpio");
    complete(wrong_value);
    wrong_value.fixture.model->high = true;
    wrong_value.browser.dispatch("read_bad");
    complete(wrong_value);
    CHECK(!wrong_value.browser.last_error().empty());
    CHECK(exported_number(wrong_value, "export_count", "count") == 2.0F);

    PackageSpec incomplete = gpio_package();
    add_open(incomplete);
    add_callback(incomplete, "on_incomplete");
    FunctionSpec& read_incomplete = incomplete.function("read_incomplete");
    read_incomplete.load_state(incomplete.state("device"))
        .push(mb::Value::from_string("on_incomplete"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::read), 2)
        .pop()
        .finish();
    add_exports(incomplete);
    Harness rejected(incomplete);
    rejected.browser.dispatch("open_gpio");
    complete(rejected);
    rejected.browser.dispatch("read_incomplete");
    rejected.fixture.model->complete_next_with(mb::IoStatus::completed, mb::EmptyResponse{});
    rejected.browser.tick();
    CHECK(exported_number(rejected, "export_status", "status") ==
          static_cast<float>(mb::IoStatus::io_failure));
    CHECK(exported_number(rejected, "export_count", "count") == 2.0F);
    CHECK(rejected.browser.last_error().empty());
}


void test_pending_budget() {
    PackageSpec package = gpio_package();
    add_open(package);
    add_write(package);
    add_exports(package);
    Harness harness(package);
    harness.browser.dispatch("open_gpio");
    complete(harness);
    for (unsigned index = 0; index < 8; ++index) harness.browser.dispatch("write_high");
    CHECK(harness.fixture.model->pending_count() == 8);
    harness.browser.dispatch("write_high");
    CHECK(std::string(harness.browser.last_error()).find("maximum pending") != std::string::npos);
    CHECK(harness.fixture.model->pending_count() == 8);
    CHECK(exported_number(harness, "export_count", "count") == 1.0F);
}

void test_subscription_delivery() {
    PackageSpec package = gpio_package();
    add_open(package);
    add_subscription(package);
    add_exports(package);
    Harness harness(package);
    harness.browser.dispatch("open_gpio");
    complete(harness);
    harness.browser.dispatch("subscribe_gpio");
    CHECK(std::holds_alternative<mb::GpioSubscribeRequest>(harness.fixture.model->pending_operation().request));
    complete(harness);
    CHECK(exported_number(harness, "export_count", "count") == 2.0F);
    CHECK(harness.fixture.model->publish(true) == mb::IoStatus::completed);
    harness.browser.tick();
    CHECK(exported_bool(harness, "export_flag", "flag"));
    CHECK(exported_number(harness, "export_count", "count") == 3.0F);
    harness.browser.dispatch("unsubscribe_gpio");
    complete(harness);
    CHECK(exported_number(harness, "export_count", "count") == 4.0F);
    CHECK(harness.fixture.model->publish(false) == mb::IoStatus::invalid_argument);
    harness.browser.tick();
    CHECK(exported_bool(harness, "export_flag", "flag"));
    CHECK(exported_number(harness, "export_count", "count") == 4.0F);
}

PackageSpec usb_package() {
    PackageSpec package = gpio_package();
    package.capabilities.clear();
    package.capabilities.push_back({{mb::Capability::usb_device, mb::peripheral_abi_major, 0}, true});
    package.permissions = {"usb_device"};
    add_callback(package, "on_open", "device");
    FunctionSpec& open = package.function("open_usb");
    open.push(mb::Value::from_integer(static_cast<std::int64_t>(mb::Capability::usb_device)))
        .push(mb::Value::from_string("usb_device"))
        .push(mb::Value::from_integer(0))
        .push(mb::Value::from_string("on_open"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::open), 4)
        .pop()
        .finish();
    add_callback(package, "on_usb_result");
    add_exports(package);
    return package;
}

void add_usb_configure(PackageSpec& package, std::string name, mb::ByteBuffer table) {
    FunctionSpec& configure = package.function(std::move(name));
    configure.load_state(package.state("device"))
        .push(mb::Value::from_bytes(mb::ByteBuffer{0x12}))
        .push(mb::Value::from_bytes(std::move(table)))
        .push(mb::Value::from_string("on_usb_result"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::configure), 4)
        .pop()
        .finish();
}

void add_usb_transfer(PackageSpec& package, std::string name, mb::ByteBuffer setup,
                      mb::ByteBuffer transmit, std::int64_t receive_bytes) {
    FunctionSpec& transfer = package.function(std::move(name));
    transfer.load_state(package.state("device"))
        .push(mb::Value::from_bytes(std::move(setup)))
        .push(mb::Value::from_bytes(std::move(transmit)))
        .push(mb::Value::from_integer(receive_bytes))
        .push(mb::Value::from_integer(50))
        .push(mb::Value::from_string("on_usb_result"))
        .device(static_cast<std::uint16_t>(mb::DeviceOperation::transfer), 6)
        .pop()
        .finish();
}

void test_usb_binary_records() {
    PackageSpec package = usb_package();
    const mb::ByteBuffer valid_table{1, 0, 7, 2, 3, 4, 3, 0, 0xaa, 0xbb, 0xcc};
    add_usb_configure(package, "configure_valid", valid_table);
    add_usb_configure(package, "configure_truncated", {1, 0, 7, 2});
    add_usb_configure(package, "configure_trailing", {0, 0, 42});
    add_usb_configure(package, "configure_too_many", {9, 0});
    add_usb_configure(package, "configure_duplicate",
                      {2, 0, 1, 2, 3, 4, 0, 0, 1, 5, 6, 7, 0, 0});
    add_usb_configure(package, "configure_budget", {1, 0, 1, 2, 3, 4, 1, 16});
    add_usb_configure(package, "configure_empty_table", {});
    add_usb_transfer(package, "transfer_direction_error", {0x80, 6, 2, 0, 0, 0, 2, 0}, {1}, 2);
    add_usb_transfer(package, "transfer_short_setup", {0x80, 6, 2, 0, 0, 0, 2}, {}, 2);
    add_usb_transfer(package, "transfer_valid", {0x80, 6, 2, 0, 0, 0, 2, 0}, {}, 2);

    FunctionSpec& make_setup = package.function("make_usb_setup");
    const std::uint16_t usb_setup_operation = static_cast<std::uint16_t>(mb::DeviceNativeOperation::usb_setup);
    make_setup.push(mb::Value::from_integer(0x80)).push(mb::Value::from_integer(6))
        .push(mb::Value::from_integer(0x1234)).push(mb::Value::from_integer(0xabcd))
        .push(mb::Value::from_integer(3)).device(usb_setup_operation, 5)
        .store_state(package.state("observed_bytes"));
    make_setup.load_state(package.state("observed_bytes"))
        .push(mb::Value::from_bytes(mb::ByteBuffer{0x80, 6, 0x34, 0x12, 0xcd, 0xab, 3, 0}))
        .equal()
        .store_state(package.state("native_bool"));
    make_setup.load_state(package.state("observed_bytes"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .store_state(package.state("observed_int"))
        .finish();

    Harness harness(package);
    harness.browser.dispatch("make_usb_setup");
    CHECK(exported_bool(harness, "export_native", "native"));
    CHECK(exported_number(harness, "export_observed", "observed") == 8.0F);
    harness.browser.dispatch("open_usb");
    harness.fixture.usb->complete_next();
    harness.browser.tick();
    for (const auto [function, message] : {
             std::pair<std::string_view, std::string_view>{"configure_truncated", "truncated"},
             {"configure_trailing", "trailing data"},
             {"configure_too_many", "at most eight"},
             {"configure_duplicate", "duplicated"},
             {"configure_budget", "maximum_bytes_per_buffer"},
             {"configure_empty_table", "truncated"},
             {"transfer_direction_error", "device-to-host"},
             {"transfer_short_setup", "exactly eight"}}) {
        harness.browser.dispatch(function);
        CHECK(std::string(harness.browser.last_error()).find(message) != std::string::npos);
        CHECK(harness.fixture.usb->pending_count() == 0);
    }
    harness.browser.dispatch("configure_valid");
    REQUIRE(harness.fixture.usb->pending_count() == 1);
    const auto& configuration =
        std::get<mb::UsbDeviceConfigureRequest>(harness.fixture.usb->pending_operation().request);
    REQUIRE(configuration.interfaces.size() == 1);
    CHECK(configuration.interfaces[0].interface_number == 7);
    CHECK(configuration.interfaces[0].interface_class == 2);
    CHECK(configuration.interfaces[0].interface_subclass == 3);
    CHECK(configuration.interfaces[0].interface_protocol == 4);
    REQUIRE(configuration.interfaces[0].descriptors != nullptr);
    CHECK(*configuration.interfaces[0].descriptors == mb::ByteBuffer({0xaa, 0xbb, 0xcc}));
    harness.fixture.usb->complete_next();
    harness.browser.tick();
    harness.browser.dispatch("transfer_valid");
    REQUIRE(harness.fixture.usb->pending_count() == 1);
    const auto& transfer = std::get<mb::UsbTransferRequest>(harness.fixture.usb->pending_operation().request);
    CHECK(transfer.setup.request_type == 0x80);
    CHECK(transfer.setup.request == 6);
    CHECK(transfer.setup.value == 2);
    CHECK(transfer.setup.index == 0);
    CHECK(transfer.receive_bytes == 2);
    CHECK(transfer.timeout_ms == 50);
    CHECK(transfer.bytes != nullptr && transfer.bytes->empty());
}

void test_metadata_outside_callback() {
    PackageSpec package = gpio_package();
    FunctionSpec& invalid = package.function("invalid_metadata");
    invalid.load_state(package.state("device"))
        .push(mb::Value::from_integer(static_cast<std::int64_t>(mb::DeviceMetadataField::timestamp_ns)))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::metadata), 2)
        .pop()
        .finish();
    Harness harness(package);
    harness.browser.dispatch("invalid_metadata");
    CHECK(std::string(harness.browser.last_error()).find("only valid inside") != std::string::npos);
}

void test_navigation_rejects_stale_completions() {
    PackageSpec original = gpio_package();
    add_open(original);
    add_read(original, false);
    add_subscription(original);
    add_exports(original);
    Harness harness(original);
    harness.browser.dispatch("open_gpio");
    complete(harness);
    harness.browser.dispatch("subscribe_gpio");
    complete(harness);
    harness.browser.dispatch("read_gpio");
    REQUIRE(harness.fixture.model->pending_count() == 1);

    PackageSpec replacement = gpio_package();
    add_open(replacement);
    add_exports(replacement);
    harness.browser.load(replacement.encode());
    CHECK(harness.fixture.model->publish(false) == mb::IoStatus::device_lost);
    CHECK(harness.fixture.model->release_count() == 1);
    harness.browser.dispatch("open_gpio");
    REQUIRE(harness.fixture.model->pending_count() == 2);
    harness.fixture.model->complete_at(0);
    harness.browser.tick();
    CHECK(exported_number(harness, "export_count", "count") == 0.0F);
    complete(harness);
    CHECK(exported_number(harness, "export_count", "count") == 1.0F);
}

void add_hex_conversion(PackageSpec& package) {
    FunctionSpec& conversion = package.function("convert_hex");
    conversion.push(mb::Value::from_string("00ff10"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes), 1)
        .store_state(package.state("observed_bytes"));
    conversion.load_state(package.state("observed_bytes"))
        .push(mb::Value::from_integer(1))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::byte_at), 2)
        .push(mb::Value::from_integer(255))
        .equal()
        .store_state(package.state("native_bool"));
    conversion.load_state(package.state("observed_bytes"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .store_state(package.state("observed_int"))
        .finish();
}

void add_one_byte_conversion(PackageSpec& package) {
    FunctionSpec& conversion = package.function("convert_one_byte");
    conversion.push(mb::Value::from_integer(255))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::one_byte), 1)
        .store_state(package.state("observed_bytes"));
    conversion.load_state(package.state("observed_bytes"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .store_state(package.state("observed_int"));
    conversion.load_state(package.state("observed_bytes"))
        .push(mb::Value::from_integer(0))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::byte_at), 2)
        .store_state(package.state("status"))
        .finish();
}

void add_named_failure(PackageSpec& package, std::string name, std::uint16_t operation,
                       std::vector<mb::Value> arguments) {
    FunctionSpec& function = package.function(std::move(name));
    for (auto& argument : arguments) function.push(std::move(argument));
    function.device(operation, static_cast<std::uint8_t>(arguments.size())).pop().finish();
}

void test_native_conversions_and_overflow() {
    PackageSpec package = gpio_package();
    package.state("appended", mb::Value::from_bytes(mb::ByteBuffer{}));
    package.state("joined", mb::Value::from_bytes(mb::ByteBuffer{}));
    add_hex_conversion(package);
    add_one_byte_conversion(package);
    add_exports(package);

    FunctionSpec& append = package.function("append_immutable");
    append.push(mb::Value::from_string("00"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes), 1)
        .store_state(package.state("observed_bytes"));
    append.load_state(package.state("observed_bytes"))
        .push(mb::Value::from_integer(255))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::append_byte), 2)
        .store_state(package.state("appended"));
    append.load_state(package.state("observed_bytes"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .push(mb::Value::from_integer(1))
        .equal()
        .store_state(package.state("native_bool"));
    append.load_state(package.state("appended"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .store_state(package.state("observed_int"));
    append.load_state(package.state("appended"))
        .push(mb::Value::from_integer(1))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::byte_at), 2)
        .store_state(package.state("status"))
        .finish();

    FunctionSpec& concat = package.function("concat_immutable");
    concat.push(mb::Value::from_integer(1))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::one_byte), 1)
        .store_state(package.state("observed_bytes"));
    concat.push(mb::Value::from_integer(2))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::one_byte), 1)
        .store_state(package.state("appended"));
    concat.load_state(package.state("observed_bytes"))
        .load_state(package.state("appended"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::concatenate_bytes), 2)
        .store_state(package.state("joined"));
    concat.load_state(package.state("observed_bytes"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .push(mb::Value::from_integer(1))
        .equal()
        .store_state(package.state("native_bool"));
    concat.load_state(package.state("joined"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes_length), 1)
        .store_state(package.state("observed_int"));
    concat.load_state(package.state("joined"))
        .push(mb::Value::from_integer(1))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::byte_at), 2)
        .store_state(package.state("status"))
        .finish();

    add_named_failure(package, "byte_overflow",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::one_byte), {mb::Value::from_integer(256)});
    add_named_failure(package, "odd_hex",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::bytes), {mb::Value::from_string("0")});
    add_named_failure(package, "integer_whitespace",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::integer), {mb::Value::from_string(" 1")});
    add_named_failure(package, "integer_overflow",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::integer),
        {mb::Value::from_string("9223372036854775808")});
    add_named_failure(package, "number_precision",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::number), {mb::Value::from_integer(16'777'217)});
    add_named_failure(package, "append_budget",
        static_cast<std::uint16_t>(mb::DeviceNativeOperation::append_byte),
        {mb::Value::from_bytes(mb::ByteBuffer(4096, 0)), mb::Value::from_integer(1)});

    FunctionSpec& integer_exact = package.function("integer_exact");
    integer_exact.push(mb::Value::from_string("9223372036854775807"))
        .device(static_cast<std::uint16_t>(mb::DeviceNativeOperation::integer), 1)
        .push(mb::Value::from_integer(std::numeric_limits<std::int64_t>::max()))
        .equal()
        .store_state(package.state("native_bool"))
        .finish();

    Harness harness(package);
    harness.browser.dispatch("convert_hex");
    CHECK(exported_bool(harness, "export_native", "native"));
    CHECK(exported_number(harness, "export_observed", "observed") == 3.0F);
    harness.browser.dispatch("convert_one_byte");
    CHECK(exported_number(harness, "export_observed", "observed") == 1.0F);
    CHECK(exported_number(harness, "export_status", "status") == 255.0F);
    harness.browser.dispatch("append_immutable");
    CHECK(exported_bool(harness, "export_native", "native"));
    CHECK(exported_number(harness, "export_observed", "observed") == 2.0F);
    CHECK(exported_number(harness, "export_status", "status") == 255.0F);
    harness.browser.dispatch("concat_immutable");
    CHECK(exported_bool(harness, "export_native", "native"));
    CHECK(exported_number(harness, "export_observed", "observed") == 2.0F);
    CHECK(exported_number(harness, "export_status", "status") == 2.0F);
    harness.browser.dispatch("integer_exact");
    CHECK(exported_bool(harness, "export_native", "native"));
    for (const auto [function, message] : {
             std::pair<std::string_view, std::string_view>{"byte_overflow", "outside 0..255"},
             {"odd_hex", "even length"},
             {"integer_whitespace", "signed decimal"},
             {"integer_overflow", "overflows int64"},
             {"number_precision", "exactly representable"},
             {"append_budget", "maximum_bytes_per_buffer"}}) {
        harness.browser.dispatch(function);
        CHECK(std::string(harness.browser.last_error()).find(message) != std::string::npos);
    }
}

} // namespace

int main() {
    try {
        test_required_capability_preflight_rejects_before_on_load();
        test_optional_capability_probes();
        test_legacy_v2_without_registry();
        test_permissions_and_versions();
        test_gpio_output_and_input_callbacks();
        test_callback_type_and_completeness_failures();
        test_pending_budget();
        test_subscription_delivery();
        test_usb_binary_records();
        test_metadata_outside_callback();
        test_navigation_rejects_stale_completions();
        test_native_conversions_and_overflow();
    } catch (const std::exception& error) {
        std::cerr << "device builtin test exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " device builtin checks failed\n";
        return 1;
    }
    std::cout << "device builtin tests passed\n";
    return 0;
}
