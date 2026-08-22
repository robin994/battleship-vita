# Root 3D Resource Ownership v6

- Fixed the root cross-resource 3D corruption introduced/exposed by the post-relocation manifest: a resource finalizer is now forbidden from traversing or mutating an external dependency.
- Manifest root candidates are restricted to relocation targets physically owned by the resource being finalized.
- G_DL recursion is restricted to owner-local display lists; external G_DL edges are recorded but not traversed.
- G_VTX normalization is restricted to owner-local vertex ranges; external vertex targets are left to the dependency's own finalization/runtime typed decode.
- Added `external_vtx`, `external_dl`, and `ownership=strict` fields to `RESOURCE_3D_MANIFEST` diagnostics.
- Changed the Vita mode banner to `vertex=post-reloc-manifest-strict`.
- Fixed resource publication ordering: a range remains `LOADING` through relocation, 3D finalization, fighter fixups, audit, and integrity validation, then commits to `READY` exactly once.
- Added `RESOURCE_TRANSACTION_COMMIT` diagnostics.
- Kept the v3 pristine integrity fallback and runtime non-destructive GfxSpVertex decode fallback.
