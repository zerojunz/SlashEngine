// SDL reads URL parameters from window.location before main() runs, and Node has no window. An
// empty location lets the exported game run headless under Node, which is how CI tests web
// exports. Browsers are unaffected.
if (ENVIRONMENT_IS_NODE && typeof window == "undefined") {
  Module["preRun"] = [].concat(Module["preRun"] || [], () => {
    globalThis.window = { location: { search: "" } };
  });
}
