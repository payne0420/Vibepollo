/**
 * @file globals.h
 * @brief Declarations for globally accessible variables and functions.
 */
#pragma once

// local includes
#include "entry_handler.h"
#include "thread_pool.h"

/**
 * @brief A thread pool for processing tasks.
 */
extern thread_pool_util::ThreadPool task_pool;

/**
 * @brief A boolean flag to indicate whether the cursor should be displayed.
 */
extern bool display_cursor;

#ifdef _WIN32
  // Declare global singleton used for NVIDIA control panel modifications
  #include "platform/windows/nvprefs/nvprefs_interface.h"

/**
 * @brief A global singleton used for NVIDIA control panel modifications.
 */
extern nvprefs::nvprefs_interface nvprefs_instance;
#endif

/**
 * @brief Handles process-wide communication.
 */
namespace mail {
#define MAIL(x) \
  constexpr auto x = std::string_view { \
    #x \
  }

  /**
   * @brief A process-wide communication mechanism.
   */
  extern safe::mail_t man;

  // Global mail
  MAIL(shutdown);
  MAIL(broadcast_shutdown);
  MAIL(video_packets);
  MAIL(audio_packets);
  MAIL(switch_display);

  /**
   * @brief Returns the queue name for video packets of a given stream index.
   * Stream 0 returns "video_packets" (backward compatible), stream N returns "video_packets_N".
   */
  inline std::string video_packets_name(int stream_index) {
    if (stream_index == 0) return std::string(video_packets);
    return std::string(video_packets) + "_" + std::to_string(stream_index);
  }

  // Local mail
  MAIL(touch_port);
  MAIL(idr);
  MAIL(invalidate_ref_frames);
  MAIL(gamepad_feedback);
  MAIL(hdr);
#undef MAIL

  /**
   * @brief Returns the event name for IDR requests of a given stream index.
   * Stream 0 returns "idr" (backward compatible), stream N returns "idr_N".
   */
  inline std::string idr_name(int stream_index) {
    if (stream_index == 0) return std::string(idr);
    return std::string(idr) + "_" + std::to_string(stream_index);
  }

  /**
   * @brief Returns the event name for reference frame invalidation of a given stream index.
   * Stream 0 returns "invalidate_ref_frames" (backward compatible), stream N returns "invalidate_ref_frames_N".
   */
  inline std::string invalidate_ref_frames_name(int stream_index) {
    if (stream_index == 0) return std::string(invalidate_ref_frames);
    return std::string(invalidate_ref_frames) + "_" + std::to_string(stream_index);
  }

}  // namespace mail
