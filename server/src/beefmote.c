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

#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
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
#define BEEFMOTE_STR_MAXLENGTH 1000
#define BEEFMOTE_VOLUME_STEP 5
#define BEEFMOTE_SEEK_STEP 5

#define beefmote_debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "[beefmote] " fmt, ##__VA_ARGS__); } while (0)

typedef struct DB_beefmote_plugin_s {
    DB_misc_t misc;
} DB_beefmote_plugin_t;

enum BEEFMOTE_COMMANDS {
    BEEFMOTE_HELP,
    BEEFMOTE_PLAYLISTS,
    BEEFMOTE_TRACKLIST,
    BEEFMOTE_TRACKLIST_ADDRESS,
    BEEFMOTE_TRACKCURR,
    BEEFMOTE_PLAY,
    BEEFMOTE_PLAY_SEARCH,
    BEEFMOTE_PLAY_ADDRESS,
    BEEFMOTE_RANDOM,
    BEEFMOTE_PLAY_RESUME,
    BEEFMOTE_STOP_AFTER_CURRENT,
    BEEFMOTE_STOP,
    BEEFMOTE_PREVIOUS,
    BEEFMOTE_NEXT,
    BEEFMOTE_VOLUME_UP,
    BEEFMOTE_VOLUME_DOWN,
    BEEFMOTE_SEEK_FORWARD,
    BEEFMOTE_SEEK_BACKWARD,
    BEEFMOTE_SEARCH,
    BEEFMOTE_NOTIFY_PLAYLISTCHANGED,
    BEEFMOTE_NOTIFY_NOW_PLAYING,
    BEEFMOTE_ADD_SEARCH_PLAYBACKQUEUE,
    BEEFMOTE_EXIT,
    BEEFMOTE_COMMANDS_N // marks end of command list
};

typedef struct beefmote_command {
    char name[BEEFMOTE_STR_MAXLENGTH];
    int name_len;
    char help[BEEFMOTE_STR_MAXLENGTH];
    void (*execute)(int client_socket, void* data);
} beefmote_command;

// Globals.
static DB_functions_t *deadbeef;        // deadbeef's plugin API
static DB_beefmote_plugin_t beefmote_plugin;    // beefmote's plugin description
static uintptr_t beefmote_stopthread_mutex;
static intptr_t beefmote_tid;
static int beefmote_stopthread;
static int beefmote_socket;
static int beefmote_client_socket;
static beefmote_command beefmote_commands[BEEFMOTE_COMMANDS_N];
static DB_playItem_t* beefmote_currtrack;
static bool beefmote_notify_playlistchanged;
static bool beefmote_notify_nowplaying;

// Beefmote's settings dialog widget description.
static const char beefmote_settings_dialog[] = {
    "property \"Disable\" checkbox beefmote.disable 0;" \
    "property \"IP\" entry beefmote.ip \"\";\n" \
    "property \"Port\" entry beefmote.port \"\";\n"
};


  ///////////////////////////////
 // Beefmote's main functions //
///////////////////////////////

// Beefmote's thread function. This is where the magic happens.
static void beefmote_thread(void *data);

// Prepares Beefmote's socket for listening.
static void beefmote_listen();

// Initializes Beefmote's commands.
static void beefmote_initialize_commands();

// Processes a Beefmote command.
static void beefmote_process_command(int client_socket, char *command);

// Beefmote's event manager. This is where we process the events
// emmited by Deadbeef.
static int beefmote_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2);

// Helper function for creating Beefmote's commands.
static void beefmote_command_new(int comm_id, const char *comm_name, const char *comm_help,
                                 void (*execute)(int client_socket, void* data));

// Beefmote's commands.
static void beefmote_command_help(int client_socket, void *data);
static void beefmote_command_playlists(int client_socket, void *data);
static void beefmote_command_tracklist(int client_socket, void *data);
static void beefmote_command_tracklist_address(int client_socket, void *data);
static void beefmote_command_trackcurr(int client_socket, void *data);
static void beefmote_command_play(int client_socket, void *data);
static void beefmote_command_play_search(int client_socket, void *data);
static void beefmote_command_play_address(int client_socket, void *data);
static void beefmote_command_random(int client_socket, void *data);
static void beefmote_command_play_resume(int client_socket, void *data);
static void beefmote_command_stop(int client_socket, void *data);
static void beefmote_command_stop_after_current(int client_socket, void *data);
static void beefmote_command_previous(int client_socket, void *data);
static void beefmote_command_next(int client_socket, void *data);
static void beefmote_command_volume_up(int client_socket, void *data);
static void beefmote_command_volume_down(int client_socket, void *data);
static void beefmote_command_seek_forward(int client_socket, void *data);
static void beefmote_command_seek_backward(int client_socket, void *data);
static void beefmote_command_search(int client_socket, void *data);
static void beefmote_command_notify_playlistchanged(int client_socket, void *data);
static void beefmote_command_notify_nowplaying(int client_socket, void *data);
static void beefmote_command_add_search_playbackqueue(int client_socket, void *data);
static void beefmote_command_exit(int client_socket, void* data);


  ///////////
 // Utils //
