# Extensions

Revision: 3 (2026-09-23)

Extensions are optional and off by default. This document is shown when you turn them on. To turn them on, type "I UNDERSTAND".

## What an extension is

An extension package is software that is not part of ForgeFIRM. Somebody else can write it. You install it on the machine, and it can run there for as long as the machine is on.

The project does not test or vouch for a package unless the package is signed with the OpenGlow extension key. The machine tells you which kind a package is before you install it:

- **Official.** Signed with the OpenGlow extension key.
- **Community.** Signed with a key that you added to the machine, or with its author's key that OpenGlow's catalog names for that package. The machine asks for your typed consent. OpenGlow read a community package in its catalog before it listed it; that is not a test.
- **Unverified.** Signed by nobody the machine trusts, or not signed at all. The machine asks you to hold the button, as it does for unsigned firmware.

A signature says who made a package. It does not say the package is safe, correct, or useful.

## What the machine does about it

A package runs in a sandbox. It runs as its own account, with limits on processor time, memory, and the number of its processes. It sees its own files and the read-only parts of the system, and nothing else on the machine. It can send to the network only where its list of capabilities says, and never to the machine itself. It cannot open the laser, the motion hardware, the cooling hardware, the cameras, the settings, the firmware, or the login of the machine directly.

A package asks for capabilities, and the machine shows them to you before the install. Some capabilities need a grant from you for that one package: to hold a job, to run a program as the sender, and to keep running while a job is armed. An update that asks for more shows you what is new.

While a job is armed, every package is frozen. It does not run at all until the job is over. The one exception is a package that you allowed to keep running, and that package runs under tighter limits for that time.

The safeguards of the machine do not depend on any package. The lid, the interlock, the armed window and its button, the cooling gates, and the limits on motion work the same with extensions on. No capability reaches past them. The most a package can do to a job is hold it.

A package that keeps ending is set aside until you look at it. There are two ways to stop every package at once: turn extensions off in the settings, or make the file `/run/forgefirm/ext-safe` at the console, which is safe mode. Safe mode is the one to use when you want the setting left as it is.

## The risk

A sandbox lowers the risk. It does not remove it. A flaw in the sandbox, in the kernel, or in this firmware could let a package do more than its capabilities say.

Within its capabilities, a package can still do harm. A package that can read the machine's status, or follow its events, can send what it learns to the destination you allowed: what you cut, when you cut it, and when you are at the machine. A package that can hold a job can stop you cutting, at any time and for its own reasons. A package that runs at all can use the machine's one processor and its storage.

A package that can use a camera can take a picture of the inside of the machine and send it to the destination you allowed. A package that can jog can move the head, and it can do that while you have your hands inside the machine: the lid being open does not stop a jog, and nothing but you watching the machine will tell you it is about to move. A jog is bounded and it never fires the laser, and it is still motion you did not ask for.

A package with a page of its own can send a little data out of your browser while you have that page open. The page cannot reach the machine or the network by itself, and it cannot read your session; what it can do is a browser gap this project cannot close, and a package without a page cannot do it at all.

A package that can run a program starts a job the way you would, and the job then waits for the button and stands under every gate and limit your own jobs do.

A package that answers an M-code holds a job of yours that names it at that M-code until it answers: the head stands still and the laser is dark while it waits. If it does not answer in 30 seconds, or says it could not do its part, the job is held for you to resume or stop.

This firmware serves the capability list: reading the machine, following its events, holding a job, its own settings, a camera, a jog, a page of its own, running a program, answering an M-code, and a check of its own on the Setup page. Read the list as what you are trusting the author with.

A package can stop working, or can hold every job, after a firmware update or after its author changes something on their side. You can always turn it off or remove it.

Packages keep their own data on the machine, and that data can hold credentials for the services they talk to. Removing a package removes its data, unless you ask to keep it. Resetting your password does not touch packages, and neither does a factory return: to be rid of a package and its data, remove the package.

If you report a problem with the machine, first see whether it happens in safe mode. The project cannot debug a machine that runs software it did not write.

## What you acknowledge

When you type "I UNDERSTAND" to turn extensions on, you acknowledge these points:

- An extension package is not part of ForgeFIRM. The project does not test or vouch for a package that is not signed with the OpenGlow extension key, in its catalog or not.
- The sandbox lowers the risk of a package and does not remove it.
- The capabilities of a package, and the grants you give it, are your decision. They are a list of what you trust its author with.
- You can turn extensions off, enter safe mode, or remove a package at any time, and you will do that before you report a problem with the machine.
