#include "audio_anchor.h"

static_assert(audio_anchor_project_rtp(1000, 1000000000ULL, 1000000000ULL, 44100)
              == 1000, "Repeated historical anchor must not advance with arrival time");
static_assert(audio_anchor_project_rtp(1000, 1000000000ULL, 34000000000ULL, 44100)
              == 1456300, "Delayed control packets must use their sender timeline");
static_assert(audio_anchor_project_rtp(1456300, 34000000000ULL, 1000000000ULL, 44100)
              == 1000, "Historical updates project backwards on the same timeline");
static_assert(audio_anchor_project_rtp(0xfffffff0U, 1000000000ULL, 2000000000ULL, 44100)
              == 44084, "RTP wraparound must be preserved");
static_assert(audio_anchor_project_rtp(1000, 1000000000ULL, 1500000000ULL, 48000)
              == 25000, "Fractional seconds must preserve the negotiated rate");
static_assert((int32_t)(500U - audio_anchor_project_rtp(
                  1000, 1000000000ULL, 11000000000ULL, 44100)) < -5 * 44100,
              "Real backwards seeks must remain detectable");
static_assert((int32_t)(1000000U - audio_anchor_project_rtp(
                  1000, 1000000000ULL, 2000000000ULL, 44100)) > 5 * 44100,
              "Real forwards seeks must remain detectable");
