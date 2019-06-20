#pragma once

#include "apple_hid_usage_tables.hpp"
#include "apple_notification_center.hpp"
#include "constants.hpp"
#include "device_grabber_details/entry.hpp"
#include "device_grabber_details/fn_function_keys_manipulator_manager.hpp"
#include "device_grabber_details/notification_message_manager.hpp"
#include "device_grabber_details/simple_modifications_manipulator_manager.hpp"
#include "event_tap_utility.hpp"
#include "hid_keyboard_caps_lock_led_state_manager.hpp"
#include "hid_keyboard_num_lock_led_state_manager.hpp"
#include "iokit_utility.hpp"
#include "json_writer.hpp"
#include "krbn_notification_center.hpp"
#include "logger.hpp"
#include "manipulator/manipulator_managers_connector.hpp"
#include "manipulator/manipulators/post_event_to_virtual_devices/post_event_to_virtual_devices.hpp"
#include "monitor/configuration_monitor.hpp"
#include "monitor/event_tap_monitor.hpp"
#include "probable_stuck_events_manager.hpp"
#include "types.hpp"
#include "virtual_hid_device_client.hpp"
#include <deque>
#include <fstream>
#include <nlohmann/json.hpp>
#include <nod/nod.hpp>
#include <pqrs/osx/iokit_hid_manager.hpp>
#include <pqrs/osx/system_preferences.hpp>
#include <pqrs/spdlog.hpp>
#include <thread>
#include <time.h>

namespace krbn {
namespace grabber {
class device_grabber final : public pqrs::dispatcher::extra::dispatcher_client {
public:
  device_grabber(const device_grabber&) = delete;

