// Asks the browser for a WebGPU device before main() runs: WebGPU hands devices out only
// asynchronously, and the engine makes its render device without waiting (src/webgpu_device.cpp
// takes this one). Where the browser has no WebGPU, Module.slashWebGpu says why, and the "auto"
// driver draws with WebGL 2 instead.
Module["preRun"] = [].concat(Module["preRun"] || [], () => {
  const state = (Module["slashWebGpu"] = { device: null, unavailable: "", errors: [] });
  const gpu = typeof navigator != "undefined" ? navigator["gpu"] : undefined;
  if (!gpu) {
    state.unavailable = "this browser has no WebGPU";
    return;
  }
  addRunDependency("slash-webgpu");
  gpu
    .requestAdapter()
    .then((adapter) => {
      if (!adapter) {
        state.unavailable = "the browser offered no WebGPU adapter";
        return null;
      }
      // The compressed formats textures may come in, where the GPU samples them.
      const features = ["texture-compression-bc", "texture-compression-astc"].filter((name) =>
        adapter.features.has(name)
      );
      return adapter.requestDevice({ requiredFeatures: features });
    })
    .then((device) => {
      if (!device) {
        return;
      }
      // Errors the device reports, which the render device turns into failed calls.
      device.addEventListener("uncapturederror", (event) => {
        state.errors.push(event.error.message);
      });
      device.lost.then((info) => {
        if (info.reason != "destroyed") {
          state.errors.push("the WebGPU device was lost: " + info.message);
        }
      });
      state.device = device;
    })
    .catch((error) => {
      state.unavailable = "asking for a WebGPU device failed: " + error;
    })
    .finally(() => removeRunDependency("slash-webgpu"));
});
