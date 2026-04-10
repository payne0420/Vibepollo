/**
 * @file src/platform/windows/display_composite.cpp
 * @brief Implementation of composite display for multi-monitor capture.
 *
 * Each sub-display runs its own capture thread using the standard platf::display_t
 * interface. The composite capture loop collects the latest frame from each sub-display
 * and stitches them side-by-side into a single combined image in system memory.
 */

#include "display_composite.h"
#include "src/config.h"
#include "src/display_device.h"
#include "src/logging.h"

#include <boost/log/trivial.hpp>
#include <cstring>

namespace platf::dxgi {

  /// Image subclass that owns its pixel buffer for composite frames.
  struct composite_img_t: public ::platf::img_t {
    ~composite_img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  display_composite_t::~display_composite_t() {
    m_stop_requested = true;
    m_composite_cv.notify_all();

    for (auto &state : m_sub_displays) {
      state->running = false;
      if (state->capture_thread.joinable()) {
        state->capture_thread.join();
      }
    }

    m_sub_displays.clear();
    m_primary_display.reset();
  }

  int display_composite_t::init(
    const ::video::config_t &config,
    const std::vector<std::string> &display_names,
    int per_monitor_width,
    int per_monitor_height
  ) {
    m_monitor_count = static_cast<int>(display_names.size());
    m_per_monitor_width = per_monitor_width;
    m_per_monitor_height = per_monitor_height;
    m_combined_width = per_monitor_width * m_monitor_count;
    m_combined_height = per_monitor_height;

    // Set display_t dimensions to the combined resolution
    width = m_combined_width;
    height = m_combined_height;
    env_width = m_combined_width;
    env_height = m_combined_height;
    offset_x = 0;
    offset_y = 0;

    // Create individual display instances for each monitor
    for (int i = 0; i < m_monitor_count; i++) {
      auto sub_disp = platf::display(
        mem_type_e::system,  // Use system memory for RAM-based compositing
        display_names[i],
        config);

      if (!sub_disp) {
        BOOST_LOG(error) << "Composite display: failed to create sub-display " << i
                         << " for " << display_names[i];
        return -1;
      }

      auto state = std::make_unique<sub_display_state_t>();
      state->display = std::move(sub_disp);
      m_sub_displays.push_back(std::move(state));

      if (i == 0) {
        m_primary_display = m_sub_displays[0]->display;
      }
    }

    BOOST_LOG(info) << "Composite display initialized: " << m_monitor_count
                    << " monitors, combined " << m_combined_width << "x" << m_combined_height;

    return 0;
  }

  void display_composite_t::sub_display_capture_loop(int index) {
    auto &state = m_sub_displays[index];
    state->running = true;

    // Custom callbacks that store frames in our shared state
    auto push_cb = [this, &state, index](std::shared_ptr<img_t> &&img, bool frame_captured) -> bool {
      if (m_stop_requested) {
        return false;  // Signal the sub-display to stop
      }

      if (frame_captured && img) {
        std::lock_guard<std::mutex> lock(state->frame_mutex);
        state->latest_frame = std::move(img);
        state->has_new_frame = true;

        // Notify the composite thread that a new frame is available
        m_composite_cv.notify_one();
      }

      return !m_stop_requested;
    };

    // Pool of images for the sub-display to use
    std::list<std::shared_ptr<img_t>> img_pool;
    std::mutex pool_mutex;

    auto pull_cb = [&state, &img_pool, &pool_mutex](std::shared_ptr<img_t> &img_out) -> bool {
      std::lock_guard<std::mutex> lock(pool_mutex);

      // Try to reuse an image from the pool
      for (auto it = img_pool.begin(); it != img_pool.end(); ++it) {
        if (it->use_count() == 1) {
          img_out = *it;
          return true;
        }
      }

      // Allocate a new image
      img_out = state->display->alloc_img();
      if (img_out) {
        img_pool.push_back(img_out);
      }
      return img_out != nullptr;
    };

    bool cursor = true;
    auto status = state->display->capture(push_cb, pull_cb, &cursor);
    state->capture_status = status;
    state->running = false;

    BOOST_LOG(debug) << "Composite sub-display " << index << " capture ended with status " << (int) status;
  }

