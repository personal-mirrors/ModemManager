#!/usr/bin/env python3
# -*- Mode: python; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option) any
# later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
# details.
#
# You should have received a copy of the GNU Lesser General Public License along
# with this program; if not, write to the Free Software Foundation, Inc., 51
# Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Copyright (C) 2014 Aleksander Morgado <aleksander@aleksander.es>
#

import os, sys, gi

gi.require_version('ModemManager', '1.0')
from gi.repository import GLib, GObject, Gio, ModemManager

"""
The ModemWatcher class is responsible for monitoring ModemManager
"""
class ModemWatcher:

    """
    Constructor
    """
    def __init__(self):
        # Flag for initial logs
        self.initializing = True
        # Setup DBus monitoring
        self.connection = Gio.bus_get_sync (Gio.BusType.SYSTEM, None)
        self.manager = ModemManager.Manager.new_sync (self.connection,
                                                      Gio.DBusObjectManagerClientFlags.DO_NOT_AUTO_START,
                                                      None)
        # IDs for added/removed signals
        self.object_added_id = 0
        self.object_removed_id = 0
        # Follow availability of the ModemManager process
        self.available = False
        self.manager.connect('notify::name-owner', self.on_name_owner)
        self.on_name_owner(self.manager, None)
        # Finish initialization
        self.initializing = False

    """
    ModemManager is now available
    """
    def set_available(self):
        if self.available == False or self.initializing == True:
            print('[ModemWatcher] ModemManager service is available in bus')
        self.object_added_id = self.manager.connect('object-added', self.on_object_added)
        self.object_removed_id = self.manager.connect('object-removed', self.on_object_removed)
        self.available = True
        # Initial scan
        if self.initializing == True:
            for obj in self.manager.get_objects():
                self.on_object_added(self.manager, obj)

    """
    ModemManager is now unavailable
    """
    def set_unavailable(self):
        if self.available == True or self.initializing == True:
            print('[ModemWatcher] ModemManager service not available in bus')
        if self.object_added_id:
            self.manager.disconnect(self.object_added_id)
            self.object_added_id = 0
        if self.object_removed_id:
            self.manager.disconnect(self.object_removed_id)
            self.object_removed_id = 0
        self.available = False

    """
    Name owner updates
    """
    def on_name_owner(self, manager, prop):
        if self.manager.get_name_owner():
            self.set_available()
        else:
            self.set_unavailable()

    """
    Object added
    """
    def on_object_added(self, manager, obj):
        modem = obj.get_modem()
        print('[ModemWatcher] %s (%s) modem managed by ModemManager [%s]: %s' %
              (modem.get_manufacturer(),
               modem.get_model(),
               modem.get_equipment_identifier(),
               obj.get_object_path()))
        if modem.get_state() == ModemManager.ModemState.FAILED:
            print('[ModemWatcher,%s] ignoring failed modem' %
                  modem_index(obj.get_object_path()))

    """
    Object removed
    """
    def on_object_removed(self, manager, obj):
        print('[ModemWatcher] modem unmanaged by ModemManager: %s' %
              obj.get_object_path())