  device_grabber(std::weak_ptr<console_user_server_client> weak_console_user_server_client) : dispatcher_client(),
                                                                                              profile_(nlohmann::json::object()),
                                                                                              logger_unique_filter_(logger::get_logger()) {
    simple_modifications_manipulator_manager_ = std::make_shared<device_grabber_details::simple_modifications_manipulator_manager>();
    complex_modifications_manipulator_manager_ = std::make_shared<manipulator::manipulator_manager>();
    fn_function_keys_manipulator_manager_ = std::make_shared<device_grabber_details::fn_function_keys_manipulator_manager>();
    post_event_to_virtual_devices_manipulator_manager_ = std::make_shared<manipulator::manipulator_manager>();

    merged_input_event_queue_ = std::make_shared<event_queue::queue>();
    simple_modifications_applied_event_queue_ = std::make_shared<event_queue::queue>();
    complex_modifications_applied_event_queue_ = std::make_shared<event_queue::queue>();
    fn_function_keys_applied_event_queue_ = std::make_shared<event_queue::queue>();
    posted_event_queue_ = std::make_shared<event_queue::queue>();

    virtual_hid_device_client_ = std::make_shared<virtual_hid_device_client>();

    virtual_hid_device_client_->client_connected.connect([this] {
      logger::get_logger()->info("virtual_hid_device_client_ is connected");

      update_virtual_hid_keyboard();
      update_virtual_hid_pointing();

      update_devices_disabled();
      async_grab_devices();
    });

    virtual_hid_device_client_->virtual_hid_keyboard_ready.connect([this] {
      update_devices_disabled();
      async_grab_devices();
    });

    virtual_hid_device_client_->client_disconnected.connect([this] {
      logger::get_logger()->info("virtual_hid_device_client_ is disconnected");

      stop();
    });

    post_event_to_virtual_devices_manipulator_ =
        std::make_shared<manipulator::manipulators::post_event_to_virtual_devices::post_event_to_virtual_devices>(
            weak_console_user_server_client);
    post_event_to_virtual_devices_manipulator_manager_->push_back_manipulator(std::shared_ptr<manipulator::manipulators::base>(post_event_to_virtual_devices_manipulator_));

    complex_modifications_applied_event_queue_->enable_manipulator_environment_json_output(constants::get_manipulator_environment_json_file_path());

    // Connect manipulator_managers

    manipulator_managers_connector_.emplace_back_connection(simple_modifications_manipulator_manager_->get_manipulator_manager(),
                                                            merged_input_event_queue_,
                                                            simple_modifications_applied_event_queue_);
    manipulator_managers_connector_.emplace_back_connection(complex_modifications_manipulator_manager_,
                                                            complex_modifications_applied_event_queue_);
    manipulator_managers_connector_.emplace_back_connection(fn_function_keys_manipulator_manager_->get_manipulator_manager(),
                                                            fn_function_keys_applied_event_queue_);
    manipulator_managers_connector_.emplace_back_connection(post_event_to_virtual_devices_manipulator_manager_,
                                                            posted_event_queue_);

    external_signal_connections_.emplace_back(
        krbn_notification_center::get_instance().input_event_arrived.connect([this] {
          manipulate(pqrs::osx::chrono::mach_absolute_time_point());
        }));

    // hid_manager_

    std::vector<pqrs::cf::cf_ptr<CFDictionaryRef>> matching_dictionaries{
        pqrs::osx::iokit_hid_manager::make_matching_dictionary(
            pqrs::osx::iokit_hid_usage_page_generic_desktop,
            pqrs::osx::iokit_hid_usage_generic_desktop_keyboard),

        pqrs::osx::iokit_hid_manager::make_matching_dictionary(
            pqrs::osx::iokit_hid_usage_page_generic_desktop,
            pqrs::osx::iokit_hid_usage_generic_desktop_mouse),

        pqrs::osx::iokit_hid_manager::make_matching_dictionary(
            pqrs::osx::iokit_hid_usage_page_generic_desktop,
            pqrs::osx::iokit_hid_usage_generic_desktop_pointer),
    };

    hid_manager_ = std::make_unique<pqrs::osx::iokit_hid_manager>(weak_dispatcher_,
                                                                  matching_dictionaries,
                                                                  std::chrono::milliseconds(1000));

    hid_manager_->device_matched.connect([this](auto&& registry_entry_id, auto&& device_ptr) {
      if (device_ptr) {
        auto device_id = make_device_id(registry_entry_id);

        if (iokit_utility::is_karabiner_virtual_hid_device(*device_ptr)) {
          return;
        }

        // ----------------------------------------
        // probable_stuck_events_managers_

        add_probable_stuck_events_manager(device_id);

        // ----------------------------------------
        // entries_

        auto entry = std::make_shared<device_grabber_details::entry>(device_id,
                                                                     *device_ptr,
                                                                     core_configuration_);
        entries_[device_id] = entry;

        entry->get_hid_queue_value_monitor()->values_arrived.connect([this, device_id](auto&& values_ptr) {
          auto it = entries_.find(device_id);
          if (it != std::end(entries_)) {
            auto event_queue = event_queue::utility::make_queue(device_id,
                                                                iokit_utility::make_hid_values(values_ptr));
            event_queue = event_queue::utility::insert_device_keys_and_pointing_buttons_are_released_event(event_queue,
                                                                                                           device_id,
                                                                                                           it->second->get_pressed_keys_manager());
            values_arrived(it->second, event_queue);
          }
        });

        entry->get_hid_queue_value_monitor()->started.connect([this, device_id] {
          auto it = entries_.find(device_id);
          if (it != std::end(entries_)) {
            logger::get_logger()->info("{0} is grabbed.",
                                       it->second->get_device_name());
            logger_unique_filter_.reset();

            post_device_grabbed_event(it->second->get_device_properties());

            it->second->set_grabbed(true);

            update_caps_lock_led();

            update_num_lock_led();

            update_virtual_hid_pointing();

            apple_notification_center::post_distributed_notification_to_all_sessions(constants::get_distributed_notification_device_grabbing_state_is_changed());
          }
        });

        entry->get_hid_queue_value_monitor()->stopped.connect([this, device_id] {
          auto it = entries_.find(device_id);
          if (it != std::end(entries_)) {
            logger::get_logger()->info("{0} is ungrabbed.",
                                       it->second->get_device_name());
            logger_unique_filter_.reset();

            it->second->set_grabbed(false);

            post_device_ungrabbed_event(device_id);

            update_virtual_hid_pointing();

            apple_notification_center::post_distributed_notification_to_all_sessions(constants::get_distributed_notification_device_grabbing_state_is_changed());
          }
        });

        // ----------------------------------------

        output_devices_json();
        output_device_details_json();

        update_virtual_hid_pointing();

        // ----------------------------------------

        update_devices_disabled();
        async_grab_devices();
      }
    });

    hid_manager_->device_terminated.connect([this](auto&& registry_entry_id) {
      auto device_id = make_device_id(registry_entry_id);

      // entries_

      {
        auto it = entries_.find(device_id);
        if (it != std::end(entries_)) {
          logger::get_logger()->info("{0} is terminated.",
                                     it->second->get_device_name());
          logger_unique_filter_.reset();

          if (auto device_properties = it->second->get_device_properties()) {
            if (device_properties->get_is_keyboard().value_or(false) &&
                device_properties->get_is_karabiner_virtual_hid_device().value_or(false)) {
              virtual_hid_device_client_->async_close();
              async_ungrab_devices();

              virtual_hid_device_client_->async_connect();
            }
          }

          entries_.erase(it);
        }
      }

      // probable_stuck_events_managers_

      probable_stuck_events_managers_.erase(device_id);

      // notification_message_manager_

      if (notification_message_manager_) {
        notification_message_manager_->erase_device(device_id);
      }

      // ----------------------------------------

      output_devices_json();
      output_device_details_json();

      // ----------------------------------------

      post_device_ungrabbed_event(device_id);

      update_virtual_hid_pointing();

      // ----------------------------------------
      update_devices_disabled();
      async_grab_devices();
    });

    hid_manager_->error_occurred.connect([this](auto&& message, auto&& iokit_return) {
      logger::get_logger()->error("{0}: {1}", message, iokit_return.to_string());
      logger_unique_filter_.reset();
    });

    notification_message_manager_ = std::make_shared<device_grabber_details::notification_message_manager>(
        weak_console_user_server_client);
  }