///////////

// Sends a newline to a client.
static inline void client_print_newline(int client_socket);

// Prints a string to a client.
static void client_print_string(int client_socket, const char* string);

// Prints a track in the format "[Tool - Lateralus] 05 - Schism (6:48)" to a client.
// print_addr indicates whether the track's memory address should be prepended.
static void client_print_track(int client_socket, DB_playItem_t *track, bool print_addr);

// Prints to a client all tracks of a playlist using client_print_track. Returns number of tracks printed.
static int client_print_playlist(int client_socket, ddb_playlist_t *playlist, bool print_addr);


  /////////////////////////////////////
 // Start of Deadbeef's boilerplate //
/////////////////////////////////////

// Deadbeef's plugin load function. This is the first thing executed by Deadbeef on plugin load.
// It registers the plugin with Deadbeef and gives us access to the plugins API.
DB_plugin_t *beefmote_load(DB_functions_t *api)
{
    deadbeef = api;
    return DB_PLUGIN(&beefmote_plugin);
}

// Beefmote's entry point. Second thing executed by Deadbeef on plugin load.
static int plugin_start()
{
    beefmote_stopthread = 0;
    beefmote_currtrack = NULL;
    beefmote_notify_playlistchanged = false;
    beefmote_notify_nowplaying = false;
    beefmote_client_socket = -1;
    beefmote_stopthread_mutex = deadbeef->mutex_create_nonrecursive();
    beefmote_initialize_commands();
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
    .misc.plugin.message = beefmote_message,
    .misc.plugin.configdialog = beefmote_settings_dialog,
};

  ///////////////////////////////////
 // End of Deadbeef's boilerplate //
///////////////////////////////////

static inline void client_print_newline(int client_socket)
{
    assert(client_socket > 0);

    int bytes_n = write(client_socket, "\n", 1);
    if (bytes_n != 1) {
        beefmote_debug_print("error: couldn't send all data to client\n");
    }   
}

static void client_print_string(int client_socket, const char* string)
{
    assert(client_socket > 0);
    assert(string);

    int len = strlen(string);
    int bytes_n = write(client_socket, string, len);
    if (bytes_n != len) {
        beefmote_debug_print("error: couldn't send all data to client\n");
    }
}

static void client_print_track(int client_socket, DB_playItem_t *track, bool print_addr)
{
    assert(client_socket > 0);
    assert(track);

    const char *track_artist = deadbeef->pl_meta_for_key(track, "artist")->value;
    const char *track_album = deadbeef->pl_meta_for_key(track, "album")->value;
    const char *track_title = deadbeef->pl_meta_for_key(track, "title")->value;
    const char *track_tracknumber = deadbeef->pl_meta_for_key(track, "track")->value;
    char track_length[100];
    float len = deadbeef->pl_get_item_duration(track);
    deadbeef->pl_format_time(len, track_length, 100);
    char track_str[BEEFMOTE_STR_MAXLENGTH];

    if (print_addr) {
        sprintf(track_str, "%p [%s - %s] %s - %s (%s)\n", track, track_artist, track_album, track_tracknumber, track_title,
                track_length);
    }
    else {
        sprintf(track_str, "[%s - %s] %s - %s (%s)\n", track_artist, track_album, track_tracknumber, track_title,
                track_length);
    }

    int track_str_len = strlen(track_str);
    int bytes_n = write(client_socket, track_str, track_str_len);
    if (bytes_n != track_str_len) {
        beefmote_debug_print("error: failure while sending data to client\n");
    }
}

