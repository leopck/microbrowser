#include "device_test_fixture.h"
#include "microbrowser/device_api.h"
#include "microbrowser/package.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void advance(device_test::GpioFixture& fixture, mb::Browser& browser,
             mb::DeviceOperation expected) {
    if (fixture.model->pending_count() != 1) {
        throw std::runtime_error("GPIO artifact expected exactly one pending operation");
    }
    const mb::BackendOperation& operation = fixture.model->pending_operation();
    if (operation.id != expected) {
        throw std::runtime_error("GPIO artifact submitted an unexpected operation");
    }
    fixture.model->complete_next();
    browser.tick();
    if (!browser.last_error().empty()) {
        throw std::runtime_error("GPIO artifact reported: " + std::string(browser.last_error()));
    }
}

std::size_t log_count(const device_test::TestPlatform& platform, std::string_view message) {
    return static_cast<std::size_t>(std::count(platform.logs.begin(), platform.logs.end(), message));
}

void exercise_gpio_conformance(device_test::GpioFixture& fixture, mb::Browser& browser) {
    const std::size_t ready_before = log_count(fixture.platform, "GPIO subscription ready");
    const std::size_t notification_before = log_count(fixture.platform, "GPIO notification");
    const std::size_t complete_before = log_count(fixture.platform, "GPIO conformance complete");
    advance(fixture, browser, mb::DeviceOperation::open);
    advance(fixture, browser, mb::DeviceOperation::configure);
    advance(fixture, browser, mb::DeviceOperation::read);
    advance(fixture, browser, mb::DeviceOperation::subscribe);
    if (log_count(fixture.platform, "GPIO subscription ready") != ready_before + 1) {
        std::string message = "GPIO subscription callback mismatch; browser_error=";
        message += browser.last_error();
        for (const std::string& line : fixture.platform.logs) message += "\n  " + line;
        throw std::runtime_error(message);
    }
    if (fixture.model->publish(false) != mb::IoStatus::completed) {
        throw std::runtime_error("GPIO model could not publish a notification");
    }
    browser.tick();
    if (log_count(fixture.platform, "GPIO notification") != notification_before + 1) {
        throw std::runtime_error("GPIO notification did not reach the authored callback");
    }
    browser.dispatch("finish");
    advance(fixture, browser, mb::DeviceOperation::unsubscribe);
    advance(fixture, browser, mb::DeviceOperation::close);
    if (fixture.model->publish(true) != mb::IoStatus::invalid_argument) {
        throw std::runtime_error("GPIO notification survived subscription release");
    }
    if (fixture.model->pending_count() != 0 ||
        log_count(fixture.platform, "GPIO conformance complete") != complete_before + 1) {
        throw std::runtime_error("GPIO artifact did not complete its close sequence");
    }
    if (!browser.last_error().empty()) {
        throw std::runtime_error("GPIO artifact ended with an application error");
    }
}

void verify_confirmed_download(device_test::GpioFixture& fixture, mb::Browser& browser,
                               const std::vector<std::uint8_t>& artifact) {
    bool rejected = false;
    try {
        browser.confirm_native_package();
    } catch (const mb::ScriptError& error) {
        rejected = std::string_view(error.what()) == "native application confirmation is not current";
    }
    if (!rejected) throw std::runtime_error("native package confirmation was accepted outside its confirmation page");

    constexpr std::string_view url = "https://applications.example/gpio.mbp";
    browser.open_url(std::string(url));
    if (!fixture.platform.content_requests.empty()) {
        throw std::runtime_error("package download started before explicit confirmation");
    }
    browser.confirm_native_package();
    if (fixture.platform.content_requests.size() != 1) {
        throw std::runtime_error("confirmation did not start exactly one content request");
    }
    const mb::ContentRequest request = fixture.platform.content_requests.front().request;
    if (request.url != url || request.method != mb::ContentRequestMethod::get || !request.body.empty()) {
        throw std::runtime_error("native package download did not make an unmodified GET request");
    }
    mb::HttpResult response;
    response.status = 200;
    response.content_type = "application/octet-stream";
    response.body.assign(reinterpret_cast<const char*>(artifact.data()), artifact.size());
    fixture.platform.complete_next_content(std::move(response));
    browser.tick();
    if (!browser.last_error().empty()) {
        throw std::runtime_error("downloaded artifact did not load: " + std::string(browser.last_error()));
    }
    if (browser.metrics().package_bytes != artifact.size() || browser.page_name() != "conformance") {
        throw std::runtime_error("download did not install the expected application package");
    }
    exercise_gpio_conformance(fixture, browser);
    if (log_count(fixture.platform, "GPIO conformance complete") != 2) {
        throw std::runtime_error("downloaded and embedded artifacts did not complete identically");
    }
}

void verify_artifact(const char* path) {
    device_test::GpioFixture fixture;
    const mb::CapabilityVersion version{mb::Capability::gpio, mb::peripheral_abi_major, 0};
    fixture.registry.register_endpoint({"user_button", version, true},
                                       mb::EndpointAccess::application, fixture.model);
    fixture.model->high = true;
    const std::vector<std::uint8_t> artifact = mb::read_package_file(path);
    if (mb::parse_package(artifact).format_major != 3) {
        throw std::runtime_error("portable artifact is not package version 3");
    }
    mb::Browser browser(fixture.platform, {64, 64}, "http://127.0.0.1:8765", &fixture.registry);
    browser.load(artifact);
    exercise_gpio_conformance(fixture, browser);
    verify_confirmed_download(fixture, browser, artifact);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test_portable_gpio PACKAGE.mbp\n";
        return 2;
    }
    try {
        verify_artifact(argv[1]);
        std::cout << "portable GPIO and confirmed-download tests passed: same artifact, host-modeled HTTP\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "portable GPIO test failed: " << error.what() << '\n';
        return 1;
    }
}