  virtual ~device_grabber(void) {
    detach_from_dispatcher([this] {
      stop();

      notification_message_manager_ = nullptr;

      hid_manager_ = nullptr;

      external_signal_connections_.clear();

      post_event_to_virtual_devices_manipulator_ = nullptr;

      simple_modifications_manipulator_manager_ = nullptr;
      complex_modifications_manipulator_manager_ = nullptr;
      fn_function_keys_manipulator_manager_ = nullptr;
      post_event_to_virtual_devices_manipulator_manager_ = nullptr;
    });
  }

  void async_start(const std::string& user_core_configuration_file_path) {
    enqueue_to_dispatcher([this, user_core_configuration_file_path] {
      // We should call CGEventTapCreate after user is logged in.
      // So, we create event_tap_monitor here.
      event_tap_monitor_ = std::make_unique<event_tap_monitor>();

      event_tap_monitor_->pointing_device_event_arrived.connect([this](auto&& event_type, auto&& event) {
        auto e = event_queue::event::make_pointing_device_event_from_event_tap_event();
        event_queue::entry entry(device_id(0),
                                 event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                                 e,
                                 event_type,
                                 event);

        merged_input_event_queue_->push_back_entry(entry);

        krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
      });

      event_tap_monitor_->async_start();

      configuration_monitor_ = std::make_unique<configuration_monitor>(user_core_configuration_file_path);

      configuration_monitor_->core_configuration_updated.connect([this](auto&& weak_core_configuration) {
        if (auto core_configuration = weak_core_configuration.lock()) {
          core_configuration_ = core_configuration;

          for (auto&& e : entries_) {
            e.second->set_core_configuration(core_configuration);
          }

          logger_unique_filter_.reset();
          set_profile(core_configuration->get_selected_profile());
        }
      });

      configuration_monitor_->async_start();

      virtual_hid_device_client_->async_connect();
    });
  }

