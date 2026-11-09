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


def varyings(source, keyword):
    found = []
    pos = 0
    while pos < len(source):
        nxt = source.find("\n", pos)
        nxt = len(source) if nxt < 0 else nxt
        text = source[pos:nxt]
        if text.startswith(keyword + " ") and text.endswith(";"):
            words = text[len(keyword) + 1:-1].split(" ")
            if len(words) in (2, 3):
                found.append((pos, nxt, words[0] if len(words) == 3 else "", words[-2], words[-1]))
        pos = nxt + 1
    return found


def renamed(source, found, keyword):
    for line, end, precision, kind, name in reversed(found):
        typ = (precision + " " if precision else "") + kind
        declared = "%s %s slash_%s;\n%s %s;" % (keyword, typ, name, typ, name)
        source = source[:line] + declared + source[end:]
    return source


def at_w1(shaders):
    """The GL device's rewrite for Apple's software renderer (at_w1_vertex and at_w1_fragment)."""
    vertex = shaders["kMeshVertexGlsl"]
    outputs = varyings(vertex, "out")
    vertex = renamed(vertex, outputs, "out")
    scaling = ("    slash_scale = gl_Position.w > 1.0e-3 ? 1.0 / gl_Position.w : 1.0;\n"
               "    gl_Position *= slash_scale;\n")
    for o in outputs:
        scaling += "    slash_%s = %s * slash_scale;\n" % (o[4], o[4])
    end = vertex.rfind("}")
    vertex = vertex[:end] + scaling + vertex[end:]
    main = vertex.find("void main()")
    vertex = vertex[:main] + "out highp float slash_scale;\n" + vertex[main:]
    fragment = shaders["kMeshFragmentGlsl"]
    inputs = varyings(fragment, "in")
    fragment = renamed(fragment, inputs, "in")
    dividing = "".join("    %s = slash_%s / slash_scale;\n" % (i[4], i[4]) for i in inputs)
    main = fragment.find("void main()")
    brace = fragment.find("{", main) + 2
    fragment = fragment[:brace] + dividing + fragment[brace:]
    fragment = fragment[:main] + "in highp float slash_scale;\n" + fragment[main:]
    return {"kMeshVertexGlsl": vertex, "kMeshFragmentGlsl": fragment}


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
    rewritten = [(name + ", at w 1", at_w1(shaders)) for name, shaders in out[:-1]]
    return out[:2] + rewritten


def draw(vertex, fragment):
    # Cover the target whatever the attributes and matrices say, so every pixel runs the
    # fragment shader.
    end = vertex.rfind("}")
    vertex = (vertex[:end] + "    gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, "
              "gl_VertexID == 2 ? 3.0 : -1.0, 0.5, 1.0);\n" + vertex[end:])
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

    # Uniform blocks filled with 2047 in every word, so each option bit is set and each number
    # is small, as a draw with every feature on.
    blocks = ctypes.c_int()
    gl.glGetProgramiv(program, 0x8A36, ctypes.byref(blocks))
    words = (ctypes.c_uint * 16384)(*([2047] * 16384))
    for i in range(blocks.value):
        buf = ctypes.c_uint()
        gl.glGenBuffers(1, ctypes.byref(buf))
        gl.glBindBuffer(0x8A11, buf)
        gl.glBufferData(0x8A11, ctypes.c_ssize_t(65536), words, 0x88E4)
        gl.glUniformBlockBinding(program, i, i)
        gl.glBindBufferBase(0x8A11, i, buf)
    # Each sampler on a unit of its own (samplers of different types on one unit fail the draw),
    # with a complete texture of its kind bound there, as the device binds them.
    uniforms = ctypes.c_int()
    gl.glGetProgramiv(program, 0x8B86, ctypes.byref(uniforms))
    gl.glUseProgram(program)
    texels = (ctypes.c_ubyte * (16 * 16 * 4))(*([128] * (16 * 16 * 4)))
    depths = (ctypes.c_float * (16 * 16))(*([0.5] * (16 * 16)))
    unit = 0
    for i in range(uniforms.value):
        name = ctypes.create_string_buffer(256)
        size = ctypes.c_int()
        kind = ctypes.c_uint()
        gl.glGetActiveUniform(program, i, 256, None, ctypes.byref(size), ctypes.byref(kind), name)
        if kind.value not in SAMPLERS:
            continue
        location = gl.glGetUniformLocation(program, name)
        for j in range(size.value):
            gl.glUniform1i(location + j, unit)
            gl.glActiveTexture(0x84C0 + unit)
            tex = ctypes.c_uint()
            gl.glGenTextures(1, ctypes.byref(tex))
            if kind.value == 0x8B60:  # samplerCube
                gl.glBindTexture(0x8513, tex)
                for face in range(6):
                    gl.glTexImage2D(0x8515 + face, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, texels)
                gl.glGenerateMipmap(0x8513)
                gl.glTexParameteri(0x8513, 0x2801, 0x2703)
                gl.glTexParameteri(0x8513, 0x2800, 0x2601)
            elif kind.value == 0x8B62:  # sampler2DShadow
                gl.glBindTexture(0x0DE1, tex)
                gl.glTexImage2D(0x0DE1, 0, 0x8CAC, 16, 16, 0, 0x1902, 0x1406, depths)
                gl.glTexParameteri(0x0DE1, 0x884C, 0x884E)
                gl.glTexParameteri(0x0DE1, 0x884D, 0x0203)
                gl.glTexParameteri(0x0DE1, 0x2801, 0x2601)
                gl.glTexParameteri(0x0DE1, 0x2800, 0x2601)
            else:
                gl.glBindTexture(0x0DE1, tex)
                gl.glTexImage2D(0x0DE1, 0, 0x8058, 16, 16, 0, 0x1908, 0x1401, texels)
                gl.glGenerateMipmap(0x0DE1)
                gl.glTexParameteri(0x0DE1, 0x2801, 0x2703)
                gl.glTexParameteri(0x0DE1, 0x2800, 0x2601)
                gl.glTexParameteri(0x0DE1, 0x2802, 0x2901)
                gl.glTexParameteri(0x0DE1, 0x2803, 0x2901)
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
    depth = ctypes.c_uint()
    gl.glGenRenderbuffers(1, ctypes.byref(depth))
    gl.glBindRenderbuffer(0x8D41, depth)
    gl.glRenderbufferStorage(0x8D41, 0x8CAC, 64, 64)
    gl.glFramebufferRenderbuffer(0x8D40, 0x8D00, 0x8D41, depth)
    gl.glEnable(0x0B71)
    gl.glDepthFunc(0x0204)
    gl.glClearDepth(ctypes.c_double(0.0))
    gl.glClear(0x4100)
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
