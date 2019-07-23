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
#define BEEFMOTE_TRACKSTR_MAXLENGTH 1000
#define BEEFMOTE_COMMAND_MAXLENGTH 100
#define BEEFMOTE_COMMAND_HELP_MAXLENGTH 1000
#define BEEFMOTE_VOLUME_STEP 5
#define BEEFMOTE_SEEK_STEP 5

#define beefmote_debug_print(fmt, ...) \
        do { if (DEBUG) fprintf(stderr, "[beefmote] " fmt, ##__VA_ARGS__); } while (0)

typedef struct DB_beefmote_plugin_s {
    DB_misc_t misc;
} DB_beefmote_plugin_t;

enum BEEFMOTE_COMMANDS {
    BEEFMOTE_HELP,
    BEEFMOTE_SHOWTRACKS,
    BEEFMOTE_PLAY,
    BEEFMOTE_RANDOM,
    BEEFMOTE_PAUSE,
    BEEFMOTE_STOP_AFTER_CURRENT,
    BEEFMOTE_STOP,
    BEEFMOTE_PREVIOUS,
    BEEFMOTE_NEXT,
    BEEFMOTE_VOLUME_UP,
    BEEFMOTE_VOLUME_DOWN,
    BEEFMOTE_SEEK_FORWARD,
    BEEFMOTE_SEEK_BACKWARD,
    BEEFMOTE_EXIT,
    BEEFMOTE_COMMANDS_N // marks end of command list
};

typedef struct beefmote_command {
    char name[BEEFMOTE_COMMAND_MAXLENGTH];
    int name_len;
    char help[BEEFMOTE_COMMAND_HELP_MAXLENGTH];
    void (*execute)(int client_socket, void* data);
} beefmote_command;

// Globals.
static DB_functions_t *deadbeef;        // deadbeef's plugin API
static DB_beefmote_plugin_t beefmote_plugin;    // beefmote's plugin description
static uintptr_t beefmote_stopthread_mutex;
static intptr_t beefmote_tid;
static int beefmote_stopthread;
static int beefmote_socket;
static beefmote_command beefmote_commands[BEEFMOTE_COMMANDS_N];

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

// Initializes Beefmote's commands.
static void beefmote_initialize_commands();

// Processes a Beefmote command.
static void beefmote_process_command(int client_socket, char *command);

// Beefmote's commands.
static void beefmote_command_help(int client_socket, void *data);
static void beefmote_command_showtracks(int client_socket, void *data);
static void beefmote_command_play(int client_socket, void *data);
static void beefmote_command_random(int client_socket, void *data);
static void beefmote_command_pause(int client_socket, void *data);
static void beefmote_command_stop(int client_socket, void *data);
static void beefmote_command_stop_after_current(int client_socket, void *data);
static void beefmote_command_previous(int client_socket, void *data);
static void beefmote_command_next(int client_socket, void *data);
static void beefmote_command_volume_up(int client_socket, void *data);
static void beefmote_command_volume_down(int client_socket, void *data);
static void beefmote_command_seek_forward(int client_socket, void *data);
static void beefmote_command_seek_backward(int client_socket, void *data);
static void beefmote_command_exit(int client_socket, void *data);


  /////////////////////////////////////
 // Start of Deadbeef's boilerplate //
/////////////////////////////////////

// Beefmote's entry point. Second thing executed by Deadbeef on plugin load.
static int plugin_start()
{
    beefmote_stopthread = 0;
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
    .misc.plugin.configdialog = beefmote_settings_dialog,
};

  ///////////////////////////////////
 // End of Deadbeef's boilerplate //
///////////////////////////////////


// Prints a track in the format "[Tool - Lateralus] 05 - Schism (6:48)".
static void beefmote_print_track(int client_socket, DB_playItem_t *track)
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
    char track_str[BEEFMOTE_TRACKSTR_MAXLENGTH];

    sprintf(track_str, "[%s - %s] %s - %s (%s)\n", track_artist, track_album, track_tracknumber, track_title,
            track_length);

    int track_str_len = strlen(track_str);

    int bytes_n = write(client_socket, track_str, track_str_len);
   
    if (bytes_n != track_str_len) {
        beefmote_debug_print("error: failure while sending data to client\n");
    }
}

// Prints all tracks of a playlist using track_print. Returns number of tracks printed.
int beefmote_print_playlist(int client_socket, ddb_playlist_t *playlist)
{
    assert(client_socket > 0);
    assert(playlist);

    int i = 0;
    DB_playItem_t *track;

    int bytes_n = write(client_socket, "\n", 1);
    if (bytes_n != 1) {
        beefmote_debug_print("error: couldn't send all data to client\n");
    }

    while (track = deadbeef->plt_get_item_for_idx(playlist, i++, PL_MAIN)) {
        beefmote_print_track(client_socket, track);
        deadbeef->pl_item_unref(track);
    }

    bytes_n = write(client_socket, "\n", 1);

    if (bytes_n != 1) {
        beefmote_debug_print("error: couldn't send all data to client\n");
    }

    return --i;
}

static void beefmote_thread(void *data)
{
    char beefmote_buffer[BEEFMOTE_BUFSIZE];
    memset(beefmote_buffer, 0, BEEFMOTE_BUFSIZE);

    int client_socket;
    struct sockaddr_in client_addr;
    int client_size = sizeof(client_addr);

    fd_set readfds;
    struct timeval timeout;

    char welcome_str[] = "Hello! Welcome to Beefmote's server. " \
                         "Type \"help\" for a list of available commands\n\n";
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
            beefmote_debug_print("error: failure while sending data to client\n");
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

                    // Process commands.
                    beefmote_process_command(client_socket, beefmote_buffer);

                    //beefmote_debug_print("received %d bytes from client %s: %s", bytes_n,
                    //                     inet_ntoa(client_addr.sin_addr), beefmote_buffer);
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

static void beefmote_initialize_commands()
{
    strcpy(beefmote_commands[BEEFMOTE_HELP].name, "help");
    beefmote_commands[BEEFMOTE_HELP].name_len = strlen(beefmote_commands[BEEFMOTE_HELP].name);
    strcpy(beefmote_commands[BEEFMOTE_HELP].help, "prints this message");
    beefmote_commands[BEEFMOTE_HELP].execute = &beefmote_command_help;

    strcpy(beefmote_commands[BEEFMOTE_SHOWTRACKS].name, "show-tracks");
    beefmote_commands[BEEFMOTE_SHOWTRACKS].name_len = strlen(beefmote_commands[BEEFMOTE_SHOWTRACKS].name);
    strcpy(beefmote_commands[BEEFMOTE_SHOWTRACKS].help, "prints all the tracks in the current playlist");
    beefmote_commands[BEEFMOTE_SHOWTRACKS].execute = &beefmote_command_showtracks;

    strcpy(beefmote_commands[BEEFMOTE_PLAY].name, "play");
    beefmote_commands[BEEFMOTE_PLAY].name_len = strlen(beefmote_commands[BEEFMOTE_PLAY].name);
    strcpy(beefmote_commands[BEEFMOTE_PLAY].help, "plays current track");
    beefmote_commands[BEEFMOTE_PLAY].execute = &beefmote_command_play;

    strcpy(beefmote_commands[BEEFMOTE_RANDOM].name, "random");
    beefmote_commands[BEEFMOTE_RANDOM].name_len = strlen(beefmote_commands[BEEFMOTE_RANDOM].name);
    strcpy(beefmote_commands[BEEFMOTE_RANDOM].help, "plays random track");
    beefmote_commands[BEEFMOTE_RANDOM].execute = &beefmote_command_random;

    strcpy(beefmote_commands[BEEFMOTE_PAUSE].name, "pause");
    beefmote_commands[BEEFMOTE_PAUSE].name_len = strlen(beefmote_commands[BEEFMOTE_PAUSE].name);
    strcpy(beefmote_commands[BEEFMOTE_PAUSE].help, "pauses playback");
    beefmote_commands[BEEFMOTE_PAUSE].execute = &beefmote_command_pause;

    strcpy(beefmote_commands[BEEFMOTE_STOP_AFTER_CURRENT].name, "stop-aftercurr");
    beefmote_commands[BEEFMOTE_STOP_AFTER_CURRENT].name_len = strlen(beefmote_commands[BEEFMOTE_STOP_AFTER_CURRENT].name);
    strcpy(beefmote_commands[BEEFMOTE_STOP_AFTER_CURRENT].help, "stops playback after current track");
    beefmote_commands[BEEFMOTE_STOP_AFTER_CURRENT].execute = &beefmote_command_stop_after_current;

    strcpy(beefmote_commands[BEEFMOTE_STOP].name, "stop");
    beefmote_commands[BEEFMOTE_STOP].name_len = strlen(beefmote_commands[BEEFMOTE_STOP].name);
    strcpy(beefmote_commands[BEEFMOTE_STOP].help, "stops playback");
    beefmote_commands[BEEFMOTE_STOP].execute = &beefmote_command_stop;

    strcpy(beefmote_commands[BEEFMOTE_PREVIOUS].name, "previous");
    beefmote_commands[BEEFMOTE_PREVIOUS].name_len = strlen(beefmote_commands[BEEFMOTE_PREVIOUS].name);
    strcpy(beefmote_commands[BEEFMOTE_PREVIOUS].help, "plays previous track");
    beefmote_commands[BEEFMOTE_PREVIOUS].execute = &beefmote_command_previous;

    strcpy(beefmote_commands[BEEFMOTE_NEXT].name, "next");
    beefmote_commands[BEEFMOTE_NEXT].name_len = strlen(beefmote_commands[BEEFMOTE_NEXT].name);
    strcpy(beefmote_commands[BEEFMOTE_NEXT].help, "plays next track");
    beefmote_commands[BEEFMOTE_NEXT].execute = &beefmote_command_next;

    strcpy(beefmote_commands[BEEFMOTE_VOLUME_UP].name, "volume-up");
    beefmote_commands[BEEFMOTE_VOLUME_UP].name_len = strlen(beefmote_commands[BEEFMOTE_VOLUME_UP].name);
    strcpy(beefmote_commands[BEEFMOTE_VOLUME_UP].help, "increases volume");
    beefmote_commands[BEEFMOTE_VOLUME_UP].execute = &beefmote_command_volume_up;

    strcpy(beefmote_commands[BEEFMOTE_VOLUME_DOWN].name, "volume-down");
    beefmote_commands[BEEFMOTE_VOLUME_DOWN].name_len = strlen(beefmote_commands[BEEFMOTE_VOLUME_DOWN].name);
    strcpy(beefmote_commands[BEEFMOTE_VOLUME_DOWN].help, "decreases volume");
    beefmote_commands[BEEFMOTE_VOLUME_DOWN].execute = &beefmote_command_volume_down;

    strcpy(beefmote_commands[BEEFMOTE_SEEK_FORWARD].name, "seek-forward");
    beefmote_commands[BEEFMOTE_SEEK_FORWARD].name_len = strlen(beefmote_commands[BEEFMOTE_SEEK_FORWARD].name);
    strcpy(beefmote_commands[BEEFMOTE_SEEK_FORWARD].help, "seeks forward");
    beefmote_commands[BEEFMOTE_SEEK_FORWARD].execute = &beefmote_command_seek_forward;

    strcpy(beefmote_commands[BEEFMOTE_SEEK_BACKWARD].name, "seek-backward");
    beefmote_commands[BEEFMOTE_SEEK_BACKWARD].name_len = strlen(beefmote_commands[BEEFMOTE_SEEK_BACKWARD].name);
    strcpy(beefmote_commands[BEEFMOTE_SEEK_BACKWARD].help, "seeks backward");
    beefmote_commands[BEEFMOTE_SEEK_BACKWARD].execute = &beefmote_command_seek_backward;

    strcpy(beefmote_commands[BEEFMOTE_EXIT].name, "exit");
    beefmote_commands[BEEFMOTE_EXIT].name_len = strlen(beefmote_commands[BEEFMOTE_EXIT].name);
    strcpy(beefmote_commands[BEEFMOTE_EXIT].help, "terminates Deadbeef");
    beefmote_commands[BEEFMOTE_EXIT].execute = &beefmote_command_exit;
}

static void beefmote_process_command(int client_socket, char *command)
{
    assert(client_socket > 0);
    assert(command);
    assert(beefmote_commands);

    for (int i = 0; i < BEEFMOTE_COMMANDS_N; i++) {
        if (strncmp(beefmote_commands[i].name, command, beefmote_commands[i].name_len) == 0) {
            beefmote_commands[i].execute(client_socket, NULL);
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


  //////////////////////////////////////////////
 // Start of Beefmote's commands definitions //
//////////////////////////////////////////////

static void beefmote_command_help(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(beefmote_commands[BEEFMOTE_HELP].name);

    char help[BEEFMOTE_COMMAND_MAXLENGTH + BEEFMOTE_COMMAND_HELP_MAXLENGTH];
    int help_len, bytes_n;

    bytes_n = write(client_socket, "\n", 1);

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

    bytes_n = write(client_socket, "\n", 1);
}

static void beefmote_command_showtracks(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    ddb_playlist_t *pl_curr = deadbeef->plt_get_curr();
    if (pl_curr) {
        beefmote_print_playlist(client_socket, pl_curr);
        deadbeef->plt_unref(pl_curr);
    }
}

static void beefmote_command_play(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_PLAY_CURRENT, 0, 0, 0);
}

static void beefmote_command_random(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);
    
    deadbeef->sendmessage(DB_EV_PLAY_RANDOM, 0, 0, 0);
}

static void beefmote_command_pause(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

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

    deadbeef->volume_set_db(deadbeef->volume_get_db() + BEEFMOTE_VOLUME_STEP);
}

static void beefmote_command_volume_down(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->volume_set_db(deadbeef->volume_get_db() - BEEFMOTE_VOLUME_STEP);
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

static void beefmote_command_exit(int client_socket, void *data)
{
    assert(client_socket > 0);
    assert(deadbeef);

    deadbeef->sendmessage(DB_EV_TERMINATE, 0, 0, 0);
}