  void async_stop(void) {
    enqueue_to_dispatcher([this] {
      stop();
    });
  }

  void async_update_probable_stuck_events_by_observer(device_id device_id,
                                                      const key_down_up_valued_event& event,
                                                      event_type event_type,
                                                      absolute_time_point time_stamp) {
    enqueue_to_dispatcher([this, device_id, event, event_type, time_stamp] {
      // `karabiner_observer` may catch input events
      // while the device is grabbed due to macOS event handling issue.
      // Thus, we have to ignore events manually while the device is grabbed.
      if (!is_grabbed(device_id, time_stamp)) {
        auto m = add_probable_stuck_events_manager(device_id);

        bool needs_regrab = m->update(event,
                                      event_type,
                                      time_stamp,
                                      device_state::ungrabbed);
        if (needs_regrab) {
          grab_device(device_id);
        }
      }
    });
  }

  void async_set_observed_devices(const std::unordered_set<device_id>& observed_devices) {
    enqueue_to_dispatcher([this, observed_devices] {
      observed_devices_ = observed_devices;

      async_grab_devices();
    });
  }

  void async_grab_devices(void) {
    enqueue_to_dispatcher([this] {
      for (auto&& e : entries_) {
        grab_device(e.second);
      }
    });
  }

  void async_ungrab_devices(void) {
    enqueue_to_dispatcher([this] {
      for (auto&& e : entries_) {
        e.second->get_hid_queue_value_monitor()->async_stop();
      }

      logger::get_logger()->info("Connected devices are ungrabbed");
    });
  }

  void async_set_caps_lock_state(bool state) {
    enqueue_to_dispatcher([this, state] {
      last_caps_lock_state_ = state;
      post_caps_lock_state_changed_event(state);
      update_caps_lock_led();
    });
  }

  void async_set_num_lock_state(bool state) {
    enqueue_to_dispatcher([this, state] {
      last_num_lock_state_ = state;
      post_num_lock_state_changed_event(state);
      update_num_lock_led();
    });
  }
  void async_set_system_preferences_properties(const pqrs::osx::system_preferences::properties& value) {
    enqueue_to_dispatcher([this, value] {
      system_preferences_properties_ = value;

      fn_function_keys_manipulator_manager_->update(profile_,
                                                    system_preferences_properties_);
      async_post_system_preferences_properties_changed_event();
    });
  }

  void async_post_frontmost_application_changed_event(const pqrs::osx::frontmost_application_monitor::application& application) {
    enqueue_to_dispatcher([this, application] {
      auto event = event_queue::event::make_frontmost_application_changed_event(application);
      event_queue::entry entry(device_id(0),
                               event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                               event,
                               event_type::single,
                               event);

      merged_input_event_queue_->push_back_entry(entry);

      krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
    });
  }

  void async_post_input_source_changed_event(const pqrs::osx::input_source::properties& properties) {
    enqueue_to_dispatcher([this, properties] {
      auto event = event_queue::event::make_input_source_changed_event(properties);
      event_queue::entry entry(device_id(0),
                               event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                               event,
                               event_type::single,
                               event);

      merged_input_event_queue_->push_back_entry(entry);

      krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
    });
  }

  void async_post_system_preferences_properties_changed_event(void) {
    enqueue_to_dispatcher([this] {
      auto event = event_queue::event::make_system_preferences_properties_changed_event(
          system_preferences_properties_);
      event_queue::entry entry(device_id(0),
                               event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                               event,
                               event_type::single,
                               event);

      merged_input_event_queue_->push_back_entry(entry);

      krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
    });
  }

