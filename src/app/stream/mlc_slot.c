#include "mlc_slot.h"

#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"

#if FEATURE_SPLIT_SCREEN
#define MLC2_FN_DECL(name) extern __typeof__(name) mlc2_##name;
#define MLC2_DATA_DECL(type, name) extern type mlc2_##name;
MLC_FUNCTIONS(MLC2_FN_DECL)
MLC_DATA(MLC2_DATA_DECL)
#endif

#define MLC1_FN(name) .name = name,
#define MLC1_DATA(type, name) .name = &name,
#define MLC2_FN(name) .name = mlc2_##name,
#define MLC2_DATA(type, name) .name = &mlc2_##name,

const mlc_api_t mlc_slots[MLC_SLOTS] = {
        {MLC_FUNCTIONS(MLC1_FN) MLC_DATA(MLC1_DATA)},
#if FEATURE_SPLIT_SCREEN
        {MLC_FUNCTIONS(MLC2_FN) MLC_DATA(MLC2_DATA)},
#endif
};
