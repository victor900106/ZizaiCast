/* PM 0.7.10.1: A/B knobs for the iOS volume HUD that pops up while
 * mirroring (owner, 0.7.10: opening the Google app shows the iPhone volume
 * bar, sometimes the keyboard stops typing, with no AirPlay request reaching
 * the receiver). Hypothesis: iOS treats this PC as an AirPlay audio output
 * with a remotely adjustable volume, so an app that touches its audio
 * session re-routes / re-shows the receiver volume. Both knobs only change
 * what GET /info and the mDNS TXT records claim; the mirroring sound path
 * (SETUP stream type 96, usingScreen=1) is the same in every mode.
 *
 * settings.ini airplay_volume_control (Options::volumeControlType)
 *   -1  (default) no "volumeControlType" key in GET /info: as UxPlay and
 *       0.7.10 (iOS then assumes an adjustable AirPlay volume)
 *   0..4 sent as GET /info "volumeControlType". shairport-sync (AirPlay 2)
 *       sends 3. The values are not documented; the guess is HomeKit's
 *       VolumeControlType enum: 0 none (fixed volume), 1 relative (up/down
 *       only, like an Apple TV on HDMI-CEC), 2 relative with current,
 *       3 absolute (a slider). Anything else means -1.
 *
 * settings.ini airplay_audio_status (Options::audioStatusFlag)
 *   1  (default) statusFlags bit 2 ("audio cable attached", seen on Apple TV,
 *      HomePod, AVRs) on: GET /info statusFlags 68, _airplay TXT flags=0x4,
 *      _raop TXT sf=0x4 (0x8c / 0x84 with a PIN): as UxPlay and 0.7.10
 *   0  bit 2 off: statusFlags 64, flags=0x0, sf=0x0 (0x88 / 0x80 with a PIN).
 *      Riskiest knob: iOS may then not send the mirroring sound at all.
 *   Anything else means 1.
 * GPL-3.0-or-later. */
#ifndef PM_VOLUME_HUD_H
#define PM_VOLUME_HUD_H

#define PM_STATUS_FLAGS_UXPLAY 68u      /* GET /info statusFlags in UxPlay: bits 2 and 6 */
#define PM_STATUS_AUDIO_CABLE  0x4u     /* bit 2 */

typedef struct {
    int volume_control_type;   /* -1: no key in GET /info; else 0..4 */
    int audio_status;          /* 1: bit 2 on (default), 0: off */
    unsigned status_flags;     /* GET /info statusFlags */
} pm_volume_hud_t;

static inline pm_volume_hud_t pm_volume_hud_plan(int volume_control_type, int audio_status) {
    pm_volume_hud_t p;
    p.volume_control_type = (volume_control_type >= 0 && volume_control_type <= 4) ? volume_control_type : -1;
    p.audio_status = audio_status == 0 ? 0 : 1;
    p.status_flags = p.audio_status ? PM_STATUS_FLAGS_UXPLAY : (PM_STATUS_FLAGS_UXPLAY & ~PM_STATUS_AUDIO_CABLE);
    return p;
}

/* TXT status flags: _airplay "flags" and _raop "sf" (pin_pw 0: none, 1: PIN, 2/3: password) */
static inline unsigned pm_volume_hud_txt_flags(int audio_status, int pin_pw) {
    unsigned f = pin_pw == 1 ? 0x8cu : (pin_pw == 2 || pin_pw == 3) ? 0x84u : 0x4u;
    return audio_status ? f : (f & ~PM_STATUS_AUDIO_CABLE);
}

#endif