  void async_post_virtual_hid_keyboard_configuration_changed_event(void) {
    enqueue_to_dispatcher([this] {
      auto event = event_queue::event::make_virtual_hid_keyboard_configuration_changed_event(
          profile_.get_virtual_hid_keyboard());
      event_queue::entry entry(device_id(0),
                               event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                               event,
                               event_type::single,
                               event);

      merged_input_event_queue_->push_back_entry(entry);

      krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
    });
  }

private:
  void stop(void) {
    configuration_monitor_ = nullptr;

    async_ungrab_devices();

    event_tap_monitor_ = nullptr;

    virtual_hid_device_client_->async_close();
  }

  // This method is executed in the shared dispatcher thread.
  void manipulate(absolute_time_point now) {
    manipulator_managers_connector_.manipulate(now);

    posted_event_queue_->clear_events();
    post_event_to_virtual_devices_manipulator_->async_post_events(virtual_hid_device_client_);

    if (auto min = manipulator_managers_connector_.min_input_event_time_stamp()) {
      auto when = when_now();
      if (now < *min) {
        when += pqrs::osx::chrono::make_milliseconds(*min - now);
      }

      enqueue_to_dispatcher(
          [this, min] {
            manipulate(*min);
          },
          when);
    }
  }

  void values_arrived(std::shared_ptr<device_grabber_details::entry> entry,
                      std::shared_ptr<event_queue::queue> event_queue) {
    // Manipulate events

    if (auto probable_stuck_events_manager = find_probable_stuck_events_manager(entry->get_device_id())) {
      if (!entry->get_first_value_arrived()) {
        // First grabbed event is arrived.

        entry->set_first_value_arrived(true);
        probable_stuck_events_manager->clear();
      }

      bool needs_regrab = false;

      for (const auto& e : event_queue->get_entries()) {
        if (auto ev = e.get_event().make_key_down_up_valued_event()) {
          needs_regrab |= probable_stuck_events_manager->update(
              *ev,
              e.get_event_type(),
              e.get_event_time_stamp().get_time_stamp(),
              device_state::grabbed);
        }

        if (!entry->get_disabled()) {
          event_queue::entry qe(e.get_device_id(),
                                e.get_event_time_stamp(),
                                e.get_event(),
                                e.get_event_type(),
                                e.get_original_event());

          merged_input_event_queue_->push_back_entry(qe);
        }
      }

      if (needs_regrab) {
        grab_device(entry);
      }

      krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
      // manipulator_managers_connector_.log_events_sizes(logger::get_logger());
    }
  }

  void post_device_grabbed_event(std::shared_ptr<device_properties> device_properties) {
    if (device_properties) {
      if (auto device_id = device_properties->get_device_id()) {
        auto event = event_queue::event::make_device_grabbed_event(*device_properties);
        event_queue::entry entry(*device_id,
                                 event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                                 event,
                                 event_type::single,
                                 event);

        merged_input_event_queue_->push_back_entry(entry);

        krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
      }
    }
  }

  void post_device_ungrabbed_event(device_id device_id) {
    auto event = event_queue::event::make_device_ungrabbed_event();
    event_queue::entry entry(device_id,
                             event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                             event,
                             event_type::single,
                             event);

    merged_input_event_queue_->push_back_entry(entry);

    krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
  }

  void post_caps_lock_state_changed_event(bool caps_lock_state) {
    event_queue::event event(event_queue::event::type::caps_lock_state_changed, caps_lock_state);
    event_queue::entry entry(device_id(0),
                             event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                             event,
                             event_type::single,
                             event);

    merged_input_event_queue_->push_back_entry(entry);

    krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
  }

  void post_num_lock_state_changed_event(bool num_lock_state) {
    event_queue::event event(event_queue::event::type::num_lock_state_changed, num_lock_state);
    event_queue::entry entry(device_id(0),
                             event_queue::event_time_stamp(pqrs::osx::chrono::mach_absolute_time_point()),
                             event,
                             event_type::single,
                             event);

    merged_input_event_queue_->push_back_entry(entry);

    krbn_notification_center::get_instance().enqueue_input_event_arrived(*this);
  }  

