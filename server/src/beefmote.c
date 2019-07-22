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
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <deadbeef/deadbeef.h>

#define DEBUG 1
#define BEEFMOTE_DEFAULT_PORT 49160
#define BEEFMOTE_BUFSIZE 1000
#define BEEFMOTE_WAIT_CLIENT 1

#define beefmote_debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "[beefmote] " fmt, ##__VA_ARGS__); } while (0)

typedef struct DB_beefmote_plugin_s {
    DB_misc_t misc;
} DB_beefmote_plugin_t;

// Globals.
static DB_functions_t *deadbeef;        // deadbeef's plugin API
static DB_beefmote_plugin_t beefmote_plugin;    // beefmote's plugin description
static uintptr_t beefmote_stopthread_mutex;
static intptr_t beefmote_tid;
static int beefmote_stopthread;
static int beefmote_socket;

// Beefmote's settings dialog widget description.
static const char beefmote_settings_dialog[] = {
    "property \"Disable\" checkbox beefmote.disable 0;"
        "property \"IP\" entry beefmote.ip \"\";\n" "property \"Port\" entry beefmote.port \"\";\n"
};

// Deadbeef's plugin load function. This is the first thing executed by Deadbeef on plugin load.
// It registers the plugin with Deadbeef and gives us access to the plugins API.
DB_plugin_t *beefmote_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN(&beefmote_plugin);
}

// Beefmote's thread function. This is where the magic happens.
static void beefmote_thread(void *data);

// Prepares Beefmote's socket for listening.
static void beefmote_listen();

// Beefmote's entry point. Second thing executed by Deadbeef on plugin load.
static int plugin_start()
{
    beefmote_stopthread = 0;
    beefmote_stopthread_mutex = deadbeef->mutex_create_nonrecursive();
    beefmote_listen();
    beefmote_tid = deadbeef->thread_start(beefmote_thread, NULL);

    return 0;
}

// Beefmote's exit point. Executed by Deadbeef on program exit (i.e. when the
// DB_EV_TERMINATE signal is sent to Deadbeef).
static int plugin_stop()
{
    if (beefmote_tid) {
        deadbeef->mutex_lock(beefmote_stopthread_mutex);
        beefmote_stopthread = 1;
        deadbeef->mutex_unlock(beefmote_stopthread_mutex);

        if (beefmote_socket != -1) {
            close(beefmote_socket);
        }

        deadbeef->thread_join(beefmote_tid);    // wait for Beefmote's thread to finish
        deadbeef->mutex_free(beefmote_stopthread_mutex);
    }

    return 0;
}

