/* PM: A/B switch for "audio-only AirPlay" advertising (settings.ini
 * airplay_advertise_audio, Options::advertiseAudio).
 *
 *   1  (default, = UxPlay) features bit 9 "Audio" on, _raop._tcp registered
 *   0  bit 9 off and no _raop._tcp: the PC is offered for Screen Mirroring
 *      only, not as an AirPlay speaker (zinw/airplay-tv PR #29 does the same)
 *   2  bit 9 on, no _raop._tcp (fallback if 0 turned out to lose the
 *      mirroring sound)
 *
 * Mirroring audio itself (SETUP stream type 96 with usingScreen=1 on the
 * _airplay._tcp connection) is handled the same way in every mode: nothing
 * in the receiver looks at bit 9 or at the _raop._tcp registration.
 * Any other value means 1. GPL-3.0-or-later. */
#ifndef PM_AUDIO_ADVERT_H
#define PM_AUDIO_ADVERT_H

typedef struct {
    int mode;           /* normalised: 0, 1 or 2 */
    int feature_bit9;   /* advertise features bit 9 ("Audio") */
    int raop_service;   /* register _raop._tcp */
} pm_audio_advert_t;

static inline pm_audio_advert_t pm_audio_advert_plan(int mode) {
    pm_audio_advert_t p;
    if (mode != 0 && mode != 2) mode = 1;
    p.mode = mode;
    p.feature_bit9 = mode != 0;
    p.raop_service = mode == 1;
    return p;
}

#endif