  std::shared_ptr<probable_stuck_events_manager> add_probable_stuck_events_manager(device_id device_id) {
    auto it = probable_stuck_events_managers_.find(device_id);
    if (it != std::end(probable_stuck_events_managers_)) {
      return it->second;
    }

    auto m = std::make_shared<probable_stuck_events_manager>();
    probable_stuck_events_managers_[device_id] = m;
    return m;
  }

  std::shared_ptr<probable_stuck_events_manager> find_probable_stuck_events_manager(device_id device_id) const {
    auto it = probable_stuck_events_managers_.find(device_id);
    if (it != std::end(probable_stuck_events_managers_)) {
      return it->second;
    }
    return nullptr;
  }

  void grab_device(std::shared_ptr<device_grabber_details::entry> entry) const {
    if (make_grabbable_state(entry) == grabbable_state::state::grabbable) {
      entry->async_start_queue_value_monitor();
    } else {
      entry->async_stop_queue_value_monitor();
    }
  }

  void grab_device(device_id device_id) const {
    auto it = entries_.find(device_id);
    if (it != std::end(entries_)) {
      grab_device(it->second);
    }
  }

  grabbable_state::state make_grabbable_state(std::shared_ptr<device_grabber_details::entry> entry) const {
    if (!entry) {
      return grabbable_state::state::ungrabbable_permanently;
    }

    if (entry->is_ignored_device()) {
      auto device_properties = entry->get_device_properties();

      // If we need to disable the built-in keyboard, we have to grab it.
      if (device_properties &&
          device_properties->get_is_built_in_keyboard().value_or(false) &&
          need_to_disable_built_in_keyboard()) {
        // Do nothing
      } else {
        auto message = fmt::format("{0} is ignored.",
                                   entry->get_device_name());
        logger_unique_filter_.info(message);
        unset_device_ungrabbable_temporarily_notification_message(entry->get_device_id());
        return grabbable_state::state::ungrabbable_permanently;
      }
    }

    // ----------------------------------------
    // Ungrabbable while virtual_hid_device_client_ is not ready.

    if (!virtual_hid_device_client_->is_connected()) {
      std::string message = "virtual_hid_device_client is not connected yet. Please wait for a while.";
      logger_unique_filter_.warn(message);
      unset_device_ungrabbable_temporarily_notification_message(entry->get_device_id());
      return grabbable_state::state::ungrabbable_temporarily;
    }

    if (!virtual_hid_device_client_->is_virtual_hid_keyboard_ready()) {
      std::string message = "virtual_hid_keyboard is not ready. Please wait for a while.";
      logger_unique_filter_.warn(message);
      unset_device_ungrabbable_temporarily_notification_message(entry->get_device_id());
      return grabbable_state::state::ungrabbable_temporarily;
    }

    // ----------------------------------------
    // Ungrabbable before observed.

    if (observed_devices_.find(entry->get_device_id()) == std::end(observed_devices_)) {
      std::string message = fmt::format("{0} is not observed yet. Please wait for a while.",
                                        entry->get_device_name());
      logger_unique_filter_.warn(message);
      unset_device_ungrabbable_temporarily_notification_message(entry->get_device_id());
      return grabbable_state::state::ungrabbable_temporarily;
    }

    // ----------------------------------------
    // Ungrabbable while probable stuck events exist

    if (auto m = find_probable_stuck_events_manager(entry->get_device_id())) {
      if (auto event = m->find_probable_stuck_event()) {
        auto message = fmt::format("{0} is ignored temporarily until {1} is pressed again.",
                                   entry->get_device_name(),
                                   event->to_string());
        logger_unique_filter_.warn(message);

        if (notification_message_manager_) {
          notification_message_manager_->set_device_ungrabbable_temporarily_message(
              entry->get_device_id(),
              fmt::format("{0} is ignored temporarily until {1} is pressed again.",
                          entry->get_device_short_name(),
                          event->to_string()));
        }

        return grabbable_state::state::ungrabbable_temporarily;
      }
    }

    // ----------------------------------------

    unset_device_ungrabbable_temporarily_notification_message(entry->get_device_id());

    return grabbable_state::state::grabbable;
  }

