# Privacy

Revision: 1 (2026-09-06)

This document says what the machine sends, what it keeps, and who can see it. Tick the box when you have read it.

## What the machine sends, and when

The machine talks to these places, and to nothing else:

- **Your own network.** A responder on the machine announces the panel's names and addresses on the local network so other devices can find it.
- **Time sync.** The clock has no battery. At boot, and at intervals after that, the machine asks a public NTP pool for the time. The pool sees the machine's network address.
- **The release check.** The machine checks for a new release once a day, and when you ask for it in the panel. The check asks the release host's API what is published. It does not send your version or your identity. The release host sees the machine's network address. The request uses the HTTP client's own name, not ForgeFIRM. The panel shows an alert when a newer release is published; the machine downloads nothing until you start the install.
- **The Glowforge service.** The machine contacts it only if you turn cloud mode on, or if you choose cloud homing for GRBL. The session and the socket name this firmware as `ForgeFIRM/<version>`. Image uploads go to a storage host the service names, with a different client name. Cloud mode also contacts the vendor status host and asks what factory version is current. The Glowforge cloud service document says the rest of what it sends.
- **Remote logging.** The machine forwards log lines only if you set a log server in the panel. It is off by default.

The machine sends nothing else. There is no telemetry, no analytics, and no crash reporting to the project.

## The cameras

The machine has two cameras. One is in the lid and looks down at the bed. One is in the head and looks at the material under the lens. Neither camera captures anything while the lid is open. This covers the live view, snapshots, and every image the Glowforge service asks for.

The reason is where the lid camera points. When you raise the lid, the lid camera faces the room. The rule removes the question. A closed lid is the condition for an image to exist.

On the ordinary path the machine reads a frame from the camera, encodes it, sends it, and frees it. It does not write an image file.

The commissioning camera check is an exception. It keeps the last lid picture and the last head picture in memory until a new check replaces them or the machine services restart. Those pictures are served on the local network at a read route while open reads are allowed.

In cloud mode, the Glowforge service asks for images. The machine sends them to the storage host the service names. That is the only way an image leaves your network.

Two optional switches in the cloud-client file can keep a copy of each image sent to the service, and of each pulse file. Both are off by default. The panel has no control for them. If you turn either on, those files stay on the machine until you delete them. Do not copy that folder into a post or a log export.

## The control panel connection

The control panel uses an encrypted connection. Its certificate is one the machine made itself. Your browser does not know that certificate, so it shows a warning the first time you connect. That warning is expected.

Before you accept the warning, open `http://<the-machine>/cert` on the same network. That page shows the certificate fingerprint. Compare it with the fingerprint your browser shows. Accept the certificate only if they match. After that, the System tab shows the same fingerprint so you can check it again.

The panel login is the username and password you set during commissioning. There is no default password. The panel password, and everything you change, travel encrypted on your network.

The status view and the camera views are different. Any device on your network can read them unless you close the open-read setting. That setting has no control on the panel. It is `panel_open_reads`, on by default. Turning it off closes every read route, not only status and cameras: settings, mode, cooling, the wizards, the documents, the licenses, and the rest. Set it through the settings interface.

The panel also shows a camera key. A program that has the key can read without a login, with the views open or closed, over the encrypted connection or over plain HTTP. The key opens every read route, not only the cameras and the status: the same list as the open-read setting. Treat the key as a secret. A key in a URL travels in the clear on your network and can land in a sender's log. You can make a new key at any time. Make a new key if the old one leaks.

## The logs

The machine logs its own programs: the machine services, the controller, the cloud client, the kernel, and the system. They hold events, faults, settings changes, and job state. They stay on the machine unless you set a log server. The machine rotates them and caps their size.

You can download a log export from the panel. By default the export is sanitized. It replaces the serial number, the machine name, and the network names with placeholders. It also replaces network addresses, credentials, the camera key, and other secrets it recognizes. The sanitizer removes what it knows and what it can recognize. Skim the export before you send it.

## The commissioning sheet id

The commissioning sheet carries an id. The id is a code derived from the serial number with a secret kept on the machine. It does not reveal the serial number. Nobody can compute the serial number from the id without the secret. The sheet carries no serial number, no machine name, and no network address. You can share a photo of the sheet.

## A request to beta testers

The project learns from real machines. Please send a sanitized log export to the project by private message on the community forum at https://community.openglow.org. Send it after commissioning, and after any fault you do not understand. Do not post it in a public thread. This is voluntary. The machine never sends a log to the project on its own. A maintainer uses your log to diagnose your machine and then deletes it. The project does not publish it and does not keep a log archive.
