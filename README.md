# NeoComp (Compton)

__NeoComp__ is a fork of __Compton__, a compositor for X11

NeoComp is a (hopefully) fast and (hopefully) simple compositor for X11,
focused on delivering frames from the window to the framebuffer as
quickly as possible.

It's currently very much in development (Compton is extremely crufty and
rigid), so I don't expect it to be stable.

### How to build

To build, make sure you have the dependencies (yeah I know) then run:

```bash
# Make the main program
$ make
# Make the man pages
$ make docs
# Install
$ make install
```

## Usage

Please refer to the Asciidoc man pages (`man/compton.1.asciidoc` & `man/compton-trans.1.asciidoc`) for more details and examples.

Note a sample configuration file `compton.sample.conf` is included in the repository. (The sample configuration is in need of updating).

## License

I don't know of the lineage behind Compton. All contributions made by me
are GPL. If any previous contributor would like to claim ownership over
some part and dispute the license, please open an issue.

NeoComp is licensed under GPLv3
