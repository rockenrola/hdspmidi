/*
 *
 * Copyright (c) 2005 Clemens Ladisch <clemens@ladisch.de>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <sys/poll.h>
#include <alsa/asoundlib.h>
#include "HDSPMixerCard.h"
#include <iostream>
#include <libconfig.h++>
#include "Channel.h"
#include "midicontroller.h"
#include "bridge.h"

using namespace std;

static volatile sig_atomic_t stop = 0;

static void sighandler(int sig){
    if (sig == SIGINT || sig == SIGTERM){
        stop = 1;
    }
}

int main(int argc, char *argv[])
{
    Bridge bridge;
    libconfig::Config cfg;

    struct pollfd *pfds = NULL;
    int npfds = 0;
    int err;
    const char *port_out;
    const char *port_in;
    const char *relay_path;

    // check if config file was specified
    if (argc!=2){
        cout << "This programs needs a configuration file" << endl;
        exit(EXIT_FAILURE);
    }

    // Search and initialize RME card.
    HDSPMixerCard::search_card(&bridge.hdsp_card);
    if (bridge.hdsp_card == NULL) {
#ifndef NO_RME
        cout << "No RME cards found." << endl;
        exit(EXIT_FAILURE);
#endif //NO_RME
    }

    try{
        cfg.readFile(argv[1]);

        libconfig::Setting& si = cfg.lookup("midi_in");
        port_in = si;
        libconfig::Setting& so = cfg.lookup("midi_out");
        port_out = so;
        libconfig::Setting& rp = cfg.lookup("relay_path");
        relay_path = rp;
        libconfig::Setting& sm = cfg.lookup("main_channel");
        bridge.main = sm;
        libconfig::Setting& smon = cfg.lookup("monitors_channel");
        bridge.monitors = smon;
        libconfig::Setting& sp = cfg.lookup("phones_channel");
        bridge.phones = sp;
    }
    catch (libconfig::ConfigException& e) {
        cout << "error readind config file" << endl;
        exit(EXIT_FAILURE);
    }

    bridge.channels.read(&cfg);
    try{
        bridge.midicontroller.parse_ports_out(port_out);
        bridge.midicontroller.parse_ports_in(port_in);
    } catch (exception& e){
        cout << "Problem parsing MIDI ports." << " In: " << port_in << ". Out: " << port_out << "." << endl;
        exit(EXIT_FAILURE);
    }
    if (bridge.midicontroller.connect_ports() == false) {
        cout << "Problem connecting to MIDI ports." << " In: " << port_in << ". Out: " << port_out << "." << endl;
        exit(EXIT_FAILURE);
    }
    bridge.relay.open(relay_path);

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    npfds = snd_seq_poll_descriptors_count(bridge.midicontroller.seq, POLLIN);
    // +1 for udev due to relay
    npfds++;
    pfds = new pollfd[npfds];
    snd_seq_poll_descriptors(bridge.midicontroller.seq, pfds, npfds-1, POLLIN);
    pfds[npfds-1].fd = bridge.relay.get_fd();
    pfds[npfds-1].events = POLLIN;

    // restore BCF2000 and relay
    bridge.restore();

    while(stop == 0) {
        // wait for midi events and udev events
        if (poll(pfds, npfds, -1) < 0){
            break;
        }

        // process udev events
        if (pfds[npfds-1].revents && POLLIN ){
            if(bridge.relay.reconnect()){
                bridge.control_onair();
            }
        }

        // process midi events
        do {
            snd_seq_event_t *event;
            err = snd_seq_event_input(bridge.midicontroller.seq, &event);
            if (err < 0){
                break;
            }
            if (event){
                bridge.dump_event(event);
            }
        } while (err > 0);
    }

    bridge.channels.save(&cfg);
    cfg.writeFile(argv[1]);
    delete[] pfds;

    return 0;
}