// Plugin description.
static DB_beefmote_plugin_t beefmote_plugin = {
    .misc.plugin.api_vmajor = 1,
    .misc.plugin.api_vminor = 10,       // need at least 1.10 for the metadata functions
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
static void track_print(DB_playItem_t *track)
{
    assert(track);

    const char *track_artist = deadbeef->pl_meta_for_key(track, "artist")->value;
    const char *track_album = deadbeef->pl_meta_for_key(track, "album")->value;
    const char *track_title = deadbeef->pl_meta_for_key(track, "title")->value;
    const char *track_tracknumber = deadbeef->pl_meta_for_key(track, "track")->value;
    char track_length[100];
    float len = deadbeef->pl_get_item_duration(track);

    deadbeef->pl_format_time(len, track_length, 100);

    printf("[%s - %s] %s - %s (%s)\n", track_artist, track_album, track_tracknumber, track_title,
           track_length);
}

// Prints all tracks of a playlist using track_print. Returns number of tracks printed.
int playlist_print_tracks(ddb_playlist_t *playlist)
{
    assert(playlist);

    int i = 0;
    DB_playItem_t *track;

    while (track = deadbeef->plt_get_item_for_idx(playlist, i++, PL_MAIN)) {
        track_print(track);
        deadbeef->pl_item_unref(track);
    }

    return --i;
}

static void beefmote_thread(void *data)
{
    static bool tracks_shown = false;
    char beefmote_buffer[BEEFMOTE_BUFSIZE];

    memset(beefmote_buffer, 0, BEEFMOTE_BUFSIZE);

    int client_socket;
    struct sockaddr_in client_addr;
    int client_size = sizeof(client_addr);

    fd_set readfds;
    struct timeval timeout;

    char welcome_str[] = "Hello! Welcome to Beefmote's server. " \
                         "Type \"help\" for a list of available commands\n";
    int welcome_len = strlen(welcome_str);

    // Infinite loop. We only exit when Deadbeef calls the
    // plugin_stop function on program exit.
    for (;;) {
        deadbeef->mutex_lock(beefmote_stopthread_mutex);
        if (beefmote_stopthread == 1) {
            deadbeef->mutex_unlock(beefmote_stopthread_mutex);
            return;
        }
        deadbeef->mutex_unlock(beefmote_stopthread_mutex);

        if (beefmote_socket == -1) {
            continue;
        }

        // Accept (non-blocking) client connection.
        if ((client_socket = accept(beefmote_socket, (struct sockaddr *) &client_addr,
                                    &client_size)) < 0) {
            sleep(BEEFMOTE_WAIT_CLIENT);        // let's not kill the CPU, shall we?
            continue;
        }

        beefmote_debug_print("got connection from %s\n", inet_ntoa(client_addr.sin_addr));

        int bytes_n = write(client_socket, welcome_str, welcome_len);

        if (bytes_n != welcome_len) {
            beefmote_debug_print("error: failure while sending welcome message to client %s\n",
                                 inet_ntoa(client_addr.sin_addr));
        }

        // At this point, a client is connected. We now just have to wait for it
        // to say something to us. We check every BEEFMOTE_WAIT_CLIENT seconds to
        // see if the client said something.
        for (;;) {
            deadbeef->mutex_lock(beefmote_stopthread_mutex);
            if (beefmote_stopthread == 1) {
                deadbeef->mutex_unlock(beefmote_stopthread_mutex);
                close(client_socket);
                return;
            }
            deadbeef->mutex_unlock(beefmote_stopthread_mutex);

            // All this stuff has to be set *every* time before a select(),
            // because select() modifies it.
            FD_ZERO(&readfds);
            FD_SET(client_socket, &readfds);
            timeout.tv_sec = BEEFMOTE_WAIT_CLIENT;
            timeout.tv_usec = 0;
            int rv = select(client_socket + 1, &readfds, NULL, NULL, &timeout);

            if (rv == -1) {
                beefmote_debug_print("error: select failed\n");
                close(client_socket);
                break;
            } else if (rv == 0) {
                continue;
            } else {
                if (FD_ISSET(client_socket, &readfds)) {
                    // Read and print data from client.
                    int bytes_n = read(client_socket, beefmote_buffer, BEEFMOTE_BUFSIZE);

                    if (bytes_n < 0) {
                        beefmote_debug_print("error: failed on read()\n");
                        close(client_socket);
                        break;
                    }

                    if (bytes_n == 0) {
                        beefmote_debug_print("client %s closed connection\n",
                                             inet_ntoa(client_addr.sin_addr));
                        close(client_socket);
                        break;
                    }

                    beefmote_debug_print("received %d bytes from client %s: %s", bytes_n,
                                         inet_ntoa(client_addr.sin_addr), beefmote_buffer);
                    memset(beefmote_buffer, 0, BEEFMOTE_BUFSIZE);
                }
            }
        }
    }
}

static void beefmote_listen()
{
    // Try to get IP and port from settings.
    deadbeef->conf_lock();
    const char *ip_str = deadbeef->conf_get_str_fast("beefmote.ip", "");
    const char *port_str = deadbeef->conf_get_str_fast("beefmote.port", "");
    bool config_ip_found = false;
    bool config_port_found = false;
    struct sockaddr_in servaddr;

    memset(&servaddr, 0, sizeof(servaddr));

    // IP found in config file.
    if (strcmp(ip_str, "")) {
        beefmote_debug_print("IP found in config file: %s\n", ip_str);

        config_ip_found = true;
        inet_pton(AF_INET, ip_str, &(servaddr));

        // Debug: Print converted IP (it should match ip_str)
        if (DEBUG) {
            char str[INET_ADDRSTRLEN];

            inet_ntop(AF_INET, &(servaddr), str, INET_ADDRSTRLEN);
            beefmote_debug_print("Converted IP: %s\n", str);
        }
    }

    // Port found in config file.
    if (strcmp(port_str, "")) {
        beefmote_debug_print("Port found in config file: %s\n", port_str);

        config_port_found = true;
        int port = strtol(port_str, NULL, 10);

        servaddr.sin_port = htons(port);

        if (DEBUG) {
            beefmote_debug_print("Converted port: %d\n", port);
        }
    }

    deadbeef->conf_unlock();

    beefmote_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (beefmote_socket == -1) {
        beefmote_debug_print("error: couldn't create socket\n");
        return;
    }

    // Reuse address (useful if the user closes and opens the program quickly again).
    int enabled = 1;
    if (setsockopt(beefmote_socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1) {
        beefmote_debug_print("error: couldn't set SO_REUSEADDR\n");
    }

    // Set IP and port if they weren't found in the config file.
    servaddr.sin_family = AF_INET;

    if (!config_ip_found) {
        beefmote_debug_print("IP not found in config file, defaulting to all interfaces\n");
        servaddr.sin_addr.s_addr = htonl(INADDR_ANY);   // bind to all interfaces
    }

    if (!config_port_found) {
        beefmote_debug_print("port not found in config file, defaulting to %d\n",
                             BEEFMOTE_DEFAULT_PORT);
        servaddr.sin_port = htons(BEEFMOTE_DEFAULT_PORT);
    }

    // Set socket to non-blocking mode.
    fcntl(beefmote_socket, F_SETFL, O_NONBLOCK | fcntl(beefmote_socket, F_GETFL, 0));

    // Bind socket.
    if (bind(beefmote_socket, (struct sockaddr *) &servaddr, sizeof(servaddr))) {
        beefmote_debug_print("error: couldn't bind socket\n");
        close(beefmote_socket);
        beefmote_socket = -1;
        return;
    }

    // Put socket to listen.
    if (listen(beefmote_socket, 1)) {
        beefmote_debug_print("error: couldn't put socket to listen\n");
        close(beefmote_socket);
        beefmote_socket = -1;
        return;
    }
}