  void unset_device_ungrabbable_temporarily_notification_message(device_id id) const {
    if (notification_message_manager_) {
      notification_message_manager_->set_device_ungrabbable_temporarily_message(id, "");
    }
  }

  void event_tap_pointing_device_event_callback(CGEventType type, CGEventRef event) {
  }

  void update_caps_lock_led(void) {
    std::optional<led_state> state;
    if (last_caps_lock_state_) {
      state = *last_caps_lock_state_ ? led_state::on : led_state::off;
    }

    for (auto&& e : entries_) {
      e.second->get_caps_lock_led_state_manager()->set_state(state);
    }
  }

  void update_num_lock_led(void) {
    std::optional<led_state> state;
    if (last_num_lock_state_) {
      state = *last_num_lock_state_ ? led_state::on : led_state::off;
    }

    for (auto&& e : entries_) {
      e.second->get_num_lock_led_state_manager()->set_state(state);
    }
  }



  bool is_grabbed(device_id device_id,
                  absolute_time_point time_stamp) const {
    auto it = entries_.find(device_id);
    if (it != std::end(entries_)) {
      return it->second->is_grabbed(time_stamp);
    }
    return false;
  }

  bool is_pointing_device_grabbed(void) const {
    for (const auto& e : entries_) {
      if (auto device_properties = e.second->get_device_properties()) {
        if (device_properties->get_is_pointing_device().value_or(false) &&
            e.second->get_grabbed()) {
          return true;
        }
      }
    }
    return false;
  }

  void update_virtual_hid_keyboard(void) {
    if (virtual_hid_device_client_->is_connected()) {
      pqrs::karabiner_virtual_hid_device::properties::keyboard_initialization properties;
      properties.country_code = static_cast<uint8_t>(
          type_safe::get(profile_.get_virtual_hid_keyboard().get_country_code()));

      virtual_hid_device_client_->async_initialize_virtual_hid_keyboard(properties);
    }
  }

  void update_virtual_hid_pointing(void) {
    if (virtual_hid_device_client_->is_connected()) {
      if (is_pointing_device_grabbed() ||
          manipulator_managers_connector_.needs_virtual_hid_pointing()) {
        virtual_hid_device_client_->async_initialize_virtual_hid_pointing();
        return;
      }

      virtual_hid_device_client_->async_terminate_virtual_hid_pointing();
    }
  }

  bool need_to_disable_built_in_keyboard(void) const {
    for (const auto& e : entries_) {
      if (e.second->is_disable_built_in_keyboard_if_exists()) {
        return true;
      }
    }
    return false;
  }

  void update_devices_disabled(void) {
    for (const auto& e : entries_) {
      if (auto device_properties = e.second->get_device_properties()) {
        if (device_properties->get_is_built_in_keyboard().value_or(false) &&
            need_to_disable_built_in_keyboard()) {
          e.second->set_disabled(true);
        } else {
          e.second->set_disabled(false);
        }
      }
    }
  }

  void output_devices_json(void) const {
    connected_devices::connected_devices connected_devices;
    for (const auto& e : entries_) {
      if (auto device_properties = e.second->get_device_properties()) {
        connected_devices::details::device d(*device_properties);
        connected_devices.push_back_device(d);
      }
    }

    auto file_path = constants::get_devices_json_file_path();
    connected_devices.async_save_to_file(file_path);
  }

  void output_device_details_json(void) const {
    std::vector<device_properties> device_details;
    for (const auto& e : entries_) {
      if (auto device_properties = e.second->get_device_properties()) {
        device_details.push_back(*device_properties);
      }
    }

    std::sort(std::begin(device_details),
              std::end(device_details),
              [](auto& a, auto& b) {
                return a.compare(b);
              });

    auto file_path = constants::get_device_details_json_file_path();
    json_writer::async_save_to_file(nlohmann::json(device_details), file_path, 0755, 0644);
  }

