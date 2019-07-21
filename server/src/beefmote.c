/*  
    Copyright (C) 2019 Laureano G. Vaioli <laureano3400@gmail.com>
   
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <deadbeef/deadbeef.h>

#define DEBUG 1

#define beefmote_debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "[beefmote] " fmt, ##__VA_ARGS__); } while (0)

typedef struct DB_beefmote_plugin_s
{
    DB_misc_t misc;
} DB_beefmote_plugin_t;

// Globals.
static DB_functions_t* deadbeef; // deadbeef's plugin API
static DB_beefmote_plugin_t beefmote_plugin; // beefmote's plugin description
static uintptr_t beefmote_stopthread_mutex;
static intptr_t beefmote_tid;
static int beefmote_stopthread;
static int beefmote_socket;

// Beefmote's settings dialog widget description.
static const char beefmote_settings_dialog[] =
{
    "property \"Enabled\" checkbox beefmote.enable 0;"
    "property \"IP\" entry beefmote.ip \"\";\n"
    "property \"Port\" entry beefmote.port \"\";\n"
};

// Deadbeef's plugin load function. This is the first thing executed by Deadbeef on plugin load.
// It registers the plugin with Deadbeef and gives us access to the plugins API.
DB_plugin_t* beefmote_load(DB_functions_t* api)
{
    deadbeef = api;
    return DB_PLUGIN(&beefmote_plugin);
}

// Beefmote's thread function. This is where the magic happens.
static void beefmote_thread(void* data);

// Beefmote's entry point. Second thing executed by Deadbeef on plugin load.
static int plugin_start()
{
    beefmote_stopthread = 0;
    beefmote_stopthread_mutex = deadbeef->mutex_create_nonrecursive();
    beefmote_socket = -1; // FIXME: implement socket
    beefmote_tid = deadbeef->thread_start(beefmote_thread, NULL);

    return 0;
}

// Beefmote's exit point. Executed by Deadbeef on program exit (i.e. when the
// DB_EV_TERMINATE signal is sent to Deadbeef).
static int plugin_stop()
{
    if(beefmote_tid)
    {
        deadbeef->mutex_lock(beefmote_stopthread_mutex);
	beefmote_stopthread = 1;
        deadbeef->mutex_unlock(beefmote_stopthread_mutex);

	deadbeef->thread_join(beefmote_tid); // wait for Beefmote's thread to finish
	deadbeef->mutex_free(beefmote_stopthread_mutex);
        if(beefmote_socket != -1)
        {
	    close(beefmote_socket);
        }
    }

    return 0;
}

// Plugin description.
static DB_beefmote_plugin_t beefmote_plugin =
{
    .misc.plugin.api_vmajor = 1,
    .misc.plugin.api_vminor = 10, // need at least 1.10 for the metadata functions
    .misc.plugin.version_major = 0,
    .misc.plugin.version_minor = 1,
    .misc.plugin.type = DB_PLUGIN_MISC,
    .misc.plugin.id = "beefmote",
    .misc.plugin.name = "Beefmote",
    .misc.plugin.descr = "Beefmote: An Android DeaDBeeF remote",
    .misc.plugin.copyright =
        "Copyright (C) 2019 Laureano G. Vaioli <laureano3400@gmail.com>\n"
        "\n"
        "This program is free software: you can redistribute it and/or modify\n"
        "it under the terms of the GNU General Public License as published by\n"
        "the Free Software Foundation, either version 3 of the License, or\n"
        "(at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the\n"
        "GNU General Public License for more details.\n"
        "\n"

        "You should have received a copy of the GNU General Public License\n"
        "along with this program. If not, see <https://www.gnu.org/licenses/>.\n",
    .misc.plugin.website = "https://github.com/lgvaioli/beefmote",
    .misc.plugin.start = plugin_start,
    .misc.plugin.stop = plugin_stop,
    .misc.plugin.configdialog = beefmote_settings_dialog,
};

  ///////////////////////////////////
 // End of Deadbeef's boilerplate //
///////////////////////////////////

// Prints a track in the format "[Tool - Lateralus] 05 - Schism (6:48)".
static void track_print(DB_playItem_t* track)
{
    assert(track);

    const char* track_artist = deadbeef->pl_meta_for_key(track, "artist")->value;
    const char* track_album = deadbeef->pl_meta_for_key(track, "album")->value;
    const char* track_title = deadbeef->pl_meta_for_key(track, "title")->value;
    const char* track_tracknumber = deadbeef->pl_meta_for_key(track, "track")->value;
    char track_length[100];
    float len = deadbeef->pl_get_item_duration(track);
    deadbeef->pl_format_time(len, track_length, 100);

    printf("[%s - %s] %s - %s (%s)\n", track_artist, track_album, track_tracknumber, track_title, track_length);
}

// Prints all tracks of a playlist using track_print. Returns number of tracks printed.
int playlist_print_tracks(ddb_playlist_t* playlist)
{
    assert(playlist);

    int i = 0;
    DB_playItem_t* track;

    while(track = deadbeef->plt_get_item_for_idx(playlist, i++, PL_MAIN))
    {
        track_print(track);
        deadbeef->pl_item_unref(track);
    }

    return --i;
}

static void beefmote_thread(void* data)
{
    static bool tracks_shown = false;

    // Infinite loop. We only exit when Deadbeef calls the
    // plugin_stop function on program exit.
    for(;;)
    {
        // If plugin_stop was executed, get out of loop.
        deadbeef->mutex_lock(beefmote_stopthread_mutex);
        if(beefmote_stopthread == 1)
        {
            deadbeef->mutex_unlock(beefmote_stopthread_mutex);
            return;
        }
        deadbeef->mutex_unlock(beefmote_stopthread_mutex);

        // For now, we just display all the tracks in the current playlist.
        ddb_playlist_t* pl_curr = deadbeef->plt_get_curr();

        if(pl_curr)
        {
            if(!tracks_shown)
            {
                int tracks_num = playlist_print_tracks(pl_curr);

                if(tracks_num)
                {
                    tracks_shown = true;
                    beefmote_debug_print("Printed %d tracks\n", tracks_num);
                }
            }

            deadbeef->plt_unref(pl_curr);
        }
    }
}

