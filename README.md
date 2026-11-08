### What J-Pilot is
J-Pilot is a PalmOS device desktop for Linux/Unix.  The latest version is 2.1.1
### J-Pilot plugins
J-Pilot has a few plugins written for it.  They allow jpilot to have an interface and sync with Palm apps.
- [Pcs&Videos](https://github.com/danbodoh/picsnvideos-jpilot)
- [Manana page](http://bill.sexton.tripod.com/download.htm)  
- [KeyRing page](http://gnukeyring.sourceforge.net)

### Ubuntu install for 20.04 (focal) and 22.04 (jammy)
This will install the package and create an apt source so that it will be updated when new code is released.
```shell
curl -s https://packagecloud.io/install/repositories/judd/jpilot/script.deb.sh | sudo bash
sudo apt install jpilot jpilot-plugins
```

### Communication Permissions

#### The short version for Debian based distros and many others

Make sure your user is in the plugdev group.  Note: This will require you to logout and back in for the changes to take effect.
```shell
sudo usermod -a -G plugdev $USER
```

Create a file like `/etc/udev/rules.d/60-palm.rules` if one does not exist, containing:
```
# Give the Palm's USB device node a group the user is a member of.
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0830", MODE="0660", GROUP="plugdev"
```
Then to ask udev to read the rules and re-apply them:
```shell
sudo udevadm control --reload-rules
sudo udevadm trigger
```
Use the port 'usb:' for pilot-link and jpilot to communicate over.

#### A more detailed explanation

##### Choose to use libusb or the visor module
There are two different ways for pilot-link to reach a USB Palm, and they
use different sync ports:

- **libusb — the modern way, and what you should use.**  A pilot-link built
  against libusb talks to the USB device directly, with no serial device
  node involved.  The sync port is the literal string `usb:`, which is
  J-Pilot's default.
- **The `visor` kernel module — deprecated long ago.**  This is the old
  usb-serial approach: the module presents the Palm as a serial port and
  the sync port is a device node such as `/dev/ttyUSB0`, `/dev/ttyUSB1`,
  and so on.  It is only worth using on old systems where libusb is not
  an option.

The sync port is set in J-Pilot under File -> Preferences -> Settings, in
the "Serial Port" field.

##### Giving your user access to the Palm with a udev rule (libusb)
This is the part that matters when you sync with `usb:`.  Because libusb
talks to the raw USB device instead of a serial port, being in the
`dialout` group does not help: `dialout` only covers `/dev/tty*` nodes.
The raw USB device node that libusb opens (under `/dev/bus/usb/`) is owned
by `root` by default, so a normal user cannot open it and the sync fails
with a permission error.  A udev rule fixes this by giving that node a
group you belong to every time the Palm connects.  The rule goes in
`/etc/udev/rules.d/60-palm.rules`.

First find out which groups you are actually a member of, so you can pick
one that already applies to you:
```shell
id
```
This prints something like:
```
uid=1000(judd) gid=1000(judd) groups=1000(judd),20(dialout),46(plugdev)
```
The names in `groups=` are the ones you may use.  In this example either
`dialout` or `plugdev` would work and plugdev is the preferred one.  Remember this group.

If you need to add yourself to a group you can do so with the below command.  Note: This will require you to logout and back in for the changes to take effect.
```shell
sudo usermod -a -G plugdev $USER
```

Find your device's vendor id by running `lsusb` while the Palm is
connected and syncing; Palm, Inc. devices use `0830`, but Handspring, Sony
and other makers each have their own id.  Then create
`/etc/udev/rules.d/60-palm.rules` containing:
```
# Give the Palm's USB device node a group the user is a member of.
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0830", MODE="0660", GROUP="plugdev"
```
Replace `GROUP="plugdev"` with whichever group you remembered from the `id` command above.  Replace the `ATTR{idVendor}` value with your device's id if it is not a Palm.

Then to ask udev to read the rules and re-apply them:
```shell
sudo udevadm control --reload-rules
sudo udevadm trigger
```
The Palm only appears on the USB bus while a HotSync is actually running,
so press HotSync on the device and then check the node's ownership.  Use
`lsusb` to get the bus and device numbers, then list that node:
```shell
lsusb
ls -l /dev/bus/usb/001/007
```
It should now be group-owned by the group you chose.

##### If you use the visor module instead
The `visor` module is deprecated and is not needed for a libusb build of
pilot-link.  If you do use it (check whether it is
loaded with `lsmod | grep visor`), the Palm is presented as one or more
serial ports, `/dev/ttyUSB0`, `/dev/ttyUSB1` and so on, and you set the
sync port to one of those rather than to `usb:`.  Which node carries the
HotSync connection can vary, so if the first does not connect, try the
next one.  These are ordinary tty nodes, so it is membership in the
`dialout` group that grants access to them, not `plugdev` — the USB rule in
the previous section does not apply.  Add yourself to it with (again, you
must logout and back in for this to take effect):
```shell
sudo usermod -a -G dialout $USER
```

Because the number can change from sync to sync, the classic pilot-link
convention is a `/dev/pilot` symlink pointing at whichever node the Palm
gets, which you can create with a udev rule on the tty:
```
SUBSYSTEM=="tty", ATTRS{idVendor}=="0830", GROUP="dialout", MODE="0660", SYMLINK+="pilot"
```
`/dev/pilot` is not in J-Pilot's port drop-down, so select "other" and
type it in.  Command-line pilot-link tools accept it the same way:
```shell
pilot-xfer -p /dev/pilot -l
```

### Profiles: more than one handheld
Each handheld can have its own profile: a complete set of J-Pilot data (databases, settings, backups).  The default profile is `~/.jpilot` itself, so nothing changes until you make another one.  Other profiles live in `~/.jpilot/profiles/<name>/`.

- **File -> Profiles...** lists the profiles and the handheld each one was last synced with, and creates, opens, renames and deletes them (delete moves the profile to the Trash).  The window title shows the open profile.
- `jpilot --profile "Tungsten C"` and `jpilot-sync --profile "Tungsten C"` open a profile, creating it if needed.  Without `--profile`, the last profile used is opened.  `--profile Default` is `~/.jpilot`.
- When a handheld syncs, its data goes into the profile last synced with it, whichever profile is open, and J-Pilot then opens that profile.  A handheld no profile knows yet goes into the open profile if that has never been synced, otherwise into a new profile named after the handheld's user.  This can be turned off in the Profiles window.
- On a profile's first sync, J-Pilot checks whether the handheld uses the newer Calendar, Contacts and Memos databases (Palm OS 5.2.1 and later, e.g. the Tungsten T3) and sets the profile up to match.

To give a new handheld a name of your choosing, create the profile and open it before the handheld's first sync.

The JPILOT_HOME environment variable still works: it moves the whole of `.jpilot`, profiles included, to another directory.

### Syncing
Normally a sync takes two presses: Sync in J-Pilot, then the HotSync button on the handheld.

With **File -> Listen for HotSync** turned on, J-Pilot is always waiting for a handheld, so pressing HotSync on the handheld is enough.  Together with profiles, each handheld then syncs into its own profile by itself.  Cancel stops listening.  The setting is kept in `~/.jpilot/profiles.ini`.

On macOS the sync runs in a separate `jpilot-sync` process, so J-Pilot stays responsive while it waits for the handheld.

### Handheld Explorer
**File -> Handheld Explorer...** opens a window for looking at what is on the handheld and moving things on and off it, without syncing.  Press HotSync on the handheld to connect; the connection stays open until you click Disconnect or close the window.

- **Internal memory** lists the databases and applications with their type, creator, size, number of records and date, and shows the free memory.  *Copy to Computer* saves them as `.pdb`/`.prc`/`.pqa` files.  Databases that just hold a file are saved as that file: the Photos application keeps each picture as one, so `3.jpg` comes out as a real JPEG.  *Copy to Handheld* installs `.prc`, `.pdb` and `.pqa` files.  *Delete* removes databases from the handheld; take care with the ones the handheld itself uses.
- **Memory cards** (Palm OS 4 and later) can be browsed folder by folder.  Files and whole folders can be copied to the computer, files copied into the current folder of the card, folders made, and files and folders deleted.

Files copied to the computer never replace files already there, and keep the dates they have on the handheld.  While the explorer is open, listening for HotSync is paused.  Always end with Disconnect (or by closing the window or J-Pilot): a handheld whose connection disappears without that may hang on its sync screen until it is reset.

### BACKUP and SYNC
The Sync button syncs the 4 applications with the palm pilot and any plugins that are installed.  By default a sync DOES NOT back up your palm.

The Backup button syncs and then backs up every program and database from the palm pilot, except for AvantGo files (these are usually big and change daily).

To back up on every sync, turn on **Back up the handheld on every sync** in File -> Preferences -> Settings.  It is a setting of the profile, so it applies whichever way the sync starts (the Sync button, listening for HotSync, or jpilot-sync).  Backups are incremental: each one starts as a copy of the previous one and only fetches the databases that changed, so after the first one they are quick.

Backups are kept in the profile's directory as `backupYYYYMMDDhhmm`, with `backup` pointing at the newest; "Number of backups to be archived" in the same preferences tab says how many are kept.  Databases that have been deleted from the handheld are moved to `backup_removed`.  File -> Restore Handheld restores the newest backup.

If you get an error saying that you have a NULL user ID, then you need to run install-user from the pilot-link suite.  On Debian and Ubuntu it is installed as `pilot-install-user`.
```shell
pilot-install-user -p usb: -u "Judd" -i 12345
```
Of course replace "Judd" and "12345" with you favorite name and number.  PalmOS requires the user ID to be a 5-digit number.  Use the same port here that you use for syncing, so `/dev/ttyUSB1` (or whichever node) instead of `usb:` if you are using the visor module.

### Building a macOS app
`macos/make-app.sh` builds `J-Pilot.app` and installs it in `/Applications`, so J-Pilot can be started from the Finder, Launchpad or Spotlight like any other application:
```shell
macos/make-app.sh
```
It needs Homebrew (`gtk+3 libgcrypt sqlite gettext intltool autoconf automake libtool pkg-config imagemagick`) and pilot-link 0.15 or later, looked for in `~/.local` (set `PILOT_PREFIX` to use another place).  pilot-link is copied into the app; GTK and the other Homebrew libraries are not, so the app runs on Macs that have them installed.  The app works only in the folder it was built for: to put it somewhere else, give that folder as the argument.  Run the script again to rebuild the app after changing J-Pilot or pilot-link.  J-Pilot's data stays in `~/.jpilot`.

### Building a Debian (or ubuntu) package
From the repo root, with the tree already configured (e.g. after ./configure):

1. Ensure debian/changelog has an entry for the version in configure.in (e.g. 2.1.1-1).
2. Run:
   ./build-debian.sh
3. Collect the built artifacts in the current directory:
	jpilot-1.8.2.tar.gz
	jpilot-1.8.2.tar.gz.asc
	jpilot-1.8.2.tar.gz.md5sum
	jpilot-2.1.1.tar.gz
	jpilot-dbgsym_2.1.1-1_amd64.ddeb
	jpilot-plugins-dbgsym_2.1.1-1_amd64.ddeb
	jpilot-plugins_2.1.1-1_amd64.deb
	jpilot_2.1.1-1.debian.tar.xz
	jpilot_2.1.1-1.dsc
	jpilot_2.1.1-1_amd64.buildinfo
	jpilot_2.1.1-1_amd64.changes
	jpilot_2.1.1-1_amd64.deb
	jpilot_2.1.1.orig.tar.gz

#### For a new upstream version
1. update the jpilot version in configure.ac in the AC_INIT function.
2. Add a change in debian/changelog
3. run ./build-debian.sh.
