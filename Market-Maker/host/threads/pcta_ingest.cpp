// MMX — host/threads/pcta_ingest.cpp

#include "pcta_ingest.h"

namespace mmx {

uint8_t parse_regime_name(std::string_view s) {
    if (s == "ranging")        return kRegimeRanging;
    if (s == "trend_up")       return kRegimeTrendUp;
    if (s == "trend_down")     return kRegimeTrendDown;
    if (s == "high_vol")       return kRegimeHighVol;
    if (s == "event_imminent") return kRegimeEventImminent;
    return kRegimeRanging;
}

}  // namespace mmx
