#ifndef MODE_CONTROLLER_H
#define MODE_CONTROLLER_H

#include "vapix_client.h"
#include <pipewire/pipewire.h>

/**
 * Mode Controller
 *
 * Manages two operating modes by muting/unmuting audio outputs:
 *
 *   instructor  — C6110 OUT1 (headphone): MUTED
 *                 C6110 OUT2 (speaker):   UNMUTED
 *                 Remote speaker output:  MUTED
 *
 *   classroom   — C6110 OUT1 (headphone): UNMUTED
 *                 C6110 OUT2 (speaker):   MUTED
 *                 Remote speaker output:  UNMUTED
 *
 * Mode is read from the ACAP parameter "ActiveMode" and applied within ~1 second
 * of any change. Set the parameter via VAPIX:
 *   param.cgi?action=update&root.audio_control.ActiveMode=classroom
 *
 * VAPIX mute calls run in a short-lived thread so the PipeWire event loop is
 * never blocked during network I/O.
 */

struct mode_controller_config {
    const char *remote_ip;
    const char *remote_user;
    const char *remote_pass;
    int dual_audio; /* keep C6110 speaker active in classroom mode */
};

struct mode_controller {
    struct mode_controller_config config;
    struct vapix_client *vapix;
    struct pw_main_loop *pw_loop;
    struct spa_source *poll_timer;
    char current_mode[32]; /* last applied mode */
};

int  mode_controller_init(struct mode_controller *mc,
                           struct mode_controller_config *config,
                           struct vapix_client *vapix,
                           struct pw_main_loop *pw_loop);

void mode_controller_cleanup(struct mode_controller *mc);

#endif /* MODE_CONTROLLER_H */
