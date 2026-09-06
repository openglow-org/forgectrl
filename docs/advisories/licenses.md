# Licenses and notices

Revision: 1 (2026-09-06)

This document names the major software on this machine and the license of each part. It is a notice, not a contract. Tick the box when you have read it.

## ForgeFIRM components

- **forgectrl**, the machine-services daemon and the control panel. MIT.
- **grblHAL-glowforge**, the GRBL controller driver, with the grblHAL core. GPL-3.0-or-later. The grblHAL core is copyright Terje Io and contributors, and names Sungeun K. Jeon, Simen Svale Skogsrud, and Jens Geisler. The platform files also carry Terje Io's copyright. The platform layer derives from the grblHAL Simulator, copyright Jens Geisler and Adam Shelly.
- **kernel-module-glowforge**, the kernel driver for motion, the laser latch, the safety readbacks, and the sensors. GPL-2.0-or-later. Copyright Scott Wiederhold and Glowforge, Inc. The inherited factory module was written by Matt Sarnoff with contributions from Taylor Vaughn.
- **python3-gfhardware**, the hardware library and the cloud-mode client. MIT, with one component under another license. The Bayer demosaic routines come from libdc1394 (Damien Douxchamps, Frederic Devernay, and Dave Coffin). The AHD port is by Samuel Audet. They are LGPL-2.1-or-later. The repository has the full source of both parts and the standard build. Anyone can change the LGPL part and build the module again. The evdev input path derives from python-evdev (Georgi Valkov), BSD-3-Clause.
- **Glowforge-Utilities** (gfutilities), the factory protocol layer that cloud mode uses. MIT.
- **The documentation.** The docs site is CC BY-SA 4.0. These four documents are part of forgectrl and carry its MIT license.

## Third-party software on the image

| Software | What it does here | License |
|---|---|---|
| Linux kernel | The operating system kernel | GPL-2.0-only |
| The Yocto Project (Poky) | Builds the image and supplies the base packages | MIT for the build metadata; each package keeps its own license |
| BusyBox | The core command-line tools | GPL-2.0 |
| glibc | The C library | LGPL-2.1 |
| OpenSSH | Shell access over the network, off by default | BSD |
| rsyslog | The system logger | GPL-3.0, with LGPL-3.0 and Apache-2.0 parts |
| ntp | Time synchronization | NTP License |
| Python 3 | The runtime for cloud mode and homing | PSF-2.0 |
| ulfius | The HTTP framework of forgectrl | LGPL-2.1 |
| orcania | Utility library for ulfius | LGPL-2.1 |
| yder | Logging library for ulfius | LGPL-2.1 |
| jansson | JSON library | MIT |
| libmicrohttpd | The HTTP server library | LGPL-2.1 |
| curl | HTTP client for the release check | curl license |
| requests | HTTP client for the cloud sign-in | Apache-2.0 |
| urllib3 | HTTP library used by requests | MIT |
| websocket-client | Cloud-mode socket client | Apache-2.0 |
| certifi | CA bundle used by requests | MPL-2.0 |
| charset-normalizer | Character detection used by requests | MIT |
| idna | Internationalized domain names used by requests | BSD-3-Clause |
| GnuTLS | Encryption for the control panel connection | LGPL-2.1 |
| fwup | Applies signed firmware updates | Apache-2.0 |
| Mesa | GPU drivers for the camera image pipeline | MIT |
| libjpeg-turbo | JPEG encode and decode for the cameras | IJG, BSD-3-Clause, and zlib |
| wpa_supplicant | WiFi | BSD |
| avahi | Network discovery, so the panel is reachable by name | LGPL-2.1 |
| Bootstrap | The layout and styles of the control panel | MIT |
| NXP i.MX firmware | VPU firmware for the i.MX6 | NXP EULA, shipped at /usr/share/licenses/firmware-imx/EULA |
| TI WL18xx firmware | The WiFi module firmware | TI firmware license, shipped with linux-firmware |
| TI wlconf | The wl18xx configuration file and the wlconf tool | GPL-2.0-only |

The SDMA assembler by Eli Billauer assembles the motion-engine microcode at build time. It is GPL-2.0-or-later. It is not on the image. The motion microcode on the machine is the project's own source.

The panel footer has a Licenses link on every page. That page shows the Yocto license manifest. It also offers a download of the license bundle on the machine at `/usr/share/forgefirm/licenses.tar.gz`. The bundle holds the manifest and the text of each license the image ships, including packages this list does not name.

The NXP EULA stays on the machine at the path in the table. Keep it there in each image that you redistribute. If you ship an image, you carry the corresponding-source obligation for the GPL and LGPL parts. This project's GitHub organization is the upstream, not a substitute, unless you publish your own tree.

## The Hershey fonts

The text burned on the commissioning sheet uses the Hershey fonts. Their notice asks for these acknowledgments:

- The Hershey Fonts were originally created by Dr. A. V. Hershey while working at the U.S. National Bureau of Standards.
- The format of the font data in this distribution was originally created by James Hurt, Cognition, Inc., 900 Technology Park Drive, Billerica, MA 01821 (mit-eddie!ci-dandelion!hurt).

The notice permits anyone to use and distribute the font data for any purpose, with these acknowledgments. The font data can be converted into any other format, except the format that the U.S. NTIS distributes.

## Where the source is

Every ForgeFIRM component is open source. The repositories are on GitHub, under the openglow-org organization. The documentation is at https://docs.forgefirm.org/. Each repository has its license text and the copyright notices of its parts.

## This firmware is not for sale

This firmware has no paid tier, no license key, and no subscription. The project may sell accessories. Those are a different product. If someone sells you a build of this firmware, the licenses allow that. The build you get is theirs, not this one. Get the firmware from the source.

## The licenses grant rights

The licenses grant rights. They let you use, study, change, and share the software under their terms. This acknowledgment adds no restriction to any of them. It adds no condition to any right a license gives you. It records only that you have seen this notice.