static int client_print_playlist(int client_socket, ddb_playlist_t *playlist, bool print_addr)
{
    assert(client_socket > 0);
    assert(playlist);

    int i = 0;
    DB_playItem_t *track;

    client_print_string(client_socket, "TRACKLIST_BEGIN\n");

    while (track = deadbeef->plt_get_item_for_idx(playlist, i++, PL_MAIN)) {
        char idx[20];
        sprintf(idx, "(%d) ", i);
        client_print_string(client_socket, idx);
        client_print_track(client_socket, track, print_addr);
        deadbeef->pl_item_unref(track);
    }

    client_print_string(client_socket, "TRACKLIST_END\n");

    return --i;
}

static void beefmote_thread(void *data)
{
    char beefmote_buffer[BEEFMOTE_BUFSIZE];
    memset(beefmote_buffer, 0, BEEFMOTE_BUFSIZE);

    struct sockaddr_in client_addr;
    int client_size = sizeof(client_addr);

    fd_set readfds;
    struct timeval timeout;

    char welcome_str[BEEFMOTE_STR_MAXLENGTH];
    strcpy(welcome_str, "Hello! Welcome to Beefmote's server. Type \"");
    strcat(welcome_str, beefmote_commands[BEEFMOTE_HELP].name);
    strcat(welcome_str, "\" for a list of available commands\n\n");
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
        if ((beefmote_client_socket = accept(beefmote_socket, (struct sockaddr *) &client_addr,
                                    &client_size)) < 0) {
            sleep(BEEFMOTE_WAIT_CLIENT);        // let's not kill the CPU, shall we?
            continue;
        }

        beefmote_debug_print("got connection from %s\n", inet_ntoa(client_addr.sin_addr));

        int bytes_n = write(beefmote_client_socket, welcome_str, welcome_len);

        if (bytes_n != welcome_len) {
            beefmote_debug_print("error: failure while sending data to client\n");
        }

        // At this point, a client is connected. We now just have to wait for it
        // to say something to us. We check every BEEFMOTE_WAIT_CLIENT seconds to
        // see if the client said something.
        for (;;) {
            deadbeef->mutex_lock(beefmote_stopthread_mutex);
            if (beefmote_stopthread == 1) {
                deadbeef->mutex_unlock(beefmote_stopthread_mutex);
                close(beefmote_client_socket);
                beefmote_client_socket = -1;
                return;
            }
            deadbeef->mutex_unlock(beefmote_stopthread_mutex);

            // All this stuff has to be set *every* time before a select(),
            // because select() modifies it.
            FD_ZERO(&readfds);
            FD_SET(beefmote_client_socket, &readfds);
            timeout.tv_sec = BEEFMOTE_WAIT_CLIENT;
            timeout.tv_usec = 0;
            int rv = select(beefmote_client_socket + 1, &readfds, NULL, NULL, &timeout);

            if (rv == -1) {
                beefmote_debug_print("error: select failed\n");
                close(beefmote_client_socket);
                beefmote_client_socket = -1;
                break;
            } else if (rv == 0) {
                continue;
            } else {
                if (FD_ISSET(beefmote_client_socket, &readfds)) {
                    // Read and print data from client.
                    int bytes_n = read(beefmote_client_socket, beefmote_buffer, BEEFMOTE_BUFSIZE);

                    if (bytes_n < 0) {
                        beefmote_debug_print("error: failed on read(), errno = %d, closing client socket\n", errno);
                        close(beefmote_client_socket);
                        beefmote_client_socket = -1;
                        break;
                    }

                    if (bytes_n == 0) {
                        beefmote_debug_print("client %s closed connection\n",
                                             inet_ntoa(client_addr.sin_addr));
                        close(beefmote_client_socket);
                        beefmote_client_socket = -1;
                        break;
                    }

                    // Process commands.
                    beefmote_debug_print("received %d bytes from client %s: %s", bytes_n,
                                         inet_ntoa(client_addr.sin_addr), beefmote_buffer);
                    beefmote_process_command(beefmote_client_socket, beefmote_buffer);
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
    if (bind(beefmote_socket, (struct sockaddr*) &servaddr, sizeof(servaddr))) {
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

static void beefmote_command_new(int comm_id, const char *comm_name, const char *comm_help,
                                 void (*execute)(int client_socket, void* data))
{
    assert(comm_id >= 0 && comm_id < BEEFMOTE_COMMANDS_N);
    assert(comm_name);
    assert(comm_help);

    strcpy(beefmote_commands[comm_id].name, comm_name);
    beefmote_commands[comm_id].name_len = strlen(comm_name);
    strcpy(beefmote_commands[comm_id].help, comm_help);
    beefmote_commands[comm_id].execute = execute;
}

static void beefmote_initialize_commands()
{
    beefmote_command_new(BEEFMOTE_HELP, "h", "prints this message.", beefmote_command_help);
    beefmote_command_new(BEEFMOTE_PLAYLISTS, "pl",
                         "usage: pl [idx]. If passed with no arguments, prints all playlists (" \
                         "the current playlist is marked with (*)). " \
                         "If passed with an index number, sets the current playlist to the " \
                         "playlist with index idx.",
		    	 beefmote_command_playlists);
    beefmote_command_new(BEEFMOTE_TRACKLIST, "tl", "prints all the tracks in the current playlist.",
                         beefmote_command_tracklist);
    beefmote_command_new(BEEFMOTE_TRACKLIST_ADDRESS, "tla",
                         "like tl, but prepends each track by its memory address.",
                         beefmote_command_tracklist_address);
    beefmote_command_new(BEEFMOTE_TRACKCURR, "tc", "prints the current track.", beefmote_command_trackcurr);
    beefmote_command_new(BEEFMOTE_PLAY, "pp", "plays current track.", beefmote_command_play);
    beefmote_command_new(BEEFMOTE_PLAY_SEARCH, "ps", "usage: ps idx. " \
                         "Plays a track by its index in the search list.",
                         beefmote_command_play_search);
    beefmote_command_new(BEEFMOTE_PLAY_ADDRESS, "pa", "usage: pa memaddr. " \
                         "Plays a track by memory address; memaddr must be written in " \
                         "hex notation.", beefmote_command_play_address);
    beefmote_command_new(BEEFMOTE_PLAY_RESUME, "p",
                         "Usage: p [idx]. If passed with no arguments, pauses/resumes playback. " \
                         "If passed with an index, plays the track at index idx in the current " \
                         "playlist.", beefmote_command_play_resume);
    beefmote_command_new(BEEFMOTE_RANDOM, "r", "plays random track.", beefmote_command_random);
    beefmote_command_new(BEEFMOTE_STOP_AFTER_CURRENT, "sac", "stops playback after current track.",
                         beefmote_command_stop_after_current);
    beefmote_command_new(BEEFMOTE_STOP, "s", "stops playback.", beefmote_command_stop);
    beefmote_command_new(BEEFMOTE_PREVIOUS, "pv", "plays previous track.", beefmote_command_previous);
    beefmote_command_new(BEEFMOTE_NEXT, "nt", "plays next track.", beefmote_command_next);
    beefmote_command_new(BEEFMOTE_VOLUME_UP, "vu", "usage: vu [step]. If no argument is passed, " \
                         "increases volume by a default step of 5. If a number is passed, increases volume " \
                         "by that amount.", beefmote_command_volume_up);
    beefmote_command_new(BEEFMOTE_VOLUME_DOWN, "vd", "usage: vd [step]. If no argument is passed, " \
                         "decreases volume by a default step of 5. If a number is passed, decreases volume " \
                         "by that amount.", beefmote_command_volume_down);
    beefmote_command_new(BEEFMOTE_SEEK_FORWARD, "sf", "seeks forward.", beefmote_command_seek_forward);
    beefmote_command_new(BEEFMOTE_SEEK_BACKWARD, "sb", "seeks backward.", beefmote_command_seek_backward);
    beefmote_command_new(BEEFMOTE_SEARCH, "/", "usage: / str. Searches a string in the current " \
            "playlist and returns a list of matching tracks. The matched tracks can be played by using their index " \
            "number with the ps command.", beefmote_command_search);
    beefmote_command_new(BEEFMOTE_NOTIFY_PLAYLISTCHANGED, "ntfy-plchanged",
                         "Notifies when the current playlist has changed (meaning you'll probably " \
                         "want to get the tracklist again).", beefmote_command_notify_playlistchanged);
    beefmote_command_new(BEEFMOTE_NOTIFY_NOW_PLAYING, "ntfy-nowplaying",
                         "usage: ntfy-nowplaying true/false. Sets whether to notify when a new track " \
                         "starts to play. Default: false.",
                         beefmote_command_notify_nowplaying);
    beefmote_command_new(BEEFMOTE_ADD_SEARCH_PLAYBACKQUEUE, "aps", "usage: aps idx. Adds a searched track to the " \
                         "playback queue.", beefmote_command_add_search_playbackqueue);
    beefmote_command_new(BEEFMOTE_EXIT, "exit", "terminates Deadbeef.", beefmote_command_exit);
}

static void beefmote_process_command(int client_socket, char *command)
{
    assert(client_socket > 0);
    assert(command);
    assert(beefmote_commands);

    char* ptr = command;
    char* arg = NULL;

    // Remove trailing whitespace from command and detect argument.
    while (*ptr++) {
        if (!arg && *(ptr + 1) != 0 && isspace(*ptr) && !isspace(*(ptr + 1))) {
            arg = ptr + 1;
            *ptr = 0;
            break;
        }
        else if (isspace(*ptr)) {
            *ptr = 0;
            break;
        }
    }

    if (arg) {
        // Remove trailing \r\n from argument.
        char* arg_ptr = arg;

        while (*arg_ptr++) {
            if (*arg_ptr == '\r' || *arg_ptr == '\n') {
                *arg_ptr = 0;
            }
        }
    }

    // At this point, we are guaranteed that:
    // 1) "command" is a string which contains no whitespace (and thus
    // can potentially match a Beefmote's command name).
    // 2) arg is whatever comes after "command", minus trailing \r\n. This
    // *includes* whatever whitespace was there, so beefmote_command_* functions
    // must do any necessary cleanup themselves.

    int comm_len = strlen(command);

    for (int i = 0; i < BEEFMOTE_COMMANDS_N; i++) {
        if (comm_len != beefmote_commands[i].name_len) {
            continue;
        }

        if (strncmp(beefmote_commands[i].name, command, beefmote_commands[i].name_len) == 0) {
            beefmote_commands[i].execute(client_socket, arg);
            return;
        }
    }

    char invalid[] = "\nPlease type a valid command\n\n";
    int invalid_len = strlen(invalid);
    int bytes_n = write(client_socket, invalid, invalid_len);
    if (bytes_n != invalid_len) {
        beefmote_debug_print("error: couldn't send all data to client\n");
    }
}

static int beefmote_message(uint32_t id, uintptr_t ctx, uint32_t p1, uint32_t p2)
{
    ddb_event_track_t* ev;

    switch (id) {
    case DB_EV_SONGCHANGED:
        beefmote_currtrack = ((ddb_event_trackchange_t*) ctx)->to;

        if (beefmote_currtrack && beefmote_notify_nowplaying && beefmote_client_socket != -1) {
            client_print_string(beefmote_client_socket, "Now playing ");
            client_print_track(beefmote_client_socket, beefmote_currtrack, false);
            client_print_newline(beefmote_client_socket);
        }

        break;

    /* Deadbeef is basically useless if you want to get a by-track
     * changelog, meaning you CAN'T get a list of, e.g. added or
     * deleted tracks to/from a playlist. Such is life. When a song
     * is deleted, the case is DB_EV_PLAYLISTCHANGED, and p1 is
     * DDB_PLAYLIST_CHANGE_CONTENT. That's all the info you get,
     * meaning a client will have to get the ENTIRE tracklist
     * AGAIN for just the one deleted track if it wants to keep in
     * sync with the Deadbeef playlist. *sigh* */
    case DB_EV_PLAYLISTCHANGED:
        if (p1 == DDB_PLAYLIST_CHANGE_CONTENT) {
            if (beefmote_client_socket != -1 && beefmote_notify_playlistchanged) {
                client_print_string(beefmote_client_socket, "\nThe current playlist content changed; you " \
                                    "may want to get the tracklist again.\n\n");
            }
        }
        break;
    }

    return 0;
}

static void beefmote_command_help(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(beefmote_commands[BEEFMOTE_HELP].name);

    char help[BEEFMOTE_STR_MAXLENGTH * 2];
    int help_len, bytes_n;

    client_print_newline(client_socket);

    for (int i = 0; i < BEEFMOTE_COMMANDS_N; i++) {
        strcpy(help, beefmote_commands[i].name);
        strcat(help, "\n\t");
        strcat(help, beefmote_commands[i].help);
        strcat(help, "\n");
        help_len = strlen(help);
        bytes_n = write(client_socket, help, help_len);
        if (bytes_n != help_len) {
            beefmote_debug_print("error: couldn't send all data to client\n");
        }
    }

    client_print_newline(client_socket);
}

static void beefmote_command_playlists(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    int pl_n = deadbeef->plt_get_count();

    if (pl_n <= 0) {
        client_print_string(client_socket, "\nNo playlists\n\n");
        return;
    }

    if (data) {
        int idx = strtol((char*) data, NULL, 10);

        if (idx < 1 || idx > pl_n) {
            client_print_string(client_socket, "\nPlaylist index out of bounds\n\n");
            return;
        }

        deadbeef->plt_set_curr_idx(--idx);
        return;
    }

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();

    char str[BEEFMOTE_STR_MAXLENGTH];

    for (int i = 0; i < pl_n; i++) {
        ddb_playlist_t *pl = deadbeef->plt_get_for_idx(i);
        char pl_name[BEEFMOTE_STR_MAXLENGTH];
        deadbeef->plt_get_title(pl, pl_name, sizeof(pl_name));
        sprintf(str, "\nPlaylist %d: %s", i + 1, pl_name);
        client_print_string(client_socket, str);
        if (pl_curr == pl) {
            client_print_string(client_socket, " (*)");
        }
        client_print_newline(client_socket);
    }

    client_print_newline(client_socket);
}

static void beefmote_command_tracklist(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (pl_curr) {
        client_print_playlist(client_socket, pl_curr, false);
        deadbeef->plt_unref(pl_curr);
    }
}

static void beefmote_command_tracklist_address(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (pl_curr) {
        client_print_playlist(client_socket, pl_curr, true);
        deadbeef->plt_unref(pl_curr);
    }
}

static void beefmote_command_trackcurr(int client_socket, void *data)
{
    assert(client_socket > 0);

    if (beefmote_currtrack) {
        client_print_newline(client_socket);
        client_print_track(client_socket, beefmote_currtrack, false);
        client_print_newline(client_socket);
    }
    else {
        client_print_string(client_socket, "\nNo current track\n\n");
    }
}

static void beefmote_command_play(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_PLAY_CURRENT, 0, 0, 0);
}

static void beefmote_command_play_search(int client_socket, void *data)
{
    assert(client_socket > 0);

    if (!data) {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_PLAY_SEARCH].help);
        client_print_newline(client_socket);
        return;
    }

    int track_index = strtol((char*) data, NULL, 10);
    if (!track_index) {
        client_print_string(client_socket, "\nInvalid search index\n\n");
        return;
    }

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (!pl_curr) {
        return;
    }

    DB_playItem_t *track = deadbeef->plt_get_item_for_idx(pl_curr, --track_index, PL_SEARCH);
    if (track) {
        client_print_string(client_socket, "\nPlaying ");
        client_print_track(client_socket, track, false);
        client_print_newline(client_socket);
        int idx = deadbeef->pl_get_idx_of(track); // we gotta use the track index in the MAIN playlist
        deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, idx, 0);
        deadbeef->pl_item_unref(track);
    }
    else {
        client_print_string(client_socket, "\nInvalid search index\n\n");
    }
}

static void beefmote_command_play_address(int client_socket, void *data)
{
    assert(client_socket > 0);

    if(!data) {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_PLAY_ADDRESS].help);
        client_print_newline(client_socket);
        return;
    }

    long addr = strtol((char*) data, NULL, 16);

    DB_playItem_t *track = (DB_playItem_t*) addr;
    int idx = deadbeef->pl_get_idx_of(track); // get MAIN playlist track index

    if(idx == -1) {
        client_print_string(client_socket, "\nInvalid track memory address\n\n");
        return;
    }

    deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, idx, 0);
}

