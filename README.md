# Beefmote
Beefmote: An Android DeaDBeeF remote.

# What's this?
Beefmote is an Android remote for the Linux music player [DeaDBeeF](https://github.com/DeaDBeeF-Player/deadbeef).
The program has a server/client architecture. This repository you're in is the server.

The Beefmote server is a simple TCP/IP, string-based C server which enables you to control DeaDBeeF by connecting to it and sending it commands. It even works with telnet!

# How do I install it?
First, make sure you have the DeaDBeeF dev files installed in your machine. The simplest (and most painful) way to do this, as far as I know, is to compile DeaDBeeF from sources.

Once you've done that, simply run: `make all && make install`

This will compile the Beefmote server plugin and install it in the DeaDBeeF plugin folder (`~/.local/lib64/deadbeef`), creating it if necessary.

To check that the plugin was correctly loaded into DeaDBeeF, go to `Edit/Preferences/Plugins` and you should see `Beefmote` listed among the plugins.

# How do I use it?

The Beefmote server is meant to be used with the [Beefmote Android client](https://github.com/lgvaioli/beefmoteclient), but you can actually use it with anything that talks TCP/IP.

You can use it with telnet: `telnet 127.0.0.1 49160`