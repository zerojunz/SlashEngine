// miniaudio's decoders, built apart from the engine so its warnings flags don't apply. Vorbis
// decoding comes from stb_vorbis in miniaudio's extras: its declarations go before miniaudio's
// implementation, which then decodes Ogg Vorbis with it, and its implementation after.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include "miniaudio.c"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
