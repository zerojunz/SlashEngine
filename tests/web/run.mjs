// Runs the web build of the drawing tests in Node, with Dawn's WebGPU (the webgpu package) as the
// browser's, so the WebGPU device draws on this machine's GPU: Mesa's software Vulkan in CI.
//
//   emcmake cmake -S . -B build/web -DSLASH_BUILD_TOOLS=OFF && cmake --build build/web
//   npm ci --prefix tests/web
//   node tests/web/run.mjs build/web/tests/slash_web_tests.js [doctest options]
import { createHash } from "node:crypto";
import http from "node:http";
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

// A WebSocket server that echoes each message, for the WebSocket test: it speaks the "echo"
// subprotocol and answers a close with the same code and reason.
function frame(opcode, payload) {
  const length = payload.length;
  const head = length < 126 ? Buffer.from([0x80 | opcode, length])
    : length < 65536 ? Buffer.from([0x80 | opcode, 126, length >> 8, length & 255])
    : Buffer.concat([Buffer.from([0x80 | opcode, 127]),
      (() => { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(length)); return b; })()]);
  return Buffer.concat([head, payload]);
}
const echo = http.createServer((request, response) => response.writeHead(426).end());
echo.on("upgrade", (request, socket) => {
  const accept = createHash("sha1")
    .update(request.headers["sec-websocket-key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")
    .digest("base64");
  const protocols = (request.headers["sec-websocket-protocol"] || "").split(",").map((p) => p.trim());
  socket.write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n" +
    `Sec-WebSocket-Accept: ${accept}\r\n` +
    (protocols.includes("echo") ? "Sec-WebSocket-Protocol: echo\r\n" : "") + "\r\n");
  socket.unref();
  let pending = Buffer.alloc(0);
  socket.on("data", (data) => {
    pending = Buffer.concat([pending, data]);
    for (;;) {
      if (pending.length < 2) return;
      const opcode = pending[0] & 15;
      let length = pending[1] & 127;
      let at = 2;
      if (length === 126) { if (pending.length < 4) return; length = pending.readUInt16BE(2); at = 4; }
      else if (length === 127) { if (pending.length < 10) return; length = Number(pending.readBigUInt64BE(2)); at = 10; }
      if (pending.length < at + 4 + length) return;
      const mask = pending.subarray(at, at + 4);
      const payload = Buffer.from(pending.subarray(at + 4, at + 4 + length));
      for (let i = 0; i < payload.length; ++i) payload[i] ^= mask[i & 3];
      pending = pending.subarray(at + 4 + length);
      if (opcode === 8) {
        socket.end(frame(8, payload));
        return;
      }
      socket.write(frame(opcode === 9 ? 10 : opcode, payload));
    }
  });
  socket.on("error", () => {});
});
await new Promise((resolve) => echo.listen(0, "127.0.0.1", resolve));
echo.unref();

const start = createRequire(import.meta.url)(path.resolve(script));
start({
  arguments: args,
  // Emscripten's environment starts empty; the tests read whether CI expects a GPU from it.
  preRun: [
    (module) => {
      if (process.env.SLASH_EXPECT_GPU !== undefined) {
        module.ENV.SLASH_EXPECT_GPU = process.env.SLASH_EXPECT_GPU;
      }
      module.ENV.SLASH_WS_ECHO = `ws://127.0.0.1:${echo.address().port}`;
    },
  ],
  onExit: (status) => process.exit(status),
}).catch((error) => {
  console.error(error);
  process.exit(1);
});