static void beefmote_command_random(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);
    
    deadbeef->sendmessage(DB_EV_PLAY_RANDOM, 0, 0, 0);
}

static void beefmote_command_play_resume(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    if (data) {
        int idx = strtol((char*) data, NULL, 10);
        deadbeef->sendmessage(DB_EV_PLAY_NUM, 0, --idx, 0);
        return;
    }

    int state = deadbeef->get_output()->state();

    if (state == OUTPUT_STATE_PLAYING) {
        deadbeef->sendmessage (DB_EV_PAUSE, 0, 0, 0);
    }
    else {
        deadbeef->sendmessage (DB_EV_PLAY_CURRENT, 0, 0, 0);
    }
}

static void beefmote_command_stop(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_STOP, 0, 0, 0);
}

static void beefmote_command_stop_after_current(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    int value = deadbeef->conf_get_int("playlist.stop_after_current", 0);
    value = 1 - value;
    deadbeef->conf_set_int("playlist.stop_after_current", value);
    deadbeef->sendmessage(DB_EV_CONFIGCHANGED, 0, 0, 0);
}

static void beefmote_command_previous(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_PREV, 0, 0, 0);
}

static void beefmote_command_next(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_NEXT, 0, 0, 0);
}

static void beefmote_command_volume_up(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    int step;
    if (data) {
        step = strtol(data, NULL, 10);
    }
    else {
        step = BEEFMOTE_VOLUME_STEP;
    }
    deadbeef->volume_set_db(deadbeef->volume_get_db() + step);
}

