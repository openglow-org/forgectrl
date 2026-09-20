# Remote API example

A client of ForgeFIRM's remote API, to run on another computer. It is not
part of the image.

The documentation is on the site:
<https://docs.forgefirm.org/technical/forgefirm/remote-api/>.

- `notify.py` follows the machine's event stream with a scoped token that
  holds `events`, and tells somebody when the machine wants them (a job
  waits for the button, pauses, ends, or the controller raises an alarm): a
  line of text POSTed to a URL, or handed to a command. Python 3, standard
  library only. `python3 notify.py --help` has the options.
