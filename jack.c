/*
    JACK output plugin for DeaDBeeF
    Copyright (C) 2010 Steven McDonald <steven.mcdonald@libremail.me>
    Based on and includes parts from OSS output plugin by Alexey Yakovenko

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _GNU_SOURCE
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#define JACK_CLIENT_NAME "deadbeef"
#define CHANNELS 2

#include <unistd.h>
#include <jack/jack.h>
#include <deadbeef/deadbeef.h>
#include <signal.h>

//#define trace(...) { fprintf(stderr, __VA_ARGS__); }
#define trace(fmt,...)

static DB_output_t plugin;
DB_functions_t *deadbeef;

static jack_client_t *ch; // client handle
static jack_status_t jack_status;
static jack_port_t *jack_ports[CHANNELS];
static unsigned short state;
static short errcode; // used to store error codes
static short jack_connected = 0;
static short DidWeStartJack = 0;
static int rate;

static int
jack_stop (void);

static int
jack_init (void);

static int
jack_proc_callback (jack_nframes_t nframes, void *arg) {
    trace ("jack_proc_callback\n");
    if (!jack_connected) return -1;

    // FIXME: This function copies from the streamer to a local buffer,
    //        and then to JACK's buffer. This is wasteful. Since JACK
    //        uses floating point samples and we are using integers,
    //        this may not be fixable without rewriting the streamer.

    switch (state) {
        case OUTPUT_STATE_PLAYING: {
            char buf[nframes * CHANNELS * 2];
            unsigned bytesread = deadbeef->streamer_read (buf, sizeof(buf));

            // this avoids a crash if we are playing and change to a plugin
            // with no valid output and then switch back
            if (bytesread == -1) {
                state = OUTPUT_STATE_STOPPED;
                return 0;
            }

            float *jack_port_buffer[CHANNELS];
            for (unsigned short i = 0; i < CHANNELS; i++) {
                jack_port_buffer[i] = jack_port_get_buffer(jack_ports[i], nframes);
            }

            float vol = deadbeef->volume_get_amp ();

            for (unsigned i = 0; i < bytesread/(CHANNELS*2); i++) {
                for (unsigned short j = 0; j < CHANNELS; j++) {
                    // JACK expects floating point samples, so we need to convert from integer
                    *jack_port_buffer[j]++ = (float)(((int16_t*)buf)[(CHANNELS*i) + j]) * vol / 32768;
                }
            }

            return 0;
        }

        // this is necessary to stop JACK going berserk when we pause/stop
        default: {
            float *jack_port_buffer[CHANNELS];
            for (unsigned short i = 0; i < CHANNELS; i++) {
                jack_port_buffer[i] = jack_port_get_buffer(jack_ports[i], nframes);
            }

            for (unsigned i = 0; i < nframes; i++) {
                for (unsigned short j = 0; j < CHANNELS; j++) {
                    *jack_port_buffer[j]++ = 0;
                }
            }

            return 0;
        }
    }
}

static int
jack_rate_callback (void *arg) {
    if (!jack_connected) return -1;
    rate = (int)jack_get_sample_rate(ch);
}

static int
jack_shutdown_callback (void *arg) {
    if (!jack_connected) return -1;
    jack_connected = 0;
    // if JACK crashes or is shut down, start a new server instance
    if (deadbeef->conf_get_int ("jack.autorestart", 0)) {
        fprintf (stderr, "jack: JACK server shut down unexpectedly, restarting...\n");
        sleep (1);
        jack_init ();
    }
    else {
        fprintf (stderr, "jack: JACK server shut down unexpectedly, stopping playback\n");
        deadbeef->playback_stop ();
    }
    return 0;
}

static int
jack_init (void) {
    trace ("jack_init\n");
    jack_connected = 1;

    // create new client on JACK server
    if ((ch = jack_client_open (JACK_CLIENT_NAME, JackNullOption | (JackNoStartServer && !deadbeef->conf_get_int ("jack.autostart", 1)), &jack_status)) == 0) {
        fprintf (stderr, "jack: could not connect to JACK server\n");
        plugin.free();
        return -1;
    }

    rate = (int)jack_get_sample_rate(ch);

    // Did we start JACK, or was it already running?
    if (jack_status & JackServerStarted)
        DidWeStartJack = 1;
    else
        DidWeStartJack = 0;

    // set process callback
    if ((errcode = jack_set_process_callback(ch, &jack_proc_callback, NULL)) != 0) {
        fprintf (stderr, "jack: could not set process callback, error %d\n", errcode);
        plugin.free();
        return -1;
    }

    // set sample rate callback
    if ((errcode = jack_set_sample_rate_callback(ch, (JackSampleRateCallback)&jack_rate_callback, NULL)) != 0) {
        fprintf (stderr, "jack: could not set sample rate callback, error %d\n", errcode);
        plugin.free();
        return -1;
    }

    // set shutdown callback
    jack_on_shutdown (ch, (JackShutdownCallback)&jack_shutdown_callback, NULL);

    // register ports
    for (unsigned short i=0; i < CHANNELS; i++) {
        char port_name[11];

        // i+1 used to adhere to JACK convention of counting ports from 1, not 0
        sprintf (port_name, "deadbeef_%d", i+1);

        if (!(jack_ports[i] = jack_port_register(ch, (const char*)&port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput|JackPortIsTerminal, 0))) {
            fprintf (stderr, "jack: could not register port number %d\n", i+1);
            plugin.free();
            return -1;
        }
    }

    // tell JACK we are ready to roll
    if ((errcode = jack_activate(ch)) != 0) {
        fprintf (stderr, "jack: could not activate client, error %d\n", errcode);
        plugin.free();
        return -1;
    }

    // connect ports to hardware output
    if (deadbeef->conf_get_int ("jack.autoconnect", 1)) {
        const char **playback_ports;

        if (!(playback_ports = jack_get_ports (ch, 0, 0, JackPortIsPhysical|JackPortIsInput))) {
            fprintf (stderr, "jack: warning: could not find any playback ports to connect to\n");
        }
        else {
            for (unsigned short i=0; i < CHANNELS; i++) {
                // error code 17 means port connection exists. We do not want to return an error in this case, simply proceed.
                if ((errcode = jack_connect(ch, jack_port_name (jack_ports[i]), playback_ports[i])) && (errcode != 17)) {
                    fprintf (stderr, "jack: could not create connection from %s to %s, error %d\n", jack_port_name (jack_ports[i]), playback_ports[i], errcode);
                    plugin.free();
                    return -1;
                }
            }
        }
    }

    return 0;
}

static int
jack_get_rate (void) {
    trace ("jack_get_rate\n");
    return rate;
}

static int
jack_change_rate (int rate) {
    // FIXME: If (and ONLY IF) we started JACK (i.e. DidWeStartJack == TRUE),
    //        allow this to work by stopping and restarting JACK.
    return jack_get_rate();
}

static int
jack_play (void) {
    trace ("jack_play\n");
    if (!jack_connected) {
        if (jack_init() != 0) {
            trace("jack_init failed\n");
            plugin.free();
            return -1;
        }
    }
    state = OUTPUT_STATE_PLAYING;
    return 0;
}

static int
jack_stop (void) {
    trace ("jack_stop\n");
    state = OUTPUT_STATE_STOPPED;
    deadbeef->streamer_reset (1);
    return 0;
}

static int
jack_pause (void) {
    trace ("jack_pause\n");
    if (state == OUTPUT_STATE_STOPPED) {
        return -1;
    }
    // set pause state
    state = OUTPUT_STATE_PAUSED;
    return 0;
}

static int
jack_plugin_start (void) {
    trace ("jack_plugin_start\n");
    sigset_t set;
    sigemptyset (&set);
    sigaddset (&set, SIGPIPE);
    sigprocmask (SIG_BLOCK, &set, 0);
    return 0;
}

static int
jack_plugin_stop (void) {
    trace ("jack_plugin_stop\n");
    return 0;
}

static int
jack_unpause (void) {
    trace ("jack_unpause\n");
    jack_play ();
    return 0;
}

static int
jack_get_bps (void) {
    trace ("jack_get_bps\n");
    // as far as JACK is concerned this isn't that important
    // we convert to float anyway (see jack_proc_callback)
    return 16;
}

static int
jack_get_channels (void) {
    trace ("jack_get_channels\n");
    return CHANNELS;
}

static int
jack_get_state (void) {
    trace ("jack_get_state\n");
    return state;
}

static int
jack_get_endianness (void) {
#if WORDS_BIGENDIAN
    return 1;
#else
    return 0;
#endif
}

static int
jack_free_deadbeef (void) {
    trace ("jack_free_deadbeef\n");
    jack_connected = 0;

    // stop playback if we didn't start jack
    // this prevents problems with not disconnecting gracefully
    if (!DidWeStartJack) {
        jack_stop ();
        sleep (1);
    }

    if (ch) {
        if (jack_client_close (ch)) {
            fprintf (stderr, "jack: could not disconnect from JACK server\n");
            return -1;
        }
        ch = NULL;
    }

    // sleeping here is necessary to give JACK time to disconnect from the backend
    // if we are switching to another backend, it will fail without this
    if (DidWeStartJack)
        sleep (1);
    return 0;
}

DB_plugin_t *
jack_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}

static const char settings_dlg[] =
    "property \"Start JACK server automatically, if not already running\" checkbox jack.autostart 1;\n"
    "property \"Automatically connect to system playback ports\" checkbox jack.autoconnect 1;\n"
    "property \"Automatically restart JACK server if shut down\" checkbox jack.autorestart 0;\n"
;

// define plugin interface
static DB_output_t plugin = {
    DB_PLUGIN_SET_API_VERSION
    .plugin.version_major = 0,
    .plugin.version_minor = 1,
    .plugin.nostop = 0,
    .plugin.type = DB_PLUGIN_OUTPUT,
    .plugin.id = "jack",
    .plugin.name = "JACK output plugin",
    .plugin.descr = "plays sound via JACK API",
    .plugin.author = "Steven McDonald",
    .plugin.email = "steven.mcdonald@libremail.me",
    .plugin.website = "http://deadbeef.sf.net",
    .plugin.start = jack_plugin_start,
    .plugin.stop = jack_plugin_stop,
    .plugin.configdialog = settings_dlg,
    .init = jack_init,
    .free = jack_free_deadbeef,
    .change_rate = jack_change_rate,
    .play = jack_play,
    .stop = jack_stop,
    .pause = jack_pause,
    .unpause = jack_unpause,
    .state = jack_get_state,
    .samplerate = jack_get_rate,
    .bitspersample = jack_get_bps,
    .channels = jack_get_channels,
    .endianness = jack_get_endianness,
};