static void beefmote_command_volume_down(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    int step;
    if (data) {
        step = strtol(data, NULL, 10);
    }
    else {
        step = BEEFMOTE_VOLUME_STEP;
    }
    deadbeef->volume_set_db(deadbeef->volume_get_db() - step);
}

static void beefmote_command_seek_forward(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->playback_set_pos(deadbeef->playback_get_pos() + BEEFMOTE_SEEK_STEP);
}

static void beefmote_command_seek_backward(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->playback_set_pos(deadbeef->playback_get_pos() - BEEFMOTE_SEEK_STEP);
}

static void beefmote_command_search(int client_socket, void *data)
{
    assert(client_socket > 0);

    if (!data) {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_SEARCH].help);
        client_print_newline(client_socket);
        return;
    }

    char *arg = (char*) data;

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (!pl_curr) {
        return;
    }

    deadbeef->plt_search_process(pl_curr, arg);

    DB_playItem_t *track;
    int i = 0;
    bool once = false;

    client_print_newline(client_socket);

    while (track = deadbeef->plt_get_item_for_idx(pl_curr, i++, PL_SEARCH)) {
        once = true;
        char idx[20];
        sprintf(idx, "(%d)\t", i);
        client_print_string(client_socket, idx);
        client_print_track(client_socket, track, false);
        deadbeef->pl_item_unref(track);
    }

    if (once) {
        client_print_newline(client_socket);
    }
    else {
        client_print_string(client_socket, "(nothing was found)\n\n");
    }

    deadbeef->plt_unref(pl_curr);
}

