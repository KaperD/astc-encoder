# Roadmap

This page summarizes the major roadmap items for astcenc, as an indication of
of what we are thinking about. There are no commitments or schedules on this;
most of the engineering work here is done by volunteers.

## astcenc 3.0

The next milestone for astcenc will be a new major release, version 3.0.
Objective is to have this release around SIGGRAPH (start of August 2021), which
is one year after the 2.0 release.

The 2.x series has seen many changes to both the image quality, where we've
traded off quality to gain performance, and the core codec API. Now that we've
hit a performance point we are mostly happy with we'd like to stabilize on what
we have. The goals for version 3.0 are:

* Stable codec API.
* Stable image quality.

Primarily this will impact optimization work, where we will be applying a
policy that new changes must not reduce image quality. This doesn't rule out
more performance, but does mean that improvements are likely to be more
incremental. The one exception to this policy is the `-fastest` mode which
deliberately sets out to be as fast as possible for roughing-out, and has no
commitment to image quality ...

## astcenc training course

ASTC is a relatively feature-rich container format, and astcenc is a relatively
powerful compressor. There are some tricks for getting the best image quality
out for any given bitrate, especially for non-color data such as normal maps
and PBR material textures. We'd like to produce a short training course or
video series explaining the format, the compressor, and how to use them.

This should align with the 3.0 release, so we cna release both together.

## Normal maps and HDR texture performance

The current codec disables PSNR thresholds for both normal maps and HDR
textures, which effectively requires each search to run to completion based
only on coarse tuning heuristics. This makes normal maps and HDR textures
significantly slower than color LDR textures, and it feels like we should be
able to come up with a criteria to exit "easy blocks" faster. Simple linear
PSNR may not be a good choice, but _something_ should be possible.

This is a quality change, so we'd like to get this changed done before 3.0.

## Compression effort level

Today we have a set of distinct quality profiles (fast, medium, etc) which
have relatively large gaps between them in terms of quality and performance.
We'd like to expose a more granular "effort level", which would give users more
control over the quality-vs-time tradeoff. Existing presets would remain,
mapping to specific effort levels.

This is an API change, so we'd like to get this changed done before 3.0.

## Command line front-end implementation

The current command line front-end is very similar to the version in the 1.x
series. The options we expose have been cleaned up but the implementation has
had relatively little done to it. Major goals:

* Make the image path zero copy.
* Support the full range of KTX containers for input and output images (arrays,
  cube maps, mipmaps, etc).
* Support automatic mipmap generation.

## Command line back-end implementation

The current command line ships as multiple binaries, one per instruction set,
so the user has to pick the right one for their machine. It would be a better
user experience if this was one binary with multiple backend builds that could
be selected at runtime.

Last time we tried this using a dynamically loaded shared object was 8% slower
than a static build, which we need to investigate and reduce.

## Code cleanup

We've been doing this piecemeal, but we want to have a push on improving the
quality of the source code style and documentation for the codec. This includes
module-level documentation of the core algorithms, adding Doxygen API
documentation for the codec, and generally apply some code cleanup.
