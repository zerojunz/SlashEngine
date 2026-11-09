#!/usr/bin/env python3
"""Times how long Apple's software OpenGL renderer takes over versions of the mesh shader.

Each version is compiled, linked and drawn once in a process of its own, as a test would, then
this waits for CVMCompiler (which compiles the shader to machine code after the draw) to go
quiet, up to a limit, and stops it before the next version. Temporary: it finds which part of
the alpha hash change makes CVMCompiler run for minutes.
"""

import ctypes
import os
import re
import subprocess
import sys
import time

LIMIT = 300.0
# GL's sampler uniform types.
SAMPLERS = {0x8B5D, 0x8B5E, 0x8B5F, 0x8B60, 0x8B62, 0x8DC1, 0x8DC4, 0x8DC5, 0x8DCA, 0x8DCB, 0x8DCC,
            0x8DCF, 0x8DD2, 0x8DD3, 0x8DD4, 0x8DD7, 0x8B63, 0x8DD0, 0x8DD5, 0x8B61, 0x8B64}


def arrays(header):
    found = {}
    for name in ("kMeshVertexGlsl", "kMeshFragmentGlsl"):
        start = header.index("inline constexpr const char* %s[] = {" % name)
        end = header.index(")glsl\"};", start) + len(")glsl\"")
        chunks = re.findall(r'R"glsl\((.*?)\)glsl"', header[start:end], re.S)
        found[name] = "".join(chunks).replace("#version 300 es", "#version 330 core", 1)
    return found


def versions():
    path = "engine/render/src/mesh_shaders.hpp"
    branch = open(path).read()
    main = subprocess.run(["git", "show", "origin/main:" + path], capture_output=True,
                          text=True, check=True).stdout
    b = arrays(branch)
    m = arrays(main)
    out = [("main", m), ("branch", b)]
    hash_test = "(_907.options.x & 1024u) != 0u"
    if hash_test in b["kMeshFragmentGlsl"]:
        off = dict(b)
        off["kMeshFragmentGlsl"] = b["kMeshFragmentGlsl"].replace(hash_test, "false")
        out.append(("branch, hash branch false", off))
    flat = dict(b)
    flat["kMeshFragmentGlsl"] = re.sub(r"dFd([xy])\(v_object\)", "vec3(0.001)",
                                       b["kMeshFragmentGlsl"])
    out.append(("branch, no derivatives", flat))
    nomath = dict(b)
    nomath["kMeshFragmentGlsl"] = re.sub(r"uvec3\(1664525u\)", "uvec3(1u)",
                                         b["kMeshFragmentGlsl"])
    out.append(("branch, hash multiplier 1", nomath))
    out.append(("main again", m))
    return out


