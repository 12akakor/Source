# Sound Open Firmware for Intel Bay Trail

This branch only holds a GitHub workflow that builds the Sound Open
Firmware (SOF) image for the audio DSP of Intel Bay Trail (`sof-byt.ri`)
from source, for the `intel_lpe` audio driver of RenkuOS.

- SOF is BSD-3-Clause; Bay Trail was last supported in branch `stable-v2.4`.
- The Xtensa cross toolchain is built from SOF's crosstool-ng fork
  (`sof-gcc10x`, config `config-byt-gcc10.2-gdb9`) and cached.
- Each build publishes a release with `sof-byt.ri`, the SOF licence and the
  source revisions.
