# The Glowforge cloud service

Revision: 1 (2026-09-06)

Cloud mode is optional and off by default. Tick the box to acknowledge this document. If you turn cloud mode on later, the cloud step asks you to type "I UNDERSTAND" again.

## What cloud mode is

In cloud mode the machine speaks the factory protocol to the Glowforge service. By default it signs in with the factory identity on the control board. The cloud step and the GF Cloud tab can take a typed serial and password instead. Anyone who has that pair can stand up as that machine.

The session and the socket name this firmware. Their user agent is `ForgeFIRM/<version>`, where the version is the ForgeFIRM version on the machine. Image uploads go to a storage host the service names and use a different client name. When the service asks for settings, the report still names a factory firmware version and a factory app build.

The Glowforge app then drives the machine as it drives a machine on factory firmware. Homing, focus, material imaging, and prints work through the app. The machine keeps its own safeguards in this mode too.

A GRBL homing choice can use the service as well. That choice signs in, sends the settings report, and runs the service hunt on every home. It talks to the service even when you are not in cloud mode.

ForgeFIRM is not affiliated with or endorsed by Glowforge.

Turning cloud mode on may be enough, under Glowforge's terms, for Glowforge to disable the account or the machine. The project cannot advise you on those terms.

## Your responsibility

Terms apply to your Glowforge account and to your machine. It is up to you to make sure that your use of the service through this firmware stays within them. The project cannot advise you on them. Do not ask the project to interpret them.

Replacing the factory firmware, and using this mode, may also void a warranty. The Safety and risk document covers that.

## The risk

Glowforge may refuse, limit, or block a machine that runs software it did not make. The session user agent names ForgeFIRM. The settings report still names a factory firmware version. The project cannot prevent a refusal. The project cannot reverse it. If that matters to you, leave cloud mode off and do not choose cloud homing.

Cloud mode talks to a service and a factory firmware combination the project does not control. Those can change at any time. This firmware tracks a factory version it has tested. If the vendor moves, cloud mode may stop working until a new ForgeFIRM release pins the new combination. The control panel may warn you when the factory version string the service advertises changes. Cloud mode is never represented as always working.

## The identity

Each machine has an identity in the factory fuses of its control board: a serial number and a password. The Glowforge service knows the machine by them. Nobody can change or rotate the fuses. A leaked identity cannot be replaced. Anyone who has the serial and the password can stand up as this machine. That is why cloud mode is off by default.

The cloud step and the GF Cloud tab can send a typed serial and password instead of the fuses. That is the same standing-up. Keep whatever pair you use secret. Do not post it in an issue report. The sanitized log export masks it.

## What is sent

In cloud mode, and during cloud homing, the machine sends these things to the Glowforge service:

- The machine identity, at sign-in. That is the fuse pair, or the typed pair if you set one.
- The firmware name and version, on the session and the socket, as the user agent `ForgeFIRM/<version>`.
- A factory firmware version and a factory app build, in the settings report the service asks for.
- The head's serial, hardware id, and firmware version, and the clock time.
- Lid and button events, on every open, close, press, and release, for as long as the connection is up.
- The lid and head camera images the service asks for. The lid must be closed, or the request fails. The upload goes to a storage host the service names.
- Machine settings and status, when the service asks.
- The jobs you print. Your designs live in the app. The service prepares each job and sends it to the machine. The machine reports progress and the result. While a job runs it also sends the X and Y step positions every thirty seconds.

The machine sends no continuous sensor telemetry. It refuses a factory reset and a head firmware update from the service. It logs the refusal and answers the service with a failure. The project may add support for a head image only after it reviews and tests that image.

## What you acknowledge

When you tick the box, and when you type "I UNDERSTAND" to turn cloud mode on, you acknowledge these points:

- ForgeFIRM is not affiliated with or endorsed by Glowforge.
- Turning cloud mode on may be enough for Glowforge to disable the account or the machine under their terms. It is up to you to keep your use of the Glowforge service within the terms that apply to you. The project cannot advise you on those terms and cannot reverse a ban.
- Glowforge may refuse, limit, or block this machine. The project cannot prevent or reverse that.
- Cloud mode is not represented as always working. The vendor can change the service or the factory firmware at any time. This firmware tracks a tested combination and may stop working until a new ForgeFIRM release. The panel may warn when the advertised factory version string changes.
- Nobody can change or rotate the fuse identity. A leaked identity cannot be replaced. A typed serial and password stand up as that machine the same way.
- The machine sends the identity, the ForgeFIRM name and version on the session, factory version strings in the settings report, head identity, clock time, lid and button events, camera images, settings, status, and job data.
- Cloud homing uses the service too.
- Cloud mode is off by default. You can turn it off. A client that is already signed in stays up until the next controller start, a mode switch, or a reboot.
