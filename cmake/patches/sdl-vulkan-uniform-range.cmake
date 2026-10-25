# Lets a Vulkan shader see 16 KiB of each uniform push rather than 4 KiB. SDL3's Vulkan device
# binds each push's uniform buffer with a range of MAX_UBO_SECTION_SIZE, so a shader reads zeros
# (or, without robust buffer access, whatever is there) past the first 4 KiB, while the other
# backends see all of it: the meshes' shadow uniforms, for one, are over 10 KiB. 16 KiB is the
# least maxUniformBufferRange Vulkan lets a device have. slash_add_dependency(SDL3) runs it in
# SDL's source folder as the patch step; a script rather than a patch file, as for Dawn's.

set(file src/gpu/vulkan/SDL_gpu_vulkan.c)
file(READ ${file} text)
set(patched "#define MAX_UBO_SECTION_SIZE          16384    // 16  KiB")
if(text MATCHES "#define MAX_UBO_SECTION_SIZE +16384")
    return()
endif()
set(original "#define MAX_UBO_SECTION_SIZE +4096 +// 4 +KiB")
if(NOT text MATCHES "${original}")
    message(FATAL_ERROR "${file} no longer defines MAX_UBO_SECTION_SIZE as the patch expects")
endif()
string(REGEX REPLACE "${original}" "${patched}" text "${text}")
file(WRITE ${file} "${text}")
