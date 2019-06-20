#pragma once

#include "types.hpp"
#include <mach/mach_time.h>
#include <optional>
#include <pqrs/dispatcher.hpp>
#include <pqrs/osx/iokit_hid_device.hpp>

namespace krbn {
class hid_keyboard_num_lock_led_state_manager final : public pqrs::dispatcher::extra::dispatcher_client {
public:
  hid_keyboard_num_lock_led_state_manager(IOHIDDeviceRef device) : dispatcher_client(),
                                                                    device_(device),
                                                                    timer_(*this) {
    if (device_) {
      pqrs::osx::iokit_hid_device hid_device(*device_);
      for (const auto& e : hid_device.make_elements()) {
        auto usage_page = pqrs::osx::iokit_hid_usage_page(IOHIDElementGetUsagePage(*e));
        auto usage = pqrs::osx::iokit_hid_usage(IOHIDElementGetUsage(*e));

        if (usage_page == pqrs::osx::iokit_hid_usage_page_leds &&
            usage == pqrs::osx::iokit_hid_usage_led_num_lock) {
          element_ = e;
        }
      }
    }
  }

  ~hid_keyboard_num_lock_led_state_manager(void) {
    detach_from_dispatcher([this] {
      timer_.stop();
    });
  }

  void set_state(std::optional<led_state> value) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    state_ = value;

    enqueue_to_dispatcher([this] {
      update_num_lock_led();
    });
  }

  void async_start(void) {
    timer_.start(
        [this] {
          update_num_lock_led();
        },
        std::chrono::milliseconds(3000));
  }

  void async_stop(void) {
    timer_.stop();
  }

private:
  void update_num_lock_led(void) const {
    // macOS 10.12 sometimes synchronize caps lock LED to internal keyboard caps lock state.
    // The behavior causes LED state mismatch because
    // the caps lock state of karabiner_grabber is independent from the hardware caps lock state.
    // Thus, we monitor the LED state and update it if needed.

    if (auto integer_value = make_integer_value()) {
      if (device_ && element_) {
        if (auto value = IOHIDValueCreateWithIntegerValue(kCFAllocatorDefault,
                                                          *element_,
                                                          mach_absolute_time(),
                                                          *integer_value)) {
          IOHIDDeviceSetValue(*device_, *element_, value);

          CFRelease(value);
        }
      }
    }
  }

  std::optional<CFIndex> make_integer_value(void) const {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (state_ && element_) {
      if (*state_ == led_state::on) {
        return IOHIDElementGetLogicalMax(*element_);
      } else {
        return IOHIDElementGetLogicalMin(*element_);
      }
    }

    return std::nullopt;
  }

  pqrs::cf::cf_ptr<IOHIDDeviceRef> device_;
  pqrs::cf::cf_ptr<IOHIDElementRef> element_;
  std::optional<led_state> state_;
  mutable std::mutex state_mutex_;
  pqrs::dispatcher::extra::timer timer_;
};
} // namespace krbn