static void beefmote_command_notify_playlistchanged(int client_socket, void *data)
{
    assert(client_socket > 0);

    beefmote_notify_playlistchanged = !beefmote_notify_playlistchanged;

    char msg[BEEFMOTE_STR_MAXLENGTH];
    strcpy(msg, "\nNotification set to ");

    if (beefmote_notify_playlistchanged) {
        strcat(msg, "true.\n\n");
    }
    else {
        strcat(msg, "false.\n\n");
    }

    client_print_string(client_socket, msg);
}

static void beefmote_command_notify_nowplaying(int client_socket, void *data)
{
    assert(client_socket > 0);

    if(!data) {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_NOTIFY_NOW_PLAYING].help);
        client_print_newline(client_socket);
        return;
    }

    char *str = data;

    if (strcmp(str, "true") == 0) {
        beefmote_notify_nowplaying = true;
        beefmote_debug_print("Now playing notification set to true\n");
    }
    else if (strcmp(str, "false") == 0) {
        beefmote_notify_nowplaying = false;
        beefmote_debug_print("Now playing notification set to false\n");
    }
    else {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_NOTIFY_NOW_PLAYING].help);
        client_print_newline(client_socket);
        return;
    } 
}

static void beefmote_command_add_search_playbackqueue(int client_socket, void *data)
{
    assert(client_socket > 0);

    if (!data) {
        client_print_newline(client_socket);
        client_print_string(client_socket, beefmote_commands[BEEFMOTE_ADD_SEARCH_PLAYBACKQUEUE].help);
        client_print_newline(client_socket);
        return;
    }

    int track_index = strtol((char*) data, NULL, 10);
    if (!track_index) {
        client_print_string(client_socket, "\nInvalid search index\n\n");
        return;
    }

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (!pl_curr) {
        return;
    }

    DB_playItem_t *track = deadbeef->plt_get_item_for_idx(pl_curr, --track_index, PL_SEARCH);
    if (track) {
        deadbeef->playqueue_push(track);
        deadbeef->pl_item_unref(track);
    }
    else {
        client_print_string(client_socket, "\nInvalid search index\n\n");
    }
}

static void beefmote_command_exit(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_TERMINATE, 0, 0, 0);
}
