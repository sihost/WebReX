## Summary

<!-- What does this change and why? Link issues with "Fixes #123". -->

## Area

- [ ] GPU / WebGPU (`src/graphics/webgpu`, shaders, presenter)
- [ ] Runtime (kernel, threads, memory, fibers)
- [ ] Files / storage (HttpRangeDevice, OPFS)
- [ ] Audio
- [ ] Input
- [ ] Network (relay shim, `tools/web_relay`)
- [ ] Build / CI / docs
- [ ] Native only (D3D12 / Vulkan)

## Testing

<!-- Say what you ran and what you saw. "Builds" alone is not enough for runtime changes. -->

- [ ] wasm64 build (`wasm64-release` preset) succeeds
- [ ] Native build still succeeds (if shared code changed)
- [ ] Relevant tests pass (`tests/web/*`, `tools/web_relay` tests, shader harness)
- [ ] Ran in a browser: <!-- browser + version, GPU, OS -->
- [ ] No new `WebGPU uncaptured error` lines in the console
- [ ] For rendering changes: frames compared against native (PSNR or screenshots attached)

## Checklist

- [ ] Commits follow [Conventional Commits](https://www.conventionalcommits.org/) (`type(scope): description`)
- [ ] Code is formatted (`clang-format`) and lint passes
- [ ] New cvars / behaviour changes are documented (README, `docs/web_limitations.md`)
- [ ] **No game code, game data, keys or copyrighted assets** are included
