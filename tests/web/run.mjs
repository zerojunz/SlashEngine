// Runs the web build of the drawing tests in Node, with Dawn's WebGPU (the webgpu package) as the
// browser's, so the WebGPU device draws on this machine's GPU: Mesa's software Vulkan in CI.
//
//   emcmake cmake -S . -B build/web -DSLASH_BUILD_TOOLS=OFF && cmake --build build/web
//   npm ci --prefix tests/web
//   node tests/web/run.mjs build/web/tests/slash_web_tests.js [doctest options]
import { createRequire } from "node:module";
import path from "node:path";
import { create, globals } from "webgpu";

const [script, ...args] = process.argv.slice(2);
if (!script) {
  console.error("usage: node run.mjs <slash_web_tests.js> [doctest options]");
  process.exit(2);
}
Object.assign(globalThis, globals);
Object.defineProperty(globalThis.navigator, "gpu", { value: create([]), configurable: true });

// Node ends when nothing is left to wait for, which a wait for the GPU that never ends leaves.
// That is a failure, not a pass.
process.on("beforeExit", () => {
  console.error("the tests stopped before they finished: a wait for the GPU never ended");
  process.exit(1);
});

const start = createRequire(import.meta.url)(path.resolve(script));
start({
  arguments: args,
  // Emscripten's environment starts empty; the tests read whether CI expects a GPU from it.
  preRun: [
    (module) => {
      if (process.env.SLASH_EXPECT_GPU !== undefined) {
        module.ENV.SLASH_EXPECT_GPU = process.env.SLASH_EXPECT_GPU;
      }
    },
  ],
  onExit: (status) => process.exit(status),
}).catch((error) => {
  console.error(error);
  process.exit(1);
});
