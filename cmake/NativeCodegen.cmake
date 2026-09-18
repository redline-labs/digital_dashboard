# SPDX-License-Identifier: GPL-3.0-or-later
#
# Finding the code generators this build runs on itself, when it cannot run what
# it just built.
#
# capnp/capnpc-c++/capnpc_schema_registry, dbc_code_gen and canopen_code_gen are
# ordinary targets, which is right natively and impossible cross-compiling: the
# binary carries the TARGET's ELF interpreter, and execve() reports a missing
# interpreter exactly like a missing file --
#
#     /bin/sh: 1: .../dbc_code_gen: not found
#
# about a file that is plainly there. Sharing a CPU with the target does not
# help; it is the loader path that is absent.
#
# So point REDLINE_NATIVE_CODEGEN_DIR at host builds of these tools and every
# generator invocation resolves there. Unset -- every workstation build -- it
# resolves to the in-tree targets exactly as before.
#
#     cmake -B build -DREDLINE_NATIVE_CODEGEN_DIR=/path/to/native-tools/bin ...
#
# Each call site include()s this file by a path relative to itself, so a consumer
# taking one library on its own gets the knob without including anything first.

include_guard(GLOBAL)

# A ;-list is accepted too: a desktop build keeps capnp and
# capnpc_schema_registry in different directories (tools/build-wasm.sh).
set(REDLINE_NATIVE_CODEGEN_DIR "" CACHE STRING
    "Directory (or ;-list of directories) holding host builds of this project's \
code generators (capnp, capnpc-c++, capnpc_schema_registry, dbc_code_gen, \
canopen_code_gen). Required when cross-compiling; leave empty to build and run \
them from the tree.")

# redline_codegen_tool(<command_var> <depends_var> <target> <executable>)
#
# Sets <command_var> to what an add_custom_command COMMAND should name and
# <depends_var> to what its DEPENDS should carry. The two names differ for
# capnproto: target `capnp_tool` ships as `capnp`, `capnpc_cpp` as `capnpc-c++`.
#
# A FUNCTION resolving per call, not a variable computed once per directory:
# generate_dbc_code() and generate_eds_code() are defined in libs/ but called
# from siblings (dbcs/, eds/), where a variable set in libs/ is invisible.
# Resolving inside the call leaves no variable to lose -- PARENT_SCOPE lands in
# whichever scope the generator was expanded in. Get that wrong and the program
# drops off the command line entirely, reported as
#
#     /bin/sh: 1: --name: not found
#
# naming the first argument rather than the gap in front of it.
function(redline_codegen_tool command_var depends_var target executable)
    if(NOT REDLINE_NATIVE_CODEGEN_DIR)
        set(${command_var} "$<TARGET_FILE:${target}>" PARENT_SCOPE)
        set(${depends_var} "${target}" PARENT_SCOPE)
        return()
    endif()

    # find_program rather than string concatenation: it is the one lookup that
    # knows the HOST's executable suffix. CMAKE_EXECUTABLE_SUFFIX describes the
    # target, which is the wrong machine for a tool this build has to execute.
    string(MAKE_C_IDENTIFIER "REDLINE_NATIVE_${executable}" cache_var)
    find_program(${cache_var}
        NAMES ${executable}
        PATHS ${REDLINE_NATIVE_CODEGEN_DIR}
        NO_DEFAULT_PATH
        DOC "Host build of ${executable}, used to generate sources for the target")

    if(NOT ${cache_var})
        message(FATAL_ERROR
            "REDLINE_NATIVE_CODEGEN_DIR is '${REDLINE_NATIVE_CODEGEN_DIR}', but it "
            "holds no '${executable}'. Once the knob is set every generator has to "
            "be found there -- a half-populated directory fails much later, as an "
            "unrunnable target binary.")
    endif()

    set(${command_var} "${${cache_var}}" PARENT_SCOPE)
    # Depend on the binary itself: generated code is tied to the generator that
    # produced it, so replacing the tool must invalidate its output.
    set(${depends_var} "${${cache_var}}" PARENT_SCOPE)
endfunction()
