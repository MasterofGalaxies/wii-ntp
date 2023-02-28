# A Network Time Protocol client for the Wii

## Building

Run `make` with the variable `DEVKITPPC` set to the path of devkitPPC (as an environment variable or an argument passed thereto).

You can also set the variable `DEFAULT_NTP_SERVER` to override the default server of `pool.ntp.org` (set in `source/ntp.c`).

## Usage

In its simplest usage, launch the application without any arguments, at which point you can select the UTC offset of your time zone with the D-Pad of any connected Wii Remote or GameCube controller. After selecting your time zone, press A to continue.

Additionally, this application supports the following arguments:
```
-s, --server <server>		the NTP server to use (default: time.nist.gov)
-t, --timezone <UTC offset>	the UTC offset of your time zone
-a, --auto			set the time without any user interaction,
				with a 5 second period to be interrupted
				(overrides previous instances of -w)
-w, --wait			wait for the user to press A to set the time,
				allowing for the UTC offset to be selected
				(default, overrides previous instances of -a)
```

## License

I dedicate the contents of this repository to the public domain via Creative Commons CC0 1.0 Universal \(a copy of which can be found in `LICENSE.txt`\). Additionally, this code contains several excerpts from systemd-timesyncd, licensed under the GNU Lesser General Public License as published by the Free Software Foundation, either version 2.1 of the License, or (at your option) any later version.