  void set_profile(const core_configuration::details::profile& profile) {
    profile_ = profile;

    if (hid_manager_) {
      hid_manager_->async_set_device_matched_delay(
          profile_.get_parameters().get_delay_milliseconds_before_open_device());
      hid_manager_->async_start();
    }

    simple_modifications_manipulator_manager_->update(profile_);
    update_complex_modifications_manipulators();
    fn_function_keys_manipulator_manager_->update(profile_,
                                                  system_preferences_properties_);

    update_virtual_hid_keyboard();
    update_virtual_hid_pointing();

    update_devices_disabled();
    async_grab_devices();
    async_post_system_preferences_properties_changed_event();
    async_post_virtual_hid_keyboard_configuration_changed_event();
  }

  void update_complex_modifications_manipulators(void) {
    complex_modifications_manipulator_manager_->invalidate_manipulators();

    for (const auto& rule : profile_.get_complex_modifications().get_rules()) {
      for (const auto& manipulator : rule.get_manipulators()) {
        try {
          auto m = manipulator::manipulator_factory::make_manipulator(manipulator.get_json(),
                                                                      manipulator.get_parameters());
          for (const auto& c : manipulator.get_conditions()) {
            m->push_back_condition(manipulator::manipulator_factory::make_condition(c.get_json()));
          }
          complex_modifications_manipulator_manager_->push_back_manipulator(m);

        } catch (const pqrs::json::unmarshal_error& e) {
          logger::get_logger()->error(fmt::format("karabiner.json error: {0}", e.what()));

        } catch (const std::exception& e) {
          logger::get_logger()->error(e.what());
        }
      }
    }
  }

  std::shared_ptr<virtual_hid_device_client> virtual_hid_device_client_;

  std::vector<nod::scoped_connection> external_signal_connections_;

  std::unique_ptr<configuration_monitor> configuration_monitor_;
  std::shared_ptr<const core_configuration::core_configuration> core_configuration_;

  std::unique_ptr<event_tap_monitor> event_tap_monitor_;
  std::optional<bool> last_caps_lock_state_;
  std::optional<bool> last_num_lock_state_;
  std::unique_ptr<pqrs::osx::iokit_hid_manager> hid_manager_;
  // `operation_type::device_observed` might be received before
  // `device_grabber_details::entry` is created.
  // Thus, we manage observed state independently.
  std::unordered_set<device_id> observed_devices_;
  std::unordered_map<device_id, std::shared_ptr<probable_stuck_events_manager>> probable_stuck_events_managers_;
  std::unordered_map<device_id, std::shared_ptr<device_grabber_details::entry>> entries_;

  core_configuration::details::profile profile_;
  pqrs::osx::system_preferences::properties system_preferences_properties_;

  manipulator::manipulator_managers_connector manipulator_managers_connector_;

  std::shared_ptr<event_queue::queue> merged_input_event_queue_;

  std::shared_ptr<device_grabber_details::simple_modifications_manipulator_manager> simple_modifications_manipulator_manager_;
  std::shared_ptr<event_queue::queue> simple_modifications_applied_event_queue_;

  std::shared_ptr<manipulator::manipulator_manager> complex_modifications_manipulator_manager_;
  std::shared_ptr<event_queue::queue> complex_modifications_applied_event_queue_;

  std::shared_ptr<device_grabber_details::fn_function_keys_manipulator_manager> fn_function_keys_manipulator_manager_;
  std::shared_ptr<event_queue::queue> fn_function_keys_applied_event_queue_;

  std::shared_ptr<manipulator::manipulators::post_event_to_virtual_devices::post_event_to_virtual_devices> post_event_to_virtual_devices_manipulator_;
  std::shared_ptr<manipulator::manipulator_manager> post_event_to_virtual_devices_manipulator_manager_;
  std::shared_ptr<event_queue::queue> posted_event_queue_;

  std::shared_ptr<device_grabber_details::notification_message_manager> notification_message_manager_;

  mutable pqrs::spdlog::unique_filter logger_unique_filter_;
};
} // namespace grabber
} // namespace krbn
