/**
 * @file src/platform/windows/display_composite.h
 * @brief Composite display that captures multiple DXGI outputs and stitches them side-by-side.
 */
#pragma once

#include "display.h"
#include "src/platform/common.h"
#include "src/video.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platf::dxgi {

  /**
   * @brief A composite display that captures N individual displays and composites
   * them into a single combined frame (arranged side-by-side horizontally).
   *
   * Each sub-display runs its own capture thread. The composite display collects
   * the latest frame from each sub-display and stitches them into a single
   * combined image in system memory.
   */
  class display_composite_t: public display_t {
  public:
    display_composite_t() = default;
    ~display_composite_t() override;

    int init(const ::video::config_t &config,
             const std::vector<std::string> &display_names,
             int per_monitor_width,
             int per_monitor_height);

    capture_e capture(const push_captured_image_cb_t &push_captured_image_cb,
                      const pull_free_image_cb_t &pull_free_image_cb,
                      bool *cursor) override;

    std::shared_ptr<img_t> alloc_img() override;
    int dummy_img(img_t *img) override;

    std::unique_ptr<avcodec_encode_device_t> make_avcodec_encode_device(pix_fmt_e pix_fmt) override;
    std::unique_ptr<nvenc_encode_device_t> make_nvenc_encode_device(pix_fmt_e pix_fmt) override;

    bool is_hdr() override;
    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override;
    bool is_codec_supported(std::string_view name, const ::video::config_t &config) override;

  private:
    /// Per-sub-display state: holds its latest captured frame.
    struct sub_display_state_t {
      std::shared_ptr<display_t> display;
      std::thread capture_thread;

      std::mutex frame_mutex;
      std::shared_ptr<img_t> latest_frame;
      bool has_new_frame = false;

      std::atomic<bool> running {false};
      std::atomic<capture_e> capture_status {capture_e::ok};
    };

    void sub_display_capture_loop(int index);

    std::vector<std::unique_ptr<sub_display_state_t>> m_sub_displays;
    std::shared_ptr<display_t> m_primary_display;

    int m_monitor_count = 0;
    int m_per_monitor_width = 0;
    int m_per_monitor_height = 0;
    int m_combined_width = 0;
    int m_combined_height = 0;

    // Signaling when any sub-display produces a new frame
    std::mutex m_composite_mutex;
    std::condition_variable m_composite_cv;
    std::atomic<bool> m_stop_requested {false};
  };

}  // namespace platf::dxgi