  capture_e display_composite_t::capture(
    const push_captured_image_cb_t &push_captured_image_cb,
    const pull_free_image_cb_t &pull_free_image_cb,
    bool *cursor
  ) {
    m_stop_requested = false;

    // Start capture threads for each sub-display
    for (int i = 0; i < m_monitor_count; i++) {
      m_sub_displays[i]->capture_thread = std::thread(
        &display_composite_t::sub_display_capture_loop, this, i);
    }

    // Row stride for the combined image (4 bytes per pixel BGRA)
    const int bytes_per_pixel = 4;

    while (!m_stop_requested) {
      // Wait for any sub-display to produce a new frame
      {
        std::unique_lock<std::mutex> lock(m_composite_mutex);
        m_composite_cv.wait_for(lock, std::chrono::milliseconds(200), [this]() {
          if (m_stop_requested) return true;
          for (auto &state : m_sub_displays) {
            std::lock_guard<std::mutex> fl(state->frame_mutex);
            if (state->has_new_frame) return true;
          }
          return false;
        });
      }

      if (m_stop_requested) break;

      // Check if any sub-display capture has failed
      for (auto &state : m_sub_displays) {
        auto status = state->capture_status.load();
        if (status == capture_e::reinit || status == capture_e::error) {
          m_stop_requested = true;
          // Wait for all threads to finish
          for (auto &s : m_sub_displays) {
            s->running = false;
            if (s->capture_thread.joinable()) {
              s->capture_thread.join();
            }
          }
          return status;
        }
      }

      // Check if we have at least one frame from the primary display
      bool has_primary_frame = false;
      {
        std::lock_guard<std::mutex> lock(m_sub_displays[0]->frame_mutex);
        has_primary_frame = (m_sub_displays[0]->latest_frame != nullptr);
      }

      if (!has_primary_frame) {
        // No frame yet, send timeout
        if (!push_captured_image_cb(nullptr, false)) {
          break;
        }
        continue;
      }

      // Get a free image for the combined frame
      std::shared_ptr<img_t> combined_img;
      if (!pull_free_image_cb(combined_img)) {
        break;
      }

      if (!combined_img) {
        combined_img = alloc_img();
        if (!combined_img) {
          BOOST_LOG(error) << "Composite: failed to allocate combined image";
          break;
        }
      }

      // Composite: copy each sub-display's frame into the combined image
      std::optional<std::chrono::steady_clock::time_point> frame_ts;

      for (int i = 0; i < m_monitor_count; i++) {
        std::shared_ptr<img_t> sub_frame;
        {
          std::lock_guard<std::mutex> lock(m_sub_displays[i]->frame_mutex);
          sub_frame = m_sub_displays[i]->latest_frame;
          m_sub_displays[i]->has_new_frame = false;
        }

        if (!sub_frame || !sub_frame->data) {
          // No frame from this display yet; leave that region black
          continue;
        }

        // Use the primary display's timestamp
        if (i == 0 && sub_frame->frame_timestamp) {
          frame_ts = sub_frame->frame_timestamp;
        }

        // Copy the sub-frame into the appropriate horizontal region of the combined image
        int dst_x_offset = i * m_per_monitor_width * bytes_per_pixel;
        int src_row_bytes = sub_frame->row_pitch;
        int dst_row_bytes = combined_img->row_pitch;
        int copy_width_bytes = std::min(m_per_monitor_width * bytes_per_pixel, (int) sub_frame->row_pitch);
        int copy_height = std::min(m_per_monitor_height, (int) sub_frame->height);

        for (int row = 0; row < copy_height; row++) {
          std::uint8_t *dst = combined_img->data + row * dst_row_bytes + dst_x_offset;
          std::uint8_t *src = sub_frame->data + row * src_row_bytes;
          std::memcpy(dst, src, copy_width_bytes);
        }
      }

      combined_img->frame_timestamp = frame_ts;

      // Push the composite frame
      if (!push_captured_image_cb(std::move(combined_img), true)) {
        break;
      }
    }

    // Stop all sub-display capture threads
    m_stop_requested = true;
    for (auto &state : m_sub_displays) {
      state->running = false;
    }
    for (auto &state : m_sub_displays) {
      if (state->capture_thread.joinable()) {
        state->capture_thread.join();
      }
    }

    return capture_e::ok;
  }

  std::shared_ptr<img_t> display_composite_t::alloc_img() {
    // Allocate a system memory image at the combined resolution
    auto img = std::make_shared<composite_img_t>();
    img->width = m_combined_width;
    img->height = m_combined_height;
    img->pixel_pitch = 4;  // BGRA
    img->row_pitch = m_combined_width * 4;
    img->data = new std::uint8_t[img->row_pitch * img->height]();
    return img;
  }

  int display_composite_t::dummy_img(img_t *img) {
    if (!img) return -1;
    img->width = m_combined_width;
    img->height = m_combined_height;
    img->pixel_pitch = 4;
    img->row_pitch = m_combined_width * 4;
    if (!img->data) {
      img->data = new std::uint8_t[img->row_pitch * img->height]();
    }
    std::memset(img->data, 0, img->row_pitch * img->height);
    return 0;
  }

  std::unique_ptr<avcodec_encode_device_t> display_composite_t::make_avcodec_encode_device(pix_fmt_e pix_fmt) {
    if (m_primary_display) {
      return m_primary_display->make_avcodec_encode_device(pix_fmt);
    }
    return nullptr;
  }

  std::unique_ptr<nvenc_encode_device_t> display_composite_t::make_nvenc_encode_device(pix_fmt_e pix_fmt) {
    if (m_primary_display) {
      return m_primary_display->make_nvenc_encode_device(pix_fmt);
    }
    return nullptr;
  }

  bool display_composite_t::is_hdr() {
    if (m_primary_display) {
      return m_primary_display->is_hdr();
    }
    return false;
  }

  bool display_composite_t::get_hdr_metadata(SS_HDR_METADATA &metadata) {
    if (m_primary_display) {
      return m_primary_display->get_hdr_metadata(metadata);
    }
    return false;
  }

  bool display_composite_t::is_codec_supported(std::string_view name, const ::video::config_t &config) {
    if (m_primary_display) {
      return m_primary_display->is_codec_supported(name, config);
    }
    return true;
  }

}  // namespace platf::dxgi

namespace platf {

  std::shared_ptr<display_t> display_composite(
    mem_type_e hwdevice_type,
    const std::vector<std::string> &display_names_list,
    int per_monitor_width,
    int per_monitor_height,
    const video::config_t &config
  ) {
    if (display_names_list.empty()) {
      BOOST_LOG(error) << "display_composite: no display names provided";
      return nullptr;
    }

    // Map device IDs to display names
    std::vector<std::string> mapped_names;
    for (const auto &device_id : display_names_list) {
      auto mapped = display_device::map_output_name(device_id);
      mapped_names.push_back(mapped.empty() ? device_id : mapped);
    }

    auto composite = std::make_shared<dxgi::display_composite_t>();
    if (composite->init(config, mapped_names, per_monitor_width, per_monitor_height) != 0) {
      BOOST_LOG(error) << "display_composite: initialization failed";
      return nullptr;
    }

    return composite;
  }

}  // namespace platf