def draw(vertex, fragment):
    gl = ctypes.CDLL("/System/Library/Frameworks/OpenGL.framework/OpenGL")
    attrs = (ctypes.c_int * 5)(99, 0x3200, 70, 0x00020400, 0)
    pix = ctypes.c_void_p()
    count = ctypes.c_int()
    ctx = ctypes.c_void_p()
    assert gl.CGLChoosePixelFormat(attrs, ctypes.byref(pix), ctypes.byref(count)) == 0
    assert gl.CGLCreateContext(pix, None, ctypes.byref(ctx)) == 0
    assert gl.CGLSetCurrentContext(ctx) == 0
    gl.glCreateShader.restype = ctypes.c_uint
    gl.glCreateProgram.restype = ctypes.c_uint
    gl.glGetString.restype = ctypes.c_char_p
    print("  renderer:", gl.glGetString(0x1F01).decode(), flush=True)

    def shader(kind, source):
        s = gl.glCreateShader(kind)
        text = ctypes.c_char_p(source.encode())
        gl.glShaderSource(s, 1, ctypes.byref(text), None)
        gl.glCompileShader(s)
        ok = ctypes.c_int()
        gl.glGetShaderiv(s, 0x8B81, ctypes.byref(ok))
        if not ok.value:
            log = ctypes.create_string_buffer(8192)
            gl.glGetShaderInfoLog(s, 8192, None, log)
            print("  compile failed:", log.value.decode(errors="replace")[:2000], flush=True)
            sys.exit(1)
        return s

    t0 = time.monotonic()
    program = gl.glCreateProgram()
    gl.glAttachShader(program, shader(0x8B31, vertex))
    gl.glAttachShader(program, shader(0x8B30, fragment))
    gl.glLinkProgram(program)
    ok = ctypes.c_int()
    gl.glGetProgramiv(program, 0x8B82, ctypes.byref(ok))
    t1 = time.monotonic()
    print("  compile and link: %.1f s, linked %d" % (t1 - t0, ok.value), flush=True)
    if not ok.value:
        log = ctypes.create_string_buffer(8192)
        gl.glGetProgramInfoLog(program, 8192, None, log)
        print("  link failed:", log.value.decode(errors="replace")[:2000], flush=True)
        sys.exit(1)

    blocks = ctypes.c_int()
    gl.glGetProgramiv(program, 0x8A36, ctypes.byref(blocks))
    zeros = ctypes.create_string_buffer(65536)
    for i in range(blocks.value):
        buf = ctypes.c_uint()
        gl.glGenBuffers(1, ctypes.byref(buf))
        gl.glBindBuffer(0x8A11, buf)
        gl.glBufferData(0x8A11, ctypes.c_ssize_t(65536), zeros, 0x88E4)
        gl.glUniformBlockBinding(program, i, i)
        gl.glBindBufferBase(0x8A11, i, buf)
    # Each sampler on a unit of its own: samplers of different types on one unit fail the draw.
    uniforms = ctypes.c_int()
    gl.glGetProgramiv(program, 0x8B86, ctypes.byref(uniforms))
    gl.glUseProgram(program)
    unit = 0
    for i in range(uniforms.value):
        name = ctypes.create_string_buffer(256)
        size = ctypes.c_int()
        kind = ctypes.c_uint()
        gl.glGetActiveUniform(program, i, 256, None, ctypes.byref(size), ctypes.byref(kind), name)
        if kind.value in SAMPLERS:
            location = gl.glGetUniformLocation(program, name)
            for j in range(size.value):
                gl.glUniform1i(location + j, unit)
                unit += 1
    print("  samplers: %d, GL error %d" % (unit, gl.glGetError()), flush=True)
    vao = ctypes.c_uint()
    gl.glGenVertexArrays(1, ctypes.byref(vao))
    gl.glBindVertexArray(vao)
    rb = ctypes.c_uint()
    gl.glGenRenderbuffers(1, ctypes.byref(rb))
    gl.glBindRenderbuffer(0x8D41, rb)
    gl.glRenderbufferStorage(0x8D41, 0x8058, 64, 64)
    fb = ctypes.c_uint()
    gl.glGenFramebuffers(1, ctypes.byref(fb))
    gl.glBindFramebuffer(0x8D40, fb)
    gl.glFramebufferRenderbuffer(0x8D40, 0x8CE0, 0x8D41, rb)
    gl.glViewport(0, 0, 64, 64)
    print("  framebuffer: %x" % gl.glCheckFramebufferStatus(0x8D40), flush=True)
    gl.glDrawArrays(4, 0, 3)
    pixels = ctypes.create_string_buffer(64 * 64 * 4)
    gl.glReadPixels(0, 0, 64, 64, 0x1908, 0x1401, pixels)
    gl.glFinish()
    print("  draw: %.1f s, GL error %d" % (time.monotonic() - t1, gl.glGetError()), flush=True)


def compilers():
    ps = subprocess.run(["ps", "-axo", "pid=,pcpu=,etime=,rss=,comm="], capture_output=True,
                        text=True).stdout
    return [line.split() for line in ps.splitlines() if "CVMCompiler" in line]


def main():
    if len(sys.argv) == 3:
        draw(open(sys.argv[1]).read(), open(sys.argv[2]).read())
        return
    work = os.environ.get("RUNNER_TEMP", "/tmp")
    for name, shaders in versions():
        print("== " + name, flush=True)
        subprocess.run(["pkill", "-9", "CVMCompiler"])
        vertex = os.path.join(work, "v.glsl")
        fragment = os.path.join(work, "f.glsl")
        open(vertex, "w").write(shaders["kMeshVertexGlsl"])
        open(fragment, "w").write(shaders["kMeshFragmentGlsl"])
        start = time.monotonic()
        try:
            subprocess.run([sys.executable, __file__, vertex, fragment], timeout=LIMIT)
        except subprocess.TimeoutExpired:
            print("  the draw took over %d s" % LIMIT, flush=True)
        drawn = time.monotonic()
        last_busy = drawn
        while time.monotonic() - drawn < LIMIT:
            if [c for c in compilers() if float(c[1]) > 5.0]:
                last_busy = time.monotonic()
            elif time.monotonic() - last_busy > 5.0:
                break
            time.sleep(1.0)
        print("  CVMCompiler busy for %.0f s after the draw%s (draw process %.1f s) %s" %
              (last_busy - drawn, " (gave up)" if time.monotonic() - drawn >= LIMIT else "",
               drawn - start, compilers()), flush=True)
    subprocess.run(["pkill", "-9", "CVMCompiler"])


if __name__ == "__main__":
    main()
