# libsamplerate

- Official upstream: https://github.com/libsndfile/libsamplerate
- Immutable source revision: `0844c208f683527c08ea8a80acc13b398aa9c8bf`
- Upstream version at this revision: 0.2.2
- License: BSD-2-Clause, reproduced in [COPYING](COPYING). File headers and AUTHORS are retained.
- Purpose: persistent, bandlimited sample rate conversion between independent audio clocks.
- Source files are copied without modification. `SHA256SUMS` records each upstream
  file's SHA256 relative to this directory. Configuration verifies every checksum.
  Git attributes disable line-ending conversion and whitespace diagnostics for the
  immutable vendor directory, preserving upstream whitespace byte for byte.
  Only the source, headers and CMake files needed to build the library are included.
- Build: static C99 library, only the fastest sinc coefficient table enabled;
  examples, upstream tests, installation and shared library disabled in the parent
  `third_party` directory scope. Parent CTest remains enabled. No libsndfile is required.
- Tests force-include allocation hooks into the vendor translation units. Production
  builds with `BUILD_TESTING=OFF` have no hooks. Upstream source bytes stay unchanged.

`SRC_SINC_FASTEST` trades bandwidth for work per sample. Upstream documents 80%
relative bandwidth. This is not full-band transparency or bit-perfect conversion.
See the [upstream converter documentation](https://libsndfile.github.io/libsamplerate/api_misc.html).
The application currently offers no quality selector.

Queue accounting mirrors the pinned mono/stereo sinc loop's phase advance:
`last_ratio + output_index * (target_ratio - last_ratio) / requested_output_frames`
when the change exceeds `1e-10`. Input progress per generated frame is `1/ratio`.
This matters because `input_frames_used` includes frames copied into the SRC buffer,
not just frames that have already contributed to output. Review accounting and
rerun chunk, impulse, drift and allocation tests when updating the dependency.
