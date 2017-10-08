This is a fork of [Compton](https://github.com/chjj/compton) that adds animated window transitions. It is intended for tiling window managers, but may be useful to people who use a mouse as well.

Video: https://youtu.be/eKwPkiACqF0

It adds the following options:
* `transition-length`   length of animation in milliseconds  (default: 300)
* `transition-pow-x`    animation easing on the x-axis (default: 1.5)
* `transition-pow-y`    animation easing on the y-axis (default: 1.5)
* `transition-pow-w`    animation easing on the window width  (default: 1.5)
* `transition-pow-h`    animation easing on the window height (default: 1.5)
* `size-transition`     whether to animate window size changes (default: true)
* `spawn-center-screen` whether to animate new windows from the center of the screen (default: false)
* `spawn-center`        whether to animate new windows from their own center (default: true)
* `no-scale-down`       Whether to animate down scaling (some programs handle this poorly) (default: false)
