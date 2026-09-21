# Extensions

Revision: 1 (2026-09-21)

Extensions are optional and off by default. This document is shown when you turn them on. To turn them on, type "I UNDERSTAND".

## What an extension is

An extension package is software that is not part of ForgeFIRM. Somebody else can write it. You install it on the machine, and it can run there for as long as the machine is on.

The project does not review, test, or vouch for a package unless the package is signed with the OpenGlow extension key. The machine tells you which kind a package is before you install it:

- **Official.** Signed with the OpenGlow extension key.
- **Community.** Signed with a key that you added to the machine. The machine asks for your typed consent.
- **Unverified.** Signed by nobody the machine trusts, or not signed at all. The machine asks you to hold the button, as it does for unsigned firmware.

A signature says who made a package. It does not say the package is safe, correct, or useful.

## What the machine does about it

A package runs in a sandbox. It runs as its own account, with limits on processor time, memory, and the number of its processes. It sees its own files and the read-only parts of the system, and nothing else on the machine. It can send to the network only where its list of capabilities says, and never to the machine itself. It cannot open the laser, the motion hardware, the cooling hardware, the cameras, the settings, the firmware, or the login of the machine directly.

A package asks for capabilities, and the machine shows them to you before the install. Some capabilities need a grant from you for that one package: to hold a job, to run a program as the sender, and to keep running while a job is armed. An update that asks for more shows you what is new.

While a job is armed, every package is frozen. It does not run at all until the job is over. The one exception is a package that you allowed to keep running, and that package runs under tighter limits for that time.

The safeguards of the machine do not depend on any package. The lid, the interlock, the armed window and its button, the cooling gates, and the limits on motion work the same with extensions on. No capability reaches past them. The most a package can do to a job is hold it.

A package that keeps ending is set aside until you look at it. Safe mode stops every package. To enter safe mode, turn extensions off, or make the file `/run/forgefirm/ext-safe` at the console.

## The risk

A sandbox lowers the risk. It does not remove it. A flaw in the sandbox, in the kernel, or in this firmware could let a package do more than its capabilities say.

Within its capabilities, a package can still do harm. A package that can read the machine's status can send it to the destination you allowed. A package that can use a camera can take pictures when the lid is closed and send them there. A package that can jog the head can move it while you are near it. A package that can run a program can start a job, and the job then waits for the button like any other job. Read the capabilities as a list of what you are trusting the author with.

A package can stop working, or can hold every job, after a firmware update or after its author changes something on their side. You can always turn it off or remove it.

Packages keep their own data on the machine, and that data can hold credentials for the services they talk to. Removing a package removes its data. The forgotten-password reset offers to remove every package and its data.

If you report a problem with the machine, first see whether it happens in safe mode. The project cannot debug a machine that runs software it did not write.

## What you acknowledge

When you type "I UNDERSTAND" to turn extensions on, you acknowledge these points:

- An extension package is not part of ForgeFIRM. The project does not review or vouch for a package that is not signed with the OpenGlow extension key.
- The sandbox lowers the risk of a package and does not remove it.
- The capabilities of a package, and the grants you give it, are your decision. They are a list of what you trust its author with.
- You can turn extensions off, enter safe mode, or remove a package at any time, and you will do that before you report a problem with the machine.
