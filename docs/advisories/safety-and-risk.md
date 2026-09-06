# Safety and risk

Revision: 1 (2026-09-06)

Read this document before you use the machine. It is short on purpose. Anyone who runs a job must have read it. Keep these rules posted at the machine. At the end, type "I UNDERSTAND" and press the button on the machine.

## What this firmware is

ForgeFIRM is community software. It replaces the factory software in the machine. It is in development. It can have faults the factory firmware does not have. It can fail in ways nobody has seen yet. It is not affiliated with or endorsed by Glowforge.

Use of this software can cause serious injury or death to you or to other people. It may void your warranty. You use it at your own risk.

## What a CO2 laser does

The laser in this machine is a CO2 laser. Its beam is invisible. It burns what it touches. It can burn skin. It can blind you, and that damage is permanent. It can start a fire in the machine and in the room.

The fumes from cut material are toxic. Many of them are also flammable. Some materials release gases that corrode the machine and harm your lungs. Reflective stock can bounce the beam out of the path you expect.

## What protects you, and what does not

The machine's own factory safeguards do the last-line work. The lid switches, the interlock plug on a Pro, and the big button are wired so the laser cannot fire with the lid open, and cannot fire at all until a person presses the button. ForgeFIRM keeps those factory safeguards as they are.

The enclosure and the lid glass are your eye protection only while they are closed and undamaged. A Basic or Plus with an intact case and lid is a closed enclosure. A Pro has a pass-through. When that slot is open, the enclosure is not closed. Local rules may treat that as a different class of laser product.

The button press is also your consent in software. ForgeFIRM waits for your press before the first cut of a job. One press covers the job until the program ends, or until about a minute of idle with the beam off. That wait is a setting. A second job that starts inside the wait can run on the same press. After the wait, the laser locks again and asks for the press again.

ForgeFIRM adds its own checks on top of those factory safeguards. It holds or locks for fire, a head crash, a fan that is not running as commanded, coolant that is too hot or not moving, and a control program that goes silent or dies. An airflow hold and a serious coolant hold do not resume. Reset that job. Some other holds can clear and resume. Opening the lid stops the beam at once. By default the job ends. A setting on the GRBL tab can hold the job instead and let it resume after the lid closes. The button is still required before the beam returns.

The software checks are software. They are help, not a guarantee. The fire watch catches a developed fire, not a small flame. It is not a fire alarm. A hold can fail to happen. After a fault, the machine's idea of the head position can be wrong.

You are the safeguard that must work. Nothing in the machine replaces a person who watches it.

## The rules

- Stay with the machine for the whole job. Watch the material. Small flare-ups happen with some materials. A flame that continues to burn is a fire.
- Do not leave a child, a pet, or a person who has not read this as the watcher.
- Know how to stop before you start. Open the lid: the beam stops at once, and by default the job ends. Press the button: the job pauses. Press Stop in your sender. Turn the machine off. If anything looks wrong, stop first and think second.
- Keep a fire extinguisher in reach. Know how you will open the lid and smother a flame.
- Vent the exhaust outdoors. The fumes are toxic and flammable. A filter does not make them safe. Some materials still need outdoor exhaust.
- Know your material. Never cut PVC, vinyl, or any chlorinated plastic.
- Every job with a laser layer fires, however low the power is set. If the button lights and you did not expect it, press Stop.
- Read the messages. When the machine holds or refuses, the sender console and the control panel say why. Find the cause before you retry. An airflow hold and a serious coolant hold do not resume. Reset that job. Do not treat them as a pause.

## Do not bypass a safeguard

- Never defeat a lid switch or the interlock plug. Never run with a cover off. Never change the safety wiring on the control board. Do not run if the lid glass or the case is damaged or modified.
- Do not place magnets near the lid. They can fool a lid switch.
- Never tape, wedge, or wire the button. The press is your consent, once per job.
- Do not turn a cooling safeguard off unless you understand what you give up. The settings that can do it say so. They show a flagged value and a banner on the Status tab. They write a line in the log at every job start. No setting can turn off the factory lid, interlock, or button protections.
- Do not change the lid setting so a job holds instead of ending unless you understand what you give up. The beam is still off with the lid open. The button is still required before it returns.
- Do not work around an alarm or a hold without knowing its cause.
- Run a build you can match to the published source. A sold image is the seller's build, not this one.

## No warranty

This software comes with no warranty of any kind. Its licenses say so in their own words.

The MIT License says: "THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED". The GNU General Public License says: "THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW". The Licenses and notices document names each license and where its full text is.

## Responsibility

Modifying the machine, including replacing its firmware, may have legal and regulatory ramifications. It is up to you, the end user, to make sure that you adhere to all laws, regulations, certifications, and insurance terms that apply where you are. The project cannot advise you on them.

## What you acknowledge

When you type "I UNDERSTAND" and press the button on the machine, you acknowledge these points:

- This is community firmware, not the manufacturer's firmware. It is in development. It is not affiliated with or endorsed by Glowforge.
- Use of it can cause serious injury, death, fire, and property damage. It may void your warranty.
- The factory safeguards and the software checks are help, not a guarantee. You are the safeguard.
- Anyone who runs a job must have read this. You will keep these rules posted at the machine.
- You have read the rules, and you know how to stop the machine.
- You have read the list of safeguards, and you will not bypass one.
- The software comes with no warranty of any kind.
- It is up to you to adhere to all laws, regulations, certifications, and insurance terms that apply where you are.
- You use this software at your own risk.
