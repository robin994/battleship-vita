# Vita release shader cache

This directory is intentionally empty until a complete cold-cache run has
generated all Fast3D programs on real hardware.

Copy the two directories from the Vita without renaming their `.gxp` files:

```text
ux0:data/shader_cache/SSB64VITA/v0/v -> port/vita_shader_cache/v0/v
ux0:data/shader_cache/SSB64VITA/v0/f -> port/vita_shader_cache/v0/f
```

`Makefile.vita` packages every `.gxp` below this directory at
`app0:/shader_cache/`. The renderer detects both packaged directories before
initializing vitaGL and selects them instead of compiling the shaders again.
If they are absent, vitaGL keeps using its writable `ux0:` cache.
